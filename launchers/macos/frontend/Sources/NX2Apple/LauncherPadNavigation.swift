// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import Combine
import SwiftUI
import LauncherPadBridge
import NXLauncher

struct LauncherPadTarget {
    let id: String
    let frame: CGRect
    let role: String
    let element: Any
    var unclippedFrame: CGRect? = nil
    var clippingFrame: CGRect? = nil
    var ringRadius: CGFloat? = nil
    var ringOutset: CGFloat = 0
}

enum LauncherPadSpatial {
    /// Accessibility frames use screen coordinates (up is positive Y).
    static func next(from current: CGRect, targets: [LauncherPadTarget], action: LauncherPadAction) -> LauncherPadTarget? {
        let horizontal = action == .left || action == .right
        let sign: CGFloat = action == .left || action == .down ? -1 : 1
        func verticalGap(_ frame: CGRect) -> CGFloat {
            action == .up ? frame.minY - current.maxY : current.minY - frame.maxY
        }
        let candidates = targets.compactMap { target -> (LauncherPadTarget, CGFloat, Bool)? in
            let along = horizontal ? (target.frame.midX - current.midX) * sign : (target.frame.midY - current.midY) * sign
            guard along > 4 else { return nil }
            // Different-height controls in the same row still belong to left/right.
            guard horizontal || verticalGap(target.frame) >= -4 else { return nil }
            let across = horizontal ? abs(target.frame.midY - current.midY) : abs(target.frame.midX - current.midX)
            let overlap = horizontal ? target.frame.minY < current.maxY && target.frame.maxY > current.minY
                : target.frame.minX < current.maxX && target.frame.maxX > current.minX
            return (target, along + across * 2, overlap)
        }
        // Visit the adjacent row before preserving the column. Otherwise a lone
        // control at the far end of a row can never be reached from the row below.
        let row: [(LauncherPadTarget, CGFloat, Bool)]
        if !horizontal, let nearest = candidates.min(by: { a, b in
            verticalGap(a.0.frame) < verticalGap(b.0.frame)
        }) {
            row = candidates.filter { $0.0.frame.minY < nearest.0.frame.maxY && $0.0.frame.maxY > nearest.0.frame.minY }
        } else { row = candidates }
        let aligned = row.filter { $0.2 }
        let pool = horizontal || !aligned.isEmpty ? aligned : row
        return pool.min { $0.1 < $1.1 }?.0
    }
}

/// One focus owner for the whole app: SwiftUI views, titlebar hosting views,
/// sheets, popovers, native file panels and menus. Only this process's own public
/// accessibility objects are inspected; no Accessibility permission or global events.
@MainActor final class LauncherPadNavigation: ObservableObject, PadControlRegistry {
    @Published private(set) var focusedID: String?
    @Published private(set) var visible = false
    let source = LauncherPadSource()
    let presentation = PadPresentation()
    var language = "zh-Hans"
    var text: (String) -> String = { $0 }
    var onAction: ((LauncherPadAction) -> Bool)?
    var defaultFocus: () -> String? = { nil }
    var context: () -> String = { "home" }
    private(set) weak var window: NSWindow?
    private var scope = ""
    private var memories: [String: String] = [:]
    private var overlays: [Int: LauncherPadRing] = [:]
    private var monitors: [Any] = []
    private var notifications: [NSObjectProtocol] = []
    private var trackingMenu = false
    var keyboard: LauncherPadKeyboard?
    private var controls: [UUID: PadControl] = [:]
    private var geometryRefreshPending = false
    private let nativeButtons = LauncherPadNativeButtons()

    func register(_ control: PadControl) {
        controls[control.token] = control
        requestGeometryRefresh()
    }
    func removeControl(_ token: UUID) { controls[token] = nil; requestGeometryRefresh() }
    /// 「鼠标点了这个控件」：执行它的指针动作（PadControl.pointer，没有就是它的 activate = 按钮自己的动作）。只看当前操作的窗口；找不到返回 false
    func pointerActivate(_ id: String) -> Bool {
        guard let window = activeWindow, let control = controls.values.first(where: { $0.id == id && $0.enabled && $0.view.window === window }) else { return false }
        (control.pointer ?? control.activate)()
        return true
    }
    func isFullyVisible(_ id: String) -> Bool {
        guard let control = controls.values.first(where: { $0.id == id && $0.view.window === activeWindow }) else { return false }
        let frame = Self.screenFrame(control.view)
        return Self.clippingFrame(control.view).insetBy(dx: -1, dy: -1).contains(frame)
    }
    private static func screenFrame(_ view: NSView) -> CGRect {
        view.window?.convertToScreen(view.convert(view.bounds, to: nil)) ?? .zero
    }
    /// visibleRect is not reliable for SwiftUI's background representables: their
    /// AppKit layer can extend past the ScrollView. Intersect the actual clip ancestors.
    private static func clippingFrame(_ view: NSView) -> CGRect {
        guard let window = view.window, let content = window.contentView else { return .zero }
        var clip = screenFrame(content)
        var ancestor = view.superview
        while let current = ancestor {
            if current is NSClipView { clip = clip.intersection(screenFrame(current)) }
            ancestor = current.superview
        }
        return clip
    }
    private func registeredControls(in window: NSWindow) -> [PadControl] {
        let registered = controls.values.filter { $0.view.window === window && !$0.view.isHiddenOrHasHiddenAncestor }
        // 展开的下拉最优先；其次是面板里的浮层（scope "layer"，例如手柄面板的四键布局）：开着时方向键只在浮层里走，不落到底下的面板
        let modal = registered.filter { $0.scope == "dropdown" && controlTarget($0) != nil }
        let layer = registered.filter { $0.scope == "layer" && controlTarget($0) != nil }
        return (!modal.isEmpty ? modal : !layer.isEmpty ? layer : Array(registered)).filter { $0.enabled && !$0.shortcutOnly }
    }
    private func controlTarget(_ control: PadControl, includeOffscreen: Bool = false) -> LauncherPadTarget? {
        let full = Self.screenFrame(control.view), clip = Self.clippingFrame(control.view)
        let visible = full.intersection(clip)
        let frame = includeOffscreen ? full : visible
        guard frame.width > 2, frame.height > 2 else { return nil }
        return LauncherPadTarget(id: control.id, frame: frame, role: "AXButton", element: control.view, unclippedFrame: full, clippingFrame: clip, ringRadius: control.ringRadius, ringOutset: control.ringOutset)
    }
    func controlTargets(in window: NSWindow) -> [LauncherPadTarget] {
        registeredControls(in: window).compactMap { controlTarget($0) }.sorted(by: Self.order)
    }
    /// 单测失败时的现场：这个窗口登记的全部控件（含被筛掉的）与筛掉原因，按 id 排序一行一个
    func registrationReport(in window: NSWindow) -> String {
        controls.values.filter { $0.view.window === window }.sorted { $0.id < $1.id }.map { c in
            let full = Self.screenFrame(c.view), clip = Self.clippingFrame(c.view)
            return "\(c.id) scope=\(c.scope) enabled=\(c.enabled) shortcutOnly=\(c.shortcutOnly) hidden=\(c.view.isHiddenOrHasHiddenAncestor) frame=\(full) visible=\(full.intersection(clip))"
        }.joined(separator: "\n")
    }
    func shortcutTargets(in window: NSWindow) -> [PadShortcut: [String]] {
        var result: [PadShortcut: [String]] = [:]
        for control in controls.values where control.view.window === window && !control.view.isHiddenOrHasHiddenAncestor && controlTarget(control) != nil {
            for shortcut in control.shortcuts where shortcut != .rightTrigger || !control.shortcutsRequireFocus || control.id == focusedID {
                result[shortcut, default: []].append(control.id)
            }
        }
        return result
    }
    func focusRingFrame(in window: NSWindow) -> CGRect? {
        guard let overlay = overlays[window.windowNumber], let target = overlay.target else { return nil }
        return window.convertToScreen(overlay.convert(target.intersection(overlay.clip ?? overlay.bounds), to: nil))
    }
    private func requestGeometryRefresh() {
        guard visible, !geometryRefreshPending else { return }
        geometryRefreshPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.geometryRefreshPending = false
            self.refreshFocusGeometry()
        }
    }
    func refreshFocusGeometry() {
        guard visible, !trackingMenu, let window = activeWindow else { clearRings(); return }
        let previousID = focusedID, previousScope = scope
        let old = overlays[window.windowNumber]?.screenTarget
        let all = prepare(in: window)
        if previousScope == scope, let id = previousID, !id.hasPrefix("card.") {
            let selected = all.first { $0.id == id }
            let complete = selected.map { ($0.clippingFrame ?? window.frame).insetBy(dx: -1, dy: -1).contains($0.unclippedFrame ?? $0.frame) } ?? false
            let scroll = controls.values.first { $0.id == id && $0.view.window === window }?.view.enclosingScrollView
            // 只有滚动区里的控件被滚出视野才换到附近的完整控件。不在滚动区里的控件（顶栏搜索框）被裁只是展开 / 收起动画的一瞬，
            // 这时换走焦点就再也回不来：手柄关屏幕键盘后焦点跳到「刷新」「清空」
            if !complete, scroll != nil {
                let completeTargets = all.filter { ($0.clippingFrame ?? window.frame).insetBy(dx: -1, dy: -1).contains($0.unclippedFrame ?? $0.frame) }
                let nearby = completeTargets.filter { ($0.element as? NSView)?.enclosingScrollView === scroll }
                let candidates = nearby.isEmpty ? completeTargets : nearby
                focusedID = (selected?.frame ?? old).flatMap { frame in candidates.min { a, b in
                hypot(a.frame.midX - frame.midX, a.frame.midY - frame.midY) < hypot(b.frame.midX - frame.midX, b.frame.midY - frame.midY)
                }?.id } ?? focusedID
            }
        }
        draw(all.first { $0.id == focusedID }, in: window)
    }
    private func reveal(_ id: String, in window: NSWindow) {
        if let control = controls.values.first(where: { $0.id == id && $0.view.window === window }) {
            control.view.scrollToVisible(control.view.bounds.insetBy(dx: -6, dy: -6))
        }
    }
    private func nextInScroll(from selected: LauncherPadTarget, action: LauncherPadAction, window: NSWindow) -> LauncherPadTarget? {
        guard action == .up || action == .down,
              let view = selected.element as? NSView, let scroll = view.enclosingScrollView else { return nil }
        let candidates = registeredControls(in: window).filter {
            $0.id != selected.id && $0.view.enclosingScrollView === scroll
        }.compactMap { controlTarget($0, includeOffscreen: true) }
        return LauncherPadSpatial.next(from: selected.unclippedFrame ?? selected.frame, targets: candidates, action: action)
    }
    private static func order(_ a: LauncherPadTarget, _ b: LauncherPadTarget) -> Bool {
        let ay = Int(a.frame.midY / 24), by = Int(b.frame.midY / 24)
        if ay != by { return ay > by }
        if a.frame.midX != b.frame.midX { return a.frame.midX < b.frame.midX }
        return a.id < b.id
    }

    func attach(_ window: NSWindow) {
        guard self.window !== window else { return }
        self.window = window
        source.receive = { [weak self] action in self?.handle(action) }
        source.updatePresentation = { [weak self] connected, symbols in
            guard let self else { return }
            if self.presentation.symbols != symbols { self.presentation.symbols = symbols }
            if self.presentation.connected != connected {
                self.presentation.connected = connected
                if !connected { self.hide(); self.nativeButtons.restore() }
            }
        }
        source.refresh = { [weak self] in
            guard let self, let window = self.activeWindow else { return }
            if !self.controls.values.contains(where: { $0.view.window === window }) {
                self.nativeButtons.update(in: window, presentation: self.presentation)
            }
        }
        source.start()
        if monitors.isEmpty {
            if let monitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown], handler: { [weak self] event in
                self?.hide(); return event
            }) { monitors.append(monitor) }
            let center = NotificationCenter.default
            notifications.append(center.addObserver(forName: NSView.boundsDidChangeNotification, object: nil, queue: .main) { [weak self] note in
                MainActor.assumeIsolated {
                    guard let clip = note.object as? NSClipView, clip.window === self?.activeWindow else { return }
                    self?.requestGeometryRefresh()
                }
            })
            for name in [NSWindow.didResizeNotification, NSWindow.didMoveNotification, NSWindow.didEndSheetNotification, NSWindow.didBecomeKeyNotification] {
                notifications.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.requestGeometryRefresh() }
                })
            }
            notifications.append(center.addObserver(forName: NSMenu.didBeginTrackingNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.trackingMenu = true; self?.clearRings() }
            })
            notifications.append(center.addObserver(forName: NSMenu.didEndTrackingNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.trackingMenu = false }
            })
            notifications.append(center.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.clearRings() }
            })
            notifications.append(center.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { [weak self] note in
                MainActor.assumeIsolated {
                    guard let self, let closed = note.object as? NSWindow else { return }
                    if closed === self.window { self.detach() }
                    else {
                        self.overlays.removeValue(forKey: closed.windowNumber)?.removeFromSuperview()
                        self.memories = self.memories.filter { !$0.key.hasPrefix("\(closed.windowNumber):") }
                    }
                }
            })
        }
    }
    /// teardown：SwiftUI 正在拆这棵视图（dismantleNSView，NSHostingView.deinit 里）。这时不写 presentation 的发布属性——
    ///   拆图期间发布变更会撞上 SwiftUI 自己的独占访问，fatalError 关窗闪退（#1516，0.5.3–0.5.5）。拆完 presentation 随视图释放，
    ///   重新接上窗口时 source.updatePresentation 照实写；窗口关闭通知那条路（图还在）照旧清
    func detach(teardown: Bool = false) {
        source.stop(); clearRings(); keyboard?.close(apply: false); keyboard = nil
        nativeButtons.restore(); source.refresh = {}; source.updatePresentation = { _, _ in }
        if !teardown { presentation.connected = false; presentation.symbols = [:] }
        for monitor in monitors { NSEvent.removeMonitor(monitor) }; monitors.removeAll()
        for observer in notifications { NotificationCenter.default.removeObserver(observer) }; notifications.removeAll()
        for overlay in overlays.values { overlay.removeFromSuperview() }; overlays.removeAll()
        onAction = nil; defaultFocus = { nil }; context = { "home" }; text = { $0 }; source.relayPath = { nil }; source.relayAllowed = { true }; source.relayConnected = { false }
        source.assignment = { PlayerAssignment() }; source.faceMapping = { _ in .identity }
        controls.removeAll(); window = nil
    }
    func hide() { visible = false; clearRings() }
    func releaseRelay() { source.releaseRelay() }

    var activeWindow: NSWindow? {
        // 弹窗里再开的弹窗（手柄面板 → 齿轮选单）：一路往下找到最上层看得见的那个，焦点与按键才进得去
        guard let base = baseActiveWindow else { return nil }
        var top = base
        while let child = LauncherWindowPresenter.current.children(top).last(where: isOperableChild) { top = child }
        return top
    }
    /// 能操作的子窗口：看得见、里面有控件（注册的或原生的）。macOS 27 挂在提示框 / 面板下的玻璃背景窗口（_NSGlassEffectWindow）没有控件，不算——
    ///   否则手柄按键都送到它身上，提示框的确认 / 取消没反应
    private func isOperableChild(_ child: NSWindow) -> Bool {
        LauncherWindowPresenter.current.isShown(child) && (controls.values.contains { $0.view.window === child } || child.contentView.map(Self.holdsControl) == true)
    }
    private var baseActiveWindow: NSWindow? {
        guard let window else { return nil }
        if let keyboard, LauncherWindowPresenter.current.isShown(keyboard.panel) { return keyboard.panel }
        if let modal = NSApp.modalWindow { return modal }
        if let sheet = window.attachedSheet { return sheet.attachedSheet ?? sheet }
        if let key = NSApp.keyWindow, key !== window, LauncherWindowPresenter.current.isShown(key) { return key }
        // macOS 27 hangs a decorative glass backdrop window under alerts and panels: visible, but it holds nothing to operate.
        if let child = LauncherWindowPresenter.current.children(window).last(where: isOperableChild) { return child }
        if let popup = LauncherWindowPresenter.current.orderedWindows().first(where: { candidate in
            candidate !== window && LauncherWindowPresenter.current.isShown(candidate) && !candidate.canBecomeMain && controls.values.contains { $0.view.window === candidate }
        }) { return popup }
        return window
    }
    private static func holdsControl(_ view: NSView) -> Bool {
        view is NSControl || view.subviews.contains(where: holdsControl)
    }
    var inMainContent: Bool { activeWindow === window && !trackingMenu && !hasDropdown }
    private var hasDropdown: Bool {
        guard let window = activeWindow else { return false }
        if controls.values.contains(where: { $0.scope == "dropdown" && $0.view.window === window && !$0.view.isHiddenOrHasHiddenAncestor && controlTarget($0) != nil }) { return true }
        return Self.elements(window).contains { S2ALauncherAX.identifier($0) == "launcher.dropdown" }
    }

    static func elements(_ root: Any) -> [Any] {
        var result: [Any] = [], seen = Set<ObjectIdentifier>()
        func visit(_ item: Any) {
            let object = item as AnyObject
            guard seen.insert(ObjectIdentifier(object)).inserted else { return }
            result.append(item)
            for child in S2ALauncherAX.children(item) ?? [] { visit(child) }
        }
        visit(root)
        return result
    }
    func targets(in window: NSWindow) -> [LauncherPadTarget] {
        if !controls.values.contains(where: { $0.view.window === window }) { nativeButtons.update(in: window, presentation: presentation) }
        let registered = controlTargets(in: window)
        if controls.values.contains(where: { $0.scope == "dropdown" && $0.view.window === window && !$0.view.visibleRect.isEmpty }) { return registered }
        let all = Self.elements(window)
        let dropdown = all.first { S2ALauncherAX.identifier($0) == "launcher.dropdown" }
        let elements = dropdown.map(Self.elements) ?? all
        let roles: Set<String> = ["AXButton", "AXCheckBox", "AXRadioButton", "AXPopUpButton", "AXMenuButton", "AXMenuItem", "AXTextField", "AXTextArea", "AXComboBox", "AXSlider", "AXIncrementor", "AXDisclosureTriangle", "AXLink", "AXRow"]
        let native: [LauncherPadTarget] = elements.enumerated().compactMap { index, element in
            let role = S2ALauncherAX.role(element) ?? ""
            guard roles.contains(role), S2ALauncherAX.enabled(element) else { return nil }
            let frame = S2ALauncherAX.frame(element)
            guard frame.width > 2, frame.height > 2, window.frame.intersects(frame) else { return nil }
            if role == "AXButton", frame.width <= 18, frame.height <= 18 { return nil }
            // Also exclude clipped/disabled registered controls from the native
            // fallback; otherwise AX would reintroduce their unbounded frames.
            if controls.values.contains(where: { !$0.shortcutOnly && $0.view.window === window && Self.screenFrame($0.view).insetBy(dx: -2, dy: -2).contains(CGPoint(x: frame.midX, y: frame.midY)) }) { return nil }
            let view = (element as? NSView) ?? (element as? NSCell)?.controlView
            if let view, view.isHiddenOrHasHiddenAncestor { return nil }
            let clip = view.map(Self.clippingFrame) ?? window.frame
            let visibleFrame = frame.intersection(clip)
            guard visibleFrame.width > 2, visibleFrame.height > 2 else { return nil }
            let explicit = S2ALauncherAX.identifier(element) ?? ""
            if ["AXCloseButton", "AXMinimizeButton", "AXZoomButton"].contains(explicit) || PadExclusion.ids.contains(explicit) { return nil }
            let id = explicit.isEmpty ? "ax.\(role).\(S2ALauncherAX.label(element) ?? "").\(index)" : explicit
            return LauncherPadTarget(id: id, frame: visibleFrame, role: role, element: element, unclippedFrame: frame, clippingFrame: clip)
        }
        return (registered + native).sorted(by: Self.order)
    }
    func focus(_ id: String) {
        visible = true
        guard let window = activeWindow else { focusedID = id; return }
        _ = prepare(in: window)
        focusedID = id
        memories[scope] = id
        reveal(id, in: window)
        draw(targets(in: window).first { $0.id == id }, in: window)
    }
    private func prepare(in window: NSWindow) -> [LauncherPadTarget] {
        let nextScope = "\(window.windowNumber):\(context()):\(hasDropdown ? "dropdown" : "page")"
        let targets = targets(in: window)
        if nextScope != scope {
            if let focusedID { memories[scope] = focusedID }
            scope = nextScope
            if !targets.contains(where: { $0.id == focusedID }) { focusedID = memories[scope] }
        }
        if !targets.contains(where: { $0.id == focusedID }) && !(window === self.window && focusedID?.hasPrefix("card.") == true) {
            let registeredDefault = controls.values.first { $0.preferred && $0.enabled && $0.view.window === window && !$0.view.visibleRect.isEmpty }?.id
            // 没指定时，焦点先落在这页的确认钮（ZR 那颗，界面显示 A）：进页面直接按 A 就是确认（单只 Joy-Con 没有 ZR）
            let submit = controls.values.first { $0.shortcuts.contains(.rightTrigger) && !$0.shortcutsRequireFocus && $0.enabled
                && $0.view.window === window && !$0.view.visibleRect.isEmpty }?.id
            // 原生提示框（没注册的控件）：焦点先落在它的确认钮，按 A 就是确认
            let nativeSubmit = controls.values.contains(where: { $0.view.window === window }) ? nil
                : nativeButtons.buttons(in: window).first(where: { $0.0 == .rightTrigger }).flatMap { pair in
                    targets.first { ($0.element as? NSView) === pair.1 || ($0.element as? NSCell)?.controlView === pair.1 }?.id }
            let preferred = registeredDefault ?? (window === self.window ? defaultFocus() : nil) ?? submit ?? nativeSubmit
            let content = window === self.window ? targets.filter { $0.frame.maxY < window.frame.maxY - 64 } : targets
            focusedID = preferred.flatMap { id in targets.first(where: { $0.id == id })?.id } ?? content.first?.id ?? targets.first?.id
        }
        return targets
    }
    func handle(_ action: LauncherPadAction) {
        guard let currentWindow = activeWindow else { return }
        if !visible, currentWindow === window, let preferred = defaultFocus() { focusedID = preferred }
        visible = true
        if trackingMenu {
            switch action {
            case .confirm, .submit: sendKey(36, "\r", in: currentWindow)
            case .back: sendKey(53, "\u{1b}", in: currentWindow)
            case .up: sendKey(126, "\u{f700}", in: currentWindow)
            case .down: sendKey(125, "\u{f701}", in: currentWindow)
            case .left: sendKey(123, "\u{f702}", in: currentWindow)
            case .right: sendKey(124, "\u{f703}", in: currentWindow)
            default: break
            }
            return
        }
        let all = prepare(in: currentWindow)
        if let keyboard, currentWindow === keyboard.panel, keyboard.handle(action) { return }
        if performShortcut(action, in: currentWindow) { return }
        if inMainContent, onAction?(action) == true { return }
        let selected = all.first { $0.id == focusedID }
        switch action {
        case .confirm:
            guard let selected else { return }
            if let control = controls.values.first(where: { $0.id == selected.id && $0.enabled && $0.view.window === currentWindow }) { control.activate(); break }
            if ["AXTextField", "AXTextArea", "AXComboBox"].contains(selected.role) { edit(selected, window: currentWindow) }
            else if !S2ALauncherAX.press(selected.element) {
                S2ALauncherAX.focus(selected.element)
                if !S2ALauncherAX.pick(selected.element) { sendKey(36, "\r", in: currentWindow) }
            }
        case .back:
            // B 只关最里面那一层：开着的下拉选单先关（选单没有自己的「返回」钮，落到窗口取消会把整个面板关掉）
            if hasDropdown { NotificationCenter.default.post(name: .launcherDropdownClose, object: currentWindow); break }
            if S2ALauncherAX.cancel(currentWindow) { break }
            // 不再发 Esc（会触发 macOS 系统提示音「咚」）：主窗口无层可关时静默忽略，弹窗/浮层直接用 AppKit 关
            if currentWindow !== window {
                if let sheetParent = currentWindow.sheetParent { sheetParent.endSheet(currentWindow) }
                else if let panel = currentWindow as? NSSavePanel { panel.cancel(nil) }
                else { currentWindow.close() }
            }
        case .left, .right, .up, .down:
            let step = action == .left ? -1 : 1
            if action == .left || action == .right,
               let control = controls.values.first(where: { $0.id == focusedID && $0.enabled && $0.view.window === currentWindow }), let adjust = control.adjust { adjust(step); break }
            if let selected, ["AXSlider", "AXIncrementor"].contains(selected.role), action == .left || action == .right,
               S2ALauncherAX.adjust(selected.element, step: step) { break }
            if let selected, let next = nextInScroll(from: selected, action: action, window: currentWindow)
                ?? LauncherPadSpatial.next(from: selected.frame, targets: all.filter { $0.id != selected.id }, action: action) {
                focus(next.id)
                if next.role == "AXRow" { _ = S2ALauncherAX.pick(next.element) }
            } else if let selected, action == .up || action == .down,
                      scroll(action == .up ? -1 : 1, page: false, window: currentWindow, selected: selected) {
                let oldScope = scope
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.06) { [weak self, weak currentWindow] in
                    guard let self, let currentWindow, self.activeWindow === currentWindow, self.scope == oldScope else { return }
                    let fresh = self.targets(in: currentWindow)
                    let from = fresh.first { $0.id == selected.id } ?? selected
                    if let next = LauncherPadSpatial.next(from: from.frame, targets: fresh.filter { $0.id != selected.id }, action: action) { self.focus(next.id) }
                }
            }
        case .previousGroup: linear(-1, all: all)
        case .nextGroup: linear(1, all: all)
        case .scrollUp, .pageUp: scroll(-1, page: action == .pageUp, window: currentWindow, selected: selected)
        case .scrollDown: scroll(1, page: false, window: currentWindow, selected: selected)
        default: break
        }
        if let focusedID { memories[scope] = focusedID }
        draw(targets(in: currentWindow).first { $0.id == focusedID }, in: currentWindow)
    }
    private func performShortcut(_ action: LauncherPadAction, in window: NSWindow) -> Bool {
        let shortcut: PadShortcut
        switch action {
        case .submit: shortcut = .rightTrigger
        case .back: shortcut = .back
        case .filter: shortcut = .secondary
        case .details: shortcut = .tertiary
        default: return false
        }
        let dropdown = hasDropdown
        let matches = controls.values.filter {
            $0.view.window === window && !$0.view.isHiddenOrHasHiddenAncestor && $0.shortcuts.contains(shortcut)
                && (!dropdown || $0.scope == "dropdown") && controlTarget($0) != nil
                && (shortcut != .rightTrigger || !$0.shortcutsRequireFocus || $0.id == focusedID)
        }
        // A disabled submit/cancel must not fall through to Escape or to the page underneath.
        guard let control = matches.first(where: { $0.id == focusedID }) ?? matches.first else {
            guard !controls.values.contains(where: { $0.view.window === window }),
                  let button = nativeButtons.buttons(in: window).first(where: { $0.0 == shortcut })?.1 else { return false }
            if button.isEnabled { button.performClick(nil) }
            return true
        }
        if control.enabled { control.activate() }
        requestGeometryRefresh()
        return true
    }
    private func linear(_ step: Int, all: [LauncherPadTarget]) {
        guard !all.isEmpty else { return }
        let index = all.firstIndex { $0.id == focusedID } ?? 0
        focus(all[(index + step + all.count) % all.count].id)
    }
    func activateFocused() { handle(.confirm) }
    func editFocused() {
        guard let window = activeWindow, let target = targets(in: window).first(where: { $0.id == focusedID }) else { return }
        edit(target, window: window)
    }
    func editText(value: String, label: String, onClose: @escaping () -> Void = {}, apply: @escaping (String) -> Void) {
        guard keyboard == nil, let window = activeWindow else { return }
        keyboard = LauncherPadKeyboard(value: value, label: label, owner: window, navigation: self, onClose: onClose, apply: apply)
        keyboard?.show()
    }
    private func edit(_ target: LauncherPadTarget, window: NSWindow) {
        guard keyboard == nil else { return }
        keyboard = LauncherPadKeyboard(element: target.element, owner: window, navigation: self)
        keyboard?.show()
    }
    func sendKey(_ code: UInt16, _ characters: String, in window: NSWindow) {
        for type in [NSEvent.EventType.keyDown, .keyUp] {
            if let event = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: window.windowNumber, context: nil, characters: characters, charactersIgnoringModifiers: characters, isARepeat: false, keyCode: code) {
                NSApp.postEvent(event, atStart: false)
            }
        }
    }
    @discardableResult private func scroll(_ step: Int, page: Bool, window: NSWindow, selected: LauncherPadTarget?) -> Bool {
        func scrollViews(_ view: NSView) -> [NSScrollView] { (view as? NSScrollView).map { [$0] } ?? view.subviews.flatMap(scrollViews) }
        guard let content = window.contentView else { return false }
        let views = scrollViews(content).filter { $0.hasVerticalScroller && !$0.isHidden }
        let scrollView = views.first { view in
            guard let selected else { return true }
            return view.convert(view.bounds, to: nil).intersects(window.convertFromScreen(selected.frame))
        } ?? views.first
        guard let scrollView, let document = scrollView.documentView else { return false }
        var origin = scrollView.contentView.bounds.origin
        let before = origin
        let distance = page ? scrollView.contentView.bounds.height * 0.85 : 65
        origin.y += CGFloat(step) * distance * (document.isFlipped ? 1 : -1)
        origin.y = max(0, min(origin.y, max(0, document.bounds.height - scrollView.contentView.bounds.height)))
        scrollView.contentView.scroll(to: origin); scrollView.reflectScrolledClipView(scrollView.contentView)
        return origin != before
    }
    private func clearRings() { for overlay in overlays.values { overlay.target = nil } }
    private func draw(_ target: LauncherPadTarget?, in window: NSWindow) {
        clearRings()
        guard visible, let target, !target.id.hasPrefix("card."), let container = window.contentView?.superview else { return }
        let overlay: LauncherPadRing
        if let existing = overlays[window.windowNumber], existing.superview === container { overlay = existing }
        else {
            overlay = LauncherPadRing(frame: container.bounds)
            overlay.autoresizingMask = [.width, .height]
            container.addSubview(overlay, positioned: .above, relativeTo: nil)
            overlays[window.windowNumber] = overlay
        }
        overlay.screenTarget = target.frame
        overlay.clip = target.clippingFrame.map { overlay.convert(window.convertFromScreen($0), from: nil) }
        overlay.radius = target.ringRadius
        overlay.outset = target.ringOutset
        overlay.target = overlay.convert(window.convertFromScreen(target.unclippedFrame ?? target.frame), from: nil)
    }
}

private final class LauncherPadRing: NSView {
    var screenTarget: CGRect?
    var clip: CGRect?
    var target: CGRect? { didSet { needsDisplay = true } }
    var outset: CGFloat = 0   // 奶油底控件：框在外面、同心
    var radius: CGFloat?   // 控件自报的圆角：有就贴边画；没有（无边框的文字钮、系统控件）照旧往外 3 pt
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func isAccessibilityElement() -> Bool { false }
    override func draw(_ dirtyRect: NSRect) {
        guard let target else { return }
        NSGraphicsContext.saveGraphicsState()
        defer { NSGraphicsContext.restoreGraphicsState() }
        if let clip { NSBezierPath(rect: clip).addClip() }
        NSColor(srgbRed: 1, green: 0.94, blue: 0.84, alpha: 1).setStroke()
        let width = LauncherTheme.Focus.lineWidth
        guard let radius else {
            let r = LauncherTheme.Focus.plainRadius
            let ring = NSBezierPath(roundedRect: target.insetBy(dx: -LauncherTheme.Focus.plainPaddingX, dy: -LauncherTheme.Focus.plainPaddingY), xRadius: r, yRadius: r)
            ring.lineWidth = width; ring.stroke(); return
        }
        // 有边框的控件：焦点框画在它自己的边框上（同一外缘、同一圆角），与按钮边框对齐
        let edge = target.insetBy(dx: -outset, dy: -outset)
        let r = max(0, min(radius, target.height / 2, target.width / 2) + outset - width / 2)
        let ring = NSBezierPath(roundedRect: edge.insetBy(dx: width / 2, dy: width / 2), xRadius: r, yRadius: r)
        ring.lineWidth = width; ring.stroke()
    }
}

struct LauncherPadHost: NSViewRepresentable {
    let navigation: LauncherPadNavigation
    let update: () -> Void
    final class Host: NSView {
        var navigation: LauncherPadNavigation?
        weak var lastWindow: NSWindow?
        override func viewDidMoveToWindow() { super.viewDidMoveToWindow(); if let window { lastWindow = window; navigation?.attach(window) } }
    }
    func makeNSView(context: Context) -> Host { let host = Host(); host.navigation = navigation; return host }
    func updateNSView(_ view: Host, context: Context) { update(); if let window = view.window { navigation.attach(window) } }
    static func dismantleNSView(_ view: Host, coordinator: ()) { if view.navigation?.window === view.lastWindow { view.navigation?.detach(teardown: true) } }
}
