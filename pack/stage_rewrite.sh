#!/bin/bash
# 出包·改写阶段：pack/stage_rewrite.sh <款>
# 改写器 = 本树 C 改写器（runtime/rewriter，pack/rewriter.py c-path 编出；10-31 删 Python 改写器 patch_nso.py，正确性靠金样字节 + 进局验证）。
# 把现行产物根里 exefs/ 的每个模块（rtld 与 *.npdm 除外）离线改写，
# 产物（<模块>.patched.bin / .fixups.tsv / .gates.tsv / .sites.tsv / .gaps.tsv）与 main.npdm 放进新版本产物根顶层并原子切现行；
# Mac 入口 runtime/run_mac.sh 见到 *.patched.bin 就直接用，不再每局现改写（01006F8002326000现改写约 30 秒，10-19 实测）。
# 宽屏第二层：改写前 graphics/widen/scan/wide_scan.py 出 widen_sites.tsv（零配置），作改写器第 4 参；启用表按 main 构建号从
#   仓库 graphics/widen/tables/<款>_<构建号>/widen_enable.<档位>.tsv（handheld / docked，各档位闭环全过才有）拷进产物根，装载器按 S2A_DOCKED 读当前档位那张
#   （画幅线闭环产出、提交进仓库走列车，和代码一样有版本；没有这一档位的表 = 这一档位不加宽）。
#   10-20 前放仓库外 ~/.s2arm/run/s2arm/widen/，表一变，产物根 / 包和提交对不上（01006F8002326000主机档表写进根之前出的包仍是旧表），故迁进仓库。
# 缓存键 = 改写器 + 扫描器源码树对象号 + 启用表 sha（+ 非苹果目标平台 S2A_TARGET）+ 本阶段脚本 + 实际读过的生成器与表（.stage_rewrite.deps，tools/stage_deps.py）：
#   版本标签 rw-<不含实际读入部分的键前 10 位>（非苹果再带 -<平台>），现行根记的键相同就跳过。
#   S2A_TARGET 决定改写器出的补丁代码（如鸿蒙线程槽取法），不进键会让一个平台的产物被当成另一个平台的现行产物复用；缺省 apple 时键与原先相同。
# 改写有缺口（退出码 3、产物照出）→ 降级继续：缺口点保持原指令，按类汇总进产物根 degrade.rewrite.tsv、打【黄】（v0.3.1 拿不准不停）；
#   退出码 3 却没产物（补丁区放不下 / 丢自旋回边）与其余失败 → 中止新版本、现行不动、报红。单模块按无进展看门狗停（tools/nostall.py：CPU 在涨 / 在写就不停，连续 S2A_NOSTALL_S 秒〔缺省 600〕没进展才停；
#   v0.3.1 拿不准不停：原先固定墙钟 timeout 600，满载机上正常慢的大模块会被当挂死杀掉）。
set -u
G=${1:?款}; H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/.." && pwd); source "$H/roots.sh"
# 扫描器与本阶段各 Python 工具的解释器钉死一个（tools/rewriter-requirements.txt：capstone / lz4 版本固定，宽屏扫描器用；10-22 gate-host 的 /usr/bin/python3 没有 capstone，门禁改写全失败）
#   顺序：S2A_PYTHON → S2A_PY → tools/machine.local 的 S2A_PY（开发机在那里声明钉版 venv）→ PATH 上的 python3（10-23 维护者：不许写死私人路径；11-07 代码里不再探开发机 venv）
PY=${S2A_PYTHON:-${S2A_PY:-$(bash "$R/tools/machine.sh" get S2A_PY 2> /dev/null)}}; PY=${PY:-$(command -v python3)}
[ -n "$PY" ] && [ -x "$PY" ] || { echo "【红】$G 改写器找不到 Python 解释器：装 Python $(sed -n 's/^PYTHON_MINIMUM = (\([0-9]*\), \([0-9]*\)).*/\1.\2/p' "$R/pack/jobs/toolchain.py")+ 后按 tools/rewriter-requirements.txt 装依赖，或用 S2A_PYTHON=<解释器> 指定"; exit 1; }
CUR=$(root_cur "$G"); [ -n "$CUR" ] || { echo "【红】$G 没有现行产物根（先 pack/seed_root.sh）"; exit 1; }
[ -f "$CUR/exefs/main" ] || { echo "【红】$CUR 里没有 exefs/main"; exit 1; }
# 真实输入记录（tools/stage_deps.py，10-30 根修：键原先手工列生成器，fps_warehouse / profile_guess 等漏进键，热出包跳过改写用旧产物）：
#   各阶段同一套（pack/stage_keyfiles.sh stage_deps_on / _key / _save）：本阶段起的每个 Python 进程都记读过的文件 / 列过的目录，阶段结束存成产物根 .stage_rewrite.deps，
#   下次按它重算当前内容进键。改写器本体（下面起的 C / Python 后端）不记：它由下面的树对象号 + 后端键管
source "$H/stage_keyfiles.sh"; stage_deps_on || { echo "【红】$G 开不了实际读入记录"; exit 1; }
AD=$(mktemp -d); trap 'rm -rf "$AD"' EXIT
# 宽屏第二层（画幅线，接口文档 §4.8）：启用表是闭环验证的产出，按 main 构建号对应（版本一换自然失效），没有就不加宽
BID=$(xxd -s 0x40 -l 16 -p "$CUR/exefs/main")
# 仓库表（graphics/widen/tables，人工验过的）优先，没有用出包自动出的表（graphics/widen/scan/auto_table.sh，10-24：新款零配置）；规则在 auto_pkg.sh list
EN=(); while read -r f; do [ -n "$f" ] && EN+=("$f"); done < <(S2A_WIDEN_DIR="${S2A_WIDEN_DIR:-$R/graphics/widen/tables}" bash "$R/graphics/widen/scan/auto_pkg.sh" list "$G" "$BID")   # 选表规则一处（仓库 > 自动）
# 宽屏已验证名单（10-26 维护者：没验证过宽屏的游戏一律 16:9 原比例加黑边）：graphics/widen/tables/verified.tsv 有本款「程序号 + main 构建号」那行就写进产物根 widen_verified.tsv，
#   原生层（runtime/src/widen.c widen_verified）只认这个文件；没有 = 屏幕宽按 1920，各层宽屏都不开。那一行进改写键：登记 / 撤销都会重出改写阶段
PID=$("$PY" -c 'import struct,sys; d=open(sys.argv[1],"rb").read(); a=struct.unpack_from("<I",d,0x70)[0]; assert d[a:a+4]==b"ACI0"; print("%016X" % struct.unpack_from("<Q",d,a+0x10)[0])' "$CUR/exefs/main.npdm") || { echo "【红】$G 读不出 main.npdm 的程序号"; exit 1; }
VT="${S2A_WIDEN_DIR:-$R/graphics/widen/tables}/verified.tsv"; VR=$([ -f "$VT" ] && awk -F'\t' -v p="$PID" -v b="$BID" '!/^#/ && toupper($1) == p && tolower($2) == b && tolower($4) == "apple" { print; exit }' "$VT")   # F7：表加平台列，苹果路线只取 apple 行——安卓行不进苹果产物根，apple 行也放不开安卓（安卓标记由 pack/stage_caps.sh 写）
# 名单与依赖一致（11-05 维护者）：已验证名单里的款必须在仓库里有它依赖的宽屏表（第二层启用表 / 第三层整套表，repo_table_dir 按构建号找）；
#   缺了就不算已验证、按满屏关出包，写一行【黄】（01007EF00011E000：表按款名起、包名对不上，启用 0 点退整套放宽 → SIGBUS）
[ -n "$VR" ] && [ ${#EN[@]} -eq 0 ] && { echo "【黄】$G 在宽屏已验证名单（${PID} ${BID}）但仓库没有它依赖的宽屏表（graphics/widen/tables/*_${BID}），不算已验证、按满屏关出包"; VR=; }
# 同身份的仓库表和最近 30 局真实日志只提高已知下限（不读现行根上一次出的表：采集结果回灌自己会让旧值永不退场）。先采集再判缓存；坏表不可退成无表。
# 帧率表（10-26，体系提升方案 D）：仓库 runtime/fps/<main 构建号前 16 位>.tsv 有就拷进产物根作 fps.tsv（runtime/src/fps.c 装载后读），按「文件名 + 内容」进键；没有 = 原生帧率
#   11-18：我方帧率表（改指令 c 行打进游戏代码，如去掉游戏自己的 30 帧下限）只在内部对照口径 S2A_CAPS_OWN=1 才进包。11-13 撤我方 60 帧后运行时只把槽写成关值，
#   但 c 行照样打进程序：玩家 60 帧关着游戏也能跑到 60，按 30 帧口径写死的时钟（布料 / 头发）快一倍（#1607 过场加速）。不进包 = 程序与原版一致、社区帧率补丁照常收
#   只有 build / speed 行的表（实时步长表，至少一行 speed）不放开帧率、不改指令，只让宿主每次出图把实测帧时写进键页槽：缺省口径也进包（同 pack/orch/stages.py step_only_table）
FT="$R/runtime/fps/${BID:0:16}.tsv"; { [ -f "${FT}" ] && { [ "${S2A_CAPS_OWN:-}" = 1 ] || awk '{ sub(/#.*/, "") } NF { if ($1 == "speed") s = 1; else if ($1 != "build") b = 1 } END { exit !(s && !b) }' "${FT}"; }; } || FT=
# 游戏缺陷修补表（11-16，runtime/fix/README.md）：仓库 runtime/fix/<main 构建号前 16 位>.tsv 有就交改写器（S2A_FIX_TSV，fix_rows），按内容进键；不进产物根
XT="$R/runtime/fix/${BID:0:16}.tsv"; [ -f "${XT}" ] || XT=
# 社区画面补丁（pack/cpatch.py）：匹配上的补丁编成「双路 + 槽」进包，开关在运行时。
#   11-12 维护者拍板：出包时自动对接公开库（pack/jobs/patches.py 按真构建号查索引、取文件，交 S2A_CPATCH_DIRS），查到的全部进包、缺省关、玩家在启动设置里开；
#   10-30「公开发布的包不带上游补丁字节」作废。我方不托管补丁字节，只在玩家本机按需下载
#   缺省开 = 仓库 graphics/cpatch/verified.tsv 里本款「程序号 + main 构建号」的行（我方验过）
#   + S2A_CPATCH（逗号分隔的编号；none = 这次不进社区补丁）+ 启动器玩家选择；补丁数据目录没有 = 不进，产物与原先逐字节相同。
#   缺省开的编号、补丁索引与 pack/cpatch.py 进改写键
CT="$R/graphics/cpatch/verified.tsv"; CV=$([ -f "$CT" ] && awk -F'\t' -v p="$PID" -v b="$BID" '!/^#/ && toupper($1) == p && tolower($2) == b { print $3 }' "$CT" | paste -sd, -)
CSEL=${S2A_CPATCH:-$CV}; [ -n "${S2A_CPATCH:-}" ] && [ -n "$CV" ] && [ "$S2A_CPATCH" != all ] && [ "$S2A_CPATCH" != none ] && CSEL="$CV,$S2A_CPATCH"
# 启动器玩家选择（交付线 pack/jobs/community.py：games/<程序号>/community.json，{"enabled":{"<编号>":true}}）：开着的编号并进选择；ws:verified 是宽屏运行时开关（runtime/src/widen.c），不在这里打
if [ -n "${S2A_COMMUNITY_SELECT:-}" ] && [ -f "$S2A_COMMUNITY_SELECT" ]; then
  CJ=$("$PY" -c 'import json,sys; e=json.load(open(sys.argv[1])).get("enabled") or {}; print(",".join(sorted(k for k,v in e.items() if v is True and k != "ws:verified")))' "$S2A_COMMUNITY_SELECT") || { echo "【红】$G 读不了玩家的社区补丁选择 $S2A_COMMUNITY_SELECT"; exit 1; }
  [ -z "$CJ" ] || { [ -n "$CSEL" ] && [ "$CSEL" != none ] && CSEL="$CSEL,$CJ" || CSEL=$CJ; }
fi
# 补丁索引目录：S2A_CPATCH_DIRS（冒号分隔）没给就同启动器（pack/jobs/community.py _indexes），S2A_COMMUNITY_DIR（环境 → tools/machine.local；都没有 = 没有社区补丁，不再缺省读开发机数据盘）下 INDEX.tsv 所在目录：
#   第一轮布局 <包>/full/<名>/INDEX.tsv、第二轮布局 <包>/<类>/INDEX.tsv（10-27 gfxpatches：60fps / ultrawide / other-gfx，之前只认第一轮、整包没进）；名字以 -lite 结尾的包是同轮子集，不收
CDIR=${S2A_COMMUNITY_DIR:-$(bash "$R/tools/machine.sh" get S2A_COMMUNITY_DIR 2> /dev/null)}
# S2A_CPATCH=none（出包页取消 S 级优化，pack/jobs/patches.py prepare）：不从本机补丁库目录兜底，帧率仓库条目（下面 fps_warehouse）也不读
[ "${S2A_CPATCH:-}" = none ] || [ -n "${S2A_CPATCH_DIRS:-}" ] || [ -z "$CDIR" ] || S2A_CPATCH_DIRS=$(for f in "$CDIR"/*/full/*/INDEX.tsv "$CDIR"/*/*/INDEX.tsv; do
  [ -f "$f" ] || continue; [[ $f == */*-lite/* ]] && continue; dirname "$f"; done | paste -sd: -)
# 金手指（11-12 维护者：出包时把能编进包的金手指带上）：S2A_CHEATS_DIRS（冒号分隔，玩家导入的 + 启动器从公开库取的 Atmosphere txt）里改代码的条目
#   走同一条双路 + 槽（pack/cpatch.py scan_cheats），缺省全关、局内逐条开关；只写可写内存的条目不进包，照旧由运行时 Tamper 执行。
#   金手指文件内容进改写键（开关 enabled.txt / 状态 status.txt 不进：玩家开关不重跑改写）
CI=(); [ -n "${S2A_CPATCH_DIRS:-}" ] && IFS=: read -r -a CI <<< "$S2A_CPATCH_DIRS"   # 11-12 维护者：查到的补丁全部进包、缺省关、玩家自己开（10-30「玩家包不带上游补丁字节」作废）
CK=; if { [ ${#CI[@]} -gt 0 ] || [ -n "${S2A_CHEATS_DIRS:-}" ]; } && [ "${S2A_CPATCH:-}" != none ]; then
  CK=" cpatch=$( { printf '%s\n' "$CSEL"; cat "$R/pack/cpatch.py"; for d in ${CI[@]+"${CI[@]}"}; do cat "$d/INDEX.tsv" 2> /dev/null; done
    IFS=:; for d in ${S2A_CHEATS_DIRS:-}; do for f in "$d"/*.txt; do case ${f##*/} in enabled.txt | status.txt) continue ;; esac; [ -f "$f" ] && { printf '%s\n' "${f##*/}"; cat "$f"; }; done; done; } | shasum -a 256 | cut -c1-16)"
fi
AN=$AD/addr_need.tsv
# 程序号取 exefs/main.npdm 的 ACI0（真身份，每个根都有）；unpack.tsv 只有解包 / 直连播种才带（#459：只出通用的根没有它，出包停在这里）
# 需求表只看 main 的代码特征（静态识别开局巨大别名区），不读逐款表、不读跑局日志：玩家出包不起局、零配置
AA=("$BID" "$AN" --npdm "$CUR/exefs/main.npdm" --key --main "$CUR/exefs/main")
AK=$("$PY" "$R/tools/addr_need.py" "${AA[@]}" 2> "$AD/log") || { cat "$AD/log" >&2; exit 1; }
echo "  $G 地址需求：$(grep -E '^(别名区GB|普通堆MB|别名区上限)' "$AN" | tr '\n\t' ' ')"
UB=$(grep '^上限不定原因' "$AN" | cut -f2-); [ -z "$UB" ] || echo "  $G 地址需求：别名区上限不定，宿主不收窗口（照旧 64 GiB + 远端）——$UB"
# 改写器进键只取它的编译输入（rewriter.py source：CMakeLists / main.c / rewriter.c / rewriter.h / prepare_deps.py）+ 本阶段调的 nro_to_nso.py，
#   不取整个 runtime/rewriter 目录的树号（10-31：原先 README / 许可说明改一行，全部款整段重改写）
SD=$("$PY" "$H/rewriter.py" source) && [ -s "$R/runtime/rewriter/nro_to_nso.py" ] || { echo "【红】取不到改写器编译输入指纹（pack/rewriter.py source）或 runtime/rewriter/nro_to_nso.py"; exit 1; }
KR=$( { echo "$SD"; cat "$R/runtime/rewriter/nro_to_nso.py"; } | shasum -a 256 | cut -c1-40); [ ${#KR} = 40 ] && KS=$(scan_tree "$R") || { echo "【红】改写器指纹不完整或取不到 graphics/widen/scan 的树对象号"; exit 1; }
RK=$("$PY" "$H/rewriter.py" key) || exit 1
T=${S2A_TARGET:-apple}; TK=; [ "$T" = apple ] || TK=" target=$T"
# 自旋闸（S2A_REWRITE_SPIN=1，原生层借鉴上游的一项）改改写产物（多出 S 行与 spin 门），要进键：不进键时私有根从共享现行起步会判「改写没变」跳过，出的包没有自旋区（10-24 测试出包线）
# 10-24 缺省开（S2A_REWRITE_SPIN=0 关）：虚拟核改按需后，自旋忙等的能耗由自旋闸管（docs/研究/虚拟核与轻量调度语义.md）
export S2A_REWRITE_SPIN=${S2A_REWRITE_SPIN:-1}; SK=; [ "$S2A_REWRITE_SPIN" = 1 ] && SK=" spin=1"
# 启用表只按「文件名 + 内容」进键（10-23 测试出包线：原先 shasum 输出带完整路径，换个检出目录键就变，别的树出包 / 只出程序的键核对全判改写过期）
K=$(printf '%s %s %s%s%s' "$KR" "$KS" "$([ ${#EN[@]} -gt 0 ] && for f in "${EN[@]}"; do printf '%s %s\n' "$(basename "$f")" "$(shasum -a 256 < "$f" | cut -c1-64)"; done | shasum -a 256 | cut -c1-16 || echo 无启用表)" "$TK$SK" " addr=$AK" | shasum -a 256 | cut -c1-10)
# 补丁包 [code] 段（补丁包_v1设计.md 12.11，正确性类补丁；11-02 首款激战前锋）：补丁目录 S2A_PATCH_DIR（宿主同名变量，本机路径不进仓库）下 <程序号>/patch.tsv 有就交改写器
#   （S2A_CODE_PATCH；S2A_CODE_BUILD = main 构建号，与文件头 build 同 → 按偏移，否则按 sig 扫，见 rewriter.c code_rows）。不受 S2A_COMMUNITY_PATCHES 限制；
#   文件头 build 与 [code] 段原文进改写键（别的段是宿主运行时读的数据，改了不重改写）
CP=; PK=; [ -n "${S2A_PATCH_DIR:-}" ] && [ -f "$S2A_PATCH_DIR/$PID/patch.tsv" ] && CP=$S2A_PATCH_DIR/$PID/patch.tsv
[ -z "$CP" ] || PK=" code=$(awk '/^\[/ { s = $0; next } s == "" && $1 == "build" || s == "[code]"' "$CP" | shasum -a 256 | cut -c1-16)"
# 补丁随包（补丁包部署方案 v2，满屏补丁线 11-04；维护者：每款补丁随启动器下发、验过才开）：补丁文件头 verified_builds（空格分隔的 32 位构建号）含本包构建号
#   = 这一版真机验收过 → 整份拷进产物根 patch.tsv，进资源包根（pack/ipa_key.sh ipa_pkg_srcs），宿主在 Documents/S2A/patches 没有时读它（runtime/src/patch.c）；
#   没验过的补丁不进包（Mac 局 / 普查照旧按 S2A_PATCH_DIR 直读）。整份内容进键：补丁改了要重出包
source "$H/patch_ship.sh"; PS=; [ -n "$CP" ] && ipa_patch_ship "$CP" "$BID" && PS=$CP
[ -z "$PS" ] || PK="$PK ship=$(shasum -a 256 < "$PS" | cut -c1-16)"
# 改写器进键：替换显式 C 二进制（S2A_REWRITER_C / App 自带）必须重改写，不能复用另一份二进制的阶段戳。
K=$(printf '%s backend=%s%s%s%s%s' "$K" "$RK" "$CK" "$PK" "$([ -n "${FT}" ] && printf ' fps=%s' "$(shasum -a 256 < "${FT}" | cut -c1-16)")$([ -n "${XT}" ] && printf ' fix=%s' "$(shasum -a 256 < "${XT}" | cut -c1-16)")" "$([ -n "$VR" ] && printf ' verified=%s' "$(printf '%s' "$VR" | shasum -a 256 | cut -c1-16)")" | shasum -a 256 | cut -c1-10)
# 仓库输入清单不含产物根：main 构建号相同也可能换了 subsdk 或 NRO，按相对路径 + 完整内容进键。
# NRO 枚举同 nro_to_nso.py：任意深度（含点目录）、后缀不分大小写；分块读取只限内存，不抽样内容。
# RomFS 遍历经 graphics/nvn/tools/romfs_list.py（仓库里唯一的 RomFS 遍历，walk 同 os.walk）
# NRR 登记表（*.nrr）同样按内容进键：nro_to_nso.py 按它到 romfs 其余文件（一款第一方格斗游戏 data.arc）里找藏着的 NRO。那些大文件本身不进键（17.6 GB 每次判跳过都重算不可取）：
#   收进的 NRO 只认 NRR 登记的 SHA-256，内容由登记表定死；只有「NRR 不变、归档里换了位置 / 删了」才会让表里来源列或有无变而键不变。
# 运行时留下的 NRO（S2A_NRO_KEPT_DIR，启动器给游戏库 games/<号>/nro_kept/；#8be819 游戏自有加密归档里的 NRO，宿主 LoadNro 缺表时存下游戏解开的那份）：
#   nro_to_nso.py 第三来源，*.nro 按文件名 + 内容进键（新留下一份 = 要重改写）。没设 / 目录不在 = 无，键与原先相同
KEPT=${S2A_NRO_KEPT_DIR:-}; [ -n "$KEPT" ] && [ -d "$KEPT" ] || KEPT=
IK=$("$PY" - "$CUR" "$R" "$KEPT" <<'INPUT_KEY'
import hashlib, os, sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, os.path.join(sys.argv[2], 'graphics', 'nvn', 'tools'))
import romfs_list
paths = [p for p in (root / 'exefs').iterdir() if p.is_file()]
def walk_error(error):
    raise error
romfs = root / 'romfs'
if romfs_list.is_dir(romfs):   # RomFS 目录或镜像 romfs.std（romfs_list image_of）；镜像里的 NRO / NRR 下面经 open_file 读
    for directory, _, names in romfs_list.walk(romfs, onerror=walk_error):
        paths.extend(Path(directory) / name for name in names if name.lower().endswith(('.nro', '.nrr')))
digest = hashlib.sha256(b'rewrite-inputs-v1\0')
for path in sorted(paths, key=lambda p: os.fsencode(p.relative_to(root).as_posix())):
    relative = os.fsencode(path.relative_to(root).as_posix())
    content = hashlib.sha256()
    with romfs_list.open_file(path) as stream:   # 盘上的文件同 open；RomFS 只有镜像时从镜像读
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            content.update(block)
    digest.update(len(relative).to_bytes(8, 'little'))
    digest.update(relative)
    digest.update(content.digest())
if sys.argv[3]:
    for path in sorted(Path(sys.argv[3]).glob('*.nro'), key=lambda p: os.fsencode(p.name)):
        content = hashlib.sha256(path.read_bytes()).digest()
        digest.update(b'kept\0' + len(path.name).to_bytes(8, 'little') + os.fsencode(path.name) + content)
print(digest.hexdigest())
INPUT_KEY
) || { echo "【红】$G 读不出改写输入内容指纹" >&2; exit 1; }
[ ${#IK} = 64 ] || { echo "【红】$G 改写输入内容指纹不完整" >&2; exit 1; }
K=$(printf '%s inputs=%s' "$K" "$IK" | shasum -a 256 | cut -c1-10)
TAG=rw-$K; [ "$T" = apple ] || TAG=$TAG-$T; [ -n "$SK" ] && TAG=$TAG-spin
# 真实输入进键：现行根上次记下的清单按当前内容重算（没有清单 = 无，必然重改写一次）；收尾按本次新清单算同一口径的键写 .stage_rewrite
KB=$K; K=$(stage_deps_key "$CUR" rewrite "$KB" "$H/stage_rewrite.sh") || exit 1
root_stage_done "$CUR" rewrite "$K" && { echo "🟢 $G 现行已是改写 ${K}（改写器 / 扫描器 / 启用表 / 实际读过的生成器与表都没变），跳过"; exit 0; }
# 只算键 / 同键跳过时不编译。编译失败先停，尚未创建新版产物根。
C=$("$PY" "$H/rewriter.py" c-path) || exit 1
REWRITE=("$C")
TAG=$(root_tag "$G" "$TAG")
W=$(root_begin "$G" "$TAG") || exit 1
rm -f "$W"/*.code_patch.tsv "$W"/*.patched.bin "$W"/*.fixups.tsv "$W"/*.gates.tsv "$W"/*.sites.tsv "$W"/*.gaps.tsv "$W"/*.npdm "$W"/*.widen.tsv "$W"/widen_sites.tsv "$W"/cpatch.tsv "$W"/cpatch_rows.tsv "$W"/cpatch_plan.log "$W"/cpatch_excluded.tsv "$W"/cheat-src.*.txt "$W"/fpslocker.json "$W"/widen_enable*.tsv "$W"/widen_verified.tsv "$W"/widen_full*.tsv "$W"/addr_need.tsv "$W"/nro_exports.tsv "$W"/fps.tsv "$W"/patch.tsv   # 上一版改写产物不带进新版
t0=$SECONDS; n=0
# romfs 里的 NRO（10-29 普查 c1029c：大航海4 HD / 魔界战记6 / 忍者龟用 nn::ro 装，我方 LoadNro 映射未支持 → 游戏 Break）：
#   转成 NSO 放 nroexe/、与 exefs 模块一样改写（模块名 nro0…），运行时 ro LoadNro 按 NRO 文件 SHA-256 查 nro_modules.tsv 映射改写好的映像（runtime/src/loader.c s2a_nro_map）。
#   零配置：romfs 任意深度的 *.nro；有 .nrr 登记表时还收藏在 romfs 其余文件里（原样 / zstd 帧）、哈希登记过的 NRO（11-03 一款第一方格斗游戏 data.arc）；没有 NRO 不出表。nroexe/ 是中间产物，改写完删
rm -f "$W/nro_modules.tsv"; rm -rf "$W/nroexe"
"$PY" "$R/runtime/rewriter/nro_to_nso.py" "$W/romfs" "$W/nroexe" ${KEPT:+"$KEPT"} > "$W/nro_to_nso.log" 2>&1 || { echo "【红】$G 转 romfs 里的 NRO 失败（$(tail -1 "$W/nro_to_nso.log")）"; root_abort "$G" "$TAG"; exit 1; }
[ ! -f "$W/nroexe/nro_modules.tsv" ] || mv "$W/nroexe/nro_modules.tsv" "$W/nro_modules.tsv" || { root_abort "$G" "$TAG"; exit 1; }
"$PY" "$R/graphics/widen/scan/wide_scan.py" "$W/exefs" --tsv "$W/widen_sites.tsv" > "$W/widen_scan.log" 2>&1 || { echo "【红】$G 宽屏候选扫描失败（$(tail -1 "$W/widen_scan.log")）"; root_abort "$G" "$TAG"; exit 1; }
for t in ${EN[@]+"${EN[@]}"}; do cp "$t" "$W/" || { root_abort "$G" "$TAG"; exit 1; }; done
[ -z "$VR" ] || printf '%s\n' "$VR" > "$W/widen_verified.tsv" || { root_abort "$G" "$TAG"; exit 1; }
[ -z "${FT}" ] || cp "${FT}" "$W/fps.tsv" || { root_abort "$G" "$TAG"; exit 1; }
[ -z "$PS" ] || cp "$PS" "$W/patch.tsv" || { root_abort "$G" "$TAG"; exit 1; }
# 帧率表的 c 行（按构建号改指令）由改写器读（rewriter.c）；没有帧率表就不给，改写产物与原先逐字节相同
if [ -n "${FT}" ]; then export S2A_FPS_TSV="$W/fps.tsv"; else unset S2A_FPS_TSV; fi
if [ -n "${XT}" ]; then export S2A_FIX_TSV="${XT}"; else unset S2A_FIX_TSV; fi
if [ -n "$CP" ]; then export S2A_CODE_PATCH=$CP S2A_CODE_BUILD=$BID; else unset S2A_CODE_PATCH S2A_CODE_BUILD; fi
# 各模块并行改写（10-25 维护者「多核加速……要适配机器的核心数」）：改写器只读本模块与 widen_sites.tsv、产物都带模块名前缀（<模块>.patched.bin / .fixups.tsv / …），
#   互不依赖；同时最多 S2A_JOBS（缺省本机核数，读不出就停：#824 F-pack-sysctl-jobs）个，退出码记临时目录（不进产物根），全部跑完再按模块名顺序判
# rw_all：全部模块改写一遍；退出码 3（有缺口、产物照出）的模块记进 R3、不当失败（社区补丁轮：缺口由 cpatch.py prune 认领；其余缺口降级继续）
rw_all() {
NJ=${S2A_JOBS:-$(getconf _NPROCESSORS_ONLN)}; [ "$NJ" -ge 1 ] 2> /dev/null || { echo "【红】$G 并行度读不出（S2A_JOBS / getconf 核数），不放开并发"; root_abort "$G" "$TAG"; exit 1; }
RC=$(mktemp -d); MS=(); R3=(); n=0
for f in "$W"/exefs/* "$W"/nroexe/nro*; do
  [ -f "$f" ] || continue
  # exefs 只改写宿主会装载的代码模块（runtime/src/host_main.c：main、subsdk0–9、sdk），与真机装载器同一份名单；别的文件不是 NSO
  #   （11-02 普查 010020D01B890000 exefs 有 486 字节文本 main.moduleconfig「VIA_MODULE_AUDIORENDER;…」，原先按「不是 rtld / *.npdm」当模块改写 → 断言 NSO0 失败、整款出包红）
  m=$(basename "$f"); case $f in "$W"/exefs/*) case $m in main|sdk|subsdk[0-9]) ;; *) continue;; esac;; esac
  while [ "$(jobs -rp | wc -l)" -ge "$NJ" ]; do wait -n; done
  ( env -u S2A_DEPS_OUT "$PY" "$R/tools/nostall.py" "${S2A_NOSTALL_S:-600}" -- "${REWRITE[@]}" "$f" "$W" "$m" "$W/widen_sites.tsv" > "$W/$m.rewrite.log" 2>&1; echo $? > "$RC/$m" ) & MS+=("$m")
done; wait
for m in ${MS[@]+"${MS[@]}"}; do rc=$(cat "$RC/$m" 2> /dev/null || echo 9)
  if [ "$rc" = 3 ] && [ -f "$W/$m.patched.bin" ]; then R3+=("$m"); n=$((n + 1)); continue; fi
  [ "$rc" = 3 ] && { echo "【红】$G 改写 ${m}：这款有程序代码需要的改写空间超过了能放下的上限，出不了包（日志 $S2A_ROOTS/.failed/${G}_$m.log）"; mkdir -p "$S2A_ROOTS/.failed"; cp "$W/$m.rewrite.log" "$S2A_ROOTS/.failed/${G}_$m.log"; rm -rf "$RC"; root_abort "$G" "$TAG"; exit 1; }
  [ "$rc" = 0 ] || { echo "【红】$G 改写 $m 失败（rc=${rc}：$([ "$rc" = 124 ] && echo "${S2A_NOSTALL_S:-600} 秒没有进展被停" || tail -1 "$W/$m.rewrite.log" | cut -c1-160)；日志 $S2A_ROOTS/.failed/${G}_$m.log）"; mkdir -p "$S2A_ROOTS/.failed"; cp "$W/$m.rewrite.log" "$S2A_ROOTS/.failed/${G}_$m.log"; rm -rf "$RC"; root_abort "$G" "$TAG"; exit 1; }
  n=$((n + 1))
done; rm -rf "$RC"
}
rw_all 0
if [ -n "$CK" ]; then   # 社区补丁：先按不带补丁的改写点预筛出表，再带补丁行改写；补丁造成的缺口剔掉那份重来，别的缺口照旧报红
  : > "$W/cpatch_excluded.tsv"; : > "$W/cpatch_plan.log"
  for cr in 1 2 3 4 5 6; do
    "$PY" "$R/pack/cpatch.py" plan "$W" "${CSEL#none}" ${CI[@]+"${CI[@]}"} 2>> "$W/cpatch_plan.log" || { echo "【红】$G 社区补丁出表出错（$(tail -1 "$W/cpatch_plan.log")）"; root_abort "$G" "$TAG"; exit 1; }
    [ -s "$W/cpatch_rows.tsv" ] || break   # 没有要改代码的补丁（只有数据补丁或一份都没匹配上）：不带补丁那一轮的产物就是终稿
    S2A_CPATCH_TSV="$W/cpatch_rows.tsv" rw_all 1
    "$PY" "$R/pack/cpatch.py" prune "$W" 2>> "$W/cpatch_plan.log"; prc=$?
    [ "$prc" = 10 ] && continue
    [ "$prc" = 0 ] || { echo "【红】$G 社区补丁认领缺口出错"; root_abort "$G" "$TAG"; exit 1; }
    break   # 剩下的是补丁以外的缺口：降级继续，见下面 degrade.rewrite.tsv
  done
  echo "  $G 社区补丁：$(grep -c . "$W/cpatch.tsv" | awk '{print $1 - 1}') 份进包（双路 $(grep -c '^p' "$W/cpatch_rows.tsv") 字、常写 $(grep -c '^w' "$W/cpatch_rows.tsv") 字），不收 $(grep -c '^不收' "$W/cpatch_plan.log") 份（$W/cpatch_plan.log）"
fi
# 降级汇总（v0.3.1）：缺口按类别、兼容写法按处数，写 degrade.rewrite.tsv（出包作业收尾读它给玩家提示、诊断包收它）；没有降级就删掉旧表
"$PY" "$R/pack/rewrite_degrade.py" "$W" "$G" || { echo "【红】$G 写降级汇总失败"; root_abort "$G" "$TAG"; exit 1; }   # 与 Windows pack/orch/stages.py 共用（#893）
cp "$W/exefs/main.npdm" "$W/" || { root_abort "$G" "$TAG"; echo "【红】缺 main.npdm"; exit 1; }
[ -z "$AN" ] || cp "$AN" "$W/addr_need.tsv" || { root_abort "$G" "$TAG"; exit 1; }
cp "$AD/log" "$W/addr_need.log" || { root_abort "$G" "$TAG"; exit 1; }
# romfs 里 NRO 的导出表（10-25，零配置）：装载器把命中的未解析导入记成「延迟到 nn::ro 装载」、不计红（runtime/src/nrodefer.c）；没有 romfs/nro 不出表
python3 "$R/tools/nro_exports.py" "$W/romfs" "$W/nro_exports.tsv" > "$W/nro_exports.log" 2>&1 || { echo "【红】$G 读 romfs/nro 导出表失败（$(tail -1 "$W/nro_exports.log")）"; root_abort "$G" "$TAG"; exit 1; }   # 与 main.npdm 同目录：运行时窗口跨度 / 布局读它（svc_mem.c alias_gb_near）
# GPU 驱动模块名单（10-25，零配置）：NVIDIA OpenGL / Vulkan 驱动照样改写进包（通用模式要），直连模式装载器跳过名单里的（runtime/src/loader.c gpu_driver_skip）；
#   按模块自身的 NintendoSDK 组件判，与直连推导器同一函数（graphics/nvn/games/profile_guess.py gpu_driver_modules）；没有驱动模块时是空表
"$PY" "$R/graphics/nvn/games/profile_guess.py" --gpu-drivers "$W/exefs" > "$W/gpu_drivers.tsv" 2> "$W/gpu_drivers.log" || { echo "【红】$G 判 GPU 驱动模块失败（$(tail -1 "$W/gpu_drivers.log")）"; root_abort "$G" "$TAG"; exit 1; }
# 通用帧率表（10-29 60 帧泛化，引擎通用第二层）：仓库没有本构建号的表时，UE4 款按控制台变量名（t.MaxFPS / r.VSync）在改写后主程序里定位，
#   t.MaxFPS 唯一找到就写 fps.tsv（present display + w 行，r.VSync / 动态分辨率预算唯一找到才加；只碰数据、不用改指令）；不是 UE4 / 找不到 / 不唯一 → 不写（tools/fps_generic.py，退出 1）
# 社区同构建号帧率条目（10-30 grokbot S 级④）先于通用表：补丁索引（S2A_CPATCH_DIRS）里同构建号的 FPSLocker 条目是「[MAIN, 偏移, 0] + 已知虚幻 4
#   控制台变量」最简单形状才转（tools/fps_warehouse.py，只取位置事实）；不收 / 没有 → 再走通用表
fw=1; if [ -z "${FT}" ]; then S2A_CPATCH_DIRS="${S2A_CPATCH_DIRS:-}" "$PY" "$R/tools/fps_warehouse.py" "$W" "$BID" > "$W/fps_warehouse.log" 2>&1; fw=$?
  [ "$fw" = 2 ] && echo "【黄】$G 社区帧率条目读不出（$(tail -1 "$W/fps_warehouse.log")），改走通用帧率表"; fi
if [ -z "${FT}" ] && [ "$fw" != 0 ]; then "$PY" "$R/tools/fps_generic.py" "$W" "$BID" > "$W/fps_generic.log" 2>&1; fg=$?
  [ "$fg" = 2 ] && echo "【黄】$G 通用帧率表输入读不出（$(tail -1 "$W/fps_generic.log")），按原生帧率出包"; fi
# 包内能力清单不在本阶段出（10-29 出包线：拆成 pack/stage_caps.sh 轻阶段、自己的键，改判定代码不再整段重改写）
rm -rf "$W/nroexe"   # NRO 转出的中间 NSO（nro_to_nso.py），改写完删；要在收集阶段依赖之前删，免得进依赖表
K=$(stage_deps_save "$W" rewrite "$KB" "$H/stage_rewrite.sh") || { root_abort "$G" "$TAG"; exit 1; }
echo "$K" > "$W/.stage_rewrite"
ROOT_REPLAY='degrade.rewrite.tsv *.code_patch.tsv *.patched.bin *.fixups.tsv *.gates.tsv *.sites.tsv *.gaps.tsv *.npdm *.widen.tsv widen_sites.tsv cpatch.tsv cpatch_rows.tsv cpatch_plan.log cpatch_excluded.tsv cheat-src.*.txt fpslocker.json widen_enable*.tsv widen_verified.tsv fps.tsv nro_modules.tsv nro_to_nso.log widen_full*.tsv patch.tsv addr_need.tsv addr_need.log nro_exports.tsv nro_exports.log gpu_drivers.tsv widen_scan.log fps_generic.log *.rewrite.log .stage_rewrite .stage_rewrite.deps' root_commit "$G" "$TAG" || exit 1   # 与系统数据 / 界面布局并行（pack/refresh.sh）：本阶段只写这些
# 帧率表命中（情报 10-28：构建号对不上时增强静默失效）：没有本构建号的表要响亮；本款别的构建号有表（docs/名单_60帧/ 有登记、runtime/fps/ 有文件）就列出来——
#   换了版本一眼看出。原生就 60 帧的款（0100152000022000）没有表、也不列
FV=$(awk -F'\t' -v p="$PID" '!/^#/ && toupper($1) == p { print substr($2, 1, 16) }' "$R/docs/名单_60帧/"*.tsv 2> /dev/null | while read -r b; do [ -f "$R/runtime/fps/$b.tsv" ] && printf '%s ' "$b"; done)
FS=$([ -n "${FT}" ] && echo "帧率表 ${BID:0:16}" || { [ -f "$W/fps.tsv" ] && echo "通用帧率表（UE4 控制台变量按名定位）"; } || echo "无帧率表${FV:+（本构建号 ${BID:0:16} 没有表，本款有表的构建号 ${FV% }）}")
echo "🟢 $G 改写 $n 个模块（$((SECONDS - t0)) 秒；宽屏候选 $(grep -c . "$(root_cur "$G")/widen_sites.tsv") 行，启用表 $([ ${#EN[@]} -gt 0 ] && echo "$(for t in "${EN[@]}"; do t=${t##*/widen_enable.}; printf '%s ' "${t%.tsv}"; done)" || echo 无＝不加宽)；$([ -n "$VR" ] && echo 宽屏已验证 || echo 不在宽屏已验证名单)；${FS}；包内能力清单 $(awk -F'\t' '$1 == "widen" {w = $2} $1 == "fps60" {f = $2} END {printf "满屏 %s / 60 帧 %s", (w == 1 ? "开" : "关"), (f == 1 ? "开" : "关")}' "$(root_cur "$G")/s2a_caps.tsv")（实际开关以清单为准，原因见 s2a_caps.tsv）），现行 → $(basename "$(root_cur "$G")")"
