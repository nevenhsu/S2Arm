#!/usr/bin/env python3
"""Versioned local JSON request / NDJSON event entry point. No daemon required.

Subcommands (J10 / #186):
  run     --request <file>                      run one request (the Mac launcher's call; a bare --request still works)
  get     --private-root <dir> --task <id>      the journal of one task: state, stage, attempts, whether the worker is still there
  events  --private-root <dir> --task <id> [--since N] [--limit N]
                                                the task's NDJSON events, incrementally
  cancel  --private-root <dir> --task <id> [--signal TERM|KILL]
                                                signal the worker this task recorded (refuses a reused pid)
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import threading
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parent))
if os.name == "nt":   # 作业主程序自己也要长路径处理：子 Python 进程靠 sitecustomize 装，主进程没人装（注册表 LongPathsEnabled 关着时，数据目录 ~200 字符起启动器后端就起不来）
    sys.path.insert(1, str(Path(__file__).resolve().parents[2]))
    from pack.orch import winlong as _winlong
    _winlong.install()
import community
import diag
import embedded
import install
import ipa
import layout
import library
import winproc
import saves
import sizes
import target
import toolchain
from core import (KEEP_DAYS, KEEP_FAILED_PER_OPERATION, KEEP_PER_OPERATION, KEEP_TOTAL, Cancelled, Context, Failure, TERMINAL_STATES, cancel, describe, journal, pid_alive, prune_jobs,
                  read_events, request_path, status)

import build


def run_request(request_path, *, context_class=Context, operation_overrides=None):
    winproc.prepare_child_environment()
    path = Path(request_path)
    if not winproc.request_is_private(path):
        raise SystemExit("request must be owned by this user and private (0600)")
    request = json.loads(path.read_text())
    if request.get("schema") != 1:
        raise SystemExit("unsupported request schema")
    try:
        c = context_class(request)
    except Failure as error:
        # Validation precedes job registration: use the existing CLI rejection
        # shape, not a fabricated job result/event for a task that never started.
        print(json.dumps({"error": str(error), "code": getattr(error, "code", "job_failed")}, ensure_ascii=False))
        return 2
    except OSError as error:
        # The job cannot even register its record (full disk, read-only volume): say why and come back non-zero instead of a traceback (J10)
        print(f"[s2a] 【红】任务记录写不进磁盘，任务未开始：{error}", file=sys.stderr, flush=True)
        return 1
    signal.signal(signal.SIGTERM, lambda *_: c.stop())
    signal.signal(signal.SIGINT, lambda *_: c.stop())
    ended = threading.Event()
    def supervise():
        while not ended.wait(1):
            parent = request.get("parentPid")
            if parent and not pid_alive(parent):   # 启动器没了：停本任务（Windows 不能用 os.kill(pid, 0) 探活，见 core.pid_alive）
                c.stop()
            if not c.cancelled:
                c.emit("heartbeat")
    threading.Thread(target=supervise, daemon=True).start()
    operations = {"preflight": toolchain.preflight, "prepareEngine": toolchain.prepare, "downloadMetal": toolchain.download_metal, "downloadIosPlatform": toolchain.download_ios_platform,
                  "setup": library.setup, "keys": library.keys, "import": library.import_games,
                  "catalog": library.catalog, "customize": library.customize, "remove": library.remove, "delete": library.delete, "clean": library.clean,
                  "spaceSurvey": library.space_survey, "spaceFree": library.space_free, "storageOverview": sizes.overview,
                  "diagnostics": library.diagnostics, "saveInput": library.save_input,
                  "locations": library.list_locations, "moveGame": library.move_game, "relinkGame": library.relink_game, "forgetGame": library.forget_game,
                  "build": build.prepare, "launch": build.launch, "pullDiagnostics": diag.pull, "ipa": ipa.build, "install": install.install, "installRemove": install.remove_from_phone, "installRestore": install.restore_previous, "installCheck": install.readiness, "signingProject": install.signing_project, "installDotnet": toolchain.install_dotnet, "targetDevice": target.probe,
                  "saveStatus": saves.status, "saveBackup": saves.backup, "saveInspect": saves.inspect, "saveRestore": saves.restore,
                  "community": community.listing, "communitySave": community.save}
    if operation_overrides:
        operations.update(operation_overrides)
    try:
        if c.operation not in operations:
            raise Failure("不支持的任务类型。")
        if c.operation == "import" and c.r.get("partialResults") is True:
            c.import_result = library.initial_import_result(c)
        if c.operation not in saves.OPERATIONS:
            embedded.install(c)
        if c.operation not in NO_LIBRARY:
            layout.ensure(c)   # 旧布局的库先一次性整理成新结构（layout.py），之后各作业只按新布局找路径
        result = operations[c.operation](c)
        # A completed save-directory swap is the commit point. A later cancel
        # signal cannot turn a committed restore into a misleading cancelled job.
        if not (c.operation == "saveRestore" and result.get("committed")):
            c.check()
        ended.set()
        c.finish("completed", "操作完成", result)
        # The operation is done, but a job whose record never reached the disk must not come back as success (J10)
        return 0 if not getattr(c, "journal_broken", False) else 1
    except (Cancelled, KeyboardInterrupt):
        ended.set()
        # 一局被停是玩家自己关游戏，不是「任务取消」（11-07 新前端验收）
        # 下载 Xcode 组件的传输由系统资产服务持有：停掉本进程只是不再等，传输可能还在继续（实测 11-08：取消后 Xcode 仍在下载）
        waiting = c.operation in ("downloadIosPlatform", "downloadMetal")
        if c.operation in CLEANS_AFTER_STOP:
            library.clean_after_stop(c)
        c.finish("cancelled", "游戏已停止。" if c.operation == "launch" else
                 "已停止等待：Xcode 的下载可能仍在继续（可在 Xcode → 设置 → Components 里查看）。" if waiting else
                 "任务已取消；上次成功的运行版本和存档已保留。", library.import_failure_result(c))
        return 130
    except Exception as error:
        ended.set()
        if not isinstance(error, Failure) or c.last_log is None:
            log = c.logs / (c.task + "-exception.log")
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(c.redact(traceback.format_exc()))
            os.chmod(log, 0o600)
            c.last_log = log
        diagnostic = c.report_failure(error)
        message = diagnostic["summary"]
        if c.operation in CLEANS_AFTER_STOP:   # 错误摘要要读产物根里的日志，所以排在它后面
            library.clean_after_stop(c)
        c.finish("failed", message, library.import_failure_result(c))
        return 1
    finally:
        ended.set()
        _drop_job_tmp(c)


def _drop_job_tmp(c):
    """作业临时目录（Context.env 设的 S2A_JOB_TMP）随作业结束删掉；删不掉留给下次清 temp/"""
    gid = c.r.get("gameId")
    if gid and c.r.get("library"):
        try:
            import shutil
            shutil.rmtree(layout.job_tmp(c, gid, c.task), ignore_errors=True)
        except Exception:   # noqa: BLE001 —— 收尾不影响作业结果
            pass


CLEANS_AFTER_STOP = ("build", "ipa")
NO_LIBRARY = ("preflight", "prepareEngine", "downloadMetal", "downloadIosPlatform", "installDotnet", "targetDevice", "setup", "signingProject")


def parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command")
    run = sub.add_parser("run", help="run one request")
    run.add_argument("--request", required=True)
    for name, help_text in (("get", "print the journal of one task"), ("cancel", "signal the worker this task recorded")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--private-root", required=True)
        p.add_argument("--task", required=True)
        if name == "cancel":
            p.add_argument("--signal", default="TERM", choices=("TERM", "KILL"))
    report_parser = sub.add_parser("report", help="print the error summary of one failed task, or of the newest failed task")
    report_parser.add_argument("--private-root", required=True)
    report_parser.add_argument("--task")
    report_parser.add_argument("--game")
    report_parser.add_argument("--operations", help="comma separated")
    resume = sub.add_parser("resume", help="run the same task again from its journal (a killed attempt's stages are skipped by their own keys)")
    resume.add_argument("--private-root", required=True)
    resume.add_argument("--task", required=True)
    resume.add_argument("--force", action="store_true", help="re-run even when the task already has a terminal state")
    resume.add_argument("--dry-run", action="store_true", help="print what a resume would do, without running it")
    status_parser = sub.add_parser("status", help="counts and size of the whole job store")
    status_parser.add_argument("--private-root", required=True)
    pruner = sub.add_parser("prune", help="drop old finished job records (core.prune_jobs: total and age caps, per-operation floors; running ones are kept)")
    pruner.add_argument("--private-root", required=True)
    pruner.add_argument("--keep-days", type=float, default=KEEP_DAYS)
    pruner.add_argument("--keep-count", type=int, default=KEEP_TOTAL)
    pruner.add_argument("--per-operation", type=int, default=KEEP_PER_OPERATION, help="newest finished records kept per operation regardless of the caps")
    pruner.add_argument("--failed-per-operation", type=int, default=KEEP_FAILED_PER_OPERATION, help="newest failed / cancelled records kept per operation")
    pruner.add_argument("--dry-run", action="store_true")
    events = sub.add_parser("events", help="print the NDJSON events of one task")
    events.add_argument("--private-root", required=True)
    events.add_argument("--task", required=True)
    events.add_argument("--since", type=int, default=0, help="skip the first N events")
    events.add_argument("--limit", type=int, default=0, help="print at most N events (0 = all)")
    return ap


def cli(argv, *, context_class=Context, operation_overrides=None):
    if not argv or argv[0].startswith("-"):
        argv = ["run"] + list(argv)   # the Mac launcher passes only --request; keep that call working
    args = parser().parse_args(argv)
    if args.command == "run":
        return run_request(args.request, context_class=context_class, operation_overrides=operation_overrides)
    if not Path(args.private_root).is_dir():
        print(json.dumps({"error": "私有目录不存在。"}, ensure_ascii=False))
        return 2
    try:
        if args.command == "get":
            print(json.dumps(describe(args.private_root, args.task), ensure_ascii=False))
            return 0
        if args.command == "events":
            for event in read_events(args.private_root, args.task, since=max(0, args.since), limit=(args.limit or None)):
                print(json.dumps(event, ensure_ascii=False))
            return 0
        if args.command == "status":
            print(json.dumps(status(args.private_root), ensure_ascii=False))
            return 0
        if args.command == "prune":
            print(json.dumps(prune_jobs(args.private_root, args.keep_days, args.keep_count, args.dry_run, per_operation=args.per_operation,
                                        failed_per_operation=args.failed_per_operation), ensure_ascii=False))
            return 0
        if args.command == "resume":
            record = journal(args.private_root, args.task)
            if args.dry_run:
                # Report only: what the next attempt would be, and what the journal already knows. Whether a stage is actually skipped is each
                # stage's own product key (.stage_<阶段>), which the job layer does not predict (J10).
                terminal = record.get("state") in TERMINAL_STATES or bool(record.get("ended"))
                print(json.dumps(dict(dryRun=True, wouldResume=bool(args.force) or not terminal, operation=record.get("operation"),
                                      attemptNo=(record.get("attemptNo") or 1) + (0 if terminal and not args.force else 1),
                                      stage=record.get("stage") or "", attempts=record.get("attempts") or {},
                                      note="阶段是否跳过只由各阶段自己的产物键（.stage_<阶段>）决定，job 层不记也不预判"), ensure_ascii=False))
                return 0
            if record.get("state") in TERMINAL_STATES and not args.force:
                # Re-running a finished task on purpose is allowed, but a caller that thinks it is resuming must not silently restart it
                print(json.dumps(dict(resumed=False, reason="任务已有终态，需要 --force 才会重跑。", state=record.get("state"),
                                      attempts=record.get("attempts") or {}), ensure_ascii=False))
                return 2
            request = request_path(args.private_root, args.task)
            if not request.is_file():
                print(json.dumps(dict(resumed=False, reason="这条任务没有留下请求记录，无法续跑。", code="job_no_request"), ensure_ascii=False))
                return 2
            # 记一行：这次续跑会重跑整条操作，已完成阶段由各阶段自己的产物键跳过（job 层不记也不预判）
            print(json.dumps(dict(resumed=True, attemptNo=(record.get("attemptNo") or 1) + 1,
                                  stage=record.get("stage") or "", attempts=record.get("attempts") or {}), ensure_ascii=False),
                  file=sys.stderr, flush=True)
            return run_request(request, context_class=context_class, operation_overrides=operation_overrides)
        if args.command == "report":
            import report
            return report.main(args.private_root, args.task, args.game, args.operations.split(",") if args.operations else None)
        if args.command == "cancel":
            result = cancel(args.private_root, args.task, args.signal)
            print(json.dumps(result, ensure_ascii=False))
            return 0 if result.get("cancelled") else 2
    except Failure as error:
        print(json.dumps({"error": str(error), "code": getattr(error, "code", "job_failed")}, ensure_ascii=False))
        return 2
    return 2


def main(*, context_class=Context, operation_overrides=None):
    """The launcher's original call shape (`--request` in argv); tests and packaged workers may inject context / operations."""
    return cli(sys.argv[1:], context_class=context_class, operation_overrides=operation_overrides)


if __name__ == "__main__":
    sys.exit(cli(sys.argv[1:]))
