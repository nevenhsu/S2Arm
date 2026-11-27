# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Independent Windows environment contracts; fake probes are not device proof."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "pack/jobs/winenv.py"
sys.path.insert(0, str(ROOT / "pack/jobs"))   # 用到真 library / toolchain 的几条（Mac 预检同一份规则）：从仓库根跑时也找得到
sys.path.insert(0, str(Path(__file__).resolve().parent))   # tests/winenv_test.sh 用 python3 -I 跑，-I 不把脚本目录放进 sys.path：jobcase 要显式找
import jobcase  # noqa: E402（作业上下文替身统一工厂 jobcase.fake_context）


class FakeProbe:
    def __init__(self):
        self.calls = []
        self.values = {
            "host": {"system": "Windows", "bits": 64},
            "python": {"version": [3, 13, 5], "packages": {
                "capstone": {"version": "5.0.7"},
                "lz4": {"version": "4.4.5"},
                "numpy": {"version": "2.4.2"},
                "Pillow": {"version": "12.3.0"},
            }},
            "pack": [["prebuilt-dotnet", True, "预编 C# 件齐", "ok", None]],
            "impactor": {"verdict": "ok", "version": "2.7.2", "tested": "2.7.2",
                         "download": "https://github.com/claration/impactor/releases/tag/v2.7.2", "plumesign": True},
            "service": {"state": 4},
            "file": {"exists": True, "regular": True},
            "disk": {"free": 1000},
            "metal": FileNotFoundError(),
            "usbmux_port": {"connected": True},
            "usb_devices": {"USB": 0, "Network": 0},
            "icloud": {"packages": [], "legacy": []},
            "apple_devices": {"packages": []},
        }

    def __call__(self, kind, **options):
        self.calls.append((kind, options))
        result = self.values[kind]
        if isinstance(result, Exception):
            raise result
        return result


class EnvironmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("winenv_under_test", SOURCE)
        cls.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.mod)

    def setUp(self):
        self.probe = FakeProbe()
        self.request = {"schema": 1, "targetDir": ".", "requiredFreeBytes": 1000}
        original = self.mod._text
        fixtures = {
            self.mod.RUNTIME: "capstone==5.0.7\nlz4==4.4.5\nnumpy==2.4.2\nPillow==12.3.0\n",
        }
        override = patch.object(self.mod, "_text", side_effect=lambda name: fixtures.get(name, None)
                                if name in fixtures else original(name))
        override.start()
        self.addCleanup(override.stop)

    def report(self, **changes):
        return self.mod.check(dict(self.request, **changes), probe=self.probe)

    def item(self, report, ident):
        rows = [c for c in report["checks"] if c["id"] == ident]
        self.assertEqual(len(rows), 1, "检查项缺失或重复：" + ident)
        return rows[0]

    def test_complete_inventory_but_unpublished_contracts_never_ready(self):
        report = self.report()
        self.assertEqual(report["schema"], 1)
        self.assertFalse(report["ready"])
        ids = [c["id"] for c in report["checks"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue({"host", "python", "pack.prebuilt-dotnet", "impactor", "metal",
                         "apple_driver", "anisette", "disk"} <= set(ids))
        self.assertFalse({"dotnet_sdk", "rewriter", "sidecar"} & set(ids))   # 随包运行时 + 预编件之后作废的三项
        for ident in ("apple_usb", "anisette"):
            self.assertEqual(self.item(report, ident)["status"], "unverified")
        self.assertEqual(self.item(report, "metal")["status"], "missing")
        self.assertTrue(self.item(report, "apple_driver")["ok"])

    def test_exact_shared_python_packages(self):
        report = self.report()
        for name in ("capstone", "lz4", "numpy", "Pillow"):
            c = self.item(report, "python." + name)
            self.assertTrue(c["ok"])
            self.assertIn("requirements-runtime.txt", c["source"])

    def test_python_minimum_from_existing_preflight(self):
        # 下限按 toolchain.PYTHON_MINIMUM 现算（AST 直接读那一行，不经被测代码）：低一个小版本的最后一个补丁版判 mismatch，正好等于下限的 .0 判 ok。
        #   原先写死 [3, 10, 99] / [3, 11, 0]，下限改 3.12 后这里成了又一份要跟着改的拷贝（11-26）
        import ast
        tree = ast.parse((ROOT / "pack/jobs/toolchain.py").read_text(encoding="utf-8"))
        low = next(tuple(ast.literal_eval(n.value)) for n in ast.walk(tree)
                   if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "PYTHON_MINIMUM" for t in n.targets))
        self.probe.values["python"]["version"] = [low[0], low[1] - 1, 99]
        self.assertEqual(self.item(self.report(), "python")["status"], "mismatch")
        self.probe.values["python"]["version"] = [low[0], low[1], 0]
        self.assertTrue(self.item(self.report(), "python")["ok"])

    def test_configured_interpreter_is_the_one_probed(self):
        self.report(python="C:/中文 空格/python.exe")
        call = next(opts for kind, opts in self.probe.calls if kind == "python")
        self.assertEqual(call["executable"], "C:/中文 空格/python.exe")
        self.assertEqual(call["modules"]["Pillow"], "PIL")

    def test_missing_and_wrong_packages_are_all_named(self):
        self.probe.values["python"]["packages"] = {
            "capstone": {"version": "5.0.6"},
            "lz4": {"error": "ModuleNotFoundError"}, "numpy": {"version": "0"},
            "Pillow": {"version": "13.0.0"}}
        report = self.report()
        self.assertEqual([self.item(report, "python." + n)["status"] for n in
                          ("capstone", "lz4", "numpy", "Pillow")],
                         ["mismatch", "missing", "mismatch", "mismatch"])

    def test_python_failure_does_not_hide_other_checks(self):
        self.probe.values["python"] = FileNotFoundError()
        report = self.report()
        self.assertEqual(self.item(report, "python")["status"], "missing")
        self.assertEqual(self.item(report, "disk")["status"], "ok")
        self.assertIn("impactor", [c["id"] for c in report["checks"]])

    def test_python_failure_is_one_item_with_the_real_reason(self):
        self.probe.values["python"] = self.mod.ProbeError("失败", nonzero=True, detail="退出码 1：ImportError: DLL load failed")
        report = self.report()
        self.assertEqual([c["id"] for c in report["checks"] if c["id"].startswith("python.")], [])
        self.assertEqual(self.item(report, "python")["status"], "error")
        self.assertIn("ImportError: DLL load failed", self.item(report, "python")["message"])

    def test_exit_reason_names_missing_runtime_and_stderr(self):
        self.assertIn("Visual C++", self.mod._exit_reason(-1073741515, b""))       # 0xC0000135
        self.assertIn("Visual C++", self.mod._exit_reason(3221225595, b""))        # 0xC000007B
        reason = self.mod._exit_reason(1, ("Traceback\n" + str(Path.home()) + "/x: boom\n").encode())
        self.assertEqual(reason, "退出码 1：~/x: boom")

    def test_impactor_uses_the_bundled_copy_when_launcher_says_where(self):
        with tempfile.TemporaryDirectory() as tmp:
            sys.path.insert(0, str(Path(self.mod.__file__).resolve().parents[2] / "pack/jobs"))
            import impactor
            folder = Path(tmp) / impactor.BUNDLE_DIR; folder.mkdir(parents=True)
            native = self.mod.NativeProbe()
            (folder / impactor.IMPACTOR_EXE).write_bytes(b"x"); (folder / impactor.PLUMESIGN_EXE).write_bytes(b"x")
            found = native("impactor", resources=tmp)
            self.assertEqual((found["verdict"], found["plumesign"], found["bundled"]), ("ok", True, True))

    def test_signing_tools_never_block_import_or_export(self):
        for ident, kind, value in (("impactor", "impactor", {"verdict": "missing", "version": None, "tested": "2.7.2", "download": "https://github.com/claration/impactor/releases/tag/v2.7.2", "plumesign": False}),
                                   ("anisette", "icloud", {"packages": [], "legacy": []})):
            with self.subTest(ident=ident):
                self.probe = FakeProbe(); self.capabilities(); self.probe.values[kind] = value
                report = self.report()
                item = self.item(report, ident)
                self.assertFalse(item["ok"])
                self.assertFalse(item["blocking"])
                self.assertEqual(item["requiredFor"], ["install"])
                self.assertTrue(report["ready"])

    def test_unknown_results_only_advise_but_definite_missing_blocks(self):
        self.capabilities()
        self.probe.values["python"] = self.mod.ProbeError("超时", detail="90 秒内没有运行完")
        report = self.report()
        self.assertFalse(self.item(report, "python")["blocking"])
        self.assertTrue(report["ready"])
        self.probe = FakeProbe(); self.capabilities()
        self.probe.values["python"] = FileNotFoundError()   # 解释器起不来也是认不出
        self.assertTrue(self.report()["ready"])
        self.probe = FakeProbe(); self.capabilities()
        self.probe.values["python"]["packages"]["lz4"] = {"error": "ImportError"}   # 版本认不出
        self.assertTrue(self.report()["ready"])
        self.probe = FakeProbe(); self.capabilities()
        self.probe.values["python"]["packages"]["lz4"] = {"error": "ModuleNotFoundError"}   # 确实缺
        report = self.report()
        self.assertTrue(self.item(report, "python.lz4")["blocking"])
        self.assertFalse(report["ready"])

    def test_damaged_import_is_not_reported_as_absent(self):
        self.probe.values["python"]["packages"]["lz4"] = {"error": "ImportError"}
        self.assertEqual(self.item(self.report(), "python.lz4")["status"], "error")

    def test_minimum_definition_is_read_instead_of_duplicated(self):
        # 读的是预检那一个常量（toolchain.PYTHON_MINIMUM，剃刀 H11），不另抄：把它改成 3.99，环境检查跟着判不符
        import toolchain
        with patch.object(toolchain, "PYTHON_MINIMUM", (3, 99)):
            self.assertEqual(self.item(self.report(), "python")["status"], "mismatch")

    def test_timeout_does_not_hide_inventory_or_leak_exception(self):
        self.probe.values["python"] = subprocess.TimeoutExpired("secret=ABC", 3)
        report = self.report()
        self.assertEqual(self.item(report, "python")["status"], "error")
        self.assertNotIn("ABC", json.dumps(report))
        self.assertIn("disk", [c["id"] for c in report["checks"]])

    def test_service_access_denied_is_not_absent(self):   # 端口不通时才查服务；读不到服务照实写「读不到」，不漏私密原文
        self.probe.values["usbmux_port"] = {"connected": False}
        self.probe.values["service"] = PermissionError("device_id=PRIVATE")
        report = self.report()
        c = self.item(report, "apple_driver")
        self.assertEqual(c["status"], "missing"); self.assertIn("服务 Apple Mobile Device Service：读不到", c["message"])
        self.assertNotIn("PRIVATE", json.dumps(report))

    def test_service_stopped_is_not_ready(self):
        self.probe.values["usbmux_port"] = {"connected": False}
        self.probe.values["service"] = {"state": 1}
        c = self.item(self.report(), "apple_driver")
        self.assertEqual(c["status"], "mismatch"); self.assertIn("服务 Apple Mobile Device Service：已停止", c["message"])

    def test_service_absent_is_named(self):
        self.probe.values["usbmux_port"] = {"connected": False}
        self.probe.values["service"] = FileNotFoundError()
        c = self.item(self.report(), "apple_driver")
        self.assertEqual(c["status"], "missing")
        for place in ("本机usbmux端口 127.0.0.1:27015：连不上", "服务 Apple Mobile Device Service：没装", "商店版 Apple Devices：没装"):
            self.assertIn(place, c["message"])   # 「复制错误摘要」带上查过的三处

    def test_store_apple_devices_without_legacy_service_is_ready(self):   # #424：商店版不注册老式服务名，端口通即就绪
        self.probe.values["service"] = FileNotFoundError()
        self.probe.values["apple_devices"] = {"packages": [{"Name": "AppleInc.AppleDevices", "Status": "Ok", "Version": "1.0.0.0"}]}
        report = self.report()
        self.assertTrue(self.item(report, "apple_driver")["ok"])
        self.assertFalse(any(k == "service" for k, _ in self.probe.calls))   # 端口通就不再看服务名

    def test_store_apple_devices_port_closed_says_open_app(self):
        self.probe.values["usbmux_port"] = {"connected": False}
        self.probe.values["service"] = FileNotFoundError()
        self.probe.values["apple_devices"] = {"packages": [{"Name": "AppleInc.AppleDevices", "Status": "Ok", "Version": "1.0.0.0"}]}
        c = self.item(self.report(), "apple_driver")
        self.assertEqual(c["status"], "missing")
        self.assertIn("打开一次Apple Devices", c["message"]); self.assertIn("商店版 Apple Devices：已装（1.0.0.0 Ok）", c["message"])

    def test_missing_binary_has_manual_hint(self):
        self.probe.values["file"] = {"exists": False, "regular": False}
        c = self.item(self.report(metal="missing.exe"), "metal")
        self.assertEqual(c["status"], "missing")
        self.assertIn("https://developer.apple.com/download/", c["help"])

    def test_disk_exact_boundary_and_shortage(self):
        self.assertTrue(self.item(self.report(), "disk")["ok"])
        self.probe.values["disk"] = {"free": 999}
        self.assertEqual(self.item(self.report(), "disk")["status"], "mismatch")

    def test_missing_budget_never_says_enough(self):
        # 没选游戏（无预算、无 catalog / gameId）：空间项不列、不拖 ready；只给 catalog 不给 gameId 仍是 unverified（不判够）
        self.request.pop("requiredFreeBytes")
        self.assertNotIn("disk", [c["id"] for c in self.report()["checks"]])
        self.assertEqual(self.item(self.report(catalog="catalog.json"), "disk")["status"], "unverified")

    def test_disk_error_does_not_discard_other_results(self):
        self.probe.values["disk"] = OSError("PRIVATE_PATH")
        report = self.report()
        self.assertEqual(self.item(report, "disk")["status"], "error")
        self.assertTrue(self.item(report, "python")["ok"])
        self.assertNotIn("PRIVATE_PATH", json.dumps(report))

    def test_implicit_public_anisette_rejected(self):
        c = self.item(self.report(anisette={"source": "public",
                         "endpoint": "https://example.invalid"}), "anisette")
        self.assertEqual(c["status"], "mismatch")

    def test_explicit_public_source_is_not_health_proof(self):
        c = self.item(self.report(anisette={"source": "public", "allowPublic": True,
                         "endpoint": "https://example.invalid"}), "anisette")
        self.assertEqual(c["status"], "user-selected")

    def test_self_hosted_never_calls_network_or_outputs_endpoint(self):
        report = self.report(anisette={"source": "self-hosted",
                             "endpoint": "https://private.invalid"}, offline=True)
        self.assertEqual(self.item(report, "anisette")["status"], "unverified")
        self.assertNotIn("private.invalid", json.dumps(report))
        self.assertFalse(any(k in ("http", "network", "login") for k, _ in self.probe.calls))

    def test_unsafe_or_unknown_anisette_configuration(self):
        for value in ({"source": "made-up"},
                      {"source": "self-hosted", "endpoint": "http://example.invalid"},
                      {"source": "self-hosted", "endpoint": "https://u:p@example.invalid"},
                      {"source": "self-hosted", "endpoint": "https://example.invalid/?token=x"}):
            with self.subTest(value=value):
                self.assertEqual(self.item(self.report(anisette=value), "anisette")["status"], "mismatch")

    def test_offline_and_partial_installs_list_same_check_ids(self):
        a, b = self.report(), self.report(offline=True)
        self.assertEqual([c["id"] for c in a["checks"]], [c["id"] for c in b["checks"]])
        self.assertFalse(b["ready"])

    def test_non_windows_never_claims_windows_ready(self):
        self.probe.values["host"] = {"system": "Linux", "bits": 64}
        report = self.report()
        self.assertEqual(self.item(report, "host")["status"], "mismatch")
        self.assertFalse(report["ready"])

    def test_malformed_probe_data_never_turns_green(self):
        for kind, bad in (("python", {"version": "3.13", "packages": {}}),
                          ("impactor", {"verdict": "ok"}),
                          ("disk", {"free": True}),
                          ("usbmux_port", {"connected": 1})):
            with self.subTest(kind=kind):
                self.probe.values[kind] = bad
                ident = {"python": "python", "impactor": "impactor",
                         "disk": "disk", "usbmux_port": "apple_driver"}[kind]
                self.assertEqual(self.item(self.report(), ident)["status"], "error")
                self.probe = FakeProbe()

    def test_request_strict_types_and_unknown_fields(self):
        for request in ([], {"schema": True}, {"requiredFreeBytes": True},
                        {"requiredFreeBytes": -1}, {"python": "bad\0path"},
                        {"offline": "yes"}, {"ready": True}, {"password": "SECRET"}):
            with self.subTest(request=request):
                with self.assertRaises(ValueError):
                    self.mod.check(request, probe=self.probe)

    def test_redaction_does_not_load_local_keys(self):
        with patch.object(self.mod._redact_module(), "load_key_values",
                          side_effect=AssertionError("must not read keys")):
            text = self.mod._safe("password=SECRET email=fixture@example.invalid")
            self.assertNotIn("SECRET", text)
            self.assertNotIn("fixture@", text)
            self.assertIn("REDACTED", text)

    def test_summary_same_states_and_required_schema(self):
        report = self.report()
        rendered = self.mod.render(report)
        self.assertIn("未就绪", rendered)
        for c in report["checks"]:
            self.assertTrue({"id", "name", "ok", "status", "source", "message",
                             "requiredFor", "group", "help", "action"} <= set(c))
            self.assertIn(c["name"], rendered)
            self.assertEqual(c["ok"], c["status"] == "ok")

    def test_native_python_probe_reads_actual_selected_interpreter(self):
        p = self.mod.NativeProbe()
        result = p("python", executable=sys.executable, modules={"stdlib": "json"})
        self.assertEqual(result["version"], list(sys.version_info[:3]))
        self.assertIn("stdlib", result["packages"])

    def test_native_output_bound_and_timeout(self):
        p = self.mod.NativeProbe(timeout=0.3)
        with self.assertRaises(self.mod.ProbeError):
            p.run([sys.executable, "-I", "-B", "-c", "print('x'*200000)"])
        with self.assertRaises(self.mod.ProbeError):
            p.run([sys.executable, "-I", "-B", "-c", "import time; time.sleep(10)"])

    def test_native_disk_does_not_create_target(self):
        with tempfile.TemporaryDirectory(prefix="winenv 中文 空格 ") as tmp:
            target = Path(tmp) / "not-created" / "out"
            data = self.mod.NativeProbe()("disk", path=str(target))
            self.assertGreater(data["free"], 0)
            self.assertFalse(target.exists())

    def test_native_file_probe_refuses_non_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = self.mod.NativeProbe()("file", path=tmp)
            self.assertFalse(data["regular"])

    def test_native_python_missing_is_distinct(self):
        error = self.captured_error(lambda: self.mod.NativeProbe()(
            "python", executable="s2a-no-such-python", modules={}))
        self.assertIsInstance(error, self.mod.ProbeError)
        self.assertTrue(error.missing)

    def captured_error(self, action):
        try:
            action()
        except Exception as error:
            return error
        self.fail("Expected a bounded probe failure")

    def test_native_launch_oserror_family_is_bounded(self):
        for original in (FileNotFoundError(2, "PRIVATE missing path"),
                         PermissionError(13, "PRIVATE policy denial"),
                         OSError(8, "PRIVATE executable format")):
            with self.subTest(kind=type(original).__name__):
                with patch.object(self.mod.subprocess, "Popen", side_effect=original):
                    error = self.captured_error(lambda: self.mod.NativeProbe().run(["private.exe"]))
                self.assertIsInstance(error, self.mod.ProbeError)
                self.assertTrue(error.missing)
                self.assertNotIn("PRIVATE", str(error))
                self.assertTrue(error.__suppress_context__)

    def test_native_absolute_missing_path_has_stable_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            absent = str(Path(tmp) / "不存在 python.exe")
            error = self.captured_error(lambda: self.mod.NativeProbe().run([absent]))
            self.assertIsInstance(error, self.mod.ProbeError)
            self.assertTrue(error.missing)
            self.assertNotIn(tmp, str(error))

    def test_native_existing_non_executable_file_is_missing(self):
        with tempfile.TemporaryDirectory(prefix="winenv noexec 中文 ") as tmp:
            path = Path(tmp) / "not executable.exe"
            path.write_bytes(b"original fixture, not executable\n")
            if os.name != "nt":
                path.chmod(0o644)
            error = self.captured_error(lambda: self.mod.NativeProbe().run([str(path)]))
            self.assertIsInstance(error, self.mod.ProbeError)
            self.assertTrue(error.missing)
            # POSIX: real chmod 0644 EACCES. Windows: real invalid image;
            # injected PermissionError above covers its access/policy error branch.
            native, fallback = self.mod.NativeProbe(), self.probe
            self.probe = lambda kind, **opts: native(kind, **opts) if kind == "python" else fallback(kind, **opts)
            report = self.report(python=str(path))
            self.assertEqual(self.item(report, "python")["status"], "missing")
            self.assertTrue(self.item(report, "disk")["ok"])

    def test_launch_missing_does_not_reclassify_probe_timeout(self):
        native, fallback = self.mod.NativeProbe(), self.probe
        self.probe = lambda kind, **opts: native(kind, **opts) if kind == "python" else fallback(kind, **opts)
        with patch.object(self.mod.subprocess, "Popen", side_effect=PermissionError(13, "PRIVATE")):
            report = self.report()
        self.assertEqual(self.item(report, "python")["status"], "missing")
        self.assertNotIn("PRIVATE", json.dumps(report))
        self.probe = FakeProbe()
        self.probe.values["python"] = self.mod.ProbeError("只读探测超时")
        self.assertEqual(self.item(self.report(), "python")["status"], "error")

    def test_cli_utf8_ordinary_entry_and_read_only_request(self):
        with tempfile.TemporaryDirectory(prefix="winenv 中文 空格 ") as tmp:
            path = Path(tmp) / "请求.json"
            path.write_text(json.dumps({"schema": 1, "offline": True, "targetDir": tmp,
                                        "requiredFreeBytes": 0}), encoding="utf-8")
            before = path.read_bytes()
            env = dict(__import__("os").environ, PYTHONUTF8="0", PYTHONDONTWRITEBYTECODE="1")
            p = subprocess.run([sys.executable, "-B", str(SOURCE), "--request", str(path), "--json"],
                               capture_output=True, env=env, timeout=30)
            self.assertEqual(p.returncode, 1, p.stderr)
            data = json.loads(p.stdout.decode("utf-8"))
            self.assertFalse(data["ready"])
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(sorted(x.name for x in Path(tmp).iterdir()), ["请求.json"])

    def test_cli_invalid_request_exit_two_no_secret_echo(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.json"
            path.write_text('{"password":"SECRET"}', encoding="utf-8")
            p = subprocess.run([sys.executable, "-B", str(SOURCE), "--request", str(path), "--json"],
                               capture_output=True, timeout=10)
            self.assertEqual(p.returncode, 2)
            self.assertNotIn(b"SECRET", p.stdout + p.stderr)
            self.assertFalse(json.loads(p.stdout)["ready"])

    def test_cli_duplicate_json_keys_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ambiguous.json"
            path.write_text('{"schema":1,"schema":1}', encoding="utf-8")
            p = subprocess.run([sys.executable, "-B", str(SOURCE), "--request", str(path), "--json"],
                               capture_output=True, timeout=10)
            self.assertEqual(p.returncode, 2)

    def capabilities(self):
        self.probe.values.update({
            "metal": {"output": "Apple metal version 99.1 (fixture-only)\n"},
            "usbmux_port": {"connected": True},
            "usb_devices": {"USB": 1, "Network": 0},
            "icloud": {"packages": [{"Name": "AppleInc.iCloud", "Publisher":   # 取自玩家机器 Get-AppxPackage 实际输出（#165）
                "CN=5BD5593D-A41B-4F89-884E-B4F3E0FBAA75",
                "SignatureKind": "Store", "Status": "Ok",
                "PackageFamilyName": "AppleInc.iCloud_nzyj5cx40ttqa", "Version": "15.10.39.0"}],
                "legacy": []},
        })
        previous = self.mod._text
        override = patch.object(self.mod, "_text", side_effect=lambda name:
            "metal_iphoneos\tApple metal version 1.0 (independent-fixture)\n"
            if name == "translator/dmsh_chain.lock" else previous(name))
        override.start()
        self.addCleanup(override.stop)

    def test_cap_metal_different_version_is_ok_note_only(self):
        self.capabilities()
        c = self.item(self.report(), "metal")
        self.assertEqual(c["status"], "ok")
        self.assertIn("99.1", c["message"])
        self.assertIn("1.0", c["message"])
        self.assertIn("仅需重编", c["message"])

    def test_cap_metal_equal_version_has_no_mismatch(self):
        self.capabilities()
        self.probe.values["metal"]["output"] = "Apple metal version 1.0 (independent-fixture)"
        c = self.item(self.report(), "metal")
        self.assertEqual(c["status"], "ok")
        self.assertNotIn("不同", c["message"])

    def test_cap_metal_bad_output_is_missing(self):
        self.capabilities()
        for text in ("", "Some other compiler version 1", "error: broken\nApple metal version 9"):
            with self.subTest(text=text):
                self.probe.values["metal"] = {"output": text}
                c = self.item(self.report(), "metal")
                self.assertEqual(c["status"], "missing")
                self.assertIn(" / ".join(text.splitlines()) or "（空）", c["message"])   # 认不出时带输出原文（D-NJGH）

    def test_cap_metal_missing_message_lists_searched_places(self):
        # 玩家装了还认不出：「复制错误摘要」读 message，要带上找过的每一处和结果
        self.capabilities()
        self.probe.values["metal"] = self.mod.ProbeError("找不到Metal", missing=True, detail="找过：官方默认目录：D:\\X\\bin\\metal.exe → 没有")
        c = self.item(self.report(), "metal")
        self.assertEqual(c["status"], "missing")
        self.assertIn("找过：官方默认目录：D:\\X\\bin\\metal.exe → 没有", c["message"])
        self.assertEqual(c["help"], c["message"])

    def test_cap_metal_missing_still_lists_usb_and_disk(self):
        self.capabilities()
        self.probe.values["metal"] = FileNotFoundError()
        report = self.report()
        self.assertEqual(self.item(report, "metal")["status"], "missing")
        self.assertTrue(self.item(report, "apple_usb")["ok"])
        self.assertTrue(self.item(report, "disk")["ok"])

    def test_cap_metal_lock_missing_or_duplicate_is_error(self):
        self.capabilities()
        for text in ("", "metal_iphoneos\tA\nmetal_iphoneos\tB\n"):
            with self.subTest(text=text):
                with patch.object(self.mod, "_text", wraps=self.mod._text) as read:
                    previous = read._mock_wraps
                    read.side_effect = lambda name: text if name.endswith("dmsh_chain.lock") else previous(name)
                    self.assertEqual(self.item(self.report(), "metal")["status"], "error")

    def test_cap_service_plus_port_is_driver_only(self):
        self.capabilities()
        self.probe.values["usb_devices"] = {"USB": 0, "Network": 0}
        report = self.report()
        self.assertTrue(self.item(report, "apple_driver")["ok"])
        self.assertEqual(self.item(report, "apple_usb")["status"], "unverified")
        self.assertTrue(report["ready"])   # 手机没连不挡导入和出包

    def test_cap_closed_port_never_probes_devices(self):
        self.capabilities()
        self.probe.values["usbmux_port"] = {"connected": False}
        report = self.report()
        self.assertEqual(self.item(report, "apple_driver")["status"], "missing")
        self.assertEqual(self.item(report, "apple_usb")["status"], "unverified")
        self.assertFalse(any(k == "usb_devices" for k, _ in self.probe.calls))

    def test_cap_network_device_is_not_usb(self):
        self.capabilities()
        self.probe.values["usb_devices"] = {"USB": 0, "Network": 2}
        report = self.report()
        self.assertEqual(self.item(report, "apple_usb")["status"], "unverified")
        self.assertEqual(self.item(report, "apple_usb")["detected"], {"USB": 0, "Network": 2})

    def test_cap_usb_devices_are_count_only(self):
        self.capabilities()
        report = self.report()
        self.assertTrue(self.item(report, "apple_usb")["ok"])
        self.assertEqual(self.item(report, "apple_usb")["detected"], {"USB": 1, "Network": 0})
        self.assertTrue(report["ready"], "全部件就绪（出包件、Impactor、Metal、Apple 设备服务、iCloud）= 可以开始；以前待发布组件永远挡住")

    def test_cap_usb_failures_are_error_not_empty_success(self):
        self.capabilities()
        for value in (TimeoutError("PRIVATE_ID"), {"USB": True, "Network": 0},
                      {"USB": -1, "Network": 0}, {"USB": 1}, {"USB": 999999, "Network": 0}):
            with self.subTest(value=value):
                self.probe.values["usb_devices"] = value
                report = self.report()
                self.assertEqual(self.item(report, "apple_usb")["status"], "error")
                self.assertNotIn("PRIVATE_ID", json.dumps(report))

    def test_cap_icloud_store_evidence_makes_local_source_ok(self):
        self.capabilities()
        c = self.item(self.report(anisette={"source": "local"}), "anisette")
        self.assertTrue(c["ok"])
        self.assertEqual(c["detected"]["source"], "local")
        self.assertNotIn("fixturepublisher", json.dumps(c))

    def test_cap_icloud_same_name_or_untrusted_source_never_ok(self):
        self.capabilities()
        original = self.probe.values["icloud"]["packages"][0]
        for field, value in (("Publisher", "CN=Not Apple"), ("SignatureKind", "Developer"),
                             ("Status", "Tampered"), ("PackageFamilyName", "unrelated"),
                             ("Name", "RandomTool"), ("Version", "")):
            with self.subTest(field=field):
                self.probe.values["icloud"]["packages"] = [dict(original, **{field: value})]
                c = self.item(self.report(anisette={"source": "local"}), "anisette")
                self.assertFalse(c["ok"])

    def test_cap_icloud_absence_denial_and_legacy_are_distinct(self):
        self.capabilities()
        self.probe.values["icloud"] = {"packages": [], "legacy": []}
        self.assertEqual(self.item(self.report(anisette={"source": "local"}), "anisette")["status"], "missing")
        self.probe.values["icloud"] = PermissionError("PRIVATE_ACCOUNT")
        report = self.report(anisette={"source": "local"})
        self.assertEqual(self.item(report, "anisette")["status"], "error")
        self.assertNotIn("PRIVATE_ACCOUNT", json.dumps(report))
        self.probe.values["icloud"] = {"packages": [], "legacy": [{"DisplayName": "iCloud", "Publisher": "Apple Inc."}]}
        item = self.item(self.report(anisette={"source": "local"}), "anisette")
        self.assertEqual(item["status"], "ok")   # 官方传统安装版（非商店）也算装好（11-08 维护者 实机）
        real = {"Name": "AppleInc.iCloud", "Publisher": "CN=5BD5593D-A41B-4F89-884E-B4F3E0FBAA75", "SignatureKind": "Store", "Status": "Ok",
                "PackageFamilyName": "AppleInc.iCloud_nzyj5cx40ttqa", "Version": "15.10.39.0"}   # #165 玩家机器的实际输出：商店版发布者不是 Apple Inc.
        for change, want in (({}, "ok"), ({"SignatureKind": "Developer"}, "unverified"), ({"Status": "Tampered"}, "unverified"),
                             ({"PackageFamilyName": "AppleInc.iCloud_aaaaaaaaaaaaa", "Publisher": "CN=Apple Inc., O=Apple Inc."}, "unverified")):
            self.probe.values["icloud"] = {"packages": [{**real, **change}], "legacy": []}
            self.assertEqual(self.item(self.report(anisette={"source": "local"}), "anisette")["status"], want, change)
        self.probe.values["icloud"] = {"packages": [], "legacy": [{"DisplayName": "iCloud", "Publisher": "Someone Else"}]}
        self.assertEqual(self.item(self.report(anisette={"source": "local"}), "anisette")["status"], "unverified")

    def test_cap_explicit_public_is_user_selected_never_ready(self):
        self.capabilities()
        report = self.report(anisette={"source": "public", "allowPublic": True,
                                      "endpoint": "https://fixture.invalid"})
        c = self.item(report, "anisette")
        self.assertEqual(c["status"], "user-selected")
        self.assertFalse(c["ok"])
        self.assertFalse(c["blocking"])   # 签名才用，只提示
        self.assertTrue(report["ready"])
        self.assertFalse(any(k == "icloud" for k, _ in self.probe.calls))

    def test_cap_default_uses_official_local_only_no_public(self):
        self.capabilities()
        self.assertTrue(self.item(self.report(), "anisette")["ok"])
        self.probe.values["icloud"] = {"packages": [], "legacy": []}
        c = self.item(self.report(), "anisette")
        self.assertEqual(c["status"], "unverified")
        self.assertNotIn("endpoint", json.dumps(c))

    def test_cap_all_required_components_still_block(self):
        self.capabilities()
        report = self.report()
        for ident in ("metal", "apple_driver", "apple_usb", "anisette"):
            self.assertTrue(self.item(report, ident)["ok"])
        self.probe.values["pack"] = [["shell", False, "预编壳 3b096983e85c86a7", "missing", None]]
        report = self.report()
        self.assertEqual(self.item(report, "pack.shell")["status"], "missing")
        self.assertFalse(report["ready"])

    def usb_transport(self, body=None, *, header=None, raw=None, fragment=3):
        payload = plistlib.dumps({"DeviceList": []} if body is None else body)
        data = (struct.pack("<IIII", *(header or (len(payload) + 16, 1, 8, 1))) + payload) if raw is None else raw
        class Transport:
            def __init__(self):
                self.data, self.sent, self.closed = data, b"", False
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self.closed = True
            def settimeout(self, value):
                self.timeout = value
            def sendall(self, value):
                self.sent += value
            def recv(self, size):
                block = self.data[:min(size, fragment)]
                self.data = self.data[len(block):]
                return block
        return Transport()

    def test_cap_native_usbmux_fragmented_list_only_count_and_no_ids(self):
        stream = self.usb_transport({"DeviceList": [
            {"DeviceID": 4, "Properties": {"ConnectionType": "USB", "SerialNumber": "PRIVATE_DEVICE"}},
            {"Properties": {"ConnectionType": "Network", "NetworkAddress": "PRIVATE_ADDRESS"}},
            {"Properties": {"ConnectionType": "USB"}}]})
        with patch.object(self.mod.socket, "create_connection", return_value=stream) as connect:
            result = self.mod.NativeProbe().usb_devices()
        self.assertEqual(result, {"USB": 2, "Network": 1})
        self.assertNotIn("PRIVATE", json.dumps(result))
        self.assertTrue(stream.closed)
        self.assertEqual(connect.call_args.args, (("127.0.0.1", 27015),))
        length, version, message, tag = struct.unpack("<IIII", stream.sent[:16])
        self.assertEqual((length, version, message, tag), (len(stream.sent), 1, 8, 1))
        request = plistlib.loads(stream.sent[16:])
        self.assertEqual(request["MessageType"], "ListDevices")
        self.assertEqual(request["kLibUSBMuxVersion"], 3)
        self.assertFalse({"DeviceID", "PairRecordID", "SerialNumber"} & set(request))

    def test_cap_native_usbmux_bad_frames_never_empty_success(self):
        size = len(plistlib.dumps({"DeviceList": []})) + 16
        for header in ((15, 1, 8, 1), (65537, 1, 8, 1), (size, 0, 8, 1),
                       (size, 1, 1, 1), (size, 1, 8, 99)):
            with self.subTest(header=header):
                stream = self.usb_transport(header=header)
                with patch.object(self.mod.socket, "create_connection", return_value=stream):
                    error = self.captured_error(self.mod.NativeProbe().usb_devices)
                self.assertIsInstance(error, self.mod.ProbeError)
                self.assertTrue(stream.closed)
        for body in ({}, {"DeviceList": "not-list"}, {"DeviceList": [{"Properties": {}}]},
                     {"DeviceList": [{"Properties": {"ConnectionType": "Surprise"}}]}):
            with self.subTest(body=body):
                with patch.object(self.mod.socket, "create_connection", return_value=self.usb_transport(body)):
                    self.assertIsInstance(self.captured_error(self.mod.NativeProbe().usb_devices), self.mod.ProbeError)

    def test_cap_native_usbmux_eof_timeout_and_shared_deadline(self):
        for stream in (self.usb_transport(raw=b"short"), self.usb_transport()):
            with self.subTest(stream=stream):
                if len(stream.data) > 5:
                    stream.recv = lambda _: (_ for _ in ()).throw(TimeoutError("PRIVATE"))
                with patch.object(self.mod.socket, "create_connection", return_value=stream):
                    error = self.captured_error(self.mod.NativeProbe().usb_devices)
                self.assertIsInstance(error, self.mod.ProbeError)
                self.assertNotIn("PRIVATE", str(error))
                self.assertTrue(stream.closed)
        stream = self.usb_transport()
        with patch.object(self.mod.socket, "create_connection", return_value=stream), \
             patch.object(self.mod.time, "monotonic", side_effect=[0, 1, 4]):
            self.assertIsInstance(self.captured_error(self.mod.NativeProbe(timeout=3).usb_devices), self.mod.ProbeError)
        self.assertTrue(stream.closed)

    def test_cap_native_driver_port_connection_sends_nothing(self):
        stream = self.usb_transport()
        with patch.object(self.mod.sys, "platform", "win32"), \
             patch.object(self.mod.socket, "create_connection", return_value=stream) as connect:
            self.assertEqual(self.mod.NativeProbe()("usbmux_port"), {"connected": True})
        self.assertEqual(stream.sent, b"")
        self.assertTrue(stream.closed)
        self.assertEqual(connect.call_args.args, (("127.0.0.1", 27015),))

    def test_cap_native_metal_explicit_env_then_path_no_directory_guess(self):
        with tempfile.TemporaryDirectory(prefix="metal 中文 空格 ") as tmp:
            paths = [str(Path(tmp) / x) for x in ("caller.exe", "env.exe", "path.exe")]
            for path in paths:
                Path(path).write_bytes(b"original fixture, never executed")
            with patch.dict(os.environ, {"S2A_METAL_EXE": paths[1]}), \
                 patch.object(self.mod.shutil, "which", return_value=paths[2]), \
                 patch.object(self.mod.NativeProbe, "run", return_value=b"Apple metal version 9\n") as run:
                self.mod.NativeProbe()("metal", executable=paths[0])
                self.assertEqual(run.call_args.args, ([paths[0], "--version"],))
                self.assertTrue(run.call_args.kwargs["merge_stderr"])
                self.mod.NativeProbe()("metal", executable=None)
                self.assertEqual(run.call_args.args, ([paths[1], "--version"],))
                with patch.dict(os.environ, {"S2A_METAL_EXE": ""}):
                    self.mod.NativeProbe()("metal", executable=None)
                self.assertEqual(run.call_args.args, ([paths[2], "--version"],))

    def test_cap_native_metal_bin_wins_path(self):
        with tempfile.TemporaryDirectory(prefix="metal bin 中文 ") as tmp:
            selected = Path(tmp) / "explicit" / "metal.exe"
            other = Path(tmp) / "PATH" / "metal.exe"
            for path in (selected, other):
                path.parent.mkdir()
                path.write_bytes(b"original fixture, never executed")
            with patch.dict(os.environ, {"S2A_METAL_BIN": str(selected.parent)}, clear=True), \
                 patch.object(self.mod.shutil, "which", return_value=str(other)), \
                 patch.object(self.mod.NativeProbe, "run", return_value=b"Apple metal version 9\n") as run:
                self.mod.NativeProbe()("metal", executable=None)
            self.assertEqual(run.call_args.args, ([str(selected), "--version"],))

    def test_cap_native_missing_explicit_metal_bin_does_not_fall_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            other = Path(tmp) / "metal.exe"
            other.write_bytes(b"original fixture, never executed")
            with patch.dict(os.environ, {"S2A_METAL_BIN": str(Path(tmp) / "missing")}, clear=True), \
                 patch.object(self.mod.shutil, "which", return_value=str(other)), \
                 patch.object(self.mod.NativeProbe, "run") as run:
                with self.assertRaises(self.mod.ProbeError) as ctx:
                    self.mod.NativeProbe()("metal", executable=None)
                run.assert_not_called()
            self.assertTrue(ctx.exception.missing)
            self.assertIn("S2A_METAL_BIN：" + str(Path(tmp) / "missing" / "metal.exe") + " → 没有", ctx.exception.detail)   # 错误摘要写明找的是哪一处

    def test_cap_metal_nonzero_is_missing_timeout_and_overflow_still_error(self):
        self.capabilities()
        for error, expected in ((self.mod.ProbeError("exit", nonzero=True), "missing"),
                                (self.mod.ProbeError("timeout"), "error"),
                                (self.mod.ProbeError("overflow"), "error")):
            with self.subTest(expected=expected):
                self.probe.values["metal"] = error
                self.assertEqual(self.item(self.report(), "metal")["status"], expected)

    def test_cap_native_icloud_script_reads_install_metadata_only(self):
        import types
        registry = types.SimpleNamespace(HKEY_LOCAL_MACHINE=1, HKEY_CURRENT_USER=2,
            KEY_WOW64_64KEY=256, KEY_WOW64_32KEY=512, KEY_READ=1,
            OpenKey=lambda *_: (_ for _ in ()).throw(FileNotFoundError()))
        with patch.object(self.mod.sys, "platform", "win32"), \
             patch.dict(sys.modules, {"winreg": registry}), \
             patch.object(self.mod.NativeProbe, "run", return_value=b"[]") as run:
            self.assertEqual(self.mod.NativeProbe().icloud(), {"packages": [], "legacy": []})
        args = run.call_args.args[0]
        self.assertIn("Get-AppxPackage", args[-1])
        self.assertIn("SignatureKind", args[-1])
        for forbidden in ("-AllUsers", "InstallLocation", "PackageUserInformation", "prod.keys", "Invoke-WebRequest", "Start-Process"):
            self.assertNotIn(forbidden, args[-1])

    def budget_catalog(self):
        temp = tempfile.TemporaryDirectory(prefix="budget 中文 空格 ")
        self.addCleanup(temp.cleanup)
        self.catalog_path = Path(temp.name) / "catalog.json"
        def package(kind, version, size, sha, title, **fields):
            return dict(type=kind, version=version, size=size, sha256=sha * 64,
                        titleId=title, **fields)
        self.budget_game = {"id": "budget-fixture", "packages": [
            package("base", 1, 90_000_000_000, "a", "0000000000000001"),
            package("base", 2, 5_000_000_000, "b", "0000000000000001"),
            package("update", 1, 80_000_000_000, "e", "0000000000000002"),
            package("update", 2, 200_000_000, "c", "0000000000000002", contentBytes=3_000_000_000,
                    path="sources/original-fixture.nsz"),
            package("dlc", 1, 70_000_000_000, "f", "0000000000000003"),
            package("dlc", 2, 2_700_000_000, "d", "0000000000000003"),
            package("dlc", 1, 5_000_000_000, "b", "0000000000000004"),
        ]}
        self.catalog_path.write_text(json.dumps({"schema": 1, "games": [self.budget_game]}), encoding="utf-8")
        self.probe.values["disk"] = {"free": 60_000_000_000}
        self.request.pop("requiredFreeBytes")
        self.request.pop("targetDir")
        return {"catalog": str(self.catalog_path), "gameId": "budget-fixture"}

    def budget_report(self, **fields):
        try:
            return self.report(**fields)
        except Exception as exc:
            self.fail("合法预算合同未产生报告：" + type(exc).__name__)

    @staticmethod
    def library_budget(context, game, need=None):
        """Mac 预检的真函数（library.space_budget），只把游戏库位置 / 盘名 / 解包是否在盘上换成夹具（剃刀 H11：原先 AST 截 library.py 片段 exec）"""
        import library
        stubs = [patch.object(library, "_unpacked_on_disk", lambda c, g: False),
                 patch.object(library, "_pack_paths", lambda c, g: (Path(c.library), Path(c.library))),   # 存放位置与工具缓存同在夹具游戏库（同卷：合成一项）
                 patch.object(library, "_volume", lambda _: "fixture-volume")]
        if need:
            stubs.append(patch.object(library, "pack_need_bytes", need))
        for stub in stubs:
            stub.start()
        try:
            return library.space_budget(context, game)
        finally:
            for stub in stubs:
                stub.stop()

    def test_budget_direct_catalog_matches_handwritten_bytes(self):
        fields = self.budget_catalog()
        before = self.catalog_path.read_bytes()
        c = self.item(self.budget_report(**fields), "disk")
        # Windows：10,700,000,000 × 1.1 × 2（解包 + 拷贝）+ 15,000,000,000，独立手算；单一 IMG 边写边腾解包、不另占（#746）
        self.assertEqual(c["detected"]["requiredFreeBytes"], 38_540_000_000)
        self.assertTrue(c["ok"])
        self.assertEqual(self.catalog_path.read_bytes(), before)
        self.assertIn("space_rule.py", c["source"])

    def test_budget_mac_function_body_keeps_direct_result(self):
        self.budget_catalog()
        context = jobcase.fake_context(self.catalog_path.parent, library=self.catalog_path.parent)
        with patch("shutil.disk_usage", return_value=types.SimpleNamespace(free=40_000_000_000)):
            result = self.library_budget(context, self.budget_game)
        self.assertEqual(result["volumes"][0]["needBytes"], 26_770_000_000)   # Mac：解包一份 × 1.1 + 15 GB（单文件镜像不再存进库，#329）
        self.assertEqual(result["gameBytes"], 10_700_000_000)
        self.assertIn("直连着色器上限", result["volumes"][0]["note"])

    def test_budget_mac_adapter_calls_shared_pure_function(self):
        self.budget_catalog()
        calls = []
        def shared(packages, **kw):
            calls.append(packages)
            return 12345
        context = jobcase.fake_context(self.catalog_path.parent, library=self.catalog_path.parent)
        with patch("shutil.disk_usage", return_value=types.SimpleNamespace(free=40000)):
            result = self.library_budget(context, self.budget_game, need=shared)
        self.assertEqual(result["volumes"][0]["needBytes"], 12345)
        self.assertEqual(len(calls), 1)

    def test_budget_explicit_bytes_override_bad_catalog(self):
        fields = self.budget_catalog()
        self.catalog_path.write_text("not JSON", encoding="utf-8")
        c = self.item(self.budget_report(**fields, requiredFreeBytes=999), "disk")
        self.assertEqual(c["status"], "ok")
        self.assertEqual(c["detected"]["requiredFreeBytes"], 999)
        self.assertTrue(c["ok"])

    def test_budget_missing_paired_fields_never_green(self):
        fields = self.budget_catalog()
        for data, reason in (({"gameId": "budget-fixture"}, "catalog"),
                             ({"catalog": fields["catalog"]}, "gameId")):
            with self.subTest(data=data):
                c = self.item(self.budget_report(**data), "disk")
                self.assertIn(c["status"], ("unverified", "mismatch"))
                self.assertFalse(c["ok"])
                self.assertIn(reason, c["message"])

    def test_budget_unknown_or_empty_game_never_green(self):
        fields = self.budget_catalog()
        c = self.item(self.budget_report(**dict(fields, gameId="not-in-catalog")), "disk")
        self.assertIn(c["status"], ("unverified", "mismatch"))
        self.budget_game["packages"] = []
        self.catalog_path.write_text(json.dumps({"schema": 1, "games": [self.budget_game]}), encoding="utf-8")
        c = self.item(self.budget_report(**fields), "disk")
        self.assertFalse(c["ok"])

    def test_budget_library_target_default_and_explicit_target(self):
        fields = self.budget_catalog()
        self.budget_report(**fields)
        self.assertEqual(next(o["path"] for k, o in self.probe.calls if k == "disk"), str(self.catalog_path.parent))
        self.probe.calls.clear()
        self.budget_report(**fields, targetDir="explicit-volume")
        self.assertEqual(next(o["path"] for k, o in self.probe.calls if k == "disk"), "explicit-volume")

    def test_budget_invalid_catalog_is_named_error_not_global_abort(self):
        fields = self.budget_catalog()
        for text in ("not JSON", '{"schema":1,"schema":1,"games":[]}',
                     json.dumps({"schema": 1, "games": [self.budget_game, self.budget_game]})):
            with self.subTest(text=text[:30]):
                self.catalog_path.write_text(text, encoding="utf-8")
                report = self.budget_report(**fields)
                self.assertEqual(self.item(report, "disk")["status"], "error")
                self.assertIn("impactor", [c["id"] for c in report["checks"]])

    def test_budget_invalid_package_values_never_make_space_green(self):
        fields = self.budget_catalog()
        original = dict(self.budget_game["packages"][1])
        for key, value in (("size", -1), ("size", True), ("contentBytes", "3000"),
                           ("version", []), ("sha256", "invalid")):
            with self.subTest(key=key, value=value):
                self.budget_game["packages"][1] = dict(original, **{key: value})
                self.catalog_path.write_text(json.dumps({"schema": 1, "games": [self.budget_game]}), encoding="utf-8")
                self.assertEqual(self.item(self.budget_report(**fields), "disk")["status"], "error")

    def test_budget_windows_reads_changed_shared_rule_not_second_formula(self):
        # Windows 环境检查用的就是 space_rule 那一份规则（剃刀 H11）：把它的直连上限改成 22 GB，结果跟着变
        fields = self.budget_catalog()
        with patch.object(self.mod._space_rule(), "DIRECT_LARGE_BYTES", int(22e9)):
            c = self.item(self.budget_report(**fields), "disk")
        self.assertEqual(c["detected"]["requiredFreeBytes"], 45_540_000_000)   # 10.7 GB × 1.1 × 2 + 22 GB，独立手算（IMG 边写边腾解包，#746）

    def test_budget_pure_function_needs_no_context_or_mac_import(self):
        self.budget_catalog()
        before = set(sys.modules)
        rules = self.mod._space_rule()
        selected = rules.selected_packages(self.budget_game)
        self.assertEqual(rules.pack_need_bytes(selected, windows=True, image=True), 38_540_000_000)
        self.assertFalse({"core", "fcntl", "library"} & (set(sys.modules) - before))

    def test_budget_native_cli_valid_catalog_no_mac_context_import(self):
        fields = self.budget_catalog()
        request = self.catalog_path.parent / "request.json"
        request.write_text(json.dumps(dict(schema=1, **fields)), encoding="utf-8")
        env = dict(os.environ, PYTHONUTF8="0", PYTHONDONTWRITEBYTECODE="1")
        p = subprocess.run([sys.executable, "-B", str(SOURCE), "--request", str(request), "--json"],
                           capture_output=True, env=env, timeout=30)
        self.assertEqual(p.returncode, 1, p.stderr)
        report = json.loads(p.stdout.decode("utf-8"))
        self.assertEqual(self.item(report, "disk")["detected"]["requiredFreeBytes"], 38_540_000_000)
        self.assertFalse(any(x.name == "sources" for x in self.catalog_path.parent.iterdir()))

    def test_budget_request_types_remain_strict(self):
        for request in ({"catalog": False}, {"gameId": []}, {"catalog": "x\0y"}, {"gameId": ""}):
            with self.subTest(request=request):
                with self.assertRaises(ValueError):
                    self.mod.check(request, probe=self.probe)

    @unittest.skipUnless(sys.platform == "win32", "Windows SCM native probe only")
    def test_native_windows_service_query(self):
        try:
            result = self.mod.NativeProbe()("service")
            self.assertIs(type(result["state"]), int)
        except FileNotFoundError:
            pass  # Explicit missing service is also a legitimate real-host observation.
        except PermissionError:
            self.skipTest("SCM query permission denied; not device proof")


class RecoveryMappingTests(unittest.TestCase):
    # Literal acceptance values, independent of RECOVERY/recovery under test.
    EXPECTED = {
        "win_host_unsupported": "请改用64位Windows主机运行S2Arm，确认后点「重新检查」。",
        "win_metal_missing": "用自己的Apple ID登录苹果开发者下载页（https://developer.apple.com/download/），下载安装官方Metal Developer Tools for Windows，装好后点「重新检查」。",
        "win_apple_devices_missing": "从Microsoft Store安装Apple Devices或iTunes（https://support.apple.com/118290），装好后点「重新检查」。",
        "win_apple_service_stopped": "在Windows「服务」中启动Apple Mobile Device Service；若没有该服务，从Microsoft Store安装Apple Devices或iTunes，完成后点「重新检查」。",
        "win_icloud_missing": "从Microsoft Store安装官方iCloud for Windows（https://support.apple.com/103232），装好后点「重新检查」。",
        "win_anisette_source": "选择本机官方iCloud或我方自建来源；公共服务器仅可手动作为备选，选中仍不判就绪，配置后点「重新检查」。",
        "win_disk_budget_unknown": "先在游戏库选择游戏并生成出包计划，再点「重新检查」。",
        "win_check_failed": "本次环境探测失败；按诊断检查工具安装和权限，处理后点「重新检查」。",
        "tool_missing": "重新安装完整的 S2Arm，点「重新检查」后重试。",
        "device_not_connected": "让你选的那台 iPhone / iPad 连上这台电脑：插上数据线，或解锁后和电脑连同一个 Wi-Fi；第一次连要在设备上点「信任」，并在「设置 → 隐私与安全性 → 开发者模式」打开开发者模式，然后再试。",
        "disk_full": "释放任务所在磁盘的空间，检查游戏库和应用数据所在磁盘后重试；不要删除存档或上次成功的运行版本。",
        "library_in_cloud": "游戏库在云盘同步目录里（iCloud 云盘、OneDrive、Dropbox 等）：出包时会写几十 GB 中间文件，云盘会上传它们、换成占位文件，出包中途会读不到。在「设置 → 游戏库与玩家数据」把游戏库改到本机磁盘上不同步的位置（例如默认位置），再重试；游戏原件放在哪都行。",
        "library_bad_format": "可以点「仍然用这里」照用这块盘（出包会多占空间）；想省空间就从下面列出的 NTFS 盘里选一块，点「放到这里」。",
    }
    setUpClass = classmethod(EnvironmentTests.setUpClass.__func__)
    setUp = EnvironmentTests.setUp
    report = EnvironmentTests.report
    item = EnvironmentTests.item

    def assert_recovery(self, report, ident, status, code):
        row = self.item(report, ident)
        self.assertEqual(row["status"], status)
        self.assertIn("code", row)
        self.assertEqual(row.get("code"), code)
        self.assertEqual(row["action"], self.EXPECTED[code])
        self.assertEqual(row["help"], row["message"])
        self.assertFalse(row["ok"])

    def test_library_location(self):
        # 11-07 启动器：游戏库在 OneDrive 里 / exFAT 盘上 → 两项不合格、给「换游戏库」；本机 NTFS 不在云盘 → 两项 ok；不带 library 不列
        self.assertFalse({"library_cloud", "library_format"} & {c["id"] for c in self.report()["checks"]})
        self.probe.values["volume"] = {"format": "exfat", "home": "C:\\Users\\player", "env": {"OneDrive": "C:\\Users\\player\\OneDrive"}}
        report = self.report(library="C:\\Users\\player\\OneDrive\\S2Arm")
        self.assert_recovery(report, "library_cloud", "mismatch", "library_in_cloud")
        self.assert_recovery(report, "library_format", "mismatch", "library_bad_format")
        self.assertIn("OneDrive", self.item(report, "library_cloud")["message"]); self.assertIn("EXFAT", self.item(report, "library_format")["message"])
        self.assertEqual(next(o for k, o in self.probe.calls if k == "volume"), {"path": "C:\\Users\\player\\OneDrive\\S2Arm"})
        self.probe.values["volume"] = {"format": "ntfs", "home": "C:\\Users\\player", "env": {"OneDrive": "C:\\Users\\player\\OneDrive"}}
        report = self.report(library="D:\\S2Arm\\游戏库")
        self.assertTrue(self.item(report, "library_cloud")["ok"]); self.assertTrue(self.item(report, "library_format")["ok"])
        self.assertEqual(self.item(report, "library_format")["message"], "游戏库所在磁盘格式可用。")
        # NAS 游戏库（映射盘 / UNC，DRIVE_REMOTE）：照样 ok，只多一句会慢，不拦
        self.probe.values["volume"] = {"format": "ntfs", "remote": True, "home": "C:\\Users\\player", "env": {}}
        report = self.report(library="\\\\nas\\share\\S2Arm")
        self.assertTrue(self.item(report, "library_format")["ok"])
        self.assertEqual(self.item(report, "library_format")["message"], "游戏库在网络盘上，出包会比较慢。")

    def test_recovery_host_mismatch(self):
        self.probe.values["host"] = {"system": "Linux", "bits": 64}
        self.assert_recovery(self.report(), "host", "mismatch", "win_host_unsupported")

    def test_recovery_python_and_dependencies(self):
        self.probe.values["python"] = FileNotFoundError()
        report = self.report()
        self.assert_recovery(report, "python", "missing", "tool_missing")
        self.probe = FakeProbe()
        self.probe.values["python"]["version"] = [3, 10, 0]
        self.probe.values["python"]["packages"]["numpy"] = {"error": "ModuleNotFoundError"}
        self.probe.values["python"]["packages"]["Pillow"] = {"version": "13.0.0"}
        report = self.report()
        for ident, status in (("python", "mismatch"), ("python.numpy", "missing"), ("python.Pillow", "mismatch")):
            self.assert_recovery(report, ident, status, "tool_missing")

    def test_recovery_metal_missing(self):
        self.assert_recovery(self.report(), "metal", "missing", "win_metal_missing")

    def test_recovery_apple_driver_missing(self):
        self.probe.values["usbmux_port"] = {"connected": False}
        self.probe.values["service"] = FileNotFoundError()
        self.assert_recovery(self.report(), "apple_driver", "missing", "win_apple_devices_missing")

    def test_recovery_apple_service_stopped(self):
        self.probe.values["usbmux_port"] = {"connected": False}
        self.probe.values["service"] = {"state": 1}
        self.assert_recovery(self.report(), "apple_driver", "mismatch", "win_apple_service_stopped")

    def test_recovery_usb_unverified(self):
        self.assert_recovery(self.report(), "apple_usb", "unverified", "device_not_connected")

    def test_recovery_icloud_missing(self):
        self.assert_recovery(self.report(anisette={"source": "local"}), "anisette", "missing", "win_icloud_missing")

    def test_recovery_anisette_sources(self):
        for config, status in (({"source": "unknown"}, "mismatch"),
                               ({"source": "public", "allowPublic": True, "endpoint": "https://example.invalid"}, "user-selected"),
                               ({"source": "self-hosted", "endpoint": "https://example.invalid"}, "unverified")):
            with self.subTest(status=status):
                self.assert_recovery(self.report(anisette=config), "anisette", status, "win_anisette_source")

    def test_recovery_disk_mismatch(self):
        self.probe.values["disk"] = {"free": 999}
        self.assert_recovery(self.report(), "disk", "mismatch", "disk_full")

    def test_recovery_disk_unknown_budget(self):
        self.request.pop("requiredFreeBytes")
        self.assert_recovery(self.report(catalog="catalog.json"), "disk", "unverified", "win_disk_budget_unknown")

    def test_recovery_probe_failure_is_not_component_pending(self):
        for kind, ident in (("host", "host"), ("python", "python"), ("impactor", "impactor"), ("pack", "pack.items"),
                            ("metal", "metal"), ("usbmux_port", "apple_driver"),
                            ("usb_devices", "apple_usb"), ("icloud", "anisette"), ("disk", "disk")):
            with self.subTest(ident=ident):
                self.probe = FakeProbe()
                self.probe.values[kind] = TimeoutError("PRIVATE probe detail")
                report = self.report()
                self.assert_recovery(report, ident, "error", "win_check_failed")
                self.assertNotIn("PRIVATE", json.dumps(report))

    def test_recovery_ok_items_have_null_code_and_action(self):
        report = self.report()
        ok = [row for row in report["checks"] if row["ok"]]
        self.assertGreater(len(ok), 0)
        for row in ok:
            self.assertIn("code", row)
            self.assertIsNone(row["code"])
            self.assertIsNone(row["action"])
        self.assertFalse(report["ready"])

    def test_recovery_unmapped_pair_is_named_contract_error(self):
        for ident, status in (("metal", "user-selected"), ("disk", "missing"),
                              ("unknown_check", "error"), ("host", "missing"), ([], "error"), ("host", [])):
            with self.subTest(ident=ident, status=status):
                with self.assertRaisesRegex(ValueError, "winenv_recovery_contract"):
                    self.mod._item(ident, ident, status, "fixture", "fixture")

    def test_recovery_trusted_module_and_no_unknown_code_fallback(self):
        loader = getattr(self.mod, "_recovery_module", None)
        self.assertTrue(callable(loader), "missing fixed-source recovery loader")
        with patch.dict(sys.modules, {"errors": types.SimpleNamespace(recovery=lambda *a, **k: "SHADOW")}):
            module = loader()
            self.assertEqual(Path(module.__file__).resolve(), (ROOT / "pack/jobs/errors.py").resolve())
            self.assertEqual(module.recovery("win_metal_missing", packaged=True), self.EXPECTED["win_metal_missing"])
            with patch.dict(module.RECOVERY, {}, clear=True):
                with self.assertRaisesRegex(ValueError, "winenv_recovery_contract"):
                    self.mod._item("metal", "Metal", "missing", "fixture", "fixture")

    def test_recovery_shared_spec_revision_and_new_code_shape(self):
        data = json.loads((ROOT / "launchers/shared/experience.v1.json").read_text(encoding="utf-8"))
        self.assertGreaterEqual(data["revision"], 8)   # 8 = 本组错误码进规格的那一版；之后别的文案改动会再 +1，钉死等号会把后来者打红
        items = {item["code"]: item for item in data["errors"]["items"]}
        for code in (key for key in self.EXPECTED if key.startswith("win_")):
            self.assertIn(code, items)
            self.assertEqual(items[code]["next_action"], {"from": "pack/jobs/errors.py"})
            self.assertEqual(set(items[code]["title"]), {"zh-Hans", "en"})
            self.assertEqual(set(items[code]["detail"]), {"zh-Hans", "en"})
            self.assertFalse(items[code]["retryable"])
            self.assertTrue(items[code]["recoverable"])

    def test_recovery_contract_defect_is_not_swallowed_by_check_or_cli(self):
        error = getattr(self.mod, "RecoveryContractError", None)
        self.assertTrue(isinstance(error, type), "missing named contract error")
        with patch.object(self.mod, "_recovery_code", side_effect=error("winenv_recovery_contract")):
            with self.assertRaisesRegex(error, "winenv_recovery_contract"):
                self.report()
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(self.mod.main(["--json"]), 2)
            report = json.loads(output.getvalue())
        self.assertEqual(report["error"]["code"], "winenv_recovery_contract")
        self.assertEqual(report["checks"], [])

    def test_pack_items_come_from_oneclick_check(self):
        # 出包件只取 oneclick_win env_items 一份：缺 → missing、版本不符 → mismatch，码都是 tool_missing（随包件缺 = 安装不完整）；
        # python / metal 两行不重复列（本模块核得更细）；有 ok 无 status 的旧三元组按 ok 判
        self.probe.values["pack"] = [["python", True, "3.12.10", "ok", None], ["metal", True, "x", "ok", None],
                                     ["zstd-cli", False, "要 1.5.7，现有 1.5.6", "mismatch", None],
                                     ["fonts", False, "开源共享字体 7 份"], ["tzdata", True, "时区"]]
        report = self.report()
        ids = [c["id"] for c in report["checks"]]
        self.assertNotIn("pack.python", ids); self.assertNotIn("pack.metal", ids)
        self.assert_recovery(report, "pack.zstd-cli", "mismatch", "tool_missing")
        self.assert_recovery(report, "pack.fonts", "missing", "tool_missing")
        self.assertEqual((self.item(report, "pack.tzdata")["status"], self.item(report, "pack.tzdata")["code"]), ("ok", None))

    def test_impactor_states_and_official_page(self):
        page = "https://github.com/claration/impactor/releases/tag/v2.7.2"
        base = {"tested": "2.7.2", "download": page, "plumesign": True}
        for st, status, code in ((dict(base, verdict="missing", version=None), "missing", "impactor_missing"),
                                 (dict(base, verdict="untested", version="2.8.0"), "mismatch", "impactor_version_untested"),
                                 (dict(base, verdict="ok", version="2.7.2", plumesign=False), "missing", "impactor_missing"),
                                 (dict(base, verdict="ok", version="2.7.2"), "ok", None)):
            with self.subTest(st=st):
                self.probe = FakeProbe(); self.probe.values["impactor"] = st
                c = self.item(self.report(), "impactor")
                self.assertEqual((c["status"], c["code"], c["url"]), (status, code, None if status == "ok" else page))

    def test_official_page_only_when_not_ready(self):
        c = self.item(self.report(), "metal")   # 缺省夹具 metal 探测 FileNotFoundError → missing
        self.assertEqual((c["status"], c["url"]), ("missing", "https://developer.apple.com/download/all/?q=metal%20developer%20tools%20for%20windows"))   # 直接到 Windows 版的下载项（要登录 Apple 账号）
        self.assertIsNone(self.item(self.report(), "host")["url"])

    def test_recovery_metadata_does_not_change_all_ok_ready(self):
        # Capability fakes only: no unpublished component is green in real checks.
        row = lambda ident: self.mod._item(ident, ident, "ok", "observed", "fixture")
        with patch.object(self.mod, "_metal", return_value=row("metal")), \
             patch.object(self.mod, "_apple", return_value=[row("apple_driver"), row("apple_usb")]), \
             patch.object(self.mod, "_anisette", return_value=row("anisette")):
            report = self.report()
        self.assertTrue(report["ready"])
        self.assertTrue(all(c["code"] is None and c["action"] is None for c in report["checks"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
