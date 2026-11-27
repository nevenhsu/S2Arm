#!/bin/bash
# 服务层单测：编译并跑 S2A.Services.Tests（假原生层 + 桩渲染器，经生产同一 C# 单一入口发 IPC 消息）。
# 输出「期望 → 真结果」对位表；退出码 0 = 全过。墙钟上限 300 秒（编译 + 运行）。
# 本机自测排在门禁后面：不在门禁里跑（没有 GATE2_PAR）就把自己降到 nice 15，子进程继承；门禁步的优先级由门禁自己定
[ -n "${GATE2_PAR:-}" ] || renice 15 -p $$ > /dev/null 2>&1 || true
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); DOTNET=${DOTNET:-$HOME/.dotnet/dotnet}
# 只在 macOS 上跑（审计 #859）：原生库（libvpx / libopus 垫片按 osx 构建、libSystem 读数）都是苹果的，Linux x86_64 不能冒充 osx-x64；
#   别的系统打一行说明、退出码 75（列车约定：环境未就绪，不算红）
if [ "$(uname -s)" != Darwin ]; then echo "服务单测：只在 macOS 上跑，本机 $(uname -s) $(uname -m)，环境未就绪（退出码 75）" >&2; exit 75; fi
# 时区系统数据：由宿主 IANA 时区库生成（只生成一次）
export S2A_TZDATA=${S2A_TZDATA:-${TMPDIR:-/tmp}/s2a_tzdata}
[ -f "$S2A_TZDATA/binaryList.txt" ] || bash "$HERE/systemdata/make_tzdata.sh" "$S2A_TZDATA" > /dev/null
# 共享字体：本机旧库数据（任天堂系统数据，不进仓库）
[ -d "$HOME/.s2arm/data/sysfont/raw" ] && export S2A_SHARED_FONT_DIR=${S2A_SHARED_FONT_DIR:-$HOME/.s2arm/data/sysfont/raw}
# 产品是 NativeAOT（没有分层编译）；JIT 分层升级的时机会让零分配计量偶发多出几字节（关掉后 3/3 稳定，开着 2/4 判红），按产品口径关掉
export DOTNET_TieredCompilation=0
# 单测 JIT 的库要匹配出包机；游戏 / iPhone 的执行目标仍由各自构建入口决定。
case $(uname -m) in
  arm64) HOST_RID=osx-arm64 ;;
  x86_64) HOST_RID=osx-x64 ;;
  *) echo "🔴 服务单测：不支持的宿主架构 $(uname -m)" >&2; exit 1 ;;
esac
# VP8 解码（libvpx + 垫片）：单测经动态库调（宿主里是静态链）
VPX_A=$(bash "$HERE/vpx/build.sh" "$HOST_RID" 2>/dev/null | tail -1); [ -f "$VPX_A" ] && export S2A_VPX_DYLIB=${VPX_A%/*}/libs2a_vpx.dylib
# Opus 解码（libopus + 垫片）：同上
OPUS_A=$(bash "$HERE/opus/build.sh" "$HOST_RID" 2>/dev/null | tail -1); [ -f "$OPUS_A" ] && export S2A_OPUS_DYLIB=${OPUS_A%/*}/libs2a_opus.dylib
cd "$HERE/S2A.Services.Tests"
# 构建成败看命令自己的退出码，筛选只决定打印什么（10-26 #414）：原先这条管道在 set -euo pipefail 下，
#   中文成功汇总（「0 个错误」）与合法空输出会让 grep 返回 1、整步提前退出，后面的单测一次都不跑；
#   中文失败诊断与超时的原退出码 42 / 124 也被管道尾巴的 1 覆盖，诊断被筛掉。判定与打印统一在 build_output.sh。
BLOG=$HERE/S2A.Services.Tests/bin/build.out
bash "$HERE/build_output.sh" run "$BLOG" timeout 240 "$DOTNET" build -c Release -nologo -v q || exit $?
# 生命周期、ARP 与动态 provider 各在独立进程，避免静态服务状态互相污染。
# 放在完整服务测试之前：即使后续既有 LDN 基线崩溃，这些门禁仍会执行并传播自身失败。
s2a_modes=$(timeout 60 "$DOTNET" bin/Release/net10.0/S2A.Services.Tests.dll --list-modes) || { echo "服务专项：--list-modes 失败（退出 $?）"; exit 1; }
# 独立模式由单测程序自己列（冻结的老模式 + 标了 [SoloTest] 的新模式，见 S2A.Services.Tests/TestDiscovery.cs）：新单测不改这里
# 各模式本来就各占一个进程、互不共享状态（临时目录按进程号 / GUID、联机端口按空闲端口取），并行起（11-14 门禁提速：120 多个进程串行约 90 秒，
#   每个大半是进程启动与 JIT），并发 = 核数一半；结论按清单顺序报，第一个失败的照旧打末 30 行、带它的退出码退出、不跑主测试
MD=$HERE/S2A.Services.Tests/bin; MJ=$(( $(sysctl -n hw.ncpu 2> /dev/null || echo 2) / 2 )); [ "$MJ" -ge 1 ] || MJ=1
for mode in $s2a_modes; do rm -f "$MD/${mode#--}.out.rc"; done
printf '%s\n' $s2a_modes | xargs -P "$MJ" -I{} sh -c 'o="$2/${1#--}.out"; timeout 60 "$3" bin/Release/net10.0/S2A.Services.Tests.dll "$1" > "$o" 2>&1; echo $? > "$o.rc"' _ {} "$MD" "$DOTNET" \
  || { echo "服务专项：并行起进程失败（xargs 退出 $?）"; exit 1; }
# 结论全部报完再判（11-27 维护者：门禁摘要要带专项射程——原先第一个失败就退，后面的专项没报、汇总行也没有）：每个专项一行，
#   最后一行「服务专项汇总：M 个全部通过」或「M 个，失败 K 个：<模式…>」（services/gate.sh 并进摘要）；有失败照旧带第一个失败的退出码退出、不跑主测试
s2a_n=0; s2a_fail=; s2a_rc=0
for mode in $s2a_modes; do
  s2a_n=$((s2a_n + 1)); OUT="$MD/${mode#--}.out"; rc=$(cat "$OUT.rc" 2> /dev/null || echo 1)
  if [ "$rc" = 0 ]; then
    echo "服务专项 ${mode}：通过"
  else
    echo "服务专项 ${mode}：失败（退出 ${rc}）"; tail -30 "$OUT"; s2a_fail="${s2a_fail:+$s2a_fail }$mode"; [ "$s2a_rc" = 0 ] && s2a_rc=$rc
  fi
done
if [ -n "$s2a_fail" ]; then echo "服务专项汇总：${s2a_n} 个，失败 $(echo $s2a_fail | wc -w | tr -d ' ') 个：${s2a_fail}"; exit "$s2a_rc"; fi
echo "服务专项汇总：${s2a_n} 个全部通过"
bash "$HERE/tests/t_native_provider.sh"
# 运行墙钟 150 秒：着色器代码表新旧对拍（01006F8002326000 + 一款第一方动作冒险游戏真实语料约 44 万样本，上游旧表平方级建表）约 40 秒
# 标准错误落本工作树的构建目录（10-21 原生层线：原先是全机固定名 $TMPDIR/s2a_svc_test.err，多棵工作树同时跑互相覆盖，查现场拿到的是别的树那份）；
#   红了把路径和末 20 行打出来（门禁日志里直接能看到崩溃栈）
ERR=$HERE/S2A.Services.Tests/bin/test.stderr; mkdir -p "${ERR%/*}"
# 卡死留现场（10-21 原生层线：门禁里单测卡在声音输出注销的 Join 上，150 秒到点被杀、什么都没留下）：
#   到上限前 10 秒还没跑完，先 sample 5 秒抓全部线程栈进 bin/test.hang.txt，再由 timeout 到点杀（退出码仍是 124）。上限 S2A_SVC_TEST_S（缺省 150，单测用短的）
HANG=$HERE/S2A.Services.Tests/bin/test.hang.txt; rm -f "$HANG"; LIM=${S2A_SVC_TEST_S:-150}
timeout "$LIM" "$DOTNET" bin/Release/net10.0/S2A.Services.Tests.dll 2> "$ERR" & TP=$!
( for _ in $(seq 1 $((LIM - 10))); do kill -0 $TP 2> /dev/null || exit 0; sleep 1; done
  DP=$(pgrep -P $TP | head -1); [ -n "$DP" ] && /usr/bin/sample "$DP" 5 -file "$HANG" > /dev/null 2>&1 ) & WD=$!
wait $TP && rc=0 || rc=$?; wait $WD 2> /dev/null || true
if [ $rc != 0 ]; then echo "服务层单测退出码 ${rc}，标准错误 ${ERR}（末 20 行）："; tail -20 "$ERR" | sed 's/^/    | /'
  [ -s "$HANG" ] && echo "卡死现场（到点前抓的线程栈）：${HANG}"; exit $rc; fi
