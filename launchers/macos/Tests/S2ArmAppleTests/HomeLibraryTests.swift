// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI
import XCTest
import LauncherPadBridge
@testable import NX2AppleUI
@testable import S2ArmAppleAdapter

@MainActor final class HomeLibraryTests: XCTestCase {
    // 免费签名到期提醒（11-20）：剩余天数按作业层给的 expires 算；关掉设置开关 → 不再显示；设置里重新打开 → 恢复；开关存进偏好、重开启动器还在
    func testFreeSignReminderDismissRestorePersist() throws {
        let suite = "s2arm-free-sign-" + UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite)); defer { defaults.removePersistentDomain(forName: suite) }
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(suite); defer { try? FileManager.default.removeItem(at: root) }
        let make = { S2ArmLauncherModel(client: JobClient(environment: JobEnvironment(privateRoot: root, resources: root)), updateDefaults: defaults) }
        let now = Date(timeIntervalSince1970: 1_000_000), day = 86400.0
        // 6 天前装、包里读到的真实到期在 2.5 天后：显示 3 天（不是按装机时刻 +7 天的 1 天）
        let game = try XCTUnwrap(GameSummary(catalog: ["id": "0100000000010000", "name": "Alpha",
            "iphoneInstall": ["at": now.timeIntervalSince1970 - 6 * day, "free": true, "expires": now.timeIntervalSince1970 + 2.5 * day]]))
        let other = try XCTUnwrap(GameSummary(catalog: ["id": "0100000000020000", "name": "Beta",
            "iphoneInstall": ["at": now.timeIntervalSince1970 - 6 * day, "free": false]]))   // 作业层判出不是启动器免费签的那份
        let model = make()
        XCTAssertTrue(model.freeSignReminder, "没碰过开关：缺省开")
        XCTAssertEqual(model.freeSignDaysLeft(game, now: now), 3)
        XCTAssertNil(model.freeSignDaysLeft(other, now: now))
        model.freeSignReminder = false
        XCTAssertNil(model.freeSignDaysLeft(game, now: now), "擦掉后不显示")
        let reopened = make()
        XCTAssertFalse(reopened.freeSignReminder, "重开启动器：仍是关")
        XCTAssertNil(reopened.freeSignDaysLeft(game, now: now))
        reopened.freeSignReminder = true   // 设置里重新打开
        XCTAssertEqual(make().freeSignDaysLeft(game, now: now), 3, "重新打开后恢复显示，且存下来")
    }

    // #1962 复测：推到 iPad 后游戏库状态写「已推到 iPad」、导出页等待提示写「等 iPad 上的 S2Arm…」（期望手写）
    func testInstalledStatusNamesDeviceKind() throws {
        let ipad = try XCTUnwrap(GameSummary(catalog: ["id": "0100000000010000", "name": "Alpha",
            "iphoneInstall": ["at": 1.0, "free": false, "deviceKey": "U1", "deviceKind": "iPad"]]))
        let old = try XCTUnwrap(GameSummary(catalog: ["id": "0100000000020000", "name": "Beta",
            "iphoneInstall": ["at": 1.0, "free": false, "deviceKey": "U1"]]))   // 0.6.9 前的记录没有 deviceKind
        XCTAssertEqual(BackendText.launcherText("library.status.installedPaid", language: "zh-Hans")
            .replacingOccurrences(of: "{0}", with: BackendText.deviceKind(ipad.iphoneInstallKind)), "已推到 iPad")
        XCTAssertEqual(BackendText.launcherText("library.status.installedPaid", language: "zh-Hans")
            .replacingOccurrences(of: "{0}", with: BackendText.deviceKind(old.iphoneInstallKind)), "已推到 iPhone / iPad")
        XCTAssertEqual(BackendText.translate("等 iPad 上的 S2Arm 装好这款的程序", language: "en"), "Waiting for S2Arm on the iPad to install this game's program")
        XCTAssertEqual(BackendText.translate("把证书交给 iPhone / iPad 上的 S2Arm", language: "en"), "Handing the certificate to S2Arm on your iPhone / iPad")
    }

    // 选中款的截图覆盖已过期签名和可游玩的 Mac 版。
    func testHomeSelectedGameWithExpiredDeviceSignature() async throws {
        _ = NSApplication.shared
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("s2arm-home-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("Jobs"), withIntermediateDirectories: true)
        try Data().write(to: root.appendingPathComponent("Jobs/main.py"))
        defer { try? FileManager.default.removeItem(at: root) }
        try Self.writeArtwork(to: root)
        let now = Date().timeIntervalSince1970, day = 86400.0
        // 第 0 款：6.5 天前装 → 还剩 1 天；第 1 款：8 天前 → 已过期；第 2 款：1 天前 → 还剩 6 天
        let runner = HomeCatalogRunner(artwork: root, installs: [0: now - 6.5 * day, 1: now - 8 * day, 2: now - day], skipped: [2, 3])
        let model = S2ArmLauncherModel(client: JobClient(environment: JobEnvironment(privateRoot: root, resources: root,
            library: root.appendingPathComponent("lib").path, libraryId: "fixture"), runner: runner))
        model.refresh()
        for _ in 0..<100 where model.games.count != 21 { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(model.games.first { $0.id == String(format: "%016X", 0) }?.iphoneDaysLeft(), 1)
        XCTAssertEqual(model.games.first { $0.id == String(format: "%016X", 1) }?.iphoneDaysLeft(), 0)
        let host = NSHostingView(rootView: S2ArmAppleRootView(model: model, brand: "S2Arm × NX2Apple"))
        let window = NSWindow(contentRect: NSRect(x: 80, y: 80, width: 1120, height: 720), styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        UIStage.show(window)
        model.selectedID = String(format: "%016X", 1)
        XCTAssertEqual(model.games.first { $0.id == String(format: "%016X", 2) }?.iphoneDaysLeft(), 6)
        XCTAssertNil(model.games.first { $0.id == String(format: "%016X", 3) }?.iphoneDaysLeft(), "没装过的款没有到期提示")
        // 数据校验不过的款（11-17）：第 2、3 款标，其余不标；角标样子同看 home-cards.png（右下角黄色三角）
        XCTAssertEqual(model.games.filter(\.integritySkipped).map(\.id), [2, 3].map { String(format: "%016X", $0) })
        for _ in 0..<20 { host.layoutSubtreeIfNeeded(); try await Task.sleep(for: .milliseconds(30)) }
        if let output = ProcessInfo.processInfo.environment["S2A_SNAPSHOT_OUT"] {
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: output).appendingPathComponent("home-cards.png"))
        }
        window.close()
    }

    func testHomeExposesHorizontalCoversAboveActions() async throws {
        _ = NSApplication.shared
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("s2arm-home-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("Jobs"), withIntermediateDirectories: true)
        try Data().write(to: root.appendingPathComponent("Jobs/main.py"))
        defer { try? FileManager.default.removeItem(at: root) }
        try Self.writeArtwork(to: root)
        let runner = HomeCatalogRunner(artwork: root)
        let model = S2ArmLauncherModel(client: JobClient(environment: JobEnvironment(privateRoot: root, resources: root,
            library: root.appendingPathComponent("lib").path, libraryId: "fixture"), runner: runner))
        model.refresh()
        for _ in 0..<100 where model.games.count != 21 { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(model.games.count, 21)
        let host = NSHostingView(rootView: S2ArmAppleRootView(model: model, brand: "S2Arm × NX2Apple"))
        let window = NSWindow(contentRect: NSRect(x: 80, y: 80, width: 1440, height: 900), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        UIStage.show(window)
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(300))
        host.layoutSubtreeIfNeeded()
        let targets = model.gamepad.controlTargets(in: window)
        XCTAssertTrue(targets.contains { $0.id == "library.sgradeUpgrade" }, "合并主线后首页仍须提供可用手柄操作的 S 级优化升级入口")
        // 手机在装 / 更新（model.busy）时入口照样可用：S 级升级在作业层排队、不阻断玩家干别的（置灰的按钮不登记手柄目标）。
        //   游戏在跑时主区域是运行页、首页不显示（玩家收起运行页后才回首页），所以这里用装单款这类不占整页的装机作业
        model.phoneSession = JobSession(kind: .install, gameID: "0000000000000001", name: "Fixture")
        XCTAssertTrue(model.busy)
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(200))
        host.layoutSubtreeIfNeeded()
        XCTAssertTrue(model.gamepad.controlTargets(in: window).contains { $0.id == "library.sgradeUpgrade" }, "装机作业在跑时 S 级优化升级入口不能置灰")
        model.phoneSession = nil
        let cards = targets.filter { $0.id.hasPrefix("card.") }
        XCTAssertGreaterThanOrEqual(cards.count, 5, "Home 首屏必须能浏览一排游戏")
        let firstCard = try XCTUnwrap(cards.min { $0.frame.minX < $1.frame.minX })
        let play = try XCTUnwrap(targets.first { $0.id == "hero.play" })
        XCTAssertLessThan(play.frame.maxY, cards.map(\.frame.minY).min()!, "游戏操作必须放在顶部封面排下方")
        XCTAssertLessThan(cards.map(\.frame.maxY).max()! - cards.map(\.frame.maxY).min()!, 2, "全部封面必须在同一横排顶对齐")
        let secondCard = try XCTUnwrap(cards.first { $0.id == "card." + String(format: "%016X", 1) })
        XCTAssertGreaterThan(firstCard.frame.width, secondCard.frame.width * 1.2, "选中封面必须明显放大")
        XCTAssertFalse(targets.contains { $0.id == "top.library" }, "回库由各页返回和欢迎页入口提供，标题栏不重复放置")
        for id in ["top.settings", "top.report", "top.controller", "top.messages", "top.player"] {
            let target = try XCTUnwrap(targets.first { $0.id == id }, id)
            XCTAssertLessThanOrEqual(target.frame.width, 44, "顶栏操作尽量图标化：\(id)")
            XCTAssertGreaterThanOrEqual(target.frame.height, 32)
        }
        let firstID = String(format: "%016X", 0)
        let firstPixel = try await waitPixel(host, x: 0.88, y: 0.5) { $0.redComponent > $0.blueComponent + 0.15 }
        XCTAssertGreaterThan(firstPixel.redComponent, firstPixel.blueComponent + 0.15, "第一款红色 banner 必须铺到窗口右半侧（没自定义过封面时用横幅）")
        model.gamepad.focus("card." + firstID)
        model.gamepad.handle(.left)
        XCTAssertEqual(model.selectedID, firstID, "第一款向左不能越界或跳进其他操作")
        model.gamepad.handle(.right)
        let secondID = String(format: "%016X", 1)
        for _ in 0..<300 where model.selectedID != secondID { try await Task.sleep(for: .milliseconds(10)) }   // 先等选中真的换到第二款
        XCTAssertEqual(model.selectedID, secondID, "右键选择下一款游戏")
        let secondPixel = try await waitPixel(host, x: 0.88, y: 0.5) { $0.blueComponent > $0.redComponent + 0.15 }   // 再等背景换图的动画走完
        XCTAssertGreaterThan(secondPixel.blueComponent, secondPixel.redComponent + 0.15, "选择第二款后全屏背景必须换成蓝色 banner")
        model.gamepad.handle(.down)
        XCTAssertEqual(model.gamepad.focusedID, "hero.play", "封面向下进入游戏操作")
        model.gamepad.handle(.up)
        XCTAssertEqual(model.gamepad.focusedID, "card." + String(format: "%016X", 1), "操作区向上回到选中游戏")
        model.gamepad.handle(.up)
        XCTAssertEqual(model.gamepad.focusedID, "home.search", "封面向上进入搜索")
        model.gamepad.handle(.down)
        XCTAssertEqual(model.gamepad.focusedID, "card." + String(format: "%016X", 1))
        for _ in 0..<19 { model.gamepad.handle(.right); try await Task.sleep(for: .milliseconds(80)) }
        let lastCard = "card." + String(format: "%016X", 20)
        for _ in 0..<100 where !model.gamepad.isFullyVisible(lastCard) {
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertEqual(model.selectedID, String(format: "%016X", 20), "能横向滚动到最后一款游戏")
        XCTAssertTrue(model.gamepad.controlTargets(in: window).contains { $0.id == "card." + String(format: "%016X", 20) }, "屏外目标要实际滚入视口")
        model.gamepad.handle(.right)
        XCTAssertEqual(model.selectedID, String(format: "%016X", 20), "最后一款向右不能越界")
        model.gamepad.handle(.left)
        try await Task.sleep(for: .milliseconds(180))
        XCTAssertEqual(model.selectedID, String(format: "%016X", 19))
        if let output = ProcessInfo.processInfo.environment["S2A_SNAPSHOT_OUT"] {
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: URL(fileURLWithPath: output).appendingPathComponent("home-console.png"))
        }
        for size in [CGSize(width: 1120, height: 630), CGSize(width: 1000, height: 720), CGSize(width: 1920, height: 1080)] {
            window.setContentSize(size)
            host.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(300))
            let resized = model.gamepad.controlTargets(in: window)
            XCTAssertLessThanOrEqual(host.fittingSize.height, size.height + 0.1, "内容最小高度不能撑大默认窗口")
            let screenBounds = window.convertToScreen(host.bounds)
            for id in ["hero.play", "hero.exportSheet", "hero.more", "hero.favorite", "home.search"] {
                let target = try XCTUnwrap(resized.first { $0.id == id }, "\(size): \(id)")
                XCTAssertTrue(screenBounds.insetBy(dx: -1, dy: -1).contains(target.frame), "\(size) 的操作必须留在窗口内：\(id) \(target.frame)")
            }
            // Toolbar buttons live in a separate AppKit hosting view, outside the page host's bounds.
            let settings = try XCTUnwrap(resized.first { $0.id == "top.settings" })
            XCTAssertTrue(window.frame.contains(settings.frame), "原生标题栏按钮必须位于窗口内")
        }
        model.home.search = String(format: "%016X", 19)
        try await Task.sleep(for: .milliseconds(180))
        XCTAssertEqual(model.gamepad.controlTargets(in: window).filter { $0.id.hasPrefix("card.") }.count, 1, "搜索到单款时保留横排选择")
        model.home.search = "no-such-original-fixture"
        try await Task.sleep(for: .milliseconds(180))
        let empty = model.gamepad.controlTargets(in: window)
        XCTAssertTrue(empty.contains { $0.id == "home.showAll" }, "无匹配项时必须能恢复全部游戏")
        XCTAssertFalse(empty.contains { $0.id.hasPrefix("hero.") }, "没有匹配项时不能操作上一次选中的游戏")
        model.home.search = ""
        try await Task.sleep(for: .milliseconds(180))
        model.gamepad.focus("card." + String(format: "%016X", 19))
        model.gamepad.handle(.confirm)
        XCTAssertNil(runner.lastLaunch, "A 只选中当前游戏，不能同时启动")
        model.gamepad.handle(.submit)
        for _ in 0..<50 where runner.lastLaunch == nil { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertEqual(runner.lastLaunch, String(format: "%016X", 19), "R2 必须启动当前选中的游戏")
        // 先等 R2 起的那一局收完尾再换夹具（11-20 连跑偶发红）：假执行器的结局是 main.async 回的，负载高时晚到，
        //   会落在下面换上的夹具局上、把它标成已结束，goToLibrary 就把它清掉而不是收起。按状态等，不按时间等
        for _ in 0..<300 where model.playBusy { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertFalse(model.playBusy, "R2 起的那一局要先收尾")
        model.playSession = JobSession(kind: .play, gameID: firstID, name: "Fixture")   // 一局在游戏槽
        model.gamepad.handle(.back)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertTrue(model.playHidden, "B 返回 Home 时运行中的一局留在后台")
        XCTAssertTrue(model.gamepad.controlTargets(in: window).contains { $0.id == "top.playTask" }, "后台的一局必须有重新进入的入口：\(model.gamepad.controlTargets(in: window).map(\.id))")
        model.gamepad.focus("top.playTask"); model.gamepad.handle(.confirm)
        XCTAssertFalse(model.playHidden)
        model.goToLibrary()
        XCTAssertTrue(model.playHidden)
        model.playSession = JobSession(kind: .play, gameID: firstID, name: "Next fixture")
        XCTAssertFalse(model.playHidden, "开始新的一局时不能继承上一局的隐藏状态")
        window.close()
    }

    func testBackdropUsesCoverWhenBannerIsMissingOrInvalid() async throws {
        _ = NSApplication.shared
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("s2arm-backdrop-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try Self.writeArtwork(to: root)
        try Data("invalid image".utf8).write(to: root.appendingPathComponent("broken.png"))
        let covers = CoverCache()
        for banner in [nil, root.appendingPathComponent("broken.png").path] as [String?] {
            var game = GameSummary(id: "fixture", name: "Original", cover: root.appendingPathComponent("cover.png").path)
            game.banner = banner
            let host = NSHostingView(rootView: HomeBackdrop(game: game, covers: covers))
            let window = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 1000, height: 720), styleMask: [.borderless], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
            host.layoutSubtreeIfNeeded()
            let color = try await waitPixel(host, x: 0.88, y: 0.5) { $0.greenComponent > $0.redComponent + 0.15 }
            XCTAssertGreaterThan(color.greenComponent, color.redComponent + 0.15, "banner 缺失或无法解码时仍显示有效封面")
            window.close()
        }
        // 玩家自己换过封面：大图用玩家的封面（绿），不用自带横幅（红）——11-15 维护者「玩家自行选择优先级更高」
        var custom = GameSummary(id: "fixture", name: "Original", cover: root.appendingPathComponent("cover.png").path, customCover: true)
        custom.banner = root.appendingPathComponent("banner-red.png").path
        let host = NSHostingView(rootView: HomeBackdrop(game: custom, covers: covers))
        let window = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 1000, height: 720), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
        host.layoutSubtreeIfNeeded()
        let color = try await waitPixel(host, x: 0.88, y: 0.5) { $0.greenComponent > $0.redComponent + 0.15 }
        XCTAssertGreaterThan(color.greenComponent, color.redComponent + 0.15, "自定义封面优先于自带横幅")
        window.close()
    }

    /// 取像素前等画面真的到位：每 30 毫秒重排、重画、取一次，满足条件就返回，最多等 timeout 秒（到点返回最后一次，交给断言报红）。
    ///   原先固定等 80 / 400 毫秒再取，门禁主机负载高时换图动画、图片解码没走完，偶发报红（11-17 bab00cb5c 上那次）。
    ///   上限 20 秒：背景图在后台队列读盘解码（CoverCache），本机负载 82 时等满 5 秒仍没换上（11-17 05:34 那次读数红 0.19 / 蓝 0.05）；
    ///   满足条件就返回，平时不多等，判据不变
    private func waitPixel(_ host: NSView, x: CGFloat, y: CGFloat, timeout: TimeInterval = 20, until ok: (NSColor) -> Bool) async throws -> NSColor {
        let deadline = Date().addingTimeInterval(timeout)
        var color = try pixel(host, x: x, y: y)
        while !ok(color), Date() < deadline {
            try await Task.sleep(for: .milliseconds(30))
            host.layoutSubtreeIfNeeded()
            color = try pixel(host, x: x, y: y)
        }
        return color
    }

    private func pixel(_ host: NSView, x: CGFloat, y: CGFloat) throws -> NSColor {
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        return try XCTUnwrap(bitmap.colorAt(x: Int(CGFloat(bitmap.pixelsWide) * x), y: Int(CGFloat(bitmap.pixelsHigh) * y))?.usingColorSpace(.deviceRGB))
    }

    /// Original synthetic artwork only; no game resources or screenshots enter the repository.
    private static func writeArtwork(to root: URL) throws {
        for (name, color, size) in [
            ("banner-red", NSColor(red: 0.9, green: 0.14, blue: 0.08, alpha: 1), NSSize(width: 1280, height: 720)),
            ("banner-blue", NSColor(red: 0.08, green: 0.24, blue: 0.9, alpha: 1), NSSize(width: 1280, height: 720)),
            ("cover", NSColor(red: 0.1, green: 0.8, blue: 0.3, alpha: 1), NSSize(width: 256, height: 256))
        ] {
            let image = NSImage(size: size)
            image.lockFocus()
            color.setFill(); NSBezierPath(rect: NSRect(origin: .zero, size: size)).fill()
            image.unlockFocus()
            let bitmap = try XCTUnwrap(NSBitmapImageRep(data: try XCTUnwrap(image.tiffRepresentation)))
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(to: root.appendingPathComponent(name + ".png"))
        }
    }

}

private final class HomeCatalogRunner: JobProcessRunner {
    let artwork: URL
    /// 第几款是用免费账号装到 iPhone 的、什么时候装的（Unix 秒）；没写的款没装过
    let installs: [Int: Double]
    /// 第几款导入时数据校验不过（packages 带 integritySkipped，11-17）
    let skipped: Set<Int>
    init(artwork: URL, installs: [Int: Double] = [:], skipped: Set<Int> = []) { self.artwork = artwork; self.installs = installs; self.skipped = skipped }
    private let lock = NSLock()
    private var launch: String?
    var lastLaunch: String? { lock.withLock { launch } }
    final class Handle: JobProcessHandle { func terminate() {} }
    func start(executable: URL, arguments: [String], environment: [String: String], onLine: @escaping (Data) -> Void,
               onStderr: @escaping (Data) -> Void, onExit: @escaping (Int32) -> Void) throws -> JobProcessHandle {
        objc_sync_enter(self); defer { objc_sync_exit(self) }   // 作业客户端在并发线程池里同时调 start：替身状态串行改（无锁并发追加数组会 SIGSEGV 带崩整个测试进程）
        let request = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: arguments.last!))) as! [String: Any]
        var result: [String: Any] = [:]
        if request["operation"] as? String == "launch" { lock.withLock { launch = request["gameId"] as? String } }
        if request["operation"] as? String == "catalog" {
            // 分开异构 JSON 的类型推断，避免 CI 工具链在整条 map 表达式上耗尽求解预算。
            let skippedContent: [String: Any] = ["stage": "import", "contents": ["x"], "place": "romfs"]
            let skippedPackage: [String: Any] = ["type": "base", "integritySkipped": [skippedContent]]
            let games = (0..<21).map { index -> [String: Any] in
                let packages = skipped.contains(index) ? [skippedPackage] : []
                var game: [String: Any] = ["id": String(format: "%016X", index),
                    "name": index == 19 ? "Original Fixture 19 — A Very Long Adventure Across the Mountains and the Distant Sea" : "Original Fixture \(index)",
                    "ready": true, "missing": [],
                    "customCover": false, "cover": artwork.appendingPathComponent("cover.png").path,
                    "banner": artwork.appendingPathComponent(index % 2 == 0 ? "banner-red.png" : "banner-blue.png").path,
                    "packages": packages]
                if let at = installs[index] {
                    game["iphoneInstall"] = ["at": at, "free": true, "expires": at + 7 * 86400] as [String: Any]   // 作业层给到期时刻（读不到真值时按 7 天估）
                } else {
                    game["iphoneInstall"] = NSNull()
                }
                return game
            }
            result = ["games": games, "keys": [:]]
        }
        let event: [String: Any] = ["schema": 1, "taskId": request["taskId"] ?? "", "type": "result", "state": "succeeded", "result": result]
        DispatchQueue.main.async { onLine(try! JSONSerialization.data(withJSONObject: event)); onExit(0) }
        return Handle()
    }
}
