// S2Arm（不是上游文件）：手柄测试面板（开发工具）的「游戏看到的」转换要跟服务层 NpadImu.Push 一致：直接读那个 C# 文件，
//   照里面「X = -X」的赋值推出每种形态取反哪几轴，跟 Swift 这份对照（两边哪边改了、另一边没跟，这里就红）。另测姿态融合、步骤文件、体感包解码
import XCTest
import simd
import SceneKit
import AppKit
import LauncherPadBridge
@testable import NX2AppleUI

final class ControllerTestModelTests: XCTestCase {
    private static var npadImu: URL {
        var url = URL(fileURLWithPath: #filePath)
        while url.path != "/" && !FileManager.default.fileExists(atPath: url.appendingPathComponent("compat/tree").path) { url.deleteLastPathComponent() }
        return url.appendingPathComponent("compat/tree/src/Ryujinx.HLE/HOS/Services/Hid/HidDevices/NpadImu.cs")
    }

    /// Push 里每个 if 块（条件 → 取反的分量，如 "accelerationG.Y"）：条件提到 Dual / dual / Pair / pair 的算一对，其余算单只
    private func serviceFlips() throws -> [ControllerTestIMU.Shape: Set<String>] {
        let source = try String(contentsOf: Self.npadImu, encoding: .utf8)
        guard let push = source.range(of: "internal void Push(") else { XCTFail("NpadImu.cs 找不到 Push"); return [:] }
        let body = String(source[push.lowerBound...].prefix(4000))
        let block = try NSRegularExpression(pattern: #"if\s*\(([^)]*)\)\s*\{([^{}]*)\}"#)
        let negate = try NSRegularExpression(pattern: #"(\w+\.[XYZ])\s*=\s*-\s*(\w+\.[XYZ])\s*;"#)
        var out: [ControllerTestIMU.Shape: Set<String>] = [:]
        for m in block.matches(in: body, range: NSRange(body.startIndex..., in: body)) {
            let cond = String(body[Range(m.range(at: 1), in: body)!]), inner = String(body[Range(m.range(at: 2), in: body)!])
            var flips = Set<String>()
            for n in negate.matches(in: inner, range: NSRange(inner.startIndex..., in: inner)) {
                let lhs = String(inner[Range(n.range(at: 1), in: inner)!]), rhs = String(inner[Range(n.range(at: 2), in: inner)!])
                if lhs == rhs { flips.insert(lhs) }
            }
            guard !flips.isEmpty else { continue }
            let shape: ControllerTestIMU.Shape = cond.range(of: "dual|pair", options: [.regularExpression, .caseInsensitive]) != nil ? .pair : .single
            XCTAssertNil(out[shape], "Push 里有两块同一形态的转换，对照不了：\(cond)")
            out[shape] = flips
        }
        return out
    }
    private func swiftFlips(_ shape: ControllerTestIMU.Shape) -> Set<String> {
        let s = ControllerTestIMU.gameView(IMUSample(accel: SIMD3(1, 1, 1), gyro: SIMD3(1, 1, 1)), shape: shape)
        var out = Set<String>()
        for (i, axis) in ["X", "Y", "Z"].enumerated() {
            if s.accel[i] < 0 { out.insert("accelerationG." + axis) }
            if s.gyro[i] < 0 { out.insert("angularVelocityDegrees." + axis) }
        }
        return out
    }

    func testGameViewMatchesServiceLayer() throws {
        let service = try serviceFlips()
        XCTAssertEqual(service.count, 2, "NpadImu.Push 里应有单只、一对两块转换：\(service)")
        for shape in [ControllerTestIMU.Shape.single, .pair] {
            XCTAssertEqual(swiftFlips(shape), service[shape], "\(shape) 跟 NpadImu.Push 不一致")
        }
        XCTAssertEqual(swiftFlips(.other), [])
    }

    /// 手写的期望（同 NpadImu.cs 注释）：单只 Y 镜像、一对绕 X 转 180°
    func testGameViewValues() {
        let s = IMUSample(accel: SIMD3(0.1, 0.2, 0.9), gyro: SIMD3(10, 20, 30))
        XCTAssertEqual(ControllerTestIMU.gameView(s, shape: .single), IMUSample(accel: SIMD3(0.1, -0.2, 0.9), gyro: SIMD3(-10, 20, -30)))
        XCTAssertEqual(ControllerTestIMU.gameView(s, shape: .pair), IMUSample(accel: SIMD3(0.1, -0.2, -0.9), gyro: SIMD3(10, -20, -30)))
    }

    func testPoseSettlesOnGravityAndIntegratesGyro() {
        var pose = PoseFilter()
        let flat = IMUSample(accel: SIMD3(0, 0, 1), gyro: .zero)
        pose.update(flat, dt: 0)
        XCTAssertEqual(simd_dot(pose.world(SIMD3(0, 0, 1)), PoseFilter.up), 1, accuracy: 1e-6, "平放：屏幕面朝上")
        XCTAssertGreaterThan(pose.world(SIMD3(0, 1, 0)).y, 0.99, "平放归正：顶端指向屏幕里")
        // 绕 +Z 每秒 90°、转 1 秒（重力仍朝上）：顶端从 +Y 转到 −X
        for _ in 0..<100 { pose.update(IMUSample(accel: SIMD3(0, 0, 1), gyro: SIMD3(0, 0, 90)), dt: 0.01) }
        XCTAssertLessThan(pose.world(SIMD3(0, 1, 0)).x, -0.98)
        XCTAssertTrue(PoseText.describe(IMUSample(accel: SIMD3(0, 0, 1), gyro: SIMD3(0, 0, 90)), pose: pose).contains(SimText.pick("往左", "往左", "left")))
        pose.recenter()
        XCTAssertGreaterThan(pose.world(SIMD3(0, 1, 0)).y, 0.99)
        // 立起：重力在 +Y，屏幕面朝人（世界 −Y）
        var upright = PoseFilter()
        upright.update(IMUSample(accel: SIMD3(0, 1, 0), gyro: .zero), dt: 0)
        XCTAssertLessThan(upright.world(SIMD3(0, 0, 1)).y, -0.99)
    }

    func testStepsParse() {
        XCTAssertEqual(ControllerTestStep.parse(Data(#"["a","b"]"#.utf8)), [.init(text: "a"), .init(text: "b")])
        XCTAssertEqual(ControllerTestStep.parse(Data(#"{"steps":[{"text":"轉 90°","seconds":3},"停"]}"#.utf8)), [.init(text: "轉 90°", seconds: 3), .init(text: "停")])
        XCTAssertNil(ControllerTestStep.parse(Data("not json".utf8)))
    }

    /// 体感包：一对时 motion 是右只（side 0）、motion_left 是左只（side 1），时间戳 64 位拆两半再拼回
    func testDecodeRelayMotion() {
        let right = s2a_launcher_pad_motion(player: 2, kind: 4, side: 0, host_timestamp_us: 0x1_0000_0002, ax: 0.1, ay: 0.2, az: 0.9, gx: 10, gy: 20, gz: 30)
        let left = s2a_launcher_pad_motion(player: 2, kind: 4, side: 1, host_timestamp_us: 0x3_0000_0004, ax: -0.5, ay: 0, az: 0.8, gx: -1, gy: -2, gz: -3)
        var bytes = [UInt8](repeating: 0, count: 256)
        let length = s2a_launcher_pad_encode_motion_for_test([right, left], 2, &bytes, bytes.count)
        XCTAssertEqual(length, 144)
        var out = [s2a_launcher_pad_motion](repeating: .init(), count: 2)
        XCTAssertEqual(s2a_launcher_pad_decode_motion(bytes, length, &out), 2)
        XCTAssertEqual(out[0].player, 2); XCTAssertEqual(out[0].kind, 4); XCTAssertEqual(out[0].side, 0)
        XCTAssertEqual(out[0].az, 0.9, accuracy: 1e-6); XCTAssertEqual(out[0].gz, 30, accuracy: 1e-6); XCTAssertEqual(out[0].host_timestamp_us, 0x1_0000_0002)
        XCTAssertEqual(out[1].side, 1); XCTAssertEqual(out[1].ax, -0.5, accuracy: 1e-6); XCTAssertEqual(out[1].gz, -3, accuracy: 1e-6)
        XCTAssertEqual(out[1].host_timestamp_us, 0x3_0000_0004)
        bytes[6] = 3   // type 改成 MOTION：不是 INPUT 包
        XCTAssertEqual(s2a_launcher_pad_decode_motion(bytes, length, &out), -1)
        XCTAssertEqual(s2a_launcher_pad_decode_motion(bytes, 100, &out), -1)
    }

    /// 内建流程的固定姿势：完整姿态里的重力方向要等于用来比对的 up（两份写法不能各说各话），左右只都一样
    func testFlowHoldPosesAgreeWithGravity() {
        for hold in [ControllerTestFlow.Hold.flat, .edge, .upright] {
            for side in ["L", "R"] {
                let q = ControllerTestFlow.pose(hold, side: side)
                XCTAssertEqual(simd_length(q.vector), 1, accuracy: 1e-9)
                XCTAssertLessThan(ControllerTestFlow.angle(q.inverse.act(PoseFilter.up), ControllerTestFlow.up(hold, side: side)), 0.01, "\(hold) \(side)")
            }
        }
        // 实录：右只横握（滑轨朝上）≈ (−0.96, 0, 0) g，立握 ≈ (0.2, 0.98, −0.1) g：都在 20° 以内
        // 横握・立用滑轨朝下（实录滑轨朝上是 (−0.96, 0, 0)，反过来就是 +X 朝上）
        XCTAssertLessThan(ControllerTestFlow.angle(SIMD3(0.96, 0, 0), ControllerTestFlow.up(.edge)), ControllerTestFlow.tolerance)
        // 滑轨（右只 −X、左只 +X）朝下；第 4 步翻完两只按键面都朝上
        XCTAssertLessThan(ControllerTestFlow.pose(.edge, side: "R").act(SIMD3(-1, 0, 0)).z, -0.99)
        XCTAssertLessThan(ControllerTestFlow.pose(.edge, side: "L").act(SIMD3(1, 0, 0)).z, -0.99)
        for side in ["L", "R"] {
            let faceUp = ControllerTestFlow.joyConSingle.illustration(side: side)[3]
            XCTAssertGreaterThan(faceUp.act(SIMD3(0, 0, 1)).z, 0.99, side)
        }
        XCTAssertLessThan(ControllerTestFlow.angle(SIMD3(0.2, 0.98, -0.1), ControllerTestFlow.up(.upright)), ControllerTestFlow.tolerance)
        XCTAssertLessThan(ControllerTestFlow.angle(SIMD3(0, 0.12, 1.0), ControllerTestFlow.up(.flat)), ControllerTestFlow.tolerance)
    }

    /// 相对转动：绕长轴 +90°（实录：横握时沿长轴翻，陀螺 Y 积分 ≈ +98°）= 用陀螺 Y 积分出来的同一个姿态
    func testFlowRotateMatchesGyroIntegration() {
        var pose = PoseFilter(); pose.gain = 0
        pose.update(IMUSample(accel: SIMD3(-1, 0, 0), gyro: .zero), dt: 0)
        let start = pose.q
        for _ in 0..<100 { pose.update(IMUSample(accel: SIMD3(-1, 0, 0), gyro: SIMD3(0, 90, 0)), dt: 0.01) }
        XCTAssertLessThan(ControllerTestFlow.angle(pose.q, ControllerTestFlow.rotated(start, .bodyLong, degrees: 90)), 1)
        // 往右转 = 从上往下看顺时针：前方 +Y 转到右边 +X
        let turned = ControllerTestFlow.rotated(simd_quatd(ix: 0, iy: 0, iz: 0, r: 1), .worldUp, degrees: -90)
        XCTAssertGreaterThan(turned.act(SIMD3(0, 1, 0)).x, 0.99)
    }

    /// 淡色模型的水平朝向跟着手上那只：只差倾斜，水平朝向一样
    func testYawAlignedKeepsTilt() {
        let target = ControllerTestFlow.pose(.edge, side: "R")
        let live = simd_quatd(angle: 1.2, axis: SIMD3(0, 0, 1)) * target
        XCTAssertLessThan(ControllerTestFlow.angle(ControllerTestFlow.yawAligned(target, to: live), live), 0.01)
        let tilted = simd_quatd(angle: 0.5, axis: SIMD3(1, 0, 0)) * live
        XCTAssertEqual(ControllerTestFlow.angle(ControllerTestFlow.yawAligned(target, to: tilted), tilted), 0.5 * 180 / .pi, accuracy: 3)
    }

    func testRecordNamePerRun() {
        let date = Date(timeIntervalSince1970: 0)
        let name = ControllerTestHub.recordName(date: date, flow: "joycon")
        XCTAssertTrue(name.hasPrefix("readings-19") || name.hasPrefix("readings-1970"))
        XCTAssertTrue(name.hasSuffix("-joycon.tsv"))
    }

    /// 内建流程每一步都有目标姿态（示意图），回起始姿势那几步跟第 1 步一样；右只往右转后顶端指向右边
    func testFlowIllustrations() {
        let flow = ControllerTestFlow.joyConSingle
        XCTAssertFalse(flow.steps.contains { $0.target == .manual })
        let poses = flow.illustration(side: "R")
        XCTAssertEqual(poses.count, flow.steps.count)
        XCTAssertLessThan(ControllerTestFlow.angle(poses[2], poses[0]), 0.01)
        XCTAssertLessThan(ControllerTestFlow.angle(poses[4], poses[0]), 0.01)
        XCTAssertGreaterThan(poses[1].act(SIMD3(0, 1, 0)).x, 0.99)
        // 左只正手往左转：转完远端（+Y）朝左；起始时两只的按键面朝向对方
        XCTAssertLessThan(flow.illustration(side: "L")[1].act(SIMD3(0, 1, 0)).x, -0.99)
        XCTAssertGreaterThan(flow.illustration(side: "L")[0].act(SIMD3(0, 0, 1)).x, 0.99)
        XCTAssertLessThan(poses[0].act(SIMD3(0, 0, 1)).x, -0.99)
        XCTAssertTrue(flow.steps.allSatisfy { $0.who == .single })
        // 一对：往左倾从立直算，跟往右倾差 90°
        let pair = ControllerTestFlow.joyConPair
        XCTAssertTrue(pair.steps.allSatisfy { $0.who == .pair })
        let pairPoses = pair.illustration(side: "R")
        XCTAssertEqual(ControllerTestFlow.angle(pairPoses[1], pairPoses[2]), 90, accuracy: 0.1)
        // 把手握法：往右下 / 左下倾差 60°，右下倾时把手右端（左右只都看右边那头）往下；转握把各转各的，另一只留在把手姿势；握把上缘（按键面背面）转向人（世界 −Y）
        let leftPoses = pair.illustration(side: "L")
        XCTAssertEqual(ControllerTestFlow.angle(pairPoses[4], pairPoses[5]), 60, accuracy: 0.1)
        XCTAssertLessThan(pairPoses[4].act(SIMD3(0, -1, 0)).z, -0.4)   // 右只顶端朝左：−Y 是右端
        XCTAssertLessThan(leftPoses[4].act(SIMD3(0, 1, 0)).z, -0.4)    // 左只顶端（+Y）朝右
        XCTAssertLessThan(ControllerTestFlow.angle(leftPoses[6], leftPoses[3]), 0.01)
        XCTAssertLessThan(ControllerTestFlow.angle(pairPoses[7], pairPoses[3]), 0.01)
        XCTAssertEqual(ControllerTestFlow.angle(pairPoses[6], pairPoses[3]), 45, accuracy: 0.1)
        XCTAssertLessThan(pairPoses[6].act(SIMD3(0, 0, -1)).y, -0.6)
        XCTAssertLessThan(leftPoses[7].act(SIMD3(0, 0, -1)).y, -0.6)
        // 把手握法：左只顶端在右、右只顶端在左，按键面朝下
        XCTAssertGreaterThan(leftPoses[3].act(SIMD3(0, 1, 0)).x, 0.99); XCTAssertLessThan(pairPoses[3].act(SIMD3(0, 1, 0)).x, -0.99)
        XCTAssertLessThan(pairPoses[3].act(SIMD3(0, 0, 1)).z, -0.99)
    }

    /// 出示意图给人看（S2A_FLOW_RENDER=<输出文件.png> 时才跑）：每一步一格，同面板的 3D 模型；单只的步骤左右只各画一只，一对的画左右两只
    @MainActor func testRenderFlowSheet() throws {
        guard let out = ProcessInfo.processInfo.environment["S2A_FLOW_RENDER"] else { throw XCTSkip("S2A_FLOW_RENDER 没设") }
        // S2A_FLOW_RENDER_FLOW=single / pair 只出一套（缺省两套都出）
        let which = ProcessInfo.processInfo.environment["S2A_FLOW_RENDER_FLOW"]
        let steps = which == "single" ? ControllerTestFlow.joyConSingle.steps : which == "pair" ? ControllerTestFlow.joyConPair.steps
            : ControllerTestFlow.joyConSingle.steps + ControllerTestFlow.joyConPair.steps
        let flow = ControllerTestFlow(id: "all", hant: "", hans: "", en: "", steps: steps)
        let right = flow.illustration(side: "R"), left = flow.illustration(side: "L")
        let cell = NSSize(width: 300, height: 230), cols = 3
        let rows = (flow.steps.count + cols - 1) / cols
        let sheet = NSImage(size: NSSize(width: cell.width * CGFloat(cols), height: cell.height * CGFloat(rows)))
        func render(_ items: [(PoseView.Shape, simd_quatd, CGFloat)]) -> NSImage {
            let scene = SCNScene(), world = SCNNode()
            world.eulerAngles.x = -.pi / 2
            scene.rootNode.addChildNode(world)
            for (shape, q, x) in items {
                let n = SCNNode(); PoseView.build(shape, tint: nil, into: n)
                n.simdOrientation = simd_quatf(ix: Float(q.imag.x), iy: Float(q.imag.y), iz: Float(q.imag.z), r: Float(q.real))
                n.position = SCNVector3(x, 0, 0); world.addChildNode(n)
            }
            let floor = SCNNode(geometry: SCNPlane(width: 0.3, height: 0.24)); floor.position = SCNVector3(0, 0, -0.06)
            floor.geometry?.firstMaterial?.diffuse.contents = NSColor(calibratedWhite: 0.9, alpha: 1); world.addChildNode(floor)
            let cam = SCNNode(); cam.camera = SCNCamera(); cam.camera?.fieldOfView = 40; cam.camera?.zNear = 0.01
            cam.position = SCNVector3(0.1, 0.11, 0.21); cam.look(at: SCNVector3(0, 0, 0)); scene.rootNode.addChildNode(cam)
            let light = SCNNode(); light.light = SCNLight(); light.light?.type = .omni; light.position = SCNVector3(0.1, 0.3, 0.3); scene.rootNode.addChildNode(light)
            let amb = SCNNode(); amb.light = SCNLight(); amb.light?.type = .ambient; amb.light?.intensity = 400; scene.rootNode.addChildNode(amb)
            scene.background.contents = NSColor(calibratedRed: 0.96, green: 0.97, blue: 0.98, alpha: 1)
            let r = SCNRenderer(device: MTLCreateSystemDefaultDevice(), options: nil); r.scene = scene; r.pointOfView = cam
            return r.snapshot(atTime: 0, with: CGSize(width: cell.width, height: cell.height - 40), antialiasingMode: .multisampling4X)
        }
        sheet.lockFocus()
        NSColor.white.setFill(); NSRect(origin: .zero, size: sheet.size).fill()
        for (i, step) in flow.steps.enumerated() {
            let col = i % cols, row = i / cols
            let origin = NSPoint(x: CGFloat(col) * cell.width, y: sheet.size.height - CGFloat(row + 1) * cell.height)
            // 单只的步骤左右只各画一只（左只在左、右只在右，各自单独测）
            // S2A_FLOW_RENDER_SIDE=L / R：单只的步骤只画那一只（看起来才不像一对）
            let only = step.who == .single ? ProcessInfo.processInfo.environment["S2A_FLOW_RENDER_SIDE"] : nil
            // S2A_FLOW_RENDER_MODEL=gen1：画一代 Joy-Con
            let gen1 = ProcessInfo.processInfo.environment["S2A_FLOW_RENDER_MODEL"] == "gen1"
            let l: PoseView.Shape = gen1 ? .gen1Left : .joyConLeft, r: PoseView.Shape = gen1 ? .gen1Right : .joyConRight
            let items: [(PoseView.Shape, simd_quatd, CGFloat)] = only == "R" ? [(r, right[i], 0)] : only == "L" ? [(l, left[i], 0)]
                : [(l, left[i], -0.065), (r, right[i], 0.065)]
            render(items).draw(in: NSRect(x: origin.x + 4, y: origin.y + 40, width: cell.width - 8, height: cell.height - 44))
            let text = "\(i + 1). " + step.hant + "\n" + step.en
            (text as NSString).draw(in: NSRect(x: origin.x + 8, y: origin.y + 2, width: cell.width - 16, height: 38),
                                    withAttributes: [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.black])
        }
        sheet.unlockFocus()
        let png = NSBitmapImageRep(data: sheet.tiffRepresentation!)!.representation(using: .png, properties: [:])!
        try png.write(to: URL(fileURLWithPath: out))
    }

    /// 转轴跟水平朝向无关：起始姿态整个转一个水平角度，转完的目标也只差同一个水平角度（实机 11-27：第 6 步「前端往下壓」示意成侧翻，因为用了世界的「右」当轴）
    func testRotateAxesIgnoreHeading() {
        let yaw = simd_quatd(angle: 1.1, axis: SIMD3(0, 0, 1))
        for side in ["L", "R"] {
            for (hold, axis, deg) in [(ControllerTestFlow.Hold.edge, ControllerTestFlow.Axis.farEndTip, -90.0), (.upright, .faceForward, 45), (.edge, .bodyLong, 90)] {
                let start = ControllerTestFlow.pose(hold, side: side)
                let a = ControllerTestFlow.rotated(start, axis, degrees: deg, side: side)
                let b = ControllerTestFlow.rotated(yaw * start, axis, degrees: deg, side: side)
                XCTAssertLessThan(ControllerTestFlow.angle(yaw * a, b), 0.01, "\(hold) \(axis) \(side)")
            }
            // 远端往下压：远端（+Y）转到朝下
            let tipped = ControllerTestFlow.rotated(yaw * ControllerTestFlow.pose(.edge, side: side), .farEndTip, degrees: -90, side: side)
            XCTAssertLessThan(tipped.act(SIMD3(0, 1, 0)).z, -0.99)
            // 往右倾：顶端（+Y）往人的右边倒（标准姿态下就是世界 +X）
            let leaned = ControllerTestFlow.rotated(ControllerTestFlow.pose(.upright, side: side), .faceForward, degrees: 45, side: side)
            XCTAssertGreaterThan(leaned.act(SIMD3(0, 1, 0)).x, 0.7)
        }
    }

    /// 一代 Joy-Con 的轴还没真机校对：先当成跟 Joy-Con 2 原始读数同一套。有真机的人校对后改 Gen1Axes，同时把这里的期望改成量到的对应
    func testGen1AxesIdentityUntilCalibrated() {
        let s = IMUSample(accel: SIMD3(0.1, 0.2, 0.9), gyro: SIMD3(10, 20, 30))
        for side in ["L", "R"] { XCTAssertEqual(Gen1Axes.toDevice(s, side: side), s) }
        // 没读到出厂色时：左电光蓝、右电光红
        XCTAssertEqual(Gen1Look.defaultTint(left: true).body, 0x0ab9e6)
        XCTAssertEqual(Gen1Look.defaultTint(left: false).body, 0xff3c28)
    }
}
