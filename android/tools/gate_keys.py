#!/usr/bin/env python3
"""安卓门禁子项键：android/test.sh 的每个 s2a_gate_step 归一个键，门禁把 9 个键拆成 9 个子步并行跑。
在仓库根跑：
  python3 android/tools/gate_keys.py keys    → 9 个键，一行一个
  python3 android/tools/gate_keys.py plan    → test.sh 行号<TAB>键<TAB>步（静态枚举）
  python3 android/tools/gate_keys.py check   → 自检：每步恰好一个键、没有陈旧规则、生产 / 消费 $out 产物同键、9 键都有步
运行时归键在 android/tools/gate_step.sh（s2a_gate_key_of），与这里的 key_of 同一口径，单测逐步核两边一致。
各键的输入不分开声明，都是整个钩子路由（android/gate.sh「# 门禁路由：」）：11-26 Linux 门禁机按文件访问时间实测各键读到的仓库文件，
  文本闭包要补齐 test.sh 步外代码、host_link 按通配补链、cmake 路径模板、MSBuild 自动读的根配置等才不漏，补齐后公共前置的闭包
  已覆盖 android/ 1649 个文件中的 1635 个——不漏的声明下任何 android/ 改动都是 9 键全跑，按键挑输入省不下东西，只增加漏跑风险。"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, 'gate_keys.tsv')
TEST_SH = 'android/test.sh'
KEYS = ('shader-withdraw', 'shader-modes', 'shader-misc', 'dq', 'runtime-host', 'runtime-core', 'replay', 'link-compiler', 'app')


def load_table(path=TABLE):
    rules = []
    for n, l in enumerate(open(path, encoding='utf-8'), 1):
        l = l.rstrip('\n')
        if not l.strip() or l.startswith('#'): continue
        a = l.split('\t')
        if a[0] == 'rule' and len(a) == 3 and a[1] in KEYS and a[2]: rules.append((a[1], a[2], n))
        else: sys.exit(f'【红】gate_keys.tsv:{n} 认不得：{l!r}（rule<TAB>键<TAB>片段；键只认 {" ".join(KEYS)}）')
    return rules


def _open_quote(s):
    """s 末尾是否还在引号里（单引号里不认转义、双引号里认 \\）"""
    q, i = None, 0
    while i < len(s):
        c = s[i]
        if q is None:
            if c == '#' and (i == 0 or s[i - 1].isspace()): return False   # 行内注释
            if c == '\\': i += 2; continue
            if c in '"\'': q = c
        elif q == '"' and c == '\\': i += 2; continue
        elif c == q: q = None
        i += 1
    return q is not None


def steps(path=TEST_SH):
    """test.sh 里静态写出的每个 s2a_gate_step：(行号, 归一化命令)。续行与跨行引号拼起来、整行注释与 here 文档正文跳过、引号去掉、
    $android_dir/ → android/、$repo_dir/ → 仓库根相对；与 gate_step.sh 运行时的归一化（绝对仓库根前缀去掉、$out 写回字面）对得上"""
    L = open(path, encoding='utf-8').read().split('\n')
    out, buf, start, heredoc = [], '', 0, None
    for i, l in enumerate(L, 1):
        if heredoc is not None:
            if l.strip() == heredoc: heredoc = None
            continue
        if not buf: start = i
        buf = (buf + ' ' + l.strip()) if buf else l
        if buf.endswith('\\'): buf = buf[:-1]; continue
        if _open_quote(buf): continue   # 引号跨行（bash -c '…多行…'）：接着拼
        s = buf.strip(); buf = ''
        if s.startswith('#'): continue
        m = re.search(r"<<-?\s*['\"]?(\w+)['\"]?", s)
        if m: heredoc = m.group(1)
        for m in re.finditer(r's2a_gate_step\s+(.*)', s):
            t = m.group(1).replace('"', '').replace("'", '')
            t = re.sub(r'\$\{?android_dir\}?/', 'android/', t)
            t = re.sub(r'\$\{?repo_dir\}?/', '', t)
            t = re.sub(r'\s*;;\s*$', '', t)
            out.append((start, ' '.join(t.split())))
    return out


def key_of(cmd, rules):
    for k, frag, n in rules:
        if frag in cmd: return k, n
    return None, None


PROD = re.compile(r'(?:-o|-d|--out|-B|mkdir -p)\s+(\$out/[\w.-]+)')
USE = re.compile(r'\$out/[\w.-]+')


def check(tsv=TABLE, test_sh=TEST_SH):
    rules = load_table(tsv)
    bad, used, keyed = [], set(), []
    st = steps(test_sh)
    for ln, cmd in st:
        k, n = key_of(cmd, rules)
        if not k: bad.append(f'test.sh:{ln} 没有规则中：{cmd[:120]}')
        else: used.add(n); keyed.append((ln, k, cmd))
    for k, frag, n in rules:
        if n not in used: bad.append(f'gate_keys.tsv:{n} 规则「{frag}」（{k}）一步都没中')   # 被更前面的规则全吃掉也算
    have = {k for _, k, _ in keyed}
    bad += [f'键 {k} 没有任何步' for k in KEYS if k not in have]
    prod = {}
    for ln, k, cmd in keyed:
        for m in PROD.finditer(cmd): prod.setdefault(m.group(1), set()).add(k)
    for ln, k, cmd in keyed:
        for u in set(USE.findall(cmd)):
            u2 = u
            while u2 not in prod and '-' in u2: u2 = u2.rsplit('-', 1)[0]   # $out/gs-path-test-debug ← -o $out/gs-path-test-debug 一类
            pk = prod.get(u) or prod.get(u2)
            if pk and k not in pk:
                bad.append(f'test.sh:{ln}（{k}）用到 {u}，生产它的步在 {"/".join(sorted(pk))}：只跑 {k} 时前置产物缺')
    return st, keyed, bad


def main(argv):
    cmd = argv[0] if argv else ''
    if cmd == 'keys':
        print('\n'.join(KEYS)); return 0
    if cmd == 'plan':
        rules = load_table()
        for ln, c in steps(): print(f'{ln}\t{key_of(c, rules)[0] or "?"}\t{c}')
        return 0
    if cmd == 'check':
        st, keyed, bad = check()
        for b in bad: print('【红】' + b)
        n = {k: sum(1 for _, kk, _ in keyed if kk == k) for k in KEYS}
        print(f'期望 {len(st)} 步每步恰好一个键、规则无陈旧、产物同键 → 真结果 {"全符" if not bad else f"{len(bad)} 处不符"}（'
              + ' '.join(f'{k}={n[k]}' for k in KEYS) + '）')
        return 1 if bad else 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
