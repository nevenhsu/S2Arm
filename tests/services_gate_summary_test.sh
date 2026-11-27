#!/bin/bash
# services/gate.sh 门禁摘要带专项射程（11-27 维护者：原先只汇总主测试「共 N 项」，加了 [SoloTest] 专项数字不变，报绿看不出专项跑了几个）。
#   做法：临时根里放生产 services/gate.sh 原件 + 假 services/test.sh（按用例吐固定输出、按用例退出），跑 gate.sh，核最后一行摘要。期望按格式手写：
#   ① 专项 2 个全过、主测试共 660 项 → 「主测试 共 660 项，不符 0 项；专项 2 个全部通过」，退出 0
#   ② 专项 3 个、--b 失败（退出 7）→ 主测试没跑：「主测试 没跑（专项先失败）；专项 3 个，失败 1 个：--b」，退出 7
#   ③ 编译就失败（什么汇总都没有，退出 1）→ 两段都写「没有汇总行（编译或运行失败）」，退出 1
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d "${TMPDIR:-/tmp}/svc_gate_sum.XXXXXX"); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/services"; cp "$ROOT/services/gate.sh" "$T/services/gate.sh"
fails=0
case_run() {   # case_run <名> <假 test.sh 的输出> <假退出码> <期望摘要行> <期望退出码>
  printf '#!/bin/bash\nprintf %%s %q\nexit %s\n' "$2" "$3" > "$T/services/test.sh"
  local got rc; got=$(bash "$T/services/gate.sh" 2>&1); rc=$?; got=$(printf '%s\n' "$got" | tail -1)
  if [ "$got" = "$4" ] && [ "$rc" = "$5" ]; then echo "✓ $1"; else echo "✗ ${1}：期望「${4}」退出 ${5} → 真结果「${got}」退出 ${rc}"; fails=$((fails + 1)); fi
}
NL=$'\n'
case_run "① 专项全过 + 主测试汇总" "服务专项 --a：通过${NL}服务专项 --b：通过${NL}服务专项汇总：2 个全部通过${NL}共 660 项，不符 0 项${NL}" 0 \
  "服务层单测：期望 全部符合 → 真结果 主测试 共 660 项，不符 0 项；专项 2 个全部通过" 0
case_run "② 专项有失败、主测试没跑" "服务专项 --a：通过${NL}服务专项 --b：失败（退出 7）${NL}服务专项 --c：通过${NL}服务专项汇总：3 个，失败 1 个：--b${NL}" 7 \
  "服务层单测：期望 全部符合 → 真结果 主测试 没跑（专项先失败）；专项 3 个，失败 1 个：--b" 7
case_run "③ 编译失败没有任何汇总" "error CS1002: ; expected${NL}" 1 \
  "服务层单测：期望 全部符合 → 真结果 主测试 没有汇总行（编译或运行失败）；专项 没有汇总行（编译或运行失败）" 1
[ $fails = 0 ] && echo "✓ services_gate_summary：3 例全过" || { echo "✗ services_gate_summary：$fails 例不符"; exit 1; }
