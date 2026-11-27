// S2Arm（不是上游文件）：出包页「签名方式」一栏 + 导入开发者证书弹窗（v0.2.4，作业层 pack/jobs/devcert.py）。
//   一栏：Apple ID 签名 / 我的开发者证书 二选一；选了证书，下面列团队、到期、包名、设备列表核对，问题黄字（不挡）、根本签不了红字。
//   弹窗：拖进或选 .p12 与 .mobileprovision，输密码，点导入；失败原因留在弹窗里，成了自动关。
//   选证书窗（#487）：一台设备第一次连上、几张证书里没给它选过时，问这台用哪张；也可当场导入一张新的给它。
//   描述文件缺权限（分发证书的 devcert_distribution 拒导入；只缺内核两项的 devcert_entitlements 照常导入）：逐条列缺项，同一页直接给
//   「复制给证书发放人的话」（作业层 requestText）和「改用 Apple ID 登录」（切到 Apple ID 签名并打开登录面板），不让玩家自己去找。
import AppKit
import SwiftUI
import UniformTypeIdentifiers
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

private func sx(_ loc: Localizer, _ id: String) -> String { BackendText.launcherText("export.sign." + id, language: loc.lang.sharedCode) }

struct DevCertRow: View {
    @ObservedObject var model: S2ArmLauncherModel
    @EnvironmentObject var loc: Localizer
    /// 标题与「不用证书」那一项的叫法：环境检查「装到 iPhone 的方式」里叫「签名身份」「Apple ID 签名」（0.6.0 起 Apple ID 在 S2Arm 里登录，不经 Xcode）
    var title: String? = nil
    var appleIdTitle: String? = nil
    var embedded = false
    /// 「改用 Apple ID 登录」打开的登录面板挂在哪一层（这一栏在设置里）
    var loginHost = "settings"
    @State private var confirmDelete = false
    @State private var addMode = false
    /// 几张证书时「删除证书」只删这台在用的那张
    private var many: Bool { (model.devCert?.certs.count ?? 0) > 1 }
    /// 导入弹窗里点了「改用 Apple ID 登录」：等弹窗收起再开登录面板（同一层同一时刻只能弹一个）
    @State private var loginAfterSheet = false

    private var on: Bool { model.devCert?.active == true }
    /// 选了「我自己用 AltStore / SideStore 签」（自签走查结构 6）
    private var external: Bool { model.installMode.signChoice == "external" }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                if !embedded { LauncherRowIcon("signature") }
                Text(title ?? sx(loc, "title")).font(LauncherTheme.TypeStyle.detail).foregroundStyle(.secondary)
                Spacer(minLength: 12)
                LauncherDropdown(options: [("xcode", appleIdTitle ?? sx(loc, "appleId")), ("certificate", sx(loc, "devcert")), ("external", sx(loc, "external"))],
                                 selection: external ? "external" : on ? "certificate" : "xcode", label: title ?? sx(loc, "title")) {
                    model.setSignChoice($0)
                }
                .launcherRowControls().controllerID("export.sign.mode")
            }
            if on && !external, let cert = model.devCert?.cert { details(cert) }
            if external { Text(sx(loc, "externalNote")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
            Text(sx(loc, "switchNote")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(embedded ? 0 : LauncherTheme.Metrics.cardInset)
        .background {
            if !embedded {
                RoundedRectangle(cornerRadius: LauncherTheme.Metrics.cardRadius).fill(LauncherTheme.Colors.card)
                    .overlay(RoundedRectangle(cornerRadius: LauncherTheme.Metrics.cardRadius).strokeBorder(LauncherTheme.Colors.border))
            }
        }
        .onAppear { model.refreshDevCert(); model.refreshInstallMode(silent: true) }
        .onChange(of: model.selectedDevice?["udid"] as? String) { _ in model.refreshDevCert() }
        .sheet(isPresented: $model.showDevCertImport, onDismiss: {
            addMode = false
            if loginAfterSheet { loginAfterSheet = false; model.switchToAppleIdLogin(host: loginHost) }
        }) {
            DevCertSheet(model: model, add: addMode, onAppleLogin: { loginAfterSheet = true }).environmentObject(loc)
        }
        .alert(sx(loc, many ? "deleteOneConfirm" : "deleteConfirm"), isPresented: $confirmDelete) {
            Button(sx(loc, "delete"), role: .destructive) { model.deleteDevCert(entry: many ? model.devCert?.cert?.entry : nil) }
            Button(sx(loc, "sheet.cancel"), role: .cancel) {}
        }
    }

    private func details(_ c: DevCertInfo) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            line(sx(loc, "team"), c.teamName.isEmpty ? c.teamId : "\(c.teamName)（\(c.teamId)）")
            line(sx(loc, "certExpires"), c.certExpires)
            line(sx(loc, "profileExpires"), c.profileExpires)
            line(sx(loc, "bundle"), c.bundleId)
            line(sx(loc, "devices"), devices(c))
            ForEach(c.blocking, id: \.self) { t in
                Label(loc.backend(t), systemImage: "xmark.octagon").font(LauncherTheme.TypeStyle.caption).foregroundStyle(LauncherPalette.danger).fixedSize(horizontal: false, vertical: true)
            }
            ForEach(c.warnings, id: \.self) { t in
                Label(loc.backend(t), systemImage: "exclamationmark.triangle").font(LauncherTheme.TypeStyle.caption).foregroundStyle(.yellow).fixedSize(horizontal: false, vertical: true)
            }
            if !c.requestText.isEmpty || c.distribution {
                DevCertFixActions(request: c.requestText, idPrefix: "export.sign") { model.switchToAppleIdLogin(host: loginHost) }
            }
            HStack(spacing: 10) {
                if (model.devCert?.certs.count ?? 0) > 1 || model.selectedDevice?["udid"] as? String != nil {
                    Button(sx(loc, "change")) { model.openDevCertChoice() }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                        .accessibilityIdentifier("export.sign.change")
                        .disabled(model.selectedDevice?["udid"] as? String == nil)
                }
                Button(sx(loc, "add")) { addMode = true; model.showDevCertImport = true }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                    .accessibilityIdentifier("export.sign.add")
                // 这台还没选证书（几张里挑不出）：上面显示的是缺省那张，不在这里换 / 删它，免得动到别的设备在用的
                if model.devCert?.needsChoice != true {
                    Button(sx(loc, "replace")) { addMode = false; model.showDevCertImport = true }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                        .accessibilityIdentifier("export.sign.replace")
                    Button(sx(loc, "delete")) { confirmDelete = true }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                        .accessibilityIdentifier("export.sign.delete")
                }
            }
            .padding(.top, 4)
        }
        .padding(.leading, 28)
        .accessibilityIdentifier("export.sign.details")
    }

    private func devices(_ c: DevCertInfo) -> String {
        if c.allDevices { return sx(loc, "devices.all") }
        let key = c.deviceListed == nil ? "devices.unknown" : c.deviceListed == true ? "devices.listed" : "devices.missing"
        return sx(loc, key).replacingOccurrences(of: "{0}", with: String(c.deviceCount))
    }

    private func line(_ k: String, _ v: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(k).font(LauncherTheme.TypeStyle.detail).foregroundStyle(.secondary).frame(width: 96, alignment: .leading)
            Text(v.isEmpty ? "-" : v).font(.manrope(12.5, .semibold)).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
        }
    }
}

struct DevCertSheet: View {
    @ObservedObject var model: S2ArmLauncherModel
    /// true = 加一张（不顶掉在用的）；device 给了 = 导入后给这台选它（选证书窗里导入）
    var add = false
    var device: String? = nil
    @EnvironmentObject var loc: Localizer
    /// 失败是分发证书描述文件时「改用 Apple ID 登录」：告诉外层收起后开登录面板
    var onAppleLogin: () -> Void = {}
    @Environment(\.dismiss) private var dismiss
    @State private var p12 = ""
    @State private var profile = ""
    @State private var password = ""
    @State private var working = false
    @State private var failure: String?
    @State private var failureCode: String?
    @State private var failureRequest = ""
    @State private var adopted: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            LauncherSheetHeading(sx(loc, "sheet.title"))
            pick(sx(loc, "sheet.p12"), path: $p12, types: ["p12", "pfx"], id: "p12")
            pick(sx(loc, "sheet.profile"), path: $profile, types: ["mobileprovision"], id: "profile")
            Text(sx(loc, "sheet.profileNote")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)   // #2015：只选 .p12 也能导入
            VStack(alignment: .leading, spacing: 6) {
                Text(sx(loc, "sheet.password")).font(LauncherTheme.TypeStyle.section)
                SecureField("", text: $password).textFieldStyle(LauncherTextFieldStyle())
                    .accessibilityLabel(sx(loc, "sheet.password")).accessibilityIdentifier("export.sign.sheet.password")
                Text(sx(loc, "sheet.passwordNote")).font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if let adopted {   // 只选 .p12、交给 Apple ID 签名成了：留在弹窗里说清（不切签名方式）
                Text(loc.backend(adopted)).font(LauncherTheme.TypeStyle.detail).fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("export.sign.sheet.adopted")
            }
            if let failure {
                Text(loc.backend(failure)).font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.danger).fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("export.sign.sheet.failure")
                if failureCode == "devcert_distribution" || !failureRequest.isEmpty {
                    DevCertFixActions(request: failureRequest, idPrefix: "export.sign.sheet") { password = ""; onAppleLogin(); dismiss() }
                }
            }
            Spacer(minLength: 0)
            HStack {
                if working {
                    ProgressView().controlSize(.small)
                    Text(sx(loc, "sheet.working")).font(LauncherTheme.TypeStyle.detail).foregroundStyle(.secondary)
                }
                Spacer()
                Button(sx(loc, "sheet.cancel")) { dismiss() }.controllerButtonStyle(LauncherSecondaryButtonStyle()).keyboardShortcut(.cancelAction).controllerShortcut(.back)
                Button(sx(loc, "sheet.import")) { start() }
                    .controllerButtonStyle(LauncherPrimaryButtonStyle()).keyboardShortcut(.defaultAction).controllerShortcut(.rightTrigger)
                    .disabled(working || p12.isEmpty || adopted != nil)
                    .accessibilityIdentifier("export.sign.sheet.import")
            }
        }
        .padding(LauncherTheme.Space.page)
        .launcherSheet(.form, height: 520)
    }

    private func start() {
        working = true; failure = nil; failureCode = nil; failureRequest = ""; adopted = nil
        Task {
            let why = await model.importDevCert(p12: p12, profile: profile, password: password, add: add, device: device)
            working = false
            if let why, why.adopted { adopted = why.text; password = "" }
            else if let why { failure = why.text; failureCode = why.code; failureRequest = why.request } else { password = ""; dismiss() }
        }
    }

    private func pick(_ title: String, path: Binding<String>, types: [String], id: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(LauncherTheme.TypeStyle.section)
            HStack(spacing: 10) {
                Text(path.wrappedValue.isEmpty ? sx(loc, "sheet.drop") : (path.wrappedValue as NSString).abbreviatingWithTildeInPath)
                    .font(LauncherTheme.TypeStyle.detail).foregroundStyle(path.wrappedValue.isEmpty ? .secondary : .primary)
                    .lineLimit(1).truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button(sx(loc, "sheet.choose")) {
                    let panel = NSOpenPanel()
                    panel.allowedContentTypes = types.compactMap { UTType(filenameExtension: $0) }
                    panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
                    if LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url { path.wrappedValue = url.path }
                }
                .controllerButtonStyle(LauncherSecondaryButtonStyle())
                .accessibilityIdentifier("export.sign.sheet.choose." + id)
            }
            .padding(.horizontal, 12).padding(.vertical, 10)
            .launcherPanel()
            .onDrop(of: [.fileURL], isTargeted: nil) { providers in
                guard let item = providers.first else { return false }
                _ = item.loadObject(ofClass: URL.self) { url, _ in
                    guard let url, types.contains(url.pathExtension.lowercased()) else { return }
                    DispatchQueue.main.async { path.wrappedValue = url.path }
                }
                return true
            }
        }
    }
}


/// 「这台用哪张证书」（#487）：每张一行（团队 · 描述文件名 · 到期），描述文件管这台的打勾、不管的标警告；这台现装的 App 是哪张签的就预选哪张。
///   选的那张包名和这台现装的不同 → 写明会另装一个 S2Arm；选了不管这台的 → 再确认一次（装不上）。也可当场导入一张新的给这台。
struct DevCertChooseSheet: View {
    @ObservedObject var model: S2ArmLauncherModel
    let choice: DevCertChoice
    @EnvironmentObject var loc: Localizer
    @State private var picked: String?
    @State private var working = false
    @State private var failure: String?
    @State private var confirmUnlisted = false
    @State private var importing = false

    private var certs: [DevCertEntry] { choice.status.certs }
    private var current: DevCertEntry? { certs.first { $0.entry == picked } }
    private var installedBundle: String? { certs.first { $0.installedHere }?.bundleId }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            LauncherSheetHeading(sx(loc, "choose.title").replacingOccurrences(of: "{0}", with: choice.name.isEmpty ? "iPhone / iPad" : choice.name))
            ScrollView {
                VStack(spacing: 8) { ForEach(certs) { row($0) } }
            }
            .frame(maxHeight: 280)
            if let c = current, let installed = installedBundle, c.bundleId != installed {
                Label(sx(loc, "choose.newApp"), systemImage: "exclamationmark.triangle").font(LauncherTheme.TypeStyle.caption).foregroundStyle(.yellow)
                    .fixedSize(horizontal: false, vertical: true).accessibilityIdentifier("export.sign.choose.newApp")
            }
            if let failure {
                Text(loc.backend(failure)).font(LauncherTheme.TypeStyle.detail).foregroundStyle(LauncherPalette.danger).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            HStack {
                Button(sx(loc, "add")) { importing = true }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                    .accessibilityIdentifier("export.sign.choose.import")
                Spacer()
                Button(sx(loc, "choose.later")) { model.devCertChoice = nil }.controllerButtonStyle(LauncherSecondaryButtonStyle())
                    .keyboardShortcut(.cancelAction).controllerShortcut(.back)
                Button(sx(loc, "choose.use")) { if current?.deviceListed == false { confirmUnlisted = true } else { use() } }
                    .controllerButtonStyle(LauncherPrimaryButtonStyle()).keyboardShortcut(.defaultAction).controllerShortcut(.rightTrigger)
                    .disabled(working || current == nil)
                    .accessibilityIdentifier("export.sign.choose.use")
            }
        }
        .padding(LauncherTheme.Space.page)
        .launcherSheet(.form, height: 460)
        .onAppear { picked = picked ?? choice.status.chosen ?? (certs.first { $0.installedHere } ?? certs.first { $0.deviceListed == true } ?? certs.first)?.entry }
        .onChange(of: model.connectedDevices.contains { $0["udid"] as? String == choice.udid }) { here in if !here { model.devCertChoice = nil } }   // 这台拔走了：不再问
        .alert(sx(loc, "choose.notListed"), isPresented: $confirmUnlisted) {
            Button(sx(loc, "choose.anyway"), role: .destructive) { use() }
            Button(sx(loc, "sheet.cancel"), role: .cancel) {}
        }
        .sheet(isPresented: $importing) { DevCertSheet(model: model, add: true, device: choice.udid).environmentObject(loc) }
    }

    private func use() {
        guard let entry = picked else { return }
        working = true; failure = nil
        Task {
            failure = await model.chooseDevCert(entry, for: choice.udid)
            working = false
        }
    }

    private func row(_ c: DevCertEntry) -> some View {
        let on = c.entry == picked
        return Button { picked = c.entry } label: {
            HStack(spacing: 12) {
                Image(systemName: on ? "largecircle.fill.circle" : "circle").foregroundStyle(on ? Color.accentColor : .secondary)
                VStack(alignment: .leading, spacing: 3) {
                    Text([c.teamName.isEmpty ? c.teamId : c.teamName, c.profileName].filter { !$0.isEmpty }.joined(separator: " · "))
                        .font(.manrope(13, .semibold)).lineLimit(1)
                    Text([c.bundleId, c.profileExpires].filter { !$0.isEmpty }.joined(separator: " · "))
                        .font(LauncherTheme.TypeStyle.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 8)
                if c.installedHere {
                    Image(systemName: "iphone").foregroundStyle(.secondary).help(sx(loc, "choose.installed"))
                        .accessibilityLabel(sx(loc, "choose.installed"))
                }
                if c.allDevices || c.deviceListed == true {
                    Image(systemName: "checkmark.seal").foregroundStyle(.green)
                } else if c.deviceListed == false {
                    Image(systemName: "exclamationmark.triangle").foregroundStyle(.yellow).help(sx(loc, "choose.notListed"))
                        .accessibilityLabel(sx(loc, "choose.notListed"))
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
            .launcherPanel()
        }
        .controllerButtonStyle(.plain).controllerRingRadius(LauncherTheme.Metrics.cardRadius)
        .controllerID("export.sign.choose.row." + c.entry)
    }
}

/// 描述文件缺权限的两条出路（导入失败页、证书详情共用）：复制给证书发放人的话（按界面语言翻好再复制）/ 改用 Apple ID 登录
struct DevCertFixActions: View {
    let request: String
    let idPrefix: String
    let onAppleLogin: () -> Void
    @EnvironmentObject var loc: Localizer
    @State private var copied = false

    var body: some View {
        HStack(spacing: 10) {
            if !request.isEmpty {
                Button(sx(loc, copied ? "copied" : "copyRequest")) {
                    NSPasteboard.general.clearContents(); NSPasteboard.general.setString(loc.backend(request), forType: .string); copied = true
                }
                .controllerButtonStyle(LauncherSecondaryButtonStyle())
                .accessibilityIdentifier(idPrefix + ".copyRequest")
            }
            Button(sx(loc, "useAppleId")) { onAppleLogin() }
                .controllerButtonStyle(LauncherPrimaryButtonStyle())
                .accessibilityIdentifier(idPrefix + ".useAppleId")
        }
        .onChange(of: request) { _ in copied = false }
    }
}
