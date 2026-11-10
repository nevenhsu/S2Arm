"""导入游戏（pack/jobs/library.py import_games）单测（从 test_error_recovery.py 拆出，10-26；夹具 jobcase.JobCase）。"""
import json
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

from jobcase import JOBS, JobCase, synthetic_nacp, synthetic_npdm, synthetic_nup
import library
from core import atomic

APP = 0x0400486024DEC000   # 时之笛的程序号（合成容器，内容是测试字节）


class DigestTests(unittest.TestCase):   # 11-06：导入的包身份 = 原件整份 SHA-256；Windows 走系统 CNG，结果必须与 hashlib 逐位相同
    def test_digest_equals_hashlib_sha256_and_reports_progress(self):
        import hashlib, os, tempfile
        import core
        with tempfile.TemporaryDirectory() as t:
            for n in (0, 1, (4 << 20) - 1, 4 << 20, (4 << 20) * 2 + 12345):   # 空文件、块边界前后、多块加零头
                p = Path(t) / f"f{n}"; data = os.urandom(n); p.write_bytes(data)
                seen = []
                self.assertEqual(core.digest(p, lambda d, total: seen.append((d, total))), hashlib.sha256(data).hexdigest(), n)
                if n: self.assertEqual(seen[-1], (n, n))


class ImportTests(JobCase):
    def test_complete_metadata_does_not_hide_truncation(self):
        self.import_keys()
        self.install_reader_fixture()
        source = self.root / "source.nsp"
        source.write_bytes(b"truncated fixture")
        _, terminal = self.worker("import", paths=[str(source)])
        self.assert_failure(terminal, "content_truncated")
        self.assertEqual(self.c.catalog()["games"], [])

    def test_multi_content_file_with_truncated_content_is_rejected_whole(self):
        # 有意：导入把一个文件当一个整体，被截断的内容之后出包会缺东西，所以整个文件拒绝，
        # 不留下半个包。解包流程仍只跳过被截断的内容（行为不变，见 tests/nx_unpack_fail_test.sh）。
        self.import_keys()
        self.install_reader_fixture()
        source = self.root / "combined.nsp"
        source.write_bytes(b"multi truncated fixture")
        _, terminal = self.worker("import", paths=[str(source)])
        self.assert_failure(terminal, "content_truncated")
        self.assertEqual(self.c.catalog()["games"], [])

    def test_truncated_key_candidate_is_not_evidence_against_keys(self):
        self.import_keys()
        self.install_reader_fixture()
        source = self.root / "source.nsp"
        source.write_bytes(b"complete fixture")
        _, terminal = self.worker("import", paths=[str(source)])
        self.assertEqual(terminal["state"], "succeeded")
        source.write_bytes(b"truncated fixture")
        _, terminal = self.worker("keys", path=str(self.c.keys))
        self.assertEqual(terminal["state"], "succeeded")
        self.assertFalse(terminal["result"]["verified"])

    def test_ticket_missing_key_candidate_is_not_evidence_against_keys(self):
        # #242：库里候选游戏缺票据，同一份格式有效的密钥重新导入要成功、记未验证；再导这款游戏时照报缺票据
        self.import_keys()
        self.install_reader_fixture()
        source = self.root / "source.nsp"
        source.write_bytes(b"complete fixture")
        _, terminal = self.worker("import", paths=[str(source)])
        self.assertEqual(terminal["state"], "succeeded")
        source.write_bytes(b"ticket fixture")
        _, terminal = self.worker("keys", path=str(self.c.keys))
        self.assertEqual(terminal["state"], "succeeded")
        self.assertFalse(terminal["result"]["verified"])
        _, terminal = self.worker("import", paths=[str(source)])
        self.assert_failure(terminal, "ticket_missing")

    def test_missing_keys_outdated_keys_and_missing_ticket_are_distinct(self):
        self.install_reader_fixture()
        source = self.root / "source.nsp"
        source.write_bytes(b"outdated fixture")
        _, terminal = self.worker("import", paths=[str(source)])
        self.assert_failure(terminal, "keys_missing")
        self.import_keys()
        _, terminal = self.worker("import", paths=[str(source)])
        self.assert_failure(terminal, "keys_outdated")
        source.write_bytes(b"ticket fixture")
        _, terminal = self.worker("import", paths=[str(source)])
        diagnostic = self.assert_failure(terminal, "ticket_missing")
        self.assertNotIn("prod.keys", diagnostic["summary"])
        self.assertNotIn("重新导出", diagnostic["suggestion"])

    def test_identity_from_content_ids_without_reading_whole_file(self):
        # 11-09 玩家反馈「只是添加也要跑几分钟」：读取器给了 content ID 就按 titleId / 类型 / 版本 / 排序后的 content ID / 文件大小算身份，不整份算 SHA-256；
        #   期望按 library.source_identity 文件头的公式手算（不调被测函数）
        import hashlib, json
        self.import_keys(); self.install_reader_fixture()
        source = self.root / "big.nsp"; source.write_bytes(b"content id fixture" + b"x" * 1000)
        _, done = self.worker("import", paths=[str(source)])
        self.assertEqual(done["state"], "succeeded")
        rows = [["0100000000000000", "base", 0, ["000000000000000000000000000000aa", "000000000000000000000000000000bb"]]]
        want = hashlib.sha256(json.dumps(dict(v=1, packages=rows, size=1018), separators=(",", ":")).encode()).hexdigest()
        [p] = self.c.catalog()["games"][0]["packages"]
        self.assertEqual(p["sha256"], want)
        self.assertNotEqual(p["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
        # 同一份再加一次：判重，不多一条
        _, done = self.worker("import", paths=[str(source)])
        self.assertEqual(done["result"]["imports"][0]["duplicate"], True)
        self.assertEqual(len(self.c.catalog()["games"][0]["packages"]), 1)

    def test_old_whole_file_record_still_dedups_and_keeps_sha(self):
        # 老库里的记录是整份 SHA-256：同一份文件（同 titleId / 类型 / 版本 / 大小）再加，要判重且不改老记录的 sha256（它进构建键，改了老游戏要冷重出）
        import hashlib
        self.import_keys(); self.install_reader_fixture()
        source = self.root / "old.nsp"; source.write_bytes(b"content id fixture" + b"y" * 100)
        old_sha = hashlib.sha256(source.read_bytes()).hexdigest()
        atomic(self.c.library / "catalog.json", dict(schema=1, games=[dict(id="0100000000000000", name="Synthetic", display="", hidden=False, packages=[
            dict(titleId="0100000000000000", baseId="0100000000000000", type="base", version=0, name="Synthetic", path=str(source), sha256=old_sha, size=source.stat().st_size)])]))
        before = library.input_identity(self.c.catalog()["games"][0])   # 构建键（出包按它判要不要重出）
        _, done = self.worker("import", paths=[str(source)])
        self.assertEqual(done["result"]["imports"][0]["duplicate"], True)
        [p] = self.c.catalog()["games"][0]["packages"]
        self.assertEqual(p["sha256"], old_sha)
        self.assertEqual(library.input_identity(self.c.catalog()["games"][0]), before)

    def test_reader_without_content_ids_falls_back_to_whole_file_hash(self):
        import hashlib
        self.import_keys(); self.install_reader_fixture()
        source = self.root / "plain.nsp"; source.write_bytes(b"synthetic game fixture")
        self.worker("import", paths=[str(source)])
        [p] = self.c.catalog()["games"][0]["packages"]
        self.assertEqual(p["sha256"], hashlib.sha256(b"synthetic game fixture").hexdigest())

    def test_import_folder_follows_symlinked_files_and_folders(self):
        # 原件挪走、原位置留软链（10-26 维护者把 Downloads 原件挪外置盘）：文件夹里指向文件的软链、软链目录都认；软链成环不转圈
        self.import_keys(); self.install_reader_fixture()
        real = self.root / "moved"; (real / "sub").mkdir(parents=True)
        (real / "source.nsp").write_bytes(b"synthetic game fixture")
        (real / "sub" / "other.nsp").write_bytes(b"synthetic game fixture 2")
        picked = self.root / "downloads"; picked.mkdir()
        (picked / "source.nsp").symlink_to(real / "source.nsp")
        (picked / "sub").symlink_to(real / "sub")
        (picked / "loop").symlink_to(picked)
        result, done = self.worker("import", paths=[str(picked)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(done["state"], "succeeded")
        # 原件原地只读（11-02）：不复制进游戏库，清单记原件的绝对路径（软链所在的路径，出包时照样跟到目标）
        self.assertFalse((self.c.library / "sources").exists())
        paths = sorted(p["path"] for g in self.c.catalog()["games"] for p in g["packages"])
        self.assertEqual(paths, sorted({str(picked.resolve() / "source.nsp"), str(picked.resolve() / "sub" / "other.nsp")}))
        self.assertEqual(sorted(Path(p).read_bytes() for p in paths), [b"synthetic game fixture", b"synthetic game fixture 2"])

    def test_readd_original_after_managed_copy_removed(self):
        # beta.3 及以前复制进游戏库的托管副本：副本还在时重放原件照旧用副本；「删除托管 NSP 副本」之后重放原件，改记成原件位置（否则这款一直缺文件）
        import json
        self.import_keys(); self.install_reader_fixture()
        original = self.root / "drive" / "game.nsp"; original.parent.mkdir(); original.write_bytes(b"synthetic game fixture")
        def run():
            result, done = self.worker("import", paths=[str(original)])
            self.assertEqual(done["state"], "succeeded", result.stderr)
            return [p["path"] for g in self.c.catalog()["games"] for p in g["packages"]]
        [path] = run()
        managed = self.c.library / "sources" / (Path(path).name + ".managed")
        managed.parent.mkdir(); managed.write_bytes(original.read_bytes())
        catalog = json.loads((self.c.library / "catalog.json").read_text())
        for g in catalog["games"]:
            for p in g["packages"]:
                p["path"] = str(managed.relative_to(self.c.library))
        (self.c.library / "catalog.json").write_text(json.dumps(catalog))
        self.assertEqual(run(), [str(managed.relative_to(self.c.library))])
        managed.unlink()
        self.assertEqual(run(), [str(original.resolve())])


class PartialImportTests(JobCase):
    def setUp(self):
        super().setUp()
        self.import_keys()
        self.install_reader_fixture()

    def source(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return str(path.resolve())

    def run_import(self, paths, **options):
        _, terminal = self.worker("import", paths=paths, **options)
        return terminal

    def test_opt_in_continues_and_distinguishes_same_name(self):
        a = self.source("a/game.nsp", b"unreadable fixture")
        b = self.source("b/game.nsp", b"complete fixture")
        failed = self.run_import([a, b])
        self.assert_failure(failed, "content_unreadable")
        self.assertEqual(self.c.catalog()["games"], [])
        done = self.run_import([a, b], partialResults=True)
        self.assertEqual(done["state"], "succeeded")
        first, second = done["result"]["sources"]
        self.assertEqual((first["status"], second["status"]), ("failed", "imported"))
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(first["path"], a)
        self.assertEqual(second["path"], b)
        self.assertIn("Check keys", first["diagnostic"])
        self.assertNotIn("synthetic-bearer", first["diagnostic"])

    def test_scan_errors_are_per_source_and_all_failed_leaves_keys_unverified(self):
        a = self.source("a.nsp", b"truncated fixture")
        b = str((self.root / "b.nsp").resolve())
        c = self.source("c.txt", b"unsupported")
        done = self.run_import([a, b, c], partialResults=True)
        self.assertEqual(done["state"], "succeeded")
        self.assertEqual([row["code"] for row in done["result"]["sources"]],
                         ["content_truncated", "input_missing", "input_type_unsupported"])
        diagnostic = done["result"]["sources"][0]["diagnostic"]
        self.assertIn("Beginning of synthetic reader diagnostic", diagnostic)
        self.assertGreater(len(diagnostic), 3000)
        self.assertNotIn("synthetic-bearer", diagnostic)
        self.assertNotIn("Beginning of synthetic reader diagnostic", done["result"]["sources"][1]["diagnostic"])
        status = json.loads((self.c.private / "keys/status.json").read_text())
        self.assertFalse(status["verified"])

    def test_per_source_diagnostic_has_a_cap_and_keeps_both_ends(self):
        from library import IMPORT_DIAGNOSTIC_LIMIT
        a = self.source("a.nsp", b"huge truncated fixture")
        done = self.run_import([a], partialResults=True)
        diagnostic = done["result"]["sources"][0]["diagnostic"]
        self.assertLessEqual(len(diagnostic), IMPORT_DIAGNOSTIC_LIMIT)
        self.assertGreater(len(diagnostic), 3000)
        self.assertIn("TAIL-MARK", diagnostic)
        saved = json.loads((self.c.library / "logs" / "jobs" / (done["taskId"] + ".json")).read_text())
        self.assertLessEqual(len(saved["sources"][0]["diagnostic"]), IMPORT_DIAGNOSTIC_LIMIT)

    def test_io_error_on_one_source_does_not_block_other_files(self):
        from jobcase import JOBS
        a = self.source("a.nsp", b"unreadable bytes fixture")
        b = self.source("b.nsp", b"complete fixture")
        entry = self.root / "source-io.py"
        entry.write_text(f"""import errno, sys
sys.path.insert(0, {str(JOBS)!r})
import main
digest = main.library.digest
def read(ctx, *args):
    if ctx.name == 'a.nsp':
        raise OSError(errno.EIO, 'synthetic source read error')
    return digest(ctx, *args)
main.library.digest = read
raise SystemExit(main.main())
""")
        _, done = self.worker("import", paths=[a, b], partialResults=True, entrypoint=entry)
        self.assertEqual(done["state"], "succeeded")
        first, second = done["result"]["sources"]
        self.assertEqual(first["code"], "input_unreadable")
        self.assertEqual(second["status"], "imported")

    def test_reader_disk_full_is_batch_failure(self):
        a = self.source("a.nsp", b"disk full fixture")
        b = self.source("b.nsp", b"complete fixture")
        failed = self.run_import([a, b], partialResults=True)
        self.assert_failure(failed, "disk_full")
        self.assertEqual([row["status"] for row in failed["result"]["sources"]], ["failed", "unprocessed"])
        self.assertEqual(self.c.catalog()["games"], [])

    def test_abort_preserves_source_states_in_terminal_event_and_record(self):
        from jobcase import JOBS
        a = self.source("a.nsp", b"complete fixture")
        b = self.source("b.nsp", b"stop fixture")
        c = self.source("c.nsp", b"later fixture")
        entry = self.root / "abort.py"
        for reason in ("disk_full", "tool_missing", "keys_invalid", "library", "cancelled"):
            with self.subTest(reason=reason):
                entry.write_text(f"""import sys
sys.path.insert(0, {str(JOBS)!r})
import main
from core import Cancelled, Failure
inspect = main.library.inspect
def stop(ctx, source, *args, **kwargs):
    if source.name == 'b.nsp':
        reason = {reason!r}
        if reason == 'cancelled':
            raise Cancelled()
        raise Failure('synthetic abort', code='job_failed' if reason == 'library' else reason)
    return inspect(ctx, source, *args, **kwargs)
main.library.inspect = stop
raise SystemExit(main.main())
""")
                _, event = self.worker("import", paths=[a, b, c], partialResults=True, entrypoint=entry)
                self.assertEqual(event["state"], "cancelled" if reason == "cancelled" else "failed")
                rows = event["result"]["sources"]
                self.assertIn(rows[0]["status"], ("imported", "duplicate"))
                self.assertEqual(rows[1]["status"], "unprocessed" if reason == "cancelled" else "failed")
                self.assertEqual(rows[2]["status"], "unprocessed")
                saved = json.loads((self.c.library / "logs" / "jobs" / (event["taskId"] + ".json")).read_text())
                self.assertEqual(saved["sources"], rows)

    def test_multi_package_source_rolls_back_if_catalog_commit_fails(self):
        from jobcase import JOBS
        bad = self.source("a.nsp", b"multi fixture")
        good = self.source("b.nsp", b"complete fixture")
        entry = self.root / "rollback.py"
        entry.write_text(f"""import sys
sys.path.insert(0, {str(JOBS)!r})
import main
from core import Failure
inspect = main.library.inspect
def multi(ctx, source, *args, **kwargs):
    packages = inspect(ctx, source, *args, **kwargs)
    if source.name == 'a.nsp':
        packages.append(dict(packages[0], titleId='0100000000000800', type='update'))
    return packages
main.library.inspect = multi
atomic = main.library.atomic
def fail_once(path, value):
    if path.name == 'catalog.json' and any(p['type'] == 'update' for g in value['games'] for p in g['packages']):
        raise Failure('synthetic commit failure', code='content_invalid')
    atomic(path, value)
main.library.atomic = fail_once
raise SystemExit(main.main())
""")
        _, done = self.worker("import", paths=[bad, good], partialResults=True, entrypoint=entry)
        self.assertEqual(done["state"], "succeeded")
        self.assertEqual([row["status"] for row in done["result"]["sources"]], ["failed", "imported"])
        [game] = self.c.catalog()["games"]
        self.assertEqual([package["path"] for package in game["packages"]], [good])


    # ---- 默认路径（不开 partialResults）与基线一致 ----

    def record(self, terminal):
        return json.loads((self.c.library / "logs" / "jobs" / (terminal["taskId"] + ".json")).read_text())

    def test_default_path_result_and_record_have_no_sources(self):
        good = self.source("good.nsp", b"complete fixture")
        done = self.run_import([good])
        self.assertEqual(done["state"], "succeeded")
        self.assertEqual(set(done["result"]), {"imports"})
        self.assertNotIn("sources", self.record(done))
        bad = self.source("bad.nsp", b"unreadable fixture")
        failed = self.run_import([bad])
        self.assert_failure(failed, "content_unreadable")
        self.assertEqual(failed["result"], {})
        self.assertNotIn("sources", self.record(failed))
        entry = self.root / "cancel.py"
        entry.write_text(f"""import sys
sys.path.insert(0, {str(JOBS)!r})
import main
from core import Cancelled
def stop(*args, **kwargs):
    raise Cancelled()
main.library.inspect = stop
raise SystemExit(main.main())
""")
        _, cancelled = self.worker("import", paths=[good], entrypoint=entry)
        self.assertEqual(cancelled["state"], "cancelled")
        self.assertEqual(cancelled["result"], {})
        self.assertNotIn("sources", self.record(cancelled))

    def test_default_path_library_write_errors_keep_explain_codes(self):
        # 基线：catalog / 封面写入的非盘满 OSError 走 explain()（permission_denied / input_missing），不统一成 job_failed
        cover = self.source("cover.nsp", b"chinese title fixture")
        plain = self.source("plain.nsp", b"complete fixture")
        cases = (("catalog", "PermissionError(13, 'synthetic')", plain, "permission_denied"),
                 ("catalog", "FileNotFoundError(2, 'synthetic')", plain, "input_missing"),
                 ("cover", "PermissionError(13, 'synthetic')", cover, "permission_denied"),
                 ("cover", "FileNotFoundError(2, 'synthetic')", cover, "input_missing"))
        entry = self.root / "write-error.py"
        for target, error, source, code in cases:
            with self.subTest(target=target, code=code):
                entry.write_text(f"""import sys
sys.path.insert(0, {str(JOBS)!r})
import main
atomic = main.library.atomic
def fail_catalog(path, value):
    if path.name == 'catalog.json' and any(g['packages'] for g in value['games']):
        raise {error}
    atomic(path, value)
def fail_cover(*args, **kwargs):
    raise {error}
if {target!r} == 'catalog':
    main.library.atomic = fail_catalog
else:
    main.library._copy_cover = fail_cover
raise SystemExit(main.main())
""")
                _, failed = self.worker("import", paths=[source], entrypoint=entry)
                self.assert_failure(failed, code)
                self.assertEqual(failed["result"], {})

    # ---- 逐来源模式 ----

    def test_sources_follow_caller_order_and_folders_expand_in_place(self):
        late = self.source("z-late.nsp", b"complete fixture")
        self.source("folder/b.nsp", b"unreadable fixture")
        self.source("folder/a.nsp", b"unreadable fixture")
        early = self.source("a-early.nsp", b"unreadable fixture")
        folder = str((self.root / "folder").resolve())
        done = self.run_import([late, folder, early], partialResults=True)
        self.assertEqual([Path(row["path"]).name for row in done["result"]["sources"]],
                         ["z-late.nsp", "a.nsp", "b.nsp", "a-early.nsp"])

    def test_catalog_records_resolved_path_and_row_keeps_given_path(self):
        real = Path(self.source("real/game.nsp", b"complete fixture"))
        link = self.root / "link"
        link.symlink_to(real.parent, target_is_directory=True)
        given = str(link / "game.nsp")
        done = self.run_import([given], partialResults=True)
        [row] = done["result"]["sources"]
        self.assertEqual((row["path"], row["status"]), (given, "imported"))
        [game] = self.c.catalog()["games"]
        self.assertEqual([p["path"] for p in game["packages"]], [str(real)])

    def test_batch_diagnostics_have_a_total_cap(self):
        from library import IMPORT_DIAGNOSTIC_LIMIT, IMPORT_DIAGNOSTIC_TOTAL
        paths = [self.source(f"huge{i}.nsp", b"huge truncated fixture") for i in range(6)]
        done = self.run_import(paths, partialResults=True)
        rows = done["result"]["sources"]
        self.assertEqual([row["code"] for row in rows], ["content_truncated"] * 6)
        full = [row for row in rows if "TAIL-MARK" in row["diagnostic"]]
        self.assertEqual(len(full), IMPORT_DIAGNOSTIC_TOTAL // IMPORT_DIAGNOSTIC_LIMIT)
        self.assertLessEqual(sum(len(row["diagnostic"]) for row in full), IMPORT_DIAGNOSTIC_TOTAL)
        for row in rows[len(full):]:
            self.assertLess(len(row["diagnostic"]), 1000)
            self.assertIn(row["log"], row["diagnostic"])
            self.assertTrue((self.c.library / "logs" / row["log"]).is_file())
        self.assertEqual(self.record(done)["sources"], rows)


class NupImportTests(JobCase):
    """Switch 2 NUP 容器导入（11-07）：pack/ns2_nup.py 认（不看扩展名）、不要 prod.keys；Switch 1 原件照旧要密钥、走读取器"""

    def nup(self, path, nacp=True):
        path.parent.mkdir(parents=True, exist_ok=True)
        synthetic_nup(path, APP, [
            (0, APP, 0, {"main": b"\x01" * 300, "main.npdm": synthetic_npdm(APP)}, {"a.bin": b"base"}, {}),
            (1, APP | 0x800, 131072, {"main": b"\x02" * 500, "main.npdm": synthetic_npdm(APP)}, {"a.bin": b"upd"},
             {2: synthetic_nacp("Ocarina T", "1.2.0"), 3: b"JPGICON"} if nacp else {}),
            (2, APP + 0x1001, 0, None, {"d.bin": b"DLC"}, {})])
        return path

    def test_nup_imported_without_prod_keys(self):
        # ① 没导 prod.keys、读取器也没装：.nup 照样收进库——本体 / 最新更新 / DLC 各一条、同一原件、format=nup；
        #   标题与显示版本取容器里的 NACP，封面取容器里的图标；密钥状态不动（没用到密钥）
        src = self.nup(self.root / "drive" / "Zelda T [0400486024dec000][v131072].nup")
        self.assertFalse(self.c.keys.exists())
        result, done = self.worker("import", paths=[str(src)])
        self.assertEqual(done["state"], "succeeded", result.stderr + str(done.get("diagnostics")))
        [game] = self.c.catalog()["games"]
        rows = sorted((p["type"], p["titleId"], p["version"], p["path"], p.get("format"), p["display"]) for p in game["packages"])
        self.assertEqual(rows, sorted([("base", "0400486024DEC000", 0, str(src.resolve()), "nup", "1.2.0"),
                                       ("update", "0400486024DEC800", 131072, str(src.resolve()), "nup", "1.2.0"),
                                       ("dlc", "0400486024DED001", 0, str(src.resolve()), "nup", "1.2.0")]))
        self.assertEqual((game["id"], game["name"]), ("0400486024DEC000", "Ocarina T"))
        self.assertEqual((self.c.library / "games/Ocarina T/cover.jpg").read_bytes(), b"JPGICON")   # 文件夹按标题（英文）
        self.assertFalse((self.c.private / "keys/status.json").exists())
        # 没带控制分区：标题取文件名去掉「 [程序号][版本]」（同一键出包）
        bare = self.nup(self.root / "drive2" / "Bare Name [0400486024dec000].nup", nacp=False)
        (self.c.library / "catalog.json").write_text('{"schema": 1, "games": []}')
        result, done = self.worker("import", paths=[str(bare)])
        self.assertEqual(done["state"], "succeeded", result.stderr)
        self.assertEqual(self.c.catalog()["games"][0]["name"], "Bare Name")

    def test_multi_package_nup_counts_as_one_source(self):
        src = self.nup(self.root / "multi.nup")
        _, done = self.worker("import", paths=[str(src)], partialResults=True)
        self.assertEqual(len(done["result"]["imports"]), 3)
        [source] = done["result"]["sources"]
        self.assertEqual(source["status"], "imported")
        self.assertFalse(source["needsKeys"])

    def test_nup_read_error_keeps_content_invalid_on_the_default_path(self):
        # 默认路径（不开 partialResults）：NUP 读错误仍是 content_invalid，与改动前一致
        from jobcase import JOBS
        src = self.nup(self.root / "bad.nup")
        entry = self.root / "nup-io.py"
        entry.write_text(f"""import errno, sys
sys.path.insert(0, {str(JOBS)!r})
import main
engine_tool = main.library.engine_tool
def read_error(*args, **kwargs):
    raise OSError(errno.EIO, 'synthetic NUP read error')
def tool(ctx, name):
    module = engine_tool(ctx, name)
    if name == 'ns2_nup':
        module.scan = read_error
    return module
main.library.engine_tool = tool
raise SystemExit(main.main())
""")
        _, done = self.worker("import", paths=[str(src)], entrypoint=entry)
        self.assert_failure(done, "content_invalid")

    def test_fake_nup_rejected(self):
        # ② 扩展名是 .nup 但不是 NUP（文件头不是 NUP 魔数）：直接选它照旧报「不支持的类型」；文件夹里混着真假两个 → 只收真的
        fake = self.root / "drive" / "fake.nup"; fake.parent.mkdir(); fake.write_bytes(b"not a NUP container at all")
        result, done = self.worker("import", paths=[str(fake)])
        self.assert_failure(done, "input_type_unsupported")
        real = self.nup(self.root / "drive" / "real.nup")
        result, done = self.worker("import", paths=[str(self.root / "drive")])
        self.assertEqual(done["state"], "succeeded", result.stderr + str(done.get("diagnostics")))
        self.assertEqual({p["path"] for g in self.c.catalog()["games"] for p in g["packages"]}, {str(real.resolve())})

    def test_switch1_nsp_import_unchanged(self):
        # ④ Switch 1 NSP：没有密钥照旧 keys_missing；有密钥走读取器（--inspect 的元数据原样记，不带 format），密钥状态记已验证
        import json
        nsp = self.root / "drive" / "game.nsp"; nsp.parent.mkdir(); nsp.write_bytes(b"synthetic game fixture")
        result, done = self.worker("import", paths=[str(nsp)])
        self.assert_failure(done, "keys_missing")
        self.import_keys(); self.install_reader_fixture()
        result, done = self.worker("import", paths=[str(nsp)])
        self.assertEqual(done["state"], "succeeded", result.stderr)
        [game] = self.c.catalog()["games"]
        [pkg] = game["packages"]
        self.assertEqual((game["id"], pkg["type"], pkg["path"], "format" in pkg, pkg["guestIsa"]),
                         ("0100000000000000", "base", str(nsp.resolve()), False, "aarch64"))
        self.assertTrue(json.loads((self.c.private / "keys/status.json").read_text())["verified"])


    def test_chinese_title_folder_uses_english_name(self):
        # 读取器给的界面名是中文、英文名另给：catalog 名字照中文、记 englishName；这款的文件夹按英文名建，封面在里面（不是游戏号文件夹）
        nsp = self.root / "drive" / "silksong.nsp"; nsp.parent.mkdir(); nsp.write_bytes(b"chinese title fixture")
        self.import_keys(); self.install_reader_fixture()
        result, done = self.worker("import", paths=[str(nsp)])
        self.assertEqual(done["state"], "succeeded", result.stderr)
        [game] = self.c.catalog()["games"]
        self.assertEqual((game["name"], game["englishName"]), ("空洞骑士：丝之歌", "Hollow Knight: Silksong"))
        self.assertEqual("englishName" in game["packages"][0], False)
        self.assertEqual((self.c.library / "games" / "Hollow Knight Silksong" / "cover.jpg").read_bytes(), b"ICON")
        self.assertFalse((self.c.library / "games" / "0100000000000000").exists())

if __name__ == "__main__":
    unittest.main()
