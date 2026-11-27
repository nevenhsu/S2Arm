#!/usr/bin/env python3
# macos_only_lint 单测（shell_lint_test.sh 调用）：负对照用 11-01 oneclick.sh 交付拷贝修前那两行
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macos_only_lint import lint_repo, lint_text  # noqa: E402

ONECLICK_OLD = '''rm -f "$OUT/$G.ipa"; cp -c "$PK/$G.ipa" "$OUT/$G.ipa" || die "拷 ipa 失败"
D=$OUT/${G}资源包; [ ! -e "$D" ] || rm -rf "$D"; cp -cR "$PK/pack" "$D" || die "拷资源包失败"
'''
ONECLICK_NEW = '''rm -f "$OUT/$G.ipa"
if ! cp -c "$PK/$G.ipa" "$OUT/$G.ipa" 2> /dev/null; then cp "$PK/$G.ipa" "$OUT/$G.ipa" || die "拷 ipa 失败"; fi
D=$OUT/${G}资源包; [ ! -e "$D" ] || rm -rf "$D"
if ! cp -cR "$PK/pack" "$D" 2> /dev/null; then rm -rf "$D"; cp -R "$PK/pack" "$D" || die "拷资源包失败"; fi
'''


class MacosOnlyLint(unittest.TestCase):
    def test_oneclick_negative_control(self):
        self.assertEqual([i for i, _ in lint_text(ONECLICK_OLD)], [1, 2])   # 修前：两行都报（|| die 不算退回）
        self.assertEqual(lint_text(ONECLICK_NEW), [])                      # 修后：if ! … then 普通拷贝

    def test_forms(self):
        bad = ["stat -f %z f", "sed -i '' s/a/b/ f", 'sed -i "" s/a/b/ f', "cp -ca a b", "x=$(stat -f %m f)",
               "cp -c a b 2> /dev/null || die 拷不了", "cp -c a b 2>/dev/null || { echo 失败; exit 1; }",
               'm=$(stat -f %m f 2> /dev/null || stat -c %Y f)', 'ino() { stat -f %i "$1" 2> /dev/null || stat -c %i "$1"; }']   # BSD 在前：GNU 下先吐文件系统信息
        good = ["# cp -c 是克隆", "cp -R a b", "sed -i.bak s/a/b/ f", "stat -c %s f",
                '[ "$(uname -s)" = Darwin ] && stat -f %z f', "cp -c a b 2> /dev/null || cp a b",
                "if ! cp -cR a b; then cp -R a b; fi", "rm -f x  # 不用 stat -f",
                'm=$(stat -c %Y f 2> /dev/null || stat -f %m f)']   # GNU 在前：BSD 不认 -c、不打标准输出
        for l in bad:
            self.assertTrue(lint_text(l), l)
        for l in good:
            self.assertEqual(lint_text(l), [], l)

    def test_allowlist_and_stale(self):
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(["git", "init", "-q", d], check=True)
            os.makedirs(os.path.join(d, "tests"))
            open(os.path.join(d, "a.sh"), "w").write("stat -f %z x\n")
            open(os.path.join(d, "b.sh"), "w").write("echo ok\n")
            subprocess.run(["git", "-C", d, "add", "a.sh", "b.sh"], check=True)
            probs, _ = lint_repo(d)
            self.assertTrue(any(p.startswith("a.sh:1:") for p in probs), probs)          # 没登记 → 报
            open(os.path.join(d, "tests/macos_only_ok.tsv"), "w").write("a.sh\t要 Xcode\nb.sh\t旧\n")
            probs, n = lint_repo(d)
            self.assertEqual(n, 2)
            self.assertEqual(probs, ["tests/macos_only_ok.tsv: b.sh 已没有 macOS 专有用法，从白名单删掉（过期）"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
