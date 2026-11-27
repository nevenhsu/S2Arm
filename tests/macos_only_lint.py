#!/usr/bin/env python3
# macOS 专有命令用法检查（shell_lint_test.sh 调用）：cp -c（APFS 克隆）、stat -f（BSD 格式串）、sed -i ''（BSD 原地改）在 Linux 上
#   要么不认、要么语义不同。贡献者在 Linux 上跑一键出包 / 单测时才暴露（11-01：oneclick.sh 交付拷贝 cp -c / cp -cR，#1042 贴出 5 条不通）。
# 合格写法（同一行）：带退回（`if ! cp -c …; then cp …` 或 `cp -c … 2> /dev/null || cp …`），或写在 Darwin 判断里（行里有 Darwin）。
#   stat -f 例外：退回只在 GNU 写法在前时才算（`stat -c … 2> /dev/null || stat -f …`），见 line_issues
# 只在 Mac 上跑的存量脚本按文件登记在 tests/macos_only_ok.tsv（路径<TAB>理由）；登记了却已没有这类用法 → 报过期。
import os
import re
import subprocess
import sys

STAT_F = re.compile(r"\bstat\s+-f\b")
PATTERNS = (
    (re.compile(r"\bcp\s+-[A-Za-z]*c[A-Za-z]*\b"), "cp -c（APFS 克隆）"),
    (STAT_F, "stat -f（BSD 格式串，GNU 是文件系统信息、失败前先往标准输出打一段；退回要 GNU 写法在前：stat -c … 2> /dev/null || stat -f …）"),
    (re.compile(r"""\bsed\s+-i\s*(''|"")"""), "sed -i ''（BSD 原地改，GNU 把 '' 当文件）"),
)
NO_FALLBACK = r"(?:die|exit|return|fail|echo|true|:)\b"
FALLBACK = re.compile(r"\bif\s+!\s|2>\s*/dev/null\s*\|\|\s*(?!" + NO_FALLBACK + r"|\{\s*" + NO_FALLBACK + r")\S")


def line_issues(line):
    s = line.lstrip()
    if s.startswith("#"):
        return []
    code = line.split(" # ", 1)[0]
    hits = [name for pat, name in PATTERNS if pat.search(code)]
    if not hits or "Darwin" in code:
        return []
    fb = bool(FALLBACK.search(code))
    out = []
    for pat, name in PATTERNS:
        if name not in hits:
            continue
        if pat is STAT_F:
            # stat -f 的退回只在 GNU 写法在前时才算（11-26：GNU 的 stat -f 先往标准输出打一整段文件系统信息再退 1，
            #   $(stat -f %m f 2> /dev/null || stat -c %Y f) 拿到「文件系统信息 + 修改时间」，前后两次永远不等，gate_route_test 在 Linux 上因此红）。
            #   合格：stat -c … 2> /dev/null || stat -f …（BSD 的 stat 不认 -c、不打标准输出，退回干净）
            if not (fb and re.search(r"\bstat\s+-c\b", code[:pat.search(code).start()])):
                out.append(name)
        elif not fb:
            out.append(name)
    return out


def lint_text(text):
    return [(i, name) for i, l in enumerate(text.split("\n"), 1) for name in line_issues(l)]


def allowlist(root):
    p = os.path.join(root, "tests/macos_only_ok.tsv")
    out = {}
    for l in open(p, encoding="utf-8").read().splitlines() if os.path.exists(p) else []:
        if l.strip() and not l.startswith("#"):
            path, _, why = l.partition("\t")
            out[path.strip()] = why.strip()
    return out


def lint_repo(root):
    files = subprocess.run(["git", "-C", root, "ls-files", "-z", "*.sh"], capture_output=True).stdout.decode().split("\0")
    ok = allowlist(root)
    problems, used = [], set()
    for f in sorted(x for x in files if x and not x.startswith("compat/tree/")):
        issues = lint_text(open(os.path.join(root, f), encoding="utf-8", errors="replace").read())
        if not issues:
            continue
        if f in ok:
            used.add(f)
            continue
        problems += [f"{f}:{i}: {name} 没有退回（Linux 上不认 / 语义不同）：改成 `if ! … ; then <可移植写法>; fi`，"
                     "或放进 Darwin 判断；脚本确实只在 Mac 跑就登记进 tests/macos_only_ok.tsv 并写理由" for i, name in issues]
    problems += [f"tests/macos_only_ok.tsv: {f} 已没有 macOS 专有用法，从白名单删掉（过期）" for f in sorted(set(ok) - used)]
    return problems, len(ok)


if __name__ == "__main__":
    probs, n = lint_repo(sys.argv[1] if len(sys.argv) > 1 else os.getcwd())
    for p in probs:
        print(p)
    print(f"macOS 专有用法检查：问题 {len(probs)} 处（白名单 {n} 个只在 Mac 跑的存量脚本）", file=sys.stderr)
    sys.exit(1 if probs else 0)
