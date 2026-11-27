// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Avalonia Headless snapshot runner: 欢迎页 + 出包页各状态 → PNG。
// 只用于截图验收，不进发布包。VisualTests 已并入本工程。
using System.Reflection;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Headless;
using Avalonia.Media.Imaging;
using Avalonia.Skia;
using Avalonia.Themes.Fluent;
using Avalonia.Styling;
using Avalonia.Threading;
using Avalonia.VisualTree;
using S2Arm.Launcher;
using S2Arm.Launcher.Protocol;

// --scroll-probe <目录>：各页滚到中间后连续刷新，逐帧读控件屏幕坐标、存帧图（跳动验收，ScrollProbe.cs）
var scrollProbe = args.Length > 0 && args[0] == "--scroll-probe";
if (scrollProbe) args = args[1..];
// --flicker-probe <目录>：出包 / 装机在跑时按真实刷新路径刷 30 拍，量进度条被换次数、新建控件数、画面帧变化（闪烁验收，FlickerProbe.cs）
var flickerProbe = args.Length > 0 && args[0] == "--flicker-probe";
if (flickerProbe) args = args[1..];
// --settings-probe <目录>：全新窗口在几种引导状态下点顶栏设置键，任何异常都算不通（玩家反馈：引导页点设置闪退，SettingsProbe.cs）
var settingsProbe = args.Length > 0 && args[0] == "--settings-probe";
if (settingsProbe) args = args[1..];
// --report-probe <目录>：各页 / 各引导状态下顶栏最右都有「故障报修」键（11-13，ReportProbe.cs），每页存帧图
var reportProbe = args.Length > 0 && args[0] == "--report-probe";
// --build-route-probe <目录>：A 在出包时打开 B 的出包页，停在 B 自己的页、说清排在 A 后面（玩家反馈，BuildRouteProbe.cs）
var buildRouteProbe = args.Length > 0 && args[0] == "--build-route-probe";
// --dismiss-probe <目录>：控制口 /dismiss（s2armctl dismiss）关得掉新功能大窗 / 更新弹窗（DismissProbe.cs）
var dismissProbe = args.Length > 0 && args[0] == "--dismiss-probe";
// --install-action-probe <目录>：装机前清单按钮落点（11-19 落点契约：action 唯一来源、/install-action、presented；InstallActionProbe.cs）
var installActionProbe = args.Length > 0 && args[0] == "--install-action-probe";
// --details-probe <目录>：装机卡片「详细过程」点开后不被进度事件刷新收起（D-237B，DetailsProbe.cs）
var detailsProbe = args.Length > 0 && args[0] == "--details-probe";
// --console-probe <目录>：首位 S2Arm 总控台窄窗 / 英文不被裁、页顶不写长编号（0.6.2 前端走查，ConsoleProbe.cs）
var consoleProbe = args.Length > 0 && args[0] == "--console-probe";
// --update-export-probe <目录>：游戏库「更新 / 导出」两件事（0.6.4 改版：大卡三态、导出确认页设备行、开始后起的单、逐台结果，UpdateExportProbe.cs）
var updateExportProbe = args.Length > 0 && args[0] == "--update-export-probe";
// --confirm-probe <目录>：推游戏遇到手机上旧 App 的确认页（#1842，phone_app_outdated 带 accept，ConfirmProbe.cs）
var confirmProbe = args.Length > 0 && args[0] == "--confirm-probe";
// --walk-probe <目录>：玩家走查截图（6 个角色从打开启动器走到手机上玩，每步整页 PNG + 屏幕文字，WalkProbe.cs）
var walkProbe = args.Length > 0 && args[0] == "--walk-probe";
// --cover-probe <目录>：坏封面只让那一格用占位图、设置框没画出来时环境检查路径不变空（D-97FT，CoverProbe.cs）
var coverProbe = args.Length > 0 && args[0] == "--cover-probe";
// --endjob-probe <目录>：出包客户端抛 catch 列表之外的异常时，收尾照样撤作业登记、按钮恢复（#2191，EndJobProbe.cs）
var endJobProbe = args.Length > 0 && args[0] == "--endjob-probe";
// --pack-mutex-probe <目录>：出包时只挡正在出的那一款（#2191，PackMutexProbe.cs）
var packMutexProbe = args.Length > 0 && args[0] == "--pack-mutex-probe";
if (reportProbe || buildRouteProbe || dismissProbe || installActionProbe || detailsProbe || consoleProbe || updateExportProbe || walkProbe || confirmProbe || coverProbe || endJobProbe || packMutexProbe) args = args[1..];
var outDir = args.Length > 0 ? args[0] : Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".s2arm/run/s2arm/cmdr/evidence/launcher/win");
Directory.CreateDirectory(outDir);

// 顶部选中游戏按钮断言（v0.4.5 修；0.6.4 改成「更新 / 导出」两件事）：有横图 / 无横图两个分支里都要找得到「导出…」、找不到「推到 iPhone」「导出自签包」；不通过时末尾退出 1
var heroActionFailures = new List<string>();

AppBuilder.Configure<SnapshotApp>()
    .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
    .With(new SkiaOptions())
    .UseSkia()
    .SetupWithoutStarting();
if (scrollProbe) { Environment.Exit(ScrollProbe.Run(outDir)); }
if (flickerProbe) { Environment.Exit(FlickerProbe.Run(outDir)); }
if (settingsProbe) { Environment.Exit(SettingsProbe.Run(outDir)); }
if (reportProbe) { Environment.Exit(ReportProbe.Run(outDir)); }
if (buildRouteProbe) { Environment.Exit(BuildRouteProbe.Run(outDir)); }
if (dismissProbe) { Environment.Exit(DismissProbe.Run(outDir)); }
if (confirmProbe) { Environment.Exit(ConfirmProbe.Run(outDir)); }
if (installActionProbe) { Environment.Exit(InstallActionProbe.Run(outDir)); }
if (detailsProbe) { Environment.Exit(DetailsProbe.Run(outDir)); }
if (consoleProbe) { Environment.Exit(ConsoleProbe.Run(outDir)); }
if (updateExportProbe) { Environment.Exit(UpdateExportProbe.Run(outDir)); }
if (coverProbe) { Environment.Exit(CoverProbe.Run(outDir)); }
if (endJobProbe) { Environment.Exit(EndJobProbe.Run(outDir)); }
if (packMutexProbe) { Environment.Exit(PackMutexProbe.Run(outDir)); }
if (walkProbe) { Environment.Exit(WalkProbe.Run(outDir)); }

// ── 统一夹具游戏（不用固定游戏名字符串以外的占位）────────────────────
static LibraryGame MockGame(bool keysReady = true) => new(
    "0100B040192FA000", "Fire Emblem: Fortune's Weave", "v1.0.1",
    "", false, [], keysReady, keysReady ? DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 86400.0 : null,
    true, [new("base", "0100B040192FA000", 917504, "v1.0.1", 31_000_000_000L, "")],
    IphoneExpires: keysReady ? DateTimeOffset.UtcNow.ToUnixTimeSeconds() + 6 * 86400.0 : null);   // 作业层给的免费签名到期（1 天前装、按 7 天估）

static LibraryCatalog MockCatalog(bool keysReady = true)
{
    var game = MockGame(keysReady);
    return new LibraryCatalog([game], keysReady, keysReady ? "密钥已就绪" : "");
}

// ── 单窗口复用：欢迎页场景 ──────────────────────────────────────────────
static EnvironmentReport FakeReport(bool ready) => EnvironmentReport.Parse(
    ready
    ? "{\"schema\":1,\"ready\":true,\"checks\":[{\"id\":\"host\",\"name\":\"Windows 64位\",\"status\":\"ok\",\"ok\":true,\"message\":\"就绪\",\"code\":null,\"action\":null}]}"
    : "{\"schema\":1,\"ready\":false,\"checks\":[{\"id\":\"python\",\"name\":\"Python\",\"status\":\"missing\",\"ok\":false,\"message\":\"需要 Python 3.12\",\"code\":\"python_missing\",\"action\":\"安装 Python\",\"url\":\"https://www.python.org\"}]}",
    ready ? 0 : 1);

var welcome = new EnvironmentWindow();
welcome.Width = 1280; welcome.Height = 900;
welcome.Show();
Dispatcher.UIThread.RunJobs();

void SaveW(string name, Action<EnvironmentWindow> setup)
{
    setup(welcome);
    SetW(welcome, "page", "library");
    CallW(welcome, "Render");
    Settle();
    var bmp = welcome.CaptureRenderedFrame();
    if (bmp is not null) { using var s = File.OpenWrite(Path.Combine(outDir, name + ".png")); bmp.Save(s); Console.WriteLine("  saved: " + name); }
    else Console.WriteLine("  WARN no frame: " + name);
}

SaveW("01_welcome_empty", w => { SetW(w, "report", null); SetW(w, "busy", false); SetW(w, "catalog", null); });
SaveW("02_welcome_env_checking", w => { SetW(w, "busy", true); SetW(w, "summaryKey", "environment.checking"); });
SaveW("03_welcome_env_ready_no_games", w => { SetW(w, "busy", false); SetW(w, "report", FakeReport(true)); SetW(w, "catalog", null); });
SaveW("04_welcome_games_chosen", w => {
    SetW(w, "report", FakeReport(true));
    SetW(w, "pendingImportPaths", new[] { "/tmp/fe_fortune.nsz" });
});
// 新电脑第一次打开：别的都就绪、只差 Metal 编译器（玩家要自己去 Apple 官网装）——欢迎页第 1 步的分步说明与「打开官方页面」
SaveW("04b_welcome_metal_missing", w => {
    SetW(w, "catalog", null);
    SetW(w, "report", EnvironmentReport.Parse(
        "{\"schema\":1,\"ready\":false,\"checks\":[{\"id\":\"host\",\"name\":\"Windows 64位\",\"status\":\"ok\",\"ok\":true,\"message\":\"就绪\",\"code\":null,\"action\":null,\"blocking\":true}," +
        "{\"id\":\"metal\",\"name\":\"官方Windows Metal编译器\",\"status\":\"missing\",\"ok\":false,\"message\":\"没装\",\"code\":\"metal_missing\",\"action\":\"install\",\"blocking\":true," +
        "\"url\":\"https://developer.apple.com/download/all/?q=metal%20developer%20tools%20for%20windows\"}]}", 1));
});
SaveW("05_welcome_oneclick", w => {
    SetW(w, "report", FakeReport(true));
    SetW(w, "welcomeGameId", MockGame().Id);
    SetW(w, "catalog", MockCatalog());
    SetW(w, "connectedFolder", "/tmp/s2arm-library");
    SetW(w, "libraryId", "mock-lib-id");
});
// 顶部公告条（v0.2.3）：内容取仓库里那份 docs/site/notice.json（启动器实际读的同一份），中 / 英各一张
var noticeDir = new DirectoryInfo(AppContext.BaseDirectory);
while (noticeDir is not null && !File.Exists(Path.Combine(noticeDir.FullName, "docs/site/notice.json"))) noticeDir = noticeDir.Parent;
var noticeJson = File.ReadAllText(Path.Combine(noticeDir?.FullName ?? throw new FileNotFoundException("docs/site/notice.json"), "docs/site/notice.json"));
SaveW("06_notice_zh", w => { SetW(w, "notice", (JsonElement?)JsonDocument.Parse(noticeJson).RootElement); SetW(w, "language", "zh-Hans"); });
SaveW("07_notice_en", w => { SetW(w, "language", "en"); });
SetW(welcome, "notice", null); SetW(welcome, "language", "zh-Hans");
// 新功能大窗（大版本首次打开）：内容取仓库里那份 docs/site/feature.json（随包那份），图换成绝对路径（同 updater.py feature_spotlight 给的），每页一张
var site = Path.Combine(noticeDir!.FullName, "docs/site");
var featureNode = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(Path.Combine(site, "feature.json")))!.AsObject();
featureNode["kind"] = "feature";
foreach (var pageNode in featureNode["pages"]!.AsArray())
{
    // 同 updater.py feature_spotlight：image 给成 {zh-Hans, en} 绝对路径，英文缺了用中文
    var raw = pageNode!["image"]!;
    string Abs(string rel) => Path.Combine(site, rel);
    var zhImg = raw is System.Text.Json.Nodes.JsonObject o1 ? Abs(o1["zh-Hans"]!.GetValue<string>()) : Abs(raw.GetValue<string>());
    var enRel = raw is System.Text.Json.Nodes.JsonObject o2 && o2["en"] is { } e ? Abs(e.GetValue<string>()) : null;
    pageNode["image"] = new System.Text.Json.Nodes.JsonObject { ["zh-Hans"] = zhImg, ["en"] = enRel is not null && File.Exists(enRel) ? enRel : zhImg };
    pageNode["layout"] = pageNode["title"] is null ? "poster" : "detail";   // 同 updater.py feature_spotlight：只有图 = 海报页
}
var featurePopup = JsonDocument.Parse(featureNode.ToJsonString()).RootElement.Clone();
for (var i = 0; i < featureNode["pages"]!.AsArray().Count; i++)
{
    var at = i;
    SaveW($"08_feature_zh_{at + 1}", w => { SetW(w, "updatePopup", (JsonElement?)featurePopup); SetW(w, "featureIndex", at); });
}
SaveW("09_feature_en_1", w => { SetW(w, "language", "en"); SetW(w, "featureIndex", 0); });
// 「现在更新并重启」时有任务在跑（11-18 维护者：认得出的风险说清后让玩家选「仍然重启 / 取消」，不再只能等）：出包在跑 → 确认块写清会中断哪个任务、重启后能接着做
var gateUpdate = JsonDocument.Parse("{\"version\":\"9.9.9\",\"notes\":null}").RootElement.Clone();
var gateJob = welcome.GetType().GetMethod("BeginJob", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(welcome, [Enum.Parse(welcome.GetType().GetNestedType("JobKind", BindingFlags.NonPublic)!, "Pack"), MockGame().Id]);
SaveW("09b_update_restart_confirm_zh", w => {
    SetW(w, "catalog", MockCatalog()); SetW(w, "availableUpdate", (JsonElement?)gateUpdate); SetW(w, "featureIndex", 0);
    SetW(w, "updatePopup", (JsonElement?)JsonDocument.Parse("{\"kind\":\"update\",\"version\":\"9.9.9\",\"notes\":null}").RootElement.Clone());
    SetW(w, "updateGate", "update.gate.confirm"); SetW(w, "language", "zh-Hans");
});
SaveW("09c_update_restart_confirm_en", w => { SetW(w, "language", "en"); });
welcome.GetType().GetMethod("EndJob", BindingFlags.Instance | BindingFlags.NonPublic)?.Invoke(welcome, [gateJob]);
SetW(welcome, "updateGate", ""); SetW(welcome, "availableUpdate", null);
SetW(welcome, "updatePopup", null); SetW(welcome, "language", "zh-Hans");

welcome.Close();
Dispatcher.UIThread.RunJobs();

// ── 单窗口复用：出包页场景 ──────────────────────────────────────────────
var pack = new EnvironmentWindow();
pack.Width = 1280; pack.Height = 900;
pack.Show();
Dispatcher.UIThread.RunJobs();

// 让过渡动画（如折叠箭头 0→180° 旋转）走完再截，否则截到半转的箭头像个「厂」字
void Settle()
{
    Dispatcher.UIThread.RunJobs();
    var until = DateTime.UtcNow.AddMilliseconds(600);
    while (DateTime.UtcNow < until) { AvaloniaHeadlessPlatform.ForceRenderTimerTick(); Dispatcher.UIThread.RunJobs(); Thread.Sleep(20); }
}

void SaveP(string name, Action setup)
{
    setup();
    Settle();
    var bmp = pack.CaptureRenderedFrame();
    if (bmp is not null) { using var s = File.OpenWrite(Path.Combine(outDir, name + ".png")); bmp.Save(s); Console.WriteLine("  saved: " + name); }
    else Console.WriteLine("  WARN no frame: " + name);
}

InjectMockGame(pack, ready: false);
SaveP("win_01_export_select", () => Navigate(pack, "build"));
// S 级优化卡片（Beta）三种状态：直接注入作业层 patchMatch 的结果（不起后端）——这个版本命中 / 只命中游戏的别的版本 / 都没有
foreach (var (name, json) in new[] {
    ("win_01b_export_sgrade_hit", "{\"build\":\"d95ec48ffe269b6e\",\"exact\":{\"widescreen\":1,\"fps\":1,\"cheat\":12},\"others\":[]}"),
    ("win_01c_export_sgrade_others", "{\"build\":\"aa11bb22cc33dd44\",\"exact\":{},\"others\":[{\"version\":\"1.1.0\",\"cats\":[\"fps\",\"widescreen\"]},{\"version\":\"1.0.0\",\"cats\":[\"cheat\"]}]}"),
    ("win_01d_export_sgrade_none", "{\"build\":\"aa11bb22cc33dd44\",\"exact\":{},\"others\":[]}"),
    ("win_01e_export_sgrade_ownwiden", "{\"build\":\"aa11bb22cc33dd44\",\"exact\":{\"cheat\":3},\"others\":[],\"ownWiden\":true}") })
{
    SaveP(name, () => { Navigate(pack, "build"); SetW(pack, "sgradeFor", MockGame(false).Id); SetW(pack, "sgradeFailed", false);
        SetW(pack, "sgrade", (JsonElement?)JsonDocument.Parse(json).RootElement); Navigate(pack, "build"); });
}

InjectMockProgress(pack, fraction: 0.05, stepIndex: 0, running: true);
SaveP("win_02_export_progress_running", () => Navigate(pack, "progress"));

InjectMockProgress(pack, fraction: 0.60, stepIndex: 1, running: true);
SaveP("win_03_export_progress_mid", () => Navigate(pack, "progress"));

InjectMockProgress(pack, fraction: 1.0, stepIndex: -1, running: false, state: "succeeded",
    ipaBytes: 1_200_000_000L, packBytes: 3_400_000_000L, elapsed: 247);
SaveP("win_04_export_success", () => Navigate(pack, "progress"));
// 降级继续（v0.3.1）：出成了，完成卡下方逐条列作业层给的玩家提示
SetW(pack, "packNotes", (object?)new List<string> { "这款有 27 处用了兼容写法，个别场景可能不稳定，遇到请反馈", "3 支着色器首次遇到时现编，开头可能稍卡" });
SaveP("win_04b_export_success_notes", () => Navigate(pack, "progress"));
// S 级加强横幅 + 补丁打入结果（v0.4.0，同 Mac）：注入作业层 boost（条数 + applied / failed）
SetW(pack, "packBoost", (JsonElement?)JsonDocument.Parse("""{"cheats":12,"fps":1,"widescreen":1,"applied":[{"cat":"widescreen","name":"满屏 21:9"},{"cat":"fps","name":"60 FPS"}],"failed":[{"cat":"gfx","name":"阴影质量","why":"result.why.mismatch"}]}""").RootElement.Clone());
SaveP("win_04c_export_success_sgrade", () => Navigate(pack, "progress"));
SetW(pack, "packBoost", null);
SetW(pack, "packNotes", (object?)new List<string>());

// 导出到文件夹前先做 iPhone 版（0.6.4 导出确认页起的一单）：出包进度最后一步是「整理 iPhone 版程序与资源包」
InjectMockProgress(pack, fraction: 0.85, stepIndex: 2, running: true, exportMode: true);
SaveP("win_04c_export_selfsign_progress", () => Navigate(pack, "progress"));
InjectMockProgress(pack, fraction: 1.0, stepIndex: -1, running: false, state: "succeeded",
    ipaBytes: 1_200_000_000L, packBytes: 3_400_000_000L, elapsed: 247, exportMode: true);
SaveP("win_04d_export_selfsign_success", () => Navigate(pack, "progress"));

InjectMockProgress(pack, fraction: 0.12, stepIndex: 0, running: false, state: "failed",
    summary: "磁盘空间不足，无法继续出包。");
SaveP("win_05_export_failed", () => Navigate(pack, "progress"));

InjectMockGame(pack, ready: true);
SaveP("win_06_library_iphone_ready", () => Navigate(pack, "library"));   // 大卡：更新（做过 iPhone 版才有）+ 导出… + 出包
CheckHeroActions(pack, "无横图的选中游戏", heroActionFailures);
// 顶部大图带操作按钮（v0.4.5 修）：有横图的选中游戏顶部也常驻「更新 / 导出… / 出包」。
//   横图是代码里合成的纯色渐变图（仓库里不放商店图）；夹具游戏名用通用占位，不用真游戏名
{
    const string syntheticBannerPngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAUAAAAC0CAIAAABqhmJGAAAC0ElEQVR42u3TWU4UAQAA0T6CO4ODgIDIIruKqIiKuCtu9z+Lc4X+mlTykneCSmrYfPoPiBokAAMDcxn45C8QZWAoD/zo5C8QZWBoD/wHiDIwpAc+/gNEGRjaA/8GogwM5YE3jn8DUQaG9MBHv4AoA4OBAQMDYwe+BqIMDOWB1w+vgSgDQ3vgn0CUgcHAgIGBkQMf/ACiDAzlgdcOfgBRBob2wN+BKANDeuD970CUgaE98DcgysBQHvjh/jcgysCQHnjvKxBlYDAwYGBg7MBfgCgDQ3ng1SdfgCgDQ3vgz0CUgcHAgIGBkQPvfgKiDAzlgVd2PwFRBob2wB+BKANDeuCdj0CUgaE98BUQZWAwMDCXgZd3roCoYXn7CoiaDfwBiDIwGBgwMDB24EsgaljeugSihgdbl0CUgaE98HsgysBgYGA+Az9+D0TNBn4HRBkYygMvPX4HRBkY2gO/BaKGpc23QJSBwcDAnAa+AKIMDOWBp5sXQNQwfXQBRM0GfgNEGRgMDBgYGDvwORA1TDfOgajh/sY5EGVgaA/8GogyMBgYmM/A66+BqNnAr4AoA0N54MX1V0CUgaE98EsgalhcewlEGRgMDMxp4DMgysBQHniydgZEDZOHZ0DUbOAXQJSBwcCAgYGxA58CUcNk9RSIGhZWT4EoA4OBgTkN/ByIMjCkB155DkQZGNoDPwOiDAzlge+tPAOiDAzpgZefAlEGBgMDBgbGDnwCRBkYygPffXACRBkY2gMfA1EGBgMDBgZGDrx0BEQZGMoD31k6AqIMDO2BD4EoA0N64OkhEGVgaA98AEQZGMoD354eAFEGhvTA9/eBKAODgQEDA2MH3gOiDAzlgW8t7gFRBob2wE+AKAODgQEDAyMHnuwCUQaG8sA3J7tAlIGhPfAOEGVgSA+8sANEGRjaA28DUQaG8sA3FraBKANDeuB7W0CUgcHAwDz8B+xdxHNM+cLrAAAAAElFTkSuQmCC";
    var bannerLib = Path.Combine(Path.GetTempPath(), "s2a-snap-" + Guid.NewGuid().ToString("N"), "S2Arm");
    var bannerFolder = Path.Combine(bannerLib, "games", "Sample Game");
    Directory.CreateDirectory(bannerFolder);
    File.WriteAllBytes(Path.Combine(bannerFolder, "banner.jpg"), Convert.FromBase64String(syntheticBannerPngBase64));
    var bannerGame = MockGame(true) with { Id = "0100F00100000000", Name = "示例游戏", Banner = Path.Combine(bannerFolder, "banner.jpg"),
        Location = new GameLocation("ok", bannerLib, bannerFolder, "", true) };
    SetW(pack, "catalog", new LibraryCatalog([bannerGame], true, "密钥已就绪"));
    SetW(pack, "selectedGame", bannerGame.Id);
    SetW(pack, "connectedFolder", bannerLib);
    SetW(pack, "libraryId", "mock-lib-id");
    SetW(pack, "librarySummary", "library.completed");
    SaveP("win_06d_library_hero_banner_actions", () => Navigate(pack, "library"));
    CheckHeroActions(pack, "有横图的选中游戏", heroActionFailures, expectBanner: true);
    InjectMockGame(pack, ready: true);   // 恢复后续场景用的无横图夹具
}
// 导入时数据校验不过、照常导入的款（11-17）：封面右下角黄色「!」，悬停 / 选中时说明；夹具用通用占位名
{
    var plain = MockGame(true) with { Id = "0100F00200000000", Name = "示例游戏 A" };
    var skipped = MockGame(true) with { Id = "0100F00300000000", Name = "示例游戏 B",
        Packages = [new("base", "0100F00300000000", 0, "v1.0.0", 1L, "", IntegritySkipped: true)] };
    SetW(pack, "catalog", new LibraryCatalog([plain, skipped], true, "密钥已就绪"));
    SetW(pack, "selectedGame", skipped.Id);
    SaveP("win_06e_library_integrity_skipped", () => Navigate(pack, "library"));
    InjectMockGame(pack, ready: true);
}
// 导出自签包的进度 / 完成（11-15：装机页只画结局卡，不画装机说明、清单、开始装机 / 删除）
{
    var g = MockGame(true);
    SetW(pack, "installExport", true); SetW(pack, "installGameId", g.Id); SetW(pack, "installGameName", g.Name); SetW(pack, "selectedGame", g.Id);
    var it = GetW(pack, "installTrack")!;
    void ITrack(string f, object? v) => it.GetType().GetField(f)!.SetValue(it, v);
    ITrack("Kind", "install"); ITrack("Started", DateTime.UtcNow.AddSeconds(-40));
    SetW(pack, "installState", "running");
    SaveP("win_06b_selfsign_running", () => Navigate(pack, "install"));
    SetW(pack, "installState", "succeeded"); ITrack("Ended", (DateTime?)DateTime.UtcNow);
    ITrack("ExportFolder", @"D:\S2Arm Games\S2Arm 自签导出"); ITrack("ExportGames", 3); ITrack("ExportBytes", (long?)9_800_000_000L);
    SaveP("win_06c_selfsign_done", () => Navigate(pack, "install"));
    ITrack("ExportFolder", ""); ITrack("ExportGames", 0); ITrack("ExportBytes", (long?)null);
    SetW(pack, "installExport", false); SetW(pack, "installState", ""); SetW(pack, "installGameId", null);
}

InjectMockGame(pack, ready: false);
InjectMockProgress(pack, fraction: 0.35, stepIndex: 1, running: true);
SetW(pack, "sessionHidden", true);
SaveP("win_07_library_capsule", () => Navigate(pack, "library"));

// 「打开游戏文件夹」找不到文件夹：按钮下方说明原因，不打开空的资源管理器
InjectMockProgress(pack, fraction: 1.0, stepIndex: -1, running: false, state: "succeeded",
    ipaBytes: 1_200_000_000L, packBytes: 3_400_000_000L, elapsed: 247);
SetW(pack, "revealProblem", "找不到这款游戏的文件夹：所在的磁盘可能没接上，或文件夹被移走了。");
SaveP("win_08_export_success_folder_missing", () => Navigate(pack, "progress"));
SetW(pack, "revealProblem", "");

// 更新手机上的 S2Arm（v0.3.1 重做：进行中 / 成功 / 失败三态同一张结局卡，PhoneJobCard.cs；11-11 维护者「成功了也看不出成功」）
//   updateTrack 用作业层真实事件喂（调它的 Begin / Ingest），状态与结局事实按作业结果设
void PhoneTrackEvents(params string[] events)
{
    var track = GetW(pack, "updateTrack")!;
    track.GetType().GetMethod("Begin")!.Invoke(track, ["installUpdate", "iPhone「我的 iPhone」"]);
    foreach (var e in events) track.GetType().GetMethod("Ingest")!.Invoke(track, [JsonDocument.Parse(e).RootElement.Clone()]);
}
void TrackField(string field, object? value) { var track = GetW(pack, "updateTrack")!; track.GetType().GetField(field)!.SetValue(track, value); }
PhoneTrackEvents(
    """{"type":"stage","stage":"找签名账号与 iPhone","message":"找签名账号与 iPhone","stageIndex":1,"stageCount":6,"overall":0.0}""",
    """{"type":"stage","stage":"把这款放进手机上的 S2Arm","message":"把这款放进手机上的 S2Arm","stageIndex":2,"stageCount":6,"overall":0.17}""",
    """{"type":"stage","stage":"用你的 Apple ID 签名","message":"用你的 Apple ID 签名","stageIndex":4,"stageCount":6,"overall":0.5}""",
    """{"type":"stage","stage":"推到 iPhone","message":"推到 iPhone","stageIndex":5,"stageCount":6,"overall":0.62}""");
SetW(pack, "updateState", "running");
SaveP("win_20_phone_update_running", () => Navigate(pack, "phoneupdate"));
TrackField("Started", DateTime.UtcNow.AddSeconds(-47)); TrackField("Ended", (DateTime?)DateTime.UtcNow);
TrackField("PackBytes", (long?)0L); TrackField("PackFiles", (int?)0); TrackField("Version", "0.3.0");
SetW(pack, "updateState", "succeeded");
SaveP("win_21_phone_update_done", () => Navigate(pack, "phoneupdate"));
// 导出模式完成（#1132）：3 款里 2 款沿用上次导出，只新写了丙 → 路径下点名「.img 只要拷这次新写的：丙.img」
TrackField("ExportFolder", @"D:\S2Arm Games\S2Arm 自签导出"); TrackField("ExportGames", 3); TrackField("ExportBytes", (long?)9_800_000_000L);
TrackField("ExportFresh", new List<string> { "丙.img" }); TrackField("ExportReused", 2);
SaveP("win_21b_export_done_copy_only_new", () => Navigate(pack, "phoneupdate"));
// 只新写点的那款：丁的 .img 这次没写 → 再点名「丁 的 .img 没写：手机上已有照常能玩，没有就点那款再导出」
TrackField("ExportUnwritten", new List<string> { "丁" });
SaveP("win_21c_export_done_unwritten", () => Navigate(pack, "phoneupdate"));
TrackField("ExportFolder", ""); TrackField("ExportGames", 0); TrackField("ExportBytes", (long?)null); TrackField("ExportFresh", new List<string>()); TrackField("ExportReused", 0); TrackField("ExportUnwritten", new List<string>());
// 失败示例用真正做不下去的情况（签名失败）；认得出的风险走 drop 态让玩家确认，不当失败（维护者 11-11）
TrackField("Suggestion", "在 Impactor 里重新登录你的 Apple ID，再点一次。");
SetW(pack, "updateMessage", "签名没成功：Impactor 里的 Apple ID 登录已过期。");
SetW(pack, "updateState", "failed");
SaveP("win_22_phone_update_failed", () => Navigate(pack, "phoneupdate"));
SetW(pack, "updateMessage", "这次会让手机上的 1 款游戏暂时不能玩：示例游戏。它们不是在这台电脑上装的，存档和资源包都还在手机上，到原来那台电脑上再点一次「推到 iPhone」就能回来。");
SetW(pack, "updateState", "drop");
SaveP("win_23_phone_update_drop", () => Navigate(pack, "phoneupdate"));

// 封面兜底：游戏包没带图标时用商店横图（banner.jpg），在方形卡片里铺满裁切；没有横图才是 S2Arm 标志
var bannerSrc = Environment.GetEnvironmentVariable("S2A_SNAPSHOT_BANNER") ?? "";
if (File.Exists(bannerSrc))
{
    var folder = Path.Combine(Path.GetTempPath(), "s2a-snap-" + Guid.NewGuid().ToString("N"), "games", "Home Sheep Home");
    Directory.CreateDirectory(folder);
    File.Copy(bannerSrc, Path.Combine(folder, "banner.jpg"));
    var withBanner = MockGame(true) with { Id = "010076000F6B8000", Name = "送小羊回家: 农场末日", Cover = Path.Combine(folder, "banner.jpg"),
        Location = new GameLocation("ok", Path.GetDirectoryName(Path.GetDirectoryName(folder))!, folder, "", true) };
    var noCover = MockGame(true) with { Id = "010024B00F9D0000", Name = "泰拉瑞亚", Cover = "" };
    SetW(pack, "catalog", new LibraryCatalog([withBanner, noCover, MockGame(true)], true, "密钥已就绪"));
    SetW(pack, "connectedFolder", Path.GetDirectoryName(Path.GetDirectoryName(folder))!);
    SaveP("win_09_library_banner_fallback", () => Navigate(pack, "library"));
}

// 出包页「移植方案」（#1272）：选过「仍用原版方案」后看得到、能更改（同 Mac ExportView downstream）
{
    var copy = new Dictionary<string, DownstreamOffer.Copy> {
        ["zh"] = new("选择运行方案", "示例说明", ["示例要点"], "使用推荐方案", "使用原版方案"),
        ["en"] = new("Choose a configuration", "Example", ["Example point"], "Use recommended", "Use original") };
    var dsGame = MockGame(false) with { Downstream = new DownstreamOffer("demo", "Demo Port", true, new Dictionary<string, string>(), copy, "") };
    SetW(pack, "catalog", new LibraryCatalog([dsGame], false, ""));
    SaveP("win_01f_export_downstream_change", () => Navigate(pack, "build"));
    InjectMockGame(pack, ready: false);
}

// 出包页「签名方式」（v0.2.3 开发者证书签名，DevCertPage.cs）：Apple ID 模式；证书模式（团队、到期、包名、设备核对、缺权限黄字）。
//   devCert 直接放作业结果（不跑 devCert 作业）；窗口加高，免得签名一栏落在首屏外
pack.Height = 1300;
InjectMockGame(pack, ready: true);
SetW(pack, "exportIos", true);
SetW(pack, "devCert", JsonDocument.Parse("{\"imported\":false,\"active\":false,\"cert\":null}").RootElement.Clone());
SaveP("win_10_export_sign_appleid", () => Navigate(pack, "build"));
SetW(pack, "devCert", JsonDocument.Parse("""
{"imported":true,"active":true,"cert":{"blocking":[],"warnings":[
 {"id":"devcert_entitlements","text":"描述文件缺下面的权限，小游戏照常，大游戏可能起不来：\n扩展寻址（com.apple.developer.kernel.extended-virtual-addressing）——没有它大游戏可能地址不够、起不来\n可以复制下面这段话发给证书发放人，补开后重新下载描述文件再导入。\n没连着 iPhone：复制后把「<你的设备 UDID>」换成你手机的 UDID。"},
 {"id":"devcert_expiring","text":"证书将于 2026-11-30 到期（还剩 20 天）：到期后 App 打不开，记得提前换新。"}],
 "teamName":"S2Arm Test Co","teamId":"ABCDE12345","certExpires":"2026-11-30","profileExpires":"2027-05-29",
 "bundleId":"io.s2arm.games.abcde12345","wildcard":true,"allDevices":false,"deviceCount":1,"deviceListed":null,
 "missingEntitlements":["com.apple.developer.kernel.extended-virtual-addressing"],
 "requestText":"请提供 Apple Development 开发证书（.p12，附密码）和对应的 iOS App Development 开发描述文件。描述文件要求：用显式 App ID（不要通配 *），包含我的设备 UDID：<你的设备 UDID>，并开启以下权限：调试权限（get-task-allow）、提高内存上限（com.apple.developer.kernel.increased-memory-limit）、扩展寻址（com.apple.developer.kernel.extended-virtual-addressing）。"}}
""").RootElement.Clone());
SaveP("win_11_export_sign_devcert", () => Navigate(pack, "build"));

// 批量出包清单页（BatchPage.cs）：出包前（勾选、Mac 置灰、缺密钥、缺文件、认不出）；出完有成有败（磁盘满暂停、释放空间、重试、装到 iPhone、上次被打断）。
//   清单由 OpenBatch 按游戏库生成；出完的状态直接放 batch.py 形状的结果（不跑 batch.py）
pack.Height = 900;
static LibraryGame BatchMock(string id, string name, string format, IReadOnlyList<string>? missing = null) => MockGame(true) with
{
    Id = id, Name = name, Missing = missing ?? [], Packages = [new("base", id, 0, "v1.0.0", 4_000_000_000L, format)]
};
SetW(pack, "catalog", new LibraryCatalog([
    BatchMock("0100AAAA00000000", "星海远征", "extract"), BatchMock("0100BBBB00000000", "Pixel Garden", "extract"),
    BatchMock("0100CCCC00000000", "雪原小镇", "nsp"), BatchMock("0100DDDD00000000", "Clockwork Isles", "extract", ["本体"]),
    BatchMock("0100EEEE00000000", "Harbor Lights", "extract")], false, ""));
SetW(pack, "connectedFolder", "/tmp/s2arm-library");
SetW(pack, "libraryId", "mock-lib-id");
SetW(pack, "batchState", null);
CallW(pack, "OpenBatch", new List<string> { "0100AAAA00000000", "0100BBBB00000000", "0100CCCC00000000", "0100DDDD00000000", "0100EEEE00000000" },
    new List<ImportOutcome.Unrecognized> { new("readme.txt", "不是 NSP、XCI 或解包好的游戏文件夹。", "input_type_unsupported") });
SaveP("win_12_batch_draft", () => { });
SetW(pack, "batchState", BatchClient.Read(JsonDocument.Parse("""
{"batch":{"schema":1,"location":null,"paused":"disk_full","items":[
 {"gameId":"0100AAAA00000000","name":"星海远征","platforms":["iphone"],"results":{"iphone":{"state":"completed","output":"/tmp/a.ipa"}}},
 {"gameId":"0100BBBB00000000","name":"Pixel Garden","platforms":["iphone"],"results":{"iphone":{"state":"failed","message":"游戏文件读不出来，可能已损坏。","code":"content_unreadable"}}},
 {"gameId":"0100EEEE00000000","name":"Harbor Lights","platforms":["iphone"],"results":{"iphone":{"state":"failed","message":"磁盘空间不足，无法继续出包。","code":"disk_full"}}},
 {"gameId":"0100CCCC00000000","name":"雪原小镇","platforms":["iphone"],"results":{"iphone":{"state":"pending","interrupted":true}}}]},
 "next":null,"summary":{"total":4,"completed":1,"failed":2,"cancelled":0,"pending":1,"done":false}}
""").RootElement.Clone()).State);
SaveP("win_13_batch_done_mixed", () => Navigate(pack, "batch"));

// 入口整理对照：已连库的游戏库页（一款在默认位置、一款在外接盘，后者卡片写「存放在 …」）、设置页（库根有 S2Arm.ipa），窗口拉高看全
SetW(pack, "batchState", null);
pack.Height = 1500;
var tidyLib = Path.Combine(Path.GetTempPath(), "s2a-snap-" + Guid.NewGuid().ToString("N"), "S2Arm");
Directory.CreateDirectory(tidyLib);
File.WriteAllBytes(Path.Combine(tidyLib, "S2Arm.ipa"), []);
var atDefault = MockGame(true) with { Location = new GameLocation("ok", tidyLib, Path.Combine(tidyLib, "games", "Fortune's Weave"), "", true) };
var elsewhere = MockGame(true) with { Id = "010024B00F9D0000", Name = "泰拉瑞亚", IphoneInstalledAt = null,
    Location = new GameLocation("ok", "D:\\S2Arm Games", "D:\\S2Arm Games\\games\\Terraria", "移动硬盘 (D:)", false) };
SetW(pack, "catalog", new LibraryCatalog([atDefault, elsewhere], true, "密钥已就绪"));
SetW(pack, "selectedGame", null);
SetW(pack, "connectedFolder", tidyLib);
SetW(pack, "libraryFolder", tidyLib);
SetW(pack, "libraryId", "mock-lib-id");
SetW(pack, "librarySummary", "library.completed");
SetW(pack, "libraryDiagnostics", "");
SetW(pack, "report", FakeReport(true));
SetW(pack, "summaryKey", "environment.ready");
SaveP("tidy_01_library_connected", () => Navigate(pack, "library"));
// 顶部大图用商店横图（11-14）：S2A_SNAP_BANNER=<一张 1920×1080 的 jpg>（仓库里不放商店图）时拷进这款文件夹，再截一张看清晰度
if (Environment.GetEnvironmentVariable("S2A_SNAP_BANNER") is { Length: > 0 } snapBanner && File.Exists(snapBanner))
{
    Directory.CreateDirectory(atDefault.Location!.Folder);
    var bannerPath = Path.Combine(atDefault.Location.Folder, "banner.jpg");
    File.Copy(snapBanner, bannerPath, true);
    SetW(pack, "catalog", new LibraryCatalog([atDefault with { Banner = bannerPath }, elsewhere], true, "密钥已就绪"));
    SaveP("tidy_01b_library_banner", () => Navigate(pack, "library"));
    SetW(pack, "catalog", new LibraryCatalog([atDefault, elsewhere], true, "密钥已就绪"));
}
// 游戏库选到 exFAT 盘（setup 报 library_bad_format，v0.3.1）：原因下面列出能放游戏库的 NTFS / ReFS 盘和剩余空间，点「放到这里」就建
SetW(pack, "catalog", null); SetW(pack, "connectedFolder", null); SetW(pack, "libraryId", null);
SetW(pack, "librarySummary", "library.failed");
SetW(pack, "libraryDiagnostics", "这个盘是 EXFAT，可以用，但按现有 2 款游戏算，出包会多占约 61 GB；放在 NTFS 格式的盘上更省空间。");
SetW(pack, "libraryFailCode", "library_bad_format"); SetW(pack, "libraryFailFolder", "E:\\S2Arm Library");
SetW(pack, "drivesOverride", (object?)new List<(string Root, string Format, long Free)> { ("C:\\", "NTFS", 212_000_000_000L), ("D:\\", "NTFS", 1_480_000_000_000L) }.OrderByDescending(d => d.Free).ToList());
SetW(pack, "importDialog", new ImportDialogState(ImportKind.Games));   // 连不上库时游戏库页是欢迎页，原因显示在「添加游戏」弹窗里
SetW(pack, "importError", "游戏库没连上（E:\\S2Arm Library）：这个盘是 EXFAT，可以用，但按现有 2 款游戏算，出包会多占约 61 GB；放在 NTFS 格式的盘上更省空间。");
SaveP("win_24_library_bad_format_drives", () => Navigate(pack, "library"));
// 第一次建库、本机不止一块盘：添加游戏弹窗先问放哪块盘（LibraryDrivePicker 选盘模式），各盘剩余空间、系统盘标出来，没有「重试」只有「换一个文件夹」
SetW(pack, "librarySummary", ""); SetW(pack, "libraryDiagnostics", ""); SetW(pack, "importError", "");
SetW(pack, "libraryFailCode", "library_choose_drive"); SetW(pack, "libraryFailFolder", @"C:\Users\player\S2Arm"); SetW(pack, "systemDriveOverride", "C:\\");
SetW(pack, "importDialog", new ImportDialogState(ImportKind.Games));
foreach (var (lang, name) in new[] { ("zh-Hans", "win_24b_library_choose_drive"), ("en", "win_24c_library_choose_drive_en") })
{
    SetW(pack, "language", lang);
    SaveP(name, () => Navigate(pack, "library"));
}
SetW(pack, "language", "zh-Hans"); SetW(pack, "systemDriveOverride", null);
// 数据目录写不进去、本人权限也补不回（11-16 拆自设权限之后的兜底）：不再甩英文异常原文，说哪个文件夹写不进去、可能原因、怎么办；
//   走真代码路径（WelcomePage.NoWrite + LibraryFailed）。Windows 实机异常原文带文件名（request-….json），NoWrite 取它的上级目录；
//   快照在非 Windows 上渲染时 Path.GetDirectoryName 不认反斜杠，所以这里直接给目录形式的原文，画面与 Windows 上取完上级目录一致
SetW(pack, "libraryFailCode", null); SetW(pack, "drivesOverride", null);
foreach (var (lang, name) in new[] { ("zh-Hans", "win_25_library_no_write"), ("en", "win_25b_library_no_write_en") })
{
    var bf = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
    SetW(pack, "language", lang);
    var denied = new UnauthorizedAccessException(@"Access to the path 'D:\S2Arm Data\S2Arm\WindowsLauncher' is denied.");
    var reason = (string)pack.GetType().GetMethod("NoWrite", bf)!.Invoke(pack, [denied, @"D:\S2Arm Data\S2Arm\WindowsLauncher"])!;
    SetW(pack, "importDialog", new ImportDialogState(ImportKind.Games));
    SetW(pack, "importError", (string)pack.GetType().GetMethod("LibraryFailed", bf)!.Invoke(pack, [@"D:\S2Arm Data\DefaultLibrary", reason])!);
    SaveP(name, () => Navigate(pack, "library"));
}
SetW(pack, "language", "zh-Hans");
SetW(pack, "importDialog", null); SetW(pack, "importError", "");
SetW(pack, "libraryFailCode", null); SetW(pack, "drivesOverride", null);
SetW(pack, "catalog", new LibraryCatalog([atDefault, elsewhere], true, "密钥已就绪")); SetW(pack, "connectedFolder", tidyLib); SetW(pack, "libraryId", "mock-lib-id");
SetW(pack, "librarySummary", "library.completed"); SetW(pack, "libraryDiagnostics", "");
pack.Height = 2100;   // 设置页最长：装机方式 + 环境检查 + 运行路径都要在一张图里
SaveP("tidy_02_settings", () => Navigate(pack, "settings"));
// 换游戏库没换成（D-3P26）：失败原因常驻在设置页游戏库一行下面（作业层原句按界面语言翻）
SetW(pack, "libraryFailure", (string)pack.GetType().GetMethod("LibraryFailed", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!
    .Invoke(pack, [@"C:\Program Files\S2Arm\games", "游戏库不能放在 S2Arm 启动器自己的文件夹里，也不能包含它（升级时这个文件夹会整个替换，库会被一起删掉）：换一个文件夹。"])!);
SaveP("tidy_03_settings_library_failed", () => Navigate(pack, "settings"));
SetW(pack, "libraryFailure", "");
// S 级优化（v0.4.0，同 Mac）：封面左上角金色 S、游戏库「S 级优化升级」入口、升级页三态（有可升级 / 都没找到原始程序 / 查找中）
pack.Height = 1100;
SetW(pack, "catalog", new LibraryCatalog([atDefault with { SGrade = true }, elsewhere], true, "密钥已就绪"));
SaveP("sgrade_01_library_badge", () => Navigate(pack, "library"));
static JsonElement SG(string json) => JsonDocument.Parse(json).RootElement.Clone();
SetW(pack, "sgradeOpen", true); SetW(pack, "sgradeScanning", true);
SetW(pack, "sgradeScan", SG("""
{"games":[
 {"id":"0100A","name":"暗黑破坏神 III","program":"saved","total":15,"patches":{"widescreen":9,"fps":1,"gfx":3,"cheat":2},"iphone":true,"off":false},
 {"id":"0100B","name":"空洞骑士：丝之歌","program":"source","total":0,"patches":{},"iphone":true,"off":false},
 {"id":"0100C","name":"哈迪斯","program":"mac","total":4,"patches":{"gfx":3,"cheat":1},"iphone":false,"off":true},
 {"id":"0100D","name":"我的世界","program":"none","total":0,"patches":{},"iphone":true,"off":false}],"found":3,"upgradable":1,"noPatch":1,"missing":1}
"""));
SaveP("sgrade_02_upgrade_page", () => { SetW(pack, "page", "library"); InvokeW(pack, "Render"); });
SetW(pack, "sgradeScan", SG("""{"games":[{"id":"0100D","name":"我的世界","program":"none","total":0,"patches":{},"iphone":true,"off":false}]}"""));
SaveP("sgrade_03_upgrade_nothing", () => InvokeW(pack, "Render"));
SetW(pack, "sgradeScan", null);
SaveP("sgrade_04_upgrade_scanning", () => InvokeW(pack, "Render"));
SetW(pack, "sgradeOpen", false); SetW(pack, "sgradeScanning", false);
// 金手指页（Windows 对齐 Mac，11-14）：一份公开库的、一份玩家导入的（其中一条版本不对）
SetW(pack, "cheatsFor", atDefault);
SetW(pack, "cheatsList", (JsonElement?)SG("""
{"items":[
 {"key":"9B9E.txt/无限血","file":"9B9E182991F3F5B5.txt","name":"无限血","from":"library","repo":"bad1dea/NXCheats","author":"Eiffel2018","ok":true,"fixed":false,"reason":""},
 {"key":"9B9E.txt/秘境进度 1000x","file":"9B9E182991F3F5B5.txt","name":"秘境进度 1000x","from":"library","repo":"bad1dea/NXCheats","author":"Eiffel2018","ok":true,"fixed":true,"reason":""},
 {"key":"mine.txt/金币锁定","file":"mine.txt","name":"金币锁定","from":"player","repo":"","author":"","ok":false,"fixed":false,"reason":"版本不对：金手指是 2607A74F5DF7754C，这份游戏是 9B9E182991F3F5B5。"}],"reason":""}
"""));
SaveP("sgrade_05_cheats_page", () => { SetW(pack, "page", "library"); InvokeW(pack, "Render"); });
SetW(pack, "cheatsFor", null); SetW(pack, "cheatsList", null);
// #1430：出包页 S 级卡「导入 Mod…」进 Mod 页（原先只在游戏库页画、从出包页点没反应）；「返回」回出包页
InjectMockGame(pack, ready: true);
SaveP("win_26_export_mods_page", () => { Navigate(pack, "build"); SetW(pack, "modsFor", MockGame(true));
    SetW(pack, "modsList", (JsonElement?)SG("""[{"name":"高清材质包","bytes":1073741824,"files":120,"enabled":true}]""")); InvokeW(pack, "Render"); });
SaveP("win_26b_export_mods_back", () => { SetW(pack, "modsFor", null); SetW(pack, "modsList", null); InvokeW(pack, "Render"); });
pack.Height = 900;

pack.Close();
Dispatcher.UIThread.RunJobs();
if (heroActionFailures.Count > 0)
{
    foreach (var failure in heroActionFailures) Console.Error.WriteLine("  FAIL hero actions: " + failure);
    Console.WriteLine("✗ 顶部选中游戏按钮断言：不通过 " + heroActionFailures.Count);
    Environment.Exit(1);
}
Console.WriteLine("done.");

// ── 辅助函数 ─────────────────────────────────────────────────────────────
static void InvokeW(object obj, string method) =>
    obj.GetType().GetMethod(method, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public, null, Type.EmptyTypes, null)!.Invoke(obj, null);
static void SetW(object obj, string field, object? value)
{
    var fi = obj.GetType().GetField(field, BindingFlags.NonPublic | BindingFlags.Instance);
    if (fi is not null) { fi.SetValue(obj, value); return; }
    var pi = obj.GetType().GetProperty(field, BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Public);
    // 只读属性（如 busy：剃刀锁一族后改为按作业登记算出）设不了：跳过并提示，别让后面的场景全截不出来
    if (pi is not null) { if (pi.CanWrite) pi.SetValue(obj, value); else Console.Error.WriteLine($"跳过只读属性 {field}（场景里这一项按实际状态显示）"); return; }
    throw new MissingMemberException($"Field/property not found: {field}");
}

static object? GetW(object obj, string field)
    => obj.GetType().GetField(field, BindingFlags.NonPublic | BindingFlags.Instance)?.GetValue(obj);

static void CallW(object target, string method, params object?[] methodArgs)
{
    var m = target.GetType().GetMethod(method, BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new InvalidOperationException("missing method " + method);
    m.Invoke(target, methodArgs);
}

static void Navigate(EnvironmentWindow w, string pg)
    => w.GetType().GetMethod("Navigate", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(w, [pg]);

// 顶部选中游戏按钮断言：当前渲染的游戏库页里，顶部「导出…」要找得到、旧的「推到 iPhone」「导出自签包」不能再有；
//   夹具只有一款游戏：列表行也有一颗「导出…」（同一个自动化 id library.hero.export），所以应出现 2 次
//   有横图时还要确认真画了横图（而不是横图解码失败悄悄退回封面卡片）——HeroBanner 用 DecodeToWidth(…, 1600)，横图宽应为 1600
static void CheckHeroActions(EnvironmentWindow window, string label, List<string> failures, bool expectBanner = false)
{
    var images = window.GetVisualDescendants().OfType<Image>().ToList();
    var ids = window.GetVisualDescendants().OfType<Button>()
        .Select(b => Avalonia.Automation.AutomationProperties.GetAutomationId(b)).ToList();
    var exportCount = ids.Count(id => id == "library.hero.export");
    if (exportCount < 2) failures.Add($"{label}：顶部「导出…」缺失（library.hero.export 出现 {exportCount} 次，含列表行应为 2 次）");
    if (ids.Any(id => id is "library.hero.install" or "library.hero.selfSign")) failures.Add($"{label}：还有旧的「推到 iPhone / 导出自签包」");
    if (expectBanner)
    {
        var widths = images.Select(i => i.Source is Bitmap b ? b.PixelSize.Width : -1).ToList();
        if (!widths.Contains(1600))
            failures.Add($"{label}：没画出横图（当前图宽：{string.Join(",", widths)}；横图 DecodeToWidth(…, 1600) 应为 1600）");
    }
}

static void InjectMockGame(EnvironmentWindow w, bool ready)
{
    var game = MockGame(ready);
    var catalog = MockCatalog(ready);
    SetW(w, "catalog", catalog);
    SetW(w, "selectedGame", game.Id);
    SetW(w, "connectedFolder", "/tmp/s2arm-library");
    SetW(w, "libraryId", "mock-lib-id");
    SetW(w, "librarySummary", "library.completed");
    if (!ready) return;
    var finishedPacks = (Dictionary<string, (string state, string ipa, long? ipaBytes, long? packBytes, double seconds)>)
        GetW(w, "finishedPacks")!;
    finishedPacks[game.Id] = ("succeeded", "/tmp/game.ipa", 1_200_000_000L, 3_400_000_000L, 247);
}

static void InjectMockProgress(EnvironmentWindow w, double fraction, int stepIndex, bool running,
    string state = "running", string? summary = null, long? ipaBytes = null, long? packBytes = null,
    double elapsed = 0, bool exportMode = false)
{
    var game = MockGame(false);
    SetW(w, "packGame", game);
    SetW(w, "packState", state);
    SetW(w, "packRunning", running);
    SetW(w, "packStarted", elapsed > 0 ? DateTime.UtcNow.AddSeconds(-elapsed) : DateTime.UtcNow.AddSeconds(-30));
    if (!running && elapsed > 0) SetW(w, "packSeconds", (object?)elapsed);
    if (summary is not null) SetW(w, "packSummary", summary);
    if (ipaBytes is not null) SetW(w, "packIpaBytes", (object?)ipaBytes);
    if (packBytes is not null) SetW(w, "packPackBytes", (object?)packBytes);
    if (ipaBytes is not null) SetW(w, "packIpa", "/tmp/game.ipa");

    var track = new MilestoneTrack();
    var planJson = """
[{"id":"unpack","label":"解包","purpose":"读出游戏程序和资源，合并更新与 DLC。","expected":120,"source":"history","samples":3,"reuse":false},
 {"id":"prepare","label":"准备资源","purpose":"整理 DLC 和宽屏界面布局，同时在后台把游戏代码改写成苹果设备能直接运行的形式。","expected":80,"source":"history","samples":3,"reuse":false},
 {"id":"ipa","label":"编 App 与签名","purpose":"编出 iPhone / iPad 版程序、签名，并打好要推进手机的资源包。","expected":40,"source":"history","samples":3,"reuse":false}]
""";
    // 装法「导出，自己签名安装」：作业层把最后一步换成自签路的名称与说明（pack/jobs/milestones.py EXPORT_LABELS）
    if (exportMode) planJson = planJson.Replace("编 App 与签名", "整理 iPhone 版程序与资源包")
        .Replace("编出 iPhone / iPad 版程序、签名，并打好要推进手机的资源包。", "编出 iPhone / iPad 版程序、打好资源包；出完接着导出成未签名的 S2Arm.ipa 和这款的 .img，你用自签工具安装。");
    var planEvent = JsonDocument.Parse($$"""{"type":"plan","milestone":null,"milestones":{{planJson}}}""");
    track.Ingest(planEvent.RootElement, 0);
    // 用真实 Clock 基准注入步骤开始时间，避免 StepElapsed 溢出
    double realClock = DateTime.UtcNow.Subtract(DateTime.UnixEpoch).TotalSeconds;
    if (stepIndex >= 0)
    {
        var ids = new[] { "unpack", "prepare", "ipa" };
        var milestoneEvent = JsonDocument.Parse($$"""{"type":"stage","milestone":"{{ids[stepIndex]}}","message":""}""");
        track.Ingest(milestoneEvent.RootElement, realClock - 30);
    }
    if (!running && state == "succeeded") track.Finish(elapsed > 0 ? elapsed : 30);
    SetW(w, "packTrack", track);

    var spaceJson = """{"volumes":[{"role":"library","needBytes":79890000000,"freeBytes":153420000000,"basis":"rule","enough":true}]}""";
    using var spaceDoc = JsonDocument.Parse(spaceJson);
    SetW(w, "packSpace", PackSpace.TryParse(spaceDoc.RootElement));
}

public sealed class SnapshotApp : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        Styles.Add(new FluentTheme());
    }
}
