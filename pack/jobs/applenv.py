"""Apple / iOS 环境的准备入口（命令行；维护者 10-23 归口 #106）。和 Mac 前端的环境检查用同一份判据：直接调 toolchain.preflight，
Xcode 按前端同一规则自动选装着的最新版（toolchain.detect_xcode），iPhone 目标的「系统比 SDK 新」用 target.check 判。

  python3 pack/jobs/applenv.py [--json] [--target <机型号> <系统大版本>]   只读：列出每一项通过 / 缺什么、怎么补
  python3 pack/jobs/applenv.py --install dotnet                           显式授权后安装 .NET 10 SDK 到 ~/.dotnet（同前端「一键安装」）
  python3 pack/jobs/applenv.py --install metal                            下载 Xcode 的 Metal 编译组件（同前端「一键下载」）
Xcode 本身要从 App Store 装（脚本不代装）。退出码：只读模式下 prepare / ipa 必需项有缺 → 1，齐 → 0。
"""
from __future__ import annotations
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from core import Context, Failure, default_developer_dir  # noqa: E402
import target as tg  # noqa: E402
import toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def machine_get(key):
    """开发机在 tools/machine.local 声明的值（tools/machine.sh get）。源码树外（App 的 Resources/Jobs）没有 tools/，返回 None"""
    m = ROOT / "tools/machine.sh"
    if not m.is_file():
        return None
    try:
        return subprocess.run(["bash", str(m), "get", key], capture_output=True, text=True, timeout=30).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def machine_py():
    """开发机在 tools/machine.local 声明的 S2A_PY（同 oneclick.sh）"""
    return machine_get("S2A_PY")
# 「装了够版本的 Python 但 pip 被拒」（uv / Homebrew 的 PEP 668）时的出路；一键出包的报错用同一句
VENV = "装不进包（externally-managed / uv 托管）就建个虚拟环境：python3 -m venv ~/s2a-venv && ~/s2a-venv/bin/python3 -m pip install -r launchers/macos/requirements-runtime.txt -r pack/fonts/requirements.txt，再 export S2A_PYTHON=~/s2a-venv/bin/python3"  # 非玩家句


def sdk_pin():
    """tools/toolchain.env 钉的 iOS SDK 主版本（IPHONEOS_SDK_MAJOR，pack/shell_build.sh 出壳时核）；源码树外没有这个文件 → None"""
    try:
        m = re.search(r"^IPHONEOS_SDK_MAJOR=(\d+)\s*$", (ROOT / "tools/toolchain.env").read_text(), re.M)
    except OSError:
        return None
    return int(m.group(1)) if m else None


def source_build_checks(run_path, dev=None):
    """源码树一键出包开跑后才现编的东西（#449：applenv 全绿、一键出包却在改写 / 出壳时红）：改写器固定依赖、cmake / ninja / meson、GNU Make。
    命令按一键出包的 PATH 查（run_path：选定 Python 所在目录在最前）。meson 只为 dav1d（AV1 软解兜底）：
    S2A_DAV1D=1 / 0 直说，缺省 1；显式 auto 探测本机 AV1 硬解（tools/av1hw.sh --want，dev 是出包用的 Xcode；有硬解 = 软解整条不要，ninja 的「谁要」也不带 dav1d）"""
    import importlib.util
    out = []
    spec = importlib.util.spec_from_file_location("s2a_pack_rewriter", ROOT / "pack/rewriter.py")
    rw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rw)
    #   改写器：S2A_REWRITER_C 指定的 / 指纹相同的随包那份直接用，否则要现编（钉版 Capstone / LZ4 源码 + cmake），查法同 tools/doctor.sh
    try:
        given = rw.override() or rw.bundled()
        deps = None if given else rw.dependency_dir()
        out.append(dict(name="改写器固定依赖", ok=True, requiredFor=["prepare"], group="system",
                        message=f"用现成的改写器 {given}" if given else f"{deps}（Capstone 5.0.7 / LZ4 4.4.5）"))  # 非玩家句
    except ValueError as e:
        out.append(dict(name="改写器固定依赖", ok=False, requiredFor=["prepare"], group="system",
                        message=f"{e}（缺了出包「改写」一步必红，不是游戏问题）"))  # 非玩家句
    pipdo = "，或 pip 装进一键出包用的那个 Python 的虚拟环境（它的 bin 目录会放到 PATH 最前）"  # 非玩家句
    # dav1d（AV1 软解兜底）要不要编：探测见 docstring；没答上来保守按要编（= 原默认行为）
    dav1d = True
    if os.environ.get("S2A_DAV1D") == "0":
        dav1d = False
    elif os.environ.get("S2A_DAV1D") == "auto":   # 缺省（没设）= 编：出的包要跑在玩家设备上，探测出包机判不了；auto 只显式设
        try:
            p = subprocess.run(["bash", str(ROOT / "tools/av1hw.sh"), "--want"], capture_output=True, text=True,
                                env=dict(os.environ, DEVELOPER_DIR=dev) if dev else None)
            lines = p.stdout.split()
            dav1d = lines[-1] != "0" if lines else True
        except OSError:
            dav1d = True
    tools = [("cmake", ["prepare", "ipa"], "编 C 改写器、glslang 要用：brew install cmake" + pipdo),  # 非玩家句
             ("ninja", ["ipa"], "编 glslang" + ("、dav1d" if dav1d else "") + " 要用：brew install ninja" + pipdo)]  # 非玩家句
    if dav1d:
        tools.append(("meson", ["ipa"], "编 dav1d（AV1 软件解码）要用：brew install meson" + pipdo))  # 非玩家句
    for name, need, why in tools:
        found = shutil.which(name, path=run_path)
        out.append(dict(name=name, ok=bool(found), requiredFor=need, group="system", message=f"用 {found}" if found else f"没有 {name}（{why}）"))  # 非玩家句
    #   GNU Make ≥ 4：出 iPhone 壳的 make -j 用（系统 make 3.81 会卡死），判法就是 tools/gmake.sh 那一处
    p = subprocess.run(["bash", "-c", 'source "$1"; s2a_gmake', "-", str(ROOT / "tools/gmake.sh")], capture_output=True, text=True,
                       env=dict(os.environ, PATH=run_path))
    out.append(dict(name="GNU Make 4", ok=p.returncode == 0, requiredFor=["ipa"], group="system",
                    message=f"用 {p.stdout.strip()}" if p.returncode == 0 else (p.stderr.strip().removeprefix("【红】") or "没有 GNU Make 4（brew install make）")))  # 非玩家句
    return out


def context(operation, private):
    # 选 Xcode 同 tools/toolchain.sh 出壳的顺序（#449：SDK 主版本要核出壳真用的那套）：S2A_DEVELOPER_DIR（环境 → tools/machine.local）→
    #   同前端：装着的最新一个；都找不到 → None，预检里报没装（不写死 Xcode.app）
    dev = os.environ.get("S2A_DEVELOPER_DIR") or machine_get("S2A_DEVELOPER_DIR") or toolchain.detect_xcode() or default_developer_dir()
    req = dict(schema=1, taskId="applenv", operation=operation, privateRoot=str(private), library=str(Path(private) / "library"),
               engineDir=str(ROOT), developerDir=dev)
    return Context(req), dev


def main(argv):
    as_json = "--json" in argv
    private = Path(tempfile.mkdtemp(prefix="s2arm-applenv-"))
    if "--install" in argv:
        what = argv[argv.index("--install") + 1]
        fn = {"dotnet": toolchain.install_dotnet, "metal": toolchain.download_metal, "ios": toolchain.download_ios_platform}.get(what)
        if not fn:
            sys.exit("--install 只认 dotnet / metal / ios（Xcode 请从 App Store 安装）")  # 非玩家句
        c, _ = context({"dotnet": "installDotnet", "metal": "downloadMetal", "ios": "downloadIosPlatform"}[what], private)
        try:
            fn(c)
        except Failure as e:
            print(f"🔴 {e}"); return 1
        print(f"🟢 {what} 已装好；再跑一次 applenv.py 核对"); return 0
    c, dev = context("preflight", private)
    with contextlib.redirect_stdout(io.StringIO()):   # 前端事件流（NDJSON）不打到终端
        checks = toolchain.preflight(c)["checks"]
    # 命令行（源码树）口径：读取器由一键出包首次自己编（pack/srcs.sh），App 私有目录里那份不适用；
    #   Python 依赖的提示从「重装应用」换成按 App 内置 Python 同一份钉版清单装
    checks = [x for x in checks if x["name"] != "S2Arm 内容读取工具"]
    # 与 pack/oneclick/oneclick.sh 开头的自检同一口径（10-23 维护者：缺什么 doctor 先抓到）：
    #   timeout 系统没有时用仓库自带 perl 版；.NET 10 SDK 认 PATH 或 ~/.dotnet；spirv-as / spirv-val 只给 graphics/metal 单测夹具用，出包不需要
    from ipa import dotnet_sdk, dotnet_missing
    #   Python 包按一键出包会用的那个解释器查（S2A_PYTHON → S2A_PY（环境 → tools/machine.local）→ S2Arm.app 内置 Python → PATH 上的 python3，同 oneclick.sh）
    app_py = "Contents/Resources/Runtime/python/bin/python3"
    cands = [Path("/Applications/S2Arm.app") / app_py, Path.home() / "Applications/S2Arm.app" / app_py]
    py = os.environ.get("S2A_PYTHON") or os.environ.get("S2A_PY") or machine_py() or next((str(p) for p in cands if os.access(p, os.X_OK)), None) \
        or shutil.which("python3") or sys.executable
    #   一键出包把这个解释器所在目录放到 PATH 最前（#447）：虚拟环境里 pip 装的 ninja / meson 它也用得上，下面查命令按同一个 PATH
    run_path = os.pathsep.join(filter(None, (str(Path(py).expanduser().parent), os.environ.get("PATH"))))
    # .NET SDK 按 compat/tree/global.json 的版本要求判（#449）：只看 10.x 会放过 10.0.301，到编 C# 时才红；装着但太旧和没装分开报
    _, sdk_exe = dotnet_sdk()
    dn_msg = None if sdk_exe else dotnet_missing()
    for x in checks:
        if x["name"] == "timeout" and (ROOT / "tools/bin/timeout").is_file():
            x.update(ok=True, message=f"用 {shutil.which('timeout') or '仓库自带的 tools/bin/timeout（perl 实现）'}")  # 非玩家句
        elif x["name"] in ("spirv-as", "spirv-val"):
            x["requiredFor"] = ["test"]
            if not x["ok"]:
                x["message"] = "只有 graphics/metal 的单测夹具要它（brew install spirv-tools）；一键出包不需要"  # 非玩家句
        elif x["name"] in (".NET 10", ".NET SDK（打 iPhone 包）"):  # 非玩家句
            x.update(ok=bool(sdk_exe), message=f"用 {sdk_exe}" if sdk_exe else dn_msg)
    #   共享字体（维护者 10-30：缺省开源替代字体，原版只在显式给 S2A_FONTS 时用）：同 pack/stages/sysdata.sh 的来源顺序；都没有时出包现做，生成器依赖齐就算通过
    #   生成器依赖按一键出包用的解释器查（#449：原先查 PATH 上的 python3，S2A_PYTHON 指的虚拟环境装齐了也报红）
    gen_ok = toolchain.probe(c, [py, "-c", "import fontTools, pathops"])[0]
    FONTDEPS = "" if gen_ok else "（生成器要的 fonttools / skia-pathops 还没装：%s -m pip install -r pack/fonts/requirements.txt；%s）" % (py, VENV)  # 非玩家句
    fonts = os.environ.get("S2A_FONTS") or next((str(f) for f in (Path("/Applications/S2Arm.app/Contents/Resources/Fonts"),
                                                  Path.home() / "Applications/S2Arm.app/Contents/Resources/Fonts",
                                                  Path(os.environ.get("S2A_OPENFONTS_DIR") or Path(os.environ.get("S2A_RUN_ROOT") or Path.home() / ".s2arm/run") / "s2arm/openfonts"))
                                                 if f.is_dir() and any(f.glob("*.bfttf"))), None)
    checks.append(dict(name="共享字体", ok=bool(fonts) or gen_ok, requiredFor=["prepare"], group="system",
                       message=f"用 {fonts}" if fonts else "出包时现做开源替代字体（pack/fonts/make_open_fonts.py）" if gen_ok else  # 非玩家句
                       f"没有共享字体：一键出包给 --fonts <目录>（S2Arm.app 的 Contents/Resources/Fonts），或装好生成器依赖由出包现做开源替代字体{FONTDEPS}"))  # 非玩家句
    for x in checks:
        if x["name"] == "Python":
            low = ".".join(map(str, toolchain.PYTHON_MINIMUM))
            x["ok"] = toolchain.probe(c, [py, "-c", f"import sys; assert sys.version_info >= {toolchain.PYTHON_MINIMUM!r}"])[0]
            x["message"] = f"用 {py}" if x["ok"] else f"{py} 低于 {low}：装 Python {low}+ 或装 S2Arm.app（自带钉版 Python），或用 S2A_PYTHON=<解释器> 指定。{VENV}"  # 非玩家句
        elif x["name"] == "zstd":
            # 一键出包只看你自己的 PATH（前端那套会补 Homebrew，这里不补），另认 S2Arm.app 自带的那份，同 oneclick.sh
            apps = [Path(a) / "Contents/Resources/Runtime/bin/zstd" for a in ("/Applications/S2Arm.app", Path.home() / "Applications/S2Arm.app")]
            found = shutil.which("zstd") or next((str(p) for p in apps if os.access(p, os.X_OK)), None)
            x["ok"] = bool(found)
            x["message"] = f"用 {found}" if found else "没有 zstd（解社区着色器参考库要用）：brew install zstd，或装 S2Arm.app（自带一份）"  # 非玩家句
        elif x["name"].startswith("Python / "):
            x["ok"] = toolchain.probe(c, [py, "-c", "import " + x["name"][9:]])[0]
            x["message"] = f"用 {py}" if x["ok"] else f"{py} 缺 {x['name'][9:]}：{py} -m pip install -r launchers/macos/requirements-runtime.txt（与 App 内置 Python 同一份钉版清单；换解释器用 S2A_PYTHON=<路径>）。{VENV}"  # 非玩家句
    if (ROOT / "tools/toolchain.env").is_file():   # 下面几项只有源码树（命令行一键出包）现编才要；App 的 Resources/Jobs 里没有 tools/
        checks.extend(source_build_checks(run_path, dev))
    env = c.env()
    sdk = None
    try:
        sdk = tg.os_choices(env)[0] if checks else None
    except tg.TargetError as e:
        checks.append(dict(name="iOS SDK", ok=False, requiredFor=["ipa"], group="system",
                           code=e.code, message=str(e)))
    #   iOS SDK 主版本（#449）：本机现出 iPhone 壳时 pack/shell_build.sh 要求它等于 tools/toolchain.env 的 IPHONEOS_SDK_MAJOR，对不上出壳报红；
    #   给了现成的壳（S2A_SHELL）或壳发布源（S2A_SHELL_SOURCE）就不在本机出壳，不要求
    pin = sdk_pin()
    if sdk and pin:
        prebuilt = os.environ.get("S2A_SHELL") or os.environ.get("S2A_SHELL_SOURCE")
        checks.append(dict(name="iOS SDK 主版本", ok=bool(prebuilt) or sdk == pin, requiredFor=["ipa"], group="system",
                           message=f"用现成的壳（{'S2A_SHELL' if os.environ.get('S2A_SHELL') else 'S2A_SHELL_SOURCE'}），不在本机出壳" if prebuilt else  # 非玩家句
                           f"iOS SDK {sdk}，与 tools/toolchain.env 的 IPHONEOS_SDK_MAJOR={pin} 一致" if sdk == pin else  # 非玩家句
                           f"这套 Xcode 带的是 iOS SDK {sdk}，出 iPhone 壳要 iOS SDK {pin}（tools/toolchain.env 的 IPHONEOS_SDK_MAJOR）：装 Xcode {pin}，装好打开一次；"  # 非玩家句
                           f"装了几套 Xcode 时用 S2A_DEVELOPER_DIR=<Xcode.app>/Contents/Developer 指定"))
    out = dict(developerDir=dev, iosSdkMajor=sdk, checks=checks)
    if "--target" in argv:
        i = argv.index("--target"); pt, osv = argv[i + 1], int(argv[i + 2])
        try:
            out["target"] = tg.check(dict(model=pt, gpuArch=tg.gpu_arch(pt, dev) if dev else None, os=osv), sdk)
        except tg.TargetError as e:
            out["target"] = dict(error=str(e))
    # 出包支持 Intel（维护者 10-24：用 Intel Mac 给自己的 iPhone 出包是正常用法）：Apple 芯片只是「在 Mac 上打包 / 玩」要的，不算 iPhone 出包的缺项
    for x in checks:
        if x["name"] in ("Apple Silicon", "macOS") and not x["ok"]:
            # macOS 15 是 S2Arm.app 的下限；命令行给 iPhone 出包不要求（10-24 #152 Intel macOS 15.8 实走）
            x["requiredFor"] = ["launch"]
            if x["name"] == "macOS":
                x["message"] = "S2Arm.app 要 macOS 15 或更新；命令行给 iPhone 出包（pack/oneclick/oneclick.sh）不受影响"
                continue
            x["message"] = "这台是 Intel Mac：在 Mac 上玩（原生执行要 arm64）需要 Apple 芯片；给 iPhone 出包不受影响，用 pack/oneclick/oneclick.sh"
    missing = [x for x in checks if not x["ok"] and any(s in x["requiredFor"] for s in ("prepare", "ipa"))]
    if as_json:
        print(json.dumps(out, ensure_ascii=False, indent=1))
    else:
        print(f"Xcode：{dev}（iOS SDK {sdk}）" if dev else "Xcode：没找到（/Applications、~/Applications 与外接卷下都没有 Xcode*.app，xcode-select 也没指向可用的）")  # 非玩家句
        for x in checks:
            need = "、".join({"prepare": "打包", "launch": "启动", "ipa": "打 iPhone 包", "test": "只单测要"}.get(s, s) for s in x["requiredFor"])  # 非玩家句
            tag = "" if need in x["name"] else f"（{need}）"
            print(f"{'🟢' if x['ok'] else '🔴'} {x['name']}{tag}" + ("" if x["ok"] else f"：{x['message']}"))
        if "target" in out:
            t = out["target"]; print(f"{'🔴 目标设备：' + t['error'] if 'error' in t else '🟢 目标设备：' + t['model'] + ' ' + t['gpuArch'] + ' iOS ' + str(t['os'])}")  # 非玩家句
        if missing:
            print("缺的项里，.NET SDK 用 `--install dotnet`、Metal 组件用 `--install metal` 补；Xcode 请从 App Store 装好后打开一次。")
    return 1 if missing or ("error" in out.get("target", {})) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
