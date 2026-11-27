// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI

package enum LauncherCopy {
    /// UI descriptions omit a final full stop; log and data strings keep their original bytes.
    package static func note(_ text: String) -> String {
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if value.hasSuffix("。") || (value.hasSuffix(".") && !value.hasSuffix("...")) { return String(value.dropLast()) }
        return value
    }
}

/// The Home palette and typeface, with one scale for every utility page and sheet.
package enum LauncherTheme {
    package enum Space {
        package static let xSmall: CGFloat = 4
        package static let small: CGFloat = 8
        package static let medium: CGFloat = 12
        package static let large: CGFloat = 16
        package static let xLarge: CGFloat = 20
        package static let section: CGFloat = 24
        package static let page: CGFloat = 32
    }
    package enum Metrics {
        package static let settingsWidth: CGFloat = 820
        package static let formWidth: CGFloat = 680
        package static let compactWidth: CGFloat = 560
        package static let settingsHeight: CGFloat = 700
        package static let readingPaneHeight: CGFloat = 320
        package static let readingPaneMinHeight: CGFloat = 80
        package static let cardRadius: CGFloat = 16
        package static let cardInset: CGFloat = Space.xLarge
        package static let controlRadius: CGFloat = 10
        package static let controlHeight: CGFloat = 36
        package static let prominentControlHeight: CGFloat = 48
        package static let controlWidth: CGFloat = 220
        package static let compactChoiceWidth: CGFloat = 124
        package static let switchWidth: CGFloat = 40
        package static let switchHeight: CGFloat = 24
        package static let navigationWidth: CGFloat = 300
        package static let libraryInset: CGFloat = 48
        package static let actionMinWidth: CGFloat = 80
        package static let icon: CGFloat = 18
    }
    /// 手柄焦点框的统一主题令牌：全站线宽、外扩、圆角与留白均从此处取用
    package enum Focus {
        package static let lineWidth: CGFloat = 2
        package static let creamOutset: CGFloat = 3
        package static let checkboxRadius: CGFloat = 4
        package static let checkboxOutset: CGFloat = 2
        package static let radioOutset: CGFloat = 0
        package static let plainPaddingX: CGFloat = 8
        package static let plainPaddingY: CGFloat = 5
        package static let plainRadius: CGFloat = 8
        package static let rowRadius: CGFloat = 8
    }
    /// 按钮形状令牌：全站按钮与控件统一从此取用，避免同一排混用方按钮与胶囊
    package enum ButtonShape: Equatable, Sendable {
        case rounded
        case capsule
    }
    /// 按钮尺寸档位令牌：集中管理高度、字号、内边距、图标尺寸、图标间距与最小宽度
    package enum ButtonSize: Equatable, Sendable {
        case compact
        case regular
        case prominent
        case hero
    }
    package struct ButtonSpec: Sendable {
        package let height: CGFloat
        package let horizontalPadding: CGFloat
        package let font: Font
        package let iconSize: CGFloat
        package let iconSpacing: CGFloat
        package let minWidth: CGFloat
    }
    package static func buttonSpec(_ size: ButtonSize, primary: Bool = false) -> ButtonSpec {
        switch size {
        case .compact:
            return ButtonSpec(height: 30, horizontalPadding: Space.medium,
                              font: .manrope(12.5, .semibold), iconSize: 13, iconSpacing: 6, minWidth: 0)
        case .regular:
            return ButtonSpec(height: Metrics.controlHeight, horizontalPadding: Space.large,
                              font: TypeStyle.button, iconSize: 14, iconSpacing: Space.small, minWidth: Metrics.actionMinWidth)
        case .prominent:
            return ButtonSpec(height: Metrics.prominentControlHeight, horizontalPadding: Space.large,
                              font: .manrope(16, .semibold), iconSize: 16, iconSpacing: Space.small, minWidth: Metrics.actionMinWidth)
        case .hero:
            return ButtonSpec(height: 52, horizontalPadding: primary ? 24 : 20,
                              font: primary ? .manrope(16, .bold) : .manrope(15, .semibold),
                              iconSize: 17, iconSpacing: Space.small, minWidth: primary ? 92 : 0)
        }
    }
    package enum TypeStyle {
        package static let title = Font.manrope(26, .heavy)
        package static let panelTitle = Font.manrope(18, .bold)
        package static let section = Font.manrope(14, .semibold)
        package static let body = Font.manrope(14)
        package static let detail = Font.manrope(13)
        package static let caption = Font.manrope(12)
        package static let button = Font.manrope(13, .semibold)
    }
    package enum Colors {
        package static let text = Color.white.opacity(0.94)
        package static let secondary = Color.white.opacity(0.65)
        package static let border = Color.white.opacity(0.10)
        package static let card = Color.white.opacity(0.045)
        package static let control = Color.white.opacity(0.065)
        package static let hover = Color.white.opacity(0.11)
        package static let pressed = Color.white.opacity(0.15)
    }
}

package enum LauncherSheetSize {
    case settings, form, compact
    package var width: CGFloat {
        switch self {
        case .settings: return LauncherTheme.Metrics.settingsWidth
        case .form: return LauncherTheme.Metrics.formWidth
        case .compact: return LauncherTheme.Metrics.compactWidth
        }
    }
    /// 弹窗窗口高度上限：AppKit beginSheet 在弹窗高度 <= 父窗口高度 - 136 pt 时才会以父窗口中心垂直居中（留出顶栏 + 对称底边距），
    ///   所以有父窗口时按它裁（不低于 360）；还不知道父窗口时按屏幕高度留 100
    package static func maxWindowHeight(parent: CGFloat?, screen: CGFloat) -> CGFloat {
        parent.map { max(360, $0 - 136) } ?? (screen - 100)
    }
    package func fitted(to screen: CGSize, height: CGFloat?) -> (width: CGFloat, height: CGFloat?) {
        (min(width, screen.width - 64), height.map { min($0, screen.height - 100) })
    }
}

extension View {
    /// One native material at the window level; content cards only add a light tonal separation.
    package func launcherSheet(_ size: LauncherSheetSize = .settings, height: CGFloat? = LauncherTheme.Metrics.settingsHeight) -> some View {
        modifier(LauncherSheetPresentation(size: size, height: height))
    }

    package func launcherPanel() -> some View {
        background(LauncherTheme.Colors.card, in: RoundedRectangle(cornerRadius: LauncherTheme.Metrics.cardRadius, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: LauncherTheme.Metrics.cardRadius, style: .continuous)
                .strokeBorder(LauncherTheme.Colors.border))
    }
}

private struct LauncherSheetPresentation: ViewModifier {
    let size: LauncherSheetSize
    let height: CGFloat?
    @State private var parentHeight: CGFloat?

    func body(content: Content) -> some View {
        let screen = NSScreen.main?.visibleFrame.size ?? CGSize(width: 1440, height: 900)
        let dimensions = size.fitted(to: screen, height: height)
        // 若只按屏幕高度裁剪，在常见 630~750 pt 外窗口上会把弹窗顶到最底边（规则见 LauncherSheetSize.maxWindowHeight）
        let maxWindowHeight = LauncherSheetSize.maxWindowHeight(parent: parentHeight, screen: screen.height)
        let fittedHeight = dimensions.height.map { min($0, maxWindowHeight) }
        let page = content
            .frame(width: dimensions.width, height: fittedHeight)
            .frame(maxHeight: min(screen.height - 100, maxWindowHeight))
            .background(.regularMaterial)
            .background(SheetParentTracker(parentHeight: $parentHeight))
            .font(LauncherTheme.TypeStyle.detail)
            .foregroundStyle(LauncherTheme.Colors.text)
            .preferredColorScheme(.dark)
            .tint(LauncherPalette.cream)
            .labelStyle(LauncherLabelStyle())
            .launcherDropdownHost()
        if #available(macOS 13.3, *) {
            page.presentationBackground(.regularMaterial)
        } else {
            page
        }
    }
}

/// 追踪弹窗所在的父窗口高度，并在父窗口大小变化或刚挂上时让弹窗保持垂直居中、不贴底边。
private struct SheetParentTracker: NSViewRepresentable {
    @Binding var parentHeight: CGFloat?

    func makeCoordinator() -> Coordinator { Coordinator(parentHeight: $parentHeight) }
    func makeNSView(context: Context) -> Host {
        let host = Host()
        host.onWindowChange = { [weak coordinator = context.coordinator] window in
            coordinator?.attach(to: window)
        }
        return host
    }
    func updateNSView(_ view: Host, context: Context) {
        context.coordinator.parentHeight = $parentHeight
        context.coordinator.attach(to: view.window)
    }

    final class Host: NSView {
        var onWindowChange: ((NSWindow?) -> Void)?
        override func hitTest(_ point: NSPoint) -> NSView? { nil }
        override func isAccessibilityElement() -> Bool { false }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            onWindowChange?(window)
        }
    }

    @MainActor final class Coordinator {
        var parentHeight: Binding<CGFloat?>
        private weak var trackedParent: NSWindow?
        private weak var trackedSheet: NSWindow?
        private var observer: NSObjectProtocol?

        init(parentHeight: Binding<CGFloat?>) {
            self.parentHeight = parentHeight
        }

        func attach(to sheet: NSWindow?) {
            guard let sheet else { return }
            guard let parent = sheet.sheetParent, parent !== sheet else {
                // SwiftUI 的 sheet host 可能在 beginSheet 绑定 sheetParent 前先进入窗口层级，下一主线程周期再检查一次。
                DispatchQueue.main.async { [weak self, weak sheet] in
                    guard let self, let sheet, let parent = sheet.sheetParent, parent !== sheet else { return }
                    self.bind(sheet: sheet, parent: parent)
                }
                return
            }
            bind(sheet: sheet, parent: parent)
        }

        private func bind(sheet: NSWindow, parent: NSWindow) {
            trackedSheet = sheet
            if trackedParent !== parent {
                if let observer { NotificationCenter.default.removeObserver(observer) }
                trackedParent = parent
                observer = NotificationCenter.default.addObserver(forName: NSWindow.didResizeNotification, object: parent, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.sync() }
                }
            }
            sync()
        }

        private func sync() {
            guard let parent = trackedParent else { return }
            let h = parent.frame.height
            if abs((parentHeight.wrappedValue ?? 0) - h) > 0.5 {
                DispatchQueue.main.async { [weak self] in
                    self?.parentHeight.wrappedValue = h
                    self?.centerSheet()
                }
            } else {
                centerSheet()
            }
        }

        private func centerSheet() {
            guard let parent = trackedParent, let sheet = trackedSheet else { return }
            guard sheet.frame.height <= parent.frame.height - 20 else { return }
            let targetOrigin = NSPoint(
                x: round(parent.frame.midX - sheet.frame.width / 2),
                y: round(parent.frame.midY - sheet.frame.height / 2)
            )
            if abs(sheet.frame.origin.y - targetOrigin.y) > 1 || abs(sheet.frame.origin.x - targetOrigin.x) > 1 {
                sheet.setFrameOrigin(targetOrigin)
            }
        }

        deinit {
            if let observer { NotificationCenter.default.removeObserver(observer) }
        }
    }
}

/// A standard, accessible material respects Reduce Transparency and Increase Contrast.
/// 叠一层深黑曜石底色渐变，避免在浅色桌面壁纸/窗口上方 `.regularMaterial` 透灰泛白，影响卡片与次要文字对比度。
package struct LauncherWindowMaterial: View {
    package init() {}
    package var body: some View {
        ZStack {
            Rectangle().fill(.regularMaterial)
            LinearGradient(
                colors: [
                    LauncherPalette.background.opacity(0.76),
                    LauncherPalette.background.opacity(0.88)
                ],
                startPoint: .top,
                endPoint: .bottom
            )
        }
        .ignoresSafeArea()
    }
}

/// Window and page subtitles follow their title on one shared text baseline.
package struct LauncherHeadingLine<Title: View, Subtitle: View>: View {
    let title: Title
    let subtitle: Subtitle
    package init(@ViewBuilder title: () -> Title, @ViewBuilder subtitle: () -> Subtitle) {
        self.title = title(); self.subtitle = subtitle()
    }
    package var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: LauncherTheme.Space.medium) {
            title.lineLimit(1).truncationMode(.tail)
            subtitle.lineLimit(1).truncationMode(.tail)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

package struct LauncherSheetHeading: View {
    let title: String
    let subtitle: String
    let note: String
    package init(_ title: String, subtitle: String = "", note: String = "") {
        self.title = title; self.subtitle = subtitle; self.note = note
    }
    package var body: some View {
        VStack(alignment: .leading, spacing: LauncherTheme.Space.small) {
            LauncherHeadingLine {
                Text(title).font(LauncherTheme.TypeStyle.title).tracking(-0.6).accessibilityAddTraits(.isHeader)
            } subtitle: {
                if !subtitle.isEmpty {
                    Text(subtitle).font(LauncherTheme.TypeStyle.body).foregroundStyle(.secondary).help(subtitle)
                }
            }
            if !note.isEmpty {
                Text(LauncherCopy.note(note)).font(LauncherTheme.TypeStyle.detail).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

package struct LauncherSheetFooter<Content: View>: View {
    let content: Content
    package init(@ViewBuilder _ content: () -> Content) { self.content = content() }
    package var body: some View {
        HStack(spacing: LauncherTheme.Space.medium) { content }
            .padding(.horizontal, LauncherTheme.Space.page).padding(.vertical, LauncherTheme.Space.large)
            .frame(maxWidth: .infinity)
            .background(.bar)
            .overlay(alignment: .top) { Rectangle().fill(LauncherTheme.Colors.border).frame(height: 1) }
    }
}

/// Short explanations use their natural height; long ones scroll without displacing the actions.
package struct LauncherReadingPane<Content: View>: View {
    let content: Content
    @State private var contentHeight = LauncherTheme.Metrics.readingPaneHeight
    package init(@ViewBuilder _ content: () -> Content) { self.content = content() }
    package var body: some View {
        ScrollView {
            content.frame(maxWidth: .infinity, alignment: .leading).fixedSize(horizontal: false, vertical: true)
                .background(GeometryReader { geometry in
                    Color.clear.preference(key: LauncherReadingHeight.self, value: geometry.size.height)
                })
        }
        .frame(minHeight: min(contentHeight, LauncherTheme.Metrics.readingPaneMinHeight),
               idealHeight: min(contentHeight, LauncherTheme.Metrics.readingPaneHeight),
               maxHeight: min(contentHeight, LauncherTheme.Metrics.readingPaneHeight))
        .onPreferenceChange(LauncherReadingHeight.self) { if $0 > 0 { contentHeight = $0 } }
    }
}

private struct LauncherReadingHeight: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = nextValue() }
}

/// Two short actions share one trailing column instead of each consuming a whole column.
package struct LauncherRowActions<Content: View>: View {
    let content: Content
    package init(@ViewBuilder _ content: () -> Content) { self.content = content() }
    package var body: some View {
        HStack(spacing: LauncherTheme.Space.small) { content }
            .environment(\.launcherControlWidth, (LauncherTheme.Metrics.controlWidth - LauncherTheme.Space.small) / 2)
            .frame(width: LauncherTheme.Metrics.controlWidth)
    }
}

/// Symbols of different intrinsic widths occupy an equal slot and share the text's visual centre.
package struct LauncherLabelStyle: LabelStyle {
    package var iconSize: CGFloat = 14
    package var spacing: CGFloat = LauncherTheme.Space.small
    package init(iconSize: CGFloat = 14, spacing: CGFloat = LauncherTheme.Space.small) {
        self.iconSize = iconSize; self.spacing = spacing
    }
    package func makeBody(configuration: Configuration) -> some View {
        HStack(alignment: .center, spacing: spacing) {
            configuration.icon
                .font(.system(size: iconSize, weight: .semibold))
                .frame(width: iconSize + 2, height: iconSize + 2)
                .accessibilityHidden(true)
            configuration.title
        }
    }
}

/// 胶囊标签（如「有新版」「BETA」）：统一使用 .circular 胶囊，避免 .continuous 在小高度下出现左右竖线破图。
package struct LauncherBadge: View {
    package let text: String
    package var foreground: Color = LauncherPalette.cream
    package var background: Color = Color.black.opacity(0.25)
    package var border: Color = LauncherPalette.cream.opacity(0.4)
    package var font: Font = LauncherTheme.TypeStyle.detail
    package var tracking: CGFloat = 0
    package var horizontalPadding: CGFloat = 10
    package var height: CGFloat = 24
    package init(_ text: String, foreground: Color = LauncherPalette.cream, background: Color = Color.black.opacity(0.25),
                 border: Color = LauncherPalette.cream.opacity(0.4), font: Font = LauncherTheme.TypeStyle.detail,
                 tracking: CGFloat = 0, horizontalPadding: CGFloat = 10, height: CGFloat = 24) {
        self.text = text; self.foreground = foreground; self.background = background; self.border = border
        self.font = font; self.tracking = tracking; self.horizontalPadding = horizontalPadding; self.height = height
    }
    package var body: some View {
        Text(text)
            .font(font).tracking(tracking).lineLimit(1).fixedSize()
            .foregroundStyle(foreground)
            .padding(.horizontal, horizontalPadding)
            .frame(height: height)
            .background(background, in: Capsule(style: .circular))
            .overlay(Capsule(style: .circular).strokeBorder(border, lineWidth: 1))
    }
}

/// 可自动换行的按钮行：多个长文案按钮并排时自动折行，杜绝超出容器挤压破图。
package struct LauncherFlowRow: Layout {
    package var spacing: CGFloat = LauncherTheme.Space.medium
    package var rowSpacing: CGFloat = LauncherTheme.Space.small
    package init(spacing: CGFloat = LauncherTheme.Space.medium, rowSpacing: CGFloat = LauncherTheme.Space.small) {
        self.spacing = spacing; self.rowSpacing = rowSpacing
    }

    package func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let maxWidth = proposal.width ?? .infinity
        var x: CGFloat = 0, y: CGFloat = 0, rowH: CGFloat = 0, usedW: CGFloat = 0
        for subview in subviews {
            let s = subview.sizeThatFits(ProposedViewSize(width: maxWidth.isFinite ? maxWidth : nil, height: nil))
            if x > 0 && x + s.width > maxWidth {
                y += rowH + rowSpacing
                x = 0; rowH = 0
            }
            x += (x > 0 ? spacing : 0) + s.width
            usedW = max(usedW, x)
            rowH = max(rowH, s.height)
        }
        return CGSize(width: usedW, height: y + rowH)
    }

    package func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, rowH: CGFloat = 0
        for subview in subviews {
            let s = subview.sizeThatFits(ProposedViewSize(width: bounds.width, height: nil))
            if x > bounds.minX && x + s.width > bounds.maxX + 0.5 {
                y += rowH + rowSpacing
                x = bounds.minX; rowH = 0
            }
            subview.place(at: CGPoint(x: x, y: y), anchor: .topLeading,
                          proposal: ProposedViewSize(width: min(s.width, bounds.width), height: s.height))
            x += min(s.width, bounds.width) + spacing
            rowH = max(rowH, s.height)
        }
    }
}

package struct LauncherControlShape: InsettableShape {
    package let capsule: Bool
    private var insetAmount: CGFloat = 0
    package init(capsule: Bool = false) { self.capsule = capsule }
    package func inset(by amount: CGFloat) -> Self {
        var copy = self
        copy.insetAmount += amount
        return copy
    }
    package func path(in rect: CGRect) -> Path {
        if capsule {
            return Capsule(style: .circular).inset(by: insetAmount).path(in: rect)
        }
        return RoundedRectangle(cornerRadius: LauncherTheme.Metrics.controlRadius, style: .continuous)
            .inset(by: insetAmount).path(in: rect)
    }
}

package struct LauncherPrimaryButtonStyle: ButtonStyle {
    package init(fill: Bool = false, prominent: Bool = false, capsule: Bool = false,
                 size: LauncherTheme.ButtonSize? = nil, shape: LauncherTheme.ButtonShape? = nil) {
        self.fill = fill
        self.size = size ?? (prominent ? .prominent : .regular)
        self.shape = shape ?? (capsule ? .capsule : .rounded)
    }
    package var fill = false
    package var size: LauncherTheme.ButtonSize = .regular
    package var shape: LauncherTheme.ButtonShape = .rounded
    package var prominent: Bool { size == .prominent }
    package var capsule: Bool { shape == .capsule }
    @Environment(\.isEnabled) private var enabled
    @Environment(\.launcherControlWidth) private var width
    @State private var hovered = false
    package func makeBody(configuration: Configuration) -> some View {
        let spec = LauncherTheme.buttonSpec(size, primary: true)
        let controlShape = LauncherControlShape(capsule: capsule)
        configuration.label
            .font(spec.font)
            .labelStyle(LauncherLabelStyle(iconSize: spec.iconSize, spacing: spec.iconSpacing))
            .lineLimit(1)
            .minimumScaleFactor(LauncherControl.minimumLabelScale)
            .padding(.horizontal, spec.horizontalPadding)
            .frame(width: width, height: spec.height)
            .frame(minWidth: width == nil ? spec.minWidth : nil)
            .frame(maxWidth: fill ? .infinity : nil)
            .foregroundStyle(LauncherPalette.background)
            .background(LauncherPalette.cream.opacity(configuration.isPressed ? 0.8 : hovered ? 0.93 : 1), in: controlShape)
            .contentShape(controlShape)
            .opacity(enabled ? 1 : 0.45)
            .onHover { hovered = $0 }
    }
}

package struct LauncherTextFieldStyle: TextFieldStyle {
    private let topInset: CGFloat
    package init(topInset: CGFloat = 0) { self.topInset = topInset }
    package func _body(configuration: TextField<Self._Label>) -> some View {
        configuration.textFieldStyle(.plain).padding(.top, topInset).modifier(LauncherFocusableField())
    }
}

private struct LauncherFocusableField: ViewModifier {
    @FocusState private var focused: Bool
    func body(content: Content) -> some View {
        content.focused($focused).modifier(LauncherFieldChrome(focused: focused))
    }
}

private struct LauncherFieldChrome: ViewModifier {
    var focused = false
    @Environment(\.isEnabled) private var enabled
    func body(content: Content) -> some View {
        content
            .padding(.horizontal, LauncherTheme.Space.medium)
            .frame(minHeight: LauncherTheme.Metrics.controlHeight)
            .background(LauncherTheme.Colors.control, in: RoundedRectangle(cornerRadius: LauncherTheme.Metrics.controlRadius))
            .overlay(RoundedRectangle(cornerRadius: LauncherTheme.Metrics.controlRadius)
                .strokeBorder(focused ? LauncherPalette.cream.opacity(0.8) : LauncherTheme.Colors.border, lineWidth: focused ? 1.5 : 1))
            .opacity(enabled ? 1 : 0.55)
    }
}

package struct LauncherSearchField: View {
    let title: String
    let clearTitle: String
    @Binding var text: String
    @FocusState private var focused: Bool
    package init(_ title: String, text: Binding<String>, clearTitle: String) {
        self.title = title; self._text = text; self.clearTitle = clearTitle
    }
    package var body: some View {
        HStack(spacing: LauncherTheme.Space.small) {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                .frame(width: LauncherTheme.Metrics.icon).accessibilityHidden(true)
            TextField("", text: $text, prompt: Text(title).foregroundColor(LauncherTheme.Colors.secondary))
                .textFieldStyle(.plain).focused($focused).accessibilityLabel(title)
            if !text.isEmpty {
                Button { text = "" } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                        .frame(width: 24, height: 28).contentShape(Rectangle())
                }
                .controllerButtonStyle(.plain).accessibilityLabel(clearTitle)
            }
        }
        .font(LauncherTheme.TypeStyle.detail).modifier(LauncherFieldChrome(focused: focused))
    }
}

package struct LauncherIconButtonStyle: ButtonStyle {
    package init(size: CGFloat = LauncherControl.height, cornerRadius: CGFloat = LauncherTheme.Metrics.controlRadius) {
        self.size = size; self.cornerRadius = cornerRadius
    }
    package var size: CGFloat = LauncherControl.height
    package var cornerRadius: CGFloat = LauncherTheme.Metrics.controlRadius
    @Environment(\.isEnabled) private var enabled
    @State private var hovered = false
    package func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        configuration.label.font(.system(size: 14, weight: .medium))
            .frame(width: size, height: size)
            .foregroundStyle(enabled ? Color.white.opacity(0.92) : .gray)
            .background(configuration.isPressed ? LauncherTheme.Colors.pressed : hovered ? LauncherTheme.Colors.hover : LauncherTheme.Colors.control,
                        in: shape)
            .overlay(shape.strokeBorder(LauncherTheme.Colors.border))
            .contentShape(shape)
            .opacity(enabled ? 1 : 0.45).onHover { hovered = $0 }
    }
}

package struct LauncherGuideStep: View {
    let number: Int
    let text: String
    package init(_ number: Int, _ text: String) { self.number = number; self.text = text }
    package var body: some View {
        HStack(alignment: .top, spacing: LauncherTheme.Space.medium) {
            Text(String(number)).font(.manrope(11, .semibold)).foregroundStyle(LauncherPalette.cream)
                .frame(width: 22, height: 22).background(LauncherTheme.Colors.control, in: Circle())
                .accessibilityHidden(true)
            Text(text).font(LauncherTheme.TypeStyle.detail).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true).padding(.top, 2)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// 手柄焦点框画在控件自己的边框上：各样式报自己的圆角
extension LauncherPrimaryButtonStyle: PadRingShaped {   // 奶油底：贴边的奶油框看不见，改外扩同心圆角
    package var padRingRadius: CGFloat { capsule ? .infinity : LauncherTheme.Metrics.controlRadius }
    package var padRingOutset: CGFloat { LauncherTheme.Focus.creamOutset }
}
extension LauncherIconButtonStyle: PadRingShaped { package var padRingRadius: CGFloat { cornerRadius } }
