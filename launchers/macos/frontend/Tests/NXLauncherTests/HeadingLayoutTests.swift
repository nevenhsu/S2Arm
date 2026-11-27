// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI
import XCTest
import NXModel
@testable import NXLauncher
@testable import NX2AppleUI

@MainActor final class HeadingLayoutTests: XCTestCase {
    func testEditableExportTitleKeepsTheVisibleTextOnTheSubtitleBaseline() async throws {
        _ = NSApplication.shared
        for width: CGFloat in [300, 680] {
            let navigation = LauncherPadNavigation()
            let host = NSHostingView(rootView: LauncherHeadingLine {
                ExportNameControl(name: "HHHH", custom: false, save: { _ in })
            } subtitle: {
                Text("HHHH").font(.manrope(14)).foregroundStyle(.white)
                    .controllerControl(id: "heading.subtitle", activate: {})
            }
            .padding(32).frame(width: width, height: 140).foregroundStyle(.white).background(Color.black)
            .environmentObject(Localizer(.en)).environment(\.padControls, navigation).preferredColorScheme(.dark))
            let window = NSWindow(contentRect: CGRect(x: 80, y: 80, width: width, height: 140), styleMask: [.titled], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
            defer { window.close() }
            host.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(100))
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            if let output = ProcessInfo.processInfo.environment["S2A_SETTINGS_SHOTS"] {
                try bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: output).appendingPathComponent("heading-editable-\(Int(width)).png"))
            }
            let scale = CGFloat(bitmap.pixelsWide) / width
            func glyphBottom(_ id: String) throws -> CGFloat {
                let target = try XCTUnwrap(navigation.controlTargets(in: window).first { $0.id == id })
                let rect = host.convert(window.convertFromScreen(target.frame), from: nil)
                let left = max(0, Int(rect.minX * scale)), right = min(bitmap.pixelsWide, Int(rect.maxX * scale))
                var bottom: Int?
                for y in 0..<bitmap.pixelsHigh {
                    for x in left..<right {
                        guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                        if min(color.redComponent, color.greenComponent, color.blueComponent) > 0.9 { bottom = y; break }
                    }
                }
                return CGFloat(try XCTUnwrap(bottom, "H 的白色笔画应真实可见")) / scale
            }
            let title = try glyphBottom("export.name"), subtitle = try glyphBottom("heading.subtitle")
            print("editable-heading \(width): titleGlyphBottom=\(title), subtitleGlyphBottom=\(subtitle)")
            XCTAssertEqual(title, subtitle, accuracy: 1, "输入标题与版本的实际字形基线应对齐，不能只核控件的名义 baseline")
        }
    }

    func testTitleAndSubtitleShareABaselineWithoutWrapping() async throws {
        for zh in [false, true] {
            for width: CGFloat in [300, 680] {
                for selectable in [false, true] {
                    for long in [false, true] {
                        let title = zh ? "标题" : "Heading"
                        let subtitle = long ? String(repeating: zh ? "原创较长的副标题说明" : "Original subtitle text ", count: 30) : "HHHH"
                        try await check(title: title, subtitle: subtitle, width: width, selectable: selectable, button: false,
                                        name: "heading-\(zh)-\(Int(width))-\(selectable)-\(long)")
                    }
                }
            }
        }
    }

    func testInteractiveTitlesKeepLongSubtitlesInsideTheLine() async throws {
        for width: CGFloat in [300, 680] {
            try await check(title: String(repeating: "Original title ", count: 20),
                            subtitle: "v123.456.789-dev+original-long-version", width: width,
                            selectable: true, button: true, name: "heading-interactive-\(Int(width))")
            try await check(title: "Exported", subtitle: String(repeating: "/Original/Long Output Folder", count: 12),
                            width: width, selectable: false, button: false, subtitleButton: true,
                            name: "heading-path-\(Int(width))")
        }
    }

    private func check(title: String, subtitle: String, width: CGFloat, selectable: Bool, button: Bool, subtitleButton: Bool = false, name: String) async throws {
        _ = NSApplication.shared
        let baselines = HeadingBaselines()
        let navigation = LauncherPadNavigation()
        let host = NSHostingView(rootView: HeadingPreview(title: title, subtitle: subtitle, width: width,
            selectable: selectable, button: button, subtitleButton: subtitleButton, baselines: baselines).environment(\.padControls, navigation))
        let window = NSWindow(contentRect: CGRect(x: 80, y: 80, width: width, height: 110),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host; UIStage.show(window)
        defer { window.close() }
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(90))
        let targets = navigation.controlTargets(in: window)
        let titleFrame = try XCTUnwrap(targets.first { $0.id == "heading.title" }).frame
        let subtitleFrame = try XCTUnwrap(targets.first { $0.id == "heading.subtitle" }).frame
        let titleBaseline = titleFrame.maxY - (try XCTUnwrap(baselines.values["title"]))
        let subtitleBaseline = subtitleFrame.maxY - (try XCTUnwrap(baselines.values["subtitle"]))
        let bounds = window.convertToScreen(host.bounds).insetBy(dx: 32, dy: 0)
        print("heading \(name): baselineDelta=\(titleBaseline - subtitleBaseline), subtitleHeight=\(subtitleFrame.height)")
        XCTAssertEqual(titleBaseline, subtitleBaseline, accuracy: 0.75)
        XCTAssertGreaterThanOrEqual(subtitleFrame.minX - titleFrame.maxX, 11.5)
        XCTAssertTrue(bounds.insetBy(dx: -1, dy: -1).contains(titleFrame.union(subtitleFrame)))
        XCTAssertLessThan(titleFrame.height, 40)
        XCTAssertLessThan(subtitleFrame.height, 24, "长副标题不能换行或将标题挤成两行")
        if let output = ProcessInfo.processInfo.environment["S2A_SETTINGS_SHOTS"] {
            let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: bitmap)
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                .write(to: URL(fileURLWithPath: output).appendingPathComponent(name + ".png"))
        }
    }
}

private final class HeadingBaselines {
    var values: [String: CGFloat] = [:]
}

private struct HeadingPreview: View {
    let title: String
    let subtitle: String
    let width: CGFloat
    let selectable: Bool
    let button: Bool
    let subtitleButton: Bool
    let baselines: HeadingBaselines
    private var titleText: some View {
        Text(title).font(.manrope(26, .heavy))
            .alignmentGuide(.firstTextBaseline) { dimensions in
                let value = dimensions[.firstTextBaseline]; baselines.values["title"] = value; return value
            }
    }
    private var subtitleText: some View {
        Text(subtitle).font(.manrope(14)).help(subtitle)
            .alignmentGuide(.firstTextBaseline) { dimensions in
                let value = dimensions[.firstTextBaseline]; baselines.values["subtitle"] = value; return value
            }
    }
    var body: some View {
        LauncherHeadingLine {
            if button {
                Button {} label: { titleText }.controllerButtonStyle(.plain).controllerID("heading.title")
            } else { titleText.controllerControl(id: "heading.title", activate: {}) }
        } subtitle: {
            if subtitleButton {
                Button {} label: { subtitleText }.buttonStyle(.plain)
                    .controllerControl(id: "heading.subtitle", activate: {})
            } else if selectable {
                subtitleText.textSelection(.enabled).controllerControl(id: "heading.subtitle", activate: {})
            } else { subtitleText.controllerControl(id: "heading.subtitle", activate: {}) }
        }
        .padding(.horizontal, 32).frame(width: width, height: 110)
        .foregroundStyle(.white).background(Color.black)
    }
}
