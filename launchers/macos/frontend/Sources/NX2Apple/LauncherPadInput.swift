// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import GameController
import LauncherPadBridge
import NXLauncher

enum LauncherPadAction: String, Equatable {
    case left, right, up, down, confirm, submit, back, details, filter, previousGroup, nextGroup, pageUp, search, menu, scrollUp, scrollDown
}

struct LauncherPadInput: Equatable {
    // The standard Npad button order, also used by the Switch helper's relay.
    var buttons: UInt64 = 0
    var x: Float = 0
    var y: Float = 0
    var rightY: Float = 0
    var neutral: Bool { buttons == 0 && abs(x) < 0.55 && abs(y) < 0.55 && abs(rightY) < 0.55 }

    static func system(_ pad: GCExtendedGamepad, mapping: ControllerFaceMapping = .identity) -> Self {
        var input = Self(x: pad.leftThumbstick.xAxis.value, y: pad.leftThumbstick.yAxis.value, rightY: pad.rightThumbstick.yAxis.value)
        let buttons: [(GCControllerButtonInput?, Int)] = [(pad.buttonA, 0), (pad.buttonB, 1), (pad.buttonX, 2), (pad.buttonY, 3),
            (pad.leftShoulder, 6), (pad.rightShoulder, 7), (pad.leftTrigger, 8), (pad.rightTrigger, 9),
            (pad.buttonMenu, 10), (pad.buttonOptions, 11)]
        for (button, bit) in buttons where button?.isPressed == true { input.buttons |= 1 << bit }
        input.buttons = mapping.apply(input.buttons)
        if pad.dpad.xAxis.value < -0.5 { input.buttons |= 1 << 12 }
        if pad.dpad.xAxis.value > 0.5 { input.buttons |= 1 << 14 }
        if pad.dpad.yAxis.value > 0.5 { input.buttons |= 1 << 13 }
        if pad.dpad.yAxis.value < -0.5 { input.buttons |= 1 << 15 }
        return input
    }

    /// HID 兜底手柄（#1151，LauncherPadBridge/HIDPads.m）：按键已是 Npad 位（含十字键 12–15），摇杆 Y 向上为正
    static func hid(_ pad: s2a_launcher_hid_pad, mapping: ControllerFaceMapping) -> Self {
        Self(buttons: mapping.apply(UInt64(pad.buttons)), x: Float(pad.lx) / 32767, y: Float(pad.ly) / 32767, rightY: Float(pad.ry) / 32767)
    }
    /// 读哪些兜底手柄（纯函数，单测）：GameController 已列出同名的那只让位（同 HIDFallbackPads.listed、宿主 s2a_hidg_route），不读两遍
    static func fallback(_ hid: [s2a_launcher_hid_pad], system: [String]) -> [s2a_launcher_hid_pad] {
        let known = Set(system.map { $0.lowercased() })
        return hid.filter { !known.contains(hidName($0).lowercased()) }
    }
    static func hidName(_ pad: s2a_launcher_hid_pad) -> String {
        withUnsafeBytes(of: pad.name) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
}

/// Buttons are edge triggered; only directions/scroll repeat. A held button must
/// be released after activation/reconnection, so returning from a game cannot launch it again.
struct LauncherPadRepeater {
    private var previous: UInt64 = 0
    private var direction: LauncherPadAction?
    private var scroll: LauncherPadAction?
    private var repeatAt: TimeInterval = 0
    private var scrollAt: TimeInterval = 0
    private var waitingForNeutral = true

    mutating func reset() { self = Self() }
    mutating func actions(_ input: LauncherPadInput, time: TimeInterval) -> [LauncherPadAction] {
        if waitingForNeutral {
            if input.neutral { waitingForNeutral = false }
            return []
        }
        let pressed = input.buttons & ~previous
        previous = input.buttons
        if pressed & (1 << 1) != 0 { return [.back] }
        let mappings: [(Int, LauncherPadAction)] = [(0, .confirm), (1, .back), (2, .filter), (3, .details), (6, .previousGroup), (7, .nextGroup),
            (8, .pageUp), (9, .submit), (10, .menu), (11, .search)]
        var result = mappings.compactMap { pressed & (1 << $0.0) != 0 ? $0.1 : nil }
        let b = input.buttons
        let threshold: Float = direction == nil ? 0.55 : 0.30
        var next: LauncherPadAction?
        if b & (1 << 12) != 0 { next = .left }
        else if b & (1 << 14) != 0 { next = .right }
        else if b & (1 << 13) != 0 { next = .up }
        else if b & (1 << 15) != 0 { next = .down }
        else if max(abs(input.x), abs(input.y)) > threshold {
            next = abs(input.x) > abs(input.y) ? (input.x > 0 ? .right : .left) : (input.y > 0 ? .up : .down)
        }
        if next != direction {
            direction = next; repeatAt = time + 0.36
            if let next { result.append(next) }
        } else if let next, time >= repeatAt {
            result.append(next); repeatAt = time + 0.095
        }
        let nextScroll: LauncherPadAction? = abs(input.rightY) > 0.55 ? (input.rightY > 0 ? .scrollUp : .scrollDown) : nil
        if let nextScroll, nextScroll != scroll || time >= scrollAt { result.append(nextScroll); scrollAt = time + 0.09 }
        scroll = nextScroll
        return result
    }
}

/// Reads system controller values without replacing anyone's input handlers.
/// The private Switch socket is held only while the launcher owns input.
@MainActor final class LauncherPadSource {
    var receive: (LauncherPadAction) -> Void = { _ in }
    var relayPath: () -> String? = { nil }
    var relayAllowed: () -> Bool = { true }
    var controllers: () -> [GCController] = { GCController.controllers() }
    var relayConnected: () -> Bool = { false }
    var assignment: () -> PlayerAssignment = { PlayerAssignment() }
    var faceMapping: (ControllerFaceFamily) -> ControllerFaceMapping = { _ in .identity }
    var isActive: () -> Bool = { NSApp.isActive }
    var updatePresentation: (Bool, [PadShortcut: String]) -> Void = { _, _ in }
    var refresh: () -> Void = {}
    /// HID 兜底手柄（#1151）：启动器在前台时开着读，失焦（游戏在前台）就关，免得和游戏里的宿主同读
    var hidOpen: () -> Void = { s2a_launcher_hid_open() }
    var hidClose: () -> Void = { s2a_launcher_hid_close() }
    var hidPads: () -> [s2a_launcher_hid_pad] = {
        var out = [s2a_launcher_hid_pad](repeating: s2a_launcher_hid_pad(), count: 8)
        let n = Int(s2a_launcher_hid_read(&out, Int32(out.count)))
        return Array(out.prefix(max(0, n)))
    }
    /// 手柄测试面板（开发工具，ControllerTestPanel）：开着时同一条连接开体感，每拍把排着的六轴交给它（连接程序一次只服务一个客户端，面板不另开连接）
    var motionWanted: () -> Bool = { false }
    var receiveMotion: ([s2a_launcher_pad_motion]) -> Void = { _ in }
    private var motionSent = false
    /// 现在连着连接程序（面板显示读数来源状态用）
    var relayOpen: Bool { client != nil }
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []
    private var client: OpaquePointer?
    private var relay = s2a_launcher_pad_sample()
    private var retryAt: TimeInterval = 0
    private var lastPacketAt: TimeInterval = 0
    private var token = ""
    private var repeater = LauncherPadRepeater()
    private let notifications: NotificationCenter
    private var announced: [ObjectIdentifier: GCController] = [:]
    private var disconnected = Set<ObjectIdentifier>()

    init(notifications: NotificationCenter = .default) { self.notifications = notifications }

    func start() {
        guard observers.isEmpty else { return }
        observers.append(notifications.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.resume() }
        })
        observers.append(notifications.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.pause() }
        })
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(notifications.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                MainActor.assumeIsolated {
                    guard let self, let controller = note.object as? GCController else { return }
                    let id = ObjectIdentifier(controller)
                    if note.name == .GCControllerDidConnect {
                        self.announced[id] = controller; self.disconnected.remove(id)
                    } else {
                        self.announced[id] = nil; self.disconnected.insert(id)
                    }
                    self.refreshPresentation()
                }
            })
        }
        refreshPresentation()
        if NSApp.isActive { resume() }
    }
    func stop() {
        pause()
        for observer in observers { notifications.removeObserver(observer) }
        observers.removeAll()
        announced.removeAll(); disconnected.removeAll()
    }
    func releaseRelay() {
        if let client { s2a_launcher_pad_close(client) }
        client = nil; relay = .init(); repeater.reset(); token = ""; motionSent = false
        refreshPresentation()
    }
    /// Connection notifications can precede the system enumeration (or remove
    /// a device before enumeration catches up). Preserve that authoritative edge.
    private func currentControllers() -> [GCController] {
        let listed = controllers()
        let ids = Set(listed.map(ObjectIdentifier.init))
        for id in ids { announced[id] = nil }
        let current = listed.filter { !disconnected.contains(ObjectIdentifier($0)) } + Array(announced.values)
        let idsToRead = Set(SystemControllers.snapshot(current).map(\.id))
        return current.filter { idsToRead.contains(ObjectIdentifier($0)) }
    }
    func refreshPresentation() {
        publishPresentation(currentControllers())
        refresh()
    }
    private func publishPresentation(_ pads: [GCController]) {
        let primary = assignment().primarySystem(in: pads)
        var symbols: [PadShortcut: String] = [:]
        if let primary {
            let family = ControllerFaceFamily.of(primary)
            symbols = faceMapping(family).symbols(for: family)
        } else if assignment().primaryIsNintendo || relayConnected() || relay.ready != 0 {
            symbols = faceMapping(.nintendo).symbols(for: .nintendo)
        }
        updatePresentation(!pads.isEmpty || relayConnected() || relay.ready != 0, symbols)
    }
    private func pause() { timer?.invalidate(); timer = nil; releaseRelay(); hidClose() }
    private func resume() {
        guard timer == nil else { return }
        repeater.reset(); hidOpen()
        let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        RunLoop.main.add(t, forMode: .common)
        RunLoop.main.add(t, forMode: .modalPanel)
        timer = t
    }
    func tick(now: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        guard isActive() else { pause(); return }
        if !relayAllowed() { if client != nil { releaseRelay() } }
        else {
            if client == nil, now >= retryAt, let path = relayPath() {
                client = path.withCString { s2a_launcher_pad_open($0) }; retryAt = now + 1
            }
            if let client {
                let want = motionWanted()
                if want != motionSent, s2a_launcher_pad_set_motion(client, want ? 1 : 0) == 0 { motionSent = want }
                let result = s2a_launcher_pad_poll(client, &relay)
                if motionSent {
                    var batch = [s2a_launcher_pad_motion](repeating: .init(), count: 128)
                    let n = Int(s2a_launcher_pad_motion_drain(client, &batch, Int32(batch.count)))
                    if n > 0 { receiveMotion(Array(batch.prefix(n))) }
                }
                if result < 0 { releaseRelay(); retryAt = now + 1 }
                else if result > 0 { lastPacketAt = now }
                else if relay.ready != 0, now - lastPacketAt > 0.25 { relay = .init() }
            }
        }
        let pads = currentControllers()
        let players = assignment()
        let primary = players.primarySystem(in: pads)
        publishPresentation(pads)
        refresh()
        var input = LauncherPadInput()
        var controllerTokens: [String] = []
        for controller in pads {
            guard let pad = controller.extendedGamepad else { continue }
            let family = ControllerFaceFamily.of(controller)
            let mapping = faceMapping(family)
            controllerTokens.append("\(ObjectIdentifier(controller)):\(mapping.code)")
            let state = LauncherPadInput.system(pad, mapping: mapping)
            input.buttons |= state.buttons
            if abs(state.x) > abs(input.x) { input.x = state.x }
            if abs(state.y) > abs(input.y) { input.y = state.y }
            if abs(state.rightY) > abs(input.rightY) { input.rightY = state.rightY }
        }
        let nintendoMapping = faceMapping(.nintendo)
        // HID 兜底手柄：Switch 样式（Pro 协议）与按位置映射的通用手柄，按键都已是任天堂语义的 Npad 位，同 NS2 中继用任天堂映射
        for pad in LauncherPadInput.fallback(hidPads(), system: SystemControllers.snapshot(pads).map(\.name)) {
            controllerTokens.append("hid:\(LauncherPadInput.hidName(pad))")
            let state = LauncherPadInput.hid(pad, mapping: nintendoMapping)
            input.buttons |= state.buttons
            if abs(state.x) > abs(input.x) { input.x = state.x }
            if abs(state.y) > abs(input.y) { input.y = state.y }
            if abs(state.rightY) > abs(input.rightY) { input.rightY = state.rightY }
        }
        if relay.ready != 0 {
            input.buttons |= nintendoMapping.apply(relay.buttons)
            if abs(relay.x) > abs(input.x) { input.x = relay.x }
            if abs(relay.y) > abs(input.y) { input.y = relay.y }
            if abs(relay.right_y) > abs(input.rightY) { input.rightY = relay.right_y }
        }
        let nextToken = controllerTokens.joined(separator: ",")
            + ":\(relay.ready):\(relay.epoch):\(nintendoMapping.code):\(primary.map { String(describing: ObjectIdentifier($0)) } ?? ""):\(players.generation)"
        if token != nextToken { token = nextToken; repeater.reset() }
        for action in repeater.actions(input, time: now) { receive(action) }
    }
    deinit {
        timer?.invalidate()
        for observer in observers { notifications.removeObserver(observer) }
        if let client { s2a_launcher_pad_close(client) }
    }
}
