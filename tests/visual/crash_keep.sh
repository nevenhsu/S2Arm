#!/bin/bash
# 回放器崩溃留证（11-05 不稳定项 15：10-26 宽屏门禁 witcher3 放宽回放退出 138 = SIGBUS，崩溃报告没留下、定不了因）。
# source 后调：crash_keep <标签> <开局时刻（epoch 秒）> <进程名> <日志…>
#   退出码由调用方判（≥ 128 且 ≠ 124 才调：被信号打死；124 是 timeout 到点，不是崩溃）。
#   做的事：等系统写崩溃报告（ReportCrash 异步，最多 CRASH_KEEP_WAIT 秒，缺省 15），收开局之后新出的 <进程名>*.ips，连同日志拷进证据目录，打印目录。
#   报告从哪找：系统 /Library/Logs/DiagnosticReports + 真实用户家目录的 Library/Logs/DiagnosticReports。派 gate-host 的门禁把 HOME 改成了远端专用目录
#     （维护方工具 export HOME=$RRA/home），报告却写在真实用户家目录下——按 HOME 找必然找不到（10-26 只查了本机，也是没找到的原因之一）。
#     单测用 CRASH_DIAG_DIRS（冒号分隔）换成假目录。
#   证据放哪：派 gate-host（GATE_OUT 非空，tools/gate.sh 的结果目录，随 维护方工具 --pull 拉回本机）→ <仓库>/$GATE_OUT/crash/<标签>/；
#     本机 → ${S2A_RUN_ROOT:-~/.s2arm/run}/s2arm/evidence/gate_crash/<月日-时分秒>_<标签>/（本机门禁的结果目录是临时的、跑完就删）。CRASH_KEEP_DIR 显式给了就用它
crash_keep() {
  local tag=$1 t0=$2 proc=$3; shift 3
  local real_home dirs d dest n=0 waited=0 f
  real_home=; [ "$(uname)" = Darwin ] && real_home=$(dscl . -read "/Users/$(id -un)" NFSHomeDirectory 2> /dev/null | awk '{print $2}')
  [ -n "$real_home" ] || real_home=$(getent passwd "$(id -un)" 2> /dev/null | cut -d: -f6); [ -n "$real_home" ] || real_home=$HOME
  dirs=${CRASH_DIAG_DIRS:-/Library/Logs/DiagnosticReports:$real_home/Library/Logs/DiagnosticReports}
  if [ -n "${CRASH_KEEP_DIR:-}" ]; then dest=$CRASH_KEEP_DIR/$tag
  elif [ -n "${GATE_OUT:-}" ]; then dest=$(git rev-parse --show-toplevel 2> /dev/null || pwd)/$GATE_OUT/crash/$tag
  else dest=${S2A_RUN_ROOT:-$HOME/.s2arm/run}/s2arm/evidence/gate_crash/$(date +%m%d-%H%M%S)_$tag; fi
  mkdir -p "$dest" || { echo "    | 崩溃证据：建不了目录 $dest"; return 1; }
  for f in "$@"; do [ -f "$f" ] && cp "$f" "$dest/" 2> /dev/null; done
  while :; do
    n=0
    local IFS=:; for d in $dirs; do
      [ -d "$d" ] || continue
      for f in "$d"/"$proc"*.ips "$d"/"$proc"*.crash; do
        [ -f "$f" ] || continue
        [ "$(stat -c %Y "$f" 2> /dev/null || stat -f %m "$f" 2> /dev/null || echo 0)" -ge "$t0" ] || continue   # BSD / GNU 两种取修改时刻
        cp "$f" "$dest/" 2> /dev/null && n=$((n + 1))
      done
    done; unset IFS
    [ "$n" -gt 0 ] || [ "$waited" -ge "${CRASH_KEEP_WAIT:-15}" ] && break
    sleep 1; waited=$((waited + 1))
  done
  echo "    | 崩溃证据：${dest}（崩溃报告 ${n} 份$([ "$n" -gt 0 ] || echo "：等了 ${waited} 秒没等到 ${proc}*.ips，查过 $dirs")）"
}
