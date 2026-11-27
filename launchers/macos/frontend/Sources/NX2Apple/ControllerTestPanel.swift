// S2Arm（不是上游文件）：App 菜单「手把測試」的浮动面板（开发工具，开关同「模擬手把」）。数据见 ControllerTestModel.swift。
//   由上到下：要看的手柄（同手柄面板的清单，只能选；连接 / 拆开 / 分配在手柄面板做）→ 读数（原始六轴、3D 姿态、最近 3 秒陀螺波形；一对左右两栏）
//   → 步骤（内建 Joy-Con 流程，或 steps.json 的临时流程）→ 录制（每次一个 readings-<时间>-<流程>.tsv）。样式同模拟手柄面板：12pt、small 控件、灰色小标题、间距 4 的倍数
import AppKit
import SceneKit
import SwiftUI

struct ControllerTestMenu: View {
    var body: some View {
        Button(SimText.pick("手把測試", "手柄测试", "Controller Test")) { ControllerTestPanel.show() }
    }
}

@MainActor enum ControllerTestPanel {
    private static var panel: NSPanel?
    private static var closeObserver: NSObjectProtocol?
    static var isOpen: Bool { panel?.isVisible == true }
    static func show() {
        let title = SimText.pick("手把測試", "手柄测试", "Controller Test")
        if let panel { panel.title = title; panel.orderFront(nil); ControllerTestHub.shared.panelOpened(); return }
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 540, height: 720),
                        styleMask: [.titled, .closable, .resizable, .utilityWindow], backing: .buffered, defer: false)
        p.title = title
        p.isFloatingPanel = true
        p.hidesOnDeactivate = false
        p.isReleasedWhenClosed = false
        p.contentView = NSHostingView(rootView: ControllerTestPanelView())
        p.center()
        closeObserver = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: p, queue: .main) { _ in
            MainActor.assumeIsolated { ControllerTestHub.shared.panelClosed() }
        }
        ControllerTestHub.shared.panelOpened()
        p.orderFront(nil)
        panel = p
    }
}

private enum AxisColor {
    static let x = Color(red: 0.898, green: 0.282, blue: 0.302)
    static let y = Color(red: 0.188, green: 0.643, blue: 0.424)
    static let z = Color(red: 0.243, green: 0.482, blue: 0.980)
    static let all = [x, y, z]
}

struct ControllerTestPanelView: View {
    @ObservedObject var hub = ControllerTestHub.shared
    @AppStorage("appLanguage") private var language = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                pickSection.padding(12)
                Divider()
                readingsSection.padding(12)
                Divider(); stepsSection.padding(12)
                Divider()
                recordSection.padding(12)
            }
        }
        .font(.system(size: 12))
        .controlSize(.small)
        .frame(minWidth: 500, minHeight: 360)
    }

    // —— 要看的手柄 ——
    private var pickSection: some View {
        let targets = hub.targets, selected = hub.selected?.id
        return VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                TestLabel(SimText.pick("要看的手把", "要看的手柄", "Controller"))
                Spacer()
                Text(SimText.pick("跟手把面板同一份清單，連接 / 拆開請到手把面板", "与手柄面板同一份列表，连接 / 拆开请到手柄面板", "Same list as the controller panel; connect or split there"))
                    .font(.system(size: 11)).foregroundStyle(.tertiary)
            }
            VStack(spacing: 0) {
                ForEach(targets) { t in
                    let on = t.id == selected
                    Button { hub.selectedID = t.id } label: {
                        HStack(spacing: 8) {
                            Text(t.player).font(.system(size: 11, weight: .semibold)).frame(width: 24, alignment: .leading)
                            ForEach(Array(t.dots.enumerated()), id: \.offset) { _, tint in TestDot(tint: tint, inverted: on) }
                            Text(t.title).lineLimit(1)
                            Spacer(minLength: 4)
                            Text(t.meta).font(.system(size: 11)).foregroundStyle(on ? Color.white.opacity(0.8) : .secondary)
                        }
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .foregroundStyle(on ? Color.white : Color.primary)
                        .background(on ? Color.accentColor : Color.clear)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    if t.id != targets.last?.id { Divider() }
                }
            }
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.secondary.opacity(0.25)))
            .overlay { if targets.isEmpty { Text(SimText.pick("還沒有手把，請到手把面板連接", "还没有手柄，请到手柄面板连接", "No controllers yet; connect one in the controller panel")).foregroundStyle(.tertiary).padding(8) } }
        }
    }

    // —— 读数 ——
    private var readingsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 16) {
                SourceItem(name: SimText.pick("來源", "来源", "Source"), value: hub.sourceName)
                SourceItem(name: SimText.pick("取樣", "采样", "Rate"), value: hub.channels.isEmpty ? "—" : hub.channels.map { "\($0.rate) Hz" }.joined(separator: " / "))
                SourceItem(name: SimText.pick("原始讀數", "原始读数", "Raw"), value: SimText.pick("未轉換", "未转换", "untransformed"))
            }
            .font(.system(size: 11))
            HStack(spacing: 8) {
                Text(SimText.pick("姿態用", "姿态用", "Pose from")).font(.system(size: 11)).foregroundStyle(.secondary)
                Picker("", selection: $hub.poseFromGame) {
                    Text(SimText.pick("原始讀數", "原始读数", "Raw")).tag(false)
                    Text(SimText.pick("遊戲看到的", "游戏看到的", "As the game sees it")).tag(true)
                }
                .labelsHidden().pickerStyle(.segmented).fixedSize()
                .help(SimText.pick("遊戲看到的 = 套上服務層給遊戲的轉換（單只 Y 鏡像、一對繞 X 轉 180°）", "游戏看到的 = 套上服务层给游戏的转换（单只 Y 镜像、一对绕 X 转 180°）",
                                   "As the game sees it = with the service-layer transform (single: mirror Y; pair: 180° about X)"))
                Spacer()
                Button(SimText.pick("歸正", "归正", "Recenter")) { hub.recenter() }.disabled(hub.status != .ok)
            }
            if let message = statusMessage {
                Text(message).foregroundStyle(.secondary).frame(maxWidth: .infinity, minHeight: 120).multilineTextAlignment(.center)
                    .background(Color(nsColor: .controlBackgroundColor)).clipShape(RoundedRectangle(cornerRadius: 8))
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.secondary.opacity(0.25)))
            } else {
                HStack(alignment: .top, spacing: 8) {
                    ForEach(hub.channels, id: \.side) { channel in
                        ChannelColumn(hub: hub, channel: channel, pair: hub.selected?.isPair == true, wholePad: hub.selected?.wholePad == true)
                    }
                }
            }
            Text(SimText.pick("立體模型照目前姿態轉動，藍色點是頂端最近 1.5 秒轉過的路線；波形是最近 3 秒的陀螺讀數，紅 X、綠 Y、藍 Z。",
                              "立体模型照目前姿态转动，蓝色点是顶端最近 1.5 秒转过的路线；波形是最近 3 秒的陀螺读数，红 X、绿 Y、蓝 Z。",
                              "The model follows the current pose; blue dots trace the top over the last 1.5 s. The wave is the last 3 s of gyro: red X, green Y, blue Z."))
                .font(.system(size: 11)).foregroundStyle(.tertiary).fixedSize(horizontal: false, vertical: true)
        }
    }
    private var statusMessage: String? {
        switch hub.status {
        case .ok: return nil
        case .noTargets: return SimText.pick("還沒有手把", "还没有手柄", "No controllers yet")
        case .simulated: return SimText.pick("模擬的手把沒有讀數", "模拟的手柄没有读数", "Simulated controllers have no readings")
        case .gameRunning: return SimText.pick("遊戲進行中，Joy-Con 2 讀數交給遊戲", "游戏进行中，Joy-Con 2 读数交给游戏", "Game running: Joy-Con 2 readings go to the game")
        case .assigning: return SimText.pick("分配玩家中，Joy-Con 2 讀數暫停", "分配玩家中，Joy-Con 2 读数暂停", "Assigning players: Joy-Con 2 readings paused")
        case .relayClosed: return SimText.pick("還沒接上 Joy-Con 2 連接程式（S2Arm 要在前景）", "还没接上 Joy-Con 2 连接程序（S2Arm 要在前台）", "Not connected to the Joy-Con 2 helper (S2Arm must be in front)")
        case .noMotion: return SimText.pick("這只手把沒有體感感測器", "这只手柄没有体感传感器", "This controller has no motion sensors")
        case .waiting: return SimText.pick("等讀數…", "等读数…", "Waiting for readings…")
        }
    }

    // —— 步骤 ——
    private var stepsSection: some View {
        let flow = hub.flow, step = hub.step
        return VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                TestLabel(SimText.pick("步驟", "步骤", "Steps"))
                Spacer()
                // 流程跟着上面选的手柄（单只 → 单只测试、一对 → 一对测试），不另给选单；有 steps.json 时用它
                Text(flow.title).font(.system(size: 11)).foregroundStyle(.secondary)
                // 开始 / 停止放在步骤上面（同时录制）
                Button { hub.toggleRun() } label: {
                    HStack(spacing: 4) {
                        Image(systemName: hub.running ? "stop.fill" : "play.fill").foregroundStyle(hub.running ? Color.red : Color.green)
                        Text(hub.running ? SimText.pick("停止", "停止", "Stop") : SimText.pick("開始", "开始", "Start"))
                    }
                }
            }
            HStack(spacing: 8) {
                Text("\(hub.stepIndex + 1) / \(flow.steps.count)").font(.system(size: 11)).foregroundStyle(.secondary).fixedSize()
                VStack(alignment: .leading, spacing: 4) {
                    Text(step?.text ?? "").font(.system(size: 13, weight: .medium)).fixedSize(horizontal: false, vertical: true)
                    if !hub.running {
                        Text(hub.finished ? SimText.pick("測完了，讀數已存好（路徑在下面）。再測一次：選好步驟按「開始」", "测完了，读数已存好（路径在下面）。再测一次：选好步骤按「开始」", "Done; readings saved (path below). To run again, pick a step and press Start")
                                          : SimText.pick("用上一步 / 下一步選從哪一步開始，按「開始」：才出現淺藍色示意模型、對上自動下一步，同時錄下讀數",
                                                         "用上一步 / 下一步选从哪一步开始，按「开始」：才出现浅蓝色示意模型、对上自动下一步，同时录下读数",
                                                         "Use Back / Next to pick the first step, then press Start: the pale blue model appears, steps advance when you match it, and readings are recorded"))
                            .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    } else if let step, step.target != .manual {
                        if hub.stepFitsSelection {
                            Text(SimText.pick("對準淺藍色的模型，停住 1 秒自動下一步", "对准浅蓝色的模型，停住 1 秒自动下一步", "Match the pale blue model and hold still for 1 s to advance"))
                                .font(.system(size: 11)).foregroundStyle(.secondary)
                            ProgressView(value: hub.holdProgress).progressViewStyle(.linear).frame(maxWidth: 160)
                        } else {
                            Text(step.who == .pair ? SimText.pick("這一步要在上面選 Joy-Con 2（一對）", "这一步要在上面选 Joy-Con 2（一对）", "Select the Joy-Con 2 pair above for this step")
                                                   : SimText.pick("這一步要在上面選單只 Joy-Con 2", "这一步要在上面选单只 Joy-Con 2", "Select a single Joy-Con 2 above for this step"))
                                .font(.system(size: 11)).foregroundStyle(.orange)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                // 按钮不让旁边的文字挤窄（11-27：「上一步」边框被裁掉）
                Button(SimText.pick("上一步", "上一步", "Back")) { hub.step(-1) }.disabled(hub.stepIndex == 0).fixedSize()
                Button(SimText.pick("下一步", "下一步", "Next")) { hub.step(1) }.disabled(hub.stepIndex >= flow.steps.count - 1).fixedSize()
            }
            .padding(8)
            .background(Color(nsColor: .controlBackgroundColor)).clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.secondary.opacity(0.25)))
        }
    }

    // —— 开始 / 停止（同时录制）——
    private var recordSection: some View {
        HStack(spacing: 8) {
            if hub.isRecording {
                let secs = Int(ProcessInfo.processInfo.systemUptime - hub.recordStart)
                Text(SimText.pick("錄製中", "录制中", "Recording") + String(format: " %02d:%02d・", secs / 60, secs % 60) + SimText.pick("\(hub.recordCount) 筆", "\(hub.recordCount) 笔", "\(hub.recordCount) rows"))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            } else if hub.recordError == nil {
                Text(SimText.pick("按「開始」同時錄製", "按「开始」同时录制", "Start also records")).font(.system(size: 11)).foregroundStyle(.tertiary)
            } else if let error = hub.recordError {
                Text(error).font(.system(size: 11)).foregroundStyle(.red).lineLimit(1)
            }
            Spacer()
            Text("~/S2Arm/dev/controller-test/" + (hub.recordURL?.lastPathComponent ?? "readings-…tsv")).font(.system(size: 11, design: .monospaced)).foregroundStyle(.tertiary)
                .lineLimit(1).truncationMode(.middle)
        }
    }
}

private struct TestLabel: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View { Text(text).font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary) }
}
private struct SourceItem: View {
    let name: String, value: String
    var body: some View { HStack(spacing: 4) { Text(name).foregroundStyle(.secondary); Text(value).fontWeight(.medium) } }
}
private struct TestDot: View {
    let tint: ControllerTint?
    var inverted = false
    var body: some View {
        Group {
            if let tint { Circle().fill(ControllerTint.color(tint.body)) } else { Circle().strokeBorder(inverted ? Color.white : Color.secondary, lineWidth: 1) }
        }
        .frame(width: 8, height: 8)
    }
}

/// 一栏：标题、3D 姿态、六轴（置中长条 + 数字）、最近 3 秒陀螺波形
private struct ChannelColumn: View {
    @ObservedObject var hub: ControllerTestHub
    @StateObject private var gizmo = GizmoState()   // 三轴钮跟着这一栏 3D 的视角转
    let channel: ControllerTestHub.Channel
    let pair: Bool
    let wholePad: Bool

    var body: some View {
        let s = channel.last
        VStack(alignment: .leading, spacing: 4) {
            if pair {
                HStack(spacing: 4) {
                    TestDot(tint: hub.selected?.dots[channel.side == "L" ? 0 : 1] ?? nil)
                    Text(channel.side == "L" ? SimText.pick("左只", "左只", "Left") : SimText.pick("右只", "右只", "Right")).font(.system(size: 11, weight: .semibold))
                }
            }
            let pose = hub.poseFromGame ? channel.gamePose : channel.rawPose
            ZStack(alignment: .topLeading) {
                PoseView(pose: pose, ghost: hub.poseFromGame ? nil : hub.ghost(for: channel), trail: hub.poseFromGame ? channel.trailGame.map(\.p) : channel.trailRaw.map(\.p),
                         shape: hub.shape(for: channel),
                         tint: hub.selected?.dots[pair && channel.side == "R" ? 1 : 0] ?? nil, gizmo: gizmo)
                    .frame(height: 150)
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                    // 拖动转过视角后一键回到原来的斜视角（「歸正」是重设手柄的水平朝向，这个只动镜头）
                    .overlay(alignment: .bottomTrailing) {
                        AxisGizmo(state: gizmo).frame(width: 34, height: 34).padding(6)
                            .help(SimText.pick("拖動轉視角，點一下回到預設視角", "拖动转视角，点一下回到默认视角", "Drag to orbit; click to reset the view"))
                            .accessibilityLabel(SimText.pick("視角回正", "视角回正", "Reset view"))
                    }
                if let s {
                    Text(PoseText.describe(hub.poseFromGame ? ControllerTestIMU.gameView(s, shape: channel.shape) : s, pose: pose))
                        .font(.system(size: 10)).foregroundStyle(Color(white: 0.43)).padding(8)   // 3D 底是固定浅色：字用固定深灰，深色模式也看得到
                }
                // 测试中：右上角一张实色示意 = 淡色模型的目标姿态（同主画面的缺省视角）
                if let poses = hub.illustration(for: channel) {
                    StepInset(pose: poses.to, shape: hub.shape(for: channel),
                              tint: hub.selected?.dots[pair && channel.side == "R" ? 1 : 0] ?? nil, camera: gizmo.camera)
                        .frame(width: 120, height: 96)
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.black.opacity(0.15)))
                        .overlay(alignment: .bottomLeading) { Text(SimText.pick("示意", "示意", "Guide")).font(.system(size: 9)).foregroundStyle(Color(white: 0.43)).padding(4) }
                        .padding(6)
                        .frame(maxWidth: .infinity, alignment: .topTrailing)
                        .allowsHitTesting(false)
                }
            }
            group(SimText.pick("加速度", "加速度", "Accelerometer"), unit: "g", values: s.map { [$0.accel.x, $0.accel.y, $0.accel.z] }, range: 2, format: "%+.2f")
            group(SimText.pick("陀螺", "陀螺", "Gyro"), unit: "°/s", values: s.map { [$0.gyro.x, $0.gyro.y, $0.gyro.z] }, range: 500, format: "%+.1f")
            Wave(points: channel.history.map { ($0.t, $0.s.gyro) }, range: 500).frame(height: 44).padding(.top, 4)
        }
        .padding(.horizontal, 8).padding(.vertical, 8)
        .frame(maxWidth: .infinity)
        .background(Color(nsColor: .controlBackgroundColor)).clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.secondary.opacity(0.25)))
    }
    private func group(_ title: String, unit: String, values: [Double]?, range: Double, format: String) -> some View {
        VStack(spacing: 0) {
            HStack { Text(title); Spacer(); Text(unit) }.font(.system(size: 11)).foregroundStyle(.secondary).padding(.top, 4).padding(.bottom, 4)
            ForEach(0..<3, id: \.self) { i in
                HStack(spacing: 4) {
                    Text(["X", "Y", "Z"][i]).font(.system(size: 11, weight: .semibold)).foregroundStyle(AxisColor.all[i]).frame(width: 12)
                    CenterBar(value: (values?[i] ?? 0) / range, color: AxisColor.all[i])
                    Text(values.map { String(format: format, $0[i]) } ?? "—").font(.system(size: 11, design: .monospaced)).frame(width: 56, alignment: .trailing)
                }
                .frame(height: 16)
            }
        }
    }
}

/// 置中长条：中线是 0，往右正、往左负；value 是 −1…1（超出画到头）
private struct CenterBar: View {
    let value: Double
    let color: Color
    var body: some View {
        GeometryReader { geo in
            let half = geo.size.width / 2, v = max(-1, min(1, value)), w = abs(v) * half
            ZStack(alignment: .leading) {
                Capsule().fill(Color.secondary.opacity(0.12))
                Capsule().fill(color).frame(width: max(w, 1)).offset(x: v >= 0 ? half : half - w)
                Rectangle().fill(Color.primary.opacity(0.25)).frame(width: 1).offset(x: half)
            }
        }
        .frame(height: 6)
    }
}

/// 最近 3 秒的波形（红 X 绿 Y 蓝 Z）
private struct Wave: View {
    let points: [(Double, SIMD3<Double>)]
    let range: Double
    var body: some View {
        Canvas { ctx, size in
            let mid = size.height / 2
            ctx.stroke(Path { $0.move(to: CGPoint(x: 0, y: mid)); $0.addLine(to: CGPoint(x: size.width, y: mid)) }, with: .color(.secondary.opacity(0.25)))
            guard let end = points.last?.0 else { return }
            for axis in 0..<3 {
                var path = Path()
                for (i, p) in points.enumerated() {
                    let x = size.width * (1 - (end - p.0) / 3)
                    let y = mid - max(-1, min(1, p.1[axis] / range)) * mid
                    if i == 0 { path.move(to: CGPoint(x: x, y: y)) } else { path.addLine(to: CGPoint(x: x, y: y)) }
                }
                ctx.stroke(path, with: .color(AxisColor.all[axis]), lineWidth: 1.2)
            }
        }
        .background(Color.secondary.opacity(0.05)).clipShape(RoundedRectangle(cornerRadius: 4))
    }
}

/// 3D 姿态（SceneKit）：基本形状拼的 Joy-Con / 整只手柄，照姿态转；蓝点是顶端最近 1.5 秒的路线。
///   世界坐标 Z 朝上（PoseFilter），SceneKit 是 Y 朝上：外层节点绕 X 转 −90° 换过去；镜头在屏幕前的人那边（世界 −Y）偏右、稍高处（斜着看才看得出立体，正对时横握只看到一端）
struct PoseView: NSViewRepresentable {
    enum Shape: Equatable { case joyConLeft, joyConRight, gen1Left, gen1Right, wholePad }
    let pose: PoseFilter
    let ghost: simd_quatd?   // 这一步的目标姿态（淡色模型）；nil = 不画
    let trail: [SIMD3<Double>]
    let shape: Shape
    let tint: ControllerTint?
    var gizmo: GizmoState? = nil   // 每画一帧把三轴在画面上的方向告诉三轴钮；三轴钮拖动 / 点一下经它转镜头 / 回到缺省视角

    final class Coordinator: NSObject, SCNSceneRendererDelegate {
        weak var gizmo: GizmoState?
        /// 每画一帧：世界三轴（世界节点绕 X 转了 −90°：X → 场景 X、Y → 场景 −Z、Z → 场景 Y）换到镜头坐标，取画面上的方向
        func renderer(_ renderer: SCNSceneRenderer, updateAtTime time: TimeInterval) {
            guard let pov = renderer.pointOfView else { return }
            let q = pov.presentation.simdWorldOrientation.inverse
            let dirs = [SIMD3<Float>(1, 0, 0), SIMD3<Float>(0, 0, -1), SIMD3<Float>(0, 1, 0)].map { v -> CGVector in
                let c = q.act(v); return CGVector(dx: CGFloat(c.x), dy: CGFloat(-c.y))
            }
            let transform = pov.presentation.simdWorldTransform
            DispatchQueue.main.async { [weak self] in self?.gizmo?.update(dirs, camera: transform) }
        }
        let camera = SCNNode()
        let scene = SCNScene()
        let world = SCNNode()
        let device = SCNNode()
        let ghost = SCNNode()
        var shownGhost: simd_quatd?   // 淡色模型现在画的姿态：换步骤时转过去，不直接跳
        var dots: [SCNNode] = []
        var shape: Shape?
        var tint: ControllerTint?
    }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> SCNView {
        let c = context.coordinator
        let view = SCNView()
        view.scene = c.scene
        view.backgroundColor = NSColor(calibratedRed: 0.96, green: 0.97, blue: 0.98, alpha: 1)
        view.antialiasingMode = .multisampling4X
        view.autoenablesDefaultLighting = true
        view.delegate = context.coordinator
        view.allowsCameraControl = true
        // 转盘式：左右绕竖轴转、上下俯仰，不会把画面转歪（缺省的自由旋转会滚动地平线）；不要惯性（放手后还在转，回正也被它带走）
        view.defaultCameraController.interactionMode = .orbitTurntable
        view.defaultCameraController.worldUp = SCNVector3(0, 1, 0)
        view.defaultCameraController.inertiaEnabled = false   // 拖动转视角、捏合缩放；双击回到原视角
        c.world.eulerAngles.x = -.pi / 2
        c.scene.rootNode.addChildNode(c.world)
        c.world.addChildNode(c.device)
        c.ghost.opacity = 0.3
        c.ghost.renderingOrder = 1   // 半透明的画在实的后面
        c.world.addChildNode(c.ghost)
        let floor = SCNNode(geometry: SCNPlane(width: 0.24, height: 0.24))
        floor.geometry?.firstMaterial?.diffuse.contents = Self.gridImage()
        floor.geometry?.firstMaterial?.isDoubleSided = true
        floor.position = SCNVector3(0, 0, -0.06)   // 世界坐标的「地板」（Z 往下），在世界节点里不用再转
        c.world.addChildNode(floor)
        let camera = c.camera
        camera.camera = SCNCamera()
        camera.camera?.fieldOfView = 40
        camera.camera?.zNear = 0.01
        c.scene.rootNode.addChildNode(camera)
        Self.placeCamera(camera, in: view)
        for i in 0..<48 {
            let dot = SCNNode(geometry: SCNSphere(radius: 0.0022))
            dot.geometry?.firstMaterial?.diffuse.contents = NSColor.systemBlue
            dot.geometry?.firstMaterial?.lightingModel = .constant
            dot.isHidden = true
            dot.name = "trail\(i)"
            c.world.addChildNode(dot)
            c.dots.append(dot)
        }
        return view
    }
    /// 缺省 / 回正的视角：屏幕前的人那边正前方、稍高处，看着原点（11-27 使用者定）
    static let cameraHome = SCNVector3(0, 0.09, 0.24)
    static func placeCamera(_ camera: SCNNode, in view: SCNView) {
        let controller = view.defaultCameraController
        controller.stopInertia()
        camera.removeAllActions()
        camera.position = cameraHome
        camera.look(at: SCNVector3(0, 0, 0), up: SCNVector3(0, 1, 0), localFront: SCNVector3(0, 0, -1))   // 不带滚转
        view.pointOfView = camera   // 拖过之后 SCNView 可能换成自己的镜头，换回来
        controller.pointOfView = camera
        controller.target = SCNVector3(0, 0, 0)
        controller.worldUp = SCNVector3(0, 1, 0)
        controller.clearRoll()
    }
    func updateNSView(_ view: SCNView, context: Context) {
        let c = context.coordinator
        if c.gizmo !== gizmo {
            c.gizmo = gizmo
            let camera = c.camera
            gizmo?.reset = { [weak view] in if let view { Self.placeCamera(camera, in: view) } }
            gizmo?.orbit = { [weak view] dx, dy in view?.defaultCameraController.rotateBy(x: Float(dx), y: Float(-dy)) }
        }
        if c.shape != shape || c.tint != tint {
            c.shape = shape; c.tint = tint
            c.device.childNodes.forEach { $0.removeFromParentNode() }
            Self.build(shape, tint: tint, into: c.device)
            c.ghost.childNodes.forEach { $0.removeFromParentNode() }
            Self.build(shape, tint: nil, monochrome: NSColor(calibratedRed: 0.04, green: 0.52, blue: 1, alpha: 1), into: c.ghost)   // 整只淡蓝：跟实色的分得开
        }
        let q = pose.q
        c.device.simdOrientation = simd_quatf(ix: Float(q.imag.x), iy: Float(q.imag.y), iz: Float(q.imag.z), r: Float(q.real))
        if let target = ghost {
            // 每次画往目标转 25%（面板约 30 次 / 秒刷新：约 0.3 秒转到位）；刚出现直接放上
            let g = c.shownGhost.map { simd_slerp($0, target, 0.25) } ?? target
            c.shownGhost = g
            c.ghost.isHidden = false
            c.ghost.simdOrientation = simd_quatf(ix: Float(g.imag.x), iy: Float(g.imag.y), iz: Float(g.imag.z), r: Float(g.real))
        } else { c.ghost.isHidden = true; c.shownGhost = nil }
        // 轨迹：最多 48 点，旧的淡
        let step = max(1, trail.count / c.dots.count)
        let picked = stride(from: trail.count - 1, through: 0, by: -step).prefix(c.dots.count).map { trail[$0] }
        for (i, dot) in c.dots.enumerated() {
            if i < picked.count {
                let p = picked[i]
                dot.isHidden = false
                dot.simdPosition = SIMD3(Float(p.x), Float(p.y), Float(p.z))
                dot.opacity = CGFloat(1 - Double(i) / Double(max(1, picked.count)))
            } else { dot.isHidden = true }
        }
    }

    /// 设备坐标：X 横、Y 长轴（+Y 顶端）、Z 屏幕面朝外（摇杆凸出那面）
    /// Joy-Con 2 照真机配色：机身深灰，内侧（滑轨那条）和摇杆圈是颜色（左只浅蓝、右只红；模拟手柄照它的颜色）。
    ///   monochrome：整只一个颜色（淡色目标模型用）
    static func build(_ shape: Shape, tint: ControllerTint?, monochrome: NSColor? = nil, into node: SCNNode) {
        func color(_ rgb: UInt32) -> NSColor { NSColor(calibratedRed: CGFloat(rgb >> 16 & 0xff) / 255, green: CGFloat(rgb >> 8 & 0xff) / 255, blue: CGFloat(rgb & 0xff) / 255, alpha: 1) }
        func add(_ g: SCNGeometry, _ c: NSColor, _ p: SCNVector3, rotateX: Bool = false) {
            g.firstMaterial?.diffuse.contents = c
            let n = SCNNode(geometry: g); n.position = p
            if rotateX { n.eulerAngles.x = .pi / 2 }   // 圆柱轴（Y）转到 Z：从屏幕面凸出来
            node.addChildNode(n)
        }
        let dark = monochrome ?? NSColor(calibratedWhite: 0.17, alpha: 1)
        switch shape {
        case .joyConLeft, .joyConRight:
            let right = shape == .joyConRight
            let accent = monochrome ?? color(tint?.body ?? (right ? 0xef5a5a : 0x41b8e8))
            let body = monochrome ?? color(0x34363b), button = monochrome ?? color(0x5a5c62)
            add(SCNBox(width: 0.034, height: 0.10, length: 0.014, chamferRadius: 0.007), body, SCNVector3(0, 0, 0))
            add(SCNBox(width: 0.004, height: 0.09, length: 0.0125, chamferRadius: 0.002), accent, SCNVector3(right ? -0.0175 : 0.0175, 0, 0))   // 内侧（滑轨那条）上色
            let stickY: CGFloat = right ? -0.016 : 0.024, buttonsY: CGFloat = right ? 0.024 : -0.016
            let ring = SCNTorus(ringRadius: 0.0085, pipeRadius: 0.0012)   // 摇杆圈上色
            add(ring, accent, SCNVector3(0, stickY, 0.0072), rotateX: true)
            add(SCNCylinder(radius: 0.0068, height: 0.006), dark, SCNVector3(0, stickY, 0.009), rotateX: true)
            for (dx, dy) in [(0.0, 0.0065), (0.0, -0.0065), (0.0065, 0.0), (-0.0065, 0.0)] {
                add(SCNCylinder(radius: 0.0026, height: 0.003), button, SCNVector3(CGFloat(dx), buttonsY + CGFloat(dy), 0.0075), rotateX: true)
            }
            add(SCNBox(width: 0.02, height: 0.004, length: 0.006, chamferRadius: 0.002), dark, SCNVector3(0, 0.049, 0))   // 顶端肩键
        case .gen1Left, .gen1Right:
            // 一代 Joy-Con：整只机身一个颜色、按键另一个颜色（读到的出厂色，没读到用电光蓝 / 电光红），滑轨深灰；比 Joy-Con 2 稍短稍窄
            let right = shape == .gen1Right
            let look = tint ?? Gen1Look.defaultTint(left: !right)
            let body = monochrome ?? color(look.body), button = monochrome ?? color(look.buttons ?? 0x1e1e1e)
            add(SCNBox(width: 0.03, height: 0.092, length: 0.014, chamferRadius: 0.008), body, SCNVector3(0, 0, 0))
            add(SCNBox(width: 0.004, height: 0.08, length: 0.011, chamferRadius: 0.002), dark, SCNVector3(right ? -0.0155 : 0.0155, 0, 0))   // 滑轨在内侧
            let stickY: CGFloat = right ? -0.014 : 0.022, buttonsY: CGFloat = right ? 0.022 : -0.014
            add(SCNCylinder(radius: 0.0065, height: 0.006), dark, SCNVector3(0, stickY, 0.009), rotateX: true)
            for (dx, dy) in [(0.0, 0.006), (0.0, -0.006), (0.006, 0.0), (-0.006, 0.0)] {
                add(SCNCylinder(radius: 0.0024, height: 0.003), button, SCNVector3(CGFloat(dx), buttonsY + CGFloat(dy), 0.0075), rotateX: true)
            }
            add(SCNBox(width: 0.018, height: 0.004, length: 0.006, chamferRadius: 0.002), button, SCNVector3(0, 0.045, 0))   // 顶端肩键
        case .wholePad:
            let body = NSColor(calibratedWhite: 0.32, alpha: 1)
            add(SCNBox(width: 0.12, height: 0.05, length: 0.022, chamferRadius: 0.011), body, SCNVector3(0, 0, 0))
            for x in [-0.045, 0.045] { add(SCNCylinder(radius: 0.014, height: 0.02), body, SCNVector3(CGFloat(x), -0.026, 0)) }   // 握把
            add(SCNCylinder(radius: 0.007, height: 0.006), dark, SCNVector3(-0.03, 0.006, 0.013), rotateX: true)
            add(SCNCylinder(radius: 0.007, height: 0.006), dark, SCNVector3(0.016, -0.01, 0.013), rotateX: true)
            for (dx, dy) in [(0.0, 0.006), (0.0, -0.006), (0.006, 0.0), (-0.006, 0.0)] {
                add(SCNCylinder(radius: 0.0025, height: 0.003), color(0x0a84ff), SCNVector3(0.035 + CGFloat(dx), 0.008 + CGFloat(dy), 0.012), rotateX: true)
            }
            add(SCNBox(width: 0.1, height: 0.004, length: 0.008, chamferRadius: 0.002), dark, SCNVector3(0, 0.026, 0))   // 顶端肩键
        }
    }
    private static func gridImage() -> NSImage {
        NSImage(size: NSSize(width: 240, height: 240), flipped: false) { rect in
            NSColor(calibratedWhite: 0.93, alpha: 1).setFill(); rect.fill()
            NSColor(calibratedWhite: 0, alpha: 0.08).setStroke()
            for i in stride(from: 0, through: 240, by: 20) {
                let v = NSBezierPath(); v.move(to: NSPoint(x: i, y: 0)); v.line(to: NSPoint(x: i, y: 240)); v.stroke()
                let h = NSBezierPath(); h.move(to: NSPoint(x: 0, y: i)); h.line(to: NSPoint(x: 240, y: i)); h.stroke()
            }
            return true
        }
    }
}

/// 步骤示意（右上角小图）：淡色模型的实色版（半透明看不清摇杆在哪边），同一个目标姿态、同主画面的镜头
struct StepInset: NSViewRepresentable {
    let pose: simd_quatd                 // 淡色模型的目标姿态
    let shape: PoseView.Shape
    let tint: ControllerTint?
    let camera: simd_float4x4?           // 主画面镜头（拖过视角也跟着）；nil = 缺省视角
    final class Coordinator { let model = SCNNode(); let camera = SCNNode(); var shape: PoseView.Shape?; var tint: ControllerTint? }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> SCNView {
        let view = SCNView(), scene = SCNScene(), world = SCNNode()
        view.scene = scene
        view.backgroundColor = NSColor(calibratedRed: 0.93, green: 0.95, blue: 0.98, alpha: 1)
        view.antialiasingMode = .multisampling4X
        view.autoenablesDefaultLighting = true
        world.eulerAngles.x = -.pi / 2
        scene.rootNode.addChildNode(world)
        world.addChildNode(context.coordinator.model)
        let camera = context.coordinator.camera
        camera.camera = SCNCamera(); camera.camera?.fieldOfView = 40; camera.camera?.zNear = 0.01
        camera.position = PoseView.cameraHome; camera.look(at: SCNVector3(0, 0, 0))
        scene.rootNode.addChildNode(camera); view.pointOfView = camera
        return view
    }
    func updateNSView(_ view: SCNView, context: Context) {
        let c = context.coordinator
        if c.shape != shape || c.tint != tint {
            c.shape = shape; c.tint = tint
            c.model.childNodes.forEach { $0.removeFromParentNode() }
            PoseView.build(shape, tint: tint, into: c.model)
        }
        if let camera { c.camera.simdWorldTransform = camera }
        c.model.simdOrientation = simd_quatf(ix: Float(pose.imag.x), iy: Float(pose.imag.y), iz: Float(pose.imag.z), r: Float(pose.real))
    }
}

/// 三轴钮现在画的方向（PoseView 每画一帧送来：世界 X、Y、Z 在画面上的方向，长度 = 跟画面平行的程度）
final class GizmoState: ObservableObject {
    /// PoseView 接上：转镜头（拖动的位移，点）/ 回到缺省视角
    var orbit: (CGFloat, CGFloat) -> Void = { _, _ in }
    var reset: () -> Void = {}
    @Published private(set) var dirs: [CGVector] = [CGVector(dx: 1, dy: 0), CGVector(dx: 0, dy: -0.35), CGVector(dx: 0, dy: -0.94)]
    /// 主画面镜头现在的位置 / 朝向（右上角示意跟着用同一个，看起来跟淡色模型一样）；nil = 还没画过，用缺省视角
    @Published private(set) var camera: simd_float4x4?
    func update(_ next: [CGVector], camera next4: simd_float4x4) {
        let moved = camera.map { c in (0..<4).contains { i in simd_length(c[i] - next4[i]) > 1e-4 } } ?? true
        guard moved || zip(next, dirs).contains(where: { abs($0.dx - $1.dx) + abs($0.dy - $1.dy) > 0.01 }) else { return }   // 没转就不重画
        dirs = next; camera = next4
    }
}

/// 视角回正钮：画世界三轴（红 X、绿 Y、蓝 Z），跟着 3D 的视角转；点一下镜头回到缺省视角
private struct AxisGizmo: View {
    @ObservedObject var state: GizmoState
    var body: some View {
        Canvas { ctx, size in
            let o = CGPoint(x: size.width / 2, y: size.height / 2), r = size.width * 0.3
            let colors = [AxisColor.x, AxisColor.y, AxisColor.z], names = ["X", "Y", "Z"]
            // 朝里的轴先画（被朝外的盖住）
            for i in [0, 1, 2].sorted(by: { depth($0) < depth($1) }) {
                let d = state.dirs[i]
                let end = CGPoint(x: o.x + d.dx * r, y: o.y + d.dy * r)
                ctx.stroke(Path { $0.move(to: o); $0.addLine(to: end) }, with: .color(colors[i]), style: StrokeStyle(lineWidth: 2, lineCap: .round))
                ctx.draw(Text(names[i]).font(.system(size: 8, weight: .bold)).foregroundColor(colors[i]), at: CGPoint(x: o.x + d.dx * (r + 6), y: o.y + d.dy * (r + 6)))
            }
        }
        .background(Circle().fill(Color.white.opacity(0.9)))
        .overlay(Circle().strokeBorder(Color.black.opacity(0.15)))
        .contentShape(Circle())
        // 拖动 = 转视角（同在 3D 画面上拖）；没拖动的一下点击 = 回到缺省视角
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { g in
                let d = CGSize(width: g.translation.width - last.width, height: g.translation.height - last.height)
                last = g.translation
                if hypot(g.translation.width, g.translation.height) > 3 { moved = true; state.orbit(d.width * 2, d.height * 2) }
            }
            .onEnded { _ in
                if !moved { state.reset() }
                moved = false; last = .zero
            })
        .accessibilityLabel(SimText.pick("視角", "视角", "View angle"))
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { state.reset() }
    }
    @State private var last = CGSize.zero
    @State private var moved = false
    /// 越短的轴越接近朝着 / 背着镜头：先画
    private func depth(_ i: Int) -> CGFloat { hypot(state.dirs[i].dx, state.dirs[i].dy) }
}
