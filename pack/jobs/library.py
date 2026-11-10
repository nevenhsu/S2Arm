"""Managed game catalogue, immutable container imports and player data.

Storage/interaction helpers adapted from LumaDock under MIT.
"""
from __future__ import annotations
import collections
import contextlib
from copy import deepcopy
import errno
import filelock
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile
from errors import explain
from core import Failure, atomic, contained, digest, prune_jobs, read
import space_rule
import toolchain
import winproc
import filelock
import layout
import sizes

DIAGNOSTIC_LOG_EDGE = 16000          # 长日志保留的开头、结尾字节数
DIAGNOSTIC_LOG_BEATS = 64000         # 中间心跳行最多保留这么多字节（10 秒一行，约一小时）
DIAGNOSTIC_LOG_SCAN = 64 << 20       # 只在结尾前这么大的范围里找心跳行
DIAGNOSTIC_LOG_CHUNK = 1 << 20
DIAGNOSTIC_LOG_SCENE = 96000         # 中间最后一次崩溃摘要 + 全线程现场块最多保留这么多字节（超了按优先级裁，见 _scene_text）


class _Scene:
    """扫中段时收最后一次「崩溃摘要 + 全线程现场」（runtime/src/crash.c 打【红】CRASH 摘要行，随后 tdump.c 每行带「[现场] 」前缀，
    「===== 全线程现场：…」起、「===== 现场完 =====」止）。#141：只留头尾时崩溃在中段就整段丢了，玩家交了诊断包也定不了因"""
    CRASH, TAG = "CRASH 信号".encode(), "[现场]".encode()
    START, END = "===== 全线程现场".encode(), "现场完".encode()

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


def _scene_text(summary, block, budget=DIAGNOSTIC_LOG_SCENE):
    """整段放得下就整段；放不下按优先级：崩溃摘要 → 现场块头 → 第一个非等待线程的整段栈 → 其余线程的头一行 → 现场完"""
    text = b"\n".join(summary + block)
    if len(text) <= budget:
        return text
    keep = list(summary[:24]) + block[:1]
    threads, cur = [], None
    for line in block[1:]:
        if "[现场] 线程 ".encode() in line: cur = [line]; threads.append(cur)
        elif cur is not None and _Scene.END not in line: cur.append(line)
    first = next((t for t in threads if "等待".encode() not in t[0]), threads[0] if threads else [])
    keep += first
    used = sum(len(x) + 1 for x in keep)
    for t in threads:
        if t is first: continue
        if used + len(t[0]) + 1 > budget - 200: keep.append(f"[现场] …（还有线程略去：现场块 {len(text)} 字节超出保留上限）".encode()); break
        keep.append(t[0]); used += len(t[0]) + 1
    keep += [x for x in block[-1:] if _Scene.END in x]
    return b"\n".join(keep)


def diagnostic_log_excerpt(source, size):
    """诊断包里的日志：短的整份；长的留开头（起局配置）+ 中间的心跳行（卡顿、内存走势）+ 中间最后一次崩溃摘要与全线程现场 + 结尾。总量有界。"""
    edge = DIAGNOSTIC_LOG_EDGE
    if size <= 2 * edge:
        return source.read(2 * edge)
    head = source.read(edge)
    start = max(edge, size - edge - DIAGNOSTIC_LOG_SCAN)
    source.seek(start)
    beats, used, partial, left = collections.deque(), 0, b"", size - edge - start
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
            if "[心跳]".encode() in line and len(line) < 4096:
                beats.append(line)
                used += len(line) + 1
                while used > DIAGNOSTIC_LOG_BEATS:
                    used -= len(beats.popleft()) + 1
    source.seek(size - edge)
    tail = source.read(edge)
    gap = lambda text: f"\n…（{text}）…\n".encode()
    if partial and len(partial) < 4096: scene.feed(partial)   # 扫描区最后一行没有换行（紧挨着结尾段）
    mid = b""
    if scene.last:
        mid = gap("中间最后一次崩溃摘要与全线程现场") + _scene_text(*scene.last)
    return head + gap(f"日志共 {size} 字节，中间只保留 {len(beats)} 行心跳") + b"\n".join(beats) + mid + gap("以下是日志结尾") + tail


DIAGNOSTIC_JOB_LIMIT = 4 << 20


# 游戏原始包：安装包 NSP 与卡带镜像 XCI，及它们的压缩版 NSZ / XCZ（nx_unpack 按内容认格式、还原压缩的 NCA）；
#   Switch 2 的 NUP 容器（.nup，一个文件装本体 + 更新 + DLC，不加密、不要 prod.keys）由 pack/ns2_nup.py 认与展开，不走 nx_unpack；
#   已解包的游戏目录（Switch 2 导出 dump / S2Arm 解包 extract / 通用解包 loose）也可直接导入，不要密钥
SOURCE_SUFFIXES = (".nsp", ".xci", ".nsz", ".xcz", ".nup")
NUP_SUFFIX = ".nup"


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


STORE_RETRY = 7 * 86400   # 查过商店（断网 / 404 / 没有横图）的款，7 天内不再查
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
        if ids is None and now - float(g.get("storeChecked") or 0) < STORE_RETRY:
            continue
        todo.append(gid)
    return todo


def store_meta_background(c, ids=None):
    """在后台查商店（pack/jobs/store_meta_bg.py：store_meta 然后 heal_folder_names）：作业本身不等联网（商品页单页要几秒）。
    有要查的款才起；起不来（系统拒绝）就算了，下个作业再试。子进程脱离本作业：作业结束、取消都不带走它（Windows 前端不把作业放进杀子进程的作业对象）。
    S2A_BANNER_FETCH=0 不起"""
    if os.environ.get("S2A_BANNER_FETCH", "1") == "0" or not store_todo(c, ids):
        return False
    import subprocess
    args = [sys.executable, "-I", "-B", str(Path(__file__).with_name("store_meta_bg.py")), str(c.private), str(c.library),
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
    断网、商品页 404、图不合格都静默跳过，只记 storeChecked；S2A_BANNER_FETCH=0 关掉。不含 ASCII 字母数字的标题不记（文件夹名用英文就是为了避开非 ASCII 路径）。
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
            got[gid] = (banner if ok and banner.is_file() else None, fetch.store_name(gid))
        if not got:
            return 0
        try:
            with c.lock():
                cat = c.catalog()
                for g in cat["games"]:
                    gid = str(g.get("id", "")).upper()
                    if gid not in got:
                        continue
                    banner, name = got[gid]
                    g["storeChecked"] = int(now)
                    if name and layout._ascii_name(name) and not g.get("englishName"):
                        g["englishName"] = name.strip()
                    if banner:
                        d = layout.find_game(c, gid) or layout.game_dir(c, gid, english=g.get("englishName") or g.get("name") or "")
                        if not (d / "banner.jpg").is_file():
                            os.replace(banner, d / "banner.jpg")
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
                raise ValueError("unpack.tsv 没有 title_id")
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
# 游戏库 = S2Arm 的工作数据（清单、解包与出包产物、缓存、存档），要放 APFS：播种、发布要原子的「不覆盖改名」和克隆，exFAT / FAT 不支持。
#   启动器选到非 APFS 的文件夹不拦：说明后游戏库放默认位置（内置盘），那块盘上的游戏照样原地导入；后端 setup 这里是最后一道（直接调接口的也拦）
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


def require_apfs(path):
    """游戏库所在卷的格式：Mac 要 APFS（原错误码 library_not_apfs），Windows 要 NTFS / ReFS（library_bad_format，libloc.format_ok）；读不到格式不拦"""
    import libloc
    fmt = volume_format(path)
    if os.name == "nt":
        if not libloc.format_ok(fmt, system="Windows"):
            raise Failure(libloc.WIN_FORMAT_MESSAGE.format(fmt=fmt.upper()), code="library_bad_format")
        return fmt
    if fmt and fmt != "apfs":
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


MOVE_SKIP = {".DS_Store", ".catalog.lock", ".execution.lock", ".s2arm-moving"}   # 锁文件要留到搬完；.DS_Store 系统会再生成


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


def copy_tree_links(src, dst):
    """跨盘拷一棵目录，链接拷链接本身、不钻进去：软链照拷；Windows 的 junction（Python 里不算软链，shutil.copytree 会钻进去展开成普通目录，
    产物根的「现行」随之失效、出包报「现行必须是目录链接」——#214 整库搬家与 v3 移动游戏同一处）重建成指向同一目标的 junction。
    目标仍指旧位置；放到新位置后由 _retarget_links 改指"""
    src, dst = Path(src), Path(dst)
    isjunction = getattr(os.path, "isjunction", lambda _: False)
    def copy(a, b):
        if isjunction(a):
            subprocess.run(["cmd", "/c", "mklink", "/J", str(b), os.readlink(a).removeprefix("\\\\?\\")], check=True, capture_output=True)
        elif a.is_symlink():
            os.symlink(os.readlink(a), b, target_is_directory=a.is_dir())
        elif a.is_dir():
            b.mkdir()
            for child in a.iterdir():
                copy(child, b / child.name)
            shutil.copystat(a, b)
        else:
            shutil.copy2(a, b)
    copy(src, dst)


def _retarget_links(root, old, new):
    """目录搬走后，指向旧位置的绝对链接（Windows 的 junction）改指新位置"""
    if not root.is_dir():
        return
    from_text, to_text = os.path.normcase(str(old)), str(new)
    for d, dirs, files in os.walk(root):
        for name in dirs + files:
            p = Path(d) / name
            if not _is_link(p):
                continue
            target = os.readlink(p).removeprefix("\\\\?\\")
            if not os.path.isabs(target) or not os.path.normcase(target).startswith(from_text):
                continue
            fresh = to_text + target[len(from_text):]
            junction = getattr(os.path, "isjunction", lambda _: False)(p)
            _unlink_link(p)
            if junction:
                subprocess.run(["cmd", "/c", "mklink", "/J", str(p), fresh], check=True, capture_output=True)
            else:
                os.symlink(fresh, p)


def _unlink_link(p):
    if getattr(os.path, "isjunction", lambda _: False)(p):
        os.rmdir(p)
    else:
        p.unlink()


def _work_root_of(c, library):
    return c.private if library.is_relative_to(c.private) else library / ".s2arm-work"


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
def _source_locked(source):
    """搬家期间旧游戏库不能有任务在跑"""
    handles = []
    try:
        for name in (".execution.lock", ".catalog.lock"):
            f = (source / name).open("a")
            handles.append(f)
            try:
                filelock.try_lock(f)
            except BlockingIOError:
                raise Failure("旧游戏库还有任务在跑，等它结束再搬。", code="library_move_failed") from None
        yield
    finally:
        for f in handles:
            f.close()


def move_library(c, source):
    """把现有游戏库搬到 c.library（空文件夹）：同盘改名、跨盘拷完校验再换，任何一步失败都回到搬之前。旧位置的内容在新位置就位之后才删"""
    source = Path(source).resolve()
    info = read(source / ".s2arm-library.json", None)
    if not isinstance(info, dict) or not info.get("id"):
        raise Failure("原来的位置不是可搬的游戏库。", code="library_move_failed")
    if c.library == source or c.library.is_relative_to(source) or source.is_relative_to(c.library):
        raise Failure("新位置不能在原来的游戏库里面，也不能包含它。", code="library_move_failed")
    with _source_locked(source):
        pairs = _move_pairs(c, source)
        c.library.mkdir(parents=True, exist_ok=True)
        cross = [(a, b) for a, b in pairs if not _same_volume(a, c.library)]
        need = sum(_size_of(a) for a, _ in cross)
        if need and shutil.disk_usage(c.library).free < need:
            raise Failure("新位置空间不够，游戏库没有搬。", code="disk_full")
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
                    copy_tree_links(a, target)
                else:
                    shutil.copy2(a, target, follow_symlinks=False)
                _verify_copy(a, target, strict=a.name in ("saves", "catalog.json", ".s2arm-library.json"))
                copied += _size_of(a)
                c.progress(copied, total)
            c.begin("把游戏库放到新位置")
            staged = {a: stage / str(n) for n, (a, _) in enumerate(cross)}
            for a, b in pairs:
                b.parent.mkdir(parents=True, exist_ok=True)
                os.rename(staged.get(a, a), b)
                done.append((a, b))
            for root in (c.library, _work_root_of(c, c.library)):
                _retarget_links(root, source, c.library)
        except BaseException:
            if done:
                _retarget_links(c.library, c.library, source)
            for a, b in reversed(done):
                if a in staged:
                    _discard(b)
                else:
                    os.rename(b, a)
            _discard(stage)
            with contextlib.suppress(OSError):
                (c.library / ".s2arm-work").rmdir()   # 回滚后别留下搬的时候建的空目录
            raise
        _discard(stage)
        for a, _ in cross:
            try:
                _discard(a)
            except OSError as error:
                c.diagnostic("游戏库已经搬好，旧位置有文件没删掉，可以手动删除：" + c.redact(str(error)), code="job_failed", severity="warning")
    for name in (".execution.lock", ".catalog.lock", ".DS_Store"):
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
        shutil.rmtree(p)


def setup(c):
    """选游戏库：已有游戏库直接打开；装着旧产物的文件夹认领下来；空文件夹按 moveFrom 把现有游戏库搬过来，没给就新建"""
    c.begin("创建游戏库")
    if c.library == Path(c.library.anchor) or c.library == Path.home() or c.library == c.engine:
        raise Failure("请选择独立的游戏库文件夹。")
    require_apfs(c.library)
    require_local(c.library)
    c.library.mkdir(parents=True, exist_ok=True)
    marker = c.library / ".s2arm-library.json"
    if marker.exists():
        info = read(marker)
        if info.get("schema") != 1 or not info.get("id"):
            raise Failure("游戏库标识不完整。")
        return dict(library=str(c.library), libraryId=info["id"], outcome="opened")
    if any(p.name != ".DS_Store" for p in c.library.iterdir()):
        if not _looks_like_library(c.library):
            raise Failure("这个文件夹里已有别的文件：请选空文件夹，或者已有的 S2Arm 游戏库。文件都没动。")
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
                              if p["type"] == "base" and p.get("format") not in ("nup", "dump", "extract", "loose") and source_path(c, p).is_file()), None)   # NUP 与解包目录不加密，验不了密钥
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
    finally:
        temporary.unlink(missing_ok=True)
    atomic(c.private / "keys/status.json", dict(ready=True, verified=verified, imported=time.time()))
    return dict(entries=count, verified=verified, message="已验证解密能力" if verified else "格式通过，首次读取游戏时验证解密能力")


# 候选游戏文件自身有毛病（截断、缺票据）：密钥验不了，但不能据此判密钥无效；导入照常、记 verified=false，源文件错误留到读那款游戏时报
KEYS_UNVERIFIABLE = ("content_truncated", "ticket_missing")
KEYS_CANNOT_DECRYPT = ("无法读取游戏内容：可能是文件不完整、缺票据或密钥不匹配。请核对原始文件；"
                       "若已确认 Switch 密钥版本不足，再重新导出，到「设置 → 玩家密钥」重新导入。")


def inspect(c, source, icon_dir, keyfile=None, *, source_id=None):
    keyfile = keyfile or c.keys
    validate_keys(keyfile)
    lines = []
    log_name = f"{c.task}-{source_id}-inspect.log" if source_id else c.task + "-inspect.log"
    try:
        c.run(["dotnet", toolchain.reader(c), keyfile, "--inspect", source, icon_dir],
              c.logs / log_name, on_line=lines.append)
    except Failure as error:
        if error.code not in ("tool_exit", "job_failed"):
            raise
        if any(line.startswith("【原因 disk_full】") for line in lines):
            raise Failure("读取游戏内容时磁盘空间不足。", code="disk_full") from None
        if "【内容截断】" in lines:
            raise Failure("游戏文件不完整，部分内容超出文件末尾。", code="content_truncated") from None
        if any(line.startswith("【原因 ticket_missing】") for line in lines):
            raise Failure("缺少内容对应的票据或标题密钥，请检查源文件是否完整。", code="ticket_missing") from None
        if any(line.startswith("【原因 keys_missing】") for line in lines):
            raise Failure("现有 prod.keys 缺少读取此文件所需的密钥。", code="keys_outdated") from None
        if any("could not be decrypted" in line or "Check keys" in line for line in lines):
            raise Failure(KEYS_CANNOT_DECRYPT, code="content_unreadable") from None
        raise
    if "【内容截断】" in lines:
        raise Failure("游戏文件不完整，部分内容超出文件末尾。", code="content_truncated")
    try:
        output = json.loads(next(line for line in reversed(lines) if line.startswith("{")))
        if output.get("schema") != 1 or not output["packages"]:
            raise ValueError()
        for package in output["packages"]:
            if not re.fullmatch(r"[0-9A-F]{16}", package["titleId"]) or not re.fullmatch(r"[0-9A-F]{16}", package["baseId"]):
                raise ValueError()
            if package["type"] not in ("base", "update", "dlc") or not isinstance(package["version"], int):
                raise ValueError()
        return output["packages"]
    except (KeyError, ValueError, StopIteration, TypeError):
        raise Failure("内容读取器返回的元数据不完整。", code="content_invalid") from None


def selected_packages(game):
    bases = [p for p in game["packages"] if p["type"] == "base"]
    updates = [p for p in game["packages"] if p["type"] == "update"]
    result = ([max(bases, key=lambda p: p["version"])] if bases else [])
    if updates:
        result.append(max(updates, key=lambda p: p["version"]))
    dlc = {}
    for p in game["packages"]:
        if p["type"] == "dlc" and (p["titleId"] not in dlc or dlc[p["titleId"]]["version"] < p["version"]):
            dlc[p["titleId"]] = p
    return result + [dlc[k] for k in sorted(dlc)]


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


# 双轨包多占的空间（10-24 动森实测，RomFS 10.7 GB）：直连库中间缓存（S2A_DIRECT_CACHE，跨次复用）+ 包里的直连库，按游戏体积换算
# 出包要多少盘（11-08 维护者）：不看这款做没做过，第一次出包也算得准，只有一个数。
#   解包 = 原包 × 1.1（解包目录已完整在盘上时算 0）；Mac 的资源包是 APFS 克隆不另算，Windows（NTFS 无克隆）再加一份原包 × 1.1；
#   直连（翻译缓存 + 着色器库 + 收尾临时，取上限）：原包 < 3 GB 记 5 GB，其余 15 GB（最坏样本巫师3 缓存 7.5 + 库 1.5 × 2 份 + 收尾临时约 4）
PACK_SMALL_BYTES, DIRECT_SMALL_BYTES, DIRECT_LARGE_BYTES = int(3e9), int(5e9), int(15e9)


def _pack_input_bytes(packages):
    # NSZ/XCZ按解包内容与原文件取大值；同一sha的原件只计一次。
    return sum(max(p.get("size", 0), p.get("contentBytes", 0))
               for p in {p["sha256"]: p for p in packages}.values())


def _pack_direct_bytes(size):
    return DIRECT_SMALL_BYTES if size < PACK_SMALL_BYTES else DIRECT_LARGE_BYTES


def pack_need_bytes(packages, unpacked=False, windows=None, extract_bytes=None):
    """这次出包要多少盘；packages 已按 selected_packages 选定。unpacked：解包目录已完整在盘上。windows 不给就按当前系统。
    extract_bytes：着色器抽取量已知（产物根 extract/ 在盘上）时直连按 space_rule 估（#265：固定 15 GB 对大游戏不够、对小游戏偏多），不知道才用经验值"""
    size = _pack_input_bytes(packages)
    copy = size * 11 // 10
    windows = os.name == "nt" if windows is None else windows
    direct = sum(space_rule.direct_need(extract_bytes)) if extract_bytes else _pack_direct_bytes(size)
    return (0 if unpacked else copy) + (copy if windows else 0) + copy + direct   # 再加一份 copy：单文件镜像 romfs.img 是真实占盘的一份（pack/romfs_image.py）


def _extract_bytes(c, gid):
    """这款现行产物根里的着色器抽取量（Mac 打包 cache/<款>/<键>/roots、打 iPhone 包 iPhone/<款>/roots；取大的）；还没抽过 → None"""
    dirs = [*(c.library / "iPhone" / gid).glob("roots/*@现行/extract"), *(c.library / "cache" / gid).glob("*/roots/*@现行/extract")]
    sizes = [space_rule.tree_bytes(d) for d in dirs if d.is_dir()]
    return max(sizes) if sizes and max(sizes) > 0 else None


# 出包实测账（11-07 联合开发者旷野之息：外接盘 NX 剩 45 GB，经验门槛要 4x GB 拦下）：每次 Mac 打包 / 打 iPhone 包成功后，
#   量这一款在游戏库所在卷上的整套占用，追加一行到 游戏库/space-usage.tsv（取最后一行，同 tools/disk_admit.py 的 est.tsv）；
#   预检先用同款实测、再用同大小档实测比例 × 本款大小，都没有才退回经验规则。余量 × 1.1 同 维护方工具 need_bytes
SPACE_LEDGER = "space-usage.tsv"
CLONED_DIRS = ("romfs", ".romfs_store")
SPACE_LEDGER_FIELDS = ("time", "gameId", "target", "direct", "inputBytes", "footprintBytes", "name", "format")
SPACE_LEDGER_FORMAT = "2"   # 2 起 RomFS 只算一份、直连缓存是压缩后的（DSZ1）；旧行偏大约 5 倍（火焰纹章记 152 GB，实占约 32 GB），不用


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


def disk_bytes(roots, device, skip_names=(), skip_under=()):
    """这些目录在 device 卷上实际分配的字节：按 (卷, inode) 去重（硬链接只算一次；多个根互相包含也只算一次），
    不跟软链（原件软链、@现行 指向）、不跨到别的卷（私有目录在内置盘时不计入外接盘游戏库的账）。
    APFS 克隆（#303）：同一克隆族（_CloneInfo）的共享块只算一次——每份记独占字节，族的共享部分取最大一份加一次；
    取不到克隆信息的文件照 st_blocks 算。skip_names / skip_under 只用于按部分分账（sizes.py：解包还在时游戏资源目录的克隆归 cache）"""
    seen, total, stack = set(), 0, [os.fspath(r) for r in roots]
    shared = {}   # 克隆族号 → 族里共享块字节（取各份最大）
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
            shared[clone[1]] = max(shared.get(clone[1], 0), alloc - clone[0])
        else:
            total += alloc
        if stat.S_ISDIR(st.st_mode):
            try:
                with os.scandir(path) as it:
                    stack.extend(e.path for e in it)
            except OSError:
                pass
    return total + sum(shared.values())


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
        size = _pack_input_bytes(selected_packages(game))
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


def _unpacked_on_disk(c, game):
    """这款的解包目录已完整在盘上（物理事实，不是历史记录）"""
    from build import extract_valid   # build 顶层 import 本模块，这里函数内取
    return "id" in game and extract_valid(c, game)


def space_budget(c, game):
    """打包前的空间预算（纯计算，不拦）：只有一个数，出包页显示和拦截都用它；公式见 pack_need_bytes。space-usage.tsv 只记账校准常量，不进判断。只写盘名，不写绝对路径。"""
    import shutil as _sh
    packages = selected_packages(game)
    size = _pack_input_bytes(packages)
    unpacked = _unpacked_on_disk(c, game)
    extract = _extract_bytes(c, game["id"]) if "id" in game else None
    need, free = pack_need_bytes(packages, unpacked=unpacked, extract_bytes=extract), _sh.disk_usage(c.library).free
    direct = f"直连着色器 {_gb(sum(space_rule.direct_need(extract)))}（按抽取 {_gb(extract)} 估）" if extract else f"直连着色器上限 {_gb(_pack_direct_bytes(size))}"
    note = f"解包 {'已在盘上' if unpacked else _gb(size * 11 // 10)}，{direct}"
    volumes = [dict(role="library", volume=_volume(c.library), needBytes=need, freeBytes=free, enough=free >= need,
                    note=note, unknown=["shader_cache"], basis="rule")]
    return dict(gameBytes=size, volumes=volumes, unknown=[dict(u) for u in SPACE_UNKNOWN])


def check_pack_space(c, game, *, compiler_policy=None):
    """打包前的磁盘预检（Mac 打包与打 iPhone 包共用）：IPA 可先按已有保留规则尝试回收；发最终 space 事件（存任务文件），再按预算拦。
    请求带 spaceOverride: true（玩家在失败页点「仍然继续」）：不拦，只记一条警告；真写满由出包中途的磁盘写满识别报错"""
    budget = space_budget(c, game)
    cache_result = None
    if compiler_policy is not None:
        from cache_retention import trim
        # 缓存只释放工具缓存所在卷（默认位置的 tool-cache，游戏库结构 P3）；不同卷的 library 不借用这笔 free。
        device = c.work_root.parent.stat().st_dev
        relevant = budget["volumes"] if c.library.stat().st_dev == device else []
        target = max([compiler_policy["min_free_bytes"], *(v["needBytes"] for v in relevant)])
        if any(not v["enough"] for v in budget["volumes"]):
            # 若不足的是另一卷，只统计缓存，不为它删除本卷冷条目。
            floor = target if any(not v["enough"] for v in relevant) else 0
            cache_result = trim(c.work_root, current_gid=game["id"],
                                **dict(compiler_policy, min_free_bytes=floor))
            budget = space_budget(c, game)
            budget["compilerCache"] = cache_result
        budget["compilerMinFreeBytes"] = target
    override = getattr(c, "r", {}).get("spaceOverride") is True
    if override and any(not v["enough"] for v in budget["volumes"]):
        budget["override"] = True   # 失败页据此不再给「仍然继续」（这次已经是继续过的）
    c.emit("space", "空间预检", space=budget)
    for v in budget["volumes"]:
        if v["enough"]:
            continue
        need, free = v["needBytes"], v["freeBytes"]
        head = f"磁盘空间不够：这次出包预计要 {need / 1e9:.0f} GB，现在只剩 {free / 1e9:.0f} GB。"
        if override:
            c.diagnostic(head + "已按你的选择仍然继续；中途真写满会停下报错，游戏原件和存档不受影响。", code="disk_full", severity="warning")
            continue
        raise Failure(head + "清出空间后点「重试」，或点「仍然继续」（预计偏保守时可以试；真写满会停下报错，游戏原件和存档不受影响）。", code="disk_full")

    return budget


def guest_isa(selected):
    # 真正跑的程序的指令集：最新更新的 ExeFS 是完整的，读得出就以它为准；读不出（unknown）才看本体。
    # 有的游戏本体 32 位、更新后 64 位（马车8 1.0 AArch32 → 4.0.1 AArch64，10-23 一键出包曾按本体误拒）
    upd = [p for p in selected if p["type"] == "update" and p.get("guestIsa") in ("aarch64", "aarch32")]
    if upd:
        return upd[0]["guestIsa"]
    return next((p.get("guestIsa") for p in selected if p["type"] == "base"), "unknown")


def requirements(c, game):
    return [str(error) for error in requirement_errors(c, game)]


def require_game(c, game):
    errors = requirement_errors(c, game)
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
        c.emit(message=f"在等系统的文件访问授权：{what}（已等 {int(time.monotonic() - t0)} 秒）。"
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
        raise Failure(f"系统不让 S2Arm 往游戏库所在的磁盘写（{path}）：到「系统设置 › 隐私与安全性 › 文件与文件夹」"
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
                raise Failure(f"系统不让 S2Arm 读 {path.name}（{path.parent}）：到「系统设置 › 隐私与安全性 › 文件与文件夹」"
                              "或「完全磁盘访问权限」里打开 S2Arm，再点一次", code="input_permission")
            raise Failure(f"读不了游戏文件 {path.name}：{e}", code="input_unreadable")


def requirement_errors(c, game):
    missing = []
    selected = selected_packages(game)
    if not any(p["type"] == "base" for p in selected):
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
                fileName=path.name, needsKeys=needs_keys, status="unprocessed", code=None, reason="", log="", diagnostic="")


# 写进结果与任务记录的诊断上限（字符）：单个来源 16k，整批合计 64k（catalog() 每次回传最近 200 条任务记录，不能随来源个数膨胀）。
# 单个超出时保留开头与结尾、中间标注已省略；整批用完后，后面的来源只留原因和按来源命名的日志文件名。
IMPORT_DIAGNOSTIC_LIMIT = 16_000
IMPORT_DIAGNOSTIC_TOTAL = 64_000


def cap_diagnostic(text, limit=IMPORT_DIAGNOSTIC_LIMIT):
    if len(text) <= limit:
        return text
    marker = "\n…[已省略中间部分]…\n"
    keep = max(0, limit - len(marker))
    head = keep // 4
    return text[:head] + marker + text[len(text) - (keep - head):]


def import_source_failed(c, row, error):
    failure = explain(error)
    if c.r.get("partialResults") is True and isinstance(error, OSError) and error.errno in (errno.EIO, errno.ENODEV):
        failure = Failure("读不了游戏文件。", code="input_unreadable", internal=str(error))
    detail = [str(failure), failure.internal]
    log = c.last_log.name if c.last_log else ""
    if c.last_log:
        try:
            detail.append(c.last_log.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    reason = c.redact(failure)
    left = c.import_diagnostic_left
    if left > 0:
        diagnostic = cap_diagnostic(c.redact("\n".join(filter(None, detail)).replace(row["path"], row["fileName"]), limit=None),
                                    min(IMPORT_DIAGNOSTIC_LIMIT, left))
        c.import_diagnostic_left = left - len(diagnostic)
    else:
        diagnostic = reason + (f"\n（本批诊断已达上限，完整日志：{log}）" if log else "\n（本批诊断已达上限）")
    row.update(status="failed", code=failure.code, reason=reason, log=log, diagnostic=diagnostic)


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


def record_import_packages(c, cat, source, packages, icons, sha, size, extracted):
    results = []
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
            same = [p for p in game["packages"] if _same_package(p, metadata, sha, size)]
        for p in same:
            # A managed copy still in the library keeps its path.
            old = source_path(c, p)
            if Path(p["path"]).is_absolute() or not (old.is_dir() if extracted else old.is_file()):
                p["path"] = str(source)
        if not same:
            icon = Path(icons) / (metadata["titleId"] + ".jpg")
            english_name = metadata.pop("englishName", "") or ""
            if english_name and "englishName" not in game:
                game["englishName"] = english_name
            if icon.is_file():
                try:
                    _copy_cover(c, tid, game, metadata, icon)
                except OSError as error:
                    # 默认路径照原样抛，交给 explain() 归类（permission_denied / input_missing 等）
                    if c.r.get("partialResults") is not True or explain(error).code == "disk_full":
                        raise
                    raise Failure("游戏库封面无法写入。", internal=str(error)) from error
            metadata.pop("icon", None)
            package_size = metadata.get("contentBytes", 0) if extracted else size
            game["packages"].append(metadata | dict(path=str(source), sha256=package_sha, size=package_size))
            if metadata["name"]:
                game["name"] = metadata["name"]
        results.append(dict(id=tid, name=source.name, duplicate=bool(same)))
    return results


def initial_import_result(c):
    c.import_diagnostic_left = IMPORT_DIAGNOSTIC_TOTAL
    paths = dict.fromkeys(Path(os.path.abspath(Path(raw).expanduser())) for raw in c.r["paths"])
    return dict(imports=[], sources=[import_source(path, needs_keys=path.suffix.lower() in SOURCE_SUFFIXES[:-1])
                                    for path in paths])


def import_failure_result(c):
    """失败 / 取消的终态事件与任务记录带的逐来源结果：只在调用方开了 partialResults 时给，默认路径与原来一样不带"""
    return getattr(c, "import_result", None) if c.r.get("partialResults") is True else None


def import_games(c):
    partial = c.r.get("partialResults") is True
    c.import_result = initial_import_result(c)
    picked = [Path(row["path"]) for row in c.import_result["sources"]]
    # 来源行按调用方给的顺序；选的文件夹在原位置展开成里面的来源（按路径排序）。行里的路径保留调用方给的原样，写进 catalog 的统一用 resolve 后的路径
    groups = {path: [row] for path, row in zip(picked, c.import_result["sources"])}
    rows = {}   # resolve 后的路径 → 来源行
    results = c.import_result["imports"]

    def publish():
        c.import_result["sources"] = [row for group in groups.values() for row in group]

    c.begin("检查导入文件")
    c.require_library()
    if not partial:
        # 外接盘 / 下载文件夹第一次读会弹系统授权框：先试读，框没点之前界面要说清在等什么，别一直停在「检查导入文件」
        wait_readable(c, [path for path in picked if path.exists()])
    files, extracts = [], []
    for path in picked:
        c.check()
        try:
            if partial and path.exists():
                wait_readable(c, [path])
            real = path.resolve()
            found_files, found_extracts, errors = scan_import(c, real, partial=partial)

            def shown(found):
                try:
                    return path / found.relative_to(real) if found != real else path
                except ValueError:
                    return found

            found = dict.fromkeys(found_files, True) | dict.fromkeys(found_extracts, False)
            group = []
            for source in sorted(found, key=shown):
                if source not in rows:   # 同一份被选了两次（文件和它所在的文件夹）只列一行
                    rows[source] = import_source(shown(source), needs_keys=found[source] and source.suffix.lower() != NUP_SUFFIX)
                    group.append(rows[source])
            for bad_path in sorted(errors, key=shown):
                row = import_source(shown(bad_path))
                import_source_failed(c, row, errors[bad_path])
                group.append(row)
            groups[path] = group
            files.extend(found_files)
            extracts.extend(found_extracts)
        except (Failure, OSError) as error:
            # Legacy callers skip empty folders alongside usable sources.
            if not partial and explain(error).code == "input_empty":
                groups[path] = []
                continue
            import_source_failed(c, groups[path][0], error)
            if not partial or groups[path][0]["code"] not in IMPORT_SOURCE_ERRORS:
                raise
        finally:
            publish()
    files = sorted(set(files))
    extracts = sorted(set(extracts))
    paths = files + extracts
    if not c.import_result["sources"]:
        raise Failure("所选位置没有 NSP、XCI 文件或解包好的游戏文件夹。", code="input_empty")
    # prod.keys 只有 Switch 1 原件（NSP / XCI / NSZ / XCZ，要读取器解密）才要；只放 Switch 2 NUP 容器或解包目录不要密钥
    switch1 = [f for f in files if rows[f]["needsKeys"]]
    if switch1 and not c.keys.is_file():
        raise Failure("请先导入 prod.keys。", code="keys_missing")
    with c.lock():
        cat = c.catalog()
        verified = False
        extract_paths = set(extracts)
        total = len(paths) * 2
        for idx, source in enumerate(paths):
            if partial:
                c.last_log = None
                c.last_rc = None
                c.require_library()
            c.check()
            row = rows[source]
            extracted = source in extract_paths
            draft = deepcopy(cat)
            diagnostics_before = len(c.job["diagnostics"])
            try:
                c.begin("读取游戏信息", idx * 2 + 1, total)
                with tempfile.TemporaryDirectory(prefix="icons-", dir=c.private) as icons:
                    if extracted:
                        packages = extract_inspect(c, source, icons)
                    elif row["needsKeys"]:
                        packages = inspect(c, source, icons, source_id=row["id"] if partial else None)
                    else:
                        packages = nup_inspect(c, source, icons)
                    row["name"] = next((p["name"] for p in packages if p.get("name")), source.name)
                    c.begin("校验游戏文件", idx * 2 + 2, total)
                    # 读完元数据后文件不见了 / 读不了归在这一步（input_missing）
                    size = None if extracted else source.stat().st_size
                    sha = "" if extracted else source_identity(packages, size)
                    if sha is None:   # 读取器没给 content ID：照旧整份算
                        sha = digest(source, c.progress)
                    entries = record_import_packages(c, draft, source, packages, icons, sha, size, extracted)
                    c.check()
                    if partial:
                        c.require_library()
                    try:
                        atomic(c.library / "catalog.json", draft)
                    except OSError as error:
                        # 默认路径照原样抛，交给 explain() 归类
                        if not partial or explain(error).code == "disk_full":
                            raise
                        raise Failure("游戏库目录无法写入。", internal=str(error)) from error
            except (Failure, OSError) as error:
                if not partial:
                    raise
                import_source_failed(c, row, error)
                if row["code"] not in IMPORT_SOURCE_ERRORS:
                    raise
                del c.job["diagnostics"][diagnostics_before:]
            else:
                cat = draft
                results.extend(entries)
                row["status"] = "duplicate" if all(entry["duplicate"] for entry in entries) else "imported"
                # 逐来源模式：读通一个 Switch 1 原件就算验证过（后面的来源失败不影响）
                if partial and row["needsKeys"] and not verified:
                    atomic(c.private / "keys/status.json", dict(ready=True, verified=True))
                    verified = True
    added = {r["id"].upper() for r in results if not r["duplicate"]}
    if added:
        store_meta_background(c, added)   # 横图、英文标题：后台查（导入不等联网），查到英文名接着改文件夹名
    if not partial:
        if switch1:   # 读取器用这份密钥读通了 Switch 1 原件才算验证过；只导 NUP 或解包目录没用到密钥，不改密钥状态
            atomic(c.private / "keys/status.json", dict(ready=True, verified=True))
        return {"imports": results}
    return c.import_result


JOB_LIST_LIMIT = 200


def _mtime(path):
    try:
        return path.stat().st_mtime
    except OSError:
        return 0


CUSTOM_NAME_LIMIT = 64
CUSTOM_COVER_LIMIT = 16 << 20
_CUSTOM_COVER = re.compile(r"cover\.custom\.[0-9a-f]{8}\.jpg")


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
    sweep_residue(c)
    cat = c.catalog()
    input_supported, input_reason = toolchain.input_interface(c)
    for game in cat["games"]:
        game["missing"] = requirements(c, game)
        game["requirementDiagnostics"] = [c.describe_failure(error) for error in requirement_errors(c, game)]
        game["compatibility"] = "unsupported" if guest_isa(selected_packages(game)) == "aarch32" else "experimental"
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
        game["iphoneRestorable"] = layout.phone_ipa(c, previous=True).is_file()   # 换壳后可装回上一版（install.restore_previous）
        game["iphoneInstall"] = layout.install_record(c, game["id"])
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
        cover = layout.path(c, game["id"], "cover.jpg", create=False)
        custom = game_custom(c, game["id"])
        game["customName"], game["customCover"] = bool(custom["name"]), custom["cover"] is not None
        game["name"] = custom["name"] or game["name"]
        game["cover"] = str(custom["cover"] or cover) if custom["cover"] or cover.is_file() else ""
        choices = [p for p in selected_packages(game) if p["type"] in ("base", "update")]
        if choices:
            game["display"] = max(choices, key=lambda p: p["version"])["display"]
        game["sizes"] = sizes.for_catalog(c, game["id"], cat["games"])   # 每款各部分大小（sizes.py：作业收尾量好记在 game.json，这里不量）
    try:
        validate_keys(c.keys)
        status = read(c.private / "keys/status.json", {}) | dict(ready=True)
    except (Failure, OSError, UnicodeError):
        status = dict(ready=False, verified=False)
    status["message"] = "已验证解密能力" if status.get("verified") else "格式通过，首次读取游戏时验证解密能力" if status["ready"] else "尚未导入 prod.keys"
    # 任务目录里还有各任务留存的请求（<任务>.request.json，J10 resume 用）：不是任务记录，没有 id / state；混进来启动器整个任务列表解码失败、一条都读不回
    # 任务目录有上限（保留规则见 core.prune_jobs）：catalog 每次启动都跑，在这里顺手清，一天最多一次
    #   （清理要把每条记录读一遍判结束没有），先清再读。只回最新 JOB_LIST_LIMIT 条：旧启动器的任务列表只看近期；
    #   11-07 实测任务目录 1.5 万条记录时整份读 + 回传 7 MB 占 catalog 数秒。
    marker = c.logs / ".jobs-pruned"
    if time.time() - _mtime(marker) > 86400:
        prune_jobs(c.private, jobs=c.jobpath.parent)
        marker.touch()
    records = sorted((p for p in c.jobpath.parent.glob("*.json") if p != c.jobpath and not p.name.endswith(".request.json")),
                     key=_mtime, reverse=True)[:JOB_LIST_LIMIT]
    jobs = [r for r in (read(p) for p in records) if isinstance(r, dict)]
    return dict(games=cat["games"], profiles=[], keys=status, jobs=jobs,
                engineInfo={"inputConfig": input_supported, "inputConfigReason": input_reason})


# 这一款在私有目录里按程序号存的出包缓存（直连 / 翻译 / 追加内容 / 原件核对 / 宽屏；都在 S2A_RUN_ROOT = <私有>/run 下，pack/jobs/core.py Context.env）
# 显式清理/删除含共享编库缓存；Q15 成功后的自动清理仅清本款中间产物，保留可复用编库条目。
def game_caches(c, gid, *, include_compiler=True):
    run = c.work_root / "run" / "s2arm"
    low = gid.lower()
    direct = run / "cache" / "direct" / gid.upper()
    out = [direct] if include_compiler else direct_transients(c, direct)
    for d, pat in ((run / "cache" / "bake", f"{low}-*"), (run / "cache" / "aoc", f"t{low}-*"), (run / "cache" / "srcver", f"t{low}-*"),
                   (run / "widen", f"t{low}_*"), (run / "widen" / "auto", f"t{low}_*")):
        out += sorted(d.glob(pat)) if d.is_dir() else []
    if include_compiler:
        out += [c.work_root / "cache" / "air", c.work_root / "cache" / "pso", c.work_root / "run" / "gal_dl_cache"]
    return [p for p in out if p.exists() or p.is_symlink()]


def direct_transients(c, direct):
    """Only disposable direct-build work; never remove the store (store/, logs/) or unknown layouts.
    S2Arm 11-01: the direct cache is one content-addressed store, one file per entry (C# dsh_batch build, store/dshb/); the former per-key trees
    ios/ and mac/ (b-<chain>/, cas/…) and work/ are the old layout, which direct_lib.sh deletes on its next build anyway"""
    from cache_retention import _safe_root
    if _safe_root(c.work_root, direct) != 'safe':
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
    if path.parent != direct or _safe_root(c.work_root, direct) != 'safe':
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


def _rmtree(p):
    """删整棵目录；遇到只读目录（产物根的 RomFS 共享存储 roots/.romfs_store/ 整棵只读，pack/roots.sh）先给它加回写权限再删。
    Windows 上封存不是只读位，是 ACL 里给当前用户的拒写项（pack/orch/roots.py seal → _win_acl），chmod 解不开：
    按 roots 同一个函数把出错的项和它的父目录解封再删（11-07 Windows 出包机：启动器出包成功后清中间产物卡在 .romfs_store，WinError 5）"""
    def unlock(func, path, _):
        parent = os.path.dirname(path)
        if os.name == "nt":
            engine = str(Path(__file__).resolve().parents[2])
            if engine not in sys.path:
                sys.path.insert(0, engine)
            from pack.orch import roots
            for q in (parent, path):
                if os.path.lexists(q) and not os.path.islink(q):
                    roots._win_acl(q, False)
        else:
            os.chmod(parent, stat.S_IMODE(os.lstat(parent).st_mode) | stat.S_IRWXU)
        func(path)
    if sys.version_info >= (3, 12):
        shutil.rmtree(p, onexc=unlock)
    else:
        shutil.rmtree(p, onerror=unlock)


def _rm(c, root, p):
    # Check the entry before resolving it: a link into saves/ is not disposable.
    root, p = Path(os.path.abspath(root)), Path(os.path.abspath(p))
    try:
        relative = p.relative_to(root)
    except ValueError:
        raise Failure("操作路径不属于托管目录。") from None
    if not relative.parts:
        raise Failure("操作路径不属于托管目录。")
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
    """产物根只留现行（*@现行 及其指向）与 RomFS 共享存储，清旧版本 → 清了几项。
    RomFS 共享存储：现行指向其中的身份，删了现行就断；没人指向的旧身份由下次提交清（pack/roots.sh root_romfs_sweep）"""
    links = list(d.glob("*@现行"))
    current = {q.name for q in links} | {q.resolve().name for q in links if _is_link(q)}
    n = 0
    for q in d.iterdir():
        if q.name not in current and q.name != ".romfs_store":
            _rm_entry(c, q); n += 1
    return n


def _windows_layout():   # 单测可打桩：Windows 的产物根在 work/ 下（ipa.current_root）
    return os.name == "nt"


CONTAINER_KIT = layout.IOS_KIT   # 每款 image/ios/kit：装机组装容器要用的文件，出包成功后从产物根存一份独立副本（游戏库结构 v3；旧布局是 iPhone/<号>/container-kit）


def save_container_kit(c, gid, root, dest=None):
    """把组装容器要读的文件（ship_ext.place_game / display_name / 随包表：顶层普通文件、ctl/control.nacp、programs/<n>/）存成不依赖产物根的副本。
    root 里的链接按内容拷，之后删产物根、RomFS 存储、Windows 的 work/ 都不影响装机、续签和单壳追加。没有 main.npdm 返回 None，不动旧的。
    dest 给了（迁移旧布局时）就存成 dest/container-kit，否则存到这款的 image/ios/kit。"""
    root = Path(root)
    if not (root / "main.npdm").is_file():
        return None
    out = Path(dest) if dest else layout.image_ios(c, gid)
    out.mkdir(parents=True, exist_ok=True)
    name = "container-kit" if dest else CONTAINER_KIT
    kit, temporary = out / name, out / (name + ".partial")
    managed = c.library if dest else layout.root_of(c, gid)
    if temporary.exists():
        _rm(c, managed, temporary)
    def copy_files(source, dest):
        dest.mkdir(parents=True, exist_ok=True)
        for p in source.iterdir():
            if p.is_file() and not p.name.startswith(".") and p.suffix != ".log":
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
    if kit.exists():
        _rm(c, managed, kit)
    os.replace(temporary, kit)
    return kit


@contextlib.contextmanager
def _execution_idle(c):
    """拿得到出包执行锁才进去（有任务在跑时 yield False）。不走 c.lock：那里先查取消，被取消的任务也要能收尾。"""
    with (c.library / ".execution.lock").open("a") as f:
        try:
            filelock.try_lock(f)
        except BlockingIOError:
            yield False
            return
        try:
            yield True
        finally:
            filelock.unlock(f)


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
    return (layout.ios_kit(c, gid) / "main.npdm").is_file() or not (layout.ios_pack(c, gid) / "manifest.json").is_file()


def _keep_stage_logs(c, gid):
    """产物根里的阶段日志要跟着删：先拷进私有日志目录，失败后的诊断包还能带上（只拷文件，单份 8 MB 以内，最多 64 份）。"""
    work = layout.ios_work(c, gid, create=False)
    roots = [*work.glob("roots/*@现行"), *work.glob("work/pkg/product"), *layout.build_dir(c, gid, create=False).glob("*/roots/*@现行"),
             # 阶段失败时各阶段把自己的日志拷进产物根旁的 .failed/（如改写 pack/stage_rewrite.sh 的 <款>_<模块>.log）：玩家报出包超时 / 改写失败时要的就是它（#251 诊断包里没有）
             *work.glob("roots/.failed"), *layout.build_dir(c, gid, create=False).glob("*/roots/.failed")]
    dest = c.logs / (c.task + "-stages")
    count = 0
    for root in roots:
        for log in sorted(root.glob("*.log")):
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
        with _execution_idle(c) as idle:
            if idle and _residue_disposable(c, gid):
                _keep_stage_logs(c, gid)
                prune_build(c, gid, record=False)
    except (Failure, OSError, ValueError) as error:
        c.diagnostic("清理中间产物没有完成，下次空闲时会再清：" + c.redact(str(error)), code="job_failed", severity="warning")


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


def sweep_residue(c):
    """启动器空闲时扫掉崩溃、断电、进程被杀留下的解包和中间产物；有出包在跑时不动。
    先不拿锁扫一遍，真有残留才拿执行锁、锁内再扫一遍再删：catalog 每次读游戏库都调它，扫描期间占着执行锁的话，
    紧接着开始的任务（起局、存档、出包）拿不到锁当场报「另一个任务正在使用游戏库」（11-09 main 守望红：SaveManagementTests 备份完刷新游戏库、查看存档撞锁）"""
    try:
        if not _residue(c):
            return
        with _execution_idle(c) as idle:
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
    direct = c.work_root / 'run/s2arm/cache/direct' / gid.upper()
    for p in game_caches(c, gid, include_compiler=not retain_compiler):
        if retain_compiler and p.parent == direct:
            n += _rm_direct_transient(c, direct, p)
        else:
            _rm(c, c.work_root, p); n += 1
    return n


def drop_from_container(c, gid):
    """游戏库里删了的款从各台手机的记录（devices/<设备>.json）摘掉，不装机：下次给那台装机时现组的容器就不再带它（#227）"""
    gid = gid.upper()
    for key, rec in layout.devices(c):
        if gid in (rec.get("games") or {}):
            rec["games"].pop(gid)
            layout.save_device(c, key, rec)


def delete(c):
    """删除这款游戏（连出包产物与缓存）：iPhone 输出整个目录（IPA / 资源包 / 中间产物）、Mac 构建缓存、game_caches，并从游戏库清单里删掉；
    保留存档（library/saves/<程序号>）与游戏原件（原地导入的不碰；旧版复制进 library/sources 的托管副本另有「删除托管 NSP 副本」）。
    执行锁可能还被这一款刚被取消的任务占着（启动器发删除前先结束它在跑的任务，进程退出要几秒）：拿不到就每 2 秒再试，最多 120 秒"""
    t0 = time.time()
    while True:
        try:
            with c.lock("execution"), c.lock():
                gid = c.game()["id"]
                c.begin("删除游戏与出包产物")
                gdir = layout.find_game(c, gid)
                if gdir and gdir.exists():
                    # 存档有内容就保留
                    saves = gdir / "saves"
                    backups = gdir / "save-backups"
                    has_saves = (saves.exists() and any(saves.iterdir())) or (backups.exists() and any(backups.iterdir()))
                    if has_saves:
                        for item in gdir.iterdir():
                            if item.name not in ("saves", "save-backups"):
                                _rm(c, layout.root_of(c, gid), item)
                    else:
                        _rm(c, layout.root_of(c, gid), gdir)
                for p in game_caches(c, gid):
                    _rm(c, c.work_root, p)
                drop_from_container(c, gid)
                cat = c.catalog()
                cat["games"] = [g for g in cat["games"] if g["id"] != gid]
                atomic(c.library / "catalog.json", cat)
            return {}
        except Failure as e:
            if "另一个任务正在使用游戏库" not in str(e) or time.time() - t0 > 120:
                raise
            c.begin("等这款游戏在跑的任务结束"); time.sleep(2)


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
    ("ipa", "已出好的 iPhone 包", "删了要重新出包才能再装到 iPhone；已装好的游戏不受影响"),
)
SPACE_KEPT = ("saves", "logs")


def iphone_ready(c, gid):
    """这款的 iPhone 版能不能装（catalog 每款都问，只看几个文件，不逐文件读）：容器件主程序表 main.npdm 在且非空、有改写产物，资源包清单在。
    审计 P2-212：原先只看「有 ipa + 有资源包目录」，容器件缺 main.npdm 也报就绪，点装机才报错；逐文件完整核对仍在装机组装时（pack/ship_ext.py check_root / place_game）"""
    kit, pack = layout.ios_kit(c, gid), layout.ios_pack(c, gid)
    try:
        return (kit / "main.npdm").stat().st_size > 0 and any(kit.glob("*.fixups.tsv")) and (pack / "manifest.json").is_file()
    except OSError:
        return False


def space_parts(c, gid):
    """{部件 id: [路径…]}，外加不给删的 saves / logs。路径都在游戏库或私有目录之内；不存在的不列"""
    out = layout.ios_work(c, gid, create=False)
    shared = {c.work_root / "cache" / "air", c.work_root / "cache" / "pso", c.work_root / "run" / "gal_dl_cache"}   # 各款共用的编库缓存：不归任何一款
    ipa = [layout.image_ios(c, gid, create=False)]
    gdir = layout.find_game(c, gid)
    parts = {
        "legacy": _build_entries(c, gid),   # 不含出包日志（下面 logs 项，只报不删）
        "shaders": [p for p in game_caches(c, gid) if p not in shared],
        "mac": [layout.image_mac(c, gid, create=False)] if gdir and gdir.exists() else [],
        "ipa": ipa,
        "saves": [layout.path(c, gid, "saves", create=False)],
        "logs": [out / "logs"],
    }
    return {k: [p for p in v if p.exists() or p.is_symlink()] for k, v in parts.items()}


def _part_bytes(paths):
    by_dev = {}
    for p in paths:
        try:
            by_dev.setdefault(p.lstat().st_dev, []).append(p)
        except OSError:
            pass
    return sum(disk_bytes(v, dev) for dev, v in by_dev.items())


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
        rows.append(dict(gameId=g["id"], name=game_custom(c, g["id"])["name"] or g.get("name") or g["id"],
                         parts=[dict(id=k, title=t, rebuild=r, bytes=_part_bytes(parts[k])) for k, t, r in SPACE_PARTS if k != "legacy" or parts[k]],   # 没有旧版残留就不显示
                         kept=[dict(id=k, bytes=_part_bytes(parts[k])) for k in SPACE_KEPT]))
    return dict(games=rows)


def space_free(c):
    """删这款勾选的部件（request.parts = 部件 id 列表，只认 SPACE_PARTS 里的）。有任务在跑拒绝；返回各部件删前字节与释放合计"""
    want = c.r.get("parts")
    ids = {k for k, _, _ in SPACE_PARTS}
    if not isinstance(want, list) or not want or any(p not in ids for p in want):
        raise Failure("要删除的部件不对：只能选" + "、".join(t for _, t, _ in SPACE_PARTS) + "。")
    with c.lock("execution"), c.lock():
        gid = c.game()["id"]
        c.begin("释放空间")
        if "legacy" in want:   # 老玩家的现行产物根是装机的唯一来源：删之前先存成容器件，否则装机会报 container_stale
            from ipa import product_root
            root = product_root(c, gid)
            if root is not None and not (layout.ios_kit(c, gid) / "main.npdm").is_file():
                save_container_kit(c, gid, root)
        parts = space_parts(c, gid)
        freed, done = 0, []
        for k in [k for k, _, _ in SPACE_PARTS if k in want]:
            before = _part_bytes(parts[k])
            for p in parts[k]:
                root = c.work_root if p.is_relative_to(c.work_root) else layout.root_of(c, gid)
                _rm(c, root, p)
            after = _part_bytes([p for p in parts[k] if p.exists() or p.is_symlink()])
            freed += before - after
            done.append(dict(id=k, bytes=before - after))
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


def save_input(c):
    config = validate_input(c.r["config"])
    if c.r.get("gameId"):
        c.game()
        target = layout.path(c, c.r["gameId"], "input.json")
    else:
        target = c.private / "default-input.json"
    atomic(target, config)
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
                return {"error": f"命令失败（退出码 {result.returncode}）"}
            return result.stdout.strip() or {"error": "命令输出为空"}
        except subprocess.TimeoutExpired:
            return {"error": "命令超过 10 秒"}
        except OSError:
            return {"error": "命令无法执行"}

    def launcher_version():
        # Installed Jobs live below Contents/Resources; source runs have no
        # launcher identity and must say so rather than invent a version.
        contents = next((p for p in Path(__file__).resolve().parents if p.name == "Contents"), None)
        if contents is None:
            return {"error": "源码运行，没有启动器 Info.plist"}
        try:
            info = plistlib.loads((contents / "Info.plist").read_bytes())
            value = info.get("CFBundleShortVersionString")
            return value if isinstance(value, str) and value.strip() else {"error": "启动器版本字段缺失"}
        except (OSError, ValueError, plistlib.InvalidFileException):
            return {"error": "启动器 Info.plist 无法读取"}

    destination = Path(c.r["path"]).expanduser().resolve()
    if c.private == destination or c.private in destination.parents or c.library == destination or c.library in destination.parents:
        raise Failure("诊断包请选择应用数据和游戏库以外的位置。")
    descriptors = []
    temporary = None
    try:
        flags = 0 if os.name == "nt" else os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_NONBLOCK
        # 日志与任务记录在 core.logs_root（默认位置的 logs/，任务记录在 logs/jobs/；库没建好退回私有目录）：逐级不跟链接打开
        home_fd = winproc.open_dir_nofollow(c.logs.parent) if os.name == "nt" else os.open(c.logs.parent, flags)
        descriptors.append(home_fd)
        roots = {}
        # Context 建任务时已写下本任务记录，logs/ 与 logs/jobs/ 都在；不是真目录（链接）照样拒
        roots["logs"] = winproc.open_dir_nofollow("logs", home_fd) if os.name == "nt" else os.open("logs", flags, dir_fd=home_fd)
        descriptors.append(roots["logs"])
        roots["jobs"] = winproc.open_dir_nofollow("jobs", roots["logs"]) if os.name == "nt" else os.open("jobs", flags, dir_fd=roots["logs"])
        descriptors.append(roots["jobs"])
        c.begin("导出脱敏诊断")

        fd, name = tempfile.mkstemp(prefix="." + destination.name + ".", suffix=".partial", dir=destination.parent)
        temporary = Path(name)
        with os.fdopen(fd, "w+b") as output:
            created = os.fstat(output.fileno())
            with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
                def write_file(parent_fd, name, member, job=False):
                    c.check()
                    info = winproc.stat_at(name, parent_fd) if os.name == "nt" else os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
                    if not stat.S_ISREG(info.st_mode):
                        return
                    if os.name == "nt":
                        source_file = winproc.open_file(name, parent_fd)
                    else:
                        file_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent_fd)
                        source_file = os.fdopen(file_fd, "rb")
                    with source_file as source:
                        opened = winproc.file_info(source) if os.name == "nt" else os.fstat(source.fileno())
                        if not stat.S_ISREG(opened.st_mode):
                            return
                        if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                            raise Failure("诊断文件在导出期间发生变化，请重试。")
                        if job:
                            limit = DIAGNOSTIC_JOB_LIMIT
                            if opened.st_size > limit:
                                raise Failure("诊断任务文件超过 4 MiB 上限，未生成诊断包。")
                            data = source.read(limit + 1)
                            if len(data) > limit:
                                raise Failure("诊断任务文件超过 4 MiB 上限，未生成诊断包。")
                        else:
                            data = diagnostic_log_excerpt(source, opened.st_size)
                    stem, suffix = os.path.splitext(member)
                    # Archive separators are known structure, not email local-part characters.
                    public_member = "/".join(redact_member_component(part) for part in stem.split("/")) + suffix
                    if public_member in archive.namelist() or public_member == "manifest.json":
                        raise Failure("脱敏后的诊断文件名重复，未生成诊断包。")
                    # These allowlisted job/log files are text. Invalid UTF-8 is
                    # replaced, never copied as raw bytes around redaction.
                    archive.writestr(public_member, redact(data.decode(errors="replace")))

                if roots["jobs"] is not None:
                    for name in sorted(winproc.listdir(roots["jobs"]) if os.name == "nt" else os.listdir(roots["jobs"])):
                        if name.endswith(".json"):
                            write_file(roots["jobs"], name, "jobs/" + name[:-5] + ".txt", job=True)

                def logs(directory_fd, relative=""):
                    for name in sorted(winproc.listdir(directory_fd) if os.name == "nt" else os.listdir(directory_fd)):
                        c.check()
                        info = winproc.stat_at(name, directory_fd) if os.name == "nt" else os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                        member = relative + name
                        if not relative and name == "jobs":
                            continue   # 任务记录上面已按 jobs/ 收过
                        if stat.S_ISDIR(info.st_mode):
                            child = winproc.open_dir_nofollow(name, directory_fd) if os.name == "nt" else os.open(name, flags, dir_fd=directory_fd)
                            try:
                                opened = child.info if os.name == "nt" else os.fstat(child)
                                if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                                    raise Failure("诊断目录在导出期间发生变化，请重试。")
                                logs(child, member + "/")
                            finally:
                                winproc.close(child) if os.name == "nt" else os.close(child)
                        elif name.endswith(".log"):
                            write_file(directory_fd, name, "logs/" + member)
                if roots["logs"] is not None:
                    logs(roots["logs"])
                manifest = dict(launcher_version=launcher_version(),
                                macos_version=platform.mac_ver()[0] or {"error": "macOS 版本不可用"},
                                xcode_version=probe(["/usr/bin/xcodebuild", "-version"]),
                                mac_model=probe(["/usr/sbin/sysctl", "-n", "hw.model"]),
                                xcode_signing=signing_status())
                manifest = redact_fields(manifest)
                # These names have already been sanitized; redacting them again
                # would hide task UUIDs and break the archive index.
                manifest["files"] = sorted(archive.namelist() + ["manifest.json"])
                archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2))
        c.check()
        current = temporary.stat(follow_symlinks=False)
        if not stat.S_ISREG(current.st_mode) or (current.st_dev, current.st_ino) != (created.st_dev, created.st_ino):
            raise Failure("诊断临时文件在导出期间发生变化，请重试。")
        os.replace(temporary, destination)
    except OSError:
        raise Failure("无法安全读取或写入诊断文件，未更新诊断包。") from None
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
        rows.append(dict(id=lid, path=str(root), volume=_volume(root), connected=ok, freeBytes=free, default=root == c.library))
    return dict(locations=rows, last=layout.load_index(c).get("last") or layout.default_id(c))


def list_locations(c):
    return location_rows(c)


def apply_location(c, game):
    """出包前按请求的 location（路径）放这款：已在别的位置就整个搬过去（存档跟着走），还没有就建在那里；记成「上次用的位置」"""
    want = c.r.get("location")
    if not want:
        return
    want = Path(want).expanduser()
    if want == Path(want.anchor) or want == Path.home():
        raise Failure("请选一个单独的文件夹作存放位置。", code="job_failed")
    where, place = layout.state(c, game["id"])
    if where == "ok" and place.parent.parent != want:
        c.begin("把这款游戏搬到新的存放位置")
        layout.move_game(c, game["id"], want)
    elif where != "ok":
        layout.game_dir(c, game["id"], location=want)
    layout.use_location(c, want)


def move_game(c):
    with c.lock("execution"), c.lock():
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
