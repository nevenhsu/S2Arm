// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import GameController
import SwiftUI
import XCTest
@testable import NXLauncher
@testable import NX2AppleUI

@MainActor final class ControllerInteractionTests: XCTestCase {
    func testVerticalNavigationVisitsTheAdjacentRowBeforePreservingTheColumn() {
        // Screen coordinates: checkbox above the platform row at its far right;
        // title in the platform's column, storage below in the checkbox's column.
        let title = LauncherPadTarget(id: "title", frame: CGRect(x: 260, y: 700, width: 700, height: 50), role: "AXTextField", element: NSObject())
        let checkbox = LauncherPadTarget(id: "checkbox", frame: CGRect(x: 940, y: 550, width: 100, height: 20), role: "AXCheckBox", element: NSObject())
        let first = LauncherPadTarget(id: "first", frame: CGRect(x: 0, y: 300, width: 240, height: 140), role: "AXButton", element: NSObject())
        let second = LauncherPadTarget(id: "second", frame: CGRect(x: 260, y: 300, width: 240, height: 140), role: "AXButton", element: NSObject())
        let location = LauncherPadTarget(id: "location", frame: CGRect(x: 940, y: 200, width: 100, height: 36), role: "AXButton", element: NSObject())
        let cover = LauncherPadTarget(id: "cover", frame: CGRect(x: 0, y: 640, width: 240, height: 140), role: "AXButton", element: NSObject())
        XCTAssertEqual(LauncherPadSpatial.next(from: second.frame, targets: [title, checkbox, first, location], action: .up)?.id, "checkbox")
        XCTAssertEqual(LauncherPadSpatial.next(from: checkbox.frame, targets: [title, first, second, location], action: .down)?.id, "second")
        XCTAssertEqual(LauncherPadSpatial.next(from: location.frame, targets: [title, checkbox, first, second], action: .up)?.id, "second")
        XCTAssertEqual(LauncherPadSpatial.next(from: first.frame, targets: [title, checkbox, second, location], action: .right)?.id, "second")
        XCTAssertEqual(LauncherPadSpatial.next(from: title.frame, targets: [cover, checkbox, first, second, location], action: .down)?.id, "checkbox")
    }

    func testConfirmationHintUsesTheMappedFaceButton() {
        let source = LauncherPadSource()
        let snapshot = GCController.withExtendedGamepad()
        source.controllers = { [snapshot] }
        var symbols: [PadShortcut: String] = [:]
        source.updatePresentation = { _, value in symbols = value }
        source.refreshPresentation()
        XCTAssertEqual(symbols[.primary], "a.circle", "未改映射时，组件选择提示应跟随实体 A")
        XCTAssertEqual(symbols[.back], "b.circle")
    }

    func testShortcutHintsFollowConnectionAndShowOneGlyphPerButton() async throws {
        _ = NSApplication.shared
        let navigation = LauncherPadNavigation()
        let host = NSHostingView(rootView: ShortcutHintPreview().environment(\.padControls, navigation))
        let window = NSWindow(contentRect: CGRect(x: 80, y: 80, width: 680, height: 140), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
        navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
        navigation.source.isActive = { true }
        defer { navigation.detach(); window.close() }
        let snapshot = GCController.withExtendedGamepad()
        for connected in [false, true, false, true] {
            navigation.source.controllers = { connected ? [snapshot] : [] }
            navigation.source.tick()
            host.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(100))
            let targets = navigation.controlTargets(in: window)
            let plain = try XCTUnwrap(targets.first { $0.id == "hint.plain" }).frame.width
            let single = try XCTUnwrap(targets.first { $0.id == "hint.single" }).frame.width
            let aliases = try XCTUnwrap(targets.first { $0.id == "hint.aliases" }).frame.width
            XCTAssertEqual(targets.contains { $0.id == "hint.footer" }, connected, "首页类型的整组手柄提示也要随连接状态显隐")
            if connected {
                XCTAssertGreaterThan(single - plain, 12, "连接后在按钮内显示按键图标")
                XCTAssertEqual(aliases, single, accuracy: 0.5, "同一按钮即使接受两个按键，也只显示一个图标")
            } else {
                XCTAssertEqual(single, plain, accuracy: 0.5, "未连接时应隐藏图标且不留空白")
                XCTAssertEqual(aliases, plain, accuracy: 0.5)
            }
            if let output = ProcessInfo.processInfo.environment["S2A_SETTINGS_SHOTS"] {
                let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                host.cacheDisplay(in: host.bounds, to: bitmap)
                try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                    .write(to: URL(fileURLWithPath: output).appendingPathComponent(connected ? "hints-connected.png" : "hints-disconnected.png"))
            }
        }
    }

    func testConnectionNotificationsHandleStaleSnapshotsWithoutForegroundInput() async throws {
        let center = NotificationCenter()
        let source = LauncherPadSource(notifications: center)
        let snapshot = GCController.withExtendedGamepad()
        var listed: [GCController] = []
        var connected = false
        source.controllers = { listed }; source.isActive = { false }
        source.updatePresentation = { connected = $0; _ = $1 }
        source.start()
        defer { source.stop() }
        XCTAssertFalse(connected)
        center.post(name: .GCControllerDidConnect, object: snapshot)
        XCTAssertTrue(connected, "连接通知先于系统枚举时，也应立即显示提示")
        source.refreshPresentation()
        XCTAssertTrue(connected)
        listed = [snapshot]; source.refreshPresentation()
        center.post(name: .GCControllerDidDisconnect, object: snapshot)
        XCTAssertFalse(connected, "失焦期间收到断开事件也应隐藏提示")
        source.refreshPresentation()
        XCTAssertFalse(connected, "滞后的枚举不能重新显示已断开的手柄")
        listed = []; source.refreshPresentation()
        listed = [snapshot]; source.refreshPresentation()
        XCTAssertFalse(connected, "没有新的连接通知前，旧设备不能因枚举抖动而复活")
        center.post(name: .GCControllerDidConnect, object: snapshot)
        XCTAssertTrue(connected)
    }

    func testRelayPresenceShowsHintsWithoutAControllerInputLease() {
        let source = LauncherPadSource()
        var paired = false, connected = false
        source.controllers = { [] }; source.isActive = { false }
        source.relayConnected = { paired }; source.relayAllowed = { false }
        source.updatePresentation = { connected = $0; _ = $1 }
        source.refreshPresentation()
        XCTAssertFalse(connected)
        paired = true; source.refreshPresentation()
        XCTAssertTrue(connected, "Switch 专用通道已连接，分配玩家或失焦时也应显示提示")
        paired = false; source.refreshPresentation()
        XCTAssertFalse(connected)
    }

    func testManualScrollSelectsTheNextCompleteControlInsteadOfAClippedSliver() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        try await Task.sleep(for: .milliseconds(80))
        fixture.navigation.focus("row.1")
        fixture.scroll.contentView.scroll(to: CGPoint(x: 0, y: 110))
        fixture.scroll.reflectScrolledClipView(fixture.scroll.contentView)
        try await Task.sleep(for: .milliseconds(80))
        XCTAssertEqual(fixture.navigation.focusedID, "row.2", "手动滚动把焦点项裁掉一半时，应移到相邻的完整可见项")
        XCTAssertTrue(fixture.navigation.isFullyVisible(fixture.navigation.focusedID ?? ""))
    }

    func testScrollTargetsStayInsideTheirViewport() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        try await Task.sleep(for: .milliseconds(100))
        let clip = fixture.scroll.contentView
        let viewport = fixture.window.convertToScreen(clip.convert(clip.bounds, to: nil))
        let targets = fixture.navigation.controlTargets(in: fixture.window)
        XCTAssertTrue(targets.contains { $0.id == "row.0" })
        for target in targets where target.id.hasPrefix("row.") {
            XCTAssertTrue(viewport.contains(target.frame), "裁剪后的控件不能把焦点框画到标题或底栏：\(target.id) \(target.frame), viewport=\(viewport)")
        }
        fixture.navigation.focus("row.0")
        clip.scroll(to: CGPoint(x: 0, y: 420))
        fixture.scroll.reflectScrolledClipView(clip)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertNotEqual(fixture.navigation.focusedID, "row.0", "滚动后不能保留已经离开视口的幽灵焦点")
        let focused = try XCTUnwrap(fixture.navigation.controlTargets(in: fixture.window).first { $0.id == fixture.navigation.focusedID })
        XCTAssertEqual(fixture.navigation.focusRingFrame(in: fixture.window), focused.frame, "焦点框必须跟随滚动后的实际控件边界")
    }

    func testDownRevealsTheNextRowBeforeJumpingToFooter() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        try await Task.sleep(for: .milliseconds(100))
        fixture.navigation.focus("row.0")
        for index in 1...11 {
            fixture.navigation.handle(.down)
            try await Task.sleep(for: .milliseconds(30))
            XCTAssertEqual(fixture.navigation.focusedID, "row.\(index)", "下一个内容项应先滚入视口，再到固定底栏")
            XCTAssertTrue(fixture.navigation.isFullyVisible("row.\(index)"))
        }
        fixture.navigation.handle(.down)
        XCTAssertEqual(fixture.navigation.focusedID, "footer")
        fixture.navigation.handle(.up)
        XCTAssertEqual(fixture.navigation.focusedID, "row.11")
    }

    func testControllerSnapshotTraversesTheProductionInputAndShortcutPath() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        try await Task.sleep(for: .milliseconds(80))
        let snapshot = GCController.withExtendedGamepad()
        let pad = try XCTUnwrap(snapshot.extendedGamepad)
        let source = fixture.navigation.source
        source.stop()
        source.controllers = { [snapshot] }
        source.isActive = { true }
        source.tick(now: 0) // require a neutral frame on connect
        fixture.navigation.focus("row.0")
        pad.dpad.setValueForXAxis(0, yAxis: -1)
        source.tick(now: 0.01)
        XCTAssertEqual(fixture.navigation.focusedID, "row.1")
        source.tick(now: 0.2)
        XCTAssertEqual(fixture.navigation.focusedID, "row.1", "长按延迟内不能重复")
        source.tick(now: 0.38)
        XCTAssertEqual(fixture.navigation.focusedID, "row.2", "持续按住方向键应按节奏重复")
        pad.dpad.setValueForXAxis(0, yAxis: 0)
        source.tick(now: 0.4)
        pad.buttonA.setValue(1)
        source.tick(now: 0.41)
        XCTAssertEqual(fixture.rowActions, 1, "A 操作当前项，不能误提交弹窗")
        XCTAssertEqual(fixture.submits, 0)
        pad.buttonA.setValue(0); source.tick(now: 0.42)
        pad.buttonMenu.setValue(1); source.tick(now: 0.43)
        XCTAssertEqual(fixture.submits, 0, "Menu 不再承担提交动作")
        pad.buttonMenu.setValue(0); source.tick(now: 0.44)
        pad.rightTrigger.setValue(1); source.tick(now: 0.45)
        XCTAssertEqual(fixture.submits, 1, "R2 在内容焦点上也能提交底栏动作")
        source.tick(now: 1)
        XCTAssertEqual(fixture.submits, 1, "提交按钮长按不能重复触发")
        pad.rightTrigger.setValue(0); source.tick(now: 1.01)
        pad.buttonB.setValue(1); source.tick(now: 1.02)
        XCTAssertEqual(fixture.cancels, 1)
        source.controllers = { [] }; source.tick(now: 1.03)
        source.controllers = { [snapshot] }; source.tick(now: 1.04)
        XCTAssertEqual(fixture.cancels, 1, "重连接时按住的 B 不能再触发")
        pad.buttonB.setValue(0); source.tick(now: 1.05)
        pad.buttonB.setValue(1); source.tick(now: 1.06)
        XCTAssertEqual(fixture.cancels, 2)
    }

    func testDisabledShortcutCannotDismissOrReachTheParent() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        try await Task.sleep(for: .milliseconds(80))
        let disabled = NSView(frame: CGRect(x: 20, y: 10, width: 120, height: 35))
        fixture.window.contentView?.addSubview(disabled)
        fixture.navigation.register(PadControl(token: UUID(), id: "disabled", scope: "page", view: disabled,
            enabled: false, activate: { XCTFail("禁用动作不能执行") }, adjust: nil, preferred: false, shortcuts: [.secondary]))
        fixture.navigation.focus("row.0")
        fixture.navigation.handle(.filter)
        XCTAssertEqual(fixture.navigation.focusedID, "row.0")
        XCTAssertEqual(fixture.submits, 0)
        XCTAssertEqual(fixture.cancels, 0)
    }

    func testFocusedSubmitOverridesThePageAction() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        let action = NSView(frame: CGRect(x: 140, y: 10, width: 120, height: 35))
        fixture.window.contentView?.addSubview(action)
        var calls = 0
        let token = UUID()
        func register(enabled: Bool) {
            fixture.navigation.register(PadControl(token: token, id: "row.submit", scope: "page", view: action,
                enabled: enabled, activate: { calls += 1 }, adjust: nil, preferred: false,
                shortcuts: [.rightTrigger], shortcutsRequireFocus: true))
        }
        register(enabled: true)
        fixture.navigation.focus("row.0"); fixture.navigation.handle(.submit)
        XCTAssertEqual(fixture.submits, 1)
        XCTAssertEqual(calls, 0, "未选中行内动作时，R2 执行页面主动作")
        fixture.navigation.focus("row.submit"); fixture.navigation.handle(.submit)
        XCTAssertEqual(calls, 1, "选中行内启动时，R2 只执行该任务")
        XCTAssertEqual(fixture.submits, 1)
    }

    func testDisabledDropdownStillOwnsFocusAndDoesNotTriggerParentShortcuts() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        fixture.navigation.focus("row.0")
        let option = NSView(frame: CGRect(x: 100, y: 130, width: 240, height: 40))
        fixture.window.contentView?.addSubview(option)
        fixture.navigation.register(PadControl(token: UUID(), id: "dropdown.disabled", scope: "dropdown", view: option,
            enabled: false, activate: { XCTFail("禁用选项不能执行") }, adjust: nil, preferred: false))
        XCTAssertFalse(fixture.navigation.inMainContent, "全禁用的下拉层也必须隔离底层输入")
        XCTAssertTrue(fixture.navigation.controlTargets(in: fixture.window).isEmpty)
        fixture.navigation.handle(.submit)
        fixture.navigation.handle(.confirm)
        XCTAssertEqual(fixture.submits, 0)
        XCTAssertEqual(fixture.rowActions, 0)
    }

    func testKeyboardBackCancelsWhileRightTriggerCommits() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        var text = "original"
        fixture.navigation.focus("row.0")
        fixture.navigation.editText(value: text, label: "Original text") { text = $0 }
        try await Task.sleep(for: .milliseconds(100))
        fixture.navigation.keyboard?.value = "cancelled"
        fixture.navigation.handle(.back)
        XCTAssertEqual(text, "original", "B 应取消输入，不应悄悄保存")
        XCTAssertNil(fixture.navigation.keyboard)
        XCTAssertEqual(fixture.navigation.focusedID, "row.0")
        fixture.navigation.editText(value: text, label: "Original text") { text = $0 }
        try await Task.sleep(for: .milliseconds(100))
        fixture.navigation.keyboard?.value = "committed"
        fixture.navigation.handle(.submit)
        XCTAssertEqual(text, "committed")
        XCTAssertEqual(fixture.navigation.focusedID, "row.0")
    }

    func testNativeAlertButtonsShowAndExecuteTheirOwnShortcuts() async throws {
        _ = NSApplication.shared
        let alert = NSAlert()
        alert.messageText = "Original controller fixture"
        let primary = alert.addButton(withTitle: "Confirm")
        let cancel = alert.addButton(withTitle: "Cancel")
        primary.keyEquivalent = "\r"; cancel.keyEquivalent = "\u{1b}"
        alert.layout()
        let window = alert.window
        window.isReleasedWhenClosed = false
        let navigation = LauncherPadNavigation()
        navigation.source.controllers = { [] }
        navigation.attach(window)
        defer { navigation.detach(); window.close() }
        UIStage.show(window)
        _ = navigation.targets(in: window)
        XCTAssertNil(primary.image, "未连接手柄时，原生按钮不能显示手柄图标")
        XCTAssertNil(cancel.image)
        let snapshot = GCController.withExtendedGamepad()
        navigation.source.stop(); navigation.source.controllers = { [snapshot] }; navigation.source.isActive = { true }
        navigation.source.tick(now: 0)
        _ = navigation.targets(in: window)
        XCTAssertEqual(primary.image?.name(), "a.circle", "原生确认框的确认钮一律显示 A（单只 Joy-Con 没有 ZR）；按 R2 照样确认")
        XCTAssertNotNil(cancel.image)
        let sink = NativeButtonSink()
        primary.target = sink; primary.action = #selector(NativeButtonSink.submit)
        cancel.target = sink; cancel.action = #selector(NativeButtonSink.cancel)
        snapshot.extendedGamepad?.rightTrigger.setValue(1); navigation.source.tick(now: 0.01)
        XCTAssertEqual(sink.submits, 1)
        navigation.handle(.back)
        XCTAssertEqual(sink.cancels, 1)
        navigation.handle(.confirm)
        XCTAssertEqual(sink.submits, 2, "焦点先落在确认钮：按 A 就是确认")
        navigation.source.controllers = { [] }; navigation.source.tick(now: 0.1)
        _ = navigation.targets(in: window)
        XCTAssertNil(primary.image, "断开后立即恢复原生按钮图标")
        XCTAssertNil(cancel.image)
        navigation.detach()
        XCTAssertNil(primary.image, "解绑时恢复原生按钮，不留下测试状态")
    }

    func testChildPanelShortcutsDoNotLeakAndParentFocusReturns() async throws {
        let fixture = ScrollFixture()
        defer { fixture.close() }
        fixture.navigation.focus("row.1")
        fixture.navigation.handle(.right) // save the parent scope
        let child = NSPanel(contentRect: CGRect(x: 120, y: 120, width: 280, height: 180), styleMask: [.titled], backing: .buffered, defer: false)
        child.isReleasedWhenClosed = false
        let button = NSView(frame: CGRect(x: 20, y: 20, width: 160, height: 40))
        child.contentView?.addSubview(button)
        var submits = 0
        let token = UUID()
        fixture.navigation.register(PadControl(token: token, id: "child.submit", scope: "page", view: button,
            enabled: true, activate: { submits += 1 }, adjust: nil, preferred: true, shortcuts: [.rightTrigger]))
        LauncherWindowPresenter.current.attach(child, fixture.window); UIStage.show(child)
        defer { LauncherWindowPresenter.current.detach(child, fixture.window); LauncherWindowPresenter.current.orderOut(child); child.close() }
        fixture.navigation.handle(.submit)
        XCTAssertEqual(submits, 1)
        XCTAssertEqual(fixture.submits, 0, "子弹窗不能触发父窗口完成")
        fixture.navigation.register(PadControl(token: token, id: "child.submit", scope: "page", view: button,
            enabled: false, activate: { XCTFail("禁用提交不能执行") }, adjust: nil, preferred: true, shortcuts: [.rightTrigger]))
        fixture.navigation.handle(.submit)
        XCTAssertEqual(submits, 1)
        XCTAssertEqual(fixture.submits, 0, "禁用的子弹窗提交也不能回落到父窗口")
        LauncherWindowPresenter.current.detach(child, fixture.window); LauncherWindowPresenter.current.orderOut(child)
        fixture.navigation.handle(.up)
        XCTAssertEqual(fixture.navigation.focusedID, "row.0", "返回父窗口后从之前的 row.1 继续导航")
        XCTAssertEqual(fixture.rowActions, 0)
    }
}

private struct ShortcutHintPreview: View {
    var body: some View {
        VStack(spacing: 16) {
            HStack(spacing: 24) {
                Button("Original action") {}.controllerID("hint.plain")
                Button("Original action") {}.controllerID("hint.single").controllerShortcut(.rightTrigger)
                Button("Original action") {}.controllerID("hint.aliases").controllerShortcut(.rightTrigger, .back)
            }.controllerButtonStyle(LauncherSecondaryButtonStyle())
            Text("Controller navigation")
                .controllerControl(id: "hint.footer", activate: {})
                .controllerConnectedOnly()
        }.padding(24).frame(width: 680, height: 140)
    }
}

@MainActor private final class NativeButtonSink: NSObject {
    var submits = 0, cancels = 0
    @objc func submit() { submits += 1 }
    @objc func cancel() { cancels += 1 }
}

@MainActor private final class ScrollFixture {
    let navigation = LauncherPadNavigation()
    let window: NSWindow
    let scroll = NSScrollView()
    var rowActions = 0, submits = 0, cancels = 0
    init() {
        _ = NSApplication.shared
        window = NSWindow(contentRect: CGRect(x: 80, y: 80, width: 480, height: 340), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let content = NSView(frame: CGRect(x: 0, y: 0, width: 480, height: 340))
        scroll.frame = CGRect(x: 20, y: 60, width: 440, height: 220)
        scroll.hasVerticalScroller = true
        let document = FlippedDocument(frame: CGRect(x: 0, y: 0, width: 420, height: 840))
        scroll.documentView = document
        content.addSubview(scroll)
        window.contentView = content
        for index in 0..<12 {
            let view = NSView(frame: CGRect(x: 20, y: 20 + index * 70, width: 360, height: 40))
            document.addSubview(view)
            navigation.register(PadControl(token: UUID(), id: "row.\(index)", scope: "page", view: view, enabled: true, activate: { [weak self] in self?.rowActions += 1 }, adjust: nil, preferred: false))
        }
        let footer = NSView(frame: CGRect(x: 280, y: 10, width: 160, height: 35))
        content.addSubview(footer)
        navigation.register(PadControl(token: UUID(), id: "footer", scope: "page", view: footer, enabled: true, activate: { [weak self] in self?.submits += 1 }, adjust: nil, preferred: false, shortcuts: [.rightTrigger]))
        let cancel = NSView(frame: CGRect(x: 20, y: 10, width: 100, height: 35))
        content.addSubview(cancel)
        navigation.register(PadControl(token: UUID(), id: "cancel", scope: "page", view: cancel, enabled: true, activate: { [weak self] in self?.cancels += 1 }, adjust: nil, preferred: false, shortcuts: [.back]))
        navigation.source.controllers = { [] }
        navigation.attach(window)
        UIStage.show(window)
    }
    func close() { navigation.detach(); window.close() }
}

private final class FlippedDocument: NSView { override var isFlipped: Bool { true } }
