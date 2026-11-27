#!/usr/bin/env bash
# 门禁路由：android/
# 门禁路由：.editorconfig .gitignore Directory.Build.rsp compat/tree/ graphics/dsx/ graphics/metal/ graphics/nvn/ graphics/third_party/ graphics/tools/ graphics/track/ graphics/vulkan/ graphics/widen/ ios/ nuget.config pack/contracts/ pack/cpatch.py pack/display_name.py pack/gen_caps.sh pack/jobs/ pack/ns2_dump.py pack/orch/ pack/rewriter.py pack/romfs_img.py pack/sysdata_mii.py pack/sysdata_synth.py pack/targets/ platform/ runtime/include/ runtime/ns2pro/ runtime/rewriter/ runtime/src/ runtime/tests/ runtime/third_party/ services/S2A.Services.Tests/ services/S2A.Services/ services/nuget.config services/opus/ services/romfs_index/ services/vpx/ tests/fixtures/ tools/_timeout.sh tools/ab_span.py tools/addr_need.py tools/bin/ tools/caps_policy.py tools/contrib_layer2.tsv tools/devprofile_lint.py tools/flock_hold.py tools/fps_truth.py tools/fps_truth.tsv tools/frame_gate.py tools/game_fps.py tools/gate_route.py tools/machine.sh tools/module_order.py tools/nro_exports.py tools/nso_plain.py tools/patch_index.py tools/pool_workers.py tools/quality_check.py tools/redact.py tools/redline.py tools/reglist.py tools/score_cells.py tools/weak_link_decl.py translator/dmsh_xlate/ translator/ns2_shaders.py translator/nvn_key.py translator/spvc_cli/
# 门禁层：合入后
# 门禁本机：* android/test.sh 要 CMake 3.18+ / NDK 29.0.14206865 / JDK 17 / 固定 venv（Capstone 5.0.7、LZ4 4.4.5）；门禁机没有 CMake
# 安卓无设备 CPU 测的门禁钩子：整套 android/test.sh（Java 单测 / 原生宿主单测与变异对照 / Python 契约 / 可移植改写准备回归）。
# 11-11 旧安卓三钩子删除后（017d7695b）android/ 是无钩子路径（tools/gate_route.py NOHOOK），test.sh 没有任何门禁入口在跑；
# 维护者当时定「新 android/gate.sh（门禁路由：android/）接上时删掉 NOHOOK」，本钩子即这一步。
# 不在这里的（各有入口，不重复）：
#   · 新图形层 NDK 交叉链接：android/native/gfx_plat/gate.sh；
#   · 安卓 APK 单库链接核对：tests/android_apk_link_test.sh；Linux runtime CTest：android/linux_ctest.sh（要 aarch64 Linux / colima）；
#   · nvn_gpu_* Vulkan 后端契约用例：tests/android_vk_test.sh。
# 门禁预算：--budget → 1800 秒（11-20 本机整套 1464 秒，基线 c11c831ed；整套串行时用。按子项键拆开后：逐步计时累加最长键约 6 分钟；与他线负载同机 9 键并行实测最长 784 秒）
[ "${1:-}" = --budget ] && { echo 1800; exit 0; }
set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"   # 仓库根
# 子项键（11-26 拆 9 键，android/tools/gate_keys.tsv / gate_keys.py）：门禁按子项拆步用 GATE_HOOK_ONLY="<键…>"（tools/gate.sh 同时放进 GATE_ONLY，两者同设须相同），
#   只跑名单里的键的步；认不得的键 = 红。各键输入都是本钩子路由（不分键挑：理由与实测见 gate_keys.py 文件头）。
#   路由第二行（android/ 以外）= 11-26 Linux 门禁机按文件访问时间实测 9 键读到的仓库文件（d037f6f68，副本不带 .git，免 git 整树重哈希刷 atime）：
#   三层以上深的取前两段目录；顶层目录下直接读的文件过半才取整个目录（按目录通配读），否则逐个列。少了这些，改 graphics/nvn 等不跑本钩子 = 假绿
#   GATE_HOOK_LIST=inputs：每键一行「键<TAB>触发前缀…」后退出（门禁据此给子步算输入）。不设键：整套照旧跑全部步
if [ "${GATE_HOOK_LIST:-}" = inputs ]; then
    route=$(sed -n 's/^# 门禁路由：//p' android/gate.sh | tr '\n' ' ' | sed 's/ $//')
    python3 android/tools/gate_keys.py keys | while read -r k; do printf '%s\t%s\n' "$k" "$route"; done
    exit 0
fi
if [ -n "${GATE_HOOK_ONLY:-}" ]; then
    [ -z "${GATE_ONLY:-}" ] || [ "$GATE_ONLY" = "$GATE_HOOK_ONLY" ] || { echo "🔴 安卓钩子：GATE_HOOK_ONLY（${GATE_HOOK_ONLY}）与 GATE_ONLY（${GATE_ONLY}）同设且不同" >&2; exit 1; }
    GATE_ONLY=$GATE_HOOK_ONLY
fi
if [ -n "${GATE_ONLY:-}" ]; then
    keys=$(python3 android/tools/gate_keys.py keys)
    for k in $GATE_ONLY; do grep -qx -- "$k" <<< "$keys" || { echo "🔴 安卓钩子：子项键 $k 认不得（只认 $(tr '\n' ' ' <<< "$keys")）" >&2; exit 1; }; done
    export GATE_ONLY
fi
# test.sh 的 Java 段用 PATH 上的 javac：先选定 JDK 17（与 JNI 检查同一个 Home），新版本 JDK 排在前面也不能顶掉它
JAVAC=$(bash android/tools/jdk17.sh || true)
[ -n "$JAVAC" ] || { echo "🔴 安卓门禁找不到 JDK 17 的 javac（可用 S2A_JAVAC=<javac 路径> 指定）" >&2; exit 1; }
JAVA_HOME=$("$JAVAC" -J-XshowSettings:properties -version 2>&1 | sed -n 's/^[[:space:]]*java.home = //p')
[ -n "$JAVA_HOME" ] && [ -x "$JAVA_HOME/bin/javac" ] || { echo "🔴 选中的 javac（${JAVAC}）没有返回完整 JDK Home" >&2; exit 1; }
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"
# 子项隔离（android/tools/gate_step.sh）：整套里每个子项失败都记下它自己的红行，结尾成段给 tools/gate.sh 的红步摘要
#   （11-23：原先 exec 直接跑整套、没 s2a_gate_init，隔离从没生效，通过的子项按设计打的负对照红行进了摘要，误导两轮定位）
source android/tools/gate_step.sh
s2a_gate_init
trap 's2a_gate_finish $?' EXIT
bash android/test.sh
