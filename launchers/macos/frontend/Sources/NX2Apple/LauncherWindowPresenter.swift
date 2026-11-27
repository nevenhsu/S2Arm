// S2Arm（不是上游文件）：启动器子窗口上屏的唯一出口——即时提示（LauncherTooltip）、屏幕键盘（LauncherPadKeyboard）、手柄面板（LauncherView 的 AppKitPopover）。
//   生产 = AppKit 原样（appKit：每个钩子就是原来那一句调用，行为不变）。单测换成不上屏的版本（Tests 的 OffscreenPresenter）：
//   这几处一调 addChildWindow / orderFront / NSPopover.show，父窗口会被一起放上真桌面（11-26 探针：不上屏的父窗口挂了子窗口 / 弹出框后「可见」变 true），
//   测试结果就随桌面焦点、前台程序变（门禁偶发红），本机跑单测还会在开发者桌面上弹窗。
import AppKit

struct LauncherWindowPresenter {
    /// 把 child 挂到 owner 上方（owner.addChildWindow(child, ordered: .above)）
    var attach: (_ child: NSWindow, _ owner: NSWindow) -> Void
    /// 从 owner 上摘下 child（owner.removeChildWindow(child)）
    var detach: (_ child: NSWindow, _ owner: NSWindow) -> Void
    var orderFront: (NSWindow) -> Void
    var makeKeyAndOrderFront: (NSWindow) -> Void
    var orderOut: (NSWindow) -> Void
    /// NSPopover.show(relativeTo:of:preferredEdge:)
    var showPopover: (_ popover: NSPopover, _ rect: NSRect, _ view: NSView, _ edge: NSRectEdge) -> Void
    /// 要它关（NSPopover.performClose：会问 delegate、关了调 popoverDidClose）
    var closePopover: (NSPopover) -> Void
    /// 立刻关、不问 delegate（NSPopover.close：拆视图时用）
    var dismissPopover: (NSPopover) -> Void
    // 查询（手柄导航判「现在操作哪个窗口」LauncherPadNavigation.activeWindow）：上屏换了出口，看得见 / 挂在谁下面也得问同一个出口
    var isShown: (NSWindow) -> Bool
    var children: (NSWindow) -> [NSWindow]
    /// 从前到后的窗口（NSApp.orderedWindows）
    var orderedWindows: () -> [NSWindow]
    // 模态（单测里真弹出来没人关，整套测试就卡死在模态循环里、不吃 CPU：11-26 出包页用例按到「选封面」卡住）
    /// 文件面板（NSOpenPanel / NSSavePanel.runModal）
    var runPanel: (NSSavePanel) -> NSApplication.ModalResponse
    /// 提示框（NSAlert.runModal）
    var runAlert: (NSAlert) -> NSApplication.ModalResponse

    static let appKit = LauncherWindowPresenter(
        attach: { child, owner in owner.addChildWindow(child, ordered: .above) },
        detach: { child, owner in owner.removeChildWindow(child) },
        orderFront: { $0.orderFront(nil) },
        makeKeyAndOrderFront: { $0.makeKeyAndOrderFront(nil) },
        orderOut: { $0.orderOut(nil) },
        showPopover: { popover, rect, view, edge in popover.show(relativeTo: rect, of: view, preferredEdge: edge) },
        closePopover: { $0.performClose(nil) },
        dismissPopover: { $0.close() },
        isShown: { $0.isVisible },
        children: { $0.childWindows ?? [] },
        orderedWindows: { NSApp.orderedWindows },
        runPanel: { $0.runModal() },
        runAlert: { $0.runModal() })

    static var current = appKit
}
