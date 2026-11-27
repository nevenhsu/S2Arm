// Derived from NX2Apple LauncherView+Library.swift (908e4e84, MIT; see ../../UPSTREAM.md).
// S2Arm: the Home layout lives in LauncherView+Home; this file keeps library status,
// management actions and file drops shared with that layout.
import AppKit
import Darwin
import SwiftUI
import UniformTypeIdentifiers
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

extension LauncherView {
    var libraryPage: some View {
        GeometryReader { geometry in
            if model.games.isEmpty && !model.libraryLoaded {
                loadingLibrary
            } else if model.showsWelcome {
                emptyLibrary
            } else {
                homeLibrary(size: geometry.size)
            }
        }
    }

    /// 第一次读游戏库还没回来：只显示「正在读取」，不先闪「还没有游戏」（11-07 界面验收）。
    var loadingLibrary: some View {
        VStack(spacing: 14) {
            ProgressView().controlSize(.regular)
            Text(loc.t("s2arm.library.loading")).font(.manrope(15)).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    /// 游戏库还是空的：欢迎页兼环境检查（11-08 维护者），有游戏后回到游戏库。
    var emptyLibrary: some View {
        WelcomeView(model: model)
    }

    func cardTargeted(_ id: String) -> Binding<Bool> {
        Binding(get: { dropCard == id }, set: { dropCard = $0 ? id : (dropCard == id ? nil : dropCard) })
    }
    static let gameFileTypes: Set<String> = ["nsp", "xci", "nsz", "xcz", "nup"]   // 同后端 pack/jobs/library.py SOURCE_SUFFIXES（#611：漏了 Switch 2 的 .nup，拖到卡片上会被当成存档）
    func dropOnCard(_ urls: [URL], game: GameSummary) {
        guard let first = urls.first else { return }
        let ext = first.pathExtension.lowercased()
        if urls.count == 1, ext == "keys" { model.importKeys(first); return }
        if urls.contains(where: { Self.gameFileTypes.contains($0.pathExtension.lowercased()) }) { model.importGames(urls, partialResults: true); return }
        saveDrop = first; saveFor = game.id
    }

    /// 显示的是上次的列表（刷新失败）：标明是上次的、说原因、给重试。后台正常刷新时只在顶栏转轻菊花，避免首屏横幅占位导致封面排下移再上跳。
    @ViewBuilder var staleBanner: some View {
        if model.showingSnapshot && !model.games.isEmpty && model.libraryError != nil {
            HStack(spacing: 12) {
                Image(systemName: "exclamationmark.arrow.triangle.2.circlepath").foregroundStyle(LauncherPalette.danger)
                Text(txt("library.stale.failed").replacingOccurrences(of: "{0}", with: loc.backend(model.staleLibraryError ?? ""))).font(.manrope(13.5, .semibold))
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 12)
                Button(txt("library.stale.retry")) { model.refresh() }.controllerButtonStyle(LauncherSecondaryButtonStyle()).accessibilityIdentifier("library.stale.retry")
            }
            .padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.vertical, 10).background(LauncherPalette.danger.opacity(0.10))
        }
    }

    func moreMenu(_ game: GameSummary) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            VStack(alignment: .leading, spacing: 6) {
                Text(txt("library.sizes.title")).font(.manrope(11, .semibold)).foregroundStyle(.secondary)
                GameSizesList(sizes: game.sizes, compact: true)
            }
            .padding(.horizontal, 10).padding(.vertical, 6)
            Divider().opacity(0.3).padding(.vertical, 4)
            if model.finishedExports[game.id] != nil, !(model.mainBusy && model.session?.gameID == game.id) {   // 这款上次出包 / 装机 / 导出的结果页（原首页圆钮）
                moreRow(txt("library.hero.results"), "checkmark.seal") { model.viewingResult = model.finishedExports[game.id] }
            }
            if game.playable || game.iphoneReady {   // 出包页（其它平台 / 重新出包）：首页那颗胶囊收进这里
                moreRow(loc.t("s2arm.export.action"), "shippingbox") { model.exportFor = game.id }
            }
            if game.appleTvReady {   // #1429：出好 Apple TV 包的款，游戏库里也有「装到 Apple TV」（原先只在出包页 / 出包结果页，离开就找不到）
                moreRow(BackendText.launcherText("export.installAppleTV", language: loc.lang.sharedCode), "appletv") { model.installAppleTV(game) }
                    .disabled(model.busy)
            }
            moreRow(loc.t("s2arm.action.freeSpace"), "externaldrive") { spaceFor = game.id }
            moreRow(txt("game.display"), "slider.horizontal.3") { displayFor = game.id }
            moreRow(txt("game.saves"), "tray.full") { saveFor = game.id }
            moreRow(txt("game.input"), "gamecontroller") { inputFor = game.id }
            moreRow(txt("game.diag"), "paperplane") { model.diagUploadFor = game.id }   // 上传给开发者；上传没成在弹窗里退回导出
            if !game.location.path.isEmpty {
                moreRow(txt("library.menu.reveal"), "folder") { model.reveal(game) }
            }
            Divider().opacity(0.3).padding(.vertical, 4)
            moreRow(txt("game.delete"), "trash", danger: true) { deleteFor = game }
        }
        .padding(8).frame(width: 220).font(.manrope(13.5, .semibold)).preferredColorScheme(.dark)
        .controllerBackAction("home.more.close") { moreFor = nil }
    }

    private func moreRow(_ title: String, _ symbol: String, danger: Bool = false, action: @escaping () -> Void) -> some View {
        let activate = { moreFor = nil; action() }
        return Button(action: activate) {
            MoreMenuRowLabel(title: title, symbol: symbol, danger: danger)
        }
        .buttonStyle(.plain)
        .controllerControl(activate: activate)
        .controllerRingRadius(LauncherTheme.Focus.rowRadius)
    }

    /// 游戏卡片右键菜单（游戏库结构 v3）：开始游戏、出包、启动设置、在访达中显示、移动到其他位置、清除缓存、删除
    @ViewBuilder func cardMenu(_ game: GameSummary) -> some View {
        let reachable = game.location.state != "offline" && game.location.state != "missing"
        if game.playable && reachable {
            Button { model.play(game, uiLanguage: uiLanguage) } label: { Label(loc.t("library.play"), systemImage: "play.fill") }
        }
        Button { model.exportFor = game.id } label: { Label(loc.t("s2arm.export.action"), systemImage: "shippingbox") }.disabled(!reachable)
        Button { displayFor = game.id } label: { Label(txt("library.menu.launchSettings"), systemImage: "gearshape") }.disabled(!reachable)
        Divider()
        Button { model.reveal(game) } label: { Label(txt("library.menu.reveal"), systemImage: "folder") }.disabled(game.location.path.isEmpty)
        Button { moveGame(game) } label: { Label(txt("library.menu.move"), systemImage: "arrow.right.doc.on.clipboard") }.disabled(!reachable || game.location.state == "none" || model.gameJobHeld(game.id))   // 这款被占着才灰（出别的款照常搬）
        Button { model.clearCache(game) } label: { Label(txt("library.menu.clearCache"), systemImage: "sparkles") }.disabled(!reachable || model.busy)   // 整库口径（clearCache 注释）
        Divider()
        Button(role: .destructive) { deleteFor = game } label: { Label(txt("game.delete"), systemImage: "trash") }
    }

    func moveGame(_ game: GameSummary) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        panel.prompt = BackendText.launcherText("export.location.choose", language: uiLanguage)
        if LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url { model.moveGame(game, to: url) }
    }

    func relinkGame(_ game: GameSummary) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false
        panel.message = txt("library.missing.pick")
        if LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url { model.relinkGame(game, folder: url) }
    }

    /// 卡片和大图上的可用状态。设备签名的到期时间在设备操作区显示。
    func statusLine(_ game: GameSummary) -> (text: String, color: Color) {
        if model.playHolds(game.id) { return (loc.t("s2arm.library.running"), LauncherPalette.building) }   // 「运行中」按游戏槽判
        if model.queuedDeletes[game.id] != nil { return (loc.t("s2arm.notice.delete.queued"), LauncherPalette.building) }   // 出包时点了删：等出完就删
        if model.mainJobs.contains(where: { $0.gameID == game.id }) { return (loc.t("s2arm.job.running"), LauncherPalette.building) }   // 主槽或按款作业槽在动它
        if model.exportQueue.contains(where: { $0.game.id == game.id }) { return (txt("export.status.queued"), LauncherPalette.building) }
        // 存放位置（游戏库结构 v3）：移动硬盘没插 → 灰色「未连接」，插回来自动恢复；文件夹没了 → 红字，点卡片重新指定或移除
        // 未连接给具体路径（同出包页下拉框）：卷名会重名（内置盘一律「系统盘」），看不出是哪个位置没连上
        if game.location.state == "offline" {
            let place = game.location.path.isEmpty ? game.location.volume : (game.location.path as NSString).abbreviatingWithTildeInPath
            return (txt("library.location.offline").replacingOccurrences(of: "{0}", with: place), LauncherPalette.unbuilt)
        }
        if game.location.state == "missing" { return (txt("library.location.missing"), LauncherPalette.danger) }
        var parts: [String] = []
        // 正在显示专门的签名状态时，那里已经写明推到了哪种设备。
        if game.iphoneInstalledAt != nil, model.freeSignDaysLeft(game) == nil {
            parts.append(txt("library.status.installedPaid").replacingOccurrences(of: "{0}", with: BackendText.deviceKind(game.iphoneInstallKind)))
        } else if game.iphoneReady, game.iphoneInstalledAt == nil {
            parts.append(txt("library.status.iphone"))
        }
        if game.playable { parts.append(loc.t("library.ready")) }
        if parts.isEmpty {
            if game.iphoneReady { return (txt("library.status.iphone"), LauncherPalette.ready) }
            return (loc.t("s2arm.library.notPrepared"), LauncherPalette.unbuilt)
        }
        return (parts.joined(separator: " · "), LauncherPalette.ready)
    }

    func status(_ game: GameSummary, prominent: Bool = false) -> some View {
        let (text, color) = statusLine(game)
        return VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 7) {
            Circle().fill(color).frame(width: prominent ? 9 : 7, height: prominent ? 9 : 7)
            Text(prominent ? text.uppercased() : text)
                .font(prominent ? .manrope(12, .bold) : .manrope(13, .medium))
                .tracking(prominent ? 1.3 : 0)
                .foregroundStyle(color)
            if !game.display.isEmpty {
                Text("· " + game.display).font(.manrope(prominent ? 13 : 12)).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        // 导入时数据校验不过、照常导入的款（11-17）：选中时黄字说一句（卡片角标只有图标，手柄玩家看不到悬停说明）
        if game.integritySkipped {
            Label(loc.backend(LauncherView.integritySkippedNote), systemImage: "exclamationmark.triangle.fill")
                .font(.manrope(12, .medium)).foregroundStyle(.yellow).fixedSize(horizontal: false, vertical: true)
        }
        // S 级优化升级没做完（上次中途退出了启动器）：一行说明，点了打开「S 级优化升级」页接着升（11-18 维护者）
        if game.sgradeIncomplete && !model.sgradeUpgrading(game.id) {
            Button { model.sgradeUpgradeOpen = true } label: {
                Label(txt("sgrade.upgrade.incomplete"), systemImage: "exclamationmark.arrow.circlepath")
                    .font(.manrope(12, .medium)).foregroundStyle(SGradeCardView.gold).fixedSize(horizontal: false, vertical: true)
            }
            .controllerButtonStyle(.plain).controllerID("sgrade.incomplete." + game.id)
        }
        }
        .accessibilityElement(children: .contain)
    }
}

/// A cover filling its frame (upstream ArtworkFill without the focus point: S2Arm covers have none).
struct CoverFill: View {
    let image: NSImage
    var body: some View {
        // 填满后溢出的部分 .clipped() 只裁显示、不裁点击区域：不限定的话，接近正方形的封面会把点击区域伸出框外，
        //   盖住上面的「返回游戏库」、旁边的卡片（11-09 维护者：丝之歌出包页返回点不了；AX 量到封面按钮高 236、看得见的只有 131）。
        //   用 Color.clear.overlay 代替 GeometryReader，避免卡片从小变大时 GeometryReader 尺寸一步到位跳过插值动画
        Color.clear
            .overlay {
                Image(nsImage: image).resizable().interpolation(.high).scaledToFill()
            }
            .clipped()
            .contentShape(Rectangle())
    }
}

/// Artwork I/O runs off the UI thread. A slow volume must not block selection or window layout.
/// Cache by path + target modification time; retry missing/invalid artwork when its file changes.
@MainActor final class CoverCache: ObservableObject {
    struct Loaded {
        let version: String
        let image: NSImage?
        var decoded = false
    }
    typealias Loader = (String, String?) -> Loaded
    private var images: [String: NSImage] = [:]
    private var versions: [String: String] = [:]
    private var pending: Set<String> = []
    private let loader: Loader
    @Published private var revision = 0

    init(loader: @escaping Loader = CoverCache.load) { self.loader = loader }

    func image(_ path: String?) -> NSImage? {
        guard let path else { return nil }
        if pending.insert(path).inserted {
            let previous = versions[path], loader = loader
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                let result = loader(path, previous)
                DispatchQueue.main.async {
                    guard let self else { return }
                    self.pending.remove(path)
                    guard self.versions[path] != result.version else { return }
                    self.versions[path] = result.version
                    self.images[path] = result.image
                    if result.decoded { LauncherWork.count(.artworkDecode) }
                    self.revision &+= 1
                }
            }
        }
        return images[path]
    }

    nonisolated private static func load(_ path: String, _ previous: String?) -> Loaded {
        // Only the target timestamp is needed, not Foundation's extra extended-attribute queries.
        var metadata = stat()
        guard path.withCString({ fstatat(AT_FDCWD, $0, &metadata, 0) }) == 0 else { return Loaded(version: "", image: nil) }
        let version = String(metadata.st_mtimespec.tv_sec) + "." + String(metadata.st_mtimespec.tv_nsec)
        guard version != previous else { return Loaded(version: version, image: nil) }
        // Read the bytes here: NSImage(contentsOfFile:) can defer file access until main-thread drawing.
        let image = (try? Data(contentsOf: URL(fileURLWithPath: path))).flatMap(NSImage.init(data:))
        _ = image?.representations
        return Loaded(version: version, image: image, decoded: true)
    }
}

/// 游戏卡片的拖放落点：拖着文件经过时封面描虚线，放下交给 perform（LauncherView.dropOnCard）。
struct CardDrop: ViewModifier {
    let targeted: Binding<Bool>
    let artwork: CGFloat
    let perform: ([URL]) -> Void
    func body(content: Content) -> some View {
        content
            .overlay(alignment: .top) {
                if targeted.wrappedValue {
                    RoundedRectangle(cornerRadius: 20).strokeBorder(LauncherPalette.cream, style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
                        .frame(height: artwork).allowsHitTesting(false)
                }
            }
            .onDrop(of: [UTType.fileURL], isTargeted: targeted) { providers in
                LauncherView.fileURLs(providers, perform)
                return true
            }
    }
}

private struct MoreMenuRowLabel: View {
    let title: String
    let symbol: String
    let danger: Bool
    @State private var hovered = false

    var body: some View {
        // 图标放固定宽度的框里、和文字按中线对齐：Label 的图标宽度跟着符号走，各行文字起点会错开（11-09 维护者截图）
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: symbol).font(.system(size: 14, weight: .medium)).frame(width: 20, height: 20, alignment: .center)
            Text(title).lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10).padding(.vertical, 8)
        .background(Color.white.opacity(hovered ? 0.08 : 0), in: RoundedRectangle(cornerRadius: 6))
        .contentShape(Rectangle())
        .foregroundStyle(danger ? LauncherPalette.danger : Color.primary)
        .onHover { hovered = $0 }
    }
}

/// 详情区「更新」按钮（0.6.4）：没做过 iPhone 版不出；做过、没有原因 → 灰色不可点「无需更新」；有原因 → 主色「更新这款」。
///   点了不二次确认、按钮当场变进度（不只标题栏）；同款正在出包 / 推送 / 在玩时置灰（作业层同一款不并行做）
struct HeroUpdateButton: View {
    @ObservedObject var model: S2ArmLauncherModel
    @EnvironmentObject var loc: Localizer
    let game: GameSummary
    private func txt(_ id: String) -> String { BackendText.launcherText(id, language: loc.lang.sharedCode) }

    @ViewBuilder var body: some View {
        let state = model.updateButton(game)
        let run = model.gameUpdate(game)
        if state != .hidden {
            if let run, run.running {
                let p = run.progress.overall ?? run.progress.fraction
                Button {} label: {
                    Label(txt("update.hint.running") + (p.map { " \(Int(($0 * 100).rounded(.down)))%" } ?? ""), systemImage: "arrow.triangle.2.circlepath")
                }
                .controllerButtonStyle(ConsolePillButtonStyle()).disabled(true)
                .overlay(alignment: .bottom) { SGradeUpgradeBar(progress: p).padding(.horizontal, 16).padding(.bottom, 6) }
                .controllerID("hero.update").accessibilityIdentifier("hero.update.running")
            } else if case .update = state {
                Button { model.updateGame(game) } label: { Label(txt("update.hint.button"), systemImage: "arrow.triangle.2.circlepath") }
                    .controllerButtonStyle(ConsolePlayButtonStyle()).disabled(model.mainHolds(game.id) || model.phoneHolds(game.id))
                    .controllerID("hero.update").accessibilityIdentifier("hero.update")
                    .launcherTooltip(txt("update.hint.tail"), placement: .above)
            } else {
                Button {} label: { Label(txt("update.hint.upToDate"), systemImage: "checkmark.circle") }
                    .controllerButtonStyle(ConsolePillButtonStyle()).disabled(true)
                    .controllerID("hero.update").accessibilityIdentifier("hero.update.upToDate")
            }
        }
    }

}

/// 按钮下的几行：橙色 = 为什么建议更新（对不上几样写几行）；黄色 = 刚做完的这次没做齐的几项（作业层 update_incomplete 原句）；红色 = 没做成的原因
struct HeroUpdateLines: View {
    @ObservedObject var model: S2ArmLauncherModel
    @EnvironmentObject var loc: Localizer
    let game: GameSummary
    private func txt(_ id: String) -> String { BackendText.launcherText(id, language: loc.lang.sharedCode) }

    /// 选这款的原件（同添加游戏收的格式；文件夹 = 解包好的游戏）
    private func pickSource() {
        let panel = NSOpenPanel()
        panel.message = txt("update.relink.pick")
        panel.canChooseFiles = true; panel.canChooseDirectories = true; panel.allowsMultipleSelection = false
        if LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url { model.relinkSource(game, source: url) }
    }

    @ViewBuilder var body: some View {
        let reasons = model.updateReasons(game).compactMap(S2ArmLauncherModel.updateReasonKey).reduce(into: [String]()) { if !$0.contains($1) { $0.append($1) } }   // libGen / shaderMiss 同着色器那句：只写一次
        let run = model.gameUpdate(game)
        let incomplete = run.map { $0.running ? [] : $0.incomplete } ?? []
        let failure = run.flatMap { $0.running ? nil : $0.failure }
        // 更新只改电脑上这份：这款装到过设备的，做完说一句手机上那份要再导一次（不然玩家以为手机上已经是新的）
        let sendAgain = run.map { !$0.running && $0.failure == nil } == true && game.iphoneInstalledAt != nil
        // #1961：要整款重新出包（宿主约定号变了，或上一次更新回的就是要重出）→ 就在按钮下写一句，别让玩家以为点一下就好
        let repackNote = model.repackNoteNeeded(game)
        if game.iphoneReady, !reasons.isEmpty || !incomplete.isEmpty || failure != nil || sendAgain || repackNote || run?.running == true {
            VStack(alignment: .leading, spacing: 3) {
                // 正在更新：当前这一段的计数、速度、还剩多久（重编着色器库一段可能几小时，只看整单百分比分不出卡没卡；11-21）
                if let run, run.running, run.progress.total != nil {
                    let p = run.progress
                    TimelineView(.periodic(from: .now, by: 1)) { timeline in
                        let count = p.countText.isEmpty ? nil : p.countText + (p.unit == "MB" ? " MB" : "")
                        let parts = [p.stage.isEmpty ? nil : loc.backend(p.stage), count, p.rateText.isEmpty ? nil : p.rateText, p.clock(at: timeline.date, left: loc.t("prog.eta"))]
                        Text(parts.compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · "))
                            .font(.manrope(12, .medium)).monospacedDigit().foregroundStyle(.white.opacity(0.85)).lineLimit(1)
                    }
                    .accessibilityIdentifier("update.progress." + game.id)
                }
                ForEach(reasons, id: \.self) { key in
                    Label(txt(key), systemImage: "arrow.triangle.2.circlepath")
                        .font(.manrope(12, .medium)).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                }
                ForEach(incomplete, id: \.self) { line in
                    Label(loc.backend(line), systemImage: "exclamationmark.triangle.fill")
                        .font(.manrope(12, .medium)).foregroundStyle(.yellow).fixedSize(horizontal: false, vertical: true)
                }
                // #1961：这款要整款重新出包（已排进批量出包）：说清要多久，别让玩家以为再点一下「更新这款」就行
                if repackNote {
                    Label(txt("update.repackNote"), systemImage: "shippingbox")
                        .font(.manrope(12, .medium)).foregroundStyle(.white.opacity(0.85)).fixedSize(horizontal: false, vertical: true)
                        .accessibilityIdentifier("update.repackNote." + game.id)
                }
                // #1961：原料和原件都不在 → 就在黄字下补原件（选错了作业层拒、不改记录）；找回后自动接着整款重出（见 repackWholeGame）
                if let run, !run.running, run.relinkSource {
                    Button(txt(run.relinking ? "update.relink.checking" : "update.relink")) { pickSource() }
                        .controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(run.relinking)
                        .accessibilityIdentifier("update.relink." + game.id)
                }
                if let run, !run.running, run.relinked {
                    Label(txt("update.relink.done"), systemImage: "checkmark.circle")
                        .font(.manrope(12, .medium)).foregroundStyle(.white.opacity(0.85)).fixedSize(horizontal: false, vertical: true)
                }
                if sendAgain {
                    Label(txt("update.hint.sendAgain"), systemImage: "iphone.and.arrow.forward")
                        .font(.manrope(12, .medium)).foregroundStyle(.white.opacity(0.85)).fixedSize(horizontal: false, vertical: true)
                }
                if let failure {
                    Text(txt("update.hint.failed").replacingOccurrences(of: "{0}", with: loc.backend(failure)))
                        .font(.manrope(12)).foregroundStyle(LauncherPalette.danger).fixedSize(horizontal: false, vertical: true)
                }
            }
            .shadow(color: .black.opacity(0.45), radius: 6, y: 1)
            .accessibilityElement(children: .contain).accessibilityIdentifier("update.lines." + game.id)
        }
    }
}
