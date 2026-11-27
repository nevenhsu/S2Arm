// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import GameController
import LauncherPadBridge
import SwiftUI
import XCTest
@testable import NX2AppleUI
@testable import S2ArmAppleAdapter

final class LauncherPadInputTests: XCTestCase {
    func testConfirmIsAnEdgeAndMustBeReleasedAfterReconnect() {
        var repeatState = LauncherPadRepeater()
        XCTAssertEqual(repeatState.actions(.init(buttons: 1), time: 0), [])
        XCTAssertEqual(repeatState.actions(.init(), time: 0.1), [])
        XCTAssertEqual(repeatState.actions(.init(buttons: 1), time: 0.2), [.confirm])
        for frame in 1...90 { XCTAssertEqual(repeatState.actions(.init(buttons: 1), time: 0.2 + Double(frame) / 60), []) }
        repeatState.reset()
        XCTAssertEqual(repeatState.actions(.init(buttons: 1), time: 2), [])
        _ = repeatState.actions(.init(), time: 2.1)
        XCTAssertEqual(repeatState.actions(.init(buttons: 1), time: 2.2), [.confirm])
    }
    func testDirectionalRepeatAndDeadzone() {
        var state = LauncherPadRepeater()
        _ = state.actions(.init(), time: 0)
        XCTAssertEqual(state.actions(.init(x: 0.2), time: 0.1), [])
        XCTAssertEqual(state.actions(.init(x: 1), time: 0.2), [.right])
        XCTAssertEqual(state.actions(.init(x: 1), time: 0.4), [])
        XCTAssertEqual(state.actions(.init(x: 1), time: 0.57), [.right])
        XCTAssertEqual(state.actions(.init(x: 1), time: 0.60), [])
        XCTAssertEqual(state.actions(.init(y: -1), time: 0.61), [.down])
    }
    func testSystemSnapshotAndHandlerOwnership() throws {
        let controller = GCController.withExtendedGamepad()
        let pad = try XCTUnwrap(controller.extendedGamepad)
        var events = 0
        pad.valueChangedHandler = { _, _ in events += 1 }
        pad.buttonA.setValue(1)
        pad.dpad.setValueForXAxis(0, yAxis: 1)
        let input = LauncherPadInput.system(pad)
        XCTAssertEqual(input.buttons, 1 | 1 << 13)
        let before = events
        pad.valueChangedHandler?(pad, pad.buttonA)
        XCTAssertEqual(events, before + 1, "读取启动器输入不能覆盖已有的系统手柄回调")
    }
    func testSpatialNavigationStaysInRowAndKeepsColumn() {
        let current = CGRect(x: 100, y: 100, width: 60, height: 60)
        let right = LauncherPadTarget(id: "right", frame: CGRect(x: 200, y: 100, width: 60, height: 60), role: "AXButton", element: NSObject())
        let above = LauncherPadTarget(id: "above", frame: CGRect(x: 100, y: 220, width: 60, height: 60), role: "AXButton", element: NSObject())
        XCTAssertEqual(LauncherPadSpatial.next(from: current, targets: [above, right], action: .right)?.id, "right")
        XCTAssertNil(LauncherPadSpatial.next(from: current, targets: [above], action: .right))
        XCTAssertEqual(LauncherPadSpatial.next(from: current, targets: [above, right], action: .up)?.id, "above")
    }
    func testRelayValidatesWireVersionLengthAndPlayer() {
        // Independent v3 wire fixture: 16-byte header, 24-byte state,
        // 56-byte motion, 8 flags, 40-byte payload (macos/ns2_relay.h).
        var bytes = Data(repeating: 0, count: 144)
        func put<T: FixedWidthInteger>(_ value: T, at offset: Int) {
            var little = value.littleEndian
            withUnsafeBytes(of: &little) { bytes.replaceSubrange(offset..<(offset + $0.count), with: $0) }
        }
        put(UInt32(0x534E3252), at: 0); put(UInt16(3), at: 4); put(UInt16(1), at: 6)
        put(UInt64(7), at: 8); put(UInt64(1), at: 16); put(Int32(16384), at: 24)
        bytes[96] = 1; bytes[100] = 1
        var sample = s2a_launcher_pad_sample()
        func decode() -> Int32 { bytes.withUnsafeBytes { s2a_launcher_pad_decode($0.baseAddress, $0.count, &sample) } }
        XCTAssertEqual(decode(), 1); XCTAssertEqual(sample.buttons, 1); XCTAssertEqual(sample.x, 0.5); XCTAssertEqual(sample.epoch, 7)
        bytes[103] = 1
        XCTAssertEqual(decode(), 0); XCTAssertEqual(sample.buttons, 0)
        bytes[103] = 0; bytes[4] = 2
        XCTAssertEqual(decode(), -1); XCTAssertEqual(sample.ready, 0)
        bytes[4] = 3; bytes.removeLast()
        XCTAssertEqual(decode(), -1)
    }
}

@MainActor final class LauncherPadNavigationTests: XCTestCase {
    func testNativeTextFieldWritesBackToSwiftUI() async throws {
        _ = NSApplication.shared
        var value = "old"
        let host = NSHostingView(rootView: TextField("Name", text: Binding(get: { value }, set: { value = $0 })).frame(width: 300).padding())
        let window = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 400, height: 140), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
        defer { window.close() }
        host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(80))
        let field = try XCTUnwrap(LauncherPadNavigation.elements(window).first { S2ALauncherAX.role($0) == "AXTextField" })
        S2ALauncherAX.setValue("new", element: field)
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertEqual(value, "new", "屏幕键盘必须更新 SwiftUI Binding，而非只改原生字段的显示")
    }

    func testChildPanelOwnsFocusAndParentIsRestored() {
        _ = NSApplication.shared
        let parent = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 400, height: 240), styleMask: [.borderless], backing: .buffered, defer: false)
        let child = NSPanel(contentRect: NSRect(x: 90, y: 90, width: 260, height: 160), styleMask: [.borderless], backing: .buffered, defer: false)
        parent.isReleasedWhenClosed = false; child.isReleasedWhenClosed = false
        let sink = PadActionSink()
        let first = NSButton(title: "Parent action", target: sink, action: #selector(PadActionSink.parentAction))
        first.frame = NSRect(x: 20, y: 40, width: 160, height: 40); parent.contentView?.addSubview(first)
        let confirm = NSButton(title: "Child action", target: sink, action: #selector(PadActionSink.childAction))
        confirm.frame = NSRect(x: 20, y: 40, width: 160, height: 40); child.contentView?.addSubview(confirm)
        let navigation = LauncherPadNavigation(); navigation.attach(parent)
        defer { navigation.detach(); LauncherWindowPresenter.current.detach(child, parent); LauncherWindowPresenter.current.orderOut(child); child.close(); parent.close() }
        let parentID = navigation.targets(in: parent).first!.id
        navigation.focus(parentID)
        // Populate the parent's scope before opening the panel.
        navigation.handle(.right)
        LauncherWindowPresenter.current.attach(child, parent); UIStage.show(child)
        XCTAssertFalse(navigation.inMainContent)
        navigation.handle(.confirm)
        XCTAssertEqual(sink.parentCount, 0, "弹窗打开后 A 不能穿透到父窗口")
        XCTAssertEqual(sink.childCount, 1)
        LauncherWindowPresenter.current.detach(child, parent); LauncherWindowPresenter.current.orderOut(child)
        navigation.handle(.left)
        XCTAssertEqual(navigation.focusedID, parentID)
    }

    func testOnScreenKeyboardCommitsAndCancels() async throws {
        _ = NSApplication.shared
        let owner = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 800, height: 500), styleMask: [.borderless], backing: .buffered, defer: false)
        owner.isReleasedWhenClosed = false
        let navigation = LauncherPadNavigation()
        let stringsURL = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("frontend/l10n/en.json")
        let strings = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: stringsURL)) as? [String: Any])
        // Size the keyboard with the real interface copy, as LauncherView configures it.
        navigation.text = { strings[$0] as? String ?? $0 }
        navigation.source.controllers = { [] }
        navigation.attach(owner); navigation.source.stop()
        defer { navigation.detach(); owner.close() }
        var result = "old"
        navigation.editText(value: result, label: "Query") { result = $0 }
        let keyboard = navigation.keyboard!
        XCTAssertTrue(navigation.activeWindow === keyboard.panel)
        try await Task.sleep(for: .milliseconds(80))
        navigation.handle(.confirm)
        XCTAssertEqual(keyboard.value, "oldq", "屏幕键盘的 A 必须实际输入当前焦点字母")
        navigation.focus("pad.keyboard.symbols"); navigation.handle(.confirm)
        XCTAssertTrue(keyboard.symbols)
        try await Task.sleep(for: .milliseconds(50))
        navigation.focus("pad.key.#"); navigation.handle(.confirm)
        XCTAssertEqual(keyboard.value, "oldq#", "密码与路径需要能输入符号页的字符")
        navigation.handle(.filter)
        XCTAssertEqual(keyboard.value, "oldq", "移除首页筛选后，手柄 X 仍须删除屏幕键盘的末尾字符")
        keyboard.value = "new"; keyboard.close(apply: false)
        XCTAssertEqual(result, "old")
        navigation.editText(value: result, label: "Query") { result = $0 }
        navigation.keyboard?.value = "new"
        navigation.keyboard?.close(apply: true)
        XCTAssertEqual(result, "new")
        XCTAssertNil(navigation.keyboard)
    }
    func testNativeButtonsAndDisabledControls() {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 420, height: 220), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let content = NSView(frame: NSRect(x: 0, y: 0, width: 420, height: 220))
        let first = NSButton(title: "First", target: nil, action: nil)
        first.frame = NSRect(x: 20, y: 120, width: 100, height: 40); first.setAccessibilityIdentifier("first")
        let disabled = NSButton(title: "Disabled", target: nil, action: nil)
        disabled.frame = NSRect(x: 150, y: 120, width: 100, height: 40); disabled.isEnabled = false; disabled.setAccessibilityIdentifier("disabled")
        let next = NSButton(title: "Next", target: nil, action: nil)
        next.frame = NSRect(x: 280, y: 120, width: 100, height: 40); next.setAccessibilityIdentifier("next")
        for button in [first, disabled, next] { content.addSubview(button) }
        window.contentView = content
        let navigation = LauncherPadNavigation()
        navigation.attach(window)
        defer { navigation.detach(); window.close() }
        let targets = navigation.targets(in: window)
        XCTAssertEqual(targets.count, 2, "禁用的原生控件不能接收焦点")
        let firstTarget = targets.first { $0.frame.minX < 200 }!
        let nextTarget = targets.first { $0.frame.minX > 200 }!
        navigation.focus(firstTarget.id); navigation.handle(.right)
        XCTAssertEqual(navigation.focusedID, nextTarget.id)
    }
    func testFavoritesRecentAndPinyinPersist() {
        let suite = "s2arm.home.test." + UUID().uuidString
        let store = UserDefaults(suiteName: suite)!
        defer { store.removePersistentDomain(forName: suite) }
        let state = HomeLibraryState(store: store)
        let games = [GameSummary(id: "a", name: "塞尔达传说"), GameSummary(id: "b", name: "Original Game")]
        state.toggleFavorite("b"); state.group = .favorites
        XCTAssertEqual(state.games(games).map(\.id), ["b"])
        state.recordPlay("b"); state.recordPlay("a"); state.recordPlay("b"); state.group = .recent
        XCTAssertEqual(state.games(games).map(\.id), ["b", "a"])
        state.group = .all; state.search = "saierda"
        XCTAssertEqual(state.games(games).map(\.id), ["a"])
        let restored = HomeLibraryState(store: store)
        XCTAssertEqual(restored.favorites, ["b"]); XCTAssertEqual(restored.recent, ["b", "a"])
    }

    func testReadyGroupContainsOnlyGamesThatCanLaunchOnMac() throws {
        XCTAssertEqual(HomeGroup.allCases.map(\.rawValue), ["all", "ready", "recent", "favorites"])
        let ready = try XCTUnwrap(HomeGroup(rawValue: "ready"))
        let suite = "s2arm.home.ready." + UUID().uuidString
        let store = UserDefaults(suiteName: suite)!
        defer { store.removePersistentDomain(forName: suite) }
        let state = HomeLibraryState(store: store)
        let games = [
            GameSummary(id: "mac-a", name: "Alpha", ready: true),
            GameSummary(id: "unprepared", name: "Not compiled"),
            GameSummary(id: "phone-only", name: "iPhone build", iphoneReady: true),
            GameSummary(id: "outdated", name: "Needs rebuild", ready: true, needsPreparation: true),
            GameSummary(id: "mac-b", name: "Beta", ready: true, iphoneReady: true)
        ]
        state.group = ready
        XCTAssertEqual(state.games(games).map(\.id), ["mac-a", "mac-b"])
        state.search = "beta"
        XCTAssertEqual(state.games(games).map(\.id), ["mac-b"])
        state.search = "Needs rebuild"
        XCTAssertTrue(state.games(games).isEmpty)
        state.search = ""
        XCTAssertTrue(state.games(Array(games[1...3])).isEmpty)
        state.group = .all; state.cycle(1)
        XCTAssertEqual(state.group, ready)
        state.cycle(1); XCTAssertEqual(state.group, .recent)
        state.cycle(-1); XCTAssertEqual(state.group, ready)
    }
}

@MainActor private final class PadActionSink: NSObject {
    var parentCount = 0
    var childCount = 0
    @objc func parentAction() { parentCount += 1 }
    @objc func childAction() { childCount += 1 }
}
