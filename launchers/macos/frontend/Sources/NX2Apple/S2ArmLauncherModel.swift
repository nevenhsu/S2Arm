// S2Arm（不是上游文件）：S2Arm × NX2Apple 界面的状态，接 S2ArmAppleAdapter 的 S2ArmEngineProvider（设计稿 2.13 EngineProvider）。
// 只持有界面要画的状态、把动作转给适配层；作业怎么起、请求长什么样都在适配层。
import AppKit
import Foundation
import Combine
import LauncherPadBridge
import NXCore
import NXModel
import S2ArmAppleAdapter
import SaveSync

/// 一次出包（勾了几个平台就几个作业，按页面顺序排队，ExportPlatform.picked）：现在做到第几个、每个平台的结局。
struct ExportRun: Equatable {
    struct Outcome: Equatable {
        var state: JobOutcome.State
        /// 没成时的原因（作业层原句，显示经 loc.backend）。
        var message = ""
        var code: String?
        /// Mac：运行文件目录；iPhone：IPA 与资源包所在文件夹。
        var output: String?
        /// iPhone：IPA 文件。
        var ipa: String?
        /// 成品大小（字节）：iPhone = IPA，Mac = 运行版本。作业层量的，量不出为 nil。
        var appBytes: Int64?
        /// iPhone 资源包大小（字节）。
        var packBytes: Int64?
        /// 作业层给的下一步建议（玩家话术）。
        var suggestion = ""
        /// 这个平台用了多久（秒）。
        var seconds: Double = 0
        /// 作业层给的失败事实（阶段、退出码、原文，英文标签）；没有为 nil。
        var failureSummary: String?
        /// 出成了、但有阶段降级继续（v0.3.1，产物根 degrade.*.tsv 的「玩家提示」）：完成卡逐条显示
        var notes: [String] = []
        /// 出包时自动配好的金手指 / 社区补丁（作业层 pack/jobs/patches.py boost：cheats / fps / widescreen / gfx 的条数）：完成页的 S 级加强动效
        var boost: [String: Int] = [:]
        /// 补丁打入结果（boost 里的 applied / failed / error，pack/jobs/patches.py record；完成页列打上 / 没打上，11-14）
        var sgrade = SGradeOutcome()
    }
    let platforms: [ExportPlatform]
    /// 正在做的那个平台（platforms 的下标）。
    var index = 0
    var outcomes: [ExportPlatform: Outcome] = [:]
    /// 整次出包、当前平台开始的时刻。
    let started: Date
    var platformStarted: Date
    var current: ExportPlatform { platforms[min(index, platforms.count - 1)] }
    var failed: [ExportPlatform] { platforms.filter { outcomes[$0].map { $0.state != .completed } ?? false } }

    init(platforms: [ExportPlatform], now: Date = Date()) {
        self.platforms = platforms; started = now; platformStarted = now
    }
}

/// 出包没全成（有平台失败）：作业页按平台写结局，整次记失败。
struct ExportIncomplete: Error {}

/// 出包页底栏「为什么还不能出包」。
enum ExportBlocker: Equatable {
    case noPlatform, game(String), keys, busy
    /// 这款已在出包或排队（同一款再点一次 / 控制口重发）：原先静默当成功、什么也不做，控制口回 ok 却没建任务
    case alreadyQueued
    /// 挡不挡开始：缺文件 / 缺 keys 只提示、不挡（v0.3.1 维护者「拿不准不停」）——这两项来自游戏库读数，可能是旧的（刚插上盘、刚导入 keys），
    ///   出包作业开头自己核（library.requirement_errors：找不到原件 input_missing、缺 prod.keys keys_missing），真缺才停、说清怎么办
    var stopsStart: Bool { switch self { case .game, .keys: return false; default: return true } }
}

/// 正在跑 / 刚结束的一个作业（一局、出包、准备运行、装到 iPhone、导入、一键处理环境、换游戏库）。主槽 / 手机槽 / 游戏槽同一时刻各一个，按款作业槽可以几个（见 Slot）。
struct JobSession: Equatable {
    enum Kind: String, CaseIterable { case play, export, prepare, install, installRemove, installRestore, installUpdate, importGames = "import", downloadMetal, downloadIosPlatform, installDotnet, setupLibrary = "setup",
                        keys, delete, diagnostics, moveGame, clearCache, installTV = "tvInstall", exportTV = "tvExport", pushSelected, phoneIdentity }
    enum Phase: Equatable { case running, stopping, finished(JobOutcome.State) }
    let kind: Kind
    let gameID: String?
    let name: String
    var phase: Phase = .running
    var stage = ""
    var message = ""
    /// 最近的作业事件（阶段 / 消息），最新在后。
    var log: [String] = []
    /// 第几步、本阶段计数、剩余时间（JobProgress，从作业事件来）。
    var progress = JobProgress()
    /// 做完可以在访达里显示的东西。
    var reveal: String?
    /// 出包：平台队列与各平台结局（别的作业为 nil）。
    var export: ExportRun?
    /// 出包前的磁盘预算（作业层 space 事件，check_pack_space）；没有为 nil。
    var space: PackSpace?
    /// 作业层给的失败码与下一步（JobFailure 的第一条诊断）：启动页按码显示一句人话 + 下一步，原文只进「复制错误摘要」
    var failureCode: String?
    var suggestion = ""
    /// 正在跑的那个作业层任务号（最近一个事件的 taskId；出包一个平台一个任务）。控制口 GET /state 的 executionTask 报它（原先恒为空，s2armctl next 判不出「本游戏正在运行」）
    var taskID: String?
    /// 开工 / 结束的时刻：手机作业结局页的「用时」
    var started = Date()
    var ended: Date?
    /// 走过的阶段（阶段事件的作业层原句，按出现顺序、不重复）：手机作业进行中页的步骤列表
    var stages: [String] = []
    /// 手机作业（装到手机 / 续签、删除、装回、更新手机上的 S2Arm）：结局页统一用 PhoneJobPanel
    /// 手机槽里整页显示的那几单：更新 iPhone 上的 S2Arm；「更新所选」没走成（#1756：原先这单的失败哪里都不显示，玩家只看到进度条跑完——整页写原因、给「重试」）
    var wholePage: Bool { kind == .installUpdate || (kind == .pushSelected && phase == .finished(.failed)) }
    var isPhoneJob: Bool { [.install, .installRemove, .installRestore, .installUpdate, .pushSelected, .phoneIdentity].contains(kind) }
}

/// 手机作业做完后结局页要说的事实（从作业结果读，不猜）：装到哪台、这次推了多少、哪几款被跳过
struct PhoneOutcome: Equatable {
    var device = ""
    var packFiles: Int? = nil
    var packBytes: Int64? = nil
    var skipped: [String] = []
    var version = ""
    /// 导出模式：放进的文件夹、几款、多大（PhoneJobPanel 成功页改写「导出好了」+ 打开文件夹）；一键装为空
    var exportFolder = ""
    var exportGames = 0
    var exportBytes: Int64? = nil
    /// 这次新写的 .img 与沿用的款数（#1132）：有沿用才在完成页点名「.img 只要拷这几个」
    var exportFresh: [String] = []
    var exportReused = 0
    /// 只新写点的那款时，这次没写 .img 的别的款（游戏名）：完成页点名、说怎么办
    var exportUnwritten: [String] = []
    /// 完成页「推到手机」（11-13）：推进手机上自签 S2Arm 的 .img 文件名（空 = 还没推）；推不成时作业给的一句「缺什么、怎么做」
    /// 这次补进 S 级优化的各款：打上 / 没打上（作业层结果 sgrade，pack/jobs/install.py；升级 / 装机 / 导出完成页列，11-14）
    var sgrade: [SGradeGameResult] = []
    /// 「更新 S2Arm」装完后这台手机上的 0.5.x 老款（作业结果 legacy：资源在、程序包不在，这台电脑能更新的）：成功页接着列出、默认全勾，点「更新这 N 款」走 pushSelected
    var legacy: [LegacyGame] = []
    /// 同上、但这台电脑的游戏库里没有它的 iPhone 包（legacyElsewhere：要回装它的那台电脑更新）：只写一行说明
    var legacyElsewhere: [String] = []
    /// 老款所在那台的设备记录键（作业结果 legacyDevice）：「更新这 N 款」推到它
    var legacyDevice = ""
    /// 独立 App（0.6.4）：成功页 A（直装）/ B（导出，app.exported）按它画；通用壳为 nil
    var app: StandaloneApp? = nil
}

/// 0.5.x 整包装上的老款（作业层 install.legacy_games）
struct LegacyGame: Equatable, Identifiable {
    let id: String
    let name: String
}

/// 排队的一次出包。
struct QueuedExport {
    let game: GameSummary
    let platforms: [ExportPlatform]
    let target: [String: Any]?
    let spaceOverride: Bool
    /// 存放位置（游戏库结构 v3）：nil = 这款现在的位置 / 上次用的位置（后端定）
    var location: String? = nil
    /// "export" = 这次出包是为了导出自签包（作业层步骤名照自签路写）
    var mode: String? = nil
    /// 安卓出好装到哪台（adb 序列号）：控制口显式给的；"" = 不装（控制口没给 serial）；nil = 界面上选的那台（androidPhone）
    var androidSerial: String? = nil
}

/// 这次出包要占多少、盘上还有多少（只取游戏库所在的卷）。
struct PackSpace: Equatable {
    let need: Int64?
    let free: Int64?
    let basis: String

    init?(_ value: Any?) {
        guard let v = value as? [String: Any], let volumes = v["volumes"] as? [[String: Any]],
              let volume = volumes.first(where: { $0["role"] as? String == "library" }) ?? volumes.first else { return nil }
        need = (volume["needBytes"] as? NSNumber)?.int64Value
        free = (volume["freeBytes"] as? NSNumber)?.int64Value
        basis = volume["basis"] as? String ?? ""
    }
}

@MainActor
public final class S2ArmLauncherModel: ObservableObject {
    let home = HomeLibraryState()
    let gamepad = LauncherPadNavigation()
    var playSessionActive: Bool { playBusy }
    @Published private(set) var games: [GameSummary] = []
    @Published private(set) var loading = false
    /// 游戏库读回来过（成功或失败）：之前库是空的只说明还没读完，不能显示「还没有游戏」。
    @Published private(set) var libraryLoaded = false
    @Published private(set) var libraryError: String?
    /// 库页上的列表是上次读到的快照（启动时先显示的，或这次刷新没成）：库页要标明，并在刷新失败时给重试（审计 P1-148）。
    @Published private(set) var showingSnapshot = false
    /// 刷新没成、库页仍显示上次的列表：这时的失败原因（没能刷新提示用；第一次就没读成、列表是空的走欢迎页的报错）。
    var staleLibraryError: String? { showingSnapshot && !games.isEmpty ? libraryError : nil }
    @Published var selectedID: String?
    /// 出包平台的勾选没能写盘。
    @Published private(set) var choiceError: String?
    @Published var session: JobSession? {   // 批量出包时编排作业一步换一款，+Batch.swift batchStepStarted 换会话
        didSet { if session?.started != oldValue?.started { sessionHidden = false } }
    }
    /// 手机作业（装到 iPhone / 导出自签包 / 从手机删 / 装回上一版 / 更新 iPhone 上的 S2Arm）单独一个槽（11-16 维护者「出包的时候没办法同时安装其他游戏到 iPhone 上，
    ///   这个控制纯属多余，要放开」）：出包、一局、导入等别的作业照跑，手机作业同时跑。手机槽同一时刻一个作业，忙时再点的排队（phoneQueue），
    ///   要装的那款正被主槽占着（出包 / 删除 / 清缓存…）也排队，主槽那单结束接着装
    @Published var phoneSession: JobSession? {
        didSet { if phoneSession?.started != oldValue?.started { phoneHidden = false } }
    }
    /// 「更新 iPhone 上的 S2Arm」的进度页收起了（回游戏库；顶栏留入口）
    @Published var phoneHidden = false
    /// 一局（launch 作业）单独一个游戏槽（作业层执行锁读写化：起局只拿执行锁共享 + 这款的按款锁，出包拿执行锁共享 + 出包锁 + 这款的按款锁）：
    ///   出包另一款时能开始玩、玩着时能给另一款出包；同一款边玩边出包、全库独占动作（删除 / 清缓存 / 导入 / 搬家…）与一局照旧互斥（canPlay / exportBlocker）
    /// 这一局已为「地址空间不够这款」弹过提示条（作业层 notice addr_short，宿主那条黄行；每局一次，开局时清）
    @Published var addrShortNoticed = false
    /// 这一局的界面语言（共享文案取词用；开局时记下）
    var playLanguage = "en"
    @Published var playSession: JobSession? {
        didSet { if playSession?.started != oldValue?.started { playHidden = false } }
    }
    /// 一局的页面收起了（回游戏库 / 去看别的作业页；顶栏留入口）
    @Published var playHidden = false
    /// 按款作业槽（维护者 11-16：出包、装机这类互斥「纯属多余」，只按真冲突互斥）：主槽在出包 / 准备某一款时，别的款的删除、搬家不排队，
    ///   在这里同时跑（每个一条，各自进度、各自收尾，见 sideKinds / gameJobHeld）。主槽空着时这两样照旧进主槽
    @Published private(set) var sideJobs: [SideJob] = []
    struct SideJob: Identifiable, Equatable { let id = UUID(); var session: JobSession }
    private var sideWork: [UUID: Task<Void, Never>] = [:]
    /// 手机槽排着的作业（按点的先后）：gameID = 它要用的那款（更新 iPhone 为 nil）；手机槽空、且那款不被主槽占着时起
    @Published var phoneQueue: [(gameID: String?, run: () -> Void)] = []
    /// 这款的手机作业排着队（手机槽前一个还没完 / 这款正在出包，出完接着装）：装机窗显示「已排队」，按钮不重复排
    func phoneQueued(_ gameID: String) -> Bool { phoneQueue.contains { $0.gameID == gameID } || installAfterQueue.contains(gameID) }
    /// 手机槽正在推这一款（装机作业在跑、装的就是它）
    func phonePushing(_ gameID: String) -> Bool { phoneBusy && phoneSession?.kind == .install && phoneSession?.gameID == gameID }
    /// 出包页开着的那一款（nil = 游戏库）。
    @Published var exportFor: String?
    /// 下游深度方案确认页（DownstreamConfirmSheet）：导出 iPhone 前弹，选完记下（downstreamChoice 作业）再接着导出这一单
    @Published var downstreamPromptFor: String?
    /// 上次开着金手指 / 画面补丁时这款异常退出、已被关掉（作业层 pack/jobs/cheats.py note_crash / crashed）：开始游戏时通知一句（LaunchSettingsSheet.swift CrashPromptHost）
    @Published var crashPrompt: CrashPrompt?
    /// 开始前问「要回到上一份存档吗」（上一局读存档出事、且有上一份，S2ArmLauncherModel+SavePrevious）；不拦，玩家选了再开局
    @Published var previousSavePrompt: PreviousSavePrompt?
    /// 刚回到上一份存档的那一款：提示一次「可以随时换回来」
    @Published var previousSaveSwitched: String?
    var downstreamPending: (platforms: [ExportPlatform]?, deviceTarget: [String: Any]?, spaceOverride: Bool, location: String?)?
    /// 存放位置（游戏库结构 v3）：出包页下拉框的各项、上次用的位置号
    @Published var locations: [StorageLocation] = []
    @Published var lastLocation = ""
    /// 「选择其他文件夹…」选的路径所在盘会不会让解包多占数倍空间（#444；已登记的位置看 StorageLocation.slackWarning）
    @Published var probedSlack: [String: LocationSlack?] = [:]
    /// 出包页记住的平台勾选（平台 id）。
    @Published private(set) var exportChoice: [String] = []
    /// 动作没能开始的原因（缺东西、没选游戏库等），显示在主按钮下面。只属于出错的那一款（导入等不针对某款的为 nil）：
    /// 换选别的游戏不显示上一款的报错（11-07 界面验收：丝之歌缺原件的红字挂到了 Hades 下面）。
    @Published private(set) var actionError: String?
    private(set) var actionErrorGameID: String?
    func actionError(for gameID: String?) -> String? { actionErrorGameID == nil || actionErrorGameID == gameID ? actionError : nil }
    func failAction(_ text: String?, game: String?) { actionErrorGameID = game; actionError = text }
    // 环境检查
    @Published private(set) var checks: [ToolCheck] = []
    @Published private(set) var checking = false
    @Published private(set) var environmentError: String?
    @Published var showEnvironment = false
    /// 装到 iPhone 的方式（环境检查「装到 iPhone 的方式」卡，installMode 作业）与壳比本版旧的手机（catalog phoneUpdates，库页右上角按钮）
    @Published var installMode = InstallModeInfo()
    /// 选了独立 App 出法、又不是用自己的证书签（Apple ID 可能是免费的）时作业层给的苹果限制提示（layoutSet 的 freeLimit，只提示不拦）；换选时清空
    @Published var layoutNotice = ""
    /// 「建议更新」（0.6.0 通用壳开关后面，S2ArmLauncherModel+UpdateHints.swift）：游戏号（大写）→ 对不上的原因；开关关着恒为空
    @Published var updateHints: [String: [String]] = [:]
    /// 「更新这款」正在做 / 刚做完的款（游戏号大写 → 进度、没做齐的几项、失败原因；S2ArmLauncherModel+UpdateHints）：详情区按钮当场变进度，做完在按钮下列黄字
    @Published var gameUpdates: [String: GameUpdateRun] = [:]
    /// 导出确认页开着哪一款（S2ArmLauncherModel+ExportSheet）；preselect = 续签时预勾的那一台
    @Published var exportSheet: ExportSheetRequest?
    /// 每款这次导出的进度：逐台导入的每台结果 / 导出到文件夹
    @Published var exportFlows: [String: ExportFlow] = [:]
    /// 导出确认页的设备列表（phoneOverview probe 现查）；nil = 还在查
    @Published var exportPhones: [PhoneOverview.Phone]?
    /// 这一单「更新 iPhone 上的 S2Arm」其实是第一次装（那台原来没有 S2Arm）：结局页写「安装」并给下一步
    @Published var shellFirstInstall = false
    /// iPhone 总控台（0.6.0 通用壳开关后面，S2ArmLauncherModel+PhoneConsole.swift）：nil = 开关关着，首页不出首位卡
    @Published var phoneOverview: PhoneOverview?
    @Published var consoleDevice: String?            // 总控台在管哪台（多台时顶部切换）；nil = 跟着默认那台
    @Published var consoleSelection: Set<String> = []   // ② 游戏同步里勾上的款（游戏号大写）
    var lastPushSelectedDevice: String?                   // 上一单「更新所选」推的那台（失败页「重试」续推它，#1756）
    @Published var consolePushing: String?               // 「更新所选」正在推的那款（作业 pushSelected 的 progress 事件 current）
    @Published var identityDelivered: Bool?              // 「把签名身份交给手机」最近一次成功的结果（作业 phoneIdentity 的 delivered）；nil = 还没成功过 / 又点了一次
    var consoleSeeded: Set<String> = []                  // 上次自动勾过的建议更新款（玩家取消的不重勾）
    @Published var consoleChoosing: String?              // 「选用哪个 S2Arm」正在记的那个包名（作业 phoneChooseApp 在跑）；nil = 没在选
    @Published var consoleChooseError: String?           // 上一次选没选成的原因（作业层原句，界面按当前语言翻）；nil = 没出错
    @Published var consoleRechoose = false               // 选过了、玩家点了「换一个 S2Arm」：重新列出候选
    @Published var pushChoice: PushChoice?               // 推送停在「装着不止一个 S2Arm、还没选」：失败页就地列的候选与选好后重推的那一次；nil = 没有
    @Published var dynamicDone = false    // 顶栏动态窗口：手机作业刚做完，闪一下 ✓（2 秒后回普通图标，S2ArmLauncherModel+PhoneConsole）
    @Published var dynamicAlert = false   // 手机作业失败 / 停下：橙色「!」一直留着，直到玩家点开看过
    @Published var phoneUpdates: [PhoneUpdate] = []
    @Published var phoneDevices = 0   // catalog phoneDevices：这台电脑往几台手机装过游戏（更新弹窗按钮写「更新启动器和 iPhone App」）
    /// 点了更新、有要重新出包的款：三选一弹框；选「更新并重新出这几款」时更新完把它们排进批量出包（只出 iPhone），批次出完逐款装上
    @Published var updateConfirm: PhoneUpdate?
    /// 「S 级优化升级」页开着（v0.4.0；SGradeUpgradeView）
    @Published var sgradeUpgradeOpen = false
    var updateRepack: [String] = []
    var batchInstallAfter = false
    var installAfterQueue: [String] = []
    var installAfterDevice: String?   // 「更新并重新出」出完逐款回装装到哪台：发起更新时那台（#1946：原先不带，多台在线时报 device_required 或装到界面选中的另一台）
    /// 导出确认页点了「开始」、这款还没做好 iPhone 版（或是旧格式）：先出 iPhone 包，出完按这份计划接着导出 / 逐台导入（S2ArmLauncherModel+UpdateExport）
    var exportAfterPack: [String: ExportPlan] = [:]
    /// 这次装机会话是导出自签包（进度 / 结局页照导出写；完成后看结局里的导出文件夹）
    @Published var selfSigning = false
    /// 添加游戏 / 放密钥弹窗（ImportSheet）：这两个作业的进度和失败原因都留在弹窗里，不换页。
    @Published var updateState: UpdateState = .idle
    @Published var notice: [String: Any]?   // 顶部公告条：最新一条没看过的重要公告（S2ArmLauncherModel+Update.swift loadInbox）；nil = 不显示
    /// 打开时最多一个弹窗：{kind: update | afterUpdate, version, notes?}（11-13 维护者：永远只给最新一期，看过不再弹）
    @Published var updatePopup: [String: Any]?
    /// 「消息」列表（公告最新在前 + 更新说明）与未读数（只算最新那条公告）
    @Published var messages: [[String: Any]] = []
    @Published var unreadMessages = 0
    /// 有新版时的那一版 {version, notes?}：顶栏「有新版 vX」点开回到弹窗
    @Published var availableUpdate: [String: Any]?
    /// 主动重启会中断进行中的工作时，等待玩家确认。
    @Published var updateGate: String?
    static let autoRestartWhenReadyKey = "update.autoRestartWhenReady"
    @Published var autoRestartWhenReady: Bool {
        didSet { updateDefaults.set(autoRestartWhenReady, forKey: Self.autoRestartWhenReadyKey) }
    }
    @Published var isCheckingForUpdates = false
    @Published var checkError: String?
    @Published var updateRestarting = false
    let updateDefaults: UserDefaults
    let updateSimulationEnabled: Bool
    var updateCheckTask: Task<Void, Never>?
    var didStartManualUpdateCheck = false
    private var didStartLauncher = false
    /// 更新失败时 Sparkle 给的原因（界面在错误码文案之外照原话显示，控制口 /state 的 update.reason）
    @Published var updateFailure: String?
    /// 自动检查更新（默认开）：开启后启动时静默向服务端确认是否有新版本
    static let autoCheckUpdatesOffKey = "S2AAutoCheckUpdatesOff"
    @Published var autoCheckUpdates: Bool {
        didSet { updateDefaults.set(!autoCheckUpdates, forKey: Self.autoCheckUpdatesOffKey) }
    }
    /// 免费签名到期提醒（缺省开；维护者 11-20：玩家换成自签或别的方式后提醒挂着取消不了）：游戏库顶部横幅点「×」即关、不再显示，设置里重新打开恢复。
    ///   关着时不显示设备操作区里的「还剩几天 / 已到期」（freeSignDaysLeft）。存的是「关」，同 autoCheckUpdatesOffKey
    static let freeSignReminderOffKey = "S2AFreeSignReminderOff"
    @Published var freeSignReminder: Bool {
        didSet { updateDefaults.set(!freeSignReminder, forKey: Self.freeSignReminderOffKey) }
    }
    /// 这款要显示的免费签名剩余天数：提醒关着或不是启动器免费签的那份 → nil
    func freeSignDaysLeft(_ game: GameSummary, now: Date = Date()) -> Int? { freeSignReminder ? game.iphoneDaysLeft(now: now) : nil }
    /// 自动下载更新（默认关）：仅在「自动检查更新」也开启时生效；开启后后台下载，关闭则由玩家自己选何时下载
    static let autoDownloadUpdatesKey = "S2AAutoDownloadUpdates"
    @Published var autoDownloadUpdates: Bool {
        didSet { updateDefaults.set(autoDownloadUpdates, forKey: Self.autoDownloadUpdatesKey) }
    }
    lazy var updateDriver: any LauncherUpdateDriver = {
        if updateSimulationEnabled { return SimulatedUpdateDriver(model: self, defaults: updateDefaults) }
        return SparkleUpdate(model: self)
    }()
    @Published var showAddGames = false
    @Published var showKeys = false
    /// 选好了但缺密钥（作业层 keys_missing）：等放入密钥后接着导入。解包好的游戏不要密钥，不经过这里。
    @Published private(set) var pendingImport: [URL] = []
    /// 留在原页显示的作业没成的原因（换作业、关弹窗时清掉）。
    @Published private(set) var inlineFailure: (kind: JobSession.Kind, message: String, suggestion: String)?
    /// 最近一次手机作业的结局事实（PhoneJobPanel 成功页用）；每个手机作业开工时清空
    @Published var phoneOutcome: PhoneOutcome?
    /// 独立 App 装上了、资源没推完就失败（作业失败结果 appInstalled）：失败页的特例标题与说明
    @Published var phoneAppPartial: StandaloneApp?
    /// 单款装机 / 导出失败页的「重试」：上一次 install(...) 原样再来
    var lastInstallRetry: (() -> Void)?
    /// 最近一次结束的手机作业（装机页的结局区读它：作业结束后 session 可能已被别的作业替换）
    @Published private(set) var lastPhoneSession: JobSession?
    /// 欢迎页上排着队的一键下载（作业同一时刻只有一个）。
    @Published private(set) var repairQueue: [ToolCheck] = []
    /// 新手流程刚导入的那款：欢迎页留着，可进入游戏库或接着一键出包。
    @Published private(set) var welcomeGameID: String?
    /// 这份密钥读不了选的游戏：放密钥弹窗提示换一份。
    @Published private(set) var keysRejected = false
    /// 刚装好的那款（装机弹窗显示完成页）。
    @Published var installDone: String?
    /// 刚装完的那次是单一 IMG：成功页多写一句「打开 S2Arm 会自动完成安装」
    @Published var installOpenToFinish = false
    @Published var installTrustKind = ""   // 这次装上的那台的类型（装机 / 更新结果 deviceKind）：「在 iPad 上：设置 → …信任」那句按它写（#1962）
    @Published var installTrustName = ""   // 这次签名的证书名（装机 / 更新结果 trustName）：完成页「点「<名字>」→ 信任」；空 = 退回不带名字的说法
    /// 最近一次手机作业（装到 iPhone / 从手机删 / 装回上一版）的结局，玩家还没回去看：顶部胶囊「已装好 · 查看结果」靠它（#308：关掉装机弹窗后找不回结果）。
    /// 新的手机作业开始、或打开这款的装机弹窗时清掉。
    @Published var phoneResult: (gameID: String, name: String, ok: Bool)?
    /// 控制口 /show install：打开这款的装机窗（同点顶栏「查看结果」/ 游戏的「推到 iPhone」）；界面接到后清空
    @Published var showInstallFor: String?
    static let phoneKinds: Set<JobSession.Kind> = [.install, .installRemove, .installRestore]
    /// 连着的手机、可选机型（出包页用）；玩家选的机型（productType）。
    @Published private(set) var connectedDevices: [[String: Any]] = []
    @Published private(set) var targetModels: [[String: Any]] = []
    @Published private(set) var targetOS: [Int] = []
    @Published var chosenModel: String?
    /// 连着多台 iPhone / iPad 时玩家在出包页选的那台（设备号，记住到下次；#272）。按机型优化和装到设备都用它
    @Published var chosenDevice: String? = UserDefaults.standard.string(forKey: S2ArmLauncherModel.exportDeviceKey) {
        didSet { UserDefaults.standard.set(chosenDevice, forKey: Self.exportDeviceKey) }
    }
    static let exportDeviceKey = "S2AExportDevice"
    /// 排队等着出的；运行页被收起（后台继续）；本次打开以来每款最近一次的出包结果；正在查看的结果。
    @Published private(set) var exportQueue: [QueuedExport] = []
    /// 批量出包（v0.2.4，S2ArmLauncherModel+Batch.swift）：清单页开着；出包前每款的勾选行；一批共用的存放位置；
    ///   后端批次（pack/jobs/batch.py show 的结果，出包中途关掉启动器也在）；这批是不是正在一款接一款地出；放进来但认不出的
    @Published var batchShown = false
    @Published var batchDraft: [BatchRow] = []
    @Published var batchLocation = ""
    @Published var batchState: [String: Any]? { didSet { pumpQueuedDeletes() } }   // 这批里排队删的款出完了：接着删
    @Published var batchRunning = false { didSet { pumpQueuedDeletes() } }
    /// 正在跑的那个批量出包编排作业（取消 = 取消它，剃刀 H13）
    var batchWork: Task<Void, Never>?
    /// 批次记录（batch.py mark / 命令）没写成的原因：整批停住、批量页显示（剃刀 M7：原先失败照样往下走，状态和真实结局分叉）；nil = 没出错
    @Published var batchError: String?
    @Published var batchUnrecognized: [ImportSource] = []
    /// 在单款出包页又放进游戏：导入完列清单时这款排第一
    var batchSeed: String?
    @Published var sessionHidden = false
    @Published private(set) var finishedExports: [String: JobSession] = [:]
    @Published var viewingResult: JobSession?
    var showsWelcome: Bool { games.isEmpty || welcomeGameID != nil || welcomeHome }
    /// 点了顶栏品牌名：游戏库页先显示欢迎页（社区贡献者补丁 #2；点「游戏库」回到列表）
    @Published var welcomeHome = false
    /// 设置页「磁盘空间」：关掉设置、给这款打开磁盘空间面板（LauncherView 收到就开）
    @Published var spaceRequest: String?
    /// 控制口 /show page = launchSettings：打开这款的启动设置（同点游戏库齿轮），LauncherView 接住后清掉
    @Published var displayRequest: String?
    var welcomeGame: GameSummary? { games.first { $0.id == welcomeGameID } }
    // 装到 iPhone 前的清单
    @Published private(set) var installReadiness: InstallReadiness?
    /// 开发者证书（v0.2.4，S2ArmLauncherModel+DevCert.swift）：现况与体检；导入弹窗开着没有
    @Published var devCert: DevCertStatus?
    var failingReads: Set<String> = []   // 被动刷新正在连续失败的那几类（reportFailure 只报头一次）
    @Published var showDevCertImport = false
    /// 几张证书（#487）：这台第一次连上（证书签名、还没给它选过）时问用哪张；本次启动每台只问一次
    @Published var devCertChoice: DevCertChoice?
    var devCertAsked: Set<String> = []
    /// Apple ID 登录（0.6.0 Mac，S2ArmLauncherModel+AppleLogin.swift）：当前账号、登录面板挂在哪一层（install / settings / main，nil = 没开）与进行到哪
    @Published var appleAccount: AppleAccount?
    @Published var appleLoginHost: String?
    @Published var appleLoginPhase: AppleLoginPhase = .idle
    /// 为什么开的登录面板：expired = 装机报 Apple ID 登录过期（apple_session_expired），面板说一句「过期了，重新登录」并带上原来的邮箱
    @Published var appleLoginReason: String?
    /// 苹果刚拒收过登录（5xx），作业层在等冷却：还要等几秒（作业层 appleLogin.wait 事件每秒报一次；nil = 没在等）
    @Published var appleLoginWait: Int?
    var appleLoginTask: Task<Void, Never>?
    /// 装机清单派发出的动作（performInstallAction）：在装机窗里弹的说明（trustHelp / developerModeHelp）、「装到哪台」那一行要不要亮出来、
    ///   设置页要定位到哪一栏（devCertSettings）、要离开装机窗去的地方（装机窗收到后先关自己再去）；开着的装机窗是哪款
    @Published var installHelp: String?
    @Published var pickDeviceFocus = false
    @Published var settingsFocus: String?
    @Published var installLeave: InstallLeave?
    var installViewOpen: String?
    @Published private(set) var customizing = false
    /// 非会话作业（换封面 / 改名、释放空间、存档）在等库锁时作业层的原句「在等「…」结束…」（phase=waiting_lock）；拿到锁或做完清空。
    /// 剃刀 M3：原先这几处只转圈，别的任务占着库时最长等 120 秒，玩家不知道在等什么
    @Published var lockWaitNote: String?

    /// 交给作业的事件去处：只认等锁进度，其余丢弃
    func lockWaitSink() -> EventSink {
        EventSink { [weak self] e in
            guard e.ev == "progress" else { return }
            let note = e.fields["phase"] as? String == "waiting_lock" ? e.message : nil
            Task { @MainActor in self?.lockWaitNote = note }
        }
    }
    @Published private(set) var customizeError: String?
    @Published private(set) var installChecking = false
    /// 当前这份装机前清单是给哪台设备查的（控制口 GET /state 的 installReadiness.checkedDevice）：换了设备先清掉旧清单，
    ///   s2armctl install-check --device 等到这台的结果才读（11-11 真机验收：连查两台，第二台读到了第一台的清单）
    @Published private(set) var installReadinessDevice: String?
    /// 正在查时又来了另一台（或另一款）的请求：记下，这次查完接着查它（原先直接丢掉）
    private var pendingInstallCheck: (game: String, device: String?)?
    @Published private(set) var installError: String?
    private(set) var installGameID: String?
    // 释放空间（spaceSurvey / spaceFree，FreeSpaceView）：只看一款
    @Published private(set) var space: GameSpace?
    @Published private(set) var spaceLoading = false
    @Published private(set) var spaceFreeing = false
    @Published private(set) var spaceError: String?
    /// 上一次释放了多少（字节）；换一款或重新打开时清掉。
    @Published private(set) var spaceFreed: Int64?
    private var spaceGameID: String?
    /// 游戏库页顶部一条操作提示（删除游戏 / 清除缓存）：点下就有「正在…」，结束换成「已…」或失败原因；有别的作业在跑时说清楚在等什么
    struct LibraryNotice: Equatable { enum Kind { case running, done, failed, queued }; let kind: Kind; let key: String; let name: String; var detail: String = ""; var gameID: String? = nil }
    @Published var libraryNotice: LibraryNotice?
    /// 读不到设备列表的原因（targetDevice 的 connectedError / 探测出错）：出包页设备那一行显示它，而不是「把 iPhone 插上」
    @Published var deviceProbeError: String?
    // 存储总览（storageOverview，FreeSpaceView 的「总览」）
    @Published private(set) var overview: StorageOverview?
    @Published private(set) var overviewLoading = false
    @Published private(set) var overviewError: String?

    let provider: S2ArmEngineProvider
    let controller: NS2LauncherController
    let xboxGipSocketURL: URL
    private var xboxGipRelayStarted = false
    /// 游戏槽那一局（provider.run 起的，停局走 provider.control）
    private var running: RunningGame?
    /// iCloud 存档同步进行中（开局前 / 退出后）：不重复开局
    @Published var cloudSyncing = false
    private var work: Task<Void, Never>?
    private var phoneWork: Task<Void, Never>?
    private var playWork: Task<Void, Never>?
    private var refreshWaiters: [() -> Void] = []
    /// 正在列时又要刷新（导入完、出包完、切回窗口……）：记一笔，这一轮列完再补列一次；这期间挂的回调归补的那一轮（剃刀 M8：原先直接丢掉，
    /// 刚导入的游戏要等下一次别的刷新才出现）。nil = 没有要补的
    private var refreshAgain: [() -> Void]?
    private var startupCatalogSent = false
    /// 启动后的后台循环（自动拉回 / 首启准备 / Apple TV 备份 / Mac S 级）是否已开：按「第一次读库成功」开，不跟 startup 那一次读绑死——
    ///   那一次读失败（首次使用还没选游戏库、外置游戏库没挂上）时原先整个启动器开着期间都不开（11-16 第 3 份候选 Mac 整链：等 6 分钟一单 phoneKeptPull 都没发）
    private(set) var startupLoopsStarted = false
    private var controllerGameStarted = false
    /// 导入后排着等准备的游戏（同旧启动器：放进来就自动准备，维护者 10-23 不让玩家选）。
    private static let logLimit = 200

    public convenience init(client: JobClient) {
        self.init(client: client, updateDefaults: .standard)
    }

    init(client: JobClient, updateDefaults: UserDefaults) {
        self.updateDefaults = updateDefaults
        updateSimulationEnabled = SimulatedUpdateDriver.isAllowed(preference: updateDefaults.bool(forKey: SimulatedUpdateDriver.enabledKey),
                                                                  info: Bundle.main.infoDictionary)
        autoRestartWhenReady = updateDefaults.bool(forKey: Self.autoRestartWhenReadyKey)
        autoCheckUpdates = !updateDefaults.bool(forKey: Self.autoCheckUpdatesOffKey)
        autoDownloadUpdates = updateDefaults.bool(forKey: Self.autoDownloadUpdatesKey)
        freeSignReminder = !updateDefaults.bool(forKey: Self.freeSignReminderOffKey)
        provider = S2ArmEngineProvider(client: client)
        controller = NS2LauncherController(
            socketURL: provider.client.environment.privateRoot.appendingPathComponent("ns2-relay.sock"), watchGame: true)   // 游戏的手柄设置画面交给面板分配玩家（#520 E）
        let preferredXboxSocket = provider.client.environment.privateRoot.appendingPathComponent("xbox-gip-relay.sock")
        xboxGipSocketURL = preferredXboxSocket.path.utf8.count < 100
            ? preferredXboxSocket
            : URL(fileURLWithPath: "/tmp/s2arm-xbox-gip-\(getuid()).sock")
        exportChoice = provider.settings.exportTargets
        // 启动首帧即先载入上次列表快照，避免先闪出加载占位页再切换成首页卡片排
        if let known = provider.library.lastKnownGames(), !known.isEmpty {
            games = known
            selectedID = known.first?.id
            libraryLoaded = true
            showingSnapshot = true
        }
    }

    var selected: GameSummary? { games.first { $0.id == selectedID } ?? games.first }
    /// 有作业在跑（一局、准备、出包、装机……，三个槽任一）：没放开的动作照旧先不让点。
    var busy: Bool { mainBusy || sideBusy || phoneBusy || playBusy }
    /// 主槽（出包、导入、删除、清缓存…）有作业在跑；一局在游戏槽，不算
    var mainBusy: Bool { if let session, case .finished = session.phase { return false }; return session != nil }
    /// 按款作业槽有作业在跑
    var sideBusy: Bool { sideJobs.contains { !isFinished($0.session) } }
    /// 主槽与按款作业槽里在跑的作业（「这款 / 整库被占着」都从这里判）
    var mainJobs: [JobSession] { (mainBusy ? [session!] : []) + sideJobs.map(\.session).filter { !isFinished($0) } }
    /// 能和别的款的出包 / 准备同时跑的作业（进按款作业槽）。后端（#2191）删除、搬家只拿执行锁共享 + 这一款的按款锁 + 清单锁，
    ///   出包 / 准备拿执行锁共享 + 出包锁 + 那一款的按款锁：两边只在同一款的按款锁上相撞（同 Windows JobSlots.Blocks 出包只锁正在出的那款）
    static let sideKinds: Set<JobSession.Kind> = [.delete, .moveGame]
    /// 这款现在能不能开始删除 / 搬家（各入口与右键菜单按它判，原先任何作业在跑都不行）。留下的互斥各对应一处真冲突：
    ///   手机槽 / 游戏槽有作业——照旧不放开（这两槽的口径不在这次改动里）；
    ///   主槽或按款作业槽在动这一款——出包 / 准备整段拿这款的按款锁（build.produce_locks），删除 / 搬家同样要它；
    ///   主槽或按款作业槽在做别的事（删除 / 搬家别的款、清缓存、导入、换库、放密钥、环境下载…）——删除 / 搬家整段拿清单锁（.catalog.lock，
    ///     library.delete / move_game），清缓存 / 放密钥 / 换库拿执行锁独占，导入、换库不知道碰哪款：同 Windows 游戏库类作业对所有款都挡；
    ///   这款排着出包、或在正出的这批里还没出完——出包轮到它时要读它的文件夹（同一款不边删边出）
    func gameJobHeld(_ gameID: String) -> Bool {
        if phoneBusy || playBusy { return true }
        if mainJobs.contains(where: { !(Self.packKinds.contains($0.kind) && $0.gameID != nil && $0.gameID != gameID) }) { return true }
        return exportQueue.contains { $0.game.id == gameID } || (batchRunning && batchPendingIDs.contains(gameID))
    }
    static let packKinds: Set<JobSession.Kind> = [.export, .prepare]
    /// 这款正在出包 / 准备（主槽或按款作业槽），或排着出包、或在正出的这批里还没出完：此时删它排队等出完（不阻断玩家主动操作）
    func packHolds(_ gameID: String) -> Bool {
        mainJobs.contains { Self.packKinds.contains($0.kind) && $0.gameID == gameID }
            || exportQueue.contains { $0.game.id == gameID } || (batchRunning && batchPendingIDs.contains(gameID))
    }
    /// 手机槽有作业在跑
    var phoneBusy: Bool { if let phoneSession, case .finished = phoneSession.phase { return false }; return phoneSession != nil }
    /// 游戏槽有一局在跑（开局到退出，含正在停）
    var playBusy: Bool { if let playSession, case .finished = playSession.phase { return false }; return playSession != nil }
    /// 一局的页面盖在最上面（有一局的会话、没收起）：主槽 / 手机槽的作业页被它压着
    var playVisible: Bool { playSession != nil && !playHidden }
    /// 忙的时候在等的那个作业（主槽优先，其次一局、手机作业）：置灰按钮的说明、控制口的忙回复说它
    var busySession: JobSession? { mainJobs.first ?? (playBusy ? playSession : phoneBusy ? phoneSession : nil) }
    /// 这款正在玩（游戏槽一局还没结束）
    func playHolds(_ gameID: String?) -> Bool { guard let gameID, playBusy else { return false }; return playSession?.gameID == gameID }
    /// 这款的启动设置（金手指 / mod / 补丁 / 设置项，作业层 communitySave、sgradeReenable）现在能不能改（#2191，维护者 11-16：互斥只在真有冲突时）。
    /// 那两个作业不拿锁、只写这款文件夹里的 cheats/、mods/、launch.json（全局设置 app.json 原子写），只和动这款文件的作业冲突：
    ///   主槽在动这一款——出包 / 准备读它的 cheats/、mods/、launch.json（patches.prepare → cheats.fetch / mods.code_index），删除 / 搬家 / 清缓存挪它的文件夹；
    ///   主槽在导入或换游戏库（会话不带游戏号）——导入可能重写这款的文件夹、换库整库搬家，不知道碰哪款，按碰到算；
    ///   这款正在玩——收局时写它的 launch.json / 金手指记录（build.launch → cheats.note_good / note_crash），同时改会丢一边；
    ///   手机槽在推这一款（或推所选几款，会话不带游戏号）——推送读它的 mods/（install.push_mods）。
    /// 主槽在出别的款、做与这款无关的事：照常能改（原先任何作业在跑都灰）。按款作业槽（别的款的删除 / 搬家）同主槽判
    func gameFilesHeld(_ gameID: String) -> Bool {
        if mainJobs.contains(where: { $0.gameID == gameID || ($0.gameID == nil && ($0.kind == .importGames || $0.kind == .setupLibrary)) }) { return true }
        if playHolds(gameID) { return true }
        if phoneBusy, let s = phoneSession, s.gameID == gameID || (s.gameID == nil && s.kind == .pushSelected) { return true }
        return false
    }
    /// 这款正被主槽或游戏槽占着（正在出包 / 删除 / 清缓存 / 玩…）：对它的手机作业要等那一单结束
    func mainHolds(_ gameID: String?) -> Bool { guard let gameID else { return false }; return mainJobs.contains { $0.gameID == gameID } || playHolds(gameID) }
    /// 开局判据：能不能现在开始玩这款。游戏槽空着、不在同步 iCloud 存档；主槽在出别的款（出包 / 准备，执行锁共享）时照样能玩，
    ///   在出这款、或在做全库独占动作（删除 / 清缓存 / 导入 / 放密钥 / 搬家 / 换游戏库 / 环境下载…）时不行（作业层排锁到上限后报忙）；
    ///   这款正在装到手机、排着队等出包、在正出的这批里还没出完，也不行（同一款不边玩边出包）
    func canPlay(_ gameID: String) -> Bool {
        if playBusy || cloudSyncing { return false }
        if mainJobs.contains(where: { !(Self.packKinds.contains($0.kind) && $0.gameID != gameID) }) { return false }   // 按款作业槽（删除 / 搬家）同主槽：全库独占，不开局
        if phoneHolds(gameID) || exportQueue.contains(where: { $0.game.id == gameID }) { return false }
        return !(batchRunning && batchPendingIDs.contains(gameID))
    }
    /// 手机槽空出来 / 主槽或游戏槽一单结束：起排着的第一个能起的（那款不被主槽 / 游戏槽占着）
    func pumpPhone() {
        guard !phoneBusy, dropConfirm == nil, let i = phoneQueue.firstIndex(where: { !mainHolds($0.gameID) }) else { return }
        phoneQueue.remove(at: i).run()
    }

    /// 启动时（启动器剃刀 M6）：碰游戏库 / 私有目录的后端作业一个接一个起——先列游戏库，再恢复批次，再查环境。
    /// 原先 onAppear 一次并发起这几个作业，每个作业开头都做迁移 / 收日志：两个作业同时收私有目录日志、老库搬家时旧库目录被建回来（坑记录）。
    /// 不碰游戏库的（控制口、更新、公告、手柄中继）照常一起起。
    func startup() {
        guard !didStartLauncher else { return }
        didStartLauncher = true
        startControl(); finishPendingUpdate(); loadInbox(); startControllerConnection()
        refreshInstallMode(silent: true)   // 装法启动时就读（原先只在设置页 / 装机页出现时读）：控制口 /install 按它判断导出模式要不要 device，界面顶栏也按它
        refresh { [weak self] in self?.recoverBatch { self?.checkEnvironment() } }
    }

    /// done：这次列完（成功或失败）后调；已经在列时不再起新作业，等那一次列完再调
    /// 「自动更新 iPhone 上的 S2Arm」开关（缺省开；存的是「关」，玩家没碰过就是开）：启动器升级后手机连上、没在玩就自动换新版 App
    static let autoUpdatePhoneOffKey = "S2AAutoUpdatePhoneOff"
    static func autoUpdatePhoneOn(_ defaults: UserDefaults = .standard) -> Bool { !defaults.bool(forKey: autoUpdatePhoneOffKey) }
    static func setAutoUpdatePhone(_ on: Bool, _ defaults: UserDefaults = .standard) { defaults.set(!on, forKey: autoUpdatePhoneOffKey) }

    /// 手机插着、启动器开着时（#8be819）：每 2 分钟把手机上宿主留下的代码模块（游戏运行中才取到的 NRO）拉回游戏库（phoneKeptPull 作业），
    ///   有还没补进 iPhone 件的就自动发起「更新 iPhone」：只改程序、只推程序部分，玩家不用判断要重出什么。同一批只自动发起一次，
    ///   没成（要确认 / 失败）就留给游戏库右上角的「更新 iPhone」按钮（phoneUpdates 列着这台）。有局 / 任务在跑、装法是导出时不发起
    func keptModulesLoop() {
        guard !keptLoopStarted else { return }
        keptLoopStarted = true
        Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 120 * 1_000_000_000)
                guard let self else { return }
                await self.keptModulesTick()
            }
        }
    }

    /// keptModulesLoop 的一次（单测直接调）：→ 这次有没有发起「更新 iPhone」
    @discardableResult func keptModulesTick() async -> Bool {
        // 只按真冲突跳过：手机正被装机 / 推包占着（phoneBusy）；出包等别的作业在跑照样收（升级首启迁移几分钟，原先 busy 整轮跳过拖到第 360 秒）
        if phoneBusy { return false }   // 出包页选了「出完之后导出自签」：不走启动器装机，自动装到手机也不做（玩家选择优先）
        let r: [String: Any]
        do {
            r = try await provider.client.perform("phoneKeptPull").result
            notePhoneAuto("phoneKeptPull", nil)
        } catch { notePhoneAuto("phoneKeptPull", String(describing: error)); return false }
        let kept = (r["kept"] as? [String]) ?? [], device = r["device"] as? String ?? ""
        // 启动器升级顺带更新 iPhone App（11-16 维护者「点一次完成整个流程」）：连着的这台壳比本版旧、且没有要按新版重出包的款（shellUpdate），
        //   开关开着（缺省开）就同样自动更新；要重出的款照旧留给顶栏按钮问
        let shellUpdate = (r["shellUpdate"] as? Bool ?? false) && Self.autoUpdatePhoneOn()
        if (kept.isEmpty && !shellUpdate) || device.isEmpty { return false }
        if r["running"] as? Bool != false { return false }   // 手机上 S2Arm 正在跑（或查不清）：装 App 会杀掉玩家这一局，等它退了下一轮再推（这一批不记已试）
        // 只发现、不装（11-16 维护者：「改成更稳妥的手动更新吧，启动器和移动端都是」）：重读游戏库，顶栏按 phoneUpdates 亮「更新 iPhone」，玩家点了才装
        let batch = device + ":" + kept.sorted().joined(separator: ",") + (shellUpdate ? ":shell" : "")
        if keptAutoTried.contains(batch) || phoneBusy { return false }
        keptAutoTried.insert(batch)
        refresh {}
        return false
    }

    public func refresh(_ done: (() -> Void)? = nil) {
        guard !loading else { refreshAgain = (refreshAgain ?? []) + (done.map { [$0] } ?? []); return }
        if let done { refreshWaiters.append(done) }
        // 启动时先显示上次的列表，不等作业；能不能玩在真正启动时还会完整校验
        if !libraryLoaded, games.isEmpty, let known = provider.library.lastKnownGames(), !known.isEmpty {
            games = known
            selectedID = known.first?.id
            libraryLoaded = true
            showingSnapshot = true
        }
        loading = true
        // 启动器这次打开后的第一次读带 startup（剃刀 H14）：作业层只在这一次清残留 / 判任务目录清理，之后每次刷新只读
        let startup = !startupCatalogSent; startupCatalogSent = true
        provider.library.load(startup: startup) { [weak self] result in
            guard let self else { return }
            self.loading = false
            self.libraryLoaded = true
            switch result {
            case .success(let games):
                self.games = games; self.libraryError = nil; self.showingSnapshot = false
                self.phoneUpdates = self.provider.library.phoneUpdates
                self.phoneDevices = self.provider.library.phoneDevices
                if !self.startupLoopsStarted {
                    self.startupLoopsStarted = true; self.resumeSGradeWatch(); self.keptModulesLoop(); self.devCertWatchLoop(); self.appleTVBackupLoop()
                    // 0.6.0（开关后面）：启动器打开时快速比对「建议更新」、读一次总控台（都只读，秒回）。之后只在出包 / 手机作业做完时再读（finish），
                    //   不跟着每次重读游戏库起：出包过程中会重读好几次，每次多起两个作业进程是浪费（ExportStreamTests）
                    self.refreshUpdateHints(); self.refreshPhoneOverview()
                }
                if self.selectedID == nil || !games.contains(where: { $0.id == self.selectedID }) { self.selectedID = games.first?.id }
            case .failure(let error):
                self.libraryError = error.message
                if error.message == LibraryProvider.noLibraryMessage {   // 没有游戏库（含文件夹被删、适配层已忘掉，#1412）：不留旧列表，回欢迎页
                    self.games = []; self.selectedID = nil; self.showingSnapshot = false
                } else if error.code == "library_id_mismatch", !self.provider.client.environment.library.isEmpty {
                    // 这个位置是另一个游戏库（库号和记下的不同，11-17 维护者：说清是哪个、玩家点一下确认）：沿用换库确认，
                    //   「仍然继续」= 对这个文件夹重走选库（已有库直接打开、记下它的库号）后重读；「先不」= 照旧显示上次的列表
                    self.showingSnapshot = !self.games.isEmpty
                    self.libraryConfirm = LibraryConfirm(url: URL(fileURLWithPath: self.provider.client.environment.library), move: false,
                                                         message: error.message, code: "library_id_mismatch")
                } else {
                    self.showingSnapshot = !self.games.isEmpty   // 列表没换：还是上次的
                }
            }
            let waiters = self.refreshWaiters; self.refreshWaiters = []
            waiters.forEach { $0() }
            if let again = self.refreshAgain { self.refreshAgain = nil; self.refreshWaiters = again; self.refresh() }
        }
    }

    // ---- environment ----

    /// 作业层「这台 Mac 缺东西」的错误码（pack/jobs/errors.py RECOVERY）：失败行据此给环境检查页入口（剃刀 M4）
    static let environmentCodes: Set<String> = ["xcode_unavailable", "xcode_setup_required", "metal_unavailable", "metal_download_unreachable", "ios_platform_missing",
        "ios_platform_unregistered", "dotnet_unavailable", "dotnet_sdk_missing", "os_unsupported", "architecture_unsupported",
        "engine_incomplete", "engine_toolchain_newer"]

    enum EnvironmentState { case unknown, ready, readyMac, missing }
    /// ready = 准备、启动、打 iPhone 包都齐；readyMac = Mac 上准备/启动齐了、但打 iPhone 包还缺（如 iOS 平台组件只挡 ipa）。
    var environmentState: EnvironmentState {
        if checks.isEmpty { return .unknown }
        guard provider.environment.ready(for: "prepare") else { return .missing }
        return checks.contains { !$0.ok && $0.required(for: "ipa") } ? .readyMac : .ready
    }

    func checkEnvironment() {
        guard !checking else { return }
        checking = true; environmentError = nil
        Task {
            do { checks = try await provider.environment.check() }
            catch { environmentError = error.localizedDescription }
            checking = false
        }
    }

    /// 一键处理：Xcode 去 App Store / 打开 Xcode / 系统设置交给系统打开；Metal 组件、iOS 平台组件、.NET SDK 交作业，做完自动重新检查。
    func repair(_ check: ToolCheck) {
        guard let action = check.action else { return }
        switch provider.environment.repairStep(for: action) {
        case .open(let url):
            NSWorkspace.shared.open(url)
        case .job(let operation):
            if busy { if !repairQueue.contains(check) { repairQueue.append(check) }; return }
            inlineFailure = nil
            showEnvironment = false
            start(JobSession.Kind(rawValue: operation) ?? .installDotnet, game: nil, name: check.name) { [weak self] sink in
                let list = try await self?.provider.environment.repair(operation, events: sink)
                if let list { self?.checks = list }
                return nil
            }
        case .chooseLibrary:
            break   // 选目录面板由环境检查页开（面板文案走界面语言），选好交 changeLibrary
        case .recheck:
            checkEnvironment()   // 组件已下载、只是没注册就绪：重查一遍，不再下一遍 8 GB
        case .unavailable:
            environmentError = check.message
        }
    }

    /// 换游戏库：setup 作业（作业层拦不支持的盘格式、云盘同步目录、非空的生文件夹）→ 写回偏好、重读游戏库、重新检查环境。
    /// 作业跑在作业页，没成的原因（如选的还在云盘里）在作业页显示；成了作业页可在访达中显示新游戏库。
    /// 换游戏库时作业层报认得出的风险（v0.3.1：认得出的风险让玩家确认）：问玩家「仍然继续 / 先不」，同意带对应参数重发，参数累积
    ///   library_in_cloud：选的文件夹在云盘同步目录里；library_folder_not_empty：文件夹里已有别的文件、又不是游戏库（S2Arm 只动自己建的文件）
    static var libraryConfirmCodes: [String: String] { BackendText.confirmFlags }   // 共享体验表 errors 的 accept（11-19 结构 9，同 Windows 读同一处）
    struct LibraryConfirm: Equatable { let url: URL; let move: Bool; let message: String; var code = "library_in_cloud"; var accept: Set<String> = [] }
    @Published var libraryConfirm: LibraryConfirm?

    func changeLibrary(to url: URL, move: Bool = false, accept: Set<String> = []) {
        guard !busy else { return }
        showEnvironment = false
        failAction(nil, game: nil)
        start(.setupLibrary, game: nil, name: url.lastPathComponent) { [weak self] sink in
            guard let self else { return nil }
            let library: String
            do {
                library = try await self.provider.library.change(to: url, moveFrom: move ? self.provider.client.environment.library : nil, accept: accept, events: sink)
            } catch let f as JobFailure where Self.libraryConfirmCodes[f.code ?? ""].map({ !accept.contains($0) }) == true {
                self.libraryConfirm = LibraryConfirm(url: url, move: move, message: f.errorDescription ?? "", code: f.code ?? "",
                                                     accept: accept.union([Self.libraryConfirmCodes[f.code ?? ""]!]))
                throw f
            }
            self.games = []; self.selectedID = nil; self.libraryLoaded = false
            self.checks = (try? await self.provider.environment.check()) ?? self.checks
            return library
        }
    }

    // ---- free space ----

    /// 读这款的占用（spaceSurvey）。换了一款先清掉上一款的。
    func clearSpaceFreed() { spaceFreed = nil }

    func loadOverview() {
        guard !overviewLoading else { return }
        overviewLoading = true; overviewError = nil
        Task {
            do { overview = try await provider.space.overview() }
            catch { overviewError = error.localizedDescription }
            overviewLoading = false
        }
    }

    func loadSpace(_ gameID: String) {
        if spaceGameID != gameID { spaceGameID = gameID; space = nil; spaceFreed = nil }
        spaceError = nil
        // 还在读上一款：读完按现在要的这款再读（原先直接返回，上一款的结果因游戏号对不上被丢，这一款永远读不出来，「释放」按钮一直灰着）
        guard !spaceLoading else { return }
        spaceLoading = true
        Task {
            do {
                let list = try await provider.space.survey(gameId: gameID)
                if spaceGameID == gameID { space = list.first { $0.gameId == gameID } }
            } catch {
                if spaceGameID == gameID { spaceError = error.localizedDescription }
            }
            spaceLoading = false
            if let want = spaceGameID, want != gameID { loadSpace(want) }
        }
    }

    /// 删勾选的部件（spaceFree，直接删、不进废纸篓）。有作业在跑不让点；作业层拒绝（别的任务在用游戏库）显示它的原话。删完重读占用与游戏库。
    func freeSpace(_ gameID: String, parts: Set<String>) {
        guard !busy, !spaceFreeing, !parts.isEmpty, spaceGameID == gameID else { return }
        spaceFreeing = true; spaceError = nil; spaceFreed = nil
        // 按作业层部件顺序交
        let ordered = (space?.parts.map(\.id) ?? []).filter { parts.contains($0) }
        Task {
            do {
                let freed = try await provider.space.free(gameId: gameID, parts: ordered, events: lockWaitSink())
                if spaceGameID == gameID { spaceFreed = freed.freedBytes }
            } catch {
                if spaceGameID == gameID { spaceError = error.localizedDescription }
            }
            spaceFreeing = false; lockWaitNote = nil
            loadSpace(gameID)
            refresh()   // 删了 Mac 运行版本，这款要重新准备
            if overview != nil { loadOverview() }
        }
    }


    // ---- import ----

    enum ImportPage { case selection, keys, results }
    @Published private(set) var importPage: ImportPage?
    @Published private(set) var importSources: [ImportSource] = []
    @Published private(set) var importBatchFailure: String?
    /// #797：刚添加的款只有更新 / DLC、缺本体（游戏名）：添加窗口不自动关，黄字说缺本体，玩家可以当场把本体拖进来
    @Published private(set) var importNoBase: [String] = []
    private var importUsesResults = false

    var pendingSources: [ImportSource] { importSources.filter { !$0.succeeded } }
    var importedSourceCount: Int { importSources.filter(\.succeeded).count }

    /// 任何作业在跑（含按款作业槽）都不起（整库口径，同 Windows 导入作业对所有款都挡）：导入不知道碰哪款，可能重写正在出 / 正在删的那款的
    ///   文件夹与 catalog.json 条目；换游戏库（setup）、放密钥（keys 拿执行锁独占）同理
    func importGames(_ urls: [URL], partialResults: Bool) {
        guard !urls.isEmpty, !busy else { return }
        if partialResults {
            importSources = []
            importPage = nil
            pendingImport = []
            if !showKeys { showAddGames = true }
        }
        submitImport(urls, partialResults: partialResults)
    }

    private func submitImport(_ urls: [URL], partialResults: Bool) {
        guard let first = urls.first, !busy else { return }
        importUsesResults = partialResults
        importBatchFailure = nil
        importNoBase = []
        failAction(nil, game: nil)
        inlineFailure = nil
        start(.importGames, game: nil, name: first.lastPathComponent + (urls.count > 1 ? " +\(urls.count - 1)" : "")) { [weak self] sink in
            guard let self else { return nil }
            let wasEmpty = self.games.isEmpty
            do {
                let result = try await self.provider.importGames(GameInputs(paths: urls), partialResults: partialResults, events: sink)
                try Task.checkCancellation()
                if partialResults { self.mergeImportSources(result.sources, submitted: urls) }
                self.importNoBase = result.missingBase.map { id in result.games.first { $0.titleId == id }?.name ?? id }
                self.selectImportedGame(result.games.first, forWelcome: wasEmpty)
                // 批量出包（v0.2.4）：一次放进多款、或在出包页又放进游戏 → 识别完列成清单（出包页那款排第一）；认不出的也列上、写原因
                let seed = self.batchSeed; self.batchSeed = nil
                let ids = (seed.map { [$0] } ?? []) + result.games.map(\.titleId).filter { $0 != seed }
                if ids.count > 1 { self.openBatch(ids, unrecognized: self.pendingSources) }
            } catch let failure as JobFailure where partialResults {
                try Task.checkCancellation()
                let rows = ImportSource.rows(in: failure.outcome.result)
                self.mergeImportSources(rows, submitted: urls)
                if failure.code == "keys_missing" {
                    self.pendingImport = self.pendingSources.isEmpty ? urls : self.pendingImportURLs
                    if wasEmpty && !self.showKeys && self.importedSourceCount == 0 {
                        // The welcome flow shows the selection and offers its existing keys step.
                        self.showAddGames = false
                        self.importPage = nil
                    } else {
                        self.importPage = .keys
                    }
                    return nil
                }
                if let imports = failure.outcome.result["imports"] as? [[String: Any]], !imports.isEmpty {
                    let games = await self.provider.importedGames(in: failure.outcome.result)
                    try Task.checkCancellation()
                    self.selectImportedGame(games.first, forWelcome: wasEmpty)
                }
                if !rows.contains(where: { $0.status == .failed && $0.code == failure.code }) {
                    self.importBatchFailure = failure.errorDescription
                }
                throw failure
            } catch {
                if partialResults && !Task.isCancelled { self.importBatchFailure = error.localizedDescription }
                throw error
            }
            return nil
        }
    }

    private var pendingImportURLs: [URL] { pendingSources.map { URL(fileURLWithPath: $0.path) } }

    private func selectImportedGame(_ game: GameInfo?, forWelcome: Bool) {
        guard let game else { return }
        selectedID = game.titleId
        if forWelcome { welcomeGameID = game.titleId }
    }

    private func mergeImportSources(_ rows: [ImportSource], submitted: [URL]) {
        guard !rows.isEmpty else { return }
        // A failed folder scan may expand to child sources on retry.
        let submittedPaths = Set(submitted.map(\.path))
        let returnedIDs = Set(rows.map(\.id))
        importSources.removeAll { !$0.succeeded && submittedPaths.contains($0.path) && !returnedIDs.contains($0.id) }
        var positions = Dictionary(uniqueKeysWithValues: importSources.enumerated().map { ($0.element.id, $0.offset) })
        for row in rows {
            if let index = positions[row.id] {
                if !importSources[index].succeeded { importSources[index] = row }
            } else {
                positions[row.id] = importSources.count
                importSources.append(row)
            }
        }
        if !pendingSources.isEmpty { importPage = .results }
    }

    func retryImport() {
        submitImport(pendingImportURLs, partialResults: true)
    }

    func backFromImportResults() {
        guard !busy else { return }
        if pendingSources.contains(where: \.needsKeys) {
            pendingImport = pendingImportURLs
            keysRejected = pendingSources.contains { $0.code == "keys_outdated" }
            importPage = .keys
        } else {
            importPage = .selection
        }
    }

    func cancelImport() {
        if let session, [.importGames, .keys].contains(session.kind) { stop() }
        closeImport()
        clearInlineFailure()
    }

    private func closeImport() {
        showAddGames = false
        showKeys = false
        importPage = nil
        importSources = []
        pendingImport = []
        importBatchFailure = nil
        importNoBase = []
        keysRejected = false
    }

    // ---- export ----

    /// 勾了的平台，按页面顺序（也是排队顺序）。
    var exportTargets: [ExportPlatform] { ExportPlatform.picked(exportChoice) }

    /// 勾上 / 取消一个平台，当场记住（未开放的点不动）。
    func toggleExport(_ platform: ExportPlatform) {
        guard platform.released else { return }
        exportChoice = ExportPlatform.toggled(exportChoice, platform)
        do { try provider.settings.setExportTargets(exportChoice) } catch { choiceError = error.localizedDescription }
    }

    /// 现在不能出包的原因（nil = 能出）。顺序同旧启动器导出页：没勾平台 → 游戏缺文件 → 缺 keys → 有任务在跑。
    ///   环境不挡（剃刀 M4）：前端的检查结果可能是旧的（玩家在外面修好 Xcode 回来仍被挡），作业层开头自己校验环境、缺什么回真实原因
    func exportBlocker(_ game: GameSummary, platforms chosen: [ExportPlatform]? = nil) -> ExportBlocker? {
        let platforms = chosen ?? exportTargets
        if platforms.isEmpty { return .noPlatform }
        if !game.buildable { return .game(Self.requirementsText(game)) }
        if game.needsKeys, provider.library.keysReady == false { return .keys }
        // 别的出包在跑可以排队（出包锁 .produce.lock 一次一个）；手机槽（装机等）在跑不挡出包。
        //   主槽 / 按款作业槽有非出包作业（删除 / 搬家 / 导入 / 清缓存…）在跑：不起，同 Windows 游戏库类作业对所有款都挡（JobSlots.Blocks）
        if mainJobs.contains(where: { $0.export == nil }) { return .busy }
        if playHolds(game.id) { return .busy }   // 正在玩这款：同一款不边玩边出包（玩别的款照出，游戏槽与主槽并行）
        return nil
    }

    /// 出包：勾了几个平台就几个作业（Mac = build、iPhone / iPad = ipa），按页面顺序一个接一个；解包等平台无关阶段作业层共用（游戏库/cache/<程序号>/extract）。
    /// 一个平台没成不挡后面的平台；取消就整次停。
    /// platforms 不给 = 出包页勾的；本机控制口（AI 操作）按命令给一个平台，deviceTarget 只用于 iPhone 出包（提示地址窗口）。
    @discardableResult
    func export(_ game: GameSummary, platforms chosen: [ExportPlatform]? = nil, deviceTarget: [String: Any]? = nil, spaceOverride: Bool = false, location: String? = nil,
                mode: String? = nil, androidSerial: String? = nil) -> ExportBlocker? {
        if let blocker = exportBlocker(game, platforms: chosen), blocker.stopsStart { return blocker }
        // 这款有下游深度方案、玩家还没选过、这次要出 iPhone：先问一次（选完 chooseDownstream 接着导出）。选过就照选的走，作业层按记下的选择分流
        if game.downstream != nil, game.downstream?.choice == nil, (chosen ?? exportTargets).contains(.ios) {
            downstreamPending = (chosen, deviceTarget, spaceOverride, location)
            downstreamPromptFor = game.id
            return nil
        }
        let item = QueuedExport(game: game, platforms: chosen ?? exportTargets, target: deviceTarget ?? currentDeviceTarget, spaceOverride: spaceOverride, location: location, mode: mode,
                                androidSerial: androidSerial)
        exportFor = nil
        welcomeGameID = nil
        if mainBusy || batchRunning || phoneHolds(game.id) {   // 正在出别的 / 一批正在出 / 这款正在装到手机（装机读它的程序件）：排队，出完 / 装完接着出（11-08 维护者：出好一个接着出下一个）
            if session?.gameID == game.id || exportQueue.contains(where: { $0.game.id == game.id }) { return .alreadyQueued }
            exportQueue.append(item)
            return nil
        }
        sessionHidden = false
        runExport(item)
        return nil
    }

    /// 确认页选完：记下选择（scheme = 方案 id；"" = 仍用原版），读库后接着导出刚才那一单
    func chooseDownstream(_ gameID: String, scheme: String) {
        downstreamPromptFor = nil
        let pending = downstreamPending; downstreamPending = nil
        Task { @MainActor [weak self] in
            guard let self else { return }
            do { try await self.provider.setDownstreamChoice(gameId: gameID, scheme: scheme, events: .discard) }
            catch { self.failAction(error.localizedDescription, game: gameID); return }
            self.refresh { [weak self] in
                guard let self, let p = pending, let g = self.games.first(where: { $0.id == gameID }) else { return }
                _ = self.export(g, platforms: p.platforms, deviceTarget: p.deviceTarget, spaceOverride: p.spaceOverride, location: p.location)
            }
        }
    }

    /// 空闲时接着做排着的事（启动器剃刀 M2）：先装批量出完的款，再出批次进行中单独点的款。批次结束、装机结束都从这里走，
    ///   原先批次结束只看自动装机，批次期间排进 exportQueue 的单款永远「排队中」
    func pumpQueue() {
        if !installAfterQueue.isEmpty, !phoneBusy { installNextQueued() }   // 出完等着装的：手机槽空就装，主槽在出别的也照装
        guard !mainBusy, !sideBusy, !batchRunning, let i = exportQueue.firstIndex(where: { !phoneHolds($0.game.id) }) else { return }   // 按款作业槽在删 / 搬：做完再接（同 exportBlocker）
        let next = exportQueue.remove(at: i); session = nil; runExport(next)
    }
    /// 这款正在装到手机（手机槽作业用它的程序件）：对它的出包等装完
    func phoneHolds(_ gameID: String) -> Bool { phoneBusy && phoneSession?.gameID == gameID }

    /// 出一款：起一个出包编排作业（剃刀 H13，pack/jobs/export_plan.py），计划只有这一款的这几个平台、不写批次文件。
    /// 平台顺序、iPhone 之后还有平台时先不清解包（followUp）、这台电脑出不了的平台都在作业层；这里只按它的事件画每个平台的进度与结局。
    /// 出包页「S 级优化（Beta）· 出包时打入」（v0.4.0 维护者：默认勾上，玩家可取消）：按款记住；没记过 = 勾上
    func sgradeOn(_ gameID: String) -> Bool { !UserDefaults.standard.bool(forKey: "S2ASGradeOff." + gameID) }
    func setSgrade(_ gameID: String, _ on: Bool) { objectWillChange.send(); UserDefaults.standard.set(!on, forKey: "S2ASGradeOff." + gameID) }

    func runExport(_ item: QueuedExport) {
        failAction(nil, game: nil)
        let game = item.game, platforms = item.platforms
        var session = JobSession(kind: .export, gameID: game.id, name: game.name)
        session.export = ExportRun(platforms: platforms)
        var plan: [String: Any] = ["source": "inline", "items": [["gameId": game.id, "name": game.name, "platforms": platforms.compactMap(Self.batchPlatform)]]]
        if let location = item.location { plan["location"] = location }
        if let target = item.target { plan["deviceTarget"] = target }
        if item.spaceOverride { plan["spaceOverride"] = true }
        if !sgradeOn(game.id) { plan["sgrade"] = false }   // 出包页取消「S 级优化（Beta）」：这次不带社区补丁与金手指（pack/jobs/patches.py prepare）
        // 出好接着装到这台安卓手机（export_plan → installSerial）：控制口给的 > 界面选的；"" = 明确不装
        if platforms.contains(.android), let serial = item.androidSerial ?? androidPhone, !serial.isEmpty { plan["androidSerial"] = serial }
        start(session) { [weak self] sink in
            guard let self else { return nil }
            do { _ = try await self.provider.export(plan: plan, hasIPhone: platforms.contains(.ios), mode: item.mode, events: self.exportSink(sink)) }
            catch let pause as JobFailure where pause.state == .paused { throw ExportIncomplete() }   // 磁盘满：那个平台已按 stepEnded 记失败（码 disk_full），整次算没全成
            if self.session?.export?.failed.isEmpty == false { throw ExportIncomplete() }
            return nil
        }
    }

    /// 出包编排作业的事件 → 现有的出包会话：step 换到这个平台（批量出包时换到这一款的会话）、child 当成普通作业事件、stepEnded 记这个平台的结局
    func exportSink(_ sink: EventSink) -> EventSink {
        var failure: [String: Any] = [:]   // 子作业终态里的诊断（建议、失败事实），stepEnded 时并进结局
        return EventSink { [weak self] event in
            guard let self else { return }
            let step = event.fields["step"] as? [String: Any] ?? [:]
            let platform = (step["platform"] as? String).flatMap(Self.exportPlatform)
            switch event.ev {
            case "step":
                if let gid = step["gameId"] as? String { self.batchStepStarted(gid) }
                if let platform, let k = self.session?.export?.platforms.firstIndex(of: platform) { self.beginPlatform(k) }
                failure = [:]
            case "child":
                guard let inner = event.fields["event"] as? [String: Any], let job = JobEvent(json: inner) else { return }
                if job.isFinal { failure = (job.fields["diagnostics"] as? [[String: Any]] ?? []).last(where: { ($0["severity"] as? String) == "error" }) ?? [:]; return }
                sink.emit(EngineEvent(job: job))
            case "stepEnded":
                guard let platform, let o = event.fields["outcome"] as? [String: Any] else { return }
                let state = JobOutcome.State(rawValue: o["state"] as? String ?? "") ?? .failed
                self.endPlatform(platform, ExportRun.Outcome(state: state, message: o["message"] as? String ?? "", code: o["code"] as? String,
                                                             output: o["outputPath"] as? String ?? o["output"] as? String, ipa: o["ipa"] as? String,
                                                             appBytes: ((o["ipaBytes"] ?? o["outputBytes"]) as? NSNumber)?.int64Value,
                                                             packBytes: (o["packBytes"] as? NSNumber)?.int64Value,
                                                             suggestion: failure["suggestion"] as? String ?? "", failureSummary: failure["failureSummary"] as? String,
                                                             notes: o["notes"] as? [String] ?? [],
                                                             boost: (o["boost"] as? [String: Any] ?? [:]).compactMapValues { ($0 as? NSNumber)?.intValue },
                                                             sgrade: SGradeOutcome(o["boost"] as? [String: Any] ?? [:])))
                // 导出确认页「开始」点在还没做好 iPhone 版的款上：iPhone 包出好就按那份计划接着导出 / 逐台导入（D-9ZF9：原先出完包停在完成页，要再点一次）。
                //   排进 installAfterQueue，这次出包会话结束后由 pumpQueue 接着跑（installNextQueued 按 exportAfterPack 走）
                if platform == .ios, state == .completed, let gid = self.session?.gameID, self.exportAfterPack[gid] != nil,
                   !self.batchRunning, !self.installAfterQueue.contains(gid) { self.installAfterQueue.append(gid) }
                if self.batchRunning { self.batchStepEnded() }
            default:
                sink.emit(event)
            }
        }
    }

    /// 界面上的目标设备：玩家选过 → 只认那台（连着就是它；不连着为 nil，界面说「它不在线」，不替玩家换成别的——11-11 玩家实报：
    ///   续签时选的 iPhone 拔了线，同网的 iPad 被装上）。没选过 → 有线连着的那台，否则第一台（#272），装的就是界面上显示的这台
    nonisolated static func pickDevice(_ connected: [[String: Any]], chosen: String?) -> [String: Any]? {
        if let chosen { return connected.first(where: { $0["id"] as? String == chosen || $0["udid"] as? String == chosen }) }
        return connected.first(where: { $0["wired"] as? Bool == true }) ?? connected.first
    }

    /// 装机 / 续签 / 更新手机上的 S2Arm 一律带上的设备号：玩家选的那台（此刻不在线也照样带上，作业层如实说「iPhone「名字」不在线」），
    /// 没选过时就是界面上显示的那台。作业层收不到设备号就拒绝，不自己挑
    var installDeviceID: String? { chosenDevice ?? (selectedDevice?["udid"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? selectedDevice?["id"] as? String }

    /// 选过的设备此刻不在线（不在连着的列表里）：界面提示并让玩家改选
    var chosenDeviceOffline: Bool { chosenDevice != nil && selectedDevice == nil }

    /// 这次实际装上的那台（装机结果的 deviceKind / deviceName）：完成页、顶部胶囊写「已装到 iPad「名字」」，不写死 iPhone
    @Published var installedTo: String = ""
    nonisolated static func deviceLabel(kind: String, name: String, language: String = Lang.initial().sharedCode) -> String {
        named(kind.isEmpty ? "iPhone / iPad" : kind, name, language: language)
    }
    /// 类型 + 设备名，引号按界面语言：中文 / 日文「名字」紧跟类型，其余语言空一格加弯引号 “名字”（同英文文案里设备名的写法）
    nonisolated static func named(_ kind: String, _ name: String, language: String) -> String {
        if name.isEmpty { return kind }
        return ["zh-Hans", "zh-Hant", "zh", "ja"].contains(language) ? "\(kind)「\(name)」" : "\(kind) “\(name)”"
    }
    /// 正在装 / 要装的那台的称呼（进度胶囊用）：选中且连着 → 类型 + 名字；否则「iPhone / iPad」
    var installTargetLabel: String {
        guard let d = selectedDevice else { return "iPhone / iPad" }
        return Self.deviceLabel(kind: installTargetKind, name: d["name"] as? String ?? "")
    }
    /// 「第一次打开前，在 <设备> 上：…点「<证书名>」→ 信任」：{0} 证书名，{1} 这次装上的那台的类型（读不到写「iPhone / iPad」，#1962）
    func trustNamedText(language: String) -> String {
        BackendText.launcherText("install.done.trustNamed", language: language)
            .replacingOccurrences(of: "{0}", with: installTrustName).replacingOccurrences(of: "{1}", with: BackendText.deviceKind(installTrustKind))
    }
    /// 正在装 / 要装的那台的类型（指代这一台的文案里填 {0}，#1962）：选中且连着 → iPad / iPhone；否则「iPhone / iPad」
    var installTargetKind: String {
        guard let d = selectedDevice else { return "iPhone / iPad" }
        return Self.padLike(d["productType"] as? String) ? "iPad" : "iPhone"
    }
    /// 某一台（设备号）的类型：总控台读到的 kind 优先，没有退回要装的那台（installTargetKind）
    func deviceKind(of device: String?) -> String {
        phoneOverview?.phones.first { $0.device == device }?.kind.map { BackendText.deviceKind($0) } ?? installTargetKind
    }

    var selectedDevice: [String: Any]? { Self.pickDevice(connectedDevices, chosen: chosenDevice) }

    /// 选中的设备是 iPad 时，「推到 iPhone」这几处入口文字换成「装到 iPad」（#487）；没连设备照旧写 iPhone
    var installTargetIsPad: Bool { Self.padLike(selectedDevice?["productType"] as? String) }
    /// iPad，以及按 iPad 兼容装的 Apple Vision Pro（机型号 RealityDevice*；理论支持、未真机验证）：入口和文案写 iPad
    /// 总控台卡片与页顶画的设备轮廓（#1967）：作业认出 iPad 画 iPad，其余（iPhone / 还没连过 / 读不到类型）画 iPhone；两处同一个符号，matchedGeometryEffect 才接得上
    /// #1967：不知道类型（旧记录、还没现查）画不分机型的设备图标，不再默认 iPhone
    static func deviceSymbol(_ kind: String?) -> String { kind == "iPad" ? "ipad" : kind == "iPhone" ? "iphone" : "ipad.and.iphone" }

    static func padLike(_ productType: String?) -> Bool {
        guard let t = productType else { return false }
        return t.hasPrefix("iPad") || t.hasPrefix("RealityDevice")
    }
    /// 入口文字的键：iPad 版键名 = 原键 + "Pad"（zh / en 两份 l10n 同步）
    func installKey(_ key: String) -> String { installTargetIsPad ? key + "Pad" : key }

    /// 出 iPhone 包的目标：连着的手机优先（多台时用选中的那台），其次玩家选的机型；都没有交给作业层按常见机型出。
    var currentDeviceTarget: [String: Any]? {
        if let device = selectedDevice { return device }
        guard let chosenModel, var model = targetModels.first(where: { $0["productType"] as? String == chosenModel }), let os = targetOS.first else { return nil }
        model["os"] = os
        return model
    }

    func refreshTargets() async {
        // 读失败 = 设备状态不明，不留上一次的列表（原先保留旧值：拔了手机还显示已插入，11-11 玩家反馈）；原因照实显示在设备行（deviceProbeError），
        //   并按剃刀 M13 只报一次（5 秒轮询不重复弹）
        let probe: [String: Any]
        do { probe = try await provider.targetDevices(); readOK("s2arm.fail.targets") }
        catch {
            // 探测被取消不是读不到：设备行一消失（装完换到完成页）SwiftUI 就取消这次轮询，原先当成失败清空设备、弹「读不到连接的设备 /
            //   任务已取消」（玩家截图：顶栏刚写「已推到 iPhone」就弹这框）。列表照旧，不弹
            if Task.isCancelled || (error as? JobFailure)?.state == .cancelled { return }
            connectedDevices = []; deviceProbeError = error.localizedDescription; reportFailure("s2arm.fail.targets", error, passive: true); return
        }
        connectedDevices = probe["connected"] as? [[String: Any]] ?? []
        deviceProbeError = (probe["connectedError"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        targetModels = probe["models"] as? [[String: Any]] ?? []
        targetOS = probe["osChoices"] as? [Int] ?? []
        await askDevCertIfNew()
    }

    /// 换到第 k 个平台：进度、里程碑从头算（日志接着记）。
    func beginPlatform(_ k: Int) {
        guard var session, var run = session.export else { return }
        run.index = k; run.platformStarted = Date()
        session.export = run; session.progress = JobProgress(); session.stage = ""
        self.session = session
    }

    func endPlatform(_ platform: ExportPlatform, _ outcome: ExportRun.Outcome) {
        guard var session, var run = session.export else { return }
        var outcome = outcome
        outcome.seconds = Date().timeIntervalSince(run.platformStarted)
        run.outcomes[platform] = outcome
        session.export = run; session.progress.finish()
        self.session = session
    }

    /// 后端错误摘要（作业层 report）拿不到时复制的内容（剃刀 M19）：原先前端另拼一份格式不同的完整摘要，反馈时分不清贴的是哪份；
    ///   现在只留后端那一份，拿不到就明说，附界面上那句原因和错误码
    static func reportUnavailable(_ message: String, code: String?) -> String {
        Localizer().t("s2arm.report.unavailable") + "\n" + message + (code.map { " (\($0))" } ?? "")
    }

    /// 把最近一次失败作业的错误摘要放进剪贴板。正文由作业层生成（带任务号、阶段、错误码、真实日志）；没有作业记录才用 fallback。
    func copyErrorReport(platform: String?, gameID: String?, operations: [String]?, fallback: String) async {
        let info = Bundle.main.infoDictionary ?? [:]
        var extra: [String: Any] = ["app": info["CFBundleShortVersionString"] as? String ?? "", "build": info["S2ABuildID"] as? String ?? "",
                                    "xcode": provider.client.environment.developerDir.replacingOccurrences(of: "/Contents/Developer", with: ""),
                                    "checks": checks.filter { !$0.ok }.map { ["name": $0.name, "message": $0.message] }]
        if let platform { extra["platform"] = platform }
        if let device = currentDeviceTarget.flatMap({ ($0["model"] ?? $0["name"]) as? String }) { extra["device"] = device }
        let report = operations == nil ? nil : await provider.client.errorReport(gameID: gameID, operations: operations ?? [], extra: extra)
        let text = report ?? fallback
        NSPasteboard.general.clearContents(); NSPasteboard.general.setString(text, forType: .string)
    }

    /// 游戏库层的「上传给开发者」弹窗（LauncherView 挂）：游戏库「更多」菜单与控制口 /diagnostics {action: upload} 打开；"" = 不针对某款
    @Published var diagUploadFor: String?
    /// 控制口替玩家点弹窗里的按钮（send / copy / close / cancel）：弹窗收到就照点、清掉（UploadDiagnosticsSheet.onReceive）
    struct DiagUploadCommand: Equatable { let id = UUID(); let action: String }
    @Published var diagUploadCommand: DiagUploadCommand?

    /// 上传给开发者（diagnosticsUpload 作业）→ 短码。错误摘要的附加信息同 copyErrorReport（App 版本、目标设备、环境检查未过项）；
    ///   operations 为 nil（没有作业记录的失败，如环境检查）时把界面上的 fallback 正文交给作业放进包里。失败抛作业的原因（界面退回导出）
    struct DiagUploadReceipt: Equatable { let code: String; let issueURL: URL?; let repo: String }
    func uploadDiagnostics(gameID: String?, operations: [String]?, fallback: String, note: String = "") async throws -> DiagUploadReceipt {
        let info = Bundle.main.infoDictionary ?? [:]
        var extra: [String: Any] = ["app": info["CFBundleShortVersionString"] as? String ?? "", "build": info["S2ABuildID"] as? String ?? "",
                                    "checks": checks.filter { !$0.ok }.map { ["name": $0.name, "message": $0.message] }]
        if let device = currentDeviceTarget.flatMap({ ($0["model"] ?? $0["name"]) as? String }) { extra["device"] = device }
        var request: [String: Any] = ["extra": extra]
        if let gameID { request["gameId"] = gameID }
        if let operations { request["operations"] = operations } else if !fallback.isEmpty { request["summary"] = fallback }
        let said = note.trimmingCharacters(in: .whitespacesAndNewlines)
        if !said.isEmpty { request["note"] = said }   // 玩家一句「发生了什么」→ 作业层写进 manifest.json 的 note（pack/jobs/library.py diagnostics）
        let outcome = try await provider.client.perform("diagnosticsUpload", extra: request)
        guard let code = outcome.result["code"] as? String, !code.isEmpty else { throw UploadNoCode() }
        // 预填好的新帖页与仓库地址由作业层给（pack/jobs/diag_upload.py issue_url，iOS 同格式）
        return DiagUploadReceipt(code: code, issueURL: (outcome.result["issueUrl"] as? String).flatMap(URL.init(string:)),
                                 repo: outcome.result["repo"] as? String ?? "github.com/jhaizhou-ops/S2Arm")
    }
    struct UploadNoCode: LocalizedError { var errorDescription: String? { Localizer().t("s2arm.diagUpload.noCode") } }
    /// 界面语言写进作业环境：之后每个作业请求都带 uiLanguage（zh-Hans / en），作业层按它出玩家看得到的文件名与说明
    public func setUILanguage(_ code: String) { provider.client.environment.uiLanguage = code }

    /// 装机前清单（装机页开着时每几秒调一次）。换了一款先清掉上一款的清单。
    func checkInstall(_ gameID: String, device: String? = nil) {
        let target = device ?? installDeviceID
        if installGameID != gameID || installReadinessDevice != target {   // 换了款或换了设备：旧清单不能当这台的
            installGameID = gameID; installReadinessDevice = target; installReadiness = nil; installError = nil
        }
        if installChecking { pendingInstallCheck = (gameID, target); return }
        installChecking = true
        Task {
            do {
                let readiness = try await provider.installReadiness(gameId: gameID, device: target)
                if installGameID == gameID && installReadinessDevice == target { installReadiness = readiness; installError = nil }
            } catch {
                if installGameID == gameID && installReadinessDevice == target { installError = error.localizedDescription }
            }
            installChecking = false
            if let next = pendingInstallCheck { pendingInstallCheck = nil; checkInstall(next.game, device: next.device) }
        }
    }

    /// 在 Xcode 打开引导项目；玩家运行一次后，装机页每 5 秒的重查会自动打勾。
    func openSigningProject(completion: ((Error?) -> Void)? = nil) {
        Task {
            do {
                let path = try await provider.signingProject()
                guard !path.isEmpty else { throw CocoaError(.fileNoSuchFile) }
                let url = URL(fileURLWithPath: path)
                guard NSWorkspace.shared.open(url) else { throw CocoaError(.fileReadUnknown) }
                completion?(nil)
            } catch {
                if let completion { completion(error) } else { installError = error.localizedDescription }
            }
        }
    }

    /// 装机 / 删除会让手机上别的电脑装的游戏暂时不能玩（作业层 container_would_drop）：界面弹确认，确认了带 acceptDropped 重跑。
    /// ask = false（控制口发起）不弹，原样失败，由调用方带 acceptDropped 再发。
    /// 更新手机上的 S2Arm 也会撞上（v0.3.0 只换壳验收：原先这条路没接确认，界面说「确认继续再点一次」却永远过不去）：update 记着当时的参数，确认后原样带 acceptDropped 重跑
    var queuedPhoneUpdates = Set<UpdateArgs>()
    struct UpdateArgs: Hashable { let acceptStale: Bool; let repack: [String]; let language: String; let device: String?; var accept: Set<String> = []
        var mode: String? = nil; var sgradeUpgrade = false }   // 「S 级优化升级」页发起的：重发时照带装法与升级标记
    /// 作业层可确认继续的停装码 → 玩家点「仍然继续」后重发时带的参数（维护者 11-11 装机原则：认得出的风险让玩家确认，确认了就装）。
    ///   结局页（PhoneJobPanel）按码在页内问「仍然继续 / 先不」，不弹散的对话框；参数累积（先确认挤掉、再撞上备份失败，第二次重发两个都带）
    ///   11-19 结构 9：表只在共享体验表 errors 的 accept 一处（Windows 读同一处）；原先这里另写一份，没有 cert_revoke_confirm，「仍然继续」点不了
    static var confirmCodes: [String: String] { BackendText.confirmFlags }
    struct DropConfirm { let kind: JobSession.Kind; var game: GameSummary? = nil; let message: String; var update: UpdateArgs? = nil
                         var code = "container_would_drop"; var accept: Set<String> = []
                         /// 当初装到哪台：确认后原样带回（不能退回「界面选中的那台」，控制口指定的设备会被换掉）
                         var device: String? = nil
                         /// 当初是「导出自签包」：确认后照导出重发
                         var export = false }
    @Published var dropConfirm: DropConfirm?
    /// 上一次「更新手机上的 S2Arm」的参数：失败页「重试」按它再来
    var lastUpdateArgs: UpdateArgs?
    /// S 级优化升级队列的实时状态（11-18 维护者：点了才升、可批量；队列只在作业层 sgradeUpgradeBatch 一份，两端共用）：
    ///   游戏号 → (状态, 进度)，来自跑队列作业的事件与 sgradeUpgradeStatus 轮询，盖在游戏库 catalog 的 sgradeState / sgradeProgress 上
    ///   （S2ArmLauncherModel+SGradeUpgrade.swift）
    @Published var sgradeLive: [String: SGradeLive] = [:]
    @Published var sgradeBatchLive: SGradeBatch?
    var sgradePolling = false
    var keptLoopStarted = false, keptAutoTried: Set<String> = []   // keptModulesLoop（#8be819）
    var phoneAutoFails: [String: Int] = [:]   // 自动更新后台作业连续失败次数（notePhoneAuto，+PhoneBanner.swift）
    /// Apple TV（tvOS 版，S2ArmLauncherModel+AppleTV）：配对过的电视、出包页选的那台、存档自动备份循环起没起
    /// 安卓（开关后，S2ArmLauncherModel+Android.swift）：androidDevices 作业回报的开关与已连接手机、玩家选的那台（nil = 只连一台就用它）
    @Published var androidEnabled = false
    @Published var androidPhones: [[String: Any]] = []
    @Published var androidPhoneChoice: String?
    @Published var appleTVs: [[String: Any]] = []
    @Published var appleTVChoice: String?
    var tvBackupStarted = false
    func retryPhoneUpdate() {
        guard let a = lastUpdateArgs else { return }
        updatePhone(acceptStale: a.acceptStale, repack: a.repack, language: a.language, device: a.device, accept: a.accept, mode: a.mode, sgradeUpgrade: a.sgradeUpgrade)   // 已确认过的照带
    }

    /// 环境检查「装到 iPhone 的方式」里手填的 Xcode 团队号（10 位）：一键装、不用证书时每次装机都带上（原先只在装机页点「用这个团队」那一次带）
    static let teamCodeKey = "s2arm.enteredTeamCodeId"
    static var enteredTeam: String? {
        let t = (UserDefaults.standard.string(forKey: teamCodeKey) ?? "").trimmingCharacters(in: .whitespaces).uppercased()
        return t.count == 10 ? t : nil
    }

    /// export = true：导出到文件夹、不连手机（作业层 install 请求带 mode = export）
    /// sign：导出确认页选的签名（appleId / devCert / none）；nil = 按设置里的
    /// done：这一单真正结束后调（nil = 成了；否则是失败 / 取消的原因）：导出确认页逐台导入时按它记每台结果、接着下一台。
    ///   在作业槽收尾（finish）之后才调，接着起下一单不会被「这款已在推」挡掉
    func install(_ game: GameSummary, teamId: String? = nil, device: String? = nil, acceptDropped: Bool = false, accept: Set<String> = [], ask: Bool = true, export: Bool = false,
                 sign: String? = nil, done: ((Error?) -> Void)? = nil) {
        let accept = acceptDropped ? accept.union(["acceptDropped"]) : accept
        // 不在这里再核装机页清单（启动器剃刀 M1）：装机页按钮和控制口调用前各自核过；批量出完自动装的款从没打开过装机页，原先在这里被悄悄挡掉、队列停住
        // 出包等主槽作业照跑也能装（手机槽）；手机槽忙 / 这款正被主槽占着（正在出这款）→ 排队，那单结束接着装
        // 这一款已在推 / 已排上：不重复排（D-YXDW：点一下就给同一款再排一单，推完又推一遍）。排队那单出队后才执行、确认后的续推在前一单结束后才发，都不受影响
        if phonePushing(game.id) || phoneQueued(game.id) { return }
        guard !phoneBusy, !mainHolds(game.id) else {
            phoneQueue.append((game.id, { [weak self] in self?.install(game, teamId: teamId, device: device, accept: accept, ask: ask, export: export, sign: sign, done: done) })); return
        }
        let teamId = teamId ?? Self.enteredTeam
        failAction(nil, game: nil)
        // 装到玩家选的那台（控制口明说了 device 的照它；s2armctl 不带就是空，作业层拒绝）。界面发起的一律带上，不靠作业层挑
        let device = export ? nil : device ?? installDeviceID
        installedTo = ""
        phoneOutcome = nil
        selfSigning = export
        // 失败页「重试」：同样的参数原样再来一次（0.6.4：单款装机 / 导出失败原先只有「改为导出」）
        lastInstallRetry = { [weak self] in self?.install(game, teamId: teamId, device: device, accept: accept, ask: ask, export: export, sign: sign, done: done) }
        phoneAppPartial = nil
        let report = { (e: Error?) in if let done { Task { @MainActor in done(e) } } }   // 排到这单收尾（finish）之后
        start(.install, game: game, name: game.name) { [weak self] sink in
            do {
                do {
                    let r = try await self?.provider.install(gameId: game.id, teamId: teamId, device: device, accept: accept, mode: export ? "export" : nil, sign: sign, events: sink)
                    if let r, let app = r.app {   // 独立 App（0.6.4）：成功页 A / B 自己有「在访达中显示」，不自动弹访达
                        self?.phoneOutcome = PhoneOutcome(device: Self.deviceLabel(kind: r.deviceKind, name: r.deviceName), packFiles: r.packFiles, packBytes: r.packBytes, app: app)
                        if !app.exported { self?.installedTo = Self.deviceLabel(kind: r.deviceKind, name: r.deviceName) }
                    } else if let r, !r.exportFolder.isEmpty {   // 导出模式：没装手机，打开放好的文件夹
                        self?.phoneOutcome = PhoneOutcome(skipped: r.skipped, exportFolder: r.exportFolder, exportGames: r.exportGames, exportBytes: r.exportBytes,
                                                         exportFresh: r.exportFresh, exportReused: r.exportReused, exportUnwritten: r.exportUnwritten)
                        NSWorkspace.shared.open(URL(fileURLWithPath: r.exportFolder))
                    } else if let r {
                        self?.installedTo = Self.deviceLabel(kind: r.deviceKind, name: r.deviceName)
                        self?.phoneOutcome = PhoneOutcome(device: Self.deviceLabel(kind: r.deviceKind, name: r.deviceName), packFiles: r.packFiles, packBytes: r.packBytes)
                    }
                    self?.installOpenToFinish = r?.openToFinish ?? false
                    self?.installTrustName = r?.trustName ?? ""
                    self?.installTrustKind = r?.deviceKind ?? ""
                }
                catch let f as JobFailure where ask && Self.confirmCodes[f.code ?? ""] != nil {
                    self?.dropConfirm = DropConfirm(kind: .install, game: game, message: f.errorDescription ?? "", code: f.code ?? "",
                                                    accept: accept.union([Self.confirmCodes[f.code ?? ""]!]), device: device, export: export)
                    throw f
                }
                catch let f as JobFailure {   // 装着不止一个 S2Arm、还没选：失败页就地列候选，选好重推这一款（同样的参数）
                    self?.notePushChoice(f) { [weak self] in self?.install(game, teamId: teamId, device: device, accept: accept, ask: ask, export: export, sign: sign, done: done) }
                    // 独立 App 装上了、资源没推完（作业失败结果 appInstalled）：失败页写「App 已装上，资源没推完」，重试只推剩下的
                    self?.phoneAppPartial = (f.outcome.result["appInstalled"] as? [String: Any]).map { StandaloneApp($0) }
                    throw f
                }
            } catch { report(error); throw error }
            report(nil)
            return nil
        }
    }

    /// 确认框点了「仍然继续」
    func confirmDrop(_ d: DropConfirm) {
        dropConfirm = nil; clearInlineFailure()
        if let u = d.update { updatePhone(acceptStale: u.acceptStale || d.accept.contains("acceptStale"), repack: u.repack, language: u.language, device: u.device, accept: d.accept,
                                          mode: u.mode, sgradeUpgrade: u.sgradeUpgrade); return }
        guard let game = d.game else { return }
        if d.kind == .installRemove { removeFromPhone(game, device: d.device, accept: d.accept) } else { install(game, device: d.device, accept: d.accept, export: d.export) }
    }

    /// 确认页上与「仍然继续」并列的另一条路（共享体验表 errors 的 alternate；自签走查结构 7：新建证书确认的「改用已有证书」）：码 → (动作, 文案键)
    static var confirmAlternates: [String: (action: String, labelKey: String)] { BackendText.confirmAlternates }

    /// 确认页点了并列的另一条路：不重发，收起这一单的结局页，按装机前清单动作的同一个派发去落点（devCertSettings = 设置里签名方式那一栏）
    func takeConfirmAlternate(_ d: DropConfirm) {
        guard let alt = Self.confirmAlternates[d.code], Self.installActions.contains(alt.action) else { return }
        dropConfirm = nil; clearInlineFailure(); backFromPhone()
        performInstallAction(alt.action, gameId: d.game?.id ?? "")
    }

    /// 装回上一版手机 S2Arm（只换宿主后新宿主不好用时）。
    func restorePhone(_ game: GameSummary, device: String? = nil) {
        guard !phoneBusy, !mainHolds(game.id) else { phoneQueue.append((game.id, { [weak self] in self?.restorePhone(game, device: device) })); return }
        failAction(nil, game: nil)
        let device = device ?? game.iphoneInstallDevice ?? installDeviceID   // 装回当初那台
        phoneOutcome = PhoneOutcome(device: installTargetLabel)
        start(.installRestore, game: game, name: game.name) { [weak self] sink in
            try await self?.provider.restorePhone(device: device, events: sink)
            return nil
        }
    }

    /// accept：玩家确认过的参数（同 install：container_would_drop → acceptDropped、删前没能备份存档 save_backup_failed → acceptNoBackup），逐次累积
    func removeFromPhone(_ game: GameSummary, acceptDropped: Bool = false, ask: Bool = true, device: String? = nil, accept: Set<String> = []) {
        let accept = acceptDropped ? accept.union(["acceptDropped"]) : accept
        guard !phoneBusy, !mainHolds(game.id) else {
            phoneQueue.append((game.id, { [weak self] in self?.removeFromPhone(game, ask: ask, device: device, accept: accept) })); return
        }
        failAction(nil, game: nil)
        let device = device ?? game.iphoneInstallDevice ?? installDeviceID   // 从装着它的那台删
        phoneOutcome = PhoneOutcome(device: installTargetLabel)
        start(.installRemove, game: game, name: game.name) { [weak self] sink in
            do { _ = try await self?.provider.removeFromPhone(gameId: game.id, accept: accept, device: device, events: sink) }
            catch let f as JobFailure where ask && Self.confirmCodes[f.code ?? ""] != nil {
                self?.dropConfirm = DropConfirm(kind: .installRemove, game: game, message: f.errorDescription ?? "", code: f.code ?? "",
                                                accept: accept.union([Self.confirmCodes[f.code ?? ""]!]), device: device)
                throw f
            }
            return nil
        }
    }

    static func requirementsText(_ game: GameSummary) -> String {
        game.compatibility == "unsupported" ? "这款游戏暂不支持。" : "这款还缺文件：" + game.missing.joined(separator: "；")
    }

    // ---- play ----

    func play(_ game: GameSummary, uiLanguage: String?) {
        guard canPlay(game.id) else { return }
        playLanguage = uiLanguage ?? "en"; addrShortNoticed = false
        failAction(nil, game: nil)
        // 崩了能恢复（11-13 维护者改定：开局前不问）：上次开着某几条时异常退出，作业层已当场关掉（pack/jobs/cheats.py note_crash）→
        //   这次照常开局，同时弹一条通知说关了什么、想用去设置里再开；读不到记录照常开局
        Task {
            let names = (try? await crashedBeforePlay(game.id)) ?? []
            if !names.isEmpty {
                crashPrompt = CrashPrompt(game: game, uiLanguage: uiLanguage, names: names)
                _ = try? await saveJob("communitySave", gameID: game.id, extra: ["crashAnswer": "ack"])
            }
            // 上一局读存档出事、且有上一份存档：先问要不要回到上一份（PreviousSavePromptHost），玩家选了再开局
            if let prompt = await previousSavePromptIfNeeded(game, uiLanguage: uiLanguage) {
                previousSavePrompt = prompt
                return
            }
            playChecked(game, uiLanguage: uiLanguage)
        }
    }

    func startAfterPreviousSave(_ game: GameSummary, uiLanguage: String?) { playChecked(game, uiLanguage: uiLanguage) }

    private func crashedBeforePlay(_ gameID: String) async throws -> [String] {
        let result = try await saveJob("community", gameID: gameID, extra: [:])
        // 帧率补丁那条（kind fps）、满屏开关那条（kind fs，自适应满屏一族跟它走）没有补丁名（作业层 cheats.note_crash / note_stall）：交 CrashPromptHost 按界面语言写「帧率」「满屏」
        return (result["crashed"] as? [[String: Any]] ?? []).compactMap {
            switch $0["kind"] as? String { case "fps": return CrashPrompt.fpsName; case "fs": return CrashPrompt.fullscreenName; default: return $0["name"] as? String } }
    }

    private func playChecked(_ game: GameSummary, uiLanguage: String?) {
        guard canPlay(game.id) else { return }
        // 不按前端的环境检查结果拦（剃刀 M4）：launch 作业自己校验运行文件与环境，缺什么回真实原因
        // 开了 iCloud 存档：先取回 / 处理冲突再开局（S2ArmLauncherModel+CloudSaves）；同步失败本地存档没动，照常开局、错误行写原因
        if cloudFolder != nil {
            cloudSyncing = true
            Task {
                let why = await cloudSync(game.id)
                cloudSyncing = false
                // 同步期间别的作业（出这款 / 导入 / 删除…）可能已经开始：按开局判据再核一次，不行就不开局（11-09 审查）
                if !canPlay(game.id) { failAction(Self.cloudBusyAfterSync, game: game.id); return }
                startPlay(game, uiLanguage: uiLanguage)
                if let why { failAction(Self.cloudFailure(why), game: game.id) }
            }
            return
        }
        startPlay(game, uiLanguage: uiLanguage)
    }

    private func startPlay(_ game: GameSummary, uiLanguage: String?) {
        // The helper accepts one consumer. Release the launcher's reader before
        // the host can connect, including launches initiated by the control port.
        gamepad.releaseRelay()
        let relaySocket = controller.relaySocketForGame()
        var controllerArguments: [String: String] = [
            "switch2ProAutoConnect": relaySocket == nil ? "0" : "1",   // 有通道就让游戏自己接（还没连上时游戏会重试）
        ]
        if let relaySocket { controllerArguments["switch2ProRelaySocket"] = relaySocket }
        if xboxGipRelayStarted { controllerArguments["xboxGipRelaySocket"] = xboxGipSocketURL.path }
        // 本地玩家（#623）：顶栏选的玩家随 launch 请求给作业层（pack/jobs/build.py runtime_env → 服务层 S2A_PLAYER_USER / 这个玩家的 SaveData）；没建过玩家不带（旧行为）
        if let player = Self.launchPlayer() {
            controllerArguments["playerUser"] = player.serviceUser
            controllerArguments["playerName"] = player.name
            controllerArguments["playerFolder"] = player.folder
        }
        if let listJSON = Self.localPlayersJSON() {
            controllerArguments["localPlayers"] = listJSON
        }
        controllerGameStarted = false
        do {
            let sink = EventSink { [weak self] event in self?.ingest(event, slot: .game) }
            running = try provider.run(InstalledGame(gameId: game.id, name: game.name),
                                       settings: S2ArmAppleAdapter.Settings(
                                        values: controllerArguments,
                                        uiLanguage: uiLanguage), events: sink)
            playSession = JobSession(kind: .play, gameID: game.id, name: game.name)
        } catch {
            failAction(error.localizedDescription, game: game.id)
        }
    }

    func startControllerConnection() {
        controller.startIfEnabled()
        let env = ProcessInfo.processInfo.environment
        // 单测里不起（会去独占真实 USB 手柄）：XCTestConfigurationFilePath 只有 Xcode 跑测试才设，swift test 不设，另按 XCTest 是否已加载判
        guard !xboxGipRelayStarted,
              env["XCTestConfigurationFilePath"] == nil, NSClassFromString("XCTestCase") == nil,
              env["S2ARM_CONTROL"] != "1" else { return }
        let logsDir = provider.client.environment.privateRoot.appendingPathComponent("logs", isDirectory: true)
        try? FileManager.default.createDirectory(at: logsDir, withIntermediateDirectories: true)
        s2a_xbox_gip_relay_set_log_path(logsDir.appendingPathComponent("xbox-gip-relay.log").path)
        if s2a_xbox_gip_relay_start(xboxGipSocketURL.path, nil, nil) == 0 {
            xboxGipRelayStarted = true
        }
    }

    // ---- 本地玩家（#623，规则在共享包 SaveSync LocalPlayers，同 iPhone / iPad）----

    /// 本机的玩家表（App 偏好，不在游戏库文件夹里）；firstDefault = 第一个玩家没改过名时显示的名字（按界面语言）
    static func playerStore(firstDefault: String = "Player 1", defaults: UserDefaults = .standard) -> LocalPlayers {
        LocalPlayers(defaults: defaults, firstDefault: firstDefault)
    }
    /// 起局要带的玩家：没建过玩家 = nil（旧行为，作业层不设玩家）
    static func launchPlayer(_ defaults: UserDefaults = .standard) -> LocalPlayer? {
        defaults.object(forKey: LocalPlayers.listKey) == nil ? nil : playerStore(defaults: defaults).current
    }
    /// 作业请求里的 player 字段（起局 pack/jobs/build.py、导入社区存档 saves.py 都按 saves.requested_player 校验）；没建过玩家 = nil
    static func playerRequest(_ defaults: UserDefaults = .standard) -> [String: String]? {
        launchPlayer(defaults).map { ["user": $0.serviceUser, "name": $0.name, "folder": $0.folder] }
    }
    /// 全部本地玩家的 JSON 数组串，供多人手柄起局时同步进 Switch AccountManager（与 NS 多用户行为一致）
    static func localPlayersJSON(_ defaults: UserDefaults = .standard) -> String? {
        let items = playerStore(defaults: defaults).all.map {
            ["user": $0.serviceUser, "name": $0.name, "folder": $0.folder]
        }
        guard let data = try? JSONSerialization.data(withJSONObject: items, options: []),
              let str = String(data: data, encoding: .utf8) else { return nil }
        return str
    }
    func shutdownControllerConnection() {
        controller.shutdown()
        if xboxGipRelayStarted {
            s2a_xbox_gip_relay_stop()
            xboxGipRelayStarted = false
        }
    }

    // ---- 放密钥 / 删除 / 诊断（本机控制口用；参数与作业同旧启动器）----

    /// 放密钥：作业页显示进度，完了重读游戏库（keysReady 跟着更新）。
    func importKeys(_ file: URL) {
        guard !busy else { return }
        failAction(nil, game: nil)
        inlineFailure = nil
        keysRejected = false
        if !showAddGames { showKeys = true }
        start(.keys, game: nil, name: file.lastPathComponent) { [weak self] sink in
            _ = try await self?.provider.importKeys(file, events: sink)
            return nil
        }
    }

    // ---- 存放位置（游戏库结构 v3）----

    /// 重读各存放位置（出包页打开时调；剩余空间是当时的）。
    func loadLocations(probe: String? = nil) {
        Task { [weak self] in
            guard let self else { return }
            do {
                let r = try await self.provider.locations(probe: probe); self.readOK("s2arm.fail.locations")
                self.lastLocation = r.last; self.locations = r.rows
                if let probe, let slack = r.probeSlack { self.probedSlack[probe] = slack }
            } catch { self.reportFailure("s2arm.fail.locations", error, passive: true) }   // 读不到就保留上次的列表
        }
    }

    /// 出包页「存放位置」下拉框一项显示什么名字（批量出包页、存储总览行首同）：具体路径（家目录缩写成 ~），不是卷名——
    /// 同一块盘上的几个位置卷名一样（内置盘一律「系统盘」），只给卷名分不清是哪个文件夹；卷名收进来是为了在判据里明确不取它
    nonisolated static func locationName(path: String, volume: String) -> String {
        (path as NSString).abbreviatingWithTildeInPath
    }

    /// 存放位置下面那行提示（#444），只提示不拦；按原因说对：大分配单元「每个小文件至少占 N、多占数倍」/ 不是 APFS「不能克隆、多占一份」。没问题给 nil
    func locationSlackText(_ path: String, language: String) -> String? {
        let row = locations.first(where: { $0.path == path })
        guard let slack = row != nil ? row?.slack : probedSlack[path] ?? nil else { return nil }
        if slack.reason == "cluster" {
            let size = ByteCountFormatter.string(fromByteCount: slack.clusterBytes, countStyle: .binary)
            return BackendText.launcherText("export.location.slack_cluster", language: language).replacingOccurrences(of: "{0}", with: size)
        }
        return BackendText.launcherText("export.location.slack_format", language: language)
    }

    /// 优先恢复上次手动选择；还没选过时沿用这款现在的位置、后端上次用的位置、默认位置。
    /// 出包页缺省存放位置，玩家自己定过的优先（维护者 11-15「玩家自行选择优先级更高」）：
    ///   ① 给这款手动选过的文件夹 → ② 这款现在放的位置 → ③ 上次手动选的（只给还没放过的新游戏）→ ④ 上次出包的位置 / 缺省位置
    func defaultLocation(for game: GameSummary) -> String {
        if let picked = provider.settings.exportLocation(forGame: game.id), !picked.isEmpty { return picked }
        if game.location.state == "ok" || game.location.state == "offline", !game.location.path.isEmpty { return game.location.path }
        if let saved = provider.settings.exportLocation, !saved.isEmpty { return saved }
        return locations.first(where: { $0.id == lastLocation && $0.connected })?.path ?? locations.first(where: \.isDefault)?.path ?? ""
    }

    func rememberExportLocation(_ path: String, game: GameSummary) {
        do { try provider.settings.setExportLocation(path, game: game.id) } catch { choiceError = error.localizedDescription }
    }

    /// 移动到其他位置（整个文件夹，存档跟着走）。
    func moveGame(_ game: GameSummary, to folder: URL) {
        guard !gameJobHeld(game.id) else { return }   // 主槽在出别的款：照常搬（进按款作业槽）
        failAction(nil, game: nil)
        start(.moveGame, game: game, name: game.name) { [weak self] sink in
            try await self?.provider.moveGame(game.id, to: folder.path, events: sink)
            self?.loadLocations()
            return nil
        }
    }

    /// 清除缓存（出包缓存 + 运行时着色器缓存；镜像、存档不动）。
    /// 任何作业在跑都不起（整库口径，同 Windows PackHoldsAll）：后端 library.clean 拿执行锁独占（.execution.lock）+ 清单锁，
    ///   还清全体共用的编译缓存 cache/air、cache/pso（prune_build retain_compiler=False），别的款出包正读写它们
    func clearCache(_ game: GameSummary) {
        guard !busy else { refuseNotice(game); return }
        failAction(nil, game: nil)
        libraryNotice = LibraryNotice(kind: .running, key: "s2arm.notice.clearCache", name: game.name)
        start(.clearCache, game: game, name: game.name) { [weak self] sink in
            try await self?.provider.clearCache(game.id, events: sink)
            return nil
        }
    }

    /// 「找不到游戏文件」→ 重新指定文件夹 / 从游戏库移除：很快，不进作业页，失败写在游戏库页上。
    func relinkGame(_ game: GameSummary, folder: URL) {
        Task { [weak self] in
            do { try await self?.provider.relinkGame(game.id, folder: folder.path); self?.refresh() }
            catch { self?.failAction(error.localizedDescription, game: game.id) }
        }
    }

    func forgetGame(_ game: GameSummary) {
        Task { [weak self] in
            do { try await self?.provider.forgetGame(game.id); if self?.selectedID == game.id { self?.selectedID = nil }; self?.refresh() }
            catch { self?.failAction(error.localizedDescription, game: game.id) }
        }
    }

    /// 在访达中选中这款的文件夹（catalog 给的 location.folder：按索引解析、可能在玩家另选的存放位置，不按游戏号拼）；文件夹不在就显示所在的存放位置；
    /// 两个都不在 → 系统提示框说明原因，不在访达里打开空窗口。
    func reveal(_ game: GameSummary) {
        guard let path = [game.location.folder, game.location.path].first(where: { !$0.isEmpty && FileManager.default.fileExists(atPath: $0) }) else {
            explainMissing("s2arm.reveal.folderMissing"); return
        }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }

    /// 存档位置（services/S2A.Services/SaveFolder.cs）：这款游戏文件夹里的 SaveData/，没有就建出来再在访达里打开（打开的是文件夹里面，玩家直接往里放）
    ///   本地玩家（#623）：当前玩家的那个（第一个玩家 SaveData，其他人 SaveData-<名字>，同 iOS）
    func openSaveFolder(_ game: GameSummary) {
        guard !game.location.folder.isEmpty, FileManager.default.fileExists(atPath: game.location.folder) else { explainMissing("s2arm.reveal.folderMissing"); return }
        let url = URL(fileURLWithPath: game.location.folder).appendingPathComponent(Self.launchPlayer()?.folder ?? LocalPlayers.firstFolder, isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        NSWorkspace.shared.open(url)
    }
    /// 存档位置里放的存档上次起局没读进来的原因（服务层写在数据根 saves/savedata-problem.txt，读进来后删掉）；没有 = nil
    func saveFolderProblem(_ game: GameSummary) -> String? {
        guard !game.location.folder.isEmpty else { return nil }
        // 本地玩家（#623）：原因按玩家分文件（services SaveFolder.ProblemPath），第一个玩家仍是原文件名
        let player = Self.launchPlayer()
        let file = URL(fileURLWithPath: game.location.folder).appendingPathComponent(player.map { $0.isFirst ? "saves/savedata-problem.txt" : "saves/savedata-problem-\($0.uid).txt" } ?? "saves/savedata-problem.txt")
        guard let text = try? String(contentsOf: file, encoding: .utf8) else { return nil }
        let why = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return why.isEmpty ? nil : why
    }

    /// 「在访达中显示 .ipa」：装机时组装、整个游戏库共用的那份（默认位置根 S2Arm.ipa）；不在 → 提示框说明，不打开空窗口
    func revealIpa(_ path: String) {
        guard !path.isEmpty, FileManager.default.fileExists(atPath: path) else { explainMissing("s2arm.reveal.ipaMissing"); return }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }

    /// 失败如实报（剃刀 G9 M12 / M13）：原先 try? 吞掉，读失败当成「没有」、切换失败界面停在旧值像点了没反应。
    ///   弹一行原因（标题按 titleKey，正文是后端 / 系统给的原因句）；值保持原样，不拿失败当结果。
    ///   passive（打开页面时的被动刷新）：同一类连续失败只报头一次，成功一次后复位（readOK）
    /// 弹提示框的出口：生产照常 runModal；单测注入成记录（单测里走到真 NSAlert 会在开发机桌面弹窗、并把那条用例卡住等人点，11-16）
    var runAlert: (NSAlert) -> NSApplication.ModalResponse = { LauncherWindowPresenter.current.runAlert($0) }

    func reportFailure(_ titleKey: String, _ error: Error, passive: Bool = false) {
        if passive { guard failingReads.insert(titleKey).inserted else { return } }
        let alert = NSAlert()
        alert.messageText = Localizer().t(titleKey)
        alert.informativeText = error.localizedDescription
        if Self.bundleIncomplete(error) { offerFullDownload(alert) } else { _ = runAlert(alert) }
    }
    func readOK(_ titleKey: String) { failingReads.remove(titleKey) }

    /// 最新完整安装包（GitHub「latest」固定链接，跳到当前最新版的 DMG）
    static let fullDownloadURL = URL(string: "https://github.com/jhaizhou-ops/S2Arm/releases/latest/download/S2Arm-mac-arm64.dmg")!

    static func bundleIncomplete(_ error: Error) -> Bool { (error as? JobClientError)?.code == JobClientError.bundleIncomplete }

    /// App 包不完整（内置 Python 缺失等）：不只说一句「请重新安装」，直接给「下载完整安装包」按钮，点了用浏览器下载最新 DMG
    func offerFullDownload(_ alert: NSAlert = NSAlert(), message: String? = nil) {
        if let message { alert.messageText = Localizer().t("s2arm.bundleIncomplete.title"); alert.informativeText = message }
        alert.informativeText += "\n\n" + Localizer().t("s2arm.bundleIncomplete.howto")
        alert.addButton(withTitle: Localizer().t("s2arm.bundleIncomplete.download"))
        alert.addButton(withTitle: Localizer().t("s2arm.bundleIncomplete.later"))
        if runAlert(alert) == .alertFirstButtonReturn { NSWorkspace.shared.open(Self.fullDownloadURL) }
    }

    private func explainMissing(_ key: String) {
        let alert = NSAlert()
        alert.messageText = Localizer().t(key)
        _ = runAlert(alert)
    }

    /// 删除 / 清缓存现在起不了：游戏库顶部那条说在等谁。那条正显示着进行中的删除 / 清缓存（如按款作业槽在删别的款）时不盖掉它——
    ///   否则那一单结束时找不到进行中的提示，结果就丢了
    private func refuseNotice(_ game: GameSummary) {
        if libraryNotice?.kind == .running { return }
        libraryNotice = LibraryNotice(kind: .failed, key: "s2arm.notice.waitBusy", name: game.name, detail: busySession?.name ?? "")
    }

    /// 删这款（IPA / 资源包 / 中间产物 / 缓存；存档与原始游戏文件保留）。
    func deleteGame(_ game: GameSummary) {
        if packHolds(game.id) { queueDelete(game); return }
        guard !gameJobHeld(game.id) else { refuseNotice(game); return }
        failAction(nil, game: nil)
        libraryNotice = LibraryNotice(kind: .running, key: "s2arm.notice.delete", name: game.name)
        start(.delete, game: game, name: game.name) { [weak self] sink in
            try await self?.provider.deleteGame(game.id, events: sink)
            if self?.selectedID == game.id { self?.selectedID = nil }
            return nil
        }
    }

    /// 出包时点了删这款的：等这款不再有出包在跑或排着就删（游戏号 → 点删除时的那款）
    @Published private(set) var queuedDeletes: [String: GameSummary] = [:]

    /// 排进去（同一款再点不重复排），顶部那条提示说「出完包就删」、带取消
    private func queueDelete(_ game: GameSummary) {
        queuedDeletes[game.id] = game
        libraryNotice = LibraryNotice(kind: .queued, key: "s2arm.notice.delete", name: game.name, gameID: game.id)
    }

    /// 顶部提示上的「取消」：这款不删了；还有别的款在排，提示换成那一款的（不然它没处取消）
    func cancelQueuedDelete() {
        guard let n = libraryNotice, n.kind == .queued, let id = n.gameID else { return }
        queuedDeletes[id] = nil
        libraryNotice = queuedDeletes.values.first.map { LibraryNotice(kind: .queued, key: "s2arm.notice.delete", name: $0.name, gameID: $0.id) }
    }

    /// 排着删的款不再有出包在跑或排着：走正常删除（结果写进同一条提示）。还被别的库类作业（删 / 搬别的款、清缓存…）占着就再等它结束；
    ///   被手机槽 / 游戏槽占着的照常拒绝、说在等谁。一次起一款，其余等这一单结束（finish）再接
    func pumpQueuedDeletes() {
        guard let game = queuedDeletes.values.first(where: { !packHolds($0.id) && (!gameJobHeld($0.id) || phoneBusy || playBusy) }) else { return }
        queuedDeletes[game.id] = nil
        if libraryNotice?.kind == .queued { libraryNotice = nil }   // 让正常删除 / 拒绝的提示接上这一条
        deleteGame(games.first { $0.id == game.id } ?? game)
    }

    /// 改名字 / 换封面（nil 不动，空字符串恢复默认）：很快，不进作业页，失败写在出包页上。
    func customize(_ game: GameSummary, name: String? = nil, coverPath: String? = nil) {
        guard !busy, !customizing else { return }
        customizing = true; customizeError = nil
        Task {
            do { try await provider.customizeGame(game.id, name: name, coverPath: coverPath, events: lockWaitSink()); refresh() }
            catch { customizeError = error.localizedDescription }
            customizing = false; lockWaitNote = nil
        }
    }

    /// 导出 Mac 端诊断包（path）或拉回连着的 iPhone 上的诊断（path 为 nil）；做完可在访达里显示。
    func diagnostics(exportTo path: URL?) {
        guard !busy else { return }
        failAction(nil, game: nil)
        start(.diagnostics, game: nil, name: path?.lastPathComponent ?? "iPhone") { [weak self] sink in
            guard let self else { return nil }
            if let path { try await self.provider.exportDiagnostics(to: path, events: sink); return path.path }
            return try await self.provider.pullDiagnostics(events: sink)["outputPath"] as? String
        }
    }

    // ---- 装到 iPhone 时带主屏小组件（11-16，缺省开；设置写 launch-settings.json 的 homeWidget）----

    @Published private(set) var homeWidgetError: String?
    var homeWidget: Bool { provider.settings.homeWidget }

    func setHomeWidget(_ on: Bool) {
        homeWidgetError = nil
        objectWillChange.send()
        do { try provider.settings.setHomeWidget(on) } catch { homeWidgetError = error.localizedDescription }
    }

    // ---- 自己选择 Xcode（D-9WFW：装在扩展硬盘等自动找不到的位置；设置写 launch-settings.json 的 xcodeApp，选过就优先用）----

    @Published private(set) var xcodeChooseError: String?
    var chosenXcode: String? { provider.settings.xcodeApp }
    /// 现在作业用的 Xcode（Contents/Developer）；还没找到为空串
    var activeDeveloperDir: String { provider.client.environment.developerDir }

    /// 选一份 Xcode.app（nil = 改回自动查找）；不是完整的 Xcode 不记、就近说原因。记下后重查环境，作业立刻改用它
    func setXcode(_ app: URL?) {
        xcodeChooseError = nil
        if let app, XcodeLocator.developerDir(app: app.path) == nil { xcodeChooseError = "s2arm.env.xcode.invalid"; objectWillChange.send(); return }
        objectWillChange.send()
        do { try provider.settings.setXcodeApp(app?.path) } catch { xcodeChooseError = error.localizedDescription; return }
        checkEnvironment()
    }

    // ---- 启动游戏时自动全屏（#575，缺省关；设置写 launch-settings.json 的 launchFullscreen，单款显式设过的照单款）----

    @Published private(set) var launchFullscreenError: String?
    var launchFullscreen: Bool { provider.settings.launchFullscreen }

    func setLaunchFullscreen(_ on: Bool) {
        launchFullscreenError = nil
        objectWillChange.send()
        do { try provider.settings.setLaunchFullscreen(on) } catch { launchFullscreenError = error.localizedDescription }
    }

    // ---- 允许 AI 操作 S2Arm（本机控制口，见 S2ArmControl）----

    /// 开关状态（--control / S2ARM_CONTROL=1 启动时恒为开）。
    @Published private(set) var aiControl = false
    @Published private(set) var aiControlError: String?
    private var control: S2ArmControl?

    /// 窗口出来时调：开关开着（或强制开）就起控制口。
    public func startControl() {
        aiControl = provider.settings.aiControl || ControlPort.forced
        if aiControl { openControl() }
    }

    /// 环境页的开关：记住选择，当场起 / 停控制口。
    func setAIControl(_ on: Bool) {
        aiControlError = nil
        do { try provider.settings.setAIControl(on) } catch { aiControlError = error.localizedDescription }
        aiControl = on || ControlPort.forced
        if aiControl { openControl() } else { control?.stop(); control = nil }
    }

    private func openControl() {
        guard control == nil else { return }
        do {
            control = try S2ArmControl(model: self) { [weak self] why in
                // 起不来 / control.json 写不上：不算已开，开关退回关，原因摆在环境页（审计 18:00 P2-35，同旧启动器）
                guard let self else { return }
                self.control = nil
                if !ControlPort.forced { try? self.provider.settings.setAIControl(false); self.aiControl = false }
                self.aiControlError = why
                ControlPort.reportStartFailure(privateRoot: self.provider.client.environment.privateRoot, why: why)
            }
        } catch {
            aiControlError = error.localizedDescription
            ControlPort.reportStartFailure(privateRoot: provider.client.environment.privateRoot, why: error.localizedDescription)
            if !ControlPort.forced { try? provider.settings.setAIControl(false); aiControl = false }
        }
    }

    /// 关掉界面上的提示 / 错误（同旧启动器「知道了」）。
    func dismissErrors() { failAction(nil, game: nil); environmentError = nil; aiControlError = nil; installError = nil }

    /// 密钥齐没齐（游戏库读回来的；nil = 还没读过）。
    var keysReady: Bool? { provider.library.keysReady }

    // ---- jobs ----

    /// 起一个作业：body 交适配层（事件进 sink），回来的路径做完后可在访达里显示。可停（stop = 取消这个 Task，适配层请作业停）。
    func start(_ kind: JobSession.Kind, game: GameSummary?, name: String,
                       _ body: @escaping @MainActor (EventSink) async throws -> String?) {
        start(JobSession(kind: kind, gameID: game?.id, name: name), body)
    }

    /// 作业槽：手机作业（JobSession.isPhoneJob）进手机槽，一局进游戏槽；主槽在跑时起的删除 / 搬家（sideKinds，入口已按 gameJobHeld 判过）
    ///   进按款作业槽，各一条（side 带它的号）；其余进主槽
    enum Slot: Equatable { case main, phone, game, side(UUID) }
    private func job(_ slot: Slot) -> JobSession? {
        switch slot {
        case .main: return session; case .phone: return phoneSession; case .game: return playSession
        case .side(let id): return sideJobs.first { $0.id == id }?.session
        }
    }
    private func setJob(_ slot: Slot, _ value: JobSession?) {
        switch slot {
        case .main: session = value; case .phone: phoneSession = value; case .game: playSession = value
        case .side(let id):   // 只动这一条：别的按款作业、主槽的出包照旧
            guard let i = sideJobs.firstIndex(where: { $0.id == id }) else { return }
            if let value { sideJobs[i].session = value } else { sideJobs.remove(at: i) }
        }
    }

    func start(_ fresh: JobSession, _ body: @escaping @MainActor (EventSink) async throws -> String?) {
        let slot: Slot
        if fresh.isPhoneJob { slot = .phone } else if fresh.kind == .play { slot = .game }
        else if mainBusy, Self.sideKinds.contains(fresh.kind) { let side = SideJob(session: fresh); sideJobs.append(side); slot = .side(side.id) }
        else { slot = .main }
        setJob(slot, fresh)
        if Self.phoneKinds.contains(fresh.kind) { phoneResult = nil }
        if fresh.isPhoneJob { pushChoice = nil }   // 上一次「还没选」的失败页换成这一单了
        let sink = EventSink { [weak self] event in self?.ingest(event, slot: slot) }
        let task = Task { [weak self] in
            do {
                let reveal = try await body(sink)
                self?.finish(.completed, message: "", reveal: reveal, slot: slot)
            } catch let failure as JobFailure {
                self?.finish(failure.state, message: failure.errorDescription ?? "", code: failure.code,
                             suggestion: failure.outcome.failureDiagnostic?["suggestion"] as? String ?? "", slot: slot)
            } catch is CancellationError {
                self?.finish(.cancelled, message: "", slot: slot)
            } catch is ExportIncomplete {
                self?.finish(.failed, message: "", slot: slot)
            } catch {
                self?.finish(.failed, message: error.localizedDescription, slot: slot)
                if Self.bundleIncomplete(error) { self?.offerFullDownload(message: error.localizedDescription) }
            }
        }
        switch slot { case .main: work = task; case .phone: phoneWork = task; case .game: playWork = task; case .side(let id): sideWork[id] = task }
    }

    func ingest(_ event: EngineEvent, slot: Slot = .main) {
        guard var session = job(slot), !isFinished(session) else { return }
        if let id = event.fields["taskId"] as? String, !id.isEmpty, session.taskID != id { session.taskID = id; setJob(slot, session) }
        if event.ev == "heartbeat" { return }
        if event.ev == "space", let space = PackSpace(event.fields["space"]) { session.space = space; setJob(slot, session); return }
        if let stage = event.stage, !stage.isEmpty {
            session.stage = stage
            if event.ev == "stage", session.stages.last != stage { session.stages.append(stage) }
            if session.kind == .play, event.fields["stageKey"] as? String == "game_running" {   // 后端固定机读名（build.py begin key），不认中文阶段名
                if let id = session.gameID { home.recordPlay(id) }
                controllerGameStarted = true
                controller.gameDidStart()
            }
        }
        // 地址空间不够这款（宿主黄行 → 作业层 notice addr_short，pack/jobs/build.py addr_short_watch）：Mac 宿主没有确认页，游戏照常跑，
        //   这里在游戏窗口之上弹一条不阻塞的提示条（文案同 iPhone 那页），玩家对着卡住的画面能知道是设备地址空间不够。每局一次
        if session.kind == .play, event.ev == "notice", (event.code ?? event.fields["code"] as? String) == "addr_short", !addrShortNoticed {
            addrShortNoticed = true
            AddrShortBanner.show(title: BackendText.launcherText("play.addrShort.title", language: playLanguage),
                                 body: BackendText.launcherText("play.addrShort.body", language: playLanguage))
        }
        session.progress.ingest(event)
        if !event.message.isEmpty {
            session.message = event.message
            let line = event.ev == "stage" ? "▸ " + event.message : event.message
            if session.log.last != line { session.log.append(line) }
            if session.log.count > Self.logLimit { session.log.removeFirst(session.log.count - Self.logLimit) }
        }
        setJob(slot, session)
        // 一局的结局只从事件来（run 是同步起的）；别的作业由 Task 结束收尾（中间可能有好几个作业，如先装 .NET 再出包）
        if session.kind == .play, event.isFinal {
            let state = JobOutcome.State(rawValue: event.fields["state"] as? String ?? "") ?? .interrupted
            finish(state, message: event.message, slot: slot)
            // 游戏里点了「切换游戏」（宿主菜单栏，pack/jobs/build.py launch 结果带 returnToLibrary）：不停在结果页，回到游戏库并切到前台，玩家直接点下一款。
            //   只关这一局的结果页，主槽的出包会话（在跑或结果页）照旧
            if state == .completed, (event.fields["result"] as? [String: Any])?["returnToLibrary"] as? Bool == true {
                setJob(slot, nil)
                NSApp.activate(ignoringOtherApps: true)
            }
        }
    }

    private func isFinished(_ s: JobSession) -> Bool { if case .finished = s.phase { return true }; return false }

    func finish(_ state: JobOutcome.State, message: String, reveal: String? = nil, code: String? = nil, suggestion: String = "", slot: Slot = .main) {
        guard var session = job(slot), !isFinished(session) else { return }
        session.phase = .finished(state)
        session.failureCode = code; session.suggestion = suggestion
        if !message.isEmpty {
            session.message = message
            if session.log.last != message { session.log.append(message) }
        }
        session.reveal = reveal
        session.progress.finish()
        session.ended = Date()
        setJob(slot, session)
        if session.isPhoneJob { lastPhoneSession = session; dynamicFinished(state) }
        // 出包 / 推完 / 装完：game.json 的产物记录和设备记录才会变，建议更新和总控台再读一次（开关关着不读）
        if phoneOverview != nil, session.isPhoneJob || [.export, .prepare].contains(session.kind) { refreshUpdateHints(); refreshPhoneOverview() }
        switch slot { case .main: work = nil; case .phone: phoneWork = nil; case .game: running = nil; playWork = nil; case .side(let id): sideWork[id] = nil }
        defer { pumpPhone() }   // 手机槽空了 / 主槽或游戏槽这单不再占着那款：起排着的手机作业
        if session.kind == .play, controllerGameStarted {
            controllerGameStarted = false
            controller.gameDidExit()
        }
        // 放密钥、添加游戏、欢迎页上的下载：结果留在原页（弹窗 / 欢迎页那一行），不换到日志页（11-08 维护者）；环境页发起的下载成了也直接回来
        if isInline(session) || (state == .completed && Self.repairKinds.contains(session.kind)) {
            setJob(slot, nil)
            inlineFailure = (state == .failed || state == .interrupted) ? (session.kind, session.message, session.suggestion) : nil
        }
        if state == .completed && session.kind == .install { installDone = session.gameID }
        if session.kind == .installUpdate { phoneUpdateEnded(completed: state == .completed) }
        // 导出确认页要先出包的款：出包没成（起不来 / 失败 / 取消）就没排进 installAfterQueue，计划清掉——
        //   否则这款以后正常出包会被接着导出、批量「更新并重新出」排进来的装机会误走导出
        if session.kind == .export, let gid = session.gameID, !installAfterQueue.contains(gid) { exportAfterPack[gid] = nil }
        if session.isPhoneJob || (session.kind == .export && !installAfterQueue.isEmpty) { Task { pumpQueue() } }   // 手机作业结束：排着的装机 / 等它的出包接上   // 导出模式出完包排了导出（runExport / exportSink）
        if case .side = slot { Task { pumpQueue() } }   // 按款作业槽的删 / 搬做完：主槽那单出包结束时没接上的下一单（等它）接着出
        if [.delete, .clearCache].contains(session.kind), let n = libraryNotice, n.kind == .running {   // 删除 / 清缓存的结果写进游戏库页顶部那条提示
            libraryNotice = LibraryNotice(kind: state == .completed ? .done : .failed, key: n.key, name: n.name,
                                          detail: state == .completed ? "" : (session.message.isEmpty ? JobOutcome.interruptedMessage : session.message))
        }
        // 用户手动取消不提示失败胶囊（只有真的成了 / 失败才在顶栏显示）
        if Self.phoneKinds.contains(session.kind), let id = session.gameID, state != .cancelled { phoneResult = (id, session.name, state == .completed) }
        // 一局结束：开了 iCloud 存档就上传这局的进度（失败写在出包页错误行上，本地存档不动）
        if session.kind == .play, let id = session.gameID, cloudFolder != nil {
            cloudSyncing = true
            Task {
                let why = await cloudSync(id)
                cloudSyncing = false
                if let why { failAction(Self.cloudFailure(why), game: id) }
            }
        }
        if state == .completed && session.kind == .diagnostics, let reveal { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: reveal)]) }
        if session.kind == .export, let id = session.gameID {
            finishedExports[id] = session
            if batchRunning {   // 批量出包：下一款由编排作业的 step 事件换（batchStepStarted），这里不管
            } else if state == .cancelled {
                exportQueue = []
            } else if !exportQueue.isEmpty, !sideBusy {   // 按款作业槽还在删 / 搬：等它做完（pumpQueue）再接
                let next = exportQueue.removeFirst()
                self.session = nil
                runExport(next)
            }
        }
        if state == .completed && session.kind == .keys {
            if pendingImport.isEmpty {
                closeImport()
            } else {
                let urls = pendingImport
                pendingImport = []
                submitImport(urls, partialResults: true)
            }
        }
        if state == .completed && session.kind == .importGames && importUsesResults &&
            pendingImport.isEmpty && pendingSources.isEmpty && importNoBase.isEmpty {
            closeImport()
        }
        if Self.repairKinds.contains(session.kind) {
            if state == .completed, !repairQueue.isEmpty { repair(repairQueue.removeFirst()) } else if state != .completed { repairQueue = [] }
        }
        pumpQueuedDeletes()   // 出包结束（成 / 败 / 取消）或占着的库类作业做完：排着删的款接着删
        refresh()   // 运行文件 / 存档 / 游戏库状态可能变了
    }

    private static let repairKinds: Set<JobSession.Kind> = [.downloadMetal, .downloadIosPlatform, .installDotnet]

    /// 这个作业的进度留在原页（弹窗 / 欢迎页），不换到作业日志页。
    func isInline(_ s: JobSession) -> Bool {
        [.keys, .importGames, .install, .installRemove, .installRestore, .delete, .clearCache, .diagnostics].contains(s.kind) || (showsWelcome && Self.repairKinds.contains(s.kind))
    }

    /// 欢迎页的「一键出包」：进和游戏库同一张出包页（维护者 11-11：首次出包缺设置项、选不了 iPhone / Mac），
    ///   平台、存放位置、目标设备、安装方式都在那页由玩家定；同 Windows 欢迎页（StartOneClick → 出包页）。
    func oneClickExport() {
        guard let game = welcomeGame else { return }
        exportFor = game.id
        welcomeGameID = nil
    }

    func clearInlineFailure() { inlineFailure = nil }

    /// 停手机槽的作业（装机页 / 手机作业页的「取消」）
    func stopPhone() {
        guard var s = phoneSession, s.phase == .running else { return }
        s.phase = .stopping; phoneSession = s
        phoneWork?.cancel()
    }
    /// 停按款作业槽里的这一条（游戏库顶部那行的「取消」）：只停它，主槽的出包照旧
    func stopSide(_ id: UUID) {
        guard var s = job(.side(id)), s.phase == .running else { return }
        s.phase = .stopping; setJob(.side(id), s)
        sideWork[id]?.cancel()
    }
    /// 按款作业做完、玩家看过结果：关掉那一行（还在跑的不关）
    func dismissSide(_ id: UUID) {
        guard let s = job(.side(id)), isFinished(s) else { return }
        setJob(.side(id), nil)
    }

    /// 手机作业结束后从它的作业页回游戏库（更新 iPhone 上的 S2Arm 用整页显示）
    func backFromPhone() {
        guard let s = phoneSession, isFinished(s) else { return }
        phoneSession = nil
        if dropConfirm?.kind == s.kind { dropConfirm = nil }
        pumpPhone()
    }

    /// 停主槽的作业（出包 / 导入 / 下载…的「取消」）
    func stop() {
        guard var session = session, session.phase == .running else { return }
        session.phase = .stopping
        self.session = session
        if batchRunning, session.kind == .export { batchWork?.cancel() }   // 批量出包：取消整个编排作业（正在出的那步停下，已出好的保留）
        else { work?.cancel() }
    }

    /// 停这一局（一局页面的「停止」）：请作业层关掉游戏，结局从事件来
    func stopPlay() {
        guard var s = playSession, s.phase == .running else { return }
        s.phase = .stopping; playSession = s
        if let running { provider.control(running, .stop) } else { playWork?.cancel() }
    }

    /// 回库：只有作业结束了才能关掉作业页。
    /// 页面返回 / 欢迎页「进入游戏库」：出包还在跑就留在后台，游戏库上的进度条可以回去。
    func goToLibrary() {
        exportFor = nil; viewingResult = nil; showEnvironment = false; welcomeHome = false; welcomeGameID = nil
        if let phone = phoneSession, phone.kind == .installUpdate { if isFinished(phone) { phoneSession = nil } else { phoneHidden = true } }
        if let play = playSession { if isFinished(play) { playSession = nil } else { playHidden = true } }   // 一局同主槽：结束了就清、没结束就收起
        guard let current = session else { return }
        if isFinished(current) { session = nil } else { sessionHidden = true }
    }

    func backToLibrary() {
        if viewingResult != nil { viewingResult = nil; return }
        if let play = playSession, !playHidden {   // 最上面是一局的页面：只关它（结束了才关），下面的出包会话不动
            if isFinished(play) { playSession = nil }
            return
        }
        sessionHidden = false
        guard let current = session, isFinished(current) else { return }
        session = nil
    }
}
