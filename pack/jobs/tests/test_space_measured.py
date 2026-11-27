import tempfile
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""出包空间按实测（11-07）：实测记账口径、预算三档依据、拦截文案、「仍然继续」放行。全为合成文件，期望值手算。"""
import collections
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

from jobcase import JobCase, ROOT, as_windows
from core import Context, Failure
import layout
import library

sys.path.insert(0, str(ROOT))
from pack.contracts.validate import validate, ContractError

MIB = 1 << 20
GID = "01007EF00011E000"
DU = collections.namedtuple("du", "total used free")


def write(path, size):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(os.urandom(size))   # 随机内容：不让文件系统压缩 / 稀疏，1 MiB 就占 1 MiB 块
    return path


class SpaceMeasured(JobCase):
    def setUp(self):
        super().setUp()
        env = patch.dict(os.environ, {})   # 只有直连（11-07）；实测账只认直连行
        env.start(); self.addCleanup(env.stop)

    def context(self, operation="ipa", **extra):
        self.sequence += 1
        return Context(self.request | dict(taskId=f"space-{self.sequence}", operation=operation, gameId=GID) | extra)

    def game(self, size, gid=GID, name="塞尔达传说 旷野之息"):
        pkg = dict(titleId=gid, baseId=gid, type="base", version=0, path="sources/x.nsp", sha256=gid.lower() * 4, size=size, guestIsa="aarch64")
        return dict(id=gid, name=name, packages=[pkg])

    def ledger(self, *rows):
        # time gameId target direct inputBytes footprintBytes name
        with (self.c.library / library.SPACE_LEDGER).open("a", encoding="utf-8") as f:
            for r in rows:
                f.write("\t".join(map(str, list(r) + [""] * (7 - len(r)) + [library.SPACE_LEDGER_FORMAT])) + "\n")

    def budget(self, c, game, free=45 * 10**9):
        with patch("shutil.disk_usage", return_value=DU(0, 0, free)):
            return library.space_budget(c, game)["volumes"][0]

    # —— 实测记账口径 ——
    def test_disk_bytes_skips_named_dirs_only_under_given_roots(self):
        root = Path(self.t if hasattr(self, "t") else tempfile.mkdtemp())
        keep = root / "cache" / "romfs"; clone = root / "iphone" / "romfs"
        keep.mkdir(parents=True); clone.mkdir(parents=True)
        (keep / "a").write_bytes(b"x" * MIB); (clone / "a").write_bytes(b"x" * MIB)
        dev = root.stat().st_dev
        both = library.disk_bytes([root], dev)
        one = library.disk_bytes([root], dev, skip_names=library.CLONED_DIRS, skip_under=[str(root / "iphone")])
        self.assertGreaterEqual(both - one, MIB)       # 克隆那份不算
        self.assertGreaterEqual(one, MIB)              # 解包那份还算

    @unittest.skipUnless(sys.platform == "darwin", "APFS 克隆只在 macOS")
    def test_apfs_clones_count_shared_blocks_once(self):
        """#303：同一份 RomFS 的克隆（解包 / 各层产物根 / .romfs_store / Mac 运行版本，cp -c）只算一份共享块。
        8 MiB 随机文件克隆成 4 份 → 约 8 MiB（不是 32 MiB）；其中一份改 1 字节（换新克隆族、份数 1）→ 它按整份再算一遍：约 16 MiB（偏大、保守）"""
        import subprocess
        root = Path(tempfile.mkdtemp(dir=self.c.library))
        src = write(root / "build" / "extract" / "romfs" / "data.arc", 8 * MIB)
        if library._CloneInfo.get(src) is None:
            self.skipTest("这台机器取不到 APFS 克隆信息（非 APFS 卷）")
        for rel in ("build/k/roots/a@1/romfs", "build/k/roots/.romfs_store/x", "image/mac/romfs"):
            (root / rel).mkdir(parents=True)
            subprocess.run(["cp", "-c", str(src), str(root / rel / "data.arc")], check=True)
        dev = root.stat().st_dev
        got = library.disk_bytes([root], dev)
        self.assertGreaterEqual(got, 8 * MIB); self.assertLess(got, 8 * MIB + 2 * MIB, got)   # 余量 = 目录元数据
        with open(root / "image/mac/romfs/data.arc", "r+b") as f:
            f.seek(100); f.write(b"\x00" if f.read(1) != b"\x00" else b"\x01")
        got = library.disk_bytes([root], dev)
        self.assertGreaterEqual(got, 16 * MIB); self.assertLess(got, 16 * MIB + 2 * MIB, got)
        # 负对照：stat 口径（不认克隆）是 4 份满额
        stat_sum = sum(p.stat().st_blocks * 512 for p in root.rglob("data.arc"))
        self.assertGreaterEqual(stat_sum, 32 * MIB)

    @unittest.skipUnless(sys.platform == "darwin", "APFS 克隆只在 macOS")
    def test_pack_footprint_counts_romfs_clones_once_and_skips_saves(self):
        """#303 记账：宝可梦 Z-A 一类，解包 + 多层产物根 + Mac 运行版本都是同一份 RomFS 的克隆 → 记一份；存档 / 运行时着色器缓存不算出包占用"""
        import subprocess
        c = self.context("build")
        library.atomic(self.c.library / "catalog.json", dict(schema=1, games=[self.game(8 * MIB)]))
        d = layout.game_dir(c, GID, english="Zelda")
        src = write(d / "build" / "extract" / "romfs" / "data.arc", 8 * MIB)
        if library._CloneInfo.get(src) is None:
            self.skipTest("这台机器取不到 APFS 克隆信息（非 APFS 卷）")
        for rel in ("build/k/roots/a@1/romfs", "build/k/roots/b@1/romfs", "build/k/roots/.romfs_store/x", "image/mac/romfs"):
            (d / rel).mkdir(parents=True)
            subprocess.run(["cp", "-c", str(src), str(d / rel / "data.arc")], check=True)
        write(d / "saves" / "s.bin", 4 * MIB); write(d / "shader-cache" / "p.bin", 4 * MIB)
        got = library.pack_footprint(c, GID)
        self.assertGreaterEqual(got, 8 * MIB); self.assertLess(got, 8 * MIB + 2 * MIB, got)

    def test_disk_bytes_counts_hardlink_once_skips_symlink_and_other_volume(self):
        root = self.root / "measure"
        a = write(root / "a.bin", MIB)
        base = library.disk_bytes([root], root.stat().st_dev)
        self.assertGreaterEqual(base, MIB); self.assertLess(base, MIB + 64 * 1024)   # 1 MiB 文件 + 目录元数据（APFS 目录 0 块）
        (root / "sub").mkdir()
        os.link(a, root / "sub" / "a-link.bin")
        (root / "a-sym.bin").symlink_to(a)
        sub = library.disk_bytes([root], root.stat().st_dev) - base   # 硬链接 + 软链：数据不重复算，只多一个子目录的元数据
        self.assertLess(sub, 64 * 1024)
        write(root / "b.bin", MIB)
        self.assertEqual(library.disk_bytes([root], root.stat().st_dev) - base - sub, MIB)   # 新增一份真数据 = 正好 1 MiB
        self.assertEqual(library.disk_bytes([root, root / "sub", a], root.stat().st_dev), base + sub + MIB)   # 根互相包含也只算一次
        self.assertEqual(library.disk_bytes([root], root.stat().st_dev + 1), 0)   # 别的卷上的不算

    def test_record_measures_library_dirs_before_prune(self):
        # 打 iPhone 包成功后的自动清理（prune_build）先记一行：要清掉的中间产物也算进去（量的是峰值，不是清完剩下的）
        c = self.context("ipa")
        game = self.game(10 * 10**9)
        library.atomic(self.c.library / "catalog.json", dict(schema=1, games=[game]))
        ios = layout.ios_work(self.c, GID)   # iPhone 出包工作目录 build/ios
        extract = write(layout.build_dir(self.c, GID) / "extract" / "romfs.bin", MIB)
        write(ios / "junk.tmp", 2 * MIB)   # 成功后会被清掉的中间产物
        (ios / "roots" / "r@1").mkdir(parents=True)
        os.link(extract, ios / "roots" / "r@1" / "romfs.bin")   # 产物根硬链解包：不重复算
        write(self.c.library / "runtime-cache" / GID / "play.bin", MIB)   # 玩的时候才长的运行时缓存：不算
        write(layout.ios_work(self.c, "0100000000000001") / "other.bin", MIB)   # 别款：不算
        library.prune_build(c, GID)
        self.assertFalse((ios / "junk.tmp").exists())   # 清理照常
        lines = [l.split("\t") for l in (self.c.library / library.SPACE_LEDGER).read_text(encoding="utf-8").splitlines() if not l.startswith("#")]
        self.assertEqual(len(lines), 1)
        r = dict(zip(library.SPACE_LEDGER_FIELDS, lines[0]))
        self.assertEqual((r["gameId"], r["target"], r["direct"], r["inputBytes"], r["name"]), (GID, "ipa", "1", str(10 * 10**9), "塞尔达传说 旷野之息"))
        fp = int(r["footprintBytes"])
        self.assertGreaterEqual(fp, 3 * MIB); self.assertLess(fp, 3 * MIB + 64 * 1024)   # 1 MiB 解包（硬链接那份不重算）+ 2 MiB 中间产物
        self.assertTrue((self.c.library / library.SPACE_LEDGER).read_text(encoding="utf-8").startswith("# time\tgameId"))

    # —— 预算三档依据 ——
    # 期望值手算：29.13 GB × 1.1 + 15 GB；验收数 火纹 ≈ 47、0100F2C0115B6000 ≈ 34、小游戏 ≈ 8（维护者 11-08）。单文件镜像不再存进库（#329），不另加一份
    def need(self, size, **kw):
        return library.pack_need_bytes([dict(size=size, sha256="a" * 64)], **kw)

    def test_formula_acceptance_numbers(self):
        self.assertEqual(self.need(29_130_000_000, windows=False), 32_043_000_000 + 15_000_000_000)
        self.assertEqual(self.need(17_480_000_000, windows=False), 19_228_000_000 + 15_000_000_000)
        self.assertEqual(self.need(2_700_000_000, windows=False), 2_970_000_000 + 5_000_000_000)

    def test_shared_original_sums_item_contents(self):
        # #444：一个压缩合集（XCZ）装本体 + 更新，原件 10 GB，两项解开各 8 GB、6 GB → 要按 14 GB 算；原先只留最后一项 → max(10, 6) = 10 GB
        shared = [dict(size=10_000_000_000, contentBytes=8_000_000_000, sha256="b" * 64),
                  dict(size=10_000_000_000, contentBytes=6_000_000_000, sha256="b" * 64)]
        self.assertEqual(library.pack_input_bytes(shared), 14_000_000_000)
        # 不压缩的合集 / NUP：各项之和不超过原件 → 仍按原件；另一个原件照加
        nup = [dict(size=10_000_000_000, contentBytes=7_000_000_000, sha256="c" * 64),
               dict(size=10_000_000_000, contentBytes=2_000_000_000, sha256="c" * 64),
               dict(size=3_000_000_000, sha256="d" * 64)]
        self.assertEqual(library.pack_input_bytes(nup), 13_000_000_000)

    def test_nup_items_overlap_take_largest(self):
        # NUP 容器内按块去重，更新是整份文件系统、和本体共用数据块：0400486024DEC000 实测文件 25.62 GB、本体与更新各 26.27 GB
        #   → 按 26.27 GB 算（取大值）；相加会成 52.54 GB（v0.2.5 的回退）。同一 sha 里只要有一项是 nup 就按这条算
        nup = [dict(size=25_622_267_860, contentBytes=26_270_000_000, sha256="e" * 64, format="nup"),
               dict(size=25_622_267_860, contentBytes=26_270_000_000, sha256="e" * 64, format="nup")]
        self.assertEqual(library.pack_input_bytes(nup), 26_270_000_000)
        # 另一个原件（非 nup）照旧单独加上
        self.assertEqual(library.pack_input_bytes(nup + [dict(size=3_000_000_000, sha256="f" * 64, format="nsp")]), 29_270_000_000)

    def test_nup_dlc_adds_to_the_largest_item(self):
        # #444：NUP 里本体 / 更新重叠取最大值，DLC 是另一棵树、不重叠，要加上（原先 DLC 也参与取最大值，整份漏算）。
        #   手算：本体、更新各 26.27 GB，DLC 1.5 GB → 26.27 + 1.5 = 27.77 GB
        common = dict(size=25_622_267_860, sha256="e" * 64, format="nup")
        nup = [dict(common, type="base", contentBytes=26_270_000_000), dict(common, type="update", contentBytes=26_270_000_000),
               dict(common, type="dlc", contentBytes=1_500_000_000)]
        self.assertEqual(library.pack_input_bytes(nup), 27_770_000_000)

    def nup_game(self):
        # 合成 NUP（jobcase.synthetic_nup，期望就是写进去的字节）：本体 exefs main 300 B + main.npdm（256 B）、romfs a.bin 3000 B + b.bin 100 B；
        #   更新 exefs main 500 B + main.npdm、romfs a.bin（与本体同内容 → 全量）+ c.bin 200 B；DLC romfs d1 10 B + d2 1500 B
        from jobcase import synthetic_npdm, synthetic_nup
        app = 0x0400486024DEC000
        src = Path(self.root) / "drive" / "T [0400486024dec000][v131072].nup"; src.parent.mkdir(exist_ok=True)
        a = os.urandom(3000)
        synthetic_nup(src, app, [(0, app, 0, {"main": b"\x01" * 300, "main.npdm": synthetic_npdm(app)}, {"a.bin": a, "b.bin": b"b" * 100}, {}),
                                 (1, app | 0x800, 131072, {"main": b"\x02" * 500, "main.npdm": synthetic_npdm(app)}, {"a.bin": a, "c.bin": b"c" * 200}, {}),
                                 (2, app + 0x1001, 0, None, {"d1": b"d" * 10, "d2": b"e" * 1500}, {})])
        common = dict(baseId="0400486024DEC000", path=str(src), sha256="e" * 64, size=src.stat().st_size, format="nup", guestIsa="unknown")
        packages = [dict(common, titleId="0400486024DEC000", type="base", version=0, contentBytes=3100),
                    dict(common, titleId="0400486024DEC800", type="update", version=131072, contentBytes=3200),
                    dict(common, titleId="0400486024DED001", type="dlc", version=0, contentBytes=1510)]
        return src, dict(id="0400486024DEC000", name="T", packages=packages)

    def test_nup_unpack_bytes_by_cluster(self):
        # #444：NUP 解包目录在分配单元 1024 字节的盘上的实占（每文件向上取整、每目录一簇），手算：
        #   解包目录本身 1 簇；exefs 取更新的：main 500 → 1 簇、main.npdm 256 → 1 簇，exefs/ 1 簇 = 3 簇；romfs 全量用更新 = a 3000 + c 200 = 3200 字节：
        #   镜像形态一个 romfs.std → 4 簇；DLC d1 10 → 1、d2 1500 → 2、romfs/ 1 = 4 簇 → 共 1 + 3 + 4 + 4 = 12 簇 = 12288
        #   散文件形态：a 3 簇 + c 1 簇 + romfs/ 1 簇 = 5 簇 → 1 + 3 + 5 + 4 = 13 簇 = 13312；读不到簇（0）：原始字节 500 + 256 + 3200 + 10 + 1500 = 5466
        _, game = self.nup_game()
        c = self.context("ipa")
        pk = library.selected_packages(game)
        with patch.dict(os.environ, {"S2A_ROMFS_IMAGE": "1"}):
            self.assertEqual(library.nup_unpack_bytes(c, pk, 1024), 12 * 1024)
            self.assertEqual(library.nup_unpack_bytes(c, pk, 0), 5466)
        with patch.dict(os.environ, {"S2A_ROMFS_IMAGE": "0"}):
            self.assertEqual(library.nup_unpack_bytes(c, pk, 1024), 13 * 1024)
        self.assertIsNone(library.nup_unpack_bytes(c, [dict(p, format="nsp") for p in pk], 1024))   # 不是 NUP：照原件大小算

    def test_budget_counts_nup_by_cluster_on_exfat(self):
        # #444：exFAT 1 MiB 簇的外接盘、Mac 打 iPhone 包：解包按实占（簇 1 MiB 时上一项的 12 个单位 → 1 + 3 + 1 + 3 = 8 MiB：exefs 两个文件各 1 簇 + 目录 1 簇，
        #   romfs.std 1 簇，DLC 两个文件各 1 簇 + 目录 1 簇，解包目录 1 簇）× 1.1 = 9227468；exFAT 不能克隆再加一份；直连按原件（< 3 GB）5 GB
        #   → 9227468 × 2 + 5e9 = 5018454936。说明写出分配单元
        _, game = self.nup_game()
        c = self.context("ipa")
        with patch.dict(os.environ, {"S2A_ROMFS_IMAGE": "1"}), patch.object(library, "volume_format", return_value="exfat"), \
                patch.object(library, "volume_cluster", return_value=1 << 20):
            b = self.budget(c, game)
        self.assertEqual(b["needBytes"], 9_227_468 * 2 + 5_000_000_000)
        self.assertIn("（按这块盘 1024 KB 的分配单元逐文件算）", b["note"])
        # 4 KB 簇（APFS 常见）：不写分配单元那句
        with patch.dict(os.environ, {"S2A_ROMFS_IMAGE": "1"}), patch.object(library, "volume_format", return_value="apfs"), \
                patch.object(library, "volume_cluster", return_value=4096):
            self.assertNotIn("分配单元", self.budget(c, game)["note"])

    def test_disk_full_after_enough_budget_says_estimate_low(self):
        # #444：预检判这块盘够、出到一半写满 → 原因说清是预估偏低；预检说不够 / 盘名对不上 / 没有预检：原样
        import errors
        said = errors.DISK_FULL_REASON.format("Extreme SSD", "3.5", "0.9")
        ok = dict(volumes=[dict(volume="Extreme SSD", enough=True, needBytes=19_571_257_436)])
        self.assertEqual(errors.estimate_low(said, ok), errors.DISK_FULL_LOW.format("Extreme SSD", "3.5", "0.9", "20"))
        self.assertEqual(errors.estimate_low(said, dict(volumes=[dict(volume="Extreme SSD", enough=False, needBytes=1)])), said)
        self.assertEqual(errors.estimate_low(said, dict(volumes=[dict(volume="系统盘", enough=True, needBytes=1)])), said)
        self.assertEqual(errors.estimate_low(said, None), said)
        # 作业层收尾：Context 上有这次的预检就换句
        c = self.context("ipa")
        c.space_budget = ok
        self.assertEqual(c.describe_failure(Failure(said, code="disk_full"))["summary"], errors.DISK_FULL_LOW.format("Extreme SSD", "3.5", "0.9", "20"))

    def test_ns2_dump_consume_moves_dlc_instead_of_copying(self):
        # #444：ns2_nup 的中间目录用完即删，--consume 时 DLC 散文件同盘挪进解包目录（inode 不变、来源没了），不留第二份；不带时照旧拷、来源还在
        from jobcase import synthetic_npdm
        spec = __import__("importlib.util").util.spec_from_file_location("ns2_dump_t", ROOT / "pack" / "ns2_dump.py")
        dump = __import__("importlib.util").util.module_from_spec(spec); spec.loader.exec_module(dump)
        app = 0x0400486024DEC000
        for consume in (True, False):   # ns2_dump 当脚本跑时 pack/ 在模块路径里（收尾改名用 pack/orch/dirrename）：单测里同样放进去
            with self.subTest(consume=consume), patch.dict(os.environ, {"S2A_ROMFS_IMAGE": "0"}), patch.object(sys, "path", [str(ROOT / "pack")] + sys.path):
                d = Path(self.root) / f"dump{int(consume)}"
                (d / "base/exefs").mkdir(parents=True); (d / "base/exefs/main").write_bytes(b"\x01" * 300)
                (d / "base/exefs/main.npdm").write_bytes(synthetic_npdm(app))
                (d / "base/romfs").mkdir(); (d / "base/romfs/a.bin").write_bytes(b"a" * 30)
                dl = d / f"dlc/{app + 0x1001:016X}/0/romfs"; dl.mkdir(parents=True); (dl / "x.bin").write_bytes(b"x" * 40)
                ino = os.stat(dl / "x.bin").st_ino
                out = Path(self.root) / f"out{int(consume)}"
                dump.convert(str(d), str(out), "T", consume=consume)
                moved = out / "aoc/1/x.bin"
                self.assertEqual(moved.read_bytes(), b"x" * 40)
                self.assertEqual((dl / "x.bin").exists(), not consume)
                if consume:
                    self.assertEqual(os.stat(moved).st_ino, ino)
                self.assertIn("\t1\t40\n", (out / "aoc.tsv").read_text())   # aoc.tsv 的文件数 / 字节照记（挪之前量）

    def test_unpacked_counts_zero_and_windows_adds_a_copy(self):
        self.assertEqual(self.need(10_000_000_000, unpacked=True, windows=False), 15_000_000_000)
        self.assertEqual(self.need(10_000_000_000, windows=True), 11_000_000_000 * 2 + 15_000_000_000)
        # v0.3.1 Mac 放开 HFS+：不能克隆，同 Windows 多算一份（11 + 11 + 15）
        self.assertEqual(library.pack_need_bytes([dict(size=10_000_000_000, sha256="a" * 64)], windows=False, clones=False), 11_000_000_000 * 2 + 15_000_000_000)

    def test_windows_exfat_location_counts_two_more_copies(self):
        # v0.3.1：Windows 存放位置在 exFAT / FAT32（建不了目录链接，实目录模式 + 工作根整份拷）：再多两份。原件 10 GB 的 iPhone 包：
        #   NTFS = 解包 11 + Windows 不克隆 11 + 直连 15 = 37 GB（IMG 边写边腾解包、不另占，#746）；exFAT 再加 22 = 59 GB
        c = self.context("ipa")
        for fmt, need in (("ntfs", 37_000_000_000), ("exfat", 59_000_000_000)):
            import space_rule
            with self.subTest(fmt=fmt), as_windows(library, space_rule), patch.object(library, "volume_format", return_value=fmt):
                b = self.budget(c, self.game(10 * 10**9))
                self.assertEqual(b["needBytes"], need)
                self.assertEqual("建不了目录链接" in b.get("note", ""), fmt == "exfat")

    def test_budget_counts_extra_copy_on_hfs(self):
        # 存放位置读到是 HFS+：预算按不克隆多算一份、说明里写清；读到 APFS / 读不到：不多算。原件 10 GB 的 Mac 出包（不写 IMG）：
        #   APFS 11 + 15 = 26 GB；HFS+ 再加 11 = 37 GB
        c = self.context("build")
        for fmt, need in (("apfs", 26_000_000_000), ("", 26_000_000_000), ("hfs", 37_000_000_000)):
            with self.subTest(fmt=fmt), patch.object(library, "volume_format", return_value=fmt):
                b = self.budget(c, self.game(10 * 10**9))
                self.assertEqual(b["needBytes"], need)
                self.assertEqual("不能克隆" in b.get("note", ""), fmt == "hfs")

    def test_budget_splits_location_and_cache_volumes(self):
        # P1-165：存放位置与工具缓存在两块盘时，各按自己那份需求和自己的余量判；期望手算：原件 16 GB → 存放位置 16 × 1.1 = 17.6 GB
        #   （打 iPhone 包写 game.img 在存放位置，边写边腾解包、不另占，#746），工具缓存 15 GB（≥ 3 GB 档）
        c = self.context("ipa")
        loc, cache = Path(self.root) / "ext", Path(self.root) / "cache"
        class St:   # 两个假卷号
            def __init__(self, dev): self.st_dev = dev
        disks = {str(loc): (1, 20 * 10**9), str(cache): (2, 10 * 10**9)}
        fake = lambda path: type("V", (), {"stat": lambda self, d=disks[str(path)][0]: St(d), "__fspath__": lambda self, p=str(path): p,
                                           "__str__": lambda self, p=str(path): p})()
        with patch.object(library, "_existing", side_effect=lambda p: fake(loc if p != c.work_root else cache)), \
                patch("shutil.disk_usage", side_effect=lambda p: DU(0, 0, disks[os.fspath(p)][1])), \
                patch.object(library, "_volume", side_effect=lambda p: os.fspath(p)):
            vols = library.space_budget(c, self.game(16 * 10**9))["volumes"]
        self.assertEqual([(v["role"], v["needBytes"], v["freeBytes"], v["enough"]) for v in vols],
                         [("library", 17_600_000_000, 20 * 10**9, True), ("cache", 15_000_000_000, 10 * 10**9, False)])
        # 同一块盘：合成一项，需求 = 两份之和（同原来）
        self.assertEqual(self.budget(c, self.game(16 * 10**9))["needBytes"], 17_600_000_000 + 15_000_000_000)

    def test_direct_cache_follows_game_volume(self):
        # 11-10 网友：存放位置设到外接盘，系统盘仍被写满。直连缓存（含编译中的翻译中间产物）跟着游戏的存放位置走：
        #   同一块盘 → 工具缓存下（同原来）；游戏在另一块盘 → 这款文件夹第一层 direct-cache/；还没有游戏文件夹 → 工具缓存
        c = self.context("ipa")
        tool = c.work_root / "run" / "s2arm" / "cache" / "direct" / GID
        game_dir = Path(self.root) / "ext" / "games" / GID
        class St:
            def __init__(self, dev): self.st_dev = dev
        def existing(dev_of_game):
            return lambda p: type("V", (), {"stat": lambda self: St(dev_of_game if Path(p) == game_dir else 1)})()
        with patch.object(library.layout, "find_game", return_value=game_dir), patch.object(library, "_existing", side_effect=existing(1)):
            self.assertEqual(library.direct_cache_dir(c, GID), tool)
        with patch.object(library.layout, "find_game", return_value=game_dir), patch.object(library, "_existing", side_effect=existing(2)):
            self.assertEqual(library.direct_cache_dir(c, GID), game_dir / "direct-cache")
        with patch.object(library.layout, "find_game", return_value=None):
            self.assertEqual(library.direct_cache_dir(c, GID), tool)

    def test_scratch_follows_game_volume(self):
        # D-NDV5（Windows 0.6.1）：游戏库设在别的盘，「更新」几款时重编着色器库的临时活写到系统盘。临时目录同直连缓存跟着游戏走（期望手写）：
        #   同一块盘 → 工具缓存 tmp/；游戏在另一块盘 → 这款文件夹第一层 .work-tmp/；目录当场建好
        c = self.context("installUpdate")
        game_dir = Path(self.root) / "ext" / "games" / GID
        with patch.object(library, "direct_cache_dir", return_value=c.work_root / "run" / "s2arm" / "cache" / "direct" / GID):
            self.assertEqual(library.scratch_root(c, GID), c.work_root / "tmp")
        with patch.object(library, "direct_cache_dir", return_value=game_dir / "direct-cache"):
            got = library.scratch_root(c, GID)
        self.assertEqual(got, game_dir / ".work-tmp")
        self.assertTrue(got.is_dir())

    def test_aoc_cache_follows_game_volume(self):
        # #1725（诊断 D-K82N）：游戏库在外接盘，DLC 解到工具缓存（系统盘）写满。追加内容缓存同直连缓存跟着游戏走：
        #   同一块盘 → 工具缓存下的 cache/aoc（同原来）；游戏在另一块盘 → 这款文件夹第一层 aoc-cache/；还没有游戏文件夹 → 工具缓存；出包环境把它交给 S2A_AOC_CACHE
        c = self.context("ipa")
        tool = c.work_root / "run" / "s2arm" / "cache" / "aoc"
        game_dir = Path(self.root) / "ext" / "games" / GID
        class St:
            def __init__(self, dev): self.st_dev = dev
        def existing(dev_of_game):
            return lambda p: type("V", (), {"stat": lambda self: St(dev_of_game if Path(p) == game_dir else 1)})()
        with patch.object(library.layout, "find_game", return_value=game_dir), patch.object(library, "_existing", side_effect=existing(1)):
            self.assertEqual(library.aoc_cache_dir(c, GID), tool)
        with patch.object(library.layout, "find_game", return_value=game_dir), patch.object(library, "_existing", side_effect=existing(2)):
            self.assertEqual(library.aoc_cache_dir(c, GID), game_dir / "aoc-cache")
        with patch.object(library.layout, "find_game", return_value=None):
            self.assertEqual(library.aoc_cache_dir(c, GID), tool)
        import sizes
        self.assertIn("aoc-cache", sizes.CACHE_NAMES, "空间页把它算进这款的缓存（清缓存能清掉）")

    def test_ledger_does_not_change_budget(self):
        c = self.context("ipa")
        self.ledger(("2026-11-07T10:00:00", GID, "ipa", "1", 16 * 10**9, 500 * 10**9, "旧账"))
        v = self.budget(c, self.game(16 * 10**9))
        self.assertEqual((v["basis"], v["needBytes"]), ("rule", 17_600_000_000 + 15_000_000_000))   # 解包 17.6 + 直连 15（IMG 边写边腾解包）

    def test_rejection_names_need_and_free(self):
        c = self.context("ipa")
        with patch("shutil.disk_usage", return_value=DU(0, 0, 20 * 10**9)):
            library.check_pack_space(c, self.game(16 * 10**9))   # 11-14 不许堵门：不拦，只记一条盘满警告
        [d] = c.job["diagnostics"]; msg = d["summary"]
        self.assertEqual((d["code"], d["severity"]), ("disk_full", "warning"))
        self.assertIn("要 33 GB", msg);   # 16 × 1.1 + 直连 15 = 32.6 → 33 GB（IMG 边写边腾解包，#746）
        self.assertIn("解包 18 GB", msg); self.assertIn("直连着色器上限 15 GB", msg)   # 分项（#746）
        self.assertIn("只剩 20 GB", msg); self.assertIn("这是预估，照常开工", msg)

    def test_mac_build_has_no_image_and_ocarina_peak_is_covered(self):
        # 单一 IMG 只在打 iPhone 包时写（Mac 打包不写）。#746：写 IMG 时边写边腾解包，iPhone 包和 Mac 打包一样多；
        #   同一款接着还要出别的平台（followUp）留着解包，IMG 照旧另占一份。
        #   11-11 实测 0400486024DEC000（NUP，资源 26.27 GB）冷出 iPhone 包、不边写边腾时整次峰值 57.90 GiB（写 game.img 时）：留解包的预留须 ≥ 峰值且 ≤ 峰值 × 1.2。
        #   手算：26.27 × 1.1 + IMG 26.27 + 直连 15 = 70.167 GB；峰值 57.90 GiB = 62.17 GB；比值 1.13
        mac, ipa = self.budget(self.context("build"), self.game(16 * 10**9))["needBytes"], self.budget(self.context("ipa"), self.game(16 * 10**9))["needBytes"]
        self.assertEqual(ipa, mac)
        keep = self.budget(self.context("ipa", followUp=True), self.game(16 * 10**9))["needBytes"]
        self.assertEqual(keep - mac, 16 * 10**9)
        need = library.pack_need_bytes([dict(size=25_622_267_860, contentBytes=26_270_000_000, sha256="b" * 64, format="nup")], windows=False, image=True, keep_unpack=True)
        self.assertEqual(need, 70_167_000_000)
        peak = int(57.90 * 2**30)
        self.assertGreaterEqual(need, peak)
        self.assertLessEqual(need, peak * 1.2)

    def test_old_iphone_pack_counts_as_reclaimable_and_override_drops_it_first(self):
        # #746：这款上一次出的 iPhone 资源包出完会被新包换掉。空间不够时失败页点名它、说「仍然继续会先删掉它」；
        #   点了「仍然继续」先删掉它再出（旧包和新包同时在盘上那段原先是峰值的一部分）；一键装 / Mac 打包不算它
        c = self.context("ipa")
        old = library.layout.image_ios(c, GID) / library.layout.IOS_PACK
        write(old / "game.img", 1 << 20)
        v = self.budget(c, self.game(16 * 10**9), free=20 * 10**9)
        self.assertGreaterEqual(v["reclaimBytes"], 1 << 20)
        self.assertEqual(self.budget(self.context("build"), self.game(16 * 10**9))["reclaimBytes"], 0)
        with patch("shutil.disk_usage", return_value=DU(0, 0, 20 * 10**9)):
            library.check_pack_space(c, self.game(16 * 10**9))   # 11-14 不许堵门：不拦，警告里说旧包出完会被换掉
        self.assertIn("这款上一次的 iPhone 包", c.job["diagnostics"][0]["summary"])
        self.assertTrue(old.is_dir())   # 没点过「仍然继续」：不删
        c = self.context("ipa", spaceOverride=True)
        with patch("shutil.disk_usage", return_value=DU(0, 0, 20 * 10**9)):
            library.check_pack_space(c, self.game(16 * 10**9))
        self.assertFalse(old.exists())
        [d] = c.job["diagnostics"]
        self.assertIn("已按你的选择先删掉这款上一次的 iPhone 包", d["summary"])

    def test_player_746_numbers(self):
        # #746 玩家原数：NUP 原件 43.46 GB、可用 36.86 GB、库里旧 iPhone 包 29 GB（升级后解包不在盘上）。独立手算：
        #   预估 = 解包 43.46 × 1.1 = 47.806（显示 48 GB）+ 直连 15 = 62.806 → 「要 63 GB」（原先再加 IMG 43.46 = 106.27）；
        #   不够 → 失败页给「仍然继续」并说会先删旧包腾出 29 GB；删后 36.86 + 29 = 65.86 ≥ 62.8，放得下
        c = self.context("ipa")
        old = library.layout.image_ios(c, GID) / library.layout.IOS_PACK
        write(old / "game.img", 1 << 20)
        nup = dict(id=GID, name="x", packages=[dict(titleId=GID, baseId=GID, type="base", version=0, path="sources/x.nsp", sha256="c" * 64,
                                                     size=43_460_000_000, contentBytes=43_460_000_000, format="nup", guestIsa="aarch64")])
        with patch.object(library, "_part_bytes", return_value=29 * 10**9), patch("shutil.disk_usage", return_value=DU(0, 0, 36_860_000_000)):
            v = library.space_budget(c, nup)["volumes"][0]
            self.assertEqual((v["needBytes"], v["reclaimBytes"]), (62_806_000_000, 29 * 10**9))
            self.assertLessEqual(v["needBytes"], v["freeBytes"] + v["reclaimBytes"])
            library.check_pack_space(c, nup)   # 11-14 不许堵门：不拦
        msg = c.job["diagnostics"][0]["summary"]
        for want in ("要 63 GB", "解包 48 GB", "直连着色器上限 15 GB", "只剩 37 GB", "这款上一次的 iPhone 包（29 GB）出完会被新包换掉"):
            self.assertIn(want, msg)

    def test_space_override_passes_with_warning(self):
        c = self.context("ipa", spaceOverride=True)
        with patch("shutil.disk_usage", return_value=DU(0, 0, 20 * 10**9)):
            budget = library.check_pack_space(c, self.game(16 * 10**9))
        self.assertTrue(budget["override"]); self.assertFalse(budget["volumes"][0]["enough"])
        [d] = c.job["diagnostics"]
        self.assertEqual((d["code"], d["severity"]), ("disk_full", "warning"))
        self.assertIn("已按你的选择仍然继续", d["summary"])

    def test_follow_up_request_contract(self):
        # 剃刀 M21：followUp 只给打 iPhone 包、只认布尔值（期望按 pack/contracts/README.md 手写）
        self.context("ipa", followUp=True); self.context("ipa", followUp=False)
        for op, value in (("build", True), ("ipa", "true"), ("catalog", True)):
            with self.subTest(op=op, value=value), self.assertRaises(Failure):
                self.context(op, followUp=value)
        base = dict(schema=1, contract="s2a.job-request.v1", privateRoot="/p", library="/l", libraryId="x", gameId=GID)
        validate("request", base | dict(operation="ipa", followUp=True))
        with self.assertRaises(ContractError):
            validate("request", base | dict(operation="ipa", followUp="yes"))

    def test_space_override_request_contract(self):
        self.context("build", spaceOverride=True); self.context("ipa", spaceOverride=False)
        for op, value in (("catalog", True), ("ipa", "true"), ("build", 1), ("launch", True)):
            with self.subTest(op=op, value=value), self.assertRaises(Failure):
                self.context(op, spaceOverride=value)
        base = dict(schema=1, contract="s2a.job-request.v1", privateRoot="/p", library="/l", libraryId="x", gameId=GID)
        validate("request", base | dict(operation="build", spaceOverride=True))
        validate("request", base | dict(operation="ipa", spaceOverride=False))
        for bad in (dict(operation="catalog", spaceOverride=True), dict(operation="ipa", spaceOverride="true")):
            with self.subTest(bad=bad), self.assertRaises(ContractError):
                validate("request", base | bad)


if __name__ == "__main__":
    unittest.main()
