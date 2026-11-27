// S2Arm（不是上游文件）：手柄测试面板的内建测试流程（固定步骤：文字 + 淡色目标模型，不用图片素材）。
//   坐标同 PoseFilter：世界 Z 朝上、+Y = 离开屏幕前的人、+X = 右；设备 Z = 按键面朝外、+Y = 顶端、X = Y × Z（Joy-Con 2 左右只同一套轴，实录：平放面朝上两只都是加速度 +Z）。
//   目标有三种：
//   · 固定姿势（hold）：只比重力方向（水平朝向没有绝对参考）。淡色模型的水平朝向跟着手上的那只转，只差倾斜
//   · 相对转动（rotate）：从这一步开始时的姿态，绕某个轴转几度（往右转 = 绕世界竖轴 −90°）
//   · 回到上一步开始时的姿态（back）
//   实录对照（11-27 一对录制）：平放面朝上 ≈ (0, +0.12, +1.0) g；右只横拿、摇杆那头朝人（桌球握法）≈ (−0.96, 0, 0)；
//   拳击握法（立起）≈ (±0.2, +0.98, ∓0.1)；桌球握法把按键面往右翻，陀螺 Y 积分 ≈ +98°
import Foundation
import simd

struct ControllerTestFlow {
    enum Who: Equatable { case single, pair }
    enum Hold: Equatable { case flat, paddle, boxing }
    enum Axis: Equatable { case worldUp, worldForward, worldRight, bodyLong }
    enum Target: Equatable {
        case hold(Hold)
        case rotate(Axis, degrees: Double)
        case back
        case manual   // steps.json 的步骤：没有目标，手动下一步
    }
    struct Step: Equatable {
        let id: String
        let hant: String, hans: String, en: String
        let who: Who?
        let target: Target
        var text: String { SimText.pick(hant, hans, en) }
    }
    let id: String
    let hant: String, hans: String, en: String
    let steps: [Step]
    var title: String { SimText.pick(hant, hans, en) }

    /// 内建：一只一只测（选单只 Joy-Con 2），再测一对
    static let joyCon = ControllerTestFlow(id: "joycon", hant: "Joy-Con 標準測試", hans: "Joy-Con 标准测试", en: "Joy-Con standard test", steps: [
        Step(id: "1-flat", hant: "平放在桌上，按鍵面朝上", hans: "平放在桌上，按键面朝上", en: "Lay it flat on the desk, buttons up", who: .single, target: .hold(.flat)),
        Step(id: "2-paddle", hant: "橫拿（桌球握法），搖桿那頭朝向你", hans: "横拿（乒乓球握法），摇杆那头朝向你", en: "Hold it sideways (table-tennis grip), stick end toward you", who: .single, target: .hold(.paddle)),
        Step(id: "3-turn-right", hant: "保持橫拿，水平往右轉 90°", hans: "保持横拿，水平往右转 90°", en: "Keep the grip, turn 90° right horizontally", who: .single, target: .rotate(.worldUp, degrees: -90)),
        Step(id: "4-turn-back", hant: "轉回來", hans: "转回来", en: "Turn back", who: .single, target: .back),
        Step(id: "5-face-right", hant: "把按鍵面往右翻 90°", hans: "把按键面往右翻 90°", en: "Roll the button face 90° to the right", who: .single, target: .rotate(.bodyLong, degrees: 90)),
        Step(id: "6-face-back", hant: "翻回來", hans: "翻回来", en: "Roll back", who: .single, target: .back),
        Step(id: "7-tip-down", hant: "前端往下壓 90°", hans: "前端往下压 90°", en: "Tip the far end down 90°", who: .single, target: .rotate(.worldRight, degrees: -90)),
        Step(id: "8-tip-back", hant: "抬回來", hans: "抬回来", en: "Bring it back up", who: .single, target: .back),
        Step(id: "9-boxing", hant: "換成一對：左右手各拿一只，立起來（拳擊握法），按鍵面朝向彼此", hans: "换成一对：左右手各拿一只，立起来（拳击握法），按键面朝向彼此",
             en: "Now the pair: one in each hand, upright (boxing grip), button faces toward each other", who: .pair, target: .hold(.boxing)),
        Step(id: "10-lean-right", hant: "兩只一起往右傾 45°", hans: "两只一起往右倾 45°", en: "Lean both 45° to the right", who: .pair, target: .rotate(.worldForward, degrees: 45)),
        Step(id: "11-lean-back", hant: "回正", hans: "回正", en: "Back to upright", who: .pair, target: .back),
        Step(id: "12-lean-left", hant: "兩只一起往左傾 45°", hans: "两只一起往左倾 45°", en: "Lean both 45° to the left", who: .pair, target: .rotate(.worldForward, degrees: -45)),
        Step(id: "13-lean-back", hant: "回正", hans: "回正", en: "Back to upright", who: .pair, target: .back),
    ])

    /// steps.json（临时流程，盖过内建）：只有文字，手动下一步
    static func custom(_ steps: [ControllerTestStep]) -> ControllerTestFlow {
        ControllerTestFlow(id: "custom", hant: "steps.json", hans: "steps.json", en: "steps.json", steps: steps.enumerated().map { i, s in
            Step(id: "\(i + 1)", hant: s.text, hans: s.text, en: s.text, who: nil, target: .manual)
        })
    }

    // —— 目标姿态 ——
    /// 固定姿势的重力方向（设备坐标，加速度读数在静止时就是这个方向）
    static func up(_ hold: Hold) -> SIMD3<Double> {
        switch hold {
        case .flat: return SIMD3(0, 0, 1)
        case .paddle: return SIMD3(-1, 0, 0)
        case .boxing: return SIMD3(0, 1, 0)
        }
    }
    /// 固定姿势的完整姿态（画淡色模型用；水平朝向之后会跟手上那只对齐）。side：L / R
    static func pose(_ hold: Hold, side: String) -> simd_quatd {
        let right = side != "L"
        // 三栏 = 设备 X、Y、Z 在世界里指向哪
        let (x, y, z): (SIMD3<Double>, SIMD3<Double>, SIMD3<Double>)
        switch hold {
        case .flat: (x, y, z) = (SIMD3(1, 0, 0), SIMD3(0, 1, 0), SIMD3(0, 0, 1))
        // 桌球握法：X 朝下；右只顶端（摇杆在下半）朝前、按键面朝右，左只顶端（摇杆在上半）朝人、按键面朝左
        case .paddle: (x, y, z) = right ? (SIMD3(0, 0, -1), SIMD3(0, 1, 0), SIMD3(1, 0, 0)) : (SIMD3(0, 0, -1), SIMD3(0, -1, 0), SIMD3(-1, 0, 0))
        // 拳击握法：顶端朝上，按键面朝向另一只（右只朝左、左只朝右）
        case .boxing: (x, y, z) = right ? (SIMD3(0, -1, 0), SIMD3(0, 0, 1), SIMD3(-1, 0, 0)) : (SIMD3(0, 1, 0), SIMD3(0, 0, 1), SIMD3(1, 0, 0))
        }
        return simd_quatd(simd_double3x3(x, y, z))
    }
    /// 相对转动：从开始姿态 start 绕轴转 degrees
    static func rotated(_ start: simd_quatd, _ axis: Axis, degrees: Double) -> simd_quatd {
        let a: SIMD3<Double>
        switch axis {
        case .worldUp: a = SIMD3(0, 0, 1)
        case .worldForward: a = SIMD3(0, 1, 0)
        case .worldRight: a = SIMD3(1, 0, 0)
        case .bodyLong: a = start.act(SIMD3(0, 1, 0))
        }
        return simd_normalize(simd_quatd(angle: degrees * .pi / 180, axis: a) * start)
    }
    /// 把目标的水平朝向对齐现在的姿态（只留倾斜的差）：q 对 target 的相对转动里取绕世界竖轴那一份
    static func yawAligned(_ target: simd_quatd, to live: simd_quatd) -> simd_quatd {
        let r = live * target.inverse
        let yaw = 2 * atan2(r.imag.z, r.real)
        return simd_normalize(simd_quatd(angle: yaw, axis: SIMD3(0, 0, 1)) * target)
    }
    /// 两个姿态差几度
    static func angle(_ a: simd_quatd, _ b: simd_quatd) -> Double {
        2 * acos(min(1, abs(simd_dot(a.vector, b.vector)))) * 180 / .pi
    }
    /// 两个方向差几度
    static func angle(_ a: SIMD3<Double>, _ b: SIMD3<Double>) -> Double {
        let n = simd_length(a) * simd_length(b)
        return n > 0 ? acos(max(-1, min(1, simd_dot(a, b) / n))) * 180 / .pi : 180
    }
    static let tolerance = 20.0      // 差几度以内算对上
    static let stillGyro = 20.0      // 角速度低于这个（度/秒）算停住
    static let holdSeconds = 1.0     // 对上并停住这么久自动下一步
}
