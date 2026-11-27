// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import GameController
import SwiftUI
import XCTest
import NXLauncher
@testable import NX2AppleUI

@MainActor final class ControllerPopoverLayoutTests: XCTestCase {
    func testSystemSnapshotHonorsConnectionEventsAndDeviceIdentity() {
        let dualSense = PopoverLayoutController("DualSense Wireless Controller")
        let xbox = PopoverLayoutController("Xbox Wireless Controller")
        XCTAssertEqual(SystemControllers.snapshot([], connected: dualSense).map(\.id), [ObjectIdentifier(dualSense)], "连接通知先于枚举列表时也要显示新设备")
        XCTAssertEqual(SystemControllers.snapshot([dualSense, xbox], disconnected: dualSense).map(\.id), [ObjectIdentifier(xbox)], "断开通知必须移除滞后枚举里的设备，保留其他手柄")
        XCTAssertEqual(SystemControllers.snapshot([dualSense, dualSense], connected: dualSense).count, 1, "枚举和连接通知指向同一设备时只列一次")
        let other = PopoverLayoutController("DualSense Wireless Controller")
        XCTAssertEqual(SystemControllers.snapshot([dualSense, other]).count, 2, "同名的两只实际设备各自保留")
        let switchPad = PopoverLayoutController("Nintendo Switch 2 Pro Controller")
        XCTAssertTrue(SystemControllers.snapshot([switchPad], connected: switchPad).isEmpty, "专用通道设备不重复列入系统手柄")
    }

    func testSystemConnectionDoesNotInheritSwitchHelperFailures() throws {
        let pad = PopoverLayoutController("DualSense Wireless Controller")
        let suite = "s2arm-connection-presentation-" + UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        let order = FileManager.default.temporaryDirectory.appendingPathComponent(suite + ".json")
        defer { defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: order) }
        let controller = NS2LauncherController(socketURL: URL(fileURLWithPath: "/tmp/" + suite + ".sock"), store: defaults,
            orderFile: order, systemControllers: { [pad] }, notifications: NotificationCenter())
        let systems = SystemControllers(pads: [.init(id: ObjectIdentifier(pad), name: "DualSense Wireless Controller", family: .playstation)])
        for zh in [true, false] {
            for phase in [NS2LauncherController.Phase.off, .scanning, .connecting, .failed] {
                controller.setPhaseForTesting(phase)
                // Switch 2 区块与蓝牙区块常驻、各讲各的通道：Switch 的失败只落在 Switch 2 区块，不借 DualSense 的名字
                XCTAssertNotEqual(controller.switch2Title(lang: zh ? "zh" : "en"), "DualSense Wireless Controller",
                                  "Switch 的 \(phase) 不能借用 DualSense 的名字")
                XCTAssertEqual(controller.title(lang: zh ? "zh" : "en"), "DualSense Wireless Controller")
                XCTAssertEqual(controller.buttonStatus, "已连接")
            }
            controller.apply(status: 4, ["status": 4, "kind": 1, "players": 1, "devices": [["kind": 1, "id": "fixture-switch"]]])
            XCTAssertEqual(controller.switch2Title(lang: zh ? "zh" : "en"), "Switch 2 Pro", "混连时 Switch 2 区块写自己的手柄")
            XCTAssertEqual(controller.status, .connected)
            controller.setPhaseForTesting(.handedOff)
            XCTAssertEqual(controller.status, .inUse)
            controller.apply(status: 0, ["status": 0, "kind": 0, "devices": []])
            controller.setPhaseForTesting(.off)
            let another = NSObject()
            let two = SystemControllers(pads: systems.pads + [.init(id: ObjectIdentifier(another), name: "DualSense Wireless Controller", family: .playstation)])
            XCTAssertEqual(controller.switch2Title(lang: zh ? "zh" : "en"), "Joy-Con 2 · Pro 2", "空闲的 Switch 2 区块不能借用系统手柄名称")
        }
    }

    /// #1400：接上蓝牙手柄后面板内容变多，矮屏 / 大字体下面板不能超过屏幕可用高度（顶部被裁、滚不动）：
    ///   封顶后面板高度不超过上限、内容可滚动、各控件仍都在；不封顶（上限够高）时照旧按内容自然高度
    func testPanelCapsHeightAndScrollsWhenTallerThanScreen() async throws {
        _ = NSApplication.shared
        let pad = PopoverLayoutController("DualSense Wireless Controller")
        let suite = "s2arm-popover-cap-" + UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        let order = FileManager.default.temporaryDirectory.appendingPathComponent(suite + ".json")
        defer { defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: order) }
        let controller = NS2LauncherController(socketURL: URL(fileURLWithPath: "/tmp/" + suite + ".sock"), store: defaults,
            orderFile: order, systemControllers: { [pad] }, notifications: NotificationCenter())
        let systems = SystemControllers(pads: [.init(id: ObjectIdentifier(pad), name: "DualSense Wireless Controller", family: .playstation)])
        controller.updateSystem(systems.presentKeys)
        func render(_ cap: CGFloat?) async throws -> (CGFloat, Set<String>) {
            let navigation = LauncherPadNavigation()
            let host = NSHostingView(rootView: Switch2ProConnectionPopover(controller: controller, systems: systems, lang: "zh", maxHeight: cap)
                .environment(\.padControls, navigation))
            let window = NSWindow(contentRect: CGRect(x: 100, y: 100, width: 380, height: 900), styleMask: [.borderless], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
            defer { window.close() }
            for _ in 0..<3 { host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(60)) }
            window.setContentSize(host.fittingSize); host.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(60))
            if let output = ProcessInfo.processInfo.environment["S2A_SNAPSHOT_OUT"], let cap {
                let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                host.cacheDisplay(in: host.bounds, to: bitmap)
                try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                    .write(to: URL(fileURLWithPath: output).appendingPathComponent("controller-popover-capped-\(Int(cap)).png"))
            }
            return (host.fittingSize.height, Set(navigation.controlTargets(in: window).map(\.id)))
        }
        let (natural, allIDs) = try await render(4000)
        XCTAssertGreaterThan(natural, 260, "夹具内容要比 260 高，才能测到封顶")
        let (capped, cappedIDs) = try await render(260)
        XCTAssertLessThanOrEqual(capped, 260.5, "面板高度不超过给定上限")
        XCTAssertTrue(cappedIDs.contains("controller.settings"), "封顶后面板顶部的齿轮仍在（不被裁掉）")
        XCTAssertTrue(allIDs.isSuperset(of: ["controller.settings", "controller.assign"]))
        print("popover-cap natural=\(natural) capped=\(capped)")
    }

    func testNintendoControllersShowIndependentFaceEditor() async throws {
        _ = NSApplication.shared
        let pad = PopoverLayoutController("Pro Controller")
        for channel in [false, true] {
            let suite = "s2arm-popover-nintendo-" + UUID().uuidString
            let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
            let order = FileManager.default.temporaryDirectory.appendingPathComponent(suite + ".json")
            let face = order.deletingPathExtension().appendingPathExtension("face.json")
            defer {
                defaults.removePersistentDomain(forName: suite)
                try? FileManager.default.removeItem(at: order)
                try? FileManager.default.removeItem(at: face)
            }
            // 专用通道（Switch 2 Pro）：不经系统手柄，玩家位是 whole 单元
            if channel { defaults.set(Data(#"{"units":[{"whole":{"id":"pro","kind":4}}]}"#.utf8), forKey: "S2AMacNS2PlayerAssignment") }
            let controller = NS2LauncherController(socketURL: URL(fileURLWithPath: "/tmp/" + suite + ".sock"), store: defaults,
                orderFile: order, systemControllers: { channel ? [] : [pad] }, notifications: NotificationCenter())
            let systems = SystemControllers(pads: channel ? [] : [.init(id: ObjectIdentifier(pad), name: "Pro Controller", family: .nintendoPro)])
            if !channel { controller.updateSystem(systems.presentKeys) }
            let navigation = LauncherPadNavigation()
            let host = NSHostingView(rootView: Switch2ProConnectionPopover(controller: controller, systems: systems, lang: "zh")
                .environment(\.padControls, navigation))
            let window = NSWindow(contentRect: CGRect(origin: CGPoint(x: 100, y: 100), size: host.fittingSize), styleMask: [.borderless], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
            defer { window.close() }
            host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
            navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
            defer { navigation.detach() }
            XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "controller.player.0.faceMap" },
                          "\(channel ? "专用通道" : "系统认的") 任天堂手柄也应显示四键布局方钮")
            navigation.focus("controller.player.0.faceMap"); navigation.handle(.confirm)
            host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
            navigation.focus("controller.face.layout"); navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(150))
            let dropdownIDs = Set(navigation.controlTargets(in: window).map(\.id))
            XCTAssertTrue(dropdownIDs.contains("dropdown.xbox"), "任天堂手柄布局选单包含 Xbox 布局")
            XCTAssertFalse(dropdownIDs.contains("dropdown.switch"), "任天堂手柄布局选单不显示 Switch 布局")
            navigation.focus("dropdown.xbox"); navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(150))
            XCTAssertEqual(controller.faceMaps.mapping(for: .nintendo).code, 0xb1, "任天堂手柄选 Xbox 布局后立刻写入独立配置")
            XCTAssertEqual(controller.faceMaps.mapping(for: .playstation), .identity, "改任天堂四键布局不能影响非任天堂手柄的共用配置")
            navigation.handle(.back)
            host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
        }
    }

    func testLongControllerNamesPreservePanelInsets() async throws {
        _ = NSApplication.shared
        for name in ["DualSense Wireless Controller", "DualSense Edge Wireless Controller — A deliberately long device name"] {
            for zh in [true, false] {
                let pad = PopoverLayoutController(name)
                let suite = "s2arm-popover-layout-" + UUID().uuidString
                let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
                let order = FileManager.default.temporaryDirectory.appendingPathComponent(suite + ".json")
                let controller = NS2LauncherController(socketURL: URL(fileURLWithPath: "/tmp/" + suite + ".sock"), store: defaults,
                    orderFile: order, systemControllers: { [pad] }, notifications: NotificationCenter())
                let systems = SystemControllers(pads: [.init(id: ObjectIdentifier(pad), name: name, family: .playstation)])
                controller.updateSystem(systems.presentKeys)
                let navigation = LauncherPadNavigation()
                let host = NSHostingView(rootView: Switch2ProConnectionPopover(controller: controller, systems: systems, lang: zh ? "zh" : "en")
                    .environment(\.padControls, navigation))
                let size = host.fittingSize
                let window = NSWindow(contentRect: CGRect(origin: CGPoint(x: 100, y: 100), size: size), styleMask: [.borderless], backing: .buffered, defer: false)
                window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
                defer {
                    window.close(); defaults.removePersistentDomain(forName: suite)
                    try? FileManager.default.removeItem(at: order)
                }
                host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
                let frames = navigation.controlTargets(in: window).map { window.convertFromScreen($0.frame) }
                XCTAssertEqual(Set(navigation.controlTargets(in: window).map(\.id)), ["controller.settings", "controller.system.settings", "controller.player.0.faceMap", "controller.player.0", "controller.assign"],
                               "Switch 2 连接 / 断开不给手柄按（只给鼠标）；齿轮、蓝牙设置、玩家选择与它那一行的四键方钮、分配玩家（四键编辑收在子页面）")
                XCTAssertEqual(host.bounds.width, 380, accuracy: 0.5)
                XCTAssertGreaterThanOrEqual(frames.count, 2, "必须测到齿轮和蓝牙设置按钮")
                print("popover-insets name=\(name) zh=\(zh) width=\(host.bounds.width) frames=\(frames)")
                for frame in frames {
                    XCTAssertGreaterThanOrEqual(frame.minX, 19.5, "\(name): 内容左侧必须保留 20 pt，不能被长标题挤出")
                    XCTAssertLessThanOrEqual(frame.maxX, 360.5, "\(name): 内容右侧必须保留 20 pt")
                }
                if let output = ProcessInfo.processInfo.environment["S2A_SNAPSHOT_OUT"], name == "DualSense Wireless Controller", zh {
                    let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                    host.cacheDisplay(in: host.bounds, to: bitmap)
                    try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                        .write(to: URL(fileURLWithPath: output).appendingPathComponent("controller-popover.png"))
                }
                if name == "DualSense Wireless Controller" {
                    navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
                    defer { navigation.detach() }
                    // 四键布局：同齿轮选单的浮层，开着时手柄方向键只在浮层里走，面板高度不变；改了立刻生效，B 关浮层回方钮
                    let panelHeight = host.bounds.height
                    navigation.focus("controller.player.0.faceMap"); navigation.handle(.confirm)
                    host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
                    let sub = Set(navigation.controlTargets(in: window).map(\.id))
                    XCTAssertEqual(sub, ["controller.face.layout", "controller.face.0", "controller.face.1", "controller.face.2", "controller.face.3"],
                                   "浮层开着只能在四键编辑里移动")
                    XCTAssertEqual(host.fittingSize.height, panelHeight, accuracy: 0.5, "浮层不改面板高度")
                    if let output = ProcessInfo.processInfo.environment["S2A_SNAPSHOT_OUT"] {
                        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                        host.cacheDisplay(in: host.bounds, to: bitmap)
                        try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                            .write(to: URL(fileURLWithPath: output).appendingPathComponent("controller-popover-facemap-\(zh ? "zh" : "en").png"))
                    }
                    // B 只关最里面那一层：选单开着按 B 只收选单，浮层还在；改布局立刻写入，再按 B 关浮层回到面板
                    navigation.focus("controller.face.layout"); navigation.handle(.confirm)
                    try await Task.sleep(for: .milliseconds(150))
                    navigation.handle(.back)
                    host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(150))
                    XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "controller.face.layout" }, "选单开着按 B：只关选单，四键浮层还在")
                    navigation.focus("controller.face.layout"); navigation.handle(.confirm)
                    try await Task.sleep(for: .milliseconds(150))
                    navigation.focus("dropdown.switch"); navigation.handle(.confirm)
                    try await Task.sleep(for: .milliseconds(150))
                    XCTAssertEqual(controller.faceMaps.mapping(for: .playstation).code, 0xb1, "改了立刻写入")
                    navigation.handle(.back)
                    host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
                    XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "controller.assign" }, "B 关浮层回到面板")
                    controller.faceMaps.set(nil, for: .playstation)
                }
                if name == "DualSense Wireless Controller", zh {
                    navigation.source.controllers = { [] }
                    navigation.attach(window)
                    defer { navigation.detach() }
                    controller.setAssignmentForTesting(.init())
                    try await Task.sleep(for: .milliseconds(100))
                    let shortcuts = navigation.shortcutTargets(in: window)
                    XCTAssertEqual(shortcuts[.rightTrigger]?.count, 1, "分配玩家的完成按钮必须使用 R2")
                    XCTAssertTrue(shortcuts[.back]?.contains("controller.panel.back") == true, "B 由面板接住（分配中 = 取消分配），不落到系统取消 / Esc（会「叮」一声）")
                    navigation.handle(.back)
                    XCTAssertFalse(controller.assigning, "B 应实际取消当前分配")
                }
            }
        }
    }
}

@MainActor final class ControllerAssignNavigationTests: XCTestCase {
    /// 分配中一代 Joy-Con：系统已当整只手柄读的那一侧，导航只走系统手柄那一路（不再按 HID 换算，免得按一下导航两次、竖握一对的左只「下」被当确认）
    func testGen1NavigationDefersToSystemReadSide() {
        let pair = [(name: "Joy-Con (L/R)", extended: true)]
        XCTAssertTrue(Switch2ProConnectionPopover.gen1ReadBySystem(side: 0, systemPads: pair), "系统合成的一对：左只交给系统")
        XCTAssertTrue(Switch2ProConnectionPopover.gen1ReadBySystem(side: 1, systemPads: pair), "系统合成的一对：右只交给系统")
        XCTAssertFalse(Switch2ProConnectionPopover.gen1ReadBySystem(side: 0, systemPads: [(name: "Joy-Con (L)", extended: false)]), "系统没当整只读（无 extendedGamepad）：照旧 HID 横握换算")
        XCTAssertFalse(Switch2ProConnectionPopover.gen1ReadBySystem(side: 0, systemPads: [(name: "Joy-Con (R)", extended: true)]), "只有右只被系统读：左只照旧 HID")
        XCTAssertFalse(Switch2ProConnectionPopover.gen1ReadBySystem(side: 1, systemPads: [(name: "Joy-Con 2 (R)", extended: true), (name: "Xbox Wireless Controller", extended: true)]), "Joy-Con 2、别的手柄不算一代")
    }

    /// 分配中 Switch 2 按键（原始、未转横握）→ 界面导航：竖握 A/B，单只横握照 NS2JoyCon.c 的转法
    func testSwitch2ButtonsMapToNavigation() {
        func bits(_ n: Int...) -> UInt64 { n.reduce(0) { $0 | (1 << UInt64($1)) } }
        // Pro / 一对（竖握）：A 确认、B 返回、十字键与摇杆方向
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(0), kind: 1, sideways: false), .confirm)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(1), kind: 1, sideways: false), .back)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(14), kind: 1, sideways: false), .right)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(35), kind: 1, sideways: false), .right)
        // 左只横握：下 → A、左 → B；摇杆原始「下」= 横握的右
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(15), kind: 2, sideways: true), .confirm)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(12), kind: 2, sideways: true), .back)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(33), kind: 2, sideways: true), .right)
        // 右只横握：X → A、A → B；摇杆原始「上」= 横握的右
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(2), kind: 3, sideways: true), .confirm)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(0), kind: 3, sideways: true), .back)
        XCTAssertEqual(NS2LauncherController.padAction(ns2Buttons: bits(32), kind: 3, sideways: true), .right)
        // 一代 Joy-Con：只取确认 / 返回
        XCTAssertEqual(NS2LauncherController.padAction(gen1Key: NS2LauncherController.keyDown, side: 0, sideways: true), .confirm)
        XCTAssertEqual(NS2LauncherController.padAction(gen1Key: "X", side: 1, sideways: true), .confirm)
        XCTAssertEqual(NS2LauncherController.padAction(gen1Key: "B", side: 0, sideways: false), .back)
    }

    /// 确认键（ZR 那颗）界面一律显示 A：单只 Joy-Con 没有 ZR
    func testSubmitShortcutDisplaysA() {
        XCTAssertEqual(PadShortcut.rightTrigger.displayed, .primary)
        XCTAssertEqual(PadShortcut.back.displayed, .back)
    }

    /// 分配画面：焦点落在「完成」；Switch 2 连接 / 断开不在手柄导航里（连着时按 A 会断开正在用的手柄）
    func testAssignFocusesDoneAndMainActionIsMouseOnly() async throws {
        _ = NSApplication.shared
        let suite = "s2arm-assign-nav-" + UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        let order = FileManager.default.temporaryDirectory.appendingPathComponent(suite + ".json")
        defer { defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: order) }
        let controller = NS2LauncherController(socketURL: URL(fileURLWithPath: "/tmp/" + suite + ".sock"), store: defaults,
            orderFile: order, systemControllers: { [] }, notifications: NotificationCenter())
        let navigation = LauncherPadNavigation()
        let host = NSHostingView(rootView: Switch2ProConnectionPopover(controller: controller, systems: SystemControllers(pads: []), lang: "en")
            .environment(\.padControls, navigation))
        let window = NSWindow(contentRect: CGRect(origin: CGPoint(x: 100, y: 100), size: host.fittingSize), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
        defer { window.close() }
        navigation.source.controllers = { [] }
        navigation.attach(window); defer { navigation.detach() }
        host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(100))
        XCTAssertFalse(navigation.targets(in: window).contains { $0.id == "controller.main-action" }, "连接 / 断开只给鼠标，手柄导航（含 AX 兜底）都碰不到")
        controller.setAssignmentForTesting(.init())
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(navigation.focusedID, "controller.assign.done", "进分配画面焦点落在「完成」，按 A 就完成")
        navigation.handle(.left)
        XCTAssertEqual(navigation.focusedID, "controller.assign.cancel")
        navigation.handle(.right)
        XCTAssertEqual(navigation.focusedID, "controller.assign.done")
    }
}

@MainActor final class ControllerAcceptanceGen1Tests: XCTestCase {
    /// 分配画面 + 注入：HID 直读那一路走生产换算（gen1AssignAction），系统手柄那一路注入假 GCController（名字照系统给一代 Joy-Con 的名字），
    ///   两路都接到同一个 LauncherPadNavigation，数「完成」被按了几次
    private func assignScreen() async throws -> (NS2LauncherController, LauncherPadNavigation, NSWindow, NSView, () -> Void) {
        _ = NSApplication.shared
        let suite = "s2arm-acceptance-gen1-" + UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        let order = FileManager.default.temporaryDirectory.appendingPathComponent(suite + ".json")
        let controller = NS2LauncherController(socketURL: URL(fileURLWithPath: "/tmp/" + suite + ".sock"), store: defaults,
            orderFile: order, systemControllers: { [] }, notifications: NotificationCenter())
        let navigation = LauncherPadNavigation()
        let host = NSHostingView(rootView: Switch2ProConnectionPopover(controller: controller, systems: SystemControllers(pads: []), lang: "zh")
            .environment(\.padControls, navigation))
        let window = NSWindow(contentRect: CGRect(origin: CGPoint(x: 100, y: 100), size: host.fittingSize), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
        navigation.source.controllers = { [] }
        navigation.attach(window); navigation.source.stop(); navigation.source.isActive = { true }
        host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(100))
        controller.setAssignmentForTesting(.init())
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(navigation.focusedID, "controller.assign.done")
        return (controller, navigation, window, host, { navigation.detach(); window.close(); defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: order) })
    }
    private func shot(_ host: NSView, _ name: String) throws {
        guard let output = ProcessInfo.processInfo.environment["S2A_SETTINGS_SHOTS"] else { return }
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: output).appendingPathComponent(name + ".png"))
    }

    /// v0.5.0 手柄验收 B8：单只一代 Joy-Con 横握（系统没把它当整只手柄读）：按「确认」（左只横握的下）只完成一次
    func testAcceptanceSingleGen1SidewaysConfirmsOnce() async throws {
        let (controller, navigation, _, host, close) = try await assignScreen(); defer { close() }
        try shot(host, "acceptance-B8-assign")
        let left = PlayerAssignment.Gen1Device(id: "gen1-left", side: 0)
        let system = AcceptanceGen1Controller("Joy-Con (L)", extended: false)   // 系统给的单只：无 extendedGamepad，LauncherPadInput 不读
        navigation.source.controllers = { [system] }; navigation.source.tick(now: 0)
        let systemPads = [(name: "Joy-Con (L)", extended: false)]
        let action = Switch2ProConnectionPopover.gen1AssignAction(id: left.id, key: NS2LauncherController.keyDown, units: [], gen1: [left], systemPads: systemPads)
        XCTAssertEqual(action, .confirm, "左只横握：下 = 确认")
        if let action { navigation.handle(action) }
        navigation.source.tick(now: 0.01)
        XCTAssertFalse(controller.assigning, "完成被按了一次：分配结束")
        // 同一下没有第二路：系统手柄那一路没读这只（无 extendedGamepad）
        XCTAssertTrue(navigation.source.controllers().allSatisfy { $0.extendedGamepad == nil })
    }

    /// v0.5.0 手柄验收 B9：一对一代 Joy-Con 竖握（系统合成「Joy-Con (L/R)」整只读）：左只按「下」不会完成分配，只往下走；按 A 完成一次
    func testAcceptanceGen1PairVerticalDownDoesNotFinish() async throws {
        let (controller, navigation, _, host, close) = try await assignScreen(); defer { close() }
        let left = PlayerAssignment.Gen1Device(id: "gen1-left", side: 0), right = PlayerAssignment.Gen1Device(id: "gen1-right", side: 1)
        let system = AcceptanceGen1Controller("Joy-Con (L/R)", extended: true)
        let pad = try XCTUnwrap(system.extendedGamepad)
        navigation.source.controllers = { [system] }; navigation.source.tick(now: 0)
        let systemPads = [(name: "Joy-Con (L/R)", extended: true)]
        // 同一下「下」：HID 那一路交给系统（nil），系统那一路是方向键下
        XCTAssertNil(Switch2ProConnectionPopover.gen1AssignAction(id: left.id, key: NS2LauncherController.keyDown, units: [], gen1: [left, right], systemPads: systemPads),
                     "系统已整只读：HID 不再把左只的下换算成确认")
        pad.dpad.setValueForXAxis(0, yAxis: -1); navigation.source.tick(now: 0.01)
        pad.dpad.setValueForXAxis(0, yAxis: 0); navigation.source.tick(now: 0.02)
        try await Task.sleep(for: .milliseconds(80))
        XCTAssertTrue(controller.assigning, "竖握按下不会点到「完成」")
        try shot(host, "acceptance-B9-after-down")
        // 回到「完成」再按 A：只完成一次（HID 同一下也是 nil）
        navigation.focus("controller.assign.done")
        XCTAssertNil(Switch2ProConnectionPopover.gen1AssignAction(id: right.id, key: "A", units: [], gen1: [left, right], systemPads: systemPads))
        pad.buttonA.setValue(1); navigation.source.tick(now: 0.03)
        XCTAssertFalse(controller.assigning, "A 完成分配")
    }
}

/// 假的系统手柄：名字照系统给一代 Joy-Con 的名字；extended = 系统是否当整只手柄读
private final class AcceptanceGen1Controller: GCController {
    private let name: String
    private let extended: Bool
    private let gamepadSnapshot = GCController.withExtendedGamepad()
    init(_ name: String, extended: Bool) { self.name = name; self.extended = extended; super.init() }
    override var vendorName: String? { name }
    override var productCategory: String { name }
    override var extendedGamepad: GCExtendedGamepad? { extended ? gamepadSnapshot.extendedGamepad : nil }
}

private final class PopoverLayoutController: GCController {
    private let name: String
    private let gamepadSnapshot = GCController.withExtendedGamepad()
    init(_ name: String) { self.name = name; super.init() }
    override var vendorName: String? { name }
    override var productCategory: String { "Fixture Controller" }
    override var extendedGamepad: GCExtendedGamepad? { gamepadSnapshot.extendedGamepad }
}
