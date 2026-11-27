#!/usr/bin/env bash
# D8 第一段验收：nvn_gpu_* Vulkan 后端（缓冲 / 提交 / 读回 / 失败 / 保活 / close）在 CPU 参考实现（Mesa lavapipe）
# 上跑契约用例 C01–C08 + C23 粘滞错误，并开 Khronos validation 层。
#
# 用法：
#   bash android/tests/vk/run.sh              # 默认：lavapipe + validation
#   bash android/tests/vk/run.sh --tsan       # 追加 TSan（C04 发布顺序）
#   bash android/tests/vk/run.sh --asan       # 追加 ASan/UBSan
#   bash android/tests/vk/run.sh --fsr        # FSR1 EASU 专项（算法对照，不接上屏）
#   bash android/tests/vk/run.sh --present-upscale  # 上屏 EASU 开/关夹具（独立进程）
#   bash android/tests/vk/run.sh --clear-capacity  # 48/60 个清除管线键、段完成/放弃与后续绘制
#   bash android/tests/vk/run.sh --pipe-creation-feedback  # 管线创建反馈三形态
#   bash android/tests/vk/run.sh --pipe-call-lifetime  # 在途管线调用阻止 close 拆资源，并允许超时后重试
#   bash android/tests/vk/run.sh --feedback-core    # 附件反馈快照、VVL 与同包负控（独立夹具，见 standalone/）
#   bash android/tests/vk/run.sh --<名>            # android/tests/vk/standalone/<名>.sh 里的任一独立夹具
# 环境：
#   S2A_LAVAPIPE_ICD   指定 lavapipe ICD json（默认找 ~/.local / /usr/share/vulkan/icd.d）
#   S2A_VK_LAYER_DIR   指定 validation 层目录（默认找 ~/.local / /usr/share/vulkan/explicit_layer.d）
# 没装 lavapipe / validation 时不静默跳过（#784）：macOS 一律、Linux 宿主缺原生工具时进共享离线容器（tests/vulkan/），
# 容器也没准备好才退出码 2（红）。
# 默认档先在宿主跑真编译器链（android/tests/gs_compiler_chain_test.py → build/696-real-compiler）再跑后端用例。链要 dotnet
# （.NET 10 SDK，在 PATH 上）、固定的 NDK 29.0.14206865（设 ANDROID_NDK_HOME，或放在 $ANDROID_HOME/ndk/、~/Android/Sdk/ndk/ 下）、
# 首次联网还原 NuGet，所以只能在宿主上出（macOS 每次重出，Linux 缺 program.json 才出）；离线容器里跑不了。
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
mode=${1:-}
# 独立夹具：android/tests/vk/standalone/<模式名>.sh（模式名不带前导 --），单独 `--<模式名>` 只跑它，默认档逐个跑。
#   新夹具只加一个文件，不改本脚本：原先每支 PR 都往下面的模式名单和分派段里加一行，同一天十几支在这几行上互相冲突。
standalone_mode() { [ -n "$mode" ] && [ -f "$repo/android/tests/vk/standalone/${mode#--}.sh" ]; }
# 窄 shim 的 wrap 直通用例（#1091 复审）：不设驱动时 5 个入口必须逐条落 __real_*。
# 只依赖编译器与仓内 Vulkan 头，不需要 lavapipe / validation，放在容器分流之前无条件跑。
if [ "$(uname -s)" = Linux ]; then
    shim_out="$repo/build/vk-loader-shim"
    mkdir -p "$shim_out"
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror \
        -DS2A_ANDROID_VK_LOADER_SHIM=1 \
        -I "$repo/graphics/nvn/third_party/vulkan-headers/include" \
        -I "$repo/android/native/direct" -I "$repo/android/native" \
        "$repo/android/native/vk_loader_shim.c" \
        "$repo/android/tests/vk/vkfake_driver_stub.c" "$repo/android/tests/vk/t_loader_shim_wrap.c" \
        -Wl,--wrap=vkEnumerateInstanceVersion \
        -Wl,--wrap=vkEnumerateInstanceExtensionProperties \
        -Wl,--wrap=vkEnumerateInstanceLayerProperties \
        -Wl,--wrap=vkCreateInstance \
        -Wl,--wrap=vkGetInstanceProcAddr \
        -o "$shim_out/t_loader_shim_wrap"
    timeout 30 "$shim_out/t_loader_shim_wrap"
else
    echo "窄 shim wrap 直通用例：$(uname -s) 的链接器不支持 -Wl,--wrap，改在 Linux / 离线容器里跑（本机跳过）" >&2
fi
# TBUF 真编译器链（build/tbuf-real-red）在宿主出料：默认档与 --tbuf-compiler-core 都要出。默认档进容器后在容器里递归进
#   --tbuf-compiler-core，容器里设了 S2A_VULKAN_IMAGE_ID 不出料、只用宿主出好的产物——宿主默认档不先出，容器里就没有产物、
#   回放 rc=2（11-16 #1300 把默认档的出料去掉后 main 上 android_vk_test 连红即此）。
# 出料口径：缺省每次重出。只有模拟机型档（sim_tiers.sh 设 S2A_TBUF_REUSE=1，同一检出连跑十几次 run.sh）才复用，
#   且三样产物（两份 json + 编译器 CLI）都在才复用，缺一样照样重出。
# 链本身的断言在 android/test.sh 的门禁里照跑，这里只要它的产物。
tbuf_cli=$repo/graphics/tools/direct/compiler/cli/bin/Release/net10.0/S2A.DirectCompiler.Cli.dll
if { [ "$mode" = "--tbuf-compiler-core" ] || [ -z "$mode" ]; } && [ -z "${S2A_VULKAN_IMAGE_ID:-}" ] &&
   { [ "${S2A_TBUF_REUSE:-0}" != 1 ] || [ ! -f "$repo/build/tbuf-real-red/production.json" ] ||
     [ ! -f "$repo/build/tbuf-real-red/profile.json" ] || [ ! -f "$tbuf_cli" ]; }; then
    python3 "$repo/android/tests/tbuf_compiler_chain_test.py"
fi
if { [ "$mode" = "--shown-only" ] || [ "$mode" = "--descriptor-indexing" ] || standalone_mode || [ "$mode" = "--compute-read-core" ] || [[ "$mode" == --feedback-read* ]] || [ "$mode" = "--compute-workgroup" ] || [[ "$mode" == --pool-table* ]] || [ -z "$mode" ] || [[ "$mode" == --tbuf-* ]]; } && [ -z "${S2A_VULKAN_IMAGE_ID:-}" ]; then
    . "$repo/android/tools/ndk_toolchain.sh"
    ndk=$(s2a_ndk_resolve) || { echo 'NDK 29 missing' >&2; exit 2; }
    grep -q '^Pkg.Revision = 29\.0\.14206865$' "$ndk/source.properties" || { echo 'NDK 29.0.14206865 required' >&2; exit 2; }
    tag=$(s2a_ndk_prebuilt_tag "$ndk") || exit 2
    header_dir="$ndk/toolchains/llvm/prebuilt/$tag/sysroot/usr/include/vulkan"
    [ -f "$header_dir/vulkan_core.h" ] || { echo 'NDK 29 Vulkan headers missing' >&2; exit 2; }
    mkdir -p "$repo/build/c883-ndk"
    cp -R "$header_dir" "$repo/build/c883-ndk/"
    cp -R "$(dirname "$header_dir")/vk_video" "$repo/build/c883-ndk/"
fi
# Isolated production replay indirect test. Exit before every legacy test path.
# --zero-attr-core（#977）：同一套真 rp_draw + 真后端，跑零属性用例 t_replay_zero_attr.c。
if [ "$mode" = "--indirect-core" ] || [ "$mode" = "--zero-attr-core" ]; then
    if [ "$mode" = "--indirect-core" ]; then
        core_test=t_replay_indirect; core_shaders=(replay_null_storage.vert replay_front_face.vert solid_green.frag quad.vert quad.frag count_args.comp replay_drv0.vert replay_drv0.frag replay_drv0_args.comp replay_binst.vert replay_binst.frag replay_binst_args.comp)
    else
        core_test=t_replay_zero_attr; core_shaders=(zattr.vert quad.frag)
    fi
    out="$repo/build/vk-${mode#--}"; mkdir -p "$out/shaders"
    glslc_bin=${GLSLC:-glslc}
    : "${S2A_LAVAPIPE_ICD:?set S2A_LAVAPIPE_ICD for isolated replay test}"
    : "${S2A_VK_LAYER_DIR:?set S2A_VK_LAYER_DIR for isolated replay test}"
    for shader in "${core_shaders[@]}"; do
        "$glslc_bin" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader" -o "$out/shaders/$shader.spv"
    done
    "$glslc_bin" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/depth_state.vert.spv"
    inc=(); for d in android/tests/vkhost graphics/nvn/third_party/vulkan-headers/include graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/tex graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
    src=("$repo/android/tests/vk/$core_test.c")
    for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
    src+=("$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c")
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=nvn_gpu_draw -lvulkan -lpthread -lm -o "$out/$core_test"
    export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    set +e
    "$out/$core_test" "$out/shaders" 2>&1 | tee "$out/run.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/run.log"; then
        echo "【红】隔离回放用例失败或有 Vulkan 校验错误（rc=${rc}）" >&2
        exit 1
    fi
    exit 0
fi
if [ "$mode" = "--compute-read-core" ] && [ "$(uname -s)" = Linux ]; then
    exec bash "$repo/android/tests/vk/run_compute_read.sh"
fi
if [ "$(uname -s)" = Darwin ]; then
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    if [ -z "$mode" ]; then python3 "$repo/android/tests/gs_compiler_chain_test.py"; fi
    exec bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh "$@"
fi
# 以下是 Linux：在宿主上（原生跑；缺原生工具时转容器，见下），或已在 tests/vulkan/run.sh 起的离线容器里（带 S2A_VULKAN_IMAGE_ID）。
in_container=${S2A_VULKAN_IMAGE_ID:+1}
if [ -z "$mode" ] && [ ! -f "$repo/build/696-real-compiler/compiled/program.json" ]; then
    if [ -n "$in_container" ]; then
        echo "【红】缺 build/696-real-compiler/compiled/program.json：真编译器链要 dotnet、NDK 29、首次联网还原 NuGet，离线容器里跑不了。" >&2
        echo "     先在宿主上出链：python3 android/tests/gs_compiler_chain_test.py；或直接运行 bash android/tests/vk/run.sh（宿主出链、再进容器）。" >&2
        exit 2
    fi
    if ! command -v dotnet >/dev/null 2>&1; then
        echo "【红】找不到 dotnet：真编译器链要 .NET 10 SDK（版本见 compat/tree/global.json），请装好并放到 PATH 上。" >&2
        exit 2
    fi
    if ! PYTHONPATH="$repo/android/core-link" python3 -c 'import sys, link_symbols; sys.exit(0 if link_symbols.find_ndk() else 1)'; then
        echo "【红】找不到 NDK 29.0.14206865：真编译器链要用它的 Vulkan 头文件，不联网下载、不跳过，其他版本的 NDK 不认。" >&2
        echo "     装好该版本后设 ANDROID_NDK_HOME=<NDK 目录>，或放到 \$ANDROID_HOME/ndk/29.0.14206865、~/Android/Sdk/ndk/29.0.14206865。" >&2
        exit 2
    fi
    python3 "$repo/android/tests/gs_compiler_chain_test.py"
fi

find_icd() {
    if [ -n "${S2A_LAVAPIPE_ICD:-}" ]; then printf '%s\n' "$S2A_LAVAPIPE_ICD"; return; fi
    for f in "$HOME/.local/share/vulkan/icd.d/lvp_icd.json" "$HOME/.local/share/vulkan/icd.d/lvp_icd."*.json \
             /usr/share/vulkan/icd.d/lvp_icd.json /usr/share/vulkan/icd.d/lvp_icd.*.json; do
        [ -f "$f" ] && { printf '%s\n' "$f"; return; }
    done
}
find_layer() {
    if [ -n "${S2A_VK_LAYER_DIR:-}" ]; then printf '%s\n' "$S2A_VK_LAYER_DIR"; return; fi
    for d in "$HOME/.local/share/vulkan/explicit_layer.d" /usr/share/vulkan/explicit_layer.d; do
        [ -f "$d/VkLayer_khronos_validation.json" ] && { printf '%s\n' "$d"; return; }
    done
}

icd=$(find_icd || true)
layer=$(find_layer || true)
need_glslc=1
if [ "$mode" = "--pipe-call-lifetime" ]; then need_glslc=0; fi
# 宿主缺原生 Vulkan 工具（lavapipe / validation / glslc 缺任一）：同 macOS，链已在上面出好，进共享的离线容器跑。
# 容器没准备好（tests/vulkan/run.sh 退 75）就保持下面原来的红，只多提示一句；容器里跑出的结果（含红）原样退出。
if [ -z "$in_container" ] && { [ -z "$icd" ] || [ -z "$layer" ] || { [ "$need_glslc" = 1 ] && [ -z "${GLSLC:-$(command -v glslc || true)}" ]; }; }; then
    if [ "$need_glslc" = 1 ]; then
        echo "本机缺原生 Vulkan 工具（lavapipe / validation / glslc），改走共享离线容器（同 macOS）" >&2
    else
        echo "本机缺原生 Vulkan 工具（lavapipe / validation），改走共享离线容器（同 macOS）" >&2
    fi
    export S2A_VULKAN_CACHE="$repo/build/vulkan-cache"
    rc=0
    bash "$repo/tests/vulkan/run.sh" android/tests/vk/run.sh "$@" || rc=$?
    [ "$rc" = 75 ] || exit "$rc"
    echo "     容器环境也没准备好（见上）：装好原生工具，或先显式运行 bash tests/vulkan/prepare.sh" >&2
fi
if [ -z "$icd" ]; then
    echo "【红】找不到 lavapipe ICD：本用例要求 CPU 参考实现（Mesa lavapipe），不静默跳过。" >&2
    echo "     Arch: 解包 vulkan-swrast 后设 S2A_LAVAPIPE_ICD=<dir>/lvp_icd.json" >&2
    exit 2
fi
if [ -z "$layer" ]; then
    echo "【红】找不到 VK_LAYER_KHRONOS_validation：验收要求 0 条 validation 报错。" >&2
    echo "     Arch: 解包 vulkan-validation-layers 后设 S2A_VK_LAYER_DIR=<dir>" >&2
    exit 2
fi

glslc=${GLSLC:-$(command -v glslc || true)}
if [ -z "$glslc" ] && [ "$need_glslc" = 1 ]; then
    echo "【红】找不到 glslc：纹理三角形用例要现编测试着色器（glslang / shaderc）。" >&2
    exit 2
fi


if [ "$mode" = "--compute-workgroup" ] || [ -z "$mode" ]; then
    S2A_LAVAPIPE_ICD="$icd" S2A_VK_LAYER_DIR="$layer" GLSLC="$glslc" bash "$repo/android/tests/vk/workgroup.sh"
    [ -z "$mode" ] || exit 0
fi

# Descriptor indexing capability handshake: use real NDK 29 Vulkan core types,
# a real device and validation; test wrappers only remove advertised capabilities.
if [ "$mode" = "--descriptor-indexing" ]; then
    out="$repo/build/vk-descriptor-indexing"; mkdir -p "$out"
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror \
        -Wno-misleading-indentation -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
        -I "$repo/android/tests/vkhost" -I "$repo/build/c883-ndk" -I "$repo/android/native/direct" \
        "$repo/android/tests/vk/t_descriptor_indexing.c" "$repo/android/native/direct/vk_device.c" \
        "$repo/android/native/direct/device_caps.c" -Wl,--gc-sections \
        -Wl,--wrap=vkEnumerateDeviceExtensionProperties -Wl,--wrap=vkGetPhysicalDeviceFeatures2 \
        -Wl,--wrap=vkCreateDevice -lvulkan -lpthread -lm -o "$out/test"
    set +e
    VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" \
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation timeout 120 "$out/test" 2>&1 | tee "$out/run.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/run.log"; then
        echo "【红】descriptor indexing: rc=$rc or Vulkan validation error" >&2
        exit 1
    fi
    exit 0
fi

# Pool-table contract fixture shares this validated Vulkan container entry.
# 上面 find_icd / find_layer 已按通配找到 lavapipe 与 validation 层（各发行版 ICD 文件名不一，如 lvp_icd.json / lvp_icd.x86_64.json），
#   原样传给池表用例，只留这一处负责找（11-17：池表用例自己只认 lvp_icd.x86_64.json，CI 上找不到、退 2，contract-sim 四片全红）
export S2A_LAVAPIPE_ICD="$icd" S2A_VK_LAYER_DIR="$layer"
if [ "$mode" = --feedback-read-mask-repro ]; then   # 独立夹具 feedback-read 的诊断变体（带参数，不在默认档）
    exec bash "$repo/android/tests/vk/standalone/feedback-read.sh" --mask-repro
fi
if standalone_mode; then
    exec bash "$repo/android/tests/vk/standalone/${mode#--}.sh"
fi
if [ "$mode" = --copy-t2b-core ]; then
    exec bash "$repo/android/tests/vk/run_replay_copy_t2b.sh"
elif [ "$mode" = --clear-tex-core ]; then
    exec bash "$repo/android/tests/vk/run_replay_clear_tex.sh"
fi
if [ -z "$mode" ]; then
    for f in "$repo"/android/tests/vk/standalone/*.sh; do bash "$f"; done
fi
if [[ "$mode" == --pool-table* ]]; then
    exec bash "$repo/android/tests/vk/pool_table.sh" "${mode#--pool-table}"
fi
if [ -z "$mode" ]; then
    bash "$repo/android/tests/vk/pool_table.sh"
fi

# FSR1 EASU 专项（--fsr）：合成图 1280x720 → 2133x1200，compute 版 EASU 对照 CPU 双线性。
# 与上屏路径（backend_vulkan_easu.inc 的片元版）各自独立；这里只验算法更锐且无错。
# 着色器出处与 MIT 许可见 shaders/fsr_easu.LICENSE.txt。
if [ "$mode" = "--fsr" ]; then
    fsr_out="$repo/build/fsr-spike"; mkdir -p "$fsr_out"
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/fsr_easu.comp" -o "$fsr_out/fsr_easu.comp.spv"
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror \
        -I "$repo/android/tests/vkhost" \
        -I "$repo/graphics/nvn/third_party/vulkan-headers/include" \
        "$repo/android/tests/vk/t_fsr_upscale.c" \
        -lvulkan -lm -o "$fsr_out/t_fsr_upscale"
    echo "lavapipe: $icd"
    echo "validation: $layer"
    set +e
    VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" \
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
        timeout 120 "$fsr_out/t_fsr_upscale" "$fsr_out" "$fsr_out" 2>&1 | tee "$fsr_out/run.log"
    rc=${PIPESTATUS[0]}
    set -e
    bad=$(grep -cE "Validation Error|VUID-" "$fsr_out/run.log" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
        echo "【红】FSR1 EASU 专项：rc=${rc}，validation 报错 ${bad} 条" >&2
        exit 1
    fi
    grep -q '^device: llvmpipe' "$fsr_out/run.log" || { echo '【红】不是 lavapipe 设备' >&2; exit 1; }
    grep -q '^FSR_SPIKE: PASS' "$fsr_out/run.log" || { echo '【红】FSR 专项未 PASS' >&2; exit 1; }
    echo "【绿】FSR1 EASU 专项：边缘指标达门槛、无 NaN/越界、validation 0 条；对照图在 build/fsr-spike/"
    exit 0
fi

# 临时目录：离线容器里（带 S2A_VULKAN_IMAGE_ID）放容器自己的 /tmp，不放宿主挂进来的 /src（macOS colima 的 virtiofs）。
# 放 /src 时整套大编译（存储缓冲窗口等夹具十几个源文件、-g）的 cc 临时文件间歇报「cannot open /src/build/vk-tests/cc*.s for writing:
# Permission denied」（11-19 Mac 构建机上三次，都在同一处），而且走 virtiofs 写也慢；各夹具的产出目录跑完即删，不需要留在仓库里。
if [ -n "${S2A_VULKAN_IMAGE_ID:-}" ]; then vk_tmp=/tmp/s2a-vk-tests; else vk_tmp="$repo/build/vk-tests"; fi
mkdir -p "$vk_tmp"
export TMPDIR="$vk_tmp"
out=$(mktemp -d "$TMPDIR/s2a-nvn-vk.XXXXXX")
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/shaders"
if [ "$mode" != "--pipe-call-lifetime" ]; then
for s in vs_store.vert vs_load.vert vs_clip.vert vs_cull.vert msaa_read.frag count_args.comp logic_float.frag logic_uint.frag logic_float_mrt.frag logic_uint_mrt.frag ubo_dyn.frag ubo_dyn9.frag ubo_dyn34.frag ubo_high.frag gs_probe.vert gs_probe.geom gs_probe.frag gs_tstrip.vert gs_tstrip.geom gs_tstrip.frag tri.vert tri.frag tri_combined.frag tri3.vert tri_solid.frag tri_invert.frag solid_green.frag solid_blue.frag out1_green.frag tex_u.frag quad.vert quad.frag inst.vert div.vert point.vert cube_at.comp quad2.frag arr.frag cube.frag cubearr.frag tex3d.frag comp.comp pushc.frag pushc.comp tbuf.comp itbuf.comp itbuf_unknown.comp itbuf_unknown_read.comp img.comp img_rgba8.comp tsamp.comp arr2.frag separate.vert samp_at.comp samp_at_i.comp samp_at_u.comp separate.frag; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$s" -o "$out/shaders/$s.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/tbuf_replay.frag" -o "$out/shaders/tbuf_replay.frag.spv"
fi

inc=(-I "$repo/android/tests/vkhost"
     -I "$repo/graphics/nvn/third_party/vulkan-headers/include"
     -I "$repo/graphics/nvn"
     -I "$repo/graphics/nvn/replay"
     -I "$repo/graphics/nvn/gfx/nvn"
     -I "$repo/graphics/nvn/gfx/nvn/include"
     -I "$repo/android/native"
     -I "$repo/android/native/nvn"
     -I "$repo/android/native/direct"
     -I "$repo/runtime/include")
# 计算派发前的屏障（#978 与其跟帖；真 rp_dispatch / rp_buf / 后端，开同步校验；臂的含义见 t_replay_dispatch_barrier.c 文件头）：
#   --dispatch-barrier      传输写 → 游戏屏障 → 派发：读回 i*6，同步校验 0 条；
#   --dispatch-barrier-neg  检测器负控：测试钩子关掉后端派发前那道屏障、也不发游戏屏障，同步校验必须报写后读冒险；
#   --xfer-dispatch         不发游戏屏障的四臂（none / resync / xseg / dd）：同步校验 0 条、读回 i*6，派发前后端自己补了屏障、连续派发之间 0 道。
# 原始日志不回显、只报条数（容器会把日志里的校验报错当成失败），留在 build/978-tests/。
if [ "$mode" = "--dispatch-barrier" ] || [ "$mode" = "--dispatch-barrier-neg" ] || [ "$mode" = "--xfer-dispatch" ]; then
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/bar_double.comp" -o "$out/shaders/bar_double.comp.spv"
    src=("$repo/android/tests/vk/t_replay_dispatch_barrier.c")
    for f in graphics/nvn/replay/rp_draw.c graphics/nvn/replay/rp_buf.c graphics/nvn/replay/rp_state.c graphics/nvn/replay/nvn_unimpl.c android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
    cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 "${inc[@]}" "${src[@]}" -Wl,--gc-sections -Wl,--wrap=vkGetDeviceProcAddr -lvulkan -lpthread -lm -o "$out/dispatch-barrier"
    mkdir -p "$repo/build/978-tests"
    db_red=0
    db_arm() {   # $1 臂；$2 期望的写后读冒险：0 或 hit（≥1）；$3 额外的屏障数判据（awk 表达式，变量 before / between），可空
        local arm=$1 want=$2 cond=${3:-} rc raw other line before between tag=$1 reads
        [ -n "${db_log_off:-}" ] && tag="$1-dirtylog-off"   # db_log_off=1：开关 nvk_buf_dirtylog 关那一档（逐页退回路径）
        tag="dq$db_dq-$tag"   # 两级录制 nvk_dq 关 / 开两档都跑（夹具在 fence 之后按真实 vkCmd 快照数屏障，不靠关 DQ）
        set +e
        VK_DRIVER_FILES="$icd" VK_LAYER_PATH="$layer" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
            env S2A_REPLAY_DB_DQ=$db_dq ${db_log_off:+S2A_REPLAY_DIRTYLOG_OFF=1} timeout 120 "$out/dispatch-barrier" "$out/shaders" "$arm" > "$out/db-$tag.log" 2>&1
        rc=$?
        set -e
        cp "$out/db-$tag.log" "$repo/build/978-tests/$tag.log"
        # 传输写 → 派发缺屏障这一对：开了 syncval_shader_accesses_heuristic 后校验层把派发对存储缓冲的访问按整段写算，同一对报成
        # WRITE-AFTER-WRITE（「先前由 vkCmdCopyBuffer 写」）而不是 READ-AFTER-WRITE；两种都记作这一对冒险，其余报错照旧单独数
        raw=$(grep -cE 'SYNC-HAZARD-(READ|WRITE)-AFTER-WRITE' "$out/db-$tag.log" || true)
        other=$(grep -E 'Validation Error|VUID-|SYNC-HAZARD-' "$out/db-$tag.log" | grep -vcE 'SYNC-HAZARD-(READ|WRITE)-AFTER-WRITE' || true)
        line=$(grep '^dispatch-barrier ' "$out/db-$tag.log" || true)
        reads=$(printf '%s\n' "$line" | sed -n 's/.*dirtylog_reads=\([0-9]*\).*/\1/p')
        echo "臂 ${tag}：写后读 / 写后写冒险 $raw 条、其余报错 $other 条、rc=${rc}；${line#dispatch-barrier }"
        # 受跟踪的臂（resync 以外）：缺省开那一档必须真读了日志，关那一档一次都没读
        if [ "$arm" != resync ] && [ -n "$line" ]; then
            if [ -n "${db_log_off:-}" ]; then [ "${reads:-x}" = 0 ] || { echo "  【红】臂 ${tag}：开关关了还在读脏页日志（$reads 次）" >&2; db_red=1; return; }
            else [ "${reads:-0}" -ge 1 ] 2>/dev/null || { echo "  【红】臂 ${tag}：缺省开却没走脏页日志（${reads:-?} 次）" >&2; db_red=1; return; }; fi
        fi
        before=$(printf '%s\n' "$line" | sed -n 's/.*barriers_before_dispatch=\([0-9]*\).*/\1/p'); between=$(printf '%s\n' "$line" | sed -n 's/.*barriers_between_dispatches=\([0-9]*\).*/\1/p')
        grep -q '^device: llvmpipe' "$out/db-$tag.log" || { echo "  【红】臂 $tag 不是 lavapipe 设备" >&2; db_red=1; return; }
        if [ "$other" -ne 0 ] || [ -z "$line" ]; then echo "  【红】臂 ${tag}：其余报错或没跑完" >&2; db_red=1; return; fi
        if [ "$want" = hit ]; then [ "$raw" -ge 1 ] || { echo "  【红】臂 ${tag}：负控未击中（同步校验没报写后读 / 写后写冒险）" >&2; db_red=1; }; return; fi
        if [ "$rc" -ne 0 ] || [ "$raw" -ne 0 ]; then echo "  【红】臂 ${tag}：读回或同步校验不对" >&2; db_red=1; return; fi
        if [ -n "$cond" ] && ! awk -v before="${before:-x}" -v between="${between:-x}" "BEGIN{exit !($cond)}"; then echo "  【红】臂 ${tag}：屏障数不符（要 ${cond}）" >&2; db_red=1; fi
    }
    for db_dq in 0 1; do
    case "$mode" in
        --dispatch-barrier) db_arm barrier 0 ;;
        --dispatch-barrier-neg) db_arm none-nobar hit 'before == 0' ;;
        --xfer-dispatch) db_arm none 0 'before == 1'; db_arm resync 0 'before >= 2'; db_arm xseg 0 'before == 1'; db_arm dd 0 'before == 1 && between == 0'
                         db_log_off=1; db_arm none 0 'before == 1'; db_arm xseg 0 'before == 1'; db_arm dd 0 'before == 1 && between == 0'; db_log_off= ;;   # 关那一档：受跟踪的三臂走逐页路径再跑一遍
    esac
    done
    if [ "$db_red" -ne 0 ]; then echo "【红】$mode" >&2; exit 1; fi
    echo "【绿】$mode"; exit 0
fi
if [ "$mode" = "--shown-only" ] || [[ "$mode" == --tbuf-* ]]; then
    [ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 Vulkan headers not staged' >&2; exit 2; }
    inc=(-I "$repo/build/c883-ndk" "${inc[@]}")
fi
src=("$repo/android/tests/vk/t_backend_vk.c"
     "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/android/native/capprobe/capprobe_host.c"
     "$repo/android/native/nvn/nvn_vk_format.c"
     "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/nvn/nvn_vk_driver_cache.c"
     "$repo/android/native/direct/sha256.c"
     "$repo/graphics/nvn/replay/nvn_unimpl.c")
inc+=(-I "$repo/graphics/nvn/gfx/tex")
src+=("$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c")
bc_src=("$repo/android/tests/vk/t_bc_decode.c"
     "$repo/android/native/nvn/nvn_vk_bcdec.c"
     "$repo/android/native/direct/vk_device.c"
     "$repo/android/native/direct/gs_path.c"
     "$repo/graphics/nvn/gfx/tex/nvn_bcdec.c")
# 与发行 APK 同一组建设备规则（android/CMakeLists.txt 的 nvn_vk 后端定义此宏）。
cflags=(-D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -DS2A_ANDROID_VK_BACKEND_NVN_VK=1)
if [[ "$mode" == --gpu-deswizzle* ]]; then cflags+=(-DS2A_GPU_DESWIZZLE_TEST=1); fi
cflags+=(-DS2A_LAND_READBACK_TEST=1)
case "$mode" in
    --tsan) cflags+=(-fsanitize=thread) ;;
    --asan) cflags+=(-fsanitize=address,undefined -fno-omit-frame-pointer) ;;
    --shown-only) cflags+=(-DS2A_C883_TEST=1) ;;
    --pipe-call-lifetime) cflags+=(-DS2A_BACKEND_PIPE_CALL_TEST=1) ;;
    ""|--pipe-creation-feedback|--high-binding|--pipe-creation-feedback-neg-valid|--sampler-minmax-only|--c63-same-view|--stencil8-only|--read-texels-core|--ds-upload-only|--gpu-deswizzle|--gpu-deswizzle-neg-height|--gpu-deswizzle-neg-stride|--gpu-deswizzle-neg-barrier|--msaa-only|--msaa-neg-pick|--msaa-neg-pipe|--msaa-neg-sampled|--msaa-neg-attachment|--present-only|--present-upscale|--separate-core|--separate-neg-sampler|--tbuf-core|--tbuf-compiler-core|--tbuf-neg-resolve|--tbuf-neg-format|--tbuf-neg-elements|--gs-only|--gs-compiler|--gs-core|--gs-no-cap|--gs-neg-stage|--gs-neg-flags|--gs-neg-cap|--clear-capacity|--clear-capacity-neg|--land-readback|--cmdpool-only|--cmdpool-neg) ;;
    *) echo "未知参数：${mode}（可用 --tsan / --asan）" >&2; exit 64 ;;
esac
# 撤改只写入临时源码副本；逐项必须红在指定行为断言。
if [[ "$mode" == --gpu-deswizzle-neg-* ]]; then
    python3 - "$mode" "$repo" "$out" <<'PYGPU'
import pathlib, sys
mode, repo, out = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
backend = (repo / 'android/native/nvn/backend_vulkan.c').read_text()
inc = (repo / 'android/native/nvn/backend_vulkan_deswizzle.inc').read_text()
changes = {
    '--gpu-deswizzle-neg-height': ('row, rows, slices, h, d, gobs, (uint32_t)nvn_gob_switch2', 'row, rows, slices, (h ? h - 1 : h), d, gobs, (uint32_t)nvn_gob_switch2'),
    '--gpu-deswizzle-neg-stride': ('row, rows, slices, h, d, gobs, (uint32_t)nvn_gob_switch2', 'row, rows, slices, h, d, (gobs > 1 ? gobs - 1 : gobs), (uint32_t)nvn_gob_switch2'),
    '--gpu-deswizzle-neg-barrier': ('vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);', '(void)barrier;'),
}
before, after = changes[mode]
assert inc.count(before) == 1
variant = out / 'deswizzle-mutant.inc'; variant.write_text(inc.replace(before, after))
a = '#include "backend_vulkan_deswizzle.inc"'; assert backend.count(a) == 1
backend = backend.replace(a, '#include "' + str(variant) + '"')
backend = backend.replace('#include "../../../graphics/nvn/gfx/tex/nvn_deswizzle.h"', '#include "' + str(repo/'graphics/nvn/gfx/tex/nvn_deswizzle.h') + '"')
(out/'deswizzle-mutant.c').write_text(backend)
PYGPU
    src[1]="$out/deswizzle-mutant.c"
fi
if [ "$mode" = --clear-capacity-neg ]; then
    python3 - "${src[1]}" "$out" <<'PYCLEAR'
import pathlib, sys
source, out = map(pathlib.Path, sys.argv[1:])
inc = (source.parent / 'backend_vulkan_clear.inc').read_text()
anchor = '    for (struct clr_pipe_temp *p = c->clear_pipes;'
if inc.count(anchor) != 1:
    raise SystemExit('clear capacity mutation anchor changed')
mutant = out / 'clear-capacity-mutant.inc'
mutant.write_text(inc.replace(anchor, '    if (g_clr.n == CLR_MAX) return VK_NULL_HANDLE;\n' + anchor))
backend = source.read_text()
anchor = '#include "backend_vulkan_clear.inc"'
if backend.count(anchor) != 1:
    raise SystemExit('clear include anchor changed')
(out / 'clear-capacity-mutant.c').write_text(backend.replace(anchor, '#include "' + str(mutant) + '"'))
PYCLEAR
    src[1]="$out/clear-capacity-mutant.c"
fi
if [[ "$mode" == --msaa-neg-* ]]; then
    python3 - "$mode" "${src[1]}" "$out/msaa_mutant.c" <<'PYMSAA'
import pathlib, sys
mode, source, target = sys.argv[1:]
s = pathlib.Path(source).read_text()
changes = {
    '--msaa-neg-pick': ('uint32_t actual = samples <= 16 ? msaa_pick(samples, ip.sampleCounts) : 0;', 'uint32_t actual = 0;'),
    '--msaa-neg-pipe': ('c->pass_natt && p->pass && p->actual_samples != c->pass_samples', 'c->pass_natt && p->pass && 0 && p->actual_samples != c->pass_samples'),
    '--msaa-neg-sampled': ('if (bt && bt->samples == 1 && (bt->target', 'if (bt && 0 && bt->samples == 1 && (bt->target'),
    '--msaa-neg-attachment': ('if (attachment_samples && attachment_samples != n) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "msaa_attachment_mismatch: 颜色附件样本数不一致"); return; }', 'if (0 && attachment_samples && attachment_samples != n) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "msaa_attachment_mismatch: 颜色附件样本数不一致"); return; }'),
}
a, b = changes[mode]
if s.count(a) != 1: raise SystemExit('MSAA negative control anchor changed: ' + mode)
pathlib.Path(target).write_text(s.replace(a, b))
PYMSAA
    src[1]="$out/msaa_mutant.c"
fi

echo "lavapipe: $icd"
echo "validation: $layer"
if [ "$mode" = "--present-only" ]; then
    src[0]="$repo/android/tests/vk/t_present_encoding.c"
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [ "$mode" = "--gs-core" ]; then
    src=("$repo/android/tests/vk/t_replay_gs.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$repo/android/native/gfx_plat/nvk_shader.c" "$repo/android/native/nvn/nvn_vk_prog_warm.c" "$repo/android/native/direct/libpack_vk.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c"
         "$repo/android/native/nvn/nvn_vk_driver_cache.c"
         "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
         "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c" "$repo/android/native/direct/compile_worker.c"
         "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c")
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [ "$mode" = "--separate-core" ] || [ "$mode" = "--separate-neg-sampler" ]; then
    src=("$repo/android/tests/vk/t_replay_separate.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_driver_cache.c" "$repo/android/native/direct/sha256.c")   # 后端的管线缓存落盘（#856）
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [[ "$mode" == --tbuf-* ]]; then
    src=("$repo/android/tests/vk/t_replay_tbuf.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_buf.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_driver_cache.c" "$repo/android/native/direct/sha256.c")
    cflags+=(-ffunction-sections -fdata-sections -Wno-unused-function)
fi
if [ "$mode" = "--tbuf-compiler-core" ]; then
    src=("$repo/android/tests/vk/t_replay_tbuf_compiler.c" "$repo/graphics/nvn/replay/rp_draw.c" "$repo/graphics/nvn/replay/rp_buf.c" "$repo/graphics/nvn/replay/rp_state.c" "$repo/graphics/nvn/replay/nvn_unimpl.c" "$repo/android/native/gfx_plat/nvk_shader.c" "$repo/android/native/nvn/nvn_vk_prog_warm.c" "$repo/android/native/direct/libpack_vk.c"
         "$repo/android/native/nvn/backend_vulkan.c" "$repo/android/native/nvn/nvn_pipe_flight.c" "$repo/android/native/nvn/nvn_vk_pipe_once.c" "$repo/android/native/direct/vk_device.c" "$repo/android/native/direct/gs_path.c" "$repo/android/native/capprobe/capprobe_host.c"
         "$repo/android/native/nvn/nvn_vk_format.c" "$repo/graphics/nvn/gfx/nvn/nvn_fmt_expand.c" "$repo/android/native/nvn/nvn_vk_bcdec.c"
         "$repo/android/native/nvn/nvn_vk_program_result.c" "$repo/android/native/nvn/nvn_vk_compute_request.c" "$repo/android/native/nvn/nvn_vk_driver_cache.c"
         "$repo/android/native/direct/source_store.c" "$repo/android/native/direct/control.c" "$repo/android/native/direct/input_identity.c" "$repo/android/native/direct/texture_facts.c"
         "$repo/android/native/direct/sha256.c" "$repo/android/native/direct/json.c" "$repo/android/native/direct/jv.c" "$repo/android/native/direct/compile_request.c" "$repo/android/native/direct/compile_worker.c"
         "$repo/android/native/direct/offline_library.c" "$repo/android/native/direct/program_cache.c" "$repo/runtime/src/direct_host.c")
fi
if [[ "$mode" == --tbuf-neg-* ]]; then
    python3 - "$mode" "${src[1]}" "$out/tbuf_mutant.c" <<'PYTBUF'
import pathlib, sys
mode, source, target = sys.argv[1:]
s = pathlib.Path(source).read_text()
changes = {
    '--tbuf-neg-resolve': ('if (!rp_buf_resolve(c, (uint64_t)(uintptr_t)t.mem, bytes, &buf, &off)) return skip("tbuf_buffer_unresolved", rec);', 'return skip("tbuf_buffer_unresolved", rec);'),
    '--tbuf-neg-format': ('uint32_t fmt = hv && v.set_format ? v.format : (uint32_t)t.d.format;', 'uint32_t fmt = NVN_FORMAT_RGBA8UI;'),
    '--tbuf-neg-elements': ('uint64_t bytes = (uint64_t)t.d.width * base.bytes;', 'uint64_t bytes = (uint64_t)t.d.width * base.bytes * 2;'),
}
a, b = changes[mode]
if s.count(a) != 1: raise SystemExit('TBUF negative control anchor changed: ' + mode)
pathlib.Path(target).write_text(s.replace(a, b))
PYTBUF
    src[1]="$out/tbuf_mutant.c"
fi
if [ "$mode" = "--separate-neg-sampler" ]; then
    python3 - "$repo/graphics/nvn/replay/rp_draw.c" "$out/rp_draw_mutant.c" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'ts[st][i] = rp_samp_for_tex((int)(sh >> 20), h ? (int)(h & 0xFFFFF) : rp_S.tex_base);'
b = 'ts[st][i] = rp_samp_for_tex((int)(h >> 20), h ? (int)(h & 0xFFFFF) : rp_S.tex_base); (void)sh;'
if s.count(a) != 1: raise SystemExit('sampler negative control anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, b))
PYMUT
    src[1]="$out/rp_draw_mutant.c"
fi
# 负控只修改临时源副本，不关闭 validation，也不改设备特性。
case "$mode" in
    --gs-neg-stage|--gs-neg-flags|--gs-neg-cap)
        python3 - "$mode" "${src[1]}" "$out/backend_mutant.c" <<'PYMUT'
import pathlib, sys
mode, source, target = sys.argv[1:]
s = pathlib.Path(source).read_text()
changes = {
    '--gs-neg-stage': ('if (!d->st[st].sh) continue;', 'if (!d->st[st].sh || st == NVN_GPU_SH_GS) continue;'),
    '--gs-neg-flags': ('if (m[i].stages & (1u << st)) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);', 'if (st != NVN_GPU_SH_GS && (m[i].stages & (1u << st))) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);'),
    '--gs-neg-cap': ('if (s->stage == NVN_GPU_SH_GS && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))', 'if (0 && s->stage == NVN_GPU_SH_GS && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))'),
}
a, b = changes[mode]
if s.count(a) != 1: raise SystemExit('mutation anchor changed: ' + mode)
pathlib.Path(target).write_text(s.replace(a, b))
PYMUT
        src[1]="$out/backend_mutant.c" ;;
esac
# 负控只改源码副本：撤销前置缓冲复用，池会再次累积分配。
if [ "$mode" = "--cmdpool-neg" ]; then
    python3 - "${src[1]}" "$out/cmdpool_mutant.c" <<'PYNEG'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'if (!c->init_cb && vkAllocateCommandBuffers(g.d, &ai, &c->init_cb) != VK_SUCCESS)'
b = 'if (vkAllocateCommandBuffers(g.d, &ai, &c->init_cb) != VK_SUCCESS)'
if s.count(a) != 1: raise SystemExit('C1581 negative control anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, b))
PYNEG
    src[1]="$out/cmdpool_mutant.c"
fi
if [ "$mode" = "--pipe-creation-feedback-neg-valid" ]; then
    python3 - "${src[1]}" "$out/pipe_creation_feedback_mutant.c" <<'PYNEG'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
check = "if (!feedback || !(feedback->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {"
assert source.count(check) == 1
Path(sys.argv[2]).write_text(source.replace(check, "if (!feedback) {"))
PYNEG
    src[1]="$out/pipe_creation_feedback_mutant.c"
fi
# 管线缓存夹具与独立回放入口各有自己的链接期替身；保持后者的既有链接参数。
pc_wrap=(-Wl,--wrap=vkCreatePipelineCache -Wl,--wrap=vkGetPipelineCacheData
         -Wl,--wrap=vkCreateGraphicsPipelines -Wl,--wrap=vkCreateComputePipelines)
case "$mode" in --gs-core|--separate-core|--separate-neg-sampler|--tbuf-*) pc_wrap=() ;; esac
# t_backend_vk.c 也被多个独立夹具直接包含。枚举替身定义与链接参数只在运行三形态时成对加入。
case "$mode" in
    ""|--pipe-creation-feedback*) cflags+=(-DS2A_PIPE_CREATION_FEEDBACK_TEST=1); pc_wrap+=(-Wl,--wrap=vkEnumerateDeviceExtensionProperties) ;;
esac
if [ "$mode" = "--shown-only" ]; then pc_wrap+=(-Wl,--wrap=vkGetDeviceProcAddr); fi
case "$mode" in --gs-core|--separate-core|--separate-neg-sampler|--tbuf-*) ;;
    *) cflags+=(-DS2A_CMDPOOL_TEST=1); pc_wrap+=(-Wl,--wrap=vkBeginCommandBuffer -Wl,--wrap=vkAllocateCommandBuffers -Wl,--wrap=vkFreeCommandBuffers -Wl,--wrap=vkDestroyCommandPool -Wl,--wrap=vkResetCommandPool) ;;
esac
cc "${cflags[@]}" "${inc[@]}" "${src[@]}" -Wl,--wrap=vkAllocateDescriptorSets -Wl,--wrap=vkGetPhysicalDeviceFormatProperties \
    ${pc_wrap[@]+"${pc_wrap[@]}"} -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/t_backend_vk"

export VK_DRIVER_FILES=$icd
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export TSAN_OPTIONS="halt_on_error=0 suppressions=$repo/android/tests/vk/tsan.supp ${TSAN_OPTIONS:-}"
# TSan 档不开 validation 层：层内部用 rwlock + 延迟释放，在 TSan 下会报层自身的竞争，
# 淹没被测代码的报告。validation 由默认档（不带 TSan）单独跑。
if [ "$mode" != "--tsan" ]; then
    export VK_LAYER_PATH=$layer
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT   # 同步校验：缺屏障会报 SYNC-HAZARD-*
    if [ "$mode" = "--pipe-call-lifetime" ]; then
        S2A_TEST_LOGICOP_ONLY=1 timeout 60 "$out/t_backend_vk" "$out/shaders" --pipe-call-lifetime 2>&1 | tee "$out/pipe-call-lifetime.log"
        exit 0
    fi

if [[ "$mode" == --pipe-creation-feedback* ]]; then
    log="$out/${mode#--}.log"
    set +e
    S2A_TEST_LOGICOP_ONLY=1 timeout 60 "$out/t_backend_vk" "$out/shaders" --pipe-creation-feedback 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    set -e
    if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then exit 1; fi
    if [ "$mode" = "--pipe-creation-feedback-neg-valid" ]; then
        [ "$rc" -eq 1 ] &&
            grep -q 'PSO feedback bucket 0 (supported): created=2 valid=2 no_feedback=0 app_hit=2' "$log" &&
            grep -q 'PSO feedback bucket 3 (supported): created=1 valid=1 no_feedback=0' "$log" &&
            grep -q '^PSO feedback failures=2$' "$log"
        exit $?
    fi
    exit "$rc"
fi

if [ "$mode" = "--high-binding" ]; then
    timeout 120 "$out/t_backend_vk" "$out/shaders" --high-binding 2>&1 | tee "$out/high-binding.log"
    if ! grep -q 'HIGH_BINDING failures=0' "$out/high-binding.log" || grep -Eq 'Validation Error|VUID-' "$out/high-binding.log"; then
        echo "【红】绑定号 ≥ 256 的布局用例失败或有校验层报错"
        exit 1
    fi
    echo "【绿】绑定号 ≥ 256（262 与 6 并存）：管线建成、两个 UBO 各取各的值，validation 0 条"
    exit 0
fi
if [ "$mode" = "--land-readback" ]; then
    for dq in 0 1; do
        if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
        timeout 60 "$out/t_backend_vk" "$out/shaders" --land-readback 2>&1 | tee "$out/land-readback-dq${dq}.log"
        if grep -Eq 'Validation Error|SYNC-HAZARD|VUID-' "$out/land-readback-dq${dq}.log"; then
            echo "【红】回写暂存（录制队列 ${dq}）校验层报错"
            exit 1
        fi
    done
    exit 0
fi
if [ "$mode" = "--cmdpool-only" ] || [ "$mode" = "--cmdpool-neg" ]; then
    bad=0
    for dq in 0 1; do
        if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
        set +e
        timeout 120 "$out/t_backend_vk" "$out/shaders" --cmdpool-only 2>&1 | tee "$out/cmdpool-$dq.log"
        rc=${PIPESTATUS[0]}
        set -e
        if ! grep -q '^device: llvmpipe' "$out/cmdpool-$dq.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/cmdpool-$dq.log"; then
            echo "【红】C1581 DQ=$dq 非 lavapipe 或校验错误" >&2; bad=1
        elif [ "$mode" = "--cmdpool-neg" ]; then
            if [ "$rc" -ne 1 ] || ! grep -q 'FAIL .*C1581 池内最高命令缓冲数期望 2' "$out/cmdpool-$dq.log"; then
                echo "【红】C1581 负控 DQ=$dq 没有在预期断言处失败 rc=$rc" >&2; bad=1
            fi
        elif [ "$rc" -ne 0 ] || ! grep -q 'C1581 isolated failures=0' "$out/cmdpool-$dq.log"; then
            echo "【红】C1581 DQ=$dq rc=$rc" >&2; bad=1
        fi
    done
    [ "$bad" -eq 0 ] || exit 1
    echo "【绿】C1581 $mode DQ 开/关，validation 0 条"; exit 0
fi

# 上屏 EASU 开/关夹具（--present-upscale）：独立进程跑一次，避开 surface 单例与 C71 的提交信号序列。
if [ "$mode" = "--present-upscale" ]; then
    set +e
    S2A_TEST_PRESENT_UPSCALE_ONLY=1 timeout 180 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_present_upscale"
    rc=${PIPESTATUS[0]}
    set -e
    bad=$(grep -cE "Validation Error|VUID-" "$out/log_present_upscale" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ] || ! grep -q "PRESENT_UPSCALE isolated failures=0" "$out/log_present_upscale"; then
        echo "【红】上屏 EASU 开关夹具：rc=${rc}，validation 报错 ${bad} 条" >&2
        grep -E "Validation Error|VUID-|PRESENT_UPSCALE" "$out/log_present_upscale" | head -20 >&2
        exit 1
    fi
    grep "PRESENT_UPSCALE" "$out/log_present_upscale" || true
    echo "【绿】上屏 EASU 开关夹具：关走原合成、开走 EASU 通道，validation 0 条"
    exit 0
fi
fi
# 机型能力模拟（可选）：S2A_VK_PROFILE_LAYER_DIR 指 VK_LAYER_KHRONOS_profiles 所在目录时，把它排在 validation
# 之下（离驱动更近），让 lavapipe 按 VK_KHRONOS_PROFILES_* 指定的机型能力档上报；validation 看到的是模拟后的能力。
# 成套的模拟机型档（无 BC / 无 D24 / 无顶点存储 / 无裁剪距离 / 无双源混合 / 无校准时间戳 / 负对照）见 sim_tiers.sh（#429）。
# 设了 VK_LAYER_PATH 时加载器不认 VK_ADD_LAYER_PATH / VK_LOADER_LAYERS_ENABLE，只能在这里拼。
if [ -n "${S2A_VK_PROFILE_LAYER_DIR:-}" ]; then
    export VK_LAYER_PATH=${VK_LAYER_PATH:+$VK_LAYER_PATH:}$S2A_VK_PROFILE_LAYER_DIR
    export VK_INSTANCE_LAYERS=${VK_INSTANCE_LAYERS:+$VK_INSTANCE_LAYERS:}VK_LAYER_KHRONOS_profiles
fi

# 真实探测：独立进程执行控制、GS 单独和两个 GS+遮挡臂；只有四臂全 ok
# 才生成供生产 gs_path 读取的结果。此判定是生产 verdict 的严格子集。
cc -std=c11 -D_DEFAULT_SOURCE -DS2A_CAPPROBE_STANDALONE "${inc[@]}" \
    "$repo/android/native/capprobe/capprobe.c" -lvulkan -o "$out/capprobe"
timeout 90 "$out/capprobe" all --badlist "$repo/android/native/capprobe/known-bad.txt" > "$out/probe.jsonl"
python3 - "$out/probe.jsonl" "$out/probe-result.json" <<'PYPROBE'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip().startswith('{')]
fp = [r for r in rows if r.get('type') == 'fingerprint' and 'vendorID' in r and 'driverVersion' in r]
arms = {r.get('arm'): r.get('outcome') for r in rows if r.get('type') == 'arm'}
required = ('nogsocc', 'noq', 'occ', 'occimp')
if len(fp) != 1 or any(arms.get(a) != 'ok' for a in required) or any(r.get('type') == 'blocked' for r in rows):
    raise SystemExit('真实 GS 探测未通过: ' + repr(rows))
print('真实 GS 探测：' + ', '.join(a + '=' + arms[a] for a in required))
with open(sys.argv[2], 'w') as f:
    json.dump({'source': 'probe', 'gs_occlusion': 'supported'}, f, separators=(',', ':'))
PYPROBE
export S2A_CAPPROBE_RESULT="$out/probe-result.json"

if [[ "$mode" == --msaa-* ]]; then
    set +e
    if [ "$mode" = "--msaa-neg-attachment" ]; then
        mkdir -p "$repo/build/msaa-tests"
        timeout 60 "$out/t_backend_vk" "$out/shaders" --msaa-only >"$repo/build/msaa-tests/negative-attachment.log" 2>&1
        rc=$?
        cp "$repo/build/msaa-tests/negative-attachment.log" "$out/msaa-log"
    else
        timeout 60 "$out/t_backend_vk" "$out/shaders" --msaa-only 2>&1 | tee "$out/msaa-log"
        rc=${PIPESTATUS[0]}
    fi
    set -e
    if [ "$mode" = "--msaa-only" ]; then
        if grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/msaa-log"; then exit 1; fi
        exit "$rc"
    fi
    case "$mode" in
        --msaa-neg-pick) expected='C1072 2x requested, expect 1x image' ;;
        --msaa-neg-pipe) expected='C1072 pipeline mismatch rejected before draw' ;;
        --msaa-neg-sampled) expected='C1072 single sample must reject MS shader before draw' ;;
        --msaa-neg-attachment) expected='C1072 inconsistent attachments rejected before renderpass' ;;
    esac
    if [ "$rc" -eq 0 ] || ! grep -Fq "$expected" "$out/msaa-log"; then
        echo "【红】$mode 没有红在预期断言（rc=${rc}）" >&2; exit 1
    fi
    if [ "$mode" = "--msaa-neg-attachment" ]; then
        count=$(grep -c 'Validation Error' "$out/msaa-log" || true)
        if [ "$count" -ne 1 ] || ! grep -q 'VUID-VkSubpassDescription-pColorAttachments-09430' "$out/msaa-log"; then
            echo "【红】混合附件负控的校验报错不是预期一条（count=${count}）；原文在 build/msaa-tests/negative-attachment.log" >&2; exit 1
        fi
        echo 'PASS: 混合附件触发预期校验错误 1 条，原文在 build/msaa-tests/negative-attachment.log'
    elif grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/msaa-log"; then
        echo "【红】$mode 有非预期 Vulkan 校验报错" >&2; exit 1
    fi
    echo "PASS: $mode 命中预期断言（validation 输出见上）"
    exit 0
fi

if [ -z "$mode" ]; then
    set +e
    timeout 120 "$out/t_backend_vk" "$out/shaders" --msaa-only 2>&1 | tee "$out/msaa-default-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q 'C1072 isolated failures=0' "$out/msaa-default-log" ||
       grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/msaa-default-log"; then
        echo "【红】MSAA 降档专项失败或 validation 报错（rc=${rc}）" >&2; exit 1
    fi
fi


if [[ "$mode" == --clear-capacity* ]]; then
    failed=0
    for dq in 0 1; do
        if [ "$dq" = 1 ]; then export S2A_TEST_NVK_DQ=1; else unset S2A_TEST_NVK_DQ; fi
        set +e
        timeout 120 "$out/t_backend_vk" "$out/shaders" --clear-capacity 2>&1 | tee "$out/clear-capacity-dq${dq}.log"
        rc=${PIPESTATUS[0]}
        set -e
        log="$out/clear-capacity-dq${dq}.log"
        expected_rc=0; expected_failures=0
        if [ "$mode" = --clear-capacity-neg ]; then expected_rc=1; expected_failures=4; fi
        if [ "$rc" -ne "$expected_rc" ] || ! grep -q "C1501 clear capacity failures=${expected_failures}" "$log" ||
           ! grep -q 'C1501 keys=48 failures=0' "$log" ||
           ! grep -q "C1501 keys=60 failures=${expected_failures}" "$log" ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$log"; then
            echo "【红】清除缓存容量（DQ=${dq}）：rc=${rc}，未命中预期断言或 validation 失败" >&2
            failed=1
        fi
        if [ "$mode" = --clear-capacity-neg ] && ! grep -q '建不出清除管线（槽 3 颜色掩码 0x4' "$log"; then
            echo '【红】清除缓存负控没有红在第49个键' >&2; failed=1
        fi
    done
    unset S2A_TEST_NVK_DQ
    exit "$failed"
fi

if [ "$mode" = "--sampler-minmax-only" ]; then
    set +e
    timeout 120 "$out/t_backend_vk" "$out/shaders" --sampler-minmax-only 2>&1 | tee "$out/sampler-minmax-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/sampler-minmax-log"; then
        echo "【红】sampler minmax: rc=$rc or Vulkan validation error" >&2; exit 1
    fi
    exit 0
fi

# C63c：同一 view 写进同一描述符数组的两个元素（合法用法），取元素 1 必须读回 128。
# 两遍都要绿：备忘开（生产缺省）与备忘关（nvk_desc_memo=0，套件第二遍；写入跳过只在这遍生效）。
if [ "$mode" = "--c63-same-view" ]; then
    for pass in memo-on memo-off; do
        if [ "${pass}" = memo-off ]; then export S2A_TEST_DESC_MEMO=0; else unset S2A_TEST_DESC_MEMO; fi
        set +e
        timeout 120 "$out/t_backend_vk" "$out/shaders" --c63-same-view 2>&1 | tee "$out/c63-same-view-${pass}.log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || ! grep -q 'C63SAMEVIEW failures=0' "$out/c63-same-view-${pass}.log" ||
           grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/c63-same-view-${pass}.log"; then
            echo "【红】C63 同 view 双元素（${pass}）：rc=${rc}，没有 failures=0 或有 validation 报错" >&2; exit 1
        fi
        grep 'C63c 同 view 双元素' "$out/c63-same-view-${pass}.log" || true
    done
    unset S2A_TEST_DESC_MEMO
    echo '【绿】C63 同 view 双元素：两遍（备忘开/关）都读回 128，validation 0 条'; exit 0
fi

# C1581K：D24S8 的 CPU 上传拆成深度面、模板面两个区域（一个区域只能有一个面，VUID-VkBufferImageCopy-aspectMask-09103），
#   深度面回读低 24 位一致、上传的模板经模板测试看得见。validation 0 条才绿。
if [[ "$mode" == --gpu-deswizzle-neg-* ]]; then
    unset S2A_TEST_NVK_DQ
    set +e
    timeout 120 "$out/t_backend_vk" "$out/shaders" --gpu-deswizzle >"$out/gpu-deswizzle-negative.log" 2>&1
    rc=$?
    set -e
    if [ "$mode" = "--gpu-deswizzle-neg-barrier" ]; then
        grep -q 'SYNC-HAZARD-READ-AFTER-WRITE' "$out/gpu-deswizzle-negative.log" || { echo '【红】移除屏障没有被同步校验拦住'; exit 1; }
    else
        [ "$rc" -ne 0 ] && grep -q 'GPU matrix byte mismatch' "$out/gpu-deswizzle-negative.log" || { echo "【红】几何变异未在字节断言处变红（rc=${rc}）"; tail -25 "$out/gpu-deswizzle-negative.log"; exit 1; }
    fi
    echo "【绿】$mode 触发预期负控（rc=${rc}）"; exit 0
fi

if [ "$mode" = "--gpu-deswizzle" ]; then
    for dq in 0 1; do
        if [ "$dq" = 0 ]; then unset S2A_TEST_NVK_DQ; else export S2A_TEST_NVK_DQ=1; fi
        set +e
        timeout 120 "$out/t_backend_vk" "$out/shaders" --gpu-deswizzle 2>&1 | tee "$out/gpu-deswizzle-${dq}.log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || ! grep -q 'GPU_DESWIZZLE failures=0' "$out/gpu-deswizzle-${dq}.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/gpu-deswizzle-${dq}.log"; then
            echo "【红】GPU deswizzle dq=${dq}: rc=${rc} 或校验失败" >&2; exit 1
        fi
    done
    unset S2A_TEST_NVK_DQ
    export S2A_TEST_GPU_DESWIZZLE_OFF=1
    set +e
    timeout 120 "$out/t_backend_vk" "$out/shaders" --gpu-deswizzle 2>&1 | tee "$out/gpu-deswizzle-off.log"
    rc=${PIPESTATUS[0]}
    set -e
    unset S2A_TEST_GPU_DESWIZZLE_OFF
    if [ "$rc" -ne 0 ] || ! grep -q 'GPU_DESWIZZLE failures=0' "$out/gpu-deswizzle-off.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/gpu-deswizzle-off.log"; then
        echo "【红】GPU 解排关闭：rc=${rc} 或校验失败" >&2; exit 1
    fi
    echo '【绿】GPU deswizzle 两级录制开关、关闭能力、校验层通过'; exit 0
fi

if [ "$mode" = "--ds-upload-only" ]; then
    set +e
    timeout 120 "$out/t_backend_vk" "$out/shaders" --ds-upload-only 2>&1 | tee "$out/ds-upload.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q 'C1581K failures=0' "$out/ds-upload.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/ds-upload.log"; then
        echo "【红】C1581K 深度模板上传：rc=${rc}，没有 failures=0 或有 validation 报错" >&2; exit 1
    fi
    echo '【绿】C1581K 深度模板上传：深度面回读一致、模板面生效，validation 0 条'; exit 0
fi

# ReadTexels 读 GPU 内容的独立档（默认档里也跑）：只跑 run_replay_read_texels.sh，给红 / 绿 / 负控取证用
if [ "$mode" = "--read-texels-core" ]; then exec bash "$repo/android/tests/vk/run_replay_read_texels.sh"; fi

# C1581O：纯模板格式 STENCIL8 当模板附件：图像 / 视图的面只有模板面，管线的兼容通道按 sfmt 挂上这个附件。左绿右蓝、validation 0 条才绿。
if [ "$mode" = "--stencil8-only" ]; then
    set +e
    timeout 120 "$out/t_backend_vk" "$out/shaders" --stencil8-only 2>&1 | tee "$out/stencil8.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q 'C1581O failures=0' "$out/stencil8.log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/stencil8.log"; then
        echo "【红】C1581O 纯 STENCIL8 模板附件：rc=${rc}，没有 failures=0 或有 validation 报错" >&2; exit 1
    fi
    echo '【绿】C1581O 纯 STENCIL8 模板附件：左绿右蓝，validation 0 条'; exit 0
fi

if [ "$mode" = "--present-only" ]; then
    for order in rgba bgra; do
    if [ "$order" = bgra ]; then export S2A_TEST_PRESENT_BGRA=1; else unset S2A_TEST_PRESENT_BGRA; fi
    set +e
    timeout 60 "$out/t_backend_vk" 2>&1 | tee "$out/present-log"
    rc=${PIPESTATUS[0]}
    set -e
    bad=$(grep -cE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/present-log" || true)
    [ "$rc" -eq 0 ] && [ "$bad" -eq 0 ] || exit 1
    done
    unset S2A_TEST_PRESENT_BGRA
    exit 0
fi

if [ "$mode" = "--separate-neg-sampler" ]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/separate-neg-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -eq 0 ] || ! grep -q 'round 0 pixel 0: 0,255,0,255' "$out/separate-neg-log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-|commit round|wait round' "$out/separate-neg-log"; then
        echo '【红】分离采样器来源负控未击中像素断言，或 validation 有报错' >&2; exit 1
    fi
    echo 'PASS: 改错采样器来源命中像素负控，validation 0 条'; exit 0
fi
if [ "$mode" = "--separate-core" ]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/separate-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q llvmpipe "$out/separate-log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/separate-log"; then
        cp "$out/separate-log" "$repo/build/vk-separate-failure.log"
        echo "【红】分离来源回放采样 rc=$rc 或 validation 有报错；完整日志 build/vk-separate-failure.log" >&2; exit 1
    fi
    echo '【绿】分离来源回放采样：repeat 红、clamp 绿，validation 0 条'; exit 0
fi
if [ "$mode" = "--tbuf-compiler-core" ]; then
    rm -rf "$repo/build/tbuf-real-red/core-compiler"
    set +e
    timeout 90 "$out/t_backend_vk" "$repo/build/tbuf-real-red" 2>&1 | tee "$out/tbuf-compiler-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || ! grep -q llvmpipe "$out/tbuf-compiler-log" ||
       ! grep -q 'core Maxwell TBUF: expected 17,34,51,255 on 16 pixels, failures=0' "$out/tbuf-compiler-log" ||
       grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/tbuf-compiler-log"; then
        echo "【红】真实 Maxwell TBUF 回放 rc=$rc 或 validation 有报错" >&2; exit 1
    fi
    echo 'PASS: 真实生产请求、Maxwell 编译、池镜像与 Vulkan 16 像素，validation 0 条'; exit 0
fi
if [[ "$mode" == --tbuf-* ]]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/tbuf-log"
    rc=${PIPESTATUS[0]}
    set -e
    if ! grep -q llvmpipe "$out/tbuf-log" || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/tbuf-log"; then
        echo '【红】TBUF 回放设备非 lavapipe 或 validation 有报错' >&2; exit 1
    fi
    case "$mode" in
        --tbuf-core) [ "$rc" -eq 0 ] && grep -q 'TBUF replay: expected 9,13,4,255 on 16 pixels, failures=0' "$out/tbuf-log" ;;
        --tbuf-neg-resolve) [ "$rc" -ne 0 ] && grep -q 'two TBUF draws without skips' "$out/tbuf-log" ;;
        --tbuf-neg-format) [ "$rc" -ne 0 ] && grep -q 'TBUF pixel 0: 0,0,4,255 expected 9,13,4,255' "$out/tbuf-log" ;;
        --tbuf-neg-elements) [ "$rc" -ne 0 ] && grep -q 'TBUF pixel 0: 9,13,8,255 expected 9,13,4,255' "$out/tbuf-log" ;;
    esac || { echo "【红】$mode 未命中预期" >&2; exit 1; }
    echo "PASS: $mode 命中预期，validation 0 条"; exit 0
fi
if [ "$mode" = "--shown-only" ]; then
    for shown_mode in --shown-only --shown-unsupported; do
        set +e
        timeout 90 "$out/t_backend_vk" "$out/shaders" "$shown_mode" 2>&1 | tee "$out/shown-log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/shown-log"; then exit 1; fi
    done
    echo 'C883 WSI validation 0 条'; exit 0
fi

if [ "$mode" = "--gs-core" ]; then
    rm -rf "$repo/build/696-real-compiler/core-compiler"
    set +e
    timeout 60 "$out/t_backend_vk" "$repo/build/696-real-compiler" 2>&1 | tee "$out/gs-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/gs-log"; then echo "【红】GS 核心整链 rc=$rc" >&2; exit 1; fi
    echo '【绿】GS 核心整链 validation 0 条'; exit 0
fi
if [ "$mode" = "--gs-no-cap" ] || [ "$mode" = "--gs-neg-cap" ]; then
    set +e
    env -u S2A_CAPPROBE_RESULT timeout 60 "$out/t_backend_vk" "$out/shaders" --gs-no-cap 2>&1 | tee "$out/gs-log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$mode" = "--gs-neg-cap" ]; then
        if [ "$rc" -eq 0 ] || ! grep -q 'no capability refuses GS shader before Vulkan' "$out/gs-log"; then echo '【红】无能力负控未击中' >&2; exit 1; fi
        echo 'PASS: 忽略无能力时目标断言红'; exit 0
    fi
    if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-|SYNC-HAZARD-' "$out/gs-log"; then echo '【红】无探测拒绝失败' >&2; exit 1; fi
    echo 'PASS: 无探测时 GS 未启用且 shader 拒绝'; exit 0
fi
if [ "$mode" = "--gs-neg-stage" ] || [ "$mode" = "--gs-neg-flags" ]; then
    set +e
    timeout 60 "$out/t_backend_vk" "$out/shaders" --gs-only > "$out/gs-log" 2>&1
    rc=$?
    set -e
    mkdir -p "$repo/build/696-tests"
    cp "$out/gs-log" "$repo/build/696-tests/${mode#--}.log"
    if [ "$mode" = "--gs-neg-stage" ]; then expected='GS pass'; else expected='VUID-'; fi
    if ! grep -q "$expected" "$out/gs-log" || { [ "$rc" -eq 0 ] && ! grep -qE 'Validation Error|VUID-' "$out/gs-log"; }; then
        echo "【红】$mode 负控未击中预期" >&2; exit 1
    fi
    echo "PASS: $mode 命中预期红"; exit 0
fi
if [ "$mode" = "--gs-only" ] || [ "$mode" = "--gs-compiler" ]; then
    shader_dir="$out/shaders"
    if [ "$mode" = "--gs-compiler" ]; then shader_dir="$repo/build/696-real-compiler"; fi
    set +e
    timeout 60 "$out/t_backend_vk" "$shader_dir" "$mode" 2>&1 | tee "$out/gs-log"
    rc=${PIPESTATUS[0]}
    set -e
    bad=$(grep -cE "Validation Error|VUID-" "$out/gs-log" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
        echo "【红】GS 用例 rc=$rc validation=$bad" >&2; exit 1
    fi
    echo "【绿】GS 用例：validation 0 条"
    exit 0
fi

if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --gs-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --separate-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --tbuf-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run.sh" --tbuf-compiler-core; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_copy_t2b.sh"; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_read_texels.sh"; fi   # ReadTexels 读 GPU 内容（rp_read_texels.inc + 真后端）
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_clear_tex.sh"; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_draw_tex.sh"; fi
if [ "$mode" = "" ]; then bash "$repo/android/tests/vk/run_replay_ssbo_window.sh"; fi
# 计算读回要求校验层声明 syncval_shader_accesses_heuristic；共享镜像的层（Ubuntu noble 1.3.275）没有，夹具退 75（环境验不了）。
# 原先在 set -e 下直接调，75 让整个默认档就此退出：其后的主套件（t_backend_vk 三遍、上屏、BC 解码等）在任何门禁机上都没跑过，
# 每次 10 分钟左右只换来「环境未就绪」。改为记下这一项未证、其余照跑；其余全绿时末尾仍退 75（不把未证的一项判绿），其余有红照常报红。
compute_read_unready=
if [ "$mode" = "" ]; then
    set +e; bash "$repo/android/tests/vk/run_compute_read.sh"; cr_rc=$?; set -e
    if [ "$cr_rc" -eq 75 ]; then compute_read_unready=1; echo "【未证】计算读回：本机校验层验不了（见上一行），记未证、其余用例照跑"
    elif [ "$cr_rc" -ne 0 ]; then exit "$cr_rc"; fi
fi

set +e
# 整套三遍的超时只用来发现卡死，不量性能：负载高的机器（列车机同时跑多份同类用例，软件渲染）上单遍已接近原先的 180 / 240 秒，
# 主线自己也会被误判超时。放到 600 秒。
timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log"
rc=${PIPESTATUS[0]}
set -e

if [ "$rc" -ne 0 ]; then
    echo "【红】用例失败（rc=${rc}）" >&2
    exit 1
fi
if ! grep -q "llvmpipe" "$out/log"; then
    echo "【红】设备不是 lavapipe（CPU 参考实现）：" >&2
    grep '^device:' "$out/log" >&2 || true
    exit 1
fi
if [ "$(grep -c "C697e 小 mip 整块区域：已核对" "$out/log")" -lt 2 ]; then
    echo "【红】C697e（小 mip 整块区域，原生 / 现解两条路径）没跑全" >&2
    exit 1
fi
if ! grep -q "C697f 3D BC 按切片号上传：已核对" "$out/log"; then
    echo "【红】没跑到 C697f（缓冲 → 3D BC 现解贴图按切片号上传）" >&2
    exit 1
fi
if ! grep -q "C206 段内已引用句柄备忘：重复引用只记一次 / release 之后再用照样报错且报错的不进备忘 / 无关 release 作废备忘 / 归还次数对得上 / 新段不继承旧段的备忘 / 并发无竞争，缺省与对照两档已核对" "$out/log"; then
    echo "【红】没跑到 C206（段内已引用句柄备忘：重复引用只记一次、release 之后再用照样报错且报错的不进备忘、无关 release 作废备忘、归还次数对得上、新段不继承旧段的备忘、并发无竞争）" >&2
    exit 1
fi
if ! grep -q "C907 保留路径管线缓存落盘：已核对" "$out/log"; then
    echo "【红】没跑到 C907（停局保留会话时的管线缓存落盘）" >&2
    exit 1
fi
if ! grep -q "C205 图形管线散列表：增长 / 多次翻倍 / 增长失败沿用旧表 / 逐字节逐字段 / 先比散列 / 两条退路已核对" "$out/log"; then
    echo "【红】没跑到 C205（图形管线散列表：增长重排、起始 16 桶多次翻倍、增长分配失败沿用旧表、散列覆盖每个字节 / 字段、先比存下的散列、两条退路）" >&2
    exit 1
fi
if ! grep -q "C204 描述符组按布局回收：A…N 十四个臂已核对" "$out/log"; then
    echo "【红】没跑到 C204（描述符组按布局回收，十四个臂）" >&2
    exit 1
fi
if ! grep -q "C208 close 不等满时限：.*已核对" "$out/log"; then
    echo "【红】没跑到 C208（close 不等满时限：退休队列清空后 worker 要唤醒 close）" >&2
    exit 1
fi
if ! grep -q "C207 诊断读数互不清零：近 300 帧读数与逐笔读数各自读增量、不清零共享累加器，已核对" "$out/log"; then
    echo "【红】没跑到 C207（诊断读数互不清零：近 300 帧读数与每 5 秒的逐笔读数读同一批累加器，各自读增量）" >&2
    exit 1
fi
if ! grep -q "C61s sRGB 贴图按 UNORM 视图存储写：16 像素逐字节相同，已核对" "$out/log"; then
    echo "【红】没跑到 C61s（sRGB 贴图按同位宽 UNORM 视图当存储写：lavapipe 是 Vulkan 1.1+，后端该宣告 STORAGE_SRGB_ALIAS）" >&2
    exit 1
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log" || true)
if [ "$bad" -ne 0 ]; then
    echo "【红】validation 报错 $bad 条：" >&2
    # 带上下文打：校验层把对象 / 组号 / binding 写在 Validation Error 的下一行，只打匹配行定不了因（CI 日志只留这里打的）
    grep -B2 -A3 -E "Validation Error" "$out/log" | head -60 >&2
    first=$(grep -n -m1 "Validation Error" "$out/log" | cut -d: -f1)
    echo "     第一条报错前 25 行（看是哪个用例触发）：" >&2
    sed -n "$(( first > 25 ? first - 25 : 1 )),${first}p" "$out/log" >&2
    exit 1
fi

# 同一套用例在「两级回放开」（vkCmd* 调用交给录制线程）下再跑一遍：像素期望和校验层判据不变。
set +e
S2A_TEST_NVK_DQ=1 timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_dq"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    echo "【红】两级回放开着的那一遍用例失败（rc=${rc}）" >&2
    exit 1
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log_dq" || true)
if [ "$bad" -ne 0 ] || ! grep -q "两级回放：交给录制线程" "$out/log_dq"; then
    echo "【红】两级回放开着的那一遍：validation 报错 $bad 条，或没有调用进队列：" >&2
    grep -E "Validation Error|VUID-" "$out/log_dq" | head -20 >&2
    exit 1
fi

# 描述符组备忘关掉（开关 nvk_desc_memo=0：UBO 用静态偏移、不备忘，回到 #1233 之前的老路径）再跑一遍：像素期望不变、校验层 0 条。
# 缺省那几遍跑的是备忘开着（缺省开，管线的 UBO 不超设备上限就走动态偏移）；C208 在这一遍里核「备忘关则一律静态」，
# 并保留按「分配了几组」写的那几条期望（备忘开着时内容相同的组不再分配，那几条在缺省遍里不比）。
set +e
S2A_TEST_DESC_MEMO=0 timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_memo"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_memo" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
    echo "【红】描述符组备忘关着的那一遍：rc=${rc}，validation 报错 $bad 条" >&2
    grep -E "Validation Error|VUID-" "$out/log_memo" | head -20 >&2
    exit 1
fi

# 描述符组按布局回收关掉（开关 nvk_dset_rec=0，回到每笔向驱动分配）再跑一遍：像素期望不变、校验层 0 条。
# C204 各臂自带开关值，这一遍管的是其余用例走老路径（缺省那三遍跑的是回收开着）。
set +e
S2A_TEST_DSET_REC=0 timeout 600 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_dset0"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_dset0" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
    echo "【红】描述符组回收关着的那一遍：rc=${rc}，validation 报错 $bad 条" >&2
    grep -E "Validation Error|VUID-" "$out/log_dset0" | head -20 >&2
    exit 1
fi

# 提交失败的首因（#889）：同步返回与回调给出同一首错原因；独立会话单跑。
set +e
S2A_TEST_ERROR697_ONLY=1 timeout 120 "$out/t_backend_vk" "$out/shaders" 2>&1 | tee "$out/log_e697"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_e697" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ] || ! grep -q "E697 isolated failures=0" "$out/log_e697"; then
    echo "【红】提交失败首因那一遍（#889）：rc=${rc}，validation 报错 $bad 条" >&2
    exit 1
fi

# 生产回放的普通间接绘制与单次 DRV0（#941）：隔离用例（真 rp_draw / rp_buf + 真后端），同一容器里两级回放关 / 开各跑一遍
for dq in 0 1; do
    set +e
    if [ "$dq" = 1 ]; then export S2A_REPLAY_INDIRECT_DQ=1 S2A_TEST_NVK_DQ=1; else unset S2A_REPLAY_INDIRECT_DQ S2A_TEST_NVK_DQ; fi
    S2A_LAVAPIPE_ICD="$icd" S2A_VK_LAYER_DIR="$layer" GLSLC="$glslc" timeout 300 bash "$repo/android/tests/vk/run.sh" --indirect-core 2>&1 | tee "$out/log_indirect_$dq"
    rc=${PIPESTATUS[0]}
    set -e
    unset S2A_REPLAY_INDIRECT_DQ S2A_TEST_NVK_DQ
    bad=$(grep -cE "Validation Error|VUID-" "$out/log_indirect_$dq" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ] || ! grep -q "pixel-failures0" "$out/log_indirect_$dq"; then
        echo "【红】间接绘制隔离用例（#941，两级回放 ${dq}）：rc=${rc}，validation 报错 $bad 条" >&2
        exit 1
    fi
done

# 零属性（#977）：着色器读了但没设格式的顶点属性，核心补全 0 属性；修前校验层报这个输入没有来源。
set +e
S2A_LAVAPIPE_ICD="$icd" S2A_VK_LAYER_DIR="$layer" GLSLC="$glslc" timeout 300 bash "$repo/android/tests/vk/run.sh" --zero-attr-core 2>&1 | tee "$out/log_zero_attr"
rc=${PIPESTATUS[0]}
set -e
bad=$(grep -cE "Validation Error|VUID-" "$out/log_zero_attr" || true)
if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ] || ! grep -q "zero-attr cases2 pixel-failures0" "$out/log_zero_attr"; then
    echo "【红】零属性隔离用例（#977）：rc=${rc}，validation 报错 $bad 条" >&2
    exit 1
fi

# BC1–BC7 compute 解码逐像素用例（#1199）：16 格式 × 2 尺寸 + 反证；同一 validation 环境。
# S2A_BCDEC_HOST_TEST 使 S2A_VK_BC_DECODE=force 在主机用例里生效（等同 debug 构建门）。
cc "${cflags[@]}" -DS2A_BCDEC_HOST_TEST "${inc[@]}" "${bc_src[@]}" -lvulkan -lpthread -lm -o "$out/t_bc_decode"
set +e
S2A_VK_BC_DECODE=force timeout 180 "$out/t_bc_decode" 2>&1 | tee "$out/log_bc"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    echo "【红】BC 解码用例失败（rc=${rc}）" >&2
    exit 1
fi
bad=$(grep -cE "Validation Error|VUID-" "$out/log_bc" || true)
if [ "$bad" -ne 0 ]; then
    echo "【红】BC 解码 validation 报错 $bad 条：" >&2
    grep -E "Validation Error|VUID-" "$out/log_bc" | head -20 >&2
    exit 1
fi
if [ "$mode" = "--tsan" ]; then
    echo "【绿】C01–C08 + C23/C25/C26/C27/C28/C29/C30/C31/C32/C33/C34/C35/C36-55/C70/C71/C72/C73 + 清屏/复制/深度/纹理三角形：TSan 0 报告（validation 由默认档单独跑）；设备 $(grep '^device:' "$out/log" | head -1)"
else
    echo "【绿】C01–C08 + C23/C25/C26/C27/C28/C29/C30/C31/C32/C33/C34/C35/C36-55/C70/C71/C72/C73 + 清屏/复制/深度/纹理三角形 + BC1–BC7 逐像素解码：validation 0 条；设备 $(grep '^device:' "$out/log" | head -1)"
fi

# #1142：默认 vk 套件也验非线性上屏，立即 / 延迟两档均开同步校验。
if [ -z "$mode" ]; then
    (unset S2A_TEST_NVK_DQ; bash "$repo/android/tests/vk/run.sh" --present-only)
    S2A_TEST_NVK_DQ=1 bash "$repo/android/tests/vk/run.sh" --present-only
fi

# 计算读回未证（见上）：其余全部跑完且没有红，整步仍按「环境验不了这一项」退 75
if [ -n "${compute_read_unready:-}" ]; then echo "【未证】其余用例全绿；计算读回一项待新校验层的环境验证" >&2; exit 75; fi
