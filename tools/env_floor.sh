# 测试 / 门禁入口的环境下限：低于项目下限直接报「环境不对：要 X，拿到 Y」退出，不跑用例（错环境跑出的红是假红）。
#   用法：. tools/env_floor.sh; s2a_env_floor <仓库根> [解释器…]（缺省只核 PATH 上的 python3）；不够 → 打一行到 stderr、返回 2。
#   下限：bash 5（门禁 / 列车按 bash 5 写和测，同 tools/doctor.sh）；Python = pack/jobs/toolchain.py 的 PYTHON_MINIMUM（只此一处，这里现读不另抄）。
#   本文件要能被 macOS 自带的 bash 3.2 解析到报错那一步：只用 3.2 也认的写法。
s2a_env_floor() {
    local root=$1 py need got
    shift
    if [ "${BASH_VERSINFO[0]:-0}" -lt 5 ]; then
        echo "🔴 环境不对：要 bash 5+，拿到 ${BASH_VERSION:-?}（${BASH:-bash}）。macOS 自带的是 3.2：brew install bash，用 /opt/homebrew/bin/bash 跑" >&2
        return 2
    fi
    need=$(sed -n 's/^PYTHON_MINIMUM = (\([0-9][0-9]*\), *\([0-9][0-9]*\)).*/\1.\2/p' "$root/pack/jobs/toolchain.py" 2> /dev/null)
    [ -n "$need" ] || { echo "🔴 环境下限：读不出 $root/pack/jobs/toolchain.py 的 PYTHON_MINIMUM" >&2; return 2; }
    [ $# -gt 0 ] || set -- python3
    for py in "$@"; do
        got=$("$py" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2> /dev/null) || got=
        if [ -z "$got" ]; then
            echo "🔴 环境不对：要 Python ${need}+，拿到「${py}」跑不起来" >&2
            return 2
        fi
        if ! "$py" -c "import sys; sys.exit(0 if sys.version_info[:2] >= (${need/./, }) else 1)" 2> /dev/null; then
            echo "🔴 环境不对：要 Python ${need}+，拿到 ${got}（${py}）。macOS 自带的 /usr/bin/python3 是 3.9：用 S2A_PY 指定项目的解释器" >&2
            return 2
        fi
    done
    return 0
}
