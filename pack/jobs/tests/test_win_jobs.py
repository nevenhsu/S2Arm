# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Windows 作业层打底（启动器线 11-07）：作业环境只用随包运行时、读取器取预编 nx_unpack、探活不用 os.kill(pid, 0)。
期望按随包布局手写（launchers/windows/bundle_runtime_win.py：Runtime/{python, python/Scripts, dotnet, git/usr/bin, zstd}），不跑被测实现推。"""
import contextlib
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
import unittest.mock
from unittest.mock import patch

JOBS = Path(__file__).resolve().parents[1]
ROOT = JOBS.parents[1]
sys.path.insert(0, str(JOBS))
import core
import toolchain
from jobcase import OsView, as_windows, ios_fixture
import jobcase  # noqa: E402（作业上下文替身统一工厂 jobcase.fake_context）
import layout


class AtomicReplace(unittest.TestCase):
    # #130：Windows 上目标被占用时 os.replace 抛 PermissionError，一次撞车不能毁掉整局
    def run_case(self, name, failures, sleeps):
        from unittest import mock
        real = os.replace; left = [failures]
        def replace(a, b):
            if left[0]:
                left[0] -= 1
                raise PermissionError(13, "拒绝访问。")
            return real(a, b)
        with tempfile.TemporaryDirectory() as d, mock.patch.object(core, "os", OsView(name, replace=replace)), mock.patch.object(core.time, "sleep", sleeps.append), \
                mock.patch.object(core.winproc, "replace_file", replace):   # Windows 分支走 winproc.replace_file（POSIX 语义替换）；这里只测外层退避
            target = Path(d, "job.json"); target.write_text("old")
            try:
                core.replace_retrying(self.tmp(d), target); error = None
            except PermissionError as e:
                error = e
            return error, target.read_text()

    @staticmethod
    def tmp(d):
        p = Path(d, "job.tmp"); p.write_text("new"); return p

    def test_windows_retries_until_target_is_free(self):
        sleeps = []
        self.assertEqual(self.run_case("nt", 2, sleeps), (None, "new"))
        self.assertEqual(sleeps, list(core.WIN_REPLACE_RETRIES[:2]))

    def test_windows_gives_up_after_all_retries(self):
        sleeps = []
        error, content = self.run_case("nt", len(core.WIN_REPLACE_RETRIES) + 1, sleeps)
        self.assertIsInstance(error, PermissionError); self.assertEqual(content, "old")
        self.assertEqual(sleeps, list(core.WIN_REPLACE_RETRIES))

    def test_other_platforms_do_not_retry(self):
        sleeps = []
        error, content = self.run_case("posix", 1, sleeps)
        self.assertIsInstance(error, PermissionError); self.assertEqual((content, sleeps), ("old", []))


class RuntimeEnv(unittest.TestCase):
    def test_paths_are_bundled_only(self):
        env = core.win_runtime_env(r"D:\S2Arm\Runtime", r"D:\S2Arm\Engine", r"C:\Windows")
        self.assertEqual(env["PATH"].split(";"), [
            r"D:\S2Arm\Runtime\python", r"D:\S2Arm\Runtime\python\Scripts",
            r"D:\S2Arm\Runtime\git\usr\bin", r"D:\S2Arm\Runtime\zstd",
            r"C:\Windows\system32", r"C:\Windows", r"C:\Windows\System32\WindowsPowerShell\v1.0"])
        self.assertNotIn("DOTNET_ROOT", env, "v0.5.0 起不带 .NET：C# 件是 NativeAOT 单文件")
        self.assertEqual((env["S2A_BASH"], env["PYTHONHOME"], env["S2A_PREBUILT_TOOLS"]),
                         (r"D:\S2Arm\Runtime\git\usr\bin\bash.exe",
                          r"D:\S2Arm\Runtime\python", r"D:\S2Arm\Engine\Binaries\Prepare"))
        self.assertEqual(env["LC_ALL"], "C.UTF-8", "机器上 LANG=C / en_US 时 cygpath 在中文处截断路径（#85）")


    def test_git_usr_bin_keeps_only_named_programs(self):
        # 安装包瘦身（11-16）：usr/bin 只带引擎脚本文本里叫得出名字的程序 + bash 等必带件；vim 一族照旧不带。期望手写
        import importlib.util
        spec = importlib.util.spec_from_file_location("brw", ROOT / "launchers/windows/bundle_runtime_win.py")
        brw = importlib.util.module_from_spec(spec); spec.loader.exec_module(brw)
        d = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        for n in ("sort.exe", "svn.exe", "vim.exe", "bash.exe", "sh.exe", "msys-2.0.dll", "join.exe", "joined.exe"):
            (d / n).write_bytes(b"not a PE")
        texts = ["printf 'x' | sort -u | join - other.tsv", "# 编辑用 vim"]
        self.assertEqual(brw.git_keep(d, texts), {"sort.exe", "join.exe", "bash.exe", "sh.exe", "msys-2.0.dll"})
        self.assertEqual(brw.pe_imports(d / "sort.exe"), [], "不是 PE：没有导入")

    def test_env_cmd_saves_player_path_before_replacing_it(self):
        # Metal 工具装在自定义目录、只加进 PATH：s2arm_env.cmd 换 PATH 之前存进 S2A_USER_PATH，作业层按它找（11-09 Windows 出包机实测 #137）
        import importlib.util
        spec = importlib.util.spec_from_file_location("brw", ROOT / "launchers/windows/bundle_runtime_win.py")
        brw = importlib.util.module_from_spec(spec); spec.loader.exec_module(brw)
        with tempfile.TemporaryDirectory() as d:
            brw.env_cmd(Path(d)); lines = Path(d, "s2arm_env.cmd").read_text(encoding="utf-8").splitlines()
        save = lines.index('if not defined S2A_USER_PATH set "S2A_USER_PATH=%PATH%"')
        reset = next(i for i, l in enumerate(lines) if l.startswith('set "PATH='))
        self.assertLess(save, reset)

    def test_metal_dir_is_chosen_in_parent_and_passed_explicitly(self):
        # 作业进程按玩家环境选一次 Metal 目录（显式 → 玩家 PATH → 官方默认）；显式目录无效也原样传下去，由出包预检报缺
        with tempfile.TemporaryDirectory(prefix="metal 中文 ") as d:
            tools = Path(d, "自定义 安装"); tools.mkdir()
            exe = tools / "metal.exe"; exe.write_text("#!/bin/sh\n", encoding="utf-8"); exe.chmod(0o755)
            with patch.dict(os.environ, {"PATHEXT": ".EXE"}):
                self.assertEqual(core.win_metal_dir({"PATH": str(Path(d, "随包")), "S2A_USER_PATH": str(tools)}), str(tools))
            self.assertEqual(core.win_metal_dir({"S2A_METAL_BIN": str(tools)}), str(tools))
            self.assertEqual(core.win_metal_dir({"S2A_METAL_BIN": str(Path(d, "没有这个目录"))}), str(Path(d, "没有这个目录")))
            self.assertEqual(core.win_metal_dir({"PATH": "", "S2A_USER_PATH": ""}), str(Path(r"C:\Program Files\Metal Developer Tools\bin")))

    def test_metal_found_by_registry_and_program_files(self):
        # 官方安装器（NSIS）不写 PATH：改过安装目录只记在卸载登记里；启动器开着时才装的，进程 PATH 是旧的要读注册表现在的 PATH；
        #   系统不在 C 盘时官方默认目录跟 %ProgramW6432%。每种都要认得，且找过的每一处写进错误摘要
        from pack.orch import oneclick_win as ow
        def put(*parts):
            p = Path(*parts, "metal.exe"); p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("#!/bin/sh\n", encoding="utf-8"); p.chmod(0o755); return p
        with tempfile.TemporaryDirectory(prefix="metal 中文 ") as d, patch.dict(os.environ, {"PATHEXT": ".EXE"}):
            stale = {"PATH": "", "S2A_USER_PATH": str(Path(d, "旧 PATH"))}
            custom = put(d, "D 盘 工具", "bin")                 # 安装界面改到别处：卸载登记的安装目录
            self.assertEqual(ow.metal_exe(stale, reg=([], [str(Path(d, "D 盘 工具"))])), custom)
            old = put(d, "早期版本", "ios", "bin")               # 早期版本布局 ios\bin
            self.assertEqual(ow.metal_exe(stale, reg=([], [str(Path(d, "早期版本"))])), old)
            fresh = put(d, "装完没重启", "bin")                  # 注册表里现在的 PATH 优先于卸载登记
            self.assertEqual(ow.metal_exe(stale, reg=([str(fresh.parent)], [str(Path(d, "D 盘 工具"))])), fresh)
            pf = put(d, "E 盘 Program Files", "Metal Developer Tools", "bin")   # 系统在别的盘
            self.assertEqual(ow.metal_exe(dict(stale, ProgramW6432=str(Path(d, "E 盘 Program Files"))), reg=([], [])), pf)
            user = put(d, "玩家 PATH")                           # 启动时 PATH 里有的仍最优先
            self.assertEqual(ow.metal_exe({"S2A_USER_PATH": str(user.parent)}, reg=([str(fresh.parent)], [str(Path(d, "D 盘 工具"))])), user)
            picked = put(d, "手选 文件夹")                       # 玩家手动指定的优先于自动查找
            self.assertEqual(ow.metal_exe(dict(stale, S2A_METAL_PICKED=str(picked.parent)), reg=([str(fresh.parent)], [])), picked)
            self.assertEqual(ow.metal_exe(dict(stale, S2A_METAL_PICKED=str(Path(d, "手选后删了"))), reg=([str(fresh.parent)], [])), fresh)   # 选过的被删了：照常往下找
            rows = ow.metal_search(stale, reg=([], [str(Path(d, "没装"))]))
            self.assertEqual([r[0] for r in rows], ["启动器启动时的 PATH", "注册表里现在的 PATH", "官方安装器登记的安装目录", "官方安装器登记的安装目录", "官方默认目录", "官方默认目录"])
            # 最后那条写死的 C:\Program Files\Metal Developer Tools 查的是这台机器真的系统目录：装了 Metal 工具的 Windows 上会找到（11-16 原生 Windows 上红），
            #   不算在「夹具里各处都没有」里
            sysdef = r"c:\program files\metal developer tools"
            self.assertFalse(any(ok for _, path, ok in rows if not str(path).lower().startswith(sysdef)))
            text = ow.metal_search_text(rows)
            self.assertIn("启动器启动时的 PATH：里面没有 metal.exe", text)
            self.assertIn(str(Path(d, "没装", "ios", "bin", "metal.exe")) + " → 没有", text)

    def test_env_cmd_is_pure_ascii(self):
        # cmd 按玩家机代码页（中文系统 936）读 .cmd：不带 BOM 的 UTF-8 中文会按 GBK 拆字、吞掉行尾，下一行 set 失效
        import importlib.util
        spec = importlib.util.spec_from_file_location("brw", ROOT / "launchers/windows/bundle_runtime_win.py")
        brw = importlib.util.module_from_spec(spec); spec.loader.exec_module(brw)
        with tempfile.TemporaryDirectory() as d:
            brw.env_cmd(Path(d)); raw = Path(d, "s2arm_env.cmd").read_bytes()
        self.assertEqual([b for b in raw if b > 0x7F], [])

    def test_same_path_as_bundled_env_cmd(self):
        # 作业环境与随包 s2arm_env.cmd（启动器 / 手工起出包用）同一份 PATH：两处各写一遍，这里核一致
        import importlib.util, ntpath
        spec = importlib.util.spec_from_file_location("brw", ROOT / "launchers/windows/bundle_runtime_win.py")
        brw = importlib.util.module_from_spec(spec); spec.loader.exec_module(brw)
        with tempfile.TemporaryDirectory() as d:
            brw.env_cmd(Path(d)); text = Path(d, "s2arm_env.cmd").read_text(encoding="utf-8")
        self.assertIn('set "LC_ALL=C.UTF-8"', text.splitlines())
        line = next(l for l in text.splitlines() if l.startswith('set "PATH='))
        bundled = [x.replace("%S2A_RT%", "") for x in line[len('set "PATH='):-1].split(";") if x.startswith("%S2A_RT%")]
        self.assertEqual(bundled, core.WIN_RUNTIME_PATH)


class Reader(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__("shutil").rmtree(self.t, ignore_errors=True))
        self.diags = []
        self.c = types.SimpleNamespace(engine=self.t, diagnostic=lambda message, **kw: self.diags.append(dict(kw, message=message)))
        self.d = self.t / "Binaries/Prepare/nx_unpack"; self.d.mkdir(parents=True)
        fake = types.SimpleNamespace(nx_unpack_id=lambda: "abc123abc123")   # 身份由 oneclick_win 算（另有单测）；这里只核「相同才用」
        self.mods = patch.dict(sys.modules, {"pack.orch.oneclick_win": fake, "pack.orch": types.SimpleNamespace(oneclick_win=fake)})
        self.mods.start(); self.addCleanup(self.mods.stop)

    def test_prebuilt_same_identity(self):
        """给 NativeAOT 单文件 nx_unpack.exe（调用方都直接执行它，不再有 .dll）"""
        (self.d / "nx_unpack.exe").write_bytes(b"x"); (self.d / ".src_digest").write_text("abc123abc123\n")
        with as_windows(toolchain):
            self.assertEqual(toolchain.reader(self.c), self.d / "nx_unpack.exe")
        self.assertEqual(self.diags, [])

    def test_stale_warns_and_continues(self):
        """维护者 11-17：校验不过不再拒绝。期望手写：身份对不上、exe 在 → 照常返回 exe、发一条 warning 级 reader_unavailable；
        身份不在（.src_digest 读不到）同样照用；exe 不在 → 物理上用不了，报 app_control_blocked（D-9G73）"""
        (self.d / "nx_unpack.exe").write_bytes(b"x")
        for case in ("stale", "no_digest"):
            with self.subTest(case=case), as_windows(toolchain):
                self.diags.clear()
                if case == "stale": (self.d / ".src_digest").write_text("000000000000\n")
                else: (self.d / ".src_digest").unlink()
                self.assertEqual(toolchain.reader(self.c), self.d / "nx_unpack.exe")
                self.assertEqual([(d["code"], d["severity"]) for d in self.diags], [("reader_unavailable", "warning")])

    def test_missing_is_named_failure(self):
        (self.d / ".src_digest").write_text("abc123abc123\n")   # 身份对、exe 不在
        with as_windows(toolchain), self.assertRaises(core.Failure) as e:
            toolchain.reader(self.c)
        # D-9G73：装好后 exe 不见了 = 杀毒软件隔离；期望手写：码 app_control_blocked，原因句点名 nx_unpack.exe、指去保护历史还原，不叫玩家重装
        self.assertEqual(e.exception.code, "app_control_blocked")
        self.assertIn("nx_unpack.exe", str(e.exception))
        self.assertIn("保护历史记录", str(e.exception))
        self.assertNotIn("重新安装", str(e.exception))


class WinOneclick(unittest.TestCase):
    # 打 iPhone 包的 Windows 分支：oneclick_win 出完，交付成 <款>.ipa + <款>资源包（与 Mac 同名同处，收尾 deliver 搬进这款的 image/ios），末条 result 改指交付件；
    #   出包红（result ok 假）不交付。期望手写：款名取 oneclick_win 产物 ipa 的文件名（t<程序号小写>）
    def setUp(self):
        import ipa
        self.ipa = ipa
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__("shutil").rmtree(self.t, ignore_errors=True))
        (self.t / "Shell").mkdir(); (self.t / "Shell/manifest.json").write_text("{}")
        self.out = self.t / "iPhone/0100FD70134FA000"; self.out.mkdir(parents=True)
        self.events = self.t / "ev.jsonl"; self.calls = []
        self.envs = []
        def run(args, log, env=None, on_line=None, **kw):
            self.calls.append([str(a) for a in args]); self.envs.append(env)
            if "--from-extract" in map(str, args):
                pk = self.out / "work/pkg/product/pkg"; (pk / "pack").mkdir(parents=True)
                (pk / "t0100fd70134fa000.ipa").write_bytes(b"ipa"); (pk / "pack/a.bin").write_bytes(b"a")
                self.events.write_text(__import__("json").dumps(dict(ev="result", ok=self.ok, ipa=str(pk / "t0100fd70134fa000.ipa"), pack=str(pk / "pack"),
                                                                      build_id="b1", graphics_mode="direct")) + "\n")
            else:   # deliver_pack.py <源> <目标>
                __import__("shutil").copytree(args[-2], args[-1])
        self.c = types.SimpleNamespace(r={"resources": str(self.t)}, private=self.t, engine=ROOT, run=run, keys=self.t / "keys/prod.keys")

    def go(self):
        self.ipa.win_oneclick(self.c, ROOT / "pack/orch/oneclick_win.py", self.t / "extract", self.t / "Fonts", self.out, self.events, self.t / "log", {}, None)
        return self.ipa.oneclick_result(self.events)

    def test_delivered_like_mac(self):
        self.ok = True
        res = self.go()
        self.assertEqual((res["ipa"], res["pack"]), (str(self.out / "t0100fd70134fa000.ipa"), str(self.out / "t0100fd70134fa000资源包")))
        self.assertEqual(((self.out / "t0100fd70134fa000.ipa").read_bytes(), (self.out / "t0100fd70134fa000资源包/a.bin").read_bytes()), (b"ipa", b"a"))
        first = self.calls[0]
        self.assertEqual(first[first.index("--shell") + 1], str(self.t / "Shell"))
        self.assertEqual((res["build_id"], res["graphics_mode"]), ("b1", "direct"))

    def test_sources_follow_from_extract(self):   # #613：原件与游戏号跟在 --from-extract 后面交给 oneclick_win（多程序合集的其余序号从原件解）
        self.ok = True
        self.ipa.win_oneclick(self.c, ROOT / "pack/orch/oneclick_win.py", self.t / "extract", self.t / "Fonts", self.out, self.events, self.t / "log", {}, None,
                              sources=[self.t / "a.xci", self.t / "b.nsp"], gid="0100FD70134FA000")
        self.assertEqual(self.calls[0][-4:], ["--title-id", "0100FD70134FA000", str(self.t / "a.xci"), str(self.t / "b.nsp")])
        # #943：从原件解其余序号要玩家导入的密钥，作业层交 S2A_KEYS 下去（不给就落回 ~/.switch/prod.keys，玩家机没有）
        self.assertEqual(self.envs[0]["S2A_KEYS"], str(self.t / "keys/prod.keys"))

    def test_failed_pack_not_delivered(self):
        self.ok = False
        res = self.go()
        self.assertFalse(res["ok"]); self.assertEqual(list(self.out.glob("*.ipa")), [])

    def test_no_shell_is_named(self):
        (self.t / "Shell/manifest.json").unlink()
        with self.assertRaises(core.Failure) as e:
            self.ipa.bundled_shell(self.c)
        self.assertEqual(e.exception.code, "tool_missing")


class ShellEngineMatch(unittest.TestCase):
    """#1043：应用内更新回滚不完整，Engine/engine.json 新壳号、Shell/manifest.json 旧壳号 → 出包开头就报「没装完整、覆盖安装」（engine_incomplete），
    不等到 ship_ext 核壳编号才挂。维护者 11-17：校验不过不再拒绝出包。期望手写（壳号取自玩家原报）：对得上 → 照常返回 Shell 目录、不发诊断；
    对不上 → 照常返回 Shell 目录、发一条 warning 级 engine_incomplete（说「照常出包」与「覆盖安装」、内部原文带两个壳号）；任一边没写壳号（开发树 / 旧格式）→ 不发"""
    def setUp(self):
        import ipa
        self.ipa = ipa
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__("shutil").rmtree(self.t, ignore_errors=True))
        (self.t / "Shell").mkdir(); (self.t / "Engine").mkdir()
        self.diags = []
        self.c = types.SimpleNamespace(r={"resources": str(self.t)}, private=self.t, engine=self.t / "Engine",
                                       diagnostic=lambda message, **kw: self.diags.append(dict(kw, message=message)))

    def put(self, engine, shell):
        import json
        (self.t / "Engine/engine.json").write_text(json.dumps({"shellId": engine} if engine else {}))
        (self.t / "Shell/manifest.json").write_text(json.dumps({"details": {"shellId": shell}} if shell else {}))

    def test_same_shell_ok(self):
        self.put("e0c983c072e29416", "e0c983c072e29416")
        self.assertEqual(self.ipa.bundled_shell(self.c), self.t / "Shell"); self.assertEqual(self.diags, [])

    def test_half_updated_app_warns_and_continues(self):
        self.put("e0c983c072e29416", "b96e9b50fef413c0")
        self.assertEqual(self.ipa.bundled_shell(self.c), self.t / "Shell")
        self.assertEqual(len(self.diags), 1); d = self.diags[0]
        self.assertEqual((d["code"], d["severity"]), ("engine_incomplete", "warning"))
        self.assertIn("照常出包", d["message"]); self.assertIn("覆盖安装", d["message"])
        self.assertIn("e0c983c072e29416", d["internal"]); self.assertIn("b96e9b50fef413c0", d["internal"])

    def test_missing_ids_do_not_block(self):
        for engine, shell in ((None, "b96e9b50fef413c0"), ("e0c983c072e29416", None), (None, None)):
            with self.subTest(engine=engine, shell=shell):
                self.put(engine, shell)
                self.assertEqual(self.ipa.bundled_shell(self.c), self.t / "Shell"); self.assertEqual(self.diags, [])


class FakeW:
    """pack/orch/install_win.py 的替身：设备服务列表、签名装机推包结果由用例给"""
    class Fail(Exception):
        def __init__(self, msg, code=1, err=None): super().__init__(msg); self.code = code; self.err = err
    def __init__(self, devs, bid="s2arm.T.0100", fail=None, impactor="ok", plume=True):
        self.devs, self.bid, self.fail, self.calls = devs, bid, fail, []
        import impactor as real
        def status(resources=None):   # 随包判定走真实现（resources 下有没有两份二进制）；不随包时按用例给
            st = real.status(resources=resources) if resources else None
            return st if st else {"verdict": impactor, "code": None if impactor == "ok" else "impactor_missing",
                                  "download": "https://github.com/claration/impactor/releases/tag/v2.7.3"}
        self.team = "ABCDE12345"   # 真实 accounts.json 每个账号都带十位团队号（审计 P2-253 Windows 实机核过）
        self.unselected, self.picks = [], []   # 账号存档里存着却没选中的 Apple ID（D-ZM37）；picks 记 select_only_account 收到的 plumesign
        def select_only(exe):
            self.picks.append(exe)
            if len(self.unselected) != 1: return None
            self.unselected = []; return "a@x"
        self.impactor = types.SimpleNamespace(status=status, bundled=real.bundled, logged_in=lambda: self.logged, selected_team=lambda: self.team,
                                              unselected_accounts=lambda: self.unselected, select_only_account=select_only)
        self.logged = False
        self.plumesign_path = lambda given=None: "C:/plumesign.exe" if plume else None
        self.cert_hit, self.cert_calls = None, []   # cert_check（签前核开发证书）：缺省「不用问」；用例给 {name, machine, created} 测问玩家
    def cert_check(self, exe, team, log=None):
        self.cert_calls.append((exe, team))
        return self.cert_hit
    def mux_devices(self):
        if isinstance(self.devs, Exception): raise self.devs
        return self.devs
    def run(self, ns, say):
        self.calls.append(ns)
        if self.fail: raise self.Fail("plumesign 退 1", 1, self.fail)
        return self.bid
    def install_app(self, ns, say=None):
        """新接口（WinPhone.install 调）：同 run 但只签名装机，返回包名"""
        self.calls.append(ns)
        if self.fail: raise self.Fail("plumesign 退 1", 1, self.fail)
        return self.bid
    def push_pack(self, udid, bid, pack, devpush=None, full=False, say=None, docs=None, ctx=None, apply_after=False, progress=None, stage=None, pack_root=None, accept_low_space=False):
        """新接口（WinPhone.push 调）→ (文件数, 字节数)；progress 记下来供核对（#266 / #308：推送进度回调）；pack_root 记下来核资源包根单独给"""
        self.progress = progress; self.pack_root = pack_root
        if progress: progress(0, 100, 0, 1); progress(100, 100, 1, 1)
        if getattr(self, "needs_player", None):
            e = self.Fail(self.needs_player, 0, "manual_unpack_required"); e.transferred = (787, 23_000_000_000); raise e
        return sum(1 for f in Path(pack).rglob("*") if f.is_file()), sum(f.stat().st_size for f in Path(pack).rglob("*") if f.is_file())


class WinInstall(unittest.TestCase):
    # 期望手写：同一台 USB + Wi-Fi 两条算一台；请求点名的 UDID 不在列 → device_not_connected；两台不点名 → device_not_connected；
    #   install_win 的共用错误码原样带出（login_identity_rejected）；成功回装机后实际包名
    def setUp(self):
        import install
        self.m = install
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__("shutil").rmtree(self.t, ignore_errors=True))
        p = patch.object(install, "phone_games", lambda c, ph: set()); p.start(); self.addCleanup(p.stop)   # 读手机游戏清单另有用例
        p = patch.object(install, "pick_pack_root", lambda c, ph, g: f"Library/Application Support/S2Arm/games/{g}/资源包"); p.start(); self.addCleanup(p.stop)   # 选哪处另有用例（test_install）
        (self.t / "lib").mkdir(); (self.t / "logs").mkdir()
        self.gid = "0100000000000047"
        self.progress_events = []   # 作业计数事件 (done, total, unit)
        emit = lambda *a, **k: self.progress_events.append((k.get("done"), k.get("total"), k.get("unit"))) if k.get("unit") else None
        self.c = jobcase.fake_context(self.t, r={"gameId": self.gid}, library=self.t / "lib", work_root=self.t / "work", private=self.t, logs=self.t / "logs", task="k", engine=self.t, emit=emit,
                                       begin=lambda *a, **k: None, last_log=None, catalog=lambda: {"schema": 1, "games": []},
                                       lock=lambda *a, **k: contextlib.nullcontext())   # 装机类作业的手机锁 / 程序锁（ipa.phone_lock / program_locks）
        ios_fixture(self.c, self.gid)   # 出好的 iPhone 版：image/ios/kit + image/ios/pack

    def go(self, w, **r):
        """「安装 S2Arm」的 Windows 分支（update_phone → phone_for → WinPhone + assemble_shell + install_shell：plumesign 签名装壳）"""
        import ipa as packer
        r = {"device": "U1", **r}   # 装机一律带玩家选的那台；device=None 测「没带设备号」
        self.c.r.update({k: v for k, v in r.items() if v is not None})
        if r["device"] is None: self.c.r.pop("device", None)
        gid = self.c.r["gameId"].upper()
        # assemble_shell 替身：在游戏库临时目录出一个真 zip ipa（WinPhone.install 不解包，但 install_shell 要把它放到默认位置根 S2Arm.ipa）
        def fake_assemble(c, *, dest=None):
            self.assembled = self.t / "lib" / ".container-t" / "S2Arm.ipa"; self.assembled.parent.mkdir(exist_ok=True)
            with __import__("zipfile").ZipFile(self.assembled, "w") as z:
                z.writestr("Payload/S2Arm.app/Info.plist", __import__("plistlib").dumps({"CFBundleIdentifier": "dev.s2arm.games", "CFBundleExecutable": "S2Arm"}))
            return self.assembled, [], "SID1"
        import rewrite_refresh   # 改写产物核对另有单测（test_rewrite_refresh）；这里只测 Windows 装机
        with patch.object(self.m, "win_install_mod", return_value=w), \
             patch.object(rewrite_refresh, "refresh_ios", return_value=[]), \
             patch.object(packer, "assemble_shell", side_effect=fake_assemble), patch.object(packer, "current_shell_id", return_value="SID1"), \
             patch.object(self.m.WinPhone, "backup", lambda ph, g: None), \
             as_windows(self.m):   # 覆盖装前的存档备份另有用例（test_overwrite_backup_* / test_no_record_and_no_team_*）
            return self.m.update_phone(self.c)

    def test_one_phone_two_links(self):
        w = FakeW([("U1", "USB", 3), ("U1", "Network", 9)])
        res = self.go(w)
        self.assertEqual((res["bundleId"], res["device"]), ("s2arm.T.0100", "U1"))
        self.assertEqual(w.calls[0].udid, "U1")
        self.assertEqual(Path(w.calls[0].ipa).suffix, ".ipa")
        # plumesign 定的包名记在这台的设备记录里（不再有 bundle.txt），下次 WinPhone 起来就认得、覆盖装前能备份
        rec = layout.load_device(self.c, "U1")
        self.assertEqual((rec["bundleId"], rec.get("games", {}), rec["shellId"]), ("s2arm.T.0100", {}, "SID1"))   # 只装壳、不推任何一款
        self.assertTrue((self.t / "lib" / "S2Arm.ipa").is_file())
        with patch.object(self.m, "win_install_mod", return_value=w), as_windows(self.m):
            self.assertEqual(self.m.WinPhone(self.c).bid, "s2arm.T.0100")

    def test_signed_record_keeps_device_kind(self):
        # #1962 复测：游戏库状态写「已推到 iPad」靠设备记录里存的 deviceKind（期望手写：设备服务报 ipad → 记 iPad；报不出 → 空，前端填「iPhone / iPad」）
        for reported, want in (("ipad", "iPad"), (None, "")):
            with self.subTest(reported=reported):
                w = FakeW([("U1", "USB", 3)])
                if reported:
                    w.device_kind = lambda udid, k=reported: k
                real = w.install_app
                def signed(ns, say=None):   # 同 install_win.install_app：签好的包里读到期限才记 signed
                    ns.signed_expiry = dict(expires=1000, days=7)
                    return real(ns, say)
                w.install_app = signed
                self.go(w)
                self.assertEqual(layout.load_device(self.c, "U1")["signed"]["deviceKind"], want)

    def test_bundle_matches_mac_lowercase_team(self):
        # 9401：交给 plumesign 的包名和 Mac 一致（io.s2arm.games.<小写团队号>）；这台记录过只差大小写的包名就沿用（不在手机上多出一个 S2Arm）；
        #   读不到团队号 → 不传（照旧由 plumesign 定）
        w = FakeW([("U1", "USB", 3)], bid="io.s2arm.games.abcde12345"); w.team = "ABCDE12345"
        p = patch.object(self.m.WinPhone, "backup", lambda self, g: None); p.start(); self.addCleanup(p.stop)   # 只看包名：覆盖装前的存档备份另有用例
        self.go(w)
        self.assertEqual(w.calls[-1].bundle, "io.s2arm.games.abcde12345")
        layout.save_device(self.c, "U1", dict(layout.load_device(self.c, "U1"), bundleId="io.s2arm.games.ABCDE12345"))
        self.go(w)
        self.assertEqual(w.calls[-1].bundle, "io.s2arm.games.ABCDE12345")
        w.team = None
        self.go(w)
        self.assertIsNone(w.calls[-1].bundle)

    def test_overwrite_backup_only_skips_when_phone_says_not_installed(self):
        # #333：覆盖装前备份存档。只有手机明确答「没有这个 App」（ApplicationLookupFailed / S2A_APP_MISSING）才跳过；锁屏 / 断开等取不到 →
        #   不悄悄当成没装过直接覆盖，停在确认 save_backup_failed（装机原则 11-11）；玩家确认（acceptNoBackup）后照装。
        #   这台没记录包名时按这次要装的包名查（可能有 Mac 装的同包名 App 和存档）
        w = FakeW([("U1", "USB", 3)]); w.team = "ABCDE12345"
        asked = []
        def client(result):
            def make(c, udid, bid, work):
                asked.append(bid)
                class Cl:
                    def get_tree(self, rel, dest):
                        if result is True:
                            Path(dest).mkdir(parents=True); (Path(dest) / "s.dat").write_text("x"); return True, ""
                        return False, result
                return Cl()
            return make
        with patch.object(self.m, "win_install_mod", return_value=w), as_windows(self.m):
            self.c.r["device"] = "U1"; ph = self.m.WinPhone(self.c)
            self.assertIsNone(ph.bid)
            with patch.object(self.m, "devpush_client", client("取不到 io.s2arm.games.abcde12345 的容器（App 没装或不是开发签名）：unknown error `ApplicationLookupFailed` returned from device")):
                self.assertIsNone(ph.backup(self.gid))
            self.assertEqual(asked[-1], "io.s2arm.games.abcde12345")
            with patch.object(self.m, "devpush_client", client("S2A_APP_MISSING：手机上没装 io.s2arm.games.abcde12345（installation_proxy 查过）")):
                self.assertIsNone(ph.backup(self.gid))
            with patch.object(self.m, "devpush_client", client("取不到 io.s2arm.games.abcde12345 的容器（App 没装或不是开发签名）：device socket io failed")), \
                 self.assertRaises(self.m.Failure) as e:
                ph.backup(self.gid)
            self.assertEqual(e.exception.code, "save_backup_failed"); self.assertIn("可能丢档", str(e.exception))
            self.c.r["acceptNoBackup"] = True
            with patch.object(self.m, "devpush_client", client("取不到 io.s2arm.games.abcde12345 的容器（App 没装或不是开发签名）：device socket io failed")):
                self.assertIsNone(ph.backup(self.gid))   # 玩家确认后：不备份照装
            del self.c.r["acceptNoBackup"]
            with patch.object(self.m, "devpush_client", client(True)):
                self.assertTrue((ph.backup(self.gid) / "s.dat").is_file())

    def test_no_record_and_no_team_asks_instead_of_overwriting(self):
        # 审计 P2-253：这台没记过包名、又读不到团队号 → 查不了手机上有没有存档 → 停在确认 save_backup_failed，不悄悄交给 plumesign 直接覆盖；
        #   玩家确认（acceptNoBackup）后照装（装机原则 11-11）
        w = FakeW([("U1", "USB", 3)]); w.team = None
        with patch.object(self.m, "win_install_mod", return_value=w), as_windows(self.m):
            self.c.r["device"] = "U1"; ph = self.m.WinPhone(self.c)
            self.assertIsNone(ph.bid)
            with patch.object(self.m, "devpush_client", side_effect=AssertionError("不该去手机上查")), self.assertRaises(self.m.Failure) as e:
                ph.backup(self.gid)
            self.assertEqual(e.exception.code, "save_backup_failed"); self.assertIn("团队号", e.exception.internal)
            self.c.r["acceptNoBackup"] = True
            with patch.object(self.m, "devpush_client", side_effect=AssertionError("不该去手机上查")):
                self.assertIsNone(ph.backup(self.gid))

    def test_device_choice_errors(self):
        """装到哪台（11-13 维护者：插着一台手机就别说识别不了）。期望手写：设备服务里只有一台、没带设备号 → 装它；带了号而那台这次没列出 → 不换装到连着的另一台，
        报 device_not_connected（11-21：原先「只连着一台就装它」把游戏装进了没打算装的 iPad）；
        选的那台列着 → 装它（同网也算，不换插线的另一台）；多台对不上 → 没带号报 device_required、带了号报 device_not_connected 并说连着几台；
        一台都没有 → device_not_connected；设备服务连不上 → device_pairing_required"""
        import install
        with patch.object(install, "LINK_RETRY_WAIT", 0):
            w = FakeW([("U1", "USB", 3)]); self.go(w, device=None)
            self.assertEqual(w.calls[0].udid, "U1")
            for kind in ("USB", "Network"):
                with self.subTest(absent=kind), self.assertRaises(core.Failure) as e:
                    self.go(FakeW([("U2", kind, 3)]), device="U1")
                self.assertEqual(e.exception.code, "device_not_connected"); self.assertIn("连着 1 台", str(e.exception))
            with self.assertRaises(core.Failure) as e:
                self.go(FakeW([("U1", "USB", 3), ("U2", "USB", 4)]), device=None)
            self.assertEqual(e.exception.code, "device_required")
            __import__("shutil").rmtree(self.c.library / "devices", ignore_errors=True)   # 前面装过壳的设备记录会让它改说「连着的这台已是新版」（other_device_current，另有用例）
            with self.assertRaises(core.Failure) as e:
                self.go(FakeW([("U2", "USB", 3), ("U3", "Network", 4)]), device="U1")
            self.assertEqual(e.exception.code, "device_not_connected"); self.assertIn("连着 2 台", str(e.exception))
            for want in (None, "U1"):
                with self.subTest(empty=want), self.assertRaises(core.Failure) as e:
                    self.go(FakeW([]), device=want)
                self.assertEqual(e.exception.code, "device_not_connected")
            w = FakeW([("U2", "USB", 3), ("U1", "Network", 9)])   # 选的那台走 Wi-Fi、另一台插着线：装选的那台
            self.go(w, device="U1")
            self.assertEqual(w.calls[0].udid, "U1")
            with self.assertRaises(core.Failure) as e:
                self.go(FakeW(ConnectionRefusedError()))
            self.assertEqual(e.exception.code, "device_pairing_required")

    def test_shared_code_passes_through(self):
        with self.assertRaises(core.Failure) as e:
            self.go(FakeW([("U1", "USB", 3)], fail="login_identity_rejected"))
        self.assertEqual(e.exception.code, "login_identity_rejected")

    def test_cert_revoke_asked_before_signing(self):
        # plumesign 要新建证书、列表里有别人的：签名装机之前停下问（cert_revoke_confirm），不调 install_app；玩家点继续（acceptCertRevoke）照装
        w = FakeW([("U1", "USB", 3)]); w.cert_hit = dict(name="Apple Development: Fake Xcode", machine="Fake-Mac", created="2025-03-01")
        p = patch.object(self.m.WinPhone, "backup", lambda self, g: None); p.start(); self.addCleanup(p.stop)
        with self.assertRaises(core.Failure) as e:
            self.go(w)
        self.assertEqual(e.exception.code, "cert_revoke_confirm")
        self.assertIn("「Apple Development: Fake Xcode」", str(e.exception))
        self.assertEqual(w.calls, [])
        self.assertEqual(w.cert_calls, [("C:/plumesign.exe", w.team)])
        self.go(w, acceptCertRevoke=True)
        self.assertEqual(len(w.calls), 1)

    def test_appid_limit_shows_plain_reason_not_plumesign_english(self):
        """Apple 9120（Windows 实机原文：Developer API error 9120 … maximum App ID limit）：上屏是 free_appid_limit 的人话原因与办法，英文原文只进诊断"""
        w = FakeW([("U1", "USB", 3)], fail="free_appid_limit")
        with self.assertRaises(core.Failure) as e:
            self.go(w)
        import errors
        self.assertEqual(e.exception.code, "free_appid_limit")
        self.assertIn(errors.RECOVERY["free_appid_limit"], str(e.exception))
        self.assertNotIn("plumesign 退 1", str(e.exception))   # FakeW 的原文
        self.assertIn("plumesign 退 1", e.exception.internal)

    def test_gui_install_runs_the_11_06_backend(self):
        # 11-07 闸门接线核对：启动器「开始装机」发 install 作业 → install_windows → 真的 pack/orch/install_win.py（不用 FakeW），
        #   外部只换掉 plumesign / 设备服务 / 推包。期望照 11-06 金刚真机通过那次手写（状态文件里程碑节、evidence/winline_e2e_1106/kong_c3.log）：
        #   plumesign sign --package <ipa> --apple-id --register-and-install --udid <设备服务里 USB 那条的设备编号> -o <签好的 ipa>；
        #   0.6.0 起「安装 S2Arm」只装壳：不推任何一款的资源包（推游戏是另一个作业）
        import io, plistlib, subprocess, zipfile
        self.c.engine = Path(__file__).resolve().parents[3]
        W = self.m.win_install_mod(self.c)
        dp = self.c.engine / "Binaries/Prepare/devpush/s2a-devpush.exe"
        seen = {}
        def fake_run(cmd, **k):
            seen["cmd"] = [str(x) for x in cmd]
            out = Path(cmd[cmd.index("-o") + 1]); buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w") as z: z.writestr("Payload/S2Arm.app/Info.plist", plistlib.dumps({"CFBundleIdentifier": "s2arm.t.0100.ABCDE12345"}))
            out.write_bytes(buf.getvalue())
            return subprocess.CompletedProcess(cmd, 0, "Installation progress: 100%\nInstallation complete!\n", "")
        def fake_push(udid, bid, pack, devpush=None, full=False, say=None, docs=None, ctx=None, apply_after=False, progress=None, stage=None, pack_root=None, accept_low_space=False):
            seen["push"] = (udid, bid, Path(pack).name, devpush, full)
            progress(4 << 30, 8 << 30, 4, 8)   # Windows 推送进度回调 → 作业字节计数（#266 / #308）
            return (0, 0)
        # 玩家 Impactor 里选中的团队（impactor.selected_team，读真机 %APPDATA% 的 Impactor 账号存储）：登录过 Impactor 的 Windows 上能读到，
        #   装机就会带 --custom-identifier <容器包名>（install_win.sign_install 的设计）。这里按 11-06 那次「没读到团队」的期望测，带包名的另有用例
        with patch.object(W, "plumesign_path", return_value="C:/Impactor/plumesign.exe"), \
             patch.object(W, "mux_devices", return_value=[("U1", "Network", 9), ("U1", "USB", 7)]), \
             patch.object(W.impactor, "selected_team", lambda *a, **k: None), \
             patch.object(W.subprocess, "run", fake_run), patch.object(W, "push_pack", fake_push):
            res = self.go(W)
        cmd = seen["cmd"]
        self.assertEqual(cmd[:-1], ["C:/Impactor/plumesign.exe", "sign", "--package", str(self.assembled),
                                    "--apple-id", "--register-and-install", "--udid", "7", "-o"])
        self.assertIn("signed.ipa", cmd[-1])   # 签名输出文件名含 signed.ipa
        self.assertNotIn("push", seen, "安装 S2Arm 只装壳，不推资源包")
        self.assertEqual((res["bundleId"], res["device"]), ("s2arm.t.0100.ABCDE12345", "U1"))

    def test_bundled_impactor_zero_jump(self):
        # 11-07 维护者：Impactor 随启动器下发、玩家零跳转。期望手写：
        #   随包两份都在 + 已登录 → impactor / impactor_login 都 ✓，任何一项都没有下载链接；没登录 → 登录项 ✗、动作 openImpactor（不是链接）；
        #   随包缺一份 → impactor ✗、说安装包不完整（重新解压），没有下载链接；装机时把随包 plumesign 交给 install_win
        import impactor
        res = self.t / "launcher"; d = res / impactor.BUNDLE_DIR; d.mkdir(parents=True)
        for n in impactor.BUNDLED: (d / n).write_bytes(b"x")
        self.c.r["resources"] = str(res)
        w = FakeW([("U1", "USB", 3)], impactor="missing"); w.logged = True
        with patch.object(self.m, "win_install_mod", return_value=w):
            r = self.m.readiness_windows(self.c, self.gid)
        self.assertEqual([(i["id"], i["ok"], i["url"]) for i in r["items"]],
                         [("package", True, None), ("impactor", True, None), ("impactor_login", True, None), ("connected", True, None)])
        self.assertTrue(r["ready"]); self.assertIn("第三方开源工具", r["items"][1]["title"])
        w.logged = False
        with patch.object(self.m, "win_install_mod", return_value=w):
            r = self.m.readiness_windows(self.c, self.gid)
        login = r["items"][2]
        self.assertEqual((login["ok"], login["action"], login["url"]), (False, "openImpactor", None)); self.assertTrue(r["ready"])
        (d / impactor.PLUMESIGN_EXE).unlink()
        # 随包不全时会接着找玩家自己装的 Impactor（卸载登记、桌面 / 下载）：真装了 Impactor 的 Windows 上会找到，这里按「没装」测
        with patch.object(self.m, "win_install_mod", return_value=w), patch.object(impactor, "candidates", lambda: iter(())):
            r = self.m.readiness_windows(self.c, self.gid)
        imp = r["items"][1]
        self.assertFalse(imp["ok"]); self.assertTrue(r["ready"])   # 随包不全就接着认玩家自己装的；认不出也只提示
        (d / impactor.PLUMESIGN_EXE).write_bytes(b"x")
        w2 = FakeW([("U1", "USB", 3)]); self.go(w2)
        self.assertEqual(w2.calls[0].plumesign, str(d / impactor.PLUMESIGN_EXE))

    def test_unselected_account(self):
        # D-ZM37：Impactor 存了 Apple ID 却没选中，plumesign 报「No account selected」。期望手写：
        #   只存一个 → 装机签名前用 plumesign 选上（交的是装机用的同一份 plumesign），照常装好；
        #   存多个 → 清单登录项 ✗、说「没有选中」、动作 openImpactor，仍不挡装机（只提示）
        w = FakeW([("U1", "USB", 3)]); w.unselected = ["a@x"]
        res = self.go(w)
        self.assertEqual((w.picks, w.unselected, res["device"]), (["C:/plumesign.exe"], [], "U1"))
        import impactor   # 登录项只在随包形态（发版启动器）出现
        d = self.t / "launcher" / impactor.BUNDLE_DIR; d.mkdir(parents=True)
        for n in impactor.BUNDLED: (d / n).write_bytes(b"x")
        self.c.r["resources"] = str(self.t / "launcher")
        w = FakeW([("U1", "USB", 3)]); w.logged = True; w.unselected = ["a@x", "b@x"]
        with patch.object(self.m, "win_install_mod", return_value=w):
            r = self.m.readiness_windows(self.c, self.gid)
        login = next(i for i in r["items"] if i["id"] == "impactor_login")
        self.assertEqual((login["ok"], login["action"]), (False, "openImpactor")); self.assertIn("没有选中", login["detail"]); self.assertTrue(r["ready"])

    def test_logged_in_reads_impactor_account_store(self):
        import json as _j, impactor
        base = self.t / "appdata"; (base / "PlumeImpactor").mkdir(parents=True)
        self.assertFalse(impactor.logged_in(str(base)))                                   # 没有存档
        (base / "PlumeImpactor/accounts.json").write_text(_j.dumps({"selected_account": None, "accounts": {}}))
        self.assertFalse(impactor.logged_in(str(base)))                                   # 存档空
        (base / "PlumeImpactor/accounts.json").write_text(_j.dumps({"selected_account": "a@b.c", "accounts": {"a@b.c": {}}}))
        self.assertTrue(impactor.logged_in(str(base)))

    def test_bundle_pins_own_build_hashes(self):
        # 打包（11-18 起自编版）：缓存有就用，没有 / 不对从固定来源（我们公开仓的 Release）下，SHA-256 必须等于钉的值，不符拒打；不去官方发布页下；
        #   带许可、补丁原文与「基于上游 v2.7.3 的修改版」来源说明
        import hashlib, impactor
        sys.path.insert(0, str(ROOT / "launchers/windows")); import bundle_win
        cache = self.t / "cache"; cache.mkdir()
        for n in impactor.BUNDLED: (cache / n).write_bytes(n.encode())
        pins = {n: hashlib.sha256(n.encode()).hexdigest() for n in impactor.BUNDLED}
        patches = sorted(p.name for p in bundle_win.IMPACTOR_PATCHES.glob("*.patch"))
        self.assertTrue(patches, "仓库里有自编补丁")
        with patch.object(impactor, "BUNDLED", pins):
            bundle_win.bundle_impactor(self.t / "out1", cache=cache)
            self.assertEqual(sorted(p.name for p in (self.t / "out1").iterdir()),
                             sorted([*pins, *patches, "LICENSE-Impactor-MIT.txt", "LICENSE-apple-codesign-MPL-2.0.txt", "README-来源与许可.txt"]))
            readme = (self.t / "out1/README-来源与许可.txt").read_text(encoding="utf-8")
            for want in ("修改版", patches[0], "3rdparty/apple-codesign-0.29.0", pins[impactor.PLUMESIGN_EXE], impactor.SOURCE_TREE):
                self.assertIn(want, readme)
            # 缓存里的被改过：从固定来源重下、核对了才换上（缓存只是加速）
            (cache / impactor.IMPACTOR_EXE).write_bytes(b"tampered")
            got = []
            def fetch(url, to): got.append(url); Path(to).write_bytes(impactor.IMPACTOR_EXE.encode())
            bundle_win.bundle_impactor(self.t / "out2", cache=cache, fetch=fetch)
            self.assertEqual(got, [impactor.OWN_RELEASE + impactor.IMPACTOR_EXE])
            self.assertEqual((cache / impactor.IMPACTOR_EXE).read_bytes(), impactor.IMPACTOR_EXE.encode())
            # 缓存没有、来源上的也不对：拒打，说清地址与哈希，不留半截文件
            (cache / impactor.IMPACTOR_EXE).unlink()
            with self.assertRaises(SystemExit) as e:
                bundle_win.bundle_impactor(self.t / "out3", cache=cache, fetch=lambda url, to: Path(to).write_bytes(b"other"))
            self.assertIn("拒打", str(e.exception)); self.assertIn(impactor.OWN_RELEASE, str(e.exception))
            self.assertEqual(sorted(p.name for p in cache.iterdir()), [impactor.PLUMESIGN_EXE])
            # 下不来：报真原因（地址 + 错误）
            def down(url, to): raise OSError("连不上")
            with self.assertRaises(SystemExit) as e:
                bundle_win.bundle_impactor(self.t / "out4", cache=cache, fetch=down)
            self.assertIn("连不上", str(e.exception)); self.assertIn(impactor.OWN_RELEASE + impactor.IMPACTOR_EXE, str(e.exception))

    def test_readiness(self):
        with patch.object(self.m, "win_install_mod", return_value=FakeW([("U1", "USB", 3)], impactor="missing")):
            r = self.m.readiness_windows(self.c, self.gid)
        self.assertEqual([(i["id"], i["ok"]) for i in r["items"]], [("package", True), ("impactor", False), ("connected", True)])
        self.assertEqual((r["ready"], r["device"]), (True, "U1"))   # 缺 Impactor 只提示，直接去签名
        # 缺 Impactor 那一项带官方发布页（启动器按钮），其余项没有链接
        self.assertEqual([i["url"] for i in r["items"]], [None, "https://github.com/claration/impactor/releases/tag/v2.7.3", None])
        with patch.object(self.m, "win_install_mod", return_value=FakeW([("U1", "USB", 3)])):
            self.assertIsNone(self.m.readiness_windows(self.c, self.gid)["items"][1]["url"])


    def test_readiness_package_button_lands_on_export(self):
        # 11-19 落点契约（同 Mac）：iPhone 版没做好 → package 项带 openExport（按钮去这款的出包页，原先 Windows 没按钮）；做好了没有动作
        with patch.object(self.m, "win_install_mod", return_value=FakeW([("U1", "USB", 3)])), \
                patch.object(self.m, "ios_pack", side_effect=self.m.Failure("没做好")):
            pkg = self.m.readiness_windows(self.c, self.gid)["items"][0]
        self.assertEqual((pkg["id"], pkg["ok"], pkg["action"], pkg["url"]), ("package", False, "openExport", None))
        with patch.object(self.m, "win_install_mod", return_value=FakeW([("U1", "USB", 3)])):
            self.assertIsNone(self.m.readiness_windows(self.c, self.gid)["items"][0]["action"])

    def test_readiness_tells_ipad_from_iphone(self):
        # #487：连着的是 iPad 时入口写「装到 iPad」：清单结果带 deviceKind；设备服务读不出（只在 Wi-Fi 上）为空、界面照旧写 iPhone
        w = FakeW([("U1", "USB", 3)])
        for kind in ("ipad", "iphone", ""):
            w.device_kind = lambda udid, k=kind: k
            with patch.object(self.m, "win_install_mod", return_value=w):
                self.assertEqual(self.m.readiness_windows(self.c, self.gid)["deviceKind"], kind)
        w.device_kind = lambda udid: (_ for _ in ()).throw(OSError("设备服务断了"))
        with patch.object(self.m, "win_install_mod", return_value=w):
            self.assertEqual(self.m.readiness_windows(self.c, self.gid)["deviceKind"], "")

    def test_device_kind_reads_usb_product_id(self):
        # 苹果 USB 产品号：iPad 0x12AB、iPhone 0x12A8（本机实测 iPhone）；只认 USB 那条，Wi-Fi 那条没有产品号
        sys.path.insert(0, str(ROOT / "pack" / "orch")); import install_win
        rows = [{"DeviceID": 1, "Properties": {"SerialNumber": "PAD", "ConnectionType": "USB", "ProductID": 0x12AB}},
                {"DeviceID": 2, "Properties": {"SerialNumber": "PAD", "ConnectionType": "Network"}},
                {"DeviceID": 3, "Properties": {"SerialNumber": "PHONE", "ConnectionType": "USB", "ProductID": 0x12A8}},
                {"DeviceID": 4, "Properties": {"SerialNumber": "AIR", "ConnectionType": "Network"}}]
        with patch.object(install_win, "_mux_list", return_value=rows):
            self.assertEqual([install_win.device_kind(u) for u in ("PAD", "PHONE", "AIR", "")], ["ipad", "iphone", "", ""])
            self.assertEqual(install_win.mux_devices(), [("PAD", "USB", 1), ("PAD", "Network", 2), ("PHONE", "USB", 3), ("AIR", "Network", 4)])

class WinTarget(unittest.TestCase):
    def test_connected_only_no_models(self):
        import target
        c = types.SimpleNamespace(r={}, engine=ROOT)
        with as_windows(target), patch("install.win_install_mod", return_value=FakeW([("U1", "Network", 9), ("U1", "USB", 3), ("U2", "Network", 5)])):
            r = target.probe(c)
        self.assertEqual(r, dict(connected=[dict(id="U1", udid="U1", connection="wired"), dict(id="U2", udid="U2", connection="network")], models=[], osChoices=[]))


class WinPreflight(unittest.TestCase):
    # 环境检查在 Windows 上 = winenv（子进程、作业环境）：清单与 ready 原样转给启动器；winenv 没给 JSON → win_check_failed，不当成就绪
    def setUp(self):
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__("shutil").rmtree(self.t, ignore_errors=True))
        self.c = jobcase.fake_context(self.t, r={"resources": str(self.t)}, private=self.t, engine=ROOT, library=self.t / "lib",
                                       env=lambda: {"PATH": "x"}, fonts_dir=lambda: self.t / "Fonts")

    def run_with(self, stdout):
        seen = {}
        def fake(args, **kw):
            seen.update(args=args, env=kw["env"], req=__import__("json").loads(Path(args[-1]).read_text()))
            return types.SimpleNamespace(stdout=stdout, stderr="boom", returncode=1)
        with patch.object(toolchain.subprocess, "run", side_effect=fake):
            return toolchain.preflight_windows(self.c), seen

    def test_report_passed_through(self):
        rows = [{"id": "pack.fonts", "ok": False, "status": "missing", "code": "tool_missing"}]
        r, seen = self.run_with(__import__("json").dumps({"schema": 1, "ready": False, "checks": rows}))
        self.assertEqual((r["checks"], r["ready"]), (rows, False))
        self.assertEqual(seen["req"], {"shell": str(self.t / "Shell"), "targetDir": str(self.t / "lib")})
        self.assertEqual(seen["env"]["S2A_FONTS"], str(self.t / "Fonts"))

    def test_error_report_is_failure(self):
        # winenv 回 {ready: false, checks: [], error: {...}}：作业失败、不转空清单（11-07 发版产物端到端：空清单被当成功）
        with self.assertRaises(core.Failure) as e:
            self.run_with(__import__("json").dumps({"schema": 1, "ready": False, "checks": [], "error": {"code": "invalid_request", "message": "未知请求格式"}}))
        self.assertEqual(e.exception.code, "win_check_failed"); self.assertIn("invalid_request", str(e.exception))

    def test_no_json_is_failure(self):
        with self.assertRaises(core.Failure) as e:
            self.run_with("Traceback ...")
        self.assertEqual(e.exception.code, "win_check_failed")


class WinenvReadsRealSource(unittest.TestCase):
    def test_python_minimum_from_real_toolchain(self):
        # winenv 的 Python 最低版本按 toolchain.preflight 源码里的 add("Python", sys.version_info >= …) 读（不另写一份）：
        #   用真源码（不用夹具）核得出，preflight 改结构时这里先红（11-07 Windows 出包机作业层端到端：拆成两个函数后 python 一项全报 error）
        import importlib.util
        spec = importlib.util.spec_from_file_location("winenv_real", JOBS / "winenv.py")
        m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        self.assertEqual(m._python_minimum(), (3, 12))   # 手写期望：toolchain.preflight 现写的下限


class Clone(unittest.TestCase):
    def test_non_darwin_never_loads_libc(self):
        # 内置引擎初始化逐文件 core.clone：非 macOS 不探 clonefile（Windows 上 ctypes.CDLL(None) 抛 TypeError），照常拷字节、报进度
        import ctypes
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d, "a"), Path(d, "x/b"); a.write_bytes(b"abc"); seen = []
            with patch.object(core.sys, "platform", "win32"), patch.object(ctypes, "CDLL", side_effect=TypeError("CDLL(None)")):
                core.clone(a, b, lambda n, t: seen.append((n, t)))
            self.assertEqual((b.read_bytes(), seen[-1]), (b"abc", (3, 3)))


class EngineSnapshotCoversWinenv(unittest.TestCase):
    def test_files_winenv_reads_are_in_snapshot(self):
        # winenv 在玩家机上读的源文件（钉版清单、Python 最低版本定义、Metal 锁、选包预算、错误码）都要进发版引擎快照（bundle_engine.INPUTS）
        import importlib.util
        spec = importlib.util.spec_from_file_location("winenv_snap", JOBS / "winenv.py"); we = importlib.util.module_from_spec(spec); spec.loader.exec_module(we)
        sys.path.insert(0, str(ROOT / "launchers/macos"))
        with patch.dict(sys.modules):
            import bundle_engine as be
        def shipped(rel):
            return be.include_source(Path(rel)) and any(rel == top or rel.startswith(top + "/") for top in be.INPUTS)
        reads = [we.RUNTIME, we.PREFLIGHT, we.METAL_LOCK, we.SPACE_RULE, "pack/jobs/errors.py", "pack/orch/oneclick_win.py"]
        self.assertEqual([r for r in reads if not shipped(r)], [])

    def test_missing_pins_is_reinstall_not_contract_error(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("winenv_pins", JOBS / "winenv.py"); we = importlib.util.module_from_spec(spec); spec.loader.exec_module(we)
        self.assertEqual(we._recovery_code("python_requirements", "missing"), "tool_missing")


class PidAlive(unittest.TestCase):
    def test_windows_never_calls_os_kill(self):
        with as_windows(core, kill=unittest.mock.Mock(side_effect=AssertionError("Windows 上 os.kill(pid, 0) = 对进程组发 Ctrl+C"))), \
             patch.object(core.winproc, "alive", return_value=False) as alive:
            self.assertFalse(core.pid_alive(4321))
        alive.assert_called_once_with(4321)


if __name__ == "__main__":
    unittest.main(verbosity=2)


@unittest.skipUnless(os.name == "nt", "Windows ACL 封存只在 Windows 上有")
class SealedRomfsPrune(unittest.TestCase):
    def test_rmtree_removes_acl_sealed_store(self):
        # 照 pack/orch/roots.seal 在 Windows 上的封法（当前用户拒写 ACL）封一棵小树，出包后清理（library._rmtree）要能删掉
        sys.path.insert(0, str(ROOT))
        from pack.orch import roots
        import library
        with tempfile.TemporaryDirectory() as d:
            store = Path(d, "work", "roots", ".romfs_store", "g", "seed", "A")
            store.mkdir(parents=True); (store / "x.bin").write_bytes(b"x")
            roots.seal(Path(d, "work", "roots", ".romfs_store"))
            try:
                (store / "x.bin").unlink()   # 封存生效：普通删除被拒（启动器起的作业是普通令牌）
            except PermissionError:
                pass
            else:
                self.skipTest("当前令牌不受拒写 ACL 约束（提权管理员），封存复现不了；用普通令牌跑")
            library._rmtree(Path(d, "work"))
            self.assertFalse(Path(d, "work").exists())

    def test_rmtree_removes_readonly_attribute_files(self):
        # 11-16 维护者：只读位（别的来源放进来的只读文件）os.unlink 不认、报 WinError 5；library._rmtree 要去掉只读位后删干净。期望手写：整棵删掉
        import stat
        import library
        with tempfile.TemporaryDirectory() as d:
            sub = Path(d, "work", "sub"); sub.mkdir(parents=True); (sub / "a.bin").write_bytes(b"a")
            os.chmod(sub / "a.bin", stat.S_IREAD)
            with self.assertRaises(PermissionError):
                os.unlink(sub / "a.bin")
            library._rmtree(Path(d, "work"))
            self.assertFalse(Path(d, "work").exists())
