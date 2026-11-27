// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI
import ObjectiveC

enum LauncherTooltipPlacement { case automatic, above, below }

extension View {
    /// App-owned timing and appearance, without a second system help bubble.
    func launcherTooltip(_ text: String, placement: LauncherTooltipPlacement = .automatic) -> some View {
        background(LauncherTooltipTracking(text: text, placement: placement)).accessibilityHint(Text(text))
    }
}

private struct LauncherTooltipTracking: NSViewRepresentable {
    let text: String
    let placement: LauncherTooltipPlacement
    func makeNSView(context: Context) -> LauncherTooltipAnchor { LauncherTooltipAnchor() }
    func updateNSView(_ view: LauncherTooltipAnchor, context: Context) {
        view.text = text; view.placement = placement
        view.presenter?.refresh(for: view)
    }
    static func dismantleNSView(_ view: LauncherTooltipAnchor, coordinator: ()) { view.presenter?.dismiss(for: view) }
}

/// A noninteractive background tracks the same bounds as the original SwiftUI button.
final class LauncherTooltipAnchor: NSView {
    var text = ""
    var placement = LauncherTooltipPlacement.automatic
    weak var presenter: LauncherTooltipPresenter?
    private var tracking: NSTrackingArea?
    override init(frame: NSRect) {
        super.init(frame: frame)
        identifier = NSUserInterfaceItemIdentifier("launcher.tooltip.anchor")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override func isAccessibilityElement() -> Bool { false }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: .zero, options: [.activeInKeyWindow, .mouseEnteredAndExited, .inVisibleRect], owner: self)
        addTrackingArea(area); tracking = area
    }
    override func viewWillMove(toWindow newWindow: NSWindow?) {
        presenter?.dismiss(for: self)
        super.viewWillMove(toWindow: newWindow)
    }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        presenter = window.map(LauncherTooltipPresenter.forWindow)
    }
    override func mouseEntered(with event: NSEvent) { presenter?.show(for: self) }
    override func mouseExited(with event: NSEvent) { presenter?.dismiss(for: self) }
}

/// Each window owns one presenter: a late exit from the old icon cannot dismiss the new tip.
final class LauncherTooltipPresenter {
    private static var associationKey: UInt8 = 0
    static func forWindow(_ window: NSWindow) -> LauncherTooltipPresenter {
        if let presenter = objc_getAssociatedObject(window, &associationKey) as? LauncherTooltipPresenter { return presenter }
        let presenter = LauncherTooltipPresenter()
        objc_setAssociatedObject(window, &associationKey, presenter, .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
        return presenter
    }
    private weak var source: LauncherTooltipAnchor?
    private var panel: NSPanel?
    private var observers: [NSObjectProtocol] = []
    private var clickMonitor: Any?

    func show(for anchor: LauncherTooltipAnchor) {
        dismiss()
        guard let window = anchor.window, LauncherWindowPresenter.current.isShown(window), !anchor.isHiddenOrHasHiddenAncestor,
              !anchor.visibleRect.isEmpty, !anchor.text.isEmpty else { return }
        source = anchor
        func observe(_ name: Notification.Name, object: AnyObject) {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: object, queue: .main) { [weak self] _ in self?.dismiss() })
        }
        for name in [NSWindow.willCloseNotification, NSWindow.didResignKeyNotification,
                     NSWindow.didMoveNotification, NSWindow.didResizeNotification] { observe(name, object: window) }
        observe(NSApplication.didResignActiveNotification, object: NSApplication.shared)
        if let clip = anchor.enclosingScrollView?.contentView {
            clip.postsBoundsChangedNotifications = true
            observe(NSView.boundsDidChangeNotification, object: clip)
        }
        clickMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown, .scrollWheel, .keyDown]) { [weak self] event in
            self?.dismiss(); return event
        }
        present(anchor, in: window)
    }

    func refresh(for anchor: LauncherTooltipAnchor) {
        guard source === anchor else { return }
        guard let window = anchor.window, !anchor.isHiddenOrHasHiddenAncestor, !anchor.text.isEmpty else { dismiss(); return }
        present(anchor, in: window)
    }

    func dismiss(for anchor: LauncherTooltipAnchor? = nil) {
        if let anchor, source !== anchor { return }
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers.removeAll()
        if let clickMonitor { NSEvent.removeMonitor(clickMonitor) }; clickMonitor = nil
        if let panel { if let parent = panel.parent { LauncherWindowPresenter.current.detach(panel, parent) }; LauncherWindowPresenter.current.orderOut(panel) }
        source = nil
    }

    private func present(_ anchor: LauncherTooltipAnchor, in owner: NSWindow) {
        let anchorFrame = owner.convertToScreen(anchor.convert(anchor.bounds, to: nil))
        let screen = owner.screen?.visibleFrame ?? owner.frame
        let available = owner.frame.intersection(screen).insetBy(dx: 8, dy: 8)
        guard available.width > 32, available.height > 32 else { dismiss(); return }
        let bubble = LauncherTooltipBubble(text: anchor.text, maxWidth: min(360, available.width))
        let layout = LauncherTooltipLayout(anchor: anchorFrame, size: bubble.frame.size, available: available, preferred: anchor.placement)
        bubble.above = layout.above; bubble.arrowX = layout.arrowX
        let tip: NSPanel
        if let panel { tip = panel }
        else {
            tip = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
            tip.identifier = NSUserInterfaceItemIdentifier("launcher.tooltip")
            tip.isReleasedWhenClosed = false; tip.backgroundColor = .clear; tip.isOpaque = false
            tip.hasShadow = true; tip.ignoresMouseEvents = true; tip.hidesOnDeactivate = true
            tip.animationBehavior = .none; tip.collectionBehavior = [.transient, .ignoresCycle, .fullScreenAuxiliary]
            panel = tip
        }
        tip.contentView = bubble
        tip.setFrame(layout.frame, display: true)
        if tip.parent !== owner { LauncherWindowPresenter.current.attach(tip, owner) }
        LauncherWindowPresenter.current.orderFront(tip); tip.invalidateShadow()
    }

    deinit {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        if let clickMonitor { NSEvent.removeMonitor(clickMonitor) }
        panel?.orderOut(nil)
    }
}

struct LauncherTooltipLayout {
    let frame: CGRect
    let above: Bool
    let arrowX: CGFloat
    init(anchor: CGRect, size: CGSize, available: CGRect, preferred: LauncherTooltipPlacement) {
        let prefersAbove = preferred == .above || (preferred == .automatic && anchor.midY < available.midY)
        let aboveY = anchor.maxY + 6, belowY = anchor.minY - size.height - 6
        let aboveFits = aboveY + size.height <= available.maxY, belowFits = belowY >= available.minY
        above = prefersAbove ? (aboveFits || !belowFits) : (!belowFits && aboveFits)
        let x = max(available.minX, min(anchor.midX - size.width / 2, available.maxX - size.width))
        let y = max(available.minY, min(above ? aboveY : belowY, available.maxY - size.height))
        frame = CGRect(origin: CGPoint(x: x, y: y), size: size)
        let arrowInset = min(size.width / 2, (size.height - 7) / 2 + 10)
        arrowX = max(arrowInset, min(anchor.midX - x, size.width - arrowInset))
    }
}

/// One solid silhouette joins the capsule and its softly curved pointer, with no outline.
final class LauncherTooltipBubble: NSView {
    var above = true { didSet { needsDisplay = true; layoutLabel() } }
    var arrowX: CGFloat = 0 { didSet { needsDisplay = true } }
    private let label: NSTextField
    init(text: String, maxWidth: CGFloat) {
        label = NSTextField(labelWithString: text)
        let font = NSFont.systemFont(ofSize: 13, weight: .medium)
        label.font = font; label.textColor = .white; label.maximumNumberOfLines = 3
        label.lineBreakMode = .byWordWrapping
        // NSTextField's cell adds horizontal and vertical insets beyond the glyph bounds.
        let measured = (text as NSString).boundingRect(with: CGSize(width: maxWidth - 28, height: font.boundingRectForFont.height * 3),
            options: [.usesLineFragmentOrigin, .usesFontLeading], attributes: [.font: font])
        let size = CGSize(width: min(maxWidth, max(56, ceil(measured.width) + 28)), height: max(30, ceil(measured.height) + 16) + 7)
        super.init(frame: CGRect(origin: .zero, size: size))
        addSubview(label); layoutLabel()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override var isOpaque: Bool { false }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    private func layoutLabel() { label.frame = CGRect(x: 12, y: (above ? 7 : 0) + 7, width: bounds.width - 24, height: bounds.height - 21) }
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        let body = CGRect(x: 0, y: above ? 7 : 0, width: bounds.width, height: bounds.height - 7)
        NSBezierPath(roundedRect: body, xRadius: body.height / 2, yRadius: body.height / 2).fill()
        func point(_ x: CGFloat, _ y: CGFloat) -> NSPoint { NSPoint(x: arrowX + x, y: above ? y : bounds.height - y) }
        let pointer = NSBezierPath()
        pointer.move(to: point(-10, 9))
        pointer.curve(to: point(-2, 1.5), controlPoint1: point(-6, 9), controlPoint2: point(-4, 3))
        pointer.curve(to: point(2, 1.5), controlPoint1: point(-0.7, 0.2), controlPoint2: point(0.7, 0.2))
        pointer.curve(to: point(10, 9), controlPoint1: point(4, 3), controlPoint2: point(6, 9))
        pointer.close(); pointer.fill()
    }
}
