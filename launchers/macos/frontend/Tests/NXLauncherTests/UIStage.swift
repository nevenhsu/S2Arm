import AppKit
@testable import NX2AppleUI

/// 单测窗口一律不上屏（11-26）：进程里第一次用到时装上不上屏的舞台（NX2AppleUI LauncherOffscreenStage），之后提示 / 屏幕键盘 / 手柄面板都只记账、不碰窗口服务器。
///   用例窗口用 UIStage.show(window) 登记成「看得见」，不调 orderFront / orderBack / makeKeyAndOrderFront
@MainActor enum UIStage {
    static let shared: LauncherOffscreenStage = { let s = LauncherOffscreenStage(); s.install(); return s }()
    static func show(_ window: NSWindow) { shared.stage(window) }
}
