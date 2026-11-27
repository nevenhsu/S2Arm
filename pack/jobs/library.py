"""Managed game catalogue, immutable container imports and player data.

Storage/interaction helpers adapted from LumaDock under MIT.
"""
from __future__ import annotations
import collections
import contextlib
from copy import deepcopy
import errno
import filelock
from space_rule import pack_direct_bytes, pack_input_bytes, pack_need_bytes, selected_packages   # 出包空间规则的唯一定义（剃刀 H11）
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import zlib
import uuid
import zipfile
from errors import explain
from core import Failure, atomic, contained, digest, prune_jobs, read, resolve_path, task_of_log
import toolchain
import winproc
import filelock
import downstream
import layout
import sizes

# 诊断包里的日志整份放（zip 压缩，文本约 10 倍）：一份超过 DIAGNOSTIC_LOG_LIMIT 才截，截时留开头 1/8、结尾 7/8（崩溃、存档前后的读数多在后段），
#   中间另捞心跳行与最后一次崩溃现场，manifest.json 的 truncated 写明每份原长、留了多少、截掉多少。
#   原先每份只留头尾各 16 KB + 心跳行，帧读数整段丢，玩家只能再传完整日志（#41）
DELETE_LOCK_WAIT_S = 120   # 删除等执行锁的上限：启动器发删除前先取消这款在跑的任务，它杀子进程组收尾比短作业排队（core.LOCK_WAIT_S）久
DIAGNOSTIC_LOG_LIMIT = 12 << 20     # 单份原文上限：诊断收件按成员解开后的大小收（tools/diag_contract/diag_contract.json limits.member_bytes 16 MB），
#   超了整包被拒、玩家丢整份诊断（11-16 原先 200 MB，长局 Mac 日志整包 400）。头尾 12 MB + 中间心跳 / 现场段（≤ 约 160 KB）+ 脱敏与非法字节替换的余量仍在 16 MB 内；
#   tests/diag_contract_test.sh 核「上限 + 中间段 + 余量 ≤ 契约 member_bytes」并真打超长日志过收件判定
DIAGNOSTIC_LOG_HEAD = DIAGNOSTIC_LOG_LIMIT // 8
DIAGNOSTIC_LOG_TAIL = DIAGNOSTIC_LOG_LIMIT - DIAGNOSTIC_LOG_HEAD
DIAGNOSTIC_LOG_BEATS = 64000         # 中间心跳行最多保留这么多字节（10 秒一行，约一小时）
DIAGNOSTIC_LOG_SCAN = 64 << 20       # 只在结尾前这么大的范围里找心跳行
DIAGNOSTIC_LOG_CHUNK = 1 << 20
DIAGNOSTIC_LOG_SCENE = 96000         # 中间最后一次崩溃摘要 + 全线程现场块最多保留这么多字节（超了按优先级裁，见 _scene_text）


class _Scene:
    """扫中段时收最后一次「崩溃摘要 + 全线程现场」（runtime/src/crash.c 打【红】CRASH 摘要行，随后 tdump.c 每行带「[现场] 」前缀，
    「===== 全线程现场：…」起、「===== 现场完 =====」止）。#141：只留头尾时崩溃在中段就整段丢了，玩家交了诊断包也定不了因"""
    CRASH, TAG = "CRASH 信号".encode(), "[现场]".encode()  # 非玩家句
    START, END = "===== 全线程现场".encode(), "现场完".encode()  # 非玩家句

    def __init__(self):
        self.summary, self.block, self.in_block, self.taking = [], [], False, False
        self.last = None   # (摘要行, 现场块行)

    def feed(self, line):
        if self.CRASH in line and self.TAG not in line:
            self.summary, self.taking = [line], True
            if not self.in_block: self.last = (self.summary, [])   # 只有摘要、没等到现场块也留
            return
        if self.TAG not in line:
            if self.taking and len(self.summary) < 60: self.summary.append(line)
            return
        if self.START in line:
            self.block, self.in_block, self.taking = [line], True, False
            self.last = (self.summary if self.summary else [], self.block)
            self.summary = []
        elif self.in_block:
            self.block.append(line)
            if self.END in line: self.in_block = False


def key_generation_note(lines):
    """读取器解不开时多打的「【密钥代】<要的代>\t<prod.keys 有到>\t<带 rights id>」（十六进制，同 prod.keys 里 master_key_XX 的写法，#1511）
    → 玩家句补一句「这份要第几代、你的有到第几代」；读取器没给（读不出 NCA 头）就不补"""
    line = next((x for x in lines if x.startswith("【密钥代】")), None)  # 非玩家句
    parts = line[len("【密钥代】"):].split("\t") if line else []  # 非玩家句
    if len(parts) < 2 or parts[1] == "-":
        return ""
    need, top = parts[0], parts[1]
    if int(need, 16) > int(top, 16):
        return "\n" + f"这份文件要 master_key_{need}，你的 prod.keys 只到 master_key_{top}：不够新。"
    return "\n" + f"这份文件要 master_key_{need}，你的 prod.keys 有到 master_key_{top}：代数够，多半是这一行的值不对。"


def _scene_text(summary, block, budget=DIAGNOSTIC_LOG_SCENE):
    """整段放得下就整段；放不下按优先级：崩溃摘要 → 现场块头 → 第一个非等待线程的整段栈 → 其余线程的头一行 → 现场完"""
    text = b"\n".join(summary + block)
    if len(text) <= budget:
        return text
    keep = list(summary[:24]) + block[:1]
    threads, cur = [], None
    for line in block[1:]:
        if "[现场] 线程 ".encode() in line: cur = [line]; threads.append(cur)  # 非玩家句
        elif cur is not None and _Scene.END not in line: cur.append(line)
    first = next((t for t in threads if "等待".encode() not in t[0]), threads[0] if threads else [])
    keep += first
    used = sum(len(x) + 1 for x in keep)
    for t in threads:
        if t is first: continue
        if used + len(t[0]) + 1 > budget - 200: keep.append(f"[现场] …（还有线程略去：现场块 {len(text)} 字节超出保留上限）".encode()); break  # 非玩家句
        keep.append(t[0]); used += len(t[0]) + 1
    keep += [x for x in block[-1:] if _Scene.END in x]
    return b"\n".join(keep)


def _line_head(source, limit):
    """读开头至多 limit 字节，截在最后一个换行后（不把一行拦腰截断）"""
    data = source.read(limit)
    cut = data.rfind(b"\n")
    return data[:cut + 1] if cut >= 0 else data


def diagnostic_log_chunks(source, size, head=None, tail=None):
    """诊断包里一份日志的内容，按块给出（调用方逐块脱敏、写进 zip，不整份进内存）。不超 head + tail 整份；超了：开头 head（截在行尾）
    + 中间的心跳行与最后一次崩溃摘要、全线程现场 + 结尾 tail（从行首起）。最后一块之后返回 (留下的字节, 截掉的字节)，经 StopIteration.value 取"""
    head = DIAGNOSTIC_LOG_HEAD if head is None else head
    tail = DIAGNOSTIC_LOG_TAIL if tail is None else tail
    # 只读到打开时量的 size 为止（11-17 D-4PE6：游戏开着、日志边导边涨，原先读到文件尾，导出慢的时候一份 game.log 涨过收件单个成员 16 MB 上限，
    #   整包被拒「单个文件超过 16 MB」）；导出之后才写进来的部分不收
    if size <= head + tail:
        kept = 0
        while kept < size and (chunk := source.read(min(DIAGNOSTIC_LOG_CHUNK, size - kept))):
            kept += len(chunk); yield chunk
        return kept, 0
    first = _line_head(source, head)
    kept = len(first); yield first
    tail_start = size - tail
    source.seek(tail_start - 1)
    if source.read(1) != b"\n":   # 结尾段从下一行行首起
        probe = source.read(min(DIAGNOSTIC_LOG_CHUNK, tail))
        nl = probe.find(b"\n")
        tail_start += nl + 1 if nl >= 0 else 0
    source.seek(len(first))
    beats, used, partial, left = collections.deque(), 0, b"", tail_start - len(first)
    scene = _Scene()
    while left > 0:
        chunk = source.read(min(DIAGNOSTIC_LOG_CHUNK, left))
        if not chunk:
            break
        left -= len(chunk)
        lines = (partial + chunk).split(b"\n")
        partial = lines.pop()
        if len(partial) > DIAGNOSTIC_LOG_CHUNK:   # 没有换行的超长段不是心跳行，也别让它占内存
            partial = b""
        for line in lines:
            if len(line) < 4096: scene.feed(line)
            if "[心跳]".encode() in line and len(line) < 4096:  # 非玩家句
                beats.append(line)
                used += len(line) + 1
                while used > DIAGNOSTIC_LOG_BEATS:
                    used -= len(beats.popleft()) + 1
    if partial and len(partial) < 4096: scene.feed(partial)
    omitted = tail_start - len(first)
    gap = lambda text: f"\n…（{text}）…\n".encode()
    mid = gap(f"日志共 {size} 字节，超过 {DIAGNOSTIC_LOG_LIMIT >> 20} MB 上限，中间截掉 {omitted} 字节，只保留其中 {len(beats)} 行心跳") + b"\n".join(beats)  # 非玩家句
    if scene.last:
        mid += gap("中间最后一次崩溃摘要与全线程现场") + _scene_text(*scene.last)  # 非玩家句
    yield mid + gap("以下是日志结尾")  # 非玩家句
    source.seek(tail_start)
    pos = tail_start
    while pos < size and (chunk := source.read(min(DIAGNOSTIC_LOG_CHUNK, size - pos))):
        pos += len(chunk); kept += len(chunk); yield chunk
    return kept, omitted


def diagnostic_log_excerpt(source, size, head=None, tail=None):
    """diagnostic_log_chunks 拼成一份（单测用）"""
    return b"".join(diagnostic_log_chunks(source, size, head, tail))


DIAGNOSTIC_JOB_LIMIT = 4 << 20
PHONE_TEXT = (".log", ".txt", ".tsv", ".json", ".ips", ".csv")   # 拉回的手机诊断里按文本脱敏收的类型（host.log、logs/、frames.tsv、threads.txt、diag/*.json、metrickit 等）
DIAG_RUN_FILE_LIMIT = 4 << 20    # 诊断局 PNG：单张上限
PHONE_ZIP_LIMIT = 256 << 20      # 手机 App 自己导出的诊断包（diag/S2Arm-诊断-*.zip）：这么大以内的打开、按成员收（D-778G），更大的整份跳过
DIAG_PSO_FILE_LIMIT = 4 << 20   # 本机管线记录 pso_user.tsv 单份上限（实测约 1 MB；收件单个成员上限 16 MB）
DIAG_SALVAGE_GAME_LIMIT = (8 << 20) + (1 << 20)   # 着色器补料每款上限（运行时 S2A_SHADER_SALVAGE_MB 缺省 8 MB 的字节 + index.tsv 余量）
DIAG_SALVAGE_TOTAL_LIMIT = 16 << 20   # 着色器补料整包上限（诊断包压缩后 ≤ 30 MB，给日志 / 截图留地方）
DIAG_RUN_TOTAL_LIMIT = 40 << 20  # 诊断局 PNG：总量上限（#438 定）
# 整包预算（11-16：Mac 诊断包压缩后超过收件上限整包被拒，#1308 玩家只能手删 pso / shader_miss / diagrun / phone 再传）：
#   收件端 tools/diag_contract/diag_contract.json limits.zip_bytes = 30 MB；留 1 MB 给 manifest 与 zip 目录（tests/diag_contract_test.sh 核两者对得上）。
#   日志与任务记录先写、优先；后面的可选成员（手机诊断、本机崩溃报告、诊断局截图、管线记录、着色器补料）放之前估压缩后大小，放不下就跳过，
#   日志本身快到预算时余下的日志也跳过；跳过的都记进 manifest budget.skipped，不悄悄丢
DIAGNOSTIC_ZIP_BUDGET = (30 << 20) - (1 << 20)
# 本机（Mac）崩溃报告（11-14 诊断#43f7d3：宿主起局 1 秒内 SIGTRAP，没走到写日志，诊断包里一份现场都没有）：系统写在 ~/Library/Logs/DiagnosticReports，
#   只收我们自己的进程（宿主 s2arm-host、启动器 S2Arm*）近 7 天的 .ips，最新的至多 10 份、单份至多 4 MB，按文本脱敏
MAC_CRASH_DIR = Path.home() / "Library" / "Logs" / "DiagnosticReports"
MAC_CRASH_NAME = re.compile(r"^(?:s2arm-host|S2Arm[A-Za-z]*)-\d{4}-\d{2}-\d{2}-\d{6}(?:-\d+)?\.ips$")
MAC_CRASH_DAYS, MAC_CRASH_MAX, MAC_CRASH_FILE_LIMIT = 7, 10, 4 << 20


# 游戏原始包：安装包 NSP 与卡带镜像 XCI，及它们的压缩版 NSZ / XCZ（nx_unpack 按内容认格式、还原压缩的 NCA）；
#   Switch 2 的 NUP 容器（.nup，一个文件装本体 + 更新 + DLC，不加密、不要 prod.keys）由 pack/ns2_nup.py 认与展开，不走 nx_unpack；
#   已解包的游戏目录（Switch 2 导出 dump / S2Arm 解包 extract / 通用解包 loose）也可直接导入，不要密钥
SOURCE_SUFFIXES = (".nsp", ".xci", ".nsz", ".xcz", ".nup")
NUP_SUFFIX = ".nup"
KEYLESS_FORMATS = ("nup", "dump", "extract", "loose")   # 不加密、不要 prod.keys 的格式（NUP 容器与解包目录）：要不要密钥只按这一份判（剃刀 W10）


def needs_keys(packages):
    """这组包（selected_packages 选定的）要不要 prod.keys：有任一项格式不在 KEYLESS_FORMATS（NSP / XCI / NSZ / XCZ 加密原件）就要"""
    return any(p.get("format") not in KEYLESS_FORMATS for p in packages)


def engine_tool(c, name):
    """引擎里的单文件 Python 工具（pack/<name>.py）按路径载入：作业层与一键出包用同一份实现（Switch 2 的 ns2_nup / ns2_dump），不另写一套"""
    import importlib.util
    spec = importlib.util.spec_from_file_location("s2a_" + name, c.engine / "pack" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def is_nup(c, path):
    """真 NUP（pack/ns2_nup.py --detect 同一判法 find_nup：按文件头魔数，不看扩展名）"""
    return engine_tool(c, "ns2_nup").find_nup(str(path)) is not None


def is_ns2_dump(c, path):
    """Switch 2 导出目录（pack/ns2_dump.py --detect：有 base/exefs/main.npdm）"""
    dump = engine_tool(c, "ns2_dump")
    return dump.find_dump(str(path)) is not None


def is_extracted_game(d):
    """这个目录本身是不是一款解包好的游戏：Switch 2 导出（base/exefs/main.npdm）、S2Arm 解包（exefs/main + unpack.tsv）、通用解包（exefs/main + main.npdm + romfs/）"""
    return ((d / "base" / "exefs" / "main.npdm").is_file()
            or ((d / "exefs" / "main").is_file() and (d / "unpack.tsv").is_file())
            or ((d / "exefs" / "main").is_file() and (d / "exefs" / "main.npdm").is_file() and (d / "romfs").is_dir()))


def _copy_cover(c, tid, game, metadata, icon):
    """导入时放封面：这款还没有文件夹就按英文名建（此刻它还没写进磁盘上的 catalog，layout.game_dir 自己去 catalog 取名会取到空、落成游戏号）。
    英文名没有时用读出的标题（纯中文标题清洗后为空，folder_name 退回游戏号）"""
    english = game.get("englishName") or metadata.get("name") or game.get("name") or ""
    target = layout.game_dir(c, tid, english=english) / "cover.jpg"
    shutil.copyfile(icon, target)


STORE_RETRY = 7 * 86400   # 商店给了定论（抓到 / 商品页 404 / 没有合格横图）的款，7 天内不再查；断网 / 超时 / 预算用尽不算查过，下次照查
STORE_LOGIC = 2   # 取图逻辑的版本：storeSettled 只在同版本下算数。2 = 美区 404 再查港区（11-14，亚洲版程序号原先被美区 404 定论锁 7 天）；以后取图口径放宽就加一
STORE_BUDGET = 10.0       # 每次最多花的联网时间（秒）：单页 / 单图各一跳短超时（banner_fetch），超了剩下的下次再查


def store_todo(c, ids=None):
    """要查商店的款（不联网，只看 catalog 与文件夹）：缺横图或英文名；ids 不给时跳过 7 天内查过的"""
    now = time.time()
    todo = []
    for g in c.catalog()["games"]:
        gid = str(g.get("id", "")).upper()
        if not re.fullmatch(r"[0-9A-F]{16}", gid) or (ids is not None and gid not in ids):
            continue
        d = layout.find_game(c, gid)
        if g.get("englishName") and d is not None and (d / "banner.jpg").is_file():
            continue
        if ids is None and g.get("storeLogic") == STORE_LOGIC and now - float(g.get("storeSettled") or 0) < STORE_RETRY:   # 旧字段 storeChecked（断网也记）不认：11-13 前一次没联上就锁 7 天
            continue
        todo.append(gid)
    return todo


STORE_QUIET_WAIT = 30.0   # 删除 / 移动 / 搬家前等后台商店查询结束的上限（秒）：它一轮联网最多 STORE_BUDGET 10 秒，加改文件夹名


def store_meta_quiet(c, library=None):
    """删游戏、移动游戏、整库搬家期间不让后台商店查询（store_meta_bg.py）往游戏库里写：拿它同一把锁 .store-meta.lock（library 缺省本任务的库）。
    它正在查就排队等它查完（最多 STORE_QUIET_WAIT 秒，它一轮联网有 STORE_BUDGET 上限），到上限报 library_busy、说清占锁者——
    不再「等不到就不拿锁照常继续」（启动器剃刀 H2：那等于没锁）；拿到后它再起来见锁被占就这次不查"""
    return c.lock("store-meta", wait=STORE_QUIET_WAIT, root=library)


def store_meta_background(c, ids=None):
    """在后台查商店（pack/jobs/store_meta_bg.py：store_meta 然后 heal_folder_names）：作业本身不等联网（商品页单页要几秒）。
    有要查的款才起；起不来（系统拒绝）就算了，下个作业再试。子进程脱离本作业：作业结束、取消都不带走它（Windows 前端不把作业放进杀子进程的作业对象）。
    S2A_BANNER_FETCH=0 不起"""
    if os.environ.get("S2A_BANNER_FETCH", "1") == "0" or not store_todo(c, ids):
        return False
    import subprocess
    args = [sys.executable, "-I", "-X", "utf8", "-B", str(Path(__file__).with_name("store_meta_bg.py")), str(c.private), str(c.library),
            str(c.r.get("libraryId") or ""), str(c.engine), *sorted(ids or ())]
    common = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    try:
        if os.name == "nt":
            flags = 0x00000200 | 0x08000000   # CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW
            try:
                subprocess.Popen(args, creationflags=flags | 0x01000000, **common)   # CREATE_BREAKAWAY_FROM_JOB：本作业万一在作业对象里也不被带走
            except OSError:
                subprocess.Popen(args, creationflags=flags, **common)
        else:
            subprocess.Popen(args, start_new_session=True, **common)
        return True
    except OSError:
        return False


def store_meta(c, ids=None, budget=STORE_BUDGET):
    """游戏库的商店元数据（与出到哪个平台无关，Mac / iPhone / Windows 都读这一份）：按程序号打开美区商品页一次，
    取横版封面放进这款文件夹（banner.jpg）、英文标题补进 catalog 的 englishName（NACP 没有英文标题时的第二来源：NACP 有的导入时已记，优先）。
    ids 给了（导入刚加的款）就只查这些；不给（作业开始时）查缺横图或英文名、7 天内没查过的款。
    断网、商品页 404、图不合格都静默跳过；只有商店给了定论（banner_fetch.settled）才记 storeSettled——
    断网 / 超时 / 失败预算用尽不记，下个作业再查（11-13：一次没联上就锁 7 天，维护者本机库 6 款全缺横图）。S2A_BANNER_FETCH=0 关掉。不含 ASCII 字母数字的标题不记（文件夹名用英文就是为了避开非 ASCII 路径）。
    调用方不能拿着目录锁（这里自己拿；拿不到这次跳过）→ 记了几款"""
    if os.environ.get("S2A_BANNER_FETCH", "1") == "0":
        return 0
    now = time.time()
    todo = store_todo(c, ids)
    if not todo:
        return 0
    try:
        fetch = engine_tool(c, "banner_fetch")
    except Exception:   # noqa: BLE001 —— 抓取工具载不进来：元数据是锦上添花，不挡作业
        return 0
    got = {}
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="store-", dir=c.private) as tmp:
        for gid in todo:
            if time.monotonic() - t0 > budget:
                break
            banner = Path(tmp) / (gid + ".jpg")
            try:
                ok, _why = fetch.fetch_banner(gid, banner)
            except Exception:   # noqa: BLE001
                ok = False
            got[gid] = (banner if ok and banner.is_file() else None, fetch.store_name(gid), fetch.settled(gid))
        if not got:
            return 0
        try:
            with c.lock():
                cat = c.catalog()
                for g in cat["games"]:
                    gid = str(g.get("id", "")).upper()
                    if gid not in got:
                        continue
                    banner, name, settled = got[gid]
                    g.pop("storeChecked", None)
                    if settled:
                        g["storeSettled"], g["storeLogic"] = int(now), STORE_LOGIC
                    if name and layout._ascii_name(name) and not g.get("englishName"):
                        g["englishName"] = name.strip()
                    if banner:
                        d = layout.find_game(c, gid) or layout.game_dir(c, gid, english=g.get("englishName") or g.get("name") or "")
                        if not (d / "banner.jpg").is_file():
                            # 临时目录在私有目录、游戏库常在外置盘：不能直接 os.replace 过去（跨卷 EXDEV），先拷进目标文件夹再原子改名
                            part = d / ".banner.jpg.part"
                            shutil.copyfile(banner, part)
                            os.replace(part, d / "banner.jpg")
                atomic(c.library / "catalog.json", cat)
        except Failure:   # 目录锁被别的任务拿着：下次再查
            return 0
    return len(got)


def extract_inspect(c, source, icon_dir):
    """解包目录 → 与 inspect / nup_inspect 同形的包元数据。三种 format（已在 import_games 按判据分开）：
    a. dump（Switch 2 导出）: 有 base/exefs/main.npdm
    b. extract（S2Arm 解包）: 有 exefs/main 与 unpack.tsv
    c. loose（通用解包）: 有 exefs/main、exefs/main.npdm 和 romfs/ 目录
    程序号：dump / loose 从 main.npdm 的 ACI0 读（复用 ns2_dump.read_tid），extract 取 unpack.tsv。
    名称：extract 取 unpack.tsv，其余有 control.nacp 用它（dump 取 ctl/，loose 取 exefs/），没有用目录名。
    只返回本体（type=base），version=0，不哈希整个目录、只哈希 exefs/main + exefs/main.npdm（dump 取 base/exefs 下同名文件）"""
    source = Path(source)
    dump = engine_tool(c, "ns2_dump")

    # 判定 format 与读程序号
    if (source / "unpack.tsv").is_file():
        # extract: S2Arm 解包目录，从 unpack.tsv 读
        fmt = "extract"
        try:
            tsv = (source / "unpack.tsv").read_text(encoding="utf-8")
            for line in tsv.splitlines():
                if line.startswith("title_id\t"):
                    tid = int(line.split("\t")[1], 16)
                    break
            else:
                raise ValueError("unpack.tsv 没有 title_id")  # 非玩家句
        except (OSError, ValueError, IndexError) as e:
            raise Failure(f"读不出解包目录的元数据（{source.name}）：{e}", code="content_invalid") from None
        main_path = source / "exefs" / "main"
        npdm_path = source / "exefs" / "main.npdm"
    elif (source / "base" / "exefs" / "main.npdm").is_file():
        # dump: Switch 2 导出目录
        fmt = "dump"
        try:
            tid = dump.read_tid(str(source / "base" / "exefs" / "main.npdm"))
        except (OSError, ValueError) as e:
            raise Failure(f"读不出 Switch 2 导出目录的程序号（{source.name}）：{e}", code="content_invalid") from None
        main_path = source / "base" / "exefs" / "main"
        npdm_path = source / "base" / "exefs" / "main.npdm"
    else:
        # loose: 通用解包目录
        fmt = "loose"
        try:
            tid = dump.read_tid(str(source / "exefs" / "main.npdm"))
        except (OSError, ValueError) as e:
            raise Failure(f"读不出解包目录的程序号（{source.name}）：{e}", code="content_invalid") from None
        main_path = source / "exefs" / "main"
        npdm_path = source / "exefs" / "main.npdm"

    # 散文件 / Switch 2 导出没有整包校验：导入时先把程序文件（NSO）试解一遍，物理上解不开当场点名（#1522：原先到出包解包才抛 LZ4 的 Python 栈）；
    #   段哈希对不上照常收、记黄（维护者 11-17：校验不过不拒收，可能是打过补丁 / mod 的程序）
    if fmt in ("dump", "loose"):
        exefs_dirs = [main_path.parent] + ([source / "updates" / n / "exefs" for n in dump.numdirs(str(source / "updates"))] if fmt == "dump" else [])
        for exefs in exefs_dirs:
            if not exefs.is_dir():
                continue
            rel = exefs.relative_to(source).as_posix()
            try:
                off = dump.check_exefs(str(exefs))
            except dump.NsoDamaged as e:
                raise Failure(f"程序文件 {rel}/{e.name} 的 {e.seg} 段解不开：这个程序文件坏了或被改过，请重新导出（最好导成 NSP / XCI）。", code="content_invalid") from None
            if off:
                c.diagnostic(SOURCE_INTEGRITY_SKIPPED, code="source_integrity_skipped", severity="warning",
                             internal="；".join(f"{rel}/{n} 的 {s} 段哈希对不上" for n, s in off))  # 非玩家句

    # 读名称与显示版本；english = 文件夹取名用的英文名（unpack.tsv 的 name / NACP 美式 → 英式英语），没有就空
    english = ""
    if fmt == "extract" and (source / "unpack.tsv").is_file():
        try:
            tsv = (source / "unpack.tsv").read_text(encoding="utf-8")
            name, name_zh, display = "", "", ""
            for line in tsv.splitlines():
                parts = line.split("\t")
                if len(parts) >= 2:
                    if parts[0] == "name":
                        name = parts[1]
                    elif parts[0] == "name_zh":
                        name_zh = parts[1]
                    elif parts[0] == "display_version":
                        display = parts[1]
            english = name
            name = name_zh or name or source.name
        except (OSError, UnicodeDecodeError):
            name, display = source.name, ""
    else:
        # dump 取 ctl/control.nacp，loose 取 exefs/control.nacp
        nacp_path = source / "ctl" / "control.nacp" if fmt == "dump" else source / "exefs" / "control.nacp"
        if nacp_path.is_file():
            try:
                nacp = nacp_path.read_bytes()
                if len(nacp) == 0x4000:
                    english = dump.nacp_title(nacp, 0) or dump.nacp_title(nacp, 1) or ""
                    name = dump.nacp_title(nacp, 0) or dump.nacp_title(nacp, 14) or dump.nacp_title(nacp, 13) or source.name
                    display = nacp[0x3060:0x3070].split(b"\0")[0].decode("ascii", "replace")
                else:
                    name, display = source.name, ""
            except (OSError, UnicodeDecodeError):
                name, display = source.name, ""
        else:
            name, display = source.name, ""

    # 封面：dump 取 ctl/icon.jpg，loose 取 exefs/icon.jpg
    icon_path = source / "ctl" / "icon.jpg" if fmt == "dump" else source / "exefs" / "icon.jpg"
    if icon_path.is_file():
        try:
            (Path(icon_dir) / f"{tid:016X}.jpg").write_bytes(icon_path.read_bytes())
        except OSError:
            pass

    # 哈希 exefs/main + exefs/main.npdm（不哈希整个 romfs）
    h = hashlib.sha256()
    for p in [main_path, npdm_path]:
        if p.is_file():
            h.update(p.read_bytes())
    sha = h.hexdigest()

    total = _part_bytes([source])   # 目录内文件的实占字节

    version = 0
    if fmt == "extract":   # 解包目录 unpack.tsv 的 update 列是合并进来的更新版本号；没有显示版本时界面用它
        cells = dict(l.split("\t", 1) for l in (source / "unpack.tsv").read_text(encoding="utf-8").splitlines() if "\t" in l)
        version = int(cells.get("update") or 0) if (cells.get("update") or "0").isdigit() else 0
    return [dict(titleId=f"{tid:016X}", baseId=f"{tid:016X}", type="base", version=version, name=name, englishName=english,
                 display=display, guestIsa="unknown", builds={}, contentBytes=total, format=fmt, sha256=sha)]


def nup_inspect(c, source, icon_dir):
    """NUP 容器 → 与读取器 --inspect 同形的包元数据（本体 / 最新更新 / 每个 DLC 各一条，同一原件；出包时 build.ensure_extract 整个容器一起展开）。
    程序号、更新版本、DLC 由 ns2_nup.scan 认（同一键出包）；标题与显示版本取容器里的控制分区（NACP，后面的版本覆盖前面的），没带就用文件名去掉「 [程序号][版本]」
    （同 pack/oneclick/oneclick.sh：ns2_dump 没有控制分区时拿它当标题）。程序位数不在这里判：出包解包后由 nx_unpack --npdm-isa 判（全仓唯一判法）"""
    nup, dump = engine_tool(c, "ns2_nup"), engine_tool(c, "ns2_dump")
    try:
        with nup.Nup(str(source)) as n:
            app, base, vers, dlc = nup.scan(n)
            ver = max(vers, default=0)
            meta = {}
            for k in sorted((k for k in n.packages if k.kind in (nup.BASE, nup.UPDATE)), key=lambda k: (k.kind, k.version)):
                meta.update(k.meta())
            own = {id(k): n.tree_bytes(k.root) for k in [base, vers.get(ver), *dlc.values()] if k is not None}   # 每项各自的内容字节（界面 / 错误摘要逐项显示）
    except (nup.Error, OSError, ValueError) as e:
        # 默认路径：读错误一律 content_invalid（与原来一致）。只有逐来源模式下的磁盘空间不足要照原样往上抛，中止整批。
        if isinstance(e, OSError) and c.r.get("partialResults") is True and explain(e).code == "disk_full":
            raise
        raise Failure(f"读不出这个 NUP 容器（{Path(source).name}）：{e}", code="content_invalid") from None
    nacp = meta.get(nup.T_NACP) if len(meta.get(nup.T_NACP) or b"") == 0x4000 else None
    name = ((dump.nacp_title(nacp, 0) or dump.nacp_title(nacp, 14) or dump.nacp_title(nacp, 13)) if nacp else None) \
        or re.sub(r" \[.*$", "", Path(source).stem)
    display = nacp[0x3060:0x3070].split(b"\0")[0].decode("ascii", "replace") if nacp else ""
    icon = meta.get(nup.T_ICON) or nup.localized_icon(meta.get(nup.T_LICONS, b""))
    if icon:
        (Path(icon_dir) / f"{app:016X}.jpg").write_bytes(icon)
    common = dict(baseId=f"{app:016X}", display=display, guestIsa="unknown", builds={}, format="nup")
    out = [dict(common, titleId=f"{app:016X}", type="base", version=0, name=name, contentBytes=own.get(id(base), 0))]
    if ver:
        out.append(dict(common, titleId=f"{app | 0x800:016X}", type="update", version=ver, name=name, contentBytes=own[id(vers[ver])]))
    out += [dict(common, titleId=f"{tid:016X}", type="dlc", version=k.version, name="", contentBytes=own[id(k)]) for tid, k in sorted(dlc.items())]
    return out


def volume_format(path):
    """游戏库所在卷的文件系统（apfs / exfat / msdos / hfs …，小写）；读不到给空串。路径还不存在时看最近一级已存在的上级目录。
    df 取挂载点，再从 mount 的列表里取这一挂载点的格式（diskutil info 只认挂载点 / 设备，不认普通文件夹）"""
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    if os.name == "nt":   # Windows（11-07 启动器）：卷根 GetVolumePathNameW → 文件系统名 GetVolumeInformationW（ntfs / refs / exfat / fat32 …）
        return _win_volume_format(str(p))
    try:
        mount_point = subprocess.run(["/bin/df", "-P", str(p)], capture_output=True, text=True, timeout=20).stdout.splitlines()[-1].split(None, 5)[5]
        for line in subprocess.run(["/sbin/mount"], capture_output=True, text=True, timeout=20).stdout.splitlines():
            m = re.match(r".+? on (.+) \(([^,)]+)", line)
            if m and m.group(1) == mount_point:
                return m.group(2).strip().lower()
    except (OSError, IndexError, subprocess.TimeoutExpired):
        pass
    return ""


# 游戏原件放哪块盘都行（exFAT 外接盘也行）：导入只记原件的位置与内容哈希，不复制、不改、不删，出包时原地只读（11-02 维护者定）。
# 游戏库 = S2Arm 的工作数据（清单、解包与出包产物、缓存、存档）：Mac 上 APFS（克隆）或 exFAT（普通拷贝 + 不覆盖发布，见 tools/clonedir.py）；
#   FAT32 等仍拦。启动器选到不支持格式的文件夹不拦：说明后游戏库放默认位置；后端 setup 这里是最后一道（直接调接口的也拦）
from libloc import LIBRARY_FORMAT_MESSAGE   # 文案单一来源在 libloc（Mac / Windows 共用的游戏库位置检查）


def source_path(c, package):
    """包的原件位置：导入时记的绝对路径（原件原地只读）；相对路径是 0.1.0-beta.3 及以前复制进游戏库 sources/ 的托管副本（contained 防越界）"""
    path = Path(package["path"])
    return path if path.is_absolute() else contained(c.library, c.library / path)


def stage_sources(c, packages, stage):
    """出包要的原件目录：每个原件软链进来（按内容哈希命名，去重整合版一个 xci 列两次的情况），不复制——原件在外接 exFAT 盘上也不用整份拷。
    解包目录用目录名后缀"""
    stage.mkdir(parents=True, exist_ok=True)
    for p in {p["sha256"]: p for p in packages}.values():
        source = source_path(c, p)
        if source.is_dir():
            os.symlink(source, stage / (p["sha256"] + "_" + source.name))
        else:
            os.symlink(source, stage / (p["sha256"] + source.suffix.lower()))


def _win_volume_format(path):
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        root = ctypes.create_unicode_buffer(1024)
        if not k32.GetVolumePathNameW(ctypes.c_wchar_p(winproc.win_path(path)), root, 1024):
            return ""
        fs = ctypes.create_unicode_buffer(64)
        if not k32.GetVolumeInformationW(root, None, 0, None, None, None, fs, 64):
            return ""
        return fs.value.strip().lower()
    except (OSError, AttributeError, ValueError):
        return ""


def volume_cluster(path):
    """路径所在卷的分配单元（簇）字节数；读不到给 0。Mac：statvfs 的 f_frsize（文件系统基本块；f_bsize 在 macOS 上是建议的读写大小，APFS 读出 1 MB，不能用）；
    Windows：GetDiskFreeSpaceW 的「每簇扇区数 × 每扇区字节数」。路径还不存在时看最近一级已存在的上级目录"""
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    if os.name == "nt":
        try:
            import ctypes
            k32 = ctypes.windll.kernel32
            root = ctypes.create_unicode_buffer(1024)
            if not k32.GetVolumePathNameW(ctypes.c_wchar_p(winproc.win_path(str(p))), root, 1024):
                return 0
            spc, bps, free, total = (ctypes.c_ulong() for _ in range(4))
            if not k32.GetDiskFreeSpaceW(root, ctypes.byref(spc), ctypes.byref(bps), ctypes.byref(free), ctypes.byref(total)):
                return 0
            return spc.value * bps.value
        except (OSError, AttributeError, ValueError):
            return 0
    try:
        return os.statvfs(p).f_frsize
    except OSError:
        return 0


def location_slack(path):
    """存放位置的卷格式、分配单元，以及要不要提示出包会多占空间、为什么（libloc.slack_reason：cluster 大簇 / format 不能克隆或硬链接；只提示不拦）"""
    import libloc
    fmt, cluster = volume_format(path), volume_cluster(path)
    reason = libloc.slack_reason(fmt, cluster, system="Windows" if os.name == "nt" else "Darwin")
    return dict(format=fmt, clusterBytes=cluster, slackWarning=reason is not None, slackReason=reason)

def require_apfs(path, accept=False, extra=(None, 0)):
    """游戏库所在卷的格式：Mac 要 APFS / HFS+ / exFAT（原错误码 library_not_apfs），Windows 要 NTFS / ReFS（library_bad_format，libloc.format_ok）；读不到格式不拦"""
    import libloc
    fmt = volume_format(path)
    if os.name == "nt":
        # exFAT / FAT32：建不了目录链接，产物根走实目录模式、联接退回拷贝（pack/orch/roots.py links_supported），多占空间、断电时产物可能要重出——
        #   认得出的风险，玩家确认（accept）就照用（v0.3.1）；FAT32 单个文件超 4 GB 才是真做不下去，出包开头按 IMG 大小判（require_fat32_fits）
        if not libloc.format_ok(fmt, system="Windows") and not accept:
            raise Failure(libloc.win_format_message(fmt, *extra), code="library_bad_format")
        return fmt
    # Mac 放开 HFS+ 与 exFAT（v0.3.1「拿不准不停」，libloc.MAC_OK_FORMATS；exFAT 来自外部 PR #395）：克隆自动退回普通拷贝（cp -c、core / clonedir / 各工具的 clonefile 都退）；
    #   HFS+ 有 RENAME_EXCL，exFAT 没有（11-12 映像实测 ENOTSUP）——出包链路上的发布处退回「目标不存在才改名」（tools/clonedir.py、pack/app_bundle.py 组 IPA）；
    #   空间预算按不克隆多算一份（space_budget）。FAT32 等单文件 4 GB 上限，照拦
    if not libloc.format_ok(fmt, system="Darwin"):
        raise Failure(LIBRARY_FORMAT_MESSAGE.format(fmt=fmt.upper()), code="library_not_apfs")
    return fmt


def require_local(path):
    """游戏库不能在云盘同步目录里（11-07 启动器，libloc.cloud_folder）：iCloud Drive / OneDrive / Dropbox 等会上传几十 GB 中间产物、
    换成占位文件，出包中途读不到。预检已经提示（toolchain.preflight / winenv），这里是最后一道（直接调接口的也拦）"""
    import libloc
    system = "Windows" if os.name == "nt" else "Darwin"
    cloud = libloc.cloud_folder(str(path), system=system, home=str(Path.home()), env=dict(os.environ), exists=os.path.exists)
    if cloud:
        raise Failure(libloc.CLOUD_MESSAGE.format(cloud=cloud), code="library_in_cloud")


FOREIGN_MESSAGE = ("这个文件夹里已有别的文件。S2Arm 只在自己建的文件和子文件夹里读写，不改也不删它们；"
                   "以后「搬游戏库」时它们会跟着一起搬到新位置。仍要用这个文件夹吗？不想这样就选一个空文件夹。")
MOVE_SKIP = {".DS_Store", ".catalog.lock", ".execution.lock", ".store-meta.lock", ".s2arm-moving"}   # 锁文件要留到搬完；.DS_Store 系统会再生成


def _looks_like_library(root):
    """没有 marker 但装着 S2Arm 旧产物的文件夹：玩家手工拷过来的，或标识丢了的旧游戏库"""
    phones = root / "iPhone"
    return (root / "catalog.json").is_file() or (phones.is_dir() and any(_is_game_dir(p) for p in phones.iterdir()))


def _is_game_dir(p):
    return p.is_dir() and re.fullmatch(r"[0-9A-Fa-f]{16}", p.name) is not None


def _adopt_catalog(root):
    """认领旧产物：catalog.json 能读就用；否则按 iPhone/<游戏号> 和 games/<游戏号> 重建。名字先用游戏号，玩家可以改"""
    existing = read(root / "catalog.json", None)
    if isinstance(existing, dict) and isinstance(existing.get("games"), list):
        return existing
    ids = sorted({p.name.upper() for sub in ("iPhone", "games") if (root / sub).is_dir() for p in (root / sub).iterdir() if _is_game_dir(p)})
    return dict(schema=1, games=[dict(id=i, name=i, display="", packages=[], hidden=False) for i in ids])


def _same_volume(a, b):
    return os.stat(a).st_dev == os.stat(b).st_dev


def _size_of(path):
    if path.is_symlink() or path.is_file():
        return path.lstat().st_size
    return sum((Path(d) / f).lstat().st_size for d, _, files in os.walk(path) for f in files)


def _size_on(path, cluster):
    """拷到分配单元为 cluster 字节的卷上要占的空间：每个文件按簇向上取整、每个目录至少占一簇（exFAT 目录项本身也占簇）；cluster ≤ 0 = 读不到，按原始大小。
    #1112：exFAT 1 MB 簇的外接盘上，92 GB / 21 万个文件的游戏库实际要约 280 GB，按原始大小估「251 GB 够」放行后写到盘满"""
    if cluster <= 0:
        return _size_of(path)
    up = lambda n: -(-n // cluster) * cluster
    if path.is_symlink() or path.is_file():
        return up(path.lstat().st_size)
    total = 0
    for d, dirs, files in os.walk(path):
        total += cluster
        total += sum(up((Path(d) / f).lstat().st_size) for f in files)
    return total


def _sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _files_under(root):
    if root.is_symlink() or root.is_file():
        return [Path(".")]
    return [(Path(d) / f).relative_to(root) for d, dirs, files in os.walk(root) for f in files + [x for x in dirs if (Path(d) / x).is_symlink()]]


def _verify_copy(src, dst, strict):
    """拷完逐个对文件数和大小；存档和清单（strict）再对内容，大文件只对大小，免得搬家多花一倍时间"""
    for rel in _files_under(src):
        a, b = src / rel, dst / rel
        if not (b.exists() or b.is_symlink()) or a.lstat().st_size != b.lstat().st_size and not a.is_symlink():
            raise Failure(f"拷到新位置后校验不一致：{rel}", code="library_move_failed")
        if strict and a.is_file() and not a.is_symlink() and _sha256(a) != _sha256(b):
            raise Failure(f"拷到新位置后校验不一致：{rel}", code="library_move_failed")


def copy_tree_links(src, dst, check=None):
    """跨盘拷一棵目录，链接拷链接本身、不钻进去：软链照拷；Windows 的 junction（Python 里不算软链，shutil.copytree 会钻进去展开成普通目录，
    产物根的「现行」随之失效、出包报「现行必须是目录链接」——#214 整库搬家与 v3 移动游戏同一处）重建成指向同一目标的 junction。
    目标仍指旧位置；放到新位置后由 _retarget_links 改指。
    check：每个目录 / 文件之前调一次（作业的取消检查，#924：原先整个大目录拷完才看一次取消，玩家点了取消还要等几分钟）"""
    src, dst = Path(src), Path(dst)
    isjunction = getattr(os.path, "isjunction", lambda _: False)
    def materialize(a, b):
        """目标盘建不了链接（exFAT / FAT32，v0.3.1 玩家确认过照用）：把链接指向的目录整份拷过来。
        老库产物根的现行链接「<款>@现行」不展开：拷成指针文件（内容 = 指向的版本目录名，pack/orch/current.py；那个版本目录同在这棵树里照拷）"""
        target = Path(os.path.realpath(a))
        if a.name.endswith(layout.CURRENT):
            b.write_text(target.name + "\n", encoding="utf-8")
            return
        copy(target, b)
    def copy(a, b):
        if check:
            check()
        if isjunction(a):
            r = subprocess.run(["cmd", "/c", "mklink", "/J", str(b), os.readlink(a).removeprefix("\\\\?\\")], capture_output=True)
            if r.returncode != 0:
                materialize(a, b)
        elif a.is_symlink():
            try:
                os.symlink(os.readlink(a), b, target_is_directory=a.is_dir())
            except OSError:
                if not a.is_dir():
                    raise
                materialize(a, b)
        elif a.is_dir():
            b.mkdir()
            for child in a.iterdir():
                copy(child, b / child.name)
            shutil.copystat(a, b)
        else:
            shutil.copy2(a, b)
    copy(src, dst)


def _retarget_links(root, old, new):
    """目录搬走后，指向旧位置的绝对链接（Windows 的 junction）改指新位置。
    老库的现行链接（<款>@现行）直接换成指针文件（三个平台现行只用指针文件，pack/orch/current.py）。
    别的链接（版本里 RomFS 指向共享存储）重建不了（玩家诊断 #0fd185：安全软件 / 受控文件夹访问拦重解析点）：不用链接，
    把它指向的目录整目录挪到链接原处（新位置同卷，改名、不拷），见 _links_to_moves"""
    if not root.is_dir():
        return
    from_text, to_text = os.path.normcase(str(old)), str(new)
    failed = []
    for d, dirs, files in os.walk(root):
        for name in dirs + files:
            p = Path(d) / name
            if not _is_link(p):
                continue
            target = os.readlink(p).removeprefix("\\\\?\\")
            if not os.path.isabs(target) or not os.path.normcase(target).startswith(from_text):
                continue
            fresh = to_text + target[len(from_text):]
            if name.endswith(layout.CURRENT):
                _unlink_link(p)
                _write_pointer(p, Path(fresh).name)
                continue
            junction = getattr(os.path, "isjunction", lambda _: False)(p)
            _unlink_link(p)
            try:
                if junction:
                    subprocess.run(["cmd", "/c", "mklink", "/J", str(p), fresh], check=True, capture_output=True)
                else:
                    os.symlink(fresh, p)
            except (OSError, subprocess.CalledProcessError) as e:
                failed.append((p, Path(fresh), e))
    if failed:
        _links_to_moves(failed)


def _write_pointer(p, name):
    """现行指针文件（内容 = 版本目录名一行）：同目录临时文件写好再原子改名，读方只看到旧值或新值"""
    tmp = p.with_name(f".{p.name}.new-{os.getpid()}")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(name + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, p)


def _current_name(roots, game):
    try:
        return (roots / (game + layout.CURRENT)).read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def _links_to_moves(failed):
    """重建不了的链接改成整目录移动（不拷）：链接指向的目录挪到链接原处。同一个目录被几条链接指着（共享存储里一份 RomFS 给现行和上一版共用）
    只能挪给一条：先给各款现行版本，其余几条只删掉链接、打黄——那几个旧版本没有 RomFS 了、不再可用，下次修剪时走"""
    def is_current(p):
        version = p.parent
        game = version.name.split("@", 1)[0]
        return _current_name(version.parent, game) == version.name
    moved = set()
    for p, target, error in sorted(failed, key=lambda x: (not is_current(x[0]), str(x[0]))):
        key = os.path.normcase(str(target))
        if key in moved or not target.is_dir():
            print(f"【黄】{p} 的链接重建不了（{error!r}），它指向的 {target} {'已挪给现行版本' if key in moved else '不在'}：这一版不再可用，下次修剪时删", file=sys.stderr)   # 非玩家句
            continue
        if os.name == "nt":   # 作业主程序在 Windows 上把引擎根放进了 sys.path（main.py），可以取编排层
            from pack.orch.dirrename import rename_dir   # 杀软 / 索引扫描开着句柄时目录改名报 WinError 5：按 #361 同一套退避重试
            from pack.orch.roots import _win_acl
            _win_acl(target, False)   # 共享存储里的身份封存过（拒写），改名前先解封顶层（同 pack/orch/roots.py 收进存储前的做法）
            rename_dir(target, p)
        else:
            os.replace(target, p)
        moved.add(key)
        print(f"【黄】{p} 的链接重建不了（{error!r}）：把 {target} 整目录挪到这里（不拷）", file=sys.stderr)   # 非玩家句


def _unlink_link(p):
    if getattr(os.path, "isjunction", lambda _: False)(p):
        os.rmdir(p)
    else:
        p.unlink()


def _work_root_of(c, library):
    return c.private if library.is_relative_to(c.private) else library / ".s2arm-work"


def _rename_library_path(c, source, target):
    """同盘搬库的一次改名（外部 PR #363）：Windows 上资源管理器 / 杀软 / 下载工具开着里面的文件时改名报 WinError 5 / 32，扫完会放开——
    按 pack/orch/roots.rename_dir 同一套退避（DIR_RENAME_RETRIES，合计约 1 分钟）重试；等的时候在界面上说在等什么、等了多久，能取消（不静默卡住）。
    仍不行报人话，原游戏库保留在原位置。其它错误（盘满等）立即报"""
    # 作业主程序（pack/jobs/main.py）只把 pack/jobs 放进 sys.path：pack.orch 从作业的引擎根导入（#812 换库报 No module named 'pack'；同 toolchain.reader）
    if str(c.engine) not in sys.path:
        sys.path.insert(0, str(c.engine))
    from pack.orch.roots import DIR_RENAME_RETRIES
    delays = DIR_RENAME_RETRIES if os.name == "nt" else ()
    waited = 0.0
    for delay in (*delays, None):
        try:
            os.rename(source, target)
            return
        except PermissionError as error:
            if os.name != "nt" or getattr(error, "winerror", None) not in (5, 32):
                raise
            if delay is None:
                raise Failure(
                    f"Windows 重试约 1 分钟后，游戏库项目“{Path(source).name}”仍被占用，无法改名。"
                    "请关闭正在访问游戏库的资源管理器窗口、杀毒扫描或下载工具后重试；原游戏库仍保留在原位置。",
                    code="library_move_failed",
                ) from error
            c.emit(message=f"游戏库里的「{Path(source).name}」正被别的程序占用（资源管理器、杀毒扫描或下载工具），等它放开再搬（已等 {int(waited)} 秒）")
            c.check()
            time.sleep(delay)
            waited += delay


def _move_pairs(c, source):
    """要搬的 (旧, 新)：游戏库里的每一项，加上放在库外的工作目录（缺省游戏库在私有目录里时，着色器缓存在私有目录的 cache / run 下）。marker 排最后：有它才算搬完"""
    old_work, new_work = _work_root_of(c, source), _work_root_of(c, c.library)
    to_private = new_work == c.private and old_work != c.private   # 工作目录要回到私有目录：按子目录搬，别盖掉已有的；随库走的情况整个目录当普通项搬
    pairs = [(p, c.library / p.name) for p in sorted(source.iterdir(), key=lambda x: x.name == ".s2arm-library.json")
             if p.name not in MOVE_SKIP and not (to_private and p.name == ".s2arm-work")]
    if old_work == c.private and new_work != c.private or to_private:
        pairs[:0] = [(old_work / n, new_work / n) for n in ("cache", "run") if (old_work / n).exists() and not (new_work / n).exists()]
    return pairs


@contextlib.contextmanager
def _source_locked(c, source):
    """搬家期间旧游戏库不能有任务在跑：旧库的执行锁、目录锁按库锁同一语义排队（剃刀 H2：原先拿不到当场报错）"""
    with c.lock("execution", root=source), c.lock(root=source):
        yield


def move_library(c, source):
    """把现有游戏库搬到 c.library（空文件夹）：同盘改名、跨盘拷完校验再换，任何一步失败都回到搬之前。旧位置的内容在新位置就位之后才删"""
    source = resolve_path(source)
    info = read(source / ".s2arm-library.json", None)
    if not isinstance(info, dict) or not info.get("id"):
        raise Failure("原来的位置不是可搬的游戏库。", code="library_move_failed")
    here = resolve_path(c.library)   # c.library 是玩家写的样子（映射盘保留盘符，core.resolve_library_path），和解析过的 source 比要先解析
    if here == source or here.is_relative_to(source) or source.is_relative_to(here):
        raise Failure("新位置不能在原来的游戏库里面，也不能包含它。", code="library_move_failed")
    with store_meta_quiet(c, source), _source_locked(c, source):
        pairs = _move_pairs(c, source)
        c.library.mkdir(parents=True, exist_ok=True)
        cross = [(a, b) for a, b in pairs if not _same_volume(a, c.library)]
        cluster = volume_cluster(c.library)
        need = sum(_size_on(a, cluster) for a, _ in cross)   # 按新位置的簇算实际占用（#1112：exFAT 大簇上小文件多，实际要原始大小的好几倍）
        free = shutil.disk_usage(c.library).free
        if need and free < need:
            big = f"（这块盘的分配单元是 {cluster // 1024} KB，游戏库里小文件多，实际要占约 {need / 1e9:.0f} GB）" if cluster >= (64 << 10) else ""
            raise Failure(f"新位置空间不够，游戏库没有搬：要约 {need / 1e9:.0f} GB，只剩 {free / 1e9:.0f} GB{big}。", code="disk_full")
        stage = c.library / ".s2arm-moving"
        done, staged = [], {}
        try:
            total, copied = max(need, 1), 0
            if cross:
                c.begin("把游戏库拷到新位置")
            for n, (a, b) in enumerate(cross):
                c.check()
                target = stage / str(n)
                target.parent.mkdir(parents=True, exist_ok=True)
                if a.is_dir() and not _is_link(a):
                    try:
                        copy_tree_links(a, target, check=c.check)
                    except FileNotFoundError as error:
                        # #924：拷到一半新位置的临时目录被删了（玩家手动清掉 .s2arm-moving）：说人话；下面的回滚照常把一切放回原样
                        if not stage.is_dir():
                            raise Failure("换库途中，新位置的临时文件夹（.s2arm-moving）被删掉了，这次换库已停下：游戏库还在原来的位置，"
                                          "没有任何改动。要换库就再点一次「换游戏库」；想中途停下请用「取消」，不用手动删文件夹。",
                                          code="library_move_failed") from error
                        raise
                else:
                    shutil.copy2(a, target, follow_symlinks=False)
                _verify_copy(a, target, strict=a.name in ("saves", "catalog.json", ".s2arm-library.json"))
                copied += _size_of(a)
                c.progress(copied, total)
            c.begin("把游戏库放到新位置")
            staged = {a: stage / str(n) for n, (a, _) in enumerate(cross)}
            for a, b in pairs:
                b.parent.mkdir(parents=True, exist_ok=True)
                _rename_library_path(c, staged.get(a, a), b)
                done.append((a, b))
            for root in (c.library, _work_root_of(c, c.library)):
                _retarget_links(root, source, c.library)
        except BaseException as failure:
            if cross and isinstance(failure, OSError) and failure.errno in (errno.ENOSPC, errno.EIO):
                # #1112：exFAT 写满盘失败后删掉残留，空闲空间仍显示 0（无日志的文件系统空闲位图没跟着回来），要跑一次修复才找得回
                # #1112 玩家复核：盘本身没坏（verifyVolume 0），在访达里删的残留进了这块盘自己的废纸篓（.Trashes），要接着盘清倒才腾出空间
                bin_, repair = (("回收站", "在「此电脑」里右键这块盘 →「属性」→「工具」→「检查」") if os.name == "nt"
                                else ("废纸篓", "用「磁盘工具」选中这块盘点「急救」"))
                c.diagnostic(f"换库写到一半失败（盘满或写入出错）。手动删新位置残留的临时文件夹后，要在这块盘还接着时清倒{bin_}，空间才会腾出来；"
                             f"清倒以后可用空间仍明显比实际少，再{repair}检查一次（不会删文件）。", code="library_move_failed", severity="warning")
            if done:
                _retarget_links(c.library, c.library, source)
            for a, b in reversed(done):
                if a in staged:
                    _discard(b)
                else:
                    _rename_library_path(c, b, a)
            try:
                _discard(stage)
            except OSError as error:   # #1112：清临时目录自己出错不许盖住起因（原先盘满的 ENOSPC 被 rmtree 的 FileNotFoundError 盖掉，251 GB 留在盘上没人知道）
                c.diagnostic(f"换库失败后，新位置的临时文件夹没删干净，可以手动删除：{c.redact(str(stage))}（{c.redact(str(error))}）",
                             code="library_move_failed", severity="warning")
            with contextlib.suppress(OSError):
                (c.library / ".s2arm-work").rmdir()   # 回滚后别留下搬的时候建的空目录
            raise
        _discard(stage)
        for a in staged:
            try:
                _discard(a)
            except OSError as error:
                c.diagnostic("游戏库已经搬好，旧位置有文件没删掉，可以手动删除：" + c.redact(str(error)), code="job_failed", severity="warning")  # 非玩家句
    for name in (".execution.lock", ".catalog.lock", ".store-meta.lock", ".DS_Store"):
        (source / name).unlink(missing_ok=True)
    with contextlib.suppress(OSError):
        _discard(source / ".s2arm-work")   # 工作目录回私有目录后剩的，都是能重新生成的
    with contextlib.suppress(OSError):
        source.rmdir()
    return info


def _discard(p):
    if p.is_symlink() or p.is_file():
        p.unlink(missing_ok=True)
    elif p.exists():
        # 解封存放开权限（出包产物有只读目录）：只改真目录、真文件，不跟随链接——os.walk 把指向文件的软链列在 files 里，
        #   os.chmod 会顺着它改到库外（整库搬家收尾删旧位置时，库里链到原件 / 引擎的文件被改成 644）；Windows junction 不进
        junction = getattr(os.path, "isjunction", lambda _: False)
        for d, dirs, files in os.walk(p):
            os.chmod(d, 0o755)
            dirs[:] = [x for x in dirs if not junction(os.path.join(d, x))]
            for f in files:
                if os.path.islink(os.path.join(d, f)):
                    continue
                with contextlib.suppress(OSError):
                    os.chmod(Path(d) / f, 0o644)
        _rmtree_gone_ok(p)


def _rmtree_gone_ok(p):
    """删整棵目录，中途已经不在的项当删完（#1112：exFAT 上 AppleDouble「._」附属文件随主文件一起没了，rmtree 报 FileNotFoundError 半途停下）；
    别的错照抛"""
    def gone(_fn, _path, exc):
        error = exc if isinstance(exc, BaseException) else exc[1]
        if not isinstance(error, FileNotFoundError):
            raise error
    if sys.version_info >= (3, 12):
        shutil.rmtree(p, onexc=gone)
    else:
        shutil.rmtree(p, onerror=gone)


def _within(path, parent):
    return path == parent or parent in path.parents


def _occupied(d):
    """建不了游戏库的文件夹：有系统文件以外的东西，又不是游戏库（没有标识、也不像旧游戏库）"""
    return not (d / ".s2arm-library.json").exists() and layout.has_content(d) and not _looks_like_library(d)


def setup(c):
    """选游戏库（见 _setup）。请求带 acceptCloud = 玩家确认过「游戏库放在云盘同步目录里也继续用」：不拦云盘目录，并把这个选择记进游戏库标识
    （cloudAccepted，libloc.cloud_accepted 读），环境检查之后按提示显示、不再标红（v0.3.1：认得出的风险讲清后果、让玩家选）"""
    result = _setup(c)
    marker = c.library / ".s2arm-library.json"
    if c.r.get("acceptCloud"):
        atomic(marker, dict(read(marker), cloudAccepted=True))
    if c.r.get("acceptFormat"):   # Windows 游戏库在 exFAT / FAT32 上（library_bad_format）也继续用：同上记下，之后不再问
        atomic(marker, dict(read(marker), formatAccepted=True))
    return result


def _no_links_extra(c):
    """游戏库放到建不了目录链接的盘（Windows exFAT / FAT32）会多占多少：按要搬过来的库（moveFrom）或这个文件夹里已有游戏库的款算，
    每款多两份解包大小（space_rule.pack_need_bytes links=False）→ (字节, 款数)；读不到 / 没有游戏给 (None, 0)，文案按每款说"""
    for folder in (c.r.get("moveFrom"), c.library):
        try:
            games = [g for g in json.loads((Path(folder) / "catalog.json").read_text(encoding="utf-8")).get("games", []) if not g.get("hidden")] if folder else []
        except (OSError, ValueError):
            continue
        if games:
            return sum(2 * (pack_input_bytes(selected_packages(g)) * 11 // 10) for g in games), len(games)
    return None, 0


def _setup(c):
    """选游戏库：已有游戏库直接打开；装着旧产物的文件夹认领下来；空文件夹按 moveFrom 把现有游戏库搬过来，没给就新建。
    库路径规则只在这里（剃刀 W19，两端前端只把玩家输入转成绝对路径）：
      盘根 / 用户主目录 / 引擎目录不行；请求带 installDir（启动器安装目录，升级时整个替换）时，库在它里面或包含它都不行；
      请求带 freeDefault（玩家没选过位置，用的是缺省库）而缺省位置被别的文件占着（多半是把 S2Arm 解压到了同名文件夹）：
      换到旁边的「<名字> Library」「<名字> Library 2」…，结果里的 library 是实际用的位置，前端照它记住"""
    c.begin("创建游戏库")
    if c.library == Path(c.library.anchor) or c.library == Path.home() or c.library == c.engine:
        raise Failure("请选择独立的游戏库文件夹，不能直接用整个盘或用户文件夹。")
    install = c.r.get("installDir")
    if install and (_within(resolve_path(c.library), resolve_path(install)) or _within(resolve_path(install), resolve_path(c.library))):
        raise Failure("游戏库不能放在 S2Arm 启动器自己的文件夹里，也不能包含它（升级时这个文件夹会整个替换，库会被一起删掉）：换一个文件夹。")
    if c.r.get("freeDefault") and _occupied(c.library):
        c.library = next((n for n in (c.library.with_name(f"{c.library.name} Library" + (f" {i}" if i > 1 else "")) for i in range(1, 10))
                          if not _occupied(n)), c.library)   # 都被占就原样往下走，由下面报「已有别的文件」
    import libloc
    require_apfs(c.library, accept=bool(c.r.get("acceptFormat")) or libloc.format_accepted(str(c.library)), extra=_no_links_extra(c))
    # 云盘同步目录：认得出的风险，失败码 library_in_cloud，前端问玩家「仍然继续 / 先不」，同意后带 acceptCloud 重发；
    #   这个库确认过（标识里 cloudAccepted）就不再问——启动时连已保存的库也走 setup，否则每次打开都问一遍
    if not c.r.get("acceptCloud") and not libloc.cloud_accepted(str(c.library)):
        require_local(c.library)
    c.library.mkdir(parents=True, exist_ok=True)
    marker = c.library / ".s2arm-library.json"
    if marker.exists():
        info = read(marker)
        if not isinstance(info, dict):
            info = {}
        if info.get("schema") != 1 or not info.get("id"):
            # v0.3.1（认不出的直接做）：标识读坏了不挡打开游戏库——有库号照用；没有就补一个新的（手机上按旧库号记的游戏
            #   之后装机时会当成别的电脑装的，装机那一步本来就会问玩家）；只记一笔
            print(f"[s2a] 游戏库标识不完整（schema={info.get('schema')!r}，有库号={bool(info.get('id'))}），已补全后照常打开", file=sys.stderr)
            info = dict(info, schema=1, id=info.get("id") or str(uuid.uuid4()))
            atomic(marker, info)
        return dict(library=str(c.library), libraryId=info["id"], outcome="opened")
    foreign = layout.has_content(c.library) and not _looks_like_library(c.library)   # 只有系统自动生成的文件（.DS_Store / desktop.ini / Thumbs.db）照样算空文件夹
    if foreign:
        # v0.3.1（认得出的风险讲清后果、让玩家选）：原先直接拒绝。S2Arm 只在自己建的文件和子文件夹里读写，删游戏也只删它自己的文件夹；
        #   失败码 library_folder_not_empty，前端问「仍然继续 / 先不」，同意后带 acceptForeign 重发
        if c.r.get("acceptForeign") is not True:
            raise Failure(FOREIGN_MESSAGE, code="library_folder_not_empty")
        source = c.r.get("moveFrom")
        if source:   # 搬过来的东西和这里已有的重名会盖掉玩家的文件：物理上不能做，说清是哪个
            clash = [b.name for _, b in _move_pairs(c, resolve_path(source)) if b.exists() or b.is_symlink()]
            if clash:
                raise Failure(f"这个文件夹里已有和游戏库里同名的「{'、'.join(clash[:3])}」，搬过来会盖掉它们：换一个文件夹，或者先把它们挪走。文件都没动。",
                              code="library_move_failed")
    elif layout.has_content(c.library):
        info = dict(schema=1, id=str(uuid.uuid4()))
        atomic(c.library / "catalog.json", _adopt_catalog(c.library))
        atomic(marker, info)
        return dict(library=str(c.library), libraryId=info["id"], outcome="adopted")
    source = c.r.get("moveFrom")
    if source:
        info = move_library(c, source)
        return dict(library=str(c.library), libraryId=info["id"], outcome="moved")
    info = dict(schema=1, id=str(uuid.uuid4()))
    atomic(c.library / "catalog.json", dict(schema=1, games=[]))
    atomic(c.library / ".s2arm-layout.json", dict(version=layout.VERSION, games=[]))
    atomic(marker, info)
    return dict(library=str(c.library), libraryId=info["id"], outcome="created")


def validate_keys(path):
    # 玩家把游戏原件当密钥选了进来（诊断 D-HZF3：同一位玩家连选 4 次 .nsp / .nsz，只看到「过大」「格式无效」）：认得出就直说
    if Path(path).suffix.lower() in SOURCE_SUFFIXES:
        raise Failure("选的是游戏文件，不是密钥：这里要的是 prod.keys 文本文件（通常几十 KB）；游戏请从「添加游戏」导入。", code="keys_invalid")
    # 选的文件不在了（被移动 / 删除，或 iCloud 占位文件没下载）：原先直接 stat 抛 FileNotFoundError，落到通用 job_failed，
    #   报成「S2Arm 自己的文件找不到了」，方向说反（诊断#c719ce D-4R5W）
    if not Path(path).is_file():
        raise Failure("选的密钥文件找不到了（可能被移动、删除，或还在 iCloud 里没下载到本机）：请重新选择 prod.keys。", code="keys_invalid")
    if Path(path).stat().st_size > 1 << 20:
        raise Failure("密钥文件过大。", code="keys_invalid")
    entries = {}
    try:
        lines = Path(path).read_text(encoding="utf-8-sig").splitlines()
    except UnicodeError:
        raise Failure("密钥文件格式无效，请选择 prod.keys 文本文件。", code="keys_invalid") from None
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"([A-Za-z0-9_]+)\s*=\s*((?:0x)?[a-fA-F0-9]{2,2048})", line)
        if not m or len(m[2].removeprefix("0x")) % 2:
            raise Failure("密钥文件格式无效，请选择 prod.keys 文本文件。", code="keys_invalid")
        if m[1] in entries and entries[m[1]] != m[2]:
            raise Failure("密钥文件存在冲突条目。", code="keys_invalid")
        entries[m[1]] = m[2].removeprefix("0x")
    if len(entries.get("header_key", "")) != 64:
        raise Failure("缺少有效的 header_key。", code="keys_invalid")
    return len(entries)


def keys(c):
    c.begin("导入玩家密钥")
    source = Path(c.r["path"])
    count = validate_keys(source)
    c.keys.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = c.keys.with_name(c.task + ".candidate")
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as f:
            f.write(source.read_bytes())
        verified = False
        with c.lock("execution"), c.lock():
            candidate = next((source_path(c, p) for g in c.catalog()["games"] for p in selected_packages(g)
                              if p["type"] == "base" and p.get("format") not in KEYLESS_FORMATS and source_path(c, p).is_file()), None)   # NUP 与解包目录不加密，验不了密钥
            if candidate:
                with tempfile.TemporaryDirectory(dir=c.private) as icons:
                    diagnostics_before = len(c.job["diagnostics"])
                    try:
                        inspect(c, candidate, icons, keyfile=temporary)
                    except Failure as error:
                        if error.code not in KEYS_UNVERIFIABLE:   # 源文件自身的毛病只说明这次验不了，不是密钥坏（#242）
                            raise
                        del c.job["diagnostics"][diagnostics_before:]
                    else:
                        verified = True
            c.check()
            os.replace(temporary, c.keys)
            atomic(c.private / "keys/status.json", dict(ready=True, verified=verified, imported=time.time()))
    finally:
        temporary.unlink(missing_ok=True)
    return dict(entries=count, verified=verified, message="已验证解密能力" if verified else "格式通过，首次读取游戏时验证解密能力")


# 候选游戏文件自身有毛病（截断、缺票据）：密钥验不了，但不能据此判密钥无效；导入照常、记 verified=false，源文件错误留到读那款游戏时报
KEYS_UNVERIFIABLE = ("content_truncated", "ticket_missing")
def integrity_skips(lines):
    """读取器的「【校验跳过】阶段\t内容号\t第一处坏块」行 → [{stage, contents, place}]（同一处只记一次，按出现顺序）"""
    out = []
    for line in lines:
        if line.startswith("【校验跳过】"):  # 非玩家句
            stage, _, rest = line[len("【校验跳过】"):].partition("\t")  # 非玩家句
            contents, _, place = rest.partition("\t")
            item = dict(stage=stage.strip(), contents=contents.strip(), place=place.strip())
            if item not in out:
                out.append(item)
    return out


def integrity_skipped(game):
    """这款导入时有内容跳过了完整性校验（任一个包记了 integritySkipped）"""
    return any(p.get("integritySkipped") for p in (game or {}).get("packages", []))


SOURCE_INTEGRITY_FAILED_NOTE = "导入时这款数据校验不过（多半是改版 / 整合版或个别块损坏），这次失败可能与此有关；换一份原版完整的文件再试。"
SOURCE_INTEGRITY_SKIPPED = "这份游戏文件里有数据和原版对不上（多半是改版 / 整合版，或个别块损坏），已跳过完整性校验照常导入。"
KEYS_CANNOT_DECRYPT = ("无法读取游戏内容：可能是文件不完整、缺票据或密钥不匹配。请核对原始文件；"
                       "若已确认 Switch 密钥版本不足，再重新导出，到「设置 → 玩家密钥」重新导入。")


def inspect(c, source, icon_dir, keyfile=None, *, source_id=None):
    keyfile = keyfile or c.keys
    validate_keys(keyfile)
    lines = []
    log_name = f"{c.task}-{source_id}-inspect.log" if source_id else c.task + "-inspect.log"
    # NCZ 解压临时文件落游戏库那块盘（#612：Windows 上游戏库在别的盘、系统盘只剩百来 MB 时，读取器缺省写 %TEMP%，导入报空间不足）
    scratch = tempfile.TemporaryDirectory(prefix=".s2a-inspect-", dir=c.library if c.library.is_dir() else c.private)
    try:
        c.run(
            [
                toolchain.reader(c),
                keyfile,
                "--inspect",
                source,
                icon_dir,
                f"--temp-dir={scratch.name}",
            ],
            c.logs / log_name,
            on_line=lines.append,
        )
    except Failure as error:
        if error.code not in ("tool_exit", "job_failed"):
            raise
        if any(line.startswith("【原因 disk_full】") for line in lines):  # 非玩家句
            raise Failure("读取游戏内容时磁盘空间不足。", code="disk_full") from None
        if "【内容截断】" in lines:  # 非玩家句
            raise Failure("游戏文件不完整，部分内容超出文件末尾。", code="content_truncated") from None
        if any(line.startswith("【原因 ticket_missing】") for line in lines):  # 非玩家句
            raise Failure("缺少内容对应的票据或标题密钥，请检查源文件是否完整。", code="ticket_missing") from None
        if any(line.startswith("【原因 keys_missing】") for line in lines):  # 非玩家句
            raise Failure("现有 prod.keys 缺少读取此文件所需的密钥。", code="keys_outdated") from None
        # 解不开、但同一份文件里同代密钥的别的内容能解开（#1511 再判：玩家换两组密钥同处坏、控制数据能读）：密钥是对的，是文件本身被改过 / 坏了
        if any(line.startswith("【原因 content_damaged】") for line in lines):  # 非玩家句
            raise Failure("这份文件的内容被改过或损坏了：同一份文件里用同一代密钥的其它内容能正常解开，密钥是对的。请换一份原版完整的文件（XCI / NSP）再导入。", code="content_invalid") from None
        # 解出来的数据对不上（分区头魔数不对，#1511）：prod.keys 里这款那一代的密钥不对，同「缺密钥」让玩家重新导出 prod.keys，原话说清是解不开、不是文件被改过
        if any(line.startswith("【原因 keys_wrong】") for line in lines):  # 非玩家句
            raise Failure("现有 prod.keys 解不开这份文件：解出来的数据对不上，多半是这款用的那一代密钥不对。请用升级到最新系统的 Switch 重新导出 prod.keys 再导入。"
                          + key_generation_note(lines), code="keys_outdated") from None
        # 读取器自己给了确定原因（「【原因 hash_mismatch】源文件数据哈希校验不过…换一份原版完整的文件」等，pack/nx_unpack Inspect.cs / Program.cs）：
        #   照它的原话报，不再落到「文件不完整、缺票据或密钥不匹配」三选一的猜测（#1008：本体哈希校验不过，玩家被指去查密钥）
        reason = next((line.split("】", 1)[1].strip() for line in lines if line.startswith("【原因 ") and "】" in line and line.split("】", 1)[1].strip()), "")  # 非玩家句
        if reason:
            raise Failure("无法读取游戏内容：" + reason, code="content_unreadable") from None  # 非玩家句
        if any("could not be decrypted" in line or "Check keys" in line for line in lines):
            raise Failure(KEYS_CANNOT_DECRYPT, code="content_unreadable") from None
        raise
    finally:
        scratch.cleanup()
    if "【内容截断】" in lines:  # 非玩家句
        raise Failure("游戏文件不完整，部分内容超出文件末尾。", code="content_truncated")
    # 读取器哈希不符后只对那份内容跳过完整性校验、照常读出（维护者 11-17：校验不过不再整单拒收，#1467）：每处一行「【校验跳过】阶段\t内容号\t第一处坏块」。
    #   记进这份原件的每个包（integritySkipped），游戏库据此标「数据校验不过」，之后出包 / 起局失败时报错补一句（main.py）；同时发一条 warning 级诊断
    skipped = integrity_skips(lines)
    if skipped:
        c.diagnostic(SOURCE_INTEGRITY_SKIPPED, code="source_integrity_skipped", severity="warning",
                     internal="；".join(" ".join(x for x in s.values() if x) for s in skipped))
    try:
        output = json.loads(next(line for line in reversed(lines) if line.startswith("{")))
        if output.get("schema") != 1 or not output["packages"]:
            raise ValueError()
        for package in output["packages"]:
            if not re.fullmatch(r"[0-9A-F]{16}", package["titleId"]) or not re.fullmatch(r"[0-9A-F]{16}", package["baseId"]):
                raise ValueError()
            if package["type"] not in ("base", "update", "dlc") or not isinstance(package["version"], int):
                raise ValueError()
        if skipped:
            for package in output["packages"]:
                package["integritySkipped"] = skipped
        return output["packages"]
    except (KeyError, ValueError, StopIteration, TypeError):
        raise Failure("内容读取器返回的元数据不完整。", code="content_invalid") from None


def input_identity(game):
    return hashlib.sha256(json.dumps([(p["titleId"], p["version"], p["sha256"])
                                     for p in selected_packages(game)], separators=(",", ":")).encode()).hexdigest()


def _volume(path):
    # 给提示用的磁盘名。Mac：/Volumes/<名> 下就是那块外接盘，否则是系统盘。
    #   Windows：按盘符，系统盘（%SystemDrive%）叫系统盘，别的盘用卷标「卷标（D:）」，取不到卷标叫「D 盘」（11-09：原先只认 /Volumes，Windows 每个盘都显示系统盘）
    if os.name == "nt":
        from pathlib import PureWindowsPath
        drive = PureWindowsPath(str(path)).drive
        if not drive or drive.upper() == os.environ.get("SystemDrive", "C:").upper():
            return "系统盘"
        if drive.startswith("\\\\"):
            return drive
        label = _win_volume_label(drive + "\\")
        return f"{label}（{drive.upper()}）" if label else f"{drive[0].upper()} 盘"
    parts = Path(path).resolve().parts
    return parts[2] if len(parts) > 2 and parts[1] == "Volumes" else "系统盘"


def _win_volume_label(root):
    """Windows 卷标（GetVolumeInformationW）；取不到返回空串"""
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(261)
        ok = ctypes.windll.kernel32.GetVolumeInformationW(ctypes.c_wchar_p(root), buf, 261, None, None, None, None, 0)
        return buf.value.strip() if ok else ""
    except (AttributeError, OSError):
        return ""


# 预检里没算进门槛、也量不准的开销（只列出来给玩家看，不编常数）
SPACE_UNKNOWN = [
    dict(id="shader_cache", role="library", label="着色器缓存", note="着色器缓存另占若干 GB，随游戏与设备而变，未量化，不计入门槛。"),
]


# 出包实测账（11-07 联合开发者01007EF00011E000：外接盘 NX 剩 45 GB，经验门槛要 4x GB 拦下）：每次 Mac 打包 / 打 iPhone 包成功后，
#   量这一款在游戏库所在卷上的整套占用，追加一行到 游戏库/space-usage.tsv（取最后一行，同 tools/disk_admit.py 的 est.tsv）；
#   预检先用同款实测、再用同大小档实测比例 × 本款大小，都没有才退回经验规则。余量 × 1.1 同 维护方工具 need_bytes
SPACE_LEDGER = "space-usage.tsv"
CLONED_DIRS = ("romfs", ".romfs_store")
SPACE_LEDGER_FIELDS = ("time", "gameId", "target", "direct", "inputBytes", "footprintBytes", "name", "format")
SPACE_LEDGER_FORMAT = "2"   # 2 起 RomFS 只算一份、直连缓存是压缩后的（DSZ1）；旧行偏大约 5 倍（一款第一方策略 RPG记 152 GB，实占约 32 GB），不用


def _space_target(c):
    """实测账按目标分：打 iPhone 包 ipa，Mac 打包（build）mac——两边落盘的东西不同（Mac 运行版本整份落 RomFS，iPhone 出 IPA + 资源包）"""
    return "ipa" if getattr(c, "operation", "") == "ipa" else "mac"


class _CloneInfo:
    """APFS 克隆感知（#303）：getattrlist 取每个文件的独占字节 ATTR_CMNEXT_PRIVATESIZE、克隆族 ATTR_CMNEXT_CLONEID、族里份数 ATTR_CMNEXT_CLONE_REFCNT。
    stat 看不出克隆共享块（clonefile / cp -c 出的每份各有 inode、st_blocks 满额），原先照份数累加：宝可梦 Z-A 6.45 GB 的游戏记成 49.9 GB。
    实测（macOS 27，APFS）：没改过的克隆同族同 CLONEID、独占 0；整目录克隆里每个文件也同族；克隆后改过的文件换新 CLONEID、族里份数 1、
    独占 = 改过的块——它和原族共享的块查不出归属，按整份算（偏大、但只出现在改过的克隆上）。取不到（非 macOS / 非 APFS / 老系统）→ None，照旧按 st_blocks"""
    _fn = None

    @classmethod
    def get(cls, path):
        if sys.platform != "darwin":
            return None
        if cls._fn is None:
            import ctypes, ctypes.util, struct
            libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)

            class attrlist(ctypes.Structure):
                _fields_ = [("bitmapcount", ctypes.c_ushort), ("reserved", ctypes.c_ushort), ("commonattr", ctypes.c_uint32),
                            ("volattr", ctypes.c_uint32), ("dirattr", ctypes.c_uint32), ("fileattr", ctypes.c_uint32), ("forkattr", ctypes.c_uint32)]
            # sys/attr.h：ATTR_CMN_RETURNED_ATTRS；扩展通用属性走 forkattr 位并要 FSOPT_ATTR_CMN_EXTENDED
            want = 0x8 | 0x100 | 0x1000   # PRIVATESIZE | CLONEID | CLONE_REFCNT
            req = attrlist(5, 0, 0x80000000, 0, 0, 0, want)
            buf = ctypes.create_string_buffer(64)

            def fn(p):
                if libc.getattrlist(os.fsencode(p), ctypes.byref(req), buf, 64, 0x20 | 0x1):   # FSOPT_ATTR_CMN_EXTENDED | FSOPT_NOFOLLOW
                    return None
                got = struct.unpack_from("5I", buf, 4)[4]
                if got & want != want:
                    return None
                priv, cid, refs = struct.unpack_from("<qQI", buf, 24)
                return priv, cid, refs
            cls._fn = fn
        try:
            return cls._fn(path)
        except (OSError, ValueError):
            return None


def disk_bytes(roots, device, skip_names=(), skip_under=(), state=None):
    """这些目录在 device 卷上实际分配的字节：按 (卷, inode) 去重（硬链接只算一次；多个根互相包含也只算一次），
    不跟软链（原件软链、RomFS 存储链接）、不跨到别的卷（私有目录在内置盘时不计入外接盘游戏库的账）。
    APFS 克隆（#303）：同一克隆族（_CloneInfo）的共享块只算一次——每份记独占字节，族的共享部分取最大一份加一次；
    取不到克隆信息的文件照 st_blocks 算。skip_names / skip_under 只用于按部分分账。
    state（sizes.measure 分部分量一款时传同一个 {}）：几次调用共用「见过的 inode」与「克隆族已记的共享块」——硬链接、克隆族跨部分也只算一次，
    记在第一个碰到它的部分（#329：原先每部分各自去重，imageMac / imageIos / 解包共享的块各算一次，显示比实占多一两倍）"""
    if state is None:
        state = {}
    seen, shared = state.setdefault("seen", set()), state.setdefault("shared", {})   # 克隆族号 → 已记的共享块字节（取各份最大）
    total, stack, grow = 0, [os.fspath(r) for r in roots], {}
    while stack:
        path = stack.pop()
        try:
            st = os.lstat(path)
        except OSError:
            continue
        if st.st_dev != device or stat.S_ISLNK(st.st_mode) or (st.st_dev, st.st_ino) in seen:
            continue
        if os.path.basename(path) in skip_names and any(path.startswith(u + os.sep) for u in skip_under):
            continue
        seen.add((st.st_dev, st.st_ino))
        blocks = getattr(st, "st_blocks", None)
        alloc = blocks * 512 if blocks is not None else st.st_size
        clone = _CloneInfo.get(path) if stat.S_ISREG(st.st_mode) and alloc else None
        if clone and clone[2] >= 2 and 0 <= clone[0] <= alloc:
            total += clone[0]
            if alloc - clone[0] > max(shared.get(clone[1], 0), grow.get(clone[1], 0)):
                grow[clone[1]] = alloc - clone[0]
        else:
            total += alloc
        if stat.S_ISDIR(st.st_mode):
            try:
                with os.scandir(path) as it:
                    stack.extend(e.path for e in it)
            except OSError:
                pass
    for cid, b in grow.items():   # 本次调用让这个族的共享块变大的部分才记到本次
        total += b - shared.get(cid, 0); shared[cid] = b
    return total


def _existing(path):
    """path 或它最近的已存在的上级（还没建出来的工具缓存目录也能取到所在卷）"""
    path = Path(path)
    while not path.exists() and path != path.parent:
        path = path.parent
    return path


def _pack_paths(c, game):
    """(这款的存放位置根, 工具缓存根)，都取到已存在的那一级（取余量 / 判同卷用；P1-165）。没有程序号的游戏记录按默认游戏库"""
    gid = game.get("id")
    return _existing(layout.root_of(c, gid) if gid else c.library), _existing(c.work_root)


def pack_volumes(c, gid):
    """这一款出包会写的目录按卷分组（P1-165：存放位置可以和默认游戏库、工具缓存不在同一块盘）：
    [{role, path（取余量用）, device, roots（量本款占用用）}]。存放位置那块盘：这款的 build/、image/；工具缓存那块盘：本款的直连 / 翻译缓存
    （不含各款共用的编库缓存）。两者同卷就合成一项（role=library）"""
    loc = _existing(layout.root_of(c, gid))
    gdir = layout.find_game(c, gid)
    shared = {c.work_root / "cache" / "air", c.work_root / "cache" / "pso", c.work_root / "run" / "gal_dl_cache"}
    cache_at = _existing(c.work_root)
    vols = {loc.stat().st_dev: dict(role="library", path=loc, device=loc.stat().st_dev, roots=[gdir / "build", gdir / "image"] if gdir else [])}
    cache_dev = cache_at.stat().st_dev
    vols.setdefault(cache_dev, dict(role="cache", path=cache_at, device=cache_dev, roots=[]))
    for p in game_caches(c, gid):
        if p in shared:
            continue
        try:
            dev = p.stat().st_dev
        except OSError:
            continue
        vols.setdefault(dev, dict(role="cache", path=_existing(p), device=dev, roots=[]))["roots"].append(p)
    return list(vols.values())


def pack_footprint(c, gid):
    """这一款出包在所在卷上的整套占用：这款文件夹里出包产生的（出包缓存 build/：共用解包、Mac 各层产物根、iPhone 出包工作目录；
    镜像 image/：Mac 运行版本、iPhone 容器件与资源包），再加私有目录 / 工具缓存里本款的出包缓存（直连 / 翻译…，game_caches）——后者只在同一卷时才算进来。
    不含各款共用的编库缓存（air / pso / gal_dl_cache）、存档、运行时着色器缓存（shader-cache，玩的时候才长）"""
    shared = {c.work_root / "cache" / "air", c.work_root / "cache" / "pso", c.work_root / "run" / "gal_dl_cache"}
    gdir = layout.find_game(c, gid)
    roots = [gdir / "build", gdir / "image"] if gdir else []
    roots += [p for p in game_caches(c, gid) if p not in shared]
    # APFS 上解包、各层产物根、.romfs_store、Mac 运行版本里的 RomFS 都是同一份的克隆：disk_bytes 按克隆族只算一次（#303）；Windows（NTFS 无克隆）照实算
    return disk_bytes(roots, layout.root_of(c, gid).stat().st_dev)   # 这款可能在别的存放位置（别的卷）


def record_pack_usage(c, game):
    """出包成功后（清中间产物之前）记一行实测：程序号、目标、直连与否、输入大小、整套占用。量不出来不影响这次出包，只少一行账"""
    try:
        size = pack_input_bytes(selected_packages(game))
        if size <= 0:
            return None
        footprint = pack_footprint(c, game["id"])
        path = c.library / SPACE_LEDGER
        name = re.sub(r"[\t\r\n]+", " ", game.get("name") or "")
        row = [time.strftime("%Y-%m-%dT%H:%M:%S"), game["id"], _space_target(c), "1", str(size), str(footprint), name, SPACE_LEDGER_FORMAT]
        with path.open("a", encoding="utf-8") as f:
            if f.tell() == 0:
                f.write("# " + "\t".join(SPACE_LEDGER_FIELDS) + "\n")
            f.write("\t".join(row) + "\n")
        return footprint
    except (OSError, KeyError, TypeError, ValueError):   # 清单记录不全 / 盘读不了：只少这一行账
        return None


def _gb(n):
    return f"{n / 1e9:.0f} GB" if n >= 10e9 else f"{n / 1e9:.1f} GB"


def nup_unpack_bytes(c, packages, cluster):
    """NUP 原件解成的解包目录（pack/ns2_nup.py --to-extract → pack/ns2_dump.py）在分配单元为 cluster 字节的卷上实际要占多少：读容器目录表（不解包，
    时之笛 25 GB 的 NUP 约 0.4 秒），按 ns2_dump 的同一套选法——exefs 取最新更新的（没有取本体）；romfs 全量用更新、差量本体叠更新；镜像形态（缺省）
    romfs 是一个 romfs.std 文件，S2A_ROMFS_IMAGE=0 是散文件；DLC 一律散文件——每个文件按簇向上取整、每个目录占一簇（同 _size_on，#1112）。
    cluster ≤ 0（读不到）按原始大小。没有 NUP / 读不了 → None（预算照原件大小算）。#444：exFAT 1 MB 簇的外接盘上，解包中间目录与解包目录两份
    共 5 万个文件、13.4 GB，实占 59 GB；按原件 8.9 GB × 1.1 估出 9.8 GB 放行，出到一半盘满"""
    main = next((p for p in packages if p.get("format") == "nup" and p["type"] == "update"), None) \
        or next((p for p in packages if p.get("format") == "nup"), None)
    if main is None:
        return None
    nup, dump = engine_tool(c, "ns2_nup"), engine_tool(c, "ns2_dump")
    up = (lambda n: -(-n // cluster) * cluster) if cluster > 0 else (lambda n: n)
    one = max(cluster, 0)   # 一个目录占的簇
    try:
        with nup.Nup(str(source_path(c, main))) as n:
            _, base, vers, dlc = nup.scan(n)
            upd = vers.get(max(vers, default=0))

            def alloc(d):   # 目录 d 下整棵树作散文件写盘的实占（不含 d 自己）
                return sum(alloc(i) + one if isdir else up(n.files[i][1]) for _, isdir, i in n.children(d))

            def files(d, pre=""):   # 相对路径 → (字节, SHA-256)：ns2_dump.same_file 判全量 / 差量用
                out = {}
                for raw, isdir, i in n.children(d):
                    rel = pre + nup.safe_name(n, raw)
                    if isdir:
                        out.update(files(i, rel + "/"))
                    else:
                        out[rel] = (n.files[i][1], "", 0, 0, "", n.files[i][2].hex())
                return out
            ex = upd if upd is not None and upd.exefs != nup.NO_DIR and n.child(upd.exefs, b"main") is not None else base
            total = one + (alloc(ex.exefs) + one if ex is not None and ex.exefs != nup.NO_DIR else 0)
            rb = files(base.romfs) if base is not None and base.romfs != nup.NO_DIR else {}
            ru = files(upd.romfs) if upd is not None and upd.romfs != nup.NO_DIR else {}
            rom = ru if ru and (not rb or dump.same_file(rb, ru)) else {**rb, **ru}
            if dump.image_mode():
                total += up(sum(v[0] for v in rom.values()))
            else:
                dirs = set()   # romfs/ 下每一级目录各占一簇
                for rel in rom:
                    parts = rel.split("/")[:-1]
                    dirs.update("/".join(parts[:k]) for k in range(1, len(parts) + 1))
                total += sum(up(v[0]) for v in rom.values()) + one * (len(dirs) + 1)
            total += sum(alloc(k.romfs) + one for k in dlc.values() if k.romfs != nup.NO_DIR)
            return total
    except (nup.Error, OSError, ValueError):
        return None


def _unpacked_on_disk(c, game):
    """这款的解包目录已完整在盘上（物理事实，不是历史记录）"""
    from build import extract_valid   # build 顶层 import 本模块，这里函数内取
    return "id" in game and extract_valid(c, game)


def space_budget(c, game):
    """打包前的空间预算（纯计算，不拦）：只有一个数，出包页显示和拦截都用它；公式见 pack_need_bytes。space-usage.tsv 只记账校准常量，不进判断。只写盘名，不写绝对路径。"""
    import shutil as _sh
    packages = selected_packages(game)
    size = pack_input_bytes(packages)
    unpacked = _unpacked_on_disk(c, game)
    # P1-165：解包 / 产物写在这款的存放位置，直连缓存写在工具缓存；两块盘各按自己那份需求、自己的余量判（同卷合成一项，同原来）
    direct_need = pack_direct_bytes(size)
    image = getattr(c, "operation", None) == "ipa"   # 打 iPhone 包写 game.img（写在存放位置）；Mac 打包不写
    loc, cache = _pack_paths(c, game)
    fmt = volume_format(loc)
    clones = False if os.name != "nt" and fmt and fmt != "apfs" else None   # 读到是非 APFS（HFS+ 等）才按不克隆多算一份；读不到按 APFS（不多算、不拦）
    links = not (os.name == "nt" and fmt and fmt not in ("ntfs", "refs"))   # Windows exFAT / FAT32：建不了目录链接，多两份（space_rule）
    keep = getattr(c, "r", {}).get("followUp") is True   # 同一款接着还要出别的平台：解包留着，写 IMG 不边写边删（ipa.to_img）
    cluster = volume_cluster(loc)
    tree = nup_unpack_bytes(c, packages, cluster)   # NUP：解包目录在存放位置那块盘上的实占（按分配单元）；别的格式 None = 按原件大小
    copy_need = pack_need_bytes(packages, unpacked=unpacked, image=image, clones=clones, links=links, keep_unpack=keep, unpack_bytes=tree) - direct_need
    one = (size if tree is None else tree) * 11 // 10   # 解包目录一份
    by_cluster = tree is not None and cluster >= (64 << 10) and not unpacked   # 大簇盘上 NUP 按实占算：说明里写出分配单元
    img_extra = image and (keep or unpacked)
    # 分项之间用「；」：两端翻译按「；」拆开逐项翻（BackendText.translate），用「，」会整串翻不了（11-14 i18n）
    unpack_note = (f"解包 {_gb(one)}（按这块盘 {cluster // 1024} KB 的分配单元逐文件算）" if by_cluster else f"解包 {'已在盘上' if unpacked else _gb(one)}") + \
        (f"；单一 IMG {_gb(size)}" if img_extra else "；单一 IMG 边写边腾解包、不另占" if image else "") + \
        (f"；{fmt.upper()} 盘不能克隆、资源包另占 {_gb(one)}" if clones is False else "") + \
        (f"；{fmt.upper()} 盘建不了目录链接、另占 {_gb(2 * one)}" if not links else "")
    direct_note = f"直连着色器上限 {_gb(direct_need)}"
    if loc.stat().st_dev == cache.stat().st_dev:
        parts = [("library", loc, copy_need + direct_need, f"{unpack_note}；{direct_note}")]
    else:
        parts = [("library", loc, copy_need, unpack_note), ("cache", cache, direct_need, direct_note)]
    volumes = []
    # 这款上一次出的 iPhone 资源包（image/ios/.pack）：这次出完会被新包换掉。空间不够时算进「能腾出的」，玩家点「仍然继续」后先删掉它（#746：
    #   v0.2.x 升级上来的旧包 29 GB 一直占着、预估不抵扣）。只在写 IMG 的那块盘（存放位置）上算
    old = layout.ios_pack(c, game["id"].upper()) if image and "id" in game else None
    old_bytes = _part_bytes([old]) if old is not None and old.is_dir() else 0
    for role, path, need, note in parts:
        free = _sh.disk_usage(path).free
        reclaim = old_bytes if role == "library" else 0
        volumes.append(dict(role=role, volume=_volume(path), needBytes=need, freeBytes=free, enough=free >= need,
                            reclaimBytes=reclaim, note=note, unknown=["shader_cache"], basis="rule"))
    return dict(gameBytes=size, volumes=volumes, unknown=[dict(u) for u in SPACE_UNKNOWN])


FAT32_FILE_LIMIT = (4 << 30) - 1   # FAT32 单个文件上限（4 GiB − 1 字节）


def ntfs_drives():
    """Windows 本机能放游戏库 / 存放位置的盘（NTFS / ReFS）：[(盘根, 格式, 剩余字节)]，剩余大的在前；不是 Windows 给空"""
    if os.name != "nt":
        return []
    import ctypes, string
    out = []
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    for i, letter in enumerate(string.ascii_uppercase):
        if not mask >> i & 1:
            continue
        root = f"{letter}:\\"
        fmt = _win_volume_format(root)
        if fmt in ("ntfs", "refs"):
            try:
                out.append((root, fmt.upper(), shutil.disk_usage(root).free))
            except OSError:
                pass
    return sorted(out, key=lambda d: -d[2])


def require_fat32_fits(c, game, loc):
    """FAT32 存放位置：单个文件不能超过 4 GB。打 iPhone 包最后写一个单一 IMG（约等于游戏内容大小），写之前就能算出来——超了物理上做不下去，
    出包开头就停，列出本机 NTFS 盘让玩家换存放位置（v0.3.1：不等写到一半才失败）；没超照常出"""
    if os.name != "nt" or getattr(c, "operation", None) != "ipa" or volume_format(loc) not in ("fat32", "fat", "fat16"):
        return
    size = pack_input_bytes(selected_packages(game))
    if size <= FAT32_FILE_LIMIT:
        return
    drives = "、".join(f"{root}（{fmt}，剩余 {_gb(free)}）" for root, fmt, free in ntfs_drives()[:4])  # 非玩家句
    raise Failure(f"这款游戏的 iPhone 资源包要写成一个约 {_gb(size)} 的文件，存放位置所在的 FAT32 盘单个文件最多 4 GB：在出包页「存放位置」换到 NTFS 格式的盘"  # 非玩家句
                  + (f"（这台电脑上可用：{drives}）" if drives else "（这台电脑上没找到 NTFS 盘：接上一块 NTFS 盘，或把一块盘格式化成 NTFS——会清空那块盘）") + "，再出包。",  # 非玩家句
                  code="fat32_file_too_large")


def check_pack_space(c, game, *, compiler_policy=None):
    """打包前的磁盘预检（Mac 打包与打 iPhone 包共用）：IPA 可先按已有保留规则尝试回收；发最终 space 事件（存任务文件），再按预算拦。
    预估不够一律不拦、只记一条警告（11-14 不许堵门）；请求带 spaceOverride: true 且这款有上一次的 iPhone 包时先删掉它腾地方（旧版失败页「仍然继续」）。
    真写满由出包中途的磁盘写满识别报错"""
    require_fat32_fits(c, game, _pack_paths(c, game)[0])
    budget = space_budget(c, game)
    cache_result = None
    if compiler_policy is not None:
        from cache_retention import trim
        # 缓存只释放工具缓存所在卷（默认位置的 tool-cache，游戏库结构 P3）；不同卷的 library 不借用这笔 free。
        # 删缓存只释放工具缓存所在卷：存放位置也在这块盘才帮得上全部各项，否则只帮得上 cache 那一项（P1-165）
        loc, cache = _pack_paths(c, game)
        same = loc.stat().st_dev == cache.stat().st_dev
        relevant = budget["volumes"] if same else [v for v in budget["volumes"] if v["role"] == "cache"]
        target = max([compiler_policy["min_free_bytes"], *(v["needBytes"] for v in relevant)])
        if any(not v["enough"] for v in budget["volumes"]):
            # 若不足的是另一卷，只统计缓存，不为它删除本卷冷条目。
            floor = target if any(not v["enough"] for v in relevant) else 0
            cache_result = trim(c.work_root, current_gid=game["id"],
                                **dict(compiler_policy, min_free_bytes=floor))
            from cache_retention import trim_build_caches
            trim_build_caches(c.work_root / "run" / "s2arm", current_gid=game["id"])   # 盘紧时连带出包可重建缓存（aoc 等）按总量上限回收
            budget = space_budget(c, game)
            budget["compilerCache"] = cache_result
        budget["compilerMinFreeBytes"] = target
    override = getattr(c, "r", {}).get("spaceOverride") is True
    if override and any(not v["enough"] for v in budget["volumes"]):
        budget["override"] = True   # 失败页据此不再给「仍然继续」（这次已经是继续过的）
    c.emit("space", "空间预检", space=budget)
    c.space_budget = budget   # 出到一半写满时，失败原因据此说清是不是预估偏低（errors.estimate_low，Context.describe_failure）
    for v in budget["volumes"]:
        if v["enough"]:
            continue
        need, free, reclaim = v["needBytes"], v["freeBytes"], v.get("reclaimBytes", 0)
        where = "" if len(budget["volumes"]) == 1 else ("游戏存放位置所在的盘" if v["role"] == "library" else "工具缓存所在的盘")
        # 分项（#746：原先只给一个大数，玩家不知道为什么从 48 GB 跳到 106 GB、该清什么）
        note = f"（{v['note']}）" if v.get("note") else ""
        head = f"磁盘空间不够：{where}这次出包预计要 {need / 1e9:.0f} GB{note}，现在只剩 {free / 1e9:.0f} GB。"  # 非玩家句
        if override:
            if reclaim:   # 失败页说过「仍然继续会先删掉它」：旧包和新包同时在盘上的那段是峰值的一部分，先删才腾得出来
                _rm(c, layout.root_of(c, game["id"].upper()), layout.ios_pack(c, game["id"].upper()))
                c.diagnostic(head + f"已按你的选择先删掉这款上一次的 iPhone 包，腾出 {_gb(reclaim)}。", code="disk_full", severity="warning")  # 非玩家句
                continue
            c.diagnostic(head + "已按你的选择仍然继续。", code="disk_full", severity="warning")  # 非玩家句
            continue
        # 预估只当提示、不拦（不许堵门，11-14 定：预估偏保守，玩家盘可能其实够）：记一条警告照常开工；真写满时写盘报 ENOSPC，那才停、归 disk_full
        c.diagnostic(head + "这是预估，照常开工；真写满时会停下，释放空间后重试即可。"  # 非玩家句
                     + (f"这款上一次的 iPhone 包（{_gb(reclaim)}）出完会被新包换掉。" if reclaim else ""), code="disk_full", severity="warning")  # 非玩家句

    return budget


def guest_isa(selected):
    # 真正跑的程序的指令集：最新更新的 ExeFS 是完整的，读得出就以它为准；读不出（unknown）才看本体。
    # 有的游戏本体 32 位、更新后 64 位（0100152000022000 1.0 AArch32 → 4.0.1 AArch64，10-23 一键出包曾按本体误拒）
    upd = [p for p in selected if p["type"] == "update" and p.get("guestIsa") in ("aarch64", "aarch32")]
    if upd:
        return upd[0]["guestIsa"]
    return next((p.get("guestIsa") for p in selected if p["type"] == "base"), "unknown")


def requirements(c, game):
    return [str(error) for error in requirement_errors(c, game)]


def require_game(c, game):
    errors = requirement_errors(c, game)
    # 缺 prod.keys：出包开头当场核（v0.3.1：前端不再按游戏库读数里的 keysReady 置灰，读数可能是旧的——刚导入完 keys 回来仍是灰的）。
    #   加密原件没 keys 读不了，物理上做不下去：停、说清去哪导入。不进 requirement_errors：那是游戏库读数里的「缺文件」，keys 不属于这一款。
    #   原件都还没齐时不再报（先把文件找齐，一次只说该先做的那件）
    if not errors and needs_keys(selected_packages(game)) and not c.keys.is_file():
        errors.append(Failure("这款游戏的原件要 prod.keys 才能读：先在「设置 → 玩家密钥」导入 prod.keys，再出包。", code="keys_missing"))
    if errors:
        for error in errors:
            c.report_failure(error)
        raise errors[-1]
    wait_readable(c, [source_path(c, p) for p in selected_packages(game)])
    wait_writable(c, c.work_root)


def _wait_probe(c, what, probe, poll):
    """后台线程跑 probe（读 / 写一下，系统的文件访问授权框由它触发）；每 poll 秒没回来就在界面上说在等什么、等了多久。返回 probe 抛的 OSError，没有返回 None"""
    import threading
    box = {}
    def run():
        try: probe()
        except OSError as e: box["err"] = e
    t = threading.Thread(target=run, daemon=True); t0 = time.monotonic(); t.start()
    while not t.join(poll) and t.is_alive():
        c.check()
        c.emit(message=f"在等系统的文件访问授权：{what}（已等 {int(time.monotonic() - t0)} 秒）。"  # 非玩家句
                       "请在系统弹出的「允许访问」对话框里点「允许」；看不到对话框就把它从别的窗口后面切出来")
    return box.get("err")


def wait_writable(c, path, poll=5.0):
    """开工前往出包工作目录（缓存 / 产物根，跟着游戏库走）试写一次（11-09：外接盘第一次写同样会弹系统的「可移除的宗卷」授权框——
    框没点，写就停在系统调用里不动（手测推包挂了 33 分钟）；框被拒，后面第一个写库盘的阶段才失败（玩家只见「拷字体失败」）。
    建目录、写一个探针文件再删；等的时候界面说清在等什么；系统拒绝报 permission_denied 并说去哪里打开，其余错误交给 errors.explain 归类"""
    def probe():
        path.mkdir(parents=True, exist_ok=True)
        f = path / f".s2a-write-probe.{os.getpid()}"
        try: f.write_bytes(b"s2a")
        finally: f.unlink(missing_ok=True)
    e = _wait_probe(c, f"写 {path.name}", probe, poll)
    if isinstance(e, PermissionError):
        if os.name == "nt":   # D-4MAH：Windows 没有「系统设置 › 隐私与安全性」，下一步见 errors.WINDOWS_RECOVERY
            raise Failure(f"系统不让 S2Arm 往游戏库所在的磁盘写（{path}）", code="permission_denied")
        raise Failure(f"系统不让 S2Arm 往游戏库所在的磁盘写（{path}）：到「系统设置 › 隐私与安全性 › 文件与文件夹」"  # 非玩家句
                      "（外接盘看「可移除的宗卷」）里打开 S2Arm，再点一次", code="permission_denied")
    if e is not None:
        raise e


def wait_readable(c, paths, poll=5.0):
    """开工前把每个原件读一下（S2Arm 10-31 出包管线深修 4.5：游戏库在「下载」/ 外接盘时，第一次读会弹系统的文件访问授权框，
    框被别的窗口挡住，读就一直停在系统调用里——零 CPU、零磁盘，界面停在某一步不动；10-31 一位开发者冷出勇者斗恶龙 XI S 停了 15 分钟以上）。
    每个原件在后台线程试读开头 4 KB；每 poll 秒没读到就在界面上说「在等什么、等了多久」；系统拒绝（权限错误）直接报出去哪里打开。
    解包目录（format=dump/extract/loose）：dump 读 base/exefs/main，其余读 exefs/main"""
    for path in paths:
        # 判断是否解包目录：目录且有 exefs/main 或 base/exefs/main
        probe_path = path
        if path.is_dir():
            if (path / "base" / "exefs" / "main").is_file():
                probe_path = path / "base" / "exefs" / "main"
            elif (path / "exefs" / "main").is_file():
                probe_path = path / "exefs" / "main"
        def probe(path=probe_path):
            if os.path.isdir(path):
                os.listdir(path)   # 普通文件夹（导入时玩家选的）：列一下，系统的文件访问授权框由这一步触发
            else:
                with open(path, "rb") as f: f.read(4096)
        e = _wait_probe(c, f"读 {path.name}", probe, poll)
        if e is not None:
            if isinstance(e, PermissionError):
                if os.name == "nt":   # D-4MAH：同上，Windows 的下一步见 errors.WINDOWS_RECOVERY
                    raise Failure(f"系统不让 S2Arm 读 {path.name}（{path.parent}）", code="input_permission")
                raise Failure(f"系统不让 S2Arm 读 {path.name}（{path.parent}）：到「系统设置 › 隐私与安全性 › 文件与文件夹」"  # 非玩家句
                              "或「完全磁盘访问权限」里打开 S2Arm，再点一次", code="input_permission")
            raise Failure(f"读不了游戏文件 {path.name}：{e}", code="input_unreadable")


def lacks_base(game):
    """只有更新 / DLC、没有本体（出包的 input_base_missing 与添加时的提示同一个判据，#797）"""
    return not any(p["type"] == "base" for p in selected_packages(game))


def requirement_errors(c, game):
    missing = []
    selected = selected_packages(game)
    if lacks_base(game):
        missing.append(Failure("缺游戏本体：只放了更新或 DLC，把本体的 NSP / XCI 也放进来", code="input_base_missing"))
    if guest_isa(selected) == "aarch32":
        missing.append(Failure("这款游戏是 32 位程序，S2Arm 暂时只支持 64 位游戏，打不了包", code="guest_isa_unsupported"))
    for p in selected:
        source = source_path(c, p)
        # 解包目录（format=dump/extract/loose）是目录，其余原件是文件
        is_extract = p.get("format") in ("dump", "extract", "loose")
        if not (source.is_dir() if is_extract else source.is_file()):
            missing.append(Failure(f"找不到游戏文件「{source.name}」：它所在的磁盘没接上，或文件被挪走了。接上磁盘，或重新添加这款游戏"
                                   if Path(p["path"]).is_absolute() else "这款游戏的文件找不到了，请重新添加游戏文件。", code="input_missing"))
    return missing


def source_identity(packages, size):
    """原件身份（catalog 里的 sha256，64 位十六进制，判重 / 构建键 / 解包暂存名都用它）：按读取器给的各包 titleId、类型、版本与 NCA content ID
    （任天堂按内容算的哈希）再加文件大小算，不读全文件——原先整份算 SHA-256，十几 GB 的原件放在机械盘 / U 盘上「只是添加」就要几分钟（11-09 玩家反馈）。
    一个文件一个身份（同一个 XCI 里的本体与更新共用，下游按它去重）。读取器没给 content ID（NUP 容器、旧读取器）→ None，调用方照旧整份算"""
    rows = []
    for m in packages:
        ids = m.get("contentIds")
        if not isinstance(ids, list) or not ids or not all(isinstance(x, str) and re.fullmatch(r"[0-9a-f]{32}", x) for x in ids):
            return None
        rows.append([m["titleId"], m["type"], m["version"], sorted(ids)])
    return hashlib.sha256(json.dumps(dict(v=1, packages=sorted(rows), size=size), separators=(",", ":")).encode()).hexdigest()


def _same_package(p, metadata, sha, size):
    """已在库里的同一份：身份相同；或 titleId、类型、版本、文件大小都相同——老记录的 sha256 是整份哈希、新导入按 content ID 算，两种都要认得
    （老记录不改写：它进构建键，改了老游戏都得冷重出）"""
    if p["titleId"] != metadata["titleId"]:
        return False
    return p["sha256"] == sha or (p.get("type") == metadata["type"] and p.get("version") == metadata["version"] and p.get("size") == size)


# Errors attributable to one source. Other failures stop the whole batch.
IMPORT_SOURCE_ERRORS = {
    "content_truncated", "content_unreadable", "content_invalid", "keys_outdated", "ticket_missing",
    "input_missing", "input_type_unsupported", "input_empty", "input_permission",
    "input_unreadable", "permission_denied",
}


def import_source(path, *, needs_keys=False):
    return dict(id=hashlib.sha256(os.fsencode(path)).hexdigest(), path=str(path), name=path.name,
                fileName=path.name, needsKeys=needs_keys, status="unprocessed", code=None, reason="", diagnostic="")


# 单个来源写进作业记录的诊断上限（字符）。超出时保留开头与结尾，中间标注已省略。
IMPORT_DIAGNOSTIC_LIMIT = 100_000


def cap_diagnostic(text, limit=IMPORT_DIAGNOSTIC_LIMIT):
    if len(text) <= limit:
        return text
    marker = "\n…[已省略中间部分]…\n"  # 非玩家句
    keep = limit - len(marker)
    head = keep // 4
    return text[:head] + marker + text[len(text) - (keep - head):]


def import_source_failed(c, row, error):
    failure = explain(error)
    if c.r.get("partialResults") is True and isinstance(error, OSError) and error.errno in (errno.EIO, errno.ENODEV):
        failure = Failure("读不了游戏文件。", code="input_unreadable", internal=str(error))
    detail = [str(failure), failure.internal]
    if c.last_log:
        try:
            detail.append(c.last_log.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    row.update(status="failed", code=failure.code, reason=c.redact(failure),
               diagnostic=cap_diagnostic(c.redact("\n".join(filter(None, detail)).replace(row["path"], row["fileName"]), limit=None)))


def scan_import(c, picked, *, partial):
    if picked.is_dir():
        files, extracts, errors, seen = [], [], {}, set()

        def walk_error(error):
            if partial:
                if explain(error).code == "disk_full":
                    raise error
                errors[Path(error.filename or picked)] = error

        # Keep paths inside the picked folder, while guarding against symlink loops.
        for directory, dirs, names in os.walk(picked, followlinks=True, onerror=walk_error):
            real = os.path.realpath(directory)
            if real in seen:
                dirs[:] = []
                continue
            seen.add(real)
            path = Path(directory)
            if is_extracted_game(path):
                extracts.append(path)
                dirs[:] = []
            else:
                files.extend(path / name for name in names if name.lower().endswith(SOURCE_SUFFIXES)
                             and not layout.os_junk(name)   # exFAT 盘上访达留下的 ._游戏.nsp 附属文件不是原件（v0.2.3 清理）
                             and os.path.isfile(path / name)
                             and (not name.lower().endswith(NUP_SUFFIX) or is_nup(c, path / name)))
        if files or extracts or errors:
            return files, extracts, errors
        raise Failure("所选位置没有 NSP、XCI 文件或解包好的游戏文件夹。", code="input_empty")
    if picked.is_file() and picked.suffix.lower() in SOURCE_SUFFIXES and (
            picked.suffix.lower() != NUP_SUFFIX or is_nup(c, picked)):
        return [picked], [], {}
    if not picked.exists():
        raise Failure("所选游戏文件或文件夹不存在。", code="input_missing")
    raise Failure("请选择 NSP、XCI 文件、解包好的游戏文件夹或包含它们的文件夹。", code="input_type_unsupported")


def record_import_packages(c, cat, source, packages, icons, sha, extracted):
    """一份来源读出的各包记进 catalog（cat 是草稿，调用方写盘）：判重同 main（原件按 _same_package 认老整份哈希与新 content ID 身份、
    解包目录按 sha256 + titleId）；新包记英文名、按英文名建这款文件夹放封面（_copy_cover，不按游戏号拼路径）"""
    results = []
    size = None if extracted else source.stat().st_size
    for metadata in packages:
        package_sha = metadata.get("sha256", "") if extracted else sha
        tid = metadata["baseId"]
        game = next((g for g in cat["games"] if g["id"] == tid), None)
        if not game:
            game = dict(id=tid, name=metadata["name"] or tid, display="", packages=[], hidden=False)
            cat["games"].append(game)
        game["hidden"] = False
        if extracted:
            same = [p for p in game["packages"] if p["sha256"] == package_sha and p["titleId"] == metadata["titleId"]]
        else:
            same = [p for p in game["packages"] if _same_package(p, metadata, package_sha, size)]
        for p in same:
            # 同一份内容换了位置再放一次：改记新位置；复制进库的旧托管副本还在就照旧用
            old = source_path(c, p)
            if Path(p["path"]).is_absolute() or not (old.is_dir() if extracted else old.is_file()):
                p["path"] = str(source)
        if not same:
            icon = Path(icons) / (metadata["titleId"] + ".jpg")
            english_name = metadata.pop("englishName", "") or ""
            if english_name and not game.get("englishName"):   # #765：旧记录里英文名是空串也补上（原先只判键在不在，空串挡住新读到的英文名，文件夹落成游戏号）
                game["englishName"] = english_name
            if icon.is_file():
                try:
                    _copy_cover(c, tid, game, metadata, icon)
                except (OSError, Failure) as error:   # Failure：如这款文件夹所在位置没连上（layout.game_dir）
                    # #765：封面只是显示用，写不进不让整个添加失败（原先报「游戏库封面无法写入」、这款进不了库）——记一笔，库里照常显示（没封面用缺省图）
                    print(f"[s2a] 添加 {tid}：封面写不进，照常添加（{error}）", file=sys.stderr)
            metadata.pop("icon", None)
            package_size = metadata.get("contentBytes", 0) if extracted else size
            game["packages"].append(metadata | dict(path=str(source), sha256=package_sha, size=package_size))
            if metadata["name"]:
                game["name"] = metadata["name"]
        results.append(dict(id=tid, name=source.name, duplicate=bool(same)))
    return results


def initial_import_result(c):
    paths = dict.fromkeys(Path(os.path.abspath(Path(raw).expanduser())) for raw in c.r["paths"])
    return dict(imports=[], sources=[import_source(path, needs_keys=path.suffix.lower() in SOURCE_SUFFIXES[:-1])
                                    for path in paths])


def import_games(c):
    partial = c.r.get("partialResults") is True
    c.import_result = initial_import_result(c)
    sources = {Path(row["path"]): row for row in c.import_result["sources"]}
    picked = list(sources)
    results = c.import_result["imports"]
    c.begin("检查导入文件")
    c.require_library()
    files, extracts = [], []
    for path in picked:
        c.check()
        try:
            if path.exists():
                wait_readable(c, [path])
            found_files, found_extracts, errors = scan_import(c, path if partial else resolve_path(path), partial=partial)
            files.extend(found_files)
            extracts.extend(found_extracts)
            for source in found_files:
                sources[source] = import_source(source, needs_keys=source.suffix.lower() != NUP_SUFFIX)
            for source in found_extracts:
                sources[source] = import_source(source)
            if path not in found_files and path not in found_extracts:
                sources.pop(path, None)
            for bad_path, error in errors.items():
                row = sources[bad_path] = import_source(bad_path)
                import_source_failed(c, row, error)
        except (Failure, OSError) as error:
            # Legacy callers skip empty folders alongside usable sources.
            if not partial and explain(error).code == "input_empty":
                sources.pop(path, None)
                continue
            import_source_failed(c, sources[path], error)
            if not partial or sources[path]["code"] not in IMPORT_SOURCE_ERRORS:
                raise
        finally:
            c.import_result["sources"] = list(sources.values())
    files = sorted(set(files))
    extracts = sorted(set(extracts))
    paths = files + extracts
    c.import_result["sources"] = [sources[p] for p in sorted(sources)]
    if not sources:
        raise Failure("所选位置没有 NSP、XCI 文件或解包好的游戏文件夹。", code="input_empty")
    if any(sources[p]["needsKeys"] for p in files) and not c.keys.is_file():
        raise Failure("请先导入 prod.keys。", code="keys_missing")
    # 读原件（元数据、身份）不持锁，只在记进 catalog、放封面、写 catalog.json 那一下拿目录锁（启动器剃刀 H27：原先整段持锁，导入几款大原件的几分钟里
    #   改名 / 换封面 / 删游戏 / 移动全报「另一个任务正在使用游戏库」）。锁里重读 catalog：排队期间别的任务可能改过，判重按最新的
    verified = False
    extract_paths = set(extracts)
    total = len(paths) * 2
    for idx, source in enumerate(paths):
        c.last_log = None
        c.last_rc = None
        c.check()
        c.require_library()
        row = sources[source]
        extracted = source in extract_paths
        diagnostics_before = len(c.job["diagnostics"])
        try:
            c.begin("读取游戏信息", idx * 2 + 1, total)
            with tempfile.TemporaryDirectory(prefix="icons-", dir=c.private) as icons:
                if extracted:
                    packages = extract_inspect(c, source, icons)
                elif row["needsKeys"]:
                    packages = inspect(c, source, icons, source_id=row["id"])
                else:
                    packages = nup_inspect(c, source, icons)
                row["name"] = next((p["name"] for p in packages if p.get("name")), source.name)
                # 「重新添加原件…」（#1961）带 expectGameId：认出的不是这一款就停，不记进 catalog（原先会把选错的那款当新游戏加进库）
                want = str(c.r.get("expectGameId") or "").upper()
                if want and {str(p.get("baseId") or "").upper() for p in packages} != {want}:
                    other = "、".join(sorted({str(p.get("baseId") or "?").upper() for p in packages}))
                    raise Failure(f"选的是另一款游戏（{other}），不是这一款（{want}）：这款的记录没有改动，请选这款的原件。", code="input_wrong_game")
                c.begin("校验游戏文件", idx * 2 + 2, total)
                sha = ""
                if not extracted:   # 原件身份按读取器给的 content ID + 大小算，不读全文件；读取器没给才整份算（source_identity）
                    sha = source_identity(packages, source.stat().st_size) or digest(source, c.progress)
                with c.lock():
                    draft = deepcopy(c.catalog())
                    entries = record_import_packages(c, draft, source, packages, icons, sha, extracted)
                    c.check()
                    try:
                        atomic(c.library / "catalog.json", draft)
                    except OSError as error:
                        failure = explain(error)
                        if failure.code == "disk_full" or not partial:   # 默认路径同 #124 之前：原始 OSError 交 explain 归码
                            raise
                        raise Failure("游戏库目录无法写入。", internal=str(error)) from error
        except (Failure, OSError) as error:
            import_source_failed(c, row, error)
            if not partial or row["code"] not in IMPORT_SOURCE_ERRORS:
                raise
            del c.job["diagnostics"][diagnostics_before:]
        else:
            results.extend(entries)
            row["status"] = "duplicate" if all(entry["duplicate"] for entry in entries) else "imported"
            if row["needsKeys"] and not verified:
                atomic(c.private / "keys/status.json", dict(ready=True, verified=True))
                verified = True
    # #797：只放了更新 / DLC、库里也还没有这款的本体：添加完当场标出来（missingBase），前端在添加窗口里就说缺本体，不等到出包「检查游戏输入」才报。
    #   按整批加完后的游戏库判：同一批里先读到更新、后读到本体不算缺
    if results:
        nobase = {g["id"].upper() for g in c.catalog()["games"] if lacks_base(g)}
        for entry in results:
            entry["missingBase"] = entry["id"].upper() in nobase
    added = {r["id"].upper() for r in results if not r["duplicate"]}
    if added:
        store_meta_background(c, added)   # 横图、英文标题：后台查（导入不等联网），查到英文名接着改文件夹名
    # 不开 partialResults 时结果形状同 #124 之前：只有 imports（逐来源的 sources 只给要它的调用方）
    return c.import_result if partial else {"imports": results}


JOB_LIST_LIMIT = 200
DIAG_JOBS = 50   # 诊断包只带最近这么多个任务（记录 + 各自的日志），外加还没结束的（上传服务单包 1000 个文件上限；11-13）
# 另留名额（11-25 #2229 / #2206 D-7P56）：最近 50 个常被 phoneKeptPull 这类轮询任务占满，玩家报的那次失败 / 那款最近一次出包早于它们就带不上
#   （870 个任务里只收到最近 50 个、两次失败都不在；11-22 那次出包的完整日志也没进包）。最近 DIAG_FAILED 个失败任务、每款最近一次出包类任务
#   （至多 DIAG_PACK_GAMES 款）不跟最近 50 个抢名额，一律收（记录 + 各自的日志；整包预算照旧由 DIAGNOSTIC_ZIP_BUDGET 管）
DIAG_FAILED = 20
DIAG_PACK_GAMES = 20
DIAG_PACK_OPS = ("ipa", "updateGame", "install", "pushSelected", "sgradeUpgradeGame", "sgradeMac", "android", "tvInstall", "tvExport")


def _mtime(path):
    try:
        return path.stat().st_mtime
    except OSError:
        return 0


CUSTOM_NAME_LIMIT = 64
CUSTOM_COVER_LIMIT = 16 << 20
_CUSTOM_COVER = re.compile(r"cover\.custom\.[0-9a-f]{8}\.jpg")


def game_cover(c, gid, custom=None):
    """这款用哪张封面（游戏库列表与起局给宿主的 Dock 图标同一份定义）→ 路径字符串，没有 → ""。
    三级：玩家换的 → 游戏包自带图标（导入时从控制分区 icon_*.dat 取，cover.jpg）→ 商店横图（store_meta，banner.jpg）→ 空（界面用 S2Arm 标志）。
    #994：起局拿的是 catalog.json 里的原始记录（没有 cover 字段），原先只在列表这边现算，Dock 图标永远拿不到封面"""
    custom = custom if custom is not None else game_custom(c, gid)
    if custom["cover"]:
        return str(custom["cover"])
    for name in ("cover.jpg", "banner.jpg"):
        p = layout.path(c, gid, name, create=False)
        if p.is_file():
            return str(p)
    return ""


def game_custom(c, gid):
    """玩家改过的显示名和封面（games/<程序号>/custom.json）。只影响显示，不改 catalog.json，不让已出的包失效；重新导入也不会覆盖。"""
    folder = layout.path(c, gid, create=False)
    try:
        data = read(folder / "custom.json", {})
    except (OSError, ValueError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    name = data.get("name") if isinstance(data.get("name"), str) else ""
    cover = data.get("cover") if isinstance(data.get("cover"), str) and _CUSTOM_COVER.fullmatch(data["cover"]) else ""
    return dict(name=name, cover=folder / cover if cover and (folder / cover).is_file() else None)


def _custom_cover_bytes(path):
    try:
        with open(path, "rb") as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode):
                raise Failure("封面要选一张图片文件。")
            if info.st_size > CUSTOM_COVER_LIMIT:
                raise Failure("封面图片太大了：请选 16 MB 以内的图片。")
            data = source.read(CUSTOM_COVER_LIMIT + 1)
    except OSError as error:
        raise Failure("读不了这张图片，请换一张试试。") from error
    if not data.startswith((b"\xff\xd8\xff", b"\x89PNG\r\n\x1a\n")):
        raise Failure("封面只支持 JPG 或 PNG 图片。")
    return data


def customize(c):
    """改游戏显示名 / 封面：name、coverPath 没给就不动，给空字符串恢复默认。"""
    with c.lock():
        gid = c.game()["id"]
        c.begin("保存游戏名字和封面")
        folder = layout.path(c, gid)
        current = game_custom(c, gid)
        name, cover = current["name"], current["cover"].name if current["cover"] else ""
        data = None
        if "name" in c.r:
            name = c.r["name"].strip() if isinstance(c.r["name"], str) else None
            if name is None or len(name) > CUSTOM_NAME_LIMIT or any(ord(ch) < 32 or ord(ch) == 127 for ch in name):
                raise Failure(f"游戏名字要在 {CUSTOM_NAME_LIMIT} 个字以内，不能有换行。")
        if "coverPath" in c.r:
            if not isinstance(c.r["coverPath"], str):
                raise Failure("封面路径不对。")
            cover = ""
            if c.r["coverPath"]:
                data = _custom_cover_bytes(c.r["coverPath"])
                cover = f"cover.custom.{hashlib.sha256(data).hexdigest()[:8]}.jpg"   # 换文件名：两端按路径缓存封面，同名会显示旧图
        folder.mkdir(parents=True, exist_ok=True)
        if data is not None:
            temporary = folder / (cover + ".partial")
            temporary.write_bytes(data)
            os.replace(temporary, folder / cover)
        if name or cover:
            atomic(folder / "custom.json", dict(schema=1, name=name, cover=cover))
        else:
            (folder / "custom.json").unlink(missing_ok=True)
        for old in folder.glob("cover.custom.*.jpg"):
            if old.name != cover:
                old.unlink(missing_ok=True)
    return dict(gameId=gid, name=name, cover=str(folder / cover) if cover else "")


def catalog(c):
    from build import validate_runtime
    import toolchain
    import host_update
    c.begin("读取游戏库")
    startup = c.r.get("startup") is True   # 启动器打开后的第一次刷新（两端前端带 startup: true）
    if startup:
        sweep_residue(c)   # 崩溃 / 断电 / 被杀留下的解包与中间产物：启动时扫一次（剃刀 H14：原先每次刷新都扫两遍；正常失败与取消由 clean_after_stop 当场清）
        sweep_trash(c)   # 删除时挪进 .s2a-trash 而当时没删干净的：同在启动时清一次（玩家反馈 11-11 删除先挪再删）
        sweep_private_tmp(c)   # 作业被打断（卡死 / 强退 / 断电）留在私有目录的临时目录：同在启动时清一次（#1471）
        unseal_builds(c)   # 旧版本留下的只读 RomFS 共享存储：每款一次加回写权限，玩家手动删旧产物不再被挡
    cat = c.catalog()
    input_supported, input_reason = toolchain.input_interface(c)
    # 设备记录里装在某台手机上的款：删除确认框据此多写一句「下次往这台手机装游戏时，手机上的这款会打不开」（容器现组时游戏库里没有它就带不进）
    on_phone = {g.upper() for _, r in layout.devices(c) if isinstance(r, dict) for g in (r.get("games") or {})}
    import install
    import sgrade
    sq = sgrade.queue_status(c)   # S 级升级队列（排队只在作业层一套）：每款 sgradeState / sgradeProgress，顶层 sgradeBatch
    for game in cat["games"]:
        game["missing"] = requirements(c, game)
        game["requirementDiagnostics"] = [c.describe_failure(error) for error in requirement_errors(c, game)]
        game["compatibility"] = "unsupported" if guest_isa(selected_packages(game)) == "aarch32" else "experimental"
        game["needsKeys"] = needs_keys(selected_packages(game))   # 出包要不要 prod.keys：前端直接用，不再各算一份格式名单（剃刀 W10）
        image = layout.image_mac(c, game["id"], create=False)
        try:
            manifest = validate_runtime(c, image, full=False, files=False)
            game["ready"] = manifest.get("memoryRequirementsVersion") == 2
        except (Failure, OSError, ValueError):
            manifest = {}
            game["ready"] = False
        # 升级启动器不算要重新准备（11-08 维护者：覆盖升级状态要保留）：运行版本自带宿主，旧版照常能玩；输入变了才要重做
        game["needsPreparation"] = not game["ready"] or manifest.get("inputIdentity") != input_identity(game)
        game["iphoneReady"] = iphone_ready(c, game["id"])
        game["iphoneExportable"] = game["iphoneReady"] and iphone_img(c, game["id"])   # 「导出自签包」能直接导出（单一 IMG 资源包）；旧格式要先重出
        import sgrade
        st = sgrade.upgrade_state(c, game["id"])   # S 级优化单款升级（11-17 起玩家手动点）：在跑 → 界面画进度、禁开始；标记在但作业不在 → 「升级未完成，点 S 级优化继续」
        game["sgradeUpgrading"], game["sgradeIncomplete"] = st == "upgrading", st == "incomplete"
        qs = next((x for x in sq["games"] if x["gameId"] == game["id"].upper()), None)   # 升级队列里这款（sgrade.queue_status）：queued / running / done / skipped / incomplete
        game["sgradeState"] = qs["state"] if qs else ("incomplete" if st == "incomplete" else None)
        game["sgradeProgress"] = qs["progress"] if qs else None
        game["appleTvReady"] = tvos_ready(c, game["id"])   # Apple TV 版已出好（image/tvos 的容器件 + 资源包，pack/jobs/tvos.py）：出包页给「装到 Apple TV」
        game["downstream"] = downstream.describe(c, game)   # 下游深度方案（pack/downstream/schemes.json）：命中时启动器弹确认页
        game["sgrade"] = sgrade_badge(c, game["id"], image)   # 游戏库封面左上角金色 S（v0.4.0）
        game["iphoneRestorable"] = layout.phone_ipa(c, previous=True).is_file()   # 换壳后可装回上一版（install.restore_previous）
        game["iphoneInstall"] = layout.install_record(c, game["id"])
        game["onPhone"] = game["id"].upper() in on_phone
        # F06：按宿主二进制真实能力报，不能一律 True（否则 UI 的「旧引擎缺接口」提示永远不出现）
        game_info = layout.game_info(c, game["id"])
        host_mac = game_info.get("hostMac")
        game["inputConfig"] = toolchain.input_interface(c, host_update.host_exe(c, host_mac))[0] if host_mac and game["ready"] else input_supported
        game["hostUpdate"] = host_update.status(c, game["id"], image, manifest) if game["ready"] else "none"
        # 存放位置（游戏库结构 v3）：ok / offline（位置没连上，如移动硬盘没插）/ missing（位置在、文件夹没了）/ none（还没出过包）
        where, place = layout.state(c, game["id"])
        root = place.parent.parent if where == "ok" else place
        game["location"] = dict(state=where, path=str(root) if root else "", folder=str(place) if where == "ok" else "",
                                volume=_volume(root) if root else "", default=root == c.library if root else True)
        custom = game_custom(c, game["id"])
        game["customName"], game["customCover"] = bool(custom["name"]), custom["cover"] is not None
        game["name"] = custom["name"] or game["name"]
        game["cover"] = game_cover(c, game["id"], custom)
        banner = layout.path(c, game["id"], "banner.jpg", create=False)
        # 已下载的商店横图另给一份（#378）：Mac 游戏库顶部大图按「玩家换的 → 横图 → 封面」取，不把 256×256 的图标放大；列表卡片照旧用 cover
        game["banner"] = str(banner) if banner.is_file() else ""
        choices = [p for p in selected_packages(game) if p["type"] in ("base", "update")]
        if choices:
            game["display"] = max(choices, key=lambda p: p["version"])["display"]
        game["sizes"] = sizes.for_catalog(c, game["id"], cat["games"])   # 每款各部分大小（sizes.py：作业收尾量好记在 game.json，这里不量）
        played = read(layout.path(c, game["id"], "playtime.json", create=False), {})   # Mac 局游玩时间（运行时 playtime.c 写，S2A_PLAYTIME）；没玩过不给
        if isinstance(played, dict) and isinstance(played.get("seconds"), int) and played["seconds"] > 0:
            game["playtime"] = dict(seconds=played["seconds"], lastPlayed=played.get("lastPlayed") or 0)
    try:
        validate_keys(c.keys)
        status = read(c.private / "keys/status.json", {}) | dict(ready=True)
    except (Failure, OSError, UnicodeError):
        status = dict(ready=False, verified=False)
    status["message"] = "已验证解密能力" if status.get("verified") else "格式通过，首次读取游戏时验证解密能力" if status["ready"] else "尚未导入 prod.keys"
    # 任务目录里还有各任务留存的请求（<任务>.request.json，J10 resume 用）：不是任务记录，没有 id / state；混进来启动器整个任务列表解码失败、一条都读不回
    # 任务目录有上限（保留规则见 core.prune_jobs）：启动时那次 catalog 顺手清，一天最多一次
    #   （清理要把每条记录读一遍判结束没有），先清再读。只回最新 JOB_LIST_LIMIT 条：旧启动器的任务列表只看近期；
    #   11-07 实测任务目录 1.5 万条记录时整份读 + 回传 7 MB 占 catalog 数秒。
    marker = c.logs / ".jobs-pruned"
    if startup and time.time() - _mtime(marker) > 86400:   # 只在启动那次判（剃刀 H14），仍一天最多一次
        prune_jobs(c.private, jobs=c.jobpath.parent)
        marker.parent.mkdir(parents=True, exist_ok=True)   # catalog 是只读探测、不落自己的记录（剃刀 H4）：日志目录不一定已被本作业建出来
        marker.touch()
    # 数据目录在 exFAT 等非 APFS 盘上搬过时，每条记录旁有一份 ._<名>.json 附属文件（非 UTF-8 元数据）：按 os_junk 跳过；
    #   单条记录读不出只记一行、跳过这一条，不拖垮整个游戏库刷新（#1010，同 #911 设备记录）
    records = sorted((p for p in c.jobpath.parent.glob("*.json")
                      if p != c.jobpath and not p.name.endswith(".request.json") and not layout.os_junk(p.name)),
                     key=_mtime, reverse=True)[:JOB_LIST_LIMIT]
    jobs = []
    for p in records:
        try:
            r = read(p)
        except (OSError, ValueError) as e:   # UnicodeDecodeError / JSONDecodeError 都是 ValueError
            print(f"[s2a] 任务记录 {p.name} 读不出，跳过：{type(e).__name__}: {e}", file=sys.stderr)
            continue
        if isinstance(r, dict):
            jobs.append(r)
    try:   # 「更新 iPhone 上的 S2Arm」：各台手机的设备记录里壳比本版旧的（只读本机记录，不连手机，install.phone_updates）
        import install
        phone_updates = install.phone_updates(c)
    except (Failure, OSError, ValueError):   # 算不出本版壳号（开发树没出过壳等）：不提示，不挡游戏库
        phone_updates = []
    # 这台电脑往几台手机装过游戏（设备记录里有款的）：启动器更新弹窗据此把按钮写成「更新启动器和 iPhone App」（11-16 维护者「点一次完成整个流程」）
    phone_devices = sum(1 for _, r in layout.devices(c) if isinstance(r, dict) and r.get("games"))
    return dict(games=cat["games"], sgradeBatch=dict((k, sq[k]) for k in ("running", "task", "index", "count")), profiles=[], keys=status, jobs=jobs, phoneUpdates=phone_updates, phoneDevices=phone_devices,
                engineInfo={"inputConfig": input_supported, "inputConfigReason": input_reason})


# 这一款在私有目录里按程序号存的出包缓存（直连 / 翻译 / 追加内容 / 原件核对 / 宽屏；都在 S2A_RUN_ROOT = <私有>/run 下，pack/jobs/core.py Context.env）
# 显式清理/删除含共享编库缓存；Q15 成功后的自动清理仅清本款中间产物，保留可复用编库条目。
def aoc_cache_dir(c, gid):
    """本款追加内容（DLC）缓存（pack/stages/aoc.sh 的 S2A_AOC_CACHE，按键存解出的 DLC，大游戏的 DLC RomFS 好几 GB）。
    同 direct_cache_dir：游戏和工具缓存在同一块盘 → 工具缓存下（同原来）；游戏放外接盘 → 这款游戏文件夹第一层的 aoc-cache/，
    解包写入跟着游戏落到外接盘，不再把几 GB 写进系统盘、再跨盘拷回游戏（#1725 诊断 D-K82N：游戏库在外接盘，DLC 解到内置盘写满）"""
    tool = c.work_root / "run" / "s2arm" / "cache" / "aoc"
    loc = layout.find_game(c, gid)
    if loc is None or _existing(loc).stat().st_dev == _existing(c.work_root).stat().st_dev:
        return tool
    return loc / "aoc-cache"


def direct_cache_dir(c, gid):
    """本款直连缓存（graphics/tools/direct/direct_lib.py 的 S2A_DIRECT_CACHE：内容寻址仓 + 编译中的翻译中间产物，大游戏编译中几十 GB）。
    存放位置与工具缓存在同一块盘：工具缓存下（同原来）；不在同一块盘（游戏放外接盘）：这款游戏文件夹第一层的 direct-cache/，
    大头写入跟着游戏落到外接盘，系统盘只留固定开销（引擎、宿主）。放在 build/ 外面：出包成功后清 build/ 不带走它，重出包照样复用"""
    tool = c.work_root / "run" / "s2arm" / "cache" / "direct" / gid.upper()
    loc = layout.find_game(c, gid)
    if loc is None or _existing(loc).stat().st_dev == _existing(c.work_root).stat().st_dev:
        return tool
    return loc / "direct-cache"


def scratch_root(c, gid):
    """重出着色器库这类前台临时活（sgrade_bf：从 game.img 拷出 RomFS、克隆产物根、重抽重翻，大游戏几十 GB）的临时目录放哪：同 direct_cache_dir 跟着游戏走——
    和工具缓存同一块盘就放工具缓存 tmp/，游戏在别的盘就放这款文件夹第一层的 .work-tmp/。原先一律放私有目录（Windows 在系统盘），
    玩家把游戏库设到别的盘、更新几款时系统盘照样被写满（D-NDV5）。目录建好再返回（tempfile 要它在）"""
    d = direct_cache_dir(c, gid)
    out = d.parent / ".work-tmp" if d.name == "direct-cache" else c.work_root / "tmp"
    out.mkdir(parents=True, exist_ok=True)
    return out


def game_caches(c, gid, *, include_compiler=True):
    run = c.work_root / "run" / "s2arm"
    low = gid.lower()
    direct = direct_cache_dir(c, gid)
    out = [direct] if include_compiler else direct_transients(c, direct)
    for d, pat in ((run / "cache" / "bake", f"{low}-*"), (run / "cache" / "aoc", f"t{low}-*"), (run / "cache" / "srcver", f"t{low}-*"),
                   (run / "widen", f"t{low}_*"), (run / "widen" / "auto", f"t{low}_*")):
        out += sorted(d.glob(pat)) if d.is_dir() else []
    if include_compiler:
        out += shared_compiler_caches(c)
    return [p for p in out if p.exists() or p.is_symlink()]


def shared_compiler_caches(c):
    """全体共用的编译缓存（不分款）：着色器编译中间件 air、管线缓存 pso、直连库下载缓存。任何一款出包 / 起局都在读写它们"""
    return [c.work_root / "cache" / "air", c.work_root / "cache" / "pso", c.work_root / "run" / "gal_dl_cache"]


def delete_shared_caches(c, gid):
    """删除游戏时顺手清共用编译缓存（删除语义一向包含它们）：只在执行锁拿得到独占（没有任何一款在出包 / 在玩）时清，拿不到这次不清、不等——
    它们不属于被删的这一款，别的款出包正读写着它们（#2191 前删除拿执行锁独占，等于先等所有出包结束）。→ 挪进回收站的 [(根, 路径)]"""
    from core import trace
    out = []
    with c.idle("execution") as free:
        if not free:
            trace(c, "删除游戏：有别的款在出包 / 在玩，这次不清共用编译缓存（air / pso / 直连库下载缓存）")  # 非玩家句
            return out
        for p in shared_compiler_caches(c):
            if p.exists() or p.is_symlink():
                moved = _to_trash(c, c.work_root, p, gid)
                if moved:
                    out.append((c.work_root, moved))
    return out


def _direct_base(c, direct):
    """查「沿途没有链接」从哪查起：直连缓存在工具缓存下就从工具缓存查；跟着游戏放在别的盘（这款文件夹的 direct-cache，direct_cache_dir）就从这款文件夹查。
    #754：原先一律从工具缓存查，游戏库和数据目录不在一块盘时 relative_to 抛 ValueError，出包失败后的清理整个没做（「F:… is not in the subpath of D:…」）"""
    return c.work_root if Path(direct).is_relative_to(c.work_root) else Path(direct).parent


def direct_transients(c, direct):
    """Only disposable direct-build work; never remove the store (store/, logs/) or unknown layouts.
    S2Arm 11-01: the direct cache is one content-addressed store, one file per entry (C# dsh_batch build, store/dshb/); the former per-key trees
    ios/ and mac/ (b-<chain>/, cas/…) and work/ are the old layout, which direct_lib.py deletes on its next build anyway"""
    from cache_retention import _safe_root
    if _safe_root(_direct_base(c, direct), direct) != 'safe':
        return []
    out = [direct / name for name in ('work', 'merged', 'mac', 'ios')]
    for path in sorted(direct.glob('*.stage.*')):
        name, _, pid = path.name.partition('.stage.')
        if name not in ('work', 'mac', 'ios') or not pid.isdecimal() or int(pid) <= 0:
            continue
        try:
            os.kill(int(pid), 0)
        except ProcessLookupError:
            out.append(path)
        except (PermissionError, OverflowError):
            pass  # unknown/live process: retain its in-progress stage
    # Retain links: only real, producer-owned transient directories are eligible.
    safe = []
    for path in out:
        try:
            if not stat.S_ISLNK(path.lstat().st_mode):
                safe.append(path)
        except FileNotFoundError:
            pass
    return safe


def _rm_direct_transient(c, direct, path):
    """Remove only this lexical child, without resolving its final component."""
    from cache_retention import _safe_root
    if path.parent != direct or _safe_root(_direct_base(c, direct), direct) != 'safe':
        return False
    # Pin the checked parent; rmtree's dir_fd implementation refuses a directory
    # replaced by a symlink instead of traversing it. Global _rm stays unchanged.
    if os.name == "nt":
        with winproc.open_dir_nofollow(direct) as parent:
            try:
                winproc.remove_at(parent, path.name)
            except FileNotFoundError:
                return False
            except OSError as error:
                if error.errno == errno.ELOOP:
                    return False
                raise
        return True
    fd = os.open(direct, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        try:
            mode = os.stat(path.name, dir_fd=fd, follow_symlinks=False).st_mode
        except FileNotFoundError:
            return False
        if stat.S_ISLNK(mode):
            return False
        if stat.S_ISDIR(mode):
            shutil.rmtree(path.name, dir_fd=fd)
        else:
            os.unlink(path.name, dir_fd=fd)
        return True
    finally:
        os.close(fd)


# 删目录时「目录非空」（#369：游戏库在外置盘，出包清理报 Errno 66 / rm 报 Directory not empty，同样的文件放内置盘就成功）：
#   删的过程中有别的程序往这个目录里写东西——访达在打开的目录里写 .DS_Store（玩家关了 DSDontWriteUSBStores 仍出现）、非 APFS 盘的 ._ 附属文件。
#   本机复现：rmtree 期间另一个线程往目录写 .DS_Store → Errno 66。只清系统自动生成的文件（layout.os_junk，三端同一份名单）再删，最多 5 次；
#   清完还剩别的文件 = 真有人在往里写，报红并点名剩下的前几项，不硬删
def _rmdir_past_os_junk(path, error, tries=5, wait=0.2):
    import layout
    left = []
    for i in range(tries):
        try:
            left = os.listdir(path)
        except FileNotFoundError:
            return
        for name in left:
            q = os.path.join(path, name)
            if layout.os_junk(name) and not os.path.isdir(q) and not os.path.islink(q):
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(q)
        try:
            os.rmdir(path)
            return
        except FileNotFoundError:
            return
        except OSError as again:
            if again.errno != errno.ENOTEMPTY:
                raise
        time.sleep(wait)
    others = [n for n in left if not layout.os_junk(n)]
    raise OSError(errno.ENOTEMPTY, f"目录删不空：删的同时有程序往里写（剩 {', '.join(others[:5]) or '系统文件'}）", path) from error  # 非玩家句


SHARING_RETRY_S = (0.1, 0.2, 0.4, 0.8, 1.5, 2.0)   # Windows 文件被别的程序短暂打开（杀毒 / 索引 / 预览刚扫到）时，删同一项的重试间隔（合计约 5 秒）


def _retry_shared(func, path, error, sleep=time.sleep):
    """Windows 共享冲突（WinError 32：另一个程序正在使用此文件）多是杀毒 / 系统索引刚扫到、马上就放：同一项隔一会儿重删，
    几次还占着就把最后一次的错如实抛出（#cdec0e：解包前清旧解包目录撞上这个，整单 job_failed）。不是 32 的原样抛"""
    if getattr(error, "winerror", None) != 32:
        raise error
    for delay in SHARING_RETRY_S:
        sleep(delay)
        try:
            return func(path)
        except OSError as again:
            if getattr(again, "winerror", None) != 32:
                raise
            error = again
    raise error


def _rmtree(p):
    """删整棵目录；遇到只读目录（产物根的 RomFS 共享存储 roots/.romfs_store/ 整棵只读，pack/roots.sh）先给它加回写权限再删。
    Windows 上封存不是只读位，是 ACL 里给当前用户的拒写项（pack/orch/roots.py seal → _win_acl），chmod 解不开：
    按 roots 同一个函数把出错的项和它的父目录解封、再去掉只读位后再删（11-07 Windows 出包机：启动器出包成功后清中间产物卡在 .romfs_store，WinError 5）。
    exFAT 上删数据文件时 AppleDouble 附属文件（._*）会一并消失，scandir 里还挂着的名字再 unlink 得到 ENOENT——当作已删完。"""
    def unlock(func, path, err):
        error = err[1] if isinstance(err, tuple) else err   # onerror 给 exc_info、onexc 给异常本身
        errno_ = getattr(error, "errno", None)
        if errno_ == errno.ENOENT:
            # 删除期间目录可能被另一个清理者先移走；扫描阶段也按幂等删除处理。其他 I/O 错误仍需上报。
            return
        if func in (os.rmdir, os.unlink) and errno_ == errno.ENOTEMPTY:
            return _rmdir_past_os_junk(path, error)
        if getattr(error, "winerror", None) == 32:   # 被别的程序短暂占着：重试同一项，不改权限
            return _retry_shared(func, path, error)
        parent = os.path.dirname(path)
        if os.name == "nt":
            engine = str(Path(__file__).resolve().parents[2])
            if engine not in sys.path:
                sys.path.insert(0, engine)
            from pack.orch import roots
            for q in (parent, path):
                if os.path.lexists(q) and not os.path.islink(q):
                    roots._win_acl(q, False)
            if os.path.lexists(path) and not os.path.islink(path) and not os.path.isdir(path):
                os.chmod(path, stat.S_IWRITE)   # 只读位（别的来源放进来的只读文件，os.unlink 不认、报 WinError 5）：去掉再删；还删不掉就照抛
        else:
            os.chmod(parent, stat.S_IMODE(os.lstat(parent).st_mode) | stat.S_IRWXU)
        func(path)
    if sys.version_info >= (3, 12):
        shutil.rmtree(p, onexc=unlock)
    else:
        shutil.rmtree(p, onerror=unlock)


NOT_MANAGED = "操作路径不属于托管目录。"


class NotManaged(Failure):
    """要删 / 挪的路径不在这个存放位置里：清理收尾只记日志、不进错误摘要（不是这次失败的原因，原先作为第二个 job_failed 把人带偏）"""


def _managed_anchor(root, p):
    """p 在存放位置 root 里 → (锚, 相对部分)。锚是 p 的某一级上级目录：就是 root，或它的真实路径等于 root 的真实路径
    （游戏库经符号链接、换了一种写法、在另一块盘上挂出来访问时，原先按字面比判成「不属于托管目录」）。只解析锚这一级，锚以下照原样、不跟随链接
    （_rm 逐级查目录链接靠它：顺着链接删进 saves/ 不行）。p 就是 root 本身、或哪一级都对不上 → NotManaged"""
    root, p = Path(os.path.abspath(root)), Path(os.path.abspath(p))
    real = os.path.realpath(root)
    for anchor in p.parents:
        if anchor == root or os.path.realpath(anchor) == real:
            return anchor, p.relative_to(anchor)
    raise NotManaged(NOT_MANAGED)


def _rm(c, root, p):
    # Check the entry before resolving it: a link into saves/ is not disposable.
    p = Path(os.path.abspath(p))
    root, relative = _managed_anchor(root, p)
    parent = root
    for part in relative.parts[:-1]:
        parent = parent / part
        if _is_link(parent):
            raise Failure("清理路径包含目录链接，已停止操作。")
    if _is_link(p):
        # A Windows junction is removed with rmdir, a symlink with unlink.
        if p.is_symlink():
            p.unlink()
        else:
            p.rmdir()
    elif p.is_dir():
        _rmtree(p)
    else:
        p.unlink(missing_ok=True)


def _is_link(p):
    """软链或 Windows junction（目录软链建不了时 pack/orch/roots.py directory_link 退到 junction；Path.is_symlink 认不出 junction）"""
    return p.is_symlink() or getattr(os.path, "isjunction", lambda _: False)(p)


def _rm_entry(c, p):
    """链接只删链接本身、不进指向；其余同 _rm"""
    _rm(c, c.library, p)


def _prune_roots(c, d):
    """产物根只留现行（*@现行 指针文件及其指向的版本目录）与 RomFS 共享存储，清旧版本 → 清了几项。
    RomFS 共享存储：现行指向其中的身份，删了现行就断；没人指向的旧身份由下次提交清（pack/roots.sh root_romfs_sweep）"""
    pointers = list(d.glob("*" + layout.CURRENT))
    current = {q.name for q in pointers}
    for q in pointers:
        version = layout.current_root(c, q)   # 老库的链接在这里先迁移成指针文件
        if version is not None:
            current.add(version.name)
    n = 0
    for q in d.iterdir():
        if q.name not in current and q.name != ".romfs_store":
            _rm_entry(c, q); n += 1
    return n


def _windows_layout():   # 单测可打桩：Windows 的产物根在 work/ 下（ipa.current_root）
    return os.name == "nt"


def table_rows(path):
    """补丁表（cpatch.tsv）里的条目数（去掉表头）；没有 / 读不了 = 0"""
    try:
        return max(0, sum(1 for line in open(path, encoding="utf-8") if line.strip()) - 1)
    except OSError:
        return 0


def sgrade_badge(c, gid, image):
    """游戏库封面左上角的金色 S（v0.4.0 维护者）：这款已经带上 S 级优化——Mac 运行版本或 iPhone 程序件里打进了社区补丁 / 金手指。出包页取消过「出包时打入」的款不亮"""
    gid = gid.upper()
    if layout.game_info(c, gid).get("sgradeOff") is True:
        return False
    # 只认真打进去了的（11-14：原先「补丁库里有这款的包」也亮，没出过包的款也亮了；发布说明写的是「亮起的已经带上了」）：
    #   Mac 运行版本 / iPhone 程序件里补丁表有条目，或带着随包金手指原文（多数金手指只改内存、不进补丁表）。Mac 老游戏启动器打开时会补上（sgradeMac），补完就亮
    for d in (Path(image), layout.ios_kit_present(c, gid)):   # 只读已有的：清单里不迁移、不现建（见 layout.ios_kit_present）
        if d is None:
            continue
        try:
            # 维护者 11-15「两者合成一套」：没补丁、但本包在宽屏已验证名单（我方通用满屏缺省开，产物根 widen_verified.tsv）也算带上了 S 级优化的满屏
            if table_rows(d / "cpatch.tsv") or any(d.glob("cheat-src.*")) or (d / "widen_verified.tsv").is_file():
                return True
        except OSError:
            continue
    return False


CONTAINER_KIT = layout.IOS_KIT   # 每款 .programs/<程序号>/kit（11-16 起；之前在 image/ios/kit）：装机组装容器要用的文件，出包成功后从产物根存一份独立副本（游戏库结构 v3；旧布局是 iPhone/<号>/container-kit）


RESOURCE_IMAGES = ("romfs.std", "game.img")   # 产物根顶层不进程序件的资源镜像（封闭集合，产出它们的就这几处）
RESOURCE_SUFFIXES = (".nsp", ".xci", ".nca")   # 原件 / 内容包
KIT_LOUD_BYTES = 300 * 1024 * 1024   # 程序件除改写产物外（= 常驻配方）的正常上限：11-16 常驻库 14 款都在 50 MB 以内，超过就是混进了不该有的大文件


def is_resource_image(name):
    return name in RESOURCE_IMAGES or Path(name).suffix.lower() in RESOURCE_SUFFIXES


def loud_if_kit_oversized(c, gid, kit):
    """程序件里除各模块原始程序与改写产物以外（即常驻配方那部分）超过 KIT_LOUD_BYTES：出包照常完成，但当场报出来、写出最大的几个文件——
    下次再有新的大资源混进同层，第一次出包就被看见（11-16 romfs.std 混进配方是事后才发现的）。→ 报了没有"""
    import programs as progs
    skip = set(progs.MODULES) | {f"{m}.{o}" for m in progs.MODULES for o in progs.OUTPUTS}
    files = [(p.stat().st_size, p.relative_to(kit).as_posix()) for p in Path(kit).rglob("*") if p.is_file() and p.name not in skip]
    total = sum(n for n, _ in files)
    if total <= KIT_LOUD_BYTES:
        return False
    top = "、".join(f"{name}（{n // (1024 * 1024)} MB）" for n, name in sorted(files, reverse=True)[:3])
    c.diagnostic(f"{gid} 的程序件里混进了大文件，共 {total // (1024 * 1024)} MB（正常不到 50 MB）：{top}", code="kit_oversized", severity="warning")  # 非玩家句
    return True


def save_container_kit(c, gid, root, dest=None):
    """把组装容器要读的文件（ship_ext.place_game / display_name / 随包表：顶层普通文件、ctl/control.nacp、programs/<n>/）存成不依赖产物根的副本。
    root 里的链接按内容拷，之后删产物根、RomFS 存储、Windows 的 work/ 都不影响装机、续签和单壳追加。没有 main.npdm 返回 None，不动旧的。
    dest 给了（迁移旧布局时）就存成 dest/container-kit，否则存到程序缓存的 kit、并存常驻配方（pack/jobs/programs.py）。"""
    root = Path(root)
    if not (root / "main.npdm").is_file():
        return None
    import programs as progs   # 本函数下面另有局部变量 programs（产物根的 programs/ 目录）
    out = Path(dest) if dest else progs.cache(c, gid)
    out.mkdir(parents=True, exist_ok=True)
    name = "container-kit" if dest else CONTAINER_KIT
    kit, temporary = out / name, out / (name + ".partial")
    managed = c.library if dest else c.work_root
    if temporary.exists():
        _rm(c, managed, temporary)
    def copy_files(source, dest):
        dest.mkdir(parents=True, exist_ok=True)
        for p in source.iterdir():
            # 产物根顶层的资源镜像（RESOURCE_IMAGES：RomFS 直出的 romfs.std、资源包、原件）是游戏资源，不是程序件：不跳过就整份进程序缓存和常驻配方
            #   （11-16 Windows 端到端实测：romfs.std 让小款配方 315 MB，组装容器本来也不读它）。不用白名单：程序件有约 30 种、读写方各不相同，漏一项升级就坏
            if p.is_file() and not p.name.startswith(".") and p.suffix != ".log" and not is_resource_image(p.name):
                shutil.copyfile(p, dest / p.name)
    copy_files(root, temporary)
    if (root / "ctl/control.nacp").is_file():
        (temporary / "ctl").mkdir()
        shutil.copyfile(root / "ctl/control.nacp", temporary / "ctl/control.nacp")
    programs = root / "programs"
    if programs.is_dir():
        for p in sorted(programs.iterdir()):
            if p.is_dir() and (p / "main.npdm").is_file():
                copy_files(p, temporary / "programs" / p.name)
    loud_if_kit_oversized(c, gid, temporary)
    progs._put_dir(c, temporary, kit)   # 旧的先改名挪开、新的改名就位，两次都按 #361 退避重试（Windows 杀软 / 索引开着刚拷完的文件报 WinError 5，#50cb93 三台设备）
    if not dest:
        progs.save_recipe(c, gid)   # 常驻配方（原始程序在常驻里的模块，改写产物不存、现建）
        for legacy, root in ((layout.image_ios(c, gid, create=False) / CONTAINER_KIT, layout.root_of(c, gid)),
                             (layout.program_store(c, gid) / CONTAINER_KIT, c.library)):
            if legacy.is_dir():   # 旧位置的那份已被这次新存的取代
                _rm(c, root, legacy)
    return kit


def _build_entries(c, gid):
    """这款出包缓存 build/ 里能删的项：除 iPhone 出包日志 build/ios/logs 之外的全部。日志是释放空间的「保留」项（SPACE_KEPT），
    出包成功清理、清缓存、释放「旧版残留」都不删它（旧布局 iPhone/<号>/logs 同样一直留着）"""
    build, out = layout.build_dir(c, gid, create=False), layout.ios_work(c, gid, create=False)
    if not build.is_dir():
        return []
    if not os.path.lexists(out / "logs"):
        return [build]   # 没有要留的：整个删
    return [q for q in build.iterdir() if q != out] + [q for q in out.iterdir() if q.name != "logs"]


def _residue_disposable(c, gid):
    """解包和中间产物能不能删：有容器件，或这款没有已出好的 IPA。老玩家有 IPA 没容器件时，现行产物根是装机的唯一来源，留给「旧版残留」项先存容器件再删。"""
    return layout.has_ios_kit(c, gid) or not (layout.ios_pack(c, gid) / "manifest.json").is_file()


def _keep_stage_logs(c, gid):
    """产物根里的阶段日志要跟着删：先拷进私有日志目录，失败后的诊断包还能带上（只拷文件，单份 8 MB 以内，最多 64 份）。"""
    work = layout.ios_work(c, gid, create=False)
    current = [layout.current_root(c, p) for p in [*work.glob("roots/*" + layout.CURRENT), *layout.build_dir(c, gid, create=False).glob("*/roots/*" + layout.CURRENT)]]
    roots = [*(r for r in current if r is not None), *work.glob("work/pkg/product"),
             *work.glob("work"),   # Windows 一键出包各步的日志（seed.log 等，pack/orch/oneclick_win.py Run.sh 写在工作目录下）：#754 失败后随 build/ 删掉，诊断包里没有
             # 阶段失败时各阶段把自己的日志拷进产物根旁的 .failed/（如改写 pack/stage_rewrite.sh 的 <款>_<模块>.log）：玩家报出包超时 / 改写失败时要的就是它（#251 诊断包里没有）
             *work.glob("roots/.failed"), *layout.build_dir(c, gid, create=False).glob("*/roots/.failed"),
             *work.glob("work/roots/.failed")]   # Windows 一键出包的产物根在工作目录下（pack/jobs/ipa.py win_oneclick --out <工作目录>/work）：改写失败留的 <款>_<模块>.rewrite.log（#1476 诊断包里只有 light.log）
    dest = c.logs / (c.task + "-stages")
    count = 0
    for root in roots:
        for log in sorted(p for p in root.glob("*.log") if not layout.os_junk(p.name)):
            if count >= 64 or not log.is_file() or log.stat().st_size > 8 << 20:
                continue
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(log, dest / log.name)
            count += 1


def clean_after_stop(c):
    """出包失败或被取消后清解包和中间产物，在错误摘要生成之后调。已完成的着色器缓存、之前成功的 IPA、资源包、容器件、Mac 运行版本和存档都不动；这里出错不改变任务结果，下次空闲时再扫。"""
    try:
        gid = c.r.get("gameId")
        if not any(g.get("id") == gid for g in read(c.library / "catalog.json", {}).get("games", [])):
            return   # 这次失败可能正因为游戏不在库里，没有东西要清，也不该多报一条
        with c.idle("execution") as idle:
            if idle and _residue_disposable(c, gid):
                _keep_stage_logs(c, gid)
                prune_build(c, gid, record=False)
    except NotManaged as error:   # 不是这次失败的原因：只记日志，不作为第二个错误码给玩家看
        print(f"[s2a] 清理中间产物跳过了不在存放位置里的路径：{c.redact(str(error))}", file=sys.stderr)  # 非玩家句
    except (Failure, OSError, ValueError) as error:
        c.diagnostic("清理中间产物没有完成，下次空闲时会再清：" + c.redact(str(error)), code="job_failed", severity="warning")  # 非玩家句


def _residue(c):
    """崩溃、断电、进程被杀留下的解包和中间产物 → [(游戏号, 有可删的中间产物, [半成品运行版本…])]，只列有东西的款。只读，不拿锁"""
    found = []
    for game in c.catalog()["games"]:
        gid = game["id"]
        stale = bool(_build_entries(c, gid))   # 解包、Mac 产物根、iPhone 出包工作目录 build/ios 都在这里（出包日志不算残留）
        stale = stale and _residue_disposable(c, gid)
        partials = list(layout.path(c, gid, "image", "mac.partial", create=False).parent.glob("mac.partial")) + list(layout.path(c, gid, "image", "mac.old", create=False).parent.glob("mac.old"))
        if stale or partials:
            found.append((gid, stale, partials))
    return found


UNSEALED = ".unsealed-v1"   # 产物目录里的标记：只读修复做过了


def unseal_builds(c):
    """旧版本把产物根里的 RomFS 共享存储（build/<键>/roots/.romfs_store）整棵设成只读（Windows 是拒写的访问控制项），
    玩家在访达 / 资源管理器里手动删旧产物删不掉（11-12 维护者转玩家反馈）。新版玩家机不再设只读（pack/roots.sh 文件头）；
    这里把存量一次性加回写权限：每款产物目录做完写标记，以后启动只看一眼标记。外接盘没插着的款下次插上再做。出错不影响读游戏库"""
    for game in c.catalog().get("games", []):
        try:
            d = layout.build_dir(c, game["id"], create=False)
            if not d.is_dir() or (d / UNSEALED).exists():
                continue
            unseal_tree(d)
            (d / UNSEALED).write_text("")
        except (OSError, ValueError, KeyError):
            continue


def unseal_tree(top):
    """top 下（含自己）每个没写位的目录 / 文件加回写权限；Windows 另去掉拒写项与只读属性。链接 / 联接只看本身、不跟进去"""
    top = Path(top)
    if os.name == "nt":
        engine = str(Path(__file__).resolve().parents[2])
        if engine not in sys.path:
            sys.path.insert(0, engine)
        from pack.orch import roots
    def fix(p):
        st = os.lstat(p)
        if stat.S_ISLNK(st.st_mode):
            return
        if os.name == "nt":
            if roots.is_link(p):
                return
            roots._win_acl(Path(p), False)
            if st.st_file_attributes & stat.FILE_ATTRIBUTE_READONLY:
                os.chmod(p, stat.S_IWRITE)
        elif not st.st_mode & stat.S_IWUSR:
            os.chmod(p, stat.S_IMODE(st.st_mode) | stat.S_IWUSR)
    fix(top)
    for dirpath, dirs, files in os.walk(top):
        for name in dirs + files:
            fix(os.path.join(dirpath, name))
        if os.name == "nt":
            dirs[:] = [n for n in dirs if not roots.is_link(os.path.join(dirpath, n))]


def sweep_residue(c):
    """启动器空闲时扫掉崩溃、断电、进程被杀留下的解包和中间产物；有出包在跑时不动。
    先不拿锁扫一遍，真有残留才拿执行锁、锁内再扫一遍再删：catalog 每次读游戏库都调它，扫描期间占着执行锁的话，
    紧接着开始的任务（起局、存档、出包）拿不到锁当场报「另一个任务正在使用游戏库」（11-09 main 守望红：SaveManagementTests 备份完刷新游戏库、查看存档撞锁）"""
    try:
        if not _residue(c):
            return
        with c.idle("execution") as idle:
            if not idle:
                return
            for gid, stale, partials in _residue(c):
                if stale:
                    prune_build(c, gid, record=False)
                for partial in partials:
                    _rm(c, layout.root_of(c, gid), partial)
    except (Failure, OSError, ValueError):
        pass   # 清不掉不影响读游戏库，下次再扫


def prune_build(c, gid, *, retain_compiler=True, record=True):
    """出包结束后清本款解包和中间产物，保留共享编库缓存；显式清缓存用 retain_compiler=False。
    交付件（容器件、资源包）出包收尾已搬进 image/ios，build/ 整个删；有资源包没容器件时留现行产物根，免得装机报 container_stale。
    出包成功后的清理先记一行出包实测（record_pack_usage）：要量清之前的整套占用"""
    if retain_compiler and record:
        game = next((g for g in read(c.library / "catalog.json", {}).get("games", []) if g.get("id") == gid), None)
        if game:
            record_pack_usage(c, game)
    out = layout.ios_work(c, gid, create=False)
    n = 0
    kit_ready = _residue_disposable(c, gid)
    if retain_compiler and kit_ready:
        for p in _build_entries(c, gid):   # 含 iPhone 出包工作目录 build/ios（交付件已搬进 image/ios），只留出包日志 build/ios/logs
            _rm(c, layout.root_of(c, gid), p); n += 1
    elif out.is_dir():
        # 有资源包没容器件（只会是出包收尾存容器件前被打断）：现行产物根是装机的唯一来源，留着，其余照清
        for p in out.iterdir():
            if p.name == "logs":
                continue
            if p.name == "roots" and retain_compiler and p.is_dir() and not _is_link(p):
                n += _prune_roots(c, p)
                continue
            if _windows_layout() and p.name == "work" and retain_compiler and p.is_dir() and not _is_link(p):
                for q in p.iterdir():
                    if q.name == "roots" and q.is_dir() and not _is_link(q):
                        n += _prune_roots(c, q)
                    elif q.name == "pkg" and q.is_dir() and not _is_link(q):
                        for r in q.iterdir():
                            if r.name != "product":
                                _rm_entry(c, r); n += 1
                        if os.path.lexists(q / "product" / "pkg"):
                            _rm_entry(c, q / "product" / "pkg"); n += 1
                    elif not re.fullmatch(r"direct(_p\d+)?", q.name):
                        _rm_entry(c, q); n += 1
                continue
            _rm(c, layout.root_of(c, gid), p); n += 1
    # 直连缓存跟 game_caches 取同一处（direct_cache_dir）：游戏在别的盘时它在这款文件夹的 direct-cache/，不在工具缓存下，
    #   删它按 _direct_base 取托管根（诊断#0f1c42 D-CMJ7：「清理缓存」原先拿工具缓存当根删它，报「操作路径不属于托管目录」）
    direct = direct_cache_dir(c, gid)
    for p in game_caches(c, gid, include_compiler=not retain_compiler):
        if retain_compiler and p.parent == direct:
            n += _rm_direct_transient(c, direct, p)
        else:
            _rm(c, _direct_base(c, direct) if p == direct else c.work_root, p); n += 1
    return n


def drop_from_container(c, gid):
    """游戏库里删了的款从各台手机的记录（devices/<设备>.json）摘掉，不装机：下次给那台装机时现组的容器就不再带它（#227）。
    同时记进这台的 deleted（本机删过的款）：手机上它还在时，组壳提示说「已从这台电脑的游戏库删除」，不说成「在别的电脑上装的」（11-16）"""
    gid = gid.upper()
    for key, rec in layout.devices(c):
        if gid in (rec.get("games") or {}):
            rec["games"].pop(gid)
            rec["deleted"] = sorted(set(rec.get("deleted") or []) | {gid})
            layout.save_device(c, key, rec)


def delete(c):
    """删除这款游戏（连出包产物与缓存）：iPhone 输出整个目录（IPA / 资源包 / 中间产物）、Mac 构建缓存、game_caches，并从游戏库清单里删掉；
    保留存档（library/saves/<程序号>）与游戏原件（原地导入的不碰；旧版复制进 library/sources 的托管副本另有「删除托管 NSP 副本」）。
    锁（#2191 出包时放开别的款）：执行锁只拿共享（和别的款的出包 / 起局并行，挡清理 / 整库搬家等全库独占动作）+ 这一款的按款锁（这款在出包 /
    在玩 / 在升级 S 级优化时等它结束）+ 清单锁。这一款刚被取消的任务收尾要杀子进程组：按款锁排队最多 DELETE_LOCK_WAIT_S 秒，
    期间照锁的排队进度说「在等『X』结束」（启动器剃刀 H2）。原先拿执行锁独占：别的款出包十几分钟里删任何一款都等满 120 秒报忙。
    三份全体共用的编译缓存（shared_compiler_caches：别的款出包 / 起局正在读写）不在这把锁里动，锁外没人在出包、在玩时才顺手清（delete_shared_caches）"""
    import sgrade
    with store_meta_quiet(c), c.lock("execution", shared=True, wait=DELETE_LOCK_WAIT_S), \
            c.lock(sgrade.game_lock(c.r.get("gameId") or ""), wait=DELETE_LOCK_WAIT_S), c.lock():
        gid = c.game()["id"]
        c.begin("删除游戏与出包产物")
        # 上次删到一半（目录删不空）时 game.json 可能已被删掉、find_game 认不出：按索引找回同一个文件夹接着删，
        #   不能当成「已经没有了」只把条目从清单里去掉、留下残留目录（#433）
        gdir = layout.find_game(c, gid) or layout.indexed_dir(c, gid)
        # 先整块挪进存放位置的 .s2a-trash/（同盘改名，原子；有程序正开着里面的文件也挪得动），清单立刻去掉这款，再删挪走的那堆：
        #   原先边删边可能撞上删不掉的文件（被占用 / 有程序在写），抛出去时清单里还留着这款、目录删了一半，界面成了「找不到游戏文件」，
        #   下次再删又卡在同一个文件上（11-11 玩家反馈「删除有时生效有时不生效」，真 App 复现）。挪走的删不干净只记黄字，空闲时 sweep_trash 接着删
        trashed = []
        if gdir and gdir.exists():
            # 存档类（layout.save_entries 唯一来源：saves/、save-backups/、save_backup/、SaveData*）有内容就全部保留，其余照删（金手指、mod 不算存档）
            has_saves = any(layout.has_content(p) for p in layout.save_entries(gdir))   # 只有 .DS_Store 之类不算有存档
            root = gdir.parent.parent   # 这款所在存放位置的根（root_of 靠 find_game，认不出时会退回默认位置）
            for item in ([i for i in gdir.iterdir() if not layout.is_save_entry(i.name)] if has_saves else [gdir]):
                moved = _to_trash(c, root, item, gid)
                if moved:
                    trashed.append((root, moved))
        # 缓存（game_caches：这款的直连库 / 烘焙 / 补丁 / 宽屏，连同删除语义里一并清的共用编库缓存）同样锁内挪进工具缓存根的 .s2a-trash：
        #   锁一放，别的款出包重建共用缓存、重新加回同一款再出包，都不会和锁外的删除撞在同一个目录上
        shared = set(shared_compiler_caches(c))
        for p in game_caches(c, gid):
            if p in shared:
                continue
            moved = _to_trash(c, c.work_root, p, gid)
            if moved:
                trashed.append((c.work_root, moved))
        drop_from_container(c, gid)
        import programs as progs   # 删除游戏 = 从库里删掉、也从各台手机记录摘掉：这款的程序常驻与缓存一并删
        for root, d in ((c.library, layout.program_store(c, gid)), (c.work_root, progs.cache(c, gid))):
            if d.exists():
                moved = _to_trash(c, root, d, gid)
                if moved:
                    trashed.append((root, moved))
        cat = c.catalog()
        cat["games"] = [g for g in cat["games"] if g["id"] != gid]
        atomic(c.library / "catalog.json", cat)
    trashed += delete_shared_caches(c, gid)
    # 真正删文件放在锁外（诊断#dab32d：一款 100 GB 级的游戏在慢盘上删了 53 分钟，整段占着执行锁，期间开别的款 6 次全 library_busy）。
    #   上面已经挪进 .s2a-trash、从清单去掉，别的任务看不到也碰不到这堆文件，不需要锁
    left = []
    for root, p in trashed:
        try:
            _rm(c, root, p)
        except (Failure, OSError) as e:
            if p.exists() or p.is_symlink():   # 删的过程中被别处（启动时的 sweep_trash）删完了，不算没删掉
                left.append(f"{p.name}（{e}）")
    if left:
        c.diagnostic("这款已从游戏库删掉；有几个文件当时删不掉（可能正被别的程序打开），S2Arm 空闲时会再删：" + "；".join(left)[:400],  # 非玩家句
                     code="job_failed", severity="warning")
    return {}


TRASH = ".s2a-trash"   # 每个存放位置根下：删除时先把这款整块挪进来再删（delete），删不干净的空闲时清（sweep_trash）


def _to_trash(c, root, item, gid):
    """把 root 下的 item 改名挪进 root/.s2a-trash/<游戏号>-<毫秒>-<原名>（同盘改名）→ 新路径；已经不在了 → None；挪不动（权限 / 只读盘）照实报"""
    item = Path(os.path.abspath(item))
    root, _ = _managed_anchor(root, item)   # 不在这个存放位置里的、链接都不碰（同 _rm 的托管目录检查）
    if _is_link(item):
        raise NotManaged(NOT_MANAGED)
    dest = root / TRASH / f"{gid}-{int(time.time() * 1000)}-{item.name}"
    n = 1
    while os.path.lexists(dest):   # 同一毫秒挪两个同名的（widen/ 与 widen/auto/ 下同名的缓存目录）
        dest, n = dest.with_name(f"{gid}-{int(time.time() * 1000)}-{n}-{item.name}"), n + 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.rename(item, dest)
    except FileNotFoundError:
        # 列目录后它已经没了：exFAT 上挪走 banner.jpg 时系统连带挪走 ._banner.jpg（AppleDouble），清单里还挂着它的名字。
        #   要删的东西不存在就是删完了，不能整单 job_failed（诊断#3d03f4：删游戏报「删不掉 ._banner.jpg：No such file or directory」）
        return None
    except OSError as e:
        raise Failure(f"删不掉「{item.name}」：{e.strerror or e}。确认这块盘没被设成只读、文件没被别的程序锁住，再删一次。", code="job_failed") from e
    return dest


# 私有目录里作业建的临时目录：TemporaryDirectory(dir=c.private) 的缺省名 tmp + 8 位，装机 / 下游装机的 mkdtemp 前缀。只认这几种名字（我们自己建的）
PRIVATE_TMP = re.compile(r"(tmp|s2a-install-|s2a-ds-install-)[a-z0-9_]{8}")
PRIVATE_TMP_AGE_S = 24 * 3600


def sweep_private_tmp(c, now=None):
    """作业被打断后留在私有目录的临时目录（#1471：S 级优化给 Mac 补着色器的临时产物根 tmp*/roots 被打断就留下，玩家看到几十 GB）。
    启动器开局时清：只清我们自己建的名字（PRIVATE_TMP）、真目录（不跟软链）、本库没有任务在跑（执行锁与自动重活锁都拿得到）、
    且它和第一层内容超过一天没动过（防另一份游戏库 / 另一个启动器实例的任务还在用）。删不掉的下次再试；→ 删了几个"""
    now = time.time() if now is None else now
    try:
        cands = [p for p in c.private.iterdir() if PRIVATE_TMP.fullmatch(p.name) and p.is_dir() and not _is_link(p)]
    except OSError:
        return 0
    if not cands:
        return 0
    n = 0
    with c.idle("execution") as free, c.idle("auto-heavy") as auto_free:
        if not (free and auto_free):
            return 0
        for p in cands:
            try:
                newest = max([p.lstat().st_mtime] + [e.stat(follow_symlinks=False).st_mtime for e in os.scandir(p)])
                if now - newest < PRIVATE_TMP_AGE_S:
                    continue
                _rm(c, c.private, p); n += 1
            except (Failure, OSError):
                continue
    if n:
        print(f"清掉私有目录里作业被打断留下的临时目录 {n} 个", file=sys.stderr)  # 非玩家句
    return n


def sweep_trash(c):
    """删掉各存放位置 .s2a-trash/ 里上次没删干净的（delete 挪进来的、已不在游戏库里）；删不掉下次再试，不影响读游戏库"""
    for root, ok in [(root, ok) for _, root, ok in layout.locations(c)] + [(c.work_root, True)]:   # 工具缓存根：delete 挪进来的这款自己的缓存
        t = root / TRASH
        if not ok or not t.is_dir():
            continue
        for p in t.iterdir():
            try:
                _rm(c, root, p)
            except (Failure, OSError):
                pass


def remove(c):
    with c.lock():
        cat = c.catalog()
        for game in cat["games"]:
            if game["id"] == c.r["gameId"]:
                game["hidden"] = True
        atomic(c.library / "catalog.json", cat)
    return {}


def clean(c):
    with c.lock("execution"), c.lock():
        game = c.game()
        kind = c.r.get("kind")
        if kind == "cache":
            c.begin("清理构建缓存")
            for target in _build_entries(c, game["id"]):   # 出包日志 build/ios/logs 留着
                _rm(c, layout.root_of(c, game["id"]), target)
            shader = layout.path(c, game["id"], "shader-cache", create=False)   # 清缓存 = 本款出包缓存 + 运行时着色器缓存（游戏库结构双端统一「清缓存」）
            if shader.is_dir():
                _rm(c, layout.root_of(c, game["id"]), shader)
            prune_build(c, game["id"], retain_compiler=False)   # 用户显式清缓存保留原有全清语义，不套用 Q15 自动保留策略
            from cache_retention import engine_installed_at, prune_tool_keys
            prune_tool_keys(c.work_root, engine_installed_at(c.engine))   # 顺带清当前引擎用不到的工具缓存旧键
            from cache_retention import trim_build_caches
            trim_build_caches(c.work_root / "run" / "s2arm")   # 出包可重建缓存总量上限（同 ipa.py 出包收尾）
        elif kind == "sources":
            # 只删 0.1.0-beta.3 及以前复制进游戏库的托管副本；原地导入的原件是玩家的文件，只读、不删
            other = {p["path"] for g in c.catalog()["games"] if g["id"] != game["id"] for p in g["packages"]}
            for p in game["packages"]:
                if not Path(p["path"]).is_absolute() and p["path"] not in other:
                    contained(c.library / "sources", c.library / p["path"]).unlink(missing_ok=True)
        else:
            raise Failure("未知清理类型。")
        sizes.refresh(c, game["id"])
    return {}


# —— 释放空间（11-07 启动器，参考 NX2Apple frontend FreeSpace.swift 的部件划分）——
# 每款按部件列出占用，玩家勾选后删除。只列「下次出包会重建」的部件和已出好的包；存档、日志、配置、清单、游戏原件只报大小、不给删。
#   直接删，不放废纸篓（废纸篓仍占同一块盘）。有任务在跑（执行锁拿不到）拒绝，不等。
SPACE_PARTS = (
    ("legacy", "旧版残留", "旧版本留下的解包和中间文件，出包成功后不再保留；删了不影响已出好的包和 Mac 版本"),
    ("shaders", "着色器与出包缓存", "下次出包会重新翻译着色器（大游戏要多花较长时间）"),
    ("mac", "Mac 运行版本", "下次在 Mac 上玩之前会重新准备"),
    ("ipa", "已出好的 iPhone 包", "只删资源包：手机上已装的不受影响，以后更新 S2Arm 也照样带上这款；要装到别的手机或重新推资源时再出一次包"),
    ("selfsign", "自签导出的游戏文件", "「S2Arm 自签导出」文件夹里这款的 .img：手机上已导入的不受影响；要再装到手机时重新「导出自签包」"),
)
SPACE_KEPT = ("saves", "logs")


def tvos_ready(c, gid):
    import tvos
    return tvos.tv_ready(c, gid)


def iphone_ready(c, gid):
    """这款的 iPhone 版能不能装（catalog 每款都问，只看几个文件，不逐文件读）：容器件主程序表 main.npdm 在且非空、有改写产物，资源包清单在。
    审计 P2-212：原先只看「有 ipa + 有资源包目录」，容器件缺 main.npdm 也报就绪，点装机才报错；逐文件完整核对仍在装机组装时（pack/ship_ext.py check_root / place_game）"""
    pack = layout.ios_pack(c, gid)
    try:   # 程序件只问「有没有」（缓存 kit 或常驻配方），不现建：游戏库清单每款都问，现建要跑改写器
        return layout.has_ios_kit(c, gid) and (pack / "manifest.json").is_file()
    except OSError:
        return False


def iphone_img(c, gid):
    """这款的 iPhone 资源包是不是单一 IMG 形态（manifest format = img，同 install.img_pack）：是才能直接导出成 .img；
    旧格式（散文件 / tar）点「导出自签包」时先重出 iPhone 包再导出（11-15 维护者：玩家点的是导出，就把导出做完）"""
    try:
        return json.loads((layout.ios_pack(c, gid) / "manifest.json").read_text(encoding="utf-8")).get("format") == "img"
    except (OSError, ValueError, AttributeError):
        return False


def space_parts(c, gid):
    """{部件 id: [路径…]}，外加不给删的 saves / logs。路径都在游戏库或私有目录之内；不存在的不列"""
    out = layout.ios_work(c, gid, create=False)
    shared = {c.work_root / "cache" / "air", c.work_root / "cache" / "pso", c.work_root / "run" / "gal_dl_cache"}   # 各款共用的编库缓存：不归任何一款
    layout.ios_kit(c, gid)   # 旧库程序件还在 image/ios/kit 的，先搬进专用文件夹（不随 iPhone 包一起删）
    ipa = [layout.ios_pack(c, gid)]   # 只删资源包：程序件在专用文件夹（layout.program_store），删了 iPhone 包也照样能更新 S2Arm、带上这款
    gdir = layout.find_game(c, gid)
    parts = {
        "legacy": _build_entries(c, gid),   # 不含出包日志（下面 logs 项，只报不删）
        "shaders": [p for p in game_caches(c, gid) if p not in shared] + [Path(c.work_root) / "cache" / "programs" / gid.upper()],   # 程序缓存：缺了按常驻配方现建（pack/jobs/programs.py）；出包收尾不清它
        "mac": [layout.image_mac(c, gid, create=False)] if gdir and gdir.exists() else [],
        "ipa": ipa,
        "saves": layout.save_entries(gdir) if gdir else [],   # 只报不删（SPACE_KEPT）；存档类唯一来源 layout.save_entries
        "logs": [out / "logs"],
        "selfsign": selfsign_imgs(c, gid),
    }
    return {k: [p for p in v if p.exists() or p.is_symlink()] for k, v in parts.items()}


def selfsign_imgs(c, gid):
    """「S2Arm 自签导出」文件夹（中 / 英两个定名都看）里这款的 .img（完整的和着色器更新包）：单独写出、不和 games/ 共用磁盘块，
    每款等于再占一份（#1471）。按文件头里的游戏号认，不按文件名"""
    import install
    out = []
    for name in install.EXPORT_DIRS.values():
        folder = c.library / name
        if folder.is_dir():
            out += [p for p in layout.entries(folder, "*.img") if p.is_file() and install._img_head(p)[0] == gid.upper()]
    return out


def _part_bytes(paths):
    by_dev = {}
    for p in paths:
        try:
            by_dev.setdefault(p.lstat().st_dev, []).append(p)
        except OSError:
            pass
    return sum(disk_bytes(v, dev) for dev, v in by_dev.items())


def freeable(parts):
    """{部件 id: [路径…]} → {勾选组合键: 删掉这几项真能腾出的字节}，组合键是排好序的部件 id 用「+」连（全部非空组合都给）。
    只按目录各自量再相加会多算（玩家报：一款游戏 Mac 运行版本 15.45 GB、iPhone 包 15.13 GB，可两份 romfs 是同一组 APFS 克隆块，
    单删 iPhone 包实际只腾出约 1.5 GB）。按「腾出单位」算：
      · 每个 inode 的独占字节（克隆文件取 ATTR_CMNEXT_PRIVATESIZE，否则 st_blocks）——它的硬链接（st_nlink 份）全在勾选里才腾出；
      · 每个克隆族的共享块（各份 已分配 − 独占 取最大）——族里的份数（CLONE_REFCNT）全在勾选里才腾出。
    族里还有份在别处（别的部件没勾、解包目录、别款）就不算腾出。同卷才算（不跟软链、不跨卷，同 disk_bytes）；Windows 没有克隆，只剩硬链接这条"""
    units, seen_inodes = {}, {}   # 单位键 → [字节, 要几份, {部件: 已见份数}]
    def hold(key, size, need, part):
        u = units.setdefault(key, [0, need, {}])
        u[0] = max(u[0], size); u[2][part] = u[2].get(part, 0) + 1
    for part, roots in parts.items():
        visited = set()   # 本部件走过的目录（按 inode）和文件（按路径）：根互相包含只记一次；同一 inode 的两条硬链接各算一份
        for root in roots:
            try:
                device = Path(root).lstat().st_dev
            except OSError:
                continue
            stack = [os.fspath(root)]
            while stack:
                path = stack.pop()
                try:
                    st = os.lstat(path)
                except OSError:
                    continue
                if st.st_dev != device or stat.S_ISLNK(st.st_mode):
                    continue
                key = ("inode", st.st_dev, st.st_ino)
                mark = key if stat.S_ISDIR(st.st_mode) else os.path.normpath(path)
                if mark in visited:
                    continue
                visited.add(mark)
                blocks = getattr(st, "st_blocks", None)
                alloc = blocks * 512 if blocks is not None else st.st_size
                if stat.S_ISDIR(st.st_mode):
                    hold(key, alloc, 1, part)
                    try:
                        with os.scandir(path) as it:
                            stack.extend(e.path for e in it)
                    except OSError:
                        pass
                    continue
                clone = _CloneInfo.get(path) if stat.S_ISREG(st.st_mode) and alloc else None
                if clone and clone[2] >= 2 and 0 <= clone[0] <= alloc:
                    hold(key, clone[0], max(1, st.st_nlink), part)
                    if key not in seen_inodes.setdefault(clone[1], set()):   # 族里的份数按 inode 数（硬链接不算多一份）
                        seen_inodes[clone[1]].add(key)
                        hold(("clone", st.st_dev, clone[1]), alloc - clone[0], clone[2], part)
                else:
                    hold(key, alloc, max(1, st.st_nlink), part)
    ids = sorted(parts)
    out = {}
    for mask in range(1, 1 << len(ids)):
        chosen = {ids[i] for i in range(len(ids)) if mask >> i & 1}
        out["+".join(sorted(chosen))] = sum(size for size, need, held in units.values() if sum(n for p, n in held.items() if p in chosen) >= need)
    return out


def space_key(ids):
    """勾选组合 → freeable 的键"""
    return "+".join(sorted(ids))


def space_survey(c):
    """每款（gameId 给了只这一款）按部件的占用（字节，按所在卷实测）：parts 可勾选删除，kept 只报不删"""
    games = c.catalog()["games"]
    gid = c.r.get("gameId")
    if gid:
        games = [g for g in games if g["id"] == gid]
        if not games:
            raise Failure("游戏未登记在当前游戏库中。")
    c.begin("统计各游戏占用的空间")
    rows = []
    for g in games:
        c.check()
        parts = space_parts(c, g["id"])
        shown = [(k, t, r) for k, t, r in SPACE_PARTS if k not in ("legacy", "selfsign") or parts[k]]   # 没有旧版残留 / 没自签导出过这款就不显示
        combos = freeable({k: parts[k] for k, _, _ in shown})
        # 每行是「只删这一项」腾出的字节；几项共用的克隆块只在都勾上时算进合计（combos），sharedBytes = 全勾比各项单删相加多出的部分
        rows.append(dict(gameId=g["id"], name=game_custom(c, g["id"])["name"] or g.get("name") or g["id"],
                         parts=[dict(id=k, title=t, rebuild=r, bytes=combos[k]) for k, t, r in shown],
                         combos=combos, sharedBytes=combos[space_key(k for k, _, _ in shown)] - sum(combos[k] for k, _, _ in shown),
                         kept=[dict(id=k, bytes=_part_bytes(parts[k])) for k in SPACE_KEPT]))
    return dict(games=rows)


def space_free(c):
    """删这款勾选的部件（request.parts = 部件 id 列表，只认 SPACE_PARTS 里的）。有任务在跑拒绝；返回各部件删前字节与释放合计"""
    want = c.r.get("parts")
    ids = {k for k, _, _ in SPACE_PARTS}
    if not isinstance(want, list) or not want or any(p not in ids for p in want):
        raise Failure("要删除的部件不对：只能选" + "、".join(t for _, t, _ in SPACE_PARTS) + "。")  # 非玩家句
    with c.lock("execution"), c.lock():
        gid = c.game()["id"]
        c.begin("释放空间")
        if "legacy" in want:   # 老玩家的现行产物根是装机的唯一来源：删之前先存成容器件，否则装机会报 container_stale
            from ipa import product_root
            root = product_root(c, gid)
            if root is not None and not layout.has_ios_kit(c, gid):
                save_container_kit(c, gid, root)
        parts = space_parts(c, gid)
        # 腾出多少按删前的组合账算（同 space_survey 的合计）：逐项「删前 − 删后」会把共用克隆块记在最后删的那项上、单项又各算一遍
        combos = freeable({k: parts[k] for k, _, _ in SPACE_PARTS})
        freed, done = combos[space_key(want)], []
        for k in [k for k, _, _ in SPACE_PARTS if k in want]:
            for p in parts[k]:
                root = c.work_root if p.is_relative_to(c.work_root) else c.library if k == "selfsign" else layout.root_of(c, gid)
                _rm(c, root, p)
            done.append(dict(id=k, bytes=combos[k]))
        sizes.refresh(c, gid)
        return dict(gameId=gid, parts=done, freedBytes=freed)


def validate_input(config):
    if not isinstance(config, dict) or config.get("schema") != 1:
        raise Failure("不支持的输入映射格式。")
    targets = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
               "LEFT", "UP", "RIGHT", "DOWN", "LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT",
               "RS_UP", "RS_DOWN", "RS_LEFT", "RS_RIGHT"}
    pad = {"east", "south", "north", "west", "leftStick", "rightStick", "leftShoulder", "rightShoulder",
           "leftTrigger", "rightTrigger", "menu", "options", "dpadLeft", "dpadUp", "dpadRight", "dpadDown"}
    axes = {"leftX", "leftY", "rightX", "rightY"}
    for name in ("keyboard", "mouse", "gamepad", "axes"):
        if not isinstance(config.get(name), dict):
            raise Failure("输入映射不完整。")
        seen = set()
        for target, binding in config[name].items():
            if target not in ({"LX", "LY", "RX", "RY"} if name == "axes" else targets):
                raise Failure("无效输入目标。")
            if name == "keyboard":
                if not isinstance(binding, dict) or type(binding.get("code")) is not int or not 0 <= binding["code"] <= 127 or type(binding.get("modifiers", 0)) is not int or not 0 <= binding.get("modifiers", 0) <= 7:
                    raise Failure("无效键盘映射。")
                token = (binding["code"], binding.get("modifiers", 0))
            elif name == "mouse":
                if type(binding) is not int or not 0 <= binding <= 4:
                    raise Failure("无效鼠标映射。")
                token = binding
            elif name == "gamepad":
                if not isinstance(binding, str) or binding not in pad:
                    raise Failure("无效手柄映射。")
                token = binding
            else:
                if not isinstance(binding, dict) or binding.get("source") not in axes or type(binding.get("invert", False)) is not bool:
                    raise Failure("无效摇杆映射。")
                token = binding["source"]
            if token in seen:
                raise Failure("输入映射存在重复绑定。")
            seen.add(token)
    back = config.get("backButtons", {})
    if not isinstance(back, dict) or any(side not in {"GL", "GR"} or not isinstance(source, str) or source not in pad for side, source in back.items()):
        raise Failure("无效手柄映射。")
    if not isinstance(config.get("deadzone"), (int, float)) or not 0 <= config["deadzone"] < 1:
        raise Failure("摇杆死区必须在 0 到 1 之间。")
    options = config.get("mouseOptions")
    if not isinstance(options, dict) or options.get("mode") not in ("camera", "off"):
        raise Failure("无效鼠标选项。")
    for name in ("sensitivityX", "sensitivityY"):
        if type(options.get(name)) not in (int, float) or not .1 <= options[name] <= 10:
            raise Failure("鼠标灵敏度必须在 0.1 到 10 之间。")
    for name in ("invertX", "invertY", "recenter"):
        if type(options.get(name, False)) is not bool:
            raise Failure("无效鼠标选项。")
    return config


def _usable_input(path):
    """这份键位文件能不能用：在、是 JSON、过 validate_input → (配置, 有没有问题)。不在 → (None, False)；在但读不出 / 不合规 → (None, True)"""
    if not path.is_file():
        return None, False
    try:
        return validate_input(read(path)), False
    except (Failure, ValueError, OSError):   # ValueError 含 JSONDecodeError（core.read 只接编码问题，非 JSON 原样抛）
        return None, True


def effective_input(c, gid, player=1):
    """某位玩家这款实际生效的键位（#2081：起局 build.py 与键位弹窗 loadInput 作业都只调它，两边判同一份）→
    dict(config=配置或 None（标准键位）, source=game / default / standard / player1, problem=存过的那份读不出或不合规)。
    玩家 1：这款的 input.json → 私有目录 default-input.json → 标准键位，每一份都要读得出且过 validate_input，不行就往下一份走；
    玩家 2–4：这款的 input-pN.json 能用就用，否则沿用玩家 1 实际生效的那套（source=player1）"""
    if player != 1:
        config, problem = _usable_input(layout.path(c, gid, f"input-p{player}.json", create=False))
        if config is not None:
            return dict(config=config, source="game", problem=False)
        first = effective_input(c, gid, 1)
        return dict(config=first["config"], source="player1", problem=problem)
    config, problem = _usable_input(layout.path(c, gid, "input.json", create=False))
    if config is not None:
        return dict(config=config, source="game", problem=False)
    fallback, _ = _usable_input(c.private / "default-input.json")
    return dict(config=fallback, source="default" if fallback is not None else "standard", problem=problem)


def load_input(c):
    """作业 loadInput（#2081）：键位弹窗读四位玩家实际生效的键位，规则同起局（effective_input）；config 为 None = 标准键位。
    default（#1987）：所有游戏的默认，default-input.json 能用时的那份，否则 None（没设过 = 标准键位）；弹窗「恢复默认」回到它。
    defaultProblem：default-input.json 在但读不出或不合规（这时 default 为 None，各款按标准键位走），弹窗提示一行，不再静默退回"""
    gid = c.game()["id"]
    default, default_problem = _usable_input(c.private / "default-input.json")
    return dict(players=[dict(player=p, **effective_input(c, gid, p)) for p in (1, 2, 3, 4)],
                default=default, defaultProblem=default_problem)


# 标准键位（#1987）：没设所有游戏的默认时，「生效的缺省」就是它。同 Mac 启动器 InputConfig.standard（S2ArmLauncherModel+Input.swift）
#   与宿主 s2a_input_config_default（runtime/src/input_config.c）；test_jobs 读 Swift 原文逐项核对，改一边记得改另一边。
#   gamepad：前 16 个按键位依次对 16 个手柄键；axes：四轴各照各的源
STANDARD_INPUT = dict(
    schema=1,
    keyboard={t: {"code": k} for t, k in dict(A=14, B=56, X=49, L=12, R=5, ZR=15, LS=59, RS=8, PLUS=48, MINUS=46, LEFT=123, UP=126,
                                                 RIGHT=124, DOWN=125, LS_UP=13, LS_DOWN=1, LS_LEFT=0, LS_RIGHT=2).items()},
    mouse=dict(Y=0, ZL=1),
    gamepad=dict(zip(("A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"),
                     ("east", "south", "north", "west", "leftStick", "rightStick", "leftShoulder", "rightShoulder", "leftTrigger",
                      "rightTrigger", "menu", "options", "dpadLeft", "dpadUp", "dpadRight", "dpadDown"))),
    axes=dict(LX={"source": "leftX"}, LY={"source": "leftY"}, RX={"source": "rightX"}, RY={"source": "rightY"}),
    deadzone=.15,
    mouseOptions=dict(mode="camera", sensitivityX=1, sensitivityY=1, invertX=False, invertY=False, recenter=True))


def _input_norm(config):
    """两份键位「是不是同一套」（#1987 存时和生效的缺省比）：按宿主的读法比——省略的 modifiers / invert / invertX / invertY 记缺省值，
    recenter 省略记开，没有 backButtons 与空的一样，数值按大小比（1 与 1.0 相同）；schema 与契约外的多余字段不参与"""
    o = config.get("mouseOptions") or {}
    return (
        {t: (b["code"], b.get("modifiers", 0)) for t, b in config["keyboard"].items()},
        dict(config["mouse"]), dict(config["gamepad"]),
        {t: (a["source"], a.get("invert", False)) for t, a in config["axes"].items()},
        dict(config.get("backButtons") or {}), float(config["deadzone"]),
        (o.get("mode"), float(o.get("sensitivityX", 1)), float(o.get("sensitivityY", 1)),
         o.get("invertX", False), o.get("invertY", False), o.get("recenter", True)))


def _same_input(a, b):
    return _input_norm(a) == _input_norm(b)


def effective_default_input(c):
    """生效的缺省（#1987）：「恢复默认」回到的、这款「和它一样就不单独存」比的那一套。
    私有目录 default-input.json 读得出且合规就是它（同 effective_input 的回落），否则标准键位"""
    config, _ = _usable_input(c.private / "default-input.json")
    return config if config is not None else STANDARD_INPUT


def _input_player(c, need_game=False):
    """键位作业的玩家序号（1–4；没给 = 1）。玩家 2–4 的键位只存在这款文件夹里，必须带 gameId；need_game：这个作业只对某一款"""
    player = c.r.get("player", 1)
    if type(player) is not int or not 1 <= player <= 4 or ((player != 1 or need_game) and not c.r.get("gameId")):
        raise Failure("无效的玩家序号。")
    return player


def _game_input_path(c, player):
    c.game()
    return layout.path(c, c.r["gameId"], "input.json" if player == 1 else f"input-p{player}.json")


def save_input(c):
    config = validate_input(c.r["config"])
    # player：键位设置的玩家（1–4；1 / 没给 = 玩家 1 的 input.json，2–4 = 这款文件夹的 input-pN.json）。
    #   起局时 build.py 读这些文件交给宿主（S2A_INPUT_CONFIG_P2..P4，槽位各照各玩家的映射采样）
    # #1987（同 iPad #2079 s2a_kbd_store_save）：玩家 1 存这款时和生效的缺省比，相同就删掉这款的 input.json、以后跟着所有游戏的默认走；
    #   不带 gameId 存的就是所有游戏的默认，和标准键位相同就删掉 default-input.json（= 没设过）。玩家 2–4 照旧写自己的文件。
    #   返回 followsDefault：存完是不是跟着生效的缺省走（没单独存文件）
    player = _input_player(c)
    if c.r.get("gameId"):
        target = _game_input_path(c, player)
        follows = player == 1 and _same_input(config, effective_default_input(c))
    else:
        target = c.private / "default-input.json"
        follows = _same_input(config, STANDARD_INPUT)
    if follows:
        target.unlink(missing_ok=True)
    else:
        atomic(target, config)
    return dict(followsDefault=follows)


def reset_input(c):
    """作业 resetInput（#1987「恢复默认」，同 iPad s2a_kbd_store_restore）：删掉这款这位玩家单独存的键位。
    玩家 1 → 回到所有游戏的默认（没设过就是标准键位）；玩家 2–4 → 回到沿用玩家 1（同起局 effective_input）。没存过也算成功"""
    _game_input_path(c, _input_player(c, need_game=True)).unlink(missing_ok=True)
    return {}


def set_default_input(c):
    """作业 setDefaultInput（#1987「设为所有游戏的默认」，同 iPad #2079 s2a_kbd_store_set_default）：只对玩家 1。
    写 default-input.json，和标准键位相同就删掉它（= 没设过）；带 gameId 时这款这时和默认相同，删掉这款的 input.json。
    Mac 键位弹窗点「保存」时才调它，没有撤销（#1987 维护方定：弹窗按「保存才写」一种规矩），所以不留覆盖前的快照"""
    config = validate_input(c.r["config"])
    if c.r.get("player", 1) != 1:
        raise Failure("所有游戏的默认只有玩家 1 的一份。")
    dflt = c.private / "default-input.json"
    mine = _game_input_path(c, 1) if c.r.get("gameId") else None
    if _same_input(config, STANDARD_INPUT):
        dflt.unlink(missing_ok=True)
    else:
        atomic(dflt, config)
    if mine:
        mine.unlink(missing_ok=True)
    return {}


def signing_status():
    try:
        from install import signing_summary
        return signing_summary()
    except Exception as error:
        return {"error": type(error).__name__}


def diagnostics(c):
    # Keep one path/rule engine, with UI truncation disabled for exported files.
    import platform
    import plistlib
    import subprocess

    def redact(value):
        return c.redact(value, limit=None)

    task_prefix = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")

    def redact_member_component(value):
        # This position is a generated task ID, not a device identifier.
        match = task_prefix.match(value)
        return match[0] + redact(value[match.end():]) if match else redact(value)

    def redact_fields(value):
        if isinstance(value, str):
            return redact(value)
        if isinstance(value, dict):
            return {key: redact_fields(item) for key, item in value.items()}
        if isinstance(value, list):
            return [redact_fields(item) for item in value]
        return value

    def probe(args):
        try:
            result = subprocess.run(args, env=c.env(), capture_output=True, text=True, timeout=10)
            if result.returncode:
                return {"error": f"命令失败（退出码 {result.returncode}）"}  # 非玩家句
            return result.stdout.strip() or {"error": "命令输出为空"}  # 非玩家句
        except subprocess.TimeoutExpired:
            return {"error": "命令超过 10 秒"}
        except OSError:
            return {"error": "命令无法执行"}  # 非玩家句

    def launcher_version():
        # Installed Jobs live below Contents/Resources; source runs have no
        # launcher identity and must say so rather than invent a version.
        contents = next((p for p in Path(__file__).resolve().parents if p.name == "Contents"), None)
        if contents is None:
            return {"error": "源码运行，没有启动器 Info.plist"}  # 非玩家句
        try:
            info = plistlib.loads((contents / "Info.plist").read_bytes())
            value = info.get("CFBundleShortVersionString")
            return value if isinstance(value, str) and value.strip() else {"error": "启动器版本字段缺失"}  # 非玩家句
        except (OSError, ValueError, plistlib.InvalidFileException):
            return {"error": "启动器 Info.plist 无法读取"}  # 非玩家句

    destination = resolve_path(Path(c.r["path"]).expanduser())
    library = resolve_path(c.library)
    if c.private == destination or c.private in destination.parents or library == destination or library in destination.parents:
        raise Failure("诊断包请选择应用数据和游戏库以外的位置。")
    descriptors = []
    temporary = None
    step = "打开日志目录"   # 失败时写进报错：哪一步、什么错误（#427：SMB 共享盘上导不出，原先只有一句笼统的话，看不出卡在哪）
    try:
        flags = 0 if os.name == "nt" else os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_NONBLOCK
        # 日志与任务记录在 core.logs_root（固定 <私有目录>/logs，任务记录在 logs/jobs/，剃刀 H6）：逐级不跟链接打开
        home_fd = winproc.open_dir_nofollow(c.logs.parent) if os.name == "nt" else os.open(c.logs.parent, flags)
        descriptors.append(home_fd)
        roots = {}
        # Context 建任务时已写下本任务记录，logs/ 与 logs/jobs/ 都在；不是真目录（链接）照样拒
        roots["logs"] = winproc.open_dir_nofollow("logs", home_fd) if os.name == "nt" else os.open("logs", flags, dir_fd=home_fd)
        descriptors.append(roots["logs"])
        roots["jobs"] = winproc.open_dir_nofollow("jobs", roots["logs"]) if os.name == "nt" else os.open("jobs", flags, dir_fd=roots["logs"])
        descriptors.append(roots["jobs"])
        c.begin("导出脱敏诊断")

        step = "在所选位置建临时文件"
        destination.parent.mkdir(parents=True, exist_ok=True)   # #917：命令行给的路径上一级还不存在（玩家指定的就是要放那里）：建好再写，不报错不回落桌面
        fd, name = tempfile.mkstemp(prefix="." + destination.name + ".", suffix=".partial", dir=destination.parent)
        temporary = Path(name)
        with os.fdopen(fd, "w+b") as output:
            created = os.fstat(output.fileno())
            with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
                def fits(member, estimate):
                    # 整包预算：已写进 zip 的字节（顺序写，output.tell() 即压缩后已用）+ 这份的压缩后估算放得下才放；放不下记一笔
                    if output.tell() + estimate + 512 > DIAGNOSTIC_ZIP_BUDGET:
                        budget_skipped.append(member)
                        return False
                    return True

                def packed(data):
                    return len(zlib.compress(data, 6))   # 同 ZIP_DEFLATED 缺省级别：估压缩后大小

                def write_file(parent_fd, name, member, job=False):
                    # #917：在跑的作业（含这次导出自己）一直原子替换自己的任务记录，查名字到打开之间换了一份（inode 变）很常见；
                    #   原先整包报「发生变化，请重试」不生成，重试照样撞上。改为重查名字再开，三次都在换就跳过这一份、记进 manifest changed
                    for _ in range(3):
                        c.check()
                        try:
                            info = winproc.stat_at(name, parent_fd) if os.name == "nt" else os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
                        except FileNotFoundError:
                            return   # 列目录后被删（作业收尾清理）：没有可收的
                        if not stat.S_ISREG(info.st_mode):
                            return
                        try:
                            if os.name == "nt":
                                source_file = winproc.open_file(name, parent_fd)
                            else:
                                file_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent_fd)
                                source_file = os.fdopen(file_fd, "rb")
                        except FileNotFoundError:
                            return
                        held = contextlib.ExitStack()
                        source = held.enter_context(source_file)
                        opened = winproc.file_info(source) if os.name == "nt" else os.fstat(source.fileno())
                        if (opened.st_dev, opened.st_ino) == (info.st_dev, info.st_ino):
                            break
                        held.close()
                    else:
                        changed.append(member)
                        return
                    with held:
                        if not stat.S_ISREG(opened.st_mode):
                            return
                        stem, suffix = os.path.splitext(member)
                        # Archive separators are known structure, not email local-part characters.
                        public_stem = "/".join(redact_member_component(part) for part in stem.split("/"))
                        public_member, n = public_stem + suffix, 1
                        # 两个文件脱敏后同名（文件名里的设备号、邮箱之类被换成同一个标签）：后一个加序号照样收进去。
                        #   原先整包报「文件名重复」不生成，玩家交不出诊断包（#457）
                        while public_member in archive.namelist() or public_member == "manifest.json":
                            n += 1
                            public_member = f"{public_stem} ({n}){suffix}"
                        if not fits(public_member, min(opened.st_size, DIAGNOSTIC_JOB_LIMIT if job else DIAGNOSTIC_LOG_LIMIT) // 4):
                            return   # 文本按 1/4 估（实测日志压缩约 1/10，留余量）
                        if job:
                            # 超过单份上限的任务记录（长局 / AI 连续操作攒下的事件）：留前 4 MiB、记进 manifest truncated，不再整包拒绝（#917）
                            limit = DIAGNOSTIC_JOB_LIMIT
                            data = source.read(limit + 1)
                            if len(data) > limit:
                                truncated.append(dict(file=public_member, bytes=opened.st_size, kept=limit, omitted=max(opened.st_size, len(data)) - limit))
                                data = data[:limit]
                            # These allowlisted job/log files are text. Invalid UTF-8 is
                            # replaced, never copied as raw bytes around redaction.
                            archive.writestr(public_member, redact(data.decode(errors="replace")))
                            return
                        # 日志：逐块脱敏、流式写进 zip（整份可达上百 MB，不整份进内存）。块在换行处切开，脱敏规则不会碰到半行
                        chunks = diagnostic_log_chunks(source, opened.st_size)
                        with archive.open(public_member, "w", force_zip64=True) as out:
                            carry = b""
                            while True:
                                try:
                                    piece = next(chunks)
                                except StopIteration as done:
                                    kept, omitted = done.value
                                    break
                                c.check()
                                piece = carry + piece
                                cut = piece.rfind(b"\n") + 1
                                carry, piece = (piece[cut:], piece[:cut]) if cut else (piece, b"")
                                if piece:
                                    out.write(redact(piece.decode(errors="replace")).encode())
                            if carry:
                                out.write(redact(carry.decode(errors="replace")).encode())
                        if omitted:
                            truncated.append(dict(file=public_member, bytes=opened.st_size, kept=kept, omitted=omitted))

                step = "读日志与任务记录"
                truncated = []
                changed = []   # 导出期间一直在换、跳过的（#917）
                budget_skipped = []   # 整包预算放不下、跳过的成员（DIAGNOSTIC_ZIP_BUDGET）
                # 只带最近 DIAG_JOBS 个任务（按记录改动时间）+ 还没结束的：任务记录 / 各任务日志原先整目录收，用久了上万个文件，
                #   超过上传服务 1000 个文件上限（「文件数超过 1000 个」）、打包也慢（11-13 维护者机器 703 个任务 + 12296 个任务日志）
                picked, job_total, included = set(), 0, 0
                if roots["jobs"] is not None:
                    records = []
                    for name in (winproc.listdir(roots["jobs"]) if os.name == "nt" else os.listdir(roots["jobs"])):
                        if name.endswith(".json") and not name.endswith(".request.json"):
                            try:
                                info = winproc.stat_at(name, roots["jobs"]) if os.name == "nt" else os.stat(name, dir_fd=roots["jobs"], follow_symlinks=False)
                            except OSError:
                                continue
                            records.append((info.st_mtime, name))
                    records.sort(reverse=True)
                    job_total = len(records)
                    failed_kept, pack_games = 0, set()
                    for rank, (_, name) in enumerate(records):
                        task = name[:-5]
                        try:   # 坏的任务记录（空 / 半截 / 非 UTF-8）不拖垮整个导出：读不出当空记录，只按排名定收不收（原先只给 50 名之后的读，现在每份都读）
                            record = read(c.jobpath.parent / name, {}) or {}
                        except (OSError, ValueError):
                            record = {}
                        if not isinstance(record, dict): record = {}
                        keep = rank < DIAG_JOBS or record.get("state") in ("queued", "running")
                        if record.get("state") == "failed" and failed_kept < DIAG_FAILED:   # 最近的失败（新的在前）：不论排在第几
                            failed_kept += 1; keep = True
                        game = record.get("gameId")
                        if record.get("operation") in DIAG_PACK_OPS and game and game not in pack_games and len(pack_games) < DIAG_PACK_GAMES:
                            pack_games.add(game); keep = True   # 这款最近一次出包类任务（记录按改动时间新的在前，第一次碰到的就是最近的）
                        if keep:
                            picked.add(task.lower()); included += 1
                            write_file(roots["jobs"], name, "jobs/" + task + ".txt", job=True)
                picked.add(c.task.lower())   # 本次导出自己
                skipped_logs = [0]

                def logs(directory_fd, relative=""):
                    for name in sorted(winproc.listdir(directory_fd) if os.name == "nt" else os.listdir(directory_fd)):
                        c.check()
                        info = winproc.stat_at(name, directory_fd) if os.name == "nt" else os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                        member = relative + name
                        if not relative and name == "jobs":
                            continue   # 任务记录上面已按 jobs/ 收过
                        owner = task_of_log(name) if not relative else None
                        if owner and owner not in picked:   # 没选进来的任务的日志（<任务>.log、<任务>-install.log、<任务>/…）：不收，只计数
                            skipped_logs[0] += 1
                            continue
                        if stat.S_ISDIR(info.st_mode):
                            child = winproc.open_dir_nofollow(name, directory_fd) if os.name == "nt" else os.open(name, flags, dir_fd=directory_fd)
                            try:
                                opened = child.info if os.name == "nt" else os.fstat(child)
                                if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                                    changed.append(member + "/")   # 列到打开之间被换掉的目录：跳过、记一笔（#917）
                                    continue
                                logs(child, member + "/")
                            finally:
                                winproc.close(child) if os.name == "nt" else os.close(child)
                        elif name.endswith(".log"):
                            write_file(directory_fd, name, "logs/" + member)
                if roots["logs"] is not None:
                    logs(roots["logs"])
                # 手机诊断（s2armctl diag pull / 设置页「拉回 iPhone 诊断」，pack/jobs/diag.py pull）：拉回在 <私有目录>/diagnostics/<时刻>-…/，
                #   原先导出不收，照「反复闪退：先 diag pull 再 diag export」做的玩家交上来的包里没有手机日志。只收最近一次拉回的那份；
                #   文本走和 logs/ 同一套脱敏（拉回的是没脱敏的原始文件），渲染出的 PNG 同诊断局读数原样收、限大小，别的类型跳过并在 manifest 里写明
                step = "读手机诊断"  # 非玩家句
                phone = None
                pulls = c.private / "diagnostics"
                latest = max((d for d in pulls.iterdir() if d.is_dir() and not d.is_symlink()), key=lambda d: d.stat().st_mtime, default=None) \
                    if pulls.is_dir() and not pulls.is_symlink() else None
                if latest is not None:
                    phone = dict(folder=latest.name, files=0, skipped=[])
                    try:
                        phone.update({k: v for k, v in json.loads((latest / "pull.json").read_text(encoding="utf-8")).items() if k in ("pulledAt", "devices", "apps")})
                    except (OSError, ValueError):
                        phone["pulledAt"] = time.strftime("%Y-%m-%d %H:%M:%S %z", time.localtime(latest.stat().st_mtime)) + "（按目录修改时间，旧版拉回没有记录）"  # 非玩家句
                    pulls_fd = winproc.open_dir_nofollow("diagnostics", home_fd) if os.name == "nt" else os.open("diagnostics", flags, dir_fd=home_fd)
                    descriptors.append(pulls_fd)
                    latest_fd = winproc.open_dir_nofollow(latest.name, pulls_fd) if os.name == "nt" else os.open(latest.name, flags, dir_fd=pulls_fd)
                    descriptors.append(latest_fd)

                    def phone_unique(member):   # 脱敏后同名时加序号（同 write_file）
                        stem, suffix = os.path.splitext(member)
                        public, n = member, 1
                        while public in archive.namelist() or public == "manifest.json":
                            n += 1
                            public = f"{stem} ({n}){suffix}"
                        return public

                    def phone_zip(directory_fd, name, member):
                        # D-778G：手机 App「导出诊断」生成的 S2Arm-诊断-*.zip（宿主日志、崩溃报告、诊断 json 都在里面）原先按「别的类型」整份跳过，交上来的包里没有手机现场。
                        #   打开它、按成员收：文本类型走同一套脱敏（成员可能是没脱敏的原文）、PNG 同上限原样收；别的成员与读不了的包记进 skipped（「包名!成员」）。
                        #   成员路径去掉 ..、空段与绝对路径前缀，各段同样脱敏；单个成员按解开后的大小限长读（不信成员头里写的大小）
                        with (winproc.open_file(name, directory_fd) if os.name == "nt"
                              else os.fdopen(os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd), "rb")) as f:
                            try:
                                inner_zip = zipfile.ZipFile(f)
                                entries = inner_zip.infolist()
                            except (zipfile.BadZipFile, OSError, ValueError):
                                phone["skipped"].append(member)
                                return
                            for entry in entries:
                                c.check()
                                if entry.is_dir():
                                    continue
                                parts = [x for x in entry.filename.replace("\\", "/").split("/") if x not in ("", ".", "..")]
                                if not parts:
                                    continue
                                inner = member + "/" + "/".join(parts)
                                public = phone_unique("phone/" + "/".join(redact_member_component(x) for x in inner.split("/")))
                                ext = os.path.splitext(parts[-1])[1].lower()
                                try:
                                    if ext in PHONE_TEXT:
                                        with inner_zip.open(entry) as source:
                                            data = source.read(DIAGNOSTIC_LOG_LIMIT + 1)
                                        if len(data) > DIAGNOSTIC_LOG_LIMIT:
                                            truncated.append(dict(file=public, bytes=entry.file_size, kept=DIAGNOSTIC_LOG_LIMIT, omitted=max(entry.file_size, len(data)) - DIAGNOSTIC_LOG_LIMIT))
                                            data = data[:DIAGNOSTIC_LOG_LIMIT]
                                        text = redact(data.decode(errors="replace")).encode()
                                        if not fits(public, packed(text)):
                                            continue
                                        archive.writestr(public, text)
                                        phone["files"] += 1
                                    elif ext == ".png" and 0 < entry.file_size <= DIAG_RUN_FILE_LIMIT:
                                        with inner_zip.open(entry) as source:
                                            data = source.read(DIAG_RUN_FILE_LIMIT + 1)
                                        if len(data) > DIAG_RUN_FILE_LIMIT or not fits(public, packed(data)):
                                            continue
                                        archive.writestr(public, data)
                                        phone["files"] += 1
                                    else:
                                        phone["skipped"].append(member + "!" + "/".join(parts))
                                except (zipfile.BadZipFile, OSError, ValueError, RuntimeError, EOFError, zlib.error):
                                    phone["skipped"].append(member + "!" + "/".join(parts))

                    def pulled(directory_fd, relative=""):
                        for name in sorted(winproc.listdir(directory_fd) if os.name == "nt" else os.listdir(directory_fd)):
                            c.check()
                            info = winproc.stat_at(name, directory_fd) if os.name == "nt" else os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                            member = relative + name
                            if not relative and name == "pull.json":
                                continue   # 拉回记录进 manifest，不另收
                            if stat.S_ISDIR(info.st_mode):
                                child = winproc.open_dir_nofollow(name, directory_fd) if os.name == "nt" else os.open(name, flags, dir_fd=directory_fd)
                                try:
                                    opened = child.info if os.name == "nt" else os.fstat(child)
                                    if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                                        raise Failure("诊断目录在导出期间发生变化，请重试。")
                                    pulled(child, member + "/")
                                finally:
                                    winproc.close(child) if os.name == "nt" else os.close(child)
                            elif not stat.S_ISREG(info.st_mode):
                                continue
                            elif os.path.splitext(name)[1].lower() in PHONE_TEXT:
                                write_file(directory_fd, name, "phone/" + member)
                                phone["files"] += 1
                            elif name.lower().endswith(".zip") and 0 < info.st_size <= PHONE_ZIP_LIMIT:
                                phone_zip(directory_fd, name, member)
                            elif name.lower().endswith(".png") and 0 < info.st_size <= DIAG_RUN_FILE_LIMIT:
                                with (winproc.open_file(name, directory_fd) if os.name == "nt"
                                      else os.fdopen(os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd), "rb")) as f:
                                    data, png = f.read(), "phone/" + "/".join(redact_member_component(x) for x in member.split("/"))
                                if not fits(png, packed(data)):
                                    continue
                                archive.writestr(png, data)
                                phone["files"] += 1
                            else:
                                phone["skipped"].append(member)
                    pulled(latest_fd)
                step = "读本机崩溃报告"  # 非玩家句
                mac_crash = None
                if sys.platform == "darwin" and MAC_CRASH_DIR.is_dir() and not MAC_CRASH_DIR.is_symlink():
                    cutoff = time.time() - MAC_CRASH_DAYS * 86400
                    found = []
                    for f in MAC_CRASH_DIR.iterdir():
                        if MAC_CRASH_NAME.match(f.name) and not f.is_symlink() and f.is_file():
                            st = f.stat()
                            if st.st_mtime >= cutoff:
                                found.append((st.st_mtime, st.st_size, f))
                    found.sort(key=lambda x: x[0], reverse=True)
                    mac_crash = dict(found=len(found), files=0, skipped=[])
                    for _, size, f in found[:MAC_CRASH_MAX]:
                        c.check()
                        if size > MAC_CRASH_FILE_LIMIT:
                            mac_crash["skipped"].append(f.name)
                            continue
                        data = redact(f.read_text(encoding="utf-8", errors="replace")).encode()
                        if not fits("mac_crash/" + f.name, packed(data)):
                            continue
                        archive.writestr("mac_crash/" + f.name, data)
                        mac_crash["files"] += 1
                # 诊断局（#438，community.diag_run_env）的读数：各款游戏文件夹 diagrun/ 里的 PNG（出图画面与颜色目标，渲染出来的图、不含游戏数据），
                #   原样放 diagrun/<游戏号>_<原名>。单张超过 4 MB 跳过、总量至多 40 MB；链接不跟（不越出游戏文件夹）
                step = "读诊断局读数"  # 非玩家句
                used = 0
                for g in sorted(c.catalog().get("games", []), key=lambda g: g.get("id", "")) if c.library.is_dir() else []:
                    gdir = layout.find_game(c, g.get("id", ""))
                    run = gdir / "diagrun" if gdir else None
                    if run is None or run.is_symlink() or not run.is_dir():
                        continue
                    for f in sorted(run.iterdir()):
                        c.check()
                        if f.is_symlink() or not f.is_file() or f.suffix.lower() != ".png":
                            continue
                        n = f.stat().st_size
                        if not n or n > DIAG_RUN_FILE_LIMIT or used + n > DIAG_RUN_TOTAL_LIMIT:
                            continue
                        data = f.read_bytes()
                        if not fits(f"diagrun/{g['id']}_{f.name}", packed(data)):
                            continue
                        archive.writestr(f"diagrun/{g['id']}_{f.name}", data)
                        used += n
                # 本机管线记录（11-13 维护者：玩家上传的诊断收进随包种子，取件按程序号收进众包 zc_crowd.tsv）：各款文档目录 temp/<号>/wire/<号>/pso_user.tsv
                #   （build.runtime_env 的 S2A_DOCS，宿主 nvn_mtl.m pso_user_path），原样放 pso/<号>.tsv。只有着色器键与管线状态的十六进制，不含个人信息、不过脱敏；
                #   单份超过 4 MB 跳过；链接不跟。Windows 不在本机跑游戏，没有这份就不放
                step = "读本机管线记录"  # 非玩家句
                for g in sorted(c.catalog().get("games", []), key=lambda g: g.get("id", "")) if c.library.is_dir() else []:
                    c.check()
                    gid = (g.get("id") or "").upper()
                    if not re.fullmatch(r"[0-9A-F]{16}", gid):
                        continue
                    # 同目录的局内用到的着色器（shader_seen.tsv，宿主 hle_nvn.c seen_note：只有键与阶段号）放 pso/<号>.seen.tsv，同一份上限
                    for name, rel in (("pso_user.tsv", f"pso/{gid}.tsv"), ("shader_seen.tsv", f"pso/{gid}.seen.tsv")):
                        f = c.library / "temp" / gid / "wire" / gid / name
                        if f.is_symlink() or not f.is_file() or not 0 < f.stat().st_size <= DIAG_PSO_FILE_LIMIT:
                            continue
                        data = f.read_bytes()
                        if fits(rel, packed(data)):
                            archive.writestr(rel, data)
                # 着色器补料（graphics/nvn/host/dsh_salvage.inc）：各款 temp/<号>/wire/<号>/shader_miss/ 里库里没有 / 现编失败的着色器（<键>.ctl / .code + index.tsv），
                #   每款打成一个 tar 放 shader_miss/<号>.tar（诊断包成员数有上限：tools/diagnostic_report.py MAX_FILES 1000，一款就可能上千个文件）；
                #   取件按程序号归档进管线种子目录 rt_shaders/，出包编进那款的直连库。只有游戏着色器字节与键，不含个人信息、不过脱敏（index.tsv 的失败原文是翻译器 rc，不含路径）。
                #   每款至多 DIAG_SALVAGE_GAME_LIMIT（运行时同上限）、整包至多 DIAG_SALVAGE_TOTAL_LIMIT；索引先放，每键两份成对放（放不下整对不放）；链接不跟
                step = "读着色器补料"  # 非玩家句
                salvage_used = 0
                for g in sorted(c.catalog().get("games", []), key=lambda g: g.get("id", "")) if c.library.is_dir() else []:
                    c.check()
                    gid = (g.get("id") or "").upper()
                    if not re.fullmatch(r"[0-9A-F]{16}", gid):
                        continue
                    d = c.library / "temp" / gid / "wire" / gid / "shader_miss"
                    if d.is_symlink() or not d.is_dir():
                        continue
                    picked, game_used = [], 0
                    keys = sorted({f.name[:16] for f in d.iterdir() if re.fullmatch(r"[0-9a-f]{16}\.ctl", f.name)})
                    for names in [("index.tsv",)] + [(f"{k}.ctl", f"{k}.code") for k in keys]:
                        fs = [d / x for x in names]
                        if any(f.is_symlink() or not f.is_file() for f in fs):
                            continue
                        n = sum(f.stat().st_size for f in fs)
                        if not n or game_used + n > DIAG_SALVAGE_GAME_LIMIT or salvage_used + game_used + n > DIAG_SALVAGE_TOTAL_LIMIT:
                            continue
                        picked += fs; game_used += n
                    if not picked:
                        continue
                    buf = io.BytesIO()
                    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
                        for f in picked:
                            data = f.read_bytes(); ti = tarfile.TarInfo(f.name); ti.size = len(data); ti.mtime = 0; ti.mode = 0o644
                            t.addfile(ti, io.BytesIO(data))
                    if not fits(f"shader_miss/{gid}.tar", packed(buf.getvalue())):
                        continue
                    archive.writestr(f"shader_miss/{gid}.tar", buf.getvalue())
                    salvage_used += game_used
                manifest = dict(schema=1,   # 诊断上传服务收件要它（维护方方案「上传协议」：没有 schema 判 not_diag 拒收）；iOS ios/diag.m 同字段
                                launcher_version=launcher_version(),
                                macos_version=platform.mac_ver()[0] or {"error": "macOS 版本不可用"},  # 非玩家句
                                xcode_version=probe(["/usr/bin/xcodebuild", "-version"]),
                                mac_model=probe(["/usr/sbin/sysctl", "-n", "hw.model"]),
                                xcode_signing=signing_status())
                # 这份包是哪款游戏的（同 iOS ios/diag.m 的 game.title_id，diagnostic_report 读它）：只认导出请求带的游戏号——
                #   启动器「故障报修」从玩家当时选中的那款传进来。没带就不写、不猜（猜最近任务会把 A 的问题记到 B 头上，
                #   11-14 诊断#e68047 就是报告级程序号「-」把两款的停帧并成一单），取件按包里每一局自己的程序号分
                gid = c.r.get("gameId")
                if isinstance(gid, str) and re.fullmatch(r"[0-9A-Fa-f]{16}", gid):
                    manifest["game"] = dict(title_id=gid.upper())
                # 降级汇总（v0.3.1）：出包成功收尾存进日志目录的 <任务>-degrade.log（build.collect_degrade，上面已随 logs/ 收进包），按 阶段/类别 计处数
                from build import degrade_rows
                degrade = {}
                for log in sorted(c.logs.glob("*-degrade.log")):
                    c.check()
                    try:
                        rows, _ = degrade_rows(log.read_text(encoding="utf-8", errors="replace"))
                    except OSError:
                        continue
                    for r in rows:
                        degrade.setdefault(log.name[:-len("-degrade.log")], {}).setdefault(r["stage"] + "/" + r["kind"], 0)
                        degrade[log.name[:-len("-degrade.log")]][r["stage"] + "/" + r["kind"]] += r["count"]
                if degrade:
                    manifest["degrade_summary"] = degrade   # 任务号 → {阶段/类别: 处数}
                if mac_crash is not None:
                    manifest["mac_crash_reports"] = mac_crash   # 本机崩溃报告：近 7 天符合的共几份、收了几份、超大跳过的
                if phone is not None:
                    manifest["phone_diagnostics"] = phone   # 最近一次拉回的手机诊断：拉回时刻、来源设备（机型 / 系统）、收了几份、跳过了哪些
                manifest = redact_fields(manifest)
                # 玩家在「故障报修」里写的一句「发生了什么」（选填，11-13 维护者）：原样放进 manifest.json 的 note（iOS ios/diag.m 同字段）；
                #   收件服务去控制字符、截 200 字后存进元数据，取件把它放在定位卡最上面。是玩家自己要交的原话，不过脱敏、不设必填
                note = c.r.get("note")
                if isinstance(note, str) and note.strip():
                    manifest["note"] = note.strip()
                # These names have already been sanitized; redacting them again
                # would hide task UUIDs and break the archive index.
                manifest["files"] = sorted(archive.namelist() + ["manifest.json"])
                if budget_skipped:   # 整包预算放不下跳过的成员（有才写，免得改动现有 manifest 形状）
                    manifest["budget"] = dict(limit=DIAGNOSTIC_ZIP_BUDGET, skipped=budget_skipped)
                manifest["truncated"] = truncated   # 超过单份上限被截的日志 / 任务记录：原长、留下、截掉的字节（空 = 全部整份）
                manifest["changed"] = changed   # 导出期间一直在被替换、没收进来的（#917；空 = 全收到）
                manifest["jobs"] = dict(total=job_total, included=included, limit=DIAG_JOBS, failedLimit=DIAG_FAILED, packGamesLimit=DIAG_PACK_GAMES, omittedLogs=skipped_logs[0])   # 任务记录共几个、收了几个（含没结束的）、省掉的任务日志数
                archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2))
            # 身份在写完、落盘后再取（仍握着句柄，防调包不变）：exFAT / FAT 上新文件写入数据分到存储后 inode 号会变，
            #   建文件时取的身份必然对不上（诊断包存到 exFAT 盘上一律报「临时文件在导出期间发生变化」）
            output.flush(); os.fsync(output.fileno())
            created = os.fstat(output.fileno())
        c.check()
        step = "把诊断包放到所选位置"
        current = temporary.stat(follow_symlinks=False)
        if not stat.S_ISREG(current.st_mode) or (current.st_dev, current.st_ino) != (created.st_dev, created.st_ino):
            raise Failure("诊断临时文件在导出期间发生变化，请重试。")
        os.replace(temporary, destination)
    except OSError as error:
        # 只带步骤、错误码与系统给的原因，不带路径（路径里可能有用户名，诊断摘要会被贴到公开帖子里）
        code = getattr(error, "winerror", None) or error.errno
        reason = (error.strerror or type(error).__name__) + (f"，错误码 {code}" if code else "")  # 非玩家句
        raise Failure(f"无法安全读取或写入诊断文件，未更新诊断包（{step}：{reason}）。") from None
    finally:
        for descriptor in reversed(descriptors):
            winproc.close(descriptor) if os.name == "nt" else os.close(descriptor)
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return dict(outputPath=str(destination))


# ---- 存放位置（游戏库结构 v3）----

def location_rows(c):
    """出包页「存放位置」下拉框：各位置的路径、卷名、剩余空间、连没连上；last = 上次用的位置"""
    rows = []
    for lid, root, ok in layout.locations(c):
        free = shutil.disk_usage(root).free if ok else None
        rows.append(dict(id=lid, path=str(root), volume=_volume(root), connected=ok, freeBytes=free, default=root == c.library,
                         **(location_slack(root) if ok else {})))
    # last 只给还在、连着的位置（#922：换库后 last 还指着已不存在的旧位置，界面默认选中一个空目录）；不然给默认位置
    last = layout.load_index(c).get("last")
    ok = {r["id"] for r in rows if r["connected"]}
    return dict(locations=rows, last=last if last in ok else layout.default_id(c))


def list_locations(c):
    """出包页存放位置下拉框；请求带 probe（刚在「选择其他文件夹…」里选的路径）时另给这个路径的卷格式与提示（#444）"""
    out = location_rows(c)
    if c.r.get("probe"):
        probe = Path(c.r["probe"]).expanduser()
        out["probe"] = dict(path=str(probe), **location_slack(probe))
    return out


def apply_location(c, game):
    """出包前按请求的 location（路径）放这款：已在别的位置就整个搬过去（存档跟着走），还没有就建在那里；记成「上次用的位置」"""
    want = c.r.get("location")
    if not want:
        return
    want = Path(want).expanduser()
    if want == Path(want.anchor) or want == Path.home():
        # 选了整块盘的根 / 用户主目录（预检清单 #62：原先拒绝）：玩家的意思是「放这块盘上」——在里面建一个 S2Arm 文件夹放，不往根上撒文件
        want = want / "S2Arm"
    where, place = layout.state(c, game["id"])
    if where == "offline" and place != want:
        # 原来的位置没连上（盘没插 / 文件夹已删），玩家在出包页另选了位置（#453）：原先照样先查原位置、报「没连上」，玩家的选择不起作用。
        #   改在新位置重新建：解包、产物重新做；原位置上的文件（含 Mac 存档）不动，盘插回来可用「重新找到」认回
        c.diagnostic(f"这款原来的存放位置没连上（{place}），这次改在新选的位置重新准备；原位置上的文件（包括 Mac 上的存档）没有动。",
                     code="job_failed", severity="warning")
        layout.forget(c, game["id"])
        where = "none"
    if where == "ok" and place.parent.parent != want:
        c.begin("把这款游戏搬到新的存放位置")
        layout.move_game(c, game["id"], want)
    elif where != "ok":
        layout.game_dir(c, game["id"], location=want)
    layout.use_location(c, want)


def move_game(c):
    """搬这一款到别的存放位置。锁（#2191 出包时放开别的款）：执行锁共享 + 这一款的按款锁 + 清单锁——只动这一款的文件夹和它的索引条目
    （索引写回按条目合并，layout.save_index），别的款出包 / 起局照常；原先拿执行锁独占，别的款出包时搬任何一款都等满报忙"""
    import sgrade
    with store_meta_quiet(c), c.lock("execution", shared=True), c.lock(sgrade.game_lock(c.r.get("gameId") or "")), c.lock():
        game = c.game()
        if not c.r.get("location"):
            raise Failure("没有选新的存放位置。", code="job_failed")
        apply_location(c, game)
        sizes.refresh(c, game["id"])   # 跨卷拷过去克隆会展开成实占，重量
        return dict(gameId=game["id"], folder=str(layout.find_game(c, game["id"])))


def relink_game(c):
    with c.lock():
        game = c.game()
        return dict(gameId=game["id"], folder=str(layout.relink(c, game["id"], Path(c.r.get("folder") or ""))))


def forget_game(c):
    """「从游戏库移除」（找不到 / 不要了）：清单和索引里去掉，磁盘上的文件不动"""
    with c.lock():
        gid = c.game()["id"]
        layout.forget(c, gid)
        cat = c.catalog()
        cat["games"] = [g for g in cat["games"] if g["id"] != gid]
        atomic(c.library / "catalog.json", cat)
    return {}
