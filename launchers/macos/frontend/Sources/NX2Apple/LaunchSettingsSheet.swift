// S2Arm（不是上游文件）：一款游戏的启动设置（「开始游戏」旁的齿轮、「更多」菜单里同一入口；11-09 维护者）。
//   设置项只从共享定义表 launchers/shared/game_settings.v1.json 读（iOS 游戏内菜单同一份），这里不另写文案和缺省；
//   读写走作业层 community / communitySave（存这款游戏文件夹的 launch.json），改完下次开始游戏生效，不用重新准备。
//   可用与否按这款的能力清单，不可用时显示原因。「手柄设置」打开现有的手柄设置页。
import SwiftUI
import AppKit
import UniformTypeIdentifiers
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

/// 一项的状态（作业层 community 结果的 mac 字段）：值是 Bool（开关）或 String（选项）。
struct LaunchOption {
    let available: Bool
    let value: Any?
    let reason: String
    /// 这款没有对应的社区补丁（11-13 起只用补丁，60 帧 / 满屏没补丁就是原生游戏）：这一行不出
    let hidden: Bool
    /// 游戏语言（11-14）：这款 control.nacp 支持的选项值（含「跟随系统」）；没有 = 不知道，列定义表全部
    let choices: [String]?
    /// 游戏语言：选的 / 系统语言这款不支持时，作业层写明起局实际用哪种（中英两份，不弹窗）
    let note: [String: String]
    /// 列着但这款用不了的选项值（画质提升：没走直连时的锐利放大）：置灰，原因在 reason
    let blocked: [String]

    init(_ raw: Any?) {
        let row = raw as? [String: Any] ?? [:]
        available = row["available"] as? Bool ?? false
        hidden = row["hidden"] as? Bool ?? false
        value = row["value"]
        reason = row["reason"] as? String ?? ""
        choices = row["choices"] as? [String]
        note = row["note"] as? [String: String] ?? [:]
        blocked = row["blocked"] as? [String] ?? []
    }
}

/// 画面补丁一项（community 结果的 graphics；玩家包一般没有）。
struct LaunchPatch: Identifiable {
    let id: String
    let label: String
    let enabled: Bool
    /// 上一局的实际结果（pack/jobs/community.py patch_result：没生效的原因 / 量到的效果；没有为空）
    var result = ""
}

/// 金手指一条（community 结果的 cheats.items；pack/jobs/cheats.py listing）：来源 library / player，版本不对时 ok 为假并带原因。
struct LaunchCheat: Identifiable {
    let id: String
    let name: String
    let enabled: Bool
    let ok: Bool
    let reason: String
    let fromLibrary: Bool
    let repo: String
    let author: String
    let license: String
    let status: String
    let fixed: Bool   // 写死地址（pack/jobs/cheats.py fixed_address）：标「地址写死，可能不生效」
    let shared: Bool  // 玩家分享、未验证（补丁库「玩家分享」上游，pack/jobs/cheats.py PLAYER_REPO）：打开前提示一句可能坏存档，不拦

    init?(_ raw: [String: Any]) {
        guard let key = raw["key"] as? String else { return nil }
        id = key; name = raw["name"] as? String ?? key; enabled = raw["enabled"] as? Bool ?? false; ok = raw["ok"] as? Bool ?? false
        reason = raw["reason"] as? String ?? ""; fromLibrary = raw["from"] as? String == "library"; repo = raw["repo"] as? String ?? ""
        author = raw["author"] as? String ?? ""; license = raw["license"] as? String ?? ""; status = raw["status"] as? String ?? ""
        fixed = raw["fixed"] as? Bool ?? false; shared = raw["shared"] as? Bool ?? false
    }
}

/// 资源替换类 mod 一份（community 结果的 mods；pack/jobs/mods.py listing）：游戏文件夹 mods/<名字>/romfs，起局按名字升序叠上
struct LaunchMod: Identifiable, Equatable {
    let id: String   // = 名字
    let enabled: Bool   // 关掉 = 作业层改名为 mods/.off-<名字>，起局不叠
    let files: Int
    let bytes: Int64
    init?(_ raw: [String: Any]) {
        guard let name = raw["name"] as? String else { return nil }
        id = name; enabled = raw["enabled"] as? Bool ?? true; files = raw["files"] as? Int ?? 0; bytes = (raw["bytes"] as? NSNumber)?.int64Value ?? 0
    }
}

/// 导入 / 删除 mod（作业层 communitySave modsImport / modsRemove）后的这款 mod 列表与导入说明（modsImported.notes）
struct LaunchMods: Equatable {
    var items: [LaunchMod] = []
    var imported: [String] = []
    var notes: [String] = []
}

extension S2ArmLauncherModel {
    /// 读（change 为空）或改这款的启动设置；change 的键是定义表 id，值 Bool / String，NSNull = 回缺省。
    func launchSettings(_ gameID: String, change: [String: Any]? = nil, patches: [String: Bool]? = nil) async throws -> ([String: LaunchOption], [LaunchPatch]) {
        let all = try await launchSettingsAll(gameID, change: change, patches: patches)
        return (all.0, all.1)
    }

    /// 同上，另带金手指：cheats 改开关（键「文件名/条目名」），cheatsImport 导入 txt（作业层 communitySave，pack/jobs/cheats.py）
    func launchSettingsAll(_ gameID: String, change: [String: Any]? = nil, patches: [String: Bool]? = nil,
                           cheats: [String: Bool]? = nil, cheatsImport: [String]? = nil) async throws -> ([String: LaunchOption], [LaunchPatch], [LaunchCheat], String) {
        let all = try await launchSettingsFull(gameID, change: change, patches: patches, cheats: cheats, cheatsImport: cheatsImport)
        return (all.0, all.1, all.2, all.3)
    }

    /// 同上，另带 S 级每类状态（作业层 sgradeState：widescreen / fps / gfx，作业层；没出这个字段 = 空，界面不画状态句）
    func launchSettingsFull(_ gameID: String, change: [String: Any]? = nil, patches: [String: Bool]? = nil, cheats: [String: Bool]? = nil,
                            cheatsImport: [String]? = nil, cheatsShare: Bool? = nil,
                            modsImport: [String]? = nil, modsRemove: [String]? = nil, modsEnable: [String: Bool]? = nil) async throws -> ([String: LaunchOption], [LaunchPatch], [LaunchCheat], String, [String: SGradeCategoryState], LaunchMods) {
        var extra: [String: Any] = [:]
        if let change { extra["settings"] = change }
        if let patches { extra["graphics"] = patches }
        if let cheats { extra["cheats"] = cheats }
        if let cheatsImport { extra["cheatsImport"] = cheatsImport }
        if let cheatsShare { extra["cheatsShare"] = cheatsShare }
        if let modsImport { extra["modsImport"] = modsImport }
        if let modsRemove { extra["modsRemove"] = modsRemove }
        if let modsEnable { extra["modsEnable"] = modsEnable }
        let result = try await saveJob(extra.isEmpty ? "community" : "communitySave", gameID: gameID, extra: extra)
        let mac = (result["mac"] as? [String: Any] ?? [:]).mapValues(LaunchOption.init)
        let gfx = (result["graphics"] as? [[String: Any]] ?? []).compactMap { g -> LaunchPatch? in
            guard let id = g["id"] as? String else { return nil }
            return LaunchPatch(id: id, label: g["label"] as? String ?? id, enabled: g["enabled"] as? Bool ?? false, result: g["result"] as? String ?? "")
        }
        let ch = result["cheats"] as? [String: Any] ?? [:]
        let sg = (result["sgradeState"] as? [String: Any] ?? [:]).compactMapValues { ($0 as? [String: Any]).map(SGradeCategoryState.init) }
        let done = result["modsImported"] as? [String: Any] ?? [:]
        let mods = LaunchMods(items: (result["mods"] as? [[String: Any]] ?? []).compactMap(LaunchMod.init),
                              imported: done["imported"] as? [String] ?? [], notes: done["notes"] as? [String] ?? [])
        return (mac, gfx, (ch["items"] as? [[String: Any]] ?? []).compactMap(LaunchCheat.init), ch["reason"] as? String ?? "", sg, mods)
    }

    /// 一键重开（作业 sgradeReenable）：key = fullscreen / fps / 补丁编号 / 金手指键；清掉自动关记录、开关设回开
    func sgradeReenable(_ gameID: String, key: String) async throws {
        _ = try await saveJob("sgradeReenable", gameID: gameID, extra: ["key": key])
    }
}

struct LaunchSettingsSheet: View {
    @EnvironmentObject private var loc: Localizer
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var model: S2ArmLauncherModel
    let gameID: String
    /// 「手柄设置」：关掉本页、打开同一款游戏的手柄设置页
    var openInput: () -> Void = {}
    @State private var items: [GameSettingItem] = []
    @State private var options: [String: LaunchOption] = [:]
    @State private var patches: [LaunchPatch] = []
    @State private var cheats: [LaunchCheat] = []
    @State private var cheatsReason = ""
    @State private var mods = LaunchMods()
    /// S 级每类状态（作业层 sgradeState）；满屏 / 60 帧两行按它写状态句、没补丁时也照样出一行灰字说为什么（定稿表）
    @State private var sgrade: [String: SGradeCategoryState] = [:]
    @State private var failure: String?
    /// 正在存的改动先照玩家选的画（键 opt.<id> / patch.<id> / cheat.<id> / mod.<id>），整队存完再按作业层回的状态画。
    /// 以前一改就整页置灰等作业层回话（11-23 维护者：切一个开关全部输入都不能点）；现在只排队、不锁别的控件。
    @State private var shown: [String: Any] = [:]
    @State private var inflight = 0
    @State private var queue: Task<Void, Never>?
    @State private var loaded = false
    /// 待确认的玩家分享金手指 id（改走 SwiftUI .alert 挂在当前 sheet 上，避免在 Toggle 回调里同步 NSAlert.runModal() 导致 macOS 26/27 Liquid Glass 按钮文字空白）
    @State private var pendingSharedCheat: String?

    private var lang: String { loc.lang.sharedCode }
    private func t(_ id: String) -> String { BackendText.launcherText(id, language: lang) }
    private var gameName: String { model.games.first { $0.id == gameID }?.name ?? gameID }
    /// 这款的文件正被别的作业占着（S2ArmLauncherModel.gameFilesHeld）：设置项、金手指、mod 先不让改；别的款在出包时照常能改（#2191）
    private var held: Bool { model.gameFilesHeld(gameID) }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            LauncherSheetHeading(t("display.title"), subtitle: gameName, note: t("display.hint"))
                .padding(LauncherTheme.Space.page)
            if let failure {
                Label { Text(loc.backend(failure)).fixedSize(horizontal: false, vertical: true) } icon: { Image(systemName: "exclamationmark.circle.fill") }
                    .font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.danger)
                    .padding(.horizontal, LauncherTheme.Space.page).padding(.bottom, LauncherTheme.Space.large)
            }
            if !loaded && failure == nil {
                ProgressView().controlSize(.small).frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if loaded {
                ScrollView {
                    VStack(alignment: .leading, spacing: LauncherTheme.Space.section) {
                        // 分组顺序：画面 → S 级优化（社区补丁 + 金手指，金色卡）→ 手柄 → 其他（定义表 launchers/shared/game_settings.v1.json 的 group）
                        ForEach(["display", "sgrade", "controls", "other"], id: \.self) { group in
                            let rows = items.filter { groupOf($0) == group && visible($0) }
                            if !rows.isEmpty {
                                if group == "sgrade" {
                                    SGradeGroupCard(t("display.group.sgrade"), note: t("export.sgrade.hit.note")) { sgradeRows(rows) }
                                } else {
                                    CardGroup(t("display.group." + group)) { groupRows(rows) }
                                }
                            }
                        }
                        PreviousSaveRow(model: model, gameID: gameID, t: t)   // 回到上一份存档（有上一份才显示）
                    }
                    .padding(.horizontal, LauncherTheme.Space.page).padding(.bottom, LauncherTheme.Space.section)
                }
            }
            Spacer(minLength: 0)
            LauncherSheetFooter {
                Spacer()
                Button(t("display.done")) { dismiss() }.controllerButtonStyle(LauncherPrimaryButtonStyle()).keyboardShortcut(.defaultAction).controllerShortcut(.rightTrigger, .back)
                    .controllerID("launch.done")
            }
        }
        .launcherSheet()
        .alert(t("display.cheats.shared.warn.title"),
               isPresented: Binding(get: { pendingSharedCheat != nil }, set: { if !$0 { pendingSharedCheat = nil } })) {
            Button(t("display.cheats.shared.warn.on")) {
                if let id = pendingSharedCheat {
                    pendingSharedCheat = nil
                    submit(["cheat." + id: true]) { await load(nil, cheats: [id: true]) }
                }
            }
            Button(t("action.cancel"), role: .cancel) { pendingSharedCheat = nil }
        } message: {
            Text(t("display.cheats.shared.warn.message"))
        }
        .onExitCommand { dismiss() }
        .task { if !loaded { await load(nil) } }
    }

    /// 满屏 / 帧率解锁（连同帧率档位）放进 S 级金卡最上面（11-15 维护者定：三项挪到一级、金色 S、页面更简洁），定义表里仍在 display 组
    private static let sgradeTop = ["widescreen", "fps60", "fpsPick"]
    private func groupOf(_ item: GameSettingItem) -> String { Self.sgradeTop.contains(item.id) ? "sgrade" : item.group }

    /// S 级优化组：最上面满屏 / 帧率解锁两行（金字 + 金色 S）、帧率档位；下面金手指（标题带金色 S）与默认收起的社区补丁明细
    @ViewBuilder private func sgradeRows(_ rows: [GameSettingItem]) -> some View {
        let p = rows.first { $0.type == "patches" }, c = rows.first { $0.type == "cheats" }
        let top = Self.sgradeTop.compactMap { id in rows.first { $0.id == id } }
        ForEach(Array(top.enumerated()), id: \.element.id) { i, item in
            if i > 0 { CardDivider() }
            if item.id == "fpsPick" { row(item) } else { topRow(item) }
        }
        if !top.isEmpty && (p != nil || c != nil) { CardDivider() }
        SGradeSettingsRows(
            patchesTitle: t("sgrade.cpatch.detail"), patchesNote: p?.detail(lang) ?? "",
            patches: p == nil ? [] : patches.map { SGradeToggle(id: $0.id, label: $0.label, detail: $0.result.isEmpty ? "" : loc.backend($0.result), on: flag("patch." + $0.id, $0.enabled)) },
            cheatsTitle: c?.text(lang) ?? "", cheatsNote: cheats.isEmpty && !cheatsReason.isEmpty ? loc.backend(cheatsReason) : (c?.detail(lang) ?? ""),
            cheats: c == nil ? [] : cheats.map { SGradeToggle(id: $0.id, label: $0.name, detail: cheatDetail($0), on: flag("cheat." + $0.id, $0.enabled), enabled: $0.ok) },
            importTitle: t("display.cheats.import"), busy: held,
            onPatch: { id, on in submit(["patch." + id: on]) { await load(nil, patches: [id: on]) } },
            onCheat: { id, on in toggleCheat(id, on) },
            onImport: { importCheats() },
            summary: { on, all in t("display.cheats.summary").replacingOccurrences(of: "{0}", with: String(on)).replacingOccurrences(of: "{1}", with: String(all)) },
            searchPrompt: t("display.cheats.search"), clearSearchTitle: loc.t("home.clearSearch"),
            patchesSummary: { on, all in t("display.cheats.summary").replacingOccurrences(of: "{0}", with: String(on)).replacingOccurrences(of: "{1}", with: String(all)) },
            cheatsIcon: SGradeCardView.sGold)
    }

    /// 金卡最上面的满屏 / 帧率解锁：同一个开关（作业层 widescreen / fps60），副标题写现在用的是什么；自动关给「重新打开」；真用不了置灰
    @ViewBuilder private func topRow(_ item: GameSettingItem) -> some View {
        let option = options[item.id], cat = item.id == "widescreen" ? "widescreen" : "fps"
        let st = sgrade[cat], on = value(item.id) as? Bool ?? false
        let note = SGradeStateText.rowNote(cat: cat, st, hasFpsPatch: st?.source == "patch" || (st?.target ?? 0) > 0, fpsOn: on, t: t,
                                           date: { $0.formatted(Date.FormatStyle(date: .abbreviated, time: .omitted).locale(Locale(identifier: lang))) },
                                           backend: { loc.backend($0) })
        let reenable = stateLine(item.id)?.reenable
        SGradeTopRow(cat == "fps" ? t("sgrade.row.fps") : item.text(lang),
                     note: option?.available == false ? loc.backend(option?.reason ?? "") : note?.text ?? item.detail(lang), dim: note?.disabled == true) {
            if let reenable {
                Button(t("sgrade.state.reenable")) { submit { await self.reenable(reenable) } }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                    .accessibilityIdentifier("launch." + item.id + ".reenable").disabled(held)
            } else {
                Toggle("", isOn: Binding(get: { on }, set: { v in submit(["opt." + item.id: v]) { await load([item.id: v]) } }))
                    .toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(item.text(lang))
                    .accessibilityIdentifier("launch." + item.id)
                    .disabled(option?.available != true || note?.disabled == true || held)
            }
        }
    }

    @ViewBuilder private func groupRows(_ rows: [GameSettingItem]) -> some View {
        ForEach(Array(rows.enumerated()), id: \.element.id) { i, item in
            if i > 0 { CardDivider() }
            row(item)
        }
    }

    /// 补丁一栏只在这款列得出补丁时出现；开关 / 选项要作业层给了状态才画
    private func visible(_ item: GameSettingItem) -> Bool {
        // 主玩家四键改在顶栏手柄弹窗设置；共享定义和旧布局值仍供其他平台读取。
        //   玩家以前在这里明确选过 Switch / Xbox 布局（不是缺省「自动」）的照样起作用（community.py → S2A_PAD_FACE），这一行就留着让他看得到、改得回（11-15「玩家自行选择优先」）
        if item.id == "padFace" { return (options[item.id]?.value as? String ?? "auto") != "auto" }
        switch item.type {
        case "patches": return !patches.isEmpty
        case "page", "cheats", "mods": return true
        // 满屏 / 60 帧：没补丁原先整行藏起来；有状态句（没有补丁 / 版本不同 / 没打入…）就照样出一行灰字说为什么（维护者 11-15：不隐藏）
        default: return options[item.id].map { !$0.hidden || stateLine(item.id) != nil } ?? false
        }
    }

    @ViewBuilder private func row(_ item: GameSettingItem) -> some View {
        let title = item.text(lang)
        switch item.type {
        case "page":
            CardRow(title, detail: item.detail(lang)) {
                Button(t("display.open")) { dismiss(); openInput() }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                    .controllerID("launch." + item.id)
            }
        case "patches":
            CardRow(title, detail: item.detail(lang)) { EmptyView() }
            ForEach(patches) { p in
                CardRow("  " + p.label, detail: p.result.isEmpty ? "" : loc.backend(p.result)) {
                    Toggle("", isOn: Binding(get: { flag("patch." + p.id, p.enabled) }, set: { on in submit(["patch." + p.id: on]) { await load(nil, patches: [p.id: on]) } }))
                        .toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(p.label).disabled(held)
                }
            }
        case "cheats":
            CardRow(title, detail: cheats.isEmpty && !cheatsReason.isEmpty ? loc.backend(cheatsReason) : item.detail(lang)) {
                Button(t("display.cheats.import")) { importCheats() }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                    .accessibilityIdentifier("launch.cheats.import").disabled(held)
            }
            ForEach(cheats) { ch in
                CardRow("  " + ch.name, detail: cheatDetail(ch)) {
                    Toggle("", isOn: Binding(get: { flag("cheat." + ch.id, ch.enabled) }, set: { on in toggleCheat(ch.id, on) }))
                        .toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(ch.name)
                        .accessibilityIdentifier("launch.cheat." + ch.id)
                        .disabled(!ch.ok || held)
                }
            }
        case "text":
            let option = options[item.id]
            CardRow(title, detail: item.detail(lang)) {
                TextField("192.168.1.20", text: Binding(get: { option?.value as? String ?? "" }, set: { value in
                    options[item.id] = LaunchOption(["available": true, "value": value])
                }))
                .textFieldStyle(.roundedBorder).frame(width: 150).accessibilityLabel(title)
                .accessibilityIdentifier("launch." + item.id)
                .onSubmit { submit { await load([item.id: options[item.id]?.value as? String ?? ""]) } }
                .disabled(held)
                Button(t("input.save")) {
                    submit { await load([item.id: options[item.id]?.value as? String ?? ""]) }
                }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(held)
            }
        case "mods":
            CardRow(title, detail: modsNote(item)) {
                HStack(spacing: 10) {
                    ModHelpButton(text: t("mods.help"), label: t("mods.help.label"), id: "launch.mods.help")   // mod 格式说明（11-17 维护者：玩家不知道支持什么、去哪找）
                    Button(t("display.mods.import")) { importMods() }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                        .controllerID("launch.mods.import").disabled(held)
                }
            }
            ForEach(mods.items) { m in
                CardRow("  " + m.id, detail: t("display.mods.files").replacingOccurrences(of: "{0}", with: String(m.files))
                            .replacingOccurrences(of: "{1}", with: ByteCountFormatter.string(fromByteCount: m.bytes, countStyle: .file))) {
                    HStack(spacing: 10) {
                        Button(t("display.mods.remove")) { submit { await load(nil, modsRemove: [m.id]) } }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                            .controllerID("launch.mods.remove." + m.id).disabled(held)
                        Toggle("", isOn: Binding(get: { flag("mod." + m.id, m.enabled) }, set: { on in submit(["mod." + m.id: on]) { await load(nil, modsEnable: [m.id: on]) } }))
                            .toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(m.id)
                            .accessibilityIdentifier("launch.mods.toggle." + m.id).disabled(held)
                    }
                }
            }
        case "choice":
            let option = options[item.id]
            let current = value(item.id) as? String, picked = item.options.first { $0.value == current }
            CardRow(title, detail: !(option?.reason ?? "").isEmpty ? loc.backend(option?.reason ?? "") : option?.note[lang] ?? picked?.detail(lang) ?? item.detail(lang)) {
                LauncherDropdown(options: item.options.filter { o in
                    option?.choices.map { $0.contains(o.value) || o.value == current } ?? true
                }.map { ($0.value, $0.label[lang] ?? $0.value) },
                                 selection: current ?? "", label: title,
                                 disabledOptions: Set(option?.blocked ?? [])) { v in submit(["opt." + item.id: v]) { await load([item.id: v]) } }
                .controllerID("launch." + item.id)
                .disabled(option?.available != true || held)
            }
        default:
            let option = options[item.id], state = stateLine(item.id)
            CardRow(title, detail: option?.available == false ? loc.backend(option?.reason ?? "") : item.detail(lang), detail2: state?.text ?? "") {
                if let key = state?.reenable {
                    Button(t("sgrade.state.reenable")) { submit { await reenable(key) } }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                        .accessibilityIdentifier("launch." + item.id + ".reenable").disabled(held)
                } else if option?.hidden != true {   // 包里没有这一类的补丁：只写为什么，不给开关
                    Toggle("", isOn: Binding(get: { value(item.id) as? Bool ?? false }, set: { on in submit(["opt." + item.id: on]) { await load([item.id: on]) } }))
                        .toggleStyle(LauncherSwitchStyle()).labelsHidden().accessibilityLabel(title)
                        .accessibilityIdentifier("launch." + item.id)
                        .disabled(option?.available != true || held)
                }
            }
        }
    }

    /// 满屏 / 60 帧那一行的状态句（SGradeStateText，定稿表）；其余设置项没有
    private func stateLine(_ id: String) -> (text: String, reenable: String?)? {
        let cat = id == "widescreen" ? "widescreen" : id == "fps60" ? "fps" : ""
        guard !cat.isEmpty, let s = sgrade[cat] else { return nil }
        return SGradeStateText.line(cat: cat, catName: t("export.sgrade.cat." + cat), s, t: t,
                                    date: { $0.formatted(Date.FormatStyle(date: .abbreviated, time: .omitted).locale(Locale(identifier: lang))) },
                                    backend: { loc.backend($0) })
    }
    private func reenable(_ key: String) async {
        do { try await model.sgradeReenable(gameID, key: key) } catch { failure = (error as? JobClientError)?.message ?? error.localizedDescription }
        await load(nil)
    }

    /// 选项现在该画的值：正在存的先照玩家选的，否则作业层回的
    private func value(_ id: String) -> Any? { shown["opt." + id] ?? options[id]?.value }
    private func flag(_ key: String, _ stored: Bool) -> Bool { shown[key] as? Bool ?? stored }

    /// 一次改动：画面先照改（shown），作业层按点的顺序一件件存（同一份设置文件，不并发写）；别的控件照常能点。
    /// 整队存完才清掉 shown、全按作业层回的画——中途回来的结果不会把还在排队的开关弹回去；存失败也是在这时退回原值（failure 那行写原因）。
    private func submit(_ show: [String: Any] = [:], _ work: @escaping @MainActor () async -> Void) {
        shown.merge(show) { $1 }
        inflight += 1
        let previous = queue
        queue = Task { @MainActor in
            await previous?.value
            await work()
            inflight -= 1
            if inflight == 0 { shown = [:] }
        }
    }

    /// 每条写清来源与作者（维护者 11-12：每条在游戏内和启动器里写清作者和来源库）；版本不对写原因；运行中写 Cheats.cs 的状态
    private func cheatDetail(_ ch: LaunchCheat) -> String {
        if !ch.ok { return loc.backend(ch.reason) }
        var parts = [ch.fromLibrary ? t("display.cheats.library").replacingOccurrences(of: "{0}", with: ch.repo + (ch.license.isEmpty ? "" : "（\(ch.license)）"))
                                    : t("display.cheats.player")]
        if !ch.author.isEmpty { parts.append(t("display.cheats.author").replacingOccurrences(of: "{0}", with: ch.author)) }
        if !ch.status.isEmpty { parts.append(ch.status) }
        if ch.fixed { parts.append(t("display.cheats.fixedAddress")) }
        return parts.joined(separator: " · ")
    }

    /// 打开玩家分享、未验证的条目前提示一句（可能坏存档），不拦：玩家点「仍然打开」照开
    private func toggleCheat(_ id: String, _ on: Bool) {
        if on, cheats.first(where: { $0.id == id })?.shared == true {
            pendingSharedCheat = id
        } else {
            submit(["cheat." + id: on]) { await load(nil, cheats: [id: on]) }
        }
    }

    /// 刚导入的说明（导入了哪几份、哪些没收及原因，作业层 mods.import_paths notes）优先，否则定义表的说明
    private func modsNote(_ item: GameSettingItem) -> String {
        let said = (mods.imported.isEmpty ? [] : [t("display.mods.imported").replacingOccurrences(of: "{0}", with: mods.imported.joined(separator: loc.t("s2arm.punct.list")))])
            + mods.notes.map { loc.backend($0) }
        return said.isEmpty ? item.detail(lang) : said.joined(separator: "\n")
    }

    /// 选 zip 或文件夹（可多选），交作业层找 romfs、存进这款游戏文件夹的 mods/
    private func importMods() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true; panel.canChooseDirectories = true; panel.canChooseFiles = true
        panel.allowedContentTypes = [.zip, .folder]
        guard LauncherWindowPresenter.current.runPanel(panel) == .OK, !panel.urls.isEmpty else { return }
        let paths = panel.urls.map(\.path)
        submit { await load(nil, modsImport: paths) }
    }

    /// 缺省勾不勾「分享给其他玩家」（维护者定，改这一行；同 pack/jobs/cheats.py SHARE_DEFAULT、iOS kS2ACheatsShareDefault）。玩家上次的选择记在 App 级
    static let shareDefault = false   // 11-15 维护者「金手指分享缺省不勾」
    private func importCheats() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true; panel.canChooseDirectories = false
        panel.allowedContentTypes = [.plainText]
        // 玩家分享（11-15）：勾了就把这次导入的金手指原文 + 游戏版本传给我们（diag.s2arm.com/v1/cheats），收进补丁库「玩家分享」，别的玩家出包时带上
        let box = NSButton(checkboxWithTitle: t("display.cheats.share"), target: nil, action: nil)
        box.state = (UserDefaults.standard.object(forKey: "S2ACheatsShare") as? Bool ?? Self.shareDefault) ? .on : .off
        box.toolTip = t("display.cheats.share.note"); box.setAccessibilityIdentifier("launch.cheats.share")
        let note = NSTextField(wrappingLabelWithString: t("display.cheats.share.note")); note.font = .systemFont(ofSize: NSFont.smallSystemFontSize); note.textColor = .secondaryLabelColor
        let stack = NSStackView(views: [box, note]); stack.orientation = .vertical; stack.alignment = .leading; stack.edgeInsets = NSEdgeInsets(top: 8, left: 12, bottom: 8, right: 12)
        stack.widthAnchor.constraint(equalToConstant: 460).isActive = true
        panel.accessoryView = stack; panel.isAccessoryViewDisclosed = true
        guard LauncherWindowPresenter.current.runPanel(panel) == .OK, !panel.urls.isEmpty else { return }
        let paths = panel.urls.map(\.path), share = box.state == .on
        UserDefaults.standard.set(share, forKey: "S2ACheatsShare")
        submit { await load(nil, cheatsImport: paths, cheatsShare: share) }
    }

    private func load(_ change: [String: Any]?, patches change2: [String: Bool]? = nil, cheats change3: [String: Bool]? = nil, cheatsImport: [String]? = nil, cheatsShare: Bool? = nil,
                      modsImport: [String]? = nil, modsRemove: [String]? = nil, modsEnable: [String: Bool]? = nil) async {
        do {
            if items.isEmpty { items = try GameSettingsSpec.macItems() }
            (options, patches, cheats, cheatsReason, sgrade, mods) = try await model.launchSettingsFull(gameID, change: change, patches: change2, cheats: change3, cheatsImport: cheatsImport, cheatsShare: cheatsShare,
                                                                                                       modsImport: modsImport, modsRemove: modsRemove, modsEnable: modsEnable)
            failure = nil; loaded = true
        } catch {
            failure = (error as? JobClientError)?.message ?? error.localizedDescription
        }
    }
}

/// 开局时的崩溃通知（11-13 维护者改定：不问）：上次开着 X、Y 时游戏退出了，作业层已当场关掉（pack/jobs/cheats.py note_crash），这里只告诉玩家关了哪几条、可去启动设置重开
struct CrashPrompt: Identifiable {
    let id = UUID()
    let game: GameSummary
    let uiLanguage: String?
    let names: [String]
    static let fpsName = "\u{1}fps"   // 帧率补丁那条的占位名，显示时换成界面语言的「帧率」（export.sgrade.cat.fps）
    static let fullscreenName = "\u{1}fs"   // 满屏开关那条的占位名，显示时换成「满屏」（export.sgrade.cat.widescreen）
}

struct CrashPromptHost: View {
    @ObservedObject var model: S2ArmLauncherModel
    @EnvironmentObject var loc: Localizer
    private func t(_ id: String) -> String { BackendText.launcherText(id, language: loc.lang.sharedCode) }
    var body: some View {
        let p = model.crashPrompt
        Color.clear.frame(width: 0, height: 0)
            .alert(t("crash.cheats.title"),
                   isPresented: Binding(get: { model.crashPrompt != nil }, set: { if !$0, model.crashPrompt?.id == p?.id { model.crashPrompt = nil } })) {
                Button(t("crash.cheats.ok"), role: .cancel) { model.crashPrompt = nil }
            } message: {
                Text(t("crash.cheats.body").replacingOccurrences(of: "{0}", with: (p?.names ?? []).map { $0 == CrashPrompt.fpsName ? t("export.sgrade.cat.fps") : $0 == CrashPrompt.fullscreenName ? t("export.sgrade.cat.widescreen") : $0 }.joined(separator: loc.t("s2arm.punct.list"))))
            }
    }
}
