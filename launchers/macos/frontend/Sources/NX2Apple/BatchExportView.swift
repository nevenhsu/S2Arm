// S2Arm（不是上游文件）：批量出包清单页（v0.2.4，维护者批）。出包前：一批游戏、每款勾 iPhone / Mac（可同时勾，表头全选）、一批共用的存放位置；
// 出包中 / 出完：每款的进度与成败、汇总、失败的重试、磁盘满暂停给「释放空间」、出好的直接「推到 iPhone」「在 Mac 上玩」。规则在 pack/jobs/batch.py。
import AppKit
import SwiftUI
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

struct BatchExportView: View {
    @ObservedObject var model: S2ArmLauncherModel
    @EnvironmentObject var loc: Localizer
    @ObservedObject var covers: CoverCache
    let install: (String) -> Void
    let freeSpace: (String) -> Void
    let uiLanguage: String

    var body: some View {
        VStack(spacing: 0) {
            ScrollView(.vertical) {
                VStack(alignment: .leading, spacing: 22) {
                    header
                    if model.batchState == nil { draftList } else { progressList }
                }
                .padding(.horizontal, 40).padding(.top, 22).padding(.bottom, 40)
                .frame(maxWidth: 960).frame(maxWidth: .infinity)
            }
            Divider().opacity(0.3)
            actionBar
        }
        .onExitCommand { model.closeBatch() }
    }

    private func t(_ id: String) -> String { BackendText.launcherText("batch." + id, language: uiLanguage) }

    private var header: some View {
        VStack(alignment: .leading, spacing: 14) {
            Button { model.closeBatch() } label: { Label(loc.t("s2arm.export.back"), systemImage: "chevron.left").font(.manrope(12.5, .semibold)) }
                .controllerButtonStyle(LauncherSecondaryButtonStyle()).accessibilityIdentifier("batch.back")
            LauncherHeadingLine {
                Text(t("title")).font(.manrope(32, .heavy)).tracking(-0.8)
            } subtitle: {
                Text(t("hint")).font(.manrope(13)).foregroundStyle(.secondary).help(t("hint"))
            }
            if model.batchState != nil { summaryLine }
        }
    }

    // ---- 出包前 ----

    private var draftList: some View {
        VStack(alignment: .leading, spacing: 10) {
            locationRow
            HStack {
                Text(t("game")).font(.manrope(12.5, .semibold)).foregroundStyle(.secondary)
                Spacer()
                allButton("iphone", "iPhone"); allButton("mac", "Mac")
            }
            .padding(.horizontal, 14).padding(.top, 6)
            ForEach($model.batchDraft) { $row in
                HStack(spacing: 14) {
                    cover(row.id)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(row.name).font(.manrope(15, .semibold)).lineLimit(1)
                        if let problem = row.problem {   // 只提示、不挡（v0.3.1）：照样能勾，出包时作业层核实
                            Text(problem == "keys" ? t("keys") : loc.backend(problem)).font(.manrope(12.5)).foregroundStyle(LauncherPalette.building).lineLimit(2)
                        }
                    }
                    Spacer(minLength: 12)
                    Toggle("iPhone", isOn: $row.iphone).toggleStyle(PadToggleStyle(.checkbox)).font(.manrope(13)).frame(width: 96, alignment: .leading)
                        .accessibilityIdentifier("batch.iphone." + row.id)
                    Toggle("Mac", isOn: $row.mac).toggleStyle(PadToggleStyle(.checkbox)).font(.manrope(13)).frame(width: 70, alignment: .leading)
                        .accessibilityIdentifier("batch.mac." + row.id)
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .background(LauncherPalette.surface.opacity(0.7), in: RoundedRectangle(cornerRadius: 12))
            }
            ForEach(model.batchUnrecognized) { source in
                HStack(spacing: 14) {
                    Image(systemName: "questionmark.square.dashed").font(.system(size: 22, weight: .light)).frame(width: 64, height: 36)
                    Text((source.path as NSString).lastPathComponent).font(.manrope(15, .semibold)).lineLimit(1)
                    Spacer(minLength: 12)
                    Text(t("unrecognized").replacingOccurrences(of: "{0}", with: loc.backend(source.reason))).font(.manrope(12.5)).foregroundStyle(LauncherPalette.danger).lineLimit(2)
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .background(LauncherPalette.surface.opacity(0.4), in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private func allButton(_ platform: String, _ label: String) -> some View {
        Button { model.batchToggleAll(platform) } label: { Text(t("all") + " " + label).font(.manrope(12.5, .semibold)) }
            .controllerButtonStyle(LauncherSecondaryButtonStyle()).accessibilityIdentifier("batch.all." + platform)
    }

    /// 一批共用一个存放位置（游戏库结构 v3 的存放位置表，同单款出包页）
    private var locationRow: some View {
        var options: [(value: String, label: String)] = model.locations.map { l in
            let free = l.freeBytes.map { ByteCountFormatter.string(fromByteCount: $0, countStyle: .file) } ?? "?"
            return (l.path, S2ArmLauncherModel.locationName(path: l.path, volume: l.volume) + " · " + free)
        }
        if !model.batchLocation.isEmpty, !options.contains(where: { $0.value == model.batchLocation }) {
            options.append((model.batchLocation, (model.batchLocation as NSString).abbreviatingWithTildeInPath))
        }
        options.append(("__other__", BackendText.launcherText("export.location.other", language: uiLanguage)))
        return VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                LauncherRowIcon("externaldrive")
                Text(t("location")).font(.manrope(13)).foregroundStyle(.secondary)
                Spacer(minLength: 12)
                LauncherDropdown(options: options, selection: model.batchLocation, label: t("location")) { value in
                    if value == "__other__" {
                        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
                        if LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url { model.batchLocation = url.path; model.loadLocations(probe: url.path) }
                    } else { model.batchLocation = value }
                }
                .accessibilityIdentifier("batch.location")
            }
            if let slack = model.locationSlackText(model.batchLocation, language: uiLanguage) {   // #444 同单款出包页
                Label(slack, systemImage: "exclamationmark.triangle")
                    .font(.manrope(12)).foregroundStyle(LauncherPalette.building).fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("batch.location.slack")
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 12)
        .background(LauncherPalette.surface.opacity(0.7), in: RoundedRectangle(cornerRadius: 12))
        .onAppear { model.loadLocations() }
    }

    // ---- 出包中 / 出完 ----

    private var summaryLine: some View {
        let s = model.batchSummary
        let text = t("summary").replacingOccurrences(of: "{0}", with: "\(s["total"] as? Int ?? 0)")
            .replacingOccurrences(of: "{1}", with: "\(s["completed"] as? Int ?? 0)").replacingOccurrences(of: "{2}", with: "\(s["failed"] as? Int ?? 0)")
        return VStack(alignment: .leading, spacing: 6) {
            Text(text).font(.manrope(15, .semibold))
            if model.batchPaused == "disk_full" {
                Text(t("paused")).font(.manrope(13)).foregroundStyle(LauncherPalette.building).fixedSize(horizontal: false, vertical: true)
            } else if !model.batchRunning, model.batchStoppedAt > 0 {   // 上次没出完（启动器退出 / 取消）：停在第几款，「继续」只出还没出的（剃刀 H13）
                Text(t("stopped_at").replacingOccurrences(of: "{0}", with: "\(model.batchStoppedAt)")).font(.manrope(13)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let error = model.batchError {
                Text(t("stopped").replacingOccurrences(of: "{0}", with: loc.backend(error))).font(.manrope(13)).foregroundStyle(LauncherPalette.building)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var progressList: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(model.batchItems.enumerated()), id: \.offset) { _, item in
                let gid = item["gameId"] as? String ?? ""
                let results = item["results"] as? [String: [String: Any]] ?? [:]
                let platforms = item["platforms"] as? [String] ?? []
                HStack(alignment: .center, spacing: 14) {
                    cover(gid)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(item["name"] as? String ?? gid).font(.manrope(15, .semibold)).lineLimit(1)
                        ForEach(platforms, id: \.self) { p in platformLine(gid, p, results[p] ?? [:]) }
                    }
                    Spacer(minLength: 12)
                    rowActions(gid, platforms, results)
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .background(LauncherPalette.surface.opacity(0.7), in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private func platformLine(_ gid: String, _ p: String, _ r: [String: Any]) -> some View {
        let state = r["state"] as? String ?? "pending"
        let running = state == "running" && model.session?.gameID == gid
        let label = p == "iphone" ? "iPhone" : "Mac"
        var text = label + " · " + t("state." + state)
        if running, let stage = model.session?.stage, !stage.isEmpty { text += " · " + loc.backend(stage) }
        if state == "pending", r["interrupted"] as? Bool == true { text += " · " + t("interrupted") }
        if state == "failed", let why = r["message"] as? String, !why.isEmpty { text += "：" + loc.backend(why) }
        let color: Color = state == "completed" ? LauncherPalette.ready : state == "failed" ? LauncherPalette.danger : state == "running" ? LauncherPalette.building : .secondary
        return HStack(spacing: 6) {
            Image(systemName: state == "completed" ? "checkmark.circle.fill" : state == "failed" ? "xmark.octagon.fill" : state == "running" ? "circle.dotted" : "circle")
                .foregroundStyle(color)
            Text(text).font(.manrope(12.5)).foregroundStyle(state == "failed" ? LauncherPalette.danger : .secondary).lineLimit(2)
        }
    }

    @ViewBuilder private func rowActions(_ gid: String, _ platforms: [String], _ results: [String: [String: Any]]) -> some View {
        let state = { (p: String) in results[p]?["state"] as? String }
        HStack(spacing: 8) {
            if platforms.contains(where: { state($0) == "failed" }) {
                if results.values.contains(where: { $0["code"] as? String == "disk_full" }) {
                    Button(t("free")) { freeSpace(gid) }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(model.busy).busyHint(model)
                }
                let held = model.mainBusy || model.batchHoldsPlay([gid])   // 正在玩这款：退出这局再重出
                Button(t("retry")) { model.retryBatch(gid) }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(held).busyHint(model, disabled: held).controllerShortcut(.rightTrigger, focused: true)
                    .accessibilityIdentifier("batch.retry." + gid)
            }
            if state("iphone") == "completed" {
                Button(BackendText.launcherText("exportsheet.open", language: uiLanguage)) { install(gid) }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(model.phoneQueued(gid))   // 打开导出确认页（别的作业在跑也能开，开始后排队）
            }
            if state("mac") == "completed", let game = model.games.first(where: { $0.id == gid }) {
                let blocked = !model.canPlay(gid)   // 这批还在出别的款也能玩（游戏槽）；在玩别的 / 全库动作在跑时置灰
                Button(t("play")) { model.play(game, uiLanguage: uiLanguage) }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(blocked).busyHint(model, disabled: blocked).controllerShortcut(.rightTrigger, focused: true)
            }
        }
    }

    private func cover(_ gid: String) -> some View {
        GameArtwork(covers: covers, cover: model.games.first(where: { $0.id == gid })?.cover)
            .frame(width: 64, height: 36).clipShape(RoundedRectangle(cornerRadius: 6))
    }

    // ---- 底栏 ----

    private var actionBar: some View {
        HStack(spacing: 12) {
            if model.batchState == nil {
                let count = model.batchDraft.filter { !$0.platforms.isEmpty }.count
                let held = model.mainBusy || model.batchHoldsPlay(model.batchDraft.filter { !$0.platforms.isEmpty }.map(\.id))   // 玩别的款不挡，玩着清单里的一款挡
                Spacer()
                Button { model.startBatch() } label: {
                    Label(t("start").replacingOccurrences(of: "{0}", with: "\(count)"), systemImage: "shippingbox.fill")
                }
                .controllerButtonStyle(LauncherPrimaryButtonStyle(prominent: true)).disabled(count == 0 || held).busyHint(model, disabled: held)
                .keyboardShortcut(.defaultAction).controllerID("batch.start").controllerShortcut(.rightTrigger)
            } else {
                let s = model.batchSummary
                if model.batchRunning {
                    Spacer()
                    Button(t("cancel")) { model.stop() }.controllerButtonStyle(LauncherSecondaryButtonStyle()).accessibilityIdentifier("batch.cancel")
                } else {
                    // 暂停着（磁盘满）只给「重试」：它解除暂停、重出失败的那个平台；「继续」不解除暂停（审计 P1-164）
                    if (s["failed"] as? Int ?? 0) > 0 || model.batchPaused != nil {
                        let held = model.mainBusy || model.batchHoldsPlay()
                        Button(t("retry_failed")) { model.retryBatch() }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(held).busyHint(model, disabled: held).controllerShortcut(.rightTrigger, focused: true)
                    }
                    if model.batchPaused == nil, (s["cancelled"] as? Int ?? 0) > 0 || (s["pending"] as? Int ?? 0) > 0 {
                        let held = model.mainBusy || model.batchHoldsPlay()
                        Button(t("resume")) { model.resumeBatch() }.controllerButtonStyle(LauncherSecondaryButtonStyle()).disabled(held).busyHint(model, disabled: held)
                            .controllerID("batch.resume").controllerShortcut(.rightTrigger, focused: true)
                    }
                    Spacer()
                    Button(t("close")) { model.closeBatch() }.controllerButtonStyle(LauncherSecondaryButtonStyle()).accessibilityIdentifier("batch.close")
                }
            }
        }
        .padding(.horizontal, 40).padding(.vertical, 16)
        .frame(maxWidth: 960).frame(maxWidth: .infinity)
    }
}
