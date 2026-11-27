// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import GameController
import LauncherPadBridge
import SwiftUI
import Vision
import XCTest
import NXCore
import NXModel
@testable import NXLauncher
import S2ArmAppleAdapter
@testable import NX2AppleUI

/// Real SwiftUI pages with original, asset-free job responses; never reads the player's library.
@MainActor final class SettingsLayoutTests: XCTestCase {
    func testHomeIconTooltipsAppearImmediatelyAndPointTowardTheirButtons() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        func descendants(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(descendants) }
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120, 1440] {
                let page = AnyView(LauncherView(model: fixture.model, brand: "Original tooltip fixture")
                    .frame(width: width, height: 760))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                // 游戏库读取期间有的按钮置灰、不登记手柄目标：先等读完，再轮询等目标出现
                for _ in 0..<300 where fixture.model.loading { try await Task.sleep(for: .milliseconds(10)) }
                let anchors = descendants(try XCTUnwrap(window.contentView?.superview))
                    .filter { $0.identifier?.rawValue == "launcher.tooltip.anchor" }
                for (id, above) in [("hero.more", true), ("top.settings", false), ("home.more", false)] {   // 刷新在「更多」菜单里，顶栏图标看「更多」
                    var found = fixture.model.gamepad.controlTargets(in: window).first { $0.id == id }
                    for _ in 0..<300 where found == nil {
                        try await Task.sleep(for: .milliseconds(10))
                        found = fixture.model.gamepad.controlTargets(in: window).first { $0.id == id }
                    }
                    let button = try XCTUnwrap(found, "\(id) 的手柄目标没出现")
                    let anchor = try XCTUnwrap(anchors.first {
                        let frame = window.convertToScreen($0.convert($0.bounds, to: nil))
                        return abs(frame.midX - button.frame.midX) < 2 && abs(frame.midY - button.frame.midY) < 2
                    }, "\(id) 必须使用即时提示，不能等系统 help 的计时器")
                    let event = try XCTUnwrap(NSEvent.enterExitEvent(with: .mouseEntered, location: .zero,
                        modifierFlags: [], timestamp: 0, windowNumber: window.windowNumber, context: nil,
                        eventNumber: 0, trackingNumber: 0, userData: nil))
                    let started = ProcessInfo.processInfo.systemUptime
                    anchor.mouseEntered(with: event)
                    let panel = try XCTUnwrap(UIStage.shared.children(of: window).first { $0.identifier?.rawValue == "launcher.tooltip" && UIStage.shared.isShown($0) },
                        "悬停进入时必须同步显示提示")
                    let elapsed = ProcessInfo.processInfo.systemUptime - started
                    print("tooltip \(language.code)/\(Int(width))/\(id): \(elapsed * 1000) ms, above=\(above)")
                    // 「即时」由上面那句证明：mouseEntered 刚返回就取得到、且可见 = 同步弹出，不靠系统 help 计时器。耗时只打印、不判（11-19：
                    //   原判 < 0.1 秒，负载高时 0.128 秒假红）
                    XCTAssertTrue(panel.ignoresMouseEvents, "提示不能挡住原按钮的鼠标操作")
                    XCTAssertFalse(panel.canBecomeKey, "提示不能夺走键盘或手柄窗口")
                    let frame = window.convertToScreen(anchor.convert(anchor.bounds, to: nil))
                    if above { XCTAssertGreaterThanOrEqual(panel.frame.minY, frame.maxY + 4) }
                    else { XCTAssertLessThanOrEqual(panel.frame.maxY, frame.minY - 4) }
                    XCTAssertTrue(window.frame.insetBy(dx: 4, dy: 4).contains(panel.frame), "提示不能跑出窗口边缘")
                    let label = try XCTUnwrap(descendants(try XCTUnwrap(panel.contentView)).compactMap { $0 as? NSTextField }.first)
                    XCTAssertFalse(label.stringValue.isEmpty)
                    XCTAssertEqual(label.textColor, NSColor.white)
                    XCTAssertGreaterThanOrEqual(label.frame.width, label.intrinsicContentSize.width, "提示文字必须完整显示，包含原生控件的内边距")
                    if width == 1120 { try snapshot(try XCTUnwrap(panel.contentView), "tooltip-\(id)-\(language.code)") }
                    anchor.mouseExited(with: event)
                    XCTAssertFalse(UIStage.shared.isShown(panel), "离开图标立即收起")
                }
                _ = host
            }
        }
    }

    func testControllerSettingsHideSharedFaceBindingsForEveryPlayer() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            for player in 0..<4 {
                let (host, window) = try await render(AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")), fixture: fixture, language: language)
                let navigation = fixture.navigation
                navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
                UIStage.show(window)
                defer { navigation.detach(); window.close() }
                try await settle(window, navigation, fixture.model.gamepad)
                let players = navigation.controlTargets(in: window).filter { abs($0.frame.height - 30) < 0.5 }
                    .sorted { $0.frame.minX < $1.frame.minX }
                XCTAssertEqual(players.count, 4)
                let playerRect = window.convertFromScreen(players[player].frame)
                try press(players[player].id, navigation)
                // 等选中态真画出来再判：固定等 180 ms 在整套负载下偶发取到旧色。每 50 ms 截一次取色，选中即停，最多等 3 秒
                let point = host.convert(CGPoint(x: playerRect.minX + 5, y: playerRect.midY), from: nil)
                var red: CGFloat = 0
                let deadline = Date().addingTimeInterval(3)
                repeat {
                    try await Task.sleep(for: .milliseconds(50))
                    let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                    host.cacheDisplay(in: host.bounds, to: bitmap)
                    let scale = CGFloat(bitmap.pixelsWide) / host.bounds.width
                    let sampleY = host.isFlipped ? point.y : host.bounds.maxY - point.y
                    red = try XCTUnwrap(bitmap.colorAt(x: Int(point.x * scale), y: Int(sampleY * scale))?.usingColorSpace(.deviceRGB)).redComponent
                } while red <= 0.8 && Date() < deadline
                XCTAssertGreaterThan(red, 0.8, "必须实际选中玩家\(player + 1)，不能重复验证玩家 1")
                for target in ["A", "B", "Y", "X", "L"] {
                    navigation.focus("input.map." + target)
                    let key = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == "input.map." + target })
                    _ = key
                    try press("input.map." + target, navigation)
                    if let scroll = scrollViews(in: host).first(where: { $0.hasVerticalScroller }) {
                        scroll.contentView.scroll(to: .zero)
                        scroll.reflectScrolledClipView(scroll.contentView)
                    }
                    try await Task.sleep(for: .milliseconds(70))
                    let targets = navigation.controlTargets(in: window)
                    XCTAssertEqual(targets.contains { $0.id == "input.gamepad" }, target == "L",
                        "\(language.code)/玩家\(player + 1)/\(target)：四键由共享弹窗管理，其他实体键继续可编辑")
                    XCTAssertTrue(targets.contains { $0.id == "input.keyboard.record" }, "四键的独立键盘绑定仍须可用")
                    if player == 3 && target == "X" { try snapshot(host, "input-shared-faces-" + language.code) }
                }
                navigation.focus("input.gamepad"); navigation.handle(.confirm)
                try await waitUntil { navigation.controlTargets(in: window).contains { $0.id == "dropdown.leftShoulder" } }
                let ids = navigation.controlTargets(in: window).map(\.id)
                for code in InputConfig.padCodes.prefix(4) {
                    XCTAssertFalse(ids.contains("dropdown." + code), "所有玩家都不能用逐游戏配置覆盖实体四键")
                }
                XCTAssertTrue(ids.contains("dropdown.leftShoulder"), "其余实体键仍须可绑定")
            }
        }
    }

    func testStorageModeAlignsWithTheHeadingAndContentInBothModes() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(FreeSpaceView(model: fixture.model, gameID: "fixture")), fixture: fixture, language: language)
            let navigation = fixture.navigation
            navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
            defer { navigation.detach(); window.close() }
            for overview in [false, true] {
                try await Task.sleep(for: .milliseconds(80))
                XCTAssertEqual(navigation.shortcutTargets(in: window)[.secondary] != nil, overview, "只有存储总览显示刷新操作，确认切换实际生效")
                let modes = navigation.controlTargets(in: window).filter { abs($0.frame.height - 30) < 0.5 }
                    .sorted { $0.frame.minX < $1.frame.minX }
                XCTAssertEqual(modes.count, 2)
                let first = try XCTUnwrap(modes.first), last = try XCTUnwrap(modes.last)
                let bounds = window.convertToScreen(host.bounds)
                print("storage-mode \(language.code)/\(overview): left=\(first.frame.minX - bounds.minX - 3)")
                XCTAssertEqual(first.frame.minX - bounds.minX, 35, accuracy: 1,
                    "分段控件与页头、内容共用 32 pt 左边距；按钮本身另有 3 pt 内边距")
                XCTAssertEqual(last.frame.maxX - first.frame.minX + 6, 300, accuracy: 1, "仍保持 300 pt 的紧凑宽度")
                try snapshot(host, "storage-mode-\(language.code)-\(overview ? "overview" : "game")")
                if !overview { navigation.focus(last.id); navigation.handle(.confirm) }
            }
        }
    }

    func testPlayResultAlignsWithPageInsetsAndTheSharedPrimaryAction() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogCover = "original-play-cover"
        let artwork = NSImage(size: CGSize(width: 100, height: 100), flipped: false) { rect in
            NSColor(deviceRed: 0.8, green: 0.1, blue: 0.9, alpha: 1).setFill()
            rect.fill(); return true
        }
        let covers = CoverCache { _, _ in CoverCache.Loaded(version: "original", image: artwork) }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        for language in [Lang.zh, .en] {
            for (width, height): (CGFloat, CGFloat) in [(800, 450), (1120, 630), (1440, 810)] {
                let session = JobSession(kind: .play, gameID: game.id, name: game.name,
                    phase: .finished(.completed), stage: "游戏运行中")
                fixture.model.playSession = session
                let page = AnyView(PlaySessionView(model: fixture.model, covers: covers, session: session,
                    uiLanguage: language == .zh ? "zh-Hans" : "en")
                    .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text)
                    .preferredColorScheme(.dark).frame(width: width, height: height))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                for connected in [false, true] {
                    fixture.navigation.presentation.connected = connected
                    try await Task.sleep(for: .milliseconds(80))
                    let back = try XCTUnwrap(fixture.navigation.controlTargets(in: window).first)
                    let bounds = window.convertToScreen(host.bounds)
                    let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                    host.cacheDisplay(in: host.bounds, to: bitmap)
                    let scale = CGFloat(bitmap.pixelsWide) / width
                    // The original magenta cover isolates the information column from text and material shadows.
                    let first = try XCTUnwrap((0..<bitmap.pixelsWide).first { x in
                        guard let color = bitmap.colorAt(x: x, y: Int(80 * scale))?.usingColorSpace(.deviceRGB) else { return false }
                        return color.redComponent - color.greenComponent > 0.3 && color.blueComponent - color.greenComponent > 0.3
                    })
                    let left = CGFloat(first) / scale, buttonFrame = back.frame
                    print("play-result \(language.code)/\(width)/\(connected): left=\(left), right=\(bounds.maxX - buttonFrame.maxX), bottom=\(buttonFrame.minY - bounds.minY), height=\(buttonFrame.height)")
                    XCTAssertEqual(left, 48, accuracy: 1, "游戏信息与游戏库共用 48 pt 左边距")
                    XCTAssertEqual(back.id, "play.back")
                    XCTAssertEqual(bounds.maxX - buttonFrame.maxX, 48, accuracy: 1, "回库按钮应放在右下角，与其他页面右边缘对齐")
                    XCTAssertEqual(buttonFrame.minY - bounds.minY, 16, accuracy: 1)
                    XCTAssertEqual(buttonFrame.height, 48, accuracy: 1, "使用共用的突出主按钮尺寸")
                    XCTAssertTrue(bounds.contains(buttonFrame))
                    let shortcuts = fixture.navigation.shortcutTargets(in: window)
                    XCTAssertEqual(shortcuts[.rightTrigger], ["play.back"])
                    XCTAssertEqual(shortcuts[.back], ["play.back"])
                    try snapshot(host, "play-result-\(language.code)-\(Int(width))-\(connected ? "connected" : "disconnected")")
                }
            }
        }
    }

    func testPlayFooterActionsKeepTheirBehaviorInRunningStoppingAndFailedStates() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        for phase in [JobSession.Phase.running, .stopping, .finished(.failed)] {
            let session = JobSession(kind: .play, gameID: game.id, name: game.name, phase: phase, stage: "游戏运行中")
            fixture.model.playSession = session   // 一局在游戏槽
            let page = AnyView(PlaySessionView(model: fixture.model, covers: CoverCache(), session: session, uiLanguage: "en")
                .background(LauncherWindowMaterial()).frame(width: 800, height: 450))
            let (host, window) = try await render(page, fixture: fixture, language: .en)
            let navigation = fixture.navigation
            navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
            defer { navigation.detach(); window.close() }
            navigation.presentation.connected = true
            try await Task.sleep(for: .milliseconds(80))
            let bounds = window.convertToScreen(host.bounds)
            let actionID = phase == .finished(.failed) ? "play.back" : "play.stop"
            let button = navigation.controlTargets(in: window).first { $0.id == actionID }
            if phase == .stopping {
                XCTAssertNil(button, "正在停止时不能重复发送停止动作")
            } else {
                let frame = try XCTUnwrap(button).frame
                XCTAssertTrue(bounds.contains(frame), "小窗口中失败说明滚动，底栏操作始终可见")
                XCTAssertEqual(bounds.maxX - frame.maxX, 48, accuracy: 1)
                XCTAssertEqual(frame.minY - bounds.minY, 16, accuracy: 1)
                XCTAssertEqual(frame.height, 48, accuracy: 1)
            }
            if phase == .finished(.failed) {
                for action in [LauncherPadAction.confirm, .submit, .back] {
                    fixture.model.playSession = session
                    navigation.focus("play.back"); navigation.handle(action)
                    XCTAssertNil(fixture.model.playSession, "A、R2 与 B 均应执行原有回库操作")
                }
            } else {
                navigation.handle(.submit)
                XCTAssertEqual(fixture.model.playSession?.phase, .stopping)
            }
            XCTAssertFalse(fixture.runner.requests.contains { $0["operation"] as? String == "launch" }, "底栏操作不能意外重启游戏")
            try snapshot(host, "play-footer-\(actionID)-\(phase == .stopping ? "stopping" : "active")")
        }
    }

    func testControllerSettingsScrollKeepsFocusOnVisibleControls() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(EnvironmentView(model: fixture.model)), fixture: fixture, language: language)
            let navigation = fixture.navigation
            navigation.source.controllers = { [] }
            navigation.attach(window)
            defer { navigation.detach(); window.close() }
            let snapshot = GCController.withExtendedGamepad()
            let pad = try XCTUnwrap(snapshot.extendedGamepad)
            navigation.source.stop()
            navigation.source.controllers = { [snapshot] }
            navigation.source.isActive = { true }
            var time: TimeInterval = 0
            func press(_ down: Bool) {
                time += 0.1
                pad.dpad.setValueForXAxis(0, yAxis: down ? -1 : 1)
                navigation.source.tick(now: time)
                pad.dpad.setValueForXAxis(0, yAxis: 0)
                navigation.source.tick(now: time + 0.01)
            }
            navigation.source.tick(now: 0)
            navigation.focus("settings.language")
            let scroll = try XCTUnwrap(scrollViews(in: host).first { $0.hasVerticalScroller })
            for down in [true, false] {
                for _ in 0..<22 {
                    press(down)
                    try await Task.sleep(for: .milliseconds(25))
                    let targets = navigation.controlTargets(in: window)
                    let focused = try XCTUnwrap(targets.first { $0.id == navigation.focusedID })
                    let view = try XCTUnwrap(focused.element as? NSView)
                    if view.enclosingScrollView != nil {
                        let viewport = window.convertToScreen(scroll.contentView.convert(scroll.contentView.bounds, to: nil))
                        XCTAssertTrue(viewport.insetBy(dx: -1, dy: -1).contains(focused.unclippedFrame ?? focused.frame),
                            "\(language.code)：方向导航必须先将整个控件滚入可见区域，\(focused.id)")
                    }
                }
            }
            navigation.focus("settings.tab.iphone"); navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(80))
            navigation.focus("export.sign.mode")
            for step in 0..<14 {
                navigation.handle(.scrollDown)
                try await Task.sleep(for: .milliseconds(25))
                if let focused = navigation.controlTargets(in: window).first(where: { $0.id == navigation.focusedID }) {
                    let view = try XCTUnwrap(focused.element as? NSView)
                    let frame = window.convertToScreen(view.convert(view.bounds, to: nil))
                    XCTAssertTrue(frame.intersects(window.convertToScreen(host.bounds)), "滚动第 \(step) 步不能留下脱离控件的焦点")
                    XCTAssertTrue(navigation.isFullyVisible(focused.id), "手动滚动后也应选中完整可见的项")
                    let ring = try XCTUnwrap(navigation.focusRingFrame(in: window))
                    XCTAssertEqual(ring.minX, focused.frame.minX, accuracy: 1)
                    XCTAssertEqual(ring.minY, focused.frame.minY, accuracy: 1)
                    XCTAssertEqual(ring.width, focused.frame.width, accuracy: 1)
                    XCTAssertEqual(ring.height, focused.frame.height, accuracy: 1)
                } else { XCTFail("滚动后焦点必须对应仍然可见的控件") }
            }
            try self.snapshot(host, "controller-settings-scrolled-" + language.code)
            if let container = host.superview { try self.snapshot(container, "controller-settings-focus-ring-" + language.code) }
        }
    }

    func testExportLocationSurvivesReopeningAndRestartingWithAnUnchangedList() async throws {
        for language in [Lang.zh, .en] {
            let fixture = try SettingsFixture()
            defer { fixture.remove() }
            let first = "/original/storage-default"
            let chosen = "/original/storage-selected"
            fixture.runner.storageResponse = ["locations": [
                ["id": "default", "path": first, "default": true, "connected": true, "freeBytes": 1000000],
                ["id": "selected", "path": chosen, "connected": true, "freeBytes": 2000000]
            ], "last": "default"]
            fixture.model.loadLocations()
            for _ in 0..<100 where fixture.model.locations.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
            XCTAssertEqual(fixture.model.locations.count, 2)
            let game = GameSummary(id: "fixture", name: "Original location fixture",
                location: GameLocation(catalog: ["state": "ok", "path": first]))
            for stage in 0..<4 {
                let model = stage < 2 ? fixture.model : S2ArmLauncherModel(client: fixture.model.provider.client)
                fixture.quiet(model)
                if stage >= 2 {
                    if stage == 3 { fixture.runner.storageResponse = ["locations": [], "last": ""] }
                    model.loadLocations()
                    try await Task.sleep(for: .milliseconds(100))
                }
                let dropdown = LauncherDropdownState()
                let padFocus = PadFocus()
                let page = AnyView(ExportView(model: model, covers: CoverCache(), game: game)
                    .environmentObject(padFocus).padScope("main", padFocus)
                    .environment(\.launcherDropdownHost, dropdown)
                    .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text).preferredColorScheme(.dark)
                    .frame(width: 1120, height: 900))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                fixture.navigation.source.controllers = { [] }
                fixture.navigation.attach(window)
                defer { fixture.navigation.detach(); window.close() }
                fixture.navigation.focus("export.location"); fixture.navigation.handle(.confirm)
                try await Task.sleep(for: .milliseconds(80))
                let request = try XCTUnwrap(dropdown.request)
                let expected = stage == 0 ? first : chosen
                print("export-location \(language.code)/stage-\(stage): \(request.selection)")
                XCTAssertEqual(request.selection, expected, "缓存列表不变、重开页面及重启模型后都应显示已选位置")
                XCTAssertTrue(request.options.contains { $0.value == expected }, "已选但尚未登记的文件夹也应有可见名称")
                if stage == 0 { request.choose(chosen) }
                dropdown.close()
                if stage == 1 { try snapshot(host, "export-location-remembered-" + language.code) }
            }
            XCTAssertFalse(fixture.runner.requests.contains { $0["operation"] as? String == "export" }, "只选位置不能开始出包或搬游戏")
        }
    }

    func testExportActionsLiveInBottomRightAndHaveControllerShortcuts() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for platform in fixture.model.exportTargets where platform != .mac { fixture.model.toggleExport(platform) }
        if !fixture.model.exportTargets.contains(.mac) { fixture.model.toggleExport(.mac) }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120, 1440] {
                let padFocus = PadFocus()
                let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
                    .environmentObject(padFocus).padScope("main", padFocus).frame(width: width, height: 630))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
                fixture.navigation.source.stop(); fixture.navigation.source.isActive = { true }
                defer { fixture.navigation.detach(); window.close() }
                let snapshot = GCController.withExtendedGamepad()
                for connected in [false, true, false] {
                    fixture.navigation.source.controllers = { connected ? [snapshot] : [] }
                    fixture.navigation.source.tick()
                    fixture.navigation.hide()
                    if let scroll = scrollViews(in: host).first, let document = scroll.documentView {
                        let clip = scroll.contentView
                        clip.scroll(to: CGPoint(x: 0, y: document.isFlipped ? 0 : max(0, document.bounds.height - clip.bounds.height)))
                        scroll.reflectScrolledClipView(clip)
                    }
                    try await Task.sleep(for: .milliseconds(70))
                    let targets = fixture.navigation.controlTargets(in: window)
                    let ids = Set(LauncherPadNavigation.elements(window).compactMap { S2ALauncherAX.identifier($0) })
                    XCTAssertFalse(ids.contains("export.back"), "出包页不再显示左上角返回")
                    let start = try XCTUnwrap(targets.first { $0.id == "export.start" })
                    let cancel = try XCTUnwrap(targets.first { $0.id == "export.cancel" })
                    let cover = try XCTUnwrap(targets.first { $0.id == "export.cover" })
                    let bounds = window.convertToScreen(host.bounds)
                    XCTAssertEqual(bounds.maxY - cover.frame.maxY, 22, accuracy: 1, "移除返回按钮后，游戏内容直接从 22 pt 顶部边距开始")
                    XCTAssertTrue(bounds.contains(cancel.frame.union(start.frame)))
                    XCTAssertGreaterThan(cancel.frame.midX, bounds.midX, "取消和开始出包都应位于右下角")
                    XCTAssertLessThanOrEqual(cancel.frame.maxX, start.frame.minX)
                    XCTAssertEqual(cancel.frame.midY, start.frame.midY, accuracy: 1)
                    XCTAssertEqual(cancel.frame.height, start.frame.height, accuracy: 1, "出包页的取消和开始按钮应同高")
                    XCTAssertEqual(cancel.frame.height, 48, accuracy: 1)
                    XCTAssertLessThan(start.frame.maxY, bounds.minY + 90)
                    let shortcuts = fixture.navigation.shortcutTargets(in: window)
                    XCTAssertEqual(shortcuts[.rightTrigger], ["export.start"])
                    XCTAssertEqual(shortcuts[.back], ["export.cancel"])
                    fixture.model.exportFor = game.id
                    fixture.navigation.focus("export.location"); fixture.navigation.handle(.back)
                    XCTAssertNil(fixture.model.exportFor, "B 与右下角取消执行相同的返回操作")
                    XCTAssertFalse(fixture.runner.requests.contains { $0["operation"] as? String == "export" }, "取消不应开始出包")
                    try self.snapshot(host, "export-actions-\(language.code)-\(Int(width))-\(connected ? "connected" : "disconnected")")
                }
            }
        }
    }

    func testSelfSignSessionMatchesTheExportPageLayout() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogCover = "original-export-cover"
        let artwork = NSImage(size: CGSize(width: 100, height: 100), flipped: false) { rect in
            NSColor(deviceRed: 0.9, green: 0.05, blue: 0.9, alpha: 1).setFill()
            rect.fill(); return true
        }
        let covers = CoverCache { _, _ in CoverCache.Loaded(version: "original", image: artwork) }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        fixture.model.exportAfterPack[game.id] = ExportPlan(sign: nil, devices: [])   // 导出确认页要先出包的那一步：完成页照导出写
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120, 1440] {
                fixture.model.session = nil
                let focus = PadFocus()
                let reference = AnyView(ExportView(model: fixture.model, covers: covers, game: game)
                    .environmentObject(focus).padScope("main", focus).background(Color.black)
                    .frame(width: width, height: 630))
                let (referenceHost, referenceWindow) = try await render(reference, fixture: fixture, language: language)
                defer { referenceWindow.close() }
                let referenceBounds = referenceWindow.convertToScreen(referenceHost.bounds)
                let cover = try XCTUnwrap(fixture.navigation.controlTargets(in: referenceWindow).first { $0.id == "export.cover" })
                XCTAssertEqual(cover.frame.minX - referenceBounds.minX, 48, accuracy: 1)
                XCTAssertEqual(referenceBounds.maxY - cover.frame.maxY, 22, accuracy: 1)
                XCTAssertEqual(cover.frame.width, 236, accuracy: 1)
                XCTAssertEqual(cover.frame.height, 236 / 1.8, accuracy: 1)
                let expectedArtwork = try exportArtworkBounds(referenceHost)
                for state in [JobOutcome.State.cancelled, .completed, .failed] {
                    var run = ExportRun(platforms: [.ios])
                    if state == .completed { run.outcomes[.ios] = .init(state: .completed, appBytes: 1024, packBytes: 2048, seconds: 90) }
                    if state == .failed { run.outcomes[.ios] = .init(state: .failed, message: "Original output volume is full", code: "disk_full", suggestion: "Free space and retry") }
                    let session = JobSession(kind: .export, gameID: game.id, name: game.name, phase: .finished(state), export: run)
                    fixture.model.session = session
                    let page = AnyView(ExportSessionView(model: fixture.model, covers: covers, session: session, run: run,
                        install: { _ in }, freeSpace: { _ in }, uiLanguage: language.sharedCode)
                        .background(Color.black).foregroundStyle(LauncherTheme.Colors.text).frame(width: width, height: 630))
                    let (host, window) = try await render(page, fixture: fixture, language: language)
                    defer { window.close() }
                    // Compare painted pixels with the reference page: the rounded border
                    // excludes its outer pixels from the original magenta artwork.
                    let artworkBounds = try exportArtworkBounds(host)
                    print("self-sign-layout \(language.code)/\(width)/\(state): artwork=\(artworkBounds), reference=\(expectedArtwork)")
                    XCTAssertEqual(artworkBounds.minX, expectedArtwork.minX, accuracy: 1)
                    XCTAssertEqual(artworkBounds.minY, expectedArtwork.minY, accuracy: 1)
                    XCTAssertEqual(artworkBounds.width, expectedArtwork.width, accuracy: 1)
                    XCTAssertEqual(artworkBounds.height, expectedArtwork.height, accuracy: 1)
                    let bounds = window.convertToScreen(host.bounds)
                    let targets = fixture.navigation.controlTargets(in: window)
                    let back = try XCTUnwrap(targets.first { $0.id == (state == .completed ? "result.ios.install" : "export.session.back") })
                    print("self-sign-footer: right=\(bounds.maxX - back.frame.maxX), bottom=\(back.frame.minY - bounds.minY), height=\(back.frame.height)")
                    XCTAssertEqual(bounds.maxX - back.frame.maxX, 48, accuracy: 1)
                    XCTAssertEqual(back.frame.minY - bounds.minY, 16, accuracy: 1)
                    XCTAssertEqual(back.frame.height, 48, accuracy: 1)
                    for target in targets { XCTAssertTrue(bounds.insetBy(dx: -1, dy: -1).contains(target.frame), "\(state)/\(width): \(target.id) \(target.frame)") }
                    if state == .completed {
                        let action = try XCTUnwrap(targets.first { $0.id == "result.ios.install" })
                        XCTAssertEqual(action.frame.height, 48, accuracy: 1, "结果页的主动作沿用出包页按钮规格")
                        XCTAssertEqual(action.id, back.id, "单平台结果的主动作应在底栏最右侧")
                        let footer = targets.filter { $0.frame.maxY < bounds.minY + 90 }
                        XCTAssertTrue(footer.contains { $0.id == "export.session.back" })
                        for target in footer { XCTAssertTrue(bounds.contains(target.frame)); XCTAssertEqual(target.frame.height, 48, accuracy: 1) }
                    }
                    try snapshot(host, "self-sign-\(language.code)-\(Int(width))-\(state)")
                }
            }
        }
    }

    func testSelfSignSessionFooterKeepsBackgroundCancelAndReturnActions() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        for phase in [JobSession.Phase.running, .stopping, .finished(.cancelled), .finished(.failed)] {
            let run = ExportRun(platforms: [.ios])
            let session = JobSession(kind: .export, gameID: game.id, name: game.name, phase: phase, export: run)
            fixture.model.session = session
            let page = AnyView(ExportSessionView(model: fixture.model, covers: CoverCache(), session: session, run: run,
                install: { _ in }, freeSpace: { _ in }, uiLanguage: "en").frame(width: 800, height: 450))
            let (host, window) = try await render(page, fixture: fixture, language: .en)
            let navigation = fixture.navigation
            navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
            navigation.source.isActive = { true }
            defer { navigation.detach(); window.close() }
            let bounds = window.convertToScreen(host.bounds)
            let targets = navigation.controlTargets(in: window)
            switch phase {
            case .running, .stopping:
                let background = try XCTUnwrap(targets.first { $0.id == "export.session.background" })
                XCTAssertTrue(bounds.contains(background.frame))
                XCTAssertEqual(background.frame.height, 48, accuracy: 1)
                navigation.focus(background.id); navigation.handle(.confirm)
                XCTAssertTrue(fixture.model.sessionHidden, "回库时导出仍在后台进行")
                XCTAssertEqual(fixture.model.session?.phase, phase)
                fixture.model.sessionHidden = false
                if phase == .stopping { XCTAssertFalse(targets.contains { $0.id == "export.session.cancel" }) }
                else {
                    navigation.handle(.submit)
                    XCTAssertEqual(fixture.model.session?.phase, .stopping, "右扳机仍执行原取消动作")
                }
            case .finished:
                let back = try XCTUnwrap(targets.first { $0.id == "export.session.back" })
                XCTAssertEqual(bounds.maxX - back.frame.maxX, 48, accuracy: 1)
                for action in [LauncherPadAction.confirm, .submit, .back] {
                    fixture.model.session = session
                    navigation.focus(back.id); navigation.handle(action)
                    XCTAssertNil(fixture.model.session, "结束后的回库操作必须清除会话")
                }
            }
            XCTAssertFalse(fixture.runner.requests.contains { $0["operation"] as? String == "export" }, "底栏不能意外开始另一单导出")
        }
    }

    func testExportSubmitShortcutUsesTheExistingExportAction() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for platform in fixture.model.exportTargets where platform != .mac { fixture.model.toggleExport(platform) }
        if !fixture.model.exportTargets.contains(.mac) { fixture.model.toggleExport(.mac) }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        fixture.model.exportFor = game.id
        let padFocus = PadFocus()
        let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
            .environmentObject(padFocus).padScope("main", padFocus).frame(width: 1120, height: 630))
        let (_, window) = try await render(page, fixture: fixture, language: .zh)
        let navigation = fixture.navigation
        navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
        navigation.source.isActive = { true }
        defer { navigation.detach(); window.close() }
        let snapshot = GCController.withExtendedGamepad()
        navigation.source.controllers = { [snapshot] }; navigation.source.tick(now: 0)
        navigation.focus("export.location")
        snapshot.extendedGamepad?.rightTrigger.setValue(1); navigation.source.tick(now: 0.1)
        for _ in 0..<100 where !fixture.runner.requests.contains(where: { $0["operation"] as? String == "export" }) {
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertNil(fixture.model.exportFor, "R2 应与开始出包按钮执行同一操作")
        XCTAssertEqual(fixture.runner.requests.filter { $0["operation"] as? String == "export" }.count, 1)
        navigation.source.tick(now: 0.8)
        XCTAssertEqual(fixture.runner.requests.filter { $0["operation"] as? String == "export" }.count, 1, "长按不能重复开始出包")
    }

    /// #1272：选过方案后出包页看得到选的是哪套、能「更改」（弹同一个确认页，不顺带开出包）；没选过不出这一行（导出 iPhone 时确认页会问）
    func testExportPageShowsDownstreamChoiceWithChange() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for choice in [nil, ""] as [String?] {
            fixture.runner.catalogDownstreamChoice = choice
            fixture.model.refresh()
            for _ in 0..<200 where fixture.model.games.first?.downstream?.choice != choice || fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
            let game = try XCTUnwrap(fixture.model.games.first)
            XCTAssertEqual(game.downstream?.choice, choice)
            let padFocus = PadFocus()
            let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
                .environmentObject(padFocus).padScope("main", padFocus).frame(width: 1120, height: 900))
            let (host, window) = try await render(page, fixture: fixture, language: .zh)
            let navigation = fixture.navigation
            navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
            defer { navigation.detach(); window.close() }
            let has = navigation.controlTargets(in: window).contains { $0.id == "export.downstream.change" }
            XCTAssertEqual(has, choice != nil, "选过才出「更改」（choice \(String(describing: choice))）")
            if choice != nil {
                try snapshot(host, "export-downstream-change-zh")
                let before = fixture.runner.requests.count
                navigation.focus("export.downstream.change"); navigation.handle(.confirm)
                for _ in 0..<100 where fixture.model.downstreamPromptFor == nil { try await Task.sleep(for: .milliseconds(10)) }
                XCTAssertEqual(fixture.model.downstreamPromptFor, game.id, "「更改」弹确认页")
                XCTAssertFalse(fixture.runner.requests.dropFirst(before).contains { $0["operation"] as? String == "export" }, "更改不顺带开出包")
                fixture.model.downstreamPromptFor = nil
            }
        }
    }

    /// 11-16 维护者「出包的时候没办法同时安装其他游戏到 iPhone 上，这个控制纯属多余」：手机作业走独立的手机槽。期望按规则手写：
    ///   A 在出包（主槽）时装 B → 立刻发 install、主槽那单不动；装 A → 排队不发；手机槽忙时再装 B → 排队；主槽 A 结束 → A 的装机接上
    func testInstallRunsWhileAnotherGameExportsAndQueuesRealConflicts() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogGameCount = 2
        fixture.model.refresh()
        for _ in 0..<200 where fixture.model.games.count < 2 { try await Task.sleep(for: .milliseconds(10)) }
        let a = try XCTUnwrap(fixture.model.games.first { $0.id == "fixture" }), b = try XCTUnwrap(fixture.model.games.first { $0.id == "fixture-1" })
        func installs(_ id: String) -> Int { fixture.runner.requests.filter { $0["operation"] as? String == "install" && $0["gameId"] as? String == id }.count }
        var export = JobSession(kind: .export, gameID: a.id, name: a.name); export.export = ExportRun(platforms: [.ios])
        fixture.model.session = export   // A 正在出包（主槽）
        XCTAssertTrue(fixture.model.mainBusy && !fixture.model.phoneBusy)

        fixture.model.install(b, device: "dev-1")
        XCTAssertEqual(fixture.model.phoneSession?.gameID, b.id, "A 出包时装 B：手机槽立刻起")
        XCTAssertEqual(fixture.model.session?.gameID, a.id, "主槽那单不动")
        fixture.model.install(a, device: "dev-1")
        XCTAssertTrue(fixture.model.phoneQueued(a.id), "装正在出包的 A：排队")
        fixture.model.install(b, device: "dev-1")
        XCTAssertEqual(fixture.model.phoneQueue.map(\.gameID), [a.id], "B 正在推时再装 B：不再排第二单（D-YXDW）")
        for _ in 0..<200 where installs(b.id) < 1 || fixture.model.phoneBusy { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(installs(a.id), 0, "A 还在出包：A 的装机不发")
        XCTAssertEqual(installs(b.id), 1, "B 只推一遍")
        XCTAssertFalse(fixture.model.phoneBusy, "手机槽空了：排着的 A 被主槽占着，不起")
        XCTAssertEqual(fixture.model.phoneQueue.map(\.gameID), [a.id])

        fixture.model.finish(.completed, message: "")   // A 出包结束
        for _ in 0..<200 where installs(a.id) < 1 { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(installs(a.id), 1, "A 出完：A 的装机接上")
        XCTAssertTrue(fixture.model.phoneQueue.isEmpty)
    }

    func testExportNameControllerOpensKeyboardWithoutRenameAlert() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let game = GameSummary(id: "fixture", name: "Original editable game title")
        for language in [Lang.zh, .en] {
            let priorRequests = fixture.runner.requests.filter { $0["operation"] as? String == "customize" }.count
            let padFocus = PadFocus()
            let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
                .environmentObject(padFocus).padScope("main", padFocus).frame(width: 1120, height: 630)
                .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text).preferredColorScheme(.dark))
            let (host, window) = try await render(page, fixture: fixture, language: language)
            let navigation = fixture.navigation
            navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
            let loc = Localizer(language)
            navigation.text = { loc.t($0) }
            defer { navigation.detach(); window.close() }
            navigation.focus("export.name"); navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(120))
            let keyboard = try XCTUnwrap(navigation.keyboard, "手柄确认游戏标题应直接打开屏幕键盘")
            XCTAssertEqual(keyboard.value, game.name, "从现有标题开始修改")
            XCTAssertNil(window.attachedSheet, "不再打开修改名字的输入弹窗")
            XCTAssertTrue(UIStage.shared.children(of: window).contains { $0 === keyboard.panel }, "屏幕键盘挂在出包页窗口上")
            try snapshot(host, "export-name-ready-" + language.code)
            try snapshot(try XCTUnwrap(keyboard.panel.contentView), "export-name-keyboard-" + language.code)
            keyboard.value = "Cancelled title"
            navigation.handle(.back)
            try await Task.sleep(for: .milliseconds(80))
            XCTAssertEqual(fixture.runner.requests.filter { $0["operation"] as? String == "customize" }.count, priorRequests, "B 取消不能保存")
            XCTAssertEqual(navigation.focusedID, "export.name")
            navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(100))
            let second = try XCTUnwrap(navigation.keyboard)
            XCTAssertEqual(second.value, game.name)
            second.value = "Original renamed title " + language.code
            navigation.handle(.submit)
            try await Task.sleep(for: .milliseconds(150))
            let requests = fixture.runner.requests.filter { $0["operation"] as? String == "customize" }
            XCTAssertEqual(requests.count, priorRequests + 1, "确认只保存一次")
            XCTAssertEqual(requests.last?["name"] as? String, second.value)
            XCTAssertEqual(requests.last?["gameId"] as? String, game.id)
            XCTAssertNil(navigation.keyboard)
            XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == navigation.focusedID }, "保存时的短暂禁用不能留下失效焦点")
            try snapshot(host, "export-name-controller-" + language.code)
        }
    }

    func testExportCheckboxCanBeReachedAndLeftWithTheDpad() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let game = GameSummary(id: "fixture", name: "Original long game title for controller navigation")
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120, 1440] {
                let padFocus = PadFocus()
                let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
                    .environmentObject(padFocus).padScope("main", padFocus).frame(width: width, height: 810)
                    .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text).preferredColorScheme(.dark))
                let (_, window) = try await render(page, fixture: fixture, language: language)
                let navigation = fixture.navigation
                navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
                defer { navigation.detach(); window.close() }
                let targets = navigation.controlTargets(in: window)
                let checkbox = try XCTUnwrap(targets.first { $0.id == "export.sgrade.toggle" })
                print("export-checkbox \(language.code)/\(width): \(targets.map { "\($0.id)=\($0.frame)" }.joined(separator: ", "))")
                // 平台卡按标识认（iPhone / Apple TV / Mac 三张，Apple TV 卡在出包机配了 Apple TV 时才有），不按高度猜
                let platforms = targets.filter { $0.id.hasPrefix("export.platform.") }.sorted { $0.frame.minX < $1.frame.minX }
                let mac = try XCTUnwrap(platforms.first { $0.id == "export.platform.mac" })
                // 复选框与平台之间夹着 mod 一行（「？」格式说明 + 导入按钮）：方向键逐行走，不跳行
                let modRow = Set(targets.map(\.id).filter { $0 == "export.sgrade.mod.help" || $0 == "export.sgrade.mod.import" })
                navigation.focus(mac.id); navigation.handle(.up)
                if !modRow.isEmpty {
                    XCTAssertTrue(navigation.focusedID.map(modRow.contains) == true, "向上应先进平台上方紧邻的 mod 一行")
                    navigation.handle(.up)
                }
                XCTAssertEqual(navigation.focusedID, checkbox.id, "向上应先经过紧邻的平台上方的复选框，不能跳过整行")
                navigation.focus("export.name"); navigation.handle(.down)
                XCTAssertTrue(["export.sgrade.details", checkbox.id].contains(navigation.focusedID), "标题向下应进入 S 级卡片首行，不能停在同一行的封面")
                navigation.focus(checkbox.id); navigation.handle(.up)
                XCTAssertNotEqual(navigation.focusedID, checkbox.id, "向上应离开复选框回到游戏信息")
                navigation.focus(checkbox.id); navigation.handle(.down)
                if !modRow.isEmpty { XCTAssertTrue(navigation.focusedID.map(modRow.contains) == true, "向下先进 mod 一行"); navigation.handle(.down) }
                XCTAssertTrue(platforms.contains { $0.id == navigation.focusedID }, "向下应离开复选框进入可选平台")
                let before = fixture.model.sgradeOn(game.id)
                navigation.focus(checkbox.id); navigation.handle(.confirm)
                try await waitUntil { fixture.model.sgradeOn(game.id) != before }
                XCTAssertEqual(fixture.model.sgradeOn(game.id), !before)
                XCTAssertEqual(navigation.focusedID, checkbox.id, "切换勾选后仍保持有效焦点")
                navigation.handle(.down)
                if !modRow.isEmpty { XCTAssertTrue(navigation.focusedID.map(modRow.contains) == true); navigation.handle(.down) }
                XCTAssertTrue(platforms.contains { $0.id == navigation.focusedID }, "勾选状态改变后也能移出")
                XCTAssertEqual(fixture.model.sgradeOn(game.id), !before, "方向导航不能修改勾选")
            }
        }
    }

    func testExportNameMouseEditsTheHeadingAndEscapeCancels() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let game = GameSummary(id: "fixture", name: "Original long title to edit directly in the heading")
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120, 1440] {
                fixture.model.exportFor = game.id
                let padFocus = PadFocus()
                let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
                    .environmentObject(padFocus).padScope("main", padFocus).frame(width: width, height: 630)
                    .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text).preferredColorScheme(.dark))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                let navigation = fixture.navigation
                navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
                UIStage.show(window)
                defer { navigation.detach(); window.close() }
                func clickTitle() throws {
                    host.layoutSubtreeIfNeeded()
                    try focusTextField(in: "export.name", navigation, window: window)
                }
                let before = fixture.runner.requests.filter { $0["operation"] as? String == "customize" }.count
                try clickTitle()
                try await waitUntil { window.firstResponder is NSTextView }
                try await Task.sleep(for: .milliseconds(60))
                let editor = try XCTUnwrap(window.firstResponder as? NSTextView)
                XCTAssertEqual(editor.string, game.name, "点击标题应直接开始编辑原有名字")
                editor.selectAll(nil)
                let replacement = "Original edited heading " + language.code
                editor.insertText(replacement, replacementRange: NSRange(location: NSNotFound, length: 0))
                try await Task.sleep(for: .milliseconds(60))
                try key(36, "\r", in: window)
                try await waitUntil {
                    !fixture.model.customizing
                        && fixture.runner.requests.filter { $0["operation"] as? String == "customize" }.count == before + 1
                        && navigation.controlTargets(in: window).contains { $0.id == "export.name" }
                        && !(window.firstResponder is NSTextView)
                }
                let saved = fixture.runner.requests.filter { $0["operation"] as? String == "customize" }
                XCTAssertEqual(saved.count, before + 1, "Return 保存与失焦回调合计只提交一次")
                XCTAssertEqual(saved.last?["name"] as? String, replacement)
                XCTAssertNil(window.attachedSheet)
                XCTAssertNil(navigation.keyboard, "鼠标编辑保持原位输入")
                try snapshot(host, "export-name-inline-\(language.code)-\(Int(width))")
                try clickTitle()
                try await waitUntil { window.firstResponder is NSTextView }
                try await Task.sleep(for: .milliseconds(60))
                let cancelEditor = try XCTUnwrap(window.firstResponder as? NSTextView)
                cancelEditor.selectAll(nil)
                cancelEditor.insertText("Discard this heading", replacementRange: NSRange(location: NSNotFound, length: 0))
                try await Task.sleep(for: .milliseconds(60))
                try key(53, "\u{1b}", in: window)
                try await waitUntil { !(window.firstResponder is NSTextView) }
                XCTAssertEqual(fixture.runner.requests.filter { $0["operation"] as? String == "customize" }.count, before + 1)
                XCTAssertEqual(fixture.model.exportFor, game.id, "Esc 只取消标题编辑，不能退出出包页")
                try clickTitle()
                try await waitUntil { window.firstResponder is NSTextView }
                try await Task.sleep(for: .milliseconds(60))
                XCTAssertEqual((window.firstResponder as? NSTextView)?.string, game.name, "取消后应恢复已有标题")
                try key(53, "\u{1b}", in: window)
            }
        }
    }

    private func key(_ code: UInt16, _ characters: String, in window: NSWindow) throws {
        for type in [NSEvent.EventType.keyDown, .keyUp] {
            let event = try XCTUnwrap(NSEvent.keyEvent(with: type, location: .zero, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber, context: nil,
                characters: characters, charactersIgnoringModifiers: characters, isARepeat: false, keyCode: code))
            window.sendEvent(event)
        }
    }

    func testHomeShouldersMoveTheFocusRingToTheSelectedTab() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let navigation = fixture.model.gamepad
        navigation.source.controllers = { [] }
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120] {
                fixture.model.home.group = .all
                let (_, window) = try await render(AnyView(LauncherView(model: fixture.model, brand: "Original tab fixture")
                    .frame(width: width, height: 630)), fixture: fixture, language: language)
                navigation.attach(window); navigation.source.stop(); navigation.source.isActive = { true }
                defer { navigation.detach(); window.close() }
                let controller = GCController.withExtendedGamepad(), pad = try XCTUnwrap(controller.extendedGamepad)
                navigation.source.controllers = { [controller] }; navigation.source.tick(now: 0)
                navigation.focus("home.group.all")
                XCTAssertTrue(navigation.activeWindow === window, "前提：当前操作的是首页窗口（舞台上还显示着：\(UIStage.shared.shownWindows.map { "\(type(of: $0)) \($0.identifier?.rawValue ?? "")" })）")
                var time = 0.01
                for expected in [HomeGroup.ready, .recent, .favorites, .all] {
                    pad.rightShoulder.setValue(1); navigation.source.tick(now: time)
                    pad.rightShoulder.setValue(0); navigation.source.tick(now: time + 0.01)
                    time += 0.1
                    let id = "home.group." + expected.rawValue
                    try await waitUntil {
                        guard fixture.model.home.group == expected, navigation.focusedID == id,
                              let tab = navigation.controlTargets(in: window).first(where: { $0.id == id }) else { return false }
                        return navigation.focusRingFrame(in: window) == tab.frame
                    }
                    XCTAssertEqual(fixture.model.home.group, expected)
                    XCTAssertEqual(navigation.focusedID, id, "肩键切页后焦点应与新 tab 同步，包括空分组")
                    let tab = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == id })
                    XCTAssertEqual(navigation.focusRingFrame(in: window), tab.frame)
                }
                navigation.focus("card.fixture")
                pad.leftShoulder.setValue(1); navigation.source.tick(now: time)
                pad.leftShoulder.setValue(0); navigation.source.tick(now: time + 0.01)
                try await waitUntil { fixture.model.home.group == .favorites && navigation.focusedID == "home.group.favorites" }
                XCTAssertEqual(fixture.model.home.group, .favorites)
                XCTAssertEqual(navigation.focusedID, "home.group.favorites", "从卡片切页也应聚焦新 tab")
                navigation.source.controllers = { [] }
            }
        }
    }

    /// 首页：左右移动就是选中；按界面映射后的确认键执行主操作（同 R2 / ZR），按住不连发。
    func testHomeConfirmOnFocusedCardRunsThePrimaryActionLikeRightTrigger() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogNeedsPreparation = true
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        let navigation = fixture.model.gamepad
        navigation.source.controllers = { [] }
        let (host, window) = try await render(AnyView(LauncherView(model: fixture.model, brand: "Original R2 fixture")
            .frame(width: 1120, height: 630)), fixture: fixture, language: .zh)
        navigation.attach(window); navigation.source.stop(); navigation.source.isActive = { true }
        defer { navigation.detach(); window.close() }
        // 生产路由优先处理 keyWindow；合成按键前明确把焦点交给被测窗口。
        UIStage.show(window)
        try await waitUntil { navigation.activeWindow === window && navigation.inMainContent }
        let controller = GCController.withExtendedGamepad(), pad = try XCTUnwrap(controller.extendedGamepad)
        let receive = navigation.source.receive
        var actions: [LauncherPadAction] = []
        navigation.source.receive = { actions.append($0); receive($0) }
        navigation.source.controllers = { [controller] }; navigation.source.tick(now: 0)
        navigation.focus("card." + game.id)
        fixture.model.selectedID = game.id
        XCTAssertEqual(navigation.shortcutTargets(in: window)[.rightTrigger], ["hero.export"])
        try snapshot(host, "home-r2-prepare")
        let mapping = navigation.source.faceMapping(ControllerFaceFamily.of(controller))
        let confirm = [pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY][mapping.source(for: 0)]
        confirm.setValue(1); navigation.source.tick(now: 0.01)
        XCTAssertEqual(actions, [.confirm], "合成输入须按当前四键映射产出确认，不能假定本机使用默认排列")
        XCTAssertEqual(fixture.model.exportFor, game.id, "卡片确认执行主操作（准备游戏），同 R2")
        try await Task.sleep(for: .milliseconds(150))
        navigation.source.tick(now: 0.8)
        XCTAssertFalse(fixture.runner.requests.contains { $0["operation"] as? String == "export" },
            "按住确认键从首页进入出包页，不能再触发新页面的开始出包")
    }

    /// v0.5.0 手柄验收 A1：首页横向封面，手柄左右切游戏，选中的游戏（大图与标题）跟着换。注入 GCController 十字键，走生产输入路径
    func testAcceptanceHomeDpadSwitchesTheSelectedGame() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogGameCount = 3
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.count < 3 { try await Task.sleep(for: .milliseconds(10)) }
        let navigation = fixture.model.gamepad
        navigation.source.controllers = { [] }
        let (host, window) = try await render(AnyView(LauncherView(model: fixture.model, brand: "Original UI fixture")
            .frame(width: 1280, height: 760)), fixture: fixture, language: .zh)
        navigation.attach(window); navigation.source.stop(); navigation.source.isActive = { true }
        defer { navigation.detach(); window.close() }
        let controller = GCController.withExtendedGamepad(), pad = try XCTUnwrap(controller.extendedGamepad)
        navigation.source.controllers = { [controller] }; navigation.source.tick(now: 0)
        fixture.model.selectedID = "fixture"; navigation.focus("card.fixture")
        try await Task.sleep(for: .milliseconds(120))
        try snapshot(host, "acceptance-A1-0")
        var now = 0.01
        for expected in ["fixture-1", "fixture-2"] {
            pad.dpad.setValueForXAxis(1, yAxis: 0); navigation.source.tick(now: now); now += 0.01
            pad.dpad.setValueForXAxis(0, yAxis: 0); navigation.source.tick(now: now); now += 0.01
            try await Task.sleep(for: .milliseconds(200))
            XCTAssertEqual(fixture.model.selectedID, expected, "十字键右一下切到下一款")
            XCTAssertEqual(navigation.focusedID, "card." + expected)
            try snapshot(host, "acceptance-A1-" + expected)
        }
        pad.dpad.setValueForXAxis(-1, yAxis: 0); navigation.source.tick(now: now); now += 0.01
        pad.dpad.setValueForXAxis(0, yAxis: 0); navigation.source.tick(now: now)
        try await Task.sleep(for: .milliseconds(200))
        XCTAssertEqual(fixture.model.selectedID, "fixture-1", "十字键左一下切回上一款")
    }

    /// v0.5.0 手柄验收 A6：玩过的游戏首页显示游玩时长（单独一块）；没玩过的不显示
    func testAcceptanceHomeShowsPlaytimeOnlyForPlayedGames() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogGameCount = 2
        fixture.runner.catalogPlaytime = ["seconds": 9059, "lastPlayed": Date().timeIntervalSince1970 - 86_400]
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.count < 2 { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(fixture.model.games.first { $0.id == "fixture" }?.playtime?.seconds, 9059)
        XCTAssertNil(fixture.model.games.first { $0.id == "fixture-1" }?.playtime)
        fixture.model.gamepad.source.controllers = { [] }
        for (id, played) in [("fixture", true), ("fixture-1", false)] {
            fixture.model.selectedID = id
            let (host, window) = try await render(AnyView(LauncherView(model: fixture.model, brand: "Original UI fixture")
                .frame(width: 1280, height: 760)), fixture: fixture, language: .zh)
            defer { window.close() }
            XCTAssertEqual(fixture.model.games.first { $0.id == id }?.playtime != nil, played, played ? "玩过的带游玩时长（首页那块按它显示，截图为证）" : "没玩过的不带")
            try snapshot(host, "acceptance-A6-" + id)
        }
    }

    func testWelcomeDoesNotUseTheLibrarySelectionAsANewImport() async throws {
        for language in [Lang.zh, .en] {
            let fixture = try SettingsFixture()
            defer { fixture.remove() }
            fixture.model.refresh()
            fixture.model.checkEnvironment()
            try await waitUntil { !fixture.model.games.isEmpty && !fixture.model.checking }
            fixture.model.welcomeHome = true
            XCTAssertNotNil(fixture.model.selected)
            XCTAssertNil(fixture.model.welcomeGame)
            XCTAssertEqual(fixture.model.keysReady, false)
            let (host, window) = try await render(AnyView(WelcomeView(model: fixture.model).frame(width: 1440, height: 900).background(Color.black)), fixture: fixture, language: language)
            fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window); fixture.navigation.source.stop()
            defer { fixture.navigation.detach(); window.close() }
            let texts = normalizedText(try renderedText(host, language: language).map(\.text).joined(separator: "\n"))
            XCTAssertTrue(texts.contains(normalizedText(BackendText.launcherText("welcome.step.mac.done", language: language.sharedCode))), "必须实际读到欢迎页文案，避免空图让否定断言空过")
            XCTAssertFalse(texts.contains(normalizedText(BackendText.launcherText("welcome.step.keys.notNeeded", language: language.sharedCode))), "库里选中的游戏不能让本次引导跳过密钥")
            XCTAssertFalse(texts.contains(normalizedText("Original UI fixture 0")), "本次还没选游戏，不应显示库里选中的那款")
            XCTAssertFalse(fixture.navigation.shortcutTargets(in: window)[.rightTrigger, default: []].contains("welcome.library"), "进入游戏库不能占用提交键")
            XCTAssertTrue(fixture.navigation.controlTargets(in: window).contains { $0.id == "welcome.library" }, "已有游戏时，回库入口不应依赖本次引导进度")
            try snapshot(host, "welcome-existing-library-" + language.code)
            fixture.navigation.focus("welcome.library"); fixture.navigation.handle(.confirm)
            XCTAssertFalse(fixture.model.showsWelcome)
        }
    }

    func testWelcomeImportKeepsExportAsTheDefaultAction() async throws {
        for language in [Lang.zh, .en] {
            for keyboard in [false, true] {
                let fixture = try SettingsFixture()
                defer { fixture.remove() }
                fixture.model.checkEnvironment()
                try await waitUntil { !fixture.model.checking }
                fixture.model.importGames([fixture.root.appendingPathComponent("original-game.nup")], partialResults: false)
                try await waitUntil { !fixture.model.busy && fixture.model.welcomeGame != nil }
                let (host, window) = try await render(AnyView(WelcomeView(model: fixture.model).frame(width: 1440, height: 900).background(Color.black)), fixture: fixture, language: language)
                fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window); fixture.navigation.source.stop()
                defer { fixture.navigation.detach(); window.close() }
                let targets = fixture.navigation.controlTargets(in: window)
                let export = try XCTUnwrap(targets.first { $0.id == "welcome.oneclick" })
                let library = try XCTUnwrap(targets.first { $0.id == "welcome.library" })
                XCTAssertGreaterThan(export.frame.minY, library.frame.maxY, "回库次按钮放在一键出包下面")
                XCTAssertEqual(fixture.navigation.shortcutTargets(in: window)[.rightTrigger], ["welcome.oneclick"])
                try snapshot(host, "welcome-import-" + language.code + (keyboard ? "-keyboard" : "-controller"))
                if keyboard {
                    let event = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                        windowNumber: window.windowNumber, context: nil, characters: "\r", charactersIgnoringModifiers: "\r", isARepeat: false, keyCode: 36))
                    XCTAssertTrue(window.performKeyEquivalent(with: event))
                } else {
                    fixture.navigation.handle(.submit)
                }
                XCTAssertEqual(fixture.model.exportFor, "fixture", "Return 和 R2 都应打开本次导入游戏的出包页")
            }
        }
    }

    func testWelcomeToolNamesShareTheSameColumn() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.preflightChecks = [["id": "xcode", "name": "Xcode", "ok": false, "action": "appStoreXcode", "requiredFor": ["prepare", "ipa"]]]
        fixture.model.checkEnvironment()
        try await waitUntil { !fixture.model.checking }
        do {
            let language = Lang.zh
            let (host, window) = try await render(AnyView(WelcomeView(model: fixture.model).frame(width: 1440, height: 1100).background(Color.black)), fixture: fixture, language: language)
            defer { window.close() }
            let texts = try renderedText(host, language: language)
            try snapshot(host, "welcome-tools-" + language.code)
            let names = try ["Xcode", "Metal", "iOS", "Apple ID"].map { title in
                let item = try XCTUnwrap(texts.first { $0.text.localizedCaseInsensitiveContains(title) }, title)
                let range = try XCTUnwrap(item.text.range(of: title, options: .caseInsensitive))
                let bounds = try XCTUnwrap(item.candidate.boundingBox(for: range)).boundingBox
                return CGRect(x: bounds.minX * host.bounds.width, y: bounds.minY * host.bounds.height,
                              width: bounds.width * host.bounds.width, height: bounds.height * host.bounds.height)
            }
            for frame in names {
                XCTAssertGreaterThan(frame.width, 0)
                // OCR 测的是不同首字母的字形边界，容许 3 pt 字面差异；原 iOS 行缺列会偏移 36 pt。
                XCTAssertEqual(frame.minX, names[0].minX, accuracy: 3, "包括 iOS 组件在内的四行工具名应左对齐（实际字形边界）")
            }
            print("welcome-tool-columns \(language.code): \(names.map(\.minX))")
        }
    }

    func testReportRemainsAvailableOnWelcomeExportAndJobPages() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        try await waitUntil { !fixture.model.games.isEmpty }
        fixture.model.gamepad.source.controllers = { [] }
        let (host, window) = try await render(AnyView(LauncherView(model: fixture.model, brand: "Original UI fixture").frame(width: 1440, height: 900)), fixture: fixture, language: .zh)
        defer { window.close() }
        for page in ["welcome", "export", "job"] {
            fixture.model.goToLibrary()
            switch page {
            case "welcome": fixture.model.welcomeHome = true
            case "export": fixture.model.exportFor = "fixture"
            default: fixture.model.playSession = JobSession(kind: .play, gameID: "fixture", name: "Original session")
            }
            try await settle(window, fixture.model.gamepad)
            XCTAssertTrue(fixture.model.gamepad.controlTargets(in: window).contains { $0.id == "top.report" }, "\(page) 页面应直接看到全局故障报修")
            if page != "welcome" {
                fixture.model.gamepad.handle(.back)
                try await settle(window, fixture.model.gamepad)
                XCTAssertTrue(fixture.model.gamepad.controlTargets(in: window).contains { $0.id == "home.more" }, "\(page) 返回应直接到游戏库")
            }
        }
        try snapshot(window.contentView?.superview ?? host, "review-global-titlebar")
    }

    func testTitlebarActionsKeepTheConfirmedCompactRightInset() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        fixture.model.gamepad.source.controllers = { [] }
        for language in Lang.all {   // 六种界面语言都要（W1：英 / 日 / 西 / 法默认宽度下顶栏按钮压住品牌名）
            for width: CGFloat in [800, 1120, 1440] {
                let page = AnyView(LauncherView(model: fixture.model, brand: "Original UI fixture").frame(width: width, height: 630))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                // The native titlebar has its own hosting view; it registers its controls on a later main-loop pass.
                try await waitUntil {
                    let ids = Set(fixture.model.gamepad.controlTargets(in: window).map(\.id))
                    return ids.contains("top.report") && ids.contains("home.more")
                }
                let targets = fixture.model.gamepad.controlTargets(in: window)
                let report = try XCTUnwrap(targets.first { $0.id == "top.report" })
                let more = try XCTUnwrap(targets.first { $0.id == "home.more" })
                let bounds = window.convertToScreen(host.bounds)
                print("titlebar-right-edge \(language.code)/\(width): report=\(bounds.maxX - report.frame.maxX), home=\(bounds.maxX - more.frame.maxX)")
                XCTAssertEqual(bounds.maxX - report.frame.maxX, 6, accuracy: 0.5, "保留已确认的标题栏右侧 6 pt 留白")
                XCTAssertEqual(bounds.maxX - more.frame.maxX, 48, accuracy: 0.5, "首页控制栏继续使用原边距")
                XCTAssertFalse(targets.contains { $0.id == "home.refresh" }, "刷新保留在更多菜单中")
                try snapshot(host, "titlebar-alignment-\(language.code)-\(Int(width))")
            }
        }
    }

    func testInlineTitlebarKeepsLongBrandAwayFromNativeActions() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        fixture.model.gamepad.source.controllers = { [] }
        for language in Lang.all {   // 六种界面语言都要（W1：英 / 日 / 西 / 法默认宽度下顶栏按钮压住品牌名）
            for width: CGFloat in [800, 1120, 1440] {
                let page = AnyView(LauncherView(model: fixture.model,
                    brand: "Original launcher title with a deliberately long name").frame(width: width, height: 630))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                for backgroundTask in [false, true, false] {
                    fixture.model.session = backgroundTask ? JobSession(kind: .play, gameID: "fixture", name: "Original task") : nil
                    fixture.model.sessionHidden = backgroundTask
                    // The native titlebar has its own hosting view. Its layout
                    // and the page's inset update on separate main-loop passes.
                    for _ in 0..<100 {
                        let ready = fixture.model.gamepad.controlTargets(in: window)
                        if let brand = ready.first(where: { $0.id == "top.brand" }),
                           let action = ready.filter({ $0.id.hasPrefix("top.") && $0.id != "top.brand" }).min(by: { $0.frame.minX < $1.frame.minX }),
                           ready.contains(where: { $0.id == "top.currentTask" }) == backgroundTask,
                           brand.frame.maxX + 12 <= action.frame.minX { break }
                        try await Task.sleep(for: .milliseconds(20))
                    }
                    let targets = fixture.model.gamepad.controlTargets(in: window)
                    let brand = try XCTUnwrap(targets.first { $0.id == "top.brand" })
                    let action = try XCTUnwrap(targets.filter { $0.id.hasPrefix("top.") && $0.id != "top.brand" }.min { $0.frame.minX < $1.frame.minX })
                    let bounds = window.convertToScreen(host.bounds)
                    print("titlebar-heading \(language.code)/\(width)/\(backgroundTask): gap=\(action.frame.minX - brand.frame.maxX), fitting=\(host.fittingSize.width)")
                    XCTAssertLessThanOrEqual(host.fittingSize.width, width + 1)
                    XCTAssertGreaterThanOrEqual(brand.frame.minX - bounds.minX, 89)
                    XCTAssertLessThanOrEqual(brand.frame.maxX + 12, action.frame.minX, "长品牌名不能覆盖原生标题栏操作")
                    XCTAssertGreaterThan(brand.frame.width, 2)
                    try snapshot(host, "titlebar-long-\(language.code)-\(Int(width))-\(backgroundTask)")
                }
            }
        }
    }

    func testHomeSearchDismissesOutsideClicksAndFocusLossWithoutClearingTheQuery() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let state = SearchControlState()
        let (host, window) = try await render(AnyView(SearchControlPreview(state: state)), fixture: fixture, language: .zh)
        defer { window.close() }
        UIStage.show(window)
        let navigation = fixture.navigation
        navigation.source.controllers = { [] }
        navigation.attach(window)
        defer { navigation.detach() }
        navigation.focus("home.search"); navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertTrue(state.expanded)
        XCTAssertTrue(window.firstResponder is NSTextView, "展开后应聚焦原生输入框")
        state.query = "Original"
        try await Task.sleep(for: .milliseconds(80))
        try press("home.search.clear", navigation)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertTrue(state.expanded, "点击胶囊内的清空按钮不能收起")
        XCTAssertEqual(state.query, "")
        state.query = "Original"
        let outside = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == "search-test.outside" })
        let outsideFrame = window.convertFromScreen(outside.frame)
        // 收起靠的是本地鼠标监听（HomeSearchControl.Host）：原样的鼠标按下经 NSApp 分派才会经过它。「不吞点击」= 它把事件原样交下去：
        //   排在它后面装的监听能收到同一个事件（不上屏的窗口里 SwiftUI 按钮本身不接合成鼠标事件，外部按钮的计数不能当证据）
        var passedOn = 0
        let after = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown]) { e in if e.window === window { passedOn += 1 }; return e }
        defer { if let after { NSEvent.removeMonitor(after) } }
        try rawClick(CGPoint(x: outsideFrame.midX, y: outsideFrame.midY), in: window)
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertFalse(state.expanded, "点击搜索框外应自动收起")
        XCTAssertEqual(passedOn, 1, "收起搜索不能吞掉外部按钮的点击（鼠标按下要原样交给后面）")
        XCTAssertEqual(state.query, "Original", "自动收起应保留查询")
        state.expanded = true
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertTrue(window.makeFirstResponder(nil))
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertFalse(state.expanded, "键盘焦点移出搜索框后应自动收起")
        XCTAssertEqual(state.query, "Original")
        state.expanded = true
        try await Task.sleep(for: .milliseconds(350))
        try rawClick(CGPoint(x: 20, y: 20), in: window)
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertFalse(state.expanded, "点击非焦点控件的空白处也应收起")
        try snapshot(host, "home-search-dismissed")
    }

    func testCollapsedSearchIconDoesNotChangeWhenTheQueryIsNonempty() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let state = SearchControlState()
        let (host, window) = try await render(AnyView(SearchControlPreview(state: state)), fixture: fixture, language: .zh)
        defer { window.close() }
        let target = try XCTUnwrap(fixture.navigation.controlTargets(in: window).first { $0.id == "home.search" })
        let rect = host.convert(window.convertFromScreen(target.frame), from: nil)
        func pixels() throws -> Data {
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: rect))
            host.cacheDisplay(in: rect, to: bitmap)
            return try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
        }
        let empty = try pixels()
        state.query = "Original"
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertEqual(try pixels(), empty, "收起时查询非空不能在放大镜上添加圆点")
        try snapshot(host, "home-search-query-without-badge")
    }

    /// 鼠标点控件的等价操作：不上屏的窗口里 SwiftUI 控件不接合成鼠标事件（11-26 探针：发给窗口 / 发给 App 都是 0 次），SwiftUI 按钮也不在无障碍树里；
    ///   经登记表执行它的指针动作（LauncherPadNavigation.pointerActivate：按钮自己的动作，鼠标与手柄动作不同的控件另登记了 pointer）
    private func press(_ id: String, _ navigation: LauncherPadNavigation, file: StaticString = #filePath, line: UInt = #line) throws {
        XCTAssertTrue(navigation.pointerActivate(id), "当前窗口里没有可用的控件 \(id)", file: file, line: line)
    }

    /// 鼠标点文本框的等价操作：让它成为第一响应者（AppKit 鼠标按下文本框做的就是这一步；不上屏也成立，11-26 探针）。
    ///   SwiftUI 的无障碍标识不落到底层 NSTextField 上，按登记表里控件 control 的位置找落在它里面的可编辑文本框
    private func focusTextField(in control: String, _ navigation: LauncherPadNavigation, window: NSWindow, file: StaticString = #filePath, line: UInt = #line) throws {
        let target = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == control }, "窗口里没有控件 \(control)", file: file, line: line)
        func find(_ v: NSView) -> NSTextField? {
            if let t = v as? NSTextField, t.isEditable, window.convertToScreen(t.convert(t.bounds, to: nil)).intersects(target.frame) { return t }
            for c in v.subviews { if let r = find(c) { return r } }
            return nil
        }
        let field = try XCTUnwrap(window.contentView.flatMap(find), "\(control) 里没有可编辑的文本框", file: file, line: line)
        XCTAssertTrue(window.makeFirstResponder(field), "\(control) 的文本框拿不到输入焦点", file: file, line: line)
    }

    /// 原样的鼠标按下 / 抬起经 NSApp 分派：只用来触发本地鼠标监听（点外面收起这类）；不上屏的窗口里 SwiftUI 控件本身不接它
    private func rawClick(_ point: CGPoint, in window: NSWindow) throws {
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber,
                context: nil, eventNumber: 0, clickCount: 1, pressure: 1))
            NSApp.sendEvent(event)
        }
    }

    /// 屏幕键盘已出现且面板上的按键已登记（可以按键 / 提交）
    private func keyboardReady(_ navigation: LauncherPadNavigation) -> Bool {
        guard let k = navigation.keyboard else { return false }
        return navigation.activeWindow === k.panel && navigation.controlTargets(in: k.panel).contains { $0.id == "pad.key.q" }
    }

    /// 按条件等：满足立刻返回，上限 10 秒。原先 2 秒，门禁机整套并发跑、负载高时搜索胶囊展开 + 拿焦点会超时偶发红
    ///   （11-18 合入后步红在首页搜索用例，单跑必过），条件判定不变，快的时候不多等
    private func waitUntil(_ ready: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<1500 {   // 等界面状态到位，30 秒兜底（11-19：原 10 秒，负载上百时渲染慢于此 → 假红）
            if ready() { return }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertTrue(ready(), "界面未在 30 秒内完成状态切换", file: file, line: line)
    }

    func testHomeSearchExpandsInsideTheHomeWindowAndKeepsItsQuery() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogGameCount = 21
        fixture.model.refresh()
        // 游戏库读出来才是首页（有搜索按钮）；原先只等 1 秒，门禁机负载高时还停在欢迎页、找不到 home.search（门禁 06:36 第 1254 行）
        try await waitUntil { !fixture.model.games.isEmpty }
        let navigation = fixture.model.gamepad
        navigation.source.controllers = { [] }
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1000, 1120, 1440] {
                fixture.model.home.search = ""
                let page = AnyView(LauncherView(model: fixture.model, brand: "Original UI fixture").frame(width: width, height: 630))
                // 窗口不上屏（UIStage）：不会成为焦点窗口，也就不会因失焦按设计收起搜索；结果不随桌面焦点 / 前台程序变
                let (host, window) = try await render(page, fixture: fixture, language: language)
                navigation.attach(window)
                navigation.source.stop()
                UIStage.show(window)
                defer { navigation.detach(); window.close() }
                func settleSearch() async throws {
                    var previous = CGRect.null
                    var stable = 0
                    try await waitUntil {
                        guard let frame = navigation.controlTargets(in: window).first(where: { $0.id == "home.search" })?.frame else { return false }
                        stable = frame == previous ? stable + 1 : 0
                        previous = frame
                        return stable >= 8
                    }
                    if !navigation.controlTargets(in: window).contains(where: { $0.id == "home.search" }) {
                        XCTFail("""
                            \(language)/\(width): 没有 home.search；游戏 \(fixture.model.games.count) 款
                            可用控件：\(navigation.controlTargets(in: window).map(\.id))
                            登记：
                            \(navigation.registrationReport(in: window))
                            """)
                    }
                }
                // 点搜索按钮（鼠标那条：Button 自己的动作，原位展开；手柄确认走的是屏幕键盘，另有用例）
                func toggleSearch() async throws {
                    try await settleSearch()
                    let expanded = { navigation.controlTargets(in: window).contains { $0.id == "home.search.field" } }
                    let before = expanded()
                    try press("home.search", navigation)
                    try await waitUntil { expanded() != before }
                }
                let initialWindows = Set(UIStage.shared.shownWindows.map(\.windowNumber))
                XCTAssertFalse(navigation.controlTargets(in: window).contains { $0.id == "home.filter" }, "首页不再显示重复的筛选按钮")
                try await toggleSearch()
                // Let the 240 ms capsule transition finish before the next mouse hit test.
                try await Task.sleep(for: .milliseconds(300))
                try await waitUntil {
                    navigation.controlTargets(in: window).contains { $0.id == "home.search.field" }
                        && window.firstResponder is NSTextView
                }
                let field = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == "home.search.field" }, "搜索框应直接在首页展开")
                XCTAssertEqual(Set(UIStage.shared.shownWindows.map(\.windowNumber)), initialWindows, "展开搜索不能创建 popup")
                let editor = try XCTUnwrap(window.firstResponder as? NSTextView, "鼠标展开后必须直接接受文本输入")
                editor.insertText("Original", replacementRange: NSRange(location: NSNotFound, length: 0))
                try await Task.sleep(for: .milliseconds(80))
                XCTAssertEqual(fixture.model.home.search, "Original", "输入必须写回真实首页查询")
                XCTAssertTrue(window.convertToScreen(host.bounds).contains(field.frame))
                for id in ["home.search", "home.more", "library.sgradeUpgrade", "home.group.all", "home.group.ready", "home.group.recent", "home.group.favorites"] {
                    let control = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == id })
                    XCTAssertTrue(window.convertToScreen(host.bounds).contains(control.frame), "\(language)/\(width): 展开后控件必须留在窗口内")
                    XCTAssertLessThanOrEqual(control.frame.height, 44, "\(language)/\(width): 首页控件不能被换行撑高")
                }
                fixture.model.home.search = "Original"
                try await Task.sleep(for: .milliseconds(80))
                try snapshot(host, "home-search-expanded-\(language.code)-\(Int(width))")
                try await toggleSearch()
                try await Task.sleep(for: .milliseconds(300))
                try await waitUntil { !navigation.controlTargets(in: window).contains { $0.id == "home.search.field" } }
                XCTAssertFalse(navigation.controlTargets(in: window).contains { $0.id == "home.search.field" })
                XCTAssertEqual(fixture.model.home.search, "Original", "收起搜索保留筛选条件")
                try await toggleSearch()
                try await Task.sleep(for: .milliseconds(300))
                try await waitUntil {
                    navigation.controlTargets(in: window).contains { $0.id == "home.search.clear" }
                        && window.firstResponder is NSTextView
                }
                try await settleSearch()
                navigation.focus("home.search.clear"); navigation.handle(.confirm)
                try await Task.sleep(for: .milliseconds(80))
                XCTAssertEqual(fixture.model.home.search, "")
                XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "home.search.field" }, "清空后保持展开")
                let groups = try ["all", "ready", "recent", "favorites"].map { id in
                    try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == "home.group." + id })
                }
                for pair in zip(groups, groups.dropFirst()) {
                    XCTAssertLessThanOrEqual(pair.0.frame.maxX, pair.1.frame.minX, "分组应按全部、已就绪、最近、收藏顺序排列")
                }
                navigation.focus("home.group.ready"); navigation.handle(.confirm)
                XCTAssertEqual(fixture.model.home.group.rawValue, "ready")
                fixture.model.home.group = .all
                // 「…」菜单（SwiftUI popover）的项目与动作由 testHomeMoreMenuOffersShellIpaExportWithoutWindow 不开窗核对；这里不再真弹它：
                //   不上屏的窗口弹不出 SwiftUI popover，原先等菜单落空后对着不存在的 home.refresh 按 A，焦点退回第一张卡片、点开了游戏，
                //   下一轮渲染出的是游戏页、「没有 home.search」（门禁偶发红的同一条链，11-26）
                XCTAssertFalse(navigation.controlTargets(in: window).contains { $0.id == "top.library" })
            }
        }
    }

    /// 游戏库「…」菜单有「导出 S2Arm.ipa」（11-21 维护者：不连手机、不经总控台也能拿到本版 S2Arm.ipa），点了发的是不连手机的导出作业。
    ///   不开窗：核菜单的项目表、执行这一项的动作看作业请求；离屏 NSHostingView 出图核画得出来（S2A_SETTINGS_SHOTS 给目录就存 PNG）
    func testHomeMoreMenuOffersShellIpaExportWithoutWindow() async throws {
        _ = NSApplication.shared
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let menu = HomeMoreMenu(model: fixture.model, loc: Localizer(language), dismiss: {})
            let item = try XCTUnwrap(menu.items.first { $0.id == "home.exportShellIpa" }, "更多菜单里有「导出 S2Arm.ipa」")
            XCTAssertEqual(item.title, language == .zh ? "导出 S2Arm.ipa" : "Export S2Arm.ipa")
            XCTAssertFalse(item.disabled, "不连手机也能点")
        }
        let menu = HomeMoreMenu(model: fixture.model, loc: Localizer(.zh), dismiss: {})
        try XCTUnwrap(menu.items.first { $0.id == "home.exportShellIpa" }).action()
        for _ in 0..<200 where !fixture.runner.requests.contains(where: { $0["operation"] as? String == "installUpdate" }) { try await Task.sleep(for: .milliseconds(10)) }
        let request = try XCTUnwrap(fixture.runner.requests.first { $0["operation"] as? String == "installUpdate" }, "点了发出更新 / 导出作业")
        XCTAssertEqual(request["mode"] as? String, "export", "走导出：只出不带游戏的通用 S2Arm.ipa（export_shell_ipa）")
        XCTAssertNil(request["device"], "不指定手机：不依赖手机登记")
        let host = NSHostingView(rootView: menu.environment(\.padControls, fixture.navigation))
        host.frame = CGRect(x: 0, y: 0, width: 240, height: 120)
        host.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        if let output = ProcessInfo.processInfo.environment["S2A_SETTINGS_SHOTS"] {
            try bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: output).appendingPathComponent("home-more-menu.png"))
        }
        XCTAssertGreaterThan(bitmap.pixelsWide, 0)
    }

    func testHomeSearchControllerOpensKeyboardAndRestoresTextFocus() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let navigation = fixture.model.gamepad
        navigation.source.controllers = { [] }
        for language in [Lang.zh, .en] {
            navigation.source.controllers = { [] }
            let (host, window) = try await render(AnyView(LauncherView(model: fixture.model, brand: "Original search fixture")
                .frame(width: 1120, height: 630)), fixture: fixture, language: language)
            UIStage.show(window)
            navigation.attach(window); navigation.source.stop()
            defer { navigation.detach(); window.close() }
            let controller = GCController.withExtendedGamepad(), pad = try XCTUnwrap(controller.extendedGamepad)
            navigation.source.controllers = { [controller] }; navigation.source.isActive = { true }
            navigation.source.faceMapping = { _ in .identity }
            navigation.source.assignment = { PlayerAssignment() }
            navigation.source.tick(now: 0)
            navigation.focus("home.search")
            pad.buttonA.setValue(1); navigation.source.tick(now: 0.01)
            pad.buttonA.setValue(0); navigation.source.tick(now: 0.02)
            // 键盘出来还不够：要等键盘面板上的按键已登记（面板布局完），否则按键 / 提交落在半成品面板上，关键盘时焦点回不到输入框
            try await waitUntil { keyboardReady(navigation) && navigation.controlTargets(in: window).contains { $0.id == "home.search.field" } }
            let keyboard = try XCTUnwrap(navigation.keyboard, "手柄确认展开搜索时应自动弹出屏幕键盘")
            XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "home.search.field" }, "键盘不能使搜索框失焦收起")
            XCTAssertTrue(navigation.activeWindow === keyboard.panel)
            keyboard.value = "Original"
            navigation.focus("pad.key.q"); navigation.handle(.confirm)
            XCTAssertEqual(keyboard.value, "Originalq")
            try snapshot(try XCTUnwrap(keyboard.panel.contentView), "search-keyboard-" + language.code)
            navigation.handle(.submit)
            try await waitUntil { navigation.keyboard == nil && navigation.focusedID == "home.search.field" && window.firstResponder is NSTextView }
            XCTAssertNil(navigation.keyboard)
            XCTAssertEqual(fixture.model.home.search, "Originalq")
            XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "home.search.field" })
            XCTAssertEqual(navigation.focusedID, "home.search.field", "关闭键盘后，手柄焦点应回到输入框")
            XCTAssertTrue(window.firstResponder is NSTextView, "关闭键盘后恢复原生文本焦点")
            navigation.handle(.confirm)
            try await waitUntil { keyboardReady(navigation) }
            let cancelled = try XCTUnwrap(navigation.keyboard)
            cancelled.value = "discarded"
            navigation.handle(.back)
            try await waitUntil { navigation.keyboard == nil && window.firstResponder is NSTextView }
            XCTAssertEqual(fixture.model.home.search, "Originalq", "B 取消不改变查询")
            XCTAssertTrue(window.firstResponder is NSTextView)
            navigation.handle(.back)
            try await waitUntil { !navigation.controlTargets(in: window).contains { $0.id == "home.search.field" } }
            XCTAssertFalse(navigation.controlTargets(in: window).contains { $0.id == "home.search.field" })
            navigation.handle(.search)
            try await waitUntil { keyboardReady(navigation) }
            let shortcutKeyboard = try XCTUnwrap(navigation.keyboard, "搜索快捷键与搜索按钮共用屏幕键盘入口")
            let key = try XCTUnwrap(navigation.controlTargets(in: shortcutKeyboard.panel).first { $0.id == "pad.key.q" })
            let rect = shortcutKeyboard.panel.convertFromScreen(key.frame)
            let beforeClick = shortcutKeyboard.value
            try rawClick(CGPoint(x: rect.midX, y: rect.midY), in: shortcutKeyboard.panel)   // 经过搜索框的点外面监听：键盘面板上的点击不能让它收起
            try press("pad.key.q", navigation)
            XCTAssertEqual(shortcutKeyboard.value, beforeClick + "q", "点击屏幕键盘不能使下方搜索收起")
            XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "home.search.field" })
            navigation.handle(.back)
            try await Task.sleep(for: .milliseconds(150))
            XCTAssertEqual(fixture.model.home.search, "Originalq")
            try snapshot(host, "search-controller-return-" + language.code)
        }
    }

    func testOnScreenKeyboardKeepsCompleteActionLabelsWithAndWithoutControllerHints() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let (_, window) = try await render(AnyView(Color.black.frame(width: 1120, height: 630)), fixture: fixture, language: .en)
        let navigation = fixture.navigation
        navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
        defer { navigation.detach(); window.close() }
        for language in [Lang.zh, .en] {
            let loc = Localizer(language)
            navigation.text = { loc.t($0) }
            navigation.editText(value: "Original", label: "Name") { _ in }
            let keyboard = try XCTUnwrap(navigation.keyboard)
            let controller = GCController.withExtendedGamepad()
            let pad = try XCTUnwrap(controller.extendedGamepad)
            navigation.source.controllers = { [controller] }
            navigation.source.isActive = { true }
            navigation.source.tick(now: 0)
            pad.leftTrigger.setValue(1); navigation.source.tick(now: 0.1)
            XCTAssertEqual(keyboard.value, "", "左扳机应清空屏幕键盘中的文字")
            keyboard.value = "Original"
            navigation.source.tick(now: 0.8)
            XCTAssertEqual(keyboard.value, "Original", "长按左扳机不能重复清空")
            pad.leftTrigger.setValue(0); navigation.source.tick(now: 0.9)
            for connected in [false, true] {
                navigation.presentation.connected = connected
                navigation.presentation.symbols = ControllerFaceMapping.identity.symbols(for: .playstation)
                try await Task.sleep(for: .milliseconds(120))
                let targets = navigation.controlTargets(in: keyboard.panel)
                let shortcuts = navigation.shortcutTargets(in: keyboard.panel)
                let clearShortcut = try XCTUnwrap(PadShortcut(rawValue: "leftTrigger"), "清空提示应支持左扳机")
                for (shortcut, key) in [(PadShortcut.tertiary, "space"), (.back, "cancel"), (.rightTrigger, "done"), (clearShortcut, "clear")] {
                    let id = try XCTUnwrap(shortcuts[shortcut]?.first)
                    let target = try XCTUnwrap(targets.first { $0.id == id })
                    let label = NSHostingView(rootView: Text(loc.t("home.keyboard." + key)).font(.manrope(13, .semibold)))
                    let needed = label.fittingSize.width + 32 + (connected ? 22 : 0)
                    let actual = (target.unclippedFrame ?? target.frame).width
                    print("keyboard-label \(language.code)/\(connected)/\(key): needed=\(needed) actual=\(actual)")
                    XCTAssertGreaterThanOrEqual(actual + 0.5, needed, "\(language.code)/\(key)：完整文字、图标与内边距必须放得下")
                }
                let bounds = keyboard.panel.convertToScreen(try XCTUnwrap(keyboard.panel.contentView).bounds)
                for target in targets {
                    XCTAssertTrue(bounds.insetBy(dx: -0.5, dy: -0.5).contains(target.unclippedFrame ?? target.frame), "键盘不能裁切按钮")
                }
                try snapshot(try XCTUnwrap(keyboard.panel.contentView), "keyboard-labels-\(language.code)-\(connected)")
            }
            keyboard.close(apply: false)
        }
    }

    func testSigningIdentityUsesCardInsetsWithoutModeSwitch() async throws {
        let previous = UserDefaults.standard.object(forKey: S2ArmLauncherModel.teamCodeKey)
        UserDefaults.standard.set("fixture-team-id", forKey: S2ArmLauncherModel.teamCodeKey)
        defer { UserDefaults.standard.set(previous, forKey: S2ArmLauncherModel.teamCodeKey) }
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            for width: CGFloat in [680, 820] {
                let page = AnyView(ScrollView { InstallModeCard(model: fixture.model).padding(32) }.frame(width: width, height: 700)
                    .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text).preferredColorScheme(.dark))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                let controls = fixture.navigation.controlTargets(in: window)
                let identity = try XCTUnwrap(controls.first { $0.id == "export.sign.mode" })
                XCTAssertFalse(controls.contains { $0.id.hasPrefix("env.installMode.") }, "导出不再是设置中的装法选项")
                let rightEdge = window.convertToScreen(host.bounds).maxX - 32 - LauncherTheme.Metrics.cardInset
                let delta = rightEdge - identity.frame.maxX
                print("signing-right-edge language=\(language.code) width=\(width) delta=\(delta)")
                XCTAssertEqual(delta, 0, accuracy: 0.5, "签名身份应沿用卡片内容的右边距")
                try snapshot(host, "signing-right-edge-\(language.code)-\(Int(width))")
            }
        }
    }

    func testReadyGroupShowsEmptyStateAndCanReturnToAllGames() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.runner.catalogNeedsPreparation = true
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.first?.needsPreparation != true { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(fixture.model.games.count, 1)
        XCTAssertEqual(fixture.model.games.first?.needsPreparation, true)
        let navigation = fixture.model.gamepad
        navigation.source.controllers = { [] }
        for language in [Lang.zh, .en] {
            let page = AnyView(LauncherView(model: fixture.model, brand: "Original UI fixture").frame(width: 1120, height: 630))
            let (host, window) = try await render(page, fixture: fixture, language: language)
            navigation.attach(window)
            defer { navigation.detach(); window.close() }
            navigation.focus("home.group.ready"); navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(120))
            XCTAssertEqual(fixture.model.home.group.rawValue, "ready")
            let targets = navigation.controlTargets(in: window)
            XCTAssertTrue(targets.contains { $0.id == "home.showAll" })
            XCTAssertFalse(targets.contains { $0.id.hasPrefix("card.") || $0.id.hasPrefix("hero.") })
            try snapshot(host, "home-ready-empty-\(language.code)")
            navigation.focus("home.showAll"); navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(80))
            XCTAssertEqual(fixture.model.home.group, .all)
            XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "card.fixture" }, "返回全部游戏后应恢复待准备的游戏卡片")
        }
    }

    func testSheetsFitCommonDisplayBudgets() {
        let displays: [(CGSize, CGSize)] = [
            (CGSize(width: 1440, height: 900), CGSize(width: 820, height: 700)),
            (CGSize(width: 1024, height: 720), CGSize(width: 820, height: 620)),
            (CGSize(width: 800, height: 600), CGSize(width: 736, height: 500))
        ]
        for (display, expected) in displays {
            let size = LauncherSheetSize.settings.fitted(to: display, height: 700)
            XCTAssertEqual(size.width, expected.width)
            XCTAssertEqual(size.height, expected.height)
        }
    }

    func testBackendDescriptionsAreConciseWhileLogsKeepTheirPunctuation() {
        let loc = Localizer(.zh)
        XCTAssertEqual(loc.backend("测试说明。"), "测试说明")
        XCTAssertEqual(loc.backendLine("▸ 测试说明。"), "▸ 测试说明。")
        XCTAssertEqual(loc.backend("等待中..."), "等待中...")
    }

    func testSigningGuideFailureIsReportedToTheCallingPage() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        var failure: Error?
        fixture.model.openSigningProject { failure = $0 }
        for _ in 0..<100 where failure == nil { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertNotNil(failure, "引导项目打开失败必须回到发起操作的设置页")
        XCTAssertNil(fixture.model.installError, "不能把设置页错误写进另一个安装弹窗")
    }

    func testLongConfirmationKeepsActionsOnScreen() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(fixture.model.games.count, 1)
        for language in [Lang.zh, .en] {
            let page = AnyView(DownstreamConfirmSheet(model: fixture.model, gameID: "fixture", zh: language == .zh).frame(height: 500))
            let (host, window) = try await render(page, fixture: fixture, language: language)
            defer { window.close() }
            XCTAssertLessThanOrEqual(host.bounds.height, (NSScreen.main?.visibleFrame.height ?? 900) - 100,
                                     "长说明应在内容区滚动，不能将操作推出屏幕")
            let bounds = window.convertToScreen(host.bounds)
            for id in ["downstream.original", "downstream.recommend"] {
                let control = try XCTUnwrap(fixture.navigation.controlTargets(in: window).first { $0.id == id }, "长说明不能隐藏 \(id)")
                XCTAssertTrue(bounds.contains(control.frame), "操作必须位于窗口内")
            }
            try snapshot(host, "confirmation-long-\(language.code)")
        }
    }

    func testDescriptionPresentationOmitsFullStopsAndPreservesEllipses() throws {
        // Catalogs and shared specs also serve other frontends. Check the Mac
        // presentation boundary rather than rewriting upstream translations.
        XCTAssertEqual(LauncherCopy.note("Original description."), "Original description")
        XCTAssertEqual(LauncherCopy.note("原始说明。"), "原始说明")
        XCTAssertEqual(LauncherCopy.note("Loading..."), "Loading...")
        func check(_ text: String, _ context: String) {
            let note = LauncherCopy.note(text)
            XCTAssertFalse(note.hasSuffix("。") || (note.hasSuffix(".") && !note.hasSuffix("...")), context)
        }
        for (language, catalog) in L10n.catalogs {
            for (key, text) in catalog {
                check(text, "\(language)/\(key)")
            }
        }
        for item in try GameSettingsSpec.macItems() {
            for language in ["zh-Hans", "en"] {
                check(item.detail(language), "\(item.id)/\(language)")
            }
        }
        let source = try GameSettingsSpec.url().deletingLastPathComponent().appendingPathComponent("experience.v1.json")
        let spec = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: source)) as? [String: Any])
        let launcher = try XCTUnwrap(spec["launcher"] as? [String: Any])
        for row in try XCTUnwrap(launcher["items"] as? [[String: Any]]) {
            for language in ["zh-Hans", "en"] {
                let text = try XCTUnwrap(row[language] as? String)
                check(text, "\(row["id"] ?? "")/\(language)")
            }
        }
    }

    func testUpgradeActionsStayVisibleInAShortWindow() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let games = (1...9).map { SGradeScanGame(["id": "original-\($0)", "name": "Original game \($0)", "program": "original", "total": 1, "iphone": true, "patches": ["fps": 1]]) }
        for language in [Lang.zh, .en] {
            let code = language == .zh ? "zh-Hans" : "en"
            let page = AnyView(SGradeUpgradeBody(games: games, failure: nil, zh: language == .zh,
                x: { BackendText.launcherText("sgrade.upgrade." + $0, language: code) }, upgrade: { _ in }, close: {}).frame(height: 500))
            let (host, window) = try await render(page, fixture: fixture, language: language)
            defer { window.close() }
            let bounds = window.convertToScreen(host.bounds)
            let list = try XCTUnwrap(scrollViews(in: host).first { $0.hasVerticalScroller })
            XCTAssertGreaterThan(list.contentView.bounds.height, 60, "游戏列表不能被压成不可见的滚动区")
            XCTAssertGreaterThan(try XCTUnwrap(list.documentView).bounds.height, 100)
            for id in ["sgrade.upgrade.close"] {
                let action = try XCTUnwrap(fixture.navigation.controlTargets(in: window).first { $0.id == id }, "升级操作必须始终可见")
                XCTAssertTrue(bounds.contains(action.frame))
            }
            try snapshot(host, "upgrade-short-\(language.code)")
        }
    }

    /// S 级优化手动 / 批量升级（11-18 维护者：点了才做；可批量，写明批量升级、有全选）：每款一个勾选框和「升级」，
    ///   点某款「升级」只交那一款；页脚「批量升级所选（N 款）」N 跟勾选走、没勾时不可点；正在升级的那款勾选框和按钮都不可用；
    ///   批量进行中页脚换成「第 i 款 / 共 n 款」+「停止」；没有可升级的款不出行按钮；进度环 / 进度条四种状态出图
    /// #2066：没在这台 Mac 上玩过的这款，恢复只放进待放区——状态读出等着的文件数（不再当「还没有存档」）、恢复结果标 pending；旧作业层不给时照旧（期望手写）
    /// #2081：键位弹窗读作业层 loadInput 的结果（同起局的规则）：存坏了标 problem、显示回落后的那份；玩家 2 沿用玩家 1 标 inherited；没有配置（null）= 标准键位。期望手写
    func testInputPopupReadsJobLayerRule() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let fallback: [String: Any] = ["schema": 1, "keyboard": ["A": ["code": 19]], "mouse": [String: Any](), "gamepad": ["A": "east"], "axes": [String: Any](),
                                       "deadzone": 0.15, "mouseOptions": ["mode": "off", "sensitivityX": 1, "sensitivityY": 1]]
        fixture.runner.loadInputPlayers = [["player": 1, "config": fallback, "source": "default", "problem": true],
                                           ["player": 2, "config": fallback, "source": "player1", "problem": false],
                                           ["player": 3, "config": NSNull(), "source": "standard", "problem": false],
                                           ["player": 4, "config": NSNull(), "source": "standard", "problem": false]]
        let load = try await fixture.model.loadInputs(gameID: "fixture")
        let all = load.players
        XCTAssertEqual(fixture.runner.requests.filter { $0["operation"] as? String == "loadInput" }.count, 1, "四位玩家一次读")
        XCTAssertEqual(all[1]?.source, "default")
        XCTAssertFalse(load.defaultSet, "旧作业层不带 default：当没设过")
        XCTAssertEqual(load.allGamesDefault, .standard)
        XCTAssertEqual(all[1]?.problem, true)
        XCTAssertEqual(all[1]?.config.keyboard["A"]?.code, 19, "显示回落后的那份（19 ≠ 标准键位的 14，解码失败落回标准会被看出来）")
        XCTAssertEqual(all[2]?.inherited, true)
        XCTAssertEqual(all[2]?.config, all[1]?.config)
        XCTAssertEqual(all[3]?.config, .standard)
        XCTAssertEqual(all[3]?.problem, false)
    }

    /// #1987 Mac「管理键位」：导入的几道检查（同 iPad：读不了 / 太大 / 不是 JSON / 不是 schema 1 / 缺项 / 重复绑定都拒收）；导出的文件能原样导回来。期望手写
    func testInputImportChecksAndExportRoundTrip() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("input-import-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        func file(_ name: String, _ data: Data) throws -> URL { let u = dir.appendingPathComponent(name); try data.write(to: u); return u }
        func problem(_ url: URL) -> InputConfig.ImportProblem? { if case .failure(let p) = InputConfig.imported(from: url) { return p }; return nil }
        var custom = InputConfig.standard; custom.keyboard["A"] = KeyBinding(code: 19); custom.backButtons = ["GL": "east"]
        let exported = try file("out.json", custom.exportData())
        guard case .success(let back) = InputConfig.imported(from: exported) else { return XCTFail("导出的文件导不回来") }
        XCTAssertEqual(back, custom)
        XCTAssertEqual(problem(dir.appendingPathComponent("missing.json")), .read)
        XCTAssertEqual(problem(try file("big.json", Data(repeating: 0x20, count: 64 * 1024 + 1))), .tooLarge)
        XCTAssertEqual(problem(try file("text.json", Data("{ not json".utf8))), .json)
        XCTAssertEqual(problem(try file("v2.json", Data(#"{"schema": 2}"#.utf8))), .schema)
        XCTAssertEqual(problem(try file("bool.json", Data(#"{"schema": true}"#.utf8))), .schema, "true 不是 schema 1")
        XCTAssertEqual(problem(try file("short.json", Data(#"{"schema": 1, "keyboard": {}}"#.utf8))), .invalid)
        var dup = InputConfig.standard; dup.keyboard["B"] = KeyBinding(code: 14)   // 和 A 同一个键
        XCTAssertEqual(problem(try file("dup.json", JSONEncoder().encode(dup))), .duplicate)
        // 省略 modifiers / invert 等（iPad 导出或手写的 input.json）按缺省读，和标准键位是同一套；空背键与没有背键一样
        let terse = #"{"schema":1,"keyboard":{"A":{"code":14}},"mouse":{},"gamepad":{},"axes":{},"deadzone":0.15,"mouseOptions":{"mode":"camera","sensitivityX":1,"sensitivityY":1},"backButtons":{}}"#
        guard case .success(let t) = InputConfig.imported(from: try file("terse.json", Data(terse.utf8))) else { return XCTFail("省略缺省字段的文件应能导入") }
        var expect = InputConfig(keyboard: ["A": KeyBinding(code: 14)], mouse: [:], gamepad: [:], axes: [:])
        XCTAssertTrue(t.same(expect)); expect.deadzone = 0.2; XCTAssertFalse(t.same(expect))
    }

    /// #1987：「恢复默认」回到所有游戏的默认（不是标准键位），点「保存」才写；「设为所有游戏的默认」点了不写，点「保存」时只调 setDefaultInput。期望手写
    func testInputRestoreAndSetDefaultApplyOnlyOnSave() async throws {
        func custom(_ code: Int) -> [String: Any] {
            var c = InputConfig.standard; c.keyboard["A"] = KeyBinding(code: code)
            return try! JSONSerialization.jsonObject(with: JSONEncoder().encode(c)) as! [String: Any]
        }
        func ops(_ fixture: SettingsFixture) -> [String] { fixture.runner.requests.compactMap { $0["operation"] as? String }.filter { $0 != "loadInput" && $0 != "locations" && $0 != "catalog" } }
        func waitFor(_ fixture: SettingsFixture, _ op: String) async throws {
            for _ in 0..<100 where !fixture.runner.requests.contains(where: { $0["operation"] as? String == op }) { try await Task.sleep(for: .milliseconds(10)) }
        }
        for flow in ["restore", "setDefault"] {
            let fixture = try SettingsFixture()
            defer { fixture.remove() }
            fixture.runner.loadInputDefault = custom(19)
            fixture.runner.loadInputPlayers = [["player": 1, "config": custom(20), "source": "game", "problem": false]]
                + (2...4).map { ["player": $0, "config": custom(20), "source": "player1", "problem": false] }
            let (_, window) = try await render(AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")), fixture: fixture, language: .en)
            fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
            defer { fixture.navigation.detach(); window.close() }
            fixture.navigation.focus(flow == "restore" ? "input.restoreDefault" : "input.manage.setDefault"); fixture.navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(100))
            XCTAssertEqual(ops(fixture), [], "\(flow)：点了先不写")
            if flow == "setDefault" { try snapshotBottom(window, "input-manage-pending-en") }
            fixture.navigation.focus("input.save"); fixture.navigation.handle(.confirm)
            let op = flow == "restore" ? "saveInput" : "setDefaultInput"
            try await waitFor(fixture, op)
            XCTAssertEqual(ops(fixture), [op], "\(flow)：保存时只调 \(op)")
            let request = try XCTUnwrap(fixture.runner.requests.first { $0["operation"] as? String == op })
            let code = (((request["config"] as? [String: Any])?["keyboard"] as? [String: Any])?["A"] as? [String: Any])?["code"] as? Int
            XCTAssertEqual(code, flow == "restore" ? 19 : 20, flow == "restore" ? "恢复到所有游戏的默认（19），不是标准键位（14）" : "用弹窗里当前的键位设默认")
            XCTAssertEqual(request["player"] as? Int, 1)
            XCTAssertEqual(request["gameId"] as? String, "fixture")
        }
    }

    /// #1987 复审建议：所有游戏的默认文件坏了（作业层 defaultProblem）→ 弹窗提示一行，不静默退回标准键位；
    ///   没坏 / 旧作业层不带这个字段 → 不提示；重新「设为默认」并保存后收掉（点了还没保存时还在）。期望手写
    func testInputBrokenAllGamesDefaultShowsHintUntilReplaced() async throws {
        // 提示是纯文字（不是控件），按渲染出来的字认：弹窗顶部有没有这句
        func shown(_ window: NSWindow) throws -> Bool {
            let host = try XCTUnwrap(window.contentView)
            if let scroll = scrollViews(in: host).first(where: { $0.hasVerticalScroller }) {
                scroll.contentView.scroll(to: CGPoint(x: 0, y: scroll.documentView?.isFlipped == false ? max(0, (scroll.documentView?.bounds.height ?? 0) - scroll.contentView.bounds.height) : 0))
                scroll.reflectScrolledClipView(scroll.contentView)
            }
            host.layoutSubtreeIfNeeded()
            return try renderedText(host, language: .en).map(\.text).joined(separator: " ").contains("default for all games could not")
        }
        for problem in [nil, false, true] as [Bool?] {
            let fixture = try SettingsFixture()
            defer { fixture.remove() }
            fixture.runner.loadInputDefault = NSNull()
            fixture.runner.loadInputDefaultProblem = problem
            let (host, window) = try await render(AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")), fixture: fixture, language: .en)
            fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
            defer { fixture.navigation.detach(); window.close() }
            try await Task.sleep(for: .milliseconds(100))
            XCTAssertEqual(try shown(window), problem == true, "defaultProblem=\(String(describing: problem))")
            guard problem == true else { continue }
            try snapshot(host, "input-default-problem-en")
            fixture.navigation.focus("input.manage.setDefault"); fixture.navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(100))
            XCTAssertTrue(try shown(window), "点了「设为默认」还没保存：默认文件还是坏的，提示留着")
            fixture.navigation.focus("input.save"); fixture.navigation.handle(.confirm)
            for _ in 0..<100 where !fixture.runner.requests.contains(where: { $0["operation"] as? String == "setDefaultInput" }) { try await Task.sleep(for: .milliseconds(10)) }
            try await Task.sleep(for: .milliseconds(100))
            XCTAssertFalse(try shown(window), "设为默认并保存后默认文件已替换，提示收掉")
        }
    }

    /// #1987：玩家 2–4「设为所有游戏的默认」不可用；还沿用玩家 1、没改就保存 → resetInput（继续沿用玩家 1，不单独写一份）；
    ///   「恢复默认」回到玩家 1 存着的那套再保存同样回到沿用。期望手写
    func testInputPlayer2CannotSetDefaultAndFollowsPlayer1() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        var mine = InputConfig.standard; mine.keyboard["A"] = KeyBinding(code: 21)
        let own = try JSONSerialization.jsonObject(with: JSONEncoder().encode(mine))
        fixture.runner.loadInputPlayers = [["player": 1, "config": NSNull(), "source": "standard", "problem": false],
                                           ["player": 2, "config": own, "source": "game", "problem": false],
                                           ["player": 3, "config": NSNull(), "source": "player1", "problem": false],
                                           ["player": 4, "config": NSNull(), "source": "player1", "problem": false]]
        let (host, window) = try await render(AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")), fixture: fixture, language: .zh)
        let navigation = fixture.navigation
        navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
        UIStage.show(window)
        defer { navigation.detach(); window.close() }
        try await settle(window, navigation, fixture.model.gamepad)
        let players = navigation.controlTargets(in: window).filter { abs($0.frame.height - 30) < 0.5 }.sorted { $0.frame.minX < $1.frame.minX }
        XCTAssertEqual(players.count, 4)
        try press(players[1].id, navigation)
        try await Task.sleep(for: .milliseconds(200))
        try snapshotBottom(window, "input-manage-player2-zh")
        _ = host
        XCTAssertTrue(navigation.controlTargets(in: window).contains { $0.id == "input.manage.import" }, "管理键位组已在视野里")
        XCTAssertFalse(navigation.controlTargets(in: window).contains { $0.id == "input.manage.setDefault" }, "玩家 2 下「设为默认」置灰，手柄也选不到")
        navigation.focus("input.restoreDefault"); navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(100))
        navigation.focus("input.save"); navigation.handle(.confirm)
        for _ in 0..<100 where !fixture.runner.requests.contains(where: { ["resetInput", "saveInput", "setDefaultInput"].contains($0["operation"] as? String ?? "") }) {
            try await Task.sleep(for: .milliseconds(10))
        }
        try await Task.sleep(for: .milliseconds(50))
        let writes = fixture.runner.requests.filter { ["resetInput", "saveInput", "setDefaultInput"].contains($0["operation"] as? String ?? "") }
        XCTAssertEqual(writes.compactMap { $0["operation"] as? String }, ["resetInput"], "玩家 2 恢复默认 = 回到沿用玩家 1；设为默认点不动")
        XCTAssertEqual(writes.first?["player"] as? Int, 2)
    }

    /// 滚到弹窗最底下截图（只在设了 S2A_SETTINGS_SHOTS 时写图）
    private func snapshotBottom(_ window: NSWindow, _ name: String) throws {
        guard let host = window.contentView, let scroll = scrollViews(in: host).first(where: { $0.hasVerticalScroller }), let document = scroll.documentView else { return }
        scroll.contentView.scroll(to: CGPoint(x: 0, y: max(0, document.bounds.height - scroll.contentView.bounds.height)))
        scroll.reflectScrolledClipView(scroll.contentView)
        host.layoutSubtreeIfNeeded()
        try snapshot(host, name)
    }

    func testSaveStatusReadsPendingSeed() throws {
        let waiting = try XCTUnwrap(SaveStatusInfo(["hasSave": false, "fileCount": 0, "totalBytes": 0, "pendingSeed": ["fileCount": 3, "totalBytes": 45056]]))
        XCTAssertEqual(waiting.pendingFiles, 3)
        XCTAssertNil(try XCTUnwrap(SaveStatusInfo(["hasSave": false])).pendingFiles)
        XCTAssertTrue(SaveRestoreInfo(["committed": true, "hadOriginal": false, "pending": true]).pending)
        XCTAssertFalse(SaveRestoreInfo(["committed": true, "hadOriginal": true]).pending)
        XCTAssertEqual(BackendText.launcherText("save.pending", language: "zh-Hans").replacingOccurrences(of: "{0}", with: "3"),
                       "已放好 3 个文件，第一次在这台 Mac 上启动这款时放进存档")
    }

    /// 回到上一份存档（Mac）：上一局出事才问；回到上一份 → 再换回，两次都交 saveRestorePrevious、状态翻回原样；齿轮页一行有上一份才出
    func testPreviousSaveAskRestoreAndSwitchBack() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let game = GameSummary(id: "fixture", name: "Original UI fixture")
        fixture.runner.previousTrouble = ""
        let quiet = await fixture.model.previousSavePromptIfNeeded(game, uiLanguage: "zh-Hans")
        XCTAssertNil(quiet, "有上一份但上一局没出事：不问")
        fixture.runner.previousTrouble = "earlyCrash"
        let ask = await fixture.model.previousSavePromptIfNeeded(game, uiLanguage: "zh-Hans")
        XCTAssertEqual(ask?.trouble, "earlyCrash", "上一局开局就崩、有上一份：问")
        let first = await fixture.model.switchPreviousSave("fixture")
        XCTAssertNil(first)
        XCTAssertTrue(fixture.runner.previousSwapped, "回到上一份")
        XCTAssertEqual(fixture.model.previousSaveSwitched, "fixture", "换完提示可以换回来")
        fixture.model.previousSaveSwitched = nil
        let second = await fixture.model.switchPreviousSave("fixture")
        XCTAssertNil(second)
        XCTAssertFalse(fixture.runner.previousSwapped, "再点一次换回原样")
        XCTAssertEqual(fixture.runner.requests.filter { $0["operation"] as? String == "saveRestorePrevious" }.count, 2)
        for language in [Lang.zh, .en] {
            let code = language == .zh ? "zh-Hans" : "en"
            let t = { (k: String) in BackendText.launcherText(k, language: code) }
            XCTAssertEqual(t("save.previous.ask.title"), language == .zh ? "这款上次没能正常读存档，要回到上一份存档吗？" : "This game couldn’t load its save last time. Go back to the previous save?")
            XCTAssertEqual(t("save.previous.ask.restore"), language == .zh ? "回到上一份" : "Go back")
            XCTAssertEqual(t("save.previous.ask.start"), language == .zh ? "照常开始" : "Start as usual")
            let (host, window) = try await render(AnyView(PreviousSaveRow(model: fixture.model, gameID: "fixture", t: t).frame(width: 640).padding(24).launcherSheet()), fixture: fixture, language: language)
            defer { window.close() }
            for _ in 0..<40 where !Set(fixture.navigation.controlTargets(in: window).map(\.id)).contains("save.previous.switch") { try await Task.sleep(nanoseconds: 50_000_000) }
            XCTAssertTrue(Set(fixture.navigation.controlTargets(in: window).map(\.id)).contains("save.previous.switch"), "有上一份：齿轮页出「换到上一份」")
            window.setContentSize(host.fittingSize)   // 首次渲染时还没查到「有上一份」、那一行是空的，窗口按空的量：查到后按实际内容重量再截
            host.layoutSubtreeIfNeeded()
            try snapshot(host, "previous-save-row-" + language.code)
        }
    }

    func testSGradeManualUpgradeOnMac() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let games = [SGradeScanGame(["id": "alpha", "name": "Original Alpha", "program": "original", "total": 2, "patches": ["fps": 1, "widescreen": 1]]),
                     SGradeScanGame(["id": "beta", "name": "Original Beta", "program": "original", "total": 1, "patches": ["fps": 1]])]
        for language in [Lang.zh, .en] {
            let code = language == .zh ? "zh-Hans" : "en"
            let x = { (k: String) in BackendText.launcherText("sgrade.upgrade." + k, language: code) }
            var picked: [[String]] = []
            let body = SGradeUpgradeBody(games: games, failure: nil, zh: language == .zh, x: x, upgrade: { picked.append($0) }, close: {}, upgrading: ["beta"])
            body.upgrade(["alpha"])
            XCTAssertEqual(picked, [["alpha"]], "点某款只交那一款")
            XCTAssertEqual(body.chosen, [], "没勾选时批量所选为空")
            let (host, window) = try await render(AnyView(body), fixture: fixture, language: language)
            defer { window.close() }
            let ids = Set(fixture.navigation.controlTargets(in: window).map(\.id))
            XCTAssertTrue(ids.isSuperset(of: ["sgrade.upgrade.row.alpha", "sgrade.upgrade.pick.alpha", "sgrade.upgrade.selectAll"]), "每款有勾选和「升级」，组标题旁有全选")
            XCTAssertFalse(ids.contains("sgrade.upgrade.row.beta"), "正在升级的那款按钮不可点")
            XCTAssertFalse(ids.contains("sgrade.upgrade.pick.beta"), "正在升级的那款勾选框不可选")
            XCTAssertFalse(ids.contains("sgrade.upgrade.batch"), "没勾选时「批量升级所选」不可点")
            XCTAssertEqual(x("batch").replacingOccurrences(of: "{0}", with: "2"), language == .zh ? "批量升级所选（2 款）" : "Batch upgrade selected (2)")
            try snapshot(host, "sgrade-manual-page-" + language.code)
            let running = SGradeUpgradeBody(games: games, failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {},
                                            upgrading: ["alpha", "beta"], batch: SGradeBatch(running: true, index: 1, count: 2),
                                            states: ["alpha": "running", "beta": "queued"], progress: ["alpha": 0.37])
            XCTAssertEqual(running.rowTitle("alpha"), language == .zh ? "正在升级 37%" : "Upgrading 37%", "正在做的那行带百分比")
            XCTAssertEqual(running.rowTitle("beta"), language == .zh ? "排队中" : "Queued", "排着的写排队中，不冒充在升")
            let after = SGradeUpgradeBody(games: games, failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {},
                                          states: ["alpha": "done", "beta": "incomplete"])
            XCTAssertEqual(after.rowTitle("alpha"), language == .zh ? "已完成" : "Done")
            XCTAssertEqual(after.rowTitle("beta"), language == .zh ? "继续升级" : "Resume upgrade", "被停 / 出错的款再点接着升")
            let skipped = SGradeUpgradeBody(games: games, failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {}, states: ["alpha": "skipped"])
            XCTAssertEqual(skipped.rowTitle("alpha"), language == .zh ? "正在玩，稍后再升" : "Playing — upgrade later")
            XCTAssertEqual(skipped.rowTitle("beta"), language == .zh ? "升级" : "Upgrade")
            // 作业层说已没有要补的段（bfPending 全 false）：写「已完成」、不可点不可选；有一边要补的照常「升级」
            let current = [SGradeScanGame(["id": "gamma", "name": "Up to date", "program": "original", "total": 3, "bfPending": ["iphone": false, "mac": false]]),
                           SGradeScanGame(["id": "delta", "name": "Needs iPhone", "program": "original", "total": 3, "bfPending": ["iphone": true, "mac": false]])]
            let cur = SGradeUpgradeBody(games: current, failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {})
            XCTAssertEqual(cur.rowTitle("gamma"), language == .zh ? "已完成" : "Done", "已是最新的款不再显示「升级」")
            XCTAssertTrue(cur.locked("gamma")); XCTAssertFalse(cur.locked("delta"))
            XCTAssertEqual(cur.rowTitle("delta"), language == .zh ? "升级" : "Upgrade")
            let curStopped = SGradeUpgradeBody(games: current, failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {}, states: ["gamma": "incomplete"])
            XCTAssertFalse(curStopped.locked("gamma"), "被停的款即使查找时没要补的也能再点（查找结果可能是旧的）")
            // 没有补丁（total 0）但还缺片上读取用的着色器（bfPending 有一边 true）：进可升组、能点能勾（同作业层 upgradable 计数）
            let bfOnly = SGradeUpgradeBody(games: [SGradeScanGame(["id": "eps", "name": "Shader only", "program": "original", "total": 0, "bfPending": ["iphone": true, "mac": false]]),
                                                  SGradeScanGame(["id": "zeta", "name": "Nothing", "program": "original", "total": 0, "bfPending": ["iphone": false, "mac": false]])],
                                           failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {})
            XCTAssertEqual(bfOnly.upgradeIDs, ["eps"], "只缺片上读取着色器的款可升，什么都不缺的不进可升组")
            XCTAssertFalse(bfOnly.locked("eps"))
            XCTAssertEqual(x("bf"), language == .zh ? "补上片上读取用的着色器" : "Add on-chip read shaders")
            let (host3, window3) = try await render(AnyView(running), fixture: fixture, language: language)
            defer { window3.close() }
            let ids3 = Set(fixture.navigation.controlTargets(in: window3).map(\.id))
            XCTAssertTrue(ids3.contains("sgrade.upgrade.stop"), "批量进行中有「停止」")
            XCTAssertFalse(ids3.contains("sgrade.upgrade.batch"), "批量进行中不出「批量升级所选」")
            XCTAssertEqual(x("batchProgress").replacingOccurrences(of: "{0}", with: "1").replacingOccurrences(of: "{1}", with: "2"),
                           language == .zh ? "批量升级：第 1 款 / 共 2 款" : "Batch upgrade: game 1 of 2")
            try snapshot(host3, "sgrade-manual-batch-running-" + language.code)
            let noPatch = [SGradeScanGame(["id": "delta", "name": "No patch", "program": "original", "total": 0])]
            let none = AnyView(SGradeUpgradeBody(games: noPatch, failure: nil, zh: language == .zh, x: x, upgrade: { _ in }, close: {}))
            let (_, window2) = try await render(none, fixture: fixture, language: language)
            defer { window2.close() }
            XCTAssertFalse(fixture.navigation.controlTargets(in: window2).contains { $0.id.hasPrefix("sgrade.upgrade.row.") }, "没有可升级的款不出行按钮")
            let txt = { (id: String) in BackendText.launcherText(id, language: code) }
            XCTAssertEqual(LauncherView.sgradeUpgradingTitle(0.379, txt), language == .zh ? "正在升级 37%" : "Upgrading 37%")
            XCTAssertEqual(LauncherView.sgradeUpgradingTitle(nil, txt), language == .zh ? "正在升级…" : "Upgrading…")
        }
        for (name, p) in [("0", 0.0), ("37", 0.37), ("100", 1.0), ("wait", -1.0)] as [(String, Double)] {
            let progress: Double? = p < 0 ? nil : p
            let view = AnyView(VStack(spacing: 18) {
                SGradeUpgradeRing(progress: progress, side: 180).frame(width: 180, height: 180).background(Color.gray)
                SGradeUpgradeBar(progress: progress).frame(width: 220).padding(8).background(Color.white)
            }.padding(20))
            let (host, window) = try await render(view, fixture: fixture, language: .zh)
            defer { window.close() }
            try snapshot(host, "sgrade-manual-progress-" + name)
        }
    }

    func testUpgradeRowsAndConfirmationUseConsistentAlignment() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let games = [
            SGradeScanGame(["id": "alpha", "name": "Original Alpha", "program": "original", "total": 4,
                            "patches": ["widescreen": 1, "fps": 1, "gfx": 1, "cheat": 1]]),
            SGradeScanGame(["id": "beta", "name": "Original Beta", "program": "original", "total": 3,
                            "patches": ["widescreen": 1, "fps": 1, "gfx": 1]])
        ]
        for language in [Lang.zh, .en] {
            let code = language == .zh ? "zh-Hans" : "en"
            let page = AnyView(SGradeUpgradeBody(games: games, failure: nil, zh: language == .zh,
                x: { BackendText.launcherText("sgrade.upgrade." + $0, language: code) }, upgrade: { _ in }, close: {}))
            let (host, window) = try await render(page, fixture: fixture, language: language)
            defer { window.close() }
            let bounds = window.convertToScreen(host.bounds)
            let actions = fixture.navigation.controlTargets(in: window)
            XCTAssertTrue(Set(actions.map(\.id)).isSuperset(of: ["sgrade.upgrade.row.alpha", "sgrade.upgrade.row.beta", "sgrade.upgrade.selectAll", "sgrade.upgrade.close"]), "每款「升级」、全选、关闭都在")
            let button = try XCTUnwrap(actions.first { $0.id == "sgrade.upgrade.close" })   // 最右那颗量边距
            print("upgrade-footer \(language.code): bottom=\(button.frame.minY - bounds.minY), right=\(bounds.maxX - button.frame.maxX)")
            XCTAssertEqual(button.frame.minY - bounds.minY, 16, accuracy: 1, "升级弹窗应使用与设置页一致的 16 pt 底边距")
            XCTAssertEqual(bounds.maxX - button.frame.maxX, 32, accuracy: 1)
            let document = try XCTUnwrap(scrollViews(in: host).first?.documentView)
            let bitmap = try XCTUnwrap(document.bitmapImageRepForCachingDisplay(in: document.bounds))
            document.cacheDisplay(in: document.bounds, to: bitmap)
            // Only the two gray descriptions occupy the middle column; names are on the left, the per-game buttons and
            //   「全选」 on the far right (scan stops before them).
            var rows: [(firstY: Int, lastY: Int, minX: Int)] = []
            for y in 0..<bitmap.pixelsHigh {
                var first: Int?
                for x in (bitmap.pixelsWide / 2)..<(bitmap.pixelsWide * 7 / 10) {
                    if let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB),
                       min(color.redComponent, color.greenComponent, color.blueComponent) > 0.4 {
                        first = x; break
                    }
                }
                if let first {
                    if let last = rows.last, y - last.lastY <= 4 {
                        rows[rows.count - 1] = (last.firstY, y, min(last.minX, first))
                    } else { rows.append((y, y, first)) }
                }
            }
            rows = rows.filter { $0.lastY - $0.firstY >= 4 } // Ignore the thin row separator.
            XCTAssertEqual(rows.count, 2, "应能从实际渲染结果中找到两行优化说明")
            if rows.count == 2 {
                print("upgrade-description-left \(language.code): \(rows.map(\.minX))")
                XCTAssertEqual(rows[0].minX, rows[1].minX, accuracy: 2, "右侧说明文字应共用左边界")
            }
            try snapshot(host, "upgrade-aligned-\(language.code)")
        }
    }

    func testExportContentUsesABoundedWidthAlignedToTheLeft() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        let game = try XCTUnwrap(fixture.model.games.first)
        for language in [Lang.zh, .en] {
            for width: CGFloat in [800, 1120, 1440, 1920] {
                let padFocus = PadFocus()
                let page = AnyView(ExportView(model: fixture.model, covers: CoverCache(), game: game)
                    .environmentObject(padFocus).padScope("main", padFocus)
                    .background(LauncherWindowMaterial()).foregroundStyle(LauncherTheme.Colors.text).preferredColorScheme(.dark)
                    .frame(width: width, height: 630))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                let bounds = window.convertToScreen(host.bounds)
                let targets = fixture.navigation.controlTargets(in: window)
                let cover = try XCTUnwrap(targets.first { $0.id == "export.cover" })
                let start = try XCTUnwrap(padFocus.frames["main"]?.first { $0.id == "export.start" })
                // 接鼠标、没接触控板的 Mac 是常显滚动条，要从可用宽度里扣掉它占的那条
                let expectedContentWidth = width - 96 - (scrollViews(in: host).first.map(scrollerReserve) ?? 0)
                print("export-bounded \(language.code)/\(width): left=\(cover.frame.minX - bounds.minX), right=\(width - start.frame.maxX)")
                XCTAssertEqual(cover.frame.minX - bounds.minX, 48, accuracy: 1, "出包内容与底栏共用 48 pt 左边距")
                XCTAssertEqual(width - start.frame.maxX, 48, accuracy: 1, "底栏操作应保持距窗口右边缘 48 pt")
                XCTAssertTrue(bounds.contains(cover.frame))
                XCTAssertTrue(CGRect(x: 0, y: 0, width: width, height: 630).contains(start.frame))
                fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window); fixture.navigation.source.stop()
                defer { fixture.navigation.detach() }
                fixture.navigation.focus("export.location")
                try await Task.sleep(for: .milliseconds(60))
                let location = try XCTUnwrap(fixture.navigation.controlTargets(in: window).first { $0.id == "export.location" })
                XCTAssertEqual(location.frame.maxX - cover.frame.minX, expectedContentWidth - 14, accuracy: 1, "存放位置行的右缘也应与内容对齐，只含行内的 14 pt 边距")
                fixture.navigation.hide()
                if let scroll = scrollViews(in: host).first, let document = scroll.documentView {
                    let clip = scroll.contentView
                    clip.scroll(to: CGPoint(x: 0, y: document.isFlipped ? 0 : max(0, document.bounds.height - clip.bounds.height)))
                    scroll.reflectScrolledClipView(clip)
                }
                try snapshot(host, "export-bounded-\(language.code)-\(Int(width))")
            }
        }
    }

    func testImportFooterUsesTheStandardSheetMargins() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            for kind in [ImportSheet.Kind.games, .keys] {
                fixture.model.showAddGames = kind == .games
                fixture.model.showKeys = kind == .keys
                let (host, window) = try await render(AnyView(ImportSheet(model: fixture.model, kind: kind)), fixture: fixture, language: language)
                let navigation = fixture.navigation
                navigation.source.controllers = { [] }; navigation.attach(window); navigation.source.stop()
                defer { navigation.detach(); window.close() }
                for connected in [false, true] {
                    navigation.presentation.connected = connected
                    try await Task.sleep(for: .milliseconds(70))
                    let cancelID = try XCTUnwrap(navigation.shortcutTargets(in: window)[.back]?.first)
                    let cancel = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == cancelID })
                    let bounds = window.convertToScreen(host.bounds)
                    print("import-footer \(language.code)/\(kind)/\(connected): right=\(bounds.maxX - cancel.frame.maxX), bottom=\(cancel.frame.minY - bounds.minY)")
                    XCTAssertEqual(bounds.maxX - cancel.frame.maxX, 32, accuracy: 1)
                    XCTAssertEqual(cancel.frame.minY - bounds.minY, 16, accuracy: 1, "取消按钮与其他弹窗共用 16 pt 底边距")
                    XCTAssertEqual(cancel.frame.height, 36, accuracy: 1, "普通弹窗仍使用普通控件高度")
                    XCTAssertTrue(bounds.contains(cancel.frame))
                    try snapshot(host, "import-footer-\(language.code)-\(kind)-\(connected ? "connected" : "disconnected")")
                }
                navigation.handle(.back)
                XCTAssertFalse(fixture.model.showAddGames)
                XCTAssertFalse(fixture.model.showKeys)
                XCTAssertFalse(fixture.runner.requests.contains { ["import", "keys"].contains($0["operation"] as? String ?? "") })
            }
        }
    }

    /// 0.6.0 Mac：清单按钮只按作业层给的 action 画（appleId → 「登录 Apple ID」打开 S2Arm 自己的登录面板）；不再有「打开 Xcode」「去设置」两个
    func testInstallChecklistButtonFollowsItemAction() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.checkInstall("fixture")
        for _ in 0..<100 where fixture.model.installReadiness == nil { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertNotNil(fixture.model.installReadiness)
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(InstallView(model: fixture.model, gameID: "fixture")), fixture: fixture, language: language)
            fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
            defer { fixture.navigation.detach(); window.close() }
            let targets = fixture.navigation.controlTargets(in: window)
            let login = try XCTUnwrap(targets.first { $0.id == "install.action.appleId" })
            XCTAssertFalse(targets.contains { $0.id == "install.openXcode" || $0.id == "install.goSettings" })
            XCTAssertTrue(window.convertToScreen(host.bounds).contains(login.frame))
            try snapshot(host, "install-repair-\(language.code)")
            fixture.navigation.focus("install.action.appleId"); fixture.navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(100))
            XCTAssertEqual(fixture.model.appleLoginHost, "install", "点了就从装机窗弹登录面板")
            fixture.model.closeAppleLogin()
        }
    }

    // 登录成功页发现登错了号：期望（手写）成功页有「换一个 Apple ID」，点了面板不关（host 仍是 main）、回到输入页（phase = idle，邮箱框出现）
    func testAppleLoginDonePageCanSwitchToAnotherAccount() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let model = fixture.model
        model.openAppleLogin(host: "main")
        model.appleLoginPhase = .done(AppleAccount(account: "a@example.com", team: "TEAM", teamName: "T"))
        let (_, window) = try await render(AnyView(AppleLoginSheet(model: model)), fixture: fixture, language: .zh)
        fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
        defer { fixture.navigation.detach(); window.close() }
        XCTAssertTrue(fixture.navigation.controlTargets(in: window).contains { $0.id == "appleLogin.switch" }, "成功页有换号入口")
        fixture.navigation.focus("appleLogin.switch"); fixture.navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertEqual(model.appleLoginHost, "main", "面板不关")
        XCTAssertEqual(model.appleLoginPhase, .idle, "回到输入页")
        model.closeAppleLogin()
    }

    // #1756：「更新所选」没走成时整页显示原因、给「重试」（原先这单的失败页哪里都不显示，玩家只看到进度条跑完）。
    //   期望（手写）：失败的 pushSelected 整页（wholePage），完成的不整页；整页里有「重试」，点了按上一单那台续推（作业请求带 resume）
    func testFailedUpdateSelectedShowsWholePageWithRetry() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        var failed = JobSession(kind: .pushSelected, gameID: nil, name: "更新所选", phase: .finished(.failed))
        failed.message = "资源包写进 iPhone 时中断（连不上 Apple 设备服务）"
        XCTAssertTrue(failed.wholePage)
        XCTAssertFalse(JobSession(kind: .pushSelected, gameID: nil, name: "更新所选", phase: .finished(.completed)).wholePage, "推成了不整页")
        XCTAssertTrue(JobSession(kind: .installUpdate, gameID: nil, name: "更新 S2Arm").wholePage, "更新 S2Arm 照旧整页")
        fixture.model.lastPushSelectedDevice = "PHONE-1"
        let (_, window) = try await render(AnyView(JobSessionView(model: fixture.model, session: failed)), fixture: fixture, language: .zh)
        fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
        defer { fixture.navigation.detach(); window.close() }
        XCTAssertTrue(fixture.navigation.controlTargets(in: window).contains { $0.id == "phone.retry" }, "失败页有「重试」")
    }

    func testSettingsPagesHaveRoomForDescriptionAndControls() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let pages: [(String, AnyView)] = [
                ("settings", AnyView(EnvironmentView(model: fixture.model))),
                ("launch", AnyView(LaunchSettingsSheet(model: fixture.model, gameID: "fixture"))),
                ("input", AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")))
            ]
            for (name, page) in pages {
                let (host, window) = try await render(page, fixture: fixture, language: language)
                defer { window.close() }
                // 780 pt leaves >= 380 pt for prose beside a 220 pt control, card insets and page margins.
                XCTAssertGreaterThanOrEqual(host.bounds.width, 780, "\(name)/\(language.code): 设置页应有舒适的阅读宽度")
                try snapshot(host, "\(name)-\(language.code)")
            }
        }
    }

    func testLaunchChoicesAndActionsHaveTheSameTrailingEdge() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(LaunchSettingsSheet(model: fixture.model, gameID: "fixture")),
                                                fixture: fixture, language: language)
            defer { window.close() }
            let targets = fixture.navigation.controlTargets(in: window)
            let action = try XCTUnwrap(targets.first { $0.id == "launch.input" }, "手柄设置入口必须可操作")
            let choice = try XCTUnwrap(targets.first { $0.id == "launch.rumbleLevel" }, "震动强度必须可操作")
            XCTAssertEqual(choice.frame.maxX, action.frame.maxX, accuracy: 1,
                           "\(language.code): 下拉框不能越过同列按钮的右边界")
            XCTAssertEqual(choice.frame.width, action.frame.width, accuracy: 1,
                           "\(language.code): 同列下拉框与按钮等宽")
            let content = window.convertToScreen(host.bounds)
            XCTAssertLessThanOrEqual(choice.frame.maxX, content.maxX - 40, "控件与弹窗右缘保持安全边距")
        }
    }

    func testSettingsActionsShareOneColumnAndDropdownStaysInItsWindow() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let previous = UserDefaults.standard.object(forKey: "appLanguage")
        defer { UserDefaults.standard.set(previous, forKey: "appLanguage") }
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(EnvironmentView(model: fixture.model)), fixture: fixture, language: language)
            fixture.navigation.source.controllers = { [] }
            fixture.navigation.attach(window)
            defer { fixture.navigation.detach(); window.close() }
            let targets = fixture.navigation.controlTargets(in: window)
            let open = try XCTUnwrap(targets.first { $0.id == "settings.library.open" })
            let change = try XCTUnwrap(targets.first { $0.id == "settings.library.change" })
            let picker = try XCTUnwrap(targets.first { $0.id == "settings.language" })
            XCTAssertEqual(open.frame.height, 36, accuracy: 0.5)
            XCTAssertEqual(change.frame.height, 36, accuracy: 0.5)
            XCTAssertEqual(picker.frame.height, 36, accuracy: 0.5)
            XCTAssertEqual(change.frame.maxX, picker.frame.maxX, accuracy: 1)
            XCTAssertEqual(change.frame.maxX - open.frame.minX, 220, accuracy: 1,
                           "两个操作共用 220 pt，不能挤占说明区域")
            fixture.navigation.focus("settings.language"); fixture.navigation.handle(.confirm)
            try await Task.sleep(for: .milliseconds(100))
            let options = fixture.navigation.controlTargets(in: window).filter { $0.id.hasPrefix("dropdown.") }
            XCTAssertEqual(Set(options.map(\.id)), Set(([""] + Lang.all.map(\.code)).map { "dropdown." + $0 }),
                           "下拉列表必须在当前设置窗口内显示跟随系统与全部可用语言")
            let bounds = window.convertToScreen(host.bounds)
            for option in options { XCTAssertTrue(bounds.insetBy(dx: -1, dy: -1).contains(option.frame)) }
            fixture.navigation.focus("dropdown." + language.code); fixture.navigation.handle(.confirm)
            for _ in 0..<20 where fixture.navigation.controlTargets(in: window).contains(where: { $0.id.hasPrefix("dropdown.") }) {
                try await Task.sleep(for: .milliseconds(30))
            }
            XCTAssertFalse(fixture.navigation.controlTargets(in: window).contains { $0.id.hasPrefix("dropdown.") }, "选择后恢复页面操作")
        }
    }

    func testStoredTeamIdDoesNotPreventCollapsingTheGuide() async throws {
        let previous = UserDefaults.standard.object(forKey: S2ArmLauncherModel.teamCodeKey)
        UserDefaults.standard.set("fixture-team-id", forKey: S2ArmLauncherModel.teamCodeKey)
        defer { UserDefaults.standard.set(previous, forKey: S2ArmLauncherModel.teamCodeKey) }
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let page = AnyView(ScrollView { AppleTVXcodeTeam(model: fixture.model).padding(32) }.launcherSheet())   // Apple TV 用的 Xcode 团队（出包页 Apple TV 那一行下面）
        let (host, window) = try await render(page, fixture: fixture, language: .zh)
        fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
        defer { fixture.navigation.detach(); window.close() }
        XCTAssertTrue(fixture.navigation.controlTargets(in: window).contains { $0.id == "env.teamGuideOpen" })
        try snapshot(host, "signing-guide-expanded-zh")
        fixture.navigation.focus("env.teamManual"); fixture.navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertFalse(fixture.navigation.controlTargets(in: window).contains { $0.id == "env.teamGuideOpen" }, "保存过 Team ID 也应能收起引导")
        fixture.navigation.focus("env.teamManual"); fixture.navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertTrue(fixture.navigation.controlTargets(in: window).contains { $0.id == "env.teamGuideOpen" })
        XCTAssertEqual(UserDefaults.standard.string(forKey: S2ArmLauncherModel.teamCodeKey), "fixture-team-id")
    }

    func testKeyboardClearIsSeparateFromRecordAndSavesOnlyThatBinding() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let (_, window) = try await render(AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")), fixture: fixture, language: .en)
        fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
        defer { fixture.navigation.detach(); window.close() }
        let targets = fixture.navigation.controlTargets(in: window)
        let record = try XCTUnwrap(targets.first { $0.id == "input.keyboard.record" })
        let clear = try XCTUnwrap(targets.first { $0.id == "input.keyboard.clear" })
        XCTAssertEqual(record.frame.height, 36, accuracy: 1)
        XCTAssertEqual(clear.frame.width, 36, accuracy: 1)
        XCTAssertEqual(clear.frame.height, 36, accuracy: 1)
        XCTAssertLessThanOrEqual(record.frame.maxX, clear.frame.minX + 0.5, "清除与录入是两个独立点击区域")
        fixture.navigation.focus("input.keyboard.clear"); fixture.navigation.handle(.confirm)
        try await Task.sleep(for: .milliseconds(50))
        fixture.navigation.focus("input.save"); fixture.navigation.handle(.confirm)
        for _ in 0..<50 where !fixture.runner.requests.contains(where: { $0["operation"] as? String == "saveInput" }) {
            try await Task.sleep(for: .milliseconds(10))
        }
        let request = try XCTUnwrap(fixture.runner.requests.first { $0["operation"] as? String == "saveInput" })
        let keyboard = try XCTUnwrap((request["config"] as? [String: Any])?["keyboard"] as? [String: Any])
        XCTAssertNil(keyboard["A"], "清除 A 后保存的配置不能保留旧绑定")
        XCTAssertEqual((keyboard["B"] as? [String: Any])?["code"] as? Int, 56, "B 的 Shift 绑定保持原样")
    }

    func testSliderAdjustsWithControllersAndHonorsDisabledState() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        let value = LayoutSliderValue()
        for enabled in [true, false] {
            let preview = AnyView(LayoutSliderPreview(value: value, enabled: enabled).padding(32).launcherSheet(.compact, height: 160))
            let (_, window) = try await render(preview, fixture: fixture, language: .en)
            fixture.navigation.source.controllers = { [] }; fixture.navigation.attach(window)
            defer { fixture.navigation.detach(); window.close() }
            let target = fixture.navigation.controlTargets(in: window).first { $0.id == "preview.slider" }
            if enabled {
                XCTAssertEqual(try XCTUnwrap(target).frame.height, 36, accuracy: 1)
                fixture.navigation.focus("preview.slider"); fixture.navigation.handle(.right)
                try await Task.sleep(for: .milliseconds(30))
                XCTAssertEqual(value.value, 0.6, accuracy: 0.0001)
                fixture.navigation.handle(.left)
                try await Task.sleep(for: .milliseconds(30))
                XCTAssertEqual(value.value, 0.5, accuracy: 0.0001)
            } else {
                XCTAssertNil(target, "禁用滑块不能接收手柄调整")
                fixture.navigation.focus("preview.slider"); fixture.navigation.handle(.right)
                try await Task.sleep(for: .milliseconds(30))
                XCTAssertEqual(value.value, 0.5, accuracy: 0.0001)
            }
        }
    }

    func testInputAxisControlsFitTheSharedColumnAfterScrolling() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture")), fixture: fixture, language: language)
            defer { window.close() }
            let scroll = try XCTUnwrap(scrollViews(in: host).first { $0.hasVerticalScroller })
            let document = try XCTUnwrap(scroll.documentView)
            // 往下滚到四个摇杆的控件都露出来（#1987 起摇杆下面还有「管理键位」组，滚到底时摇杆已经出了视野）
            let bottom = max(0, document.bounds.height - scroll.contentView.bounds.height)
            var targets: [LauncherPadTarget] = []
            for y in stride(from: 0, through: bottom + 40, by: 40) {
                scroll.contentView.scroll(to: CGPoint(x: 0, y: min(y, bottom)))
                scroll.reflectScrolledClipView(scroll.contentView)
                host.layoutSubtreeIfNeeded()
                try await Task.sleep(for: .milliseconds(30))
                targets = fixture.navigation.controlTargets(in: window)
                if InputConfig.axisTargets.allSatisfy({ a in targets.contains { $0.id == "input.axis.source." + a } && targets.contains { $0.id == "input.axis.invert." + a } }) { break }
            }
            try await Task.sleep(for: .milliseconds(100))
            targets = fixture.navigation.controlTargets(in: window)
            for axis in InputConfig.axisTargets {
                let choice = try XCTUnwrap(targets.first { $0.id == "input.axis.source." + axis })
                let invert = try XCTUnwrap(targets.first { $0.id == "input.axis.invert." + axis })
                XCTAssertEqual(choice.frame.width, 124, accuracy: 1, "来源选择与反向开关必须共用 220 pt 操作列")
                XCTAssertEqual(choice.frame.maxX, window.convertToScreen(host.bounds).maxX - 52 - scrollerReserve(scroll), accuracy: 1)
                XCTAssertEqual(choice.frame.union(invert.frame).width, 172, accuracy: 1)
            }
            try snapshot(host, "input-axes-\(language.code)")
        }
    }

    /// 「现在更新并重启」时有局在跑（11-18 维护者）：弹窗里的确认块写清关掉哪款、给「取消 / 仍然重启」两个按钮，都能用手柄点到
    func testUpdateRestartConfirmShowsGameAndBothChoices() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let uiLanguage = language == .zh ? "zh-Hans" : "en"
            fixture.model.updatePopup = ["kind": "available", "version": "1.0.0", "notes": NSNull()]
            fixture.model.playSession = JobSession(kind: .play, gameID: "fixture", name: "Original Game")
            fixture.model.updateGate = "update.gate.confirm"
            let (host, window) = try await render(AnyView(UpdatePopupView(model: fixture.model, language: uiLanguage)), fixture: fixture, language: language)
            defer { window.close() }
            let ids = Set(fixture.navigation.controlTargets(in: window).map(\.id))
            XCTAssertTrue(ids.isSuperset(of: ["update.gate.cancel", "update.gate.restart"]), "确认块两个按钮都在：\(language.code)")
            XCTAssertEqual(fixture.model.updateRestartImpactText { BackendText.launcherText($0, language: uiLanguage) },
                           language == .zh ? "正在玩的「Original Game」会被关掉，没存的进度会丢；可以先在游戏里存一下再重启。"
                                           : "\"Original Game\" will be closed and unsaved progress will be lost — save in the game first if you can.")
            try snapshot(host, "update-restart-confirm-" + language.code)
            fixture.model.updateGate = nil
            fixture.model.playSession = nil
        }
    }

    func testAllUtilitySheetsRenderWithContainedControlsInBothLanguages() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        fixture.model.messages = [["id": "original-message", "text": ["zh-Hans": "原创界面检查消息", "en": "Original interface review message"]]]
        fixture.model.refresh()
        for _ in 0..<100 where fixture.model.games.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
        for language in [Lang.zh, .en] {
            let uiLanguage = language == .zh ? "zh-Hans" : "en"
            fixture.model.updatePopup = ["kind": "available", "version": "1.0.0", "notes": ["sections": [
                ["title": "UI", "items": [["zh-Hans": "统一设置界面", "en": "Consistent settings"]]]]]]
            let pages: [(String, AnyView)] = [
                ("settings", AnyView(EnvironmentView(model: fixture.model))),
                ("launch-settings", AnyView(LaunchSettingsSheet(model: fixture.model, gameID: "fixture"))),
                ("input-settings", AnyView(InputSettingsSheet(model: fixture.model, gameID: "fixture"))),
                ("downstream", AnyView(DownstreamConfirmSheet(model: fixture.model, gameID: "fixture", zh: language == .zh))),
                ("storage", AnyView(FreeSpaceView(model: fixture.model, gameID: "fixture"))),
                ("saves", AnyView(SaveManageSheet(model: fixture.model, gameID: "fixture"))),
                ("certificate", AnyView(DevCertSheet(model: fixture.model))),
                ("import-games", AnyView(ImportSheet(model: fixture.model, kind: .games))),
                ("import-keys", AnyView(ImportSheet(model: fixture.model, kind: .keys))),
                ("install", AnyView(InstallView(model: fixture.model, gameID: "fixture"))),
                ("update", AnyView(UpdatePopupView(model: fixture.model, language: uiLanguage))),
                ("messages", AnyView(MessagesPanel(model: fixture.model, language: uiLanguage))),
                ("diagnostics", AnyView(UploadDiagnosticsSheet(model: fixture.model, gameID: nil, operations: nil, fallback: "") {})),
                ("upgrade", AnyView(SGradeUpgradeBody(games: [], failure: nil, zh: language == .zh,
                    x: { BackendText.launcherText("sgrade.upgrade." + $0, language: uiLanguage) }, upgrade: { _ in }, close: {})))
            ]
            for (name, page) in pages {
                let (host, window) = try await render(page, fixture: fixture, language: language)
                for connected in [false, true] {
                    fixture.navigation.presentation.connected = connected
                    try await Task.sleep(for: .milliseconds(70))
                    let bounds = window.convertToScreen(host.bounds)
                    XCTAssertLessThanOrEqual(host.bounds.height, (NSScreen.main?.visibleFrame.height ?? 900) - 100,
                                             "\(name)/\(language.code): 弹层必须适合屏幕")
                    if name == "update" || name == "messages" {
                        let reading = try XCTUnwrap(scrollViews(in: host).first { $0.hasVerticalScroller })
                        XCTAssertGreaterThan(reading.contentView.bounds.height, 40, "\(name): 说明列表必须有可见空间")
                    }
                    for target in fixture.navigation.controlTargets(in: window) {
                        XCTAssertTrue(bounds.insetBy(dx: -1, dy: -1).contains(target.frame),
                                      "\(name)/\(language.code): 控件溢出 \(target.id) \(target.frame), \(bounds)")
                    }
                    if name != "messages" {
                        let shortcuts = fixture.navigation.shortcutTargets(in: window)
                        XCTAssertNil(shortcuts[.primary], "\(name)：确认键只操作焦点组件，提交动作应使用 R2")
                        if !["import-games", "import-keys"].contains(name) {
                            XCTAssertNotNil(shortcuts[.rightTrigger], "\(name)：确认／完成／启动操作必须登记 R2")
                        }
                        XCTAssertFalse(shortcuts.isEmpty, "\(name)/\(language.code)：弹窗底栏必须登记手柄快捷键")
                        for (key, ids) in shortcuts {
                            XCTAssertEqual(ids.count, 1, "\(name)/\(language.code)：同一按键不能同时触发多个底栏按钮：\(key) \(ids)")
                        }
                        if !["downstream", "import-games", "import-keys"].contains(name) {
                            XCTAssertNotNil(shortcuts[.back], "\(name)：必须能用 B 关闭或取消")
                        }
                    }
                    try snapshot(host, "\(name)-\(language.code)-\(connected ? "connected" : "disconnected")")
                }
                window.close()
            }
            for feature in [false, true] {
                fixture.model.updatePopup = feature
                    ? ["kind": "feature", "pages": [["title": ["zh-Hans": "原创说明", "en": "Original guide"]], ["title": ["zh-Hans": "第二页", "en": "Second page"]]]]
                    : ["kind": "afterUpdate", "version": "1.0.0", "notes": [:]]
                let page = feature ? AnyView(FeatureSpotlightView(model: fixture.model, language: uiLanguage))
                    : AnyView(UpdatePopupView(model: fixture.model, language: uiLanguage))
                let (host, window) = try await render(page, fixture: fixture, language: language)
                for connected in [false, true] {
                    fixture.navigation.presentation.connected = connected
                    try await Task.sleep(for: .milliseconds(70))
                    let keys = fixture.navigation.shortcutTargets(in: window)
                    XCTAssertEqual(keys[.rightTrigger]?.count, 1)
                    XCTAssertEqual(keys[.back]?.count, 1)
                    try snapshot(host, (feature ? "feature-" : "updated-") + language.code + (connected ? "-connected" : "-disconnected"))
                }
                window.close()
            }
        }
    }

    func testSettingsTabsExposeCategorizedControlsWithinBounds() async throws {
        let fixture = try SettingsFixture()
        defer { fixture.remove() }
        for language in [Lang.zh, .en] {
            let (host, window) = try await render(AnyView(EnvironmentView(model: fixture.model)), fixture: fixture, language: language)
            fixture.navigation.source.controllers = { [] }
            fixture.navigation.attach(window)
            defer { fixture.navigation.detach(); window.close() }
            let expected: [(tab: String, controls: [String])] = [
                ("settings.tab.general", ["settings.language", "settings.library.open", "settings.keys.import"]),
                ("settings.tab.iphone", ["export.sign.mode", "env.cloud.choose"]),
                ("settings.tab.environment", ["settings.tab.environment"]),
                ("settings.tab.updates", ["settings.updates.check", "env.aiControl"]),
            ]
            for entry in expected {
                fixture.navigation.focus(entry.tab); fixture.navigation.handle(.confirm)
                try await Task.sleep(for: .milliseconds(80))
                let bounds = window.convertToScreen(host.bounds)
                for id in entry.controls {
                    fixture.navigation.focus(id)
                    try await Task.sleep(for: .milliseconds(50))
                    let targets = fixture.navigation.controlTargets(in: window)
                    XCTAssertTrue(targets.contains { $0.id == id }, "\(language.code)/\(entry.tab): 缺少控件 \(id)")
                }
                for target in fixture.navigation.controlTargets(in: window) {
                    XCTAssertTrue(bounds.insetBy(dx: -1, dy: -1).contains(target.frame),
                                  "\(language.code)/\(entry.tab): 控件越界 \(target.id) \(target.frame) vs \(bounds)")
                }
            }
        }
    }

    /// 设置页这类弹窗在外窗口里垂直居中：靠 LauncherSheetSize.maxWindowHeight 把弹窗高度压在「父窗口高 − 136」以内（AppKit beginSheet 只在这个高度内居中）。
    ///   单测不上屏，SwiftUI 的 sheet 不会真挂到父窗口（11-26），这里核规则本身；期望手算：630 − 136 = 494；400 − 136 = 264 低于下限取 360；没父窗口按屏幕 900 − 100 = 800
    func testSettingsSheetCentersVerticallyInsideParentWindow() {
        XCTAssertEqual(LauncherSheetSize.maxWindowHeight(parent: 630, screen: 900), 494)
        XCTAssertEqual(LauncherSheetSize.maxWindowHeight(parent: 400, screen: 900), 360)
        XCTAssertEqual(LauncherSheetSize.maxWindowHeight(parent: nil, screen: 900), 800)
    }

    private func exportArtworkBounds(_ host: NSView) throws -> CGRect {
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        let scale = CGFloat(bitmap.pixelsWide) / host.bounds.width
        var xs: [Int] = [], ys: [Int] = []
        for y in 0..<400 {
            for x in 0..<600 {
                guard let color = bitmap.colorAt(x: Int(CGFloat(x) * scale), y: Int(CGFloat(y) * scale))?.usingColorSpace(.deviceRGB),
                      color.redComponent - color.greenComponent > 0.5, color.blueComponent - color.greenComponent > 0.5 else { continue }
                xs.append(x); ys.append(y)
            }
        }
        let left = try XCTUnwrap(xs.min()), top = try XCTUnwrap(ys.min())
        return CGRect(x: left, y: top, width: try XCTUnwrap(xs.max()) - left + 1, height: try XCTUnwrap(ys.max()) - top + 1)
    }

    /// 窗口不上屏（UIStage）：结果不随桌面焦点 / 前台程序变
    private func render(_ page: AnyView, fixture: SettingsFixture, language: Lang) async throws -> (NSView, NSWindow) {
        _ = NSApplication.shared
        let host = NSHostingView(rootView: page.environmentObject(Localizer(language))
            .environment(\.padControls, fixture.navigation).environment(\.colorScheme, .dark))
        host.appearance = NSAppearance(named: .darkAqua)
        let window = NSWindow(contentRect: CGRect(origin: CGPoint(x: 80, y: 80), size: host.fittingSize),
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        UIStage.show(window)
        try await settle(window, fixture.navigation, fixture.model.gamepad)
        return (host, window)
    }

    /// 等页面布局稳定再量：手柄可操作的控件已登记、位置连续 3 次采样（约 90 ms）不变；一个控件都没有的页面，空着连续约 300 ms 也算稳。
    /// 设置类页面的控件登记在 fixture.navigation，首页（LauncherView）登记在 model.gamepad，两张表都看。
    /// 固定等 250 ms 在整套 / 门禁高负载下会量到还没登记的控件（XCTUnwrap 落空）或还在挪的位置（点歪、焦点框差几 pt）。最多等 5 秒，超时交给后面的断言报真实状态
    private func settle(_ window: NSWindow, _ registries: LauncherPadNavigation...) async throws {
        var last = "", same = 0
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline {
            window.contentView?.layoutSubtreeIfNeeded()
            let now = registries.flatMap { $0.controlTargets(in: window) }.map { "\($0.id)@\($0.frame)" }.joined(separator: ";")
            same = now == last ? same + 1 : 0
            if same >= (now.isEmpty ? 10 : 3) { return }
            last = now
            try await Task.sleep(for: .milliseconds(30))
        }
    }

    /// 常显滚动条占掉的宽度（浮层滚动条为 0）。
    private func scrollerReserve(_ scroll: NSScrollView) -> CGFloat {
        scroll.bounds.width - scroll.contentView.frame.width
    }

    private func scrollViews(in view: NSView) -> [NSScrollView] {
        if let scroll = view as? NSScrollView { return [scroll] }
        return view.subviews.flatMap { scrollViews(in: $0) }
    }

    private func normalizedText(_ text: String) -> String { text.filter { !$0.isWhitespace }.lowercased() }

    /// SwiftUI 的滚动内容没有完整暴露静态 AX 文本，用实际字形核对文案与对齐。
    private func renderedText(_ host: NSView, language: Lang) throws -> [(text: String, candidate: VNRecognizedText)] {
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.recognitionLanguages = language == .zh ? ["zh-Hans", "en-US"] : ["en-US"]
        try VNImageRequestHandler(cgImage: XCTUnwrap(bitmap.cgImage)).perform([request])
        return try (request.results ?? []).map { observation in
            let candidate = try XCTUnwrap(observation.topCandidates(1).first)
            return (candidate.string, candidate)
        }
    }

    private func snapshot(_ host: NSView, _ name: String) throws {
        guard let output = ProcessInfo.processInfo.environment["S2A_SETTINGS_SHOTS"] else { return }
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
            .write(to: URL(fileURLWithPath: output).appendingPathComponent(name + ".png"))
    }
}

@MainActor private final class SearchControlState: ObservableObject {
    @Published var expanded = false
    @Published var query = ""
    var outsideClicks = 0
}

private struct SearchControlPreview: View {
    @ObservedObject var state: SearchControlState
    var body: some View {
        HStack {
            Spacer()
            Button("Outside") { state.outsideClicks += 1 }
                .controllerButtonStyle(.plain).controllerID("search-test.outside")
            HomeSearchControl(isExpanded: $state.expanded, text: $state.query, width: 300, edit: { finished in finished() })
        }
        .padding(30).frame(width: 600, height: 120).background(Color.black)
    }
}

@MainActor private final class SettingsFixture {
    let root: URL
    let model: S2ArmLauncherModel
    let navigation = LauncherPadNavigation()
    let runner = SettingsRunner()
    /// 模型想弹的提示框（标题：内容）：走记录不走真 NSAlert（runAlert 注入）
    private(set) var alerts: [String] = []
    init() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("s2arm-settings-layout-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("Jobs"), withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("library"), withIntermediateDirectories: true)
        try Data().write(to: root.appendingPathComponent("Jobs/main.py"))
        model = S2ArmLauncherModel(client: JobClient(environment: JobEnvironment(privateRoot: root, resources: root,
            library: root.appendingPathComponent("library").path, libraryId: "layout-fixture"), runner: runner))
        quiet(model)
    }
    /// 这个夹具里建的模型（含重启后新建的）一律接上：提示框记下来，不弹真窗
    func quiet(_ m: S2ArmLauncherModel) { m.runAlert = { [weak self] a in self?.alerts.append(a.messageText + "：" + a.informativeText); return .alertSecondButtonReturn } }
    func remove() { try? FileManager.default.removeItem(at: root) }
}


private final class SettingsRunner: JobProcessRunner {
    final class Handle: JobProcessHandle { func terminate() {} }
    private let lock = NSLock()
    private var captured: [[String: Any]] = []
    private var gameCount = 1
    private var needsPreparation = false
    private var cover = ""
    private var storage: [String: Any] = [:]
    private var checks: [[String: Any]] = [["id": "xcode", "name": "Xcode", "ok": true, "group": "apple", "requiredFor": ["prepare", "launch", "ipa"]]]
    var preflightChecks: [[String: Any]] {
        get { lock.withLock { checks } }
        set { lock.withLock { checks = newValue } }
    }
    var storageResponse: [String: Any] {
        get { lock.withLock { storage } }
        set { lock.withLock { storage = newValue } }
    }
    var catalogNeedsPreparation: Bool {
        get { lock.withLock { needsPreparation } }
        set { lock.withLock { needsPreparation = newValue } }
    }
    var catalogCover: String {
        get { lock.withLock { cover } }
        set { lock.withLock { cover = newValue } }
    }
    var catalogGameCount: Int {
        get { lock.withLock { gameCount } }
        set { lock.withLock { gameCount = newValue } }
    }
    private var downstreamChoice: String? = nil
    /// 夹具款上次选的下游方案（nil = 没选过；"" = 仍用原版）
    var catalogDownstreamChoice: String? {
        get { lock.withLock { downstreamChoice } }
        set { lock.withLock { downstreamChoice = newValue } }
    }
    private var playtime: [String: Any]? = nil
    var catalogPlaytime: [String: Any]? {
        get { lock.withLock { playtime } }
        set { lock.withLock { playtime = newValue } }
    }
    private var trouble = ""
    private var swapped = false
    /// loadInput 回的四位玩家（#2081）：缺省四位都是标准键位（config = null），要测回落的用例自己设
    private var inputRows: [[String: Any]] = (1...4).map { ["player": $0, "config": NSNull(), "source": "standard", "problem": false] }
    var loadInputPlayers: [[String: Any]] {
        get { lock.withLock { inputRows } }
        set { lock.withLock { inputRows = newValue } }
    }
    /// loadInput 回的「所有游戏的默认」（#1987）：nil = 不带这个字段（旧作业层）；NSNull = 没设过
    private var inputDefault: Any? = nil
    var loadInputDefault: Any? {
        get { lock.withLock { inputDefault } }
        set { lock.withLock { inputDefault = newValue } }
    }
    /// loadInput 回的 defaultProblem（默认文件读不出或不合规）：nil = 不带这个字段（旧作业层）
    private var inputDefaultProblem: Bool? = nil
    var loadInputDefaultProblem: Bool? {
        get { lock.withLock { inputDefaultProblem } }
        set { lock.withLock { inputDefaultProblem = newValue } }
    }
    private var follows = false
    var saveInputFollows: Bool {
        get { lock.withLock { follows } }
        set { lock.withLock { follows = newValue } }
    }
    /// 回到上一份存档：savePrevious 回的 trouble；saveRestorePrevious 每次把「已换到上一份」翻转一次（作业层的交换是可逆的）
    var previousTrouble: String {
        get { lock.withLock { trouble } }
        set { lock.withLock { trouble = newValue } }
    }
    var previousSwapped: Bool { lock.withLock { swapped } }
    var requests: [[String: Any]] { lock.withLock { captured } }
    func start(executable: URL, arguments: [String], environment: [String: String], onLine: @escaping (Data) -> Void,
               onStderr: @escaping (Data) -> Void, onExit: @escaping (Int32) -> Void) throws -> JobProcessHandle {
        objc_sync_enter(self); defer { objc_sync_exit(self) }   // 作业客户端在并发线程池里同时调 start：替身状态串行改（无锁并发追加数组会 SIGSEGV 带崩整个测试进程）
        let request = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: arguments.last!))) as! [String: Any]
        lock.withLock { captured.append(request) }
        var result: [String: Any] = [:]
        if request["operation"] as? String == "locations" { result = storageResponse }
        if request["operation"] as? String == "catalog" {
            let copy: [String: Any] = [
                "zh": ["title": "选择运行方案", "body": "以下为原创布局说明，用于验证长内容滚动", "points": (1...24).map { "原创说明第 \($0) 项：此处展示较长的方案说明，以检查窗口与底部操作的边界" },
                       "recommend": "使用推荐方案", "original": "使用原版方案"],
                "en": ["title": "Choose a configuration", "body": "Original layout fixture for long content", "points": (1...24).map { "Original item \($0): a longer configuration description to verify scrolling and keep the actions visible" },
                       "recommend": "Use recommended", "original": "Use original"]]
            result = ["keys": ["ready": false], "games": (0..<catalogGameCount).map { index in
                var game: [String: Any] = ["id": index == 0 ? "fixture" : "fixture-\(index)", "name": "Original UI fixture \(index)", "cover": catalogCover, "ready": true, "needsPreparation": catalogNeedsPreparation, "missing": [],
                 "downstream": ["id": "original-demo", "name": "Original Demo", "eligible": true, "copy": copy]]
                if index == 0, let playtime = catalogPlaytime { game["playtime"] = playtime }
                if let choice = catalogDownstreamChoice { game["downstream"] = ["id": "original-demo", "name": "Original Demo", "eligible": true, "copy": copy, "choice": choice] }
                return game
            }]
        }
        if request["operation"] as? String == "import" { result = ["imports": [["id": "fixture"]]] }
        if request["operation"] as? String == "community" {
            var options: [String: Any] = [:]
            for item in try GameSettingsSpec.macItems() {
                options[item.id] = ["available": true, "hidden": item.group != "controls",
                                    "value": item.type == "choice" ? (item.options.first?.value ?? "") : true]
            }
            result = ["mac": options, "graphics": [], "cheats": ["items": []]]
        }
        if request["operation"] as? String == "installMode" { result = ["mode": "device"] }
        if request["operation"] as? String == "installCheck" {
            result = ["ready": true, "items": [["id": "appleId", "title": "Apple ID", "detail": "还没用 Apple ID 登录", "ok": false, "action": "appleLogin"]]]
        }
        if request["operation"] as? String == "preflight" {
            result = ["checks": preflightChecks]
        }
        if request["operation"] as? String == "loadInput" {   // #2081：缺省四位标准键位；#1987：设了 loadInputDefault 才带 default
            result = ["players": inputRows]
            if let dflt = loadInputDefault { result["default"] = dflt }
            if let problem = loadInputDefaultProblem { result["defaultProblem"] = problem }
        }
        if request["operation"] as? String == "saveInput" { result = ["followsDefault": saveInputFollows] }
        if request["operation"] as? String == "savePrevious" { result = ["hasPrevious": true, "saveIds": ["0000000000000001"], "trouble": previousTrouble] }
        if request["operation"] as? String == "saveRestorePrevious" { lock.withLock { swapped.toggle() }; result = ["restored": 1] }
        if request["operation"] as? String == "saveStatus" {
            result = ["hasSave": true, "fileCount": 3, "totalBytes": 20480]
        }
        if request["operation"] as? String == "spaceSurvey" {
            result = ["games": [["gameId": "fixture", "name": "Original UI fixture", "parts": [
                ["id": "shaders", "title": "着色器缓存", "rebuild": "下次启动时重新生成", "bytes": 1048576]],
                "kept": [["id": "saves", "bytes": 20480]]]]]
        }
        let failed = request["operation"] as? String == "signingProject"
        let event: [String: Any] = ["schema": 1, "taskId": request["taskId"] ?? "", "type": "result",
                                  "state": failed ? "failed" : "succeeded", "result": result,
                                  "message": failed ? "原创测试：引导项目暂不可用" : "", "code": failed ? "tool_missing" : ""]
        let data = try JSONSerialization.data(withJSONObject: event)
        DispatchQueue.main.async { onLine(data); onExit(0) }
        return Handle()
    }
}

@MainActor private final class LayoutSliderValue: ObservableObject {
    @Published var value = 0.5
}
private struct LayoutSliderPreview: View {
    @ObservedObject var value: LayoutSliderValue
    let enabled: Bool
    var body: some View {
        LauncherSlider(value: value.value, range: 0...1, step: 0.1) { value.value = $0 }
            .frame(width: 220).controllerID("preview.slider").disabled(!enabled)
    }
}
