"""Local job protocol and storage used by the Mac launcher and the CLI.

UI/storage helpers are adapted from LumaDock (MIT; launchers/macos/README.md).
Engine operations always invoke S2Arm's own tools.
"""
from __future__ import annotations

import contextlib
import datetime
import errno
import filelock
import winproc
import overall as overall_progress
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import threading
import time
import uuid
from progress_activity import BatchActivity
from errors import Failure, device_of, estimate_low, explain, failure_summary, last_output_lines, recovery, library_lock_failure


def default_developer_dir():
    """请求没带 developerDir 时用哪套 Xcode：装着的 Xcode 里最新的（toolchain.detect_xcode，含 xcode-select 指的那个；与启动器缺省、tools/toolchain.sh 同一条规则）。
    11-07 Duo 定因：原先取 xcode-select（本机 26.5），命令行 / 脚本发的请求不带 developerDir 就用 26.5 SDK 出壳，iOS 27 新机型上整个 App 缩小居中。
    找不到 → None（不设 DEVELOPER_DIR，由工具链检查说清「没有 Xcode」）"""
    from toolchain import detect_xcode
    p = detect_xcode()
    return p if p and os.path.isdir(p) else None


def shared_redact():
    # App 里 build_app.py 把 tools/redact.py 拷进 Resources/Jobs；源码树里从仓库 tools/ 取
    try:
        import redact
    except ImportError:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import redact
    return redact


# 直连段的空间复核（11-10）：出包预检与 space_rule.py 按原件大小记直连段上限 15 GB，着色器多的大游戏远不止——
#   0400A3A01EE4C000 预检 22.3 GB，直连缓存在「翻译与收尾」（99084 个着色器）期间涨到约 44 GB、编批库开始后反而涨得慢，把系统盘写满。
#   每个着色器 / 每 MB 批库占多少盘因游戏差很多（同一口径下另一款 8734 个着色器整局峰值约 1 GB），不能用常数：
#   按这一局自己的实际消耗外推——计数器开始时记下（已完成量、这一款出包的真实占用 library.pack_footprint：克隆共享块只算一次），
#   之后每 10 秒按「占用增量 ÷ 已完成 × 剩余 × 1.1」估还要多少，按卷分开（存放位置、工具缓存可以是两块盘，P1-165），大于那块盘当前可用就停下说清楚。只量自己的目录、不用卷余量差：
#   同卷别的作业写盘不算进本局消耗（11-10 第四次采样卷余量被别的作业带偏、作废）。完成量到总量 5%、增量超过 256 MB 才判。
#   玩家点过「仍然继续」只打一次黄。
#   先前按编批库总量 × 3 估（790efd9ec）：3 是拿 du 推的，du 把 APFS 克隆的解包内容重复算，且占盘大头在翻译阶段、编批库前就已发生。
#   模块级函数、按 getattr 读上下文：stage_progress 也被只带进度字段的替身对象调用（tests/progress_test.sh 启动器读端）
DIRECT_SPACE_COUNTERS = ("直连库·翻译与收尾", "直连库·编批库（MB）")  # 非玩家句
DIRECT_SPACE_EVERY_S = 10
DIRECT_SPACE_MIN_FRACTION = 0.05
DIRECT_SPACE_MIN_SPENT = 256 << 20


def job_footprint(context):
    """这一款出包按卷的真实占用 {卷: (取余量的路径, 字节, 角色)}：存放位置那块盘量 build/ image/，工具缓存那块盘量本款直连 / 翻译缓存
    （P1-165：两块盘可以不同；APFS 克隆共享块只算一次）"""
    import library   # library 导入 core：用到时再取
    return {v["device"]: (v["path"], library.disk_bytes(v["roots"], v["device"]), v["role"])
            for v in library.pack_volumes(context, context.r.get("gameId"))}


def direct_space_recheck(context, counters, now=None):
    library = getattr(context, "library", None)
    if getattr(context, "operation", None) not in ("build", "ipa") or library is None:
        return
    now = time.monotonic() if now is None else now
    marks = getattr(context, "_direct_space_marks", None)
    if marks is None:
        marks = context._direct_space_marks = {}
    for name in DIRECT_SPACE_COUNTERS:
        c = counters.get(name)
        if c is None or c["done"] >= c["total"]:
            continue
        m = marks.get(name)
        if m is not None and (m["total"] != c["total"] or c["done"] < m["done"]):
            m = None   # 同名计数器重新开始
        if m is not None and now - m["at"] < DIRECT_SPACE_EVERY_S:
            continue
        try:
            used = job_footprint(context)
        except OSError:
            return
        if m is None:
            marks[name] = dict(total=c["total"], done=c["done"], used={dev: u[1] for dev, u in used.items()}, at=now)
            continue
        m["at"] = now
        done = c["done"] - m["done"]
        if done < c["total"] * DIRECT_SPACE_MIN_FRACTION:
            continue
        short = None
        for dev, (path, bytes_now, role) in used.items():   # 每块盘只算写在它上面的、和它自己的余量比（P1-165）
            spent = bytes_now - m["used"].get(dev, bytes_now)
            if spent < DIRECT_SPACE_MIN_SPENT:
                continue
            need = int(spent / done * (c["total"] - c["done"]) * 1.1)
            try:
                free = shutil.disk_usage(path).free
            except OSError:
                continue
            if free < need:
                short = (role, need, free, spent)
                break
        if short is None:
            continue
        role, need, free, spent = short
        where = "游戏存放位置所在的盘" if role == "library" else "工具缓存所在的盘"
        gb = lambda n: f"{n / 1e9:.0f} GB"
        head = (f"磁盘空间不够：编译着色器这一步（{name.split('·')[-1]}）照现在的进度还要再占约 {gb(need)}，"  # 非玩家句
                f"{where}只剩 {gb(free)}（已完成 {c['done']} / {c['total']}，这段已用掉 {gb(spent)}）。")
        # 按进度外推只当提示、不拦（不许堵门，11-14 定）：每单只提示一次，照常编；中途真写满时写盘报 ENOSPC，那才停、归 disk_full
        if not getattr(context, "_direct_space_warned", False):
            context._direct_space_warned = True
            context.diagnostic(head + "这是按进度估的，照常继续；中途真写满会停下报错，游戏原件和存档不受影响。", "disk_full", severity="warning")  # 非玩家句
        return


class Cancelled(Exception):
    pass


class Paused(Exception):
    """作业停在一个要玩家处理的点上（批量出包磁盘满，剃刀 H13）：以独立终态 paused 结束，不算完成也不算失败。result 随终态带回"""
    def __init__(self, message, result=None):
        super().__init__(message)
        self.result = result or {}


def system_encoding():
    """真实的系统编码（中文 Windows cp936、西文 Windows cp1252）。不用 getpreferredencoding：主作业带 -X utf8，UTF-8 模式下它回 utf-8，
    v0.2.7 因此跳过系统编码、把 GBK 写坏的索引当 cp1252 写成了乱码（中文 Windows 实机复现）。getencoding（3.11+）不受 UTF-8 模式影响"""
    import locale
    get = getattr(locale, "getencoding", None)
    return get() if get else locale.getpreferredencoding(False)


def _norm(enc):
    return enc.replace("-", "").replace("_", "").lower()


def _undo_misheal(path, value):
    """v0.2.7 写成乱码的索引改回来：<名>.cp1252.bak 在、真实系统编码不是 cp1252、备份按系统编码严格解得开 = 当时该按系统编码解却按 cp1252 解了。
    同一份备份分别按 cp1252（当时的乱码）与系统编码（原文）解，逐串配对，现有内容里的乱码串按表改回（之后库里新加的不动）；
    备份改名为 <名>.<系统编码>.bak，下次不再改"""
    bak = path.with_name(path.name + ".cp1252.bak")
    if not bak.exists():
        return value
    enc = system_encoding()
    if _norm(enc) in ("cp1252", "utf8", "windows1252"):
        return value
    raw = bak.read_bytes()
    try:
        good, bad = json.loads(raw.decode(enc)), json.loads(raw.decode("cp1252"))
    except (UnicodeDecodeError, ValueError, LookupError):
        return value
    pairs = {}
    def pair(b, g):
        if isinstance(b, str) and isinstance(g, str):
            if b != g:
                pairs[b] = g
        elif isinstance(b, list) and isinstance(g, list) and len(b) == len(g):
            for x, y in zip(b, g):
                pair(x, y)
        elif isinstance(b, dict) and isinstance(g, dict) and len(b) == len(g):
            for (kb, vb), (kg, vg) in zip(b.items(), g.items()):
                pair(kb, kg); pair(vb, vg)
    pair(bad, good)
    def fix(v):
        if isinstance(v, str):
            return pairs.get(v, v)
        if isinstance(v, list):
            return [fix(x) for x in v]
        if isinstance(v, dict):
            return {fix(k): fix(x) for k, x in v.items()}
        return v
    fixed = fix(value)
    if fixed != value:
        atomic(path, fixed)
    keep = path.with_name(f"{path.name}.{enc}.bak")
    if keep.exists():
        bak.unlink()
    else:
        os.replace(bak, keep)
    return fixed


def read(path, default=None):
    """读共享 JSON：一律按 UTF-8（atomic 一律写 UTF-8；不靠 UTF-8 模式——#514 后台商店查询用 -I 起、PYTHONUTF8 不生效，
    非中文 Windows 上 catalog.json 被按 cp1252 写坏，之后每次导入在初始化就 UnicodeDecodeError）。
    UTF-8 解不开 = 旧版按系统编码写坏的：依次严格试系统编码、cp1252，解得开且是 JSON 就当场自愈——原字节备份成 <名>.<编码>.bak，
    按 UTF-8 原子回写，回读核对与原对象相等。都解不开照样抛错（不返回空游戏库）"""
    path = Path(path)
    try:
        raw = winproc.read_shared(path) if os.name == "nt" else path.read_bytes()   # Windows 读时带共享删除，不挡作业替换记录（#561）
    except (FileNotFoundError, NotADirectoryError):   # NotADirectoryError：路径中间一级是文件（遍历到 .DS_Store 再读 .DS_Store/game.json，#343）＝没有
        return default
    try:
        return _undo_misheal(path, json.loads(raw.decode("utf-8")))
    except UnicodeDecodeError as first:
        for enc in dict.fromkeys(e for e in (system_encoding(), "cp1252") if e and _norm(e) != "utf8"):
            try:
                value = json.loads(raw.decode(enc))
            except (UnicodeDecodeError, ValueError, LookupError):
                continue
            bak = path.with_name(f"{path.name}.{enc}.bak")
            if not bak.exists():
                bak.write_bytes(raw)
            atomic(path, value)
            if json.loads(path.read_bytes().decode("utf-8")) != value:
                raise Failure(f"{path.name} 从 {enc} 转成 UTF-8 后核对不一致；原文件备份在 {bak.name}。")
            return value
        raise first


# Windows：替换走 winproc.replace_file（POSIX 语义，读方带共享删除时不再撞「拒绝访问」，#245 / #561 的根因）。
#   剩下的撞车只有第三方不带共享删除打开（杀毒 / 索引的短暂占用，或卷不支持 POSIX 语义回落 MoveFileEx）：仍按下面的退避重试（#130）
WIN_REPLACE_RETRIES = (0.02, 0.05, 0.1, 0.2, 0.4, 0.8)


def replace_retrying(tmp, path):
    nt = os.name == "nt"
    for delay in (*WIN_REPLACE_RETRIES, None) if nt else (None,):
        try:
            return winproc.replace_file(tmp, path) if nt else os.replace(tmp, path)
        except PermissionError:
            if delay is None:
                raise
            time.sleep(delay)


REWRITE_FAILED_LOG = re.compile(r"改写 \S+ 失败.*?日志 ([^）\n]+?\.log)")  # 非玩家句
REWRITE_DIGEST_LINES = 400


def write_rewrite_digest(source, target):
    """改写日志摘要：缺口按类别计数 + 非缺口行 + 前若干条缺口（地址和指令类型，不含游戏内容）"""
    counts, other, gaps, total = {}, [], [], 0
    with open(source, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("GAP\t"):
                total += 1
                detail = line.split("\t", 3)[-1]
                kind = re.sub(r"0x[0-9a-fA-F]+|\b[0-9a-f]{8}\b|#-?\w+|S\d_\d_C\d+_C\d+_\d|\b[xw]\d+\b", "_", detail)[:80]
                counts[kind] = counts.get(kind, 0) + 1
                if len(gaps) < REWRITE_DIGEST_LINES:
                    gaps.append(line)
            elif len(other) < REWRITE_DIGEST_LINES:
                other.append(line)
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"改写缺口共 {total} 条，按类别：\n")  # 非玩家句
        f.writelines(f"{n}\t{k}\n" for k, n in sorted(counts.items(), key=lambda kv: -kv[1])[:60])
        f.write("\n--- 非缺口行 ---\n"); f.writelines(l + "\n" for l in other)  # 非玩家句
        f.write("\n--- 前若干条缺口 ---\n"); f.writelines(l + "\n" for l in gaps)  # 非玩家句


def atomic(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with os.fdopen(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666), "w", encoding="utf-8") as f:   # 一律 UTF-8（#514：不靠 UTF-8 模式）；权限用系统缺省（11-16 不再自设）
            json.dump(value, f, ensure_ascii=False, indent=2)
            f.flush()
            os.fsync(f.fileno())
        replace_retrying(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def _windows():
    """resolve_path 的平台判断单独一处：单测只换它，不改全局 os.name（改了 pathlib 会去建 WindowsPath，在 Mac / Linux 上直接抛异常）"""
    return os.name == "nt"


def resolve_path(path):
    """Resolve a path while keeping Windows ERROR_UNRECOGNIZED_VOLUME (1005) volumes usable.

    Some mounted volumes allow normal file I/O but reject GetFinalPathNameByHandle. In that case,
    use an absolute lexical path only after checking existing components for symlinks/junctions;
    without real-path data, containment checks must not follow a reparse point unchecked.
    """
    path = Path(path).expanduser()
    try:
        return path.resolve()
    except OSError as error:
        if not _windows() or getattr(error, "winerror", None) != 1005:
            raise
        absolute = Path(os.path.abspath(path))
        if not absolute.anchor or absolute.anchor.startswith("\\\\"):
            raise
        try:
            linked = _links_on_path(absolute)
        except OSError:
            raise error
        if linked:
            raise error
        return absolute


def _links_on_path(absolute):
    """从盘根往下逐级 lstat 现存的各级：有符号链接 / junction（重解析点）→ True；读不了的一级抛 OSError（由调用方决定怎么报）"""
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            return False
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400):
            return True
    return False


def _letter_drive(path):
    return len(path.drive) == 2 and path.drive[1] == ":"


def resolve_library_path(path):
    """玩家选的游戏库根：存、显示、往下拼路径都用玩家写的样子。
    Windows 映射网络驱动器（Z: 盘 → NAS 共享）经 resolve 会被换成 UNC 内网地址：库根写进索引、回给前端显示的就成了「\\\\192.168.x.x\\…」，
    玩家看到盘符被换掉（11-14 维护者转 NAS 玩家）。所以盘符写法解析成 UNC 时保留盘符（路上没有符号链接 / junction 才留，有就照解析结果）；
    别的情况同 resolve_path。判「是不是同一位置 / 越不越界」一律两边都过 resolve_path 再比（contained），不拿这里的结果直接和解析过的路径比"""
    resolved = resolve_path(path)
    if not _windows():
        return resolved
    absolute = Path(os.path.abspath(Path(path).expanduser()))
    if _letter_drive(absolute) and not _letter_drive(resolved):
        try:
            if not _links_on_path(absolute):
                return absolute
        except OSError:
            pass
    return resolved


def contained(root, path):
    root, path = resolve_path(root), resolve_path(path)
    if path == root or root not in path.parents:
        raise Failure("操作路径不属于托管目录。")
    return path


def output_lines(stream, chunk=65536):
    """按行切子进程输出：LF 与 CR 都算换行。

    xcodebuild 这类工具用 CR 原地刷新进度（一整段下载可能十几分钟只有 CR、没有 LF）：
    按 readline() 读会把它当成一行 60 KB 的文本，调用方用第一个百分比判进度，就一直停在 0.0%。
    """
    read = getattr(stream, "read1", None) or stream.read   # read1：有数据就返回，不等凑满（CR 进度不换行）
    pending = b""
    while True:
        data = read(chunk)
        if not data:
            break
        pending += data
        start = 0
        i = 0
        while i < len(pending):
            byte = pending[i]
            if byte in (10, 13):
                yield pending[start:i]
                if byte == 13 and i + 1 < len(pending) and pending[i + 1] == 10:
                    i += 1   # CRLF 是一处换行，不产生空行
                start = i + 1
            i += 1
        pending = pending[start:]
    if pending:
        yield pending


def _sha256():
    """SHA-256 计算器：与资源包 / 核对共用 tools/sha256_fast.py（Windows 走 CNG）。App 里 build_app.py 把它拷进 Resources/Jobs；源码树里从仓库 tools/ 取"""
    try:
        import sha256_fast
    except ImportError:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import sha256_fast
    return sha256_fast.new()


def release_version(engine):
    """S2Arm 发版号（三端同号：Mac 启动器、Windows 启动器、iPhone 容器都显示这一个，11-09 维护者）：
    发布的引擎快照读打包时写进 engine.json 的 version；开发树取发版 tag 的数字段（v0.2.3-beta.1 → 0.2.3；规则见 _release_tag_numbers）。取不到 → 空串"""
    eng = Path(engine) / "engine.json"
    if eng.is_file():
        try:
            v = json.loads(eng.read_text(encoding="utf-8")).get("version")
            if isinstance(v, str) and v:
                return v
        except (OSError, ValueError):
            pass
    return _release_tag_numbers(engine)


def _release_tag_numbers(engine):
    """开发树的发版号数字段：HEAD 正好落在发版 tag 上就用它（热修分支上打的旧版号照旧是它自己）；否则取仓库里版本号最大的发版 tag。
    不用 git describe：热修 tag（v0.2.7–v0.2.9 打在热修分支上、没合回 main）不在 main 的祖先里，describe 一直找到 v0.2.6，
    开发版界面、CFBundleShortVersionString、engine.json 的 version 三端一起显示成发布前好几版（11-11 维护者截图发现）"""
    def git(*args):
        try:
            r = subprocess.run(["git", "-C", str(engine), *args], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return ""
        return r.stdout if r.returncode == 0 else ""
    def numbers(tag):
        m = re.match(r"v?(\d+(?:\.\d+)*)", tag.strip())
        return m.group(1) if m else ""
    exact = numbers(git("describe", "--tags", "--exact-match", "HEAD"))
    if exact:
        return exact
    found = [n for n in (numbers(t) for t in git("tag", "-l", "v[0-9]*").split() if "-test" not in t) if n]   # 测试版 tag（v0.6.0-test.1）不算发版号：正式 0.6.0 发布前开发版仍显示 0.5.x（11-19）
    return max(found, key=lambda n: tuple(int(x) for x in n.split(".")), default="")


def display_version(engine):
    """界面上给玩家看的版本字（11-11 维护者：玩家截图里要看得出是哪一版；Mac / Windows 启动器顶栏、iPhone 设置页与游戏库同一口径）：
    正式版「v0.3.0」；开发版「v<最新发版号>-dev+<7 位短提交号>」（最新发版号见 _release_tag_numbers：不要求是 HEAD 的祖先）。HEAD 正好落在发版 tag 上且已跟踪文件没改动 = 正式版。
    发布的引擎快照读打包时写进 engine.json 的 displayVersion；开发树在 engine 目录用 git 现算；git 不可用退回「v<release_version>」，连它也没有 → 空串"""
    eng = Path(engine) / "engine.json"
    if eng.is_file():
        try:
            v = json.loads(eng.read_text(encoding="utf-8")).get("displayVersion")
            if isinstance(v, str) and v:
                return v
        except (OSError, ValueError):
            pass
    short = release_version(engine)
    if not short:
        return ""

    def git(*args):
        try:
            r = subprocess.run(["git", "-C", str(engine), *args], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        return r.stdout.strip() if r.returncode == 0 else None

    commit = git("rev-parse", "--short=7", "HEAD")
    if not commit:
        return f"v{short}"
    dirty = git("status", "--porcelain", "--untracked-files=no")
    tag = git("describe", "--tags", "--exact-match", "HEAD") or ""
    if not dirty and re.fullmatch(r"v?" + re.escape(short), tag):
        return f"v{short}"
    pre = re.fullmatch(r"v?" + re.escape(short) + r"-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)", tag) if not dirty else None
    if pre:   # 测试版 tag（11-19：0.6.0 测试包 v0.6.0-test.1）：界面显示带 test 的完整版本号，同 Mac build_app.py
        return f"v{short}-{pre[1]}"   # 版本字里的 test 字样就是测试版标记（版本字中英文界面共用，不夹中文）
    return f"v{short}-dev+{commit[:7]}"


def digest(path, progress=None):
    h = _sha256()
    done = 0
    total = Path(path).stat().st_size
    with Path(path).open("rb") as f:
        while chunk := f.read(4 << 20):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    return h.hexdigest()


def clone(source, destination, progress=None):
    """→ True = 真克隆（APFS 共享块，不另占盘）；False = 退成了普通复制（跨卷 EXDEV、非 APFS、Windows）"""
    source, destination = Path(source), Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # macOS clonefile is an optimization, never part of the data format.
    import ctypes
    copied = False
    if sys.platform == "darwin" and hasattr(ctypes.CDLL(None), "clonefile"):   # Windows 上 CDLL(None) 直接抛 TypeError（11-07 内置引擎初始化）
        libc = ctypes.CDLL(None, use_errno=True)
        copied = libc.clonefile(os.fsencode(source), os.fsencode(destination), 0) == 0
    if not copied:
        with source.open("rb") as src, destination.open("xb") as dst:
            done, total = 0, source.stat().st_size
            while chunk := src.read(4 << 20):
                dst.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
    if progress:
        progress(source.stat().st_size, source.stat().st_size)
    return copied


def tree_hash(root, folders):
    h = hashlib.sha256()
    for folder in folders:
        top = Path(root) / folder
        paths = top.rglob("*") if top.is_dir() else [top]
        for p in sorted(paths):
            if set(p.relative_to(root).parts) & {"bin", "obj", "build", ".build", ".git", "__pycache__"}:
                continue
            if p.is_file():
                h.update(str(p.relative_to(root)).encode())
                h.update(bytes.fromhex(digest(p)))
    return h.hexdigest()


# ---- 默认位置下的公共目录（游戏库结构 P3：玩家指定的库是入口，私有目录只留设置、密钥和控制口文件）----
LIBRARY_MARKER = ".s2arm-library.json"


def default_home(private, library=None):
    """日志、任务记录、工具缓存、引擎副本的根：默认位置（请求里的 library）建好了（有库标识文件）就是它；还没建好（setup 之前、没选库）退回私有目录。
    只拿得到私有目录的读者（main.py get / events / cancel / prune / report）不传 library，按索引表 library-index.json 第一项记的默认位置算
    （layout.load_index 总把默认位置排第一，layout.follow_task 每个作业建任务时核一次它与请求一致）"""
    private = Path(private).expanduser().resolve()
    if library is None:
        try:
            locations = (read(private / "library-index.json", {}) or {}).get("locations") or []
        except (OSError, ValueError, AttributeError):
            locations = []
        library = locations[0].get("path") if locations and isinstance(locations[0], dict) else None
    if library:
        root = resolve_library_path(library)
        if (root / LIBRARY_MARKER).is_file():
            return root
    return private


def logs_root(private):
    """任务日志 + 任务记录（logs/jobs/）的目录：固定 <私有目录>/logs（剃刀 H6：原先「库建好了放 <库>/logs、没建好退回私有目录」，
    任务记录在三个目录间搬、后端和三个前端各算一遍，规则一漂控制口 / 崩溃日志就去错的地方找；并发作业同时收日志还出过事故）。
    后端所有读写日志 / 任务记录的地方都调它；前端照同一条规则：Swift S2ArmPaths.logsRoot、C# LibraryPaths.LogsRoot、s2armctl.logs_root"""
    return Path(private).expanduser().resolve() / "logs"


def jobs_dir(private):
    return logs_root(private) / "jobs"


def _job_dirs(private):
    """读一条任务时依次找：<私有目录>/logs/jobs；还没迁回来的旧位置 <默认位置>/logs/jobs（v0.3.0 之前库建好后写在库里，
    下一个作业建任务时 layout.adopt_library_logs 迁回私有目录）；P3 之前的 <私有目录>/jobs"""
    private = Path(private).expanduser().resolve()
    old = default_home(private) / "logs" / "jobs"
    return list(dict.fromkeys([jobs_dir(private), *([old] if old != jobs_dir(private) else []), private / "jobs"]))


IMPORT_RECORD_SOURCE_DIAGNOSTIC = 16_000    # 任务记录里每个来源的诊断上限（字符）：完整的在终态事件与按来源写的 inspect 日志里
IMPORT_RECORD_DIAGNOSTICS_TOTAL = 200_000   # 任务记录里全部来源诊断的总上限：catalog 每次要读回最近 200 条任务记录，不能被一次大批导入撑大


def record_sources(sources):
    """逐来源导入结果写进任务记录的份：诊断每条截到 IMPORT_RECORD_SOURCE_DIAGNOSTIC（保留头尾），总量超 IMPORT_RECORD_DIAGNOSTICS_TOTAL
    后其余来源只留 code / reason / status（诊断清空）"""
    out, budget = [], IMPORT_RECORD_DIAGNOSTICS_TOTAL
    for row in sources:
        row = dict(row)
        text = row.get("diagnostic") or ""
        if len(text) > IMPORT_RECORD_SOURCE_DIAGNOSTIC:
            marker = "\n…[已省略中间部分]…\n"  # 非玩家句
            keep = IMPORT_RECORD_SOURCE_DIAGNOSTIC - len(marker)
            text = text[:keep // 4] + marker + text[len(text) - (keep - keep // 4):]
        if len(text) > budget:
            text = ""
        budget -= len(text)
        row["diagnostic"] = text
        out.append(row)
    return out


# 只读探测（启动器剃刀 H4）：结果走标准输出，不落任务记录（request / journal / events 三个文件 + 起 ps 取进程起始时刻），作业开头也不收日志、
#   不起后台商店查询、引擎副本有安装标记就直接用（逐文件复核留给真用引擎产物的作业，#304）。装机页每 5 秒一次的 installCheck、
#   设备探测、刷新游戏库都在这里：原先每次都落三个文件，开一小时约 720 个作业记录
PROBES = ("catalog", "installCheck", "targetDevice", "locations", "patchMatch")
JOURNAL_PROGRESS_S = 5.0   # 进度事件最多隔这么久重写一次任务记录（剃刀 H5：原先每条事件都原子重写 + fsync）；阶段、诊断、终态每次都写并 fsync
LOCK_WAIT_S = 30   # 短作业排队等游戏库锁的上限（秒）：出包这类长任务占着时，过了上限就如实报「在等谁」，不无限挂着
OPERATION_NAMES = {"build": "出包", "ipa": "打 iPhone 包", "import": "导入游戏", "install": "推到 iPhone", "updateGame": "更新这款", "installUpdate": "更新 iPhone 上的 S2Arm", "installUpdatePrepare": "准备更新 iPhone 上的 S2Arm",
                   "installRemove": "从 iPhone 删除", "installRestore": "恢复 iPhone 上的上一版", "catalog": "刷新游戏库", "customize": "改游戏名或封面",
                   "remove": "移出游戏库", "delete": "删除游戏", "clean": "清理空间", "spaceFree": "释放空间", "launch": "开始游戏",
                   "moveGame": "移动游戏", "relinkGame": "重新关联原件", "forgetGame": "忘掉游戏", "setup": "设置游戏库", "keys": "导入密钥",
                   "saveBackup": "备份存档", "saveRestore": "恢复存档", "saveInspect": "查看存档", "saveStatus": "读存档状态", "savePrevious": "查上一份存档", "saveRestorePrevious": "回到上一份存档", "saveDiscardSeed": "删掉待放入的存档",
                   "communitySave": "保存补丁设置", "diagnostics": "导出诊断", "sgradeScan": "查找可升级 S 级优化的游戏", "sgradeMac": "给 Mac 游戏补上 S 级优化", "sgradeReenable": "重新打开 S 级优化"}


def describe_holder(info):
    """占锁任务的一句话：「出包 · 0100…（任务 t12）」；不知道是谁时「另一个任务」"""
    if not info:
        return "另一个任务"
    op = OPERATION_NAMES.get(info.get("operation"), info.get("operation") or "另一个任务")
    return op + (" · " + info["gameId"] if info.get("gameId") else "") + ("（任务 " + str(info["taskId"]) + "）" if info.get("taskId") else "")  # 非玩家句


class Context:
    _default_developer = ()   # 未查

    @property
    def default_developer(self):
        """没传 Xcode 时的缺省（default_developer_dir）：第一次要用时才找、本任务全程共用。原先建任务时就找，
        不用 Xcode 的任务（导入、推到 iPhone、环境检查等）也去翻目录，找不到内置的还会翻外接卷、弹系统隐私窗"""
        if self._default_developer == ():
            self._default_developer = None if self.r.get("developerDir") else default_developer_dir()
        return self._default_developer

    def __init__(self, request):
        # 空间预检没过、玩家点「仍然继续」重交的出包任务（library.check_pack_space）：只认布尔值、只给 Mac 打包 / 打 iPhone 包
        if "spaceOverride" in request and (not isinstance(request["spaceOverride"], bool) or request.get("operation") not in ("build", "ipa")):
            raise Failure("spaceOverride 仅支持 Mac 打包（build）与打 iPhone 包（ipa）任务的布尔值。")
        # 同一次出包后面还有平台（pack/contracts/README.md「同一次出包后面还有平台」）：只给打 iPhone 包，只认布尔值
        if "followUp" in request and (not isinstance(request["followUp"], bool) or request.get("operation") != "ipa"):
            raise Failure("followUp 仅支持打 iPhone 包（ipa）任务的布尔值。")
        self.r = request
        self.task = request.get("jobId") or request.get("taskId") or str(uuid.uuid4())
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", self.task):
            raise Failure("无效的任务标识。")
        self.operation = request["operation"]
        self.private = Path(request["privateRoot"]).expanduser().resolve()
        self.private.mkdir(parents=True, exist_ok=True)   # 系统缺省权限（~/Library、%LOCALAPPDATA% 本来就只归当前用户；11-16 不再自设）
        self.engine = Path(request.get("engineDir") or ".").expanduser().resolve()
        self.library = resolve_library_path(request.get("library") or self.private / "unconfigured")
        import layout
        self.library = layout.relocate_default(self.private, self.library)   # 旧默认库（私有目录里的）已搬到 ~/S2Arm：照新位置用（游戏库结构 P3）
        self.logs = logs_root(self.private)   # 本任务的日志与任务记录（logs/jobs/）：固定在私有目录（剃刀 H6）
        self.keys = self.private / "keys/prod.keys"
        self.t0 = time.monotonic()
        self.stage = ""
        self.stage_key, self.keychain_prompt = None, False   # 本阶段的机读标记（begin 设）：前端认这两个，不按中文阶段名 / 步骤号猜
        self.number = self.count = self.seq = 0
        self.cancelled = False
        self.children = set()
        self.guard = threading.RLock()
        self.last_progress = 0
        self.overall = overall_progress.Overall()   # 整单进度（每条事件的 overall，只增不减；pack/jobs/overall.py）
        self.last_log = None
        self.frontend_gone = False
        self.recorded = self.operation not in PROBES
        self.last_journal = 0.0
        self.jobpath = self.logs / "jobs" / (self.task + ".json")
        self.events = self.logs / "jobs" / (self.task + ".events.jsonl")
        self.requestpath = self.logs / "jobs" / (self.task + ".request.json")
        layout.follow_task(self)   # 索引表第一项核成默认库位置；同一任务上次跑在库建好之前（记录在私有目录）：先把它的记录搬过来，续跑的次数、阶段接得上
        if not self.recorded:
            self.job = dict(schema=1, id=self.task, operation=self.operation, gameId=request.get("gameId"),
                            state="running", started=time.time(), diagnostics=[], pid=os.getpid(), attemptNo=1)
            return
        # Keep the request next to the journal so `resume` can run the same task again after a kill without the caller having to keep the
        # request file around (J10).
        if not self.requestpath.exists():
            atomic(self.requestpath, request)
        # The journal of a previous run of this same task: an attempt that never wrote a terminal state (killed, power loss, frontend gone) is the
        # only thing that can tell a restart apart from a first run, so keep what it reached instead of overwriting it silently (J10 / #186).
        previous = read(self.jobpath) or {}
        attempt = dict(previous.get("attempts") or {}) if previous.get("id") == self.task else {}
        self.job = dict(schema=1, id=self.task, operation=self.operation, gameId=request.get("gameId"),
                        state="running", started=time.time(), diagnostics=[], pid=os.getpid(), pidStart=pid_start(os.getpid()),
                        attempts=attempt)
        if previous.get("id") == self.task:
            # Every new run of the same task is another attempt, whether the last one was killed or finished (`resume --force`); what the last
            # attempt reached is kept either way, and `ended` on it tells `describe()` whether it was interrupted.
            self.job.update(attemptNo=int(previous.get("attemptNo") or 1) + 1, previousState=previous.get("state"),
                            previousStage=previous.get("stage") or "", previousStarted=previous.get("started"),
                            previousEnded=previous.get("ended"))
        else:
            self.job["attemptNo"] = 1
        atomic(self.jobpath, self.job)

    def check(self):
        if self.cancelled:
            raise Cancelled()

    def require_source(self):
        required = ("runtime/include/s2a_native.h", "runtime/rewriter/rewriter.c", "services/nxaot.sh",
                    "pack/nx_unpack/nx_unpack.csproj", "tools/safe_killpg.sh")
        missing = [p for p in required if not (self.engine / p).is_file()]
        if missing:
            raise Failure("请选择完整的 S2Arm 源码目录，缺少：" + "、".join(missing), code="engine_incomplete")  # 非玩家句

    def require_library(self):
        marker = read(self.library / ".s2arm-library.json", {})
        if not self.r.get("libraryId") or marker.get("id") != self.r["libraryId"]:
            parts = self.library.parts
            if len(parts) > 2 and parts[1] == "Volumes" and not Path("/", "Volumes", parts[2]).exists():
                raise Failure(f"游戏库所在的磁盘「{parts[2]}」没连上：插上这块盘后再试一次。")
            if marker.get("id"):
                # 这个位置是一个游戏库，只是不是启动器记着的那个（从备份恢复 / 别处拷来 / 重装启动器后记录丢了）：说清是哪个、有几款，
                #   玩家确认就改用它（前端重走选库、记下它的库号），不直接拒、也不悄悄改用（11-17 维护者）
                try:
                    count = sum(1 for g in json.loads((self.library / "catalog.json").read_text(encoding="utf-8")).get("games", []) if not g.get("hidden"))
                except (OSError, ValueError, AttributeError):
                    count = 0
                raise Failure(f"「{self.library.name}」（{self.library}）里是另一个游戏库，有 {count} 款游戏，不是 S2Arm 上次记下的那个。点「仍然继续」就改用它；不是你要的，就到设置里重新选游戏库。",
                              code="library_id_mismatch", facts={"libraryId": marker["id"]})
            raise Failure("游戏库未连接或标识不符，请重新选择游戏库。")

    def lock_holder(self):
        """写进锁旁挂文件的「我是谁」：别的任务据此说清在等谁"""
        return dict(taskId=getattr(self, "task", None), operation=getattr(self, "operation", None),
                    gameId=(getattr(self, "r", None) or {}).get("gameId"), pid=os.getpid(),
                    host=socket.gethostname(), startedAt=round(time.time(), 3))

    @contextlib.contextmanager
    def lock(self, name="catalog", wait=None, root=None, shared=False):
        """游戏库锁（启动器剃刀 H1 / H2：库内只有这一种等待语义）：拿不到就排队，最多等 wait 秒（缺省 lock_wait / LOCK_WAIT_S），期间发进度「在等『X』结束」
        （phase=waiting_lock、holder=占锁任务）；到上限报 code=library_busy、原句「另一个任务正在使用游戏库」，失败事件带 holder，占锁者也进 internal。
        root 给了是另一个库（整库搬家的旧位置），不核本任务的库标识。拿到后在旁挂文件记下本任务。报错原句不改：旧前端按它做本地化匹配。
        shared = True：共享模式（filelock 读写锁的「读」）——执行锁上起局与出包拿共享（各自再拿这一款的锁），清理 / 删除 / 搬家等全库动作拿独占"""
        self.check()
        if root is None:
            self.require_library()
        wait = getattr(self, "lock_wait", LOCK_WAIT_S) if wait is None else wait
        def waiting(info, waited):
            try:
                self.emit("progress", "在等「" + describe_holder(info) + "」结束…", phase="waiting_lock", holder=info)  # 非玩家句
            except Exception:   # 进度发不出去（作业已在收尾 / 测试里的裸上下文）不影响排队
                pass
        try:
            with filelock.held(Path(root or self.library) / ("." + name + ".lock"), wait_s=wait, holder=self.lock_holder(), on_wait=waiting, shared=shared):
                yield
        except filelock.Busy as busy:
            detail = f"等了 {wait:g} 秒，游戏库{name}锁仍被占：{describe_holder(busy.holder)}（{json.dumps(busy.holder, ensure_ascii=False)}）"  # 非玩家句
            raise library_lock_failure(busy.holder, "另一个任务正在使用游戏库，请稍后重试。", detail) from None  # 非玩家句

    @contextlib.contextmanager
    def idle(self, name="execution"):
        """「库里没任务在用这把锁才顺手做、否则这次不做」的活（清残留、改文件夹名）：拿得到 yield True（照样记占锁者），拿不到不等、yield False。
        和 lock 同一把锁、同一个旁挂文件；不查取消：被取消的任务收尾也要能用"""
        with contextlib.ExitStack() as stack:
            try:
                stack.enter_context(filelock.held(self.library / ("." + name + ".lock"), holder=self.lock_holder()))
            except filelock.Busy:
                yield False
                return
            yield True

    def catalog(self):
        self.require_library()
        return read(self.library / "catalog.json", {"schema": 1, "games": []})

    def game(self):
        if not self.r.get("gameId"):   # 要游戏的作业请求里没带游戏号：前端不再按名单拦（自己人之间不设允许清单），这里说清
            raise Failure("请求里没有游戏号：这项任务要指定是哪款游戏。")   # 非玩家句
        game = next((g for g in self.catalog()["games"] if g["id"] == self.r.get("gameId")), None)
        if not game:
            raise Failure("游戏未登记在当前游戏库中。")
        return game

    def emit(self, kind="progress", message="", done=None, total=None, **extra):
        with self.guard:
            self.check()
            now = time.monotonic()
            # 上次前进时刻（#1770 / #1773：玩家分不清「编得慢」和「卡死」）：换了一步，或这一步 / 任一计数器的完成数变了，才算前进；
            #   心跳、批次活动脉冲（只走时间不走分子）不算。随每条事件与作业记录发出，界面显示「上次前进 N 秒前」
            if kind in ("stage", "progress"):
                sig = (self.number, self.stage, done, total, tuple((c.get("name"), c.get("done")) for c in extra.get("counters") or ()))
                if sig != getattr(self, "_advance_sig", None):
                    self._advance_sig, self.advanced_at = sig, time.time()
            if kind == "progress" and now - self.last_progress < .2 and done != total:
                return
            if kind == "progress":
                self.last_progress = now
            elif kind == "stage":
                self.last_progress = 0
                self.job.update(done=None, total=None, unit=None)
            if kind not in ("stage", "progress"):
                done, total = self.job.get("done"), self.job.get("total")
                if self.job.get("unit"):
                    extra.setdefault("unit", self.job["unit"])
            self.seq += 1
            state = extra.pop("state", self.job["state"])
            transient = extra.pop("transient", False)   # 只给标准输出、不进事件记录（出包编排转发子作业事件：子作业自己的记录里已有，剃刀 H13）
            if kind == "plan":
                self.overall.plan(extra.get("milestones"))
            overall = self.overall.feed(kind, now - self.t0, getattr(self, "milestone", None), state, self.number, self.count, done, total,
                                        sub=kind == "progress" and "counters" in extra)   # 阶段工具的子计数（emit_stage_counter）
            if overall is not None:
                extra["overall"] = overall
                self.job["overall"] = overall
            event = dict(schema=1, jobId=self.task, taskId=self.task, seq=self.seq,
                         timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(), type=kind,
                         operation=self.operation, gameId=self.r.get("gameId"), stage=self.stage,
                         stageIndex=self.number, stageCount=self.count, state=state,
                         doneUnits=done, totalUnits=total, done=done, total=total,
                         elapsed=now-self.t0, message=message, milestone=getattr(self, "milestone", None),
                         stageKey=self.stage_key, keychainPrompt=self.keychain_prompt,
                         advancedAt=getattr(self, "advanced_at", None), **extra)
            if kind == "stage":
                self.job.update(done=None, total=None, unit=None)
            elif kind == "progress":
                self.job.update(done=done, total=total, unit=extra.get("unit"))
            if kind == "plan":
                self.job["milestones"] = extra.get("milestones")
            elif kind == "space":
                # 空间预算单独存一份：后面的阶段消息只覆盖 message，不冲掉它
                self.job["space"] = extra.get("space")
            self.job["milestone"] = getattr(self, "milestone", None)
            self.job.update(stage=self.stage, stageIndex=self.number, stageCount=self.count,
                            elapsed=event["elapsed"], seq=self.seq, advancedAt=event["advancedAt"])
            if message:
                self.job["message"] = message
            line = json.dumps(event, ensure_ascii=False)
            try:
                # 剃刀 H5：心跳只给标准输出（前端据此知道作业还活着），不落盘；进度追加进事件文件、不 fsync，任务记录最多隔 JOURNAL_PROGRESS_S 重写一次；
                #   阶段 / 诊断 / 终态等每条都重写任务记录并 fsync（被杀时不丢最后一个阶段，J10）。原先每条事件（含每秒心跳）都整份重写 + 两次 fsync
                if self.recorded and kind != "heartbeat" and not transient:
                    durable = kind != "progress"
                    if durable or now - self.last_journal >= JOURNAL_PROGRESS_S:
                        atomic(self.jobpath, self.job)
                        self.last_journal = now
                    with os.fdopen(os.open(self.events, os.O_WRONLY | os.O_APPEND | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o666), "a", encoding="utf-8") as f:
                        f.write(line + "\n")
                        if durable:
                            f.flush()
                            os.fsync(f.fileno())   # a killed worker must not take its own last stage line with it (J10 journal)
            except OSError as error:
                # A full or failing disk used to let the job finish "successfully" with its record missing: whoever looks for it later finds
                # nothing, or finds it stuck at an old stage. Say so and fail the job instead (J10).
                self.journal_broken = True
                raise Failure(f"任务记录写不进磁盘（{error.strerror or error}）：这次的记录没有落盘，不当作完成。", code="journal_write_failed") from error
            # 启动器（读本进程标准输出的那一端）没了：作业照跑，事件只写记录（jobs/<任务>.json + .events.jsonl），重开的启动器 / s2armctl wait 按记录接上。
            #   10-26 Spare-host：内存吃紧启动器失联，作业不该跟着死。标准输出 / 错误改指 /dev/null：之后起的子进程不会因写断掉的管道被 SIGPIPE 杀掉
            if not self.frontend_gone:
                try:
                    print(line, flush=True)
                except OSError:   # BrokenPipeError 等
                    self.frontend_gone = True
                    try:
                        null = os.open(os.devnull, os.O_WRONLY); os.dup2(null, 1); os.dup2(null, 2); os.close(null)
                    except OSError:
                        pass

    def begin(self, stage, number=1, count=1, key=None, keychain=False):
        """key：本阶段的固定机读名（前端按它认，中文阶段名只给人看、随时会改）；keychain：本阶段可能弹出钥匙串授权（前端据此挂提示）。
        两者都随本阶段的每条事件发（stageKey / keychainPrompt），下一次 begin 重置"""
        self.stage_key, self.keychain_prompt = key, bool(keychain)
        # Deliberately no "completed stages" list (维护方 10-25 口径): whether a stage is done has exactly one judge, its product key in the product
        # root (.stage_<阶段>, pack/refresh.sh). A second copy in the journal would disagree the moment a kill lands between committing a stage and
        # writing the journal. What this journal does keep — attempts / previousStage — is for display and diagnosis only and never decides a skip.
        self.stage, self.number, self.count = stage, number, count
        # How many times this stage was started: the number a resume reads to say "this stage was interrupted, not finished"
        self.job.setdefault("attempts", {})
        self.job["attempts"][stage] = int(self.job["attempts"].get(stage, 0)) + 1
        self.emit("stage", stage)

    def stage_progress(self, path, pos, final=False):
        """读进度文件 pos 之后新增的整行，发一次 progress；返回新的读到位置。
        出包时几个计数器交替写（刷新各阶段 7/10、直连库 2/6、编库 n/23502、通用翻译 n/45455…），只按最后一行发会来回跳（10-26 维护者：
        「进行中的环节能看到着色器总数和编译数量的实时变化最好」）：每个计数器各记「已完成 / 总数 / 起止时刻」，
        counters = 还没做完的逐项计数（总数 ≥ 20，同 tools/progress.sh 的阶段计数口径），各带每秒速度；done / total 取最近一次更新的逐项计数，没有就取最后一行"""
        activity = getattr(self, "_batch_activity", None)
        if activity is None:
            activity = self._batch_activity = BatchActivity(engine=self.engine, alive=pid_alive)
        previous = activity.data
        try:
            with open(path, "rb") as f:
                f.seek(pos)
                chunk = f.read()
        except OSError:
            activity.clear()
            self.clear_stage_activity(previous)
            return pos
        end = chunk.rfind(b"\n")
        if end < 0:
            if final:   # 收尾补读没有新行：批次活动已结束，收掉，不再发脉冲
                activity.clear()
                self.clear_stage_activity(previous)
            else:
                self.stage_activity()
            return pos
        seen = getattr(self, "_counters", None)
        if seen is None:
            seen = self._counters = {}
        last, fine = None, None
        # 行尾 \r\n 也认（协议是 LF；Windows 上用文本模式写的工具会写成 CRLF，末列带 \r 就整行判成不是数字、进度丢掉）
        lines = [line.rstrip("\r") for line in chunk[:end].decode(errors="replace").split("\n")]
        for line in lines:
            activity.accept(line)
        if final:   # 子进程树已收完时的补读：批次活动必然已结束，只发计数、不再发「还在编」的活动脉冲
            activity.clear()
        for line in lines:
            f = line.split("\t")
            if len(f) != 4 or not f[2].isdigit() or not f[3].isdigit() or int(f[3]) <= 0:
                continue
            try:
                t = float(f[0])
            except ValueError:
                continue
            name, done, total = f[1], int(f[2]), int(f[3])
            c = seen.get(name)
            if c is None or total != c["total"] or done < c["done"]:   # 新计数器 / 同名计数器重新开始：速度从这一行重算
                c = seen[name] = dict(name=name, total=total, t0=t, d0=done)
            c.update(done=done, t=t)
            if not hasattr(self, "_activity_counters"):
                self._activity_counters = {}
            self._activity_counters[name] = c
            last = c
            if total >= 20 or name.startswith("直连库·"):  # 非玩家句
                fine = c
        if last is not None:
            main = fine or last
            self.emit_stage_counter(main)
            direct_space_recheck(self, seen)   # 直连段按本局实际消耗外推复核盘（#498 重构发进度后挪到这里）
        else:
            if previous and activity.data is None:
                self.clear_stage_activity(previous)
            else:
                self.stage_activity()
        return pos + end + 1

    def emit_stage_counter(self, main, pulse=False):
        activity = getattr(self, "_batch_activity", None)
        data = activity.snapshot() if activity else None
        active = data is not None and data["stage"] == main["name"]
        counters = []
        for c in getattr(self, "_counters", {}).values():
            if (c["total"] < 20 and not c["name"].startswith("直连库·")) or c["done"] >= c["total"]:  # 非玩家句
                continue
            # Only elapsed time advances during a long batch, never its MB numerator.
            t = time.time() if pulse and c["name"] == main["name"] else c["t"]
            rate = (c["done"] - c["d0"]) / (t - c["t0"]) if t > c["t0"] and c["done"] > c["d0"] else None
            counters.append(dict(name=c["name"], done=c["done"], total=c["total"], rate=round(rate, 3) if rate else None))
        message = f"{main['name']} {main['done']}/{main['total']}"
        if active:
            message += "（" + activity.describe(data) + "）"
            activity.sent()
        # 编批库的计数是源码 MB（Build.cs Ticker「直连库·编批库（MB）」）：单位照实写，原先一律「项」，界面显示「1894 / 64490 项」（#944）
        unit = "MB" if main["name"].endswith("（MB）") else "项"  # 非玩家句
        self.emit(message=message, done=main["done"], total=main["total"], unit=unit, counters=counters,
                  **({"activity": data} if active else {}))

    def stage_activity(self):
        activity = getattr(self, "_batch_activity", None)
        if activity is None or not activity.due():
            return
        previous = activity.data
        data = activity.snapshot()
        if data is None:
            self.clear_stage_activity(previous)
            return
        main = getattr(self, "_activity_counters", {}).get(data["stage"])
        if main is not None:
            self.emit_stage_counter(main, pulse=True)
        elif main is None:  # Cache-only batches have no MB denominator; report activity without inventing work.
            self.emit(message=data["stage"] + "（" + activity.describe(data) + "）", activity=data)
            activity.sent()

    def clear_stage_activity(self, previous):
        if previous is None:
            return
        main = getattr(self, "_activity_counters", {}).get(previous["stage"])
        if main is not None:
            self.emit_stage_counter(main)
        else:
            self.emit(message=previous["stage"])

    def progress(self, done, total):
        self.check()
        self.emit(done=done, total=total, unit="B")

    def redact(self, value, *, limit=3000):
        # 只留一份脱敏规则（维护者 10-23）：形状规则 + 按值匹配都在 tools/redact.py（与诊断报告共用，PR #64）。
        # 这里先把游戏库 / 私有目录 / 引擎 / 主目录换成标签（保留可读的相对位置），再交给共用规则；按值匹配用玩家导入的 prod.keys
        text = str(value)
        for p, label in ((self.library, "[LIBRARY]"), (self.private, "[PRIVATE]"), (self.engine, "[SOURCE]"), (Path.home(), "~")):
            text = text.replace(str(p), label)
        text = shared_redact().redact_text(text, key_values=self.key_values())
        return text if limit is None else text[-limit:]

    def key_values(self):
        if not hasattr(self, "_key_values"):
            self._key_values = shared_redact().load_key_values([self.keys] if getattr(self, "keys", None) else [])
        return self._key_values

    def failure_roots(self):
        """explain 判「原件不见了 / 去系统设置授权」用的玩家侧路径（启动器剃刀 H8）：这次导入的路径、这款登记的原件，加游戏库根。取不到的省略"""
        sources = [Path(p).expanduser() for p in (self.r.get("paths") or []) if isinstance(p, str)]
        if self.r.get("gameId"):
            try:
                from library import selected_packages, source_path
                sources += [source_path(self, p) for p in selected_packages(self.game())]
            except Exception:   # 游戏没登记 / catalog 读不了：只按已有的判，判不了的落 job_failed
                pass
        return dict(sources=sources, library=getattr(self, "library", None))

    def describe_failure(self, error, severity="error", exit_code=None):
        failure = explain(error, **self.failure_roots())
        code = failure.code
        said = estimate_low(str(failure), getattr(self, "space_budget", None)) if code == "disk_full" else str(failure)   # 预检说够却写满：说清是预估偏低（#444）
        result = dict(code=code, severity=severity, summary=self.redact(said),
                      suggestion=getattr(failure, "advice", None) or recovery(code, packaged=bool(self.r.get("engineBundle")), tv=str(getattr(self, "operation", "")).startswith("tv"), device=device_of(self)), detail="", stage=self.stage,
                      component="S2Arm", logPath=str(self.last_log) if self.last_log else None)
        if exit_code is not None:
            result["exitCode"] = exit_code
        if failure.holder is not None:   # library_busy：占锁任务 {taskId, operation, gameId, pid, startedAt}，前端据此说「在等谁」
            result["holder"] = failure.holder
        if getattr(failure, "facts", None):   # 机读失败事实（errors.cause_facts）：改写失败的模块 / 缺口数 / 缺口类别
            result["facts"] = failure.facts
        if self.last_log and self.last_log.is_file():
            with self.last_log.open("rb") as f:
                f.seek(max(0, self.last_log.stat().st_size - 16000))
                result["detail"] = self.redact(f.read().decode(errors="replace"))
        # 内部原文（阶段 / rc / 构建号 / 红行 / 归因依据）不上屏，放 detail 开头随「复制错误摘要」带走
        internal = getattr(failure, "internal", "")
        if internal:
            result["detail"] = self.redact(internal) + ("\n\n" + result["detail"] if result["detail"] else "")
        # 失败事实随错误摘要带走（玩家 #5 #8：摘要里只有「工具退出」，看不出阶段、退出码和原文）
        rc = exit_code if exit_code is not None else (self.last_rc or None if hasattr(self, "last_rc") else None)
        result["failureSummary"] = failure_summary(self.stage, rc, [self.redact(line, limit=None) for line in last_output_lines(self.last_log)])
        return result

    def diagnostic(self, message, code="job_failed", severity="error", exit_code=None, internal="", holder=None, facts=None, advice=None):
        # holder / facts 原样带进失败结果（原先这里只按原句 + 码 + internal 重建 Failure，library_busy 的占锁者、改写失败的机读事实走真作业路径时都丢了）
        result = self.describe_failure(Failure(message, code=code, internal=internal, holder=holder, facts=facts, advice=advice), severity, exit_code)
        self.job["diagnostics"].append(result)
        self.emit("diagnostic", result["summary"], diagnostic=result, error=result,
                  diagnostics=self.job["diagnostics"], logRef=result["logPath"])
        return result

    def save_rewrite_digest(self, failure):
        """改写失败的明细日志在产物根 .failed/ 下（可达几十 MB）：诊断包只收日志目录（core.logs_root），这里存一份摘要进去，否则定不了缺的是哪类指令（#108）"""
        match = REWRITE_FAILED_LOG.search(str(failure) + "\n" + getattr(failure, "internal", ""))
        if not match:
            return
        target = self.logs / f"{self.task}-rewrite-gaps.log"
        try:
            write_rewrite_digest(Path(match.group(1)), target)
        except OSError:
            pass

    def report_failure(self, error):
        failure = explain(error, **self.failure_roots())
        self.save_rewrite_digest(failure)
        if self.last_log is None:
            self.last_log = self.logs / (self.task + "-failure.log")
            self.last_log.parent.mkdir(parents=True, exist_ok=True)
            self.last_log.write_text(self.redact(failure) + "\n" + (self.redact(failure.internal) + "\n" if getattr(failure, "internal", "") else ""))
        records = self.job["diagnostics"]
        previous = records[-1] if records else None
        said = estimate_low(str(failure), getattr(self, "space_budget", None)) if failure.code == "disk_full" else str(failure)   # 同 describe_failure
        if (previous and previous["severity"] == "error" and previous["code"] == failure.code
                and previous["summary"] == self.redact(said)):
            if previous["logPath"] == (str(self.last_log) if self.last_log else None):
                return previous
            records.pop()
        # A caller may know more than the subprocess runner (e.g. inspect's
        # documented unreadable-container result). Replace only this stage's
        # generic tool error and preserve its evidence and real exit code.
        exit_code = None
        if (previous and previous["code"] == "tool_exit" and failure.code != "job_failed"
                and previous["stage"] == self.stage and previous["logPath"] == str(self.last_log)):
            exit_code = records.pop().get("exitCode")
        return self.diagnostic(str(failure), failure.code, exit_code=exit_code, internal=getattr(failure, "internal", ""),
                               holder=getattr(failure, "holder", None), facts=getattr(failure, "facts", None))

    def fonts_dir(self):
        # 共享字体：玩家在设置里选了原版字体目录就用它，否则用应用自带的开源替代字体（Resources/Fonts，build_app.py 打进去；维护者 10-23：不再作为检测项）
        for raw in (self.r.get("fontsDir"), str(Path(self.r.get("resources") or self.private) / "Fonts")):
            if raw:
                d = Path(raw).expanduser().resolve()
                if d.is_dir() and any(d.glob("*.bfttf")):
                    return d
        return None

    @property
    def work_root(self):
        """跨游戏的工具缓存（cache/、run/）：默认位置的 tool-cache/（游戏库结构 P3；原来库在私有目录里时 = 私有目录、否则 = 库/.s2arm-work，
        layout._upgrade_4 搬过来）。库还没建好（setup 之前的环境检查等）退回 <私有目录>/tool-cache"""
        return default_home(self.private, self.library) / "tool-cache"

    def env(self):
        env = dict(os.environ)
        for k in list(env):
            if k.startswith(("S2A_", "NXGAL_", "DM_", "DMSH", "GAL_", "PSO_", "PYTHON")):
                env.pop(k)
        cache = self.work_root / "cache"
        cache.mkdir(parents=True, exist_ok=True)
        env.pop("DEVELOPER_DIR", None)
        # GUI 不接受继承的通用 J；抽取与直连分别由 cores.sh 的阶段预算决定。
        env.pop("J", None)
        # 宿主静默崩溃（runtime/src/native.h s2a_crash_quiet）：出包类作业里起的 s2arm-host 都是辅助运行（游戏 GLSLC / Oodle 工具模式、管线归档），
        #   出错只写日志、按退出码交回，不按信号死、不弹 macOS「意外退出」框（玩家看到系统崩溃框、出包却成功）。起局（launch）是玩家真在玩：
        #   照旧交给系统；只有启动器按 --control 测试实例起（S2ARM_CONTROL=1）时也静默
        if getattr(self, "operation", None) != "launch" or os.environ.get("S2ARM_CONTROL") == "1":
            env["S2A_CRASH_QUIET"] = "1"
        if self.r.get("spaceOverride") is True:   # 玩家点过「仍然继续」：直连阶段开工前的空间复核也只打黄（pack/jobs/space_rule.py）
            env["S2A_SPACE_OVERRIDE"] = "1"
        gid = self.r.get("gameId")
        if gid and self.operation in ("build", "ipa"):   # 直连缓存跟着游戏的存放位置走（外接盘时落外接盘，library.direct_cache_dir）
            import library
            env["S2A_DIRECT_CACHE"] = str(library.direct_cache_dir(self, gid))
            env["S2A_AOC_CACHE"] = str(library.aoc_cache_dir(self, gid))   # 追加内容缓存同样跟着游戏走（#1725）
        developer = self.r.get("developerDir") or self.default_developer   # 启动器传的 > 装着的最新 Xcode；都没有就不设
        if developer:
            env.update(DEVELOPER_DIR=developer, S2A_DEVELOPER_DIR=developer)
        env.update(PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1", PYTHONUNBUFFERED="1", LC_ALL="C",
                   DOTNET_CLI_HOME=str(cache / "dotnet-home"), NUGET_PACKAGES=str(cache / "nuget"),
                   DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1", S2A_PSO_REMOTE="off", S2A_DL_REMOTE="off",
                   DOTNET_GENERATE_ASPNET_CERTIFICATE="false", DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
                   S2A_CS_CACHE=str(cache / "nxaot"), S2A_VPX_CACHE=str(cache / "vpx"),
                   S2A_OPUS_CACHE=str(cache / "opus"), S2A_SHX_CACHE=str(cache / "shx"),
                   S2A_SHCOLLECT=str(cache / "shcollect"), GAL_AIR_CACHE=str(cache / "air"),
                   S2A_CSCLOSURE_CACHE=str(cache / "csclosure"),
                   S2A_XCKEY_CACHE=str(cache / "xckey"), S2A_SCHED_DIR=str(cache / "local-observations"),
                   S2A_PROGRESS=str(self.logs / (self.task + ".progress.tsv")),
                   GAL_PSO_CACHE=str(cache / "pso"), S2A_ROOTS_NO_BOOT="1",
                   S2A_PACK_PLAT="macos", S2A_TARGET="macos", S2A_SEED_AOC="1",
                   # 出包用途 = 用户包（10-29 维护者定口径，pack/jobs/archs.py）：管线归档 / 函数体只出这个包要装的设备那几对——
                   #   Mac 包 = 出包的这台 Mac；iPhone 包由 ipa.py 按目标设备传 S2A_PSO_ARCHS / S2A_PSO_OS，没传就报红、不猜（不按我方跑局机清单出）
                   S2A_PACK_PURPOSE="user",
                   # 出包链里其余缺省落在 ~/.s2arm/run 的缓存 / 锁 / 编好的小工具（直连缓存 6.5 GB、翻译缓存 5.3 GB…，10-26 Spare-host 0100C2500FC20000）一律改到私有目录：
                   #   玩家机上不出现 ~/.s2arm/run，清数据目录 / 删游戏能连它们一起删（守卫 tests/userpath_cache_test.py）
                   S2A_RUN_ROOT=str(self.work_root / "run"),
                   # 玩家作业不认我方开发机的规矩（10-26 侍魂晓在启动器里被 130 G 磁盘闸 + 独占局让路白卡 43 分钟）：不读 tools/machine.local
                   #   （记账、130 G 总量闸、签名团队、gate-host / 远端派活都是我方共享机器的设定），按用户机器出包（守卫 tests/userpath_cache_test.py ④）
                   S2A_MACHINE_NO_LOCAL="1", S2A_CPU_LEDGER="0",
                   # 玩家导入的客体着色器缓存（启动器社区资源面板拷进 <数据目录>/着色器缓存/<程序号>/，出包时自动并入；着色器线 docs/客体着色器缓存导入.md）
                   S2A_USER_SHCACHE=str(self.private / "着色器缓存"))
        import upgrade_budget
        if getattr(self, "operation", None) in upgrade_budget.OPERATIONS:   # S 级升级：按此刻核数 / 可用内存 / 有没有游戏在跑定路数，不吃满玩家的机器
            env.update(upgrade_budget.env(self))
        # 作业临时目录（游戏库结构 v3，出包修复线约定）：这款所在存放位置的 temp/<任务号>，与游戏数据同卷；作业结束由 main.run_request 删
        gid = self.r.get("gameId")
        if gid and self.r.get("library") and (self.library / ".s2arm-library.json").is_file():
            try:
                import layout
                tmp = layout.job_tmp(self, gid, self.task)
                tmp.mkdir(parents=True, exist_ok=True)
                env["S2A_JOB_TMP"] = str(tmp)
            except (Failure, OSError):
                pass
        # Git 只许在引擎目录里找仓库（出包各阶段用 git -C 引擎 取源码对象号）：不许往上认到外层仓库（10-23 走查：认到用户主目录的仓库）
        env["GIT_CEILING_DIRECTORIES"] = str(Path(self.engine).resolve().parent)
        env["S2A_PYTHON"] = os.sys.executable
        # 随启动器发版的补丁库（补丁包部署方案 v2，满屏补丁线 11-04；launchers/macos/build_app.py 只收真机验过的）：出包时交 pack/stage_rewrite.sh，
        #   验过的整份拷进产物根 patch.tsv、随资源包下发。起局不给（build.runtime_env 去掉）：宿主会往补丁根写 .applied，App 包内只读、写了破签名
        patches = Path(self.r.get("resources") or self.private) / "Patches"
        if (patches / "INDEX.tsv").is_file():
            env["S2A_PATCH_DIR"] = str(patches)
        runtime = Path(self.r.get("resources") or self.private) / "Runtime"
        if os.name == "nt" and (runtime / "s2arm_env.cmd").is_file():   # Windows 随包运行时（launchers/windows/bundle_runtime_win.py）
            env.update(win_runtime_env(runtime, self.engine, env.get("SystemRoot") or r"C:\Windows"))
            env["S2A_METAL_BIN"] = win_metal_dir(os.environ)
            return env
        # 3、4、5 都是「作业 PATH 只有 Runtime + 系统目录」的便携运行环境。4 起不再带 oead / GNU timeout；5 起不再带 .NET 运行时；
        # 旧 schema 3 仍走这条 PATH（构建侧 build_app 会因 schema / runtimeDeps 不符而重打，不在这里改 PATH）。
        schema = read(runtime / "versions.json", {}).get("runtimeSchema")
        if schema in (3, 4, 5):
            # 5 起 Runtime 不带 .NET 运行时（出包用的 C# 工具随引擎预编为 NativeAOT 单文件，见 launchers/macos/bundle_engine.py）：
            #   作业环境不再给 DOTNET_*；3 / 4 的旧 Runtime 还带，DOTNET 一并固定成自带的（#441 D-NDRE）：只设 DOTNET_ROOT / PATH 时，
            #   translator/dmsh_chain.py 等按「DOTNET → ~/.dotnet/dotnet → PATH」找，玩家自己在 ~/.dotnet 装了别的版本就抢在前面（同 ipa.py 的口径）
            if schema in (3, 4):
                env.update(
                    DOTNET=str(runtime / "dotnet" / "dotnet"),
                    DOTNET_ROOT=str(runtime / "dotnet"),
                    DOTNET_MULTILEVEL_LOOKUP="0",
                )
            env["S2A_PREBUILT_TOOLS"] = str(self.engine / "Binaries/Prepare")
            paths = [str(runtime / "python/bin"), str(runtime / "bin"), "/usr/bin", "/bin", "/usr/sbin", "/sbin"]
            # App 自带的 C 改写器（pack/rewriter.py 核源码指纹，与引擎不符时不用）：玩家机器零安装
            env["S2A_REWRITER_BUNDLED"] = str(runtime / "bin/s2a-rewrite")
            # 发版 App 自带已编好的 Mac 宿主（含 C#，S2A_CS=1）：RomFS 解码器或运行时 GLSLC 时不再现编（issue #8 根因：Runtime-only dotnet 编不出）
            from embedded import artifact
            # 只有源码树（没有 engine.json，artifact 回 None）才没有内置宿主、按需现编；发版清单里缺它或校验不符，artifact 抛「请重新安装完整应用」照常抛出
            #   （审计 P2-196：原先 except Exception 一律吞掉、改去现场编，红字指向「编译 Mac 宿主失败」，玩家按缺 SDK 去排查）
            # Intel 出包 App 按设计不带 Mac 宿主（游戏是 AArch64 原生执行）：清单没列这项是合法出包形态，optional 返回 None；列了但缺文件照旧报错。
            bundled_host = artifact(self, "S2ArmGame.app", optional=platform.machine() == "x86_64")
            if bundled_host:
                if not ((bundled_host / "Contents/MacOS/s2arm-host").is_file() and (bundled_host / "Contents/MacOS/libs2a_vaguard.dylib").is_file()):
                    raise Failure("应用内置引擎产物缺失，请重新安装完整应用。")
                env["S2A_BUNDLED_HOST"] = str(bundled_host / "Contents/MacOS/s2arm-host")
                self.host_app = bundled_host   # 辅助运行会执行这个 .app、系统随即登记它（#2024）：作业结束时注销（main._run_request）
                env["DM_GLSLC_HOST"] = str(bundled_host / "Contents/MacOS/s2arm-host")
                env["DM_OODLE_HOST"] = str(bundled_host / "Contents/MacOS/s2arm-host")  # LZ5 / OodleLZ 解码器（uepak.oodle_host_find，缺省回退 build/s2arm-host 在发版包里不存在）
        else:
            paths = [str(Path(os.sys.executable).parent), str(runtime / "bin"), "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin", env.get("PATH", "")]
        env["PATH"] = os.pathsep.join(dict.fromkeys(paths))
        return env

    def reap(self, process, out):
        try:
            left = reap_session(process.pid)
        except Exception as error:   # 收尾动作：自己出错不改变任务结果，记一行
            out.write(f"[s2a] 【黄】收本阶段工具会话的残留进程时出错：{self.redact(error)}\n")  # 非玩家句
            return
        if left:
            out.write(f"[s2a] 收掉本阶段工具会话里的残留进程 {len(left)} 个：{'、'.join(left[:8])}\n")  # 非玩家句
            out.flush()

    def stop_child(self, process, out, term_wait):
        """停本阶段工具：TERM 等 term_wait 秒，没退再 KILL 等 5 秒。KILL 之后还在 = 进程卡在系统内核里（等换页的磁盘读、等显卡驱动），
        信号要等它从内核回来才生效：记一条诊断、不抛异常。原先第二次等待超时直接抛 TimeoutExpired，盖掉了正在处理的取消 / 失败，
        这一局的诊断也没记上（#837：8 GB Mac 上客体内存文件后备换页卡死，玩家点停止后只看到一串 TimeoutExpired）"""
        self.signal_child(process, "TERM")
        try:
            process.wait(timeout=term_wait)
            return True
        except subprocess.TimeoutExpired:
            pass
        self.signal_child(process, "KILL")
        try:
            process.wait(timeout=5)
            return True
        except subprocess.TimeoutExpired:
            message = (f"{self.stage or '这一步'}的进程收到结束信号后 {term_wait + 5} 秒仍未退出：卡在系统内核里"
                       "（常见于内存不够时等换页的磁盘读，或等显卡驱动），系统处理完后会自行结束。")
            out.write(f"[s2a] 【红】{message}\n")  # 非玩家句
            out.flush()
            self.diagnostic(message, "child_unkillable")
            return False

    def signal_child(self, process, sig):
        if os.name == "nt":
            return winproc.signal_child(process)
        if process.poll() is not None:
            return
        # The repository's only process-group signal entry point validates both
        # the group and its live leader before sending a signal.
        helper = self.engine / "tools/safe_killpg.sh"
        if not helper.is_file():
            process.send_signal(getattr(signal, "SIG" + sig))
            return
        # Popen still owns this unreaped, live child, so its PID cannot have
        # been reused. macOS may replace argv[0] (notably framework Python),
        # and ps truncates long commands by default. Match the live leader's
        # prefix, rather than guessing a basename from the original argv.
        command = subprocess.run(["/bin/ps", "-ww", "-o", "command=", "-p", str(process.pid)],
                                 capture_output=True, text=True, timeout=5, check=False)
        marker = command.stdout.strip()[:64]
        if not marker:
            if process.poll() is not None:
                return
            raise Failure("无法确认本任务的子进程组，未发送信号。")
        result = subprocess.run(["/bin/bash", "-c", 'source "$1"; safe_killpg "$2" "$3" "$4"',
                                 "s2arm-cancel", str(helper), sig, str(process.pid), marker],
                                capture_output=True, text=True, timeout=5, check=False)
        if result.returncode and process.poll() is None:
            raise Failure("无法停止本任务进程组：" + self.redact(result.stderr))  # 非玩家句

    def stop(self):
        self.cancelled = True

    def run(self, args, log=None, env=None, cwd=None, on_line=None, timeout=None, ok_codes=(0,), until=None):
        # ok_codes：哪些退出码算这一步成功（如 refresh.sh 3 = 直连没出成、只出通用）；实际退出码留在 self.last_rc
        # until：可选，返回真表示「产物已经就绪」。这类工具（xcodebuild -downloadPlatform）把活干完后常在收尾步挂住，
        #   等它自己退出会让作业一直挂着；就绪即收（TERM 进程组），退出码不算数，判据是 until 的读数。最多每 2 秒问一次。
        self.check()
        args = list(map(str, args))
        self.last_log = Path(log or self.logs / (self.task + ".log"))
        self.last_log.parent.mkdir(parents=True, exist_ok=True)
        # Stage timeout (J10): a tool that hangs would otherwise hold the job (and the frontend's progress bar) until the user gives up. The
        # default comes from the request so the launcher and the CLI agree; 0 or absent means no timer, as before.
        limit = timeout if timeout is not None else self.r.get("stageTimeoutSeconds")
        deadline = time.monotonic() + float(limit) if limit else None
        run_env = env or self.env()
        self._activity_counters = {}  # A cache-only phase must not reuse a previous command's MB denominator.
        if getattr(self, "_batch_activity", None):
            self._batch_activity.clear()
        with self.last_log.open("a", encoding="utf-8") as out:
            if run_env.get("S2A_DEVELOPER_DIR"):
                out.write("S2Arm frontend toolchain: DEVELOPER_DIR=" + run_env["DEVELOPER_DIR"] + "\n")
                out.flush()
            # 进度文件的起读位置在起子进程之前量：起了再量，机器忙时子进程可能已经写完退出，起读位置落在它写的行之后，
            #   这一步的进度一行都发不出（11-15 满载跑作业层全量：失败工具的最后进度读成 None）
            pf = run_env.get("S2A_PROGRESS")
            ppos = os.path.getsize(pf) if pf and os.path.exists(pf) else 0
            try:
                spawn = winproc.popen if os.name == "nt" else subprocess.Popen
                p = spawn(args, cwd=cwd or self.engine, env=run_env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            except OSError as error:
                out.write(self.redact(error) + "\n")
                if not Path(cwd or self.engine).is_dir():
                    raise FileNotFoundError(errno.ENOENT, "working directory does not exist", str(cwd or self.engine)) from error
                if error.errno == errno.ENOENT and Path(cwd or self.engine).is_dir():
                    raise Failure("任务所需的工具无法启动：" + args[0], code="tool_missing") from error  # 非玩家句
                raise
            self.children.add(p)
            try:
                # Drain in a reader thread so cancellation is responsive even
                # when a compiler or the running game produces no output.
                reader_errors = []
                def drain():
                    try:
                        for raw in output_lines(p.stdout):
                            line = raw.decode(errors="replace")
                            out.write(line + "\n")
                            out.flush()
                            if on_line and line.strip():
                                on_line(line.rstrip())
                    except Exception as error:
                        reader_errors.append(error)
                reader = threading.Thread(target=drain, daemon=True)
                reader.start()
                # 阶段内进度（10-25 维护者「能看到进度么」）：阶段往 S2A_PROGRESS 追加「时刻 阶段 已完成 总数」（tools/progress.sh / progress.py），
                #   这里每秒读一次新增的行、发 progress 事件，前端阶段条按完成比例填、按本步实际速度外推剩余时间
                pnext = 0.0
                until_at = 0.0
                satisfied = False
                while p.poll() is None:
                    if pf and time.monotonic() >= pnext:
                        pnext = time.monotonic() + 1
                        ppos = self.stage_progress(pf, ppos)
                    if reader_errors:
                        raise reader_errors[0]
                    self.check()
                    if until and time.monotonic() >= until_at:
                        until_at = time.monotonic() + 2
                        try:
                            satisfied = bool(until())
                        except Exception:
                            satisfied = False   # 判据自己出错不影响工具运行，只当还没就绪
                        if satisfied:
                            out.write("[s2a] 产物已就绪，提前结束本阶段工具。\n")  # 非玩家句
                            out.flush()
                            break
                    if deadline and time.monotonic() > deadline:
                        # Say which stage ran out of time before the group is torn down, so the record and the frontend agree
                        message = f"阶段「{self.stage or '当前阶段'}」超过 {int(limit)} 秒未完成，已停止该阶段的进程组。"
                        self.diagnostic(message, "stage_timeout")
                        raise Failure(message, code="stage_timeout")
                    time.sleep(.1)
                if satisfied:
                    # 产物已就绪，工具只是收尾挂住：收掉进程组，退出码不作判据（调用方已用 until 的读数确认）
                    self.stop_child(p, out, term_wait=10)
                    self.reap(p, out)
                    if pf: ppos = self.stage_progress(pf, ppos, final=True)   # 收完进程组再补读一次：退出前写的最后几行也发出去（同下）
                    reader.join(timeout=5)
                    self.check()
                    self.last_rc = p.returncode
                    return self.last_log
                # 工具退出了：先收它会话里的残留进程（还占着输出管道的也在内，否则读线程等不到结束、整个任务判「输出未关闭」），再等读线程
                self.reap(p, out)
                # 子进程树已收完、不会再写：补读一次进度。循环只在子进程活着时每秒读，退出前那一秒写的行原先发不出去，界面停在旧读数
                #   （失败退出也读：错误页看得到停在哪一步）
                if pf: ppos = self.stage_progress(pf, ppos, final=True)
                reader.join(timeout=5)
                if reader.is_alive():
                    raise Failure("子进程输出未关闭，任务未确认完成。")
                if reader_errors:
                    raise reader_errors[0]
                self.check()
                # 被信号结束的子进程（宿主崩溃处理写完现场后按默认处置重抛，系统才出 .ips）Python 给负数：折回 shell 口径 128 + 信号，
                #   摘要、诊断、按退出码分类的地方照旧看到 139 / 134（runtime/src/crash.c crash_reraise）
                rc = 128 - p.returncode if p.returncode < 0 and os.name != "nt" else p.returncode
                self.last_rc = rc
                if rc not in ok_codes:
                    if self.operation == "build":
                        from errors import build_tool_failure
                        failure = build_tool_failure(args, getattr(self, "milestone", None), self.last_log)
                        self.diagnostic(str(failure), failure.code, exit_code=rc, internal=failure.internal, facts=failure.facts, advice=failure.advice)
                        raise failure
                    from errors import tool_reason
                    # D-Z2VA：超长原因留头（异常类型与消息）和尾（日志名），省中间。原先只留末 240 字，长红行只剩调用栈尾巴
                    reason = self.redact(tool_reason(self.last_log), limit=None)
                    if len(reason) > 240:
                        reason = reason[:160] + " … " + reason[-77:]
                    message = f"{self.stage or '这一步'}失败（退出码 {rc}）：{reason}"
                    self.diagnostic(message, "tool_exit", exit_code=rc)
                    raise Failure(message, code="tool_exit")
            finally:
                if p.poll() is None:
                    self.stop_child(p, out, term_wait=4)
                self.reap(p, out)   # 异常 / 取消 / 超时路径（正常路径上面已收过，这里收不到东西）
                p.stdout.close()
                self.children.discard(p)
                if getattr(self, "_batch_activity", None):
                    self._batch_activity.clear()
        return self.last_log

    def finish(self, state, message, result=None):
        self.cancelled = False  # Allow the terminal event after cancellation.
        self.job.update(state=state, ended=time.time(), message=message)
        if self.operation == "import" and "sources" in (result or {}) and self.r.get("partialResults") is True:
            self.job["sources"] = record_sources(result["sources"])
        if (result or {}).get("outputPath"):
            self.job["outputPath"] = result["outputPath"]
        # 启动器重开后从任务记录恢复完成卡：IPA 路径 / 装到的设备 / 推了几个文件原先只在结果事件里，任务记录不存，
        #   重开后完成卡读不到包（画面模式「未知」、IPA / 资源包两行没了，11-01 测试开发者截图）。字段名同启动器 TaskRecord
        for src, dst in (("ipa", "ipaPath"), ("device", "device"), ("packFiles", "packFiles")):
            if (result or {}).get(src) not in (None, ""):
                self.job[dst] = result[src]
        try:
            self.emit("result", message, state="succeeded" if state == "completed" else state,
                      result=result or {}, artifactRef=(result or {}).get("outputPath"), diagnostics=self.job["diagnostics"])
            if self.recorded:
                atomic(self.jobpath, self.job)
        except (Failure, OSError) as error:
            # The job is over either way; what must not happen is the caller seeing success while nothing was recorded. main.py turns
            # journal_broken into a non-zero exit, and this line is the only place a user can read why (J10).
            self.journal_broken = True
            self.job["journalBroken"] = str(error)[:300]
            print(f"[s2a] 【红】任务记录写不进磁盘，这次不当作完成：{error}", file=sys.stderr, flush=True)


# ---- Journal readers and the job control plane (J10 / #186) ---------------------------------------------------------------------------------
# Everything below only reads the journal the worker already writes, or signals a worker it recorded. The Mac launcher and the CLI reach a job the
# same way: a task id plus the private root. No daemon, no second state store, and a job that never wrote a terminal event is reported as such
# instead of being mistaken for a finished one.
TERMINAL_STATES = ("completed", "failed", "cancelled", "succeeded", "paused")   # paused：出包编排磁盘满停下（剃刀 H13）


def request_path(private, task):
    """The request a task was started with, kept beside its journal so a killed attempt can be run again."""
    job, _ = job_files(private, task)
    return job.with_suffix(".request.json")


def job_files(private, task):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", str(task)):
        raise Failure("无效的任务标识。", code="job_unknown")
    dirs = _job_dirs(private)
    jobs = next((d for d in dirs if (d / (task + ".json")).is_file()), dirs[0])
    return jobs / (task + ".json"), jobs / (task + ".events.jsonl")


def journal(private, task):
    path, _ = job_files(private, task)
    record = read(path)
    if not isinstance(record, dict):
        raise Failure("这条任务的记录不存在。", code="job_unknown")
    return record


def read_events(private, task, since=0, limit=None):
    _, path = job_files(private, task)
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        lines = []
    out = []
    for number, line in enumerate(lines):
        if number < since or not line.strip():
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue   # a line cut short by a kill is not an event; the journal still has the last whole one
        if limit and len(out) >= limit:
            break
    return out


def pid_start(pid):
    """Process start time as ps prints it: a reused pid has a different one, so a journal cannot signal a stranger."""
    if os.name == "nt":
        return winproc.pid_start(pid)
    try:
        result = subprocess.run(["/bin/ps", "-o", "lstart=", "-p", str(int(pid))], capture_output=True, text=True, timeout=5, check=False)
    except OSError:
        # 沙箱/受限运行时可能连 /bin/ps 都不许 exec：退化为空串，辨陌生人检查自动跳过（构建期 bundle-engine 就在这类环境里）。
        return ""
    return result.stdout.strip()


WIN_RUNTIME_PATH = ["python", r"python\Scripts", r"git\usr\bin", "zstd"]   # 同 bundle_runtime_win.env_cmd 的 PATH（tests 核两处一致）


def win_metal_dir(environ):   # 应用布局：只在 Windows 上调（env() 里 os.name == "nt" 那支），Windows 的作业层就在引擎快照的 pack/jobs 里，__file__ 往上两级是引擎根
    """Windows 出包用的 Metal 工具目录：在作业进程（还带着玩家的 S2A_METAL_BIN 与 s2arm_env.cmd 存下的 S2A_USER_PATH）按 oneclick_win.metal_exe
    选一次，显式交给子进程——env() 剥掉 S2A_*、PATH 换成只含随包工具，子进程自己选只会落到官方默认目录（11-09 Windows 出包机实测 #137：
    S2A_METAL_BIN 指自定义目录 / 只设 PATH / 指不存在的目录，三种都悄悄用了 C:\\Program Files 那份）。显式配置无效时原样传下去，由出包预检报缺"""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from pack.orch.oneclick_win import metal_exe
    return str(metal_exe(environ).parent)


def win_runtime_env(runtime, engine, system_root):
    """Windows 出包作业的环境（同随包 s2arm_env.cmd）：只用随包 Python / bash / zstd + 系统目录（不带 .NET：C# 件是 NativeAOT 单文件），预编件在引擎 Binaries/Prepare（同 Mac）"""
    import ntpath
    rt = str(runtime)
    paths = [ntpath.join(rt, d) for d in WIN_RUNTIME_PATH] + [ntpath.join(system_root, "system32"), system_root,
                                                               ntpath.join(system_root, r"System32\WindowsPowerShell\v1.0")]
    return dict(PATH=";".join(paths),
                S2A_BASH=ntpath.join(rt, r"git\usr\bin\bash.exe"), PYTHONHOME=ntpath.join(rt, "python"), PYTHONUTF8="1",
                LC_ALL="C.UTF-8",   # 机器上设了 LANG=C / en_US 时 cygpath -m 在第一个中文字符处截断路径（#85：D:/浏览器下载/… → D:/）；与 MSYS 无语言变量时的缺省相同
                S2A_PREBUILT_TOOLS=ntpath.join(str(engine), r"Binaries\Prepare"),
                PYTHONPATH=ntpath.join(str(engine), r"pack\orch\site"))   # 子 Python 进程的长路径处理（pack/orch/winlong.py）


def pid_alive(pid):
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    if os.name == "nt":
        return winproc.alive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True   # someone else's process: alive, and cancel refuses it later
    return True


def describe(private, task):
    """What a restart may believe about this task: state, last stage, attempts, and whether the worker is still there."""
    record = journal(private, task)
    pid = record.get("pid")
    running = pid_alive(pid)
    terminal = record.get("state") in TERMINAL_STATES or bool(record.get("ended"))
    events = read_events(private, task)
    return dict(job=record, state=record.get("state"), operation=record.get("operation"), stage=record.get("stage") or "",
                attempts=record.get("attempts") or {},
                attemptNo=record.get("attemptNo") or 1, pid=pid, running=running,
                terminal=terminal, interrupted=(not terminal and not running), events=len(events),
                lastEvent=events[-1] if events else None, canResume=not terminal)


def _scan_job_record(path):
    """Missing journals are normal while queued; bad journals stay isolated."""
    missing = object()
    try:
        if path.is_symlink():
            return None, "linked_record"
        record = read(path, missing)
    except (OSError, ValueError) as error:
        return None, type(error).__name__
    if record is missing:
        return None, None
    if not isinstance(record, dict) or record.get("id") != path.stem:
        return None, "invalid_record_identity"
    return record, None


# 任务记录保留（游戏库结构 P3 收紧；原规则「结束超过 14 天的清掉、最新 50 条总留着」让一台维护者机器 14 天攒到 45207 条——前端每次刷新都起
# catalog / 空间统计等短作业，一天上千条，启动器启动时整目录读一遍、诊断包整目录打包都被拖慢）：
#   - 总量上限 KEEP_TOTAL 条 + 时间上限 keep_days 天，取更严的：超出任一条就清。2000 条约是正常玩家半年的量、高频刷新下也够看最近一两天，
#     记录每条几 KB，整目录几十 MB 以内，启动器读一遍在一秒内；
#   - 每类操作（operation）至少留最近 KEEP_PER_OPERATION 条成功的、KEEP_FAILED_PER_OPERATION 条失败 / 取消的，不受上面两条限制：
#     刷新类作业刷屏时，最近的出包 / 装机 / 存档记录不能被挤掉；失败的多留，「复制错误摘要」、s2armctl errors / report、反馈问题都要往回翻失败记录；
#   - 没写终态的（作业可能还在写）一律不动。
#   - 各任务在日志根下的文件（<任务>.log、<任务>-install.log、<任务>.progress.tsv、<任务>/ 等）跟着任务记录一起清，任务记录已不在的孤儿日志同样清
#     （11-13 维护者机器：任务记录轮转正常、留 703 条，日志根却攒了 12296 个 <任务>.log、144 MB 从没清过；诊断包整目录打包超过上传服务 1000 个文件上限）。
#   11-13 总量上限 2000 → 300：维护者机器 14 天内 703 条全在时间窗里、一条不清；300 条够看最近几天，诊断包另只带最近 DIAG_JOBS 条（library.diagnostics）
KEEP_TOTAL = 300
KEEP_DAYS = 14
KEEP_PER_OPERATION = 20
KEEP_FAILED_PER_OPERATION = 50


TASK_LOG = re.compile(r"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})(?:[.-].*)?", re.I)   # 日志根下属于某个任务的文件 / 目录名 → 任务号
TASK_LOG_GRACE = 86400   # 任务号对不上记录的日志：改动超过一天才清（任务刚起、记录还没写的那一刻不误删）


def task_of_log(name):
    m = TASK_LOG.fullmatch(name)
    return m.group(1).lower() if m else None


def prune_jobs(private, keep_days=KEEP_DAYS, keep_count=KEEP_TOTAL, dry_run=False, *, per_operation=KEEP_PER_OPERATION,
               failed_per_operation=KEEP_FAILED_PER_OPERATION, jobs=None, logs=None):
    """Keep the job store bounded (J10, rules above). Only the three files this module owns per task are removed; anything else in the
    directory is left alone. `jobs` names the directory directly (layout.adopt_library_logs prunes the old location before moving it).
    `logs`：日志根（缺省 jobs 的上一级）——各任务的日志跟着清：任务被清了、或任务记录早已不在（孤儿，改动超过 TASK_LOG_GRACE）的都删"""
    jobs = Path(jobs) if jobs is not None else jobs_dir(private)
    if not jobs.is_dir():
        return dict(removed=[], kept=0, skippedRunning=0, bytes=0, invalidRecords=[])
    by_task = {}
    try:
        listed = list(jobs.iterdir())
    except FileNotFoundError:   # 并发的作业刚把这个目录收走（layout.adopt_library_logs）
        return dict(removed=[], kept=0, skippedRunning=0, bytes=0, invalidRecords=[])
    for path in listed:
        if path.is_symlink() or not path.is_file():
            continue
        if path.suffix not in (".json", ".jsonl"):
            continue
        name = path.name
        task = name[:-len(".request.json")] if name.endswith(".request.json") else name[:-len(".events.jsonl")] if name.endswith(".events.jsonl") else name[:-len(".json")]
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", task):
            continue
        by_task.setdefault(task, []).append(path)
    cutoff = time.time() - float(keep_days) * 86400 if keep_days else None

    records, invalid = {}, []
    for task in by_task:
        record, reason = _scan_job_record(jobs / (task + ".json"))
        if reason:
            invalid.append(dict(taskId=task, reason=reason))
        else:
            records[task] = record or {}  # No journal yet: retain the running/request-only task.

    finished = {t for t, r in records.items() if r.get("state") in TERMINAL_STATES or bool(r.get("ended"))}
    running = [t for t in records if t not in finished]   # counted before anything is deleted: a removed record must not look like a live one
    def mtime(p):   # 列完目录后被并发的作业搬走的文件：当作不在（不再整个作业失败）
        try:
            return p.stat().st_mtime
        except FileNotFoundError:
            return 0.0
    newest = {t: max(mtime(p) for p in by_task[t]) for t in finished}
    finished_tasks = sorted(finished, key=lambda t: newest[t], reverse=True)
    floor, seen = set(), {}
    for task in finished_tasks:
        failed = records[task].get("state") in ("failed", "cancelled")
        key = (records[task].get("operation") or "", failed)
        seen[key] = seen.get(key, 0) + 1
        if seen[key] <= (failed_per_operation if failed else per_operation):
            floor.add(task)
    limit = max(0, int(keep_count)) if keep_count is not None else None
    removed, freed, kept = [], 0, 0
    for rank, task in enumerate(finished_tasks):
        expired = (cutoff is not None and newest[task] < cutoff) or (limit is not None and rank >= limit)
        if task in floor or not expired:
            kept += 1
            continue
        files = by_task[task]
        size = sum(p.stat().st_size for p in files if p.exists())
        if not dry_run:
            for p in files:
                p.unlink(missing_ok=True)
        removed.append(task)
        freed += size
    logs = Path(logs) if logs is not None else jobs.parent
    keep_ids = {t.lower() for t in by_task} - {t.lower() for t in removed}
    logs_removed = 0
    try:
        listed_logs = list(logs.iterdir()) if logs.is_dir() and logs != jobs else []
    except FileNotFoundError:
        listed_logs = []
    for path in listed_logs:
        owner = task_of_log(path.name)
        if not owner or owner in keep_ids or path == jobs:
            continue
        try:
            info = path.lstat()
        except FileNotFoundError:
            continue
        if owner not in {t.lower() for t in removed} and time.time() - info.st_mtime < TASK_LOG_GRACE:
            continue   # 对不上记录、又是刚写的：任务可能刚起
        size = info.st_size if not path.is_dir() or path.is_symlink() else sum(p.stat().st_size for p in path.rglob("*") if p.is_file() and not p.is_symlink())
        if not dry_run:
            try:
                shutil.rmtree(path) if path.is_dir() and not path.is_symlink() else path.unlink(missing_ok=True)
            except OSError:
                continue
        logs_removed += 1
        freed += size
    return dict(removed=removed, kept=kept + len(running) + len(invalid), skippedRunning=len(running),
                bytes=freed, invalidRecords=invalid, logsRemoved=logs_removed)


def status(private):
    """One look at the whole job store: how many are running vs finished, how big it is, and which task last moved.
    The store is every directory a record can be read from (_job_dirs: the current one plus the private fallbacks not yet adopted)."""
    dirs = [d for d in _job_dirs(private) if d.is_dir()]
    result = dict(running=0, finished=0, interrupted=0, bytes=0, newest=None, oldest=None, invalidRecords=[])
    newest = oldest = None
    for path in (p for d in dirs for p in d.iterdir()):
        if path.is_symlink() or not path.is_file():
            continue
        result["bytes"] += path.stat().st_size
        stamp = path.stat().st_mtime
        if newest is None or stamp > newest[0]:
            newest = (stamp, path.name)
        if oldest is None or stamp < oldest[0]:
            oldest = (stamp, path.name)
    import layout
    for record_path in (p for d in dirs for p in layout.entries(d, "*.json")):   # ._<任务>.json（数据目录在 exFAT 上搬过）不是坏记录（#1010）
        if record_path.name.endswith(".request.json") or record_path.is_symlink():
            continue
        record, reason = _scan_job_record(record_path)
        if reason:
            result["invalidRecords"].append(dict(taskId=record_path.stem, reason=reason))
            continue
        if record is None:
            continue  # Another process removed the record after directory enumeration.
        if record.get("state") in TERMINAL_STATES or record.get("ended"):
            result["finished"] += 1
        elif pid_alive(record.get("pid")):
            result["running"] += 1
        else:
            result["interrupted"] += 1
    if newest:
        result["newest"] = newest[1]
    if oldest:
        result["oldest"] = oldest[1]
    return result


def reap_session(sid, grace=2.0):
    """收掉会话 sid（本任务工具的会话：run() 用 start_new_session 起，会话号 = 工具 pid）里还活着的进程，返回收掉的进程名。
    工具退出后组长已不在，孙进程照跑：10-26 斯普拉遁3 Spare-host 出包，dl_gen 超时只杀了 metal-tt，air-tt / applegpu-nt 过继给 launchd 占 2 核 16 分钟，
    出包失败、启动器退出后还在编。有意活得比任务久的后台（宽屏自动出表）自立会话，不在这里面。会话还有成员时 sid 不会被复用，逐个按 pid 发信号、发前再核会话号"""
    if os.name == "nt":
        return winproc.reap_session(sid)
    def members():
        out = subprocess.run(["/bin/ps", "-A", "-o", "pid=,comm="], capture_output=True, text=True, timeout=10, check=False).stdout
        found = []
        for row in out.splitlines():
            pid, _, comm = row.strip().partition(" ")
            if not pid.isdigit() or int(pid) == os.getpid():
                continue
            try:
                if os.getsid(int(pid)) == sid:
                    found.append((int(pid), os.path.basename(comm.strip())))
            except OSError:
                pass
        return found
    def send(sig):
        for pid, _ in members():
            with contextlib.suppress(OSError):
                if os.getsid(pid) == sid:
                    os.kill(pid, sig)
    first = members()
    if not first:
        return []
    send(signal.SIGTERM)
    end = time.monotonic() + grace
    while members() and time.monotonic() < end:
        time.sleep(.1)
    send(signal.SIGKILL)
    return [name for _, name in first]


def trace(c, text):
    """作业过程记一行进本任务日志 <任务>-decisions.log（同 install.note，进诊断包）：等锁、各款改写、组容器的起止（11-17 #1450）"""
    line = time.strftime("%H:%M:%S ") + text
    try:
        c.logs.mkdir(parents=True, exist_ok=True)
        with open(c.logs / f"{c.task}-decisions.log", "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except (OSError, AttributeError, TypeError):
        pass
    print("[s2a] " + text, file=sys.stderr, flush=True)  # 非玩家句


PREEMPT_WAIT_S = 120   # 前台作业等被取消的后台自动作业退出（放锁）的上限：它在收尾（停掉子进程、写终态）通常几秒


def preempt_jobs(c, operations, wait=PREEMPT_WAIT_S, say=None):
    """前台作业让路的反面（11-17 #1450）：取消正在跑的这几类后台自动作业（main.AUTO_HEAVY：installUpdatePrepare / sgradeMac），等它们真退出、放了锁
    再往下走。后台按设计降到最低优先级（nice 19 + 后台档），前台若排在它拿着的程序锁后面，就是前台等一个故意让着前台的作业（优先级倒置）：
    0.5.2 实例里「更新 iPhone」等「准备更新」改写阿尔宙斯，10 分钟没等到、玩家取消、下次从头来。取消走玩家在界面上点取消同一条路（cancel：
    核过 pid 仍是那个作业才发 SIGTERM），被取消的作业照常收尾；它做到一半的活由前台按正常优先级重做（各步按记号判做没做完，见 rewrite_refresh）。
    → 取消了的任务号。say(句子)：记进本任务日志（等多久、取消了谁）"""
    import layout
    say = say or (lambda text: None)
    hits = []
    for d in _job_dirs(c.private):
        if not d.is_dir():
            continue
        for path in layout.entries(d, "*.json"):
            if path.name.endswith(".request.json"):
                continue
            record, reason = _scan_job_record(path)
            if reason or not isinstance(record, dict) or record.get("id") == c.task or record.get("operation") not in operations:
                continue
            if record.get("state") in TERMINAL_STATES or record.get("ended") or not pid_alive(record.get("pid")):
                continue
            hits.append(record)
    done = []
    for r in hits:
        try:
            got = cancel(c.private, r["id"])
        except Failure as e:   # pid 已复用 / 不是本用户的：不碰，照常往下（最坏退回原来的排队）
            say(f"没取消后台任务 {r.get('operation')}（{r['id']}）：{e}")  # 非玩家句
            continue
        if got.get("cancelled"):
            done.append(r)
            say(f"前台要用手机和程序：取消正在跑的后台任务 {r.get('operation')}（{r['id']}，进程 {got.get('pid')}）")  # 非玩家句
    end = time.monotonic() + wait
    left = list(done)
    while left and time.monotonic() < end:
        c.check()
        left = [r for r in left if pid_alive(r.get("pid")) and (not r.get("pidStart") or pid_start(r["pid"]) == r["pidStart"])]
        if left:
            time.sleep(0.2)
    if left:
        say("后台任务 " + "、".join(str(r["id"]) for r in left) + f" 取消后 {wait:g} 秒还没退出：照常排队等锁")  # 非玩家句
    elif done:
        say("后台任务已退出、锁已放")  # 非玩家句
    return [r["id"] for r in done]


def cancel(private, task, sig="TERM"):
    """Signal the worker the journal recorded, after checking that the pid is still that worker (a reused pid is refused, not killed)."""
    record = journal(private, task)
    if record.get("state") in TERMINAL_STATES or record.get("ended"):
        return dict(cancelled=False, reason="任务已结束，无需取消。", state=record.get("state"))
    pid = record.get("pid")
    if os.name == "nt":
        try:
            winproc.cancel_verified(pid, record.get("pidStart"), sig)
        except ProcessLookupError:
            return dict(cancelled=False, reason="任务进程已不在，记录停在未收尾状态。", state=record.get("state"), interrupted=True)
        return dict(cancelled=True, pid=int(pid), signal=sig)
    if not pid_alive(pid):
        return dict(cancelled=False, reason="任务进程已不在，记录停在未收尾状态。", state=record.get("state"), interrupted=True)
    started = record.get("pidStart")
    if started:
        if pid_start(pid) != started:
            raise Failure("记录的进程号已不是这个任务，未发送信号。", code="job_pid_reused")
    else:
        command = subprocess.run(["/bin/ps", "-ww", "-o", "command=", "-p", str(pid)], capture_output=True, text=True, timeout=5, check=False)
        if "jobs/main.py" not in command.stdout:
            raise Failure("记录的进程号已不是这个任务，未发送信号。", code="job_pid_reused")
    try:
        os.kill(int(pid), getattr(signal, "SIG" + sig))
    except PermissionError:
        raise Failure("这个进程不属于当前用户，未发送信号。", code="job_not_ours") from None
    except ProcessLookupError:
        return dict(cancelled=False, reason="任务进程已不在，记录停在未收尾状态。", state=record.get("state"), interrupted=True)
    return dict(cancelled=True, pid=int(pid), signal=sig)
