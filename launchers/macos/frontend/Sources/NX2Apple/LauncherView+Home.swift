// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import AppKit
import SwiftUI
import NXCore
import NXLauncher
import S2ArmAppleAdapter
import NXModel

extension LauncherView {
    var homeGames: [GameSummary] { home.games(model.games) }
    /// 选中首位「S2Arm」卡（iPhone 总控台，开关开着才有）时没有选中游戏
    var homeSelected: GameSummary? { consoleEnabled && model.consoleSelected ? nil : homeGames.first { $0.id == model.selectedID } ?? homeGames.first }

    func homeLibrary(size: CGSize) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            staleBanner
            homeControls(size: size).padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.top, size.height < 740 ? 12 : 22).padding(.bottom, 18)
            if homeGames.isEmpty {
                homeEmptyPreview
            } else {
                homeCoverStrip(size: size)
                Spacer(minLength: 12)
                if consoleEnabled && model.consoleSelected {
                    consolePanel(size: size).padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.bottom, size.height < 740 ? 24 : 40)
                } else if let game = homeSelected {
                    homePreview(game, size: size).padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.bottom, size.height < 740 ? 52 : 74)
                }
            }
            homeFooter
        }
        .onAppear { reconcileHomeSelection() }
        .onChange(of: home.group) { _ in reconcileHomeSelection() }
        .onChange(of: home.search) { _ in reconcileHomeSelection() }
        .onChange(of: homeGames.map(\.id)) { _ in reconcileHomeSelection() }
    }

    private var homeEmptyPreview: some View {
        VStack(spacing: 18) {
            Image(systemName: home.group.symbol)
                .font(.system(size: 38, weight: .light)).foregroundStyle(.secondary)
            Text(loc.t(home.search.isEmpty ? "home.empty." + home.group.rawValue : "home.noResults"))
                .font(.manrope(20, .semibold)).multilineTextAlignment(.center)
            Button { home.group = .all; home.search = "" } label: { Image(systemName: "square.grid.2x2") }
                .controllerButtonStyle(ConsoleIconButtonStyle(size: 48))
                .launcherTooltip(loc.t("home.showAll"), placement: .above).accessibilityLabel(loc.t("home.showAll")).controllerID("home.showAll")
        }.frame(maxWidth: .infinity, maxHeight: .infinity).padding(24)
    }

    /// 游玩时间：玩家在意的数，单独一块放大——数字大号加粗，单位与「哪天」小一号；规则同 iPhone 游戏库（GamePlaytime）
    private func playtimeBadge(_ playtime: GamePlaytime) -> some View {
        let p = playtime.parts(now: Date(), calendar: .current, locale: Locale(identifier: uiLanguage), text: txt)
        let template = txt(p.template)
        let split = template.components(separatedBy: "{0}")
        var line = AttributedString(split.first ?? "")
        line.font = Font.manrope(15, .medium); line.foregroundColor = .white.opacity(0.75)
        var number = AttributedString(p.number)
        number.font = Font.manrope(24, .bold); number.foregroundColor = LauncherPalette.cream
        line.append(number)
        var unit = AttributedString(split.dropFirst().joined(separator: "{0}"))
        unit.font = Font.manrope(15, .medium); unit.foregroundColor = .white.opacity(0.75)
        line.append(unit)
        return HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: "clock.fill").font(.system(size: 15, weight: .semibold)).foregroundStyle(LauncherPalette.cream)
                .alignmentGuide(.firstTextBaseline) { $0[.bottom] - 2 }
            Text(line).monospacedDigit().lineLimit(1)
            Text(p.when).font(.manrope(13, .medium)).foregroundStyle(.white.opacity(0.55)).lineLimit(1)
        }
        .padding(.horizontal, 16).padding(.vertical, 8)
        .background(Capsule().fill(.white.opacity(0.08)))
        .overlay(Capsule().strokeBorder(.white.opacity(0.14), lineWidth: 1))
        .fixedSize()
        .accessibilityElement(children: .combine).accessibilityIdentifier("hero.playtime")
    }

    private func homePreview(_ game: GameSummary, size: CGSize) -> some View {
        let compact = size.height < 740
        return VStack(alignment: .leading, spacing: compact ? 10 : 16) {
            Text(game.name).font(.manrope(compact ? 34 : min(48, size.width * 0.04), .bold)).tracking(-1)
                .lineLimit(1).minimumScaleFactor(0.62)
                .shadow(color: .black.opacity(0.3), radius: 14, y: 2)
            VStack(alignment: .leading, spacing: 8) {
                if !compact {
                    Text(game.id).font(.manrope(12, .medium)).monospaced().tracking(1.5).foregroundStyle(.white.opacity(0.65))
                        .shadow(color: .black.opacity(0.45), radius: 6, y: 1)
                }
                status(game).lineLimit(2)
                    .shadow(color: .black.opacity(0.45), radius: 6, y: 1)
            }
            if let playtime = game.playtime { playtimeBadge(playtime) }
            if game.playable, game.hostUpdate == "incompatible" {
                // 新版改了宿主读产物的约定、换不了宿主：旧版本照常能玩，重新出一次 Mac 版才能用上新版修复（host_update.py；主线游戏库同一句）
                HStack(spacing: 10) {
                    Text(txt("library.hostUpdate.incompatible")).font(.manrope(12.5)).foregroundStyle(.white.opacity(0.75)).fixedSize(horizontal: false, vertical: true)
                    Button(txt("library.hostUpdate.prepare")) { model.exportFor = game.id }
                        .controllerButtonStyle(.link).disabled(model.mainBusy).controllerID("hero.hostUpdate")
                }
            }
            if let error = model.actionError(for: game.id) {
                Text(loc.backend(error)).font(.manrope(12)).foregroundStyle(LauncherPalette.danger).lineLimit(2)
            }
            HStack(spacing: 14) {
                Button { homePrimary(game) } label: { Label(homePrimaryTitle(game), systemImage: homePrimarySymbol(game)) }
                    .controllerButtonStyle(ConsolePlayButtonStyle()).disabled(model.cloudSyncing || model.sgradeUpgrading(game.id))
                    .overlay(alignment: .bottom) { if model.sgradeUpgrading(game.id) { SGradeUpgradeBar(progress: model.sgradeProgress(game.id)).padding(.horizontal, 14).padding(.bottom, 5) } }
                    .keyboardShortcut(.return, modifiers: [])
                    .launcherTooltip(homePrimaryTitle(game), placement: .above).accessibilityLabel(homePrimaryTitle(game))
                    .controllerID(Self.homePrimaryID(game)).controllerShortcut(.rightTrigger)
                // 0.6.4 维护者：iPhone 这一侧只有「更新」「导出」两件事。更新只在电脑上做（没做过 iPhone 版不出这颗）；导出 = 导出确认页（签名 → 出法 → 设备多选）
                HeroUpdateButton(model: model, game: game)
                if Self.homePrimaryID(game) != "hero.exportSheet" {   // 主按钮已经是「导出…」/「续签」的款不重复出
                    Button { model.openExportSheet(game.id) } label: {
                        fitLabel(txt("exportsheet.open"), "square.and.arrow.up")
                    }
                    .controllerButtonStyle(ConsolePillButtonStyle()).controllerID("hero.exportSheet")   // 只开确认页：别的作业在跑也能点，开始后排队
                    .launcherTooltip(txt("exportsheet.open.help"), placement: .above).accessibilityLabel(txt("exportsheet.open"))
                }
                // 出包页（Mac / Apple TV / 安卓等其它平台）不再并排放在这里：游戏页只有「更新」「导出」两件事（维护者 11-21 定），
                //   出包从「…」菜单和右键菜单进；还没准备的款主按钮「准备游戏」照旧去出包页
                // 只留两个人人认得的圆钮（…、♡）；「查看出包结果」「启动设置」收进「…」菜单（11-16 维护者：三条横杠看不出是启动设置，一行三种形状不统一）
                Button { moreFor = game.id } label: { Image(systemName: "ellipsis") }
                    .controllerButtonStyle(ConsoleIconButtonStyle(size: 52)).controllerID("hero.more")
                    .launcherTooltip(loc.t("home.details"), placement: .above).accessibilityLabel(loc.t("home.details"))
                    .popover(isPresented: Binding(get: { moreFor == game.id }, set: { if !$0 { moreFor = nil } }), arrowEdge: .bottom) { moreMenu(game) }
                let favorited = home.favorites.contains(game.id)
                Button { home.toggleFavorite(game.id) } label: {
                    Image(systemName: favorited ? "heart.fill" : "heart")
                        .scaleEffect(favorited ? 1.04 : 1)
                }
                .controllerButtonStyle(ConsoleIconButtonStyle(size: 52, accent: favorited ? LauncherPalette.favorite : nil))
                .controllerID("hero.favorite")
                .animation(.spring(response: 0.26, dampingFraction: 0.72), value: favorited)
                .launcherTooltip(loc.t(favorited ? "home.unfavorite" : "home.favorite"), placement: .above)
                .accessibilityLabel(loc.t(favorited ? "home.unfavorite" : "home.favorite"))
                .accessibilityAddTraits(favorited ? .isSelected : [])
            }.padding(.top, compact ? 4 : 10)
            deviceSigningStatus(game)
            HeroUpdateLines(model: model, game: game)
        }
        .frame(maxWidth: min(920, size.width * 0.78), alignment: .leading)
        .accessibilityElement(children: .contain).accessibilityIdentifier("home.preview")
    }

    /// 游戏卡次按钮的标签：整句放得下就带字，放不下（窄窗、法语这类长译文）只留图标，不截成半句（W1 同一原则：放不下就收成图标）；说明在 .help 里
    private func fitLabel(_ title: String, _ symbol: String) -> some View {
        ViewThatFits(in: .horizontal) {
            Label(title, systemImage: symbol).lineLimit(1).fixedSize()
            Image(systemName: symbol)
        }
    }

    private func homeControls(size: CGSize) -> some View {
        HStack(spacing: 10) {
            homeGroups(compact: size.width < 1200)
            Text("\(homeGames.count) / \(model.games.count)").font(.manrope(11.5)).monospacedDigit()
                .foregroundStyle(.white.opacity(0.78)).shadow(color: .black.opacity(0.35), radius: 4, y: 1)
                .fixedSize().accessibilityIdentifier("home.count").padding(.leading, 24)
            Spacer(minLength: 16)
            HomeSearchControl(isExpanded: $showHomeSearch, text: $home.search,
                              width: size.width < 1000 ? min(LauncherControl.width, max(140, size.width - 660))
                                  : size.width < 1200 ? LauncherControl.width : LauncherTheme.Metrics.navigationWidth) { finished in
                navigation.focus("home.search.field")
                navigation.editText(value: home.search, label: loc.t("home.search"), onClose: finished) { home.search = $0 }
            }
            Button { model.sgradeUpgradeOpen = true } label: {
                Image(nsImage: SGradeCardView.sGold).resizable().interpolation(.high).aspectRatio(contentMode: .fit).frame(width: 18, height: 18)
            }.controllerButtonStyle(ConsoleIconButtonStyle()).disabled(model.games.isEmpty)   // 有局 / 装机在跑也能打开：升级在作业层排队、玩游戏时降到 1 路，按款锁兜底（11-18 维护者「不阻断玩家干别的」）
                .controllerID("library.sgradeUpgrade")
                .launcherTooltip(txt("sgrade.upgrade.button"), placement: .below).accessibilityLabel(txt("sgrade.upgrade.button"))
            Button { model.showAddGames = true } label: {
                Image(systemName: "plus")
            }
            .controllerButtonStyle(ConsoleIconButtonStyle())
            .disabled(model.busy)
            .launcherTooltip(loc.t("s2arm.action.import"), placement: .below).accessibilityLabel(loc.t("s2arm.action.import")).controllerID("top.import")
            Button { showHomeMore.toggle() } label: { Image(systemName: "ellipsis") }
                .controllerButtonStyle(ConsoleIconButtonStyle()).controllerID("home.more")
                .launcherTooltip(loc.t("home.details"), placement: .below).accessibilityLabel(loc.t("home.details"))
                .popover(isPresented: $showHomeMore, arrowEdge: .bottom) { homeMoreMenu }
        }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.24), value: showHomeSearch)
    }

    private var homeMoreMenu: some View {
        HomeMoreMenu(model: model, loc: loc) { showHomeMore = false }
    }

    private func homeCoverStrip(size: CGSize) -> some View {
        let side = min(148, max(106, size.width * 0.105))
        return ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(alignment: .top, spacing: 14) {
                    if consoleEnabled { consoleCard(side: side).id("card." + S2ArmLauncherModel.consoleID) }   // 首位固定，不算进游戏数
                    ForEach(homeGames) { game in
                        homeCard(game, side: side).id("card." + game.id)
                    }
                }.padding(.horizontal, LauncherTheme.Metrics.libraryInset).padding(.top, 6).padding(.bottom, 20)
                    .background(HorizontalWheelScroll())
            }
            .frame(height: side * 1.36 + 36, alignment: .top)
            .accessibilityIdentifier("home.covers")
            .onAppear { if let game = homeSelected { proxy.scrollTo("card." + game.id, anchor: .center) } }
            .onChange(of: model.selectedID) { id in
                guard let id else { return }
                let target = "card." + id
                if !navigation.isFullyVisible(target) {
                    withAnimation(reduceMotion ? nil : .easeOut(duration: 0.24)) { proxy.scrollTo(target, anchor: .center) }
                }
            }
            .onChange(of: navigation.focusedID) { id in
                guard let id, id.hasPrefix("card.") else { return }
                let gameID = String(id.dropFirst(5))
                if consoleEnabled, gameID == S2ArmLauncherModel.consoleID {
                    if model.selectedID != gameID { model.selectedID = gameID; model.refreshPhoneOverview(probe: true) }   // 选中总控台时探一次 USB（几秒）
                    return
                }
                guard homeGames.contains(where: { $0.id == gameID }) else { return }
                if model.selectedID != gameID { model.selectedID = gameID }
                if !navigation.isFullyVisible(id) {
                    withAnimation(reduceMotion ? nil : .easeOut(duration: 0.24)) { proxy.scrollTo(id, anchor: .center) }
                }
            }
        }
        // A different result set owns a fresh scroll position, even when the selected game ID stays unchanged.
        .id(homeGames.map(\.id))
    }

    private func homeGroups(compact: Bool) -> some View {
        HStack(spacing: compact ? 8 : 24) {
            ForEach(HomeGroup.allCases, id: \.self) { group in
                Button { home.group = group } label: {
                    Group {
                        if compact {
                            Image(systemName: group.symbol)
                                .frame(width: 40, height: 40)
                        } else {
                            Label(loc.t("home.group." + group.rawValue), systemImage: group.symbol)
                                .lineLimit(1).fixedSize(horizontal: true, vertical: false)
                        }
                    }
                        .font(.manrope(14, home.group == group ? .bold : .medium))
                        .foregroundStyle(.white.opacity(home.group == group ? 1 : 0.6))
                        .padding(.vertical, compact ? 0 : 10)
                        .overlay(alignment: .bottom) { Capsule().fill(.white.opacity(home.group == group ? 0.9 : 0)).frame(height: 2) }
                        .contentShape(Rectangle())
                }.controllerButtonStyle(.plain).controllerID("home.group." + group.rawValue)
                    .help(loc.t("home.group." + group.rawValue)).accessibilityLabel(loc.t("home.group." + group.rawValue))
                    .accessibilityAddTraits(home.group == group ? .isSelected : [])
            }
        }
    }

    /// 数据校验不过的说明（作业层原句，六种语言在对照表；同 Windows LibraryPage IntegritySkippedNote）
    static let integritySkippedNote = "这份游戏文件里有数据和原版对不上（多半是改版 / 整合版，或个别块损坏），已跳过完整性校验照常导入。"

    private func homeCard(_ game: GameSummary, side: CGFloat) -> some View {
        let selected = homeSelected?.id == game.id
        let focused = navigation.visible && navigation.focusedID == "card." + game.id
        let artworkSide = selected ? side * 1.36 : side
        return Button {
            model.selectedID = game.id
            navigation.focus("card." + game.id)
            if game.location.state == "missing" { missingFor = game }
        } label: {
            HomeCardSurface(selected: selected, focused: focused) {
                GameArtwork(covers: covers, cover: game.cover).frame(width: artworkSide, height: artworkSide)
                    .clipped().clipShape(RoundedRectangle(cornerRadius: 15))
                    .overlay(alignment: .topLeading) {
                        if game.sgrade { SGradeBadge(size: max(24, artworkSide / 9)) }
                    }
                    .overlay(alignment: .topTrailing) {
                        HStack(spacing: 4) {
                            // 「建议更新」（0.6.0 通用壳开关后面）：收藏心旁边一个小角标，原因在选中后的状态行里
                            // 可点：选中这款、焦点落到详情区的「更新这款」（11-21 定稿：角标原先只是字，点了等于点卡片）
                            if !model.updateReasons(game).isEmpty {
                                Text(BackendText.launcherText("update.hint.badge", language: loc.lang.sharedCode))
                                    .font(.manrope(10.5, .bold)).foregroundStyle(.orange)
                                    .padding(.horizontal, 7).padding(.vertical, 4)
                                    .background(.black.opacity(0.68), in: Capsule()).overlay(Capsule().strokeBorder(.orange.opacity(0.6)))
                                    .contentShape(Capsule())
                                    .highPriorityGesture(TapGesture().onEnded { focusUpdate(game) })
                                    .accessibilityIdentifier("card.updateHint." + game.id)
                                    .accessibilityAddTraits(.isButton).accessibilityAction { focusUpdate(game) }
                            }
                            if home.favorites.contains(game.id) {
                                Image(systemName: "heart.fill")
                                    .font(.system(size: 10.5, weight: .semibold))
                                    .foregroundStyle(LauncherPalette.favorite)
                                    .padding(6.5)
                                    .background(.black.opacity(0.58), in: Circle())
                                    .overlay(Circle().strokeBorder(LauncherPalette.favorite.opacity(0.38), lineWidth: 0.75))
                                    .shadow(color: .black.opacity(0.35), radius: 3, y: 1)
                            }
                        }.padding(8)
                    }
                    .overlay { if model.sgradeUpgrading(game.id) { SGradeUpgradeRing(progress: model.sgradeProgress(game.id), side: artworkSide, queuedText: model.sgradeQueued(game.id) ? txt("sgrade.upgrade.queued") : nil) } }
                    .overlay(alignment: .bottomTrailing) {   // 导入时数据校验不过、照常导入的款（11-17）：黄色提示角标，悬停 / 读屏说明见卡片 help
                        if game.integritySkipped {
                            Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 11, weight: .semibold)).foregroundStyle(.yellow)
                                .padding(6).background(.black.opacity(0.6), in: Circle()).padding(8).accessibilityHidden(true)
                        }
                    }
                    .animation(reduceMotion ? nil : .spring(response: 0.30, dampingFraction: 0.84), value: selected)
            }
        }.controllerButtonStyle(.plain)
            .animation(reduceMotion ? nil : .spring(response: 0.30, dampingFraction: 0.84), value: selected)
            .help(game.name + " · " + statusLine(game).text + (game.integritySkipped ? "\n" + loc.backend(Self.integritySkippedNote) : ""))
            .accessibilityLabel(game.name).accessibilityValue(statusLine(game).text + (game.integritySkipped ? "\n" + loc.backend(Self.integritySkippedNote) : ""))
            .accessibilityAddTraits(selected ? .isSelected : []).controllerID("card." + game.id)
            .contextMenu { cardMenu(game) }
            .modifier(CardDrop(targeted: cardTargeted(game.id), artwork: artworkSide + 10) { dropOnCard($0, game: game) })
    }

    /// 免费签名只影响设备上的副本。把到期提示和续签放在选中游戏的设备操作下方。
    @ViewBuilder private func deviceSigningStatus(_ game: GameSummary) -> some View {
        if let days = model.freeSignDaysLeft(game) {
            let kind = BackendText.deviceKind(game.iphoneInstallKind)
            let text = days == 0 ? kind + " · " + loc.t("home.card.expired")
                : txt("library.status.installed").replacingOccurrences(of: "{0}", with: "\(days)")
                    .replacingOccurrences(of: "{1}", with: kind)
            HStack(spacing: 10) {
                Label(text, systemImage: S2ArmLauncherModel.deviceSymbol(game.iphoneInstallKind))
                    .font(.manrope(12, .medium))
                    .foregroundStyle(days == 0 ? LauncherPalette.danger : days <= 2 ? .orange : .secondary)
                    .accessibilityIdentifier("hero.deviceSigningStatus")
                if days <= 1, Self.homePrimaryID(game) != "hero.renew" {
                    Button(txt("library.hero.renew")) { openRenew(game) }
                        .controllerButtonStyle(LauncherSecondaryButtonStyle())
                        .controllerID("hero.deviceRenew")
                }
            }
        }
    }

    private var homeFooter: some View {
        HStack(spacing: 20) {
            Label(loc.t("home.navigate"), systemImage: "dpad").font(.manrope(11.5))
            Spacer(minLength: 8)
            homeHint(.rightTrigger, homeSelected.map(homePrimaryTitle) ?? loc.t("home.select"))
            homeHint(.back, loc.t("home.back"))
            homeHint(.tertiary, loc.t("home.details"))
        }.foregroundStyle(.white.opacity(0.6)).padding(.horizontal, LauncherTheme.Metrics.libraryInset).frame(height: 48)
            .accessibilityIdentifier("home.controller-hints").controllerConnectedOnly()
    }
    private func homeHint(_ shortcut: PadShortcut, _ title: String) -> some View {
        HomeControllerHint(presentation: navigation.presentation, shortcut: shortcut, title: title)
    }
    // 主按钮在这款开不了局 / 主槽有作业时变成「回到当前任务」：在玩（游戏槽）回那一局，否则回主槽的作业（出包 / 导入…）；
    //   出包在跑时别的款照样「开始游戏」（游戏槽并行），玩着时没出过 Mac 版的款照样「准备」（出包页）。手机作业在顶栏胶囊里看，不占主按钮
    private func homeReturnSlot(_ game: GameSummary) -> S2ArmLauncherModel.Slot? {
        if game.playable, model.canPlay(game.id) { return nil }
        if model.playBusy, game.playable || model.playHolds(game.id) { return .game }
        if model.mainBusy { return .main }
        return model.sideJobs.first { $0.session.phase == .running || $0.session.phase == .stopping }.map { .side($0.id) }   // 主槽出完、别的款还在搬：那一行在页顶
    }
    private func homePrimarySymbol(_ game: GameSummary) -> String {
        if homeReturnSlot(game) != nil { return "arrow.up.forward.app" }
        if game.playable { return model.cloudSyncing ? "arrow.triangle.2.circlepath" : "play.fill" }
        if game.iphoneReady { return "square.and.arrow.up" }
        return "shippingbox"
    }
    func homePrimaryTitle(_ game: GameSummary) -> String {
        if model.sgradeQueued(game.id) { return txt("sgrade.upgrade.queued") }   // 排在队里还没轮到：写「排队中」，不冒充在升
        if model.sgradeUpgrading(game.id) { return Self.sgradeUpgradingTitle(model.sgradeProgress(game.id), txt) }   // 这款在升 S 级优化：按钮上直接写进度，不另弹框
        if homeReturnSlot(game) != nil { return loc.t("home.currentTask") }
        if game.playable { return loc.t(model.cloudSyncing ? "s2arm.cloud.syncing" : "library.play") }
        if game.iphoneReady { return txt(Self.renews(game) ? "library.hero.renew" : "exportsheet.open") }
        return loc.t("home.prepare")
    }
    /// 主按钮的手柄 id：能玩 → 开始游戏；做过 iPhone 版 → 导出（免费账号装过的是续签）；都没有 → 准备（出包页）。
    ///   续签单独一个 id：主按钮是「续签」时旁边照样给「导出…」（续签只预勾原来那台，要装到另一台的玩家得有导出入口）
    static func homePrimaryID(_ game: GameSummary) -> String {
        game.playable ? "hero.play" : !game.iphoneReady ? "hero.export" : renews(game) ? "hero.renew" : "hero.exportSheet"
    }
    /// 免费账号装过的款：主按钮写「续签」
    static func renews(_ game: GameSummary) -> Bool { game.iphoneInstalledAt != nil && game.iphoneFree }
    /// 续签：打开导出确认页、预勾这款当初装的那台（唯一的预勾例外：续签就是重新签给原来那台；那台没连着时那一行置灰，不替玩家换）
    func openRenew(_ game: GameSummary) { model.openExportSheet(game.id, preselect: Self.renews(game) ? game.iphoneInstallDevice : nil) }
    /// 角标点了：选中这款，焦点移到「更新这款」
    func focusUpdate(_ game: GameSummary) {
        model.selectedID = game.id
        DispatchQueue.main.async { navigation.focus("hero.update") }   // 等详情区换成这款再落焦点
    }
    /// 「正在升级 xx%」；作业还没给出百分比 / 排队中写「正在升级…」
    static func sgradeUpgradingTitle(_ progress: Double?, _ txt: (String) -> String) -> String {
        guard let p = progress else { return txt("sgrade.upgrading.wait") }
        return txt("sgrade.upgrading").replacingOccurrences(of: "{0}", with: String(Int((p * 100).rounded(.down))))
    }
    func homePrimary(_ game: GameSummary) {
        if model.sgradeUpgrading(game.id) { return }   // 升级中不能开这款
        switch homeReturnSlot(game) {
        case .game?: model.playHidden = false; return
        case .main?, .phone?:
            model.sessionHidden = false
            if model.batchRunning { model.batchShown = true }
            return
        case .side?: return   // 按款作业的进度行就在页顶（LauncherView.sideJobsBar），不换页
        case nil: break
        }
        if game.location.state == "missing" { missingFor = game; return }
        if game.playable { model.play(game, uiLanguage: uiLanguage) }
        else if game.iphoneReady { openRenew(game) }
        else { model.exportFor = game.id }
    }
    func reconcileHomeSelection() {
        if consoleEnabled && model.consoleSelected { return }   // 首位卡选中着：不改
        let games = homeGames
        if !games.contains(where: { $0.id == model.selectedID }), let first = games.first { model.selectedID = first.id }
        if let id = navigation.focusedID, id.hasPrefix("card."), !games.contains(where: { "card." + $0.id == id }) {
            navigation.focus(games.first.map { "card." + $0.id } ?? "home.search")
        }
    }
    var showingHome: Bool {
        if model.showsWelcome || model.batchShown || model.viewingResult != nil || model.exportFor != nil { return false }
        if model.playVisible { return false }
        if let session = model.session, !model.sessionHidden, !model.isInline(session) { return false }
        return true
    }
    func configurePadNavigation() {
        navigation.language = uiLanguage
        navigation.text = { loc.t($0) }
        navigation.defaultFocus = { showingHome ? homeSelected.map { "card." + $0.id } : nil }
        navigation.context = { showingHome ? "home" : model.showsWelcome ? "welcome" : "job.\((model.playVisible ? "play" : nil) ?? model.exportFor ?? model.session?.kind.rawValue ?? "")" }
        navigation.source.relayPath = { model.controller.phase == .ready && !model.controller.assigning ? model.controller.socketURL.path : nil }
        navigation.source.relayAllowed = { !model.playSessionActive && !model.controller.assigning && !ControllerTestHub.shared.blocksRelay }
        if ControllerTestHub.allowed {   // 手柄测试面板（本地 / 自测构建）：同一条连接顺带要体感读数
            let hub = ControllerTestHub.shared
            hub.source = navigation.source
            hub.gameRunning = { model.playSessionActive }
            navigation.source.motionWanted = { hub.wantsRelayMotion }
            navigation.source.receiveMotion = { hub.receive($0) }
        }
        navigation.source.relayConnected = { model.controller.deviceCount > 0 }
        navigation.source.assignment = { model.controller.assignment }
        navigation.source.faceMapping = { model.controller.faceMaps.mapping(for: $0) }
        navigation.onAction = { action in
            if action == .back, !showingHome, !model.showsWelcome { model.goToLibrary(); return true }
            guard showingHome else { return false }
            switch action {
            case .previousGroup, .nextGroup:
                home.cycle(action == .previousGroup ? -1 : 1)
                navigation.focus("home.group." + home.group.rawValue)
                return true
            case .details: if let game = homeSelected { moreFor = game.id }; return true
            // 左右移动就是选中；焦点所在的卡片按 A = 主操作（同 ZR）。提示列确认键统一显示 A，单只 Joy-Con 没有 ZR
            case .confirm:
                if let game = homeSelected, navigation.focusedID == "card." + game.id { navigation.handle(.submit); return true }
            case .search:
                navigation.focus("home.search")
                navigation.activateFocused(); return true
            case .back:
                if showHomeSearch { showHomeSearch = false; navigation.focus("home.search") }
                else if !home.search.isEmpty { home.search = "" }
                else if let game = homeSelected { navigation.focus("card." + game.id) }
                return true
            default: break
            }
            if action == .up, navigation.focusedID?.hasPrefix("hero.") == true, let game = homeSelected {
                navigation.focus("card." + game.id); return true
            }
            if action == .down, let focused = navigation.focusedID,
               focused == "home.search" || focused == "home.refresh" || focused == "library.sgradeUpgrade" || focused.hasPrefix("home.group."),
               let game = homeSelected {
                navigation.focus("card." + game.id); return true
            }
            guard let id = navigation.focusedID, id.hasPrefix("card."),
                  let index = homeGames.firstIndex(where: { "card." + $0.id == id }) else { return false }
            switch action {
            case .left, .right:
                let next = index + (action == .left ? -1 : 1)
                if homeGames.indices.contains(next) {
                    model.selectedID = homeGames[next].id
                    navigation.focus("card." + homeGames[next].id)
                }
                return true
            case .down:
                let game = homeGames[index]
                navigation.focus(Self.homePrimaryID(game))
                return true
            case .up: navigation.focus("home.search"); return true
            default: return false
            }
        }
    }
}

private struct HomeControllerHint: View {
    @ObservedObject var presentation: PadPresentation
    let shortcut: PadShortcut
    let title: String
    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: presentation.symbols[shortcut.displayed] ?? shortcut.displayed.symbol).font(.system(size: 19))
            Text(title).font(.manrope(11.5))
        }.fixedSize()
    }
}

/// 角标里的小图标 + 字
struct ConsoleBadgeLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 4) { configuration.icon.font(.system(size: 10, weight: .bold)); configuration.title }
    }
}

struct HomeCardSurface<Label: View>: View {
    let selected: Bool
    let focused: Bool
    @ViewBuilder let label: () -> Label
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var hovered = false

    var body: some View {
        // 选中卡片无论当前是否持有控制器/鼠标焦点，保持完全一致的清晰奶油色边框与层次感；
        // 避免「点击时亮白框、一失焦边框就消失」的割裂感。
        let active = selected || focused
        let rimOpacity = active ? 0.82 : (hovered ? 0.45 : 0.14)
        let rimWidth: CGFloat = active ? 1.5 : (hovered ? 1.25 : 1.0)
        let rimColor: Color = active ? LauncherPalette.cream : .white
        label()
            .overlay(
                RoundedRectangle(cornerRadius: 15)
                    .strokeBorder(rimColor.opacity(rimOpacity), lineWidth: rimWidth)
            )
            .scaleEffect(!reduceMotion && hovered && !active ? 1.03 : 1)
            .shadow(color: active ? LauncherPalette.cream.opacity(0.18) : .clear, radius: 10, y: 2)
            .shadow(color: .black.opacity(active ? 0.48 : (hovered ? 0.34 : 0.22)), radius: active ? 18 : (hovered ? 10 : 6), y: active ? 9 : 5)
            .padding(5)
            .contentShape(RoundedRectangle(cornerRadius: 20))
            .animation(reduceMotion ? nil : .spring(response: 0.30, dampingFraction: 0.84), value: active)
            .animation(reduceMotion ? nil : .easeOut(duration: 0.16), value: hovered)
            .onHover { hovered = $0 }
    }
}

/// 将桌面鼠标的垂直滚轮事件转为首页封面横排的横向滚动，方便无触控板用户浏览全部游戏。
private struct HorizontalWheelScroll: NSViewRepresentable {
    func makeNSView(context: Context) -> Host { Host() }
    func updateNSView(_ view: Host, context: Context) {}
    static func dismantleNSView(_ view: Host, coordinator: ()) { view.stopObserving() }

    final class Host: NSView {
        private var monitor: Any?
        override func hitTest(_ point: NSPoint) -> NSView? { nil }
        override func isAccessibilityElement() -> Bool { false }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            stopObserving()
            guard window != nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
                self?.handle(event) ?? event
            }
        }
        private func handle(_ event: NSEvent) -> NSEvent? {
            guard let scrollView = enclosingScrollView,
                  let window = scrollView.window,
                  event.window === window,
                  !scrollView.isHiddenOrHasHiddenAncestor,
                  abs(event.scrollingDeltaY) > 0.01,
                  abs(event.scrollingDeltaX) < abs(event.scrollingDeltaY) else {
                return event
            }
            let point = scrollView.convert(event.locationInWindow, from: nil)
            guard scrollView.bounds.contains(point) else { return event }
            let clipView = scrollView.contentView
            guard let documentView = scrollView.documentView else { return event }
            let maxX = max(0, documentView.frame.width - clipView.bounds.width)
            guard maxX > 0 else { return event }
            let delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.scrollingDeltaY * 24
            let current = clipView.bounds.origin
            let nextX = min(max(0, current.x - delta), maxX)
            guard abs(nextX - current.x) > 0.01 else { return event }
            clipView.scroll(to: NSPoint(x: nextX, y: current.y))
            scrollView.reflectScrolledClipView(clipView)
            return nil
        }
        func stopObserving() {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
        }
        deinit { stopObserving() }
    }
}

/// 游戏库标题行「…」弹出的菜单（游戏库级动作）。菜单项是一张表（id / 标题 / 图标 / 动作）：界面照表画，单测直接核表里有哪几项、点了发出什么作业，
///   不用在桌面上开窗（离屏 NSHostingView 读不到无障碍树）
struct HomeMoreMenu: View {
    struct Item {
        let id: String, title: String, symbol: String
        let disabled: Bool
        let action: () -> Void
    }
    @ObservedObject var model: S2ArmLauncherModel
    let loc: Localizer
    let dismiss: () -> Void

    var items: [Item] {
        let lang = loc.lang.sharedCode
        return [
            Item(id: "home.refresh", title: loc.t("s2arm.library.refresh"), symbol: "arrow.clockwise", disabled: model.loading) { [model] in model.refresh() },
            // 不连手机、不经总控台：组不带游戏的通用 S2Arm.ipa 放进「S2Arm 游戏包」（作业 installUpdate + mode export → export_shell_ipa），
            //   给 LiveContainer / SideStore / AltStore 自签（11-21 维护者；同总控台「别的工具装的」那台手机下的同名按钮）
            Item(id: "home.exportShellIpa", title: BackendText.launcherText("console.button.exportShellIpa", language: lang), symbol: "square.and.arrow.up",
                 disabled: false) { [model] in model.updatePhone(acceptStale: false, repack: [], language: lang, mode: "export") },
        ]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(items, id: \.id) { item in
                Button { dismiss(); item.action() } label: {
                    HStack(spacing: 10) {
                        Image(systemName: item.symbol).font(.system(size: 14, weight: .medium)).frame(width: 20, height: 20)
                        Text(item.title).lineLimit(1)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10).padding(.vertical, 8).contentShape(Rectangle())
                }
                .controllerButtonStyle(.plain).controllerID(item.id).disabled(item.disabled)
            }
        }
        .padding(8).frame(width: 220).font(.manrope(13.5, .semibold)).preferredColorScheme(.dark)
    }
}
