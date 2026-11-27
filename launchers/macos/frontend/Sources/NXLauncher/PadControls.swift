// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI

/// Buttons register their actual action and view, including when SwiftUI has not
/// materialized its out-of-process accessibility tree. Native panels use AppKit.
@MainActor package protocol PadControlRegistry: AnyObject {
    var presentation: PadPresentation { get }
    func register(_ control: PadControl)
    func removeControl(_ token: UUID)
}
package struct PadControl {
    /// Identity of this native registration, independent of the semantic control ID.
    package let token: UUID
    package let id: String
    package let scope: String
    package let view: NSView
    package let enabled: Bool
    package let activate: () -> Void
    package let adjust: ((Int) -> Void)?
    package let preferred: Bool
    package var shortcuts: [PadShortcut] = []
    package var shortcutsRequireFocus = false
    /// Corner radius of the control's own edge; the focus ring is drawn on that edge with this radius (nil = theme control radius).
    package var ringRadius: CGFloat? = nil
    /// How far outside the edge the ring sits (0 = on the edge). A cream-filled control uses an outset, concentric ring: cream on cream is invisible.
    package var ringOutset: CGFloat = 0
    /// Only answers its shortcuts (e.g. B closes a nested menu); never a focus target, never hides AX controls under it.
    package var shortcutOnly = false
    /// 鼠标点它执行的动作，只有与 activate（手柄 A）不同时才给（如首页搜索钮：鼠标原位展开、A 开屏幕键盘）；nil = 同 activate。
    ///   单测不上屏、不接合成鼠标事件，「鼠标点了它」经这里走（LauncherPadNavigation.pointerActivate），与按钮自己的动作是同一个函数
    package var pointer: (() -> Void)? = nil
}
/// Identifiers of controls the controller navigation skips, including its accessibility fallback.
@MainActor package enum PadExclusion { package static var ids: Set<String> = [] }
/// A button style that tells the focus ring its shape, so the ring sits exactly on the control's border.
/// Circles and capsules return `.infinity` (clamped to half the height).
package protocol PadRingShaped { var padRingRadius: CGFloat { get }; var padRingOutset: CGFloat { get } }
extension PadRingShaped { package var padRingOutset: CGFloat { 0 } }
private struct PadRingRadiusKey: EnvironmentKey { static let defaultValue: CGFloat? = nil }
private struct PadRingOutsetKey: EnvironmentKey { static let defaultValue: CGFloat? = nil }
/// A operates the focused control; the right trigger submits the current page.
/// Hints follow the primary player's controller family and face-button mapping.
package enum PadShortcut: String, CaseIterable {
    case primary, back, secondary, tertiary, leftTrigger, rightTrigger
    /// 界面上显示的键：确认键（rightTrigger）一律显示 A——单只 Joy-Con 没有 ZR（11-16）。ZR / R2 / RT 照样能按，只是不画出来
    package var displayed: PadShortcut { self == .rightTrigger ? .primary : self }
    package var symbol: String {
        switch self {
        case .primary: return "a.circle"
        case .back: return "b.circle"
        case .secondary: return "x.circle"
        case .tertiary: return "y.circle"
        case .leftTrigger: return "lt.rectangle.roundedtop"
        case .rightTrigger: return "rt.rectangle.roundedtop"
        }
    }
}
@MainActor package final class PadPresentation: ObservableObject {
    @Published package var connected = false
    @Published package var symbols: [PadShortcut: String] = [:]
    package init() {}
}
private struct PadShortcutsKey: EnvironmentKey { static let defaultValue: [PadShortcut] = [] }
private struct PadShortcutsRequireFocusKey: EnvironmentKey { static let defaultValue = false }
private struct PadRegistryKey: EnvironmentKey { static let defaultValue: (any PadControlRegistry)? = nil }
private struct PadControlIDKey: EnvironmentKey { static let defaultValue: String? = nil }
private struct PadControlScopeKey: EnvironmentKey { static let defaultValue = "page" }
private struct PadPreferredKey: EnvironmentKey { static let defaultValue = false }
extension EnvironmentValues {
    package var padControls: (any PadControlRegistry)? { get { self[PadRegistryKey.self] } set { self[PadRegistryKey.self] = newValue } }
    package var padControlID: String? { get { self[PadControlIDKey.self] } set { self[PadControlIDKey.self] = newValue } }
    package var padRingRadius: CGFloat? { get { self[PadRingRadiusKey.self] } set { self[PadRingRadiusKey.self] = newValue } }
    package var padRingOutset: CGFloat? { get { self[PadRingOutsetKey.self] } set { self[PadRingOutsetKey.self] = newValue } }
    package var padControlScope: String { get { self[PadControlScopeKey.self] } set { self[PadControlScopeKey.self] = newValue } }
    package var padPreferred: Bool { get { self[PadPreferredKey.self] } set { self[PadPreferredKey.self] = newValue } }
    package var padShortcuts: [PadShortcut] { get { self[PadShortcutsKey.self] } set { self[PadShortcutsKey.self] = newValue } }
    package var padShortcutsRequireFocus: Bool { get { self[PadShortcutsRequireFocusKey.self] } set { self[PadShortcutsRequireFocusKey.self] = newValue } }
}
extension View {
    package func controllerID(_ id: String) -> some View { environment(\.padControlID, id).accessibilityIdentifier(id) }
    /// A control the controller must never reach (no focus, no A): mouse only. Keeps the accessibility identifier.
    package func controllerExcluded(_ id: String) -> some View {
        accessibilityIdentifier(id).onAppear { PadExclusion.ids.insert(id) }
    }
    /// The focus ring's corner radius for a hand-drawn control (`.plain` buttons); styled buttons take it from their style.
    package func controllerRingRadius(_ radius: CGFloat, outset: CGFloat = 0) -> some View {
        environment(\.padRingRadius, radius).environment(\.padRingOutset, outset)
    }
    /// B on the controller runs `action` while this view is in the active window (closes a nested menu only, not its parent popover).
    package func controllerBackAction(_ id: String, _ action: @escaping () -> Void) -> some View {
        background(Color.clear.modifier(PadControlModifier(explicitID: id, activate: action, forcedShortcuts: [.back], shortcutOnly: true)))
    }
    package func controllerScope(_ scope: String) -> some View { environment(\.padControlScope, scope) }
    package func controllerPreferredFocus(_ preferred: Bool) -> some View { environment(\.padPreferred, preferred) }
    /// Secondary submit actions in a page with multiple tasks require focus, so
    /// R2 cannot start an unrelated row or replace the page's default action.
    /// Back and other aliases remain available regardless of focus.
    package func controllerShortcut(_ shortcuts: PadShortcut..., focused: Bool = false) -> some View {
        environment(\.padShortcuts, shortcuts).environment(\.padShortcutsRequireFocus, focused)
    }
    package func controllerConnectedOnly() -> some View { modifier(PadConnectedOnlyModifier()) }
    package func controllerButtonStyle<S: ButtonStyle>(_ style: S) -> some View { buttonStyle(PadVisualButtonStyle(style: style)) }
    package func controllerButtonStyle<S: PrimitiveButtonStyle>(_ style: S) -> some View { buttonStyle(PadPrimitiveButtonStyle(style: style)) }
    package func controllerControl(id: String? = nil, activate: @escaping () -> Void, adjust: ((Int) -> Void)? = nil, pointer: (() -> Void)? = nil) -> some View {
        modifier(PadControlModifier(explicitID: id, activate: activate, adjust: adjust, pointer: pointer))
    }
}
private struct PadVisualButtonStyle<Style: ButtonStyle>: PrimitiveButtonStyle {
    let style: Style
    func makeBody(configuration: Configuration) -> some View {
        Button(action: configuration.trigger) { PadButtonLabel(label: configuration.label) }
            .buttonStyle(style).modifier(PadControlModifier(activate: configuration.trigger, styleRadius: (style as? any PadRingShaped)?.padRingRadius,
                                                   styleOutset: (style as? any PadRingShaped)?.padRingOutset ?? 0))
    }
}
private struct PadPrimitiveButtonStyle<Style: PrimitiveButtonStyle>: PrimitiveButtonStyle {
    let style: Style
    func makeBody(configuration: Configuration) -> some View {
        Button(action: configuration.trigger) { PadButtonLabel(label: configuration.label) }
            .buttonStyle(style).modifier(PadControlModifier(activate: configuration.trigger))
    }
}
private struct PadButtonLabel<Label: View>: View {
    let label: Label
    @Environment(\.padShortcuts) private var shortcuts
    @Environment(\.padControls) private var registry
    @ViewBuilder var body: some View {
        if let shortcut = shortcuts.first, let presentation = registry?.presentation {
            PadConnectedButtonLabel(label: label, shortcut: shortcut, presentation: presentation)
        } else { label }
    }
}
private struct PadConnectedButtonLabel<Label: View>: View {
    let label: Label
    let shortcut: PadShortcut
    @ObservedObject var presentation: PadPresentation
    @ViewBuilder var body: some View {
        if presentation.connected {
            HStack(spacing: 6) {
                Image(systemName: presentation.symbols[shortcut.displayed] ?? shortcut.displayed.symbol)
                    .font(.system(size: 16, weight: .medium)).accessibilityHidden(true)
                label
            }
        } else { label }
    }
}
private struct PadConnectedOnlyModifier: ViewModifier {
    @Environment(\.padControls) private var registry
    @ViewBuilder func body(content: Content) -> some View {
        if let presentation = registry?.presentation {
            PadConnectedContent(content: content, presentation: presentation)
        }
    }
}
private struct PadConnectedContent<Content: View>: View {
    let content: Content
    @ObservedObject var presentation: PadPresentation
    @ViewBuilder var body: some View {
        if presentation.connected { content }
    }
}
private struct PadControlModifier: ViewModifier {
    var explicitID: String?
    let activate: () -> Void
    var adjust: ((Int) -> Void)?
    var pointer: (() -> Void)? = nil
    var styleRadius: CGFloat? = nil
    var styleOutset: CGFloat = 0
    var forcedShortcuts: [PadShortcut]? = nil
    var shortcutOnly = false
    @Environment(\.padRingOutset) private var ringOutsetOverride
    @State private var token = UUID()
    @Environment(\.padRingRadius) private var ringRadius
    @Environment(\.padControls) private var registry
    @Environment(\.padControlID) private var identifier
    @Environment(\.padControlScope) private var scope
    @Environment(\.isEnabled) private var enabled
    @Environment(\.padPreferred) private var preferred
    @Environment(\.padShortcuts) private var shortcuts
    @Environment(\.padShortcutsRequireFocus) private var shortcutsRequireFocus
    func body(content: Content) -> some View {
        content.background {
            if registry != nil {
                PadControlFrame(registry: registry, id: explicitID ?? identifier ?? "control." + token.uuidString,
                    scope: scope, enabled: enabled, activate: activate, adjust: adjust, preferred: preferred && !shortcutOnly, shortcuts: forcedShortcuts ?? shortcuts,
                    shortcutsRequireFocus: shortcutsRequireFocus, ringRadius: ringRadius ?? styleRadius,
                    ringOutset: ringRadius == nil ? styleOutset : (ringOutsetOverride ?? 0), shortcutOnly: shortcutOnly, pointer: pointer)
            }
        }
    }
}
private struct PadControlFrame: NSViewRepresentable {
    let registry: (any PadControlRegistry)?
    let id: String
    let scope: String
    let enabled: Bool
    let activate: () -> Void
    let adjust: ((Int) -> Void)?
    let preferred: Bool
    let shortcuts: [PadShortcut]
    let shortcutsRequireFocus: Bool
    let ringRadius: CGFloat?
    let ringOutset: CGFloat
    let shortcutOnly: Bool
    var pointer: (() -> Void)? = nil
    final class Host: NSView {
        weak var registry: (any PadControlRegistry)?
        // SwiftUI can overlap old and new hosts during a button-style update.
        // Each host must remove only its own registration when dismantled.
        let token = UUID()
        override func hitTest(_ point: NSPoint) -> NSView? { nil }
        override func isAccessibilityElement() -> Bool { false }
    }
    func makeNSView(context: Context) -> Host { Host() }
    func updateNSView(_ view: Host, context: Context) {
        if let previous = view.registry, previous !== registry { previous.removeControl(view.token) }
        view.registry = registry
        registry?.register(PadControl(token: view.token, id: id, scope: scope, view: view, enabled: enabled, activate: activate, adjust: adjust, preferred: preferred, shortcuts: shortcuts, shortcutsRequireFocus: shortcutsRequireFocus, ringRadius: ringRadius, ringOutset: ringOutset, shortcutOnly: shortcutOnly, pointer: pointer))
    }
    static func dismantleNSView(_ view: Host, coordinator: ()) {
        view.registry?.removeControl(view.token)
    }
}

/// Preserve the requested system switch/checkbox appearance and expose its value
/// action directly; it must not depend on the macOS Full Keyboard Access setting.
package struct PadToggleStyle<Style: ToggleStyle>: ToggleStyle {
    let style: Style
    package init(_ style: Style) { self.style = style }
    @ViewBuilder package func makeBody(configuration: Configuration) -> some View {
        if #available(macOS 15.0, *), style is CheckboxToggleStyle {
            PadCheckboxToggleBody(style: style, configuration: configuration)
        } else {
            Toggle(isOn: configuration.$isOn) { configuration.label }.toggleStyle(style)
                .modifier(PadControlModifier(activate: { configuration.isOn.toggle() }, adjust: { configuration.isOn = $0 > 0 },
                                             styleRadius: (style as? any PadRingShaped)?.padRingRadius))
        }
    }
}

@available(macOS 15.0, *)
private struct PadCheckboxToggleBody<Style: ToggleStyle>: View {
    let style: Style
    let configuration: ToggleStyleConfiguration
    @Environment(\.labelsVisibility) private var labelsVisibility
    var body: some View {
        HStack(spacing: labelsVisibility == .hidden ? 0 : 6) {
            Toggle("", isOn: configuration.$isOn)
                .toggleStyle(style)
                .labelsHidden()
                .modifier(PadControlModifier(activate: { configuration.isOn.toggle() }, adjust: { configuration.isOn = $0 > 0 },
                                             styleRadius: LauncherTheme.Focus.checkboxRadius, styleOutset: LauncherTheme.Focus.checkboxOutset))
            if labelsVisibility != .hidden {
                configuration.label
                    .contentShape(Rectangle())
                    .onTapGesture { configuration.isOn.toggle() }
            }
        }
        .accessibilityRepresentation {
            Toggle(isOn: configuration.$isOn) { configuration.label }.toggleStyle(style)
        }
    }
}
