#!/bin/bash
# 门禁只看：tools/env_floor.sh tools/gate.sh android/test.sh pack/jobs/toolchain.py
# 测试 / 门禁入口的环境下限（tools/env_floor.sh）：错环境（macOS 自带 bash 3.2 / python 3.9）跑出的红是假红，入口直接报「环境不对」退出。约 2 秒。
# 期望（按规则手写；下限从 pack/jobs/toolchain.py 的 PYTHON_MINIMUM 现读，记作 N）：
#   ① 假解释器报 3.9.6 → s2a_env_floor 返回 2、stderr 有「环境不对：要 Python N+，拿到 3.9.6」；② 报 N.0 → 返回 0、无输出；③ 跑不起来的路径 → 返回 2、「跑不起来」
#   ④ 多个解释器有一个低 → 返回 2；⑤ 真仓库的 tools/gate.sh 在 S2A_PY=假 3.9 下退出 2、输出「环境不对」、没有开跑任何步（无「门禁」结论行）
#   ⑥ android/test.sh 在 S2A_PY=假 3.9 下退出 2、「环境不对」；⑦ 当前 bash 是 5+ 时 bash 检查放行（③⑤⑥ 只靠 Python 那条退出）
# 反证：去掉 gate.sh / android/test.sh 里那两行，⑤ / ⑥ 应红（照常开跑）
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
N=$(sed -n 's/^PYTHON_MINIMUM = (\([0-9][0-9]*\), *\([0-9][0-9]*\)).*/\1.\2/p' "$H/pack/jobs/toolchain.py")
ck "读出下限 PYTHON_MINIMUM=${N}" '[ -n "$N" ]'
fake() { printf '#!/bin/sh\ncase "$*" in *exit*) exit %s;; *) echo %s;; esac\n' "$2" "$3" > "$T/$1"; chmod +x "$T/$1"; }
fake py39 1 3.9.6; fake pyok 0 "$N.0"
. "$H/tools/env_floor.sh"
e=$(s2a_env_floor "$H" "$T/py39" 2>&1); r=$?
ck "① 3.9 返回 2（${r}）且报要 ${N}+ 拿到 3.9.6" '[ $r = 2 ] && [[ $e == *"环境不对：要 Python ${N}+，拿到 3.9.6"* ]]'
e=$(s2a_env_floor "$H" "$T/pyok" 2>&1); r=$?
ck "② ${N}.0 返回 0 无输出（${r}）" '[ $r = 0 ] && [ -z "$e" ]'
e=$(s2a_env_floor "$H" "$T/nope" 2>&1); r=$?
ck "③ 跑不起来返回 2（${r}）" '[ $r = 2 ] && [[ $e == *跑不起来* ]]'
e=$(s2a_env_floor "$H" "$T/pyok" "$T/py39" 2>&1); r=$?
ck "④ 一个低就返回 2（${r}）" '[ $r = 2 ]'
ck "⑦ 当前 bash ${BASH_VERSINFO[0]} 放行" '[ "${BASH_VERSINFO[0]}" -ge 5 ]'
o=$(cd "$H" && S2A_PY="$T/py39" GATE_LIST=1 bash tools/gate.sh 2>&1); r=$?
ck "⑤ 真仓库 gate.sh 退 2（${r}）、报环境不对、没开跑" '[ $r = 2 ] && [[ $o == *环境不对* ]] && [[ $o != *门禁通过* ]] && [ $(printf "%s\n" "$o" | grep -c .) -le 2 ]'
o=$(cd "$H" && S2A_PY="$T/py39" bash android/test.sh 2>&1); r=$?
ck "⑥ android/test.sh 退 2（${r}）、报环境不对" '[ $r = 2 ] && [[ $o == *环境不对* ]]'
[ $bad = 0 ] && echo "env_floor 单测全过" || echo "env_floor 单测有不通"
exit $bad
