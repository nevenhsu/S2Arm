#!/bin/bash
# 门禁只看：tools/py_min_syntax_lint.py pack/jobs/toolchain.py *.py
# Python 最低版本语法守卫单测（tools/py_min_syntax_lint.py，约 2 秒）：bash tests/py_min_syntax_lint_test.sh
# 期望（按 3.12 之前 f-string 的规则手写）：① 本仓库 git 跟踪的 .py 0 处；② 坏写法夹具 5 行各报 1 处（复用外层双引号的嵌套 f-string、
#   表达式里复用外层单引号的字符串、表达式里的反斜杠、表达式里的 # 注释、单引号 f-string 的表达式跨行）；③ 好写法夹具（换引号、三引号里用单引号、
#   反斜杠在字面部分、三引号 f-string 的表达式跨行）不报；④ 下限读的是 toolchain.py：改成 (3, 12) 后坏夹具也不报
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
PY=python3; "$PY" -c 'import sys; sys.exit(sys.version_info < (3, 12))' || { echo "🔴 要 Python 3.12 以上跑（tokenize 才分得出 f-string 内部）"; exit 1; }
o=$("$PY" "$H/tools/py_min_syntax_lint.py" "$H" 2>&1); r=$?
ck "① 本仓库没有作业层最低版本不认的 f-string 写法（命中：${o:0:300}）" '[ $r = 0 ] && [ -z "$o" ]'
mkdir -p "$T/r/pack/jobs"
printf 'PYTHON_MINIMUM = (3, 11)\n' > "$T/r/pack/jobs/toolchain.py"
cat > "$T/r/bad.py" <<'X'
a = f"x{f"y{1}" if 1 else ""}"
b = f'x{d.get('k')}'
c = f'x{"\n".join(l)}'
d = f"x{1 # n
}"
e = f"x{1 +
2}"
X
cat > "$T/r/good.py" <<'X'
a = f"x{f'y{1}' if 1 else ''}"
b = f'x{d.get("k")}'
c = f"""x{d.get("k")}"""
n = "\n"; d = f'x\n{n.join(l)}'
e = f"""x{1 +
2}"""
X
o=$("$PY" "$H/tools/py_min_syntax_lint.py" "$T/r" bad.py good.py 2>&1); r=$?
ck "② 坏写法 5 处各报 1 行（实际：$(echo "$o" | tr '\n' ' ')）" '[ $r = 1 ] && [ "$(echo "$o" | grep -c "^bad.py:")" = 5 ]'
ck "② 命中行号 1 2 3 4 6（复用双引号 / 复用单引号 / 反斜杠 / 注释 / 跨行）" '[ "$(echo "$o" | grep "^bad.py:" | cut -d: -f2 | tr "\n" " ")" = "1 2 3 4 6 " ]'
ck "③ 好写法不报" '! echo "$o" | grep -q "^good.py"'
printf 'PYTHON_MINIMUM = (3, 12)\n' > "$T/r/pack/jobs/toolchain.py"
o=$("$PY" "$H/tools/py_min_syntax_lint.py" "$T/r" bad.py 2>&1); r=$?
ck "④ 下限改成 3.12：不查、坏夹具也不报（实际 rc=${r}：${o:0:200}）" '[ $r = 0 ] && [ -z "$o" ]'
[ $bad = 0 ] && echo "✅ Python 最低版本语法守卫全通" || { echo "🔴 Python 最低版本语法守卫有不通"; exit 1; }
