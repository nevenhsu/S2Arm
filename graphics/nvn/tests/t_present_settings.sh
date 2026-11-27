#!/bin/bash
# #604 real production presenter, GPU-generated inputs, independent pixel checks.
# M3/M4 x four AA modes x three filters (MetalFX: 10-30, spatial upscale on the present). No game files or host/render substitutes.
# PRESENT_WIDEN_LIB may select a prepared production Widen library.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root"
source "$root/tools/toolchain.sh"
out=${PRESENT_SETTINGS_BUILD:-$root/build/tests/present-settings}
[[ "$out" = /* ]] || out="$root/$out"
# 宿主侧只编、只链清单列的源文件（t_present_settings.link.txt：t_present_settings_closure.py 按链接器实算的闭包，门禁按同一份清单算本测输入）。
#   不链的文件物理上进不了产物，所以清单外的宿主改动不必重跑本测；清单本身由下面三项核对兜住：链接报未定义符号、编译实读了清单外的头、
#   链出来多了运行期查找的符号——任一项红就是清单过期，重跑生成器。PRESENT_SETTINGS_GEN=1 + PRESENT_SETTINGS_LIST 只给生成器用：编候选全集、只构建
list=${PRESENT_SETTINGS_LIST:-$root/graphics/nvn/tests/t_present_settings.link.txt}
gen=${PRESENT_SETTINGS_GEN:-0}
rm -rf "$out/host"; mkdir -p "$out/host"   # 旧清单留下的目标文件不许混进来
common=(-O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -I"$root/runtime/include" -I"$root/runtime/ns2pro/include" -DNS2_SCAN_SECONDS=60)   # Mac 前端源文件含 Switch 2 Pro 连接（macos/ns2_controller.c、runtime/ns2pro，#277），同 launchers/macos/build.mk
srcs=(); while IFS= read -r rel; do srcs+=("$rel"); done < <(python3 graphics/nvn/tests/t_present_settings.py sources "$list")
# 只核依赖（PRESENT_SETTINGS_DEPS_ONLY=1，入车快检查用）：同一份编译参数只做预处理（-M，不编不链），把实读头文件交给 depcheck 比清单；
#   新 include 了清单外的头当场报「清单过期」（11-23、11-24 两次合进 main 后才被门禁查出）。不核链接闭包与运行期查找（那两项要整编整链，留给门禁）
if [[ ${PRESENT_SETTINGS_DEPS_ONLY:-0} = 1 ]]; then
    [[ ${#srcs[@]} -gt 0 ]] || { printf 'Link list is empty: %s\n' "$list" >&2; exit 2; }
    printf '%s\n' "${srcs[@]}" | xargs -P "$(sysctl -n hw.ncpu)" -I{} bash -c 'out=$1 root=$2 rel=$3; shift 3; arc=(); [[ "$rel" != *.m ]] || arc=(-fobjc-arc)
        exec xcrun --sdk macosx clang "$@" ${arc[@]+"${arc[@]}"} -M -MF "$out/host/${rel//\//_}.o.d" "$root/$rel"' _ "$out" "$root" {} "${common[@]}" \
        || { echo 'host source preprocess failed' >&2; exit 1; }
    xcrun --sdk macosx clang "${common[@]}" -std=gnu11 -fobjc-arc -M -MF "$out/host/t_present_settings.m.o.d" \
        -I"$root/graphics/nvn/gfx/nvn" -I"$root/graphics/nvn/gfx/nvn/include" -I"$root/graphics/nvn/host" "$root/graphics/nvn/tests/t_present_settings.m"
    ds=(); for rel in "${srcs[@]}"; do ds+=("$out/host/${rel//\//_}.o.d"); done
    exec python3 graphics/nvn/tests/t_present_settings.py depcheck "$list" "$root" "${ds[@]}" "$out/host/t_present_settings.m.o.d"
fi
if [[ -n ${PRESENT_WIDEN_LIB:-} ]]; then
    widen=$PRESENT_WIDEN_LIB
    [[ "$widen" = /* ]] || widen="$root/$widen"
    [[ -r "$widen" ]] || { printf 'Missing production Widen: %s\n' "$widen" >&2; exit 2; }
else
    widen="$out/deps/widen/S2Arm.Gfx.Widen.dylib"
    bash "$root/graphics/widen/publish_native.sh" "$out/deps/widen-artifacts" "$out/deps/widen"   # 不经顶层 Makefile：它解析时扫全部宿主源码
fi
make -j1 -C "$root/graphics/nvn" mac
make -j1 -C "$root/graphics/metal" mac OUT="$out/gal"
make -j1 -C "$root/graphics/dsx" mac OUT="$out/dsx"   # 直连翻译库（翻译段 + SPIRV-Cross，11-08 从 libnxgal 拆出）
nvn="$root/build/nvn/mac/libnvn.a"
gal="$out/gal/mac/libnxgal.a"
dsx="$out/dsx/mac/libdsx.a"
[[ ${#srcs[@]} -gt 0 ]] || { printf 'Link list is empty: %s\n' "$list" >&2; exit 2; }
objects=()
for rel in "${srcs[@]}"; do objects+=("$out/host/${rel//\//_}.o"); done
# 并行编（一个源文件一个进程，失败的整批报红）；-MD 记实读头文件给下面核对
printf '%s\n' "${srcs[@]}" | xargs -P "$(sysctl -n hw.ncpu)" -I{} bash -c 'out=$1 root=$2 rel=$3; shift 3; arc=(); [[ "$rel" != *.m ]] || arc=(-fobjc-arc)
    o="$out/host/${rel//\//_}.o"; exec xcrun --sdk macosx clang "$@" ${arc[@]+"${arc[@]}"} -MD -MF "$o.d" -c "$root/$rel" -o "$o"' _ "$out" "$root" {} "${common[@]}" \
    || { echo 'host object compile failed' >&2; exit 1; }
main_obj="$out/host/t_present_settings.m.o"
xcrun --sdk macosx clang "${common[@]}" -std=gnu11 -fobjc-arc -MD -MF "$main_obj.d" \
    -I"$root/graphics/nvn/gfx/nvn" -I"$root/graphics/nvn/gfx/nvn/include" -I"$root/graphics/nvn/host" \
    -c "$root/graphics/nvn/tests/t_present_settings.m" -o "$main_obj"
if [[ "$gen" != 1 ]]; then
    ds=(); for o in "${objects[@]}" "$main_obj"; do ds+=("$o.d"); done
    python3 graphics/nvn/tests/t_present_settings.py depcheck "$list" "$root" "${ds[@]}"
fi
# 放行参数按链接输入实算：只被弱引用、谁都不定义的符号（同生产「缺定义即空指针」）。系统库：原生层 HOST_LIBS（Foundation、Security、compression）
#   + 视频解码接管（VideoToolbox / CoreMedia / CoreVideo）+ 宿主目标文件要的 CoreFoundation，同 runtime/build.mk NVN_TEST_WEAK / HOST_LIBS；清单里的文件再加系统依赖会在这里链接报红
read -r -a weak_flags <<< "$(python3 graphics/nvn/tests/t_present_settings.py weak "${objects[@]}" "$main_obj" "$nvn" "$gal" "$dsx")"
link=(xcrun --sdk macosx clang -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -fobjc-arc "$main_obj" "${objects[@]}" "$nvn" "$gal" "$dsx" "$widen" -o "$out/t_present_settings" \
    -Wl,-dead_strip -Wl,-export_dynamic -Wl,-u,_s2a_mac_front_layer -Wl,-u,_nxgal_gpu_hold ${weak_flags[@]+"${weak_flags[@]}"} \
    -Wl,-rpath,"$(dirname "$widen")" -framework AppKit -framework GameController -framework CoreHaptics -framework CoreBluetooth -framework IOKit -framework Metal -framework Foundation \
    -framework QuartzCore -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -lc++ \
    -framework Security -framework CoreFoundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -lcompression)
printf '%s\0' "${link[@]}" > "$out/link.args0"   # 生成器拿同一条链接行做闭包实验（不带映射：生成器自己加）
[[ "$gen" == 1 ]] || link+=(-Wl,-map,"$out/link.map")
"${link[@]}"
if [[ "$gen" != 1 ]]; then
    # 链进来的静态库成员读的源文件 / 头文件都要在清单 lib 行（11-26：#2242 改 libnvn 的 nvn2_codec.c 不在本测输入里，没触发本测，下一笔背锅）
    python3 graphics/nvn/tests/t_present_settings.py libcheck "$list" "$out/link.map" "$root" "$nvn" "$gal" "$dsx"
    python3 graphics/nvn/tests/t_present_settings.py dyncheck "$list" "$out/t_present_settings" "$main_obj" "${objects[@]}" "$nvn" "$gal" "$dsx"
fi
shasum -a 256 "$root/graphics/nvn/tests/t_present_settings.m" "$root/graphics/nvn/tests/t_present_settings.sh" "$list" \
    "${objects[@]}" "$main_obj" "$nvn" "$gal" "$dsx" "$widen" "$out/t_present_settings" > "$out/link-inputs.sha256"
if [[ "$gen" == 1 ]]; then printf 'present settings candidate build: %s\n' "$out/t_present_settings"; exit 0; fi

# Compile diagnostics while another real game owns the screen.
if [[ ${PRESENT_SETTINGS_BUILD_ONLY:-0} == 1 ]]; then
    printf 'present settings built without launching UI: %s\n' "$out/t_present_settings"
    exit 0
fi

stuck_awk='
# sample 调用图 → 每线程一行：线程名｜我方最深帧（in 本测二进制）→ 叶子帧；只取每线程第一条（最重）路径
function flush() { if (th != "" && own != "") printf "%s｜%s → %s\n", th, own, leaf; th = ""; own = ""; leaf = ""; done = 0 }
/^Call graph:/ { on = 1; next }
on && /^Total number in stack/ { flush(); exit }
on && /^    [0-9]+ Thread_/ { flush(); s = $0; sub(/^ +[0-9]+ /, "", s); gsub(/ +/, " ", s); th = substr(s, 1, 60); depth = -1; next }
on && th != "" && !done {
  if ($0 !~ /\(in /) next
  d = match($0, /[0-9]+ [^ ]/); if (depth >= 0 && d <= depth) { done = 1; next }   # 回到同层或更浅 = 第一条路径走完
  depth = d; f = substr($0, d); sub(/^[0-9]+ /, "", f); sub(/ +\[0x[0-9a-f]+\].*/, "", f); sub(/  \(in /, " (", f)
  leaf = f; if (f ~ /\(t_present_settings\)/) own = f
}
END { flush() }
'
# 各格 edge 图跨跑残留：格没跑成时旧图会让 distinctions 假绿——先清
rm -f "$out"/mtl?-*/edge-*.bgra
bad=0; unproven=0; unpr=' '
for backend in mtl3 mtl4; do
    for aa in None SmaaMedium SmaaHigh; do
        for filter in Nearest Bilinear MetalFX Fsr; do   # Fsr = AMD FSR 1
            name="$backend-$aa-$filter"
            runtime="$out/$name"
            mkdir -p "$runtime"
            run_env=(env -u NXGAL_BE -u MTL_SHADER_VALIDATION -u MTL_SHADER_VALIDATION_DEFAULT_STATE \
                -u MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING -u S2A_INPUT_CONFIG -u DM_NVN_SHOT -u DM_NVN_REGLUM \
                S2A_GFX=direct S2A_GAME_DATA="$runtime" S2A_DOCS="$runtime" S2A_GAME_NAME="S2Arm #604 $name" \
                S2A_INPUT_GUIDE_PREFS="$runtime/input-guide.json" DM_PRESENT_LINK=0 S2A_WIDEN_NARROW=1 \
                S2A_ANTI_ALIASING="$aa" S2A_SCALING_FILTER="$filter" S2A_RES_SCALE=1 S2A_FPS=0 \
                MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=assert)
            [[ "$backend" != mtl3 ]] || run_env+=(NXGAL_BE=mtl3)
            # macOS 26 的 Metal 调试层与 Metal 4 + MetalFX 不兼容（11-07 本机 26.4.1 实测：MetalFX 在 MTL4DebugRenderCommandEncoder 上调
            #   globalTraceObjectID → unrecognized selector，进程 abort 134；Xcode 26.6 / 27.1 编都一样；不开调试层 3/3 全过 69/69；macOS 27 上开调试层全过）。
            #   这三格在 macOS 27 以下不开调试层跑，判据不变，并打一行说明
            if [[ "$backend" == mtl4 && "$filter" == MetalFX ]] && [ "$(sw_vers -productVersion | cut -d. -f1)" -lt 27 ]; then
                nodbg=(); for e in "${run_env[@]}"; do case $e in MTL_DEBUG_LAYER=*|MTL_DEBUG_LAYER_ERROR_MODE=*) ;; *) nodbg+=("$e");; esac; done; run_env=("${nodbg[@]}")
                echo "说明 ${name}：macOS $(sw_vers -productVersion) 的 Metal 调试层不支持 Metal 4 + MetalFX，本格不开调试层（macOS 27 起照开）"; fi
            status=0
            rm -f "$runtime/stuck.sample"
            "${run_env[@]}" "$out/t_present_settings" "$runtime" > "$runtime/run.log" 2>&1 & pid=$!
            # 一格正常 4～7 秒跑完；活过 20 秒（本测内部等待 30 秒、闹钟 60 秒之前）就是卡住了：取 3 秒线程栈，红时摘每个线程「我方最深帧 → 叶子帧」，直接读出卡在哪
            #   看门的每 0.2 秒查一次、进程一退就走（不留 sleep 占着本步输出管道），标准输入输出都不接本步
            ( for ((i=0;i<100;i++)); do kill -0 "$pid" 2> /dev/null || exit 0; sleep 0.2; done
              /usr/bin/sample "$pid" 3 -file "$runtime/stuck.sample" ) < /dev/null > /dev/null 2>&1 & wd=$!
            wait "$pid" || status=$?
            wait "$wd" 2> /dev/null || true
            # 77 = 宿主显示环境未证（屏睡/非活动 Space）：记未证不计红，见 t_present_settings.m 探针
            if [[ "$status" == 77 ]]; then unpr+="$name "; unproven=$((unproven+1)); printf 'UNPROVEN %s（宿主显示环境未证）\n' "$name"; continue; fi
            ok=1
            [[ "$status" == 0 ]] || ok=0
            if [[ "$backend" == mtl3 ]]; then expected='direct backend: Metal 3 (forced by NXGAL_BE=mtl3)'
            else expected='direct backend: Metal 4 (GPU in the Metal 4 family)'; fi
            /usr/bin/grep -Fq "$expected" "$runtime/run.log" || ok=0
            /usr/bin/grep -Eq '^present settings: [0-9]+/[0-9]+ passed;' "$runtime/run.log" || ok=0
            # MetalFX: the 67x53 edge scaled to 134x106 must have gone through the spatial scaler, on the backend under test
            if [[ "$filter" == MetalFX ]]; then
                if [[ "$backend" == mtl3 ]]; then fx='MetalFX 空间放大：67x53 → 134x106（Metal 3）'; else fx='MetalFX 空间放大：67x53 → 134x106（Metal 4）'; fi
                /usr/bin/grep -Fq "$fx" "$runtime/run.log" || ok=0
            fi
            # Fsr：67x53 → 134x106 必须真走了 EASU → RCAS（带抗锯齿时前面多一步），且在被测的后端上
            if [[ "$filter" == Fsr ]]; then
                if [[ "$aa" == None ]]; then chain='EASU → RCAS'; else chain='抗锯齿 → EASU → RCAS'; fi
                if [[ "$backend" == mtl3 ]]; then fsr="直连上屏 FSR：67x53 → 134x106，${chain}（Metal 3）"; else fsr="直连上屏 FSR：67x53 → 134x106，${chain}（Metal 4）"; fi
                /usr/bin/grep -Fq "$fsr" "$runtime/run.log" || ok=0
            fi
            # This graphics-only process has no game NPDM and requests no guest
            # address window. Keep that expected production diagnostic in the
            # raw log; every other red diagnostic still rejects this case.
            if /usr/bin/grep -Ein '^FAIL |command buffer.*error|GPU Address Fault|PageFault|Page Fault|FATAL|failed assertion|Metal Validation Error|\[红\]|【红】' "$runtime/run.log" | \
                /usr/bin/grep -Fv '【红】地址窗口：读不到本款 main.npdm（可执行文件同目录 / 模块目录），不知道要多大，不占窗口'; then ok=0; fi
            /usr/bin/grep -E '^PASS .*coverage|^PASS .*pixel-center|^present settings:' "$runtime/run.log" || true
            if [[ "$ok" == 1 ]]; then printf 'PASS %s\n' "$name"
            else printf 'FAIL %s (exit=%s, log=%s)\n' "$name" "$status" "$runtime/run.log" >&2; bad=$((bad+1))
                # 这一格日志的末 6 行随红一起打出来（exit=142 = 进程 60 秒闹钟到点被杀：末尾就是卡住前最后一步）。门禁的树会被下一局复用，run.log 留不住，
                #   门禁存下的只有红摘要。行首用「✗」：两条路径的摘要都认它——graphics/gate.sh evidence 摘 ^(✗ |不通 |FAIL) 前 20 行，
                #   作为单测（tests/present_settings_test.sh）时 tools/gate.sh 摘 ^(不通|✗) 前 10 行；「FAIL: 」与【红】那几行名额会被编译命令文本占掉
                printf '✗ %s（exit=%s）\n' "$name" "$status" >&2
                /usr/bin/grep -a '^【卡】' "$runtime/run.log" 2> /dev/null | head -n 3 | cut -c1-600 | sed "s/^/✗ 卡 ${name}｜/" >&2
                [[ ! -s "$runtime/stuck.sample" ]] || awk "$stuck_awk" "$runtime/stuck.sample" | cut -c1-220 | sed "s/^/✗ 栈 ${name}｜/" >&2
                # 末 6 行去掉每 10 秒一条的心跳 / 能耗（卡死格里它们占满 6 行、把卡住前的最后一步挤掉）
                /usr/bin/grep -av '^\[s2a\] \[心跳\]\|^\[s2a\] \[能耗\]' "$runtime/run.log" 2> /dev/null | tail -n 6 | sed "s/^/✗ 尾 ${name}｜/" >&2; fi
        done
    done
done
distinctions=0; dskip=0
for backend in mtl3 mtl4; do
    modes=(None SmaaMedium SmaaHigh)
    for ((i=0;i<3;i++)); do
        for ((j=i+1;j<3;j++)); do
            a="$out/$backend-${modes[i]}-Nearest/edge-native.bgra"
            b="$out/$backend-${modes[j]}-Nearest/edge-native.bgra"
            if [[ "$unpr" == *" $backend-${modes[i]}-Nearest "* || "$unpr" == *" $backend-${modes[j]}-Nearest "* ]]; then dskip=$((dskip+1))
            elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
            else printf 'FAIL %s AA outputs not distinct: %s / %s\n' "$backend" "${modes[i]}" "${modes[j]}" >&2; fi
        done
        a="$out/$backend-${modes[i]}-Nearest/edge-scaled.bgra"
        b="$out/$backend-${modes[i]}-Bilinear/edge-scaled.bgra"
        if [[ "$unpr" == *" $backend-${modes[i]}-Nearest "* || "$unpr" == *" $backend-${modes[i]}-Bilinear "* ]]; then dskip=$((dskip+1))
        elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
        else printf 'FAIL %s %s filters produce identical pixels\n' "$backend" "${modes[i]}" >&2; fi
        a="$out/$backend-${modes[i]}-MetalFX/edge-scaled.bgra"
        b="$out/$backend-${modes[i]}-Bilinear/edge-scaled.bgra"
        if [[ "$unpr" == *" $backend-${modes[i]}-MetalFX "* || "$unpr" == *" $backend-${modes[i]}-Bilinear "* ]]; then dskip=$((dskip+1))
        elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
        else printf 'FAIL %s %s MetalFX and Bilinear produce identical pixels\n' "$backend" "${modes[i]}" >&2; fi
        a="$out/$backend-${modes[i]}-Fsr/edge-scaled.bgra"   # FSR 1 对双线性（存的是真画布，不是抓图）
        if [[ "$unpr" == *" $backend-${modes[i]}-Fsr "* || "$unpr" == *" $backend-${modes[i]}-Bilinear "* ]]; then dskip=$((dskip+1))
        elif [[ -s "$a" && -s "$b" ]] && ! cmp -s "$a" "$b"; then distinctions=$((distinctions+1))
        else printf 'FAIL %s %s Fsr and Bilinear produce identical pixels\n' "$backend" "${modes[i]}" >&2; fi
    done
done
printf 'present settings matrix: %d/24 passed, %d unproven (logs/images: %s)\n' "$((24-bad-unproven))" "$unproven" "$out"
printf 'present settings output distinctions: %d/24 passed, %d skipped\n' "$distinctions" "$dskip"
if ((unproven)); then printf 'present settings: %d cells 未证（宿主显示环境不给真上屏位）\n' "$unproven"; fi
# 24 格（加 Fsr 一列前 18）全未证 = 这次一格真上屏都没验：退 75（门禁记「环境未就绪」重排），不能当绿（判据绿着失效）；部分未证照跳过那几格
if ((unproven == 24)); then exit 75; fi
[[ "$bad" == 0 && $((distinctions+dskip)) == 24 ]]   # 每个后端：3 种模式两两 3 对 + 每模式最近邻对双线性 3 次 + MetalFX 对双线性 3 次 + FSR 对双线性 3 次 = 12，两个后端 24；未证格的对子计 skip 不判（10-30 去掉 FXAA 前 4 种模式是 28）
