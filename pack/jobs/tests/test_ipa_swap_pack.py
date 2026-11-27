# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""iPhone 资源包就位（ipa._swap_pack：build/ios → image/ios 目录改名）在 Windows 上被占用时按 #361 同一套退避重试（#2292）。
期望手写：旧包先挪成 .old、新包改名就位；新包那次改名第一次报 WinError 5（拒绝访问）→ 退避后第二次成功；最后 image/ios 里是新包的内容、.old 删掉。
用 main 的 ipa.py（改名直接 os.replace、不经退避）跑，这一项不通过。"""
import os
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import jobcase  # noqa: E402（作业上下文替身统一工厂）

import host_update
import ipa
import layout
import rewrite_refresh


class SwapPack(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.c = jobcase.fake_context(self.root, library=self.root / "library", work_root=self.root / "work", engine=jobcase.ROOT, env=lambda: {})
        self.image = self.root / "game" / "image" / "ios"
        self.dest = self.image / layout.IOS_PACK
        self.dest.mkdir(parents=True)
        (self.dest / "old.txt").write_text("旧")
        self.pack = self.root / "game" / "build" / "ios"
        self.pack.mkdir(parents=True)
        (self.pack / "new.txt").write_text("新")
        sys.path.append(str(jobcase.ROOT))
        stubs = [mock.patch.object(layout, "image_ios", return_value=self.image),
                 mock.patch.object(layout, "root_of", return_value=self.root / "game"),
                 mock.patch.object(layout, "hide"),
                 mock.patch.object(layout, "update_info"),
                 mock.patch.object(host_update, "engine_compat", return_value={}),
                 mock.patch.object(host_update, "built_by", return_value="t"),
                 mock.patch.object(ipa, "current_shell_id", return_value="s"),
                 mock.patch.object(rewrite_refresh, "game_bid", return_value="b"),
                 mock.patch.object(rewrite_refresh, "rewriter_id", return_value="r"),
                 mock.patch.object(rewrite_refresh, "tables_id", return_value="t")]
        for s in stubs:
            s.start(); self.addCleanup(s.stop)

    def test_busy_rename_is_retried_on_windows(self):
        from pack.orch import dirrename
        real, calls = Path.rename, []
        def rename(src, dst):
            calls.append((src.name, Path(dst).name))
            if src == self.pack and sum(1 for s, _ in calls if s == self.pack.name) == 1:
                raise PermissionError(13, "拒绝访问")   # 没有 winerror 属性按 5 认（同 dirrename）
            return real(src, dst)
        with mock.patch.object(dirrename, "os", SimpleNamespace(name="nt")), mock.patch.object(dirrename, "DIR_RENAME_RETRIES", (0, 0)), \
             mock.patch.object(Path, "rename", rename):
            ipa._swap_pack(self.c, "0100000000000000", self.pack, "w")
        self.assertEqual(calls, [(layout.IOS_PACK, layout.IOS_PACK + ".old"), ("ios", layout.IOS_PACK), ("ios", layout.IOS_PACK)])
        self.assertEqual(sorted(p.name for p in self.dest.iterdir()), ["new.txt"])
        self.assertFalse((self.image / (layout.IOS_PACK + ".old")).exists())
        self.assertFalse(self.pack.exists())


if __name__ == "__main__":
    unittest.main()
