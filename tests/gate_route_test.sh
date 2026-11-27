#!/bin/bash
# 门禁只看：tools/gate_route.py tools/gate_decl.py tools/stage_inputs.py pack/stage_inputs/*.tsv tests/autoplay_test.sh tests/dev_run_test.sh tests/pack_jobs_test.sh tests/pack_test.sh tests/score_test.sh tests/stage_keys_test.sh tests/svc_hang_test.sh tests/xc_key_test.sh tests/diagzip_test.sh
#   （10-28：断言的是对真实仓库算出的路由；改断言点名的单测（含它们头上的门禁声明）、路由脚本 / 表 / 阶段闭包才跑。被点名的工具脚本改了只影响「点名边」这类断言，交给改路由的那一笔）
# 门禁路由单测（tools/gate_route.py，读本仓库真实文件，约 3 秒）：bash tests/gate_route_test.sh
# 期望按路由规则手写（各钩子文件头「# 门禁路由：」+ 单测文本引用）：改调度只跑调度波及的单测、不跑钩子与 iOS；改文档什么都不跑；
# 改服务 → 服务 / 原生 / 画幅钩子 + iOS，不跑 gfx 钩子；表外代码路径 → 钩子全跑；空改动 → 全跑；证据阶梯按目录分级
set -u; H=$(cd "$(dirname "$0")/.." && pwd); cd "$H"; bad=0
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
r() { python3 tools/gate_route.py "$@" | tr '\n' ' '; }
NT=$(( $(ls tests/*_test.sh | wc -l | tr -d ' ') + 1 ))   # 根单测 + 已登记的独立 drain_readflush 入口
# 单测自己带目录点名的本线外文件按仓库路径认（10-22：toolchain271 改 graphics/metal/Makefile、xc_key.sh，路由没带上 tests/xc_key_test.sh，main 红在它上）；
#   只认单测自己点名的，不顺着编排脚本往下认（cc_cache 单测在假仓库里跑桩，不该挂到 gfx 工具上）
o=$(r tests graphics/tools/shbake/xc_key.sh); ck "改 graphics/tools/shbake/xc_key.sh → 含 xc_key 单测、不含 dev_run / pack 单测（实得：${o}）" '[[ "$o" == *tests/xc_key_test.sh* ]] && [[ "$o" != *dev_run_test* ]] && [[ "$o" != *pack_test* ]]'
o=$(r tests graphics/metal/Makefile); ck "改 graphics/metal/Makefile（没扩展名，靠单测头「门禁覆盖」）→ 含 xc_key 单测（实得：${o}）" '[[ "$o" == *tests/xc_key_test.sh* ]]'
o=$(r tests services/opus/build.sh); ck "改 services/opus/build.sh（单测按 services/ 下相对路径写 opus/build.sh，末两段唯一）→ 含 svc_hang 单测（实得：${o}）" '[[ "$o" == *tests/svc_hang_test.sh* ]]'
# idle_timeout.sh 被多个单测门禁只看点名；safe_killpg.sh 也在闭包里：改 safe_killpg.sh 带上关联单测、不含 autoplay / dev_run
o=$(r tests tools/safe_killpg.sh); ck "改 tools/safe_killpg.sh → 单测含 killpg_lint、不含 autoplay / dev_run（实得：${o}）" '[[ "$o" == *tests/killpg_lint_test.sh* ]] && [[ "$o" != *autoplay_test* ]] && [[ "$o" != *dev_run_test* ]]'
ck "改 tools/safe_killpg.sh → 钩子 0 个、iOS 不跑" '[ -z "$(r hooks tools/safe_killpg.sh | tr -d " ")" ] && [ "$(r ios tools/safe_killpg.sh)" = "no " ]'
o=$(r tests tools/redline.py); ck "Python 同目录 import 也算覆盖：改 redline.py → score 单测（实得：${o}）" '[[ "$o" == *score_test* ]]'
ck "源文件后缀 .m 也认（10-28 维护者：改 ios/diagzip.m 原先选不中 diagzip_test）" '[[ "$(r tests ios/diagzip.m)" == *tests/diagzip_test.sh* ]]'
# #925：期望直接来自三个生产消费者的测试合同，不从路由器输出生成期望。
o=$(r tests graphics/metal/cs/NxTrack.cs); ck "C# NxTrack 改动 → 独立排空/读时回写测试（实得：${o}）" '[[ "$o" == *graphics/track/tests/drain_readflush/build_run.sh* ]]'
o=$(r tests compat/tree/src/Ryujinx.Graphics.Gpu/Shader/ShaderCache.cs); ck "C# ShaderCache 改动 → 垃圾代码与失败负缓存测试" '[[ "$o" == *tests/shader_garbage_guard_test.sh* ]] && [[ "$o" == *tests/shader_fault_cache_test.sh* ]]'
o=$(r tests compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs); ck "C# StateUpdater 改动 → 垃圾代码与失败负缓存测试" '[[ "$o" == *tests/shader_garbage_guard_test.sh* ]] && [[ "$o" == *tests/shader_fault_cache_test.sh* ]]'
o=$(r tests compat/tree/src/Ryujinx.Graphics.Gpu/Shader/S2AShaderFaultCache.cs); ck "C# 负缓存自身改动 → 对应纯逻辑测试" '[[ "$o" == *tests/shader_fault_cache_test.sh* ]]'
o=$(r tests docs/常见问题.md); ck "普通文档负对照 → 不跑三个C#目标测试" '[[ "$o" != *drain_readflush/build_run.sh* ]] && [[ "$o" != *shader_garbage_guard_test.sh* ]] && [[ "$o" != *shader_fault_cache_test.sh* ]]'
o=$(r tests graphics/metal/cs/NxTrack.cs.bak); ck "仅文件名近似 → 不选排空测试" '[[ "$o" != *drain_readflush/build_run.sh* ]]'
# 改文档要跑的 = 「每次」单测 + 头部「门禁覆盖」点名 *.md 的单测（如 no_remote_exec_pipe：用户照抄的文档里不许「下载 + 执行」一步走；11-09 它加了 *.md 覆盖后本断言没跟上，main 已知红）
#   + 列车入队预检固定跑的那组（shell_lint / shared_path / test_wired / shell_pyimports + 全部 *lint*_test，11-10 起门禁每次跑，见 gate_route.precheck_test）
MD=$( { grep -l '^# 门禁：每次' tests/*_test.sh; grep -lE '^# 门禁覆盖：.*[*][.]md( |$)' tests/*_test.sh; ls tests/*lint*_test.sh; printf 'tests/%s.sh\n' shell_lint_test shared_path_test test_wired_test shell_pyimports_test; } | sort -u | tr '\n' ' ')
ck "改文档 → 只有「每次」与点名覆盖 *.md 的单测、钩子不跑、iOS 不跑（实得：$(r tests docs/a.md)）" '[ "$(r tests docs/a.md)" = "$MD" ] && [ -z "$(r hooks docs/a.md | tr -d " ")" ] && [ "$(r ios docs/a.md)" = "no " ]'
o=$(r hooks graphics/tools/shbake/x.py); ck "改出包期着色器工具 graphics/tools → 跑 gfx 钩子、不跑画幅回放（10-24 门禁整改：回放器不编 graphics/tools；实得：${o}）" '[[ "$o" == *graphics/gate.sh* ]] && [[ "$o" != *tests/visual/gate.sh* ]]'
o=$(r hooks graphics/metal/x.m); ck "改渲染器 graphics/metal → 画幅回放照跑（实得：${o}）" '[[ "$o" == *tests/visual/gate.sh* ]]'
o=$(r hooks services/S2A.Services/A.cs); ck "改服务 → 服务钩子，不跑原生钩子（原生门禁 make host 不带 S2A_CS、不链 C#，99 组 C 单测不碰服务层；10-28 梳理）、不跑 gfx 钩子、不跑画幅回放；iOS 跑（实得：${o}）" '[[ "$o" != *runtime/gate.sh* ]] && [[ "$o" == *services/gate.sh* ]] && [[ "$o" != *tests/visual/gate.sh* ]] && [[ "$o" != *graphics/gate.sh* ]] && [ "$(r ios services/S2A.Services/A.cs)" = "yes " ]'
o=$(r hooks compat/tree/src/Ryujinx.Graphics.Vulkan/Effects/Textures/SmaaAreaTexture.bin); ck "改 SMAA 查表数据（graphics/nvn 生成 nvn_aa_lut.h、NVN 单测链它）→ 跑原生钩子（实得：${o}）" '[[ "$o" == *runtime/gate.sh* ]]'
o=$(r hooks compat/tree/src/Ryujinx.HLE/HOS/Services/Hid/IHidServer.cs); ck "改 Ryujinx 服务代码 → 不跑原生钩子、跑服务钩子（实得：${o}）" '[[ "$o" != *runtime/gate.sh* ]] && [[ "$o" == *services/gate.sh* ]]'
o=$(r hooks android/app/A.java); ck "改安卓外壳 → 安卓无设备测钩子 android/gate.sh（11-20 接回）+ 安卓新图形层钩子（实得：${o}）" '[ "$(echo $o)" = "android/gate.sh android/native/gfx_plat/gate.sh" ]'
o=$(r hooks runtime/src/svc_mem.c .github/workflows/windows-cleanup.yml); ck "改 runtime/src + 表外路径 → 全跑含两个安卓钩子（android/gate.sh 11-20 接回）、不含已删的 core-link / ctest-link 旧钩子（实得：${o}）" '[[ "$o" == *runtime/gate.sh* ]] && [[ "$o" == *android/native/gfx_plat/gate.sh* ]] && [[ " $(echo $o) " == *" android/gate.sh "* ]] && [[ "$o" != *android/core-link/* ]] && [[ "$o" != *android/ctest-link/* ]]'
o=$(r hooks runtime/rewriter/rewriter.c); ck "改改写器（runtime/rewriter/rewriter.c）→ 原生 + gfx + 鸿蒙 + 安卓钩子（ohos/gate.sh 路由含 runtime/rewriter/：鸿蒙复用同一改写器；android/gate.sh 11-26 按实测读数加 runtime/rewriter/；实得：${o}）" '[ "$(echo $o)" = "android/gate.sh graphics/gate.sh ohos/gate.sh runtime/gate.sh" ]'
o=$(r hooks weird/x.c); ck "表外代码路径 → 钩子全跑（实得：${o}）" '[ "$(wc -w <<< "$o" | tr -d " ")" = "$(find . -mindepth 2 -maxdepth 4 -name gate.sh -not -path "./tools/*" -not -path "./.git/*" | wc -l | tr -d " ")" ]'
o=$(r hooks launchers/macos/Sources/S2ArmLauncher/Store.swift); ck "改 Mac 前端 → 仅 Mac 钩子、iOS 不跑" '[ "$o" = "launchers/macos/gate.sh " ] && [ "$(r ios launchers/macos/Sources/S2ArmLauncher/Store.swift)" = "no " ]'
o=$(r hooks pack/roots.sh); ck "公共 pack 文件保留单测路由、不挂前端钩子" '[ -z "$(echo "$o" | tr -d " ")" ] && [[ "$(r tests pack/roots.sh)" == *tests/pack_test.sh* ]]'
o=$(r hooks tools/toolchain.sh); ck "公共工具链保留原有路由、不挂前端钩子" '[ -z "$(echo "$o" | tr -d " ")" ] && [ "$(r ios tools/toolchain.sh)" = "no " ]'
# tool 档要跑的钩子 = 文件头「# 门禁路由：」明确点到改动的（10-31：原先 tool 档一个钩子都不跑）
o=$(r claimed pack/jobs/core.py pack/contracts/README.md); ck "改 pack/jobs/ → claimed 出 Mac 钩子 + 安卓钩子（android/gate.sh 11-26 按实测读数加 pack/jobs/：安卓应用层单测读它；实得：${o}）" '[ "$o" = "android/gate.sh launchers/macos/gate.sh " ]'
o=$(r claimed tools/gate_plan.sh tests/gate_plan_same_test.sh pack/roots.sh); ck "改纯本线工具 → claimed 0 个钩子、不吃表外兜底（实得：${o}）" '[ -z "$(echo "$o" | tr -d " ")" ]'
o=$(r claimed weird/x.c); ck "表外代码路径 → claimed 不兜底全跑（全跑归 full 档 hooks；实得：${o}）" '[ -z "$(echo "$o" | tr -d " ")" ]'
ck "空改动 → 单测全跑（$NT 份）" '[ "$(r tests | wc -w | tr -d " ")" = "$NT" ]'
ck "证据：改 native → 整局九款" '[[ "$(r evidence runtime/src/a.c)" == *整局* ]]'
ck "证据：改 ARMeilleure（执行模型）→ 整局" '[[ "$(r evidence compat/tree/src/ARMeilleure/a.cs)" == *整局* ]]'
ck "证据：改着色器翻译 → GAL 回放、不起整局" 'o=$(r evidence compat/tree/src/Ryujinx.Graphics.Shader/a.cs); [[ "$o" == *GAL* ]] && [[ "$o" != *整局：* ]]'
ck "证据：改服务 → IPC 回放、不起整局" 'o=$(r evidence services/S2A.Services/A.cs); [[ "$o" == *IPC* ]] && [[ "$o" != *整局：* ]]'
ck "证据：只改工具 → 单测即可" '[[ "$(r evidence tools/safe_killpg.sh)" == 单测即可* ]]'
# owns（10-25 维护者：列车判红的钩子与本批有没有关）：按路由表 graphics/gate.sh 触发前缀 graphics/ runtime/ …——graphics/nvn 下的改动、runtime/ 下的改动都算它负责；文档、tools/ 不算
o=$(r owns graphics/gate.sh graphics/nvn/gfx/nvn/nvn_mtl.m runtime/src/gfxmode.c docs/a.md tools/pack_fp.sh); ck "owns graphics/gate.sh：只打 graphics/nvn/… 与 runtime/src/gfxmode.c（实得：${o}）" '[ "$o" = "graphics/nvn/gfx/nvn/nvn_mtl.m runtime/src/gfxmode.c " ]'
ck "owns：只改 tools/ 与文档 → 空" '[ -z "$(r owns graphics/gate.sh tools/pack_fp.sh docs/a.md | tr -d " ")" ]'
# 10-27 两例选测缺口：改 pack/jobs/saves.py（tool 档不跑 launchers/macos 钩子）→ 必选 pack_jobs 单测；改 pack/stage_direct.sh（在直连阶段闭包里；11-05 前样例是 runtime/run_mac.sh，出包不再起局后它不在闭包里）→ 必选 stage_keys 单测；
#   改不在任何阶段闭包里的工具（tools/redline.py）两者都不选（原样例 tools/safe_killpg.sh 11-19 起被计算管线归档阶段 cs_pipelines.py 调用、进了阶段闭包，选 stage_keys 是对的）；临时克隆里 stage_direct.sh 新调用一个新脚本 tools/zz_probe.py（#550 的 mac_chain_check.py 同型）→ 改它也必选 stage_keys
o=$(r tests pack/jobs/saves.py); ck "改 pack/jobs/saves.py → 含 pack_jobs 单测（实得：${o}）" '[[ "$o" == *tests/pack_jobs_test.sh* ]]'
o=$(r tests pack/stage_direct.sh); ck "改 pack/stage_direct.sh → 含 stage_keys 单测" '[[ "$o" == *tests/stage_keys_test.sh* ]]'
o=$(r tests tools/redline.py); ck "改 tools/redline.py（不在阶段闭包）→ 不含 pack_jobs / stage_keys 单测" '[[ "$o" != *pack_jobs_test* ]] && [[ "$o" != *stage_keys_test* ]]'
T=$(mktemp -d); git clone -q --shared "$H" "$T/c" 2> /dev/null && (cd "$T/c" && git checkout -q "$(git -C "$H" rev-parse HEAD)" 2> /dev/null; cp "$H/tools/gate_route.py" "$H/tests/stage_keys_test.sh" "$H/tests/pack_jobs_test.sh" "$T/c/tools/" 2> /dev/null
  cp "$H/tools/gate_route.py" tools/; cp "$H/tests/stage_keys_test.sh" "$H/tests/pack_jobs_test.sh" tests/; rm -f tools/stage_keys_test.sh tools/pack_jobs_test.sh
  echo 'print(1)' > tools/zz_probe.py; printf '\npython3 "$R/tools/zz_probe.py" > /dev/null\n' >> pack/stage_direct.sh
  git add -A > /dev/null && git -c user.email=t@t -c user.name=t commit -qm probe && python3 tools/gate_route.py tests tools/zz_probe.py) > "$T/o" 2>&1
ck "临时克隆：stage_direct.sh 新调用 tools/zz_probe.py，改它 → 含 stage_keys 单测（实得：$(tr '\n' ' ' < "$T/o" | cut -c1-200)）" 'grep -qx tests/stage_keys_test.sh "$T/o"'
rm -rf "$T"
# 构建清单自动进覆盖面（11-09 维护方：present_settings_test 现算 $(HOST_SRCS) 把 macos/front_mac.m 编进来，头上「门禁只看」手写清单没有它，
#   feat/launch-settings 改 front_mac.m 列车没跑、合入后 main 链接红）：期望按 launchers/macos/build.mk 的 HOST_SRCS += macos/front_mac.m 手写
o=$(r tests macos/front_mac.m); ck "改 macos/front_mac.m（HOST_SRCS 的 Mac 前端源文件）→ 含 present_settings_test（实得：${o}）" '[[ "$o" == *tests/present_settings_test.sh* ]]'
o=$(r tests runtime/src/input_config.c); ck "改 runtime/src/input_config.c（HOST_SRCS）→ 含 present_settings_test、shared_matrix_test（实得：${o}）" '[[ "$o" == *tests/present_settings_test.sh* ]] && [[ "$o" == *tests/shared_matrix_test.sh* ]]'
# 链接清单（「门禁清单」，11-19）：present_settings_test 只编只链 graphics/nvn/tests/t_present_settings.link.txt 列的宿主源文件，覆盖面按同一份清单算——
#   清单里的 front_mac.m / input_config.c 照旧选中（上两条）；清单外的宿主文件（host_main.c 从不链进本测）与拼清单的 mk 不再选中；改清单本身两支都选中
o=$(r tests runtime/src/host_main.c); ck "改 runtime/src/host_main.c（不在链接清单）→ 不含 present_settings_test（实得：${o}）" '[[ "$o" != *tests/present_settings_test.sh* ]]'
o=$(r tests launchers/macos/build.mk); ck "改 launchers/macos/build.mk → 不含 present_settings_test（实得：${o}）" '[[ "$o" != *tests/present_settings_test.sh* ]]'
o=$(r tests graphics/nvn/tests/t_present_settings.link.txt); ck "改链接清单 → 含 present_settings_test、present_settings_env_test（实得：${o}）" '[[ "$o" == *tests/present_settings_test.sh* ]] && [[ "$o" == *tests/present_settings_env_test.sh* ]]'
# 算构建清单不许有副作用（11-09：make 解析就改写 build/.host_cfg，门禁里一算路由 runtime/gate.sh 并行各组同时重编同一批 .o、链接读到空文件）：
#   在 clone 里放一份已知配置戳、带 S2A_CS=1 环境算一次路由 → 戳内容与修改时间都不变、没起 C# 构建（build/cs 不出现）
#   11-19：present_settings_test 改读链接清单后仓库里没有单测再展开构建清单，clone 里放一支展开它的探针单测，让路由真去算
T=$(mktemp -d); git clone -q --shared "$H" "$T/c" 2> /dev/null && cp "$H/tools/gate_route.py" "$T/c/tools/" && mkdir -p "$T/c/build" && printf 'S2A_CS= sentinel' > "$T/c/build/.host_cfg" && touch -t 202601010000 "$T/c/build/.host_cfg"
cat > "$T/c/tests/zz_hostsrcs_test.sh" << 'PROBE'
#!/bin/bash
printf '%s\n' 'x:;@echo $(HOST_SRCS)' | make -s -f Makefile -f - x
PROBE
(cd "$T/c" && git add tests/zz_hostsrcs_test.sh && git -c user.email=t@t -c user.name=t commit -qm probe) > /dev/null 2>&1
m0=$(stat -c %Y "$T/c/build/.host_cfg" 2> /dev/null || stat -f %m "$T/c/build/.host_cfg"); (cd "$T/c" && S2A_CS=1 python3 tools/gate_route.py tests macos/front_mac.m > "$T/o" 2> /dev/null)
m1=$(stat -c %Y "$T/c/build/.host_cfg" 2> /dev/null || stat -f %m "$T/c/build/.host_cfg")
ck "算构建清单不碰本树 build/：配置戳内容 / 修改时间不变、不起 C# 构建，路由照常（含展开构建清单的探针单测）" '[ "$(cat "$T/c/build/.host_cfg")" = "S2A_CS= sentinel" ] && [ "$m0" = "$m1" ] && [ ! -e "$T/c/build/cs" ] && grep -q zz_hostsrcs_test "$T/o"'
rm -rf "$T"
# 覆盖面缓存（11-14：同内容只算一次，存 git 公共目录 s2a-gate-route/）：同树二次调用与关缓存结果逐字相同、且快；
#   工作区里一份单测新点名 tools/l10n_merge.py → 下一次路由立刻带上它（指纹含工作区改动，不吃旧缓存）
T=$(mktemp -d); git clone -q --shared "$H" "$T/c" 2> /dev/null && (cd "$T/c" && git checkout -q "$(git -C "$H" rev-parse HEAD)" 2> /dev/null); cp "$H/tools/gate_route.py" "$T/c/tools/"
gr() { (cd "$T/c" && python3 tools/gate_route.py tests tools/l10n_merge.py 2> /dev/null | tr '\n' ' '); }
c1=$(gr); t0=$SECONDS; c2=$(gr); dt=$((SECONDS - t0)); c0=$(cd "$T/c" && GATE_ROUTE_CACHE=0 python3 tools/gate_route.py tests tools/l10n_merge.py 2> /dev/null | tr '\n' ' ')
echo 'python3 "$H/tools/l10n_merge.py" --help > /dev/null 2>&1 || :' >> "$T/c/tests/os_junk_sync_test.sh"; c3=$(gr)
ck "覆盖面缓存：二次调用 ${dt} 秒、与关缓存逐字相同；单测新点名文件后立刻路由到它" '[ "$c1" = "$c2" ] && [ "$c2" = "$c0" ] && [ $dt -le 2 ] && ! grep -q os_junk_sync_test <<< "$c1" && grep -q os_junk_sync_test <<< "$c3"'
rm -rf "$T"
# 只拷进假仓库跑的脚本不顺闭包展开、拷走的目录算输入（11-14 门禁减负）：gate_budget_test 只把 tools/gate.sh 拷进假仓库跑，
#   旧口径顺着拷贝一路吃进 tests/gcbench/scan.py 这类无关文件；dev_run_test 用 cp -R 拷走整个 pack/，旧口径改 pack/orch/__init__.py 却不跑它
r1=$(GATE_ROUTE_CACHE=0 python3 "$H/tools/gate_route.py" tests tests/gcbench/scan.py 2> /dev/null); r2=$(GATE_ROUTE_CACHE=0 python3 "$H/tools/gate_route.py" tests pack/orch/__init__.py 2> /dev/null)
ck "只拷不跑的脚本不牵出无关文件（改 tests/gcbench/scan.py 不跑 gate_budget_test）；cp -R 拷走的目录算输入（改 pack/orch/__init__.py 跑 dev_run_test）" '! grep -q gate_budget_test <<< "$r1" && grep -q dev_run_test <<< "$r2"'
[ $bad = 0 ] && echo "gate_route 单测全过" || { echo "gate_route 单测有不通"; exit 1; }
