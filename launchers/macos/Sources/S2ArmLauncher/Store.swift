// UI adapted from LumaDock (MIT); see ../../README.md.
import Foundation
import SwiftUI
import AppKit
import UniformTypeIdentifiers
import S2ArmAppleAdapter

final class WorkerOutput: @unchecked Sendable {
    private let lock = NSLock()
    private var buffer = Data()
    private var errors = Data()
    func append(_ chunk: Data) -> [Data] {
        lock.lock(); defer {lock.unlock()}; buffer.append(chunk)
        var lines: [Data] = []
        while let range = buffer.range(of:Data([10])) {lines.append(buffer.subdata(in:0..<range.lowerBound));buffer.removeSubrange(0..<range.upperBound)}
        return lines
    }
    func error(_ data:Data) {lock.lock();defer{lock.unlock()};if errors.count<32768{errors.append(data)}}
    var errorText:String {lock.lock();defer{lock.unlock()};return String(data:errors,encoding:.utf8) ?? ""}
}

@MainActor final class Store: ObservableObject {
    @Published var preferences = Preferences()
    @Published var games: [Game] = []
    @Published var downstreamPrompt: Game?   // 下游深度方案确认页（DownstreamView.swift）：导出前弹，选完记下再接着导出
    @Published var tasks: [TaskRecord] = []
    @Published var checks: [ToolCheck] = []
    @Published var platformChecks: [String:[ToolCheck]] = [:]
    var platformCheckRequests: [String:NSDictionary] = [:]
    @Published private(set) var checkingEnvironment = false
    @Published private(set) var catalogWorkers = 0
    @Published var environmentReportComplete = false
    private var environmentCallbacks: [()->Void] = []
    private var requestedStartupEnvironmentCheck = false
    private var lastWorkerEvent: [String:Date] = [:]
    // #377：一键出包的画面模式证据按「任务 + 状态 + 产物」缓一层，任务卡重绘时不反复解读 IPA
    private var packEvidenceCache: [String:PackGraphicsEvidence] = [:]
    private var documentationWindow:NSWindow?
    @Published var engineSupportsInput = false
    @Published var engineInputReason = ""   // 引擎能力探测的具体原因（F06：提示要准，不能只说「不支持」）
    @Published var profiles: [GameProfile] = []
    @Published var keyStatus = "尚未导入密钥"
    @Published var hasImportedKeys = false
    @Published var error: String? { didSet { if error == nil {errorDiagnostic=nil} } }
    /// 已出好的 iPhone 包（PackShelf.scan 读游戏库 iPhone/ 目录的产物）：任务页出现、出包 / 装机结束、删包后后台刷新
    @Published var packShelf: [PackShelfEntry] = []
    /// 存档回退失败后玩家单独确认「仍要启动」的那一款：下一次启动带 overrideRecovery（后端 pack/jobs/saves.py recover，同 iPhone 规则；审计 18:00 P0-3），用一次就清
    var recoveryOverride: String?
    @Published var errorDiagnostic: TaskDiagnostic?
    @Published var notice: String?
    // 常驻提示（10-26 维护者定）：notice 等于 stickyNotice 时不自动消失（出完 iPhone 包那条要带「查看装机步骤」），其余提示照旧 6 秒消失
    @Published var stickyNotice: String?
    @Published var showInstallGuide = false
    // 装机前清单（InstallGuideView）：查哪款、查回来的结果、玩家是否点过「推到 iPhone」（齐了自动开装）、在跑的那次查是什么时候起的
    @Published var installGuideGame: String?
    @Published var installReadiness: InstallReadiness?
    @Published var installWhenReady = false
    var installCheckStarted: Date?
    /// 选的游戏库位置在不支持的磁盘格式上：弹框说明并给改用默认位置 / 重选
    @Published var libraryFormatProblem: LibraryFormatProblem?
    /// 导出页同时勾了 iPhone / iPad 和 Mac 的游戏：iPhone 版结束后接着排 Mac 版
    var macAfterIPA: Set<String> = []
    var lastIPAPath: URL?
    @Published var selection: String? = "library"
    @Published var selectedGame: String?
    @Published var showOnboarding = false
    @Published var onboardingStep = 0
    private var addGamesAfterOnboarding = false
    private var quitAfterOnboardingDismissal: (@MainActor () -> Void)?
    @Published var editingInput = false
    @Published var inputGame: Game?
    @Published var inputGuide: InputGuidePresentation?
    var pendingGuideEditor: Game?
    @Published var saveManagerGame: Game?
    @Published var globalInput = InputConfig.standard
    @Published var queue: [String] = []
    let privateRoot: URL
    let resources: URL
    var processes: [String:Process] = [:]
    var callbacks: [String:([String:Any])->Void] = [:]
    @Published var executionTask: String?
    @Published var targetProbe = TargetProbe()
    // 正在等 macOS「想访问『下载』文件夹」这类授权框：读文件预检放到后台线程，主线程（界面 + AI 控制口）照常响应，控制口 state 报给 AI
    @Published var waitingForAccess: String?
    @Published private var pendingAccess: [([URL], Bool?)] = []   // 等授权期间又拖进来的（审计 P1-3：原先直接丢掉），上一批查完按序接着查
    // 等授权是持续状态（审计 18:00 P1-2）：不走 6 秒自动消失、带打勾的普通提示，由界面单独常驻显示；授权到达（允许或拒绝）才清
    var accessWaitNotice: String? {
        guard let waiting = waitingForAccess else { return nil }
        let name = URL(fileURLWithPath:waiting).lastPathComponent
        let wait = L("正在等系统允许读取「\(name)」：请在屏幕上的系统框里点「允许」。")
            + "\n" + L("S2Arm 只读游戏原件、不改不删；同一块盘或文件夹在这台 Mac 上只需允许一次，以后升级 S2Arm 也不用再点。")
        return pendingAccess.isEmpty ? wait : wait + "\n" + L("这期间放进来的 \(pendingAccess.count) 批会在它之后接着导入。")
    }
    var refreshTimer: Timer?
    var libraryURL: URL { URL(fileURLWithPath: preferences.library) }
    var engineBundle: URL? {let p=resources.appendingPathComponent("Engine");return FileManager.default.fileExists(atPath:p.appendingPathComponent("engine.json").path) ? p : nil}
    var executionBusy: Bool { executionTask != nil }
    // 玩家主动操作优先于后台自动准备（10-26 维护者定）：导入后自动排的 Mac 准备记在 autoQueue / autoBuildTask；玩家点「出 iPhone 包」时它们让位，
    //   在跑的停掉（任务写明已让位）、排队的撤下，出完 iPhone 包再自动排回去（Mac 准备按阶段键缓存，重排即从做完的阶段接着做）
    var autoQueue: Set<String> = []
    var autoBuildTask: String?
    var yieldedTasks: Set<String> = []
    var yieldedGames: [String] = []
    var pendingIPA: PendingIPhonePack?
    // 空间预检拦下后玩家点了「仍然继续」的 Mac 打包：排队只存游戏号，起任务时（startNext）带上 spaceOverride
    var spaceOverrideGames: Set<String> = []
    static let yieldMessage = "已让位给 iPhone 出包，出完会自动接着准备（做完的阶段不重做）。"
    static let dotnetInstallRecheckFailed = "已自动安装 .NET SDK，但复查仍未通过。请在「设置 → 环境检查」查看 .NET 一项的说明。"
    static let metalInstallRecheckFailed = "已下载 Metal 编译组件，但复查仍未通过。请在「设置 → 环境检查」查看 Metal 一项的说明。"
    static let iosPlatformInstallRecheckFailed = "已下载 iOS 平台组件，但复查仍未通过。请在「设置 → 环境检查」查看 iOS 平台一项的说明。"
    /// 一键安装后：环境检查里这一项过了才写已安装，没过写失败原因（同出 iPhone 包那条，审计 18:00 P2-36）
    func toolchainInstalled(_ action: String, _ done: String, _ failed: String) {
        if checks.first(where:{ $0.action == action })?.ok ?? true { notice = done } else { error = failed; selection = "settings" }
    }
    /// 装到 iPhone 成功句里资源包那半句：按这次实际推了几个文件写（审计 18:00 P1-58：原先写死「资源包也拷好了」）
    static func packPushedClause(_ files: Int?) -> String {
        guard let files else { return "" }
        return files > 0 ? L("，资源包推了") + " \(files) " + L("个文件") : L("，资源包已是最新（没有要推的文件）")
    }
    static let dotnetRecheckFailed = "已自动安装 .NET SDK，但复查仍未通过，iPhone 包没有开始出。请在「设置 → 环境检查」查看 .NET 一项的说明，处理后再点「打 iPhone 包」。"
    /// 出完 iPhone 包的提示：装机主路是否已验证与装机步骤页同一个开关（审计 P2-7）。一包即完整（维护者 10-27 否两段式：玩家装上就玩、不回连 Mac）
    static func ipaDoneNotice() -> String {
        let install = InstallGuideView.xcodeInstallVerified
            ? "用数据线连上 iPhone，在打包任务页点「推到 iPhone」就能装上（也可以按装机步骤改用 AltStore）。"
            : "可以用数据线连上 iPhone，在打包任务页点「推到 iPhone」（这条路还在真机验证，装不上请按装机步骤改用 AltStore）。"
        return "iPhone 包已出好。" + install
    }
    /// 出 iPhone 包要不要让位：执行位或队列只被自动 Mac 准备占着才让；有玩家自己点的任务在跑 / 排队就不让（照旧提示互斥）
    func ipaYieldPlan() -> (stop: String?, drop: [String])? {
        guard queue.allSatisfy({ autoQueue.contains($0) }) else { return nil }
        let stop: String? = executionTask != nil && executionTask == autoBuildTask ? executionTask : nil
        guard stop != nil || (executionTask == nil && !queue.isEmpty) else { return nil }
        return (stop, queue)
    }
    var refreshingLibrary: Bool {catalogWorkers>0}
    // 玩家看得见的任务（任务页、侧栏角标）：环境检查、读游戏库、存映射这类后台小动作不上卡片
    static let visibleOperations: Set<String> = ["install","build","import","keys","downloadMetal","downloadIosPlatform","launch","clean","remove","delete","diagnostics","prepareEngine","pullDiagnostics","ipa","installDotnet","platformSource","platformSetup","platformBuild","saveBackup","saveInspect","saveRestore"]
    var visibleTasks: [TaskRecord] { tasks.filter { Self.visibleOperations.contains($0.operation) } }
    var activeTasks: [TaskRecord] {tasks.filter{$0.active && $0.state != "queued" && $0.operation != "launch"}}
    var environmentReady: Bool { environmentReportComplete && !checkingEnvironment && !checks.isEmpty && checks.filter{$0.required(for:"prepare")}.allSatisfy(\.ok) }
    var runtimeReady: Bool { environmentReportComplete && !checkingEnvironment && !checks.isEmpty && checks.filter{$0.required(for:"launch")}.allSatisfy(\.ok) }
    var canManageToolchain: Bool { !executionBusy && queue.isEmpty && !checkingEnvironment }
    var importingKeys: Bool { tasks.contains { $0.operation == "keys" && $0.active } }
    var appearance: AppAppearance {
        get { AppAppearance(rawValue:preferences.appearance ?? "system") ?? .system }
        set { preferences.appearance = newValue.rawValue; applyAppearance(); savePreferences() }
    }
    func applyAppearance() {
        switch appearance {
        case .system: NSApp?.appearance = nil
        case .light: NSApp?.appearance = NSAppearance(named:.aqua)
        case .dark: NSApp?.appearance = NSAppearance(named:.darkAqua)
        }
    }
    var language: AppLanguage {
        get { AppLanguage(rawValue:preferences.language ?? "system") ?? .system }
        set {
            Localization.preference = newValue
            preferences.language = newValue.rawValue
            savePreferences()
        }
    }
    var selected: Game? { games.first { $0.id == selectedGame } }
    /// 游戏库 ↑ / ↓ 换选中（游戏库改成玻璃卡竖排后，系统列表的键盘导航由这里补上）
    func stepSelection(_ d:Int) {
        guard !games.isEmpty else { return }
        let i = games.firstIndex { $0.id == selectedGame } ?? (d > 0 ? -1 : games.count)
        selectedGame = games[max(0, min(games.count - 1, i + d))].id
    }
    // 源码树根（由本文件位置推出）只编进调试版：发布版不带出包机的源码绝对路径（10-23 beta.2 前），release 下为 nil
    static var sourceTree: URL? {
        #if DEBUG
        var u = URL(fileURLWithPath:#filePath); for _ in 0..<5 { u.deleteLastPathComponent() }; return u
        #else
        return nil
        #endif
    }
    /// 启动参数里的数据目录：认 `--data-root=<目录>`（一个整体，推荐）和 `--data-root <目录>` 两种写法。
    ///   拉起 App 要用整体写法：macOS 把启动参数按「-名字 值」成对读进系统偏好，`--control --data-root <目录>` 会让 <目录> 落单，
    ///   被当成「要打开的文件」交给 App，SwiftUI 就不建默认窗口——玩家看不到启动器、环境检查也不跑（11-02 beta.3 走查，坑记录）
    nonisolated static func dataRoot(from args: [String]) -> URL? {
        if let a = args.first(where: { $0.hasPrefix("--data-root=") }) { return URL(fileURLWithPath: String(a.dropFirst("--data-root=".count))) }
        if let i = args.firstIndex(of:"--data-root"), args.count > i+1 { return URL(fileURLWithPath:args[i+1]) }
        return nil
    }
    init(privateRoot overrideRoot: URL? = nil, resources overrideResources: URL? = nil) {
        let args = CommandLine.arguments
        if let overrideRoot { privateRoot = overrideRoot }
        else if let root = Store.dataRoot(from:args) { privateRoot = root }
        else { privateRoot = FileManager.default.urls(for:.applicationSupportDirectory,in:.userDomainMask)[0].appendingPathComponent("S2Arm") }
        if let overrideResources { resources = overrideResources }
        else if let r = Bundle.main.resourceURL, Bundle.main.bundleURL.pathExtension == "app" || FileManager.default.fileExists(atPath:r.appendingPathComponent("Jobs/main.py").path) { resources = r }
        else { resources = Store.sourceTree?.appendingPathComponent("launchers/macos") ?? Bundle.main.bundleURL }
        try? FileManager.default.createDirectory(at:privateRoot,withIntermediateDirectories:true)
        if let data = try? Data(contentsOf:privateRoot.appendingPathComponent("preferences.json")), let p = try? JSONDecoder().decode(Preferences.self,from:data) { preferences = p }
        // 旧默认库（私有目录里的「游戏库」）已被作业层同盘改名到 ~/S2Arm（layout.relocate_default）：设置跟着换，别当成库丢了
        let moved = S2ArmPaths.library(privateRoot:privateRoot, saved:preferences.library)
        if moved != preferences.library { preferences.library = moved; savePreferences() }
        forgetVanishedLibrary()
        if let data = try? Data(contentsOf:privateRoot.appendingPathComponent("default-input.json")), let p = try? JSONDecoder().decode(InputConfig.self,from:data) { globalInput = p }
        if let i = args.firstIndex(of:"--language"), args.count > i+1, let language = AppLanguage(rawValue:args[i+1]) {
            preferences.language = language.rawValue
        }
        Localization.preference = language
        if let i = args.firstIndex(of:"--appearance"), args.count > i+1, let appearance = AppAppearance(rawValue:args[i+1]) {
            preferences.appearance = appearance.rawValue
        }
        // 只在源码树里直接跑（swift run / 单测）时按源码位置找引擎；.app 里用自带 Engine
        if preferences.engineDir == nil, Bundle.main.bundleURL.pathExtension != "app", let candidate = Store.sourceTree,
           FileManager.default.fileExists(atPath:candidate.appendingPathComponent("runtime/include/s2a_native.h").path) { preferences.engineDir=candidate.path }
        applyAppearance()
        recoverTasks()
        showOnboarding = !preferences.onboardingComplete
        if !preferences.library.isEmpty { refresh() }
        // 本机控制口（只在 --control / S2ARM_CONTROL=1 时开，见 ControlServer.swift）；不挂在窗口出现上：无窗口启动时也要能连
        DispatchQueue.main.async { [weak self] in if let self { ControlServer.start(store:self) } }
    }
    func recoverTasks() {
        let paths = (try? FileManager.default.contentsOfDirectory(at:jobsRoot,includingPropertiesForKeys:nil)) ?? []
        for path in paths where path.pathExtension == "json" && !path.lastPathComponent.hasPrefix("._") {   // ._：exFAT 上 macOS 配的 AppleDouble 附属文件
            guard let data = try? Data(contentsOf:path), var obj = (try? JSONSerialization.jsonObject(with:data)) as? [String:Any], let state = obj["state"] as? String else { continue }
            // 作业进程还活着（上次启动器卡死 / 被关，作业是独立进程照跑）：接管，按作业记录接着显示进度，不改成「中断」（10-26 Spare-host 内存吃紧启动器失联）
            if ["running","cancelling"].contains(state), let pid = obj["pid"] as? Int, Self.jobAlive(pid, start:obj["pidStart"] as? String),
               let record = Self.decode(TaskRecord.self,obj) {
                tasks.append(record); adopt(record.id, path:path, pid:pid, start:obj["pidStart"] as? String)
                if Self.occupiesExecution(record.operation) { executionTask = record.id }   // 占住执行位，排队的下一款等它结束（startNext）
                continue
            }
            if ["running","cancelling","queued"].contains(state) {
                obj["state"] = "interrupted"; obj["message"] = "上次运行被中断，可验证已有阶段后重试。"
                if let d = try? JSONSerialization.data(withJSONObject:obj) { persist(d,to:path,"任务记录") }
            }
            if (obj["operation"] as? String)?.hasPrefix("platform") == true,
               let record = Self.decode(TaskRecord.self,obj) { tasks.append(record) }
        }
        tasks.sort { ($0.started ?? 0) > ($1.started ?? 0) }
    }
    /// 正常起时占执行位的作业（审计 P1-75）：接管上次还活着的这些作业时同样占位。原先只认出包，启动器重启后正在跑的游戏、
    ///   出 iPhone 包、装机、存档不占位，「构建与游戏运行互斥」挡不住，能再起一局 / 一次出包撞在一起
    nonisolated static func occupiesExecution(_ operation: String) -> Bool {
        ["build", "ipa", "install", "launch", "installDotnet", "downloadMetal", "downloadIosPlatform", "prepareEngine", "saveBackup", "saveInspect", "saveRestore", "saveStatus"].contains(operation)
            || operation.hasPrefix("platform")
    }
    // 作业进程活着没有：kill(pid, 0) 通、不是僵尸、起始时刻对得上（进程号被复用 = 已没了），同 s2armctl.py job_alive / core.py pid_start
    nonisolated static func jobAlive(_ pid: Int, start: String?) -> Bool {
        guard pid > 0, kill(pid_t(pid), 0) == 0 || errno == EPERM else { return false }
        let p = Process(); p.executableURL = URL(fileURLWithPath:"/bin/ps"); p.arguments = ["-o","stat=","-o","lstart=","-p",String(pid)]
        let out = Pipe(); p.standardOutput = out; p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }; p.waitUntilExit()
        let line = String(data:out.fileHandleForReading.readDataToEndOfFile(),encoding:.utf8)?.trimmingCharacters(in:.whitespacesAndNewlines) ?? ""
        guard let sp = line.firstIndex(of:" "), !line.hasPrefix("Z") else { return false }
        return start == nil || line[line.index(after:sp)...].trimmingCharacters(in:.whitespaces) == start
    }
    // 接管的作业：每秒读它的作业记录刷新任务行；到终态停、刷新游戏库；进程没了还停在 running = 中断
    func adopt(_ id: String, path: URL, pid: Int, start: String?) {
        Timer.scheduledTimer(withTimeInterval:1, repeats:true) { [weak self] timer in
            MainActor.assumeIsolated {
                guard let self, let i = self.tasks.firstIndex(where:{$0.id == id}) else { timer.invalidate(); return }
                if let data = try? Data(contentsOf:path), let obj = (try? JSONSerialization.jsonObject(with:data)) as? [String:Any], var rec = Self.decode(TaskRecord.self,obj) {
                    if rec.active && !Self.jobAlive(pid, start:start) {
                        rec.state = "interrupted"; rec.message = "上次运行被中断，可验证已有阶段后重试。"; self.persistTask(rec)
                    }
                    self.tasks[i] = rec
                    if !rec.active {
                        timer.invalidate(); self.refresh()
                        if self.executionTask == id { self.executionTask = nil; self.startNext() }
                    }
                }
            }
        }
    }
    func savePreferences() { if let d = try? JSONEncoder().encode(preferences) { persist(d,to:privateRoot.appendingPathComponent("preferences.json"),"设置") } }
    // 启动器自己的数据写盘（审计 P2-72：原先 try? 吞掉写失败，磁盘满 / 没权限时内存里已改、下次打开又回到旧值，玩家不知道）：写不进就在界面报一句
    @discardableResult func persist(_ data: Data, to url: URL, _ what: String, privateFile: Bool = false) -> Bool {
        do {
            try data.write(to:url,options:.atomic)
            return true
        } catch {
            self.error="\(what)没能写进磁盘（\(error.localizedDescription)）。这次运行里仍按新的来，下次打开会回到旧值；请检查磁盘空间和权限。"
            return false
        }
    }
    func chooseLibrary() {
        let panel = NSOpenPanel(); panel.title = L("选择游戏库位置：放导出时的工作数据和存档，游戏原件不用挪（首次请选择空文件夹）"); panel.canChooseFiles = false; panel.canChooseDirectories = true; panel.canCreateDirectories = true; panel.prompt = L("选择")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        // 游戏库（导出时的工作数据）要放 APFS 或 exFAT；游戏原件放哪都行、原地只读。FAT32 等仍说明后改用默认位置（后端 setup 同一规则兜底）
        let format = Store.volumeFormat(url)
        let ok = format.isEmpty || format == "apfs" || format == "exfat"
        if !ok { libraryFormatProblem = LibraryFormatProblem(path:url.path, format:format); return }
        setupLibrary(url)
    }
    /// 改用这台 Mac 上的默认位置（~/S2Arm，在内置盘上）；用户指定过 S2Arm 数据目录时是数据目录下的「游戏库」，不一定在内置盘
    func useDefaultLibrary() {
        try? FileManager.default.createDirectory(at:defaultLibraryURL, withIntermediateDirectories:true)
        setupLibrary(defaultLibraryURL)
    }
    /// 文件夹所在卷的文件系统（apfs / exfat / msdos / hfs …）；路径还不存在时看最近一级已存在的上级；读不到给空串
    nonisolated static func volumeFormat(_ url: URL) -> String {
        var u = url.standardizedFileURL
        while !FileManager.default.fileExists(atPath:u.path), u.path != "/" { u.deleteLastPathComponent() }
        var s = statfs()
        guard statfs(u.path, &s) == 0 else { return "" }
        return withUnsafeBytes(of:&s.f_fstypename) { String(decoding:$0.prefix { $0 != 0 }, as:UTF8.self) }.lowercased()
    }
    func setupLibrary(_ url: URL) {
        worker("setup",extra:["library":url.path,"libraryId":""]) { result in
            self.preferences.library = result["library"] as? String ?? url.path; self.preferences.libraryId = result["libraryId"] as? String ?? ""; self.savePreferences(); self.refresh(); self.checkEnvironment()
        }
    }
    func checkEnvironmentAtStartup() {
        guard !requestedStartupEnvironmentCheck else { return }
        checkEnvironment()
    }
    func checkEnvironment(completion: (() -> Void)? = nil) {
        // Keep this attempt scoped to the Store's app session, including failures.
        // Reappearing windows and onboarding reuse it; explicit checks still refresh.
        requestedStartupEnvironmentCheck = true
        if let completion { environmentCallbacks.append(completion) }
        guard !checkingEnvironment else { return }
        checkingEnvironment = true; environmentReportComplete = false; checks = []
        // 每次检查环境都跟到装着的最新 Xcode（不让玩家选；装了新 Xcode 自动换过去）
        if let found = Self.detectXcode(), found != preferences.developerDir {
            preferences.developerDir = found; savePreferences()
        }
        let developerDir=preferences.developerDir,libraryId=preferences.libraryId,engineDir=preferences.engineDir
        worker("preflight") { r in
        if self.preferences.developerDir != developerDir || self.preferences.libraryId != libraryId || self.preferences.engineDir != engineDir {
            self.checkingEnvironment=false;self.checkEnvironment();return
        }
        if let checks = Self.decode([ToolCheck].self,r["checks"]) { self.checks = checks;self.environmentReportComplete=true }
        let engineInfo = r["engineInfo"] as? [String:Any]
        self.engineSupportsInput = engineInfo?["inputConfig"] as? Bool == true
        self.engineInputReason = engineInfo?["inputConfigReason"] as? String ?? ""
        self.checkingEnvironment = false
        let callbacks = self.environmentCallbacks; self.environmentCallbacks = []
        callbacks.forEach { $0() }
        if self.environmentReady { self.startNext() }
    } }
    func pythonExecutable() throws -> URL {
        let bundled=resources.appendingPathComponent("Runtime/python/bin/python3")
        if FileManager.default.isExecutableFile(atPath:bundled.path) {return bundled}
        if engineBundle != nil {throw NSError(domain:"S2Arm",code:1,userInfo:[NSLocalizedDescriptionKey:L("应用内置 Python 缺失，请重新安装完整应用。")])}
        if let selected=preferences.pythonPath, !selected.isEmpty {
            guard FileManager.default.isExecutableFile(atPath:selected) else {throw NSError(domain:"S2Arm",code:1,userInfo:[NSLocalizedDescriptionKey:L("设置中的 Python 不可执行，请重新选择。")])}
            return URL(fileURLWithPath:selected)
        }
        let candidates = [resources.appendingPathComponent("Runtime/python/bin/python3").path,
                          "/opt/homebrew/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"].compactMap{$0}
        if let path=candidates.first(where:{FileManager.default.isExecutableFile(atPath:$0)}) { return URL(fileURLWithPath:path) }
        throw NSError(domain:"S2Arm",code:1,userInfo:[NSLocalizedDescriptionKey:L("找不到 Python，请在设置中指定 Python 3.12 或更新版本。")])
    }
    func jobEngineURL() throws -> URL {
        let bundled=resources.appendingPathComponent("Jobs/main.py")
        if FileManager.default.fileExists(atPath:bundled.path) { return bundled }
        let checkout=URL(fileURLWithPath:preferences.engineDir ?? "").appendingPathComponent("pack/jobs/main.py")
        guard FileManager.default.fileExists(atPath:checkout.path) else { throw NSError(domain:"S2Arm",code:2,userInfo:[NSLocalizedDescriptionKey:L("请选择包含 pack/jobs 的 S2Arm 源码目录。")]) }
        return checkout
    }
    func failEnvironmentBootstrap(_ detail:String) {
        checkingEnvironment = false; environmentReportComplete = false; environmentCallbacks = []
        checks = [.init(name:"Python",ok:false,message:"Python 无法启动环境检查",help:"请安装 Python 3.12 或更新版本，并在设置中指定可执行文件。",action:"reinstall",group:"bundled",requiredFor:["prepare","launch"],detail:detail)]
    }
    private func watchEnvironmentWorker(_ id:String) {
        DispatchQueue.main.asyncAfter(deadline:.now()+45) { [weak self] in
            guard let self,let process=self.processes[id],process.isRunning,self.checkingEnvironment else {return}
            if Date().timeIntervalSince(self.lastWorkerEvent[id] ?? .distantPast) >= 40 {
                process.terminate();self.failEnvironmentBootstrap("工具检查超时，请重新检查或修复对应组件。")
            } else { self.watchEnvironmentWorker(id) }
        }
    }
    func repairEnvironment(_ action:String) {
        switch action {
        case "prepareEngine": prepareEngine()
        case "reinstall": NSWorkspace.shared.open(URL(string:"https://github.com/jhaizhou-ops/S2Arm")!)
        case "systemUpdate": NSWorkspace.shared.open(URL(string:"x-apple.systempreferences:com.apple.Software-Update-Settings.extension")!)
        case "library": chooseLibrary()
        // 外部依赖一键处理（维护者 10-23）：Xcode 跳 App Store；装了没打开过就打开它（同意许可）；Metal 组件在 App 里一键下载，下完自动重新检查
        case "appStoreXcode": NSWorkspace.shared.open(URL(string:"macappstore://apps.apple.com/app/id497799835")!)
        case "openXcode": if !preferences.developerDir.isEmpty { NSWorkspace.shared.open(URL(fileURLWithPath:preferences.developerDir).deletingLastPathComponent().deletingLastPathComponent()) }
        case "installDotnet":
            guard canManageToolchain else { return }
            executionTask = worker("installDotnet") { _ in self.checkEnvironment { self.toolchainInstalled("installDotnet", ".NET SDK 已安装。", Store.dotnetInstallRecheckFailed) } }   // 复查过了才写已安装（审计 18:00 P2-36）
        case "downloadMetal":
            guard canManageToolchain else { return }
            executionTask = worker("downloadMetal") { _ in self.checkEnvironment { self.toolchainInstalled("downloadMetal", "Metal 编译组件已安装。", Store.metalInstallRecheckFailed) } }
        case "downloadIosPlatform":   // 11-07：新装 / 刚升级的 Xcode 不带 iOS 平台，打 iPhone 包、装到 iPhone 都要它（约 8 GB）
            guard canManageToolchain else { return }
            executionTask = worker("downloadIosPlatform") { _ in self.checkEnvironment { self.toolchainInstalled("downloadIosPlatform", "iOS 平台组件已安装。", Store.iosPlatformInstallRecheckFailed) } }
        case "recheck":   // 组件已经下载、只是没注册就绪：重查一遍，不要再下一遍 8 GB
            checkEnvironment()
        default: break
        }
    }
    func prepareEngine() {
        guard canManageToolchain,!(preferences.engineDir ?? "").isEmpty else {return}
        savePreferences()
        executionTask=worker("prepareEngine") { _ in self.notice="S2Arm 开发工具已就绪。" }
    }
    // Xcode 自动找（维护者 10-23：不让玩家选）：记的那份不完整时，先看系统当前选中的开发目录，再扫 /Applications 与 ~/Applications 里的 Xcode，取版本最高的完整一份
    static func detectXcode() -> String? {
        func full(_ dev: String) -> Bool { FileManager.default.isExecutableFile(atPath: dev + "/usr/bin/xcodebuild") && dev.hasSuffix(".app/Contents/Developer") }
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select"); p.arguments = ["-p"]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        // 取装着的 Xcode 里版本最新的一个（含 xcode-select 指的那个）：iPhone 包的着色器按目标设备系统大版本出，老 SDK 出不了新系统的
        //（10-23 本机 xcode-select 指 26.6、另装 27.1，iOS 27 设备只能用 27.x）
        var apps: [String] = []
        if (try? p.run()) != nil { p.waitUntilExit()
            let sel = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            if p.terminationStatus == 0, full(sel) { apps.append(String(sel.dropLast("/Contents/Developer".count))) } }
        for dir in ["/Applications", NSHomeDirectory() + "/Applications"] {
            for name in (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? [] where name.hasPrefix("Xcode") && name.hasSuffix(".app") { apps.append(dir + "/" + name) }
        }
        var best: (String, String)? = nil
        do {
            for app in apps {
                guard let info = NSDictionary(contentsOfFile: app + "/Contents/Info.plist"), info["CFBundleIdentifier"] as? String == "com.apple.dt.Xcode",
                      full(app + "/Contents/Developer") else { continue }
                let v = info["CFBundleShortVersionString"] as? String ?? "0"
                if best == nil || v.compare(best!.1, options: .numeric) == .orderedDescending { best = (app + "/Contents/Developer", v) }
            }
        }
        return best?.0
    }
    func openDocumentation(_ guide:AppGuide) {
        if documentationWindow == nil {
            let window=NSWindow(contentRect:NSRect(x:0,y:0,width:900,height:720),styleMask:[.titled,.closable,.miniaturizable,.resizable],backing:.buffered,defer:false)
            window.isReleasedWhenClosed=false;window.center();documentationWindow=window
        }
        documentationWindow?.title=guide.title
        documentationWindow?.contentView=NSHostingView(rootView:DocumentationView(guide:guide).environmentObject(self))
        documentationWindow?.makeKeyAndOrderFront(nil)
    }
    func openOnboarding() {
        onboardingStep = !environmentReady ? 0 : preferences.library.isEmpty ? 1 : hasImportedKeys ? 3 : 2
        showOnboarding = true
    }
    /// 放游戏还缺哪一步（控制口报给调用方，顺序同 importPrerequisitesReady）
    var importBlocker: String {
        if !environmentReady { return "环境检查没通过（s2armctl env 看缺什么）" }
        if preferences.library.isEmpty { return "还没选游戏库" }
        return "还没导入 prod.keys（s2armctl keys <文件>）"
    }
    func importPrerequisitesReady() -> Bool {
        guard environmentReady else { onboardingStep = 0; showOnboarding = true; checkEnvironment(); return false }
        guard !preferences.library.isEmpty else { onboardingStep = 1; showOnboarding = true; return false }
        guard hasImportedKeys, FileManager.default.isReadableFile(atPath:privateRoot.appendingPathComponent("keys/prod.keys").path) else {
            hasImportedKeys = false; keyStatus = "请先导入 prod.keys，再添加游戏"; onboardingStep = 2; showOnboarding = true; return false
        }
        return true
    }
    func finishOnboarding(addGames: Bool) {
        guard importPrerequisitesReady() else { return }
        preferences.onboardingComplete = true; savePreferences()
        addGamesAfterOnboarding = addGames; showOnboarding = false
    }
    func deferOnboarding() {
        // Dismissing setup never marks prerequisites as satisfied or opens import.
        addGamesAfterOnboarding = false
        quitAfterOnboardingDismissal = nil
        showOnboarding = false
    }
    func quitApplication(terminate: (@MainActor () -> Void)? = nil) {
        let quit: @MainActor () -> Void = terminate ?? { NSApp.terminate(nil) }
        addGamesAfterOnboarding = false
        // AppKit can ignore terminate while a sheet is attached. Wait for SwiftUI
        // to finish dismissing it, then use the normal delegate/worker shutdown.
        guard showOnboarding else { quit(); return }
        quitAfterOnboardingDismissal = quit
        showOnboarding = false
    }
    func onboardingDismissed() {
        if let quit = quitAfterOnboardingDismissal {
            quitAfterOnboardingDismissal = nil
            quit()
            return
        }
        if addGamesAfterOnboarding { addGamesAfterOnboarding = false; chooseGames() }
    }
    // 记着的游戏库目录整个不在了（被删、旧测试留下的路径）：忘掉它，回到缺省游戏库、重新走引导，不弹「游戏库未连接」死路框
    //（10-23 发布版实测：旧设置指向已删目录，打开就是只有「知道了 / 打开日志」的错误框）。外接盘没插不算，保持原样，报错里叫玩家插上
    func forgetVanishedLibrary() {
        let lib = preferences.library
        guard !lib.isEmpty, !FileManager.default.fileExists(atPath:lib) else { return }
        let parts = lib.split(separator:"/")
        if parts.first == "Volumes", parts.count > 1, !FileManager.default.fileExists(atPath:"/Volumes/" + parts[1]) { return }
        preferences.library = ""; preferences.libraryId = ""; preferences.onboardingComplete = false; savePreferences()
    }
    // 缺省游戏库（维护者 10-23：减少交互，首次打开不再让玩家选库）：~/S2Arm（游戏库结构 P3：访达里看得见；自定义数据目录时仍在数据目录里），设置里可换
    var defaultLibraryURL: URL { S2ArmPaths.defaultLibrary(privateRoot:privateRoot) }
    /// 任务日志 + 任务记录：固定 <私有目录>/logs（任务记录 logs/jobs/，剃刀 H6；S2ArmPaths.logsRoot，同作业层 core.logs_root）
    var logsRoot: URL { S2ArmPaths.logsRoot(privateRoot:privateRoot) }
    var jobsRoot: URL { logsRoot.appendingPathComponent("jobs") }
    func ensureLibrary(_ then: @escaping () -> Void) {
        guard preferences.library.isEmpty else { then(); return }
        worker("setup",extra:["library":defaultLibraryURL.path,"libraryId":""]) { result in
            self.preferences.library = result["library"] as? String ?? self.defaultLibraryURL.path; self.preferences.libraryId = result["libraryId"] as? String ?? ""
            self.savePreferences(); self.refresh(); then()
        }
    }
    // 密钥只由玩家给：点击选文件或拖进来（维护者 10-23：不自动查找）；导入不依赖构建环境
    func importKeys(from dropped: URL? = nil) {
        guard !importingKeys else { return }
        var picked = dropped
        if picked == nil {
            let p = NSOpenPanel(); p.title = L("选择你的 prod.keys"); p.allowsMultipleSelection = false; p.canChooseDirectories = false
            guard p.runModal() == .OK else { return }; picked = p.url
        }
        guard let url = picked else { return }
        ensureLibrary { self.worker("keys",extra:["path":url.path]) { r in self.hasImportedKeys = true; self.keyStatus = r["message"] as? String ?? "已导入"; self.refresh() } }
    }
    // 原版共享字体目录（可选）：选文件夹，存之前去首尾空白（10-23 走查：手填路径带换行，环境检查判字体缺）
    func setFontsDir(_ raw: String?) {
        let t = raw?.trimmingCharacters(in:.whitespacesAndNewlines) ?? ""
        preferences.fontsDir = t.isEmpty ? nil : t; savePreferences(); checkEnvironment()
    }
    func chooseFontsDir() {
        let p = NSOpenPanel(); p.title = L("选择含 .bfttf 的原版共享字体文件夹"); p.canChooseFiles = false; p.canChooseDirectories = true
        guard p.runModal() == .OK, let url = p.url else { return }
        setFontsDir(url.path)
    }
    // 引导页放入游戏（维护者 10-23）：只导入、留在引导页，玩家点「开始打包」才开始准备
    func addGamesFromOnboarding(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        addGames(urls, prepare: false)
    }
    // 引导页「开始打包」可点：密钥、环境都齐，库里有还没准备好的可准备游戏
    var packingCandidates: [Game] { games.filter { $0.buildable && ($0.ready != true || $0.needsPreparation == true) } }
    var canStartPacking: Bool { hasImportedKeys && environmentReady && !packingCandidates.isEmpty }
    func startPackingFromOnboarding() {
        guard canStartPacking else { return }
        preferences.onboardingComplete = true; savePreferences(); showOnboarding = false
        packingCandidates.forEach { enqueue($0) }
        selection = "tasks"
    }
    func chooseGames() { pickGames { self.addGames($0) } }
    func chooseGamesFromOnboarding() { pickGames { self.addGamesFromOnboarding($0) } }
    private func pickGames(_ then: ([URL]) -> Void) {
        guard importPrerequisitesReady() else { return }
        let p = NSOpenPanel(); p.title = L("添加 NSP 文件或扫描文件夹"); p.allowsMultipleSelection = true; p.canChooseDirectories = true; p.canChooseFiles = true
        if p.runModal() == .OK { then(p.urls) }
    }
    // 放进来先由 App 自己读一下（10-23 本机走查）：「下载」「桌面」「文稿」这类受保护文件夹的系统授权框就在放进来那一刻弹出；
    //   原来由后台读取工具第一次读时才弹，界面一直停在「读取游戏信息」，看不出在等授权。读不了（玩家点了拒绝）就直接说清楚
    static func unreadable(_ urls: [URL]) -> URL? {
        for u in urls {
            var isDir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: u.path, isDirectory: &isDir) else { return u }
            if isDir.boolValue { if (try? FileManager.default.contentsOfDirectory(atPath: u.path)) == nil { return u } }
            else if let h = try? FileHandle(forReadingFrom: u) { _ = try? h.read(upToCount: 1); try? h.close() } else { return u }
        }
        return nil
    }
    func addGames(_ urls: [URL], prepare prepareOverride: Bool? = nil) {
        guard !urls.isEmpty else { return }
        // 环境检查正在跑（导入密钥后会重查一次）：等它跑完再导。原先按「环境没齐」处理，这次放进来的游戏被静默丢掉，
        //   界面退回引导页第 0 步、没有导入任务也没有报错（11-02 用户体验线走查：导完密钥立刻放游戏，稳定复现）
        if checkingEnvironment { checkEnvironment { self.addGames(urls, prepare:prepareOverride) }; return }
        guard importPrerequisitesReady() else { return }
        readThenImport(urls, prepare:prepareOverride)
    }
    // 第一次读「下载 / 桌面 / 文稿」时 macOS 同步弹授权框、把调用线程挂住：预检放后台，主线程不被挂（10-23 AI 试用：控制口整个卡死、报「App 退了？」）。
    //   等授权时游戏库顶上写明在等什么（审计 P1-3：原先只写给控制口，界面上什么都没有）；这期间再拖进来的排队，不丢
    func readThenImport(_ urls: [URL], prepare prepareOverride: Bool?) {
        if waitingForAccess != nil {
            pendingAccess.append((urls, prepareOverride))
            return
        }
        waitingForAccess = urls[0].path
        DispatchQueue.global(qos:.userInitiated).async {
            let bad = Self.unreadable(urls)
            DispatchQueue.main.async {
                self.waitingForAccess = nil
                if let bad { self.error = L("读不了「\(bad.lastPathComponent)」：S2Arm 没有读取这个文件夹的权限。到「系统设置 → 隐私与安全性 → 文件与文件夹」里给 S2Arm 打开，再放一次。") }
                else { self.importGames(urls, prepare:prepareOverride) }
                if !self.pendingAccess.isEmpty { let (next, prepare) = self.pendingAccess.removeFirst(); self.readThenImport(next, prepare:prepare) }
            }
        }
    }
    private func importGames(_ urls: [URL], prepare prepareOverride: Bool?) {
        let libraryId = preferences.libraryId
        // 从游戏库添加：放进来就自动打包（维护者 10-23：不让玩家选；原「导入后自动准备」开关已去掉）。引导页传 false，由「开始打包」启动
        let prepare = prepareOverride ?? true
        worker("import",extra:["paths":urls.map(\.path)]) { result in
            guard libraryId == self.preferences.libraryId else { return }
            let ids = Set((result["imports"] as? [[String:Any]] ?? []).compactMap { item in item["duplicate"] as? Bool == false ? item["id"] as? String : nil })
            self.refresh {
                guard libraryId == self.preferences.libraryId else { return }
                let imported = self.games.filter { ids.contains($0.id) }
                if let first = imported.first { self.selectedGame = first.id }
                self.notice = "导入完成：游戏原件留在原处，S2Arm 只读、不复制。"
                let candidates = imported.filter { $0.buildable && ($0.ready != true || $0.needsPreparation == true) }
                guard prepare, !candidates.isEmpty else { return }
                self.checkEnvironment {
                    guard libraryId == self.preferences.libraryId else { return }
                    guard self.environmentReady else {
                        self.selection = "settings"; self.notice = "游戏已导入，请完成工具链检查后准备运行文件。"; return
                    }
                    candidates.forEach { self.enqueue($0, auto:true) }
                    self.notice = "导入完成，文件齐全的游戏已加入准备队列；完成后可启动。"
                }
            }
        }
    }
    func refresh(completion: (() -> Void)? = nil) {
        guard !preferences.library.isEmpty else { return }
        let libraryId = preferences.libraryId
        worker("catalog",visible:false) { r in
            guard libraryId == self.preferences.libraryId else { return }
            if let games = Self.decode([Game].self,r["games"]) { self.games = games.filter { !$0.hidden } }
            if let profiles = Self.decode([GameProfile].self,r["profiles"]) { self.profiles = profiles }
            let keys = r["keys"] as? [String:Any] ?? [:]
            self.hasImportedKeys = keys["ready"] as? Bool == true
            self.keyStatus = keys["message"] as? String ?? "尚未导入 prod.keys"
            if let records = Self.decode([TaskRecord].self,r["jobs"]) {
                for rec in records where rec.state=="failed" || rec.operation.hasPrefix("platform") || rec.operation=="setup" || Self.visibleOperations.contains(rec.operation) {
                    if !self.tasks.contains(where: { $0.id == rec.id }) { self.tasks.append(rec) }
                }
                self.tasks.sort { ($0.started ?? 0) > ($1.started ?? 0) }
            }
            if self.selectedGame == nil { self.selectedGame = self.games.first?.id }
            completion?()
        }
    }
    static func decode<T:Decodable>(_ type: T.Type, _ obj: Any?) -> T? {
        guard let obj, JSONSerialization.isValidJSONObject(obj), let d = try? JSONSerialization.data(withJSONObject:obj) else { return nil }
        return try? JSONDecoder().decode(type,from:d)
    }
    @discardableResult func worker(_ operation: String, extra: [String:Any] = [:], visible: Bool = true, completion: (([String:Any])->Void)? = nil) -> String {
        let id = UUID().uuidString.lowercased()
        if operation=="catalog" {catalogWorkers+=1}
        var req: [String:Any] = ["schema":1,"taskId":id,"operation":operation,"privateRoot":privateRoot.path,"resources":resources.path,"engineDir":preferences.engineDir ?? "","library":preferences.library.isEmpty ? privateRoot.appendingPathComponent("unconfigured").path : preferences.library,"libraryId":preferences.libraryId,"developerDir":preferences.developerDir,"fontsDir":preferences.fontsDir ?? "","devHud":preferences.devHud ?? false,"metalHud":preferences.metalHud ?? false,"buildId":S2ArmRelease.buildID,"parentPid":ProcessInfo.processInfo.processIdentifier]
        extra.forEach { req[$0] = $1 }
        if let engineBundle {req["engineBundle"]=engineBundle.path;req["engineDir"]=engineBundle.path}
        // Environment setup must remain available when an old library drive is offline.
        if ["preflight","prepareEngine"].contains(operation) {
            req["libraryId"] = ""
            req["storageLibraryId"] = preferences.libraryId
        }
        let dir = privateRoot.appendingPathComponent("requests"); try? FileManager.default.createDirectory(at:dir,withIntermediateDirectories:true); let file = dir.appendingPathComponent(id+".json")
        do {
            let data = try JSONSerialization.data(withJSONObject:req); try data.write(to:file,options:.atomic)
            let python = try pythonExecutable()
            let isGUI = operation.hasPrefix("platform")
            let entry = try isGUI ? guiJobURL() : jobEngineURL()
            let process = Process(); process.executableURL = python; process.arguments = ["-B","-I",entry.path,"--request",file.path]
            var env = ProcessInfo.processInfo.environment; env["PYTHONUNBUFFERED"] = "1"; env["PYTHONDONTWRITEBYTECODE"] = "1"; env["PYTHONNOUSERSITE"] = "1"; env.removeValue(forKey:"PYTHONPATH"); env.removeValue(forKey:"PYTHONHOME"); process.environment = env
            let pipe = Pipe(); process.standardOutput = pipe; let err = Pipe(); process.standardError = err
            let output = WorkerOutput(), readers=DispatchGroup()
            readers.enter();readers.enter()
            process.terminationHandler = { p in
                // EOF readers enqueue every event before this cleanup. A fast
                // worker's final result must not race termination notification.
                readers.notify(queue:.main) {
                    DispatchQueue.main.async {
                    self.processes[id] = nil; try? FileManager.default.removeItem(at:file)
                    if operation=="catalog" {self.catalogWorkers=max(0,self.catalogWorkers-1)}
                    self.lastWorkerEvent[id] = nil
                    if self.yieldedTasks.remove(id) != nil, let i = self.tasks.firstIndex(where:{$0.id == id}) {   // 让位停掉的自动准备：记取消、写明让位，不弹错
                        self.tasks[i].state = "cancelled"; self.tasks[i].message = Store.yieldMessage; self.tasks[i].diagnostics = nil; self.persistTask(self.tasks[i])
                    } else if let i = self.tasks.firstIndex(where:{$0.id == id}), self.tasks[i].active {
                        self.tasks[i].state = p.terminationStatus == 130 ? "cancelled" : "interrupted"; self.tasks[i].message = "工作进程已退出。可重试并验证完成阶段。"
                        if p.terminationStatus != 130 {
                            let log=self.logsRoot.appendingPathComponent("\(id)-bootstrap.log")
                            try? FileManager.default.createDirectory(at:log.deletingLastPathComponent(),withIntermediateDirectories:true)
                            try? output.errorText.write(to:log,atomically:true,encoding:.utf8)
                            self.tasks[i].diagnostics=[self.nativeDiagnostic(code:"worker_interrupted",summary:"工作进程已退出。可重试并验证完成阶段。",detail:output.errorText,logPath:log.path)]
                            self.persistTask(self.tasks[i])
                            self.errorDiagnostic=self.tasks[i].diagnostics?.last
                            self.error=self.tasks[i].message
                        }
                        if operation == "preflight" { self.failEnvironmentBootstrap(output.errorText.isEmpty ? "工作进程已退出。可重试并验证完成阶段。" : output.errorText) }
                    }
                    self.callbacks[id] = nil
                    if self.executionTask == id {
                        self.executionTask = nil
                        if !["prepareEngine"].contains(operation) { self.startNext() }
                    }
                    if ["prepareEngine"].contains(operation) { self.checkEnvironment() }
                    if visible { self.refresh() }
                    }
                }
            }
            if visible { tasks.insert(.init(id:id,operation:operation,state:"running",gameId:extra["gameId"] as? String,started:Date().timeIntervalSince1970,outputPath:extra["outputPath"] as? String,target:extra["target"] as? String),at:0) }
            callbacks[id] = completion; processes[id] = process
            try process.run()
            DispatchQueue.global(qos:.utility).async {
                defer {try? pipe.fileHandleForReading.close();readers.leave()}
                while true {
                    let chunk=pipe.fileHandleForReading.availableData
                    if chunk.isEmpty {break}
                    for line in output.append(chunk) {
                        guard let event=(try? JSONSerialization.jsonObject(with:line)) as? [String:Any] else {continue}
                        DispatchQueue.main.async {self.event(event,visible:visible)}
                    }
                }
            }
            DispatchQueue.global(qos:.utility).async {
                defer {try? err.fileHandleForReading.close();readers.leave()}
                while true {let chunk=err.fileHandleForReading.availableData;if chunk.isEmpty {break};output.error(chunk)}
            }
            if operation == "preflight" {lastWorkerEvent[id]=Date();watchEnvironmentWorker(id)}
        } catch {
            if operation=="catalog" {catalogWorkers=max(0,catalogWorkers-1)}
            self.error = "无法启动本地组件：\(error.localizedDescription)"; processes[id] = nil; callbacks[id] = nil
            let diagnostic=nativeDiagnostic(code:"worker_start_failed",summary:self.error ?? "",detail:error.localizedDescription)
            self.errorDiagnostic=diagnostic
            if let i=tasks.firstIndex(where:{$0.id==id}) { tasks[i].state="failed";tasks[i].message=self.error;tasks[i].diagnostics=[diagnostic];persistTask(tasks[i]) }
            else {
                let task=TaskRecord(id:id,operation:operation,state:"failed",gameId:extra["gameId"] as? String,message:self.error,started:Date().timeIntervalSince1970,target:extra["target"] as? String,diagnostics:[diagnostic])
                tasks.insert(task,at:0);persistTask(task)
            }
            try? FileManager.default.removeItem(at:file)
            if operation == "preflight" { failEnvironmentBootstrap(error.localizedDescription) }
            DispatchQueue.main.async {if self.executionTask==id {self.executionTask=nil;self.startNext()}}
        }
        return id
    }
    func event(_ event: [String:Any], visible: Bool) {
        var e=event
        if e["taskId"] == nil {e["taskId"]=e["jobId"]}
        if e["state"] as? String == "succeeded" {e["state"]="completed"}

        guard e["schema"] as? Int == 1, let id = e["taskId"] as? String else { return }
        if e["type"] as? String == "result",e["state"] as? String == "failed",!tasks.contains(where:{$0.id==id}) {
            tasks.insert(.init(id:id,operation:e["operation"] as? String ?? "worker",state:"failed",gameId:e["gameId"] as? String,started:(e["started"] as? NSNumber)?.doubleValue ?? Date().timeIntervalSince1970),at:0)
        }
        if lastWorkerEvent[id] != nil {lastWorkerEvent[id]=Date()}
        if e["type"] as? String == "check", tasks.contains(where:{$0.id==id && $0.operation=="preflight"}), let check=Self.decode(ToolCheck.self,e["check"]) {
            if let index=checks.firstIndex(where:{$0.id==check.id}) {checks[index]=check} else {checks.append(check)}
        }
        if let i = tasks.firstIndex(where:{$0.id == id}) {
            if e["type"] as? String == "space" {
                tasks[i].space=Self.decode(SpaceBudget.self,e["space"]) ?? .init(gameBytes:nil,volumes:[])
            }
            if e["type"] as? String == "plan", let plan=Self.decode([Milestone].self,e["milestones"]) {   // 同一组步重发（改标复用）只换预计、不清已走的时间
                if plan.map(\.id) != (tasks[i].milestones ?? []).map(\.id) {tasks[i].milestoneStarted=[:]}; tasks[i].milestones=plan}
            if let m=e["milestone"] as? String, tasks[i].milestone != m {
                tasks[i].milestone=m; var st=tasks[i].milestoneStarted ?? [:]; if st[m]==nil {st[m]=Date().timeIntervalSince1970}; tasks[i].milestoneStarted=st
            }
            if let stage=e["stage"] as? String {tasks[i].stage=stage}
            if let index=e["stageIndex"] as? Int {tasks[i].stageIndex=index}
            if let count=e["stageCount"] as? Int {tasks[i].stageCount=count}
            if e["type"] as? String == "stage" || e["resetProgress"] as? Bool == true {tasks[i].done=nil;tasks[i].total=nil;tasks[i].unit=nil;tasks[i].counters=nil}
            if let counters=Self.decode([TaskCounter].self,e["counters"]) {tasks[i].counters=counters}
            if e["done"] != nil || e["total"] != nil {tasks[i].done=(e["done"] as? NSNumber)?.doubleValue;tasks[i].total=(e["total"] as? NSNumber)?.doubleValue;tasks[i].unit=e["unit"] as? String}
            if let elapsed=(e["elapsed"] as? NSNumber)?.doubleValue {tasks[i].elapsed=elapsed}
            if let msg = e["message"] as? String, !msg.isEmpty { tasks[i].message = msg }
            if let state = e["state"] as? String { tasks[i].state = state }
            if let diagnostic=Self.decode(TaskDiagnostic.self,e["diagnostic"]) {
                var records=tasks[i].diagnostics ?? []
                if let existing=records.firstIndex(where:{$0.id==diagnostic.id}) {records[existing]=diagnostic}
                else {records.append(diagnostic)}
                tasks[i].diagnostics=Array(records.suffix(24))
            }
            if let diagnostics=Self.decode([TaskDiagnostic].self,e["diagnostics"]) {tasks[i].diagnostics=diagnostics}
        }
        if e["type"] as? String == "result" {
            if let result = e["result"] as? [String:Any], let index = tasks.firstIndex(where: { $0.id == id }) {
                if let path = result["outputPath"] as? String { tasks[index].outputPath = path }
                if let path = result["ipa"] as? String { tasks[index].ipaPath = path }
                if let device = result["device"] as? String { tasks[index].device = device }
                if let files = result["packFiles"] as? Int { tasks[index].packFiles = files }
                if let pending = result["unpackPending"] as? String { tasks[index].unpackPending = pending }
            }
            let state = e["state"] as? String
            if state == "completed" { let callback=callbacks.removeValue(forKey:id);callback?(e["result"] as? [String:Any] ?? [:]) }
            if let op = tasks.first(where:{$0.id==id})?.operation, op == "ipa" || op == "install" || op == "delete" { refreshPackShelf() }
            // 一次导出多个平台（11-02 维护者）：iPhone 版这条结束了（成败都算，两个平台互不依赖）再排 Mac 版——两个任务不能同时跑
            if tasks.first(where:{$0.id==id})?.operation == "ipa", let gid = tasks.first(where:{$0.id==id})?.gameId, macAfterIPA.remove(gid) != nil,
               let g = games.first(where:{$0.id==gid}) { DispatchQueue.main.async { self.enqueue(g) } }
            else if state == "failed", !yieldedTasks.contains(id) {   // 让位停掉的自动准备不弹错（进程退出时记成已让位）
                errorDiagnostic=tasks.first(where:{$0.id==id})?.diagnostics?.last(where:{!$0.isWarning})
                error = e["message"] as? String ?? "操作失败，请查看任务日志。"
                if tasks.contains(where:{$0.id==id && $0.operation=="preflight"}) {checkingEnvironment=false;environmentCallbacks=[]}
            }
        }
    }
    /// enqueue 会不会被挡回、为什么（控制口 POST /build target=mac 先问它：原先挡回时静默 return，控制口照回 ok 却没建任务）；不挡 → nil
    func enqueueBlocker(_ game: Game) -> String? {
        // 同 importPrerequisitesReady 的三条，但不弹引导页（这里只是问）
        if !environmentReady { return "环境缺东西：看 GET /state 的 checks，处理后 POST /environment 重查" }
        if preferences.library.isEmpty { return "还没选游戏库：s2armctl library <目录>" }
        if !hasImportedKeys || !FileManager.default.isReadableFile(atPath: privateRoot.appendingPathComponent("keys/prod.keys").path) { return "还没放密钥：s2armctl keys <prod.keys>" }
        if !game.buildable { return "这款还不能编译：" + ((game.missing ?? []).isEmpty ? "兼容性不支持" : (game.missing ?? []).joined(separator: "、")) }
        if queue.contains(game.id) || tasks.contains(where: { $0.operation == "build" && $0.gameId == game.id && $0.active && !$0.id.hasPrefix("queued-") }) { return "这款已在编译或排队，没有新建任务" }
        return nil
    }
    func enqueue(_ game: Game, auto: Bool = false) {
        guard importPrerequisitesReady() else { return }
        guard game.buildable else { showRequirements(game); return }
        guard !queue.contains(game.id), !tasks.contains(where: { $0.operation == "build" && $0.gameId == game.id && $0.active && !$0.id.hasPrefix("queued-") }) else { return }
        queue.append(game.id)
        if auto { autoQueue.insert(game.id) } else { autoQueue.remove(game.id) }
        let rec = TaskRecord(id:"queued-"+game.id,operation:"build",state:"queued",gameId:game.id,started:Date().timeIntervalSince1970)
        if let d = try? JSONEncoder().encode(rec) {persist(d,to:jobsRoot.appendingPathComponent("\(rec.id).json"),"出包队列")}
        selection = "tasks"; startNext()
    }
    func dequeue(_ id:String) {
        queue.removeAll {$0==id}; spaceOverrideGames.remove(id)
        tasks.removeAll {$0.id=="queued-"+id}
        let path=jobsRoot.appendingPathComponent("queued-\(id).json")
        if let d=try? Data(contentsOf:path),var record=try? JSONDecoder().decode(TaskRecord.self,from:d){record.state="cancelled";record.message="已从队列移除";if let data=try? JSONEncoder().encode(record){persist(data,to:path,"出包队列")}}
    }
    func startNext() {
        guard !executionBusy, environmentReady else { return }
        if let pending = pendingIPA { pendingIPA = nil; packIPhone(pending.game, spaceOverride:pending.spaceOverride); return }   // 让位的自动准备已停：接着出玩家点的 iPhone 包
        if !yieldedGames.isEmpty {   // iPhone 包出完（成败都算）：让位的游戏自动排回准备队列
            let ids = yieldedGames; yieldedGames = []
            games.filter { ids.contains($0.id) }.forEach { enqueue($0, auto:true) }
            if executionBusy { return }
        }
        guard !queue.isEmpty else { return }
        let id = queue.removeFirst(); tasks.removeAll {$0.id=="queued-"+id}; try? FileManager.default.removeItem(at:jobsRoot.appendingPathComponent("queued-\(id).json")); executionTask = worker("build",extra:spaceOverrideGames.remove(id) != nil ? ["gameId":id,"spaceOverride":true] : ["gameId":id]) { _ in self.notice = "打包完成，可以启动了。" }
        autoBuildTask = autoQueue.remove(id) != nil ? executionTask : nil
    }
    func launch(_ game: Game, inputGuideHandled: Bool = false) {
        guard !executionBusy, queue.isEmpty else { error = "构建与游戏运行互斥，请等待当前任务结束。"; return }
        guard runtimeReady else {
            checkEnvironment {
                if self.runtimeReady {self.launch(game, inputGuideHandled: inputGuideHandled)}
                else {self.onboardingStep=0;self.showOnboarding=true}
            }
            return
        }
        prepareInputGuide(game, handled: inputGuideHandled)
    }
    func cancel(_ task: TaskRecord) {
        // 排队中的任务（id queued-<游戏>）没有进程：移出队列（dequeue），不然只把状态置成 cancelling、按钮变灰，游戏仍在队列里、前一个跑完照样被 startNext 拉起（11-01 联合开发者反馈）
        if task.id.hasPrefix("queued-"), let gid = task.gameId { autoQueue.remove(gid); dequeue(gid); return }
        if let i = tasks.firstIndex(where:{$0.id == task.id}) { tasks[i].state = "cancelling" }
        processes[task.id]?.terminate()
    }
    func retry(_ task: TaskRecord) {
        if task.operation.hasPrefix("platform"), let platform = BuildPlatform(rawValue:task.target ?? "") {
            buildPlatform = platform; selection = "platforms"; return
        }
        guard let game = games.first(where:{$0.id == task.gameId}) else { return }
        error=nil
        switch Store.retryRoute(task) {
        case .saveManager: saveManagerGame=game
        case .launch: launch(game)
        case .ipa: packIPhone(game)
        case .install: installToPhone(game)
        case .build: enqueue(game)
        }
    }
    /// 空间预检拦下后「仍然继续」：同一款、同一种出包（Mac 打包 / iPhone 包）重交，请求带 spaceOverride，后端不拦、只记警告
    func continueDespiteSpace(_ task: TaskRecord) {
        guard task.spacePreflightBlocked, let game = games.first(where:{$0.id == task.gameId}) else { return }
        error=nil
        if task.operation == "ipa" { packIPhone(game, spaceOverride:true) }
        else {
            spaceOverrideGames.insert(game.id); enqueue(game)
            if !queue.contains(game.id) { spaceOverrideGames.remove(game.id) }   // 没排上（前置不齐等）或已起任务（startNext 已取走）：不留到下一次
        }
    }
    enum RetryRoute: Equatable { case saveManager, launch, ipa, install, build }
    /// 重试接着做失败的那一步（审计 P1-1）：「推到 iPhone」失败只重签重装（包已出好），不落回「准备」从头编译
    static func retryRoute(_ task: TaskRecord) -> RetryRoute {
        switch task.operation {
        case "saveBackup", "saveInspect", "saveRestore": return .saveManager
        case "launch": return .launch
        case "ipa": return .ipa
        case "install": return .install
        default: return .build
        }
    }
    func showRequirements(_ game: Game) {
        error = (game.missing ?? []).joined(separator:"\n")
        errorDiagnostic = game.requirementDiagnostics?.first
    }
    func remove(_ game: Game) { worker("remove",extra:["gameId":game.id]) { _ in self.refresh() } }
    // 删除这款游戏（10-26）：先撤掉它排队的打包 / 让位等着恢复的、出 iPhone 包等着的，再结束它在跑的任务（导入后自动排的 Mac 准备会占着执行锁），
    //   然后交给 worker 删（library.delete 等执行锁让出，最多 120 秒）
    func deleteGame(_ game: Game) {
        queue.removeAll { $0 == game.id }; autoQueue.remove(game.id); yieldedGames.removeAll { $0 == game.id }
        if pendingIPA?.id == game.id { pendingIPA = nil }
        for t in tasks where t.gameId == game.id && t.operation != "delete" && ["running","queued","cancelling"].contains(t.state) { cancel(t) }
        worker("delete",extra:["gameId":game.id]) { _ in self.refresh() }
    }
    func clean(_ game: Game, kind: String) { worker("clean",extra:["gameId":game.id,"kind":kind]) { _ in self.refresh() } }
    func reveal(_ url: URL) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
    // #377：只读取完成事件给出的那一个 IPA，不根据日志或资源目录猜测。
    func packEvidence(for task: TaskRecord) -> PackGraphicsEvidence {
        guard task.state == "completed" else { return PackGraphicsEvidence() }
        let key = task.id + "|" + task.state + "|" + (task.ipaPath ?? "")
        if let cached = packEvidenceCache[key] { return cached }
        let evidence = PackGraphicsEvidenceReader.read(ipa: task.ipaPath.map { URL(fileURLWithPath: $0) })
        packEvidenceCache[key] = evidence
        return evidence
    }
    func packMode(for task: TaskRecord) -> PackGraphicsMode { PackModeJudge.mode(packEvidence(for: task)) }
    func persistTask(_ task: TaskRecord) {
        let folder=jobsRoot
        let path=folder.appendingPathComponent(task.id+".json")
        try? FileManager.default.createDirectory(at:folder,withIntermediateDirectories:true)
        if let data=try? JSONEncoder().encode(task) {persist(data,to:path,"任务记录",privateFile:true)}
    }
    func nativeDiagnostic(code: String, summary: String, detail: String, logPath: String? = nil) -> TaskDiagnostic {
        let paths=[(preferences.library,"[LIBRARY]"),(privateRoot.path,"[PRIVATE]"),(preferences.engineDir ?? "","[ENGINE]"),(NSHomeDirectory(),"~")]
        return TaskDiagnostic(code:code,severity:"error",summary:TaskDiagnostic.redact(summary,paths:paths),suggestion:"检查完整应用、磁盘连接和权限后重试；查看对应日志了解工作进程退出原因。",detail:TaskDiagnostic.redact(detail,paths:paths),logPath:logPath)
    }
    var failureMessage: String {
        var message=L(error ?? "")
        if let diagnostic=errorDiagnostic {
            message += "\n\n" + L("建议处理：") + L(diagnostic.suggestion)
            message += "\n\n" + L("错误代码：") + diagnostic.code
        }
        return message
    }
    /// 这条任务日志的末几行（logsRoot/<任务号>-*.log 里最新的那份；只读末尾 16 KB），任务页「实时日志」用：去掉行首时刻与空行、把私有目录换成 ~
    func logTail(for task: TaskRecord, lines n: Int) -> [String] {
        let dir = logsRoot
        guard let names = try? FileManager.default.contentsOfDirectory(atPath:dir.path) else { return [] }
        let mine = names.filter { $0.hasPrefix(task.id) && $0.hasSuffix(".log") }.map { dir.appendingPathComponent($0) }
        guard let url = mine.max(by:{ ((try? $0.resourceValues(forKeys:[.contentModificationDateKey]).contentModificationDate) ?? .distantPast) < ((try? $1.resourceValues(forKeys:[.contentModificationDateKey]).contentModificationDate) ?? .distantPast) }),
              let h = try? FileHandle(forReadingFrom:url) else { return [] }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset:size > 16384 ? size - 16384 : 0)
        let text = String(decoding:h.readDataToEndOfFile(), as:UTF8.self)
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        return text.split(separator:"\n").map { String($0).replacingOccurrences(of:"^\\[\\d{2}:\\d{2}:\\d{2}\\]\\s*", with:"", options:.regularExpression).replacingOccurrences(of:home, with:"~").trimmingCharacters(in:.whitespaces) }
            .filter { !$0.isEmpty }.suffix(n).map { $0 }
    }
    func diagnosticLogURL(_ diagnostic: TaskDiagnostic?) -> URL {
        if let path=diagnostic?.logPath {
            let url=URL(fileURLWithPath:path).resolvingSymlinksInPath()
            let roots=[logsRoot,libraryURL.appendingPathComponent("cache")]
            if url.pathExtension == "log",roots.contains(where:{url.path.hasPrefix($0.resolvingSymlinksInPath().path+"/")}) {return url}
        }
        return logsRoot
    }
    func copyDiagnostic(_ diagnostic: TaskDiagnostic? = nil) {
        let text=(diagnostic ?? errorDiagnostic)?.reportText ?? L(error ?? "")
        NSPasteboard.general.clearContents();NSPasteboard.general.setString(text,forType:.string)
    }
    func showFailedTask() {selection="tasks";showOnboarding=false;error=nil}
    // 打 iPhone 包（pack/jobs/ipa.py，调一键出包）：没装 .NET SDK 就先一键装好再接着出（玩家只点一次）；出完在访达里打开 IPA 所在文件夹
    var dotnetReady: Bool { checks.first(where:{ $0.action == "installDotnet" })?.ok ?? true }
    func packIPhone(_ game: Game, spaceOverride: Bool = false) {
        if let plan = ipaYieldPlan() {   // 后台自动准备让位（玩家主动操作优先）
            plan.drop.forEach { yieldedGames.append($0); autoQueue.remove($0); dequeue($0) }
            if let t = plan.stop {
                if let g = tasks.first(where:{ $0.id == t })?.gameId { yieldedGames.append(g) }
                yieldedTasks.insert(t); autoBuildTask = nil; pendingIPA = PendingIPhonePack(game:game, spaceOverride:spaceOverride)
                if let i = tasks.firstIndex(where:{ $0.id == t }) { tasks[i].state = "cancelling"; tasks[i].message = Store.yieldMessage }
                processes[t]?.terminate(); notice = "后台的 Mac 准备先让位，正在出 iPhone 包。"; return
            }
        }
        guard !executionBusy, queue.isEmpty else { error = "打包和运行互斥，请等当前任务结束。"; return }
        guard game.buildable else { showRequirements(game); return }
        selection = "tasks"
        if !dotnetReady {
            executionTask = worker("installDotnet") { _ in self.checkEnvironment {
                if self.dotnetReady { self.packIPhone(game, spaceOverride:spaceOverride) }
                else { self.error = Store.dotnetRecheckFailed; self.selection = "settings" }   // 审计 P2-1：装完复查仍不过要报出来，不静默停住
            } }
            return
        }
        // 一包通用（10-30 维护者）：直连包与机型无关，出包不要求目标设备；知道目标（连着的 / 选过的）就带上，只用来提示地址窗口。
        let target = iphoneTarget
        executionTask = worker("ipa",extra:ipaRequest(game, target:target, spaceOverride:spaceOverride)) { r in
            if let p = r["outputPath"] as? String { self.reveal(URL(fileURLWithPath:p)) }
            self.lastIPAPath = (r["ipa"] as? String).map { URL(fileURLWithPath:$0) } ?? (r["outputPath"] as? String).map { URL(fileURLWithPath:$0) }
            let done = Store.ipaDoneNotice()
            self.stickyNotice = done; self.notice = done
        }
    }
    // 装到 iPhone（10-26 维护者 定：Xcode 做主路、AltStore 备选）：用玩家在 Xcode 登录的 Apple ID 重签自签 IPA、数据线装上、拷资源包并逐文件核对（pack/jobs/install.py）
    func installToPhone(_ game: Game) {
        selection = "tasks"
        executionTask = worker("install",extra:["gameId":game.id].merging(downstreamScheme(game).map { ["scheme":$0] } ?? [:]) { a, _ in a }) { r in
            let device = (r["device"] as? String).map { "「\($0)」" } ?? ""   // 设备名夹在句中：按片段先翻译再拼（提示显示时 L() 整句查不到）
            // 资源传完但没拉起 App 解开（作业结果 unpackPending，11-09 #336）：不说「已推到 iPhone」，说还差的一步和原因 / 办法（后端原话）
            let done = (r["unpackPending"] as? String) ?? L("已推到 iPhone") + device + Store.packPushedClause(r["packFiles"] as? Int) + L("。打开游戏：第一次会先导入游戏资源（几分钟），保持在前台、别锁屏，导入完会自动进入游戏。免费 Apple ID 签的 App 7 天过期，到时连上手机点游戏库第一格「S2Arm」里的「续签 S2Arm」。")
            self.stickyNotice = done; self.notice = done
        }
    }
    /// 出 iPhone 包的请求：目标设备可有可无（有才带 target）
    func ipaRequest(_ game: Game, target: IPhoneTarget?, spaceOverride: Bool = false) -> [String:Any] {
        var r: [String:Any] = ["gameId":game.id]
        if let s = downstreamScheme(game) { r["scheme"] = s }   // 玩家在确认页选了下游深度方案
        if spaceOverride { r["spaceOverride"] = true }   // 空间预检拦下后玩家点了「仍然继续」
        if let t = target { r["target"] = t.request }
        return r
    }
    // 目标设备：连着一台就用它（自动、不用选）；没连或连着多台就用玩家上次选的机型 / 系统
    var iphoneTarget: IPhoneTarget? {
        let live = targetProbe.connected.filter { !$0.gpuArch.isEmpty }
        // 连着几台同架构、同系统大版本的设备 = 同一个目标，不用选
        if let first = live.first, live.allSatisfy({ $0.gpuArch == first.gpuArch && $0.os == first.os }) { return first }
        if let t = preferences.iphoneTarget, live.isEmpty || live.contains(where: { $0.gpuArch == t.gpuArch && $0.os == t.os }) { return t }
        return nil
    }
    // 「允许 AI 操作 S2Arm」：开 = 起本机控制口（ControlServer），关 = 停掉并删 control.json
    var aiControl: Bool {
        get { preferences.aiControl == true || ControlServer.forced }
        set { preferences.aiControl = newValue; savePreferences(); objectWillChange.send()
              if newValue { ControlServer.start(store:self) } else { ControlServer.stop(store:self) } }
    }
    // 给玩家复制给 AI 的提示词（11-02 维护者：不给某家 AI 单独开接入口，一段提示词粘给任何 AI 就能代人操作、解决出包各种问题）：
    //   操作手册（skill，内置在 App 里）路径 + 命令行工具路径 + 先问 / 停下 / 底线。跟着界面语言出中文或英文
    var aiHandoffText: String {
        let ctl = resources.appendingPathComponent("s2armctl").path
        let skill = resources.appendingPathComponent("skill/s2arm-operator/SKILL.md").path
        // 数据目录不是缺省位置（--data-root 启动）时，每条命令都要带上它，否则连不到这个 S2Arm
        let def = FileManager.default.urls(for:.applicationSupportDirectory,in:.userDomainMask)[0].appendingPathComponent("S2Arm").standardizedFileURL.path
        let root = privateRoot.standardizedFileURL.path == def ? "" : " --data-root '" + privateRoot.path + "'"
        let tool = "'" + ctl + "'" + root
        if Localization.language != "zh-Hans" {
            return """
            Please use S2Arm to turn my own Switch games into versions I can play on my iPhone / iPad (or on this Mac), and install them on my phone. S2Arm is open on this Mac, and I can watch every step you take in its window.
            1. First read the whole operator manual (skill): \(skill). It has the full workflow, how to handle every error, and the ground rules.
            2. Operate S2Arm with its command-line tool: \(tool). Run `\(tool) start` to connect, then keep running `\(tool) next` and do the next step it gives you.
            3. Ask me for anything only I have (where my prod.keys or game files are). For things only I can do (system permission prompts, passwords, signing in to my Apple ID in Xcode, tapping Trust or turning on Developer Mode on my iPhone), stop, tell me exactly what to do, and wait until I say it's done.
            4. Ground rules: work only on this Mac; never upload, copy elsewhere or print my keys, game files or resource bundles; ask me before deleting anything.
            """
        }
        return """
        请用 S2Arm 帮我把我自己的 Switch 游戏做成能在 iPhone / iPad（或这台 Mac）上玩的版本，并装到我的手机上。S2Arm 已经在这台 Mac 上打开，你做的每一步我都能在它的窗口里看到。
        1. 先完整读一遍操作手册（skill）：\(skill)。里面有完整流程、每种报错怎么处理、以及底线。
        2. 用它的命令行工具操作 S2Arm：\(tool)。先运行 `\(tool) start` 连上，再反复运行 `\(tool) next`，按它给的下一步去做。
        3. 只有我才有的东西（prod.keys、游戏文件放在哪）直接问我；只有我本人能做的事（点系统授权框、输密码、在 Xcode 里登录 Apple ID、在 iPhone 上点信任或打开开发者模式），停下来告诉我具体怎么做，等我说做完再继续。
        4. 底线：只在这台 Mac 上操作；不上传、不外传、不打印我的密钥、游戏文件和资源包；删除任何东西前先问我。
        """
    }
    /// 「复制给 AI 的提示词」：复制即同意让 AI 操作，顺手打开本机控制口
    func copyAIPrompt() {
        if !aiControl { aiControl = true }
        NSPasteboard.general.clearContents(); NSPasteboard.general.setString(aiHandoffText, forType:.string)
        notice = L("已复制。粘贴给你常用的 AI（Claude、ChatGPT、Codex 等）就行：它会一步步替你操作 S2Arm，遇到只能你本人做的事会停下来告诉你。")
    }
    func chooseTarget(_ t: IPhoneTarget) { preferences.iphoneTarget = t; savePreferences(); objectWillChange.send() }
    // 两步：先 quick（只给机型 / 系统静态表，不读设备，出包页立刻能选机型），完了再完整探测（devicectl 读连着的设备，探到再标出来、默认选上）。
    //   quick 那步保留上次探到的设备，不清空（10-26 维护者：机型表要等设备探测 61 秒）
    func probeTarget() {
        probeTarget(quick:true) { self.probeTarget(quick:false) }
    }
    private func probeTarget(quick: Bool, then: (() -> Void)? = nil) {
        worker("targetDevice",extra:quick ? ["quick":true] : [:],visible:false) { r in
            func list(_ k: String, connected: Bool) -> [IPhoneTarget] {
                ((r[k] as? [[String:Any]]) ?? []).map { IPhoneTarget(model:($0["model"] ?? $0["name"]) as? String ?? "",productType:$0["productType"] as? String ?? "",gpuArch:$0["gpuArch"] as? String ?? "",os:$0["os"] as? Int ?? 0,connected:connected,deviceName:connected ? $0["name"] as? String : nil) }
            }
            self.targetProbe = TargetProbe(connected:quick ? self.targetProbe.connected : list("connected",connected:true),models:list("models",connected:false),osChoices:(r["osChoices"] as? [Int]) ?? [])
            then?()
        }
    }
    // 拉回 iPhone / iPad 上 S2Arm 游戏 App 的诊断（pack/jobs/diag.py，devicectl）：拉完在访达里打开
    func pullDeviceDiagnostics() {
        worker("pullDiagnostics") { r in
            if let p = r["outputPath"] as? String { self.reveal(URL(fileURLWithPath:p)) }
            self.notice = "已拉回 \((r["apps"] as? [Any])?.count ?? 0) 个游戏的诊断，在访达里打开了。"
        }
    }
    func exportDiagnostics() {
        let p = NSSavePanel(); p.prompt = L("保存"); p.nameFieldStringValue = L("S2Arm-诊断.zip"); guard p.runModal() == .OK, let url = p.url else { return }
        worker("diagnostics",extra:["path":url.path]) { _ in self.reveal(url) }
    }
    func input(for game: Game?) -> InputConfig {
        if let game, let d = try? Data(contentsOf:libraryURL.appendingPathComponent("mappings/\(game.id).json")), let config = try? JSONDecoder().decode(InputConfig.self,from:d) { return config }
        return globalInput
    }
    func saveInput(_ config: InputConfig, for game: Game?) {
        guard config.conflicts.isEmpty, let d = try? JSONEncoder().encode(config), let obj = try? JSONSerialization.jsonObject(with:d) else { return }
        var extra: [String:Any] = ["config":obj]; if let game { extra["gameId"] = game.id }
        worker("saveInput",extra:extra,visible:false) { _ in
            if game == nil { self.globalInput = config }; self.notice = "映射已保存，下次启动游戏时生效，无需重新编译。"; self.editingInput = false
        }
    }
    func terminate() { processes.values.forEach { if $0.isRunning { $0.terminate() } } }
}

struct LibraryFormatProblem: Identifiable, Equatable { var id: String { path }; let path: String; let format: String }
