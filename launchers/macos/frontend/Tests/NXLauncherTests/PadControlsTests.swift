// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import Combine
import SwiftUI
import XCTest
import NXLauncher
@testable import NX2AppleUI

@MainActor final class PadControlsTests: XCTestCase {
    func testControllerAdjustsCustomSliderAndSkipsDisabledButton() async throws {
        let state = ControlsState()
        let navigation = LauncherPadNavigation()
        let window = host(ControlsFixture(state: state), navigation: navigation)
        defer { navigation.detach(); window.close() }
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertFalse(navigation.controlTargets(in: window).contains { $0.id == "disabled" })
        navigation.focus("slider")
        navigation.handle(.right)
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertEqual(state.value, 0.6, accuracy: 0.0001)
        navigation.handle(.left)
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertEqual(state.value, 0.5, accuracy: 0.0001)
        navigation.focus("toggle"); navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertTrue(state.enabled)
        navigation.handle(.left)
        XCTAssertFalse(state.enabled)
        navigation.focus("default"); navigation.handle(.confirm)
        XCTAssertEqual(state.invocations, 1, "默认样式按钮也必须接收手柄确认")
    }

    func testDropdownOwnsControllerFocusAndReturnsToItsButton() async throws {
        let state = ControlsState()
        let navigation = LauncherPadNavigation()
        let window = host(ControlsFixture(state: state), navigation: navigation)
        defer { navigation.detach(); window.close() }
        try await Task.sleep(for: .milliseconds(100))
        navigation.focus("choice"); navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(100))
        let targets = navigation.controlTargets(in: window)
        XCTAssertEqual(Set(targets.map(\.id)), ["dropdown.one", "dropdown.two", "dropdown.three"])
        navigation.handle(.down)
        XCTAssertEqual(navigation.focusedID, "dropdown.three", "打开时从当前选项 two 往下移动")
        navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(180))
        XCTAssertEqual(state.choice, "three")
        navigation.handle(.up)
        XCTAssertFalse(navigation.focusedID?.hasPrefix("dropdown.") == true)
        XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "choice" })
    }

    func testSGradeCheckboxAcceptsControllerInput() async throws {
        let state = ControlsState()
        let navigation = LauncherPadNavigation()
        let card = SGradeCardView(match: nil, failed: false, zh: false,
            on: Binding(get: { state.enabled }, set: { state.enabled = $0 }), x: { $0 })
        let window = host(card, navigation: navigation)
        defer { navigation.detach(); window.close() }
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "export.sgrade.toggle" })
        navigation.focus("export.sgrade.toggle"); navigation.handle(.confirm)
        XCTAssertTrue(state.enabled)
        navigation.handle(.left)
        XCTAssertTrue(state.enabled, "复选框只由确认键切换，方向键留给导航")
    }

    func testDropdownKeepsBlockedChoicesOutOfControllerActions() async throws {
        let state = ControlsState()
        let navigation = LauncherPadNavigation()
        let window = host(ControlsFixture(state: state, disabledOptions: ["three"]), navigation: navigation)
        defer { navigation.detach(); window.close() }
        try await Task.sleep(for: .milliseconds(100))
        navigation.focus("choice"); navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertEqual(Set(navigation.controlTargets(in: window).map(\.id)), ["dropdown.one", "dropdown.two"],
                       "主线返回的 blocked 选项仍展示，但不能被手柄激活")
        navigation.focus("dropdown.three"); navigation.handle(.confirm)
        XCTAssertEqual(state.choice, "two", "禁用档位不能覆盖当前选择")
    }

    func testCapsuleButtonStylesReportCapsuleFocusRing() async throws {
        XCTAssertEqual(LauncherPrimaryButtonStyle().padRingRadius, LauncherTheme.Metrics.controlRadius)
        XCTAssertEqual(LauncherPrimaryButtonStyle(capsule: true).padRingRadius, .infinity)
        XCTAssertEqual(LauncherPrimaryButtonStyle(capsule: true).padRingOutset, 3)
        XCTAssertEqual(LauncherSecondaryButtonStyle().padRingRadius, LauncherTheme.Metrics.controlRadius)
        XCTAssertEqual(LauncherSecondaryButtonStyle(capsule: true).padRingRadius, .infinity)
        XCTAssertEqual(LauncherSecondaryButtonStyle(capsule: true).padRingOutset, 0)
        XCTAssertEqual(LauncherIconButtonStyle().padRingRadius, LauncherTheme.Metrics.controlRadius)
        XCTAssertEqual(LauncherIconButtonStyle(size: 28, cornerRadius: 8).padRingRadius, 8)

        let navigation = LauncherPadNavigation()
        let content = HStack(spacing: 12) {
            Button("Later") {}.controllerButtonStyle(LauncherSecondaryButtonStyle(capsule: true)).controllerID("later")
            Button("Update now") {}.controllerButtonStyle(LauncherPrimaryButtonStyle(capsule: true)).controllerID("now")
        }.padding(40)
        let window = host(content, navigation: navigation)
        defer { navigation.detach(); window.close() }
        try await Task.sleep(for: .milliseconds(100))
        let targets = Dictionary(uniqueKeysWithValues: navigation.controlTargets(in: window).map { ($0.id, $0) })
        XCTAssertEqual(targets["later"]?.ringRadius, .infinity)
        XCTAssertEqual(targets["later"]?.ringOutset, 0)
        XCTAssertEqual(targets["now"]?.ringRadius, .infinity)
        XCTAssertEqual(targets["now"]?.ringOutset, LauncherTheme.Focus.creamOutset)
    }

    private func host<V: View>(_ content: V, navigation: LauncherPadNavigation) -> NSWindow {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: NSRect(x: 80, y: 80, width: 500, height: 400), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = NSHostingView(rootView: content.environment(\.padControls, navigation))
        UIStage.show(window); window.contentView?.layoutSubtreeIfNeeded()
        navigation.attach(window)
        return window
    }
}

@MainActor private final class ControlsState: ObservableObject {
    @Published var value = 0.5
    @Published var enabled = false
    @Published var choice = "two"
    @Published var invocations = 0
}
private struct ControlsFixture: View {
    @ObservedObject var state: ControlsState
    var disabledOptions: Set<String> = []
    var body: some View {
        VStack(spacing: 24) {
            Button("Disabled") {}.controllerButtonStyle(.plain).controllerID("disabled").disabled(true)
            LauncherDropdown(options: [("one", "One"), ("two", "Two"), ("three", "Three")], selection: state.choice,
                             disabledOptions: disabledOptions) { state.choice = $0 }
                .controllerID("choice")
            LauncherSlider(value: state.value, range: 0...1, step: 0.1) { state.value = $0 }.controllerID("slider")
            Toggle("Enabled", isOn: $state.enabled).toggleStyle(PadToggleStyle(.switch)).controllerID("toggle")
            Button("Default action") { state.invocations += 1 }.controllerID("default")
        }.controllerButtonStyle(DefaultButtonStyle()).padding(50).frame(width: 500, height: 400).launcherDropdownHost()
    }
}
