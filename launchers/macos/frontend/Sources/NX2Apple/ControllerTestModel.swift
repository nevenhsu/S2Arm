// S2Arm（不是上游文件）：手柄测试面板的数据（开发工具，本地 / 自测构建才有，开关同「模擬手把」）。
//   面板（ControllerTestPanel）看连着的一只手柄：原始六轴、姿态、步骤、录制，用来对照体感转换（服务层 NpadImu.Push）对不对。
//   读数来源：
//   · Joy-Con 2 / Switch 2 Pro：启动器手柄导航那一条连接程序连接（LauncherPadSource）顺带开体感（连接程序一次只服务一个客户端，
//     另开一条会把导航和游戏宿主踢掉）。游戏进行中 / 分配玩家中这条连接本来就放开，读数跟着停
//   · 系统蓝牙手柄（PS5 / PS4 / Xbox / Switch Pro 等）：GameController 的 motion（选中时才开感测器）
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
    static var readingsURL: URL { folder.appendingPathComponent("readings.tsv") }

    // —— 接进来的（LauncherView 接上）——
    weak var controller: NS2LauncherController?
    weak var systems: SystemControllers?
    weak var source: LauncherPadSource?
    var gameRunning: () -> Bool = { false }

    // —— 要看的手柄 ——
    struct Target: Identifiable, Equatable {
        enum Kind: Equatable {
            case relay(player: Int, shape: ControllerTestIMU.Shape)   // Joy-Con 2 / Switch 2 Pro（连接程序）
            case system(ObjectIdentifier)                             // 系统蓝牙手柄
            case simulated                                            // 「模擬手把」加的，没有读数
        }
        let id: String
        let title: String
        let player: String   // P1… / —（没有玩家位）
        let meta: String
        let kind: Kind
        let dots: [ControllerTint?]   // 颜色点：一对两颗
        var joyConSide: String? = nil   // 单只 Joy-Con 是哪一只（L / R）：模型画左只还是右只、录制的 side 栏
        var isPair: Bool { if case .relay(_, .pair) = kind { return true }; return false }
        var wholePad: Bool { if case .relay(_, .other) = kind { return true }; if case .system = kind { return true }; return false }
    }
    @Published var selectedID: String? { didSet { if selectedID != oldValue { selectionChanged() } } }
    @Published var poseFromGame = false
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
        func tint(_ id: String, left: Bool) -> ControllerTint? { sim.ns2Tint(id) ?? ControllerTint(body: left ? 0x7ad3f0 : 0xf2a7c3) }
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
                                  joyConSide: joyCon ? (d.kind == 2 ? "L" : "R") : nil))
            }
        }
        let pads = systems?.pads ?? []
        let simPads = Set(ControllerColorSimulator.allowed ? sim.systemPads().map(\.id) : [])
        for (i, pad) in pads.enumerated() {
            let index = pads[..<i].filter { $0.name == pad.name }.count
            let simulated = simPads.contains(pad.id)
            out.append(Target(id: "sys:\(pad.name)#\(index)", title: pad.name,
                              player: player { if case let .system(n, x) = $0 { return n == pad.name && x == index }; return false },
                              meta: simulated ? simMeta : SimText.pick("系統手把", "系统手柄", "System controller"),
                              kind: simulated ? .simulated : .system(pad.id), dots: [nil]))
        }
        return out
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

    // —— 系统手柄的 motion ——
    private weak var motionPad: GCController?
    private var motionActivated = false

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
        stopRecording()
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
        case .system:
            if motionPad?.motion == nil { return .noMotion }
        }
        return channels.contains { $0.last != nil } ? .ok : .waiting
    }
    var sourceName: String {
        switch selected?.kind {
        case .relay: return SimText.pick("Joy-Con 2 連接程式", "Joy-Con 2 连接程序", "Joy-Con 2 helper")
        case .system: return "GameController"
        default: return "—"
        }
    }

    private func selectionChanged() {
        releaseMotionPad()
        shownTargetID = selected?.id
        guard let target = selected else { channels = []; return }
        switch target.kind {
        case let .relay(_, shape):
            channels = shape == .pair ? [Channel(side: "L", shape: .pair), Channel(side: "R", shape: .pair)] : [Channel(side: target.joyConSide ?? "-", shape: shape)]
        case let .system(id):
            channels = [Channel(side: "-", shape: .other)]
            if open { attachMotion(id) }
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
        if case let .system(id) = selected?.kind, motionPad == nil { attachMotion(id) }
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
    func recenter() { channels.forEach { $0.recenter() } }

    private func attachMotion(_ id: ObjectIdentifier) {
        guard let pad = GCController.controllers().first(where: { ObjectIdentifier($0) == id }) else { return }
        motionPad = pad
        guard let motion = pad.motion else { return }
        if motion.sensorsRequireManualActivation, !motion.sensorsActive { motion.sensorsActive = true; motionActivated = true }
        // 启动器别处不接 motion 的回调；面板关了 / 换手柄就还原
        motion.valueChangedHandler = { [weak self] m in
            MainActor.assumeIsolated { self?.receiveSystem(m) }
        }
    }
    private func receiveSystem(_ m: GCMotion) {
        guard open, let channel = channels.first else { return }
        let a = m.hasGravityAndUserAcceleration ? SIMD3(m.gravity.x + m.userAcceleration.x, m.gravity.y + m.userAcceleration.y, m.gravity.z + m.userAcceleration.z)
            : SIMD3(m.acceleration.x, m.acceleration.y, m.acceleration.z)
        let r = m.rotationRate
        let s = IMUSample(accel: a, gyro: SIMD3(r.x, r.y, r.z) * (180 / .pi))
        if channel.last == s { return }
        add(s, to: channel, at: ProcessInfo.processInfo.systemUptime)
    }
    private func releaseMotionPad() {
        if let motion = motionPad?.motion {
            motion.valueChangedHandler = nil
            if motionActivated { motion.sensorsActive = false }
        }
        motionPad = nil; motionActivated = false
    }

    // —— 步骤 ——
    @Published private(set) var steps: [ControllerTestStep] = []
    @Published private(set) var stepIndex = 0
    private(set) var stepStartedAt: Double = 0
    private var stepsStamp: Date?
    func loadSteps(force: Bool = false) {
        let url = Self.stepsURL
        let stamp = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
        guard force || stamp != stepsStamp else { return }
        stepsStamp = stamp
        let next = stamp == nil ? [] : (try? Data(contentsOf: url)).flatMap(ControllerTestStep.parse) ?? []
        if next != steps { steps = next; stepIndex = min(stepIndex, max(0, next.count - 1)); stepStartedAt = ProcessInfo.processInfo.systemUptime }
    }
    func step(_ delta: Int) {
        guard !steps.isEmpty else { return }
        stepIndex = max(0, min(steps.count - 1, stepIndex + delta))
        stepStartedAt = ProcessInfo.processInfo.systemUptime
    }
    var countdown: Int? {
        guard steps.indices.contains(stepIndex), let seconds = steps[stepIndex].seconds else { return nil }
        return max(0, Int((seconds - (ProcessInfo.processInfo.systemUptime - stepStartedAt)).rounded(.up)))
    }

    // —— 录制 ——
    private var recording: FileHandle?
    private var recordBuffer = ""
    private(set) var recordStart: Double = 0
    private(set) var recordCount = 0
    private var lastFlush: Double = 0
    var isRecording: Bool { recording != nil }
    @Published private(set) var recordError: String?
    func toggleRecording() { if recording == nil { startRecording() } else { stopRecording() } }
    private func startRecording() {
        let fm = FileManager.default, url = Self.readingsURL
        do {
            try fm.createDirectory(at: Self.folder, withIntermediateDirectories: true)
            // 上一份留成 readings.prev.tsv，不直接盖掉
            let prev = Self.folder.appendingPathComponent("readings.prev.tsv")
            if fm.fileExists(atPath: url.path) { try? fm.removeItem(at: prev); try? fm.moveItem(at: url, to: prev) }
            fm.createFile(atPath: url.path, contents: Data("t_ms\tcontroller\tside\tstep\tax\tay\taz\tgx\tgy\tgz\n".utf8))
            recording = try FileHandle(forWritingTo: url)
            recording?.seekToEndOfFile()
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
        let step = steps.isEmpty ? 0 : stepIndex + 1
        recordBuffer += String(format: "%d\t%@\t%@\t%d\t%.4f\t%.4f\t%.4f\t%.2f\t%.2f\t%.2f\n", ms, name, side, step,
                               s.accel.x, s.accel.y, s.accel.z, s.gyro.x, s.gyro.y, s.gyro.z)
        recordCount += 1
    }
    private func flushRecording() {
        guard let handle = recording, !recordBuffer.isEmpty else { return }
        handle.write(Data(recordBuffer.utf8)); recordBuffer = ""
    }
}
