// S2Arm（不是上游文件）：手柄测试面板的数据（开发工具，本地 / 自测构建才有，开关同「模擬手把」）。
//   面板（ControllerTestPanel）看连着的一只手柄：原始六轴、姿态、步骤、录制，用来对照体感转换（服务层 NpadImu.Push）对不对。
//   读数来源：
//   · Joy-Con 2 / Pro 2 手柄：启动器手柄导航那一条连接程序连接（LauncherPadSource）顺带开体感（连接程序一次只服务一个客户端，
//     另开一条会把导航和游戏宿主踢掉）。游戏进行中 / 分配玩家中这条连接本来就放开，读数跟着停
//   · 系统蓝牙手柄（PS5 / PS4 / Xbox / 一代 Pro 手柄等）：GameController 的 motion（选中时才开感测器）
//   一代 Joy-Con 的 HID 读数还没接（只列出经系统认到的那份）
import AppKit
import Combine
import GameController
import LauncherPadBridge
import NXCore
import simd

/// 体感读数：加速度 g、角速度 度/秒，设备轴序
struct IMUSample: Equatable {
    var accel: SIMD3<Double>
    var gyro: SIMD3<Double>
}

/// 给游戏的那一份（同服务层 compat/…/HidDevices/NpadImu.cs Push 的转换；单测 ControllerTestModelTests 读那个文件对照，两边改了要一起改）
enum ControllerTestIMU {
    enum Shape: Equatable { case single, pair, other }
    static func gameView(_ s: IMUSample, shape: Shape) -> IMUSample {
        var out = s
        switch shape {
        case .single:   // 单只：按 Y 轴镜像（加速度 Y、角速度 X / Z 取反）
            out.accel.y = -s.accel.y; out.gyro.x = -s.gyro.x; out.gyro.z = -s.gyro.z
        case .pair:     // 一对：绕 X 轴转 180°（加速度、角速度的 Y / Z 取反）
            out.accel.y = -s.accel.y; out.accel.z = -s.accel.z; out.gyro.y = -s.gyro.y; out.gyro.z = -s.gyro.z
        case .other: break
        }
        return out
    }
}

/// 姿态：静止时重力定上下、侧倾，陀螺随时更新（互补滤波）。世界坐标 Z 朝上、+Y = 离开屏幕前的人；设备坐标照读数轴序
struct PoseFilter {
    private(set) var q = simd_quatd(ix: 0, iy: 0, iz: 0, r: 1)
    private(set) var started = false
    static let up = SIMD3<Double>(0, 0, 1)
    var gain = 2.0   // 重力拉回的力度（1/秒）

    mutating func update(_ s: IMUSample, dt: Double) {
        let a = s.accel, norm = simd_length(a)
        if !started {
            guard norm > 0.1 else { return }
            q = simd_quatd(from: simd_normalize(a), to: Self.up)
            started = true
            recenter()
            return
        }
        var w = s.gyro * (.pi / 180)
        // 只在接近 1 g（没在甩）时用重力修：甩的时候加速度不是重力
        if norm > 0.1, abs(norm - 1) < 0.3 {
            let expected = q.inverse.act(Self.up)
            w += gain * simd_cross(a / norm, expected)
        }
        let angle = simd_length(w) * max(0, min(dt, 0.05))
        if angle > 1e-9 { q = simd_normalize(q * simd_quatd(angle: angle, axis: simd_normalize(w))) }
    }
    /// 歸正：水平朝向没有绝对参考、会飘。屏幕面（+Z）朝着人的就让它正对屏幕前；平放（屏幕面朝上）的让顶端（+Y）指向屏幕里
    mutating func recenter() {
        func heading(_ v: SIMD3<Double>) -> Double? { simd_length(SIMD2(v.x, v.y)) > 0.3 ? atan2(v.y, v.x) : nil }
        let target: Double, now: Double
        if let h = heading(q.act(SIMD3(0, 0, 1))) { now = h; target = -.pi / 2 }
        else if let h = heading(q.act(SIMD3(0, 1, 0))) { now = h; target = .pi / 2 }
        else { return }
        q = simd_normalize(simd_quatd(angle: target - now, axis: Self.up) * q)
    }
    /// 设备上一点（设备坐标）现在在世界的哪
    func world(_ p: SIMD3<Double>) -> SIMD3<Double> { q.act(p) }
}

/// 姿态说明（平放 / 立著 / 橫拿 + 靜止 / 往左右轉）
enum PoseText {
    static func describe(_ s: IMUSample, pose: PoseFilter) -> String {
        let a = s.accel, ax = abs(a.x), ay = abs(a.y), az = abs(a.z)
        let hold = az >= ax && az >= ay ? SimText.pick("平放", "平放", "Flat")
            : ay >= ax ? SimText.pick("立著", "立着", "Upright") : SimText.pick("橫拿", "横拿", "Sideways")
        let speed = simd_length(s.gyro)
        if speed < 8 { return hold + "\n" + SimText.pick("靜止", "静止", "Still") }
        let yaw = pose.world(s.gyro).z   // 绕世界竖轴的分量：正 = 从上往下看逆时针 = 往左
        let value = String(format: "%.0f°/s", abs(yaw) > 0.6 * speed ? abs(yaw) : speed)
        if abs(yaw) > 0.6 * speed {
            return hold + "\n" + (yaw > 0 ? SimText.pick("往左轉中 ", "往左转中 ", "Turning left ") : SimText.pick("往右轉中 ", "往右转中 ", "Turning right ")) + value
        }
        return hold + "\n" + SimText.pick("轉動中 ", "转动中 ", "Rotating ") + value
    }
}

/// 步骤文件（~/S2Arm/dev/controller-test/steps.json）：字符串数组，或 {"text": …, "seconds": 3} 的数组，或 {"steps": [...]}
struct ControllerTestStep: Equatable {
    var text: String
    var seconds: Double?
    static func parse(_ data: Data) -> [ControllerTestStep]? {
        guard let json = try? JSONSerialization.jsonObject(with: data) else { return nil }
        let list = (json as? [Any]) ?? ((json as? [String: Any])?["steps"] as? [Any])
        return list?.compactMap { item in
            if let text = item as? String { return ControllerTestStep(text: text) }
            guard let d = item as? [String: Any], let text = d["text"] as? String else { return nil }
            return ControllerTestStep(text: text, seconds: (d["seconds"] as? NSNumber)?.doubleValue)
        }
    }
}

@MainActor final class ControllerTestHub: ObservableObject {
    static let shared = ControllerTestHub()
    /// 开关同「模擬手把」（ControllerColorSimulator.allowed）
    static var allowed: Bool { ControllerColorSimulator.allowed }
    static var folder: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("S2Arm/dev/controller-test", isDirectory: true) }
    static var stepsURL: URL { folder.appendingPathComponent("steps.json") }

    // —— 接进来的（LauncherView 接上）——
    weak var controller: NS2LauncherController?
    weak var systems: SystemControllers?
    weak var source: LauncherPadSource?
    var gameRunning: () -> Bool = { false }

    // —— 要看的手柄 ——
    struct Target: Identifiable, Equatable {
        enum Kind: Equatable {
            case relay(player: Int, shape: ControllerTestIMU.Shape)   // Joy-Con 2 / Pro 2 手柄（连接程序）
            case system(ObjectIdentifier)                             // 系统蓝牙手柄
            case gen1(left: String?, right: String?)                  // 一代 Joy-Con（HID 设备 id）：读数取自系统认到的那一只（gen1Owners 对应）
            case simulated                                            // 「模擬手把」加的，没有读数
        }
        enum Model: Equatable { case joyCon2, joyCon1, whole }
        let id: String
        let title: String
        let player: String   // P1… / —（没有玩家位）
        let meta: String
        let kind: Kind
        let dots: [ControllerTint?]   // 颜色点：一对两颗
        var joyConSide: String? = nil   // 单只 Joy-Con 是哪一只（L / R）：模型画左只还是右只、录制的 side 栏
        var model: Model = .joyCon2
        var pair = false                // 一对（Joy-Con 2 拼成一对 / 一代按分配是一对）
        var isPair: Bool { pair || { if case .relay(_, .pair) = kind { return true }; return false }() }
        var wholePad: Bool { model == .whole }
    }
    @Published var selectedID: String? { didSet { if selectedID != oldValue { selectionChanged() } } }
    @Published var poseFromGame = false
    /// 3D 视角回正（按一下加一，PoseView 看到变了就放回原来的镜头）
    @Published var viewReset = 0
    /// 面板开着（不开就不读、不开体感）
    private(set) var open = false

    /// 跟手柄面板同一份：连接程序的手柄（拼成一对的算一只）、系统蓝牙手柄；有没有玩家位都列
    var targets: [Target] {
        guard let controller else { return [] }
        let slots = controller.assignment.slots
        func player(_ match: (PlayerAssignment.Unit) -> Bool) -> String {
            slots.firstIndex { $0.map(match) ?? false }.map { "P\($0 + 1)" } ?? "—"
        }
        let sim = ControllerColorSimulator.shared
        var merged: Set<Int> = []
        if controller.kind == 4 && controller.players == 1 { merged.insert(0) }
        for extra in controller.extras where extra.kind == 4 && extra.players == 1 { merged.insert(extra.group) }
        if ControllerColorSimulator.allowed { merged.formUnion(sim.ns2MergedGroups) }
        let lang = (Lang.find(UserDefaults.standard.string(forKey: "appLanguage") ?? "") ?? .device()).code
        let relayMeta = SimText.pick("連接程式", "连接程序", "Connection helper"), simMeta = SimText.pick("模擬", "模拟", "Simulated")
        func tint(_ id: String, left: Bool) -> ControllerTint? { sim.ns2Tint(id) ?? ControllerTint(body: left ? 0x41b8e8 : 0xef5a5a) }
        var out: [Target] = []
        let devices = controller.allDevices
        var groups: [Int] = []
        for d in devices where !groups.contains(d.group) { groups.append(d.group) }
        for g in groups {
            let inGroup = devices.filter { $0.group == g }
            if merged.contains(g), let l = inGroup.first(where: { $0.kind == 2 }), let r = inGroup.first(where: { $0.kind == 3 }) {
                let simulated = l.id.hasPrefix("sim-")
                out.append(Target(id: "ns2:\(l.id)+\(r.id)", title: SimText.pick("Joy-Con 2（一對）", "Joy-Con 2（一对）", "Joy-Con 2 (pair)"),
                                  player: player { if case let .pair(a, b) = $0 { return a == l.id || b == r.id }; return false },
                                  meta: (simulated ? simMeta : SimText.pick("左 + 右・", "左 + 右・", "L + R · ") + relayMeta),
                                  kind: simulated ? .simulated : .relay(player: l.player, shape: .pair), dots: [tint(l.id, left: true), tint(r.id, left: false)]))
                continue
            }
            for d in inGroup {
                let simulated = d.id.hasPrefix("sim-")
                let joyCon = d.kind == 2 || d.kind == 3
                out.append(Target(id: "ns2:\(d.id)", title: NS2LauncherController.title(kind: d.kind, lang: lang),
                                  player: player { switch $0 { case let .single(x, _), let .whole(x, _): return x == d.id; case let .pair(a, b): return a == d.id || b == d.id; default: return false } },
                                  meta: simulated ? simMeta : relayMeta,
                                  kind: simulated ? .simulated : .relay(player: d.player, shape: joyCon ? .single : .other),
                                  dots: joyCon ? [tint(d.id, left: d.kind == 2)] : [ControllerTint(body: 0x3b3b3b)],
                                  joyConSide: joyCon ? (d.kind == 2 ? "L" : "R") : nil, model: joyCon ? .joyCon2 : .whole))
            }
        }
        let pads = systems?.pads ?? []
        // 一代 Joy-Con：按分配看单只 / 一对（同手柄面板）；系统合成的「Joy-Con (L/R)」等不另列，读数从它取
        out += gen1Targets(slots: slots, player: player)
        let simPads = Set(ControllerColorSimulator.allowed ? sim.systemPads().map(\.id) : [])
        for (i, pad) in pads.enumerated() where ![.gen1Left, .gen1Right, .gen1Pair].contains(pad.family) {
            let index = pads[..<i].filter { $0.name == pad.name }.count
            let simulated = simPads.contains(pad.id)
            out.append(Target(id: "sys:\(pad.name)#\(index)", title: pad.name,
                              player: player { if case let .system(n, x) = $0 { return n == pad.name && x == index }; return false },
                              meta: simulated ? simMeta : SimText.pick("系統手把", "系统手柄", "System controller"),
                              kind: simulated ? .simulated : .system(pad.id), dots: [nil], model: .whole))
        }
        return out
    }
    /// 一代 Joy-Con 的目标：分配里的一对（.gen1Pair，或系统合成的那一位里两只都认得）算一只一对，其余每只单只；颜色用读到的出厂色
    private func gen1Targets(slots: [PlayerAssignment.Unit?], player: ((PlayerAssignment.Unit) -> Bool) -> String) -> [Target] {
        guard let systems, let controller else { return [] }
        let devices = systems.gen1
        guard !devices.isEmpty else { return [] }
        let owners = controller.assignment.gen1Owners(system: SystemControllers.keys(for: systems.pads), gen1: devices)
        func tint(_ id: String?, left: Bool) -> ControllerTint? { id.flatMap { systems.gen1Colors[$0] } ?? Gen1Look.defaultTint(left: left) }
        let meta = SimText.pick("一代・系統手把", "一代・系统手柄", "Gen-1 · system")
        var out: [Target] = [], used = Set<String>()
        var pairs: [(String, String)] = []
        for unit in slots.compactMap({ $0 }) {
            switch unit {
            case let .gen1Pair(l, r): pairs.append((l, r))
            case .system:   // 系统合成的一位：里面左右两只都认得 = 一对
                for (_, sides) in owners { if let l = sides[0], let r = sides[1], !pairs.contains(where: { $0.0 == l }) { pairs.append((l, r)) } }
            default: break
            }
        }
        for (l, r) in pairs where devices.contains(where: { $0.id == l }) && devices.contains(where: { $0.id == r }) && !used.contains(l) {
            used.formUnion([l, r])
            let simulated = l.hasPrefix("sim-")
            out.append(Target(id: "g1:\(l)+\(r)", title: SimText.pick("一代 Joy-Con（一對）", "一代 Joy-Con（一对）", "Gen-1 Joy-Con (pair)"),
                              player: player { switch $0 { case let .gen1Pair(a, b): return a == l || b == r; case let .system(n, i): return owners[.init(name: n, index: i)]?[0] == l; default: return false } },
                              meta: simulated ? SimText.pick("模擬", "模拟", "Simulated") : meta, kind: simulated ? .simulated : .gen1(left: l, right: r),
                              dots: [tint(l, left: true), tint(r, left: false)], model: .joyCon1, pair: true))
        }
        for d in devices where !used.contains(d.id) {
            let left = d.side == 0, simulated = d.id.hasPrefix("sim-")
            out.append(Target(id: "g1:\(d.id)", title: left ? SimText.pick("一代 Joy-Con（左）", "一代 Joy-Con（左）", "Gen-1 Joy-Con (L)") : SimText.pick("一代 Joy-Con（右）", "一代 Joy-Con（右）", "Gen-1 Joy-Con (R)"),
                              player: player { switch $0 { case let .gen1(x, _): return x == d.id; case let .system(n, i): return owners[.init(name: n, index: i)]?[d.side] == d.id; default: return false } },
                              meta: simulated ? SimText.pick("模擬", "模拟", "Simulated") : meta,
                              kind: simulated ? .simulated : .gen1(left: left ? d.id : nil, right: left ? nil : d.id),
                              dots: [tint(d.id, left: left)], joyConSide: left ? "L" : "R", model: .joyCon1))
        }
        return out
    }
    /// 这一边画哪种模型
    func shape(for channel: Channel) -> PoseView.Shape {
        guard let target = selected else { return .wholePad }
        let right = channel.side == "R"
        switch target.model {
        case .whole: return .wholePad
        case .joyCon1: return right ? .gen1Right : .gen1Left
        case .joyCon2: return right ? .joyConRight : .joyConLeft
        }
    }
    var selected: Target? {
        let all = targets
        return all.first { $0.id == selectedID } ?? all.first
    }

    // —— 读数 ——
    /// 一只（一对里的一边，或整只）：最近 3 秒的读数、取样率、两份姿态（原始 / 游戏看到的）、顶端的轨迹
    final class Channel {
        let side: String   // L / R / -
        let shape: ControllerTestIMU.Shape
        var history: [(t: Double, s: IMUSample)] = []
        var times: [Double] = []
        var rawPose = PoseFilter(), gamePose = PoseFilter()
        var trailRaw: [(t: Double, p: SIMD3<Double>)] = [], trailGame: [(t: Double, p: SIMD3<Double>)] = []
        var lastT: Double?
        var lastStamp: UInt64 = 0
        var last: IMUSample? { history.last?.s }
        init(side: String, shape: ControllerTestIMU.Shape) { self.side = side; self.shape = shape }
        var rate: Int { times.count }
        func add(_ s: IMUSample, at t: Double, tip: SIMD3<Double>) {
            let dt = lastT.map { t - $0 } ?? 0
            lastT = t
            rawPose.update(s, dt: dt)
            gamePose.update(ControllerTestIMU.gameView(s, shape: shape), dt: dt)
            history.append((t, s)); times.append(t)
            trailRaw.append((t, rawPose.world(tip))); trailGame.append((t, gamePose.world(tip)))
            if let i = history.firstIndex(where: { $0.t >= t - 3 }), i > 0 { history.removeFirst(i) }
            if let i = times.firstIndex(where: { $0 >= t - 1 }), i > 0 { times.removeFirst(i) }
            if let i = trailRaw.firstIndex(where: { $0.t >= t - 1.5 }), i > 0 { trailRaw.removeFirst(i); trailGame.removeFirst(min(i, trailGame.count)) }
        }
        func recenter() { rawPose.recenter(); gamePose.recenter() }
    }
    /// 选中那只的各边（一对 = L、R；其余一个「-」）
    private(set) var channels: [Channel] = []
    /// 设备上「顶端」那一点（画轨迹用）：Joy-Con 长轴 +Y 那头；整只手柄同一点
    static let tip = SIMD3<Double>(0, 0.05, 0)


    // —— 面板开关 ——
    private var timer: Timer?
    private var lastHostCheck: Double = 0
    private(set) var hostRunning = false
    func panelOpened() {
        open = true
        loadSteps(force: true)
        selectionChanged()
        guard timer == nil else { return }
        let t = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in MainActor.assumeIsolated { self?.tick() } }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }
    func panelClosed() {
        open = false
        timer?.invalidate(); timer = nil
        stopRun()
        releaseMotionPad()
    }

    /// 导航那条连接要不要开体感：面板开着、选的是连接程序的手柄
    var wantsRelayMotion: Bool { open && !hostRunning && { if case .relay = selected?.kind { return true }; return false }() }
    /// 面板开着时发现游戏宿主在跑（不是从这个启动器开的也算）：导航那条连接也放开，别把游戏踢掉
    var blocksRelay: Bool { open && hostRunning }

    enum Status: Equatable { case ok, noTargets, simulated, gameRunning, assigning, relayClosed, noMotion, waiting }
    var status: Status {
        guard let target = selected else { return .noTargets }
        switch target.kind {
        case .simulated: return .simulated
        case .relay:
            if gameRunning() || hostRunning { return .gameRunning }
            if controller?.assigning == true { return .assigning }
            if source?.relayOpen != true { return .relayClosed }
        case .system, .gen1:
            if motionPads.isEmpty || motionPads.contains(where: { $0.pad?.motion == nil }) { return .noMotion }
        }
        return channels.contains { $0.last != nil } ? .ok : .waiting
    }
    var sourceName: String {
        switch selected?.kind {
        case .relay: return SimText.pick("Joy-Con 2 連接程式", "Joy-Con 2 连接程序", "Joy-Con 2 helper")
        case .system: return "GameController"
        case .gen1: return motionPads.count == 1 && channels.count == 2 ? SimText.pick("GameController（一對共用一份）", "GameController（一对共用一份）", "GameController (one stream for the pair)") : "GameController"
        default: return "—"
        }
    }

    private func selectionChanged() {
        releaseMotionPad()
        shownTargetID = selected?.id
        stepStarts = [:]; matchedSince = nil
        if !running { pickFlow() }
        guard let target = selected else { channels = []; return }
        switch target.kind {
        case let .relay(_, shape):
            channels = shape == .pair ? [Channel(side: "L", shape: .pair), Channel(side: "R", shape: .pair)] : [Channel(side: target.joyConSide ?? "-", shape: shape)]
        case .system:
            channels = [Channel(side: "-", shape: .other)]
            if open { attachMotion() }
        case let .gen1(l, r):
            channels = target.isPair ? [Channel(side: "L", shape: .pair), Channel(side: "R", shape: .pair)] : [Channel(side: l != nil ? "L" : "R", shape: .single)]
            if open { attachMotion() }
        case .simulated: channels = []
        }
        objectWillChange.send()
    }
    private var shownTargetID: String?
    private func tick() {
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastHostCheck > 1 {
            lastHostCheck = now
            hostRunning = NSWorkspace.shared.runningApplications.contains { $0.executableURL?.lastPathComponent == "s2arm-host" }
            loadSteps()
        }
        // 清单变了（选的那只断开 / 拼对拆开）：换新的那几边
        let current = selected?.id
        if current != shownTargetID { shownTargetID = current; selectionChanged() }
        if motionPads.isEmpty, let kind = selected?.kind, { if case .system = kind { return true }; if case .gen1 = kind { return true }; return false }() { attachMotion() }
        checkStep(now: now)
        if recording != nil, now - lastFlush > 1 { lastFlush = now; flushRecording() }
        objectWillChange.send()
    }

    /// 导航那条连接送来的六轴（LauncherPadSource.receiveMotion）
    func receive(_ batch: [s2a_launcher_pad_motion]) {
        guard open, let target = selected, case let .relay(player, shape) = target.kind else { return }
        let now = ProcessInfo.processInfo.systemUptime
        for m in batch where Int(m.player) == player {
            guard let channel = shape == .pair ? channels.first(where: { $0.side == (m.side == 1 ? "L" : "R") }) : channels.first else { continue }
            // 连接程序每 8 ms 心跳会重送同一笔：按时间戳去重，取样率才是真的
            if m.host_timestamp_us != 0 {
                if m.host_timestamp_us == channel.lastStamp { continue }
                channel.lastStamp = m.host_timestamp_us
            }
            let s = IMUSample(accel: SIMD3(Double(m.ax), Double(m.ay), Double(m.az)), gyro: SIMD3(Double(m.gx), Double(m.gy), Double(m.gz)))
            if m.host_timestamp_us == 0, channel.last == s { continue }
            let t = m.host_timestamp_us != 0 ? Double(m.host_timestamp_us) / 1e6 : now
            add(s, to: channel, at: t)
        }
    }
    private func add(_ s: IMUSample, to channel: Channel, at t: Double) {
        // 时间戳是连接程序那边的时钟：轨迹 / 波形只看相对时间，换来源时清掉
        if let last = channel.lastT, t < last || t - last > 5 { channel.history.removeAll(); channel.times.removeAll(); channel.trailRaw.removeAll(); channel.trailGame.removeAll(); channel.lastT = nil }
        channel.add(s, at: t, tip: Self.tip)
        record(s, side: channel.side)
    }
    /// 歸正：各边转水平朝向；这一轮记下的各步姿态跟着转同一份，示意模型才不会跟手上的错开
    func recenter() {
        for c in channels {
            let before = c.rawPose.q
            c.recenter()
            let delta = c.rawPose.q * before.inverse
            for (i, var starts) in stepStarts { if let q = starts[c.side] { starts[c.side] = simd_normalize(delta * q); stepStarts[i] = starts } }
        }
    }

    /// 系统手柄的读数源：哪个 GameController 给哪几边（一代一对被系统合成一只时两边同一个）
    private struct MotionSource { weak var pad: GCController?; let sides: [String]; let activated: Bool }
    private var motionPads: [MotionSource] = []
    /// 选中那只每一边的读数来自哪个系统手柄
    private func motionAssignments() -> [(ObjectIdentifier, [String])] {
        guard let target = selected, let systems else { return [] }
        switch target.kind {
        case let .system(id): return [(id, ["-"])]
        case let .gen1(l, r):
            let keys = SystemControllers.keys(for: systems.pads)
            let owners = controller?.assignment.gen1Owners(system: keys, gen1: systems.gen1) ?? [:]
            var out: [ObjectIdentifier: [String]] = [:]
            for (side, id, family) in [("L", l, SystemControllers.Family.gen1Left), ("R", r, .gen1Right)] {
                guard let id else { continue }
                let channelSide = target.isPair ? side : channels.first?.side ?? side
                // 认得是哪一位（gen1Owners）就用那一位；认不得：同一边只有一只系统手柄时用它
                let key = owners.first { $0.value.values.contains(id) }?.key
                let pad = key.flatMap { k in zip(keys, systems.pads).first { $0.0 == k }?.1 }
                    ?? { let c = systems.pads.filter { $0.family == family }; return c.count == 1 ? c[0] : nil }()
                    ?? { let c = systems.pads.filter { $0.family == .gen1Pair }; return c.count == 1 ? c[0] : nil }()
                if let pad { out[pad.id, default: []].append(channelSide) }
            }
            return out.map { ($0.key, $0.value) }
        default: return []
        }
    }
    private func attachMotion() {
        releaseMotionPad()
        let all = GCController.controllers()
        for (id, sides) in motionAssignments() {
            guard let pad = all.first(where: { ObjectIdentifier($0) == id }) else { continue }
            var activated = false
            if let motion = pad.motion {
                if motion.sensorsRequireManualActivation, !motion.sensorsActive { motion.sensorsActive = true; activated = true }
                // 启动器别处不接 motion 的回调；面板关了 / 换手柄就还原
                motion.valueChangedHandler = { [weak self] m in MainActor.assumeIsolated { self?.receiveSystem(m, sides: sides) } }
            }
            motionPads.append(MotionSource(pad: pad, sides: sides, activated: activated))
        }
    }
    private func receiveSystem(_ m: GCMotion, sides: [String]) {
        guard open, let target = selected else { return }
        let a = m.hasGravityAndUserAcceleration ? SIMD3(m.gravity.x + m.userAcceleration.x, m.gravity.y + m.userAcceleration.y, m.gravity.z + m.userAcceleration.z)
            : SIMD3(m.acceleration.x, m.acceleration.y, m.acceleration.z)
        let r = m.rotationRate
        let raw = IMUSample(accel: a, gyro: SIMD3(r.x, r.y, r.z) * (180 / .pi))
        for channel in channels where sides.contains(channel.side) || sides == ["-"] {
            // 一代 Joy-Con：GameController 的轴换成跟 Joy-Con 2 原始读数同一套（Gen1Axes，待真机校正）
            let s = target.model == .joyCon1 ? Gen1Axes.toDevice(raw, side: channel.side) : raw
            if channel.last == s { continue }
            add(s, to: channel, at: ProcessInfo.processInfo.systemUptime)
        }
    }
    private func releaseMotionPad() {
        for source in motionPads {
            guard let motion = source.pad?.motion else { continue }
            motion.valueChangedHandler = nil
            if source.activated { motion.sensorsActive = false }
        }
        motionPads = []
    }

    // —— 步骤：内建流程（文字 + 淡色目标模型，对上停住 1 秒自动下一步），steps.json 有就多一个「临时流程」可选 ——
    @Published private(set) var customSteps: [ControllerTestStep] = []
    /// 哪套流程：跟着选的手柄（单只 / 一对），有 steps.json 就用 "custom"（面板不给选单）
    @Published var flowID = ControllerTestFlow.joyConSingle.id { didSet { if flowID != oldValue { stopRun(); restartFlow() } } }   // 换流程：停掉这一轮
    var flow: ControllerTestFlow {
        if flowID == "custom", !customSteps.isEmpty { return .custom(customSteps) }
        return ControllerTestFlow.builtIn.first { $0.id == flowID } ?? .joyConSingle
    }
    @Published private(set) var stepIndex = 0
    private(set) var stepStartedAt: Double = 0
    /// 每一步开始时各边的姿态（原始读数那份）：相对转动 / 回去的目标从这里算
    private var stepStarts: [Int: [String: simd_quatd]] = [:]
    private var matchedSince: Double?
    private var stepsStamp: Date?
    var step: ControllerTestFlow.Step? { flow.steps.indices.contains(stepIndex) ? flow.steps[stepIndex] : nil }
    func loadSteps(force: Bool = false) {
        let url = Self.stepsURL
        let stamp = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
        guard force || stamp != stepsStamp else { return }
        stepsStamp = stamp
        let next = stamp == nil ? [] : (try? Data(contentsOf: url)).flatMap(ControllerTestStep.parse) ?? []
        guard next != customSteps else { return }
        customSteps = next
        if running { stopRun() }
        if flowID == "custom" { restartFlow() }
        pickFlow()
    }
    func restartFlow() { goTo(0) }
    /// 按选的手柄定流程：一对 → 一对测试、其余 → 单只测试；有 steps.json 就用它
    ///   Joy-Con（单只 / 一对）一律用内建流程（有示意）；steps.json 只给没有内建流程的手柄（Pro、PS、Xbox 等）用
    ///   （11-27：留着一份旧的 steps.json，Joy-Con 也被它盖掉，测试时看不到示意模型）
    private func pickFlow() {
        guard let target = selected else { flowID = ControllerTestFlow.joyConSingle.id; return }
        if target.isPair { flowID = ControllerTestFlow.joyConPair.id }
        else if target.joyConSide != nil { flowID = ControllerTestFlow.joyConSingle.id }
        else { flowID = customSteps.isEmpty ? ControllerTestFlow.joyConSingle.id : "custom" }
    }
    /// 按「開始」才跑流程：示意模型、对上自动下一步、录制都在开始之后（11-27：没按开始，平放着的手柄就被当成第 1 步对上、自己往下走）
    @Published private(set) var running = false
    @Published private(set) var finished = false
    func toggleRun() { if running { stopRun() } else { startRun() } }
    /// 从现在选的这一步开始（上一步 / 下一步先选好，不强制从第 1 步）
    func startRun() {
        finished = false
        goTo(stepIndex)
        running = true
        startRecording()
    }
    func stopRun() {
        running = false
        matchedSince = nil
        stopRecording()
    }
    func step(_ delta: Int) { goTo(stepIndex + delta) }
    private func goTo(_ index: Int) {
        stepIndex = max(0, min(flow.steps.count - 1, index))
        stepStartedAt = ProcessInfo.processInfo.systemUptime
        matchedSince = nil
        var starts: [String: simd_quatd] = [:]
        for c in channels where c.rawPose.started { starts[c.side] = c.rawPose.q }
        stepStarts[stepIndex] = starts
        objectWillChange.send()
    }
    /// 选的手柄合不合这一步（单只的步骤要选单只 Joy-Con 2，一对的步骤要选一对）
    var stepFitsSelection: Bool {
        guard let who = step?.who else { return true }
        switch who {
        case .single: return selected?.joyConSide != nil
        case .pair: return selected?.isPair == true
        }
    }
    /// 右上角示意图：这一步的标准目标姿态；转动 / 回去类的步骤附上前一步的标准姿态（示意从那里转过来）。没开始 / 手柄不合这一步就 nil
    func illustration(side: String) -> (from: simd_quatd?, to: simd_quatd)? {
        guard running, let step, step.target != .manual, stepFitsSelection else { return nil }
        let poses = flow.illustration(side: side)
        guard poses.indices.contains(stepIndex) else { return nil }
        if case .hold = step.target { return (nil, poses[stepIndex]) }
        return (stepIndex > 0 ? poses[stepIndex - 1] : nil, poses[stepIndex])
    }
    /// 某一步做完时的姿态 = 下一步开始时记下的
    private func endPose(of id: String, side: String) -> simd_quatd? {
        guard let i = flow.steps.firstIndex(where: { $0.id == id }) else { return nil }
        return stepStarts[i + 1]?[side]
    }
    /// 这一边这一步的目标姿态（淡色模型）；没有目标 / 还没有姿态就 nil
    func ghost(for channel: Channel) -> simd_quatd? {
        guard running, let step, stepFitsSelection, channel.rawPose.started else { return nil }
        let side = channel.side
        switch step.target {
        case let .hold(hold): return ControllerTestFlow.yawAligned(ControllerTestFlow.pose(hold, side: side), to: channel.rawPose.q)
        case let .rotate(axis, degrees, from, only, mirror):
            guard let base = from.flatMap({ endPose(of: $0, side: side) }) ?? stepStarts[stepIndex]?[side] else { return nil }
            if let only, only != side { return base }   // 只有另一边转：这一边留在原处
            return ControllerTestFlow.rotated(base, axis, degrees: mirror && side == "L" ? -degrees : degrees, side: side)
        // 从中间某一步开始、那一步还没做过：用标准示意姿态（水平朝向跟着手上那只）
        case let .back(to): return endPose(of: to, side: side) ?? ControllerTestFlow.yawAligned(flow.illustration(side: side)[stepIndex], to: channel.rawPose.q)
        case .manual: return nil
        }
    }
    /// 这一边对上了没（停住、差 20° 以内）
    func matches(_ channel: Channel) -> Bool {
        guard let s = channel.last, simd_length(s.gyro) < ControllerTestFlow.stillGyro, let step else { return false }
        if case let .hold(hold) = step.target { return ControllerTestFlow.angle(s.accel, ControllerTestFlow.up(hold, side: channel.side)) < ControllerTestFlow.tolerance }
        guard let target = ghost(for: channel) else { return false }
        return ControllerTestFlow.angle(channel.rawPose.q, target) < ControllerTestFlow.tolerance
    }
    /// 对上停住的进度 0…1（到 1 自动下一步）
    var holdProgress: Double {
        guard let since = matchedSince else { return 0 }
        return min(1, (ProcessInfo.processInfo.systemUptime - since) / ControllerTestFlow.holdSeconds)
    }
    private func checkStep(now: Double) {
        guard running, let step, step.target != .manual, stepFitsSelection, !channels.isEmpty else { matchedSince = nil; return }
        // 开始姿态还没记到（刚选手柄 / 读数刚来）：现在补记
        if stepStarts[stepIndex]?.count != channels.filter(\.rawPose.started).count {
            var starts = stepStarts[stepIndex] ?? [:]
            for c in channels where c.rawPose.started && starts[c.side] == nil { starts[c.side] = c.rawPose.q }
            stepStarts[stepIndex] = starts
        }
        guard channels.allSatisfy(matches) else { matchedSince = nil; return }
        if matchedSince == nil { matchedSince = now }
        guard now - matchedSince! >= ControllerTestFlow.holdSeconds else { return }
        if stepIndex < flow.steps.count - 1 { goTo(stepIndex + 1) } else { finished = true; stopRun(); goTo(0) }   // 最后一步对上：测完、停录
    }

    // —— 录制：每次一个文件（时间 + 流程），不盖掉之前的 ——
    private var recording: FileHandle?
    private var recordBuffer = ""
    private(set) var recordStart: Double = 0
    private(set) var recordCount = 0
    private(set) var recordURL: URL?
    private var lastFlush: Double = 0
    var isRecording: Bool { recording != nil }
    @Published private(set) var recordError: String?
    nonisolated static func recordName(date: Date, flow: String) -> String {
        let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.dateFormat = "yyyyMMdd-HHmmss"
        return "readings-\(f.string(from: date))-\(flow).tsv"
    }
    private func startRecording() {
        let fm = FileManager.default
        let url = Self.folder.appendingPathComponent(Self.recordName(date: Date(), flow: flow.id))
        do {
            try fm.createDirectory(at: Self.folder, withIntermediateDirectories: true)
            fm.createFile(atPath: url.path, contents: Data("t_ms\tcontroller\tside\tstep\tax\tay\taz\tgx\tgy\tgz\n".utf8))
            recording = try FileHandle(forWritingTo: url)
            recording?.seekToEndOfFile()
            recordURL = url
            recordStart = ProcessInfo.processInfo.systemUptime; recordCount = 0; recordError = nil
        } catch { recordError = error.localizedDescription }
    }
    private func stopRecording() {
        guard let handle = recording else { return }
        flushRecording()
        try? handle.close()
        recording = nil
    }
    private func record(_ s: IMUSample, side: String) {
        guard recording != nil else { return }
        let ms = Int((ProcessInfo.processInfo.systemUptime - recordStart) * 1000)
        let name = (selected?.title ?? "").replacingOccurrences(of: "\t", with: " ")
        recordBuffer += String(format: "%d\t%@\t%@\t%@\t%.4f\t%.4f\t%.4f\t%.2f\t%.2f\t%.2f\n", ms, name, side, step?.id ?? "-",
                               s.accel.x, s.accel.y, s.accel.z, s.gyro.x, s.gyro.y, s.gyro.z)
        recordCount += 1
    }
    private func flushRecording() {
        guard let handle = recording, !recordBuffer.isEmpty else { return }
        handle.write(Data(recordBuffer.utf8)); recordBuffer = ""
    }
}
