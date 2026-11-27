#!/bin/bash
# 合入门禁（底本旧库 tools/gate.sh @a0d0574 的框架：分档、「期望 → 真结果」、核实际跑了几个 = 应跑几个）：在仓库根跑 tools/gate.sh
# 环境：GATE_TIER=doc|tool|full（缺省 full）；GATE_CHANGED="<改到的文件，空格分隔>"（列车给，钩子可按需缩小范围）
# 档：
#   doc   只改普通 docs/ 或 *.md（docs/**/*.tsv、docs/排查/、skills/ 除外）：只跑公开仓扫描；改动集按 GATE_CHANGED，没给就取 origin/main...待测提交
#   tool  只改 tools/ tests/（tests/visual 除外）pack/，或 docs/**/*.tsv、docs/排查/、skills/：本线自查（语法 + 路由选中的单测）
#         + 文件头「# 门禁路由：」明确点到这些改动的钩子（gate_route.py claimed；声明「门禁层：合入后」的钩子照旧只在合入后那层跑）
#   full  其余：本线自查 + 各线钩子
# 固定步（full 档）：Ryujinx 补丁核对（compat/verify.sh --rev HEAD）。
# 各线钩子：仓库里每个 <目录>/gate.sh（tools/ 自身除外，深度 ≤ 3）都是一条钩子，由归属线维护，例如 compat/gate.sh（上游 + 补丁 = 树内源码）、
#   runtime/gate.sh（构建 + 单测）、graphics/gate.sh、tests/visual/gate.sh（对拍 / 满屏判据）。钩子在仓库根以 bash 运行，退出 0 = 绿；
#   一行「期望 → 真结果」写到标准输出最后一行。钩子各自墙钟上限 GATE_HOOK_S（缺省 600）秒，并行跑。
# 按改动路由（10-20 维护者：回归测试拖慢开发，跑最小充分证据）：GATE_CHANGED 非空时单测 / 钩子 / iOS 编译只跑被改动波及的
#   （tools/gate_route.py：单测覆盖面按文本里点名的文件传递推、钩子与 iOS 编译查各钩子文件头「# 门禁路由：」；表外代码路径 = 钩子全跑）；空 = 全跑。
# 输出：每步一行「🟢/🔴 <步> <秒> 秒：<末行>」，最后一行「门禁 绿|红：跑了 N 步 / 应跑 N 步」；退出码 0 绿 / 1 红。
# 派 gate-host（10-21 维护者：本机 15 分钟负载 ≤36，门禁的重编译挪到 gate-host）：GATE_HOST=gate-host（缺省 local = 全在本机，同旧行为）。
#   每步缺省整步在 gate-host 跑；gate-host 做不了的写在该步自己的文件头「# 门禁本机：…」（tools/gate_decl.py local 收；实测红过的项，不猜）：键 * = 整步本机跑，其余键 = 该步在 gate-host 跑时给钩子 GATE_SKIP=<键>、
#   本机另跑同一命令带 GATE_ONLY=<键>（钩子自己认键，认不得报红）。gate-host 那份一次 维护方工具 跑完（--rev 待测提交、--sync @gate、结果目录拉回），与本机那份同时跑；
#   合并后每步一行，标「［gate-host］」「［本机］」，拆两边的步两份都绿才绿、只算一步，「跑了 N 步 / 应跑 N 步」的 N 与本机全跑相同。
#   待测提交 GATE_REV（缺省 HEAD；tools/ship.sh 给暂存区的临时提交）；工作树的跟踪文件与它不同、没有 维护方工具、gate-host 那份没出完整结果
#   （连不上 / 工具链不同 / 中途断 / 排队超时 / 盘不足）→ 原派 gate-host 的步退回本机补跑（10-29 起；GATE_FALLBACK_LOCAL=0 = 旧行为：记没跑、「环境未就绪」退 75），不换 Spare-host。gate-host 忙不退回（10-22 维护者：退回本机高负载下偶发红、连环挡人）：
#   在 维护方工具 的满核槽队列里排（优先级按 GATE_ROLE：热修 0、列车 1、其余 2，先比优先级再比先后；1 号槽只给热修 / 列车；等待上限按队列长度算），排队超时 → gate-host 的步判红、写明排第几，不在本机重跑。远端工作树名 GATE_REMOTE_NAME（缺省本树目录名；列车给 train，增量编命中）。GATE_CLEAN=1：远端工作树连被忽略的构建产物一起清掉再跑（列车判假红的干净重跑；本机那份由调用方自己给干净树）。
# 分层（10-30 维护者：合入前门禁只证明「这次改动没弄坏它影响到的东西」，目标中位 ≤5 分钟）：钩子 / tools/gate_ios.sh 文件头「# 门禁层：合入后」（tools/gate_decl.py layer 收；不写 = 合入前）。
#   GATE_LAYER=pre（列车合入前）= 去掉声明合入后的步；post = 只跑声明合入后的步；不设 = 全跑（自检 11-05 起缺省 pre、显式空值才全跑；老单测同旧行为）。GATE_LIST=1 清单同样按层过滤；
#   「# 门禁层：合入后（路由命中时合入前也跑）」= 合入后的步，但合入前层里按改动路由选中它时也跑（11-10：安卓支合入前不跑 android/gate.sh、ios 头改动不跑 iOS 编译，一天叠多处 main 红）
#   被分到另一层没跑的步在结论前列一行「另一层：<步…>」，不算红、不算没跑。
# 单测结果缓存（只缓存「单测 <名>」步，钩子不缓存）：跑前 tools/test_cache.py key 算键（输入 = 路由覆盖面 ∪ 单测头「# 门禁输入：」+ 工具链指纹），
#   命中 → 不跑、记绿「（缓存命中）」；没命中照跑，绿了 put、红了不记。缓存目录 ${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/cache/testres；
#   本机那份与 gate-host 那份（GATE_PLAN）各用各机器上的缓存。GATE_TEST_CACHE=0 关；算键失败 / 读不到一律当没命中照跑（宁可多跑）
set -u
R=$(git rev-parse --show-toplevel) || exit 1; cd "$R"
# 环境下限（bash 5 / Python = PYTHON_MINIMUM，tools/env_floor.sh）：错环境跑出的红是假红，不够就不跑
#   按本脚本所在仓库找；单测把 gate.sh 单独拷进假仓库跑（没有 env_floor.sh）时不核——外层门禁已核过同一环境；真仓库里一定在（tests/env_floor_test.sh 核）
_ef=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [ -f "$_ef/tools/env_floor.sh" ]; then . "$_ef/tools/env_floor.sh"; s2a_env_floor "$_ef" python3 ${S2A_PY:+"$S2A_PY"} || exit 2; fi; unset _ef
# 私有工具自找放最前（11-24：原先在后面「门禁机配置自找」里才从 machine.local 补 S2A_PRIVATE_TOOLS，而拼「门禁显卡：独占 / 共享」
#   单测命令时就要用它决定套不套显卡锁——各线自检派 gate-host 不带这个变量，present_settings_test 一律裸跑、不拿锁，和开窗口的组同时跑挂起在第 0 帧）
MCK=$(git rev-parse --path-format=absolute --git-common-dir 2> /dev/null); MCK=${MCK%/.git}
if [ -z "${S2A_PRIVATE_TOOLS:-}" ]; then
  for mf in ${S2A_MACHINE_LOCAL:+"$S2A_MACHINE_LOCAL"} "$R/tools/machine.local" "${MCK:+$MCK/tools/machine.local}"; do
    [ -f "$mf" ] || continue; pt=$(S2A_PRIVATE_TOOLS= bash -c '. "$1" > /dev/null 2>&1; printf %s "${S2A_PRIVATE_TOOLS:-}"' _ "$mf"); [ -n "$pt" ] && { export S2A_PRIVATE_TOOLS=$pt; break; }; done
fi
export S2A_ROMFS_SEAL=1   # 门禁出包把 RomFS 共享存储设只读：阶段误写共享份当场 EACCES（玩家机不设，pack/roots.sh 文件头）
# 停门禁只停本树（11-09：一条线 pkill -f "tools/gate.sh" 停自己的预跑，连别的会话的门禁和列车本机步骤一起杀了）：
#   本份门禁的 pid 写本工作树自己的 git 目录（git rev-parse --git-dir，每棵工作树一份、不算改动不进提交），退出删掉；
#   要停用 tools/gate_stop.sh <工作树>（核对 pid 确是这棵树的门禁、只收它的后代）。不许按进程名 pkill / killall 门禁（规矩：关进程按完整路径，别按进程名）
GPF=$(git rev-parse --absolute-git-dir 2> /dev/null)/s2a_gate.pid; echo $$ > "$GPF" 2> /dev/null || GPF=/dev/null
# 统一工具链（tools/toolchain.env）：各步编译 / 单测都用这一套。只有带这份文件的仓库才引入（列车 / 门禁单测的假仓库只拷了 gate.sh，没有工具链，也不编东西）
# 多宿主（10-30 维护者 / 维护者：自检要证明的是「这台宿主能证明的那部分」，剩下的交列车在 gate-host 上证）：
#   没有苹果工具链的宿主（Linux、WSL 等非 macOS）照样跑门禁——平台无关的步和本宿主平台的钩子（android、launchers/linux …）本机跑、照判红绿；
#   需要苹果工具链的步（tools/contrib_tools.tsv 里依赖 Xcode 的）不跑、不算红，逐条列为「交 Mac 门禁」，结论分「本机已证 / 待 Mac 证」两段，退出码只看本机跑的步。
#   仍加载工具链的公共 PATH（timeout 垫片等），容忍 Apple 探测失败。远端计划（GATE_PLAN）与派 gate-host（GATE_HOST=gate-host）只在 macOS 上成立，照旧要工具链。
LINUX_LOCAL=
if [ "$(uname -s)" != Darwin ] && [ -f "$R/tools/toolchain.env" ] && [ -z "${GATE_PLAN:-}" ] && [ "${GATE_HOST:-local}" = local ]; then LINUX_LOCAL=1; fi
# Apple 能力只有 contrib_tools.tsv 一份来源，按表中第一条命中分类。
apple_step() {
  local pat deps
  [ -f tools/contrib_tools.tsv ] || return 1
  while IFS=$'\t' read -r pat deps; do
    case $pat in \#*|'') continue;; esac
    case $1 in $pat) case ,$deps, in *,Xcode,*) return 0;; *) return 1;; esac;; esac
  done < tools/contrib_tools.tsv
  return 1
}
# 固定 Python 解释器（11-07 维护者：android_widen_disabled_test 用 ${S2A_PY:-python3}，gate-host 门禁没设 S2A_PY 就落到 homebrew 裸 python3，缺 lz4 红；
#   固定解释器 ~/.s2arm/venv 里按 tools/rewriter-requirements.txt 钉了版本、doctor.sh 预检核它）：没给 S2A_PY 且固定解释器在 → 各步统一用它
[ -z "${S2A_PY:-}" ] && [ -x "$HOME/.s2arm/venv/bin/python3" ] && export S2A_PY=$HOME/.s2arm/venv/bin/python3
# 单跑钩子的一部分（11-14 新门禁按组拆步）：GATE_HOOK_ONLY / GATE_HOOK_SKIP=<子项键…> → 本机整步跑的钩子带 GATE_ONLY / GATE_SKIP（钩子自己认键，认不得报红）；
#   GATE_HOOK_ONLY 也原样转给钩子：认它的钩子（graphics/gate.sh 的子项键与它自己的 GATE_ONLY 本机键是两套）按它挑，其余钩子只看 GATE_ONLY；
#   下文统一 unset GATE_ONLY / GATE_SKIP（远端计划用），这里先记下
HONLY=${GATE_HOOK_ONLY:-}; HSKIP=${GATE_HOOK_SKIP:-}; unset GATE_HOOK_ONLY GATE_HOOK_SKIP
TIER=${GATE_TIER:-full}; PLANM=${GATE_PLAN:-}; RER=${GATE_RERUN:-}; NOLQ=${GATE_NO_LOADQ:-}   # 列车起的门禁（下文 unset 前先记下：本机预跑令牌豁免它）
# 预跑必须按路由（11-11 维护者：各线预跑不给改动清单 → 表外代码路径按全跑算，整份 482 步、墙钟中位 1535 秒，挤占门禁机上列车的槽）：
#   合入前层（GATE_LAYER=pre）、角色不是列车 / 热修、没给 GATE_CHANGED、不是远端那一份（GATE_PLAN，它随 --env 收本机算好的清单）、
#   不是点名重跑（GATE_RERUN：点到的步按全量清单找，不能被路由滤成 0 步绿）时，
#   改动清单 = merge-base(origin/main, 待测提交) 到待测提交的改动，与列车按支算的同一口径；export 让派 gate-host 那份按同一清单路由。
#   待测提交已在 origin/main 上（merge-base = 它自己，如守望在 main 上的无缓存实跑）改动为空、照旧全跑；算不出 merge-base（假仓库没有 origin/main）同样照旧
if [ "${GATE_LAYER:-}" = pre ] && [ -z "${GATE_CHANGED:-}" ] && [ -z "$PLANM" ] && [ -z "$RER" ]; then
  case ${GATE_ROLE:-} in 列车|热修) ;; *)
    _mb=$(git merge-base origin/main "${GATE_REV:-HEAD}" 2> /dev/null) \
      && _chg=$(git -c core.quotepath=off diff --name-only "$_mb" "${GATE_REV:-HEAD}" 2> /dev/null) && _chg=${_chg//$'\n'/ } && [ -n "${_chg// /}" ] \
      && { export GATE_CHANGED=$_chg; echo "预跑按路由：改动清单取 merge-base ${_mb:0:9} → ${GATE_REV:-HEAD}（$(wc -w <<< "$_chg" | tr -d ' ') 个文件）" >&2; }
    unset _mb _chg;; esac
fi
# 与列车/自检共用 gate_tier。普通文档才降 doc；敏感文档不降档。
# 改动集为空不改档；只允许降为 doc，保留调用者显式给的其他档位。
CHG=${GATE_CHANGED:-$(git -c core.quotepath=off diff --name-only "origin/main...${GATE_REV:-HEAD}" 2> /dev/null)}
if [ -n "$CHG" ]; then
  source "$R/tools/gate_plan.sh" || exit 2
  [ "$(gate_tier "$(tr ' ' '\n' < <(printf '%s\n' "$CHG"))")" != doc ] || TIER=doc
fi
# 远端那一份（GATE_PLAN 非空）把每步结果写进仓库里的 GATE_OUT（build/ 下、被忽略），本机经 维护方工具 --pull 拉回；本机那份用临时目录
if [ -n "$PLANM" ]; then L=$R/${GATE_OUT:?远端计划要给 GATE_OUT}; rm -rf "$R/build/gate_out"; mkdir -p "$L"; trap '[ "$(cat "$GPF" 2> /dev/null)" = $$ ] && rm -f "$GPF"' EXIT
else L=$(mktemp -d); trap 'rm -rf "$L"; [ "$(cat "$GPF" 2> /dev/null)" = $$ ] && rm -f "$GPF"' EXIT; fi
# 同一棵树的 C# 编译串行（tools/dotnet_lock.c，11-10 判据去负载化：并行各步编同一批 compat/tree 工程共用 obj/ 互踩，MSB4018 / nupkg 被占 / CS0006）：
#   开头现编到 $L/bin/dotnet，各步 DOTNET 与 PATH 都指它（各步大多按 ${DOTNET:-~/.dotnet/dotnet} 调，少数直接调 dotnet）；真 dotnet 记在 S2A_DOTNET_REAL。
#   用编出来的程序而不是脚本：脚本解释器受 SIP 保护会清掉 DYLD_*（t_native_provider 靠 DYLD_INSERT_LIBRARIES 注入，11-11 经脚本包装后服务层红）。
#   编不出（没有 cc）= 不接锁照跑；嵌套门禁（S2A_DOTNET_LOCK 已给）不套两层
if [ -f tools/dotnet_lock.c ] && [ -z "${S2A_DOTNET_LOCK:-}" ] && mkdir -p "$L/bin" && cc -O2 -o "$L/bin/dotnet" tools/dotnet_lock.c 2> /dev/null; then
  export S2A_DOTNET_REAL=${DOTNET:-$HOME/.dotnet/dotnet} S2A_DOTNET_LOCK=$R/build/.dotnet_build.lock DOTNET=$L/bin/dotnet PATH=$L/bin:$PATH
  mkdir -p "$R/build"
fi
# C / ObjC / C++ 按内容编译缓存（tools/cc_cache.sh，10-30 列车线）：本机那份与 gate-host 那份各自打开，每次编译的命中情况记 $L/ccache.log（gate-host 那份随结果目录拉回）
[ -f tools/cc_cache.sh ] && { source tools/cc_cache.sh; S2A_CCACHE_LOG=$L/ccache.log s2a_ccache_on "$R"; }
# doc 仍须扫描；只有非 doc 档需要统一 Apple 编译工具链（#665）。
case $TIER in doc|tool|full) ;; *) echo "🔴 GATE_TIER 只认 doc / tool / full"; exit 1;; esac
# 本机步的环境与 gate-host 那份一样封闭（11-05 不稳定清单第 10 项）：列车 / 快档的控制变量（TRAIN_* / S2A_TRAIN_*，如快档 TRAIN_RN=train-fast）
#   不带进各步。gate-host 那份经 维护方工具 只带 --env 白名单本来就没有；本机补跑却继承了，gate_host_test ⑦ 里的 train.sh 因此把远端树取成 train-fast、判据找 train 而红。
#   本脚本自己不读这两类变量
for _v in $(compgen -e | grep -E '^(TRAIN_|S2A_TRAIN_)'); do unset "$_v"; done; unset _v
LAYER=${GATE_LAYER:-}; case $LAYER in ''|pre|post) ;; *) echo "🔴 GATE_LAYER 只认 pre / post（不设 = 全跑）"; exit 1;; esac
GLY=; if [ -n "$LAYER" ] && [ -f tools/gate_decl.py ]; then GLY=$(python3 tools/gate_decl.py layer) || { echo "🔴 门禁层声明收不齐（tools/gate_decl.py layer 失败）"; exit 1; }; fi
TCON=${GATE_TEST_CACHE:-1}; TCD=${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/cache/testres
# 门禁台账（11-10 降门禁压力：先量后改）：每步一行 JSON 追加进 GL，记步名、缓存键、在哪台跑、退出码、秒数、排队等令牌秒数、谁起的，不改任何行为。
#   只记真仓库的本机那份：远端那份（GATE_PLAN）的步由本机这份按「在哪台」记，不重复；门禁步里嵌套的门禁、临时目录里的假仓库（单测）不记。
#   GATE_LEDGER=<文件> 强制记到该文件，GATE_LEDGER=- 关掉；调用方给 GATE_ROLE（热修 / 列车 / 预跑 / 手工），不给 = 预跑（只认显式：GATE_NO_LOADQ 有人手跑预跑时也设，不能据它推断列车）
GL=${GATE_LEDGER:-${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/gate_ledger.jsonl}
if [ "$GL" = - ]; then GL=
elif [ -z "${GATE_LEDGER:-}" ]; then case $R in "${TMPDIR:-/tmp}"*|/tmp/*|/private/tmp/*|/var/folders/*|/private/var/folders/*) GL=;; esac; { [ -n "$PLANM" ] || [ -n "${GATE_IN_STEP:-}" ]; } && GL=; fi
GLROLE=${GATE_ROLE:-预跑}; GLREV=$(git rev-parse --short=12 "${GATE_REV:-HEAD}" 2> /dev/null); GLRUN=$(date +%s)-$$
case $GLROLE in 热修) TPRIO=0;; 列车) TPRIO=1;; 手工) TPRIO=3;; *) TPRIO=2;; esac   # 排步令牌的级（tok_get）：热修 > 列车 > 预跑 > 手工；只认显式 GATE_ROLE
GLT0=$(date +%s)
gl_end() {   # gl_end <结论>：台账记一行整份汇总（kind=run：起止、墙钟、结论、跑了几步；日报算门禁中位 / p90 墙钟用）
  [ -n "${GL:-}" ] || return 0
  printf '{"kind":"run","t":%s,"run":"%s","start":%s,"wall":%s,"verdict":"%s","steps":%s,"ran":%s,"role":"%s","tier":"%s","layer":"%s","rev":"%s"}\n' \
    "$(date +%s)" "${GLRUN:-}" "$GLT0" "$(( $(date +%s) - GLT0 ))" "$1" "${N:-0}" "${ran:-0}" "${GLROLE:-}" "${TIER:-}" "${LAYER:-}" "${GLREV:-}" >> "$GL" 2> /dev/null; return 0; }
OTHER=()   # 分到另一层、本次不跑的步
inlayer() {   # inlayer <步名>：本次 GATE_LAYER 要不要这一步
  [ -n "$LAYER" ] || return 0
  local post=pre pf=; LC_ALL=C awk -F'	' -v n="$1" '$1 == n && $2 == "合入后" { f = 1 } END { exit !f }' < <(printf '%s\n' "$GLY") && post=post
  [ "$post" = "$LAYER" ] && return 0
  # 「合入后（路由命中时合入前也跑）」：合入前层里只要按改动路由（RT=1）选中了它就跑——钩子 / iOS 编译只在路由命中时才 add 进来，
  #   路由表就是各自文件头「# 门禁路由」与 gate_route.py，不另列前缀（11-10：安卓支、ios 头改动合入前不跑对应钩子，一天叠多处 main 红）
  [ "$LAYER" = pre ] && [ "$post" = post ] && [ "${RT:-0}" = 1 ] || return 1
  pf=$(LC_ALL=C awk -F'	' -v n="$1" '$1 == n && $2 == "合入后" { print $3 }' < <(printf '%s\n' "$GLY")); [ "$pf" = 路由 ]; }
if [ "$TIER" != doc ] && [ -f "$R/tools/toolchain.env" ]; then
  if [ -n "$LINUX_LOCAL" ]; then
    # source 必须在当前 shell，保留 PATH；stderr 原文逐行加前缀，不伪装成步骤红。
    tc_err=$(mktemp) || exit 2
    source "$R/tools/toolchain.sh" 2> "$tc_err" || :
    tc_note=$(cat "$tc_err"; printf .); tc_note=${tc_note%.}; rm -f "$tc_err"
    if [ -n "$tc_note" ]; then
      while IFS= read -r tc_line || [ -n "$tc_line" ]; do printf '工具链提示：%s\n' "$tc_line" >&2; done < <(printf '%s' "$tc_note")
    fi
    unset tc_err tc_note tc_line
  else source "$R/tools/toolchain.sh" || exit 2; fi
fi
# 本机步 / 子项的声明（10-30 维护者批：原 tools/gate_local.tsv 中央表拆回各自文件头「# 门禁本机：…」，tools/gate_decl.py local 收成同格式：步<TAB>键<TAB>原因）
# 点名快路（11-16 门禁刀五：gate2 每一步都经整份 gate.sh，按改动路由规划——route why / tests / ios / hooks 各起一个 Python、本机声明收集再起一个，
#   负载下单步多出约 2–3 秒，只为最后被 GATE_RERUN 筛剩点名的那几步）：GATE_FAST=1、点名重跑（GATE_RERUN）的步全是「单测 <名>」/ 语法 / 公开仓扫描 /
#   Ryujinx 补丁核对、本机跑（不派 gate-host、不是远端计划）、不分层、不是清单 / 预算查询时，不做路由规划与本机声明收集，单测清单直接取点名的 tests/<名>.sh；
#   其余（环境、工具链、令牌、无进展看门狗、超时、日志、「跑了 N 步 / 应跑 N 步」结论行）与现路同一段代码。点名里有钩子 / iOS 编译 / 别的名字 → 照现路全量规划
FAST=; FTESTS=
if [ "${GATE_FAST:-}" = 1 ] && [ -n "$RER" ] && [ -z "$PLANM" ] && [ "${GATE_HOST:-local}" = local ] && [ -z "$LAYER" ] && [ -z "${GATE_LIST:-}" ] && [ -z "${GATE_BUDGET_Q:-}" ]; then
  FAST=1; IFS=$'\t' read -ra _fn <<< "$RER"
  for _n in ${_fn[@]+"${_fn[@]}"}; do case $_n in "单测 "*) FTESTS="$FTESTS tests/${_n#单测 }.sh";; 语法|公开仓扫描|"Ryujinx 补丁核对") ;; *) FAST=; break;; esac; done
  [ -n "$FAST" ] || FTESTS=; unset _fn _n
fi
GLT=; [ -z "$FAST" ] && [ -f tools/gate_decl.py ] && GLT=$(python3 tools/gate_decl.py local 2> /dev/null)
STEPS=()   # 名字<TAB>命令
add() { inlayer "$1" || { OTHER+=("$1"); return 0; }; STEPS+=("$1	$2	${3:-${GATE_HOOK_S:-600}}"); }   # 第 3 列 = 本步墙钟上限（缺省 GATE_HOOK_S）；另一层的步只记名
# 老的独立假仓库没有扫描器；真实仓库若删掉扫描器，基线仍有它，执行时必须报红。
if [ -f tools/public_repo_scan.py ] || git cat-file -e "${GATE_SCAN_BASE:-origin/main}:tools/public_repo_scan.py" 2> /dev/null; then
  printf -v SCAN_CMD 'python3 tools/public_repo_scan.py --repo %q --ref %q --path . --base %q --format gate --report build/public_repo_scan.json' "$R" "${GATE_REV:-HEAD}" "${GATE_SCAN_BASE:-origin/main}"
  add "公开仓扫描" "$SCAN_CMD"
fi
if [ "$TIER" != doc ]; then
# 本线自查
# shell 语法用 macOS 自带 /bin/bash 3.2 查全仓库 .sh（10-23 内测朋友：干净 Mac 上 tools/dev_run.sh、tests/gc_test.sh 在 3.2 下语法红——$( ) 里的 case 分支 3.2 解析不了，
#   我们机器 PATH 上是 Homebrew bash 5 一直绿）；新旧都过才算过，没有 /bin/bash 的机器退回当前 bash
add 语法 'bad=0; SB=/bin/bash; [ -x "$SB" ] || SB=bash; while IFS= read -r f; do [ -f "$f" ] || continue; "$SB" -n "$f" 2> /dev/null && bash -n "$f" || { echo "语法错 ${f}（$("$SB" -n "$f" 2>&1 | head -1)）"; bad=1; }; done < <(git -c core.quotepath=off ls-files "*.sh" | grep -v "^compat/tree/"); for f in tools/*.py tests/*.py pack/*.py; do [ -f "$f" ] || continue; python3 -m py_compile "$f" || bad=1; done; n=$(ls tools/*.sh tests/*.sh pack/*.sh tools/*.py tests/*.py pack/*.py 2>/dev/null | wc -l | tr -d " "); echo "全部脚本可解析 → $([ $bad = 0 ] && echo 是 || echo 否)（$n 个）"; exit $bad'
CH=(${GATE_CHANGED:-}); RT=0; [ -f tools/gate_route.py ] && [ ${#CH[@]} -gt 0 ] && [ -z "$FAST" ] && RT=1   # 列车单测的假仓库没有路由器：全跑；快路不路由（见上 FAST）
route() { GATE_TIER=$TIER python3 tools/gate_route.py "$1" ${CH[@]+"${CH[@]}"}; }
[ $RT = 1 ] && echo "路由：$(route why)"
if [ -n "$FAST" ]; then TESTS=$FTESTS; echo "快路：只排点名的步，不做路由规划"
elif [ $RT = 1 ]; then TESTS=$(route tests) || { echo "🔴 路由器出错"; exit 1; }; else TESTS=$(ls tests/*_test.sh 2> /dev/null); fi
# 单测自报上限（10-30 列车线：train_test 本机 562 秒、gate-host 负载下超缺省 600 秒判红）：文件头 5 行内「# 门禁预算：<秒>」→ 本步墙钟上限用它、并计入钩子预算最大值（列车据此放宽整份门禁墙钟）
TFL=   # 单测步名<TAB>单测文件（缓存算键用）
for tf in $TESTS; do [ -f "$tf" ] || continue; inlayer "单测 $(basename "$tf" .sh)" || { OTHER+=("单测 $(basename "$tf" .sh)"); continue; }
  TFL="${TFL}单测 $(basename "$tf" .sh)	$tf"$'\n'; tb=$(head -5 "$tf" | sed -n 's/^# 门禁预算：\([0-9][0-9]*\)$/\1/p' | head -1)
  [ -n "$tb" ] && [ "$tb" -gt "${BMAX:-0}" ] && BMAX=$tb
  # 开真窗口 / 真上屏的单测（文件头 6 行内「# 门禁显卡：独占」）同 gfx / visual 钩子走显卡独占锁（维护方工具 prio）：
  #   11-06 present_settings_test 和同批 gfx 钩子并行，窗口被抢焦点、宿主失焦冻结客机，矩阵 14/18 红，重跑 18/18 绿（冤退两支）
  if head -6 "$tf" | LC_ALL=C grep -q '^# 门禁显卡：独占'; then hl=${tb:-${GATE_HOOK_S:-600}}; pl=$(( hl + ${S2A_GATE_GPU_QUEUE_S:-1800} + 30 )); [ "$pl" -gt "${BMAX:-0}" ] && BMAX=$pl
    add "单测 $(basename "$tf" .sh)" "${S2A_PRIVATE_TOOLS:+bash $S2A_PRIVATE_TOOLS/mac_slot_frozen.sh prio '单测 $(basename "$tf" .sh)' $hl -- }bash $tf" $pl
  # 离屏起 Metal、不按显示时钟判的单测（文件头 6 行内「# 门禁显卡：共享」）取共享显卡锁（11-20 读写锁）：可多个同跑，独占步在等或在跑时排队；外层上限留排队余量
  elif head -6 "$tf" | LC_ALL=C grep -q '^# 门禁显卡：共享'; then pl=$(( ${tb:-${GATE_HOOK_S:-600}} + ${S2A_GATE_GPU_QUEUE_S:-1800} + 30 ))
    add "单测 $(basename "$tf" .sh)" "${S2A_PRIVATE_TOOLS:+bash $S2A_PRIVATE_TOOLS/mac_slot_frozen.sh share '单测 $(basename "$tf" .sh)' -- }bash $tf" $pl
  else add "单测 $(basename "$tf" .sh)" "bash $tf" ${tb:-}; fi; done
# 各线钩子
HOOKS=
if [ "$TIER" = tool ] && [ $RT = 1 ]; then HOOKS=$(route claimed) || { echo "🔴 路由器出错"; exit 1; }; fi
if [ "$TIER" = full ]; then
  # Ryujinx 上游 + 补丁 = 树内源码（服务接入线 compat/verify.sh；方案铁律 3、5b-6）：退出 0 一致 / 1 不一致 / 2 环境错
  [ -f compat/verify.sh ] && add "Ryujinx 补丁核对" 'bash compat/verify.sh --rev HEAD'
  # iOS 全量编译 + 链接（10-20 维护者：坐标探测的 mach_vm.h 弄坏过 main 的 iOS 出包，门禁只编 Mac 没拦住）；缓存构建目录，热编约 5 秒
  # 源码同步到私有镜像再编（gate_ios.sh），不和钩子的 dotnet 抢 obj / 锁，和钩子并行跑；只在进 App 的源码有改动时跑
  [ -f runtime/build.mk ] && [ -f tools/gate_ios.sh ] && { [ $RT = 0 ] || [ "$(route ios)" = yes ]; } && add "iOS 全量编译" 'bash tools/gate_ios.sh' "${GATE_IOS_S:-3600}"   # 有 iOS 构建入口的仓库才跑（列车单测的假仓库没有）
  if [ -n "$FAST" ]; then HOOKS=; elif [ $RT = 1 ]; then HOOKS=$(route hooks); else HOOKS=$(find . -mindepth 2 -maxdepth 4 -name gate.sh -not -path './tools/*' -not -path './.git/*' | sed 's#^\./##' | LC_ALL=C sort); fi
fi
  # 钩子自报预算（10-21：宽屏门禁离线着色器库缓存键一变整份重建 912 秒，超固定的 600 秒）：钩子文件里有「# 门禁预算：--budget」这一行才问
  #   （不认 --budget 的钩子会把它当普通参数整份跑完），bash <钩子> --budget 打一个整数秒 → 本步墙钟上限；问不出来用缺省 GATE_HOOK_S
  # 吃显卡的钩子（渲染器单测 / 画幅回放对拍）经 Mac 局槽优先跑：各线 Mac 局让门禁（维护方工具 prio）
  for hk in $HOOKS; do bl=; inlayer "钩子 $hk" || { OTHER+=("钩子 $hk"); continue; }
    if { [ -z "$LINUX_LOCAL" ] || ! apple_step "钩子 $hk"; } && LC_ALL=C grep -q '^# 门禁预算：--budget' "$hk" 2> /dev/null; then bl=$(timeout 60 bash "$hk" --budget 2> /dev/null | tail -1); case $bl in ''|*[!0-9]*) bl=;; esac; fi
    [ -n "$bl" ] && [ "$bl" -gt "${BMAX:-0}" ] && BMAX=$bl
    # 优先步骤本机独占跑（10-31 维护者：显卡步骤不跟本机重活抢，删掉「等 300 秒到点照跑」）：排队（独占锁 + 等 Mac 局让出，上限 S2A_GATE_GPU_QUEUE_S，缺省 1800 秒）
    #   不占钩子自己的上限——钩子上限交给 维护方工具 在拿齐后才计时；本步外层上限 = 钩子上限 + 排队上限 + 30 秒收尾
    case $hk in graphics/gate.sh|tests/visual/gate.sh) hl=${bl:-${GATE_HOOK_S:-600}}; pl=$(( hl + ${S2A_GATE_GPU_QUEUE_S:-1800} + 30 )); [ "$pl" -gt "${BMAX:-0}" ] && BMAX=$pl
      # visual 钩子全是离屏真显卡回放对拍：整钩子取共享显卡锁（11-20 读写锁）；graphics 钩子在自己的子项上分别取共享 / 独占
      [ "$hk" = tests/visual/gate.sh ] && add "钩子 $hk" "${S2A_PRIVATE_TOOLS:+bash $S2A_PRIVATE_TOOLS/mac_slot_frozen.sh share '钩子 $hk' -- }bash $hk" $pl || add "钩子 $hk" "bash $hk" $pl;;
    # 启动器钩子的 XCTest 开真窗口、要前台焦点：同「门禁显卡：独占」单测走显卡独占锁（维护方工具 prio = 全机共用的 Mac 局槽，与 Mac 游戏局互斥）。
    #   11-25：本机门禁启动器 UI 用例与同机 Mac 游戏局（拿 Mac 局槽、窗口到前台）重叠时失焦、等前台卡死，看门狗 300 秒无进展停掉，换空闲主机复跑绿
    launchers/macos/gate.sh) hl=${bl:-${GATE_HOOK_S:-600}}; pl=$(( hl + ${S2A_GATE_GPU_QUEUE_S:-1800} + 30 )); [ "$pl" -gt "${BMAX:-0}" ] && BMAX=$pl
      add "钩子 $hk" "${S2A_PRIVATE_TOOLS:+bash $S2A_PRIVATE_TOOLS/mac_slot_frozen.sh prio '钩子 $hk' $hl -- }bash $hk" $pl;;
    *) add "钩子 $hk" "bash $hk" $bl;; esac; done   # 11-14：不再整钩子套显卡锁——graphics 钩子只在开窗口 / 按显示时钟判的子项上自己拿（graphics/gate.sh gpu_excl），visual 钩子全是离屏对拍、不拿；外层上限仍留排队余量
fi   # 非 doc 档的语法 / 单测 / 平台钩子
# 预算查询（列车起门禁前问）：GATE_BUDGET_Q=1 bash tools/gate.sh → 打本次要跑的钩子里自报的最大预算（秒）后退出、什么都不跑
#   只算钩子自报的预算（没人自报 = 0，列车照缺省上限）；iOS 编译那一步的 3600 秒含排队、不算
if [ "${GATE_BUDGET_Q:-}" = 1 ]; then echo "${BMAX:-0}"; exit 0; fi
# 步骤清单（贡献者自检 tools/contrib_check.sh 用）：GATE_LIST=1 → 按同一路由打本次要跑的步骤名、一行一个后退出，什么都不跑
if [ "${GATE_LIST:-}" = 1 ]; then
  for s in "${STEPS[@]}"; do
    name=${s%%	*}
    if [ -n "$LINUX_LOCAL" ] && apple_step "$name"; then printf '%s\t需苹果工具链\n' "$name"; else echo "$name"; fi
    # stdout 保留一行一步的机器合同；本机登记沿用路由诊断前缀写 stderr。
    if LC_ALL=C awk -F'	' -v n="$name" '$1 == n && $2 == "*" { found=1 } END { exit !found }' < <(printf '%s\n' "$GLT"); then
      echo "路由：${name}［本机］" >&2
    fi
  done
  exit 0
fi
# 无苹果工具链的宿主点名跑的步（GATE_RERUN）必须都在本次清单里：过滤不能把未知 / 未路由的请求变成 0 步绿。
# 清单 / 预算查询已经退出；doc 档仍按本次实际清单校验显式请求。点到的苹果步不在这里拦，下面统一列为「交 Mac 门禁」
if [ -n "$LINUX_LOCAL" ] && [ -n "${GATE_RERUN+x}" ]; then
  case $RER in *$'\n'*|*$'\r'*) echo '自检步骤请求只接受 tab 分隔，不能含换行（结论不完整）' >&2; exit 2;; esac
  IFS=$'\t' read -ra REQUESTED <<< "$RER"
  [ ${#REQUESTED[@]} -gt 0 ] || { echo '自检未选中任何步骤，结论不完整' >&2; exit 2; }
  for requested in "${REQUESTED[@]}"; do
    found=
    for s in "${STEPS[@]}"; do [ "${s%%	*}" = "$requested" ] && { found=1; break; }; done
    [ -n "$found" ] || { echo "自检请求的步骤未在本次清单中：${requested}（结论不完整）" >&2; exit 2; }
  done
fi
# ---- 每步在哪跑（GATE_HOST=gate-host）----
# 假红重跑（列车用）：GATE_RERUN=<上次门禁「红步」行的名字，tab 分隔> → 只跑这几步；部分跑，绿了也不写已验记录
if [ -n "$RER" ]; then
  NEW=(); for s in "${STEPS[@]}"; do case "	$RER	" in *"	${s%%	*}	"*) NEW+=("$s");; esac; done
  STEPS=(${NEW[@]+"${NEW[@]}"}); echo "只重跑上次红的 ${#STEPS[@]} 步"
fi
# 远端那一份（GATE_PLAN 非空，由本机这一份经 维护方工具 带过去）：只跑计划里的步、按计划给钩子 GATE_SKIP；计划里有、远端算不出的步照样记一步红（两边路由不一致不许静默少跑）
if [ -n "$PLANM" ]; then
  NEW=()
  while IFS='	' read -r pn pk; do
    [ -n "$pn" ] || continue; f=
    for s in "${STEPS[@]}"; do [ "${s%%	*}" = "$pn" ] || continue; r1=${s#*	}; c=${r1%	*}; [ -n "$pk" ] && c="export GATE_SKIP='$pk'; $c"; NEW+=("$pn	$c	${s##*	}"); f=1; break; done
    [ -n "$f" ] || NEW+=("$pn	echo \"gate-host 上的门禁算不出这一步（本机计划里有：两边路由不一致）\"; exit 1	60")
  done < <(printf '%s\n' "$PLANM")
  STEPS=(${NEW[@]+"${NEW[@]}"})
fi
# 无苹果工具链的宿主：苹果步摘出来（不跑、不算红），只在结论里列「交 Mac 门禁」；其余照跑照判
MACS=()
if [ -n "$LINUX_LOCAL" ]; then
  NEW=(); for s in "${STEPS[@]}"; do if apple_step "${s%%	*}"; then MACS+=("${s%%	*}"); else NEW+=("$s"); fi; done
  STEPS=(${NEW[@]+"${NEW[@]}"})
  for m in ${MACS[@]+"${MACS[@]}"}; do echo "⏭ ${m}：本机无苹果工具链，交 Mac 门禁（列车在 gate-host 上证）"; done
fi
MACN=${#MACS[@]}; MACL=; [ "$MACN" -gt 0 ] && { MACL=$(printf '%s、' "${MACS[@]}"); MACL="；待 Mac 证 ${MACN} 步：${MACL%、}"; }
N=${#STEPS[@]}
# 0 步（GATE_RERUN 只跑的步本树都不涉及，如列车逐支核里这支路由不到红步）：直接绿退出——下面的 $(seq 1 $N) 在 macOS 上 N=0 时打出「1 0」，会越界取 STEPS
# 待 Mac 证的步另起一行机读（tab 分隔），总结行写成「本机绿、待 Mac 证」——不许让读末行的人 / 脚本把它当成整份门禁绿
macsum() { [ "$MACN" -gt 0 ] || return 0; printf '待Mac证'; printf '\t%s' "${MACS[@]}"; echo; }
[ "$N" = 0 ] && { gl_end 绿; [ ${#OTHER[@]} -gt 0 ] && echo "另一层：${OTHER[*]}"; macsum; if [ "$MACN" -gt 0 ]; then echo "门禁 本机绿、待 Mac 证：$TIER 档，本机已证 0 步（本机能跑的步本树都不涉及）${MACL}"; else echo "门禁 绿：$TIER 档，跑了 0 步 / 应跑 0 步（要跑的步本树都不涉及）"; fi; exit 0; }
# CPU 排队（10-24 维护者 重新分工：本机忙时新门禁先排队，资源让给双轨；10-29 维护者 批：判据从 15 分钟 load average 换成 CPU 实际空闲——
#   load average 把等硬盘的线程也算进去，出包读写小文件时虚高到 50–75、CPU 其实闲着）：CPU 实际空闲 < S2A_GATE_CPU_IDLE_MIN（缺省 15%；0 = 关）时每 30 秒看一次，
#   最多等 S2A_GATE_LOAD_WAIT_S（缺省 1800）秒，到点照跑并在结论里写明；双轨线的门禁设 GATE_DUAL=1 不排；远端那一份（GATE_PLAN）不排；
#   11-11 挪到算出步数之后（0 步不排：热修 post 层 0 步白等 601 秒）；GATE_ROLE=热修 不排（热修入队到开跑 < 1 分钟）
#   列车起的门禁（tools/train.sh grun 设 GATE_NO_LOADQ=1）不排：它有墙钟上限，排队会把合入通道拖成超时红
#   单测临时仓库里的假门禁（没有路由器 tools/gate_route.py）不排；专测排队的 gate_loadq_test 给 S2A_GATE_IDLE_TEST，照排。读不到空闲（没有 top）不排
if [ -z "${GATE_DUAL:-}" ] && [ -z "${GATE_NO_LOADQ:-}" ] && [ "${GLROLE:-}" != 热修 ] && [ -z "${GATE_PLAN:-}" ] && [ "${S2A_GATE_CPU_IDLE_MIN:-15}" != 0 ] \
   && { [ -f tools/gate_route.py ] || [ -n "${S2A_GATE_IDLE_TEST:-}" ]; }; then
  lw0=$SECONDS; lsaid=
  while :; do
    ci=${S2A_GATE_IDLE_TEST:-$(top -l 2 -n 0 -s 1 2> /dev/null | awk '/CPU usage/ { for (i = 1; i <= NF; i++) if ($i ~ /idle/) v = $(i - 1) } END { sub(/%/, "", v); if (v != "") printf "%d", v + 0.5 }')}
    [ -n "$ci" ] || break
    [ "$ci" -ge "${S2A_GATE_CPU_IDLE_MIN:-15}" ] && break
    [ $((SECONDS - lw0)) -ge "${S2A_GATE_LOAD_WAIT_S:-1800}" ] && { echo "【黄】本机 CPU 实际空闲 ${ci}% 仍低于 ${S2A_GATE_CPU_IDLE_MIN:-15}%，已排队 $((SECONDS - lw0)) 秒，到上限照跑"; break; }
    [ -z "$lsaid" ] && { echo "本机 CPU 实际空闲 ${ci}% < ${S2A_GATE_CPU_IDLE_MIN:-15}%：门禁先排队（双轨优先；最多 ${S2A_GATE_LOAD_WAIT_S:-1800} 秒）"; lsaid=1; }
    sleep "${S2A_GATE_LOAD_POLL_S:-30}"
  done
  [ -n "$lsaid" ] && echo "CPU 排队结束：等了 $((SECONDS - lw0)) 秒"
fi
# >>> step_par（单测 tests/gate_step_par_test.sh 按这两行标记抽出来测）
# 全机步数上限（11-04 合入运维线）：一份门禁原先把所有步同时起（job 全部 &），gate-host 上 3 份门禁叠起来 300+ 进程、负载 18–23，单测跑满 600 秒被杀成假红。
#   每步开跑前先拿一个全机令牌（内核文件锁，见下），令牌数 GATE_STEP_PAR 缺省 = 核数 × 2/3（至少 2）：
#   gate-host 12 核 24 GB → 8（有的步自己还并行编译，留余量；8 × 约 3 GB）。同一台机器上不管几份门禁、哪条线起的，同时在跑的步不超过它。超时从拿到令牌开跑才算（排队不算）；排队期间本份早停 → 不开跑、记被早停（143）。
#   嵌套：步里再起的门禁（单测夹具跑 gate.sh / train.sh）不再拿令牌——外层这步已占一个；不然外层 8 步占满令牌、各自等内层令牌 → 互等到 600 秒全被杀
#   （11-04 首次自检：gate_budget / detstop / layer / gate-host / test_cache 五步超时、train_test 红）。job 开跑前 export GATE_IN_STEP，tok_get 见它直接放行。
#   令牌 = 内核文件锁 $GATE_TOKEN_DIR/lk.<号>（tools/flock_hold.py take-any，与 plock / 维护方工具 同一份）：持有者（本步子 shell）一退出（含 kill -9、被总时限收掉）
#   守护进程随即退出、内核放锁。11-04 原先 mkdir t.<号> + 写 pid：mkdir 到写 pid 之间被杀 → 空目录永远收不回（gate-host 18:0x 8 个漏了 4 个）；
#   等待方按旧 pid 判死 rmdir 时别人刚 mkdir、还没写 pid → 同一令牌两边都用（gate.sh「t.3/pid: No such file」）。锁文件常驻不删。
#   排队（11-10 门禁减压 C）：原先每步先起子 shell 再各自排令牌，一份预跑展开上百个排队进程、每个每秒起一次解释器，停门禁时它们不随门禁退出（gate_stop 负载 80 下卡 4 分钟、
#   一棵树约 400 个排队进程只能 KILL）；令牌也不分先后，热修只绕过外层排队，内层照样卡。改为：门禁主进程按步序逐个排令牌（每份门禁只有它一个在排），
#   拿到才起这一步的子 shell；持锁者 = 门禁主进程（主进程没了内核放锁），步跑完由步自己放、门禁收尾再兜底放一遍（步被 kill -9 时不漏到门禁结束之后）。
#   先后：排队者登记 want.<级>.<开始排的秒>.<排队进程号>，有活着的、级更高（数小）或同级先来的排在前面就不去抢；级 0 热修 / 1 列车 / 2 预跑 / 3 手工（GATE_ROLE，不给 = 预跑，见 TPRIO）
tok_ahead() {   # tok_ahead <令牌目录> <级> <开始秒> <进程号>：有活着的、排在我前面的排队者 → 0；死标记顺手删
  local f b p2 t2 q2
  for f in "$1"/want.*; do [ -e "$f" ] || continue; b=${f##*/want.}; p2=${b%%.*}; b=${b#*.}; t2=${b%%.*}; q2=${b#*.}
    [ "$q2" = "$4" ] && continue
    kill -0 "$q2" 2> /dev/null || { rm -f "$f"; continue; }
    [ "$p2" -lt "$2" ] && return 0
    [ "$p2" = "$2" ] && { [ "$t2" -lt "$3" ] || { [ "$t2" = "$3" ] && [ "$q2" -lt "$4" ]; }; } && return 0
  done; return 1; }
tok_get() {   # tok_get <持有者 pid> [级]：拿到一个令牌，回显「守护进程 pid<TAB>锁文件」（在步里 → 回显空，不占）；持锁进程起不来返回 2；排队中本份早停（$L/stop）/ 持有者没了返回 3
  [ -n "${GATE_IN_STEP:-}" ] && return 0
  [ -f "${GATE_FLOCK_PY:-tools/flock_hold.py}" ] || return 0   # 单测夹具只拷了 gate.sh 的临时仓库：不参与全机令牌（真仓库里 gate.sh 与 flock_hold.py 同一提交带来，必在）
  local c n d=${GATE_TOKEN_DIR:-${TMPDIR:-/tmp}/s2a_gate_tokens} i ls= r rc w0=$SECONDS lw=$SECONDS pr=${2:-${TPRIO:-2}} t0 me=$BASHPID wf
  c=$(sysctl -n hw.ncpu 2> /dev/null || nproc 2> /dev/null || echo 6); n=${GATE_STEP_PAR:-$(( c * 2 / 3 > 2 ? c * 2 / 3 : 2 ))}
  mkdir -p "$d"; for i in $(seq 1 "$n"); do ls="$ls${ls:+:}$d/lk.$i"; done
  t0=$(date +%s); wf=$d/want.$pr.$t0.$me; : > "$wf"
  while :; do
    if [ -n "${L:-}" ] && [ -f "$L/stop" ]; then rm -f "$wf"; return 3; fi
    kill -0 "$1" 2> /dev/null || { rm -f "$wf"; return 3; }
    if tok_ahead "$d" "$pr" "$t0" "$me"; then rc=1; else r=$(python3 "${GATE_FLOCK_PY:-tools/flock_hold.py}" "$ls" "$1" take-any gate_tok 2> /dev/null); rc=$?; fi
    if [ $rc = 0 ]; then rm -f "$wf"; printf '%s\n' "$1" > "${r#*$'\t'}.holder" 2> /dev/null; echo "$r"; return 0; fi
    [ $rc = 1 ] || { rm -f "$wf"; echo "【红】门禁令牌：持锁小进程起不来（flock_hold.py 返回 ${rc}）" >&2; return 2; }
    [ $((SECONDS - lw)) -ge 60 ] && { lw=$SECONDS; echo "门禁令牌：本步已排 $((SECONDS - w0)) 秒，${n} 个全被占（${d}）" >&2; }
    declare -F tok_tick > /dev/null && tok_tick
    sleep "${GATE_TOK_POLL_S:-1}"; done; }
tok_put() { local p=${1%%$'\t'*}; [ -n "$p" ] || return 0; ps -o command= -p "$p" 2> /dev/null | grep -q gate_tok && kill -TERM "$p" 2> /dev/null; return 0; }   # 只结束自己起的持锁进程
# 步自己（跑步时）被信号杀也留退出码（11-05 不稳定清单第 1 项：oneclick_beat / public_repo_scan「没跑完（无退出码）」分不清被杀、挂死、没起来）。
#   KILL 拦不住：show 见「有 .started、无 .rc」记「被强杀」
_sigrc() { [ -f "$L/$1.rc" ] || { echo "$2 $((SECONDS - ${s0:-$SECONDS}))" > "$L/$1.rc"; echo "被信号 $3 停（步自己的进程被停，不是被测命令退出）" >> "$L/$1.log"; }; exit "$2"; }
LJ=(); LJN=(); TKS=(); job() {   # job <编号> <命令> <上限>：先排令牌（在调用者进程里排，排到才返回），再后台起这一步，写 $L/<编号>.log / .rc；.tp = 该步 timeout 进程号；.wait = 排令牌秒数（台账）
  local tk w0=$SECONDS rc=0 me=$BASHPID
  if [ -f "$L/stop" ]; then echo "143 0" > "$L/$1.rc"; return 0; fi
  # 排令牌放后台、主进程 wait：bash 3.2 在 $(…) 里阻塞时收到 TERM 不执行 trap（实测 10 秒停不下），wait 能被信号打断；TW 供 TERM 时一起停
  tok_get "$me" > "$L/$1.tk" & TW=$!; wait $TW || rc=$?; TW=; tk=$(cat "$L/$1.tk" 2> /dev/null); rm -f "$L/$1.tk"
  case $rc in 0) ;; 3) echo "143 0" > "$L/$1.rc"; return 0;;
    *) echo "75 0" > "$L/$1.rc"; echo "环境未就绪：拿不到门禁令牌（持锁进程起不来）" > "$L/$1.log"; return 0;; esac
  echo $((SECONDS - w0)) > "$L/$1.wait"; [ -n "$tk" ] && TKS+=("$tk")
  ( : > "$L/$1.started"; s0=$SECONDS
  export S2A_GATE_STEP=1   # 本进程树是某一步（不是门禁自己的规划 / 路由）：新门禁按它只采集步本身读的文件
  trap "_sigrc '$1' 143 TERM" TERM; trap "_sigrc '$1' 130 INT" INT; trap "_sigrc '$1' 129 HUP" HUP; trap 'tok_put "$tk"' EXIT
  # 末尾另起一行 exit $?：bash 5 对「-c 串的最后一条简单命令」直接 exec，组长会变成被测命令本身、命令行里没有 .pg 路径，
  #   收尸进程（tools/gate_reap.py）按这个字样经 safe_killpg 认组就认不出（11-11 门禁机上列车 run_token_test ④ 红）；最后一条是内建命令就不会 exec
  # 步时限按「无进展」判（11-10 判据去负载化：墙钟上限把满载下的正常慢判成超时——weak_link / 语法 / oneclick 一晚三个超时未判）：
  #   tools/nostall.py 连续 GATE_NOSTALL_S（缺省 300）秒日志不长、树内 CPU / 磁盘不涨、没有编译子进程才判挂死（红）；墙钟只留本步上限 × GATE_HARD_X（缺省 4）
  #   作保底（死循环一直吃 CPU 也算「有进展」），撞保底才记超时未判。GATE_NOSTALL_S=0 或缺 nostall.py（单测假仓库）= 旧行为：墙钟上限 = 本步上限
  if [ "${GATE_NOSTALL_S:-300}" != 0 ] && [ -f tools/nostall.py ]; then echo "${GATE_NOSTALL_S:-300} $(( $3 * ${GATE_HARD_X:-4} ))" > "$L/$1.ns"
    set -- "$1" "$2" "$3" "${S2A_PY:-python3}" tools/nostall.py "${GATE_NOSTALL_S:-300}" -- timeout "$(( $3 * ${GATE_HARD_X:-4} ))"
  else set -- "$1" "$2" "$3" timeout "$3"; fi
  export GATE_IN_STEP=1; "${@:4}" bash -c "echo \$\$ > '$L/$1.pg'; $2"$'\n''exit $?' > "$L/$1.log" 2>&1 & tp=$!; echo $tp > "$L/$1.tp"; wait $tp; rc=$?; rm -f "$L/$1.tp"; echo "$rc $((SECONDS - s0))" > "$L/$1.rc" ) & LJ+=($!); LJN+=("$1"); }   # LJ = 本机步的进程号（先等它们、还 CPU 账，再等 gate-host）
tok_putall() { local t; for t in ${TKS[@]+"${TKS[@]}"}; do tok_put "$t"; done; TKS=(); }   # 收尾兜底：步被 kill -9 没放的令牌（持锁者是门禁主进程，不收要等门禁退出）
# <<< step_par
# 门禁被停（列车收到 TERM 转来）时各步一起停：每步的 timeout 自成进程组、不随门禁退出，逐个 TERM 它（timeout 转给整组）；gate-host 那份停本机的 维护方工具
# *.tp 只记「还在跑的」步的进程号：步一结束就删（11-11 合入运维线：本机每秒新建约 1250 个进程、进程号约 80 秒转一圈，
#   原先跑完不删，门禁被 TERM / 收尸进程收尾时对早跑完那些步的进程号逐个发信号，号已分给别人——入队预检里 cores_lanes_test 被杀成 rc=143；tests/gate_stale_pid_test.sh）
trap 'for f in "$L"/*.tp; do [ -f "$f" ] && kill -TERM "$(cat "$f")" 2> /dev/null; done; [ -n "${TW:-}" ] && kill -TERM "$TW" 2> /dev/null; tok_putall; exit 143' TERM
HOST=${GATE_HOST:-local}; [ -n "$PLANM" ] && HOST=local; WHY=
# GATE_HOST=spare-host（10-25 列车提速：远端门禁只有 gate-host 一台，列车门禁 + 各线自检挤一个队）：与 gate-host 同一套远端流程，只是 维护方工具 带 --host spare-host、Spare-host 8 核只开 1 个槽；
#   下文 HOST=gate-host 表示「远端模式」，RHN 是实际机器名（日志与结论行用）
# 10-28 梳理裁定：Spare-host 退出门禁池（盘小、1 个满核槽，当 gate-host 备胎接门禁是假红与超时的大头），门禁 / 自检只在 gate-host；Spare-host 只跑 Mac 游戏局与出包重活
RHN=$HOST; RHA=(); case $HOST in local|gate-host) ;; spare-host) echo "🔴 门禁不在 Spare-host 跑（10-28 Spare-host 退出门禁池）：用 GATE_HOST=gate-host，gate-host 不就绪会回「环境未就绪」"; exit 2;; *) echo "🔴 GATE_HOST 只认 local / gate-host"; exit 1;; esac
if [ "$HOST" = gate-host ]; then
  REV=$(git rev-parse -q --verify "${GATE_REV:-HEAD}^{commit}") || WHY="待测提交 ${GATE_REV:-HEAD} 解析不了"
  # 门禁机配置自找，找不到红停（11-11 合入运维线，同一个坑踩了三次：新入口 / 新临时树只设 GATE_HOST，没带 S2A_PRIVATE_TOOLS 或
  #   machine.local，原先整份或逐步悄悄退回本机跑——第二道证 486 步全量在本机跑起来才发现）：
  #   ① 私有工具：没给 S2A_PRIVATE_TOOLS → 从 machine.local 读（S2A_MACHINE_LOCAL → 本树 tools/ → 主检出 tools/，主检出按 git 公共目录定位）；
  #   ② 门禁机地址：按现有顺序（tools/machine.sh）找不到、又没显式给 S2A_MACHINE_LOCAL → 用私有工具目录里那份；
  #   ③ 派之前核：remote.sh 在、配得出门禁机地址（S2A_REMOTE / S2A_REMOTE_HOSTS / S2A_REMOTE_DEFAULT）。核不过、且真有步要派门禁机 → 【红】门禁机配置缺失、退出 2，一步都不在本机跑
  #   门禁机连不上 / 盘不足等运行时不可用照旧按 GATE_FALLBACK_LOCAL 退回本机补跑（那是环境问题，这里管的是配置漏传）
  # ① 私有工具已在脚本开头自找（见开头「私有工具自找放最前」），这里只核
  miss=; [ -n "${S2A_PRIVATE_TOOLS:-}" ] && [ -f "$S2A_PRIVATE_TOOLS/remote.sh" ] || miss="私有工具 remote.sh（S2A_PRIVATE_TOOLS=${S2A_PRIVATE_TOOLS:-没设}）"
  if [ -z "${S2A_REMOTE:-}" ]; then
    rhost() { bash "$R/tools/machine.sh" get S2A_REMOTE_HOSTS 2> /dev/null; bash "$R/tools/machine.sh" get S2A_REMOTE_DEFAULT 2> /dev/null; }
    rh=$(rhost)
    if [ -z "$(tr -d '[:space:]' <<< "$rh")" ] && [ -z "${S2A_MACHINE_LOCAL:-}" ] && [ -f "${S2A_PRIVATE_TOOLS:-/nonexistent}/machine.local" ]; then
      export S2A_MACHINE_LOCAL=$S2A_PRIVATE_TOOLS/machine.local; rh=$(rhost); fi
    [ -n "$(tr -d '[:space:]' <<< "$rh")" ] || miss="${miss:+${miss}；}门禁机地址（S2A_REMOTE_HOSTS / S2A_REMOTE_DEFAULT；找过 ${S2A_MACHINE_LOCAL:-本树 tools/machine.local}${MCK:+ 与主检出 $MCK/tools/machine.local}）"
  fi
  [ -z "$WHY" ] && ! git diff --quiet "$REV" -- 2> /dev/null && WHY="工作树的跟踪文件与待测提交 ${REV:0:9} 不同（gate-host 只跑提交）"
fi
WH=(); SK=(); LPI=(); LPK=(); PLAN=; PI=(); F75=(); mx=60   # WH[i] = 本机 / gate-host；SK[i] = 该步在 gate-host 上跳过的子项键；LP* = 本机另跑的子项（步号、键）；PI[m] = 远端第 m 步对应的本机步号
for i in $(seq 1 $N); do
  s=${STEPS[$((i - 1))]}; name=${s%%	*}; lim=${s##*	}; WH[$i]=本机; SK[$i]=
  [ "$HOST" = gate-host ] && [ -z "$WHY" ] || continue
  ks=$(LC_ALL=C awk -F'	' -v n="$name" '$1 == n { print $2 }' < <(printf '%s\n' "$GLT") | tr '\n' ' ')
  case " $ks " in *" * "*) continue;; esac
  WH[$i]=gate-host; for k in $ks; do SK[$i]="${SK[$i]:+${SK[$i]} }$k"; done; [ -n "${SK[$i]}" ] && { LPI+=("$i"); LPK+=("${SK[$i]}"); }   # 同一步的几个键合成本机一份（钩子编一次回放器跑完）
  PLAN="$PLAN$name	${SK[$i]}"$'\n'; PI+=("$i"); [ "$lim" -gt $mx ] && mx=$lim
done
# 配置缺失只在真有步要派门禁机时才红停（全是本机步的批——如只改文档的扫描——照常本机跑，不碰门禁机；public_repo_scan_test 守）
[ -n "${miss:-}" ] && [ ${#PI[@]} -gt 0 ] && { echo "🔴 门禁机配置缺失：GATE_HOST=gate-host，${#PI[@]} 步要派门禁机，但找不到 ${miss}；不退回本机整份跑。给 S2A_PRIVATE_TOOLS / S2A_MACHINE_LOCAL，或在主检出放 tools/machine.local"; exit 2; }
# 门禁自己的控制变量不漏给各步（10-21 实测：远端那份的 GATE_PLAN 漏进 gate_host / train 单测里的假仓库门禁，假门禁当成远端计划、全红；
#   10-21 07:25 列车 tool 档批次的 GATE_TIER=tool 漏进 gate_host_test 的假门禁，假门禁只跑 2 步、7 例不通）。GATE_CHANGED 留着：钩子按它缩范围
# 远端那份没出完整结果（连不上 / 工具链不同 / 中途断 / 排队超时 / 盘不足）：10-29 起退回本机补跑（见下文「本机补跑」）；GATE_FALLBACK_LOCAL=0 时 = 环境未就绪、远端的步记「没跑」
#   （10-28 梳理裁定。原先先换 gate-host ↔ air 再整份退回本机：Spare-host 盘小常年 125、本机负载下计时类单测假红，是全线环境红的大头）；本机步照常出结论。
#   只因环境没跑成（本机步无真红）→ 退出 75（EX_TEMPFAIL），结论行写「门禁 环境未就绪」，调用方据此重排而不是退回分支
RN=${GATE_REMOTE_NAME:-$(basename "$R")}; GC=${GATE_CLEAN:+--clean}; unset GATE_LAYER GATE_TEST_CACHE GATE_ALT_REMOTE GATE_CLEAN GATE_RERUN GATE_PLAN GATE_OUT GATE_HOST GATE_REV GATE_REMOTE_NAME GATE_SKIP GATE_ONLY GATE_TIER GATE_BUDGET_Q GATE_NO_LOADQ GATE_DUAL
NONCE=; if [ ${#PI[@]} -gt 0 ]; then
  NONCE=$(date +%m%d%H%M%S)-$$-$RANDOM; OUTR=build/gate_out/$NONCE
  SL0=${S2A_REMOTE_SLOTS:-}
  # gate-host 满核槽 3 个（10-29 维护者：2→3，第 3 个带内存闸，见 维护方工具 memlow）：分道后两条车道各占一个，第 3 个给二分 / 预检
  rrun() {   # rrun <机器名> [维护方工具 选机参数…]：跑一份远端门禁，日志 $L/remote.log、退出码 $L/remote.rc
    local h=$1 tp; shift
    S2A_REMOTE_SLOTS=${SL0:-$([ "$h" = spare-host ] && echo 1 || echo 3)} bash "$S2A_PRIVATE_TOOLS/remote.sh" "$@" --name "$RN" $GC --rev "$REV" --timeout $((mx + 300)) --sync @gate \
      --env GATE_TIER --env GATE_SCAN_BASE --env GATE_CHANGED --env GATE_HOOK_S --env GATE_IOS_S --env GATE_IOS_WORK_S --env GATE_PLAN --env GATE_OUT --env GATE_TEST_CACHE --env GATE_ROLE \
      --pull "$OUTR=$L/r" -- bash tools/gate.sh > "$L/remote.log" 2>&1 & tp=$!; echo $tp > "$L/remote.tp"; wait $tp; r=$?; rm -f "$L/remote.tp"; echo $r > "$L/remote.rc"; echo "$h" > "$L/remote.host"; }
  ( export GATE_ROLE=$GLROLE GATE_TEST_CACHE=$TCON GATE_TIER=$TIER GATE_SCAN_BASE=${GATE_SCAN_BASE:-$(git rev-parse origin/main)} GATE_PLAN="$PLAN" GATE_OUT="$OUTR" S2A_REMOTE_PRIO=$(( TPRIO < 2 ? TPRIO : 2 ))
    rrun "$RHN" ${RHA[@]+"${RHA[@]}"} ) & RJ=$!   # 跑远端那份的子 shell：收尾限时（tailwait）不当拖尾，它有自己的远端时限
fi
TK=()   # TK[i] = 第 i 步（单测）的缓存键；没有 = 不走缓存
tc_keys() {   # tc_keys <步号…>：给其中的单测步一次算好键（tools/test_cache.py key）；关了 / 没有算键器 / 算失败 = 不走缓存
  [ "$TCON" = 1 ] && [ -f tools/test_cache.py ] || return 0
  local i n f ix=() fs=() ks k j=0
  for i; do n=${STEPS[$((i - 1))]%%	*}; f=$(LC_ALL=C awk -F'	' -v n="$n" '$1 == n { print $2; exit }' < <(printf '%s\n' "$TFL")); [ -n "$f" ] && { ix+=("$i"); fs+=("$f"); }; done
  [ ${#ix[@]} -gt 0 ] || return 0
  ks=$(python3 tools/test_cache.py key . "${fs[@]}" 2> "$L/tckey.log") || return 0
  while IFS= read -r k; do [ $j -lt ${#ix[@]} ] || break; case $k in [0-9a-f]*) TK[${ix[$j]}]=$k;; esac; j=$((j + 1)); done < <(printf '%s\n' "$ks"); }
tcw() {   # tcw <步号> <命令>：有键就包一层：命中 → 不跑、记命中标记；没命中照跑，绿了记缓存
  local k=${TK[$1]:-}; [ -n "$k" ] || { printf '%s' "$2"; return; }
  printf 'if python3 tools/test_cache.py hit %q %q; then : > %q; echo "输入与工具链没变，沿用上次的绿"; exit 0; fi; ( %s ); _tr=$?; [ $_tr = 0 ] && grep -aq "[^[:space:]]" %q && python3 tools/test_cache.py put %q %q; exit $_tr' \
    "$TCD" "$k" "$L/$1.hit" "$2" "$L/$1.log" "$TCD" "$k"; }   # 空跑（退 0、一行输出都没有）不进缓存：否则下次命中「沿用上次的绿」，空跑又绿回来（11-24）
LI=(); for i in $(seq 1 $N); do [ "${WH[$i]}" = 本机 ] && LI+=("$i"); done; tc_keys ${LI[@]+"${LI[@]}"}
# 本机预跑整份令牌（tools/run_token.sh，11-09）11-10 撤：门禁拿着整份令牌再排步令牌 = 持有并等待（21:2x 一份拿着名额 68 分钟一步没跑）；
#   同机同时在跑的步本来就受步令牌（GATE_STEP_PAR）限，整份令牌只多一层等。手工命令（bash tools/run_token.sh …）改排步令牌、级 3（手工）
# 确定性步骤一红就停本份其余步（10-29 列车线：所有步同时起、等最慢的跑完才出结论，语法 / 扫描 10 秒就红的批照样占满 10 分钟）：
#   确定性 = 结果只由树内容决定、不受负载与环境影响的步（GATE_DET_STEPS，| 分隔）；退出 75（环境）/ 124（超时）不算。停掉的步记「没跑完」，
#   结论行前打「早停：…」，列车据此不信任其余步（同批不连坐、只跑红步核都不能用没跑的步当绿）。GATE_DET_STOP=0 关
DET=${GATE_DET_STEPS:-语法|公开仓扫描|Ryujinx 补丁核对|单测 shell_lint_test|单测 shared_path_test|单测 test_wired_test}
detcheck() {   # 一轮：有确定性步骤红 → 写 $L/stop（步名）、TERM 其余还在跑的本机步
  local i n r f j
  [ ! -f "$L/stop" ] && [ "${GATE_DET_STOP:-1}" = 1 ] || return 0
  for i in $(seq 1 $N); do n=${STEPS[$((i - 1))]%%	*}; [ -f "$L/$i.rc" ] || continue
    case "|$DET|" in *"|$n|"*) ;; *) continue;; esac
    read -r r _ < "$L/$i.rc"; case $r in 0|75|124) continue;; esac
    echo "$n" > "$L/stop"
    for f in "$L"/*.tp; do j=${f##*/}; j=${j%.tp}; [ -f "$L/$j.rc" ] || kill -TERM "$(cat "$f")" 2> /dev/null; done; break; done; }
tok_tick() { detcheck; }   # 主进程排令牌的每一轮也查（tok_get 调）
detwait() {   # 等本机步（LJ）跑完；期间有确定性步骤红 → 早停
  local k alive
  # 「还在跑」= 结论没落盘且进程还在（11-10 启动器线：只看 kill -0，步跑完被回收后进程号被别的会话复用，474 步全出结论后主进程空等约 10 分钟）
  while :; do alive=0; for k in "${!LJ[@]}"; do [ -f "$L/${LJN[$k]}.rc" ] && continue; kill -0 "${LJ[$k]}" 2> /dev/null && { alive=1; break; }; done; [ $alive = 0 ] && break
    detcheck; sleep "${GATE_DET_POLL_S:-1}"; done
  wait "${LJ[@]}" 2> /dev/null; tok_putall; }
# 心跳（11-04 合入运维线）：所有步结束前不打结论，令牌 / 满核槽 / 显卡独占排队时日志长时间无输出，外层按「无进展」收卡死（tools/idle_timeout.sh）会误杀——
#   每 GATE_BEAT_S（缺省 300）秒打一行进度；disown 免得下面的 wait 等它，门禁主进程没了自己退出（被外层杀时不留孤儿）
#   sleep 不带门禁的标准输出（11-05：原写法 kill 心跳子 shell 后它睡着的 sleep 仍开着输出管道，外层 $(bash tools/gate.sh) 每份白等至多 GATE_BEAT_S 秒，
#   remote_test / gate_budget_test 等起假门禁的单测在 gate-host 上全部跑满 600 秒）；门禁任何一条路退出都收心跳（含它的 sleep）
GP=$$; ( while sleep "${GATE_BEAT_S:-300}" > /dev/null 2>&1 < /dev/null; kill -0 $GP 2> /dev/null; do echo "门禁进度 $(date '+%H:%M:%S')：本份已出 $(ls "$L"/*.rc 2> /dev/null | wc -l | tr -d ' ') 个步结论，其余仍在跑 / 排队"; done ) & HB=$!; disown $HB 2> /dev/null; trap 'pkill -P $HB 2> /dev/null; kill $HB 2> /dev/null' EXIT
echo $HB > "$L/hb.pid"; [ -f tools/gate_reap.py ] && python3 tools/gate_reap.py $$ "$L" 2> /dev/null   # 收尸进程：本主进程被 KILL 时停掉在跑的步与心跳（见 tools/gate_reap.py）
# 本机那份：整步本机的 + 拆出来的子项；gate-host 那份一次 维护方工具 跑完（带 @gate 共享数据，结果目录拉回 $L/r），两边同时跑
for i in $(seq 1 $N); do s=${STEPS[$((i - 1))]}; [ "${WH[$i]}" = 本机 ] && { r1=${s#*	}; c=${r1%	*}
  case ${s%%	*} in 钩子\ *) [ -n "$HSKIP" ] && c="export GATE_SKIP='$HSKIP'; $c"; [ -n "$HONLY" ] && c="export GATE_ONLY='$HONLY' GATE_HOOK_ONLY='$HONLY'; $c";; esac
  if [ -n "$HONLY$HSKIP" ]; then job "$i" "$c" "${s##*	}"; else job "$i" "$(tcw "$i" "$c")" "${s##*	}"; fi; }; done   # 只跑一部分：不进按步结果缓存（缓存键不含子项键，部分绿会被当整步绿）
{ j=0; for i in ${LPI[@]+"${LPI[@]}"}; do s=${STEPS[$((i - 1))]}; r1=${s#*	}; job "$i.k$j" "export GATE_ONLY='${LPK[$j]}'; ${r1%	*}" "${s##*	}"; j=$((j + 1)); done; }
[ ${#LJ[@]} -gt 0 ] && detwait
# 收尾限时（11-11 合入运维线：门禁机上推测批 154 步 13:24 前全出结论，主进程却在这里的 wait 上又等了 50 分钟到远端时限，
#   结论整份被当成「门禁机不可用」扔掉、本机重跑 74 分钟）：步都出结论后，还在跑的子进程最多再等 GATE_TAIL_WAIT_S（缺省 120）秒；
#   还不退 → 把它们的命令行列进日志（抓是谁攥着不放）、收掉、照常收尾
tailwait() { local t0=$SECONDS k p
  # 本机侧跑远端那份的子 shell（RJ）不算拖尾（11-11：73d022d6b 首版把它也收掉，远端被 TERM、每批环境未就绪；tests/gate_remote_partial_test.sh ⑤）
  while k=$(jobs -pr | grep -vx "${RJ:-0}"); [ -n "$k" ]; do
    if [ $((SECONDS - t0)) -ge "${GATE_TAIL_WAIT_S:-120}" ]; then
      echo "【黄】门禁收尾：步都出结论了，还有 $(grep -c . <<< "$k") 个子进程 ${GATE_TAIL_WAIT_S:-120} 秒不退，收掉：$(for p in $k; do ps -ww -o pid=,command= -p "$p" 2> /dev/null | cut -c1-200; pgrep -P "$p" 2> /dev/null | while read -r c; do ps -ww -o pid=,command= -p "$c" 2> /dev/null | sed 's/^/  └ /' | cut -c1-200; done; done | tr '\n' '；')"
      for p in $k; do pkill -TERM -P "$p" 2> /dev/null; kill -TERM "$p" 2> /dev/null; done; sleep 2; for p in $k; do pkill -KILL -P "$p" 2> /dev/null; kill -KILL "$p" 2> /dev/null; done; break; fi
    sleep 1; done
  wait 2> /dev/null; }
tailwait; pkill -P $HB 2> /dev/null; kill $HB 2> /dev/null
QWHY=; if [ -n "$NONCE" ] && [ "$(cat "$L/r/done" 2> /dev/null)" != "$NONCE" ]; then
  QWHY=$(grep -m1 '【红】' "$L/remote.log" 2> /dev/null | sed 's/^.*【红】\(remote：\)\{0,1\}//' | cut -c1-400); QWHY="$RHN 不可用（remote.sh rc=$(cat "$L/remote.rc" 2> /dev/null || echo ?)：${QWHY:-没有远端结果}）"
  # 按步认（11-11 维护者：门禁要的是每一步的结论，不是远端主进程正常退出）：远端没写 done，但拉回的结果目录里已有结论的步照用，
  #   只把没结论的步当不可用（退回 / 环境未就绪）；全都有结论 → 整份照用
  PM=(); m=0; for i in ${PI[@]+"${PI[@]}"}; do m=$((m + 1)); [ -f "$L/r/$m.rc" ] || PM+=("$i"); done
  if [ ${#PI[@]} -gt 0 ] && [ ${#PM[@]} -lt ${#PI[@]} ]; then
    echo "⤵ ${QWHY}：拉回的结果里 $(( ${#PI[@]} - ${#PM[@]} )) 步已有结论、照用，只 ${#PM[@]} 步没结论"
    if [ ${#PM[@]} = 0 ]; then QWHY=; else PI=("${PM[@]}"); QWHY="$RHN 上 ${#PM[@]} 步没出结论（远端没正常收尾）"; fi; fi
fi
# 远端结果齐了、但有步自报环境未就绪（退出 75，如 gate-host 没装 docker 的 Vulkan 单测）：只把这几步退回本机补跑，其余远端结果照用（11-02）
RDONE=; if [ -z "$QWHY" ] && [ -n "$NONCE" ] && [ "${GATE_FALLBACK_LOCAL:-1}" = 1 ]; then
  P75=(); m=0; for i in ${PI[@]+"${PI[@]}"}; do m=$((m + 1)); r=; read -r r _ < "$L/r/$m.rc" 2> /dev/null; [ "$r" = 75 ] && P75+=("$i"); done
  [ ${#P75[@]} -gt 0 ] && { PI=("${P75[@]}"); QWHY="$RHN 上 ${#P75[@]} 步自报环境未就绪（退出 75），只这几步"; RDONE=1; }
fi
# gate-host 不可用 → 原派 gate-host 的步退回本机补跑（10-29 14:4x 维护者：gate-host 一出问题 81–104 步全停、列车整小时 0 合入、队列堆到 28 条——
#   推翻 10-28「不换 Spare-host、不退回本机」）。Spare-host 仍不接（10-28：盘小、一个满核槽，是门禁假红与超时大头）。GATE_FALLBACK_LOCAL=0 = 旧行为（退 75）。
#   拆了子项的步本机已跑过 GATE_ONLY 那份，补跑带 GATE_SKIP 跑其余；
#   结论行标「［本机补跑：gate-host 不可用］」，汇总时 m 照常递增、远端编号不乱
# gate-host 是因为超载一直没接（维护方工具「超载等降」）→ 不退回本机（本机负载通常更高，补跑同样是假红），按环境未就绪退 75、列车留队稍后再取（10-29 维护者）
#   只在远端没跑完时看超载：远端已跑完、只有几步自报 75（RDONE）时，排队途中出现过的「超载等降」不算——那几步照常退回本机（11-04 #1503 自检：gate-host 缺 docker 的 1 步因排队时超载过一次而没补跑，第一层 34/35）
OVLD=; [ -n "${QWHY:-}" ] && [ -z "${RDONE:-}" ] && grep -q '超载等降' "$L/remote.log" 2> /dev/null && OVLD=1
# 退回上限（11-11 维护者：154 步整份退回本机跑了 74 分钟，正是要消灭的「悄悄本机整份跑」）：要退回的步超过 GATE_FALLBACK_MAX（缺省不限；列车给 20）
#   → 不本机补跑，按环境未就绪退 75，调用方稍后重派门禁机
FBMAX=; [ -n "${QWHY:-}" ] && [ ${#PI[@]} -gt "${GATE_FALLBACK_MAX:-1000000}" ] && { FBMAX=1; echo "⤵ ${QWHY}：要退回本机 ${#PI[@]} 步，超过上限 ${GATE_FALLBACK_MAX}，不本机补跑，按环境未就绪退出、稍后重派 ${RHN}"; }
FBN=0; if [ -n "${QWHY:-}" ] && [ -z "$OVLD" ] && [ -z "$FBMAX" ] && [ "${GATE_FALLBACK_LOCAL:-1}" = 1 ] && [ ${#PI[@]} -gt 0 ]; then
  echo "⤵ ${QWHY}：原派 ${RHN} 的 ${#PI[@]} 步退回本机补跑"
  LJ=(); LJN=(); tc_keys "${PI[@]}"
  for i in "${PI[@]}"; do s=${STEPS[$((i - 1))]}; r1=${s#*	}; c=${r1%	*}; WH[$i]=回退; F75[$i]=${RDONE:-}; FBN=$((FBN + 1))
    [ -n "${SK[$i]}" ] && c="export GATE_SKIP='${SK[$i]}'; $c"; job "$i" "$(tcw "$i" "$c")" "${s##*	}"; done
  [ ${#LJ[@]} -gt 0 ] && wait "${LJ[@]}"; tok_putall
  FBWHY=$QWHY; QWHY=
fi
# >>> show（单测 tests/gate_step_par_test.sh 按标记抽出来测）
gl_put() {   # gl_put <结果文件> <显示名> <退出码> <秒数>：台账记一行（GL 空 = 不记）；写不进不影响门禁
  [ -n "${GL:-}" ] || return 0
  local f=$L/$1 k=- h=本机 w=0 n
  case $1 in r/*) h=${RHN:-gate-host};; *[!0-9]*) ;; *) k=${TK[$1]:-}; k=${k:--};; esac
  [ -f "$f.wait" ] && read -r w < "$f.wait"
  n=$(printf '%s' "$2" | sed 's/\\/\\\\/g; s/"/\\"/g')
  mkdir -p "$(dirname "$GL")" 2> /dev/null
  printf '{"t":%s,"run":"%s","step":"%s","key":"%s","host":"%s","rc":"%s","sec":%s,"wait":%s,"hit":%s,"role":"%s","tier":"%s","layer":"%s","rev":"%s"}\n' \
    "$(date +%s)" "${GLRUN:-}" "$n" "$k" "$h" "$3" "${4:-0}" "${w:-0}" "$([ -f "$f.hit" ] && echo 1 || echo 0)" "${GLROLE:-}" "${TIER:-}" "${LAYER:-}" "${GLREV:-}" >> "$GL" 2> /dev/null
  return 0; }
red_view() {   # red_view <日志>：红步摘要的取材（11-23 维护者：反证输出混进红行摘要，误导两轮定位）——
  #   ① 「【负对照开始】」到「【负对照结束】」之间的行是反证按设计打出的红，一律不算；② 日志里有「【失败子项摘要开始】…【失败子项摘要结束】」
  #   （聚合钩子在结尾给出失败子项自己的红行，见 android/tools/gate_step.sh）就只取最后那段——通过的子项打的红字不进摘要
  awk '/^【失败子项摘要开始】/ { d = 1; buf = ""; next } /^【失败子项摘要结束】/ { d = 0; got = 1; out = buf; next } d { buf = buf $0 "\n"; next }
       /【负对照开始】/ { n = 1; next } /【负对照结束】/ { n = 0; next } !n { all = all $0 "\n" } END { printf "%s", got ? out : all }' "$1" 2> /dev/null
}
show() {   # show <行首> <结果文件（$L 下，不带后缀）> <显示名> <上限>：打一行（红时带细节）；返回 0 绿 / 1 红 / 2 没跑完 / 3 这一步自报环境未就绪（退出 75） / 4 被早停
  local f=$L/$2 rc sec last
  [ -f "$f.rc" ] || { gl_put "$2" "$3" 无 0; if [ -f "$f.started" ]; then echo "${1}🔴 ${3}：没跑完（无退出码：步已起、被强杀（KILL）或门禁先于它退出）"; else echo "${1}🔴 ${3}：没跑完（无退出码：步没起来）"; fi; return 2; }
  read -r rc sec < "$f.rc"; gl_put "$2" "$3" "$rc" "$sec"; last=$(grep -v '^[[:space:]]*$' "$f.log" | grep -av '^\(待宿主证\|已证宿主项\)	' | tail -1 | cut -c1-200)
  # 红步的一句话结论优先取第一条失败行（11-15 gate2：python 单测全文末尾是「OK」——那是别的子单测的，红因在中间，回执只写个 OK，属主看不出）
  [ "$rc" != 0 ] && { red_view "$f.log" > "$f.red"; fl=$(grep -a -m1 -E '^(不通|✗|FAIL: |ERROR: )|AssertionError|【红】' "$f.red" 2> /dev/null | cut -c1-200); [ -n "$fl" ] && last=$fl; }
  # 宿主项（10-30 维护者：与「待 Mac 证」对称）：步 / 子项在本宿主做不了的项打「待宿主证<TAB>类别<TAB>项」、在具备的宿主上做过且绿打「已证宿主项<TAB>类别<TAB>项」，
  #   不影响本步红绿；这里按「类别<TAB>项<TAB>步名」收进 $L/hostlines，结论里列出（列车凭它核宿主证明，见 tools/train.sh hostproof）
  grep -a '^\(待宿主证\|已证宿主项\)	' "$f.log" 2> /dev/null | awk -F'\t' -v n="$CUR" 'NF >= 3 { print $1 "\t" $2 "\t" $3 "\t" n }' >> "$L/hostlines"
  # 空跑（11-24）：退 0 却一行输出都没有 = 没跑任何子项（一台门禁机的 Mac 局槽旧副本不认 share、命令没跑就退 0，门禁显卡步 3 天 210 次记成绿）→ 判红并点名
  if [ "$rc" = 0 ] && ! grep -aq '[^[:space:]]' "$f.log" 2> /dev/null; then
    echo "${1}🔴 $3 $sec 秒（空跑：退出 0 却一行输出都没有，没跑任何子项；查包这一步的外层（锁 / 仲裁脚本）有没有吞掉命令）："; return 1; fi
  if [ "$rc" = 0 ]; then echo "${1}🟢 $3$([ -f "$f.hit" ] && printf （缓存命中） || printf ' ')$sec 秒：$last"; return 0; fi
  # 被早停 TERM 掉的步（确定性步骤已红）：记没跑完，不进红步清单
  if [ "$rc" = 143 ] && [ -f "$(dirname "$f")/stop" ]; then echo "${1}⏹ ${3}：没跑完（确定性步骤「$(cat "$(dirname "$f")/stop")」已红，早停）"; return 4; fi
  # 步自己退 75（EX_TEMPFAIL）= 它的环境没备好（10-29 #793 安卓 APK 子项缺镜像 / 缓存卷按设计退 75）：同远端没就绪，记没跑、不进红步清单，不当分支红
  # 75 只摘最后一行时，它前面那句才是原因（11-22：android_vk_test「容器启动失败 rc=125」两次，docker 自己那句报错没进结论、步日志随后清掉，查不到根因）：
  #   连同前面至多 3 行非空输出一起缩进列出
  if [ "$rc" = 75 ]; then echo "${1}🟡 $3 $sec 秒（rc=75，这一步自报环境未就绪）：$last"
    grep -av '^[[:space:]]*$' "$f.log" 2> /dev/null | tail -4 | sed '$d' | cut -c1-200 | sed "s/^/${1}    /"; return 3; fi
  # 跑满本步墙钟上限被杀（rc=124 且用满上限）= 超时未判（11-04 维护者）：多半是机器负载，不当本支红、不进红步清单；
  #   同 75 走「环境未就绪」（没有真红时整份退 75）。列车遇 75 先核干净 main，main 能过而本支超时才算本支的（真卡死照样拦下）。步自己内部 timeout 退 124 仍是红
  # 无进展看门狗判的挂死（步跑在 tools/nostall.py 下，见 job）：与负载无关，记红；墙钟保底 = 本步上限 × GATE_HARD_X，撞保底才是超时未判
  local ns= hard=$4; [ -f "$f.ns" ] && read -r ns hard < "$f.ns"
  if [ "$rc" = 124 ] && [ -n "$ns" ] && grep -q '【红】无进展看门狗' "$f.log" 2> /dev/null; then echo "${1}🔴 $3 $sec 秒（挂死：连续 ${ns} 秒无进展——日志不长、CPU / 磁盘不涨、没有编译子进程，被停）：$last"; { grep -a '无进展看门狗' "$f.log" | head -2; grep -a -A8 -E '^✗ 组 [^：]+：被停时还在跑|^被停时没有在跑的组' "$f.log" | head -60; } | sed 's/^/    | /'; return 1; fi
  #   ↑ 步自己被停时点名的「哪几组还在跑、各跑多久、输出尾巴」（runtime/gate.sh hung_report，11-19）一并摘进来：原先只摘看门狗那两行，点名写了也看不到
  set -- "$1" "$2" "$3" "$hard"
  if [ "$rc" = 124 ] && [ "$sec" -ge $(($4 - 2)) ]; then echo "${1}🟡 $3 $sec 秒（超时未判：跑满 ${4} 秒上限被杀，负载 $(sysctl -n vm.loadavg 2> /dev/null | awk '{print $2}')；不算本支红）：$last"; echo "$3" >> "$L/timeouts"; return 3; fi
  echo "${1}🔴 $3 $sec 秒（rc=$rc$([ "$rc" = 124 ] && { [ "$sec" -ge $(($4 - 2)) ] && echo "，超 ${4} 秒" || echo "，没到 ${4} 秒上限：是这一步自己退出 124（它内部的 timeout 到点）"; })）：$last"; { grep -E '^(不通|✗)' "$f.red" | head -10; grep -a -E '^(FAIL: |ERROR: )|AssertionError|^  File "[^"]+", line [0-9]+|【红】' "$f.red" | head -5; grep -a '^  ┊' "$f.red" | head -400; grep -v '^[[:space:]]*$' "$f.red" | grep -av '^  ┊' | tail -12; } | sed 's/^/    | /'   # 先列「不通 / ✗」行，再列 python 单测的 FAIL / ERROR / 断言 / 出错文件行号与【红】行（各至多 5 行）：单测全文长时末尾看不到是哪条（✗ 是 python 单测与 widen 系列的写法，10-31 widen_auto_pkg 红行被截掉）
  return 1
}
# <<< show
ran=0; red=0; real=0; nm=0; nl=0; ns=0; m=0; REDN=(); LQ=()
for i in $(seq 1 $N); do
  s=${STEPS[$((i - 1))]}; name=${s%%	*}; lim=${s##*	}; ok=0; CUR=$name
  if [ "$HOST" != gate-host ]; then show "" "$i" "$name" "$lim" || ok=$?
  elif [ "${WH[$i]}" = 本机 ]; then nl=$((nl + 1)); show "" "$i" "${name}［本机］" "$lim" || ok=$?
  elif [ "${WH[$i]}" = 回退 ]; then
    m=$((m + 1)); nl=$((nl + 1)); show "" "$i" "${name}［本机补跑：${RHN} 不可用${SK[$i]:+，不含 ${SK[$i]}}］" "$lim" || ok=$?
    # 远端自报环境未就绪（75）退回本机的步，本机补跑也没过 → 记环境未就绪（75），不记代码红（11-07 维护者：gate-host 上安卓钩子 75 退回本机，
    #   本机负载 31 时 cc 超时被判红，守望据此在 236 个候选里二分追噪声）。远端整份不可用（连不上 / 断）退回的步照旧按本机结果判
    [ -n "${F75[$i]:-}" ] && case $ok in 1|2) echo "    ↳ 按环境未就绪计：${RHN} 自报 75 的步本机补跑也没过（rc 见上），不当代码红、不触发守望二分"; ok=3;; esac
    j=0; for p in ${LPI[@]+"${LPI[@]}"}; do
      [ "$p" = "$i" ] && { show "    ↳ " "$i.k$j" "本机另跑 ${LPK[$j]}" "$lim" || { r=$?; case $r in 1) real=1; ok=1;; 3) [ $ok = 0 ] && ok=3;; *) [ $ok = 1 ] || ok=$r;; esac; }; }
      j=$((j + 1)); done
  else
    m=$((m + 1)); nm=$((nm + 1)); show "" "r/$m" "${name}［${RHN}${SK[$i]:+，不含 ${SK[$i]}}］" "$lim" || ok=$?
    j=0; for p in ${LPI[@]+"${LPI[@]}"}; do
      [ "$p" = "$i" ] && { show "    ↳ " "$i.k$j" "本机另跑 ${LPK[$j]}" "$lim" || { r=$?; case $r in 1) real=1; ok=1;; 3) [ $ok = 0 ] && ok=3;; *) [ $ok = 1 ] || ok=$r;; esac; }; }   # 子项真红记 real：远端那份没跑（2）不能吞掉本机真红
      j=$((j + 1)); done
    [ -n "${SK[$i]}" ] && ns=$((ns + 1))
  fi
  case $ok in 2|3|4) ;; *) ran=$((ran + 1));; esac; [ $ok = 3 ] && LQ+=("$name"); [ $ok = 4 ] && red=1   # 4 = 被早停：没跑、不进红步清单（红的是那步确定性步骤）
  case $ok in 0|3|4) ;; *) red=1; REDN+=("$name");; esac; [ $ok = 1 ] && real=1
done
[ $ran = "$N" ] || red=1
where=; if [ "$HOST" = gate-host ]; then
  if [ -n "$WHY" ]; then where="（GATE_HOST=${RHN}，整份本机跑：${WHY}）"; else where="（${RHN} $nm 步，其中 $ns 步的子项本机另跑；本机 $nl 步$([ $FBN -gt 0 ] && echo "，其中 $FBN 步是 ${RHN} 不可用退回本机补跑")）"; fi
  [ -f "$L/remote.log" ] && grep -m1 '^remote：rc=' "$L/remote.log" | cut -c1-300 | sed "s/^/${RHN} 执行：/"
fi
# 远端同步阶段的黄字（11-09：remote.sh 同步门禁数据时本机缺了离线包等，原先静默跳过，门禁机那份一旧就没人发现）：原样抄进门禁日志，
#   总结行在「应跑 N 步」后面点一句（列车回执、守望都只读末行，守望还截前 120 字，所以点在前面、写短）
RWARN=; if [ -f "$L/remote.log" ]; then RW=$(grep '^【黄】remote：' "$L/remote.log" | sort -u | cut -c1-300); [ -n "$RW" ] && { printf '%s\n' "$RW"; RWARN="；⚠ ${RHN} 同步缺 $(grep -c . <<< "$RW") 项（见黄字行）"; }; fi
CCS=; [ -s "$L/ccache.log" ] && CCS="本机 $(s2a_ccache_sum "$L/ccache.log")"; [ -s "$L/r/ccache.log" ] && CCS="${CCS:+${CCS}；}${RHN:-gate-host} $(s2a_ccache_sum "$L/r/ccache.log")"
[ -n "$CCS" ] && echo "编译缓存（ccache）：$CCS"
# 红步清单（机读，tab 分隔）：列车据此判假红、只重跑这几步（GATE_RERUN）；末行仍是总结
[ ${#REDN[@]} -gt 0 ] && { printf '红步'; printf '\t%s' "${REDN[@]}"; echo; }
for f in "$L/stop" "$L/r/stop"; do [ -f "$f" ] && { echo "早停：确定性步骤「$(cat "$f")」红，其余步没跑完$([ "$f" = "$L/r/stop" ] && echo "（${RHN} 那份）")"; break; }; done
[ ${#OTHER[@]} -gt 0 ] && echo "另一层：${OTHER[*]}"
[ -n "${QWHY:-}" ] && echo "环境未就绪：${RHN} 的步没跑（不换 Spare-host；$([ -n "$OVLD" ] && echo "门禁机超载、本机补跑同样不可信" || { [ -n "$FBMAX" ] && echo "要退回的步超过上限 ${GATE_FALLBACK_MAX}" || echo "GATE_FALLBACK_LOCAL=0"; })，不退回本机）：${QWHY}"
[ $FBN -gt 0 ] && echo "${RHN} 不可用，$FBN 步退回本机补跑：${FBWHY}"
[ -s "$L/timeouts" ] && echo "超时未判（跑满步上限被杀，不算本支红，环境好了重跑）：$(tr '\n' '；' < "$L/timeouts")"
[ ${#LQ[@]} -gt 0 ] && echo "环境未就绪：本机步自报（退出 75）：$(printf '%s；' "${LQ[@]}")"
# 远端那份：结果齐了的标记（本机凭它认这次的结果，没有 = 远端没跑完）。写在「环境未就绪退 75」之前（11-02：远端有一步自报 75 时整份没写标记，
#   本机当成 gate-host 不可用、把 gate-host 已绿的 11 步全部退回本机重跑，挤本机显卡与普查）
[ -n "$PLANM" ] && basename "$L" > "$L/done"
if { [ -n "${QWHY:-}" ] || [ ${#LQ[@]} -gt 0 ]; } && [ $real = 0 ]; then gl_end 环境未就绪; echo "门禁 环境未就绪：$TIER 档，跑了 $ran 步 / 应跑 $N 步${RWARN}$where"; exit 75; fi
macsum
# 宿主项汇总：机读行原样（去重）；有待证项 → 末行写「本机绿、待<类别>证」，不许被当成整份门禁绿（列车另核证明）
HPL=; if [ -s "$L/hostlines" ]; then sort -u "$L/hostlines" > "$L/hostlines.u"; cat "$L/hostlines.u"
  while IFS=$'\t' read -r k c it st; do [ "$k" = 待宿主证 ] && echo "⏭ ${it}（${st}）：本机不是${c}，待${c}证"; done < "$L/hostlines.u"
  HPL=$(LC_ALL=C awk -F'\t' '$1 == "待宿主证" { n[$2]++ } END { for (c in n) printf "；待%s证 %d 项", c, n[c] }' "$L/hostlines.u"); fi
if [ -n "$HPL" ] && [ "$MACN" = 0 ]; then echo "门禁 $([ $red = 0 ] && echo "本机绿、待宿主证" || echo 红)：$TIER 档，本机已证：跑了 $ran 步 / 应跑 $N 步${RWARN}${HPL}$where"
elif [ "$MACN" -gt 0 ]; then echo "门禁 $([ $red = 0 ] && echo 本机绿、待 Mac 证 || echo 红)：$TIER 档，本机已证：跑了 $ran 步 / 应跑 $N 步${MACL}${HPL}"
else echo "门禁 $([ $red = 0 ] && echo 绿 || echo 红)：$TIER 档，跑了 $ran 步 / 应跑 $N 步${RWARN}$where"; fi
gl_end "$([ $red = 0 ] && echo 绿 || echo 红)"
exit $red
