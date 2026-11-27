// S2Arm（不是上游文件）：手柄测试面板（开发工具）的「游戏看到的」转换要跟服务层 NpadImu.Push 一致：直接读那个 C# 文件，
//   照里面「X = -X」的赋值推出每种形态取反哪几轴，跟 Swift 这份对照（两边哪边改了、另一边没跟，这里就红）。另测姿态融合、步骤文件、体感包解码
import XCTest
import simd
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
}
