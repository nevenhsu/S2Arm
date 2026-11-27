import Foundation

/// 作业请求里每次都要带的那部分（旧 Store.worker 的固定字段）。数据目录、游戏库、引擎位置都在这里定，界面不拼路径。
public struct JobEnvironment: Equatable {
    public var privateRoot: URL
    /// 应用资源目录：.app 里是 Contents/Resources（带 Runtime/python、Jobs、Engine）；源码树里跑时是 launchers/macos。
    public var resources: URL
    public var engineDir: String?
    public var library: String
    public var libraryId: String
    public var developerDir: String
    public var fontsDir: String?
    public var devHud: Bool
    public var metalHud: Bool
    public var buildId: String
    /// 设置里指定的 Python（只在没有内置 Python、也没有内置引擎时用，同旧启动器）。
    public var pythonPath: String?
    /// 旧启动器导出页记住的平台勾选（preferences.exportTargets）：新前端还没存过自己的勾选时接着用它。
    public var exportTargets: [String]?
    /// 启动器界面语言（zh-Hans / en）：每个作业请求带上 uiLanguage，作业层据此出玩家看得到的文件名 / 说明（自签导出的文件夹与安装说明等）；前端随界面语言写
    public var uiLanguage: String?

    public init(privateRoot: URL, resources: URL, engineDir: String? = nil, library: String = "", libraryId: String = "",
                developerDir: String = "", fontsDir: String? = nil, devHud: Bool = false, metalHud: Bool = false,
                buildId: String = "", pythonPath: String? = nil) {
        self.privateRoot = privateRoot; self.resources = resources; self.engineDir = engineDir; self.library = library
        self.libraryId = libraryId; self.developerDir = developerDir; self.fontsDir = fontsDir
        self.devHud = devHud; self.metalHud = metalHud; self.buildId = buildId; self.pythonPath = pythonPath
    }

    /// 随应用带的引擎（Resources/Engine/engine.json 在才算）。
    public var engineBundle: URL? {
        let path = resources.appendingPathComponent("Engine")
        return FileManager.default.fileExists(atPath: path.appendingPathComponent("engine.json").path) ? path : nil
    }

    /// 读 S2Arm 数据目录里的 preferences.json（旧启动器写的那份，Mac 只有这一套权威库）。本适配层只回写游戏库两项（saveLibrary，换游戏库时），
    /// 其余字段只读，避免两个前端各写各的。
    public static func load(privateRoot: URL, resources: URL, buildId: String = "") -> JobEnvironment {
        var env = JobEnvironment(privateRoot: privateRoot, resources: resources, buildId: buildId)
        guard let data = try? Data(contentsOf: privateRoot.appendingPathComponent("preferences.json")),
              let p = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return env }
        env.library = S2ArmPaths.library(privateRoot: privateRoot, saved: p["library"] as? String ?? "")   // 旧默认库已搬到 ~/S2Arm 时用新位置
        env.libraryId = p["libraryId"] as? String ?? ""
        env.developerDir = p["developerDir"] as? String ?? ""
        env.engineDir = p["engineDir"] as? String
        env.fontsDir = p["fontsDir"] as? String
        env.devHud = p["devHud"] as? Bool ?? false
        env.metalHud = p["metalHud"] as? Bool ?? false
        env.pythonPath = p["pythonPath"] as? String
        env.exportTargets = p["exportTargets"] as? [String]
        return env
    }

    /// 换游戏库后把 library / libraryId 写回 preferences.json（同旧启动器 setupLibrary → savePreferences）：只改这两项，别的字段原样保留；
    /// 原子写、0600。旧启动器与本前端读同一份，换过去两边一致。
    public static func saveLibrary(privateRoot: URL, library: String, libraryId: String) throws {
        let file = privateRoot.appendingPathComponent("preferences.json")
        var p: [String: Any] = [:]
        if let data = try? Data(contentsOf: file) {
            guard let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
                throw JobClientError("设置文件 preferences.json 读不懂，没有改动它。")
            }
            p = object
        }
        p["library"] = library
        p["libraryId"] = libraryId
        do {
            try FileManager.default.createDirectory(at: privateRoot, withIntermediateDirectories: true)
            try JSONSerialization.data(withJSONObject: p, options: [.sortedKeys]).write(to: file, options: .atomic)
        } catch {
            throw JobClientError("游戏库已建好，但设置没能写进磁盘（\(error.localizedDescription)）：请检查磁盘空间和权限后再换一次。")
        }
    }
}

public struct JobClientError: Error, Equatable, LocalizedError {
    public let message: String
    /// 界面据此给出动作（bundle_incomplete：App 包不完整 → 「下载完整安装包」按钮）；没有动作的错误为 nil
    public let code: String?
    public init(_ message: String, code: String? = nil) { self.message = message; self.code = code }
    public var errorDescription: String? { message }
    /// App 包里缺了随包的东西（内置 Python 等）：多半是旧版应用内更新回滚把正在跑的包换掉了一半，只有重装完整包能好
    public static let bundleIncomplete = "bundle_incomplete"
}

/// 作业层的一行事件（pack/jobs/core.py emit）。schema 不是 1 的行不认。
public struct JobEvent {
    public let fields: [String: Any]

    /// 同旧 Store.event：taskId 缺了取 jobId；state succeeded 统一叫 completed。
    public init?(json: [String: Any]) {
        var e = json
        if e["taskId"] == nil { e["taskId"] = e["jobId"] }
        if e["state"] as? String == "succeeded" { e["state"] = "completed" }
        guard e["schema"] as? Int == 1, e["taskId"] is String else { return nil }
        fields = e
    }

    public var taskId: String { fields["taskId"] as? String ?? "" }
    public var type: String { fields["type"] as? String ?? "" }
    public var state: String? { fields["state"] as? String }
    public var operation: String? { fields["operation"] as? String }
    public var stage: String? { fields["stage"] as? String }
    public var message: String { fields["message"] as? String ?? "" }
    public var gameId: String? { fields["gameId"] as? String }
    public var done: Double? { (fields["done"] as? NSNumber)?.doubleValue }
    public var total: Double? { (fields["total"] as? NSNumber)?.doubleValue }
    public var result: [String: Any] { fields["result"] as? [String: Any] ?? [:] }
    public var isFinal: Bool { type == "result" }
}

/// 一次作业的结局。
public struct JobOutcome {
    /// paused：出包编排停在要玩家处理的点上（磁盘满 / 启动器退出，剃刀 H13 pack/jobs/export_plan.py），不算完成也不算失败
    public enum State: String { case completed, failed, cancelled, interrupted, paused }
    public let taskId: String
    public let state: State
    public let exitCode: Int32
    public let message: String
    public let result: [String: Any]
    public let diagnostics: [[String: Any]]
    /// 作业进程的标准错误（中断时给诊断用）。
    public let stderr: String

    /// 让这次作业停下的那条诊断：作业层把终态错误追加在诊断列表最后（pack/jobs/core.py report_failure），
    ///   前面可能有中途记的警告（磁盘快满、补着色器没补上……）。取最后一条 error 级的，没有就取最后一条。
    ///   原先取第一条：更新 iPhone 时先记了一条补着色器失败的警告，停装码 save_backup_failed 排在后面，结局页认不出、不出「仍然继续」。
    public var failureDiagnostic: [String: Any]? { Self.failureDiagnostic(diagnostics) }
    public var failureCode: String? { failureDiagnostic?["code"] as? String }
    public static func failureDiagnostic(_ diagnostics: [[String: Any]]) -> [String: Any]? {
        diagnostics.last(where: { $0["severity"] as? String == "error" }) ?? diagnostics.last
    }

    public static let interruptedMessage = "工作进程已退出。可重试并验证完成阶段。"
}

/// 一次在跑的作业。
public final class JobHandle {
    public let id: String
    public let operation: String
    private let process: JobProcessHandle
    public private(set) var cancelling = false

    init(id: String, operation: String, process: JobProcessHandle) {
        self.id = id; self.operation = operation; self.process = process
    }

    /// 请作业停：SIGTERM，作业层记取消并以 130 退出（同旧 Store.cancel）。
    public func cancel() {
        cancelling = true
        process.terminate()
    }
}

/// S2Arm 作业层的客户端：写请求（schema 1、0600）、用隔离的 Python 起 `Jobs/main.py run --request`、按行读 NDJSON、取消、收结局。
/// 从旧 launchers/macos/Sources/S2ArmLauncher/Store.swift（worker / pythonExecutable / jobEngineURL / cancel）迁出的等价逻辑，不带界面状态。
public final class JobClient {
    public var environment: JobEnvironment
    private let runner: JobProcessRunner
    private let callbackQueue: DispatchQueue
    private let fileManager: FileManager
    private let pid: Int32
    private let baseEnvironment: [String: String]

    public init(environment: JobEnvironment, runner: JobProcessRunner = SystemJobProcessRunner(),
                callbackQueue: DispatchQueue = .main, fileManager: FileManager = .default,
                parentPid: Int32 = ProcessInfo.processInfo.processIdentifier,
                processEnvironment: [String: String] = ProcessInfo.processInfo.environment) {
        self.environment = environment; self.runner = runner; self.callbackQueue = callbackQueue
        self.fileManager = fileManager; self.pid = parentPid; self.baseEnvironment = processEnvironment
    }

    /// 起作业用的 Python：先认应用内置的 Runtime/python；有内置引擎却缺内置 Python = 安装不完整，直接报错；源码树里才退到设置 / 系统 Python。
    public func pythonExecutable() throws -> URL {
        let bundled = environment.resources.appendingPathComponent("Runtime/python/bin/python3")
        if fileManager.isExecutableFile(atPath: bundled.path) { return bundled }
        if environment.engineBundle != nil { throw JobClientError("应用内置 Python 缺失，请重新安装完整应用。", code: JobClientError.bundleIncomplete) }
        if let selected = environment.pythonPath, !selected.isEmpty {
            guard fileManager.isExecutableFile(atPath: selected) else { throw JobClientError("设置中的 Python 不可执行，请重新选择。") }
            return URL(fileURLWithPath: selected)
        }
        for path in ["/opt/homebrew/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"] where fileManager.isExecutableFile(atPath: path) {
            return URL(fileURLWithPath: path)
        }
        throw JobClientError("找不到 Python，请在设置中指定 Python 3.12 或更新版本。")
    }

    /// 作业入口：应用里是 Resources/Jobs/main.py，源码树里是 <engineDir>/pack/jobs/main.py。
    public func jobEntry() throws -> URL {
        let bundled = environment.resources.appendingPathComponent("Jobs/main.py")
        if fileManager.fileExists(atPath: bundled.path) { return bundled }
        // 玩家装的 App（带引擎包）缺作业入口 = 包不完整，照实说重装；源码树（开发）才说选源码目录
        if environment.engineBundle != nil { throw JobClientError("应用不完整，请重新安装 S2Arm。", code: JobClientError.bundleIncomplete) }
        let checkout = URL(fileURLWithPath: environment.engineDir ?? "").appendingPathComponent("pack/jobs/main.py")
        guard fileManager.fileExists(atPath: checkout.path) else { throw JobClientError("请选择包含 pack/jobs 的 S2Arm 源码目录。") }
        return checkout
    }

    /// 请求体（schema 1）：字段与旧 Store.worker 一一对应；extra 覆盖同名字段。不带 gameLanguage / docked：
    /// 逐款的游戏语言 / 主机模式在启动设置里选（作业层 community 存这款的 launch.json、起局 launch_env 转成宿主变量；11-14 维护者确认玩家可选，推翻 11-07 的「不给玩家选」）；没选过 = 跟 Mac 系统首选语言、主机模式。
    public func request(id: String, operation: String, extra: [String: Any] = [:]) -> [String: Any] {
        let env = environment
        var req: [String: Any] = [
            "schema": 1, "taskId": id, "operation": operation,
            "privateRoot": env.privateRoot.path, "resources": env.resources.path, "engineDir": env.engineDir ?? "",
            "library": env.library.isEmpty ? env.privateRoot.appendingPathComponent("unconfigured").path : env.library,
            "libraryId": env.libraryId, "developerDir": env.developerDir, "fontsDir": env.fontsDir ?? "",
            "devHud": env.devHud, "metalHud": env.metalHud, "buildId": env.buildId,
            "parentPid": Int(pid),
        ]
        // 界面语言照原样带、由作业层认（install.py：中文界面用中文定名，其余英文；build.py：六门之外不设）。原先只带 zh-Hans / en，
        //   法 / 日 / 西 / 繁中界面整个不带，作业层回落成中文：自签导出文件夹与说明成了中文（#502，11-16 维护者）
        if let lang = env.uiLanguage, !lang.isEmpty { req["uiLanguage"] = lang }   // extra 带了（起局）照 extra
        extra.forEach { req[$0] = $1 }
        if let bundle = env.engineBundle { req["engineBundle"] = bundle.path; req["engineDir"] = bundle.path }
        // 环境准备不依赖游戏库：旧游戏库所在盘没插着时也要能跑
        if ["preflight", "prepareEngine"].contains(operation) {
            req["libraryId"] = ""
            req["storageLibraryId"] = env.libraryId
        }
        return req
    }

    /// 子进程环境：去掉 PYTHONPATH / PYTHONHOME，不写字节码、不读用户 site（同旧启动器；配合 -B -I）。
    public func processEnvironment() -> [String: String] {
        var env = baseEnvironment
        env["PYTHONUNBUFFERED"] = "1"; env["PYTHONDONTWRITEBYTECODE"] = "1"; env["PYTHONNOUSERSITE"] = "1"
        env.removeValue(forKey: "PYTHONPATH"); env.removeValue(forKey: "PYTHONHOME")
        return env
    }


    /// 跑 updater.py 的一条命令，逐行交 JSON；结束时返回最后一行。
    private func runUpdater(_ command: [String], onEvent: @escaping @Sendable ([String: Any]) -> Void = { _ in }) async -> [String: Any]? {
        await runJobsScript("updater.py", command, onEvent: onEvent)
    }

    /// 批量出包的批次（pack/jobs/batch.py，Mac / Windows 共用）：show / create / mark / recover / cancel / resume / retry / clear，
    ///   返回 {batch, next, summary}（没有批次 → 空字典）；起不来返回 nil
    public func batch(_ command: String, _ arguments: [String] = []) async -> [String: Any]? {
        await runJobsScript("batch.py", [command, "--private-root", environment.privateRoot.path] + arguments)
    }

    /// 跑 pack/jobs 下的一个小脚本（updater.py / batch.py），逐行交 JSON；结束时返回最后一行
    private func runJobsScript(_ script: String, _ command: [String], onEvent: @escaping @Sendable ([String: Any]) -> Void = { _ in }) async -> [String: Any]? {
        guard let python = try? pythonExecutable(), let entry = try? jobEntry().deletingLastPathComponent().appendingPathComponent(script) else { return nil }
        final class Last: @unchecked Sendable { let lock = NSLock(); var value: [String: Any]? }
        let last = Last()
        return await withCheckedContinuation { continuation in
            do {
                _ = try runner.start(executable: python, arguments: ["-B", "-I", entry.path] + command, environment: processEnvironment(),
                                     onLine: { line in
                                         guard let object = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any] else { return }
                                         last.lock.lock(); last.value = object; last.lock.unlock()
                                         onEvent(object)
                                     }, onStderr: { _ in },
                                     onExit: { _ in last.lock.lock(); let value = last.value; last.lock.unlock(); continuation.resume(returning: value) })
            } catch { continuation.resume(returning: nil) }
        }
    }

    /// 打开时要给玩家看什么（pack/jobs/updater.py inbox，两端共用「永远只给最新一期」）：
    ///   {popup?: {kind: update|afterUpdate, version, notes?}, banner?: 公告, messages: [...], unread, update: 查版本结果}；起不来返回 nil，不打扰玩家
    public func inbox(current: String, noticesOnly: Bool = false) async -> [String: Any]? {
        var command = ["inbox", "--private-root", environment.privateRoot.path, "--platform", "mac", "--current", current]
        if noticesOnly { command.append("--notices-only") }
        let result = await runUpdater(command)
        return result?["messages"] is [Any] ? result : nil
    }

    public func check(current: String, fresh: Bool) async -> [String: Any]? {
        var command = ["check", "--platform", "mac", "--current", current]
        if fresh { command.append("--fresh") }
        return await runUpdater(command)
    }

    /// 玩家关掉了：update = 「有新版」那版（以后再说），after = 「这版更新了什么」那版，notice = 看过的最新一条公告号，feature = 看过的新功能大窗期号
    public func inboxSeen(update: String? = nil, after: String? = nil, notice: String? = nil, feature: String? = nil) async {
        var command = ["inbox-seen", "--private-root", environment.privateRoot.path]
        if let update { command += ["--update", update] }
        if let after { command += ["--after", after] }
        if let notice { command += ["--notice", notice] }
        if let feature { command += ["--feature", feature] }
        _ = await runUpdater(command)
    }

    /// 下载、校验、解开新版（不碰正在用的安装目录）。最后一行 state = ready 或 failed。
    /// 最近一次失败作业的错误摘要（作业层 report.py，Mac / Windows 共用）。没有作业记录时返回 nil，由界面退回基本信息。
    public func errorReport(gameID: String?, operations: [String], extra: [String: Any]) async -> String? {
        guard let python = try? pythonExecutable(), let entry = try? jobEntry() else { return nil }
        var arguments = ["-B", "-I", entry.path, "report", "--private-root", environment.privateRoot.path]
        if let gameID { arguments += ["--game", gameID] }
        if !operations.isEmpty { arguments += ["--operations", operations.joined(separator: ",")] }
        var env = processEnvironment()
        if let data = try? JSONSerialization.data(withJSONObject: extra) { env["S2A_REPORT_EXTRA"] = String(decoding: data, as: UTF8.self) }
        final class Output: @unchecked Sendable {
            private let lock = NSLock(); private var lines: [String] = []
            func add(_ line: String) { lock.lock(); lines.append(line); lock.unlock() }
            var text: String { lock.lock(); defer { lock.unlock() }; return lines.joined(separator: "\n") }
        }
        let output = Output()
        return await withCheckedContinuation { continuation in
            do {
                _ = try runner.start(executable: python, arguments: arguments, environment: env,
                                     onLine: { output.add(String(decoding: $0, as: UTF8.self)) }, onStderr: { _ in },
                                     onExit: { continuation.resume(returning: $0 == 0 ? output.text : nil) })
            } catch { continuation.resume(returning: nil) }
        }
    }

    /// 交一个作业。onEvent / onFinish 都在 callbackQueue 上、按作业输出顺序到达；onFinish 只来一次、在最后。
    /// 起不来（缺 Python / 入口 / 写不进请求）直接抛错，不回调。
    @discardableResult
    /// secrets：只给这一个作业进程的环境变量（开发者证书密码，pack/jobs/devcert.py ⑤）；不进请求文件，作业层一起来就从环境摘掉
    public func start(operation: String, extra: [String: Any] = [:], id: String = UUID().uuidString.lowercased(), secrets: [String: String] = [:],
                      onEvent: @escaping (JobEvent) -> Void = { _ in },
                      onFinish: @escaping (JobOutcome) -> Void = { _ in }) throws -> JobHandle {
        let python = try pythonExecutable()
        let entry = try jobEntry()
        let dir = environment.privateRoot.appendingPathComponent("requests")
        let file = dir.appendingPathComponent(id + ".json")
        do {
            // 系统缺省权限（~/Library 本来就只归当前用户；11-16 维护者：不再自设权限）
            try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
            let data = try JSONSerialization.data(withJSONObject: request(id: id, operation: operation, extra: extra))
            try data.write(to: file, options: .atomic)
        } catch {
            throw JobClientError("无法写入任务请求：\(error.localizedDescription)")
        }
        let collector = Collector(id: id)
        let queue = callbackQueue
        let fm = fileManager
        let process: JobProcessHandle
        do {
            process = try runner.start(
                executable: python, arguments: ["-B", "-I", entry.path, "run", "--request", file.path],
                environment: processEnvironment().merging(secrets) { _, s in s },
                automatic: JobTrigger.automaticOperations.contains(operation),
                onLine: { line in
                    guard let object = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any] else { return }
                    guard let event = JobEvent(json: object) else {
                        // 校验阶段拒收（main.py 打印 {"error", "code"} 后退出 2）：不是事件，留作结局说明
                        if let message = object["error"] as? String { collector.rejection = message }
                        return
                    }
                    guard event.taskId == id else { return }
                    if event.isFinal { collector.final = event }
                    queue.async { onEvent(event) }
                },
                onStderr: { chunk in collector.stderr.append(chunk) },
                onExit: { status in
                    try? fm.removeItem(at: file)
                    let outcome = collector.outcome(exitCode: status)
                    queue.async { onFinish(outcome) }
                })
        } catch {
            try? fileManager.removeItem(at: file)
            throw JobClientError("无法启动本地组件：\(error.localizedDescription)")
        }
        return JobHandle(id: id, operation: operation, process: process)
    }

    /// 只在 runner 的串行队列上读写。
    private final class Collector {
        let id: String
        var final: JobEvent?
        var rejection: String?
        var stderr = Data()
        init(id: String) { self.id = id }

        func outcome(exitCode: Int32) -> JobOutcome {
            let errorText = String(decoding: stderr, as: UTF8.self)
            if let final, let state = JobOutcome.State(rawValue: final.state ?? "") {
                let diagnostics = final.fields["diagnostics"] as? [[String: Any]] ?? []
                return JobOutcome(taskId: id, state: state, exitCode: exitCode, message: final.message,
                                  result: final.result, diagnostics: diagnostics, stderr: errorText)
            }
            // 没有结果事件：130 = 作业层收到 SIGTERM 记的取消；其他 = 进程中途没了
            if exitCode == 130 {
                return JobOutcome(taskId: id, state: .cancelled, exitCode: exitCode, message: "任务已取消。",
                                  result: [:], diagnostics: [], stderr: errorText)
            }
            // 进程一启动就退出时没有日志可看：把错误输出的末尾带上，玩家和错误摘要才有线索
            let tail = errorText.trimmingCharacters(in: .whitespacesAndNewlines).suffix(600)
            let interrupted = tail.isEmpty ? JobOutcome.interruptedMessage : JobOutcome.interruptedMessage + "\n" + tail
            return JobOutcome(taskId: id, state: .interrupted, exitCode: exitCode, message: rejection ?? interrupted,
                              result: [:], diagnostics: [], stderr: errorText)
        }
    }
}
