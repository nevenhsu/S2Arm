#!/usr/bin/env python3
"""Python 最低版本语法守卫：作业层声明支持的最低版本（pack/jobs/toolchain.py PYTHON_MINIMUM）认不了的 f-string 写法，门禁上查出来报红。
起因（11-26）：graphics/tools/direct/merge_extract.py 内层 f-string 复用了外层的双引号，只有 3.12 起（PEP 701）才合法；出包时作业层用玩家机上的
Python 调它，3.11 上直接 SyntaxError、直连那一步挂掉。门禁各台都是 3.12 以上，这种写法在那上面全合法；ast.parse(feature_version=(3, 11)) 也查不出。

用法：py_min_syntax_lint.py <仓库根> [文件…]：不给文件就扫仓库里 git 跟踪的全部 .py；命中逐行打印「文件:行: 原因」，有就退 1。
- 下限 ≥ 3.12：PEP 701 的写法都合法，不查。
- 跑本工具的解释器在 [下限, 3.12) 之间：直接拿它 compile() 每个文件（那就是事实，不用推断）；比下限还低：判不了，退 2。
- 否则（门禁：3.12 以上）按 tokenize 查 3.12 之前 f-string 不允许的四种写法：表达式里的字符串 / 嵌套 f-string 复用外层 f-string 的引号、
  表达式里有反斜杠、表达式里有 # 注释、单行引号的 f-string 表达式跨行。"""
import ast, io, os, subprocess, sys, tokenize


def minimum(root):
    tree = ast.parse(open(os.path.join(root, "pack", "jobs", "toolchain.py"), encoding="utf-8").read())
    for n in ast.walk(tree):
        if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "PYTHON_MINIMUM" for t in n.targets):
            return tuple(ast.literal_eval(n.value))
    raise SystemExit("【红】pack/jobs/toolchain.py 里找不到 PYTHON_MINIMUM")


def quote(tok):   # 字符串 / f-string 开头记号的引号：三引号或单字符
    s = tok.lstrip("rRbBuUfF")
    return s[:3] if s[:3] in ('"""', "'''") else s[:1]


def problems(src):
    out = []
    try:
        toks = list(tokenize.generate_tokens(io.StringIO(src).readline))
    except (tokenize.TokenError, SyntaxError) as e:
        return [(0, f"tokenize 失败：{e}")]
    stack = []   # 开着的 f-string 的引号（最内层在末尾）
    for t in toks:
        if t.type == tokenize.FSTRING_START:
            q = quote(t.string)
            if q in stack:
                out.append((t.start[0], "嵌套 f-string 复用了外层 f-string 的引号"))
            stack.append(q)
        elif t.type == tokenize.FSTRING_END:
            if stack:
                stack.pop()
        elif not stack or t.type == tokenize.FSTRING_MIDDLE:
            continue   # 不在 f-string 里，或是 f-string 的字面部分
        elif t.type == tokenize.STRING:
            if quote(t.string) in stack:
                out.append((t.start[0], "f-string 表达式里的字符串复用了外层 f-string 的引号"))
            if "\\" in t.string:
                out.append((t.start[0], "f-string 表达式里有反斜杠"))
        elif t.type == tokenize.COMMENT:
            out.append((t.start[0], "f-string 表达式里有 # 注释"))
        elif t.type in (tokenize.NL, tokenize.NEWLINE) and len(stack[-1]) == 1:
            out.append((t.start[0], "单行引号的 f-string 表达式跨了行"))
    return out


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    low = minimum(root)
    if low >= (3, 12):
        return 0
    if sys.version_info[:2] < low:
        print(f"【红】本解释器 {sys.version_info[0]}.{sys.version_info[1]} 比作业层最低 {low[0]}.{low[1]} 还低，判不了：用 {low[0]}.{low[1]} 或 3.12 以上跑", file=sys.stderr)
        return 2
    files = sys.argv[2:] or subprocess.run(["git", "-C", root, "ls-files", "*.py"], capture_output=True, text=True, check=True).stdout.split()
    bad = 0
    for rel in files:
        path = rel if os.path.isabs(rel) else os.path.join(root, rel)
        try:
            src = open(path, encoding="utf-8").read()
        except (OSError, UnicodeDecodeError):
            continue
        if sys.version_info[:2] < (3, 12):   # 本解释器就在 3.12 之前：编译即事实
            try:
                compile(src, rel, "exec")
                hits = []
            except SyntaxError as e:
                hits = [(e.lineno or 0, f"Python {sys.version_info[0]}.{sys.version_info[1]} 编译失败：{e.msg}")]
        else:
            per = {}
            for line, why in problems(src):   # 一行报一次：同一处常同时犯两条（嵌套 f-string 连带它的空串、注释连带跨行）
                if why not in per.setdefault(line, []):
                    per[line].append(why)
            hits = [(line, "；".join(whys)) for line, whys in sorted(per.items())]
        for line, why in hits:
            print(f"{os.path.relpath(path, root)}:{line}: {why}（作业层最低 Python {low[0]}.{low[1]} 不认）")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
