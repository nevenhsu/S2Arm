// S2Arm（不是上游文件）：手柄 / 键位设置弹窗（同旧启动器 InputEditor：看键位、改键、恢复默认、保存）。
// 左边是 Joy-Con 按键图（自旧启动器 ControllerMap 搬来），点一个键在右边改它的键盘 / 鼠标 / 手柄绑定；下面是鼠标与摇杆选项。
// 键位按玩家保存（本地最多 4 位玩家）：上面选玩家 1–4，各自一份键位、各自保存（玩家 1 = 这款的 input.json，玩家 2–4 = input-pN.json）；
//   玩家 2–4 没单独存过的显示玩家 1 的键位（与起局一致，保存后单独生效）；切玩家不丢没保存的改动（留在弹窗里，保存才写盘）。
//   保存走 saveInput 作业（S2ArmLauncherModel+Input），成了给一句「已保存」、留着弹窗继续改别的玩家；没成的原因留在弹窗里。
//   改键下次开始游戏时生效（起局时玩家 2–4 的交给宿主，槽位各照各的映射采样；鼠标选项与背键始终照玩家 1）。
//   「管理键位」组（#1987，同 iPad 位置与文案）：导入 / 导出键位文件、设为所有游戏的默认；和恢复默认、按玩法套用一样只改工作副本，点「保存」才写。
//   恢复默认：玩家 1 回到所有游戏的默认，玩家 2–4 回到玩家 1 存着的那套（保存后回到沿用玩家 1）。
import AppKit
import SwiftUI
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

struct InputSettingsSheet: View {
    @ObservedObject var model: S2ArmLauncherModel
    @EnvironmentObject var loc: Localizer
    @Environment(\.dismiss) private var dismiss
    let gameID: String
    @State private var player = 1
    @State private var config = InputConfig.standard
    @State private var baseline = InputConfig.standard
    /// 每位玩家的工作副本（切玩家时不丢未保存的改动）与各自的基线（上次保存 / 读出）、沿用标记与读数问题
    @State private var configs: [Int: InputConfig] = [:]
    @State private var baselines: [Int: InputConfig] = [:]
    @State private var inheritedFlags: [Int: Bool] = [:]
    @State private var problems: [Int: Bool] = [:]
    @State private var loaded = false
    @State private var loadProblem = false
    /// 所有游戏的默认（default-input.json）读不出或不合规：按没设过算，提示一行；重新设为默认并保存后收掉
    @State private var defaultProblem = false
    @State private var inherited = false
    @State private var selected = "A"
    @State private var recording: String?
    @State private var saving = false
    @State private var failure: String?
    /// 刚保存成功（给一句「已保存」反馈；再改任何键就收掉）
    @State private var savedNow = false
    /// 「管理键位」组（#1987）：所有游戏的默认（「恢复默认」回到它；没设过 = 标准键位）、每位玩家存着的出处（页内标「自定义 / 默认」）、
    ///   点了「设为所有游戏的默认」还没保存（Mac 弹窗一种规矩：点「保存」时才写，「取消」放弃；只对玩家 1）
    @State private var allGamesDefault = InputConfig.standard
    @State private var defaultSet = false
    @State private var sources: [Int: String] = [:]
    @State private var pendingDefault = false

    init(model: S2ArmLauncherModel, gameID: String) {
        self.model = model
        self.gameID = gameID
    }

    private var language: String { loc.lang.sharedCode }
    private func t(_ id: String) -> String { BackendText.launcherText("input." + id, language: language) }

    /// 要绑的键已经给了别的功能（v0.3.1：原先存成冲突、保存键置灰，玩家被挡住）：先问「改给这个功能？」，确认就解开旧绑定再绑（摇杆两边互换方向），
    ///   存进去的配置照样没有重复绑定（运行时 input_config.c、作业层 validate_input 都拒收重复）
    struct Rebind: Identifiable { let id = UUID(); let kind: String; let key: String; let from: String; let to: String; let apply: () -> Void }
    @State private var rebind: Rebind?
    private func bind(_ kind: String, key: String, target: String, holder: String?, apply: @escaping () -> Void) {
        if let holder, holder != target { rebind = Rebind(kind: kind, key: key, from: holder, to: target, apply: apply) } else { apply() }
    }
    private func rebindText(_ r: Rebind) -> String {
        let name = { (x: String) in r.kind == "axes" ? names.axis(x) : names.target(x) }
        return t(r.kind == "axes" ? "rebind.axes" : "rebind.body").replacingOccurrences(of: "{0}", with: name(r.from))
            .replacingOccurrences(of: "{1}", with: name(r.to)).replacingOccurrences(of: "{2}", with: r.key)
    }
    private func setKey(_ key: KeyBinding, for target: String) {
        let holder = config.keyboard.first { $0.key != target && $0.value == key }?.key
        bind("keyboard", key: names.key(key), target: target, holder: holder) {
            if let holder { config.keyboard[holder] = nil }
            config.keyboard[target] = key; failure = nil
        }
    }
    private var names: InputNames { InputNames(text: t) }
    private var game: GameSummary? { model.games.first { $0.id == gameID } }
    private var canSave: Bool { loaded && !saving && recording == nil && config.conflicts.isEmpty }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    HStack(alignment: .top, spacing: LauncherTheme.Space.section) {
                        InputControllerMap(config: config, selected: $selected, recording: recording, names: names, text: t) { _ in recording = nil }
                        selectedPanel.frame(maxWidth: .infinity)
                    }
                    if !config.conflicts.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(Array(config.conflicts.enumerated()), id: \.offset) { _, c in
                                Text(conflictText(c)).font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.danger)
                            }
                        }
                    }
                    CardGroup(t("backButtons.title"),
                              footnote: t("backButtons.note")) {
                        ForEach(["GL", "GR"], id: \.self) { side in
                            CardRow(side) {
                                LauncherDropdown(options: [("", t("unbound"))] + Array(zip(InputConfig.padCodes, InputConfig.targets.prefix(16))).map { ($0.0, names.target($0.1)) },
                                                 selection: config.backButtons?[side] ?? "", label: side) { value in
                                    var back = config.backButtons ?? [:]
                                    back[side] = value.isEmpty ? nil : value
                                    config.backButtons = back.isEmpty ? nil : back
                                    failure = nil
                                }
                            }
                        }
                    }
                    options
                    manage
                }
                .padding(.horizontal, LauncherTheme.Space.page).padding(.bottom, LauncherTheme.Space.section)
            }
            footer
        }
        .alert(t("rebind.title"), isPresented: Binding(get: { rebind != nil }, set: { if !$0 { rebind = nil } })) {
            Button(t("rebind.confirm")) { rebind?.apply(); rebind = nil }
            Button(t("cancel"), role: .cancel) { rebind = nil }
        } message: { Text(rebind.map(rebindText) ?? "") }
        .background(InputKeyRecorder(active: recording != nil) { key in
            if let target = recording, let key { setKey(key, for: target) }
            recording = nil
        })
        .task {   // #2081：四位玩家的键位交作业层 loadInput 读（同起局的规则），读完才算 loaded
            guard !loaded else { return }
            let all = (try? await model.loadInputs(gameID: gameID)) ?? InputLoad()
            for p in 1...4 {
                let input = all.players[p] ?? LoadedInput(config: .standard, problem: false, inherited: p > 1, source: p > 1 ? "player1" : "standard")
                configs[p] = input.config; baselines[p] = input.config; inheritedFlags[p] = input.inherited; problems[p] = input.problem
                sources[p] = input.source
            }
            allGamesDefault = all.allGamesDefault; defaultSet = all.defaultSet; defaultProblem = all.defaultProblem
            config = configs[player] ?? .standard; baseline = baselines[player] ?? .standard
            loadProblem = problems[player] ?? false
            inherited = inheritedFlags[player] ?? false
            loaded = true
        }
        .onChange(of: config) { configs[player] = $0; savedNow = false }   // 改动实时记到当前玩家的工作副本上（切玩家不丢）；改了就不是「已保存」状态
        .onChange(of: player) { p in
            config = configs[p] ?? .standard; baseline = baselines[p] ?? .standard
            loadProblem = problems[p] ?? false
            inherited = inheritedFlags[p] ?? false
            selected = "A"; recording = nil; failure = nil; rebind = nil; savedNow = false
        }
        .launcherSheet()
        .onExitCommand { if recording == nil && !saving { dismiss() } }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            LauncherSheetHeading(t("title"), subtitle: game?.name ?? gameID, note: t("subtitle"))
            // 玩家选择（本地最多 4 位玩家）：每位玩家一份键位、各自保存；切玩家不丢没保存的改动
            HStack(spacing: 10) {
                Text(t("player")).font(LauncherTheme.TypeStyle.section).foregroundStyle(.secondary)
                LauncherSegmented(options: (1...4).map { (value: String($0), label: t("player\($0)")) },
                                  selection: String(player), choose: { value in
                    guard let p = Int(value), p != player, !saving else { return }
                    configs[player] = config   // 保险：切之前把这位的改动先记下（平时 onChange(of: config) 已记）
                    player = p
                })
                    .frame(width: 320)
                    .disabled(saving)
                    .accessibilityIdentifier("input.player")
            }
            .padding(.top, 6)
            Text(t("player.note")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            // 这位还没单独保存过：显示的就是玩家 1 的键位（与起局一致），保存后单独生效
            if inherited {
                Text(t("player.inherit")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(LauncherPalette.building).fixedSize(horizontal: false, vertical: true)
            }
            // #1987：页内标玩家 1 这款用的是哪一套（同 iPad 页顶）：单独存过的 = 自定义，跟着所有游戏的默认 = 默认；初始状态（标准键位）不标
            if player == 1, let tag = sourceTag {
                Text(tag).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary)
                    .accessibilityIdentifier("input.source")
            }
            if loadProblem {
                Text(t("loadProblem")).font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.building)
                    .fixedSize(horizontal: false, vertical: true).padding(.top, 4)
            }
            if defaultProblem {
                Text(t("defaultProblem")).font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.building)
                    .fixedSize(horizontal: false, vertical: true).padding(.top, 4)
                    .accessibilityIdentifier("input.defaultProblem")
            }
        }
        .padding(.horizontal, LauncherTheme.Space.page).padding(.top, LauncherTheme.Space.page).padding(.bottom, LauncherTheme.Space.section)
    }

    // ---- 选中的那个键 ----

    private var selectedPanel: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 3) {
                Text(t("selected")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary)
                Text(names.target(selected)).font(.manrope(20, .heavy)).foregroundStyle(LauncherPalette.cream)
            }
            VStack(alignment: .leading, spacing: 7) {
                Label(t("keyboard"), systemImage: "keyboard").font(LauncherTheme.TypeStyle.section)
                keyField
                LauncherDropdown(options: [("", t("key.common"))] + Self.commonKeys.map { (String($0), names.key(KeyBinding(code: $0))) },
                                 selection: "", label: t("key.common"), fill: true) { value in
                    if let code = Int(value) { setKey(KeyBinding(code: code), for: selected) }
                    recording = nil
                }
            }
            VStack(alignment: .leading, spacing: 7) {
                Label(t("mouse"), systemImage: "computermouse").font(LauncherTheme.TypeStyle.section)
                LauncherDropdown(options: [("", t("unbound"))] + (0...4).map { (String($0), names.mouseButton($0)) },
                                 selection: config.mouse[selected].map(String.init) ?? "", label: t("mouse"), fill: true) { value in
                    recording = nil
                    let button = Int(value), target = selected
                    let holder = button.flatMap { b in config.mouse.first { $0.key != target && $0.value == b }?.key }
                    bind("mouse", key: button.map(names.mouseButton) ?? "", target: target, holder: holder) {
                        if let holder { config.mouse[holder] = nil }
                        config.mouse[target] = button; failure = nil
                    }
                }
            }
            Text(loc.lang == .zh ? "手柄四键统一在顶栏手柄弹窗设置，所有玩家共用" : "Set the four face buttons in the top-bar controller popover; the layout is shared by all players")
                .font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("input.sharedFaces")
            if !ControllerFaceMapping.labels.contains(selected) {
                gamepadField
            }
            if config.conflictingTargets.contains(selected) {
                Label(t("conflict.here"), systemImage: "exclamationmark.triangle").font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.danger)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Button { config.restoreTarget(selected, from: baseline); recording = nil } label: { Text(t("restoreKey")).frame(maxWidth: .infinity) }
                .controllerButtonStyle(LauncherSecondaryButtonStyle())
            Text(t("iconNote")).font(.manrope(11.5)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(LauncherTheme.Metrics.cardInset)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .launcherPanel()
    }

    private var gamepadField: some View {
        VStack(alignment: .leading, spacing: 7) {
            Label(t("gamepad"), systemImage: "gamecontroller").font(LauncherTheme.TypeStyle.section)
            let codes = Array(InputConfig.padCodes.dropFirst(4))
            let binding = config.gamepad[selected] ?? ""
            LauncherDropdown(options: [("", t("unbound"))] + codes.map { ($0, names.pad($0)) },
                             selection: codes.contains(binding) ? binding : "", label: t("gamepad"), fill: true) { value in
                recording = nil
                let pad = value.isEmpty ? nil : value, target = selected
                let holder = pad.flatMap { p in config.gamepad.first { $0.key != target && $0.value == p }?.key }
                bind("gamepad", key: pad.map(names.pad) ?? "", target: target, holder: holder) {
                    if let holder { config.gamepad[holder] = nil }
                    config.gamepad[target] = pad; failure = nil
                }
            }
            .controllerID("input.gamepad")
        }
    }

    /// 键盘绑定：点一下进入录入（按新键，Esc 取消），右侧 × 清掉。
    private var keyField: some View {
        let on = recording == selected
        return HStack(spacing: 0) {
            Button { recording = on ? nil : selected } label: {
                Text(on ? t("key.record") : names.key(config.keyboard[selected]))
                    .font(LauncherTheme.TypeStyle.button).lineLimit(1).minimumScaleFactor(0.8)
                    .padding(.horizontal, LauncherTheme.Space.medium)
                    .frame(maxWidth: .infinity, minHeight: LauncherControl.height, alignment: .leading)
                    .contentShape(Rectangle())
            }
            .controllerButtonStyle(.plain).controllerID("input.keyboard.record")
            .accessibilityLabel(t("keyboard")).accessibilityValue(names.key(config.keyboard[selected]))
            Button { config.keyboard[selected] = nil; recording = nil } label: {
                Image(systemName: "xmark.circle").frame(width: LauncherControl.height, height: LauncherControl.height).contentShape(Rectangle())
            }
            .controllerButtonStyle(.plain).foregroundStyle(.secondary).accessibilityLabel(t("key.clear"))
            .controllerID("input.keyboard.clear")
        }
        .background(on ? LauncherPalette.cream.opacity(0.10) : LauncherTheme.Colors.control,
                    in: RoundedRectangle(cornerRadius: LauncherTheme.Metrics.controlRadius))
        .overlay(RoundedRectangle(cornerRadius: LauncherTheme.Metrics.controlRadius)
            .strokeBorder(on ? LauncherPalette.cream : LauncherTheme.Colors.border, lineWidth: on ? 1.5 : 1).allowsHitTesting(false))
        .accessibilityElement(children: .contain)
    }

    /// 「常用键」里给的键（Return、Esc、空格、Tab、Shift、Ctrl、Option）：Esc 录入时是取消键，只能从这里选。
    private static let commonKeys = [36, 53, 49, 48, 56, 59, 58]

    // ---- 鼠标与摇杆 ----

    private var options: some View {
        VStack(alignment: .leading, spacing: 20) {
            CardGroup(t("mouseOptions"), footnote: t("mouseOptions.note")) {
                CardRow(t("mouse.mode")) {
                    LauncherSegmented(options: [("camera", t("mouse.mode.camera")), ("off", t("mouse.mode.off"))],
                                      selection: config.mouseOptions.mode) { config.mouseOptions.mode = $0 }
                        .frame(width: LaunchControl.width)
                }
                CardDivider()
                slider(t("mouse.sensX"), value: config.mouseOptions.sensitivityX, range: 0.1...10, step: 0.1,
                       readout: String(format: "%.1f", config.mouseOptions.sensitivityX)) { config.mouseOptions.sensitivityX = $0 }
                CardDivider()
                slider(t("mouse.sensY"), value: config.mouseOptions.sensitivityY, range: 0.1...10, step: 0.1,
                       readout: String(format: "%.1f", config.mouseOptions.sensitivityY)) { config.mouseOptions.sensitivityY = $0 }
                CardDivider()
                toggle(t("mouse.invertX"), $config.mouseOptions.invertX)
                CardDivider()
                toggle(t("mouse.invertY"), $config.mouseOptions.invertY)
                CardDivider()
                toggle(t("mouse.recenter"), $config.mouseOptions.recenter)
            }
            CardGroup(t("sticks"), footnote: t("sticks.note")) {
                slider(t("deadzone"), value: config.deadzone, range: 0...0.8, step: 0.01,
                       readout: String(format: "%.0f%%", config.deadzone * 100)) { config.deadzone = $0 }
                ForEach(InputConfig.axisTargets, id: \.self) { target in
                    CardDivider()
                    CardRow(names.axis(target)) {
                        HStack(spacing: 8) {
                            Text(t("axis.invert")).font(LauncherTheme.TypeStyle.detail).foregroundStyle(.secondary)
                                .frame(width: 40, alignment: .leading)
                            Toggle("", isOn: Binding(get: { config.axes[target]?.invert ?? false },
                                                     set: { config.axes[target] = AxisBinding(source: config.axes[target]?.source ?? Self.ownSource(target), invert: $0) }))
                                .toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(names.axis(target) + " " + t("axis.invert"))
                                .controllerID("input.axis.invert." + target)
                            LauncherDropdown(options: InputConfig.axisSources.map { ($0, names.axis($0)) },
                                             selection: config.axes[target]?.source ?? Self.ownSource(target), label: names.axis(target)) { source in
                                let old = config.axes[target]?.source ?? Self.ownSource(target)
                                let holder = config.axes.first { $0.key != target && $0.value.source == source }?.key
                                bind("axes", key: names.axis(source), target: target, holder: holder) {   // 摇杆不能留空：两边互换方向
                                    if let holder { config.axes[holder] = AxisBinding(source: old, invert: config.axes[holder]?.invert ?? false) }
                                    config.axes[target] = AxisBinding(source: source, invert: config.axes[target]?.invert ?? false)
                                }
                            }
                            .environment(\.launcherControlWidth, LauncherTheme.Metrics.compactChoiceWidth)
                            .controllerID("input.axis.source." + target)
                        }
                        .frame(width: LaunchControl.width)
                    }
                }
            }
        }
    }

    private static func ownSource(_ target: String) -> String {
        ["LX": "leftX", "LY": "leftY", "RX": "rightX", "RY": "rightY"][target] ?? "leftX"
    }

    private func slider(_ title: String, value: Double, range: ClosedRange<Double>, step: Double, readout: String,
                        change: @escaping (Double) -> Void) -> some View {
        CardRow(title) {
            HStack(spacing: 12) {
                LauncherSlider(value: value, range: range, step: step, change: change)
                    .accessibilityLabel(title).accessibilityValue(readout)
                Text(readout).font(.manrope(12.5, .semibold)).monospacedDigit().foregroundStyle(.secondary).frame(width: 40, alignment: .trailing)
            }
            .frame(width: LaunchControl.width)
        }
    }

    private func toggle(_ title: String, _ isOn: Binding<Bool>) -> some View {
        CardRow(title) { Toggle("", isOn: isOn).toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(title) }
    }

    // ---- 管理键位（#1987，同 iPad 位置与文案：导入 / 导出 / 设为所有游戏的默认；都只改弹窗里的工作副本，点「保存」才写）----

    private var sourceTag: String? {
        switch sources[1] {
        case "game": return t("source.custom")
        case "default": return t("source.default")
        default: return nil
        }
    }

    private var manage: some View {
        CardGroup(t("manage"), footnote: t("manage.note")) {
            CardRow(t("manage.import"), detail: t("manage.import.detail")) {
                Button(t("manage.import.button")) { importFile() }
                    .controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(!loaded || saving)
                    .controllerID("input.manage.import")
            }
            CardDivider()
            CardRow(t("manage.export"), detail: t("manage.export.detail")) {
                Button(t("manage.export.button")) { exportFile() }
                    .controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(!loaded || saving)
                    .controllerID("input.manage.export")
            }
            CardDivider()
            CardRow(t("manage.setDefault"), detail: pendingDefault && player == 1 ? t("manage.setDefault.pending")
                        : t(player == 1 ? "manage.setDefault.detail" : "manage.setDefault.player1")) {
                Button(t("manage.setDefault.button")) { pendingDefault = true; savedNow = false; failure = nil; recording = nil }
                    .controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(!loaded || saving || player != 1 || pendingDefault)
                    .controllerID("input.manage.setDefault")
            }
        }
    }

    /// 导入：读进当前玩家的工作副本（同按玩法套用），点「保存」时作业层再完整校验；读不进的原因留在底栏、当前键位不动
    private func importFile() {
        let panel = NSOpenPanel()
        panel.title = t("manage.import")
        panel.allowedContentTypes = [.json]
        panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        guard LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url else { return }
        recording = nil
        switch InputConfig.imported(from: url) {
        case .success(let imported): config = imported; failure = nil
        case .failure(let problem): failure = t("manage.import.err." + problem.rawValue)
        }
    }

    /// 导出：把弹窗里当前的键位（含没保存的改动）存成文件，格式同 input.json，iPad 能导入
    private func exportFile() {
        let panel = NSSavePanel()
        panel.title = t("manage.export")
        panel.allowedContentTypes = [.json]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = t("manage.export.fileName") + ".json"
        guard LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url else { return }
        do { try config.exportData().write(to: url, options: .atomic); failure = nil }
        catch { failure = t("manage.export.err").replacingOccurrences(of: "{0}", with: error.localizedDescription) }
    }

    /// 「恢复默认」回到的那一套（#1987）：玩家 1 = 所有游戏的默认（没设过 = 标准键位）；玩家 2–4 = 玩家 1 存着的那套（保存后回到沿用玩家 1）
    private var restoreTarget: InputConfig { player == 1 ? allGamesDefault : (baselines[1] ?? allGamesDefault) }

    // ---- 底栏 ----

    private var footer: some View {
        LauncherSheetFooter {
        VStack(alignment: .leading, spacing: 10) {
            if saving {
                ProgressView().progressViewStyle(.linear).tint(LauncherPalette.cream)
            }
            if let failure {
                Text(failure).font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.danger).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 10) {
                Button(t("restoreDefault")) { config = restoreTarget; if player == 1 { pendingDefault = false }; recording = nil; failure = nil }
                    .controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(saving).controllerShortcut(.secondary)
                    .controllerID("input.restoreDefault")
                LauncherDropdown(options: [("", t("preset"))] + InputConfig.Preset.allCases.map { ($0.rawValue, t("preset." + $0.rawValue)) },
                                 selection: "", label: t("preset")) { value in
                    if let p = InputConfig.Preset(rawValue: value) { config = .preset(p); failure = nil }
                    recording = nil
                }
                .disabled(saving)
                Spacer(minLength: 8)
                if saving {
                    Text(t("saving")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary)
                } else if savedNow && config == baseline && !pendingHere {
                    Text(t("saved")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary)
                } else if config != baseline || pendingHere {
                    Text(t("unsaved")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary)
                }
                Button(t("cancel")) { dismiss() }
                    .controllerButtonStyle(LauncherSecondaryButtonStyle()).keyboardShortcut(.cancelAction).disabled(saving).controllerShortcut(.back)
                Button(t("save")) { save() }
                    .controllerButtonStyle(LauncherPrimaryButtonStyle()).keyboardShortcut(.defaultAction).disabled(!canSave).controllerShortcut(.rightTrigger)
                    .controllerID("input.save")
            }
        }
        }
    }

    /// 当前这位有没有「设为所有游戏的默认」等着保存（只有玩家 1 有）
    private var pendingHere: Bool { pendingDefault && player == 1 }

    private func save() {
        guard canSave else { return }
        saving = true; failure = nil
        let config = config
        let savedFor = player
        let asDefault = pendingHere
        // 玩家 2–4 改成和玩家 1 存着的一样（含「恢复默认」）：删掉自己那份、回到沿用玩家 1（同玩家 1 和所有游戏的默认一样就跟着默认走）
        let follow = savedFor > 1 && config.same(baselines[1] ?? allGamesDefault)
        Task {
            do {
                var inheritedNow = false
                if asDefault {
                    // #1987：点了「设为所有游戏的默认」→ 保存时用当前键位设默认，这款跟着默认走；设成标准键位 = 没设过
                    try await model.setDefaultInput(config, gameID: gameID)
                    allGamesDefault = config; defaultSet = !config.same(.standard)
                    sources[1] = defaultSet ? "default" : "standard"; pendingDefault = false; defaultProblem = false
                } else if follow {
                    try await model.resetInput(gameID: gameID, player: savedFor)
                    sources[savedFor] = "player1"; inheritedNow = true
                } else {
                    let follows = try await model.saveInput(config, gameID: gameID, player: savedFor)
                    sources[savedFor] = savedFor == 1 && follows ? (defaultSet ? "default" : "standard") : "game"
                }
                // 玩家 1 存了新的一套：还沿用玩家 1、没动过的玩家 2–4 跟着换成新的（同起局），不然再存它们会把旧的那套单独写下来
                if savedFor == 1 {
                    for p in 2...4 where inheritedFlags[p] == true && configs[p] == baselines[1] { configs[p] = config; baselines[p] = config }
                }
                baselines[savedFor] = config; configs[savedFor] = config; inheritedFlags[savedFor] = inheritedNow; saving = false
                // 还停在这一位的话当前基线跟着跟上（不然「未保存」提示一直在、恢复这个键回的是保存前的旧值）；
                //   给一句「已保存」反馈。成了不关弹窗：键位按玩家分开保存，留着继续给别的玩家改（切玩家用上面的选择器）
                if player == savedFor {
                    baseline = config
                    inherited = inheritedNow   // 存了自己的一份就收掉「沿用玩家 1」的提示；回到沿用就留着
                    savedNow = true
                }
            } catch {
                failure = names.saveFailure(error.localizedDescription, backend: loc.backend)
                saving = false
            }
        }
    }

    private func conflictText(_ c: InputConfig.Conflict) -> String {
        let a = c.kind == "axes" ? names.axis(c.first) : names.target(c.first)
        let b = c.kind == "axes" ? names.axis(c.second) : names.target(c.second)
        return t("conflict." + c.kind).replacingOccurrences(of: "{0}", with: a).replacingOccurrences(of: "{1}", with: b)
    }
}

/// 键位里各样东西的显示名（文案全在共享规格 launcher.items 的 input.*）。A / B / X / Y / L / R / ZL / ZR 与键帽符号两种语言相同，不进文案。
struct InputNames {
    let text: (String) -> String

    func target(_ t: String) -> String {
        ["A","B","X","Y","L","R","ZL","ZR"].contains(t) ? t : text("target." + t)
    }
    func pad(_ code: String) -> String { text("pad." + code) }
    /// 摇杆轴（LX…）与它的来源（leftX…）同名。
    func axis(_ code: String) -> String {
        text("axis." + (["leftX": "LX", "leftY": "LY", "rightX": "RX", "rightY": "RY"][code] ?? code))
    }
    func mouseButton(_ code: Int) -> String { (0...4).contains(code) ? text("mouseButton.\(code)") : text("unbound") }

    func key(_ b: KeyBinding?) -> String {
        guard let b else { return text("unbound") }
        let keys: [Int: String] = [0:"A",1:"S",2:"D",3:"F",4:"H",5:"G",6:"Z",7:"X",8:"C",9:"V",11:"B",12:"Q",13:"W",14:"E",15:"R",16:"Y",17:"T",18:"1",19:"2",20:"3",21:"4",22:"6",23:"5",24:"=",25:"9",26:"7",27:"-",28:"8",29:"0",30:"]",31:"O",32:"U",33:"[",34:"I",35:"P",36:"Return",37:"L",38:"J",39:"'",40:"K",41:";",42:"\\",43:",",44:"/",45:"N",46:"M",47:".",48:"Tab",49:text("key.space"),50:"`",51:text("key.delete"),53:"Esc",56:"Shift",58:"Option",59:"Ctrl",123:"←",124:"→",125:"↓",126:"↑"]
        let mods = (b.modifiers & 1 != 0 ? "⇧" : "") + (b.modifiers & 2 != 0 ? "⌃" : "") + (b.modifiers & 4 != 0 ? "⌥" : "")
        return mods + (keys[b.code] ?? text("key.code").replacingOccurrences(of: "{0}", with: String(b.code)))
    }

    /// 键位图上一个键底下的小字：先键盘，再鼠标，只有手柄时写「手柄」。
    func short(_ config: InputConfig, _ target: String) -> String {
        if let binding = config.keyboard[target] { return key(binding) }
        if let button = config.mouse[target] { return mouseButton(button) }
        return config.gamepad[target] == nil ? text("unbound") : text("short.gamepad")
    }

    /// 保存没成的原因：作业层 validate_input 的几句换成玩家看得懂的话，其余照作业层原句（经 loc.backend 翻）。
    func saveFailure(_ message: String, backend: (String) -> String) -> String {
        let known = ["不支持的输入映射格式。": "format", "输入映射不完整。": "incomplete", "无效输入目标。": "target", "无效键盘映射。": "keyboard",
                     "无效鼠标映射。": "mouse", "无效手柄映射。": "gamepad", "无效摇杆映射。": "axis", "输入映射存在重复绑定。": "duplicate",
                     "摇杆死区必须在 0 到 1 之间。": "deadzone", "无效鼠标选项。": "mouseOptions", "鼠标灵敏度必须在 0.1 到 10 之间。": "sensitivity",
                     "无效的玩家序号。": "player"]
        if let id = known[message] { return text("error." + id) }
        return text("error.failed").replacingOccurrences(of: "{0}", with: backend(message))
    }
}

/// 录键：录入时接本窗口的按键，规则在 InputKeyCapture（Esc 取消交回 nil；带 ⌘ 的不收；左右修饰键并成一个；
/// 单按 Shift / Ctrl / Option 松开时录成那个键，按着修饰键再按别的键录成组合）。录入时按键不再往下传（不触发弹窗的快捷键）。
struct InputKeyRecorder: NSViewRepresentable {
    var active: Bool
    var onKey: (KeyBinding?) -> Void
    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeNSView(context: Context) -> NSView { let v = NSView(); context.coordinator.install(v); return v }
    func updateNSView(_ view: NSView, context: Context) { context.coordinator.parent = self }
    static func dismantleNSView(_ view: NSView, coordinator: Coordinator) { coordinator.remove() }

    final class Coordinator {
        var capture = InputKeyCapture()
        var parent: InputKeyRecorder
        var monitor: Any?
        weak var view: NSView?
        init(_ parent: InputKeyRecorder) { self.parent = parent }
        func install(_ v: NSView) {
            view = v
            monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] e in
                guard let self else { return e }
                guard self.parent.active, NSApp.isActive, self.view?.window?.isKeyWindow == true else { self.capture.reset(); return e }
                if e.type == .keyUp { return nil }
                if e.modifierFlags.contains(.command) { self.capture.reset(); return e }
                let mods = (e.modifierFlags.contains(.shift) ? 1 : 0) | (e.modifierFlags.contains(.control) ? 2 : 0) | (e.modifierFlags.contains(.option) ? 4 : 0)
                guard let result = self.capture.event(code: Int(e.keyCode), modifiers: mods, flagsChanged: e.type == .flagsChanged,
                                                      repeatKey: e.type == .keyDown && e.isARepeat) else { return nil }
                let key: KeyBinding? = { if case let .bind(k) = result { return k }; return nil }()
                DispatchQueue.main.async { self.parent.onKey(key) }
                return nil
            }
        }
        func remove() { if let monitor { NSEvent.removeMonitor(monitor) }; monitor = nil }
    }
}

/// Switch 的按键位置（不是 Xbox 字母）：A 在右、B 在下、X 在上、Y 在左。
enum ControllerMapLayout {
    static let face = [["", "X", ""], ["Y", "", "A"], ["", "B", ""]]
    static let dpad = [["", "UP", ""], ["LEFT", "", "RIGHT"], ["", "DOWN", ""]]
}

/// 一代 Joy-Con 正面比例（任天堂规格 H102 × W35.9 mm），各键位置按任天堂支持页正面图标定（同旧启动器 ControllerMap），
/// 整体缩到弹窗里放得下的高度；圆形按键保持正圆。
enum JoyConGeometry {
    static let height: CGFloat = 340
    static let width: CGFloat = height * 35.9 / 102
    static let upperY: CGFloat = height * 0.255
    static let lowerY: CGFloat = height * 0.535
    static let auxiliaryY: CGFloat = height * 0.72
    static let clusterStep: CGFloat = height * 0.075
    static let buttonDiameter: CGFloat = height * 0.076
    static let stickDiameter: CGFloat = height * 0.156
    static let blue = Color(red: 0, green: 0.765, blue: 0.890)
    static let red = Color(red: 1, green: 0.271, blue: 0.329)
    static let ink = Color(red: 0.17, green: 0.18, blue: 0.20)
}

/// 一只 Joy-Con 的轮廓（内侧是平的滑轨），矢量画；按键是单独的原生按钮，不是截图。
struct JoyConShell: Shape {
    var right = false
    func path(in rect: CGRect) -> Path {
        let w = rect.width, h = rect.height, radius = rect.width * 0.61
        var p = Path()
        p.move(to: CGPoint(x: w - 9, y: 0))
        p.addLine(to: CGPoint(x: radius, y: 0))
        p.addCurve(to: CGPoint(x: 0, y: radius), control1: CGPoint(x: radius * 0.45, y: 0), control2: CGPoint(x: 0, y: radius * 0.45))
        p.addLine(to: CGPoint(x: 0, y: h - radius))
        p.addCurve(to: CGPoint(x: radius, y: h), control1: CGPoint(x: 0, y: h - radius * 0.45), control2: CGPoint(x: radius * 0.45, y: h))
        p.addLine(to: CGPoint(x: w - 9, y: h))
        p.addQuadCurve(to: CGPoint(x: w, y: h - 9), control: CGPoint(x: w, y: h))
        p.addLine(to: CGPoint(x: w, y: 9))
        p.addQuadCurve(to: CGPoint(x: w - 9, y: 0), control: CGPoint(x: w, y: 0))
        p.closeSubpath()
        return right ? p.applying(CGAffineTransform(translationX: w, y: 0).scaledBy(x: -1, y: 1)) : p
    }
}

/// 两只 Joy-Con 并排，每个键可点；肩键底下写着它现在的绑定，有重复的键描红框，选中的描奶油色框。
struct InputControllerMap: View {
    let config: InputConfig
    @Binding var selected: String
    var recording: String?
    let names: InputNames
    let text: (String) -> String
    var onSelect: ((String) -> Void)?

    init(config: InputConfig, selected: Binding<String>, recording: String?, names: InputNames, text: @escaping (String) -> String,
         onSelect: ((String) -> Void)? = nil) {
        self.config = config; _selected = selected; self.recording = recording; self.names = names; self.text = text; self.onSelect = onSelect
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(text("map.title")).font(LauncherTheme.TypeStyle.section)
                Text(text("map.hint")).font(.manrope(11.5)).foregroundStyle(.secondary)
            }
            HStack(alignment: .top, spacing: 20) {
                joycon(right: false)
                joycon(right: true)
            }
            Text(text("map.note")).font(.manrope(11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                .frame(width: JoyConGeometry.width * 2 + 20, alignment: .leading)
        }
        .padding(LauncherTheme.Metrics.cardInset)
        .launcherPanel()
    }

    private func pick(_ target: String) { selected = target; onSelect?(target) }

    private func joycon(right: Bool) -> some View {
        let w = JoyConGeometry.width, h = JoyConGeometry.height
        return VStack(spacing: 6) {
            HStack(spacing: 6) { key(right ? "R" : "ZL", wide: true); key(right ? "ZR" : "L", wide: true) }
            ZStack {
                JoyConShell(right: right)
                    .fill(right ? JoyConGeometry.red : JoyConGeometry.blue)
                    .overlay(JoyConShell(right: right).stroke(.black.opacity(0.20), lineWidth: 1.5))
                    .shadow(color: .black.opacity(0.25), radius: 7, y: 5)
                RoundedRectangle(cornerRadius: 2).fill(JoyConGeometry.ink.opacity(0.28)).frame(width: 4, height: h * 0.94)
                    .position(x: right ? 3 : w - 3, y: h * 0.50).accessibilityHidden(true)
                key(right ? "PLUS" : "MINUS", auxiliary: true).position(x: w * (right ? 0.22 : 0.78), y: h * 0.10)
                if right {
                    cluster(ControllerMapLayout.face).position(x: w / 2, y: JoyConGeometry.upperY)
                    stick(right: true).position(x: w / 2, y: JoyConGeometry.lowerY)
                } else {
                    stick(right: false).position(x: w / 2, y: JoyConGeometry.upperY)
                    cluster(ControllerMapLayout.dpad).position(x: w / 2, y: JoyConGeometry.lowerY)
                }
                Image(systemName: right ? "house.fill" : "record.circle")
                    .font(.system(size: 11, weight: .medium)).foregroundStyle(.white.opacity(0.82))
                    .frame(width: 20, height: 20)
                    .background(JoyConGeometry.ink.opacity(0.84), in: right ? AnyShape(Circle()) : AnyShape(RoundedRectangle(cornerRadius: 4)))
                    .position(x: w * (right ? 0.35 : 0.65), y: JoyConGeometry.auxiliaryY).accessibilityHidden(true)
            }
            .frame(width: w, height: h)
        }
    }

    private func cluster(_ rows: [[String]]) -> some View {
        let side = JoyConGeometry.width, mid = side / 2
        return ZStack {
            ForEach(rows.indices, id: \.self) { row in
                ForEach(rows[row].indices, id: \.self) { column in
                    let target = rows[row][column]
                    if !target.isEmpty {
                        key(target).position(x: mid + CGFloat(column - 1) * JoyConGeometry.clusterStep, y: mid + CGFloat(row - 1) * JoyConGeometry.clusterStep)
                    }
                }
            }
        }
        .frame(width: side, height: side)
    }

    private func stick(right: Bool) -> some View {
        let prefix = right ? "RS" : "LS", side = JoyConGeometry.width, mid = side / 2, reach = side * 0.34
        return ZStack {
            key(prefix, onStick: true).position(x: mid, y: mid)
            direction(prefix + "_UP", symbol: "arrow.up").position(x: mid, y: mid - reach)
            direction(prefix + "_DOWN", symbol: "arrow.down").position(x: mid, y: mid + reach)
            direction(prefix + "_LEFT", symbol: "arrow.left").position(x: mid - reach, y: mid)
            direction(prefix + "_RIGHT", symbol: "arrow.right").position(x: mid + reach, y: mid)
        }
        .frame(width: side, height: side)
    }

    private func direction(_ target: String, symbol: String) -> some View {
        let active = selected == target
        return Button { pick(target) } label: {
            Image(systemName: symbol).font(.system(size: 10, weight: .bold))
                .frame(width: 24, height: 24)
                .foregroundStyle(config.conflictingTargets.contains(target) ? LauncherPalette.danger : JoyConGeometry.ink)
                .background(active ? Color.white.opacity(0.95) : .clear, in: RoundedRectangle(cornerRadius: 7))
                .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(active ? LauncherPalette.cream : .clear, lineWidth: 2))
                .contentShape(Rectangle())
        }
        .controllerButtonStyle(.plain)
        .controllerID("input.map." + target)
        .accessibilityLabel(names.target(target)).accessibilityValue(names.short(config, target))
        .accessibilityAddTraits(active ? [.isSelected] : [])
        .help(names.target(target) + " · " + names.short(config, target))
    }

    private func key(_ target: String, wide: Bool = false, onStick: Bool = false, auxiliary: Bool = false) -> some View {
        let active = selected == target
        let conflict = config.conflictingTargets.contains(target)
        let symbol: String? = ["UP": "arrow.up", "DOWN": "arrow.down", "LEFT": "arrow.left", "RIGHT": "arrow.right"][target]
        let label = ["MINUS": "−", "PLUS": "+", "LS": "L3", "RS": "R3"][target] ?? target
        let diameter = onStick ? JoyConGeometry.stickDiameter : auxiliary ? 18 : JoyConGeometry.buttonDiameter
        let shape = RoundedRectangle(cornerRadius: wide ? 7 : diameter / 2)
        return Button { pick(target) } label: {
            VStack(spacing: 3) {
                Group {
                    if let symbol { Image(systemName: symbol).font(.system(size: 11, weight: .semibold)) }
                    else { Text(label).font(.system(size: onStick ? 12 : auxiliary ? 13 : 10.5, weight: .semibold, design: .rounded)) }
                }
                .frame(width: wide ? 52 : diameter, height: wide ? 26 : diameter)
                .foregroundStyle(.white)
                .background(JoyConGeometry.ink, in: shape)
                .overlay(shape.strokeBorder(conflict ? LauncherPalette.danger : active ? LauncherPalette.cream : .black.opacity(0.30),
                                            lineWidth: active || conflict ? 2.5 : 1))
                .shadow(color: .black.opacity(onStick ? 0.25 : 0.08), radius: onStick ? 3 : 1, y: onStick ? 3 : 1)
                if wide {
                    Text(recording == target ? "…" : names.short(config, target))
                        .font(.manrope(10, active ? .bold : .medium))
                        .foregroundStyle(conflict ? LauncherPalette.danger : .secondary)
                        .lineLimit(1).minimumScaleFactor(0.7).frame(width: 54)
                }
            }
            .contentShape(Rectangle())
        }
        .controllerButtonStyle(.plain)
        .controllerID("input.map." + target)
        .accessibilityLabel(names.target(target)).accessibilityValue(names.short(config, target))
        .accessibilityAddTraits(active ? [.isSelected] : [])
        .help(names.target(target) + " · " + names.short(config, target))
    }
}
