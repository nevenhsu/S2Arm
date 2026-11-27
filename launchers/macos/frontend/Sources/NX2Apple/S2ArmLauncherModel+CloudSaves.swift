// S2Arm（不是上游文件）：iCloud 存档同步的 Mac 一侧（方案见 launchers/shared/SaveSync）。
//   玩家在环境页选 iCloud 云盘里的一个文件夹（iPhone / iPad 上选同一个）；开始游戏前取回、游戏退出后上传。
//   导出 / 检查 / 恢复都走现成的存档作业（saveStatus / saveBackup / saveInspect / saveRestore，pack/jobs/saves.py），这里不碰存档目录。
//   同步失败不改本地存档，照常开始游戏，原因写在出包页的错误行上。
import AppKit
import Foundation
import NXModel
import S2ArmAppleAdapter
import SaveSync

extension S2ArmLauncherModel {
    static let cloudDeviceKey = "S2ASaveSync.device"

    var cloudFolder: URL? { provider.settings.cloudSavesFolder.map { URL(fileURLWithPath: $0, isDirectory: true) } }

    func setCloudFolder(_ url: URL?) throws {
        try provider.settings.setCloudSavesFolder(url?.path)
        objectWillChange.send()
    }

    /// 出包页错误行上的那句（本地存档没动，游戏照常开始）
    /// 同步期间别的作业开始了、这次没开局
    static var cloudBusyAfterSync: String { Localizer().t("s2arm.cloud.busyAfterSync") }

    /// 同步失败的原因句：SaveSyncError 只带文件名 / 设备名 / 原因，文字按界面语言取 Localizer 表 s2arm.cloud.error.*（中文与原先 SaveSync 里的句子逐字相同，#316）；别的错误照旧
    static func cloudErrorText(_ error: Error, _ loc: Localizer = Localizer()) -> String {
        func fill(_ key: String, _ args: String...) -> String {
            args.enumerated().reduce(loc.t(key)) { $0.replacingOccurrences(of: "{\($1.offset)}", with: $1.element) }
        }
        switch error {
        case SaveSyncError.notDownloaded(let name): return fill("s2arm.cloud.error.notDownloaded", name)
        case SaveSyncError.corrupt(let name): return fill("s2arm.cloud.error.corrupt", name)
        case SaveSyncError.cloudWriteAfterRestore(let from, let why): return fill("s2arm.cloud.error.writeBack", from, why)
        // 云端那份版本 / 账号对不上或有文件坏了：自动同步不覆盖本机进度（作业层带 sync 时照旧拒绝），告诉玩家可以去存档管理手动恢复
        case let f as JobFailure where ["save_version", "save_wrong_user", "save_damaged"].contains(f.code ?? ""): return loc.t("s2arm.cloud.error.notApplied")
        default: return error.localizedDescription
        }
    }

    static func cloudFailure(_ why: String) -> String { Localizer().t("s2arm.cloud.failed").replacingOccurrences(of: "{0}", with: why) }

    private static func deviceID() -> String {
        if let id = UserDefaults.standard.string(forKey: cloudDeviceKey) { return id }
        let id = UUID().uuidString.lowercased(); UserDefaults.standard.set(id, forKey: cloudDeviceKey); return id
    }

    /// 控制口「立即同步」：和开局前 / 退出后的自动同步互斥（11-09 审查：并发会弹两次冲突窗、重复上传）
    func cloudSyncNow(_ gameID: String) async -> String? {
        guard !cloudSyncing else { return Localizer().t("s2arm.cloud.syncing") }
        cloudSyncing = true
        defer { cloudSyncing = false }
        return await cloudSync(gameID)
    }

    /// 一次同步：没开云存档回 nil；失败回原因（本地存档没动）
    func cloudSync(_ gameID: String) async -> String? {
        guard let folder = cloudFolder else { return nil }
        let key = "S2ASaveSync.base.\(folder.lastPathComponent).\(gameID)"
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("s2a-cloud-sync", isDirectory: true)
        let ports = SaveSyncPorts(
            export: { url in
                guard try await self.saveStatus(gameID).hasSave else { return false }
                _ = try await self.saveBackup(gameID, to: url); return true
            },
            inspect: { url in
                let r = try await self.saveJob("saveInspect", gameID: gameID, extra: ["path": url.path, "sync": true])
                guard let list = (r["manifest"] as? [String: Any])?["files"] as? [[String: Any]] else { throw SaveResultIncomplete() }
                return try list.map {
                    guard let path = $0["path"] as? String, let sha = $0["sha256"] as? String else { throw SaveResultIncomplete() }
                    return SaveFile(path: path, sha256: sha)
                }
            },
            restore: { url in
                let preview = try await self.saveInspect(gameID, source: url, sync: true)   // 自动同步：对不上照旧拒绝，不悄悄换掉进度
                _ = try await self.saveRestore(gameID, preview: preview, sync: true)
            })
        let engine = SaveSyncEngine(folder: CloudSaveFolder(root: folder, gameID: gameID), device: Self.deviceID(),
                                    deviceName: Host.current().localizedName ?? "Mac", platform: "mac", work: work, ports: ports,
                                    loadBase: { UserDefaults.standard.string(forKey: key) }, storeBase: { UserDefaults.standard.set($0, forKey: key) })
        do {
            let outcome = try await engine.sync { choices in await MainActor.run { self.askCloudConflict(choices) } }
            NSLog("S2Arm 云存档：%@ %@", gameID, String(describing: outcome))
            return nil
        } catch {
            NSLog("S2Arm 云存档：【红】%@ 同步失败：%@", gameID, error.localizedDescription)
            return Self.cloudErrorText(error)
        }
    }

    /// 冲突：系统弹窗列出本机和各设备的版本，选一份；「先不处理」两边都不动
    @MainActor func askCloudConflict(_ choices: [SaveSyncChoice]) -> SaveSyncChoice? {
        let loc = Localizer()
        let alert = NSAlert()
        alert.messageText = loc.t("s2arm.cloud.conflict.title")
        alert.informativeText = loc.t("s2arm.cloud.conflict.body")
        let time = DateFormatter(); time.dateStyle = .short; time.timeStyle = .short
        for c in choices.prefix(3) {
            let size = ByteCountFormatter.string(fromByteCount: c.bytes, countStyle: .file)
            alert.addButton(withTitle: c.isLocal ? loc.t("s2arm.cloud.conflict.local").replacingOccurrences(of: "{0}", with: size)
                : loc.t("s2arm.cloud.conflict.remote").replacingOccurrences(of: "{0}", with: c.deviceName)
                    .replacingOccurrences(of: "{1}", with: c.savedAt.map(time.string) ?? "").replacingOccurrences(of: "{2}", with: size))
        }
        alert.addButton(withTitle: loc.t("s2arm.cloud.conflict.later"))
        let index = LauncherWindowPresenter.current.runAlert(alert).rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
        return index >= 0 && index < min(3, choices.count) ? choices[index] : nil
    }
}
