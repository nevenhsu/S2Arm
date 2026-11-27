#!/bin/bash
# 门禁覆盖：*.py
# 全仓 Python 未定义名（ruff F821 / F822 / F823，约 2 秒）：bash tests/py_undefined_names_test.sh
# 玩家报的 NameError（pack/jobs/build.py 用 tempfile 没 import，已解包目录出包必现）：只在该分支执行时才炸，门禁得静态扫。
# ruff 版本钉在 tools/style-tools.json；用 S2A_RUFF > S2A_PY 同目录的 ruff > PATH 上的 ruff > uvx。Python 语法版本按 ruff.toml（= 作业层下限 3.12）
set -eu
cd "$(dirname "$0")/.."
VER=$(python3 -c "import json; print(json.load(open('tools/style-tools.json'))['python_packages']['ruff'])")
if [ -n "${S2A_RUFF:-}" ]; then RUFF=("$S2A_RUFF")
elif [ -n "${S2A_PY:-}" ] && [ -x "$(dirname "$S2A_PY")/ruff" ]; then RUFF=("$(dirname "$S2A_PY")/ruff")   # 门禁机钉版 venv（tools/remote.sh 给 S2A_PY）
elif command -v ruff > /dev/null; then RUFF=(ruff)
elif command -v uvx > /dev/null; then RUFF=(uvx "ruff==$VER")
else echo "【红】没有 ruff：pip install ruff==${VER}，或设 S2A_RUFF 指向它"; exit 1; fi
check() { "${RUFF[@]}" check --no-cache --config ruff.toml --select F821,F822,F823 --per-file-ignores tests/fake_app.py:F821 --per-file-ignores tests/quality_check_test.py:F821 --output-format concise "$@"; }
# 负对照：夹具里的未定义名必须被报出来，判据没失效
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
printf 'def f():\n    return tempfile.mkdtemp()\n' > "$D/bad.py"
if check "$D/bad.py" > "$D/out" 2>&1 || ! grep -q F821 "$D/out"; then echo "【红】负对照没报 F821，判据失效"; cat "$D/out"; exit 1; fi
check .
echo "🟢 未定义名 0 处（负对照已报红）"
