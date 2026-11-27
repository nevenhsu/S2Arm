#!/usr/bin/env bash
# Device-free CPU tests: C/C++, CMake 3.18+, JDK 17, Android NDK 29.0.14206865,
# pinned offline rewriter sources and Python Capstone 5.0.7 / LZ4 4.4.5.
# 本机自测排在门禁后面：不在门禁里跑（没有 GATE2_PAR）就把自己降到 nice 15，子进程继承；门禁步的优先级由门禁自己定
[ -n "${GATE2_PAR:-}" ] || renice 15 -p $$ > /dev/null 2>&1 || true
set -euo pipefail
android_dir=$(cd "$(dirname "$0")" && pwd)
repo_dir=$(cd "$android_dir/.." && pwd)
# 超时用仓库的 perl 版 tools/bin/timeout（#591，同 tools/toolchain.sh / graphics/gate.sh）：直接 bash android/test.sh 时
# 子脚本（如 core-runtime/tests/run_shutdown_thread.sh）裸调 timeout，macOS 没有 GNU timeout 就 127。
source "$repo_dir/tools/_timeout.sh"
# 输出目录按运行隔离（#919）：同树两份门禁并行、或上一次运行被中断时，
# 固定目录 build/android-host-tests 里的半写 .class / 半写 boot-spec 会被当成输入，
# 红成 java.lang.ClassFormatError: Truncated class file / JSONDecodeError。
# 这里每次一个独有目录，退出即删；不重试、不放宽超时。
source "$android_dir/tools/host_out.sh"
out=$(s2a_host_out) || { echo '【红】建不出主机测试输出目录（S2A_HOST_OUT_ROOT / TMPDIR 不可写）' >&2; exit 1; }
trap 'rm -rf "$out"' EXIT
# 原生段的 CMake：$CMAKE → PATH 上的 cmake → SDK cmake/*/bin/cmake 里版本最高的（tools/cmake_pick.sh，#1568 定）
source "$android_dir/tools/cmake_pick.sh"
# 与 gate.sh / 共享改写器门禁同选固定解释器；PY 保留显式开发入口。
# gate.sh 已前置核验版本和扩展，所有 Python 子进程沿用同一个选择。
if [ "${S2A_PY+x}" ]; then
    PY=$S2A_PY
elif [ ! "${PY+x}" ]; then
    PY="$HOME/.s2arm/venv/bin/python3"
fi
export PY S2A_PY="$PY"
# 环境下限（bash 5 / Python = PYTHON_MINIMUM，tools/env_floor.sh）：错环境跑出的红是假红，不够就不跑
source "$repo_dir/tools/env_floor.sh"; s2a_env_floor "$repo_dir" "$PY" || exit 2
# The platform gate installs this helper; direct test.sh keeps original streams.
source "$android_dir/tools/gate_step.sh"
# 子项键（GATE_ONLY=<键…> 只跑这些键的步；门禁里每步都要在表里登记，见 android/tools/gate_keys.tsv）
s2a_gate_keys_init "$android_dir/tools/gate_keys.tsv" || exit 1
# Audio focus policy: lifecycle, denial and late callbacks without Android hardware.
s2a_gate_step mkdir -p "$out/java"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/AudioFocusPolicy.java" \
    "$android_dir/tests/AudioFocusPolicyTest.java"
s2a_gate_step java -ea -cp "$out/java" org.s2arm.android.AudioFocusPolicyTest
# Diagnostic archive preflight: synthetic names and sizes only; never reads logs.
s2a_gate_step bash "$android_dir/tests/diagnostics/run.sh"
# Capacity selection is shared by the production Android host implementation.
s2a_gate_step "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror "$android_dir/tests/capacity/policy_test.cpp" -o "$out/capacity-policy-test"
s2a_gate_step "$out/capacity-policy-test"
s2a_gate_step bash "$android_dir/tests/cf_dictionary_test.sh"
# Linux 专项验证真实亲和、继承及音频路径；macOS 由 Linux 门禁执行。
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step bash "$android_dir/tests/capacity/run_linux.sh" "$out/capacity"
fi
s2a_gate_step bash android/cs/video/tests/run.sh
# T117: production scratch leases and MemoryOwner fallback, CPU-only / offline.
s2a_gate_step bash "$android_dir/core-link/runtime-tests/buffer-scratch/run.sh"
# Buffer 正常上传读取（生产 Buffer.SyncAction → LoadRegion → SetData → Memmove，Mac VM 原生夹具；只在 macOS 跑，11-01 #894 接线）
s2a_gate_step bash "$android_dir/core-link/runtime-tests/buffer-read/run.sh"
# 原生 provider 归属与可选导出（显式绑定 / 苹果老入口共 11 个隔离进程，含补时两导出同一查找口径；11-01 #894 接线）
s2a_gate_step bash "$android_dir/core-link/provider-tests/run.sh"
# 两级回放队列（nvk_dq；宿主，真队列 + 真录制线程 + 记录桩）：多生产者序列 / 环回绕 / 背压 / 跨线程清空 / 两条线程同时清空 / 运行时开关；
# TSan 零报告；六种变异（没写完就发布、清空不等、不等环里有地方、环尾长度写错、先报已读、抹掉别人的「在等」标记）都必须红在预期阶段。
# 队列用 pthread 自旋锁（苹果系统没有）：只在 Linux 上跑，macOS 由 Linux 门禁执行。
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step bash "$android_dir/tests/run_dq_queue.sh"
fi
# 游戏档（nvk_game_profile，plat_nvn.c 用 <link.h> 的 dl_iterate_phdr 认模块）：macOS 没有 <link.h>，只在 Linux 上跑
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step "$PY" "$android_dir/tests/nvk_game_profile_test.py"
fi
# 描述符写入初始化（宿主）：三路描述符必须整结构清零、dstArrayElement=0；「漏字段」反例必须被抓到。
# 曾经漏填 dstArrayElement，真机第一笔 draw 就把 Adreno 驱动写崩。
s2a_gate_step bash "$android_dir/tools/nvn_vk_desc_write_test.sh"
# 反馈快照池复用/淘汰判据：在途帧的槽不可复用、不可淘汰；完成证明到手后才可复用。
s2a_gate_step bash "$android_dir/tools/nvn_vk_snap_pool_test.sh"
# 描述符 set 帧内复用：命中条件必须严格（不同 offset/range、布局/池代际变化、跨帧、哈希碰撞都不命中）。
s2a_gate_step bash "$android_dir/tools/nvn_vk_desc_cache_test.sh"
# 纹理子分配：块归属（释放 B 不能回填到 A）、对齐（512 请求不能返回 256）、切分余量、跨块不串、满块失败。
# 第一版生产代码在这里错过：先搬末条覆盖本条、再读本条偏移 → 两张在用的纹理落到同一段内存。
s2a_gate_step bash "$android_dir/tools/nvn_vk_tex_suballoc_test.sh"
# 程序结果里 bindings 超出容量必须准确失败（F1：绝不静默截断）；含撤守卫的反例说明。
s2a_gate_step bash "$android_dir/tools/nvn_vk_program_binding_cap.sh"
s2a_gate_step bash "$android_dir/tools/vk_poolahb_alias_test.sh"
# 直连逐帧帧时间（宿主，确定性）：环形语义、丢弃/计数可见、分片转储字节级核回。
s2a_gate_step bash "$android_dir/tools/nvn_vk_frame_log_test.sh"
# WSI actual-present diagnostics: NDK declarations, partial batches and generation isolation.
s2a_gate_step bash "$android_dir/tests/nvk_shown_timing_test.sh"
# 设备 / 实例扩展查询：枚举失败（能力未知、报红）与确实不支持分开；VK_INCOMPLETE 重取。
s2a_gate_step bash "$android_dir/tests/vk_dev_ext_test.sh"
# 支持范围下限：GPU 只支持 Vulkan 1.0（物理设备 apiVersion < 1.1）具名拒绝，原因短语与外壳 PlayerError 一致；3 个负对照。
s2a_gate_step bash "$android_dir/tests/vk_api_floor_test.sh"
# 整台设备能力形态档（#1749 只看能力）：每份设备档案（tests/devprofiles/*.json：Adreno、Mali、Vulkan 1.1 保守档）下各跑一遍建设备 /
#   caps JSON / 几何着色器路径 / 数组层夹取缺省 / 设备事实落盘，生成的档案与档案文件逐字段比、再当形态档回读一遍；两台真机档另与
#   devices/*.json 逐项比；「能力判断改坏」与「设备事实漏记」的负对照必须红在对应档（宿主 cc + 假驱动，约 12 秒）
s2a_gate_step bash "$android_dir/tests/caps_shape/run.sh"
# 设备档案白名单校验与导出工具（#1749 M0）：正反例、隐私负对照（白名单外的键必红）、诊断包 / run-as 两条取法（adb 替身，约 2 秒）
s2a_gate_step bash "$android_dir/tests/devprofiles/lint_test.sh"
# 调试开关读数的线程内缓存（宿主，确定性）：命中 / 过期 / 缺省 / 名字比较 / 名字上限，7 个负对照。
s2a_gate_step bash "$android_dir/tests/nvk_flag_cache_test.sh"
# 自定义边框色就近取内置三档（宿主，确定性）：没有 VK_EXT_custom_border_color 时浮点 / 整数各取最近一档。
s2a_gate_step bash "$android_dir/tests/nvk_border_nearest_test.sh"
s2a_gate_step bash "$android_dir/tools/generic_frame_trace_test.sh"
# #48 A5（宿主，确定性）：BatteryManager 净放电换算与不可用状态、采样器停采/迟到回调/读数开关。
s2a_gate_step bash "$android_dir/tools/system_power_test.sh"
# #48 A3+A4（宿主，确定性）：Surface.setFrameRate 投票边界（去重/未知清票/API<30）、
# 轮询接线（周期→票、省电档→cap、开关、停后迟到回调不采）、生产 present_period 的封顶与登记。
s2a_gate_step bash "$android_dir/tools/frame_rate_vote_test.sh"
s2a_gate_step bash "$android_dir/tools/system_tuning_test.sh"
# #48 P8（宿主，确定性）：热状态映射与 dm_thermal_now / dm_low_power_now 强定义、采样器起停 / 只在变化时推。
s2a_gate_step bash "$android_dir/tools/power_state_test.sh"
# 转储解析器（宿主，确定性）：分片拼回/完整性拒绝、全窗统计、与 16 帧聚合行对拍、C→python 对拍。
s2a_gate_step "$PY" "$android_dir/tests/direct_framelog_test.py"
# 设备锁与起局最小闭环（宿主，确定性；adb 替身不起真机）：锁并发/失效/留置、还原失败保留锁、
# 局目录被 tools/quality_check.py 安卓分支判读。
s2a_gate_step "$PY" "$android_dir/tests/device_lock_test.py"
s2a_gate_step "$PY" "$android_dir/tests/run_game_test.py"
# run_game_test 的用例收场后不留临时目录（通过 / 失败 / setUp 中途出错三种收场）。
s2a_gate_step "$PY" "$android_dir/tests/run_game_tmp_cleanup_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_game_mode_test.py"
# 通用模式编译事件：原生文件生成、丢弃负控及读取器完整性校验。
s2a_gate_step "$PY" "$android_dir/tests/generic_compile_events_test.py"
s2a_gate_step "$PY" "$android_dir/tests/compile_events_reader_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_trace_test.py"
# CMake 挑选规则的宿主单测（假 cmake，可移植，始终跑）
s2a_gate_step bash "$android_dir/tools/cmake_pick_test.sh"
# S2A_TEST_NO_NATIVE 只跳过宿主 ctest 阶段；可移植改写器矩阵始终执行。
# 非 Linux 宿主：CMakeLists 按 CMAKE_SYSTEM_NAME 门掉 Linux 专属 API 的目标
# （memory.c 的 mremap；guest_signal.c 的 SIGRTMIN/sigcontext.h/ucontext；
# host_android.c 的 eventfd；wsi_lease.c 的 pthread_condattr_setclock；
# direct/selection.c 的 getrandom；tsd.c 与 module_test.c 的 /proc/self/maps），
# 这里显式登记未测项，不静默当绿。
if [ -z "${S2A_TEST_NO_NATIVE:-}" ]; then
    s2a_cmake_pick || { s2a_gate_note "【红】安卓宿主测试找不到 CMake（见上一行）"; exit 1; }
    echo "CMake：${S2A_CMAKE}（${S2A_CMAKE_FROM}）；ctest：$S2A_CTEST"
    cmake_bin=$S2A_CMAKE
    # 原生段的子测试有的自己按 PATH 找 cmake（core-link/test_audit.py）：只给 CMake 这三步、只多 cmake / ctest
    #   两个名字（垫片目录）。不把选中目录整个提前、也不改后面各步的 PATH——它可能是 /usr/bin，会换掉 JDK 17 / venv
    s2a_cmake_shim "$out/cmake-shim" || exit 1
    cmake_env=(env "PATH=$out/cmake-shim:$PATH")
    s2a_gate_step "${cmake_env[@]}" "$cmake_bin" -S "$android_dir" -B "$out" -DCMAKE_BUILD_TYPE=Debug
    s2a_gate_step "${cmake_env[@]}" "$cmake_bin" --build "$out" --parallel 2
    s2a_gate_step "${cmake_env[@]}" "$cmake_bin" -E chdir "$out" "$S2A_CTEST" --output-on-failure --timeout 30
    [ "$(uname -s)" = Linux ] ||
        s2a_gate_note "未测：android_memory / android_plat（mremap）、android_guest_signal（SIGRTMIN）、android_host_android（eventfd）、android_wsi_lease（pthread_condattr_setclock）、android_direct_selection（getrandom）、android_tsd / android_module（/proc/self/maps）需 Linux"
fi
s2a_gate_step "$PY" "$android_dir/tests/ldn_settings_test.py"
s2a_gate_step "$PY" "$android_dir/tests/ldn_dialog_test.py"
s2a_gate_step "$PY" "$android_dir/tests/debug_environment_test.py"
s2a_gate_step "$PY" "$android_dir/tests/debug_gate_test.py"
# 通用层零专用配置（#1357）：native / gfx 源码不得出现具体游戏包名
s2a_gate_step "$PY" "$android_dir/tests/common_layer_no_package_test.py"
s2a_gate_step mkdir -p "$out/java"
s2a_gate_step javac -encoding UTF-8 -Xlint:all -Werror -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/LanDiscoverySession.java" \
    "$android_dir/tests/LanDiscoverySessionTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.LanDiscoverySessionTest
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_browser_test.py"
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_publisher_test.py"
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_jni_test.py"
s2a_gate_step "$PY" "$android_dir/tests/lan_discovery_provider_test.py"
# Pure screenshot pixel contract uses the same JDK 17; no SDK/native/Skia dependency.
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/CapturePixels.java" \
    "$android_dir/tests/capture/CapturePixelsTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.CapturePixelsTest
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ThreadedRendererConfig.java" \
    "$android_dir/tests/ThreadedRendererConfigTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.ThreadedRendererConfigTest
# Exercise the production keyboard lifecycle against a controllable host UI.
s2a_gate_step mkdir -p "$out/host-keyboard"
s2a_gate_step javac -encoding UTF-8 -d "$out/host-keyboard" \
    "$android_dir"/tests/host_keyboard_stubs/android/{app,os,text,view,widget,util}/*.java \
    "$android_dir"/tests/host_keyboard_stubs/org/{json,s2arm/android}/*.java \
    "$android_dir/app/src/main/java/org/s2arm/android/HostKeyboard.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/LegacyTextInput.java" \
    "$android_dir/tests/HostKeyboardLifecycleTest.java" \
    "$android_dir/tests/LegacyTextInputTest.java"
s2a_gate_step java -cp "$out/host-keyboard" org.s2arm.android.HostKeyboardLifecycleTest
s2a_gate_step java -cp "$out/host-keyboard" org.s2arm.android.LegacyTextInputTest
# #1456 capprobe：判定合同 + 缓存编排（known-bad / inconclusive 固化 / runtime-lost 回写 / 保留上限）。
# CapProbe 引用 NativeBridge，宿主编译用 tests/stubs 里的替身。
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/tests/stubs/org/s2arm/android/NativeBridge.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/CapProbeVerdict.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/CapProbe.java" \
    "$android_dir/tests/CapProbeVerdictTest.java" \
    "$android_dir/tests/CapProbeStoreTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.CapProbeVerdictTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.CapProbeStoreTest
# #1456 capprobe 进程模型（宿主，无设备）：父进程另一线程持锁跑探测，posix_spawn
# 经系统 linker 起的助手仍在限时内出合同行；同场景下 fork 出的子进程拿不到锁
# 必卡死（负对照，对应门检反馈）。fake linker 断言 exec 目标=linker、argv[1]
# 为「apk!/<entry>」包内 spec（#1482 owner 判据 b）。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    "$android_dir/tests/capprobe_fake_helper.c" -o "$out/capprobe-fake-helper"
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    "$android_dir/tests/capprobe_fake_linker.c" -o "$out/capprobe-fake-linker"
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I "$android_dir/native/capprobe" \
    "$android_dir/tests/capprobe_spawn_test.c" "$android_dir/native/capprobe/capprobe_host.c" \
    -o "$out/capprobe-spawn-test" -lpthread
s2a_gate_step bash -c '
    mkdir -p "'"$out"'/fakeapk/lib/arm64-v8a" &&
    cp "'"$out"'/capprobe-fake-helper" "'"$out"'/fakeapk/lib/arm64-v8a/libs2a_capprobe.so"'
s2a_gate_step env "CAPPROBE_FAKE_APK=$out/fakeapk" "CAPPROBE_FAKE_LINKER=$out/capprobe-fake-linker" \
    "$out/capprobe-spawn-test"
# #1482 GS 路径决策 + debug.s2a.gs_path 强制开关：两变体——debug 变体带
# S2A_ANDROID_DEBUG_ENV（两道门都活，开关生效），release 变体没有（编译期
# 门死，开关不生效）。属性表与 logcat 由测试桩出，门是真门。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -D__ANDROID__ \
    -DS2A_ANDROID_DEBUG_ENV -I "$android_dir/tests/hostvk" \
    "$android_dir/tests/gs_path_test.c" "$android_dir/native/direct/gs_path.c" \
    "$android_dir/native/capprobe/capprobe_host.c" \
    -o "$out/gs-path-test-debug" -lpthread
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -D__ANDROID__ \
    -I "$android_dir/tests/hostvk" \
    "$android_dir/tests/gs_path_test.c" "$android_dir/native/direct/gs_path.c" \
    "$android_dir/native/capprobe/capprobe_host.c" \
    -o "$out/gs-path-test-release" -lpthread
s2a_gate_step "$out/gs-path-test-debug"
s2a_gate_step "$out/gs-path-test-release"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamepadState.java" \
    "$android_dir/tests/GamepadStateTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GamepadStateTest
# Vulkan 款直连三态（#274）：intent 取值、缺省不是手动直连、与 native direct_choice.h 同值
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/VulkanDirectChoice.java" \
    "$android_dir/tests/VulkanDirectChoiceTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.VulkanDirectChoiceTest "$android_dir/native/direct_choice.h"
s2a_gate_step bash "$android_dir/tests/exit_order_test.sh"
# 存档原子态（#961）：缺服务层出口时轮询答「未知」（负数、红一次），Java 不当原因位提示；各带一个回退负对照。
s2a_gate_step bash "$android_dir/tests/save_atomic_poll_test.sh" "$out/save-atomic-poll"
s2a_gate_step "$PY" "$android_dir/tests/save_atomic_probe_test.py" --out "$out/save-atomic-probe"
# A0 端到端（合成包 → 改写器 → stage_a0 → current.json → 契约校验 → BootContract）：要 lz4 / capstone（用门禁同一个 ${PY}）+ JDK
PY="$PY" s2a_gate_step bash "$android_dir/tests/stage_a0_test.sh"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameTouch.java" \
    "$android_dir/tests/GameTouchTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameTouchTest
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ThreadedRendererConfig.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Json.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchLayout.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchPad.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchLayoutStore.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchRouting.java" \
    "$android_dir/tests/TouchPadTest.java" \
    "$android_dir/tests/TouchPadDpadEdgeTest.java" \
    "$android_dir/tests/TouchRoutingTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.TouchPadTest "$repo_dir/ios/layouts/classic-reference.json"
s2a_gate_step java -cp "$out/java" org.s2arm.android.TouchPadDpadEdgeTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.TouchRoutingTest
# 触屏菜单文案：英文默认和 values-zh 必须成对，MainActivity 不再硬编码（#528 第 11 项）
s2a_gate_step "$PY" "$android_dir/tests/touch_strings_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Json.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootContract.java" \
    "$android_dir/tests/BootContractTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.BootContractTest
# B6-1 F01 data/editor model: reads the existing iOS reference; no SDK or device evidence.
s2a_gate_step "$PY" "$android_dir/tests/strings_translation_test.py"
s2a_gate_step "$PY" "$android_dir/tests/touch_layout_model_test.py"
# 触屏外观（尺寸 / 透明度的模型与接线，含两条负对照）：之前没有门禁入口（11-20 android/gate.sh 接回时 test_wired 查出）
s2a_gate_step "$PY" "$android_dir/tests/touch_appearance_model_test.py"
s2a_gate_step "$PY" "$android_dir/tests/touch_appearance_wiring_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootSpec.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ProcessBirth.java" \
    "$android_dir/tests/BootSpecTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.BootSpecTest "$out/boot-spec"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/RuntimeLibrary.java" \
    "$android_dir/tests/JniAbiFixture.java" \
    "$android_dir/tests/RuntimeLibraryTest.java"
# The JNI ABI hash from source (native/jni_abi.py, baked into the library) must equal the reflection one.
s2a_gate_step java -cp "$out/java" org.s2arm.android.RuntimeLibraryTest \
    "$("$PY" "$android_dir/native/jni_abi.py" "$android_dir/tests/JniAbiFixture.java" | tail -1)"
s2a_gate_step "$PY" "$android_dir/native/jni_abi.py" "$android_dir/app/src/main/java/org/s2arm/android/NativeBridge.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchInputBridge.java" > /dev/null
# Compile the actual Android host-field adapter with a frozen HostApi fixture.
s2a_gate_step "${CC:-cc}" -std=gnu11 -DS2A_UNIT_TEST -DS2A_HOST_FIELDS_ONLY -I "$repo_dir/runtime/include" \
    "$android_dir/tests/host_fields_test.c" "$android_dir/native/plat_android.c" -o "$out/host-fields-test"
s2a_gate_step "$out/host-fields-test"
# NVN→Vulkan 格式映射纯函数：ASTC 全 14 尺寸 × UNORM/SRGB + 压缩块表，未知格式必须 UNDEFINED。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    -I "$android_dir/native/nvn" -I "$repo_dir/graphics/nvn/gfx/nvn" \
    -I "$android_dir/tests/vkhost" -I "$repo_dir/graphics/nvn/third_party/vulkan-headers/include" \
    "$android_dir/tests/nvn_vk_format_test.c" "$android_dir/native/nvn/nvn_vk_format.c" -o "$out/nvn-vk-format-test"
s2a_gate_step "$out/nvn-vk-format-test"
# 时间戳待写队列的纯记账（不依赖 Vulkan）：帧序号 FIFO 退休 / 跨帧槽位环 / validBits 回绕。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I "$android_dir/native/nvn" \
    "$android_dir/tests/ts_queue_test.c" -o "$out/ts-queue-test"
s2a_gate_step "$out/ts-queue-test"
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I "$android_dir/native/nvn" \
    -I "$repo_dir/graphics/nvn/replay" -pthread "$android_dir/tests/nvk_land_auto_test.c" -lm -o "$out/land-auto-test"
s2a_gate_step "$out/land-auto-test"
# #653：逐报告时间戳校准纯数学（不建 Vulkan device，不替代真机/VVL 验证）。
s2a_gate_step "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I "$android_dir/native/nvn" \
    "$android_dir/tests/report_ts_test.c" -o "$out/report-ts-test"
s2a_gate_step "$out/report-ts-test"
# VM 夹具端点核对（宿主源码级，mac 也跑）：svc_*.c 新引用的 core 符号漏登记当场红，不等 Linux 真链
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/vm/check_endpoints.py" --out "$out/vm-endpoints"
# 宽屏 screen_metrics 的宿主两步（screen-widen-test / widen-loader-test）11-01 挪进合入前层：tests/android_screen_host_test.sh
case "$(uname -s)" in
  Darwin) s2a_gate_step bash "$repo_dir/graphics/track/tests/drain_readflush/build_run.sh" "$out/drain-readflush" ;;
  Linux)
    s2a_gate_step "$PY" "$android_dir/native/runtime/tracking/tests/run.py" --out "$out/tracking-precise"
    s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/vm/run.py" --out "$out/vm-mirror" ;;

  *) s2a_gate_note "【红】unsupported host for READFLUSH precise regression"; exit 1 ;;
esac
s2a_gate_step "$PY" "$android_dir/tests/runtime_library_activity_test.py"
# #1474：LAUNCHER 无界面跳板——存活→:game task 回前台 / 无局→库页 / 恒 finish 不出 UI；负对照：恒走库页必须红。
s2a_gate_step "$PY" "$android_dir/tests/icon_entry_test.py"
# W6.0 全账解析：已知长帧/GPU 完成后读/present 四态/retained 负例，缺字段必须保持 unavailable。
s2a_gate_step "$PY" "$android_dir/tests/w6_account_test.py"
s2a_gate_step "$PY" "$android_dir/tests/native_bridge_load_test.py"
s2a_gate_step "$PY" "$android_dir/tests/jni_abi_test.py"
s2a_gate_step "$PY" "$android_dir/tests/dev_runtime_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_batch_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_prepare_test.py"
# 首局预热随包种子整理（android/tools/vk_warm_seed.py）：校验 / 配对 / 按程序身份滤管线 / 与来源先后无关；宿主读种子的用例在 nvk_warm_modules_test.sh
s2a_gate_step "$PY" "$android_dir/tests/vk_warm_seed_test.py"
# 普通程序（不带存储缓冲窗口）种子补件：真编译器端到端（真实请求 → 编译 → 调试采集 → 再编译 → 资源根种子）+ 补件各臂（关 / 开 / 别的 profile / 身份被改 / 摘要被改 / 文件缺失，只有「开且补件完好」免前台翻译）
s2a_gate_step "$PY" "$android_dir/tests/vk_warm_nowindow_test.py"
s2a_gate_step "$PY" "$android_dir/tests/host_link_test.py"   # 宿主单测链接闭包：自动补链 / 弱引用放行 / 无提供方点名红 / 替身优先
# 新回放层：真实图形/计算请求生产入口的贴图池容量、超限与调试读数。
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --withdraw
# 事实复用放宽（#2166）：高槽直连事实不拆程序 / 请求开关依赖来源可复用；尾部事实判据、掩码越界、请求来源白名单、高槽句柄拦截、
#   开关（含关闭态 --fact-wide-off、生产缺省档 --fact-wide-default 各单独起进程）与命中计数各撤一处，对应用例必红
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --fact-wide-mutants
# 几何着色器走出包预编库的负对照（正例在上面无参那一步里单独起进程跑）：撤掉接线各一处，--gs-libpack 必红
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --gs-libpack-mutants
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --lp-output
# 计算程序出包预编库的非对齐闸负对照（#496085）：撤掉 state_ok 对 ckey 的生效，--cs-libpack 必红
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --cs-libpack-mutants
# 图形程序出包预编库的六条状态闸与读数（正例 --lp-gate 在上面无参那一步里单独起进程跑）：六条闸各撤一位、直方图两处累加 / 快照落位 / 并列顺序各改一处，--lp-gate 必红；
#   存储缓冲「非对齐」只看程序真用到的槽（用到的槽集合 / 逐阶段 / 逐槽 / 说不准退回旧口径 / 开关 / 快路摘要 / 三个读数）各改坏一处，同样必红
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --lp-gate-mutants
# 计算程序预热（清单记账 / 阶段 / 从缓存建 / 开关）与现场编译诊断事件（图形 / 计算）的负对照：各撤一处，对应断言必红
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --compute-warm-mutants
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --diag-mutants
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --link-refused-file
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --live-compile-mutants
# 图形阶段存储图像（#999）：句柄字 72..86 偶 → 图像槽 / 表外·奇字·数组不猜 / 声明落程序记录；解析与声明各改坏一处，graphics_image_layout_test 必红
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --graphics-image-mutants
# 计算程序装载：游戏 UBO 形态的类型 3 句柄进程序 usrc 表（#1594 片 10）：撤掉这一步、或对所有项都放，必红（真解析 + 生产 build_compute）
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_pool_test.py" --cs-ubo-sampler-mutants
# Workgroup 共享字节核算的宿主件：OpTypeFloat/Int 词数分校、位宽负控、无共享变量零计数
s2a_gate_step "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$android_dir/native/direct" -I "$repo_dir/graphics/third_party/spirv_cross" \
    "$android_dir/tests/t_spv_workgroup_bytes.c" -o "$out/spv-workgroup-bytes-test"
s2a_gate_step "$out/spv-workgroup-bytes-test"
# #1865 统一 SPIR-V 合规核对的宿主件：声明扫描 / 能力扩展需求表 / push 与 Workgroup 字节，合成模块逐类负控
s2a_gate_step "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$android_dir/native/direct" -I "$repo_dir/graphics/third_party/spirv_cross" \
    "$android_dir/tests/t_spv_compliance.c" -o "$out/spv-compliance-test"
s2a_gate_step "$out/spv-compliance-test"
# Shutdown admission, bounded waits and active-context retention (including negative controls).
s2a_gate_step "$PY" "$android_dir/tests/nvk_shutdown_test.py"
# 上屏线程 CPU（rp_present.c，#1501 差距 9）：同线程配对 / 跨线程与时间倒退不计 / 首次只记起点 / 取走清零 / 会话清起点 / 上屏接线，6 个点变异各撤一处必红（纯宿主 C）
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/present_tc_mutation_test.py"
# GS 脏位、阶段绑定、管线和原始图元拒绝的独立回退反证。
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/gs_replay_mutation_test.py"
# 写跟踪取范围（find_span）的有序索引（#1133）与池查找的线程内备忘（#974）：索引的排序 / 子树最大值 / 复查代际 / 写者纪律 / 上限与停用 / 备忘的键各处的撤改必红（纯宿主 C 编译）。
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/dirty_span_memo_mutation_test.py"
# 存储缓冲槽长度语义：地址有效、长度 0 按硬件语义绑真实内存（池剩余长度，回写按它登记），没有可绑内存才绑空缓冲照画；5 个点变异各撤一处必红（纯宿主 C）
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/storage_unmapped_mutation_test.py"
# 计算派发里「贴图字在游戏常量缓冲里的类型 3 句柄」（#1594 片 10）：读计算阶段绑的常量缓冲、采样器号 = 字高位 | 池下标 + 池起点、没登记绑默认采样器 / 建不出具名跳过、
#   形状校验、取视图带声明；7 个点变异各撤一处必红（纯宿主 C）
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/cs_ubo_sampler_mutation_test.py"
# ReadTexels 读 GPU 内容（rp_read_texels.inc / 旁路段 / rp_entry 分支）：等之前的活、提交锁、行距片距、READBACK 与块对齐、栅栏推进、提交内拒绝、修前，各撤一处必红（纯宿主 C）
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/read_texels_gpu_mutation_test.py"
# 通道里着色器写之后的游戏屏障（rp_pass.c rp_pass_barrier / rp_pass_prepare_draw，rp_draw.c prog_shader_writes）：开着的通道里已有绘制写了存储缓冲 / 存储图像、其后游戏发了带顺序位的屏障 → 下一笔绘制前结束该通道；
#   没有写 / 只有缓存失效位 / 屏障在写之前 / 拆过之后的新通道，不拆；14 个点变异各撤一处必红（纯宿主 C）
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/shader_write_barrier_mutation_test.py"
# 图形阶段常量缓冲「GPU 待写回」必须在开通道前解析（rp_draw.c 准备阶段前置，#2100）：撤预解析 / 去掉「只许计算阶段」的限定，各必红
s2a_gate_step "$PY" "$repo_dir/graphics/nvn/tests/cbuf_prep_mutation_test.py"
# 原创 Maxwell 经生产请求、真实编译器与生产结果解析；GPU 像素由 Vulkan 门禁另验。
s2a_gate_step "$PY" "$android_dir/tests/gs_compiler_chain_test.py"
# BUFFER texture: real producer, Maxwell compiler and SPIR-V parser.
s2a_gate_step "$PY" "$android_dir/tests/tbuf_compiler_chain_test.py"
s2a_gate_step "$PY" "$android_dir/tests/libpack_tbuf_withdraw_test.py"
# 计算取库的原因码（s2a_libpack_why）：正例 + 改坏 libpack_vk.c 的原因码各一处必红（诊断按码分类，不读报错文本）
s2a_gate_step "$PY" "$android_dir/tests/libpack_reason_code_test.py"
# 图形阶段「用到哪几个存储缓冲槽」（s2a_libpack_stage_sbuf_slots，引擎判断非对齐绑定是否影响程序的依据）：正例 + 改坏判据各一处必红（说不准一律不猜：少报会把按对齐翻的库件交给非对齐的绑定）
s2a_gate_step "$PY" "$android_dir/tests/libpack_sbuf_slots_test.py"
# Pool-backed TBUF replay: both stages, synchronization and three mutations.
s2a_gate_step "$PY" "$android_dir/tests/tbuf_replay_core_test.py" --withdraw
# W2 compute: real C components with synthetic input / Vulkan mocks, never GPU or device evidence.
s2a_gate_step "$PY" "$android_dir/tests/direct_compute_identity_test.py"
# P0-3：constantBufferUse 宽掩码的 S2AI1 v3/v4 分叉现由计算与图形共用——正例 + 两处撤改必红（纯宿主 C 编译，秒级）。
s2a_gate_step "$PY" "$android_dir/tests/direct_input_identity_mutation_test.py"
# GS 扩容前后 VS / VS+FS / CS 规范身份逐字节一致，96 条事实不截断。
s2a_gate_step "$PY" "$android_dir/tests/direct_input_identity_compat_test.py"
# BUFFER 事实与规范身份；运行时绑定未接通前旧入口仍须拒绝。
s2a_gate_step "$PY" "$android_dir/tests/tbuf_facts_test.py"
s2a_gate_step "$PY" "$android_dir/tests/tbuf_facts_test.py" --withdraw
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_program_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_compute_request_test.py"
# kindCheck 调试开关只走请求 JSON（DirectShaderCompiler.cs:39 只读 JSON、不读进程 env）：默认构建不带该键，
# 调试门开 + debug.s2a.kind_check=0 才写 "kindCheck":false；撤成 execute 里 setenv 或删掉该键必须红（宿主 C 编译，秒级）。
s2a_gate_step "$PY" "$android_dir/tests/direct_compile_request_host_test.py"
# 现编时翻译器崩溃留下输入（#1501 D②）：崩溃线程正在编的请求原样落 compile-inflight-<pid>-<槽>.log；crash.c 安卓分支弱引用调（宿主 C 编译，秒级）。
s2a_gate_step "$PY" "$android_dir/tests/compile_inflight_test.py"
s2a_gate_step "$PY" "$android_dir/tests/w2_observation_core_test.py"
# nvk 新层：首次使用的准备工作下沉（iOS 蓝本 warm_start / pso_pend）。
# 回放计时：帧级「回放耗时」只在最外层累加一次（三层嵌套每层 1 ms 期望 3 ms，修前 6 ms）。
s2a_gate_step "$PY" "$android_dir/tests/replay_exec_time_test.py"
# 预热调度时序合同：先程序后管线、在途重试、有界 Stop、开关关时不起线程。
s2a_gate_step bash "$android_dir/tools/nvn_warm_test.sh"
# 真实回放/着色器收尾清掉上一局的程序预热清单；宿主两局与遗漏 reset 负对照。
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step sh "$android_dir/tools/nvk_prog_warm_session_test.sh"
fi
# 程序清单最近使用顺序：CANDIDATE_TREE 是本次门禁检出的仓库。
# 门禁不传 LEGACY_TREE，不依赖 Git 历史或联网；手工查旧格式兼容性时，
# runner 的第二个参数可传修复前的独立源码检出（例如本 PR 的基线）。
s2a_gate_step sh "$android_dir/tools/nvk_prog_warm_recency_test.sh" "$repo_dir"
# 预热用的两个清单模块与「同一配方只有一份」的双检（纯逻辑）：往返一致、碰撞不去重、坏文件整份拒绝、
# 4 线程并发建同一配方只装一份。
s2a_gate_step bash "$android_dir/tools/nvk_warm_modules_test.sh"
# 复核 P1-1 / P1-2：程序借用（ASan 下读收尾释放后的记录）与配方级在途表（同配方合并 / 不同配方不排队）
s2a_gate_step bash "$android_dir/tools/nvk_pipe_flight_test.sh"
# 首用待定表（纯逻辑）与 demand 后台建管线（fake Vulkan）：状态机 / 同键合并 / 有界等待 / late-stuck / P0 不串管线。
s2a_gate_step bash "$android_dir/tools/nvk_pipe_pend_test.sh"
s2a_gate_step bash "$android_dir/tools/nvn_vk_pipe_demand_test.sh"
# 会话关闭留存合同：需求线程未静止时不销毁后端，静止后可重试并安全重开。
s2a_gate_step "$PY" "$android_dir/tests/nvk_session_budget_test.py" --demand-close
# 新层首用管线异步的绘制级用例：真 rp_draw / rp_pass + 真 demand / 待定表 / 判定表，可控建管线闸（跳 / 等 / 预算 / 上限 / 停局丢弃）。
s2a_gate_step bash "$android_dir/tools/nvk_pipe_async_draw_test.sh"
s2a_gate_step bash "$android_dir/tools/nvk_shader_lease_test.sh"
# 按结果合并（#1355 方案 A）：编译结果相同（结果摘要 + 负载长度 + 阶段数 + c1 有无与内容都相同）的身份共用一份
# 着色器记录与句柄、管线随之合并；真 program_cache.c + 真缓存文件，ASan 下覆盖「收尾后不留悬空」。
# 比对键每一项各有「只改这一项 → 不合并」的负对照；--mutants 逐项撤掉生产逻辑，每个都必须变红。
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_alias_test.py"
s2a_gate_step "$PY" "$android_dir/tests/nvk_shader_alias_test.py" --mutants
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/tests/stubs/android/util/Log.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamePack.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameFingerprint.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameStorage.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameResume.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameSession.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameMods.java" \
    "$android_dir/tests/GameSessionTest.java" \
    "$android_dir/tests/GameModsTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameSessionTest
# 资源替换类 mod 导入（口径同 pack/jobs/mods.py）：发布结构 / 别的程序号 / 逃逸与符号链接 / 上限 / 开关 / 删除 / zip64
s2a_gate_step java -ea -cp "$out/java" org.s2arm.android.GameModsTest
# 库页游玩时间一行（口径同 Playtime.swift summary，期望照抄 PlaytimeTests.swift）：分钟 / 小时取整、今天昨天 N 天前、读文件、只算路径的 statePath
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Playtime.java" \
    "$android_dir/tests/PlaytimeTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.PlaytimeTest "$android_dir/app/src/main/res"
# 游戏库「重新打开帧率补丁」（StallOff：停帧救场标记在本款 state 目录，删了下次起局照常开；删不掉报真原因）
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/StallOff.java" \
    "$android_dir/tests/StallOffTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.StallOffTest
# 停帧看门屏上提示（StallNotice：救场横幅定时收、「可能还在加载」等清除才收），文案对 s2a_ui_text.h
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/StallNotice.java" \
    "$android_dir/tests/StallNoticeTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.StallNoticeTest "$android_dir/app/src/main/res" "$android_dir/../runtime/include/s2a_ui_text.h"
# The shell's BootSpec against boot-spec.v1.schema.json (with and without a region).
s2a_gate_step "$PY" - "$android_dir/contracts" "$out/boot-spec" <<'PY'
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("validate", os.path.join(sys.argv[1], "validate.py"))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)
schema = v.load(os.path.join(sys.argv[1], "boot-spec.v1.schema.json"))
for name in sorted(os.listdir(sys.argv[2])):
    v.check_document(v.load(os.path.join(sys.argv[2], name)), schema, name)
print("BootSpec schema: ok")
PY
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamePack.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameInstall.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/SaveArchive.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ImportReceiver.java" \
    "$android_dir/tests/GamePackTest.java" \
    "$android_dir/tests/GameInstallTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GamePackTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameInstallTest
# 资源包直传接收（电脑经 adb forward 直接写进 App 私有目录，不经共享存储）
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ImportReceiver.java" \
    "$android_dir/tests/ImportReceiverTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.ImportReceiverTest
s2a_gate_step "$PY" "$android_dir/tests/deploy_game_test.py"
s2a_gate_step "$PY" "$android_dir/tests/test_app_id_test.py"
s2a_gate_step "$PY" "$android_dir/tests/apk_assemble_test.py"   # 预编引擎 APK 组装：aapt2 造引擎，aapt2 dump / apksigner 判包名 / 显示名 / 图标 / 对齐 / v2 签名
s2a_gate_step "$PY" "$android_dir/tests/ns2_synth_nacp_test.py"   # Switch 2 合成控制数据：应用号非 0、显示版本「?」不让 aapt 停
s2a_gate_step "$PY" "$android_dir/tests/game_icon_test.py"   # 合成控制分区图标、各密度资源、回退与链接拒绝
s2a_gate_step "$PY" "$android_dir/tests/debug_app_admission_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" "$android_dir/tests/GameSessionAdmissionTest.java" "$android_dir/tests/GameFingerprintTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameSessionAdmissionTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.GameFingerprintTest
# 原地来源（游戏文件留在共享存储、只登记元数据）：首次核 SHA、身份命中、改 / 删 / 挪具名拒、只认登记的根、清理不碰原地文件
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" "$android_dir/tests/InPlaceSourceTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.InPlaceSourceTest
s2a_gate_step "$PY" "$android_dir/tests/android_file_stamp_test.py"
s2a_gate_step "$PY" "$android_dir/tests/verified_source_test.py"
s2a_gate_step "$PY" "$android_dir/../graphics/vulkan/wsi/tests/startup_surface_test.py"
# 11-01 接线（扫描分母按 tests 目录后查出没人调）：离线宿主回归 + 固定 NDK 只编译不执行，本机负载 30 时合计约 40 秒。
s2a_gate_step "$PY" "$android_dir/core-link/alt-tests/run.py" --out "$out/alt-tests"
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/writers-gate/renew_model.py" --out "$out/renew-model"
s2a_gate_step "$PY" "$repo_dir/runtime/tests/support/test_thread_verdict.py"
ndk_cc=$(. "$android_dir/tools/ndk_toolchain.sh" && s2a_ndk_clang "$(s2a_ndk_resolve)" 29) || { echo '【红】找不到固定 NDK 29（android/tools/ndk_toolchain.sh）' >&2; exit 1; }
s2a_gate_step "$PY" "$repo_dir/runtime/tests/support/build_pause_wait_android.py" --cc "$ndk_cc" --out "$out/pause-wait-android"
s2a_gate_step "$PY" "$android_dir/native/runtime/fault/tests/run.py" --objects-only --cc "$ndk_cc" --out "$out/fault-objects"
# 栈指针未对齐替做读写（runtime/src/emul_sp.c 安卓信号上下文版）：宿主没有 arm64 安卓信号上下文，只用固定 NDK 编译链接；执行在手机上（见用例文件末尾）
s2a_gate_step "$ndk_cc" -O1 -Wall -Werror -I"$repo_dir/runtime/include" "$android_dir/tests/emul_sp_android_test.c" \
    "$repo_dir/runtime/src/emul_sp.c" "$repo_dir/runtime/src/emul_ld.c" "$repo_dir/runtime/src/emul_st.c" -o "$out/emul-sp-android-test"
s2a_gate_step "$PY" "$android_dir/native/runtime/platform/tests/shutdown_wait_test.py"
s2a_gate_step "$PY" "$android_dir/native/direct-vk-tests/run_shutdown_contract.py"
# Switch 2 SDK 接管（直连 s2a_direct_lookup 对 nv:: / nn::vi 导入的应答）：真 direct_session.c + 假 nvn2 入口，6 组（选择状态 × 游戏代际）+ 内置负对照
#   编译运行段要链 wsi_lease.c（pthread_condattr_setclock）与 direct/selection.c（getrandom），只在 Linux；非 Linux 宿主只跑接线检查（纯源码次序）并登记未测
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step "$PY" "$android_dir/native/direct-vk-tests/run_ns2_lookup.py"
else
    s2a_gate_step "$PY" "$android_dir/native/direct-vk-tests/run_ns2_lookup.py" --wiring-only
    s2a_gate_note "未测：run_ns2_lookup.py 的 6 组查找与负对照（链 wsi_lease.c 的 pthread_condattr_setclock、direct/selection.c 的 getrandom）需 Linux；接线检查已跑"
fi
# App 纯 Java 层整体编译：抓 import 漏缺 / 签名不匹配这类编译错（#38 GameLibrary 漏 import 让 main 编不过就是这类）。
# SDK 依赖的类（GameLibrary / MainActivity / NativeBridge / BootSpecSource / TouchPadView 等 import android.*）需 Android SDK，
# 留给带 SDK 的 Gradle 构建，不在无设备门禁编。新增纯 Java 类时补到这份清单。
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/tests/stubs/android/util/Log.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchPad.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/TouchLayoutStore.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameMetadata.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameSession.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/RuntimeLibrary.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamepadState.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/Json.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootContract.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/BootSpec.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GamePack.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameFingerprint.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/GameInstall.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/ImportReceiver.java" \
    "$android_dir/app/src/main/java/org/s2arm/android/NativeRewriter.java"
# J09-S1：纯合成密钥；长空白回归由 PlayerKeysTest 启独立 JVM，10 秒限时并回收自身子进程。
# 同入口区分系统祖先软链与 App 数据根/files/中间目录软链，并核旧权限恢复及 hardlink 不改 inode。
s2a_gate_step javac -encoding UTF-8 -d "$out/java" \
    "$android_dir/app/src/main/java/org/s2arm/android/PlayerKeys.java" \
    "$android_dir/tests/PlayerKeysTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.PlayerKeysTest
echo "App 纯 Java 类编译：ok"
# Portable preparation regressions are required by the platform gate. The
# NO_NATIVE flag above only excludes the Linux runtime CTest/mremap tests.
s2a_gate_step "$PY" "$android_dir/tests/native_rewriter_test.py"
s2a_gate_step "$PY" "$android_dir/tests/native_rewriter_packaging_test.py" --format-fixture
s2a_gate_step "$PY" "$android_dir/contracts/validate.py"
s2a_gate_step "$PY" "$android_dir/tests/contract_validation_test.py"
s2a_gate_step "$PY" "$android_dir/tests/copy_3d_depth_test.py"
s2a_gate_step "$PY" "$android_dir/tests/guest_window_crop_test.py"
s2a_gate_step "$PY" "$android_dir/core-runtime/metrics-tests/cli_test.py"
s2a_gate_step bash "$android_dir/core-runtime/fps-tests/test.sh"
s2a_gate_step "$PY" "$android_dir/tests/run_manifest_test.py"
s2a_gate_step "$PY" "$android_dir/tests/game_process_manifest_test.py"
s2a_gate_step javac -encoding UTF-8 -d "$out/java" -cp "$out/java" \
    "$android_dir/app/src/androidTest/java/org/s2arm/android/SessionLogEvidence.java" \
    "$android_dir/app/src/androidTest/java/org/s2arm/android/SessionExitEvidence.java" \
    "$android_dir/tests/SessionExitEvidenceTest.java" \
    "$android_dir/tests/SessionLogEvidenceTest.java"
s2a_gate_step java -cp "$out/java" org.s2arm.android.SessionLogEvidenceTest
s2a_gate_step java -cp "$out/java" org.s2arm.android.SessionExitEvidenceTest
s2a_gate_step "$PY" "$android_dir/tests/job_contract_test.py"
s2a_gate_step "$PY" "$android_dir/tests/sysdata_pack_test.py"
s2a_gate_step "$PY" "$android_dir/tests/pack_layout_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_pack_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_library_test.py"
s2a_gate_step "$PY" "$android_dir/tests/shader_pack_test.py"
s2a_gate_step "$PY" "$android_dir/tests/direct_qa_compare_test.py"
s2a_gate_step "$PY" "$android_dir/tests/mutex_timedlock_test.py"   # macOS 无 pthread_mutex_timedlock：可移植实现的超时 / 获锁 / 不空转
s2a_gate_step "$PY" "$android_dir/tests/vulkan_modules_test.py"
s2a_gate_step "$PY" "$android_dir/tests/filecopy_test.py"

# 拆线程主机套件（PR #498）：counter/audio/metrics/Runtime.Tests 跨平台；run_lifecycle 仅 Linux。
# 不进默认门禁的 control-tests 十连、writers-gate、run_thread 见 tests/gate_not_default.tsv。
s2a_gate_step "$PY" "$android_dir/tests/verify_apk_test.py"
s2a_gate_step "$PY" "$android_dir/tests/threaded_python_selection_test.py"
s2a_gate_step "$PY" "$android_dir/tests/gate_failure_summary_test.py"
# 主机测试输出目录隔离（#919）：并行门禁与上一次被中断的残留都不得成为本次输入。
s2a_gate_step "$PY" "$android_dir/tests/host_output_isolation_test.py"
s2a_gate_step bash "$android_dir/tests/threaded_host_suites.sh"
# APK 内存装载器（macOS ARM64 + 真实 Apple TSD 夹具；#1309 接线，只在 macOS 跑）
if [ "$(uname -s)" = Darwin ]; then
    s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/apk-loader/run.py" --out "$out/apk-loader"
fi
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/readonly-copy/texture_read_test.py"
# 共享 guest 入口的主线程 fault 状态准入顺序（真实 guest_main.c；fault/SDK 端点为合成夹具；11-01 #1309 接线）。
s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/run_guest_entry.py" --out "$out/guest-entry"
# 主 guest 运行态发布（#961）：真实 HostApi / runtime / guest_main / session / direct_prompt 一起编链，带 5 个回退负对照。
#   GNU ld --wrap 与 host_android.c 的 eventfd 只在 Linux；Android 日志头取固定 NDK 29 的真头。
if [ "$(uname -s)" = Linux ]; then
    s2a_gate_step "$PY" "$android_dir/core-link/runtime-tests/run_save_running.py" --out "$out/save-running" \
        --ndk-include "${ndk_cc%/bin/*}/sysroot/usr/include"
else
    s2a_gate_note "未测：run_save_running.py（主 guest 运行态发布）需 Linux（GNU ld --wrap、eventfd）"
fi
# Q19/#530：C# 绑定入口 vs Android 单库实际链接定义（fixture + 当前树正例）。
s2a_gate_step python3 "$android_dir/core-link/link_symbols.py"
s2a_gate_step "$PY" "$android_dir/core-link/tests/t_link_symbols.py"
# Cross-compile and partially link the real game loader/provider with the pinned NDK.
s2a_gate_step "$PY" "$android_dir/core-link/test_game_core_closure.py"
# #1309：fault 的 10 个真实 Bionic API34 对象只编不链（crash.c/thread.c 按生产 CMake 带 _GNU_SOURCE）；固定 NDK，不上设备。
s2a_gate_step "$PY" "$android_dir/native/runtime/fault/tests/run_objects.py" --out "$out/fault-owner-objects"
# 真实 NDK 共享链接 / 符号可见性回归（#1309）：entry.S 惰性取核钩子引用的
# g_lazy_core / s2a_guest_return 由测试夹具补齐，生产新增外部符号时这里同步跟平。
# NDK 由测试自己按 link_symbols.find_ndk() 固定解析 29.0.14206865，不信任只按目录判断的环境变量。
s2a_gate_step "$PY" "$android_dir/native/runtime/fault/tests/run_xstack_link.py" --out "$out/xstack-link"
# Versioned managed-probe ELF contract; same pinned NDK, no AOT or device.
s2a_gate_step "$PY" "$android_dir/cs/savecommit/tests/apk/unit/managed_elf_test.py"

# qa.sh 自测：验证门禁脚本自身的编排逻辑与 preflight 拒绝路径。纯 fixture，不需要 Docker。
s2a_gate_step bash "$android_dir/tests/qa_self_test.sh"
# 10-29 基础设施线接线：其余四支已在上方运行；这里保留新的设备回合契约。
s2a_gate_step "$PY" "$android_dir/tests/device_round_test.py"
# #1449 等价比对工具：纯主机 Python，合成帧 + 负对照，不接触设备。
s2a_gate_step "$PY" "$android_dir/tests/s2a_yuvdiff_test.py"
s2a_gate_step bash "$android_dir/core-runtime/keyboard-tests/run.sh"

# This portable real-backend matrix always runs, including the macOS gate.
# Clear binary overrides only for this test: its private cache must build the
# current source. Production and direct developer tests retain CLI overrides.
# Missing CMake/pinned offline dependencies are errors, never skips/downloads.
echo "Android package C/Python backend matrix: running real rewrites and Java consumers"
(
    unset S2A_REWRITER_C S2A_REWRITER_BUNDLED
    s2a_gate_step "$PY" "$android_dir/tests/make_pack_rewriter_test.py" -v
)

# Offline codec build/packaging regression (source preparation is explicit).
s2a_gate_step python3 "$android_dir/tests/game_library_import_test.py"
# #528 第 15 项：库页「上一局异常结束」提示（真 LibraryActivity + 导入用例的替身；3 个负对照）
s2a_gate_step python3 "$android_dir/tests/last_session_notice_test.py"
s2a_gate_step python3 "$android_dir/codecs/tests/build_test.py"
s2a_gate_step "$PY" "$android_dir/codecs/tests/managed_test.py"
# #1131：宿主原生构建契约——后端路由 / NativeAOT 静态归档校验 / docker↔host 产物等价核对。
s2a_gate_step "$PY" "$android_dir/tests/build_backend_test.py"
# 宿主准备编排：首次（缓存里还没有固定 .NET SDK）先引导 SDK 再算依赖清单，一次写出完成标记。
s2a_gate_step "$PY" "$android_dir/tests/host_prepare_test.py"
s2a_gate_step "$PY" "$android_dir/tests/archive_check_test.py"
s2a_gate_step "$PY" "$android_dir/tests/compare_apks_test.py"
s2a_gate_step "$PY" "$android_dir/tools/test_compare_apks.py"
s2a_gate_step "$PY" "$android_dir/cs/test_nuget_verify.py"
s2a_gate_step "$PY" "$android_dir/cs/test_static_cache.py"
# doctor 源码布局与缓存契约（版本表、NDK 宿主目录、codec recipe-key 缓存）。
s2a_gate_step "$PY" -B "$android_dir/doctor/test_doctor.py"
# 门禁子项键：每步恰好一个键、bash / python 归键一致、挑键宁多勿漏、GATE_ONLY 只跑名单里的键（android/tools/gate_keys.*）
s2a_gate_step "$PY" "$android_dir/tests/gate_keys_test.py"
if [ -n "${S2A_GK_ON:-}" ]; then
    echo "安卓门禁子项键 ${GATE_ONLY:-全部}：跑 ${S2A_GK_RUN} 步、跳过 ${S2A_GK_SKIP} 步"
    [ "$S2A_GK_RUN" -gt 0 ] || { s2a_gate_note "【红】安卓门禁子项键 ${GATE_ONLY:-全部} 一步都没跑（键表与 test.sh 对不上）"; exit 1; }
fi
