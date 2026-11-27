// S2Arm（不是上游文件）：手柄测试面板的内建测试流程（固定步骤：文字 + 淡色目标模型，不用图片素材）。
//   坐标同 PoseFilter：世界 Z 朝上、+Y = 离开屏幕前的人、+X = 右；设备 Z = 按键面朝外、+Y = 顶端、X = Y × Z（Joy-Con 2 左右只同一套轴，实录：平放面朝上两只都是加速度 +Z）。
//   目标有三种：
//   · 固定姿势（hold）：只比重力方向（水平朝向没有绝对参考）。淡色模型的水平朝向跟着手上的那只转，只差倾斜
//   · 相对转动（rotate）：从这一步开始时的姿态，绕某个轴转几度（往右转 = 绕世界竖轴 −90°）
//   · 回到上一步开始时的姿态（back）
//   实录对照（11-27 一对录制）：平放面朝上 ≈ (0, +0.12, +1.0) g；右只横拿、摇杆那头朝人、滑轨朝上 ≈ (−0.96, 0, 0)（内建流程改用滑轨朝下 = +X 朝上）；
//   立握≈ (±0.2, +0.98, ∓0.1)；横拿时沿长轴翻，陀螺 Y 积分 ≈ +98°
import Foundation
import simd

struct ControllerTestFlow {
    enum Who: Equatable { case single, pair }
    enum Hold: Equatable { case flat, edge, upright, handlebar }
    /// 转轴：worldUp 跟水平朝向无关；其余按手柄自己的方向定（水平朝向没有绝对参考，世界的「前 / 右」不一定是人的前 / 右）
    ///   farEndTip = 远端那头往下压的轴（横握时两只的顶端 +Y 都朝前）；faceForward = 立握时人的前方（右只 −X、左只 +X）为轴
    enum Axis: Equatable { case worldUp, farEndTip, faceForward, bodyLong }
    enum Target: Equatable {
        case hold(Hold)
        /// 从某一步做完时的姿态（from = 那一步的 id；nil = 上一步做完 / 这一步开始时）绕轴转几度；only = 只有这一边（L / R）转，另一边留在 from
        ///   mirror = 左只转反方向（正手：右手往右、左手往左）
        case rotate(Axis, degrees: Double, from: String? = nil, only: String? = nil, mirror: Bool = false)
        /// 回到某一步做完时的姿态
        case back(to: String)
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

    /// 内建两套，单只、一对分开测（测到一半不用去手柄面板改分配）。面板按选的手柄自动换到对应那套
    /// 单只 Joy-Con 2：起始姿势「横握・立」，回起始姿势只在下一个动作前需要时出现一次
    static let joyConSingle = ControllerTestFlow(id: "joycon-single", hant: "Joy-Con 單只測試", hans: "Joy-Con 单只测试", en: "Joy-Con single test", steps: [
        Step(id: "1-start", hant: "橫握・立：連接邊（滑軌）朝下立起來，搖桿朝向身體中間（右只朝左、左只朝右）", hans: "横握・立：连接边（滑轨）朝下立起来，摇杆朝向身体中间（右只朝左、左只朝右）",
             en: "Sideways, standing on the rail edge, stick facing the middle (right one faces left, left one faces right)", who: .single, target: .hold(.edge)),
        Step(id: "2-forehand", hant: "往正手方向水平轉 90°（右只往右、左只往左），停在收拍的位置", hans: "往正手方向水平转 90°（右只往右、左只往左），停在收拍的位置",
             en: "Turn 90° the forehand way (right one to the right, left one to the left) and stop at the finish", who: .single,
             target: .rotate(.worldUp, degrees: -90, from: "1-start", mirror: true)),
        Step(id: "3-start", hant: "回到起始姿勢", hans: "回到起始姿势", en: "Back to the start pose", who: .single, target: .back(to: "1-start")),
        Step(id: "4-face-up", hant: "沿長邊翻 90°，讓按鍵面朝上", hans: "沿长边翻 90°，让按键面朝上", en: "Roll it 90° along its length so the buttons face up", who: .single, target: .rotate(.bodyLong, degrees: 90, from: "1-start")),
        Step(id: "5-start", hant: "回到起始姿勢", hans: "回到起始姿势", en: "Back to the start pose", who: .single, target: .back(to: "1-start")),
        Step(id: "6-tip-down", hant: "前端往下壓 90°", hans: "前端往下压 90°", en: "Tip the far end down 90°", who: .single, target: .rotate(.farEndTip, degrees: -90, from: "1-start")),
    ])
    /// 一对 Joy-Con 2：每次倾斜都从立直算，不另加「回正」
    static let joyConPair = ControllerTestFlow(id: "joycon-pair", hant: "Joy-Con 一對測試", hans: "Joy-Con 一对测试", en: "Joy-Con pair test", steps: [
        Step(id: "1-upright", hant: "左右手各拿一只立起來，按鍵面朝向彼此", hans: "左右手各拿一只立起来，按键面朝向彼此",
             en: "One in each hand, upright, button faces toward each other", who: .pair, target: .hold(.upright)),
        Step(id: "2-lean-right", hant: "兩只一起往右傾 45°", hans: "两只一起往右倾 45°", en: "Lean both 45° to the right", who: .pair, target: .rotate(.faceForward, degrees: 45, from: "1-upright")),
        Step(id: "3-lean-left", hant: "兩只一起往左傾 45°（從立直算）", hans: "两只一起往左倾 45°（从立直算）", en: "Lean both 45° to the left (from upright)", who: .pair, target: .rotate(.faceForward, degrees: -45, from: "1-upright")),
        // 把手握法：两只横拿当左右把手（左只 ZL 在右、右只 ZR 在左，摇杆朝下），像转把手一样一起左右转，右手那只往自己方向转是加油门
        Step(id: "4-handlebar", hant: "兩手各橫拿一只當把手：左只 ZL 在右、右只 ZR 在左，搖桿都朝下", hans: "两手各横拿一只当把手：左只 ZL 在右、右只 ZR 在左，摇杆都朝下",
             en: "Hold one sideways in each hand like handlebars: left one's ZL end on the right, right one's ZR end on the left, sticks facing down", who: .pair, target: .hold(.handlebar)),
        // 把手往左下 / 右下倾：绕人的前方转（把手的滑轨朝前，同立握用的 faceForward 轴）
        Step(id: "5-tilt-right", hant: "兩只一起往右下傾斜 30°（右邊往下）", hans: "两只一起往右下倾斜 30°（右边往下）", en: "Tilt both 30° down to the right (right side down)",
             who: .pair, target: .rotate(.faceForward, degrees: 30, from: "4-handlebar")),
        Step(id: "6-tilt-left", hant: "兩只一起往左下傾斜 30°（左邊往下，從水平算）", hans: "两只一起往左下倾斜 30°（左边往下，从水平算）", en: "Tilt both 30° down to the left (left side down, from level)",
             who: .pair, target: .rotate(.faceForward, degrees: -30, from: "4-handlebar")),
        // 转握把（像转油门）：握把上缘往自己这边转；左右只顶端一个朝右一个朝左、绕远端方向转的角度同号
        Step(id: "7-twist-right", hant: "回到水平，右手那只往自己的方向轉 45°（像轉油門），左手那只不動", hans: "回到水平，右手那只往自己的方向转 45°（像转油门），左手那只不动",
             en: "Back to level, then twist the right one 45° toward you (like a throttle); keep the left one still",
             who: .pair, target: .rotate(.bodyLong, degrees: -45, from: "4-handlebar", only: "R")),
        Step(id: "8-twist-left", hant: "右手那只轉回來，換左手那只往自己的方向轉 45°", hans: "右手那只转回来，换左手那只往自己的方向转 45°",
             en: "Twist the right one back, then twist the left one 45° toward you",
             who: .pair, target: .rotate(.bodyLong, degrees: -45, from: "4-handlebar", only: "L")),
    ])
    static let builtIn = [joyConSingle, joyConPair]

    /// steps.json（临时流程，盖过内建）：只有文字，手动下一步
    static func custom(_ steps: [ControllerTestStep]) -> ControllerTestFlow {
        ControllerTestFlow(id: "custom", hant: "steps.json", hans: "steps.json", en: "steps.json", steps: steps.enumerated().map { i, s in
            Step(id: "\(i + 1)", hant: s.text, hans: s.text, en: s.text, who: nil, target: .manual)
        })
    }

    // —— 目标姿态 ——
    /// 固定姿势的重力方向（设备坐标，加速度读数在静止时就是这个方向）
    static func up(_ hold: Hold, side: String = "R") -> SIMD3<Double> {
        switch hold {
        case .flat: return SIMD3(0, 0, 1)
        case .edge: return SIMD3(side == "L" ? -1 : 1, 0, 0)   // 滑轨朝下（滑轨：右只 −X、左只 +X）
        case .upright: return SIMD3(0, 1, 0)
        case .handlebar: return SIMD3(0, 0, -1)   // 按键面朝下
        }
    }
    /// 固定姿势的完整姿态（画淡色模型用；水平朝向之后会跟手上那只对齐）。side：L / R
    static func pose(_ hold: Hold, side: String) -> simd_quatd {
        let right = side != "L"
        // 三栏 = 设备 X、Y、Z 在世界里指向哪
        let (x, y, z): (SIMD3<Double>, SIMD3<Double>, SIMD3<Double>)
        switch hold {
        case .flat: (x, y, z) = (SIMD3(1, 0, 0), SIMD3(0, 1, 0), SIMD3(0, 0, 1))
        // 横握・立（11-27 使用者定：连接边 / 滑轨朝下，两只的摇杆朝向对方）。滑轨在右只 −X、左只 +X 那侧，所以右只 X 朝上、左只 X 朝下；
        //   按键面右只朝左、左只朝右；这样两只的顶端（肩键那头，+Y）都朝前
        case .edge: (x, y, z) = right ? (SIMD3(0, 0, 1), SIMD3(0, 1, 0), SIMD3(-1, 0, 0)) : (SIMD3(0, 0, -1), SIMD3(0, 1, 0), SIMD3(1, 0, 0))
        // 立握：顶端朝上，按键面朝向另一只（右只朝左、左只朝右）
        case .upright: (x, y, z) = right ? (SIMD3(0, -1, 0), SIMD3(0, 0, 1), SIMD3(-1, 0, 0)) : (SIMD3(0, 1, 0), SIMD3(0, 0, 1), SIMD3(1, 0, 0))
        // 把手握法（11-27 使用者定）：横拿，左只 ZL 那头（顶端 +Y）在右、右只 ZR 那头在左，摇杆（按键面）都朝下；两只的滑轨都朝前
        case .handlebar: (x, y, z) = right ? (SIMD3(0, -1, 0), SIMD3(-1, 0, 0), SIMD3(0, 0, -1)) : (SIMD3(0, 1, 0), SIMD3(1, 0, 0), SIMD3(0, 0, -1))
        }
        return simd_quatd(simd_double3x3(x, y, z))
    }
    /// 相对转动：从开始姿态 start 绕轴转 degrees（side：L / R，左右只的「远端」「前方」不同）
    static func rotated(_ start: simd_quatd, _ axis: Axis, degrees: Double, side: String = "R") -> simd_quatd {
        let up = SIMD3<Double>(0, 0, 1), right = side != "L"
        func horizontal(_ v: SIMD3<Double>) -> SIMD3<Double> { let h = SIMD3(v.x, v.y, 0); return simd_length(h) > 1e-6 ? simd_normalize(h) : SIMD3(0, 1, 0) }
        let a: SIMD3<Double>
        switch axis {
        case .worldUp: a = up
        // 远端往下：远端的水平方向 f，绕 f × 上 转负角（f 朝前时就是绕「右」转 −90°）
        case .farEndTip: a = simd_normalize(simd_cross(horizontal(start.act(SIMD3(0, 1, 0))), up))
        // 往右倾：绕人的前方转正角（顶端往右）
        case .faceForward: a = horizontal(start.act(SIMD3(right ? -1 : 1, 0, 0)))
        case .bodyLong: a = start.act(SIMD3(0, right ? 1 : -1, 0))   // 绕远端方向（右只 +Y、左只 −Y）：同一个角度左右只翻的方向一样
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
    /// 示意：照理想动作一步步推出每一步的目标姿态（固定姿势用标准姿态，转动 / 回去照前面的结果），出示意图、单测用
    func illustration(side: String) -> [simd_quatd] {
        var ends: [String: simd_quatd] = [:], out: [simd_quatd] = []
        var current = simd_quatd(ix: 0, iy: 0, iz: 0, r: 1)
        for step in steps {
            switch step.target {
            case let .hold(hold): current = Self.pose(hold, side: side)
            case let .rotate(axis, degrees, from, only, mirror):
                let base = from.flatMap { ends[$0] } ?? current
                current = only.map { $0 == side } ?? true ? Self.rotated(base, axis, degrees: mirror && side == "L" ? -degrees : degrees, side: side) : base
            case let .back(to): current = ends[to] ?? current
            case .manual: break
            }
            ends[step.id] = current
            out.append(current)
        }
        return out
    }
    static let tolerance = 20.0      // 差几度以内算对上
    static let stillGyro = 20.0      // 角速度低于这个（度/秒）算停住
    static let holdSeconds = 1.0     // 对上并停住这么久自动下一步
}

/// 一代 Joy-Con 的读数口径（还没有真机校对）：读数来自 GameController 的 motion（重力 + 用户加速度 g、角速度 弧度/秒 → 换成 度/秒），
///   这里把它换成跟 Joy-Con 2 原始读数同一套设备轴（Z = 按键面朝外、+Y = 顶端、X = Y × Z），步骤目标、淡色模型、「遊戲看到的」才对得上。
///   现在先当成同一套（不换）。手上有一代 Joy-Con 的人：平放面朝上看加速度哪一轴是 +1、立起顶端朝上看哪一轴是 +1，
///   照 ControllerTestModelTests.testGen1AxesIdentityUntilCalibrated 的写法改这里，同时改那个单测
enum Gen1Axes {
    static func toDevice(_ s: IMUSample, side: String) -> IMUSample { s }
}

/// 一代 Joy-Con 的样子：整只机身一个颜色（读到的出厂机身色），按键另一个颜色；没读到用电光蓝 / 电光红
enum Gen1Look {
    static func defaultTint(left: Bool) -> ControllerTint { left ? ControllerTint(body: 0x0ab9e6, buttons: 0x001e1e) : ControllerTint(body: 0xff3c28, buttons: 0x1e0a0a) }
}
