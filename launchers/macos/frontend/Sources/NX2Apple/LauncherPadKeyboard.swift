// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI
import NXLauncher
import LauncherPadBridge

/// Controller text entry stays inside the app and writes through the same public
/// accessibility setter as native text entry. Cancel preserves the original text.
@MainActor final class LauncherPadKeyboard: NSObject, ObservableObject, NSWindowDelegate {
    @Published var value: String
    @Published var uppercase = false
    @Published var symbols = false
    let panel: NSPanel
    private let applyValue: (String) -> Void
    private let onClose: () -> Void
    private weak var owner: NSWindow?
    private weak var navigation: LauncherPadNavigation?
    private let previousFocus: String?
    let label: String
    let secure: Bool
    private var closed = false

    convenience init(element: Any, owner: NSWindow, navigation: LauncherPadNavigation) {
        self.init(value: S2ALauncherAX.value(element) as? String ?? "", label: S2ALauncherAX.label(element) ?? "", owner: owner, navigation: navigation, protected: S2ALauncherAX.secure(element),
                  apply: { S2ALauncherAX.setValue($0, element: element) })
    }
    init(value: String, label: String, owner: NSWindow, navigation: LauncherPadNavigation, protected: Bool = false, onClose: @escaping () -> Void = {}, apply: @escaping (String) -> Void) {
        self.applyValue = apply; self.owner = owner; self.navigation = navigation
        self.onClose = onClose
        previousFocus = navigation.focusedID
        self.value = value; self.label = label
        secure = protected || label.localizedCaseInsensitiveContains("password") || label.contains("密码")
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: LauncherTheme.Metrics.settingsWidth, height: 410), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        super.init()
        panel.isReleasedWhenClosed = false
        panel.title = navigation.text("home.keyboard.title")
        panel.appearance = NSAppearance(named: .darkAqua)
        panel.contentView = NSHostingView(rootView: LauncherPadKeyboardView(keyboard: self).environment(\.padControls, navigation))
        panel.delegate = self
    }
    func t(_ id: String) -> String { navigation?.text(id) ?? id }
    func show() {
        guard let owner else { return }
        let frame = owner.frame
        panel.setFrameOrigin(NSPoint(x: frame.midX - panel.frame.width / 2, y: frame.midY - panel.frame.height / 2))
        LauncherWindowPresenter.current.attach(panel, owner)
        LauncherWindowPresenter.current.makeKeyAndOrderFront(panel)
        navigation?.focus("pad.key.q")
    }
    func close(apply: Bool) {
        guard !closed else { return }; closed = true
        if apply { applyValue(value) }
        if let owner { LauncherWindowPresenter.current.detach(panel, owner) }
        panel.delegate = nil; LauncherWindowPresenter.current.orderOut(panel); panel.contentView = nil; panel.close()
        if let owner { LauncherWindowPresenter.current.makeKeyAndOrderFront(owner) }
        let navigation = navigation
        navigation?.keyboard = nil
        if let previousFocus { navigation?.focus(previousFocus) }
        onClose()
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { close(apply: false); return false }
    func handle(_ action: LauncherPadAction) -> Bool {
        switch action {
        case .back: close(apply: false); return true
        case .submit: close(apply: true); return true
        case .pageUp: value = ""; return true
        case .filter: if !value.isEmpty { value.removeLast() }; return true
        case .details: value.append(" "); return true
        default: return false
        }
    }
}

private struct LauncherPadKeyboardView: View {
    @ObservedObject var keyboard: LauncherPadKeyboard
    private var rows: [String] {
        keyboard.symbols ? ["1234567890", "!@#$%^&*()", "-_=+[]{}<>", "/\\|:;\"'`~", ",.?"]
            : ["1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm-_", "@./:!?+=()"]
    }
    var body: some View {
        VStack(spacing: 10) {
            Text(keyboard.label.isEmpty ? keyboard.t("home.keyboard.title") : keyboard.label)
                .font(.manrope(13, .semibold)).foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading)
            Text(keyboard.secure ? String(repeating: "•", count: keyboard.value.count) : keyboard.value + "│")
                .font(.manrope(19, .medium)).lineLimit(1).frame(maxWidth: .infinity, alignment: .leading).padding(10)
                .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 8)).accessibilityHidden(true)
            VStack(spacing: 7) {
                ForEach(rows, id: \.self) { row in
                    HStack(spacing: 7) {
                        ForEach(Array(row), id: \.self) { char in
                            let value = keyboard.uppercase ? String(char).uppercased() : String(char)
                            Button { keyboard.value.append(value) } label: {
                                Text(value).font(.manrope(15, .semibold)).frame(width: 50, height: 33).contentShape(Rectangle())
                                    .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 7))
                            }.controllerButtonStyle(.plain).controllerID("pad.key." + String(char))
                        }
                    }.frame(maxWidth: .infinity)
                }
            }
            HStack(spacing: 10) {
                Button(keyboard.symbols ? "abc" : "#+=") { keyboard.symbols.toggle() }.controllerID("pad.keyboard.symbols")
                Button(keyboard.uppercase ? "abc" : "ABC") { keyboard.uppercase.toggle() }
                Button(keyboard.t("home.keyboard.space")) { keyboard.value.append(" ") }.controllerShortcut(.tertiary)
                Button("⌫") { if !keyboard.value.isEmpty { keyboard.value.removeLast() } }.controllerShortcut(.secondary)
                Button(keyboard.t("home.keyboard.clear")) { keyboard.value = "" }.controllerShortcut(.leftTrigger)
                Spacer(minLength: 4)
                Button(keyboard.t("home.keyboard.cancel")) { keyboard.close(apply: false) }
                    .controllerShortcut(.back)
                Button(keyboard.t("home.keyboard.done")) { keyboard.close(apply: true) }
                    .controllerButtonStyle(LauncherPrimaryButtonStyle()).keyboardShortcut(.defaultAction).controllerShortcut(.rightTrigger)
            }.controllerButtonStyle(LauncherSecondaryButtonStyle()).fixedSize(horizontal: true, vertical: false)
        }.padding(LauncherTheme.Space.section).launcherSheet(.settings, height: 410)
    }
}
