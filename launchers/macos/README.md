# S2Arm Mac launcher

Native SwiftUI/AppKit launcher migrated from [LumaDock](https://github.com/Xc1Ym/LumaDock)
`238c1dd` under MIT. The library, details, sidebar, settings and task-page surfaces
are synchronized with [LumaNX](https://github.com/Xc1Ym/LumaNX) main
`88723426bca0049ef16156683e40f8fee486bb20` (0.2.19). Original copyright and permission
notices are retained below. S2Arm keeps its own engine, storage, job operations,
community resources, graphics modes, save management and cross-platform build flow.
The library, details, onboarding, settings, appearance, translations, task progress,
diagnostics and input editor retain that interaction model. The engine adapter uses
S2Arm's own LibHac reader, rewriter, system-data/index stages, shader pipeline and
NativeAOT host.

This is the Mac launcher. Its optional cross-platform build page delegates to
existing S2Arm features with app-managed components. Other platform shells keep their existing interfaces.
The shared local job entry point is `pack/jobs/main.py`, so later frontends can
reuse the task/storage layer without depending on SwiftUI.

## Compact UI sync / 紧凑界面同步

2026-11-02 对齐 LumaNX `8872342`：游戏库统计与搜索使用同高的 42 点胶囊；
详情封面 64 点、内边距 16 点，详情默认展开且置于管理操作之前；包信息按紧凑
两行展示。任务页使用 820 点内容宽度，任务卡与诊断折叠采用同一套紧凑玻璃样式。
保留 S2Arm 的兼容性提示、存档管理、跨平台构建和任务阶段接线。
S2Arm 目录数据中的 `display` 是导入版本，字段标为“导入版本”，不将它宣称为当前运行版本。
macOS 15–25 继续使用已有材质和系统按钮兼容路径。构建显式区分最低运行版本
与当前 Xcode SDK，并核对最终 Mach-O 元数据，避免新系统误用旧式按钮。

工具页与弹窗进一步共用 `WorkspaceLayout`：820 点工具页宽度、16 点外边距、
12 点卡片内边距、32 点空副标题标题栏。设置使用紧凑控制行与说明折叠；平台选择
使用同高横排；已结束的失败任务使用摘要行和详情弹出层；实时日志按需展开。
装机说明、存档、输入设置与首次运行也使用相同间距，长操作引导内容滚动显示，
底部操作保持可见。手柄图中按键几何与命中区域保持原样。

## Optional cross-platform builds / 可选跨平台编译

在 Mac 侧栏打开「跨平台编译」，按需启用 iOS、安卓或鸿蒙。游戏详情中的
「跨平台编译…」会携带当前游戏进入 iOS 页；iPhone 出包与目标设备选择
统一收在该页，游戏详情不再单独显示。GUI 直接复用
S2Arm 已有功能，不要求玩家定位源码、编译目录、产物目录或预生成工程。
平台源码、构建缓存与产物由应用管理，默认不会下载额外平台支持。

| 页面 | 玩家操作 | 复用的源码功能 | 当前输出 |
|---|---|---|---|
| iOS | 选择游戏库中的游戏和目标设备，点击「开始编译」 | 主线 `Store.packIPhone` → `pack/jobs/ipa.py` → `pack/oneclick/oneclick.sh` | IPA 与资源包 |
| 安卓 | 按需添加平台支持、准备 Docker 工具链 | `android/docker/build.sh` | Android 壳的调试 APK；当前不暴露预生成工程接口 |
| 鸿蒙 | 按需添加平台支持；自动检测已安装的工具链 | `runtime/ohos/build.sh` | 原生目标文件 `.o`；源码尚无 HAP 编译入口 |

游戏下拉框显示游戏名称；三个平台的主按钮均为「开始编译」，使用锤子图标，
位于配置卡片顶部。
「编译环境」默认折叠，展开后可检查环境、查看结果并准备缺少的组件。
满足条件后点击顶部的编译按钮。编译结束可通过任务中的
「在访达中查看产物」打开结果，不需要设置内部路径。平台构建、下载、准备
工具链与游戏运行共用串行执行槽，沿用任务取消、进度和失败诊断。

引擎和组件定位由 Mac 前端内部完成：优先复用现有引擎；编译应用通过既有
`pack/jobs/embedded.py` 安装已验证、可写的引擎缓存；缺少的可选平台在玩家
点击「添加平台支持」时获取到应用私有目录。旧版本保存的源码、输出目录和
SDK 路径配置不再使用。iOS 沿用主线的 `<游戏库>/iPhone/<游戏号>/`，OHOS 输出放在应用私有 `exports/`，Android 沿用
原有脚本输出位置。NDK 从已安装的 DevEco Studio、应用管理的工具链，或已有
`OHOS_NDK` / `OHOS_SDK_HOME` / `DEVECO_SDK_HOME` 环境中自动发现。

iOS 直接复用主线的游戏库、目标设备检测、应用自带字体、磁盘预检、里程碑进度与
.NET 10 SDK 一键安装；不再维护独立的文件夹出包入口。仍需要完整 Xcode。安卓工具链由源码的
Dockerfile 准备，需要先安装并启动 Docker；不连接设备。当前源码没有提供的
游戏加载、渲染与 HAP 编译能力，不在 GUI 中重复实现。

The optional build page invokes S2Arm's existing features directly. Players do
not choose source checkouts, build/output directories, SDK directories or prepared
projects. The app resolves its engine, manages optional platform components and
output locations, and reuses global player data settings. Compilation stays in
the existing source scripts. iOS calls the mainline iPhone job, including target-device
selection, bundled fonts, disk checks, milestone progress and SDK installation;
the Mac-only `gui_jobs` bridge handles Android/OHOS arguments and
job presentation using the shared job/engine installation functions unchanged.

## Review dependencies

This frontend is deferred for the repository's user-experience review phase.
The shared changes from PR #45 are split into independent PRs:

| Area | PR | Contract |
|---|---|---|
| First-run root guard | [#46](https://github.com/jhaizhou-ops/S2Arm/pull/46) | Shell path resolution, absent roots and symlink aliases |
| Process-group cancellation | [#47](https://github.com/jhaizhou-ops/S2Arm/pull/47) | Full command matching, existing safety checks retained |
| Container metadata / DLC | [#48](https://github.com/jhaizhou-ops/S2Arm/pull/48) | `--inspect`, `--title-id`, opt-in `--aoc-index` and `S2A_SEED_AOC=1` |
| Explicit frontend toolchain | [#49](https://github.com/jhaizhou-ops/S2Arm/pull/49) | Child-process Python/Xcode/NuGet paths, actual Xcode path logged |
| Prebuilt preparation tools | [#50](https://github.com/jhaizhou-ops/S2Arm/pull/50) | Actual binary/assembly hashes and target SDK/Metal versions in cache keys |

These shared PRs must land before building this frontend from `main`. Integration
checks combine their commits locally; the frontend PR no longer carries their
public-file changes. Gate registration covers only `launchers/macos/` and
`pack/jobs/`, preserving existing shared-file coverage.

The scope follows `docs/贡献指南.md`, `docs/架构_多终端.md` and
`docs/standards/repository.md`: the Mac shell uses existing engine interfaces;
this change does not migrate the core directories or alter runtime/C# ABI. Launcher
NDJSON remains its local task protocol and does not implement or replace the
planned runtime log protocol in `docs/standards/logging.md`. Runtime markers and
measurement logs retain their existing contract. Full gate/train/merge remain
with the owning maintainer lines.

## 不接实体手柄测试交互

```sh
bash launchers/macos/test_controller.sh
```

测试使用 Apple 的可写入 [GCController 快照](https://developer.apple.com/documentation/gamecontroller/gccontroller/withextendedgamepad())，
把按键、方向键和摇杆样本交给启动器原有的输入采样、重复节奏、焦点导航与按钮动作链。
不需要连接手柄、辅助功能授权、游戏文件或玩家游戏库；不会往系统注册虚拟硬件。
真实 SwiftUI 设置页、弹窗、下拉框、屏幕键盘和原生确认框使用原创夹具，
中英文截图写入 `build/controller-qa/`。完整 Mac 回归仍用 `bash launchers/macos/test.sh`。
专项还覆盖提示图标随连接状态显隐、单按钮单图标、失焦期间断开及出包页底栏操作；连接状态切换后原控件仍须可导航和激活。
标题回归检查主副标题基线、长版本与路径的单行省略，以及后台任务出现和消失时原生顶栏的宽度避让。
此检查覆盖应用内交互；蓝牙配对、射频断连、震动和实体按键手感须另用真手柄验证。

四键映射在顶栏手柄弹窗编辑，所有非 Switch Pro / Joy-Con 手柄共用一份设置，不区分玩家或品牌。
点击玩家行切换实体按键外观，Switch 一侧的映射保持一致；实体键帽和下拉框里的 Switch 键帽按平台绘制颜色、轮廓与反光。
Switch Pro、各代 Joy-Con 使用原生布局，选中时隐藏四键设置。其他手柄未设置时按键帽语义处理
（PS × / Xbox A 确认，PS ○ / Xbox B 返回）；可选 Switch 位置布局或逐键修改，已占用的目标自动互换。
首页、弹窗提示固定跟随玩家分配中的主手柄，实际输入与显示使用同一份映射。
确认键操作当前焦点组件，确认、完成、保存、准备和启动等提交动作统一用右扳机（PS R2 / Xbox RT / Switch ZR），可登记的原生确认框同样适用；Menu 不再提交。系统文件选择器仍使用 macOS 原有操作。
多个任务同页时，先聚焦行内的启动按钮，再按右扳机执行该任务。四键的逐游戏手柄绑定下拉框隐藏；其他按键列表也对所有玩家排除实体四键，统一在顶栏手柄弹窗管理。
Mac 游戏读取同目录的 `pad-order.face.json`，保存后即时通知刷新，并保留轮询同步；所有玩家读取同一份四键排列，
键鼠和背键配置仍按原路径处理。文件 schema 2 的 `mapping` 保存统一排列；旧版 schema 1 按 PS、Xbox、其他的顺序取首份已保存配置。
游戏内长按主手柄 L3 + R3 一秒显示或收起其四键小浮窗。
专项测试覆盖默认 / Switch 位置布局、逐键修改、保存恢复、按住时切换、玩家切换及真实 SwiftUI 下拉操作；
游戏输入与小浮窗的无资产检查运行 `bash tests/macos_native_input_test.sh`。

### 模拟手柄（开发用）

没有那么多实体手柄时，可以在本机构建的 App 里打开「模拟手柄」面板，加几套假的一代 Joy-Con / Joy-Con 2（各自选颜色，左只 / 右只分开连）、
Switch Pro、Switch 2 Pro、PS5、Xbox，测手柄面板的玩家列表、玩家灯与图示颜色，以及分配玩家的拆分 / 合并（SL + SR、L + R、跨套同按）和断开重连。
模拟的手柄走与真手柄相同的分配逻辑（`PlayerAssignment`），按键会让玩家那一行亮出按了哪个键。

打开方式（只在没有更新源的构建里有效：本机构建或自测拷贝；带 `SUFeedURL` 的发行版即使开着也没有这个菜单）：

```sh
defaults write <App 的 bundle id> S2AControllerSimulate -bool YES
```

重开 App 后，App 菜单（S2Arm 菜单）里会出现「模拟手柄」，点开是一个浮动面板（不会点一下就关，可连续操作；开着时手柄弹窗点外面不会收起）。
面板文案跟 App 界面语言（繁中 / 简中 / 英文）。关掉：

```sh
defaults write <App 的 bundle id> S2AControllerSimulate -bool NO
```

注意：模拟的手柄会写进给游戏的顺序表（`pad-order.json`），测完在面板里按「全部清除」再开游戏。

### 手柄测试（开发用）

看手柄的原始体感读数、对照体感转换用（例如手柄左右反、倾斜方向不对时，先确认是读数本身还是转换出了问题）。
开关跟「模拟手柄」同一个（`S2AControllerSimulate`，同样只在没有更新源的构建里有效），打开后 App 菜单里「模拟手柄」下面多一项「手柄测试」，
点开是一个浮动面板（开着时手柄弹窗点外面不会收起，可以对着改连接 / 分配）。

- **要看的手柄**：跟手柄面板同一份清单，跟着分配玩家走：Joy-Con 2 拼成一对就是一对、分开就是两只单只；一代 Joy-Con 照分配的单只 / 一对；
  没有玩家位的手柄也列。这里只能选，连接、拆开、分配都在手柄面板做，两边即时同步。
- **读数**：原始六轴（加速度 g、陀螺 °/s，未转换），每轴置中长条 + 数字 + 最近 3 秒陀螺波形；一对左右两栏。
  Joy-Con 2 / Pro 2 手柄的读数走启动器手柄导航那一条连接程序连接（连接程序一次只服务一个客户端，不另开连接），
  所以游戏进行中、分配玩家中、S2Arm 不在前台时会暂停，面板上写明原因；系统蓝牙手柄读 GameController 的 motion。
- **3D 姿态**：重力 + 陀螺融合，「归正」重设水平朝向，可拖动转视角。「姿态用」切到「游戏看到的」时，读数先套服务层给游戏的转换
  （`NpadImu.Push`：单只 Y 镜像、一对绕 X 转 180°；单测会读那个文件对照，两边改了要一起改），转换不对时模型会跟手上的动作相反。
- **测试流程**：按「开始」后依步骤做，每步有浅蓝色的目标模型和右上角的示意图，对上并停住 1 秒自动下一步；「上一步 / 下一步」可以选从哪一步开始。
  流程跟着选的手柄定：单只 Joy-Con → 单只测试，一对 → 一对测试（`ControllerTestFlow.swift`）。
  `~/S2Arm/dev/controller-test/steps.json` 存在时改用它（只有文字、手动下一步，格式见 `ControllerTestStep.parse`）。
- **录制**：按「开始」同时录，每次一个 `~/S2Arm/dev/controller-test/readings-<时间>-<流程>.tsv`（时间、手柄、左右、步骤、六轴）。

现在只做了 Joy-Con 的测试流程（Joy-Con 2；一代 Joy-Con 用同一套步骤，但读数口径还没有真机校对，见 `Gen1Axes`）。
其他手柄（Pro 手柄、PS、Xbox 等）的体感测试可以接着加在这里：在 `ControllerTestFlow` 加一套流程和目标姿态，
`ControllerTestHub.pickFlow` 按选的手柄换过去。改步骤后可以出示意图检查：

```sh
S2A_FLOW_RENDER=/tmp/flow.png swift test --package-path launchers/macos/frontend --filter testRenderFlowSheet
```

（`S2A_FLOW_RENDER_FLOW=single|pair` 只出一套，`S2A_FLOW_RENDER_SIDE=L|R` 单只的步骤只画那一只，`S2A_FLOW_RENDER_MODEL=gen1` 画一代 Joy-Con。）

## DMG installation window / 拖拽安装窗口

安装窗口沿用 [GitHub Pages 下载页](../../docs/site/index.html) 的近黑底、奶油白、
细描边卡片，以及现有玻璃 S 图标与字标。中英文安装说明常驻；两个图标由
Finder 显示，S2Arm.app 可以实际拖入指向 `/Applications` 的快捷方式。
背景以 1× / 2× TIFF 输出，布局尺寸与图标位置统一读取
[`Branding/dmg-layout.json`](Branding/dmg-layout.json)。

只预览设计（需要 macOS 和 Swift，无需打包依赖或完整 App）：

```sh
python3 launchers/macos/build_dmg.py --preview build/macos/dmg-preview
```

预览图为 `build/macos/dmg-preview/preview.png`，其中图标仅用于示意；
实际背景 `background.tiff` 不含图标。传入 `--app <路径>/S2Arm.app` 可用该 App 的图标预览。

正式打包使用已经完成签名、公证和钉票据的完整 App；这是 `sign_release.sh` 之后
单独执行的发行步骤，不会自动发布或上传文件：

```sh
python3 -m venv build/dmg-venv
build/dmg-venv/bin/python -m pip install -r launchers/macos/requirements-dmg.txt
build/dmg-venv/bin/python launchers/macos/build_dmg.py \
  --app build/macos/S2Arm.app --output build/macos/S2Arm-mac-arm64.dmg
```

打包不调用 Finder 或 AppleScript，不需要桌面自动化权限；严格校验输入和复制后
App 的签名，再校验 DMG，成功后才替换目标文件。签名校验也接受本地 ad-hoc
构建；脚本不会把它当作已公证发布包。安装说明不嵌入版本号，
图标不隐藏 `.app` 扩展名，避免改写已签名应用的 FinderInfo。
依赖仅供维护者打包使用，不随玩家运行环境分发。

## Build and run

Apple Silicon and macOS 15+ are needed for direct-mode play on the Mac (games execute natively as arm64).
Intel Macs are supported for building iPhone packages only: `S2Arm.app` can be built on x86_64 to run the packaging UI and produce an IPA plus resource pack, but it contains no working Mac game host. The command-line `pack/oneclick/oneclick.sh` remains available; check the environment first with `python3 pack/jobs/applenv.py`. The application bundles Python 3.12,
pinned `capstone`, `lz4`, `numpy`, .NET 10 runtime, `zstd`, SPIRV-Tools,
the in-tree perl `timeout` (`tools/bin/timeout`) and the precompiled S2Arm preparation helpers. Players install no Python
packages, .NET SDK or Homebrew. Preparing games uses an external full Xcode with
Metal compiler components; launching a prepared game does not require Xcode.
Apple SDKs and compilers are **not** bundled. Game/keys/shared-font data are
player-provided. Checks never install or download dependencies.

### Building from source

One command checks every build dependency and prepares the ones that live in the
source tree. It reports each missing item together with the command that installs it:

```sh
python3 launchers/macos/prepare_build.py            # add --check to only inspect, --venv <dir> to choose the venv
```

| Dependency | Rule | Prepared by the command |
| --- | --- | --- |
| macOS arm64 or x86_64 | The runtime keeps the build machine's slice only; x86_64 builds are packaging-only (no Mac game host, but the iPhone shell still includes the game-library UI) | Checked |
| Full Xcode with iOS platform and Metal Toolchain | Resolved like packaging (`tools/toolchain.sh`): `S2A_DEVELOPER_DIR` or `tools/machine.local`, then `xcode-select`. A Metal compiler version different from `translator/dmsh_chain.lock` prints a `note` only (`dmsh_chain.py --build-env` lets it through; the shader cache is keyed by compiler, so a different Xcode just recompiles) | Checked |
| .NET SDK | Whatever `compat/tree/global.json` accepts: `version` plus `rollForward: latestFeature` (currently 10.0.403 or a later 10.0 feature band / patch). The command runs `dotnet --version` inside `compat/tree`, so .NET itself applies the rule. Install with `python3 pack/jobs/applenv.py --install dotnet` (the same path the launcher uses) | Checked |
| Homebrew `python@3.12 spirv-tools spirv-cross glslang zstd bash cmake` (+ `meson ninja` only when this Mac lacks hardware AV1 decode) | Runtime collection (`bundle_runtime.py`), C rewriter (CMake 3.18+), dav1d software fallback (meson / ninja; auto-skipped when the machine has hardware AV1 decode) — libvpx needs no formula either way | Checked; prints the `brew install` line |
| Rust (`cargo`) | `pack/devpush` | Checked |
| Build Python venv (default `build/venv`) | Pinned `tools/rewriter-requirements.txt`, `pack/fonts/requirements.txt` (fonttools / skia-pathops for the bundled fonts) and `launchers/macos/requirements-runtime.txt` | Created and installed |
| C rewriter sources | Pinned Capstone / LZ4 from `runtime/rewriter/prepare_deps.py`; looked up in `S2A_REWRITER_DEPS`, then `build/rewriter-deps`, then `<S2A_RUN_ROOT>/s2arm/deps/rewriter-deps` | Downloaded to `build/rewriter-deps` when none is complete |

AV1 decode prefers VideoToolbox hardware decode at runtime. `S2A_DAV1D` defaults to `1` (the dav1d software fallback is always linked and bundled, because the build machine cannot tell which Macs / iPhones the result will run on). Setting it explicitly to `auto` makes every build step probe this Mac (`tools/av1hw.sh` — `VTIsHardwareDecodeSupported` **plus** actually creating a VideoToolbox AV1 session with a real av1C; a probe failure conservatively keeps the fallback). On a Mac with hardware AV1 decode (M3/M4/M5, A17 Pro and later) the dav1d software fallback is not linked, its prebuilt library is not bundled, and `meson` / `ninja` are not required at all. Machines without hardware AV1 decode (M1/M2, A16 and earlier) keep the software fallback exactly as before. Use `auto` only for builds that run on this Mac alone; `S2A_DAV1D=0` forces the fallback off.

`S2A_VPX` works the same way for libvpx (VP8/VP9): default `1`; with an explicit `auto` it is probed by `tools/vp9hw.sh` (register the supplemental VP9 decoder, then actually create a 64x64 VP9 session — `VTIsHardwareDecodeSupported` itself is not a criterion). When the machine can take the system VP9 route, libvpx is not built, linked, or bundled (a tiny all-failure stub satisfies the C# DirectPInvoke bindings), and VP9 goes VideoToolbox-first everywhere (direct `nn::codec` line and the Nvdec service line), falling back to the managed decoder. **VP8 has no system or managed replacement on any Mac** — with the stub linked, VP8 content (e.g. the Diablo 2 intro on the Switch 1 line) fails with a clear error instead of a fake picture. This is why the default stays `1` for anything shipped to others (iOS deployment target 18 has no VideoToolbox VP9 route, so iPhone packages built with `auto` on a probed Mac would fall back to the managed VP9 decoder and error out on VP8).

Then build with the interpreter the command printed:

```sh
build/venv/bin/python3 launchers/macos/build_app.py     # --developer-dir <Xcode.app/Contents/Developer> overrides the Xcode choice
open build/macos/S2Arm.app
```

`build_app.py` rebuilds `build/macos/Runtime` through `bundle_runtime.py` whenever it is missing or stale.
For command-line packaging and tests, point `S2A_PY` at the same interpreter (environment variable or a
`S2A_PY=` line in `tools/machine.local`, see `tools/machine.local.example`). `build_app.py` hands the same
choice to the scripts it starts, shell builds included: `S2A_PY` from the environment, then `tools/machine.local`,
otherwise the interpreter running `build_app.py`. Nothing in the build or packaging
scripts falls back to a maintainer's private paths; an unset dependency is reported, not guessed.
Work caches go under `S2A_RUN_ROOT` (default `~/.s2arm/run`; the app always sets a private one).

`timeout` is copied from `tools/bin/timeout` (perl) in this repository. bash 5 stays a separately invoked GPL-3.0 program;
its license text and source offer are copied into the app. The runtime builder installs pinned
Python requirements, relocates libraries, keeps Apple Silicon binary slices,
signs native code and audits runtime links. The host and C# preparation tools are
compiled during application packaging, never on the player's machine. This is a
local ad-hoc build; this PR does not publish a notarized binary release.

### Using the built app

The packaged app includes the S2Arm NativeAOT/Metal Mac host, NSP/XCI reader and
preparation tools. No external engine checkout is needed. Native runtime libraries are relocated and
audited to reject non-system absolute links and symlinks escaping the app. In settings choose Xcode and a player-provided directory containing shared `.bfttf` fonts. Recheck, choose an empty game library, import
`prod.keys`, then add NSP/XCI files or a folder. Metadata comes from decrypted, integrity-checked
CNMT/NACP; filenames are not used to classify base/update/DLC. New complete games
can enter the serial preparation queue automatically. Launching is always explicit.

The newest imported base/update and newest revision of each DLC are selected. An
update can be imported before its base; the library displays what is missing.
AArch32 is unsupported. Other titles are marked experimental: prepared products
are not a claim that a particular game or scene is compatible. Data DLC that
cannot be decrypted stops preparation instead of silently becoming license-only.
The shared font directory and generated timezone data are recorded as preparation
inputs; this does not implement a general firmware-container importer.

The Mac game shell is enabled only by `S2A_MAC_FRONTEND=1`; default `make host`
retains its headless test behavior. It attaches a native `CAMetalLayer`, feeds the
existing `s2a_vpad_read` HID ABI, resets input on loss of focus, and closes through
the host's graceful shutdown path. Automatic test-player input is disabled.
Use the details page to choose game language and edit keyboard/controller mappings.
Language is mapped to `S2A_SYSTEM_LANGUAGE`, mode to `S2A_DOCKED`.

游戏内存由编译过程自动配置，无需选择内存模式。编译只读游戏主程序的代码特征生成包内的
`addr_need.tsv`，不需要先运行游戏、不读运行日志；Mac 与 iOS 使用相同的需求表和布局算法。
旧版手动内存设置会在保存偏好时移除，其他设置保留。旧准备包需要重新编译。

Preparation binds the table to the program ID and main NSO build ID. The alias
field is a static hint read from the main NSO: a nonzero value means the program
claims a huge alias address region at start-up (a constant size passed to
`nn::os::AllocateAddressRegion`), so the host lays the alias region out first. The
ordinary heap field is always the application pool. No run logs or per-game tables
are consulted.

The normalized requirements and collector version participate in preparation and
rewrite cache keys. Engine and release manifests declare `memoryRequirementsVersion`
so an old host cannot silently ignore the new heap field. Shader reuse follows its
existing cache rules. The retired `vammLayout` request is rejected; no environment
switch, per-game memory preference, or game-specific layout rule is required.

With the mouse set to camera mode, click inside the game view to capture relative
motion and hide the cursor. The capture click does not send an attack. Press Esc
or switch windows to release it; click again to recapture. In mouse-button-only
mode, the cursor stays free. While captured, Esc only releases the mouse and does
not send its mapped game button. Press Esc again after release to use that binding.
Camera motion feeds the Switch right stick and still
follows the game's turn-speed limit; it is not a native PC mouse-look interface.

## Mac controls guide / Mac 键位引导

Human launches show a per-game controls card before spawning the host when enabled.
The AI/control-port `/launch` path treats the guide as already handled and starts
directly, without waiting for a human confirmation. The selected game metadata supplies its name/ID; game-specific mappings take
priority over the global default. Confirming the card passes that exact mapping to
the Mac job, which validates it and writes a unique `runtime-input/<gameId>/<taskId>.json`
snapshot. The native HID adapter and in-game guide use the same dictionary loaded
once at host startup. Editing a saved mapping never relabels an already-running game.
The launch job rejects a runtime manifest belonging to a different game. It removes
its own snapshot after the host exits, including error and cancellation paths,
without deleting another run's file.

The mapping editor and pre-launch card share an interactive paired Joy-Con diagram,
with classic Neon Blue on the left and Neon Red on the right. Select a physical
button to inspect / edit its keyboard, mouse and controller inputs. Full-list,
stick/mouse options and the existing live input-test page remain available.
Input recording and testing stay scoped to this window. The controls pages use a
uniform white light-theme background (black in dark mode), neutral separators and
blue focus indicators with charcoal actions; they do not change the rest of the launcher's theme.
The editor uses left-aligned, full-width input menus and a single evenly spaced
keyboard-shortcut row. Its per-button restore action stays directly below the
binding fields rather than floating at the bottom of the panel. Menu hit areas
include the complete outlined surface, not only the text.
Expanded options share a white/black popover with checkmarks, keyboard focus,
scrolling for longer lists, and selection/escape dismissal. The full binding list,
mouse/stick options use the same controls and panel spacing;
there are no separate grey system-picker or grouped-form surfaces on these pages.
The original vector outlines preserve Nintendo's published **102 × 35.9 mm**
height/width ratio; circular controls are not stretched. Button/stick anchors are
illustrative front-view positions calibrated to Nintendo support imagery, not CAD
measurements. HOME / Capture are decorative only and do not add unsupported ABI inputs.

Reference material: [Nintendo button mapping](https://www.nintendo.com/en-gb/Support/Troubleshooting/How-to-Change-the-Button-Mapping-on-Nintendo-Switch-Controllers-1766029.html),
[Joy-Con dimensions](https://www.nintendo.com/jp/hardware/switch/modal/specs/joy-con.html?width=960),
[front-view reference](https://support.nintendo.com/jp/switch/controller/img/joycon/img02.jpg).
Nintendo photographs are references only and are not bundled in this change.

Review screenshots and execution evidence are attached to the pull request, not
stored in the source tree. UI evidence uses synthetic fixtures without game
resources, keys, saves or real device identifiers.

The guide is built into the Mac host via `S2A_MAC_FRONTEND=1` and the existing
engine packaging closure (`macos/`). No companion installation,
Python UI, private game profile or copied local helper is required. Newly built
engine bundles and newly prepared Mac game hosts include it automatically. Old
prebuilt hosts must be rebuilt; merely updating launcher sources cannot retrofit
a previously compiled executable. This does not change Android, iOS or HarmonyOS
shells/builds, and does not imply game compatibility or playability.

In game, use the menu-bar controller icon or **Controls / 查看键位 (⌘K)**. Closing
the guide returns focus to the game rather than shutting down the host. Viewing
controls does **not** pause guest execution. The native guide uses a matching
red/blue paired Joy-Con diagram: selecting any of its 24 supported targets only
changes the read-only detail panel. It never edits or saves the loaded mapping.
Stick selections also show the frozen analogue-axis sources. The menu-bar entry
uses an original, monochrome paired Joy-Con template icon that adapts to the
system appearance. The launcher keeps mapping configuration on the game detail
page and in settings, without a duplicate keyboard toolbar shortcut.

The pre-launch “show next launch” toggle publishes its file-backed preference
change immediately, so both checking and unchecking visibly reflect the saved
value. This preference remains in the launcher, not the read-only native guide.
Mouse buttons emulate controller
buttons; this is not native point-and-click menu support. Actions depend on the
game: the common guide does not invent title-specific skill/menu meanings.

The launcher's “show next launch” control and the native guide share
`<privateRoot>/input-guides/<gameId>.json` (schema 1, `gameId`, boolean `show`).
The job passes its location as `S2A_INPUT_GUIDE_PREFS`; other IDs cannot inherit
that record. `S2A_GAME_NAME`/`S2A_GAME_ID` come from catalogue metadata, while
`gameName`/`gameId` in the prepared manifest provide native fallback metadata.
The native guide never opens automatically, including CLI hosts: it is available
only through the menu or Command-K. The pre-launch preference invalidates the
visible launcher view immediately after a successful write. Native controls are Chinese/English according to
`S2A_UI_LANGUAGE` (or system language). Old hosts without confirmed input support
get an explicit warning instead of presenting saved mappings as active controls.

Tests use synthetic mappings and no game resources: `InputGuideTests` covers the
pre-launch handoff, stale-sheet rejection, per-game preferences, optional mapping
fields and bilingual rendering; `pack/jobs/tests/test_jobs.py` covers validation,
per-run snapshot isolation and wrong-game rejection; `Tests/input_test.m` exercises
the actual Mac adapter, native guide data, shared preferences and identity fallback.
For a no-game interactive native preview, run its built test binary with
`--guide-preview`, optionally setting `S2A_GAME_NAME`, `S2A_GAME_ID` and
`S2A_UI_LANGUAGE`. A preview window is not a real game acceptance result.

## Data and isolation

- Launcher identity: `org.s2arm.macos.launcher`; private settings/jobs/keys/tools:
  `~/Library/Application Support/S2Arm`. `--data-root <directory>` isolates testing.
  Existing LumaDock/NSMac settings and libraries are not automatically migrated.
- Each library has `.s2arm-library.json` with a UUID. A missing external disk or
  mismatched UUID fails closed; the worker does not create a substitute library.
- `sources/`: managed content-addressed original containers. Imports clone/copy and
  SHA-256 verify them. The original files are never modified.
- `cache/<titleId>/<identity>/`: private build roots/checkpoints. Input content,
  source content, target, tool versions and fonts determine the build identity.
- `games/<titleId>/releases/` and atomic `current`: independent complete products;
  cancellation/failure preserves the preceding successful version. Each release
  includes a file hash/size manifest checked before launch.
- `saves/<titleId>/`: persistent Ryujinx data including the player's saves.
  `S2A_SAVE_DIR` is intentionally not used: it is a seed/import interface that clears
  and replaces saves at boot. Runtime shader caches live in `runtime-cache/`.
- Game details → **存档备份与恢复… / Save backup and restoration** exports a
  verified `.s2asave` archive and previews compatibility before restoring. Stop the
  game first. Restoration retains the original data and a pre-restore archive under
  `save-backups/<titleId>/`; interrupted transactions recover before the next launch.
  The [G05 v1 format and recovery contract](../../docs/存档备份_G05.md) is the
  handoff for future platform migration; actual game readback remains to be verified.
- Remove only hides the catalogue entry. Source/cache cleanup are separate,
  confirmed actions and never delete runtime releases or saves. Shared source files
  used by another catalogue entry are retained.

Jobs use their own cache/lock/output roots and never call the scheduler, train,
device deployment, shared root refresh or a game-specific SDK hook. Engine resources
are verified and cloned from the signed app to `hosts/engine.noindex/<bundle-content-id>/` in
the default library location (only the copy in use and the previous one are kept; before a library exists the
copy goes to `engines.noindex/` in private application storage). The `.noindex` suffix keeps Spotlight from
indexing the embedded `S2ArmGame.app`, so it never shows up in Launchpad or the Open With menu. The signed application and runtime stay immutable; the
precompiled preparation helpers run from that engine copy. The original source-checkout pipeline
retains its default build behavior unless `S2A_PREBUILT_TOOLS` is explicitly set. The bundled preparation
source closure contains a fresh local-only Git snapshot for the existing pipeline
HEAD-based keys, without remotes, developer history or player data. Git maintenance
is disabled and initial compaction finishes before resource hashing and signing.
The source checkout is needed only to build the application or run CLI development
checks. A source change during packaging fails the build. `S2A_DEVELOPER_DIR` and `S2A_PYTHON` are explicit per-job overrides;
the repository's default toolchain and interpreter choices remain unchanged.

## Local job protocol

Run `python3 -I -B pack/jobs/main.py --request <0600-json-file>`. Requests include
`schema:1`, `jobId` (or the launcher's `taskId` alias), `operation`, `privateRoot`,
`engineDir` (CLI development), optional `engineBundle` (packaged app), `library`, `libraryId`, `developerDir`, and operation parameters.
Operations: `setup`, `preflight`, `prepareEngine`, `keys`, `import`, `catalog`,
`build`, `launch`, `remove`, `clean`, `saveInput`, `loadInput`, `resetInput`, `setDefaultInput`,
`diagnostics`, `saveStatus`,
`saveBackup`, `saveInspect`, `saveRestore`.

Stdout is NDJSON with `schema`, `jobId`, monotonic `seq`, UTC `timestamp`, `stage`,
`state`, `doneUnits/totalUnits/unit`, result/artifact and error/log references.
Unknown totals are null; phase counts are not measured percentages. Heartbeats
keep silent compilers visible. Terminal protocol success is `succeeded`; the UI
normalizes it to its persisted `completed` state. Extra `type`, `taskId`, `done`
and `total` fields preserve the migrated UI's event adapter. This is a concrete Mac
adapter, not a claim that all planned Android deployment/resume APIs are implemented.

SIGTERM cancels only the job's subprocess groups through `tools/safe_killpg.sh`.
Requests are removed by the launcher; diagnostic exports contain bounded redacted
text/job records, never keys, source containers, resources or saves. Interrupted
UI jobs remain visible for explicit retry; validated extraction checkpoints can be
reused. Neither frontend tasks nor diagnostics alter the game's runtime log contract.

## Verification

### 空间预算与估时依据（G03）

编译任务保留 `space` 事件中的源文件大小、各写入卷的 `needBytes` /
`freeBytes`、预算依据和未量化开销；失败、结束及重新打开后的任务仍可查看。
这些字节值来自 `pack/jobs/library.py`，界面不合计不同卷，也不把未量化缓存
算作零；「达到预检门槛」只代表当时的检查结果。未知或无效读数显示未知，
不足以红色图标和文字同时提示。

里程碑消费可选的 `source` / `samples` / `basis`：当前阶段显示历史记录数或
默认参考值，任务卡可展开逐步依据。旧任务没有这些字段时仍可读取，来源标为
未知。前端不改变预算算法、阻断条件、估时历史格式或游戏/设备构建参数。
以上为主机预检展示，手机空间、传输与安装耗时仍需真机验收。

```sh
python3 -B -m unittest discover -s pack/jobs/tests -v
DEVELOPER_DIR=<Xcode.app/Contents/Developer> swift test --disable-sandbox \
  --cache-path "$PWD/build/macos/spm-cache" --package-path launchers/macos \
  --scratch-path build/macos/spm
bash launchers/macos/test.sh
```

`python3 launchers/macos/smoke_runtime.py --runtime <Resources/Runtime>
--tools <Resources/Engine/Binaries/Prepare>` exercises the actual bundled Python,
.NET without any SDK, RomFS index, SPIR-V/GAL translation, and the external Xcode
Metal compilation/link and GPU archive. Its
minimal shader/filesystem probes do not claim game compatibility.

The PR validation record distinguishes source/unit checks, actual app UI checks,
native host builds, and any real-game evidence. No game, key, save, cover or player
cache is committed as a test asset.

## 致谢与第三方依赖

| 依赖 | 版本 | 许可 | 怎么用 |
|---|---|---|---|
| [LumaDock](https://github.com/Xc1Ym/LumaDock) | `238c1ddc0a5964b830805d4cf427fca4ef80d86e` | MIT | SwiftUI 游戏库、详情、任务、设置、输入映射、翻译及部分任务/存储辅助代码；位于 `launchers/macos/` 与 `pack/jobs/`，完整版权及许可文本见下节 |
| [LumaNX](https://github.com/Xc1Ym/LumaNX) | `88723426bca0049ef16156683e40f8fee486bb20` | MIT | 原生侧栏与游戏列表、折叠管理操作、系统材质、设置与任务页排版；仅同步前端，保留 S2Arm 业务实现 |

## Third-party notices

### LumaDock / LumaNX

MIT License

Copyright (c) 2026 LumaDock contributors
Copyright (c) 2026 jhaizhou (reference-port-derived portions)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### 本地化静态审计（不需要 Xcode）

```sh
mkdir -p build
python3 -B launchers/macos/check_localization.py > build/localization-audit.json
python3 -B -m unittest discover -s launchers/macos/Tests -p 'test_localization_scan.py' -v
```

也可向扫描器传入一个或多个 Swift 源文件，只检查指定文件。输出按调用位置列出
`covered`（已有精确键）、`unchanged`（英语路径无汉字原样保留）、`missing`
（直接字面量没有精确键或可用模板／分片路径）和 `manual`（需人工／运行时复核）。
退出码 1 表示发现未登记字面量，2 表示输入错误；0 **不代表整个界面翻译完成**。
当前存量缺口仍会报出，扫描器不会将它们涂绿。回归门禁通过
`Tests/localization_missing_baseline.json` 精确核对剩余 14 处的相对路径、原文和出现
次数（不用易变行号，也不只比较总数）。新增、换成另一条、移到另一文件、重复
出现都会失败；修复旧项后也须在同笔审查中下调基线，不能保留会复活的旧豁免。
不要自动接受新扫描结果覆盖基线。测试递归收集启动器 Swift 文件；新增目录也检查。
回归测试由 `test.sh` 的 `Tests/test_*.py` 收集，固定社区页 24 个键的独立英文期望，
并包含删除键、错键、新增漏译、同数量替换、移动与重复出现负控。社区页直接
字面量的 `missing` 已清零，但动态与分片回退仍是 manual，不代表整个页面验收。

范围仅为全局 `L(...)` 调用：不是 Swift 语法解析器，不扫描其他 UI API 或 iOS
原生文案；注释和字符串中的伪调用不算。动态表达式、拼接、raw／多行字符串、
模板和分片回退列为 `manual`，不据此推断运行结果或缺译数量。已有精确键包含
合法空译文（例如拼句的“ 完成”），不机械判错。术语暂沿用现有“社区资源 →
Community resources”“宽屏表 → widescreen table”；完整术语统一和 Mac/iPhone
实际展示、截断、辅助功能验收仍须继续，不由宿主静态检查替代。
