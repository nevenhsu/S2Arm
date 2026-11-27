// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import XCTest
@testable import NX2AppleUI

@MainActor final class LauncherTooltipTests: XCTestCase {
    func testPlacementFlipsAtEdgesAndKeepsThePointerOverTheIcon() {
        let available = CGRect(x: 8, y: 8, width: 784, height: 584)
        let cases: [(CGRect, LauncherTooltipPlacement, Bool, CGFloat)] = [
            (CGRect(x: 300, y: 540, width: 40, height: 40), .automatic, false, 260),
            (CGRect(x: 300, y: 100, width: 52, height: 52), .automatic, true, 266),
            (CGRect(x: 300, y: 540, width: 40, height: 40), .above, false, 260),
            (CGRect(x: 300, y: 8, width: 40, height: 40), .below, true, 260),
            (CGRect(x: 760, y: 540, width: 40, height: 40), .below, false, 672),
            (CGRect(x: 0, y: 100, width: 40, height: 40), .above, true, 8),
        ]
        for (anchor, preferred, above, left) in cases {
            let result = LauncherTooltipLayout(anchor: anchor, size: CGSize(width: 120, height: 37), available: available, preferred: preferred)
            XCTAssertEqual(result.above, above)
            XCTAssertEqual(result.frame.minX, left)
            XCTAssertTrue(available.contains(result.frame))
            let pointer = result.frame.minX + result.arrowX
            XCTAssertGreaterThanOrEqual(pointer, anchor.minX)
            XCTAssertLessThanOrEqual(pointer, anchor.maxX)
            if above { XCTAssertEqual(result.frame.minY, anchor.maxY + 6) }
            else { XCTAssertEqual(result.frame.maxY, anchor.minY - 6) }
        }
    }

    func testImmediateHandoffRefreshAndLateExitDoNotStealFocus() throws {
        let (window, first, second) = fixture()
        defer { window.close() }
        let presenter = LauncherTooltipPresenter.forWindow(window)
        let keyWindow = NSApp.keyWindow
        presenter.show(for: first)
        let panel = try XCTUnwrap(UIStage.shared.children(of: window).first)
        XCTAssertTrue(UIStage.shared.isShown(panel), "提示在调用栈内出现，不依赖系统或异步计时器")
        XCTAssertTrue(panel.ignoresMouseEvents)
        XCTAssertFalse(panel.canBecomeKey)
        XCTAssertTrue(NSApp.keyWindow === keyWindow)
        presenter.show(for: second)
        presenter.dismiss(for: first)
        XCTAssertTrue(UIStage.shared.isShown(panel), "旧图标移出事件不能收掉新图标的提示")
        second.text = "Updated original tooltip"
        presenter.refresh(for: second)
        let label = try XCTUnwrap(panel.contentView?.subviews.compactMap { $0 as? NSTextField }.first)
        XCTAssertEqual(label.stringValue, second.text)
        XCTAssertEqual(label.textColor, .white)
        XCTAssertTrue(panel === UIStage.shared.children(of: window).first, "切换按钮应复用本窗口的同一个提示")
        XCTAssertNil(first.hitTest(.zero), "跟踪背景不能接走原按钮的鼠标点击")
        presenter.dismiss(for: second)
        XCTAssertFalse(UIStage.shared.isShown(panel))
    }

    func testWindowChangesAndRemovingTheAnchorDismissTheTip() throws {
        let (window, anchor, _) = fixture()
        defer { window.close() }
        let presenter = LauncherTooltipPresenter.forWindow(window)
        for name in [NSWindow.didMoveNotification, NSWindow.didResizeNotification, NSWindow.didResignKeyNotification, NSWindow.willCloseNotification] {
            presenter.show(for: anchor)
            let panel = try XCTUnwrap(UIStage.shared.children(of: window).first)
            NotificationCenter.default.post(name: name, object: window)
            XCTAssertFalse(UIStage.shared.isShown(panel), "\(name)：窗口变化后不能留下孤立提示")
        }
        UIStage.show(window)   // 上面最后发的是「即将关闭」：不上屏的舞台把它当窗口关了（AppKit 里假通知分不出真关），接着用这个窗口要重新登记
        presenter.show(for: anchor)
        let panel = try XCTUnwrap(UIStage.shared.children(of: window).first)
        anchor.removeFromSuperview()
        XCTAssertFalse(UIStage.shared.isShown(panel))
    }

    func testEachWindowOwnsItsTipAndLongLabelsStayInsideTheWindow() throws {
        let (firstWindow, first, _) = fixture()
        let (secondWindow, second, _) = fixture()
        defer { firstWindow.close(); secondWindow.close() }
        second.text = String(repeating: "Original long tooltip label ", count: 12)
        for (window, anchor) in [(firstWindow, first), (secondWindow, second)] {
            LauncherTooltipPresenter.forWindow(window).show(for: anchor)
            let panel = try XCTUnwrap(UIStage.shared.children(of: window).first)
            XCTAssertTrue(UIStage.shared.isShown(panel))
            XCTAssertLessThanOrEqual(panel.frame.width, 360)
            XCTAssertTrue(window.frame.insetBy(dx: 8, dy: 8).contains(panel.frame))
        }
        LauncherTooltipPresenter.forWindow(firstWindow).dismiss()
        XCTAssertTrue(UIStage.shared.children(of: secondWindow).first.map(UIStage.shared.isShown) == true)
    }

    private func fixture() -> (NSWindow, LauncherTooltipAnchor, LauncherTooltipAnchor) {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: CGRect(x: 150, y: 150, width: 600, height: 400), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        let first = LauncherTooltipAnchor(frame: CGRect(x: 24, y: 24, width: 52, height: 52))
        first.text = "Original first tooltip"; first.placement = .above
        let second = LauncherTooltipAnchor(frame: CGRect(x: 500, y: 330, width: 40, height: 40))
        second.text = "Original second tooltip"; second.placement = .below
        window.contentView?.addSubview(first); window.contentView?.addSubview(second)
        UIStage.show(window)
        return (window, first, second)
    }
}
