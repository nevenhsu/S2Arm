// S2Arm（不是上游文件）：不上屏的「舞台」——单测用的 LauncherWindowPresenter（两个测试包 NXLauncherTests / S2ArmAppleTests 共用，所以放在这里；生产不调它）。
//   挂子窗口、显示、收起都只记账，不碰窗口服务器：子窗口不会把父窗口带上真桌面，结果不随桌面焦点、前台程序变，开发机跑单测也不弹窗。
//   弹出框（NSPopover）不 show：内容控制器放进一个离屏面板，记成锚点所在窗口的子窗口；关的时候照 AppKit 的顺序问 delegate、回调 popoverDidClose。
//   测试窗口自己用 stage(_:) 登记成「看得见」（它本身也不上屏）
import AppKit

final class LauncherOffscreenStage {
    private var shown = Set<ObjectIdentifier>()
    private var kids: [ObjectIdentifier: [NSWindow]] = [:]
    private var front: [NSWindow] = []   // 最近显示的在前
    private var hosts: [ObjectIdentifier: NSPanel] = [:]   // 弹出框 → 放它内容的离屏面板
    /// 用例走到的模态（不弹，回取消）：文件面板的标题 / 提示框的标题，按顺序
    private(set) var modals: [String] = []

    init() {}

    /// 装上（之后所有子窗口 / 弹出框都走这个舞台）；uninstall 换回 AppKit
    func install() { LauncherWindowPresenter.current = presenter }
    static func uninstall() { LauncherWindowPresenter.current = .appKit }

    /// 测试窗口登记成「看得见」（不上屏）
    func stage(_ window: NSWindow) { show(window) }

    func isShown(_ window: NSWindow) -> Bool { shown.contains(ObjectIdentifier(window)) }
    /// 显示着的窗口，最近显示的在前
    var shownWindows: [NSWindow] { front }
    func children(of window: NSWindow) -> [NSWindow] { kids[ObjectIdentifier(window)] ?? [] }
    /// 弹出框的内容所在的离屏面板（没在显示 → nil）
    func window(of popover: NSPopover) -> NSWindow? { hosts[ObjectIdentifier(popover)] }

    private var closing: [ObjectIdentifier: NSObjectProtocol] = [:]
    private func show(_ w: NSWindow) {
        shown.insert(ObjectIdentifier(w)); front.removeAll { $0 === w }; front.insert(w, at: 0)
        // 关掉的窗口同 AppKit 一样不再「看得见」，也不再挂在谁下面（用例多是直接 close，不经 orderOut；
        //   不收的话下一轮的导航会把上一轮关掉的窗口当成最上层的弹出窗口，11-26）
        if closing[ObjectIdentifier(w)] == nil {
            closing[ObjectIdentifier(w)] = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: w, queue: nil) { [weak self, weak w] _ in
                guard let self, let w else { return }
                self.forget(w)
            }
        }
    }
    private func forget(_ w: NSWindow) {
        for child in children(of: w) { forget(child) }   // 同 AppKit：父窗口关了，挂在它下面的子窗口（弹出框的离屏面板、屏幕键盘、提示）一起收
        for (k, panel) in hosts where panel === w { hosts[k] = nil }
        hide(w)
        for (k, list) in kids where list.contains(where: { $0 === w }) { kids[k] = list.filter { $0 !== w } }
        kids[ObjectIdentifier(w)] = nil
        if let o = closing.removeValue(forKey: ObjectIdentifier(w)) { NotificationCenter.default.removeObserver(o) }
    }
    private func hide(_ w: NSWindow) {
        shown.remove(ObjectIdentifier(w)); front.removeAll { $0 === w }
    }
    private func attach(_ child: NSWindow, _ owner: NSWindow) {
        var list = kids[ObjectIdentifier(owner)] ?? []
        if !list.contains(where: { $0 === child }) { list.append(child) }
        kids[ObjectIdentifier(owner)] = list
    }
    private func detach(_ child: NSWindow, _ owner: NSWindow) {
        kids[ObjectIdentifier(owner)]?.removeAll { $0 === child }
    }
    private func owner(of child: NSWindow) -> NSWindow? {
        for w in front where children(of: w).contains(where: { $0 === child }) { return w }
        return nil
    }

    private func modal(_ kind: String, _ title: String) {
        modals.append(kind + "：" + title)
        FileHandle.standardError.write(Data("【红】单测走到了模态\(kind)（不弹，回取消）：\(title)\n".utf8))
    }
    private func showPopover(_ popover: NSPopover, _ rect: NSRect, _ view: NSView, _ edge: NSRectEdge) {
        guard let anchorWindow = view.window, let content = popover.contentViewController else { return }
        let panel = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.isReleasedWhenClosed = false
        panel.appearance = popover.appearance
        panel.contentViewController = content
        let size = content.preferredContentSize.width > 0 ? content.preferredContentSize : content.view.fittingSize
        let anchor = anchorWindow.convertToScreen(view.convert(rect, to: nil))
        let y = edge == .minY ? anchor.minY - size.height : anchor.maxY   // 按钮下方 / 上方（同 NSPopover 的 preferredEdge，视图坐标不翻转时 minY 在下）
        panel.setFrame(CGRect(x: anchor.midX - size.width / 2, y: y, width: size.width, height: size.height), display: false)
        hosts[ObjectIdentifier(popover)] = panel
        attach(panel, anchorWindow); show(panel)
    }
    private func teardown(_ popover: NSPopover) -> Bool {
        guard let panel = hosts.removeValue(forKey: ObjectIdentifier(popover)) else { return false }
        if let o = owner(of: panel) { detach(panel, o) }
        hide(panel); panel.contentViewController = nil
        return true
    }
    private func closePopover(_ popover: NSPopover, ask: Bool) {
        if ask, let d = popover.delegate, d.popoverShouldClose?(popover) == false { return }
        guard teardown(popover) else { return }
        popover.delegate?.popoverDidClose?(Notification(name: NSPopover.didCloseNotification, object: popover))
    }

    var presenter: LauncherWindowPresenter {
        LauncherWindowPresenter(
            attach: { [unowned self] in attach($0, $1) },
            detach: { [unowned self] in detach($0, $1) },
            orderFront: { [unowned self] in show($0) },
            makeKeyAndOrderFront: { [unowned self] in show($0) },
            orderOut: { [unowned self] in hide($0) },
            showPopover: { [unowned self] in showPopover($0, $1, $2, $3) },
            closePopover: { [unowned self] in closePopover($0, ask: true) },
            dismissPopover: { [unowned self] in closePopover($0, ask: false) },
            isShown: { [unowned self] in isShown($0) },
            children: { [unowned self] in children(of: $0) },
            orderedWindows: { [unowned self] in front },
            runPanel: { [unowned self] panel in modal("文件面板", panel.title ?? panel.message ?? ""); return .cancel },
            runAlert: { [unowned self] alert in modal("提示框", alert.messageText); return .alertSecondButtonReturn })
    }
}
