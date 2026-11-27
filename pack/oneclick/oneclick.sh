#!/bin/bash
# 一键出包：pack/oneclick/oneclick.sh <源包文件夹> [--keys <prod.keys>] [--fonts <共享字体目录>] [--sysdata <系统数据目录>] [--out <输出目录>] [--sign adhoc|dev] [--bundle-id <包名>] [--name <款>] [--events <事件文件>] [--target ios|mac|android|ohos] [--lite] [--title-id <程序号>] [--device <UDID|设备名>] [--install-serial <安卓序列号>]
# 一个游戏的 NSP / XCI（本体 + 更新 + DLC，放同一文件夹，子目录深 4 层内都算）+ prod.keys → 可侧载的 <款>.ipa + <款>资源包/。
#   ① 认游戏：nx_unpack --inspect 读每个包的内容清单，按程序号分组；恰好一个本体才往下走，32 位程序报红（S2Arm 只跑 AArch64）
#   ② 款名：pack/oneclick/titles.tsv 里有的用已知款名（宽屏启用表 / 界面布局 / 显示名靠它对上），没有用 t<程序号小写>；--name 可指定
#   ③ 解包：nx_unpack 完整解包（--title-id 只取这一款、--aoc-index 写 DLC 清单）到 <输出>/extract；键 = 选中包的路径 / 大小 / 修改时间 + nx_unpack 程序哈希，相同就复用
#   ④ 私有产物根：S2A_ROOTS=<输出>/roots、共享根指向不存在的路径（判私有 = 实验身份，不碰共享根、不碰调度）；seed_root（S2A_SEED_AOC=1 带 DLC）→ refresh.sh 全阶段
#      （改写 / 系统数据 / 宽屏界面布局 / 追加内容 / RomFS 索引 / 着色器全量预编，与正式出包同一套）→ ipa.sh
#   ⑤ 交付：<输出>/<款>.ipa 与 <输出>/<款>资源包/（资源包 v2，按「文件」导入；见 ~/xbl 用户体验线方案第二节）
# --sysdata：玩家从自己主机固件解出的系统数据目录（<16 位号>/ 子目录，各是解出的 RomFS；Mii 资源 0802、屏蔽词 0823 等），每款都提供：有真的用真的，没有的 0802 / 0823 合成空但合法的一份（pack/sysdata_need.py），不报红。
# 缺省：--keys ~/.switch/prod.keys，--fonts 走 stage_sysdata 缺省，--out <源包文件夹>/s2arm_<款>，--sign 按本机：配了签名团队（tools/machine.local S2A_TEAM_DEFAULT）用 dev，否则 adhoc（不要开发者账号）。
# prod.keys 只按路径交给 nx_unpack，不复制、不打印。任一步红 → 退出非 0，打出停在哪一步与日志位置。
# 普查用（10-25 运维线：285 款泛化普查，本机解包、rsync 到 gate-host / Spare-host 往下出；Spare-host 没有 prod.keys）：
#   --from-extract <解包目录>（放在第一个参数位置，代替源包文件夹）：跳过 ① 认包 / ③ 解包，直接用已解好的目录（exefs/ romfs/ ctl/ aoc/ unpack.tsv，
#     一键出包 <输出>/extract 或 pack/stages/unpack.sh extract 的产物）往下播种；认游戏读 unpack.tsv，不要 prod.keys / 原始包。--out 缺省 <解包目录>/../s2arm_<款>
#     多程序合集还需 S2A_SUBPROGRAM_SOURCES 指向原件目录与 --keys（启动器已提供）；只给序号 0 的解包目录时明确报红，不交缺程序的包。
# --target（10-25 维护者：一个入口出各平台；公共阶段 认游戏 / 解包 / 私有根 / 改写 / 系统数据 只跑一次，平台段各走各的）：
#   ios（缺省）：iPhone / iPad 同一个 IPA，平台段 = ipa.sh；
#   mac：只出 Mac 能跑的私有产物根（刷新按 S2A_PACK_PLAT=macos，直连库出 Mac 侧；不起宽屏自动出表），跳过 ④ 出包（iOS 编译 / 签名 / 资源包）与 ⑤ 交付；
#     最后一行打出产物根，Mac 局 S2A_IN=<它>（原 --mac 并进来，不另留入口）；
#   android：出单游戏 APK + 资源包（pack/targets/android/package.py；S2A_ANDROID_ENGINE 给了预编引擎目录就组装、不跑 Gradle），--install-serial 接着装机；
#   ohos：开发中，开跑前就报红、退出 3（鸿蒙只有原生层目标文件 runtime/ohos，没有 HAP 打包）。不认得的目标 → 报红、退出 2
# --device <UDID|设备名>（ios 目标，10-27 R13）：着色器的管线归档与函数体动态库按「目标 GPU 架构 × 系统大版本」出，对不上的设备整份不命中。
#   出包缺省按三种模式（pack/jobs/archs.py）：1. 显式 S2A_PSO_ARCHS > 2. S2A_PSO_ARCHS_ALL=1 全机型 > 3. 我方真机集合（pack/our_devices.tsv）。
#   本参数只在目标设备架构**不在**我方集合内时才 export 单架构（在集合内不动，保住 Duo 覆盖与阶段键）；
#   认设备的口径与启动器打 iPhone 包（pack/jobs/ipa.py）相同：--device > 连着的那台 > 开发描述文件登记的 UDID。
#   定不下来（没连设备 / 连了几台 / Mac 目标）按缺省出，并打一行提示；显式给了 S2A_PSO_ARCHS / S2A_PSO_ARCHS_ALL 就不按设备改。
set -u
H=$(cd "$(dirname "$0")" && pwd); P=$(cd "$H/.." && pwd)
[ ! -f "$P/../tools/data_vol_check.sh" ] || "${BASH:-/bin/bash}" "$P/../tools/data_vol_check.sh" || exit 125   # 数据卷（2T）没挂载 / 数据区被内置盘真目录替换：明确报错，不往内置盘写
# 机器读事件流（10-26，照上游的 --progress json）：--events <文件> 时每行一个 JSON 对象，都带 ev 与 t（秒）——
#   stage（k / n / name：第几步开始）、stage_end（name / ok / elapsed）、progress（name / done / total / frac：各阶段逐项计数，随心跳）、
#   say（msg：每条输出，报错在内）、result（ok；成功带 ipa / pack / files / size / build_id / graphics_mode（Mac 目标带 root），失败带 msg）。
#   人读输出不变。给启动器（pack/jobs/ipa.py 从 result 取交付物，不再按完成行的字面抠）与 AI 代操作；没给 --events 一行不写
EVF=; EVK=0; EVN=null; EVDONE=
jesc() { local s=$1; s=${s//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\t'/\\t}; s=${s//$'\r'/\\r}; s=${s//$'\n'/\\n}; printf '%s' "$s"; }   # 纯 bash（用户机的 /usr/bin/python3 可能是装命令行工具的引导桩）
ev() { [ -z "$EVF" ] || printf '{"ev":"%s","t":%s%s}\n' "$1" "$(date +%s)" "${2:-}" >> "$EVF"; }
t() { echo "[$(date +%T)] $*"; ev say ",\"msg\":\"$(jesc "$*")\""; }
# 计时（维护者 10-23：每次端到端出到 IPA 都要有计时）：每步起止秒数记进 TL，结尾打「步骤 → 秒」表并写 <输出>/logs/timing.tsv
T0=$SECONDS; TS=$SECONDS; TN=; TL=
lap() { [ -z "$TN" ] || { TL="$TL$TN	$((SECONDS - TS))
"; t "   ${TN}完成，用时 $(prog_dur $((SECONDS - TS)))"; ev stage_end ",\"name\":\"$(jesc "$TN")\",\"ok\":true,\"elapsed\":$((SECONDS - TS))"; }
  TN=${1:-}; TS=$SECONDS; [ -z "$TN" ] || { EVK=$((EVK + 1)); t "▶ ${TN}开始"; ev stage ",\"k\":$EVK,\"n\":$EVN,\"name\":\"$(jesc "$TN")\""; }; }   # 结尾不带参数的 lap 只收最后一步（set -u 下 $1 未定义会直接退出，10-23 Spare-host 0100000000010000出完包后在这里报错、计时表没写出）
die() { t "🔴 $*"; [ -z "$TN" ] || ev stage_end ",\"name\":\"$(jesc "$TN")\",\"ok\":false,\"elapsed\":$((SECONDS - TS))"; ev result ",\"ok\":false,\"msg\":\"$(jesc "$*")\""; EVDONE=1; exit 1; }
evexit() { [ -n "$EVDONE" ] || ev result ",\"ok\":false,\"msg\":\"一键出包没跑完就退出了（退出码 ${1}）\""; }   # EXIT：不经 die 的退出（exit 2 / 3、子命令红直接 exit）也给一条结果
# 长步骤进度（10-25 维护者「速度还是好慢啊，能看到进度么？」）：各阶段往 S2A_PROGRESS（<输出>/logs/progress.tsv，tools/progress.sh）写「已完成 / 总数」，
#   刷新 / 出包期间后台每 S2A_BEAT_S（缺省 30）秒打一行：已用多久、各个还没做完的阶段完成几项 / 共几项、按本段速度预计还剩多久
source "$P/../tools/progress.sh"; BEAT=
# 心跳的 sleep 放后台、子 shell 等它，收到 TERM 先停掉自己的 sleep 再退（10-25：原先 unbeat 只杀心跳子 shell，正在睡的 sleep 成孤儿、还拿着标准输出，
#   调用方用管道 / $(...) 收输出时出包结束后要多等一个心跳周期；sleep 也不接标准输出）
#   beat <阶段> [<该阶段日志>]：日志末行是在等别的进程（同款刷新 / 同提交出包，pack/refresh.sh、pack/ipa.sh）时接在心跳后面打出来；
#   不是时再看内存准入的排队行（<S2A_PROGRESS>.wait，tools/mem_admit.sh 写，审计 #1534 P1-110）
beat() { local b0; b0=$(date +%s); ( sp=; trap '[ -n "$sp" ] && kill "$sp" 2> /dev/null; exit 0' TERM
  while sleep "${S2A_BEAT_S:-30}" > /dev/null 2>&1 < /dev/null & sp=$!; wait "$sp"; do kill -0 $$ 2> /dev/null || exit 0; p=$(prog_show "${S2A_PROGRESS:-}")
    w=; [ -n "${2:-}" ] && w=$(grep -v '^[[:space:]]*$' "$2" 2> /dev/null | tail -1 | grep -E '已有一份刷新在跑|已有出包在跑' | cut -c1-200)
    # 内存准入排队（审计 #1534 P1-110）：tools/mem_admit.sh 排队时把最新【排队】行写进 <进度文件>.wait（进程号<TAB>行），写它的进程还在才认
    wp=; [ -z "$w" ] && [ -n "${S2A_PROGRESS:-}" ] && [ -s "${S2A_PROGRESS}.wait" ] && IFS=$'\t' read -r wp w < "${S2A_PROGRESS}.wait" 2> /dev/null
    [ -n "$wp" ] && { kill -0 "$wp" 2> /dev/null || w=; w=${w:0:200}; }
  t "   … ${1}进行中，已用 $(prog_dur $(( $(date +%s) - b0 )))${p:+；$p}${w:+；在等：$w}"; [ -z "$EVF" ] || evprog; done ) & BEAT=$!; }
# 各阶段逐项计数的最新一行（progress.tsv：时刻 阶段 已完成 总数）各发一条 progress 事件
evprog() { [ -s "${S2A_PROGRESS:-}" ] || return 0; awk -F'\t' -v t="$(date +%s)" 'NF >= 4 { d[$2] = $3; n[$2] = $4; if (!($2 in o)) { o[$2] = ++k; nm[k] = $2 } }
  END { for (i = 1; i <= k; i++) { s = nm[i]; gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s); printf "{\"ev\":\"progress\",\"t\":%s,\"name\":\"%s\",\"done\":%s,\"total\":%s,\"frac\":%s}\n", t, s, d[nm[i]] + 0, n[nm[i]] + 0, (n[nm[i]] > 0 ? sprintf("%.4f", d[nm[i]] / n[nm[i]]) : "null") } }' "$S2A_PROGRESS" >> "$EVF"; }
unbeat() { [ -z "$BEAT" ] || { kill "$BEAT" 2> /dev/null; wait "$BEAT" 2> /dev/null; BEAT=; }; }
trap 'evexit $?; unbeat' EXIT   # 第 ③ 步建临时目录时换成「unbeat + 删临时目录」
[ $# -ge 1 ] || { sed -n 2,3p "$0" | sed 's/^# //'; exit 2; }
FX=; MAC=0; SRC=; TGT=ios
if [ "$1" = --from-extract ]; then FX=$(cd "${2:-/nonexistent}" 2> /dev/null && pwd -P) || die "解包目录 ${2:-（没给）} 不在（--from-extract）"
  [ -f "$FX/exefs/main" ] && [ -f "$FX/unpack.tsv" ] || die "解包目录 ${FX} 要有 exefs/main 与 unpack.tsv（一键出包 <输出>/extract 或 pack/stages/unpack.sh extract 的产物）"; shift 2
else SRC=$(cd "$1" 2>/dev/null && pwd -P) || die "源包文件夹 $1 不在"; shift; fi
KEYS=$HOME/.switch/prod.keys; FONTS=; SYSDATA=; OUT=; SIGN=; BUNDLE=; NAME=; WANT_TID=; DEVICE=; INSTALL_SERIAL=
while [ $# -gt 0 ]; do case $1 in
  --keys) KEYS=${2:?}; shift 2;; --fonts) FONTS=${2:?}; shift 2;; --sysdata) SYSDATA=${2:?}; shift 2;; --out) OUT=${2:?}; shift 2;;
  --sign) SIGN=${2:?}; shift 2;; --bundle-id) BUNDLE=${2:?}; shift 2;; --name) NAME=${2:?}; shift 2;;
  --target) TGT=${2:-}; shift; [ $# -gt 0 ] && shift;; --target=*) TGT=${1#*=}; shift;;
  --title-id) WANT_TID=$(printf '%s' "${2:?}" | tr 'a-f' 'A-F'); shift 2;;   # 文件夹里有不止一个本体（游戏 + 同系列小工具）时指定要出哪一款（16 位程序号）
  --install-serial) INSTALL_SERIAL=${2:?}; shift 2;;
  --device) DEVICE=${2:?}; shift 2;;   # 目标设备（UDID 或设备名）：按它的 GPU 架构 / 系统大版本出着色器归档（见文件头 --device）
  --events) EVF=${2:?}; : > "$EVF" || die "事件文件写不了：$EVF"; shift 2;;   # 机器读事件流（见文件头 ev()）
  *) die "不认得的参数 $1";; esac; done
case $TGT in
  ios) ;; mac) MAC=1;;
  android) export S2A_PACK_PLAT=android S2A_TARGET=android S2A_WIDEN_AUTO_RUN=0;;
  ohos) t "🔴 目标 ohos：开发中，出不了能玩的包（鸿蒙只有原生层目标文件 runtime/ohos，没有 HAP 打包）；没跑任何阶段"; exit 3;;
  *) t "🔴 不认得的目标「${TGT}」：--target 只认 ios / mac / android / ohos"; exit 2;;
esac
[ -z "$INSTALL_SERIAL" ] || [ "$TGT" = android ] || die "--install-serial 只用于安卓目标"
if [ "$TGT" = android ]; then
  [ -z "$SIGN$BUNDLE$DEVICE" ] || die "安卓测试包使用 debug 签名；设备安装请用 --install-serial，不支持 --sign、--bundle-id 或 --device"
  command -v xxd > /dev/null || die "缺 xxd：请安装 xxd（用于读取游戏模块构建号），再重试安卓出包。"
fi
EVN=$([ "$MAC" = 1 ] && echo 4 || { [ -n "$INSTALL_SERIAL" ] && echo 7 || echo 6; })   # 事件流的总步数：认游戏 / 解包 / 播种 / 刷新各阶段（+ iOS 的 出包 / 交付）
# 缺省签名按这台机器（10-25 运维线：原先一律 adhoc，维护者出给测试设备的包装不上）：配了签名团队（tools/machine.local 的 S2A_TEAM_DEFAULT 或 S2A_TEAM）→ dev 开发直签；没配 → adhoc 自签（不要开发者账号）
SW="--sign 指定"; [ -n "$SIGN" ] || { . "$P/../tools/machine.sh" 2> /dev/null; if [ -n "${S2A_TEAM:-${S2A_TEAM_DEFAULT:-}}" ]; then SIGN=dev; SW="本机配了签名团队"; else SIGN=adhoc; SW="本机没配签名团队"; fi; }
case $SIGN in adhoc|dev) ;; *) die "--sign 只认 adhoc / dev";; esac
[ "$MAC" = 1 ] || [ "$TGT" = android ] || t "签名：${SIGN}（${SW}$([ "$SIGN" = adhoc ] && echo '；自签包装测试设备 / 别人的设备要先重签，测试设备用 --sign dev')）"
# C：渠道推荐（只列能保住 IML 的方式；SideStore 0.7.0 免费账号会丢失 IML，见 docs/研究/免费账号.md）
[ "$MAC" = 1 ] || [ "$TGT" = android ] || t "推荐安装渠道（保住提高内存上限权限）：Xcode Personal Team / AltStore Classic v2.3 / GetMoreRam（补救）；不推荐：SideStore 0.7.0 免费账号（freeFeatures 不含 IML，重签后丢失；SideStore#1616）。免费账号每台设备最多 3 个自签 App 名额，AltStore 本身也占 1 个。"
# Switch 2 导出目录（主机导出的松散 base/ updates/<版本>/ dlc/<标题号>/，各含 exefs / romfs，没有 NSP / XCI、没有控制分区；11-04 维护者 给的金刚 / raiders）：
#   源包文件夹本身或下一层有 base/exefs/main.npdm 就认作导出目录，下面 Python 就绪后由 pack/ns2_dump.py 整理成解包目录（<输出>/extract），往下同 --from-extract；不要 prod.keys
NS2D=; [ -z "$SRC" ] || for d in "$SRC" "$SRC"/*/; do d=${d%/}; [ -f "$d/base/exefs/main.npdm" ] && { NS2D=$d; break; }; done
# Switch 2 的 NUP 容器（一个 .nup 装本体 + 更新 + DLC，a55uka/nup v1；11-04 维护者 给的0400486024DEC000）：源是 .nup 文件，或文件夹里恰有一个 .nup；
#   下面 Python 就绪后由 pack/ns2_nup.py 展开成导出目录布局（<输出>/ns2_dump），再走上面导出目录同一条路；不要 prod.keys
NUPF=; if [ -z "$NS2D" ] && [ -n "$SRC" ]; then
  if [ -f "$SRC" ]; then case $SRC in *.nup|*.NUP) NUPF=$SRC;; esac
  elif [ -d "$SRC" ]; then n=0; for f in "$SRC"/*.nup "$SRC"/*.NUP; do [ -f "$f" ] && { n=$((n + 1)); NUPF=$f; }; done; [ "$n" = 1 ] || NUPF=; fi
fi
[ -n "$FX$NS2D$NUPF" ] || [ -f "$KEYS" ] || die "prod.keys 不在：${KEYS}（--keys 指定）"   # 从解包目录往下不用密钥（远端没有 prod.keys）
export S2A_KEYS=$KEYS S2A_GAME_SRC=${SRC:-${FX%/*}/.s2a-no-source}   # 从解包目录往下：原件目录指向不存在的路径（原件版本核对 / 追加内容阶段按「本机没原件」黄、跳过；DLC 已在解包目录 aoc/）
# 出包环境自检（10-23 维护者：不依赖开发机的私人路径，全新账号 / 别人的 Mac 也能跑；判据同 pack/jobs/applenv.py 与 Mac 前端的环境检查）：缺什么解包前就停、给一句怎么补
#   timeout：系统没有（没装 coreutils）就用仓库自带的 perl 版 tools/bin/timeout
command -v timeout > /dev/null || export PATH="$P/../tools/bin:$PATH"
#   zstd（解社区着色器参考库 .zst）：macOS 不自带；PATH 上没有就借装着的 S2Arm.app 自带的那份
for a in /Applications/S2Arm.app "$HOME/Applications/S2Arm.app"; do command -v zstd > /dev/null || { [ -x "$a/Contents/Resources/Runtime/bin/zstd" ] && export PATH="$PATH:$a/Contents/Resources/Runtime/bin"; }; done
if ! command -v zstd > /dev/null; then
  if [ "$(uname -s)" = Linux ]; then die "没有 zstd：请用系统包管理器安装 zstd（例如 apt install zstd）"
  else die "没有 zstd（解社区着色器参考库要用）：brew install zstd，或装 S2Arm.app（自带一份）"; fi
fi
#   Python：S2A_PYTHON → S2A_PY（环境 → tools/machine.local：开发机在那里声明钉版 venv，代码里不探私人路径）→ 装着的 S2Arm.app 内置 Python（钉版、包齐）→ PATH 上的 python3；
#   出包要的包与 App 内置 Python 同一份钉版清单
PY=${S2A_PYTHON:-${S2A_PY:-$(bash "$P/../tools/machine.sh" get S2A_PY 2> /dev/null)}}
for c in /Applications/S2Arm.app/Contents/Resources/Runtime/python/bin/python3 "$HOME/Applications/S2Arm.app/Contents/Resources/Runtime/python/bin/python3"; do
  [ -n "$PY" ] || { [ -x "$c" ] && PY=$c; }; done
[ -n "$PY" ] || PY=$(command -v python3 || true)
PYMIN=$(sed -n 's/^PYTHON_MINIMUM = (\([0-9]*\), \([0-9]*\)).*/\1.\2/p' "$P/jobs/toolchain.py")   # 作业层最低版本的唯一出处（toolchain.PYTHON_MINIMUM）
[ -n "$PYMIN" ] || die "读不到 pack/jobs/toolchain.py 的 PYTHON_MINIMUM"
[ -n "$PY" ] || die "没有 Python 3：装 Python ${PYMIN}+，或用 S2A_PYTHON=<解释器> 指定（S2Arm.app 里的 Contents/Resources/Runtime/python/bin/python3 也行）"
"$PY" -c "import sys; assert sys.version_info >= tuple(map(int, '${PYMIN}'.split('.')))" 2> /dev/null || die "${PY} 版本低于 ${PYMIN}：装 Python ${PYMIN}+ 或装 S2Arm.app（自带钉版 Python），或用 S2A_PYTHON=<解释器> 指定。装不进包（externally-managed / uv 托管）就建虚拟环境：python3 -m venv ~/s2a-venv && ~/s2a-venv/bin/python3 -m pip install -r launchers/macos/requirements-runtime.txt -r pack/fonts/requirements.txt，再 export S2A_PYTHON=~/s2a-venv/bin/python3"
"$PY" -c "import capstone, lz4, numpy, PIL" 2> /dev/null || die "${PY} 缺出包要的 Python 包：${PY} -m pip install -r launchers/macos/requirements-runtime.txt（与 S2Arm.app 内置 Python 同一份钉版清单）。装不进包（externally-managed / uv 托管）就建虚拟环境：python3 -m venv ~/s2a-venv && ~/s2a-venv/bin/python3 -m pip install -r launchers/macos/requirements-runtime.txt -r pack/fonts/requirements.txt，再 export S2A_PYTHON=~/s2a-venv/bin/python3"
#   选定的解释器所在目录放到 PATH 最前（#447 评审，照启动器 pack/jobs/core.py 的 env() 做）：出包链各 .sh 里的裸 python3 都落到这一个解释器，
#   不会在装了 anaconda 的机器上拿到 PATH 最前面那个 3.9（缺包 / 版本不对，在后面某个阶段再停）
export S2A_PYTHON=$PY S2A_PY=$PY PATH="$(dirname "$PY"):$PATH"
#   .NET：只在源码树现编 C# 时要——发版应用（启动器 pack/jobs/ipa.py 给齐预编件 / 读取器 / 随包壳）的 C# 件全为随 App 预编的原生单文件（NativeAOT），运行不再要 .NET 运行时
if [ -z "${S2A_PREBUILT_TOOLS:-}" ] || [ -z "${S2A_NX_UNPACK:-}" ] || [ -z "${S2A_SHELL:-}" ]; then
  #   源码树现编 C#：SDK 版本按 compat/tree/global.json 判（#449：只 grep 10.x 会放过 10.0.301，到编 C# 时才红）：选哪个 dotnet、够不够版本、报什么，都用 pack/jobs/ipa.py
  #   dotnet_sdk() / dotnet_missing() 那一处（applenv.py 与启动器环境检查同一判法）；它按 DOTNET → DOTNET_ROOT → ~/.dotnet → PATH → 常见安装位置找
  #   读取器的 apphost 要 DOTNET_ROOT，按 --list-sdks 给的真实 SDK 根补（在 Python 之后查：选 SDK 用 Python 那一处判法）
  DNR=$("$PY" -c 'import sys; sys.path.insert(0, sys.argv[1]); from ipa import dotnet_sdk, dotnet_missing; r, e = dotnet_sdk(); print(f"{e}\t{r}" if e else "\t" + dotnet_missing())' "$P/../pack/jobs") \
    || die "查不了 .NET SDK（${PY} 跑 pack/jobs/ipa.py 失败）"
  DN=${DNR%%$'\t'*}
  [ -n "$DN" ] || die "${DNR#*$'\t'}"
  export DOTNET=$DN PATH="$(dirname "$DN"):$PATH"
  [ -n "${DOTNET_ROOT:-}" ] || export DOTNET_ROOT="${DNR#*$'\t'}"
fi
#   共享字体：没给 --fonts 就用开源替代字体（维护者 10-30：缺省开源、原版只在显式给时用；来源规则在 pack/stages/sysdata.sh，一处定）
# 并行度按本机核数（tools/cores.sh：重烘 / 出库路数 = min(核数, 内存一半 ÷ 1.5 GB)，make 等按全部核；G04 10-23 起一键出包就按本机用满，10-25 起 refresh / ipa.sh 直调也同一处算）
#   在本单起任何子进程（现编 nx_unpack、认游戏、解包）之前算：cores.sh 按 1 分钟负载封顶，原先放在解包之后，读到的是自己刚跑完的 8 路解包，
#   空闲的 8 核玩家机器也只给 5 路（11-02 Spare-host 冷出0100152000022000 直连段 1009 秒，10-30 的 8 路是 795 秒）
source "$P/../tools/cores.sh"; s2a_cores 2>&1 || die "并行度读不出（tools/cores.sh）"
source "$P/srcs.sh"   # SRC_N = nx_unpack（按源码哈希缓存编好的那份，没有就现编）
# 从解包目录往下（--from-extract）也要它：程序位数只由 nx_unpack --npdm-isa 判（全仓唯一一份判法）
[ -x "$SRC_N" ] || die "nx_unpack 没编出来（dotnet build -c Release pack/nx_unpack；要 .NET 10 SDK）"
# Switch 2 NUP 容器 / 导出目录（放在 nx_unpack 编好之后：ZBIC 压缩的 NSO 由 ns2_dump 交 nx_unpack --nso-normalize 解）
if [ -n "$NUPF" ]; then
  B=${OUT:-$(dirname "$SRC")/s2arm_$(basename "${SRC%.[nN][uU][pP]}")}; mkdir -p "$B" || die "输出目录建不了：$B"
  ND=$(basename "${NUPF%.[nN][uU][pP]}"); ND=${ND%% \[*}   # 导出目录名 = 文件名去掉「 [程序号][版本]」：没有控制分区时 ns2_dump 拿它当标题
  t "Switch 2 NUP 容器 ${NUPF}：展开成导出目录 $B/ns2_dump/${ND}（pack/ns2_nup.py）"
  "$PY" "$P/ns2_nup.py" "$NUPF" "$B/ns2_dump/$ND" 2>&1 | while IFS= read -r l; do t "   $l"; done
  [ "${PIPESTATUS[0]}" = 0 ] || die "NUP 容器展开失败（${NUPF}）"
  NS2D=$B/ns2_dump/$ND; OUT=${OUT:-$B}
fi
if [ -n "$NS2D" ]; then
  B=${OUT:-$(dirname "$SRC")/s2arm_$(basename "$SRC")}; mkdir -p "$B" || die "输出目录建不了：$B"
  t "Switch 2 导出目录 ${NS2D}：整理成解包目录 $B/extract（pack/ns2_dump.py）"
  S2A_NX_UNPACK=$SRC_N "$PY" "$P/ns2_dump.py" "$NS2D" "$B/extract" 2>&1 | while IFS= read -r l; do t "   $l"; done
  [ "${PIPESTATUS[0]}" = 0 ] || die "Switch 2 导出目录整理失败（${NS2D}）"
  FX=$(cd "$B/extract" && pwd -P); OUT=${OUT:-$B}; SRC=; export S2A_GAME_SRC=${FX%/*}/.s2a-no-source
fi
TMP=$(mktemp -d); trap 'evexit $?; unbeat; rm -rf "$TMP"' EXIT

# ① 认游戏
lap 认游戏
if [ -z "$FX" ]; then
find -L "$SRC" -maxdepth 4 \( -iname '*.nsp' -o -iname '*.xci' -o -iname '*.nsz' -o -iname '*.xcz' \) -type f -print 2> /dev/null | LC_ALL=C sort > "$TMP/files"   # -L：原件挪走、原位置留软链的也认（10-26），-type f 按目标判
[ -s "$TMP/files" ] || die "$SRC 下没有 .nsp / .xci / .nsz / .xcz"
i=0; while IFS= read -r f; do i=$((i + 1))
  "$SRC_N" "$KEYS" --inspect "$f" > "$TMP/i$i.json" 2> /dev/null || { t "🟡 读不出内容清单，跳过：${f#$SRC/}"; rm -f "$TMP/i$i.json"; }
  printf '%s\t%s\n' "$i" "$f" >> "$TMP/idx"
done < "$TMP/files"
S2A_WANT_TID=$WANT_TID python3 "$P/source_pick.py" "$TMP" > "$TMP/pick" || exit 1
else   # 从解包目录认：unpack.tsv 的程序号 / 名称 / 显示版本；32 位按 exefs/main.npdm 的 META 标志判（S2Arm 只跑 AArch64）；直连版本旁注由共享算法按解包产物写（见下面「直连零配置推导要的版本元数据」）
python3 - "$FX" "$TMP" "$SRC_N" > "$TMP/pick" <<'PY' || exit 1
import os, subprocess, sys
x, nxu = sys.argv[1], sys.argv[3]   # 第二个参数（临时目录）原先用来写旁注；版本算法搬到 pack/direct_profile_input.py 后不再用
u = dict(l.rstrip('\n').split('\t', 1) for l in open(os.path.join(x, 'unpack.tsv'), encoding='utf-8') if '\t' in l)
tid = (u.get('title_id') or '').upper()
if len(tid) != 16: print(f'🔴 {x}/unpack.tsv 没有 16 位 title_id', file=sys.stderr); sys.exit(1)
npdm = os.path.join(x, 'exefs', 'main.npdm')
# 位数工具失败 / 没输出 / unknown 一律报红停（审计 P2-29：原先只拒 aarch32，其余照常当 64 位往下出包）
r = subprocess.run([nxu, '--npdm-isa', npdm], capture_output=True, text=True) if os.path.exists(npdm) else None
isa = r.stdout.strip() if r else ''
if r and r.returncode == 0 and isa == 'aarch32': print(f'🔴 {u.get("name", tid)} 是 32 位程序，S2Arm 只跑 AArch64', file=sys.stderr); sys.exit(1)
if not r or r.returncode != 0 or isa != 'aarch64':
    why = '文件不存在' if not r else f'nx_unpack --npdm-isa 退出 {r.returncode}、输出「{isa or "空"}」' + (f'：{r.stderr.strip().splitlines()[-1][:160]}' if r.stderr.strip() else '')
    print(f'🔴 读不出 {npdm} 的程序位数（{why}），不当 64 位往下出包', file=sys.stderr); sys.exit(1)
aoc = [a for a in (u.get('aoc') or '').split(',') if a]
print(tid); print(u.get('name', '')); print(u.get('display_version', ''))
print(f'从解包目录 {x} 往下（更新 {u.get("update") or "无"}，DLC {len(aoc)} 个）')
PY
fi
{ read -r TID; read -r GNAME; read -r GVER; read -r GNOTE; } < "$TMP/pick"; [ -n "$FX" ] || sed -n '5,$p' "$TMP/pick" > "$TMP/sel"
G=$NAME; [ -n "$G" ] || G=$(awk -F'\t' -v t="$TID" '$1 == t {print $2; exit}' "$H/titles.tsv"); [ -n "$G" ] || G=t$(echo "$TID" | tr 'A-F' 'a-f')
case $G in *[!A-Za-z0-9_-]*|'') die "款名只许字母 / 数字 / _ / -（${G}）";; esac
OUT=${OUT:-${SRC:-${FX%/*}}/s2arm_$G}; mkdir -p "$OUT/logs" && OUT=$(cd "$OUT" && pwd -P) || die "输出目录建不了"
if [ -z "$FX" ]; then
  "$PY" "$P/save_identity.py" --inspect "$TMP" --title-id "$TID" --output "$OUT/save-identity.json" || die "存档游戏身份生成失败"
  export S2A_SAVE_IDENTITY=$OUT/save-identity.json
fi
t "① 认出：${GNAME}（${GVER}，程序号 ${TID}，款 ${G}）；${GNOTE}$([ -n "$FX" ] || echo "；用 $(wc -l < "$TMP/sel" | tr -d ' ') 个包")；输出 $OUT"
# 直连优先、零配置（10-25 维护者「不应该卡这么死来判断是否直连出包，咱们不是在做通用直连么」）：缺省每款都出直连包——
#   refresh.sh 先按游戏文件推导直连配置（仓库里有手写配置档就用它当加速提示，没有照样推导），再出直连着色器库。
#   10-30 起直连单轨（维护者）：直连做不成就报红、写清哪一步，不退成通用包；11-07 兼容模式（只出通用）整条拆掉
t "① 画面：直连单轨（按游戏文件推导配置、出直连着色器库；直连做不成就报红）"
# ① 目标 GPU 架构 / 系统大版本（10-27 R13 坑 1；10-28 按维护方 A3 对齐 ops/gpu-archs 的三种模式）
#   出包缺省三种模式（pack/jobs/archs.py，阶段自己也会解析）：1. 显式 S2A_PSO_ARCHS > 2. S2A_PSO_ARCHS_ALL=1 全机型 >
#   3. 我方真机集合（pack/our_devices.tsv）。命令行出包原先不认目标机型，集合外的机型装箱后才发现 link_glue 报「no stub archive」。
#   这里只做一件窄事：目标设备架构**不在**我方集合内才 export 单架构；在集合内不动（保住 Duo 覆盖、阶段键不变）。
if [ "$TGT" = android ]; then
  t "① 安卓使用 Vulkan 运行时，按设备能力选择后端"
elif [ "$MAC" = 1 ]; then
  t "① 目标架构：Mac 目标按本机 GPU 架构出（--target mac 不读设备）；给 iPhone 出包用 --target ios --device <UDID>"
elif [ -n "${S2A_PSO_ARCHS:-}" ]; then
  t "① 目标架构：${S2A_PSO_ARCHS}${S2A_PSO_OS:+ / iOS ${S2A_PSO_OS}}（显式给 S2A_PSO_ARCHS，不按设备改）"
elif [ "${S2A_PSO_ARCHS_ALL:-0}" = 1 ]; then
  t "① 目标架构：全机型（S2A_PSO_ARCHS_ALL=1，按 Xcode 设备库全集出，不按设备改）"
else
  ta=(); [ -z "$DEVICE" ] || ta=(--device "$DEVICE")
  TR=$("$PY" "$P/jobs/target.py" device ${ta[@]+"${ta[@]}"} 2> "$TMP/target.err"); trc=$?
  if [ $trc = 0 ] && [ -n "$TR" ]; then
    IFS=$'\t' read -r T_ARCH T_OS T_PT T_MODEL T_SRC T_OURS <<< "$TR"
    case " ${T_OURS:-} " in
      *" $T_ARCH "*)   # 已在集合内：不 export，下游按缺省集合出（含 Duo），阶段键不动
        t "① 目标架构：按缺省我方真机集合出（${T_OURS}；目标 ${T_MODEL} ${T_PT} 的 ${T_ARCH} 已在集合内，不单编）" ;;
      *) export S2A_PSO_ARCHS=$T_ARCH
        t "① 目标架构：${T_ARCH}（目标 ${T_MODEL} ${T_PT} 不在我方真机集合 ${T_OURS:-?} 内：只覆盖该架构，其他机型首次遇到现编；要全机型给 S2A_PSO_ARCHS_ALL=1）" ;;
    esac
    # 系统大版本只认读得到的正值："-" / 0 会出 ios0 归档、通用闸报「iOS 0」（维护方 A3 第 4 条）
    case ${T_OS:-} in ''|0|*[!0-9]*) ;; *) [ -n "${S2A_PSO_OS:-}" ] || export S2A_PSO_OS=$T_OS;; esac
  else
    TRP=$(tail -1 "$TMP/target.err" 2> /dev/null | cut -c1-160)
    t "① 目标架构：没认出目标设备，按缺省出（我方真机集合；要全机型给 S2A_PSO_ARCHS_ALL=1，要单架构给 S2A_PSO_ARCHS）${TRP:+（${TRP}）}"
  fi
fi

# ③ 解包（--from-extract：用给的目录，键 = 它的 unpack.tsv + exefs/main 的内容（审计 P1-54：原先是大小 / 修改时间，保住这两样换了 main 照用旧的））
if [ -n "$FX" ]; then
  # 键还要含 exefs 全部模块与 ctl/control.nacp 的内容：播种把它们原样克隆进产物根，而解包工具改了（如 ns2_dump 合成 nacp 补存档类别）重写这两样时 unpack.tsv 照旧不变，
  #   只按 unpack.tsv + main 定键会不重播种、旧 nacp 进包（11-05 0400486024DEC000第二版：解包目录 nacp 已声明临时存储 128 MB，包里仍是 0 → MountTemporaryStorage 2002-1002 Break）
  KM=$(cd "$FX" && for f in exefs/* ctl/control.nacp rt_shaders/*; do [ ! -f "$f" ] || printf '%s %s\n' "$(shasum -a 256 < "$f" | cut -c1-64)" "$f"; done) || die "读不出 $FX/exefs 或 ctl/control.nacp"
  # romfs/ 与追加内容（aoc/、aoc.tsv：播种同样原样克隆进产物根）也进键（审计 #1534 P1-54：原先不在键里，重解包只换了 romfs / 追加内容照用旧产物根）：
  #   目录取盘点身份（graphics/nvn/tools/romfs_list.py：相对路径 + 大小 + 修改时间纳秒，只 lstat、不读内容，27 万文件也只是盘点一遍），没有的记「-」
  KA=$(cd "$FX" && "$PY" -c 'import os, sys; sys.path.insert(0, sys.argv[1]); import romfs_list
for d in sys.argv[2:]:
    i = romfs_list.scan(d).identity if romfs_list.is_dir(d) else "-"   # is_dir：romfs/ 不在、只有镜像 romfs.std 时盘点镜像（身份随镜像换而换）
    if i is None: sys.exit(1)
    print(d, i)' "$P/../graphics/nvn/tools" romfs aoc && { [ ! -f aoc.tsv ] || shasum -a 256 < aoc.tsv; }) || die "盘点不了 $FX 的 romfs/ 或 aoc/"
  K=$( { cat "$FX/unpack.tsv"; echo "$KM"; echo "$KA"; } | shasum -a 256 | cut -c1-16); X=$FX
  lap 解包; t "③ 解包：用给的解包目录 ${FX}（不解）"; UNPACK=reused
else
# 键 = 选中的原件内容抽样（pack/srcs.sh src_sample：大小 + 首尾各 64 KiB；审计 P1-54：原先是「路径 大小 修改时间」，保住这三样换了原件照用旧解包）+ nx_unpack 哈希；读不出就停
KM=$(f=(); while IFS= read -r x; do f+=("$x"); done < "$TMP/sel"; [ ${#f[@]} = 0 ] || src_sample "${f[@]}") || die "读不出原件内容（src_sample）"
K=$( { [ -z "$KM" ] || printf '%s\n' "$KM"; shasum -a 256 "$SRC_N" "$SRC_N.dll" 2> /dev/null | cut -c1-64; } | shasum -a 256 | cut -c1-16)
X=$OUT/extract
lap 解包
if [ "$(cat "$X/.oneclick_key" 2> /dev/null)" = "$K" ] && [ -f "$X/exefs/main" ]; then t "③ 解包：输入没变，复用 $X"; UNPACK=reused
else
  UNPACK=built
  t "③ 解包（本体 + 更新合并、DLC 清单）…"; [ ! -e "$X.tmp" ] || rm -rf "$X.tmp"
  sel=(); while IFS= read -r f; do sel+=("$f"); done < "$TMP/sel"
  "$SRC_N" "$KEYS" "$X.tmp" --aoc-index --title-id="$TID" "${sel[@]}" > "$OUT/logs/unpack.log" 2>&1 || die "解包失败：$(tail -1 "$OUT/logs/unpack.log" | cut -c1-200)（$OUT/logs/unpack.log）"
  echo "$K" > "$X.tmp/.oneclick_key"; [ ! -e "$X" ] || rm -rf "$X"; mv "$X.tmp" "$X" || die "解包目录改名失败"
  t "   $(tail -1 "$OUT/logs/unpack.log" | cut -c1-200)"
fi
fi

# 直连零配置推导要的版本元数据。算法只一份：pack/direct_profile_input.py，读解包目录的 unpack.tsv 写旁注，
#   解包阶段（pack/stages/unpack.sh extract）出包后调的是同一份。这里不再自己算版本——原先「从原件认」按 inspect JSON 算、
#   「从解包目录认」按 unpack.tsv 的 update 补，两套算法口径不同（update 空时后者取 0，是错的值）；且每多一条出包路径就漏一次
#   （11-03 22:42 01002DA013484000 手测单：packq 从原件播种没补旁注，刷新停在 missing content version）。
#   目录里已有旁注（调用方给的 / 上次出的）→ 保留不覆盖；没有才按 unpack.tsv 写。
"$PY" "$P/direct_profile_input.py" "$X" || die "直连配置版本旁注没写成（解包目录 ${X}）：解包产物太旧就用当前 nx_unpack 重新解包"

# ④ 私有产物根：播种 → 全阶段刷新 → 出包
export S2A_ROOTS=$OUT/roots S2A_ROOTS_SHARED=$OUT/.no-shared-root S2A_SEED_AOC=1
[ "$MAC" = 1 ] && export S2A_PACK_PLAT=macos S2A_WIDEN_AUTO_RUN=0   # --target mac：刷新按 Mac 口径（直连库出 Mac 侧）；不起宽屏自动出表（它要占 Mac 局槽跑闭环）
[ -z "$FONTS" ] || export S2A_FONTS=$FONTS
if [ -n "$SYSDATA" ]; then S2A_SYSDATA=$(cd "$SYSDATA" 2> /dev/null && pwd -P) || die "系统数据目录 $SYSDATA 不在（--sysdata）"; export S2A_SYSDATA; fi
source "$P/roots.sh"
# result 的 aoc_missing：没解出的追加内容原始包文件名（pack/stages/aoc.sh 记在产物根 aoc.log；审计 P2-30），启动器 / AI 代操作按它提示「包内不含」
aocj() { local f s=; while IFS= read -r f; do s="$s${s:+,}\"$(jesc "$f")\""; done < <(root_aoc_missing "$(root_cur "$G")"); printf '"aoc_missing":[%s],' "$s"; }
_root_identity_shared && die "产物根被判成共享根（${S2A_ROOTS}）：不出，免得冒充正式身份"
lap 播种
if [ "$(cat "$OUT/.seeded" 2> /dev/null)" != "$K" ] || [ -z "$(root_cur "$G")" ]; then
  bash "$P/seed_root.sh" "$G" "oc-${K:0:8}" "$X" > "$OUT/logs/seed.log" 2>&1 || die "播种失败：$(tail -1 "$OUT/logs/seed.log")（$OUT/logs/seed.log）"
  echo "$K" > "$OUT/.seeded"; t "④ 播种：$(basename "$(root_cur "$G")")"
fi
lap 刷新各阶段
t "④ 刷新各阶段（改写 / 系统数据 / 宽屏界面布局 / 追加内容 / RomFS 索引 / 直连配置推导 / 抽取 / 直连库；首次几分钟到十几分钟）…"
# 调用方给了进度文件（启动器作业：<私有>/logs/<任务>.progress.tsv，每秒读新增行发给界面）就沿用、不清空；没给（命令行）才用输出目录里的一份
#   （10-26 维护者 看 Spare-host 出包页：原先这里一律改写到 <输出>/logs/progress.tsv，启动器那份收不到任何一行，界面上一直没有计数）
[ -n "${S2A_PROGRESS:-}" ] || { export S2A_PROGRESS=$OUT/logs/progress.tsv; : > "$S2A_PROGRESS"; }; beat 刷新 "$OUT/logs/refresh.log"
rfc=0; S2A_WIDEN_PKG=$([ "$MAC" = 1 ] || [ "$TGT" = android ] && echo 0 || echo 1) bash "$P/refresh.sh" "$G" > "$OUT/logs/refresh.log" 2>&1 || rfc=$?; unbeat   # S2A_WIDEN_PKG：改写段一过就起掌机档宽屏自动出表，ipa.sh 收尾前等它
# 直连单轨（10-30 维护者）：直连做不成 refresh.sh 报红「直连单轨停在 <哪一步>：<原因>」退 1，下面 die 照抄那行；不再退成通用包
[ $rfc = 0 ] && export S2A_REFRESHED=$(basename "$(root_cur "$G")")   # 交给 ipa.sh：刚刷过、现行没变就不再刷（一键流程 refresh 只跑一次）
[ $rfc = 0 ] || die "刷新停在：$(grep '【红】' "$OUT/logs/refresh.log" | grep -v '刷新停在' | tail -1 | cut -c1-300)（$OUT/logs/refresh.log）"
t "   $(tail -1 "$OUT/logs/refresh.log" | cut -c1-1200)"
if [ "$TGT" != android ]; then
  cur=$(root_cur "$G"); beat 多程序合集 "$OUT/logs/sub_programs.log"
  "$PY" "$P/multi_program.py" --extract "$X" --work "$OUT" --logs "$OUT/logs" --product "$cur" \
    --game "$G" --source-dir "${S2A_SUBPROGRAM_SOURCES:-$S2A_GAME_SRC}" --platform "$([ "$MAC" = 1 ] && echo macos || echo ios)" \
    > "$OUT/logs/sub_programs.log" 2>&1 || { unbeat; die "多程序合集构建失败：$(tail -1 "$OUT/logs/sub_programs.log" | cut -c1-300)"; }
  unbeat; cur=$(root_cur "$G"); export S2A_REFRESHED=$(basename "$cur")
  [ ! -s "$OUT/logs/sub_programs.log" ] || t "   $(tail -1 "$OUT/logs/sub_programs.log" | cut -c1-1200)"
fi
if [ "$TGT" = android ]; then
  # 刷新那行「🟢 … Android 共享资源已刷新：…；🟡 <n> 项：…」带的黄项（直连库段没出等，#783 合入线 11-03 答 ③）进出包汇总：这里打一行，完成行与 result 事件也带上
  AW=$(sed -n 's/^🟢 .*Android 共享资源已刷新：[^；]*；\(🟡 .*\)$/\1/p' "$OUT/logs/refresh.log" | tail -1)
  [ -z "$AW" ] || t "   $AW"
  lap 安卓出包; beat 安卓出包
  "$PY" "$P/targets/android/package.py" --root "$(root_cur "$G")" --out "$OUT" > "$OUT/logs/android.log" 2>&1 || { unbeat; die "Android 出包失败（$OUT/logs/android.log）：$(tail -1 "$OUT/logs/android.log" | cut -c1-300)"; }
  unbeat
  delivery=$(tail -1 "$OUT/logs/android.log")
  APK=$("$PY" -c 'import json,sys; print(json.load(sys.stdin)["apk"])' <<< "$delivery") || die "Android 出包没有 APK 路径"
  RES=$("$PY" -c 'import json,sys; print(json.load(sys.stdin)["resources"])' <<< "$delivery") || die "Android 出包没有资源包路径"
  ART=$("$PY" -c 'import json,sys; print(json.load(sys.stdin)["artifact"])' <<< "$delivery") || die "Android 出包没有账本路径"
  [ -f "$APK" ] && [ -f "$RES/manifest.json" ] && [ -f "$ART" ] || die "Android 交付不完整"
  if [ -n "$INSTALL_SERIAL" ]; then
    lap 安装与导入
    t "   装到手机 ${INSTALL_SERIAL}：手机上弹出安装确认（如「未经安全检测」）时，勾选并点「继续安装」；同款旧包签名不同时要先在手机上卸载"
    "$PY" "$P/../android/tools/deploy_game.py" --apk "$APK" --resources "$RES" --serial "$INSTALL_SERIAL" --no-start > "$OUT/logs/android-install.log" 2>&1 || die "设备安装或导入失败（$OUT/logs/android-install.log）：$(tail -1 "$OUT/logs/android-install.log" | cut -c1-300)"
  fi
  lap; t "🟢 完成：$APK + $RES + $ART$(root_aoc_note "$(root_cur "$G")")${AW:+；$AW}"
  ev result ",\"ok\":true,\"apk\":\"$(jesc "$APK")\",\"pack\":\"$(jesc "$RES")\",\"artifact\":\"$(jesc "$ART")\",$(aocj)${AW:+\"warnings\":[\"$(jesc "$AW")\"],}\"elapsed\":$((SECONDS - T0))"; EVDONE=1
  exit 0
fi
if [ "$MAC" = 1 ]; then   # --target mac：到此为止，不出 iOS 包 / 资源包
  lap; { printf '步骤\t秒\n%s' "$TL"; printf '合计\t%s\n' $((SECONDS - T0)); } > "$OUT/logs/timing.tsv"
  t "计时：$(tail -n +2 "$OUT/logs/timing.tsv" | awk -F'\t' '{printf "%s %s 秒；", $1, $2}')"
  t "🟢 完成（Mac 产物根）：${GNAME}（${GVER}）→ $(root_cur "$G")（Mac 局 S2A_IN=<它>）$(root_aoc_note "$(root_cur "$G")")"; echo "$(root_cur "$G")" > "$OUT/logs/mac_root.txt"
  ev result ",\"ok\":true,\"root\":\"$(jesc "$(root_cur "$G")")\",$(aocj)\"elapsed\":$((SECONDS - T0))"; EVDONE=1; exit 0
fi
# E：出包第一步读设备窗口提示（只提示，不中止；读不到设备 / addr_need.tsv 缺失不报错）
_IML_HINT=$(python3 "$P/iml_check.py" hint_device "${S2A_PSO_OS:-0}" "$(root_cur "$G" 2>/dev/null)/addr_need.tsv" 2>/dev/null) && [ -n "$_IML_HINT" ] && t "$_IML_HINT" || true
lap 出包
t "④ 出包（编 App、${SIGN} 签名、资源包）…"
[ -z "$BUNDLE" ] || export S2A_BUNDLE_ID=$BUNDLE
beat 出包 "$OUT/logs/ipa.log"; S2A_SIGN=$SIGN S2A_DISPLAY_SUFFIX= bash "$P/ipa.sh" "$G" > "$OUT/logs/ipa.log" 2>&1 || { unbeat; die "出包失败：$(grep '【红】' "$OUT/logs/ipa.log" | tail -1 | cut -c1-200)（$OUT/logs/ipa.log）"; }; unbeat
PK=$(root_cur "$G")/pkg; [ -f "$PK/$G.ipa" ] && [ -f "$PK/pack/manifest.json" ] || die "出包没产出 ipa / 资源包（${PK}）"
# 启动器（pack/jobs/ipa.py）成功时写 <输出>/shell.id：命令行一键出包同样写一份，否则启动器判「这款不是本壳出的」，装机 / 推资源包报 container_stale
[ ! -s "$(root_cur "$G")/.shell_id" ] || printf '%s' "$(cat "$(root_cur "$G")/.shell_id")" > "$OUT/shell.id"   # 小戳直接写；rc_lint 对 cp 要判返回码，这里不需要

# ⑤ 交付（APFS 克隆，不多占盘）
lap 交付
rm -f "$OUT/$G.ipa"
if ! cp -c "$PK/$G.ipa" "$OUT/$G.ipa" 2> /dev/null; then cp "$PK/$G.ipa" "$OUT/$G.ipa" || die "拷 ipa 失败"; fi   # cp -c 是 APFS 克隆（macOS 才有），别处退回普通拷贝
# 资源包按两份 index.tsv 增量交付（pack/oneclick/deliver_pack.py）：只克隆变了的、删掉新版没有的；原先整份 rm -rf + cp -cR，0400486024DEC000 18.7 万个文件每次约 4 分钟
D=$OUT/${G}资源包
DLV=$("$PY" "$P/oneclick/deliver_pack.py" "$PK/pack" "$D") || die "交付资源包失败（${D}）"
IFS=$'\t' read -r NF DB DCL DRM DMODE < <(printf '%s\n' "$DLV"); SZ=$(awk -v b="$DB" 'BEGIN { printf "%.1fG", b / 1073741824 }')
t "   资源包交付（${DMODE}）：克隆 $DCL 个、删 $DRM 个"
lap; { printf '步骤\t秒\n%s' "$TL"; printf '合计\t%s\n' $((SECONDS - T0)); } > "$OUT/logs/timing.tsv"
# 冷 / 热按各部件自己报的命中算（pack/oneclick/cachehits.py；10-24：按「跳过 / 复用」字样判两头都误报过）
CH=$("$PY" "$P/oneclick/cachehits.py" --unpack "$UNPACK" --refresh "$OUT/logs/refresh.log" --shader "$(root_cur "$G")/shaders/shader_pack.json" --ipa "$OUT/logs/ipa.log" --build "$PK/build.log" 2> /dev/null) || CH="未知	cachehits.py 没跑成"
t "计时（${CH%%	*}）：$(tail -n +2 "$OUT/logs/timing.tsv" | awk -F'\t' '{printf "%s %s 秒；", $1, $2}')"
t "   缓存：${CH#*	}"
[ -f "$PK/timing.tsv" ] && t "   出包子步：$(tail -n +2 "$PK/timing.tsv" | awk -F'\t' '{printf "%s %s 秒；", $1, $2}')"
# 包号（内测问题回报模板要填）：出包阶段键 ipa.sh 写在现行产物根的 .stage_pkg；同时落一份到 logs/package.txt
#   包号 = 代码提交号-整串包键的哈希前 8 位（短、能对到具体产物）；完整包键写进 logs/package.txt
PKF=$(root_cur "$G")/.stage_pkg; PKID=; . "$P/build_id.sh"; [ -s "$PKF" ] && PKID=$(s2a_build_id "$(cat "$PKF")")   # 与 IPA Info.plist 的 S2ABuildID 同一算法（pack/build_id.sh）
{ echo "包号 ${PKID:-未知}"; cat "$PKF" 2> /dev/null; } > "$OUT/logs/package.txt"
t "🟢 完成：${GNAME}（${GVER}）→ $OUT/$G.ipa（$(du -h "$OUT/$G.ipa" | cut -f1)，${SIGN} 签名）+ ${D}（$NF 个文件，${SZ}）$(root_aoc_note "$(root_cur "$G")")"
t "   包号 ${PKID:-未知}（回报问题时请填）"
# 画面模式取交付 IPA 里写进去的那份（pack/ipa_tail.py 写 S2AGraphicsMode，iOS「关于本包」与 Mac 启动器读的同一份；pack/oneclick/ipa_mode.py）。
#   11-01 #1104：原先读产物根中间目录 build/ios/S2Arm.app/Info.plist，那份不在就落 unknown，作业记录两单都是 unknown；读不到就报红，不猜
GM=$("$PY" "$P/oneclick/ipa_mode.py" "$OUT/$G.ipa") || die "交付的 IPA 读不出画面模式（$OUT/$G.ipa）"
ev result ",\"ok\":true,\"ipa\":\"$(jesc "$OUT/$G.ipa")\",\"pack\":\"$(jesc "$D")\",\"files\":${NF:-0},\"size\":\"$(jesc "$SZ")\",\"build_id\":\"$(jesc "${PKID:-}")\",\"graphics_mode\":\"$(jesc "$GM")\",$(aocj)\"elapsed\":$((SECONDS - T0))"; EVDONE=1
