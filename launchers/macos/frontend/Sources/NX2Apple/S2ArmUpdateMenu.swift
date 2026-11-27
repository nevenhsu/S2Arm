// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import Combine
import SwiftUI
import NXCore

/// 由应用程序的 About 命令组挂载，与窗口共用更新状态。
///   不直接观察整个 model：下载时进度每变一次 model 就发一次变更，菜单跟着重建，展开着的子菜单会被关掉、选不到其他项；
///   只在菜单上显示的内容真的变了才刷新
public struct S2ArmUpdateMenu: View {
    private let model: S2ArmLauncherModel
    @StateObject private var watch: UpdateMenuWatch
    @AppStorage("appLanguage") private var language = ""
    private let activateWindow: () -> Void

    public init(model: S2ArmLauncherModel, activateWindow: @escaping () -> Void) {
        self.model = model
        _watch = StateObject(wrappedValue: UpdateMenuWatch(model: model))
        self.activateWindow = activateWindow
    }

    public var body: some View {
        Button(model.updateMenuTitle(language: (Lang.find(language) ?? .device()).sharedCode)) {
            model.openUpdateMenuItem(activateWindow: activateWindow)
        }
        .disabled(model.isCheckingForUpdates)
        if let driver = model.simulatedUpdate {
            SimulatedUpdateMenu(model: model, driver: driver, activateWindow: activateWindow)
        }
        if ControllerColorSimulator.allowed {   // 本地 / 自测构建才有（发行版带 SUFeedURL）
            ControllerColorSimulatorMenu()
            ControllerTestMenu()   // 手柄测试（同一个开关）：原始体感读数与姿态
        }
    }
}

/// 菜单内容的指纹（标题 + 是否可点 + 有没有模拟入口）；model 的其他变更（例如下载百分比）不会让菜单重建
@MainActor final class UpdateMenuWatch: ObservableObject {
    @Published private(set) var signature: String
    private var cancellable: AnyCancellable?

    init(model: S2ArmLauncherModel) {
        func sign(_ model: S2ArmLauncherModel) -> String {
            "\(model.updateMenuTitle(language: "en"))|\(model.isCheckingForUpdates)|\(model.simulatedUpdate != nil)"
        }
        signature = sign(model)
        // objectWillChange 在值改之前发出，排到下一轮主循环再读新值
        cancellable = model.objectWillChange.receive(on: RunLoop.main).sink { [weak self, weak model] _ in
            guard let self, let model else { return }
            let next = sign(model)
            if next != self.signature { self.signature = next }
        }
    }
}

private struct SimulatedUpdateMenu: View {
    let model: S2ArmLauncherModel
    @ObservedObject var driver: SimulatedUpdateDriver
    let activateWindow: () -> Void

    var body: some View {
        Menu("模擬更新") {
            Button("重置更新狀態") { driver.reset() }
            Button("重置「下載完自動重啟」記憶") {
                model.autoRestartWhenReady = false
                model.updateDefaults.removeObject(forKey: S2ArmLauncherModel.autoRestartWhenReadyKey)
            }
            Divider()
            Button("模擬：啟動時檢查（有新版）") {
                driver.simulateStartupCheck(failed: false)
                if model.updatePopup?["kind"] as? String == "update" { activateWindow() }
            }
            Button("模擬：啟動時檢查（失敗）") { driver.simulateStartupCheck(failed: true) }
            Menu("下次主動檢查結果") {
                Picker("下次主動檢查結果", selection: $driver.checkResult) {
                    ForEach(SimulatedUpdateDriver.CheckResult.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.inline)
            }
            Menu("下次下載結果") {
                Picker("下次下載結果", selection: $driver.downloadResult) {
                    ForEach(SimulatedUpdateDriver.DownloadResult.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.inline)
            }
            Menu("下載速度") {
                Picker("下載速度", selection: $driver.speed) {
                    ForEach(SimulatedUpdateDriver.Speed.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                .pickerStyle(.inline)
            }
            Toggle("模擬：有遊戲 / 任務在跑", isOn: $driver.pretendBusy)
        }
    }
}
