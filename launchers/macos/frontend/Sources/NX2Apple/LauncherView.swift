// Derived from NX2Apple frontend/Sources/NX2Apple/LauncherView.swift (908e4e84, MIT; see ../../UPSTREAM.md).
// S2Arm changes: one page, the Library (the Builds and Settings pages, engine sync banner, artwork editor, Manage menu, console mode
// are not taken); the state is S2ArmLauncherModel (adapter services) instead of LauncherStores; while a game runs
// the library gives way to JobSessionView (the launch job's events and Stop), and Back returns to the library. Second cut: the top bar
// adds Add games (open panel, or drop files on the window → import job) and the environment check (EnvironmentView); the options
// sheet is joined by the install-to-iPhone sheet (InstallView); any job (prepare, install, import) runs in JobSessionView. Export
// (11-07): a game's Export opens ExportView in place of the library (platforms, options, start), and an export runs in
// ExportSessionView (platform queue, steps, time left, per-platform results); the free-up-space sheet (FreeSpaceView) and
// change library (JobSessionView, setup job) sit beside them.
// Switch 2 Pro: the top bar adds the controller button and popover (Switch2ProConnectionPopover), and the library starts the
//   controller connection on appear when the user enabled it (NS2LauncherController).
// Console Home: selected banners fill the window behind the titlebar; toolbar actions use compact, labeled icons.
// Headings: brand and version share a baseline, reserving the native titlebar actions' measured width.
import AppKit
import SwiftUI
import UniformTypeIdentifiers
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

struct LauncherView: View {
    @ObservedObject var model: S2ArmLauncherModel
    @ObservedObject var home: HomeLibraryState
    @ObservedObject var navigation: LauncherPadNavigation
    @EnvironmentObject var loc: Localizer
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @Namespace var consoleSpace   // iPhone 总控台：首位卡手机轮廓升到页顶的共享元素过渡（LauncherView+PhoneConsole）
    @StateObject var padFocus = PadFocus()
    @StateObject var covers = CoverCache()
    @State var updatePopupIsFeature = false
    @State var installFor: String?
    @State var spaceFor: String?
    @State var moreFor: String?
    @State var saveFor: String?
    /// 拖到卡片上的存档：随存档窗口一起打开、先检查（SaveManageSheet.source）
    @State var saveDrop: URL?
    /// 拖动经过的那张卡片（描边提示）
    @State var dropCard: String?
    @State var displayFor: String?
    @State var inputFor: String?
    @State var deleteFor: GameSummary?
    /// 「找不到游戏文件」的那款（点了卡片）：重新指定文件夹 / 从游戏库移除
    @State var missingFor: GameSummary?
    @State private var showController = false
    @State private var dropTargeted = false
    @State private var showMessages = false
    @State private var updateErrorDismissed = false   // 更新失败 toast 按 × 关掉；状态一变（重试 / 再失败）就重置
    @State private var titlebarActionsWidth: CGFloat = 0
    @State var showHomeSearch = false
    @State var showHomeMore = false
    @State private var returnToInstallFromEnv: String?
    @State private var returnToDisplayFromInput: String?
    @State private var returnToEnvFromSpace = false
    let brand: String

    init(model: S2ArmLauncherModel, brand: String) {
        self.model = model; self.brand = brand
        self.home = model.home; self.navigation = model.gamepad
    }

    /// What the host's own messages use: the launch job takes zh-Hans or en.
    var uiLanguage: String { loc.lang.sharedCode }

    var body: some View {
        presentedContent
            .environment(\.padControls, navigation)
            .controllerButtonStyle(DefaultButtonStyle())
            .toggleStyle(PadToggleStyle(SwitchToggleStyle()))
    }

    private var presentedContent: some View {
        VStack(spacing: 0) {
            topBar
            if !showingHome { Divider().opacity(0.3) }
            noticeBar
            phoneUpdateBar
            libraryNoticeBar
            sideJobsBar
            if model.batchShown {
                BatchExportView(model: model, covers: covers, install: { model.openExportSheet($0) }, freeSpace: { spaceFor = $0 }, uiLanguage: uiLanguage)
            } else if let result = model.viewingResult, let run = result.export {
                ExportSessionView(model: model, covers: covers, session: result, run: run, install: { model.openExportSheet($0) }, freeSpace: { spaceFor = $0 }, uiLanguage: uiLanguage)
            } else if let play = model.playSession, !model.playHidden {   // 一局（游戏槽）：盖在出包等主槽作业页上面，收起或关掉后露出下面那页
                PlaySessionView(model: model, covers: covers, session: play, uiLanguage: uiLanguage)
            } else if let session = model.session, let run = session.export, !model.sessionHidden {
                ExportSessionView(model: model, covers: covers, session: session, run: run, install: { model.openExportSheet($0) }, freeSpace: { spaceFor = $0 }, uiLanguage: uiLanguage)
            } else if let session = model.session, session.export == nil, !model.sessionHidden, !model.isInline(session) {
                JobSessionView(model: model, session: session)
            } else if let phone = model.phoneSession, phone.wholePage, !model.phoneHidden {   // 更新 iPhone 上的 S2Arm / 更新所选没走成（手机槽）：主槽没占着整页时整页显示
                JobSessionView(model: model, session: phone)
            } else if let id = model.exportFor, let game = model.games.first(where: { $0.id == id }) {
                ExportView(model: model, covers: covers, game: game)
            } else {
                libraryPage
                    .overlay {
                        if dropTargeted {
                            RoundedRectangle(cornerRadius: 18).strokeBorder(LauncherPalette.cream, style: StrokeStyle(lineWidth: 2, dash: [8, 6]))
                                .padding(16).allowsHitTesting(false)
                        }
                    }
                    .onDrop(of: [UTType.fileURL], isTargeted: $dropTargeted) { providers in
                        Self.fileURLs(providers) { urls in
                            // 拖进来的是 prod.keys 就当密钥放，别当游戏报错
                            if urls.count == 1, urls[0].pathExtension.lowercased() == "keys" { model.importKeys(urls[0]) } else { model.importGames(urls, partialResults: true) }
                        }
                        return true
                    }
            }
        }
        .overlay(alignment: .bottom) { updateErrorToast }
        .animation(.easeOut(duration: 0.2), value: model.updateState)
        .animation(.easeOut(duration: 0.2), value: model.checkError)
        .onChange(of: model.updateState) { _ in updateErrorDismissed = false }
        .padScope("main", padFocus)
        .environmentObject(padFocus)
        .launcherDropdownHost()
        .ignoresSafeArea(.container, edges: .top)
        .background {
            if showingHome {
                HomeBackdrop(game: homeSelected, covers: covers).ignoresSafeArea()
            } else if let backdropGame {
                HomeBackdrop(game: backdropGame, covers: covers, frosted: true).ignoresSafeArea()
            } else {
                LauncherWindowMaterial().ignoresSafeArea()
            }
        }
        .background(LauncherWindowChrome())
        .background(LauncherPadHost(navigation: navigation, update: configurePadNavigation))
        .onReceive(model.controller.$deviceCount.dropFirst()) { _ in
            DispatchQueue.main.async { navigation.source.refreshPresentation() }
        }
        .preferredColorScheme(.dark)
        .tint(LauncherPalette.cream)
        .font(LauncherTheme.TypeStyle.detail)
        .labelStyle(LauncherLabelStyle())
        .frame(minWidth: 800, minHeight: 450)
        .onAppear { model.startup() }
        .sheet(isPresented: $model.sgradeUpgradeOpen) { SGradeUpgradeView(model: model).environmentObject(loc) }   // S 级优化升级（v0.4.0）
        .sheet(item: Binding(get: { installFor.map(OptionsTarget.init) }, set: { installFor = $0?.id })) { target in
            InstallView(model: model, gameID: target.id, onOpenSettings: { returnToInstallFromEnv = target.id }).environmentObject(loc)
        }
        .sheet(item: $model.exportSheet) { request in   // 导出确认页（签名 → 出法 → 设备多选 → 开始；开始后同一页看结果）
            ExportSheetView(model: model, request: request).environmentObject(loc)
        }
        .onChange(of: model.spaceRequest) { id in if let id { spaceFor = id; model.spaceRequest = nil } }   // 设置页「磁盘空间」
        .onChange(of: model.displayRequest) { id in if let id { displayFor = id; model.displayRequest = nil } }   // 控制口打开启动设置
        .sheet(item: Binding(get: { spaceFor.map(OptionsTarget.init) }, set: {
            spaceFor = $0?.id
            if $0 == nil, returnToEnvFromSpace {
                returnToEnvFromSpace = false
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { model.showEnvironment = true }
            }
        })) { target in
            FreeSpaceView(model: model, gameID: target.id).environmentObject(loc)
        }
        .sheet(item: Binding(get: { displayFor.map(OptionsTarget.init) }, set: { displayFor = $0?.id })) { target in
            LaunchSettingsSheet(model: model, gameID: target.id, openInput: {
                returnToDisplayFromInput = target.id
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { inputFor = target.id }
            }).environmentObject(loc)
        }
        .sheet(item: Binding(get: { saveFor.map(OptionsTarget.init) }, set: { saveFor = $0?.id; if $0 == nil { saveDrop = nil } })) { target in
            SaveManageSheet(model: model, gameID: target.id, source: saveDrop).environmentObject(loc)
        }
        .sheet(item: Binding(get: { inputFor.map(OptionsTarget.init) }, set: {
            inputFor = $0?.id
            if $0 == nil, let back = returnToDisplayFromInput {
                returnToDisplayFromInput = nil
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { displayFor = back }
            }
        })) { target in
            InputSettingsSheet(model: model, gameID: target.id).environmentObject(loc)
        }
        .sheet(item: Binding(get: { model.diagUploadFor.map(OptionsTarget.init) }, set: { model.diagUploadFor = $0?.id })) { target in   // 上传给开发者（游戏库菜单 / 控制口）
            UploadDiagnosticsSheet(model: model, gameID: target.id.isEmpty ? nil : target.id, operations: nil, fallback: "") { model.diagUploadFor = nil }.environmentObject(loc)
        }
        .background(DownstreamPromptHost(model: model))   // 下游深度方案确认页（DownstreamConfirmSheet.swift）
        .background(CrashPromptHost(model: model))
        .background(PreviousSavePromptHost(model: model))   // 上一局读存档出事：开始前问要不要回到上一份存档；回过去后提示可以换回来       // 上次开着金手指 / 补丁时异常退出：已当场关掉，开局时通知关了哪几条
        .alert(txt("game.delete.title").replacingOccurrences(of: "{0}", with: deleteFor?.name ?? ""), isPresented: Binding(get: { deleteFor != nil }, set: { if !$0 { deleteFor = nil } })) {
            Button(txt("game.cancel"), role: .cancel) { deleteFor = nil }
            Button(txt("game.delete.confirm"), role: .destructive) { if let g = deleteFor { model.deleteGame(g) }; deleteFor = nil }
        } message: { Text(txt("game.delete.body") + (deleteFor?.onPhone == true ? "\n\n" + txt("game.delete.onPhone") : "")) }
        .alert(txt("library.missing.title").replacingOccurrences(of: "{0}", with: missingFor?.name ?? ""), isPresented: Binding(get: { missingFor != nil }, set: { if !$0 { missingFor = nil } })) {
            Button(txt("library.missing.relink")) { if let g = missingFor { relinkGame(g) }; missingFor = nil }
            Button(txt("library.missing.forget"), role: .destructive) { if let g = missingFor { model.forgetGame(g) }; missingFor = nil }
            Button(txt("game.cancel"), role: .cancel) { missingFor = nil }
        } message: { Text(txt("library.missing.body")) }
        // 换游戏库撞上认得出的风险（云盘同步目录 / 有别的文件的文件夹，S2ArmLauncherModel.libraryConfirmCodes）：讲清后果，玩家选「仍然继续」带上对应参数重来（v0.3.1）
        .alert(BackendText.errorTitle(model.libraryConfirm?.code ?? "library_in_cloud", language: uiLanguage) ?? txt("install.drop_title"), isPresented: Binding(get: { model.libraryConfirm != nil }, set: { if !$0 { model.libraryConfirm = nil } })) {
            Button(txt("install.drop_confirm")) { if let c = model.libraryConfirm { model.libraryConfirm = nil; model.changeLibrary(to: c.url, move: c.move, accept: c.accept) } }
            Button(txt("phone.drop.cancel"), role: .cancel) { model.libraryConfirm = nil }
        } message: { Text(loc.backend(model.libraryConfirm?.message ?? "")) }
        .sheet(isPresented: $model.showAddGames) { ImportSheet(model: model, kind: .games).environmentObject(loc) }
        // 打开时最多一个更新弹窗（有新版 / 这版更新了什么），只给最新一期；关掉（含 Esc）= 这一版不再弹
        .sheet(isPresented: Binding(get: { isUpdatePopupPresented }, set: { if !$0, model.updatePopup != nil { model.closeUpdatePopup() } })) {
            if updatePopupIsFeature {   // 大版本首次打开的新功能大窗（docs/site/feature.json，updater.py feature_spotlight）
                FeatureSpotlightView(model: model, language: uiLanguage)
            } else {
                UpdatePopupView(model: model, language: uiLanguage)
            }
        }
        // 关窗淡出时 updatePopup 已清空：沿用最后一次的种类，不在淡出时换成另一种弹窗
        .onReceive(model.$updatePopup) { if let next = $0 { updatePopupIsFeature = next["kind"] as? String == "feature" } }
        .alert(txt("update.gate.confirm.title"), isPresented: Binding(get: { model.updateGate != nil && model.updatePopup == nil }, set: { if !$0 { model.updateGate = nil } })) {
            // 认得出的风险说清后让玩家选（11-18 维护者）：默认焦点在「取消」，「仍然重启」照样换版
            Button(txt("update.gate.confirm.cancel"), role: .cancel) { model.updateGate = nil }.keyboardShortcut(.defaultAction)
            Button(txt("update.gate.confirm.restart"), role: .destructive) { model.restartToUpdate(confirmed: true) }
        } message: { Text(model.updateRestartImpactText(txt)) }
        .sheet(isPresented: Binding(get: { model.showKeys && !model.showEnvironment }, set: { model.showKeys = $0 })) {
            ImportSheet(model: model, kind: .keys).environmentObject(loc)
        }
        .sheet(item: $model.devCertChoice) { DevCertChooseSheet(model: model, choice: $0).environmentObject(loc) }   // 几张证书：这台第一次连上时问用哪张（#487）
        .sheet(isPresented: $model.showEnvironment) {
            EnvironmentView(model: model, onOpenSpace: { returnToEnvFromSpace = true }).environmentObject(loc)
        }
        .onChange(of: model.showEnvironment) { shown in
            if !shown, !returnToEnvFromSpace, let back = returnToInstallFromEnv {
                returnToInstallFromEnv = nil
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { installFor = back }
            }
        }
    }

    /// 只有玩家发起更新后才显示进度。可用更新由顶栏的按钮提示。
    @ViewBuilder private var phoneUpdateBar: some View {
        if let percent = model.phoneUpdatePercent {
            HStack(spacing: 10) {
                ProgressView().controlSize(.small)
                Text(txt("phone.update.banner.progress").replacingOccurrences(of: "{0}", with: "\(percent)"))
                    .font(.manrope(13, .semibold)).monospacedDigit()
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 8)
            }
            .padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.vertical, 10)
            .background(LauncherPalette.cream.opacity(0.10))
            .accessibilityIdentifier("notice.phoneUpdate")
        }
    }

    /// 删除游戏 / 清除缓存的进行中与结果。
    @ViewBuilder private var libraryNoticeBar: some View {
        if let n = model.libraryNotice {
            let text = loc.t(n.key + "." + { switch n.kind { case .running: "running"; case .done: "done"; case .failed: "failed"; case .queued: "queued" } }())
                .replacingOccurrences(of: "{0}", with: n.name).replacingOccurrences(of: "{1}", with: loc.backend(n.detail))
            HStack(spacing: 10) {
                switch n.kind {
                case .running: ProgressView().controlSize(.small)
                case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                case .failed: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(LauncherPalette.danger)
                case .queued: Image(systemName: "clock").foregroundStyle(.secondary)
                }
                Text(text).font(.manrope(13, .semibold)).fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 8)
                if n.kind == .queued {   // 排着等出完包再删：能取消（不删了）
                    Button(loc.t("s2arm.job.cancel")) { model.cancelQueuedDelete() }.controllerButtonStyle(.plain).foregroundStyle(.secondary)
                        .accessibilityIdentifier("notice.library.cancel")
                } else if n.kind != .running {
                    Button { model.libraryNotice = nil } label: { Image(systemName: "xmark") }.controllerButtonStyle(.plain).foregroundStyle(.secondary)
                        .accessibilityIdentifier("notice.library.close")
                }
            }
            .padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.vertical, 10)
            .background((n.kind == .failed ? LauncherPalette.danger : LauncherPalette.cream).opacity(0.10))
            .accessibilityIdentifier("notice.library")
            .task(id: n) { if n.kind == .done { try? await Task.sleep(nanoseconds: 6_000_000_000); if model.libraryNotice == n { model.libraryNotice = nil } } }
        }
    }

    /// 按款作业槽（主槽出包时同时跑的别的款的搬家；删除的进度与结果走上面那条提示）：每条一行，各自的阶段、进度、取消 / 关闭，互不覆盖
    @ViewBuilder private var sideJobsBar: some View {
        ForEach(model.sideJobs.filter { !model.isInline($0.session) }) { side in
            let s = side.session
            let running = s.phase == .running || s.phase == .stopping
            HStack(spacing: 10) {
                switch s.phase {
                case .running, .stopping: ProgressView().controlSize(.small)
                case .finished(.completed): Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                case .finished: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(LauncherPalette.danger)
                }
                Text(s.name + " · " + (running ? loc.t(s.phase == .stopping ? "s2arm.job.stopping" : "s2arm.job.running")
                                       : JobSessionView.text(loc, "s2arm.job.done." + s.kind.rawValue, fallback: "s2arm.job.done"))
                     + (s.message.isEmpty ? "" : " · " + loc.backend(s.message)))
                    .font(.manrope(13, .semibold)).lineLimit(2)
                Spacer(minLength: 8)
                if let f = s.progress.fraction, running { ProgressView(value: f).frame(width: 120) }
                if running {
                    Button(loc.t("s2arm.job.cancel")) { model.stopSide(side.id) }.controllerButtonStyle(.plain).disabled(s.phase == .stopping)
                } else {
                    Button { model.dismissSide(side.id) } label: { Image(systemName: "xmark") }.controllerButtonStyle(.plain).foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.vertical, 10)
            .background(LauncherPalette.cream.opacity(0.10))
            .accessibilityIdentifier("side.job")
        }
    }

    static var displayVersion: String {
        let info = Bundle.main.infoDictionary
        if let shown = info?["S2ADisplayVersion"] as? String, !shown.isEmpty { return shown }
        return (info?["CFBundleShortVersionString"] as? String).map { "v" + $0 + "-dev" } ?? "dev"   // swift run 等不经 build_app 的开发构建
    }

    private var wordmark: some View {
        Image(nsImage: BrandArt.wordmark).resizable().interpolation(.high).frame(width: 108, height: 28)
    }
    // 界面作者署名；名字带作者主页链接，不加提示，当小彩蛋
    private var credit: some View {
        Text(Self.uiCredit).font(.manrope(9, .medium)).foregroundStyle(.secondary).tint(.secondary)
            .lineLimit(1).fixedSize().accessibilityIdentifier("topbar.credit")
    }
    private var version: some View {
        Text(verbatim: Self.displayVersion).font(.manrope(8.5, .medium)).monospacedDigit().foregroundStyle(.tertiary)
            .lineLimit(1).fixedSize().textSelection(.enabled).help(Self.displayVersion).accessibilityIdentifier("topbar.version")
    }
    /// 「UI by NX2Apple, SkyRiN & shenxianmq」：两位界面作者的名字各自链到主页，样式与正文一样（shenxianmq：11-17 首页 / 出包页 / 更新流程交互设计，#1498）。
    static let uiCredit: AttributedString = {
        var text = AttributedString("UI by NX2Apple, ")
        var skyrin = AttributedString("SkyRiN")
        skyrin.link = URL(string: "https://x.com/skyrin1008")
        text.append(skyrin)
        text.append(AttributedString(" & "))
        var shen = AttributedString("shenxianmq")
        shen.link = URL(string: "https://github.com/shenxianmq")
        text.append(shen)
        return text
    }()

    /// 非首页（出包页、批量出包、游玩/作业进度与结果页）复用当前游戏的高斯模糊背景，保持与首页一致的磨砂质感。
    private var backdropGame: GameSummary? {
        let id: String?
        if model.batchShown { id = model.batchSeed }
        else if let result = model.viewingResult, result.export != nil { id = result.gameID }
        else if let session = model.session, !model.sessionHidden, !model.isInline(session) { id = session.gameID }
        else if let idExport = model.exportFor { id = idExport }
        else { id = nil }
        return id.flatMap { gid in model.games.first { $0.id == gid } } ?? homeSelected
    }

    private var topBar: some View {
        topBarRow.onChange(of: model.showInstallFor) { id in if let id { installFor = id; model.showInstallFor = nil } }
            .appleLoginSheet(model, host: "main")   // 主窗口上没有别的弹层时（欢迎页、手机总控台的失败页）从这里弹登录面板
    }   // 控制口 /show install（单独一层：挂在 body 长链上编译器类型检查超时）

    private var topBarRow: some View {
        HStack(spacing: 12) {
            // 点品牌字标回欢迎页（社区贡献者补丁 #2）
            HStack(alignment: .center, spacing: LauncherTheme.Space.medium) {
                Button { installFor = nil; model.goToLibrary(); model.welcomeHome = true } label: {
                    // 字标 957×247，左边 247×247 是 S 图标：右侧按钮太宽（法语窄窗口）时只留图标，不被挤到红绿灯上
                    ViewThatFits(in: .horizontal) {
                        wordmark
                        wordmark.frame(width: 28, alignment: .leading).clipped()
                    }
                }
                .controllerButtonStyle(.plain).controllerID("top.brand").accessibilityLabel(brand).layoutPriority(1)
                // 窄窗口先收版本号、再收署名，不压右侧按钮
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: LauncherTheme.Space.medium) { credit; version }
                    credit
                    Color.clear.frame(width: 0, height: 0)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.leading, 90).padding(.trailing, max(24, titlebarActionsWidth + LauncherTheme.Space.medium))
        .frame(height: 52)   // = a unified toolbar's title bar: the system centres the traffic lights in it, so they share this bar's midline
        .background(TitlebarDoubleClick())
        .background(UnifiedTitlebar(actions: topActions, actionsWidth: $titlebarActionsWidth))
    }

    /// 顶栏右侧的按钮放进标题栏本身：工具栏盖在窗口内容上，内容里的按钮收不到真实点击（11-08 维护者）。
    private var topActions: some View {
        HStack(spacing: 8) {
                if model.loading { ProgressView().controlSize(.small) }
                exportPill
                backgroundTaskPill
                batchPill
                PhoneUpdateButton(model: model, language: uiLanguage)
                phonePill
                updateButton
                HStack(spacing: 4) {
                    // 0.6.0 动态窗口（通用壳开关后面，iPhone 总控台「五」）：有和 App 相关的作业时出现一颗圆钮——外圈进度环 + 一行小字，做完闪 ✓，
                    //   失败 / 停下留橙色「!」，点它到总控台；没有作业、开关关着：不出现（顶栏照 #1402 只留玩家 → 手柄 → 消息 → 设置 → 报修）
                    if consoleEnabled && (model.dynamicJob != nil || model.dynamicDone || model.dynamicAlert) {
                        Button { installFor = nil; model.openConsoleFromTopBar() } label: {
                            HStack(spacing: 6) {
                                ZStack {
                                    Image(systemName: model.dynamicAlert ? "exclamationmark" : model.dynamicDone ? "checkmark" : "square.grid.2x2")
                                        .foregroundStyle(model.dynamicAlert ? .orange : model.dynamicDone ? .green : .white)
                                    if let job = model.dynamicJob {
                                        Circle().trim(from: 0, to: CGFloat(job.fraction ?? 0.25)).stroke(LauncherPalette.cream, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                                            .rotationEffect(.degrees(-90)).frame(width: 26, height: 26)   // 拿不到进度时显示一段静止的环
                                    }
                                }
                                if let job = model.dynamicJob {
                                    Text(job.label + (job.fraction.map { " \(Int($0 * 100))%" } ?? "")).font(.manrope(11)).monospacedDigit().lineLimit(1).frame(maxWidth: 150)
                                }
                            }
                            .accessibilityLabel(loc.t("s2arm.nav.library")).accessibilityValue(model.dynamicJob?.label ?? "")
                        }
                        .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true)).launcherTooltip(model.dynamicJob.map(\.label) ?? loc.t("s2arm.nav.library"), placement: .below)
                        .animation(reduceMotion ? nil : .easeOut(duration: 0.3), value: model.dynamicDone)
                        .controllerID("top.library")
                    }
                    PlayerToolbarButton(loc: loc)   // 标题栏另起视图树，显式传入本地化对象。
                    ControllerToolbarButton(controller: model.controller, show: $showController,
                                            lang: loc.lang.code)
                    messagesButton
                    // 就绪 / 检查中不挂角标；环境缺项时用红点和无障碍说明提示玩家。
                    Button { model.showEnvironment = true } label: {
                        Image(systemName: "gearshape").overlay(alignment: .topTrailing) {
                            if environmentNeedsAction {
                                Circle().fill(LauncherPalette.danger).frame(width: 5, height: 5).offset(x: 5, y: -3)
                            }
                        }
                    }
                    .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true))
                    .launcherTooltip(environmentNeedsAction ? txt("nav.settings") + "：" + loc.t("s2arm.env.missing") : txt("nav.settings"), placement: .below)
                    .accessibilityLabel(txt("nav.settings"))
                    .accessibilityValue(environmentNeedsAction ? loc.t("s2arm.env.missing") : "")
                    .controllerID("top.settings")
                }
                // 报错页的 ReportHint 指向这里：所有页面保持同一个全局入口。
                Button { model.diagUploadFor = "" } label: {
                    Label(txt("report.button"), systemImage: "exclamationmark.bubble")
                }
                .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true)).launcherTooltip(txt("report.button"), placement: .below)
                .controllerID("top.report").accessibilityIdentifier("top.report")
        }
        .padding(.trailing, 6)
        .fixedSize()   // 放在标题栏里没有宽度约束，不固定的话按钮文字会被压成省略号
        .labelStyle(.titleAndIcon)   // Popovers keep their text; icon styles are local to each toolbar button.
        .tint(LauncherPalette.cream)
        .environment(\.colorScheme, .dark)
        .environment(\.padControls, navigation)
        .controllerButtonStyle(DefaultButtonStyle())
        .toggleStyle(PadToggleStyle(SwitchToggleStyle()))
    }

    /// A unified toolbar makes the (transparent) title bar as tall as the top bar, and macOS then centres the close / minimise / zoom
    /// buttons vertically in it. The right-hand actions are its one item, so real clicks reach them.
    private struct UnifiedTitlebar<Actions: View>: NSViewRepresentable {
        let actions: Actions
        @Binding var actionsWidth: CGFloat

        final class ActionsHost: NSHostingView<Actions> {
            var didLayout: (() -> Void)?
            override func layout() { super.layout(); didLayout?() }
        }

        final class Coordinator: NSObject, NSToolbarDelegate {
            let host: ActionsHost
            @Binding private var width: CGFloat
            init(_ actions: Actions, width: Binding<CGFloat>) {
                host = ActionsHost(rootView: actions); _width = width
                super.init()
                host.didLayout = { [weak self] in self?.reportWidth() }
            }
            func update(_ actions: Actions, width: Binding<CGFloat>) {
                _width = width; host.rootView = actions
                reportWidth()
            }
            private func reportWidth() {
                guard host.bounds.width > 0, abs(host.bounds.width - width) > 0.5 else { return }
                // This hosting view lives outside the page's SwiftUI preference tree.
                // Publish its actual AppKit width after layout, using the latest value.
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.host.window != nil else { return }
                    let value = self.host.bounds.width
                    if value > 0, abs(value - self.width) > 0.5 { self.width = value }
                }
            }
            func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { [.flexibleSpace] }
            func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { [.flexibleSpace] }
            func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier, willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? { nil }
        }

        final class Host: NSView {
            var coordinator: Coordinator?
            override func viewDidMoveToWindow() {
                super.viewDidMoveToWindow()
                guard let window, window.toolbar == nil else { return }
                let toolbar = NSToolbar(identifier: "s2arm.titlebar")
                toolbar.delegate = coordinator
                toolbar.displayMode = .iconOnly
                toolbar.allowsUserCustomization = false
                window.toolbar = toolbar
                window.toolbarStyle = .unified
                window.titlebarSeparatorStyle = .none
                // 工具栏只负责标题栏高度。按钮直接放进标题栏：放进工具栏项时 macOS 26 会在整组下面再垫一层玻璃胶囊，和按钮自己的胶囊叠在一起
                guard let close = window.standardWindowButton(.closeButton), let bar = close.superview, let host = coordinator?.host else { return }
                host.translatesAutoresizingMaskIntoConstraints = false
                bar.addSubview(host)
                NSLayoutConstraint.activate([host.trailingAnchor.constraint(equalTo: bar.trailingAnchor), host.centerYAnchor.constraint(equalTo: close.centerYAnchor)])
            }
        }

        func makeCoordinator() -> Coordinator { Coordinator(actions, width: $actionsWidth) }
        func makeNSView(context: Context) -> NSView { let h = Host(); h.coordinator = context.coordinator; return h }
        func updateNSView(_ nsView: NSView, context: Context) { context.coordinator.update(actions, width: $actionsWidth) }
    }

    private struct OptionsTarget: Identifiable { let id: String }

    /// 环境缺东西、要玩家处理（顶栏设置按钮挂红点）。readyMac（Mac 上能玩、只差打 iPhone 包的项）照旧算就绪、不挂：只玩 Mac 的玩家不该一直看到提醒，缺的项在设置页单列
    private var environmentNeedsAction: Bool { model.environmentState == .missing }

    /// 顶部公告条（v0.2.3）：醒目的琥珀色底、整条宽，压在内容上方但不盖住任何按钮；× 关掉后同一条不再出现。文字取自 notice.json，代码里不写公告内容
    @ViewBuilder private var noticeBar: some View {
        if let notice = model.notice, let text = (notice["text"] as? [String: String])?[uiLanguage] {
            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(LauncherPalette.building)
                (Text(txt("notice.label") + "  ").font(.manrope(13, .bold)) + Text(text).font(.manrope(13)))
                    .foregroundStyle(LauncherPalette.cream).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                Spacer(minLength: 8)
                if let link = (notice["url"] as? String).flatMap(URL.init(string:)) {
                    Button { NSWorkspace.shared.open(link) } label: { Text(txt("notice.details")).font(.manrope(12.5, .semibold)).underline() }
                        .controllerButtonStyle(.plain).foregroundStyle(LauncherPalette.cream)
                }
                Button { model.dismissNotice() } label: { Image(systemName: "xmark").font(.system(size: 11, weight: .bold)).frame(width: 22, height: 22).contentShape(Rectangle()) }
                    .controllerButtonStyle(.plain).foregroundStyle(LauncherPalette.cream.opacity(0.8)).help(txt("notice.close")).accessibilityLabel(txt("notice.close"))
            }
            .padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.vertical, 10)
            .background(LauncherPalette.building.opacity(0.16))
            .overlay(alignment: .bottom) { Rectangle().fill(LauncherPalette.building.opacity(0.45)).frame(height: 1) }
        }
    }

    @ViewBuilder private var updateButton: some View {
        switch model.updateState {
        case .working(_, let percent):
            let tip = downloadTooltip(percent: percent)
            Button { model.showUpdatePopup() } label: {
                // 平时就是这颗 arrow.down.circle；下载中外圈调成半透明当底轨，实色进度叠在同一圈上（和头像等圆形图标一样大）
                Image(systemName: "arrow.down.circle")
                    .symbolRenderingMode(.palette).foregroundStyle(.white.opacity(0.95), .white.opacity(0.3))
                    .overlay { downloadRing(percent: percent).padding(2.5).allowsHitTesting(false) }
            }
            .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true))
            .launcherTooltip(tip, placement: .below).accessibilityLabel(tip).controllerID("top.update")
        case .ready where model.showRestartUpdateButton:
            let title = txt("update.top.restart")
            Button { model.restartToUpdate() } label: {
                Text(title)
            }
            .controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule))
            .launcherTooltip(title, placement: .below).controllerID("top.update")
        case .ready:
            Image(systemName: "arrow.down.circle")
                .font(.system(size: 16, weight: .medium))
                .symbolRenderingMode(.palette).foregroundStyle(.white.opacity(0.95), .white.opacity(0.3))
                .overlay { downloadRing(percent: 100).padding(2.5) }
                .frame(width: 40, height: 40)
                .accessibilityHidden(true)
        case .idle where model.availableUpdate != nil:
            let version = model.availableUpdate?["version"] as? String ?? ""
            updateIndicator(txt("update.menu.available").replacingOccurrences(of: "{0}", with: version), dot: LauncherPalette.cream)
        case .failed(let code):
            updateIndicator(txt("update.error." + code), dot: LauncherPalette.danger)
        case .idle:
            EmptyView()
        }
    }

    private func updateIndicator(_ title: String, dot: Color) -> some View {
        Button { model.showUpdatePopup() } label: {
            Image(systemName: "arrow.down.circle").overlay(alignment: .topTrailing) {
                Circle().fill(dot).frame(width: 5, height: 5).offset(x: 5, y: -3)
            }
        }
        .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true))
        .launcherTooltip(title, placement: .below).accessibilityLabel(title).controllerID("top.update")
    }

    private func downloadTooltip(percent: Int?) -> String {
        percent.map { txt("update.menu.downloading").replacingOccurrences(of: "{0}", with: "\($0)") }
            ?? txt("update.menu.downloadingPlain")
    }

    @ViewBuilder private func downloadRing(percent: Int?) -> some View {
        if let percent {
            downloadArc(fraction: CGFloat(min(max(percent, 0), 100)) / 100)
                .animation(.easeOut(duration: 0.2), value: percent)
        } else if reduceMotion {
            downloadArc(fraction: 0.26)
        } else {
            TimelineView(.animation) { context in
                let turn = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1.2) / 1.2
                downloadArc(fraction: 0.26).rotationEffect(.degrees(turn * 360))
            }
        }
    }

    private func downloadArc(fraction: CGFloat) -> some View {
        Circle().trim(from: 0, to: fraction)
            .stroke(LauncherPalette.cream, style: StrokeStyle(lineWidth: 1.75, lineCap: .round))
            .rotationEffect(.degrees(-90))
    }

    private var isUpdatePopupPresented: Bool {
        model.updatePopup != nil && !model.showAddGames && !model.showKeys && !model.showEnvironment
            && model.appleLoginHost == nil && model.installViewOpen == nil && model.exportSheet == nil   // 别的弹层开着时等它关了再弹，不顶掉（D-6DR6）
    }

    @ViewBuilder private var updateErrorToast: some View {
        if model.updatePopup == nil {
            if let code = model.checkError {
                updateErrorToast(code: code, checkFailure: true)
            } else if case .failed(let code) = model.updateState, !updateErrorDismissed {
                updateErrorToast(code: code, checkFailure: false)
            }
        }
    }

    private func updateErrorToast(code: String, checkFailure: Bool) -> some View {
        VStack(alignment: .trailing, spacing: LauncherTheme.Space.small) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "exclamationmark.circle.fill").foregroundStyle(LauncherPalette.danger)
                    .padding(.top, 2).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    if checkFailure {
                        Text(txt("update.checkFailed.title")).font(.manrope(13, .bold)).foregroundStyle(LauncherPalette.cream)
                    }
                    Text(txt("update.error." + code)).font(.manrope(13, checkFailure ? .regular : .semibold))
                        .foregroundStyle(LauncherPalette.cream).fixedSize(horizontal: false, vertical: true)
                    Text(txt("update.code").replacingOccurrences(of: "{0}", with: code)).font(.manrope(11.5)).foregroundStyle(.secondary)
                }
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                Button {
                    withAnimation(.easeOut(duration: 0.2)) {
                        if checkFailure { model.checkError = nil }
                        else { updateErrorDismissed = true }
                    }
                } label: {
                    Image(systemName: "xmark").font(.system(size: 11, weight: .bold)).frame(width: 22, height: 22).contentShape(Rectangle())
                }
                .controllerButtonStyle(.plain).foregroundStyle(LauncherPalette.cream.opacity(0.8))
                .help(txt("notice.close")).accessibilityLabel(txt("notice.close"))
            }
            HStack(spacing: LauncherTheme.Space.small) {
                Button(txt("update.popup.retryNow")) {
                    if checkFailure { model.checkForUpdatesManually() }
                    else { model.downloadUpdate() }
                }
                .controllerButtonStyle(LauncherSecondaryButtonStyle())
                Button(txt("update.download_page")) { NSWorkspace.shared.open(S2ArmLauncherModel.downloadPage) }
                    .controllerButtonStyle(LauncherSecondaryButtonStyle())
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 12)
        .frame(maxWidth: 560)
        .background(LauncherPalette.danger.opacity(0.14), in: RoundedRectangle(cornerRadius: 12))
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(LauncherPalette.danger.opacity(0.5), lineWidth: 1).allowsHitTesting(false))
        .shadow(color: .black.opacity(0.35), radius: 16, y: 6)
        .padding(.horizontal, 24).padding(.bottom, 24)
        .transition(.move(edge: .bottom).combined(with: .opacity))
    }

    func txt(_ id: String) -> String { BackendText.launcherText(id, language: uiLanguage) }

    /// 「消息」：公告和更新说明都在这里（旧的只在这里、不弹）；未读只算最新那条公告
    private var messagesButton: some View {
        Button { showMessages = true; model.openMessages() } label: {
            Image(systemName: "bell").overlay(alignment: .topTrailing) {
                if model.unreadMessages > 0 {
                    Circle().fill(LauncherPalette.danger).frame(width: 6, height: 6).offset(x: 4, y: -3)
                }
            }
        }
        .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true)).launcherTooltip(txt("messages.button"), placement: .below)
        .accessibilityLabel(txt("messages.button")).accessibilityValue("\(model.unreadMessages)").controllerID("top.messages")
        .popover(isPresented: $showMessages, arrowEdge: .bottom) { MessagesPanel(model: model, language: uiLanguage) }
    }

    /// 非出包任务收起后，也保留一条可用手柄返回进度页的入口。主槽的任务和一局各一颗（两样可以同时在跑）
    @ViewBuilder private var backgroundTaskPill: some View {
        // 主槽的任务收起了、或被一局的页面压着：回到它的进度页（一局收起，顶栏留它自己那颗）
        if let session = model.session, session.export == nil, !session.isPhoneJob, !model.isInline(session), model.sessionHidden || model.playVisible {
            Button { model.sessionHidden = false; model.playHidden = true } label: {
                Label(loc.t("home.currentTask"), systemImage: "list.bullet.rectangle")
            }.controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule)).controllerID("top.currentTask")
                .accessibilityLabel(loc.t("home.currentTask") + " · " + session.name)
        }
        // 收起的一局：回到它的页面（写游戏名，和主槽那颗「当前任务」分得清）
        if let play = model.playSession, model.playHidden {
            Button { model.playHidden = false } label: {
                Label(play.name, systemImage: "gamecontroller").lineLimit(1)
            }.controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule)).controllerID("top.playTask")
                .accessibilityLabel(loc.t("home.currentTask") + " · " + play.name)
        }
        // 收起的、或被主槽整页（出包进度等）/ 一局的页面压着的「更新 iPhone 上的 S2Arm」：回到它的进度页
        if let phone = model.phoneSession, phone.wholePage,
           model.phoneHidden || model.playVisible || (model.session.map { !model.sessionHidden && ($0.export != nil || !model.isInline($0)) } ?? false) {
            Button { model.phoneHidden = false; model.sessionHidden = true; model.playHidden = true } label: {
                Label(phone.name, systemImage: "iphone").lineLimit(1)
            }.controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule)).controllerID("top.phoneTask")
        }
    }

    /// 批量出包（v0.2.4）：清单页没开着时，这批正在出 / 上次还有没出完的 → 顶部胶囊回到清单页
    @ViewBuilder private var batchPill: some View {
        if !model.batchShown, model.batchRunning || model.batchUnfinished > 0 {
            Button { model.batchShown = true } label: {
                HStack(spacing: 6) {
                    if model.batchRunning { ProgressView().controlSize(.mini) } else { Image(systemName: "list.bullet.rectangle") }
                    Text(model.batchRunning ? BackendText.launcherText("batch.title", language: uiLanguage)
                         : BackendText.launcherText("batch.unfinished", language: uiLanguage).replacingOccurrences(of: "{0}", with: "\(model.batchUnfinished)"))
                        .lineLimit(1)
                }
            }
            .controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule)).accessibilityIdentifier("top.batch")
        }
    }

    /// 收起运行页、或还有排队时：顶部这颗胶囊回到出包页。
    @ViewBuilder private var exportPill: some View {
        if let session = model.session, session.export != nil, model.sessionHidden || model.playVisible {   // 被一局的页面压着也给入口
            Button { model.sessionHidden = false; model.playHidden = true } label: {
                HStack(spacing: 6) {
                    if model.mainBusy { ProgressView().controlSize(.mini) } else { Image(systemName: "checkmark.circle.fill").foregroundStyle(LauncherPalette.ready) }
                    Text(txt(model.mainBusy ? "export.pill.running" : "export.pill.done").replacingOccurrences(of: "{0}", with: session.name)).lineLimit(1)
                    if !model.exportQueue.isEmpty {
                        Text(txt("export.pill.queue").replacingOccurrences(of: "{0}", with: "\(model.exportQueue.count)")).font(.manrope(12)).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
            }
            .controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule))
        }
    }

    /// 装机弹窗关着时：手机作业在跑 → 「正在推到 iPhone · 查看进度」，点了回到同一个作业（弹窗显示 model.session，不新起安装）；
    /// 跑完还没回去看 → 「已装好 / 没装完 · 查看结果」（#308，Mac 与 Windows 同一套入口）。
    @ViewBuilder private var phonePill: some View {
        if installFor == nil {
            if let s = model.phoneSession, S2ArmLauncherModel.phoneKinds.contains(s.kind), let id = s.gameID {
                Button { openPhoneResult(id) } label: {
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.mini)
                        Text(txt(model.selfSigning && s.kind == .install ? "install.pill.exporting" : "install.pill.running").replacingOccurrences(of: "{0}", with: s.name).replacingOccurrences(of: "{1}", with: pillTarget(id))).lineLimit(1)
                        if let f = s.progress.fraction { Text("\(Int(f * 100))%").font(.manrope(12)).monospacedDigit().foregroundStyle(.secondary) }
                    }
                }
                .controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule)).accessibilityIdentifier("install.pill")
            } else if let r = model.phoneResult {
                Button { openPhoneResult(r.gameID) } label: {
                    HStack(spacing: 6) {
                        Image(systemName: r.ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill").foregroundStyle(r.ok ? LauncherPalette.ready : LauncherPalette.danger)
                        Text(txt(!r.ok ? (model.selfSigning ? "install.pill.exportFailed" : "install.pill.failed") : (model.phoneOutcome?.exportFolder ?? "").isEmpty ? "install.pill.done" : "install.pill.exported").replacingOccurrences(of: "{0}", with: r.name)
                             .replacingOccurrences(of: "{1}", with: r.ok && !model.installedTo.isEmpty ? model.installedTo : model.installTargetLabel)).lineLimit(1)
                    }
                }
                .controllerButtonStyle(LauncherSecondaryButtonStyle(size: .compact, shape: .capsule)).accessibilityIdentifier("install.pill")
            }
        }
    }

    /// 胶囊点了：这款是从导出确认页开始的 → 回到那一页看逐台结果；否则照旧开装机窗
    private func openPhoneResult(_ id: String) {
        if model.exportFlows[id] != nil { model.openExportSheet(id, results: true) } else { installFor = id }
    }
    /// 胶囊里正在导入的那台：导出确认页逐台导入时写那一行的设备，不写界面上选中的那台
    private func pillTarget(_ id: String) -> String {
        guard let row = model.exportFlows[id]?.rows.first(where: { $0.state == .running }) else { return model.installTargetLabel }
        return ExportSheetView.deviceLabel(kind: row.kind, name: row.name, language: uiLanguage)
    }

    /// The file URLs of a drop, delivered together on the main queue.
    static func fileURLs(_ providers: [NSItemProvider], _ done: @escaping ([URL]) -> Void) {
        let group = DispatchGroup()
        let lock = NSLock()
        var urls: [URL] = []
        for provider in providers where provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
            group.enter()
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                if let url { lock.lock(); urls.append(url); lock.unlock() }
                group.leave()
            }
        }
        group.notify(queue: .main) { done(urls) }
    }
}

private struct ControllerToolbarButton: View {
    @ObservedObject var controller: NS2LauncherController
    @StateObject private var systems = SystemControllers()   // 系统手柄（PS5 / Xbox / 一代 Joy-Con…），面板「玩家」列表一起列（#520 F）
    @Binding var show: Bool
    let lang: String   // 界面语言代码（Lang.code）
    @Environment(\.padControls) private var padControls

    private var toolbarStatus: String {
        NS2LauncherController.statusText(controller.hasSystemController ? .connected : controller.status, lang: lang, bluetoothDenied: controller.bluetoothDenied)
    }
    /// 开面板：还没问过蓝牙权限就先弹系统授权框（不开面板，免得授权框抢焦点把面板关掉），选完再开、按钮已是对应状态
    private func toggle() {
        if show { show = false; return }
        if controller.shouldAskBluetoothFirst { controller.askBluetoothPermission { show = true } } else { show = true }
    }
    var body: some View {
        Button { toggle() } label: {
            Image(systemName: "gamecontroller").overlay(alignment: .topTrailing) {
                Circle().fill(Color(nsColor: controller.buttonColor)).frame(width: 5, height: 5).offset(x: 5, y: -3)
            }
        }
        .controllerButtonStyle(ConsoleIconButtonStyle(backgroundless: true))
        .launcherTooltip(controller.title(lang: lang) + "：" + toolbarStatus, placement: .below)   // #901：顶栏提示写实际连着的手柄名（两条通道合并看）
        .accessibilityLabel(controller.title(lang: lang) + "：" + toolbarStatus)
        .controllerID("top.controller")
        // 面板用 AppKit 的 NSPopover 自己开关，不用 SwiftUI .popover：实机上 .popover 会在背后重新呈现、套回打开时记下的高度，
        //   先没连、面板里连上（多出玩家区块）后再开齿轮选单 / 改开关，面板内容上下被裁（11-16，真窗口单测重现不出来）。
        //   高度照内容自动撑（NSHostingController.sizingOptions = .preferredContentSize），不写死
        // 开着「模擬手把」面板（本地 / 自测构建的开发工具）时，点面板外面不收手柄面板，方便两个对着测；点手柄钮 / 按 B 照常收
        .background(AppKitPopover(isPresented: $show, stayOpen: { ControllerSimulatorPanel.isOpen }) {
            Switch2ProConnectionPopover(controller: controller, systems: systems, lang: lang, onClose: { show = false })
                .environment(\.padControls, padControls)
        })
        // 蓝牙手柄接上 / 断开：分配跟着整理（新连上的自动排进玩家位，#520 E）；HID 兜底手柄同样（#1151）
        .onReceive(systems.$pads.combineLatest(systems.$hidNames)) { pads, hid in controller.updateSystem(SystemControllers.assignmentKeys(pads: pads, hidNames: hid)) }
        .onReceive(controller.$assignment) { systems.applyPlayerLights($0) }
        .onAppear {   // 模拟手柄（本地 / 自测构建）：按键送进分配；Joy-Con 2 连上 / 断开要重对分配（一代走 systems 的表，上面两行已接）
            guard ControllerColorSimulator.allowed, ControllerColorSimulator.shared.controller == nil else { return }   // 只接一次（onAppear 会来好几次）
            let target = controller
            ControllerColorSimulator.shared.controller = target
            ControllerColorSimulator.shared.onChange.append { [weak target] in target?.reconcile() }
        }   // 手柄上的玩家灯跟着分配（一代 Joy-Con、系统手柄；Joy-Con 2 由连接程序写）
        .onReceive(systems.$gen1) { controller.updateGen1($0) }   // 一代 Joy-Con 按设备（D2）   // 用送来的新值（@Published 在改之前就送，读属性还是旧的）
        // 游戏打开了系统的手柄设置画面：面板自己弹出来（已进分配玩家）
        .onChange(of: controller.panelRequested) { wanted in if wanted { controller.panelRequested = false; if !show { toggle() } } }
    }
}

/// 用 AppKit NSPopover 呈现 SwiftUI 内容：开关由 isPresented 决定，内容大小跟着 SwiftUI 的理想大小走（preferredContentSize），
///   不会被 SwiftUI 在背后重新呈现、套回旧高度。点外面关（transient），关了把 isPresented 设回 false
private struct AppKitPopover<Content: View>: NSViewRepresentable {
    @Binding var isPresented: Bool
    var stayOpen: () -> Bool = { false }   // true 时点外面不收（只有 isPresented 改成 false 才收）
    let content: () -> Content

    final class Coordinator: NSObject, NSPopoverDelegate {
        var parent: AppKitPopover
        var popover: NSPopover?
        var host: NSHostingController<Content>?
        var requested = false   // 这次关是 isPresented 改成 false 要的（不是点外面）
        init(_ parent: AppKitPopover) { self.parent = parent }
        func popoverShouldClose(_ popover: NSPopover) -> Bool { requested || !parent.stayOpen() }
        func popoverDidClose(_ notification: Notification) {
            requested = false
            popover = nil; host = nil
            if parent.isPresented { parent.isPresented = false }
        }
    }
    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeNSView(context: Context) -> NSView { NSView() }
    func updateNSView(_ view: NSView, context: Context) {
        let c = context.coordinator
        c.parent = self
        if isPresented {
            if let host = c.host { host.rootView = content(); return }
            guard view.window != nil else { return }
            let host = NSHostingController(rootView: content())
            host.sizingOptions = .preferredContentSize
            let popover = NSPopover()
            popover.contentViewController = host
            popover.behavior = .transient
            popover.appearance = NSAppearance(named: .darkAqua)
            popover.delegate = c
            c.host = host; c.popover = popover
            DispatchQueue.main.async { [weak view] in
                guard let view, view.window != nil, c.popover === popover else { return }
                LauncherWindowPresenter.current.showPopover(popover, view.bounds, view, .minY)   // 按钮下方
            }
        } else if let popover = c.popover {
            c.requested = true
            LauncherWindowPresenter.current.closePopover(popover)
        }
    }
    static func dismantleNSView(_ view: NSView, coordinator: Coordinator) { if let popover = coordinator.popover { LauncherWindowPresenter.current.dismissPopover(popover) } }
}
