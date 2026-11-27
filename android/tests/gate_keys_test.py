#!/usr/bin/env python3
"""安卓门禁子项键单测（android/tools/gate_keys.tsv / gate_keys.py / gate_step.sh 的 GATE_ONLY）：
  ① 真 test.sh：每步恰好一个键、没有陈旧规则、生产 / 消费 $out 产物同键、9 键都有步；
  ② gate_step.sh 的 bash 归键与 gate_keys.py 逐步一致（运行时与静态枚举同一口径）；
  ③ 负对照：没登记的步、陈旧规则、产物跨键各自被 check 点名；
  ④ GATE_HOOK_LIST=inputs 每键一行、输入 = 钩子路由；gate.sh 认不得的键红；
  ⑤ GATE_ONLY 运行时：只跑名单里的键的步、没登记的步红、认不得的键红。
在仓库根跑。"""
import os, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
os.chdir(ROOT)
sys.path.insert(0, os.path.join(ROOT, 'android', 'tools'))
import gate_keys as g  # noqa: E402

bad = []
# 门禁按键跑时会把这些导出给钩子（GATE_HOOK_ONLY=app 等），本测起的子进程不能继承：否则和本测自己设的键冲突，按键跑必红
GATE_VARS = ('GATE_ONLY', 'GATE_HOOK_ONLY', 'GATE_HOOK_LIST', 'GATE_SKIP', 'GATE_HOOK_SKIP', 'GATE_CHANGED',
             'S2A_GATE_SUMMARY_DIR', 'S2A_GATE_SUMMARY_FILE')
CLEAN = {k: v for k, v in os.environ.items() if k not in GATE_VARS}


def ok(cond, what, got=''):
    print(('✓ ' if cond else '✗ ') + what + (f'：{got}' if got and not cond else ''))
    if not cond: bad.append(what)


# ① 真表
st, keyed, issues = g.check()
ok(not issues, f'真 test.sh {len(st)} 步每步恰好一个键、无陈旧规则、产物同键', '; '.join(issues[:3]))
ok(len(st) >= 200, f'静态枚举到的步数像样（{len(st)} ≥ 200，枚举没断）')
ok({k for _, k, _ in keyed} == set(g.KEYS), '9 个键都有步')

# ② bash 归键 = python 归键
rules = g.load_table()
plan = '\n'.join(f'{ln}\t{g.key_of(c, rules)[0]}\t{c}' for ln, c in st) + '\n'
sh = r'''
repo_dir=$PWD; out=/nonexistent-out
source android/tools/gate_step.sh
S2A_GATE_SUMMARY_FILE=/dev/null s2a_gate_keys_init android/tools/gate_keys.tsv || exit 3
n=0; d=0
while IFS=$'\t' read -r ln k c; do
  s2a_gate_key_of "$c" || S2A_GK_KEY=
  [ "$S2A_GK_KEY" = "$k" ] || { echo "不一致 test.sh:$ln py=$k sh=$S2A_GK_KEY"; d=$((d+1)); }
  n=$((n+1))
done
echo "N=$n D=$d"
'''
r = subprocess.run(['bash', '-c', sh], input=plan, capture_output=True, text=True, env=CLEAN)
ok(r.returncode == 0 and f'N={len(st)} D=0' in r.stdout, f'gate_step.sh 归键与 gate_keys.py 逐步一致（{len(st)} 步）', (r.stdout + r.stderr)[-300:])
# 运行时命令带绝对仓库根、真 $out 目录、多行 bash -c：归一化后同键
sh2 = r'''
repo_dir=$PWD; out=/tmp/s2a-android-host-tests.Ab12
source android/tools/gate_step.sh
S2A_GATE_SUMMARY_FILE=/dev/null s2a_gate_keys_init android/tools/gate_keys.tsv || exit 3
s2a_gate_key_of /venv/bin/python3 "$repo_dir/android/tests/nvk_shader_pool_test.py" --withdraw; a=$S2A_GK_KEY
s2a_gate_key_of /venv/bin/python3 "$repo_dir/android/tests/nvk_shader_pool_test.py" --lp-output; b=$S2A_GK_KEY
s2a_gate_key_of bash -c '
    mkdir -p "'"$out"'/fakeapk/lib/arm64-v8a"'; c=$S2A_GK_KEY
echo "$a $b $c"
'''
r = subprocess.run(['bash', '-c', sh2], capture_output=True, text=True, env=CLEAN)
ok(r.stdout.strip() == 'shader-withdraw shader-modes runtime-host', '运行时绝对路径 / $out / 多行命令归一化后归键正确', r.stdout + r.stderr)

# ③ 负对照
print('【负对照开始】')
with tempfile.TemporaryDirectory() as t:
    ts, tsv = os.path.join(t, 'test.sh'), os.path.join(t, 'keys.tsv')
    open(ts, 'w').write('s2a_gate_step "$PY" "$android_dir/tests/a_test.py"\n'
                        's2a_gate_step cc x.c -o "$out/bin-x"\n'
                        's2a_gate_step "$out/bin-x"\n'
                        's2a_gate_step "$PY" "$android_dir/tests/new_test.py"\n')
    open(tsv, 'w').write('rule\tdq\tandroid/tests/a_test.py\nrule\tapp\tx.c -o\nrule\treplay\t$out/bin-x\nrule\tshader-misc\tandroid/tests/gone_test.py\n')
    _, _, iss = g.check(tsv, ts)
    txt = '\n'.join(iss)
    ok('没有规则中' in txt and 'new_test.py' in txt, '负对照：没登记的步被点名', txt)
    ok('gone_test.py' in txt and '一步都没中' in txt, '负对照：陈旧规则被点名', txt)
    ok('生产它的步在 app' in txt, '负对照：$out 产物生产步与消费步跨键被点名', txt)
print('【负对照结束】')

# ④ GATE_HOOK_LIST=inputs：9 键各一行，触发前缀 = 钩子路由（门禁按子步算输入）
r = subprocess.run(['bash', 'android/gate.sh'], capture_output=True, text=True, env=dict(CLEAN, GATE_HOOK_LIST='inputs'))
route = ' '.join(l.split('：', 1)[1].strip() for l in open('android/gate.sh', encoding='utf-8') if l.startswith('# 门禁路由：'))
rows = [l.split('\t') for l in r.stdout.strip().split('\n')]
ok(r.returncode == 0 and [x[0] for x in rows] == list(g.KEYS) and all(x[1] == route.strip() for x in rows),
   f'GATE_HOOK_LIST=inputs：9 键各一行、输入 = 钩子路由（{route.strip()}）', r.stdout + r.stderr)
r = subprocess.run(['bash', 'android/gate.sh'], capture_output=True, text=True, env=dict(CLEAN, GATE_ONLY='no-such-key'))
ok(r.returncode != 0 and '认不得' in r.stderr, 'gate.sh：认不得的 GATE_ONLY 键 → 红', r.stdout + r.stderr)

# ⑤ GATE_ONLY 运行时（没登记的步 / 认不得的键按设计报红：反证段）
print('【负对照开始】')
fix = r'''
set -e
repo_dir=$PWD; out=$(mktemp -d)
source android/tools/gate_step.sh
s2a_gate_keys_init "$1" || exit 7
s2a_gate_step echo RAN-A android/tests/run_dq_queue.sh
s2a_gate_step echo RAN-B android/tests/PlaytimeTest-fixture
s2a_gate_step echo RAN-C "$2"
echo "RUN=$S2A_GK_RUN SKIP=$S2A_GK_SKIP"
'''
with tempfile.TemporaryDirectory() as t:
    tsv = os.path.join(t, 'k.tsv')
    open(tsv, 'w').write('rule\tdq\tandroid/tests/run_dq_queue.sh\nrule\tapp\tPlaytimeTest-fixture\nrule\tapp\tregistered-c\n')
    env = dict(CLEAN, GATE_ONLY='dq')
    env.pop('S2A_GATE_SUMMARY_FILE', None)
    r = subprocess.run(['bash', '-c', fix, 'x', tsv, 'registered-c'], capture_output=True, text=True, env=env)
    ok(r.returncode == 0 and 'RAN-A' in r.stdout and 'RAN-B' not in r.stdout and 'RAN-C' not in r.stdout and 'RUN=1 SKIP=2' in r.stdout,
       'GATE_ONLY=dq 只跑 dq 的步（1 跑 2 跳）', r.stdout + r.stderr)
    r = subprocess.run(['bash', '-c', fix, 'x', tsv, 'stray-step'], capture_output=True, text=True, env=env)
    ok(r.returncode != 0 and '没有登记子项键' in r.stderr and 'RAN-C' not in r.stdout, '没登记的步 → 红、不跑', r.stdout + r.stderr)
    env['GATE_ONLY'] = 'no-such-key'
    r = subprocess.run(['bash', '-c', fix, 'x', tsv, 'registered-c'], capture_output=True, text=True, env=env)
    ok(r.returncode == 7 and '认不得' in r.stderr and 'RAN-' not in r.stdout, '认不得的 GATE_ONLY 键 → 红、一步不跑', r.stdout + r.stderr)
    env.pop('GATE_ONLY')
    r = subprocess.run(['bash', '-c', fix, 'x', tsv, 'stray-step'], capture_output=True, text=True, env=env)
    ok(r.returncode == 0 and r.stdout.count('RAN-') == 3, '不设 GATE_ONLY、不在门禁里（直接跑 test.sh）：不归键、全部照跑', r.stdout + r.stderr)

print('【负对照结束】')
print(f'期望 全部符合 → 真结果 {"全部符合" if not bad else f"{len(bad)} 项不符：" + "；".join(bad)}')
sys.exit(1 if bad else 0)
