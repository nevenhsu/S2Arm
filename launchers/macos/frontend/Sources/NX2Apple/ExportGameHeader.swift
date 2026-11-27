// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI
import UniformTypeIdentifiers
import NXCore
import NXModel
import NXLauncher
import S2ArmAppleAdapter

/// The setup and session pages share one cover, heading and package summary.
struct ExportGameHeader: View {
    @ObservedObject var model: S2ArmLauncherModel
    @ObservedObject var covers: CoverCache
    @EnvironmentObject private var loc: Localizer
    let game: GameSummary
    var editable = true

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            HStack(alignment: .center, spacing: 24) {
                if editable {
                    Button { chooseCover(game) } label: { artwork.overlay(alignment: .bottomTrailing) { editBadge.padding(8) } }
                        .controllerButtonStyle(.plain).disabled(model.busy || model.customizing)
                        .help(loc.t("s2arm.custom.cover.help"))
                        .accessibilityLabel(loc.t("s2arm.custom.cover.help"))
                        .controllerID("export.cover")
                } else { artwork }
                VStack(alignment: .leading, spacing: 10) {
                    let version = String(format: loc.t("s2arm.export.version"), game.display.isEmpty ? (game.version("base") ?? "-") : game.display)
                    let subtitle = game.version("update") == nil && !game.id.isEmpty ? "\(version) · \(game.id)" : version
                    LauncherHeadingLine {
                        if editable {
                            ExportNameControl(name: game.name, custom: game.customName) { model.customize(game, name: $0) }
                                .id(game.id).disabled(model.busy || model.customizing)
                        } else {
                            Text(game.name).font(.system(size: 36, weight: .heavy)).tracking(-1)
                                .lineLimit(1).help(game.name)
                        }
                    } subtitle: {
                        Text(subtitle).font(.manrope(14)).foregroundStyle(.secondary).help(subtitle)
                    }
                    if editable, game.customCover {
                        Button(loc.t("s2arm.custom.cover.reset")) { model.customize(game, coverPath: "") }
                            .controllerButtonStyle(.plain).font(.manrope(12.5)).foregroundStyle(.secondary).underline()
                            .disabled(model.busy || model.customizing).accessibilityIdentifier("export.cover.reset")
                    }
                    if editable, model.customizing, let note = model.lockWaitNote {
                        Text(loc.backend(note)).font(.manrope(12.5)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                    if editable, let failed = model.customizeError {
                        Text(loc.backend(failed)).font(.manrope(12.5)).foregroundStyle(LauncherPalette.danger).fixedSize(horizontal: false, vertical: true)
                    }
                    HStack(spacing: 6) {
                        if let v = game.version("base") { chip(String(format: loc.t("s2arm.export.base"), v), "shippingbox") }
                        if let v = game.version("update") { chip(String(format: loc.t("s2arm.export.update"), v), "arrow.triangle.2.circlepath") }
                        chip(game.dlcCount > 0 ? String(format: loc.t("s2arm.export.dlc"), game.dlcCount) : loc.t("s2arm.export.noDlc"),
                             "puzzlepiece.extension", muted: game.dlcCount == 0)
                        if game.totalBytes > 0 {
                            chip(ByteCountFormatter.string(fromByteCount: game.totalBytes, countStyle: .file), "internaldrive", muted: true)
                        }
                    }
                    .padding(.top, 2)
                }
                Spacer(minLength: 0)
            }
        }
    }

    private var artwork: some View {
        GameArtwork(covers: covers, cover: game.cover)
            .frame(width: 236, height: 236 / 1.8).clipShape(RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color.white.opacity(0.08)))
            .shadow(color: .black.opacity(0.35), radius: 14, y: 6)
    }

    private func chip(_ text: String, _ symbol: String, muted: Bool = false) -> some View {
        Label(text, systemImage: symbol).font(.manrope(12, .semibold)).lineLimit(1)
            .padding(.horizontal, 10).padding(.vertical, 5)
            .foregroundStyle(muted ? Color.secondary : LauncherPalette.cream)
            .background((muted ? Color.white : LauncherPalette.cream).opacity(muted ? 0.06 : 0.10), in: Capsule())
    }

    private var editBadge: some View {
        Image(systemName: "photo").font(.system(size: 12, weight: .semibold)).foregroundStyle(.white)
            .padding(7).background(.black.opacity(0.55), in: Circle())
    }

    private func chooseCover(_ game: GameSummary) {
        let panel = NSOpenPanel()
        panel.title = loc.t("s2arm.custom.cover.help")
        panel.allowedContentTypes = [.jpeg, .png]
        panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        guard LauncherWindowPresenter.current.runPanel(panel) == .OK, let url = panel.url else { return }
        model.customize(game, coverPath: url.path)
    }

}
