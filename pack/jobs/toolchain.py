"""S2Arm checkout/tool discovery; checks never build, install or download anything."""
from __future__ import annotations
from pathlib import Path
import hashlib
import json
import os
import platform
import re
import tempfile
import threading
import time
import subprocess
import sys
from core import Failure, atomic, digest, read, tree_hash


def default_app_dirs():
    """自动找 Xcode 的目录：/Applications、~/Applications（xcode-select 指的那份由 installed_xcodes 另加）。与前端 XcodeLocator.searchDirs 同一组，两处改一处要同步改另一处。
    不翻 /Volumes：列外接卷会弹系统隐私窗「想访问可移除卷上的文件」，6aaab23d4（11-18）起每建一个任务都列一遍，接着外接盘的玩家一开启动器就弹、
    无人值守时作业挂在 opendir（0.6.2 验收卡在这里）。Xcode 装在外接盘上的玩家在设置页「选择 Xcode…」自己选（launch-settings.json 的 xcodeApp，玩家自选优先）"""
    return ("/Applications", str(Path.home() / "Applications"))


def installed_xcodes(apps_dirs=None, select="/usr/bin/xcode-select"):
    """没给 apps_dirs 用 default_app_dirs（不含外接卷）；规则见 _scan_xcodes"""
    return _scan_xcodes(default_app_dirs() if apps_dirs is None else apps_dirs, select)


def _scan_xcodes(apps_dirs, select):
    """装着的完整 Xcode（com.apple.dt.Xcode 且有 usr/bin/xcodebuild），按（版本号, 构建号）认、去重，版本新的在前：[(版本元组, 版本串, 构建号, Developer 目录)]。
    11-07 gate-host：Xcode-27.1.app 是指向 Xcode-27.app 的软链，而 Xcode-27.app 已被 App Store 原地升到 27.1——按路径名认就成了「27.0 + 27.1 两套」，
    其实是同一份。所以路径名不参与判定：版本读 Contents/Info.plist（CFBundleShortVersionString），构建号读 Contents/version.plist（ProductBuildVersion，
    同 xcodebuild -version 的 Build version，不起进程）；同版本同构建号（软链 / 改名 / 拷贝）只留第一个见到的路径（xcode-select 指的那份排最前）。
    构建号读不到的按实际路径（解软链）去重。门禁 / 工具链列 Xcode 一律走这里（tests/shellui_ios_build_test.sh 等）"""
    import plistlib
    apps = []
    try:
        sel = subprocess.run([select, "-p"], capture_output=True, text=True, timeout=10).stdout.strip()
        if sel.endswith(".app/Contents/Developer"):
            apps.append(Path(sel[: -len("/Contents/Developer")]))
    except (OSError, subprocess.TimeoutExpired):
        pass
    for d in apps_dirs:
        try:
            apps += sorted(p for p in Path(d).iterdir() if p.name.startswith("Xcode") and p.suffix == ".app")
        except OSError:
            continue
    out, seen = [], set()
    for app in apps:
        try:
            info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
        except (OSError, ValueError):
            continue
        dev = app / "Contents/Developer"
        if info.get("CFBundleIdentifier") != "com.apple.dt.Xcode" or not (dev / "usr/bin/xcodebuild").is_file():
            continue
        vs = str(info.get("CFBundleShortVersionString", "0"))
        v = tuple(int(x) for x in re.findall(r"\d+", vs)[:3])
        try:
            build = str(plistlib.loads((app / "Contents/version.plist").read_bytes()).get("ProductBuildVersion", ""))
        except (OSError, ValueError):
            build = ""
        key = (v, build) if build else (v, os.path.realpath(app))
        if key in seen:
            continue
        seen.add(key); out.append((v, vs, build, str(dev)))
    out.sort(key=lambda x: x[0], reverse=True)   # 稳定排序：同版本不同构建号保持见到的先后
    return out


def detect_xcode(apps_dirs=None, select="/usr/bin/xcode-select"):
    """装着的 Xcode 里版本最新的一个（含 xcode-select 指的那个）的 Contents/Developer；没有返回 None。规则见 installed_xcodes（按版本号 + 构建号认，不按路径名）。
    与前端 Store.detectXcode 同一规则（打 iPhone 包的着色器按目标系统大版本出，老 SDK 出不了新系统的）；两处改一处要同步改另一处。
    tools/toolchain.sh 没指定 Xcode 时也调这里（11-07：壳也要最新 SDK 链接，旧 SDK 的壳在 iOS 新机型上缩小显示），select / apps_dirs 由它传（单测换假的）"""
    xs = installed_xcodes(apps_dirs, select)
    return xs[0][3] if xs else None


def probe(c, args, timeout=15):
    try:
        p = subprocess.run(list(map(str, args)), env=c.env(), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False)
        return p.returncode == 0, p.stdout.strip()[-2000:]
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, c.redact(e)


def _input_probe(exe, c):
    """nm 一次，读导出符号：返回 (supported, reason)；宿主读不了返回 None（不是「不支持」，是不确定）。"""
    try:
        result = subprocess.run(["nm", "-gU", str(exe)], env=c.env(), stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0 or not result.stdout.strip():
        return None
    if re.search(r"\b_s2a_vpad_read\b", result.stdout):
        return True, "引擎已导出输入接口 s2a_vpad_read"
    return False, "该引擎的 Mac 宿主没有导出 s2a_vpad_read（旧引擎），映射可以保存但不会生效"


def _host_stamp(exe):
    """(大小, mtime) 作为缓存键；st_mtime_ns 比 st_mtime 细，避免同一秒内换宿主读旧结论。"""
    try:
        st = exe.stat()
    except OSError:
        return None
    return st.st_size, st.st_mtime_ns


def _cached_probe(c, exe):
    """按（路径, 大小, mtime）复用上一次的判定；缓存只在 c.private 下，不进游戏库、不进 PR。"""
    exe = Path(exe)
    stamp = _host_stamp(exe)
    if stamp is None:
        return None
    cache = c.private / "tools" / "mac-host-cache"
    key = hashlib.sha256(f"{exe}\0{stamp[0]}\0{stamp[1]}".encode()).hexdigest()[:24]
    entry = read(cache / f"{key}.json", {})
    if entry.get("path") == str(exe) and entry.get("size") == stamp[0] and entry.get("mtime") == stamp[1]:
        reason = entry.get("reason")
        if isinstance(reason, str) and reason:
            return bool(entry.get("supported")), reason
    verdict = _input_probe(exe, c)
    if verdict is not None:
        atomic(cache / f"{key}.json", dict(schema=1, path=str(exe), size=stamp[0], mtime=stamp[1],
                                           supported=verdict[0], reason=verdict[1]))
    return verdict


def input_interface(c, executable=None):
    """引擎是否真的提供 S2A_INPUT_CONFIG 读取接口（F06：旧引擎缺接口时提示要准，不能一律报支持）。

    判据 = 宿主二进制里有没有输入 ABI 的导出符号 s2a_vpad_read（services/InputPump.cs 与
    macos/front_mac.m 都经它读输入）。有 = 该引擎能读映射文件；没有 = 旧引擎，
    映射可以保存，但游戏会用引擎自己的默认操作。
    返回 (supported: bool, reason: str)；找不到可检查的宿主时保守报不支持，让 UI 说准话。
    nm 一次约 0.6 秒，而 catalog 在前端每次刷新都会跑：判定按（路径, 大小, mtime）缓存在
    c.private/tools/mac-host-cache/ 下，宿主没变就复用（维护者 复验第 3 条）。
    """
    if executable is not None:
        # A prepared game can still contain an older host than the engine cache.
        # Never fall back to a different executable when confirming its mapping.
        return _cached_probe(c, Path(executable)) or (False, "无法确认此游戏运行包是否支持输入映射接口")
    candidates = []
    if c.r.get("engineBundle"):
        from embedded import artifact
        try:
            bundled = artifact(c, "S2ArmGame.app")
        except (Failure, OSError):
            bundled = None
        if bundled:
            candidates.append(Path(bundled) / "Contents/MacOS/s2arm-host")
    cached = c.private / "tools/mac-host"
    if cached.is_dir():
        candidates += sorted(cached.glob("*/S2ArmGame.app/Contents/MacOS/s2arm-host"), reverse=True)
    candidates.append(c.engine / "build/s2arm-host")
    unreadable = False
    for exe in candidates:
        if not exe.is_file():
            continue
        verdict = _cached_probe(c, exe)
        if verdict is not None:
            return verdict
        unreadable = True   # 找到了宿主但 nm 读不出符号（架构不符 / 权限 / 超时）：继续看下一个候选
    if unreadable:
        return False, "找到了 Mac 宿主但读不出导出符号（架构、权限或超时），无法确认该引擎是否支持输入接口"
    return False, "没有找到可检查的 Mac 宿主，无法确认该引擎是否支持输入接口"


READER_RECIPE = "aot1"  # 读取器发布方式（NativeAOT 单文件）的配方号：改动发布命令时升它，缓存键跟着换（键只算源码内容，不含本文件的改动）


def reader_key(c):
    c.require_source()
    return hashlib.sha256(
        (tree_hash(c.engine, ["pack/nx_unpack", "compat/packages"]) + READER_RECIPE).encode()
    ).hexdigest()[:24]


def reader(c):
    if os.name == "nt":   # Windows 发版只带预编件（launchers/windows/bundle_prepare_win.py）：引擎 Binaries/Prepare/nx_unpack，源码身份同 oneclick_win 才用
        sys.path.insert(0, str(c.engine))
        from pack.orch import oneclick_win
        d = c.engine / "Binaries/Prepare/nx_unpack"
        try:
            same = (d / ".src_digest").read_text(encoding="utf-8").strip() == oneclick_win.nx_unpack_id()
        except OSError:
            same = False
        # nx_unpack.exe 是 NativeAOT 单文件（v0.5.0 Windows 去 .NET，bundle_prepare_win.py），调用方直接执行，作业环境不带 .NET
        #   维护者 11-17：校验不过不再整个拒绝——源码身份对不上但读取工具在，就黄字提示后照常用（读取器只读原件、只产出这款的产物）；工具根本不在才停
        if (d / "nx_unpack.exe").is_file():
            if not same:
                c.diagnostic("S2Arm 读取工具与引擎版本不符，这次照常使用；解包出错时请重新安装完整的 S2Arm。", code="reader_unavailable", severity="warning")
            return d / "nx_unpack.exe"
        # D-9G73（windows/0.6.7）：安装包里一定带着它，装好后不见了就是杀毒软件当木马隔离了（玩家原话「被防火墙拦截，提示特洛伊木马」）；
        #   归 app_control_blocked，与随包预编件缺（errors.missing_prebuilt）同一归因，玩家拿到「去保护历史还原 / 放行」而不是「重新安装」
        raise Failure("S2Arm 自带的读取工具 nx_unpack.exe 不见了：是杀毒软件把它当成病毒隔离了。到「Windows 安全中心 → 保护历史记录」里把它还原并允许，"
                      "把 S2Arm 安装文件夹加进信任后再点一次。", code="app_control_blocked")
    from embedded import artifact
    bundled = artifact(c, "Reader")
    if bundled:
        return (
            bundled / "nx_unpack"
        )  # 发版件：NativeAOT 单文件（bundle_engine.py 发版时照同一配方编），直接执行、不再要 .NET 运行时
    folder = c.private / "tools" / reader_key(c)
    path = folder / "nx_unpack"
    stamp = read(folder / "ready.json", {})
    if not path.is_file() or stamp.get("sha256") != digest(path):
        raise Failure("S2Arm 读取工具尚未准备或源码已变化，请点击准备引擎工具。", code="reader_unavailable")
    return path


def prepare(c):
    c.require_source()
    from embedded import artifact
    if artifact(c, "Reader"):
        return {"reader": str(reader(c))}
    c.begin("编译 S2Arm 内容读取工具", 1, 1)
    folder = c.private / "tools" / reader_key(c)
    folder.mkdir(parents=True, exist_ok=True)
    # NativeAOT 单文件（同发版件 Binaries/Reader 的形态，-r 按本机架构）：读取器在玩家机 / 源码树都是自包含可执行文件，作业环境不再需要 .NET 运行时
    rid = "osx-arm64" if platform.machine() == "arm64" else "osx-x64"
    c.run(
        [
            "dotnet",
            "publish",
            c.engine / "pack/nx_unpack/nx_unpack.csproj",
            "-c",
            "Release",
            "-r",
            rid,
            "-o",
            folder,
            "--nologo",
            "-p:PublishAot=true",
            "-p:UseSharedCompilation=false",
            "-p:BaseIntermediateOutputPath=" + str(folder / "obj") + "/",
        ],
        c.logs / (c.task + "-reader.log"),
    )
    path = folder / "nx_unpack"
    if not path.is_file():
        raise Failure("读取工具未生成完整产物。")
    atomic(folder / "ready.json", dict(schema=1, sha256=digest(path)))
    return {"reader": str(path)}


def preflight_windows(c):
    """Windows：统一开始前检查 winenv（出包件 + Impactor + Metal / Apple 设备服务 / iCloud；WINDOWS_ENV.md），同一份清单格式"""
    import json, tempfile
    res = c.r.get("resources")
    req = {k: v for k, v in (("shell", str(Path(res) / "Shell") if res else None),
                             ("targetDir", str(c.library) if getattr(c, "library", None) else None)) if v}
    env = c.env()   # 出包件这几项按作业环境判（预编件 / 随包运行时 / 字体），同出包时一样
    fonts = c.fonts_dir()
    if fonts:
        env["S2A_FONTS"] = str(fonts)
    with tempfile.TemporaryDirectory(dir=c.private) as t:
        rq = Path(t, "winenv.json"); rq.write_text(json.dumps(req), encoding="utf-8")
        r = subprocess.run([sys.executable, "-B", str(c.engine / "pack/jobs/winenv.py"), "--json", "--request", str(rq)],
                           capture_output=True, text=True, encoding="utf-8", env=env, timeout=300)
    try:
        report = json.loads(r.stdout)
    except ValueError:
        raise Failure("Windows 环境检查没有给出结果：" + (r.stderr or r.stdout).strip()[-300:], code="win_check_failed")  # 非玩家句
    if report.get("error") or not report.get("checks"):   # winenv 自己报错（请求 / 错误码合同）= 检查没做成，不能当成一份空清单转给启动器
        err = report.get("error") or {}
        raise Failure("Windows 环境检查没做成：" + str(err.get("message") or "清单为空") + "（" + str(err.get("code") or "") + "）", code="win_check_failed")  # 非玩家句
    return dict(checks=report["checks"], ready=report["ready"], engineInfo={"inputConfig": False, "inputConfigReason": "Windows 版不出 Mac 宿主"},
                canUninstallMetal=False)


def preflight(c):
    if os.name == "nt":   # Windows 走 winenv；本函数体（Mac 清单）里 add("Python", …) 的最低版本 winenv 按源码读（_python_minimum），别把它挪出本函数
        return preflight_windows(c)
    checks = []
    c.last_log = c.logs / (c.task + "-preflight.log")
    c.last_log.parent.mkdir(parents=True, exist_ok=True)
    c.last_log.touch()
    def add(cid, name, ok, message, scopes=("prepare",), action=None, code=None, group=None):
        # cid：稳定的英文检查项 id（剃刀 M11：前端原先按中文 name 分组判状态，这里一改名四行工具全显示「等待」）；name 只给人看
        check = dict(id=cid, name=name, ok=bool(ok), message=message, requiredFor=list(scopes),
                     group=group or ("bundled" if name.startswith(("Python", "S2Arm")) else "system"), action=action, help=message)
        with c.last_log.open("a") as log:
            log.write(c.redact(f"{name}: {'OK' if ok else 'FAIL'}: {message}") + "\n")
        if not ok:
            code = code or {
                "apple_silicon": "architecture_unsupported",
                "macos": "os_unsupported",
                "python": "engine_incomplete",
                "engine": "engine_incomplete",
                "metal": "metal_unavailable",
                "xcode": "xcode_unavailable",
                "dotnet_sdk": "dotnet_sdk_missing",
                "reader": "reader_unavailable",
            }.get(cid, "tool_missing")
            check["diagnostic"] = c.describe_failure(Failure(message, code=code))
        checks.append(check)
        c.emit("check", check=check)
    c.begin("检查 S2Arm 环境")
    # 游戏库所在盘的格式（11-02）：exFAT / FAT 外接盘上出包链必失败，打开启动器就告诉玩家，不等出包才红
    library = c.r.get("library") or ""
    if library and not library.endswith("/unconfigured"):
        from library import LIBRARY_FORMAT_MESSAGE, volume_format
        import libloc
        fmt = volume_format(library)
        ok = libloc.format_ok(fmt, system="Darwin")   # APFS / HFS+ / exFAT（v0.3.1 放开，libloc.MAC_OK_FORMATS）
        add("library_disk", "游戏库所在磁盘", ok,
            LIBRARY_FORMAT_MESSAGE.format(fmt=fmt.upper()) if not ok else
            HFS_NOTE if fmt == "hfs" else EXFAT_NOTE if fmt == "exfat" else "游戏库在 APFS 格式的磁盘上。",
            action="library", code="library_not_apfs", group="storage")
        # 云盘同步目录（11-07 启动器）：iCloud Drive / OneDrive / Dropbox 等同步目录里放游戏库，中间产物会被上传、换占位，拦下给「换游戏库」
        cloud = libloc.cloud_folder(library, system="Darwin", home=str(Path.home()), exists=os.path.exists)
        accepted = bool(cloud) and libloc.cloud_accepted(library)   # 玩家换库时确认过放在云盘里也用：照提示、不算要处理
        add("library_location", "游戏库所在位置", cloud is None or accepted,
            (libloc.CLOUD_ACCEPTED_MESSAGE if accepted else libloc.CLOUD_MESSAGE).format(cloud=cloud) if cloud else "游戏库不在云盘同步目录里。",
            action="library", code="library_in_cloud", group="storage")
    if platform.system() == "Darwin" and platform.machine() == "x86_64":
        # 给 iPhone 出包是 Intel Mac 的正式用法（#152）：Mac 本机起游戏才要 Apple 芯片，别把出包路径整段标红
        add("apple_silicon", "Apple Silicon", True, "Intel Mac：给 iPhone 出包可用；在 Mac 上起游戏仍要 Apple 芯片。", ("ipa",))
    else:
        add("apple_silicon", "Apple Silicon", platform.system() == "Darwin" and platform.machine() == "arm64",
            "需要 Apple Silicon Mac。", ("prepare", "launch"))
    add("macos", "macOS", platform.system() == "Darwin" and int(platform.mac_ver()[0].split(".")[0] or 0) >= 15,
        "启动器需要 macOS 15 或更新版本。", ("prepare", "launch"))
    add("python", "Python", sys.version_info >= PYTHON_MINIMUM, "需要 Python %s 或更新版本。" % ".".join(map(str, PYTHON_MINIMUM)), ("prepare", "launch"))
    try:
        c.require_source()
        source_ok = True
    except Failure:
        source_ok = False
    add("engine", "S2Arm 内置引擎", source_ok, "应用内置 S2Arm 宿主和准备工具。", ("prepare", "launch"))
    # .NET 检查项已删（出包 C# 工具全为随 App 预编的原生单文件，运行不再要 .NET）：源码树要 SDK 的只剩「打 iPhone 包」（下面的 dotnet_sdk 项）与命令行现编入口（各自报红）
    packaged = bool(c.r.get("engineBundle"))
    ok, version = probe(c, ["xcrun", "--sdk", "macosx", "metal", "--version"])
    developer = Path(c.r.get("developerDir") or "")
    # 没找到 Xcode 时启动器传空串：Path("") 拼出的相对路径会按工作目录（从 Finder 打开是 /）落到系统自带的
    # /usr/bin/xcodebuild 空壳上，误判成装了 Xcode（#109）。只认绝对路径下的 xcodebuild
    has_xcode = developer.is_absolute() and (developer / "usr/bin/xcodebuild").is_file()
    # 外部依赖只有这两项（其余全随应用）：缺什么就给一键处理（维护者 10-23）——Xcode 跳 App Store；装了没打开过（许可没同意）就打开它；缺 Metal 组件就一键下载
    # Xcode's missing-component shim can return zero while printing an error.
    metal_ok = ok and "Apple metal version" in version and "error:" not in version.lower()
    first_launch = has_xcode and not metal_ok and "license" in version.lower()
    add("metal", "Metal 编译组件", metal_ok,
        "Xcode 的 Metal 编译组件已就绪。" if metal_ok else
        "Xcode 还没完成首次设置：点「打开 Xcode」，同意许可后回来点重新检查。" if first_launch else
        "缺 Xcode 的 Metal 编译组件：点「一键下载」自动安装（约 1 GB），下完自动重新检查。" if has_xcode else
        "准备游戏需要 Xcode 的 Metal 编译组件；先安装 Xcode。",
        action="openXcode" if first_launch else "downloadMetal" if has_xcode else None,
        code="xcode_setup_required" if first_launch else "metal_unavailable" if has_xcode else "xcode_unavailable")
    if has_xcode:   # 打 iPhone 包 / 装到 iPhone 都要（出壳 ShellUI、申请开发描述文件走 xcodebuild 真机目标）；只在 Mac 上玩不需要
        platform_ok, sdk, why, platform_action = ios_platform(c)
        with c.last_log.open("a") as log:
            log.write(c.redact(f"iOS 平台组件判据：{why}") + "\n")  # 非玩家句
        if platform_ok is not None:
            if platform_ok:
                message = f"已装 Xcode 的 iOS {sdk} 平台组件。"
            elif platform_action == "recheck":
                message = (f"Xcode 的 iOS {sdk} 平台组件已经下载，但还没注册就绪：点「重新检查」再看一遍；"
                           "不用重复下载（约 8 GB）。只在 Mac 上玩不需要它。")
            else:
                message = (f"Xcode 的 iOS {sdk} 平台组件还没装（新装或刚升级的 Xcode 要另下）：打 iPhone 包、装到 iPhone 都要用它。"
                           "点「一键下载」自动安装（苹果官方，约 8 GB，要些时间），下完自动重新检查。只在 Mac 上玩不需要它。")
            add("ios_platform", "iOS 平台组件", platform_ok, message, ("ipa",), action=platform_action, code="ios_platform_missing")
    if has_xcode:   # 装到 iPhone 要用；不挡打包。团队号真出现才算过（只有账号不够）
        import install as inst
        # 推到 iPhone（0.6.0 Mac）：Apple ID 是随包 plumesign 自己登录的账号（apple_login / install.mac_plume），不看 Xcode；没登录 → 启动器自己的登录面板。
        #   用自己的开发者证书（mac_plume 为假）时 iPhone 不用 Apple ID，不出这一项
        if inst.mac_plume(c):
            acct = inst.plume_account(c, inst.win_install_mod(c))
            add("apple_team", "Apple ID 团队", bool(acct), inst.APPLE_SIGN_IN, ("install",), action="appleLogin", code="apple_not_signed_in", group="apple")
        # Apple TV 装机（tvos.install_tv）与下游独立 App 装机（downstream.install_ios）仍用 Xcode 账号的团队（install.pick_team(xcode_teams)）：
        #   单列一项、写明给谁用，不挡推到 iPhone（requiredFor 只标 appleTV）
        logged_in, teams = inst.xcode_account(c.env())
        add("xcode_team", "Apple TV / 独立 App 方案用的 Xcode 团队", bool(teams), inst.TEAM_PENDING if logged_in else inst.ACCOUNT_NONE, ("appleTV",), action="openXcode",
            code="xcode_team_pending" if logged_in else "xcode_account_missing", group="apple")
    ok, _ = probe(c, ["xcrun", "--sdk", "macosx", "--show-sdk-path"])
    add("xcode", "Xcode", has_xcode and ok,
        "已找到完整的 Xcode。" if has_xcode and ok else
        "没找到完整的 Xcode：点「去 App Store 安装」，装好打开一次后回来点重新检查；S2Arm 会自动找到它。",
        action="appStoreXcode")
    if packaged:
        from embedded import artifact
        why = ""
        try:
            tools = artifact(c, "Prepare")
            ready_tools = tools is not None
            if not ready_tools:
                why = "内置引擎里没有准备工具清单"
        except (Failure, OSError) as e:   # 原因写进检查项和日志，不再只剩一个 FAIL（v0.2.0 前回归时查不到为什么）
            ready_tools, why = False, str(e)
            c.emit("message", "S2Arm 游戏准备工具不可用：" + why)  # 非玩家句
        add("pack_tools", "S2Arm 游戏准备工具", ready_tools, "应用内置着色器翻译器、资源索引器和 GAL 工具。" + ("（" + why + "）" if why else ""))
    for module in ("capstone", "lz4", "numpy", "zstandard"):   # zstandard：读 zstd 存储的 Switch 2 .nup（pack/ns2_nup.py；内置 Python 3.12 没有 compression.zstd）
        ok, _ = probe(c, [sys.executable, "-c", "import " + module])
        add("python_" + module.lower(), "Python / " + module, ok, "应用内置 " + module + "；组件损坏时请重新安装完整应用。")  # 非玩家句
    # timeout 实跑一次限时命令判可用（#1133：内附的 tools/bin/timeout 不认 --version、返回 125，探 --version 把可用的它判成缺）
    for command, args in (("zstd", ["--version"]), ("spirv-as", ["--version"]), ("spirv-val", ["--version"]), ("timeout", ["5", "/usr/bin/true"])):
        ok, _ = probe(c, [command, *args])
        add(command.replace("-", "_"), command, ok,
            "应用内置 " + command + "。" if packaged else "构建环境缺少 " + command + "。")  # 非玩家句
    # 打 iPhone 包要现编 iOS 版 C# 库：需要 .NET 10 SDK（用户目录 ~/.dotnet，一键安装不要管理员密码）。只算「打 iPhone 包」的条件，不挡 Mac 打包和启动
    #   发版应用不要：C# 件随发版预编为原生单文件、iOS 壳随包（Resources/Shell），运行不要任何 .NET；只有源码树开发构建要
    if not packaged:
        from ipa import dotnet_sdk
        add("dotnet_sdk", ".NET SDK（打 iPhone 包）", dotnet_sdk()[1] is not None,
            "打 iPhone 包需要 .NET 10 SDK：点「一键安装」，从微软官方下载装到你的用户目录（约 300 MB，不要管理员密码）。只在 Mac 上玩不需要它。",
            ("ipa",), action="installDotnet")
    try:
        reader(c)
        ready = True
    except (Failure, OSError):
        ready = False
    add("reader", "S2Arm 内容读取工具", ready, "应用内置 NSP/XCI 读取器；开发模式可重新编译。", action="prepareEngine" if not c.r.get("engineBundle") else None)
    supported, reason = input_interface(c)
    return dict(checks=checks, engineInfo={"inputConfig": supported, "inputConfigReason": reason},
                canUninstallMetal=False)


# 下载 Metal 编译组件时 xcodebuild 一直不出声多久，就上屏说明一次（D-6M7D：Xcode 27.2 只打「Beginning asset download...」，之后几分钟没有任何进度输出，
#   玩家只看见转圈、等了 170 / 284 秒自己取消，分不清是网慢还是卡住）。组件大小按本机 Xcode 27.1 资产目录记的下载量 838,860,800 字节，写「近 1 GB」
METAL_QUIET_S = 60
METAL_QUIET = ("苹果的下载工具这一步不报进度：Metal 编译组件近 1 GB，网速慢时要十几分钟甚至更久，转圈就是还在下；"
               "不想等可以在 Xcode → 设置 → Components 里手动下载 Metal Toolchain，下完回来点「重新检查」。")


def download_metal(c):
    # 一键下载 Xcode 的 Metal 编译组件（xcodebuild -downloadComponent MetalToolchain，Xcode 26 起；不需要管理员密码）
    c.begin("下载 Metal 编译组件")
    heard = threading.Event()   # xcodebuild 报过进度（或已结束）：不再说明
    def line(text):
        m = DOWNLOAD_PERCENT.search(text)
        if m:
            heard.set()
            c.emit(done=float(m.group(1).replace(",", ".")), total=100.0, unit="%")
    def quiet():
        if not heard.wait(METAL_QUIET_S):
            try:
                c.emit("message", METAL_QUIET)
            except Exception:   # noqa: BLE001 —— 作业已取消 / 已结束：不再说
                pass
    threading.Thread(target=quiet, name="metal-quiet", daemon=True).start()
    try:
        c.run(["xcodebuild", "-downloadComponent", "MetalToolchain"], on_line=line)
    except Failure as e:
        # 苹果原文有确凿根因字样就换成对应的码和人话（诊断#f5eae9：原先上屏只剩 xcodebuild 输出的最后一行「})」）；没有就照原样报
        from errors import log_cause, PLAYER_REASON
        log = getattr(c, "last_log", None)
        try:
            text = Path(log).read_text(encoding="utf-8", errors="replace")[-16000:] if log else ""
        except OSError:
            text = ""
        code = log_cause([("下载日志", text)], log)[0]  # 非玩家句
        if code:
            raise Failure(PLAYER_REASON.get(code) or str(e), code=code) from e
        raise
    finally:
        heard.set()
    return {}


SDK_SIM_MAPPING = Path.home() / "Library/Developer/Xcode/SDKToSimulatorIndexMapping.plist"   # Xcode 自己写的「iOS SDK 构建号 → 可配的模拟器运行时构建号」表
BYTE_SCALE = {"b": 1, "kb": 1000, "mb": 1000 ** 2, "gb": 1000 ** 3, "tb": 1000 ** 4}   # 与 ByteCountFormatter(.file) 一致，也和 xcodebuild 打印的 8.32 GB 对得上
HFS_NOTE = "游戏库在 Mac OS 扩展（HFS+）格式的磁盘上：可以出包，但这种盘不能克隆，出包会多占一份资源包的空间。"   # v0.3.1 放开 HFS+
EXFAT_NOTE = "游戏库在 exFAT 格式的磁盘上：可以出包，但这种盘不能克隆，出包会多占一份资源包的空间。"   # v0.3.1 放开 exFAT（外部 PR #395）
PYTHON_MINIMUM = (3, 12)   # 作业层要的 Python 最低版本，唯一出处：预检（preflight）、Windows 环境检查（winenv）、applenv、oneclick.sh / stage_rewrite.sh（从这一行读）、
#   tools/py_min_syntax_lint.py 都读它。= 随包 Python 的大版本（Mac python-build-standalone 3.12、Windows 3.12），11-26 维护者定


# xcodebuild 按系统区域输出小数点或小数逗号；不能把 46,2% 的后半截读成 2%。
DOWNLOAD_NUMBER = r"\d+(?:[.,]\d+)?"
DOWNLOAD_PERCENT = re.compile(rf"(?<![\d.,])({DOWNLOAD_NUMBER})\s*%")
DOWNLOAD_BYTES = re.compile(rf"(?<![\d.,])({DOWNLOAD_NUMBER})\s*%\s*\(({DOWNLOAD_NUMBER})\s*([kmgt]?b)\s+of\s+({DOWNLOAD_NUMBER})\s*([kmgt]?b)\)", re.I)
IOS_PLATFORM_TIMEOUT = 6 * 3600    # 只防挂死：8.32 GB 在 400 KB/s 下约 6 小时，正常远快于这个上限
IOS_PLATFORM_REGISTER_WAIT = 180   # 传输完后等 CoreSimulator 注册的上限（秒）


def _sdk_platform(c):
    """选中 Xcode 的 iOS SDK：(读到了?, 版本, 构建号)。"""
    ok, ver = probe(c, ["xcrun", "--sdk", "iphoneos", "--show-sdk-version"])
    ver = ver.strip().splitlines()[-1].strip() if ok and ver.strip() else ""
    if not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", ver):
        return False, "", ""
    ok, build = probe(c, ["xcodebuild", "-version", "-sdk", "iphoneos", "ProductBuildVersion"])
    build = build.strip().splitlines()[-1].strip() if ok and build.strip() else ""
    return True, ver, build


def _allowed_runtime_builds(ver, build):
    import plistlib
    try:
        rows = plistlib.loads(SDK_SIM_MAPPING.read_bytes())
        mm = ".".join(ver.split(".")[:2])
        return {r["simulatorBuildUpdate"] for r in rows
                if r.get("sdkIdentifier") == "iphoneos" + mm and r.get("sdkBuildUpdate") == build}
    except (OSError, ValueError, KeyError, TypeError, plistlib.InvalidFileException):
        return set()


def _installed_runtimes(c):
    """已装的 iOS 运行时 [(版本, 构建号, 就绪)]；命令读不到返回 None（不判），读到空表返回 []。
    simctl runtime list 带未就绪镜像（下载完没注册的也在这里）；老 Xcode 没这个子命令时退回 list runtimes（只有已注册可用的）。"""
    try:
        out = subprocess.run(["xcrun", "simctl", "runtime", "list", "-j"], env=c.env(), capture_output=True, text=True, timeout=60)
        if out.returncode == 0 and out.stdout.strip():
            data = json.loads(out.stdout)
            if isinstance(data, dict):
                return [(str(v.get("version", "")), str(v.get("build", "")), v.get("state") == "Ready")
                        for v in data.values()]
    except (OSError, subprocess.TimeoutExpired, ValueError, AttributeError):
        pass
    try:   # 全文读（probe 只留尾部 2000 字，JSON 会被截断）
        out = subprocess.run(["xcrun", "simctl", "list", "runtimes", "-j"], env=c.env(), capture_output=True, text=True, timeout=60).stdout
        rows = json.loads(out)["runtimes"]
        return [(str(r.get("version", "")), str(r.get("buildversion", "")), bool(r.get("isAvailable")))
                for r in rows if r.get("platform") == "iOS"]
    except (OSError, subprocess.TimeoutExpired, ValueError, KeyError, TypeError):
        return None


def _runtime_matches(ver, allowed, version, build):
    if allowed:
        return build in allowed
    return ".".join(version.split(".")[:2]) == ".".join(ver.split(".")[:2])


def _runtime_ready(c, allowed, ver):
    if not ver:
        return False
    rows = _installed_runtimes(c)
    return bool(rows) and any(r[2] and _runtime_matches(ver, allowed, r[0], r[1]) for r in rows)


def _wait_runtime_ready(c, allowed, ver, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        c.check()
        if _runtime_ready(c, allowed, ver):
            return True
        time.sleep(5)
    return _runtime_ready(c, allowed, ver)


def _iphoneos_sdk_usable(c):
    """Intel Mac 不跑模拟器：只要 xcrun clang 能用这套 iphoneos SDK 编出 arm64 目标，就能出壳 / 打 IPA。
    临时文件用完即删，不动仓库，也不下载。"""
    env = c.env()
    sdk = subprocess.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], env=env,
                         capture_output=True, text=True)
    if sdk.returncode != 0 or not sdk.stdout.strip():
        return False
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / "probe.c"
        source.write_text("int s2a_probe(void) { return 0; }\n", encoding="utf-8")
        probe = subprocess.run(["xcrun", "--sdk", "iphoneos", "clang", "-arch", "arm64",
                                "-isysroot", sdk.stdout.strip(), "-c", str(source),
                                "-o", str(Path(folder) / "probe.o")],
                               env=env, capture_output=True, text=True)
        return probe.returncode == 0


def ios_platform(c):
    """选中 Xcode 的 iOS 平台组件装了没有 → (ok, SDK 版本, 详情, 一键处理)；ok=None 表示读不到、不判。
    11-07 定因（本机 Xcode 26.6 编得过、gate-host 同版编不过对照）：Xcode 26 起「iOS 平台」= 与该 Xcode 的 iOS SDK 配套的 iOS 模拟器运行时，首次打开时另下；
      没装时 xcodebuild 连真机目标（generic/platform=iOS、带设备号的目的设备、只给 -sdk iphoneos）都解析不出，报「iOS X.Y is not installed」，
      出壳 ShellUI、申请开发描述文件（pack/ensure_profile.sh）都过不去。判法同 Xcode：SDK 构建号查上面那张表得可配的运行时构建号，
      已装的 iOS 运行时里有其中之一（且 state=Ready）= 装了；表里没有这个 SDK 时退到同版本号（主.次）比。
    action 为 "recheck" 表示镜像已经下载、只是还没注册就绪：点重新检查，不要再下一遍 8 GB。"""
    found, ver, build = _sdk_platform(c)
    if not found:
        return None, "", "读不到 iOS SDK 版本", None
    allowed = _allowed_runtime_builds(ver, build)
    rows = _installed_runtimes(c)
    if rows is None:
        return None, ver, "读不到已装的 iOS 运行时", None
    have = [f"{v}（{b or '?'}）" for v, b, _ in rows]
    why = (f"SDK {ver}（{build}）可配运行时 {sorted(allowed)}，已装 {have or '无'}" if allowed else  # 非玩家句
           f"SDK {ver}（{build or '?'}）没有配套表，按同版本号比；已装 {have or '无'}")  # 非玩家句
    # Intel Mac 不出 Mac 局、也不跑 iOS 模拟器：装 8 GB 模拟器运行时对出 iPhone 包没有用处。
    # 只要本机 SDK 能直接编 arm64 目标就认这套 Xcode 可用（Xcode 26.5 / SDK 26.5 实走）。
    if platform.system() == "Darwin" and platform.machine() == "x86_64" and _iphoneos_sdk_usable(c):
        return True, ver, why + "；Intel Mac 用 iphoneos SDK 直接出壳，不要求模拟器运行时", None  # 非玩家句（拼进详情 why）
    ready = [r for r in rows if r[2] and _runtime_matches(ver, allowed, r[0], r[1])]
    pending = [r for r in rows if not r[2] and _runtime_matches(ver, allowed, r[0], r[1])]
    if ready:
        return True, ver, why, None
    if pending:
        return False, ver, why + "；有已下载、还没就绪的镜像", "recheck"  # 非玩家句
    return False, ver, why, "downloadIosPlatform"


XCODEBUILD_USAGE_EXIT = 64


def download_ios_platform(c):
    # 一键下载选中 Xcode 的 iOS 平台组件（xcodebuild -downloadPlatform iOS，不要管理员密码）。苹果只发「模拟器运行时」这一种包、没有只含真机的更小包：
    #   通用版约 10.6 GB、arm64 版约 8.5 GB（iOS 26.5；iOS 27.1 约 7.9 GB，11-07 读苹果下载目录 index2.dvtdownloadableindex）——S2Arm 只跑 Apple 芯片，固定要 arm64 版
    # 进度：xcodebuild 用 CR 原地刷新「12.3% (1.02 GB of 8.32 GB)」，按字节报（core.output_lines 已把 CR 切开），进度条与速度都按字节算。
    # 结束：运行时一变成 Ready 就收（until），不再等 xcodebuild 那个会挂住的收尾注册步；它报错退出也只当收尾失败，以 CoreSimulator 的实际状态为准。
    c.begin("下载 Xcode 的 iOS 平台组件（约 8 GB）")
    found, ver, build = _sdk_platform(c)
    allowed = _allowed_runtime_builds(ver, build) if found else set()
    def line(text):
        m = DOWNLOAD_BYTES.search(text)
        if m:
            unit = lambda u: BYTE_SCALE[u.lower()]
            c.emit(done=float(m.group(2).replace(",", ".")) * unit(m.group(3)), total=float(m.group(4).replace(",", ".")) * unit(m.group(5)), unit="B")
            return
        found_pct = DOWNLOAD_PERCENT.findall(text)
        if found_pct:
            c.emit(done=float(found_pct[-1].replace(",", ".")), total=100.0, unit="%")
    # 退出码 64 = xcodebuild 不认参数（只打用法）：-architectureVariant 是新版才有的，老版本去掉它重试（下的是通用包，大一点）
    variants = (["-architectureVariant", "arm64"], [])
    try:
        for index, variant in enumerate(variants):
            try:
                c.run(["xcodebuild", "-downloadPlatform", "iOS", *variant], on_line=line,
                      until=lambda: _runtime_ready(c, allowed, ver), timeout=IOS_PLATFORM_TIMEOUT)
                break
            except Failure as error:
                if getattr(c, "last_rc", None) != XCODEBUILD_USAGE_EXIT:
                    raise
                if index == len(variants) - 1:
                    raise Failure("这个版本的 Xcode 不支持一键下载 iOS 平台组件。请到 App Store 把 Xcode 更新到 26 或更新的版本，打开一次，再回来点「重新检查」。",
                                  code="xcode_sdk_too_old") from error
    except Failure as error:
        # 注册失败（DVTDownloadableErrorDomain Code=29 "Cannot resolve source file"）时运行时往往已经可用：以实际状态为准
        if error.code != "tool_exit" or not _wait_runtime_ready(c, allowed, ver, IOS_PLATFORM_REGISTER_WAIT):
            raise
        return {}
    if not _wait_runtime_ready(c, allowed, ver, IOS_PLATFORM_REGISTER_WAIT):
        raise Failure("iOS 平台组件下载完了，但还没有注册到 CoreSimulator：点「重新检查」再看一遍；"
                      "还不行就在 Xcode → 设置 → Components 里确认这一项。", code="ios_platform_unregistered")
    return {}

DOTNET_PROGRESS_S = 10   # .NET 安装下载进度的上报间隔（秒）


def install_dotnet(c):
    # 一键安装 .NET 10 SDK 到 ~/.dotnet：微软官方安装脚本（https://dot.net/v1/dotnet-install.sh），只当数据下载、用 bash 执行官方脚本，不要管理员密码
    c.begin("下载 .NET 安装脚本", 1, 2)
    script = c.private / "cache" / "dotnet-install.sh"
    script.parent.mkdir(parents=True, exist_ok=True)
    c.run(["curl", "-fsSL", "--max-time", "120", "-o", script, "https://dot.net/v1/dotnet-install.sh"])
    c.begin("下载并安装 .NET 10 SDK（约 300 MB）", 2, 2)
    # 官方脚本下载时不出进度（10-24 #152 Intel 机 172 KB/s 下 23 分钟零输出，看着像卡死）：先 --dry-run 取包地址和大小，
    #   让脚本把包下到已知路径（它认环境变量 zip_path），旁边每 10 秒报一次已下载字节；安装本身仍是官方脚本
    args = ["bash", script, "--channel", "10.0", "--install-dir", Path.home() / ".dotnet", "--no-path"]
    url, total = None, 0
    try:
        dry = subprocess.run([str(a) for a in args] + ["--dry-run"], capture_output=True, text=True, timeout=60)
        m = re.search(r"URL #0[^:]*: (https://\S+)", dry.stdout + dry.stderr)
        url = m.group(1) if m else None
        if url:
            head = subprocess.run(["curl", "-fsIL", "--max-time", "30", url], capture_output=True, text=True, timeout=40)
            sizes = re.findall(r"(?im)^content-length:\s*(\d+)", head.stdout)
            total = int(sizes[-1]) if sizes else 0
    except (OSError, subprocess.TimeoutExpired):
        pass
    archive = c.private / "cache" / "dotnet-sdk-download.tar.gz"
    archive.unlink(missing_ok=True)
    env = dict(c.env(), zip_path=str(archive))
    done = threading.Event()
    def watch():
        while not done.wait(DOTNET_PROGRESS_S):
            got = archive.stat().st_size if archive.exists() else 0
            if got:
                c.emit(done=float(got), total=float(total or got), unit="B")
                print(f"  .NET SDK 已下载 {got >> 20} MB" + (f" / {total >> 20} MB" if total else ""), file=sys.stderr, flush=True)  # 非玩家句
    t = threading.Thread(target=watch, daemon=True); t.start()
    try:
        c.run(args, env=env)
    finally:
        done.set(); t.join(timeout=1)
    from ipa import dotnet_sdk
    if dotnet_sdk()[1] is None:
        raise Failure(".NET SDK 装完了但没找到，请查看日志。")
    return {}
