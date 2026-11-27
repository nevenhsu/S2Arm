// 直连图形层（NVN→Metal，graphics/nvn/gfx）的宿主适配：直连层向宿主要的名字在这里接到 S2Arm 原生层（docs/设计_直连架构.md §12.2 / §12.6）。
// S2Arm 原有、同名直接用的：now_ns / guest_ns / module_of。其余在这里：
//   写跟踪接点（缺页钩子 / 写前回调 / 宿主写前通知，接原生层线 runtime/include/s2a_direct_host.h）、崩溃等待点 / 资源行、
//   切栈 dm_bigstack_enter、出图层 front_metal_layer、每款参数表 s2a_nvn_prof、SDK 版本 dm_sdk_major、回溯、起点时刻、
//   每上屏一帧通知 s2a_frame_presented、模式开关给出的对外入口（s2a_nvn_bootstrap_entry / s2a_direct_frame_count / s2a_direct_suspend）。
#include "host/dm.h"
#include "host/front.h"
#include "../../runtime/include/s2a_gfx_mode.h"
#include "../../runtime/include/s2a_direct_host.h"
#include "../../runtime/include/s2a_docs.h"   // 文档根唯一取法（S2A_DOCS，否则 $HOME/Documents）
#include "../track/s2a_track.h"
#include "gfx/nvn/nvn_count.h"
#include "gfx/nvn/nvn_drvscan.h"      // dm_nvn_driver_api：游戏自带驱动报的 NVN API 版本；dm_nvn_driver_nvrm_rgba8：驱动的 NvColorFormat 编码
#include "gfx/nvn/nvn2.h"            // Switch 2：NVN2 控制段（dsh_miss.inc 不现编）、dm_nvn2_driver_item
#include "gfx/nvn/nvn_hdr_policy.h"  // HDR 上屏策略（PR3a）：挂层块查屏、发布 hdr_snapshot 环境读数（nvn2_hdr_* 入口实现在 nvn2.c）
#include "gfx/nvn/dmsh_chain.h"      // DMSH_CHAIN：外置着色器包要带本链号的库目录才用
#include "gfx/nvn/shader_overlay.h"  // 着色器包外置：文档目录 gal/<程序号>/shaders
#include "gfx/nvn/nvn_present_fit.h"   // pf_viewport：出图层在视图里的摆放与出图层内画面的摆放同一条规则（差 < 1% 铺满）
#include <TargetConditionals.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <signal.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#include <time.h>          // PR4 层状态机 tick 的单调时钟（hdr_now_ms，clock_gettime CLOCK_MONOTONIC）
#include <objc/runtime.h>  // 无头局屏能力探针（headless_edr_note）按名取 NSScreen：objc_getClass / sel_registerName
#include <objc/message.h>  // objc_msgSend（不链 AppKit、无头探针按名调方法；double 返回 arm64 下同一入口）
#include <stdatomic.h>
#include <os/lock.h>
#include <errno.h>
int dm_gfx_session_stopping(void);
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif

uint64_t s2a_npdm_program_id(void);
void s2a_backtrace(uint64_t fp);

// ---------- 每款参数：随包的 <游戏数据>/shaders/direct/game.tsv（host/game_tsv.h）----------
// 游戏数据 = S2A_GAME_DATA；iOS 上第一次查时（重定位期）它还没设（host_main.c 装载后才设），取同一个缺省 Documents/资源包（同 runtime/src/gfxmode.c）。
//   没有文件 / 没写的键按 Switch 真实语义的缺省；档里写了的照用（出库时拷进去的配置档：零配置推导档或手写档）
static s2a_nvn_profile g_prof;
// 程序号不跟每款参数一起缓存：iOS 上第一次查每款参数在重定位期，NPDM 还没读（10-25 奥德赛 iPad 日志：库戳比对报「built for game 0100000000010000, not 0000000000000000」）
const char *s2a_nvn_game_id(void) { static char id[17]; snprintf(id, sizeof id, "%016llX", (unsigned long long)s2a_npdm_program_id()); return id; }

// gd_in：多程序合集切到序号 n 时给 <游戏数据>/programs/<n>（s2a_direct_program_switch）；NULL = 按 S2A_GAME_DATA（首次）
static void prof_load(const char *gd_in) {
        char p[1300], err[200] = ""; const char *gd = gd_in ? gd_in : getenv("S2A_GAME_DATA");
#if TARGET_OS_IPHONE
        char d[1100]; if (!gd || !*gd) { s2a_docs_path(d, sizeof d, "资源包"); gd = d; }
#endif
        if (gd && *gd) snprintf(p, sizeof p, "%s/shaders/direct/game.tsv", gd); else p[0] = 0;

        // 读 game.tsv 原文（用于判定是否显式写了 present_interval）
        char *tsv_text = NULL;
        if (p[0]) {
            FILE *tf = fopen(p, "rb");
            if (tf) {
                fseek(tf, 0, SEEK_END); long sz = ftell(tf); fseek(tf, 0, SEEK_SET);
                if (sz > 0 && sz < 1 << 20) { tsv_text = malloc((size_t)sz + 1); if (tsv_text) { fread(tsv_text, 1, (size_t)sz, tf); tsv_text[sz] = 0; } }
                fclose(tf);
            }
        }

        int got = p[0] && s2a_game_tsv_load(p, &g_prof.t, err, sizeof err);
        if (!p[0]) s2a_game_tsv_parse(NULL, &g_prof.t, err, sizeof err);

        // present_interval 决策（10-28 60 帧零配置）：显式 present_interval 优先；否则 推导 display 且 L3=ok → display；否则 game
        int explicit_set = 0;
        if (tsv_text) {
            char dummy[64]; explicit_set = s2a_game_tsv_find(tsv_text, "present_interval", dummy, sizeof dummy);
            free(tsv_text);
        }
        // 读 L3 判定结果（fps_l3_results.tsv，名单线产出、出库时拷进配置档目录）
        char l3_path[1300]; const char *prog_id = s2a_nvn_game_id(); char l3_result[20] = "";
        if (gd && *gd) { snprintf(l3_path, sizeof l3_path, "%s/shaders/direct/fps_l3_results.tsv", gd);
            FILE *f = fopen(l3_path, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof line, f)) {
                    if (line[0] == '#' || line[0] == '\n') continue;
                    char id[17], r[20];
                    if (sscanf(line, "%16s %19s", id, r) == 2 && !strcasecmp(id, prog_id)) { strlcpy(l3_result, r, sizeof l3_result); break; }
                }
                fclose(f);
            }
        }
        // 调用决策函数
        int decided = s2a_present_decide(explicit_set, g_prof.t.present_display, g_prof.t.present_display_derived, l3_result[0] ? l3_result : NULL);
        int final_display = s2a_present_fps60_gate(decided, getenv("S2A_FPS60"));   // 只用于下面这行日志：缓存的是开关前的决策，开关在读时套（game_profile.h DM_GAME_PRESENT_DISPLAY）
        const char *reason = decided && !final_display ? "60 帧关（S2A_FPS60=0），档里的 display 不生效" : explicit_set ? "显式" :
                             (final_display ? "推导 + L3 ok" :
                              (g_prof.t.present_display_derived ? (l3_result[0] ? "推导但 L3 未确认" : "推导但无 L3 判定") : "缺省"));
        if (explicit_set || g_prof.t.present_display_derived) {
            LOG("直连：present_interval 决策 → %s（依据：%s；显式=%d，推导=%d，L3=%s）",
                final_display ? "display" : "game", reason, explicit_set, g_prof.t.present_display_derived, l3_result[0] ? l3_result : "无");
        }
        g_prof.t.present_display = decided;

        if (*err) LOG("【红】直连：每款参数 %s：%s", p, err);
        const s2a_game_tsv *t = &g_prof.t;
        LOG("直连：每款参数 %s（%s）：app_pool %#llx、条件渲染 %s、CPU 写贴图上传 %s / 脏页 %s、动态贴图句柄 %s、计数器时间戳 %s、绘制遍屏障 %s、上屏 %s、帧保持 %s、按绘制转顶点 %s%s%s",
            DM_GAME_ID, got ? p : "没有随包 game.tsv，全按缺省", (unsigned long long)t->app_pool, t->render_cond ? "开" : "关", t->tex_flush_upload ? "开" : "关",
            t->tex_dirty_upload ? "开" : "关", t->tex_dyn_slots ? "开" : "关", t->gpu_ts_segment ? "GPU 起止" : "整次提交", t->draw_barriers_game ? "按游戏屏障" : "全屏障", t->present_display ? "按显示" : "按游戏",
            t->fps_hold ? "开" : "关", t->vconv_range ? "开" : "关", *t->alpha_from_red ? "、单通道补 alpha 着色器名单有" : "", *t->feedback_snap ? "、反馈快照着色器名单有" : "");
}
const s2a_nvn_profile *s2a_nvn_prof(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ prof_load(NULL); });
    return &g_prof;
}

// ---------- SDK 主版本、游戏自带 NVN / NVN2 驱动报的设备信息：与安卓共用一份（gfx/nvn/nvn_drvinfo.inc）----------
#include "gfx/nvn/nvn_drvinfo.inc"

// ---------- 杂项宿主名字 ----------
uint64_t g_t0_ns;
__attribute__((constructor)) static void t0_init(void) { g_t0_ns = now_ns(); }
void dm_backtrace(uint64_t fp) { s2a_backtrace(fp); }
// ---------- 写跟踪接点（直连层 nvn_dirty.c 的写保护脏页跟踪；直连模式下 S2Arm 写跟踪不保护任何页，只有它一个主人）----------
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { s2a_fault_hook_register(fn); }
static void (*g_wprep)(void *addr, size_t len);
static void wprep_tramp(uint64_t addr, uint64_t len) { g_wprep((void *)(uintptr_t)addr, (size_t)len); }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_wprep = fn; s2a_guest_write_prep_register(wprep_tramp); }
// 远端别名区里的内存池不让 nvn_dirty 写保护（11-14 D-GCMY，见 nvn_dirty.c nvn_dirty_add_range）：判定用原生层的远端表
int s2a_far_has(uint64_t a, uint64_t n) __attribute__((weak));   // runtime/src/far.c（没开远端时恒 0）；弱引用：只链 libnvn 不链原生层的单测没有它（登记见 nvn_dirty.c）
extern int (*nvn_dirty_far_fn)(uint64_t a, uint64_t n);
__attribute__((constructor)) static void dirty_far_init(void) { if (s2a_far_has) nvn_dirty_far_fn = s2a_far_has; }
// 直连层自己要写游戏内存前（贴图回写）：走 S2Arm 宿主写游戏内存的同一个入口，登记的写前回调从那里收到
void dm_guest_write_prepare(void *addr, size_t len) { s2a_track_write_quiet((uint64_t)(uintptr_t)addr, (uint64_t)len); }
// 崩溃等待点 / 资源行：直连层在 Metal 初始化时登记
void crash_set_wait_dump(void (*fn)(void)) { s2a_crash_wait_dump_register(fn); }
static void (*g_res_report)(uint32_t tick, const char *why);
static void res_tramp(void) { g_res_report(0, "s2a"); }
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why)) { g_res_report = fn; s2a_res_report_register(res_tramp); }
// 每上屏一帧（直连层 hle_nvn.c / hle_vk.m / egl.m 出图处调；它那边是确定性时钟的步进，S2Arm 不用）：自动玩家 / 健康表的帧数、标称帧率
static void ap_draws_tick(void);
static void ft_present(void);
static void ft_pace(void);
static int present_fps(void);
void s2a_nominal_fps_set(int fps);   // runtime/src/ap_host.c：自动玩家折算路线帧数的标称帧率
void dm_guest_frame_tick(void) { ap_draws_tick(); s2a_frame_presented(); ft_present();
    ft_pace(); s2a_nominal_fps_set(present_fps()); }

// ---------- 切栈 ----------
// 直连层让建对象 / 提交 / 上屏等做 Metal 活的函数跑在大的宿主栈上（nvn_entry.S nvn_big_entry：x16 = nvn_big_real[i]，跳 dm_bigstack_enter），
// 命令录制（nvnCommandBuffer*，每帧上万次）留在游戏栈上。S2Arm 已有同一件事的机制：切栈桩（runtime/src/xstack.c sw_xstack_wrap）。
// 直连起用时把 nvn_big_real[i] 换成各自的切栈桩，dm_bigstack_enter 就只剩跳过去：桩自己判断在不在游戏栈、切到本线程宿主栈、调实现、切回。
extern void *nvn_big_real[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];
__asm__(".text\n.p2align 2\n.globl _dm_bigstack_enter\n_dm_bigstack_enter:\n    br x16\n");
static void big_wrap(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (nvn_big_real[i]) { nvn_big_real[i] = sw_xstack_wrap(nvn_names[i], nvn_big_real[i]); n++; }
    LOG("直连：%u 个 NVN 函数经 S2Arm 切栈桩跑在宿主栈上（其余命令录制留在游戏栈）", n);
    void nvn_leaf_wrap(void *(*leaf)(const char *, void *)); void *sw_xstack_leaf(const char *name, void *fn);
    nvn_leaf_wrap(sw_xstack_leaf);   // 留在游戏栈上的那些套叶子桩：游戏 sp 不对齐时先对齐再调（runtime/src/xstack.c）
}

// ---------- 兜底现编（§12.3，着色器线 s2a_dsh_compile）----------
// ProgramSetShaders 时（控制段只在这次调用里保证有效）键不在映射表、也不是内置核：按代码区头判阶段，同步调 s2a_dsh_compile 把 <键>_b.metal / .json 写进
// DM_MSL_FALLBACK（Documents/wire/<程序号>/shaders），成功就把「键 → 阶段」补进直连层映射表旁表；建管线时 shader_load 从兜底目录现场建库。只当最后一层保险
const char *s2a_app_version(void) {   // dsh_miss.inc 着色器覆盖计数的 "app"：iOS 是 App 版本；Mac 宿主是命令行程序，没有 → ""
    static char v[32]; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSString *s = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]; snprintf(v, sizeof v, "%s", s ? s.UTF8String : ""); });
    return v;
}
#include "host/dsh_miss.inc"
#include "host/dsh_cold.inc"

// ---------- 出图层 ----------
// 直连模式独占出图层：通用渲染器（graphics/metal）的出图层不建（RenderLoop 不起）。照上游 iOS 前端（DmGameView）：BGRA8、非仅帧缓冲、不透明，
// 按游戏出图的宽高比居中放进根视图：出的是宽图就按 s2a_screen_w():1080，否则 16:9。宽图两种来源：宽屏第二层开着（游戏自己出 屏幕宽 × 1080 的宽图），
//   或直连第三层整套放宽（缺省开、S2A_WIDEN_DIRECT=0 关，且这一档位有整套放宽表 widen_full：核心把目标放宽到屏幕宽、上屏按放宽裁切；10-25 数码宝贝 GL 第三层开着，
//   出图层仍按第二层判成 16:9 挂，放宽的画面又被塞回 16:9）。Mac 宿主无窗口：返回 NULL，直连层按无窗口只计数
void nvn_be_present_out_size(int *w, int *h);   // graphics/nvn/gfx/nvn/nvn_mtl.m：最近一次上屏的源裁切像素宽高（同 gal_present.m nxgal_out_size）
void s2a_direct_out_size(int *w, int *h) { nvn_be_present_out_size(w, h); }
// 出图层只当帧缓冲（framebufferOnly = YES，同小菠萝 front_ios.m 的缺省）：上屏只用渲染遍画进图层贴图（nvn_mtl.m present_encode / present_encode_m4），
// 截图走游戏自己的源图（grab_present），都不读图层贴图；只当帧缓冲时驱动可以用更省的存储与压缩，少一次整屏读写（10-26 暗黑2 能耗对照小菠萝）。
// 要回读图层贴图的诊断才设 DM_NVN_FB_READBACK=1（关掉这项优化）
static int fb_readback(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_FB_READBACK"); v = e && !strcmp(e, "1"); } return v; }
// PR3a 策略抽象（评审稿 v3 §2.2 / §4 PR3a）：屏幕读数发布进 HDR 策略快照——iOS 挂层块（下面直连）与 PR4 起的 s2a_hdr_tick_host（Mac / iOS 的
//   250ms tick 与换屏通知共用）都走这一个入口。发布只刷环境读数（potential_edr / current_headroom / s1_ready / paper_white）——层状态机簿记
//   归 hdr_layer_tick_* 所有，发布不回卷（nvn2.c 侧合并，重探不打回层状态）；
//   s1_ready（G1 双面积烟囱，评审稿 §4 G1 第 4 条）按 G1 在档口径判：potentialEDR>1.0（本机 HDR 屏类在档——内建屏大面积 currentEDR 恒 2.0
//   非污染、U2720QM 开态大面积>1，读数在档；potential≤1.0 的屏恒 0——can 同门，SDR 屏进不了 S1，不虚假宣传）；
//   paper_white 按 v3 定稿填 100（vi_paper_white 改答是 PR5 的活）；S2A_HDR=off（缺省）时无视快照恒 S0 直通 ∧ vi 恒 on=0，行为零变化。
//   PR4 起此入口也被 tick 重探路径高频调：potential / s1_ready 变了才打行（净空波动静默发布——不刷屏），首发布必打（G1 屏态每局在档，§4 G1 第 3 条）
void s2a_hdr_screen_publish(double potential_edr, double current_headroom) {
    int s1_ready = potential_edr > 1.0;
    hdr_snapshot hs = { .potential_edr = potential_edr, .current_headroom = current_headroom, .s1_ready = s1_ready, .paper_white = 100 };
    nvn2_hdr_snapshot_publish(&hs);
    static double p_log = -1; static int sr_log = -1;
    if (potential_edr != p_log || s1_ready != sr_log) {
        p_log = potential_edr; sr_log = s1_ready;
        int sw = nvn2_hdr_switch();
        LOG("直连：HDR 策略快照发布：potentialEDR %.3f / currentEDR %.3f / s1_ready %d（G1 大面积烟囱口径）/ 纸白 100（S2A_HDR=%d：%s）", potential_edr, current_headroom, s1_ready, sw,
            sw == HDR_OFF ? (nvn2_hdr_layer_sw() != HDR_OFF ? "off＋SDR 转 HDR：层按快照进 PQ（vi 照 off 口径回答——S4 只让出图层持 PQ）" : "off，恒 S0 直通") : sw == HDR_AUTO ? "auto，按快照" : "on，强制 HDR 管线（can 不行走 S3 映射）");   // PR-B 终稿：S4 开而玩家 off 时层照进 PQ，off 分支文案如实带出（仍是同一行，无新增日志行）
    }
}
// ---------- PR4：层配置与层状态机接线（评审稿 v3 §2.3 S0/S1 行 + §4 PR4 ①②③④；实现在本文件 front_metal_layer 之后——apply 要取层）----------
//   s2a_hdr_layer_apply_to：把快照层态（hdr_layer_config）应用到一块出图层（S1 = RGB10A2Unorm + kCGColorSpaceITUR_2100_PQ + wantsEDR；
//   S0 = BGRA8Unorm + 显式 sRGB tag——创建点与从 PQ 切回点同一份配置，注意事项①）；s2a_hdr_layer_apply：取当前出图层再应用（tick 路径）。
//   s2a_hdr_preattach：开机预挂（识别 Switch 2 游戏即进层——s2a_platform_ounce 弱符号方向）；s2a_hdr_tick_host：主线程 250ms tick / 换屏通知
//   的重探发布 + 层 tick + 层配置应用（front_mac.m 经 dlsym 调——不链直连层的构建里没有它，缺了就退回 PR3a 的一次性行为）。
static void s2a_hdr_layer_apply_to(CAMetalLayer *l);
static void s2a_hdr_layer_apply(void);   // 取当前出图层再应用（实现在 front_metal_layer 之后；Mac 取用块先调，前置声明在此）
static unsigned long long hdr_now_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;   // 合成时钟的事件源（tick / 预挂——纯逻辑侧合成时钟单测在 t_hdr_layer）
}
#if TARGET_OS_IPHONE
UIView *s2a_ios_root_view(void) __attribute__((weak_import));
// 出图层该挂的视图（ios/extdisplay.m，主线程）：外接显示器接着 = 外接屏窗口的视图，否则根视图。没有这个符号的壳一律根视图
UIView *s2a_ios_present_view(void) __attribute__((weak_import));
static UIView *present_view(void) { return s2a_ios_present_view ? s2a_ios_present_view() : s2a_ios_root_view ? s2a_ios_root_view() : nil; }
int s2a_platform_ounce(void);   // Switch 2 识别接缝（评审稿 §4 PR4 ③）：原生层 svc_mem.c 强定义（NPDM 读数）、nvn2.c 弱缺省 0——PR4 开机预挂按它判
void s2a_hdr_tick_host(double potential_edr, double current_headroom);   // PR4：本文件尾部实现（front_metal_layer 之后——apply 要取层）
static void hdr_tick_ios(void);   // iOS 的 250ms tick / 屏接断通知触发：读出图层所在屏 → s2a_hdr_tick_host（实现随 iOS front_metal_layer 之后）
CGRect s2a_ios_game_area(void) __attribute__((weak_import));
int s2a_ios_game_top(void) __attribute__((weak_import));
// 出图层摆放的范围（#300 统一游戏区域）：挂在手机根视图上时取 App 壳给的游戏区域（分屏等），挂在外接屏窗口的视图上或壳没这个符号 / 区域为空时取视图自身
static CGRect game_rect(UIView *v) {
    if (s2a_ios_game_area && s2a_ios_root_view && v == s2a_ios_root_view()) { CGRect r = s2a_ios_game_area(); if (!CGRectIsEmpty(r)) return r; }
    return v.bounds;
}
void s2a_ios_attach_game_layer(UIView *root, CALayer *game) __attribute__((weak_import));
int s2a_screen_w(void); int s2a_screen_h(void); int s2a_widen_game(void); int s2a_widen_full(void); int s2a_screen_reaspect(void);   // runtime/src/widen.c
int s2a_nvn_reaspect_ok(void);   // graphics/nvn/widen_glue.inc：NVN 直连的宽屏核心在跑（能运行中换屏；Vulkan / GL 层的贴图建时定宽，换不了）
static int out_wide(void) {   // 出的是宽图：第二层，或直连第三层整套放宽（屏幕比 16:9 宽时核心才放宽，s2a_widen_init 回 OFF 时出的仍是 16:9）
    if (s2a_widen_game()) return 1;
    const char *e = getenv("S2A_WIDEN_DIRECT");
    return !(e && !strcmp(e, "0")) && s2a_screen_w() > 1920 && s2a_widen_full();
}
static double out_aspect(void);
static int out_tall(void) {   // 出的是加高图：直连第三层整套放宽、屏幕比 16:9 窄（iPhone Duo 内屏 / iPad；10-26 任意宽高比，核心宽守 1920、高放到 s2a_screen_h()）
    const char *e = getenv("S2A_WIDEN_DIRECT");
    return !(e && !strcmp(e, "0")) && s2a_screen_h() > 1080 && s2a_widen_full();
}
// 出图宽高比：宽图 / 加高图都按 屏宽 ÷ 屏高（宽屏屏高 1080、窄屏屏宽 1920，一个式子两头都对）。
//   11-03 维护者 Duo 智慧的再现 v4：窄屏启用高度点后游戏自出加高图（s2a_widen_game 为真）走进宽图分支，旧式 屏宽 ÷ 1080 给 1.778，1280×900 的画面被塞进 16:9 层、上下大黑、界面落进黑区
double s2a_cpatch_present_aspect(void);   // runtime/src/cpatch.c：自适应画幅社区补丁开着时的当前画幅（游戏按它画世界、出 16:9 尺寸的图，层按它铺满）；没开 0
static double out_aspect(void) { double ca = s2a_cpatch_present_aspect(); if (ca > 0) return ca; return pf_out_aspect(out_wide() || out_tall(), s2a_screen_w(), s2a_screen_h()); }
static CAMetalLayer *g_layer;
static double g_out_a;   // 出图宽高比（起局定，游戏内部按它渲染；NVN 直连换屏时跟着换，见 s2a_direct_relayout）
// 触屏（ios/vpad.m gameRect）按这同一个比例算画面矩形（审计 P1-79：原先触点一律按屏幕宽 : 1080，宽屏没生效 / 窄屏加高时出图是 16:9 / 1920:屏高，手指落点偏）。0 = 还没挂层
double s2a_direct_out_aspect(void) { return g_out_a; }
CGRect s2a_direct_out_rect(void) { return g_layer ? g_layer.frame : CGRectZero; }
// 按根视图当前尺寸摆出图层：出图比例 g_out_a 等比放进视图、居中，可画尺寸跟着变（主线程）。挂层时调一次；之后屏幕尺寸变了
//   （10-27 Duo 合上切外屏：层仍按内屏尺寸挂，外屏只露出左上一块）由 App 壳视图控制器布局回调经 s2a_direct_relayout 再调——
//   NVN 直连按新屏重算屏幕宽 / 高（s2a_screen_reaspect），出图比例跟着换，宽屏核心在下一个渲染遍前按新比例出图（widen_glue.inc wd_reaspect；10-27 维护者：坚持原生满屏）；
//   Vulkan / GL 直连（贴图建时定宽）照旧：游戏仍按起局比例出图，新视图比例差 > 1% 时按出图比例居中留黑、记一行
static void layout_layer(UIView *v, CAMetalLayer *l) {
    CGFloat s = v.window.screen.nativeScale ?: UIScreen.mainScreen.nativeScale; l.contentsScale = s;
    CGRect b = game_rect(v); double a = g_out_a;
    if (b.size.width <= 0 || b.size.height <= 0 || !(a > 0)) return;
    // 等比居中；出图比例与视图比例差 < 1% 时铺满视图（10-29：宽屏开着时出图比例来自取整后的屏幕宽，如 2346 / 1080 = 2.1722 对屏幕 2.1727，
    //   旧写法层比视图窄零点几个点，右侧露一条 ≤ 1 像素的黑缝；同 the reference port present_viewport 的 1% 容差）
    pf_vp f = pf_viewport(b.size.width, b.size.height, a, 1); CGFloat w = f.w, h = f.h;
    // 分屏放宽到整屏宽的上区（iPhone Duo 外屏竖屏，ios/play_layout.m S2APlayLayoutMake 的 top）：画面贴顶，空余留在画面与手柄之间
    if (s2a_ios_game_top && s2a_ios_root_view && v == s2a_ios_root_view() && s2a_ios_game_top()) f.y = 0;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    l.frame = CGRectMake(b.origin.x + f.x, b.origin.y + f.y, w, h);
    // 出图层像素倍数（S2A_OUT_SCALE，缺省 1 = 屏幕原生；帧率能耗线 10-26 维护者要量）：最后上屏这一层按原生全分辨率画，每帧多一次整屏读写；
    //   设 0.75 时可画尺寸按 0.75 倍、图层框不变，交系统合成器放大。代价可能反过来（尺寸不符时合成器要自己缩放），真机 A/B 定生死前缺省不动；换屏重摆沿用同一倍数
    static double k = -1; if (k < 0) { const char *e = getenv("S2A_OUT_SCALE"); k = e ? atof(e) : 1.0; if (!(k >= 0.5 && k <= 1.0)) k = 1.0;
        if (k != 1.0) LOG("直连：出图层可画尺寸按 %.2f 倍（S2A_OUT_SCALE），交系统合成器放大", k); }
    l.drawableSize = CGSizeMake(floor(w * s * k), floor(h * s * k));
    [CATransaction commit];
}
static int g_relayout_due;   // 主线程：后台期间出图层挪回了根视图，欠一次重摆（回前台补，见 front_metal_layer 登记的通知）
void s2a_direct_relayout(void) {   // App 壳视图控制器 viewDidLayoutSubviews、外接屏接上 / 断开（ios/extdisplay.m）调（主线程）；没挂出图层（通用模式 / 还没起）不做
    UIView *v = g_layer ? present_view() : nil; if (!v) return;
    // 外接显示器接上 / 断开：出图层跟着换到外接屏窗口的视图 / 回根视图，照旧垫在最底层；比例与可画尺寸由下面同一条重摆路按新视图算
    // 挪没挪看层在不在这个视图的层子树里：挂层入口（ios/play_reflection.m，Duo 分屏倒影）把出图层放进视图下的容器层，直接父层不是 v.layer；
    //   原先比直接父层，每次重摆都判成「挪了」，把层从容器层里拽回根视图
    int ext = s2a_ios_root_view && v != s2a_ios_root_view(), moved = 1;
    for (CALayer *q = g_layer.superlayer; q; q = q.superlayer) if (q == v.layer) { moved = 0; break; }
    if (moved) {
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        if (s2a_ios_attach_game_layer) s2a_ios_attach_game_layer(v, g_layer);   // 同建层时：经挂层入口（外接屏视图也一样，那边不分屏、没有倒影）
        else { [g_layer removeFromSuperlayer]; [v.layer insertSublayer:g_layer atIndex:0]; }
        [CATransaction commit];
        LOG("直连：出图层挪到%s（App 状态 %ld，视图所在场景状态 %ld）", ext ? "外接屏" : "手机屏",
            (long)UIApplication.sharedApplication.applicationState, (long)v.window.windowScene.activationState);
    }
    // 在后台不认布局变化（11-02 维护者 Duo 丝之歌：进后台后 iOS 截多任务快照，临时把视图摆成 466×678 点的竖条，宿主当成运行中换屏 1350 → 1318 → 1350、
    //   宽屏核心按新比例重建加高目标；回前台那一刻多一次重建）：后台不出图也不提交，快照尺寸不是玩家的屏幕。回前台视图恢复原尺寸，出图层本来没动
    //   只管根视图：外接屏窗口的视图不被快照改尺寸。刚挪回根视图的层还按外接屏摆着：根视图所在场景在前台就当场重摆，否则记欠一次、回前台补
    //   （pf_relayout_wait，11-10 iPhone 18 Pro 局中拔显示器：外接屏场景断开回调里 App 状态读到 2 = 后台，根视图所在场景状态 0 = 前台活跃、没有进后台通知）
    UIWindowScene *vs = v.window.windowScene;
    int app_bg = UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
    int scene_fg = vs && (vs.activationState == UISceneActivationStateForegroundActive || vs.activationState == UISceneActivationStateForegroundInactive);
    if (pf_relayout_wait(ext, app_bg, moved, scene_fg)) { if (moved) g_relayout_due = 1; return; }
    CGSize z = game_rect(v).size; if (z.width <= 0 || z.height <= 0) return;
    double a0 = g_out_a; int re = s2a_nvn_reaspect_ok() && s2a_screen_reaspect(); if (re) g_out_a = out_aspect();   // 先换出图比例再摆层
    CGSize d0 = g_layer.drawableSize; layout_layer(v, g_layer); CGSize d1 = g_layer.drawableSize;
    if (d0.width == d1.width && d0.height == d1.height && !re && !moved) return;
    double va = z.width / z.height;
    LOG("直连：屏幕尺寸变了，出图层重摆为 %.0fx%.0f 像素（视图 %.0fx%.0f 点，比例 %.3f；出图比例 %.3f → %.3f%s）", d1.width, d1.height, z.width, z.height, va, a0, g_out_a,
        re ? "，宽屏核心下一个渲染遍按新比例出图" : "");
    if (!re && fabs(va / g_out_a - 1) > 0.01) LOG("【黄】直连：可用区域比例 %.3f 与出图比例 %.3f 不同，按实际出图比例居中；固定尺寸的路径须重新启动游戏后应用新比例", va, g_out_a);
}
unsigned s2a_session_generation(void) __attribute__((weak));   // runtime/src/session.c：会话代号（同进程「立刻重启」新一局加一）
static _Atomic unsigned g_layer_gen;   // 出图比例按哪一局算的（会话代号）
// 同进程新一局重算出图比例（#1169）：游戏内「立刻重启」重读设置后，满屏 / 补丁画幅可能变了，宽屏核心按新屏宽高出图；
//   出图层却只在进程首次建层时算过一次比例，新画幅被装进上一局的层（iPad 横屏开满屏后重启：1.532 的画面塞进 16:9 层、四边留黑，冷启动才对）。
//   渲染线程每次取层比一次代号，换局了交主线程重算比例并按同一条重摆路摆层（后台等回前台补，同布局回调）
static void layer_session_check(void) {
    if (!g_layer || !s2a_session_generation) return;
    unsigned g = s2a_session_generation(), o = atomic_load(&g_layer_gen);
    if (g == o || !atomic_compare_exchange_strong(&g_layer_gen, &o, g)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        double a0 = g_out_a, a1 = out_aspect(); if (!(a1 > 0) || a1 == a0) return;
        g_out_a = a1; CGSize d0 = g_layer.drawableSize; s2a_direct_relayout(); CGSize d1 = g_layer.drawableSize;
        if (d0.width == d1.width && d0.height == d1.height) { g_relayout_due = 1; return; }   // 没摆（在后台）：回前台补（front_metal_layer 登记的通知）
        LOG("直连：新一局出图比例 %.3f → %.3f，出图层重摆为 %.0fx%.0f 像素", a0, a1, g_layer.drawableSize.width, g_layer.drawableSize.height);
    });
}
void *front_metal_layer(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (s2a_session_generation) atomic_store(&g_layer_gen, s2a_session_generation());
        if (!s2a_ios_root_view) { LOG("【红】直连：没有 App 壳根视图（s2a_ios_root_view），无出图层"); return; }
        void (^mk)(void) = ^{
            UIView *v = present_view(); if (!v) return;
            CAMetalLayer *l = [CAMetalLayer layer];
            l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = !fb_readback(); l.opaque = YES;
            g_out_a = out_aspect();   // 10-24 真机：宽屏开着仍按 16:9 挂，2.17 的画面被塞进 16:9 区域、左右露黑；窄屏同理按 1920:屏高
            layout_layer(v, l);
            // 挂在最底层（11-02）：原先 addSublayer 压在根视图所有子视图之上，加载页 / 提示页（没设 zPosition）全被黑色出图层盖住——
            //   怪猎测试设备1 冷首启预热 64 秒，屏上只有手柄（手柄与开发浮层靠 zPosition 压过它），玩家对着黑屏点中了看不见的「先开始玩」
            if (s2a_ios_attach_game_layer) s2a_ios_attach_game_layer(v,l);
            else [v.layer insertSublayer:l atIndex:0];
            g_layer = l;
            // Metal 性能浮层（#153）：MTL_HUD_ENABLED 只在进程启动、系统建 Metal 设备时读一次；App 设置里的开关是进程起来后才落成环境变量（ios/settings.m），
            //   已经晚了，只有启动前从外面传入才显示。iOS 16 起可按层在运行中打开：开关开着（环境变量来自设置或外面传入都算）就对出图层打开
            { const char *hud = getenv("MTL_HUD_ENABLED");
              if (hud && !strcmp(hud, "1")) { if (@available(iOS 16.0, *)) { l.developerHUDProperties = @{@"mode": @"default"}; LOG("直连：Metal 性能浮层按出图层打开（MTL_HUD_ENABLED=1）"); } } }
            // PR1 探针（HDR 全链路评审稿 v2）：挂层屏幕 EDR 能力一次性读数（M0 跑局记录屏幕配置用；能力动态重探是 PR4 的活）。EDR headroom iOS 16 起；
            //   排查指引只写在 Mac 侧（front_mac.m screen_edr_note）——iOS 的设置路径与那行文案不同，这里只报读数
            { UIScreen *sc = v.window.screen ?: UIScreen.mainScreen;
              float pe = 0, ce = 0;
              if (@available(iOS 16.0, *)) { pe = sc.potentialEDRHeadroom; ce = sc.currentEDRHeadroom; }
              LOG("直连：出图层所在屏 EDR 探针：potentialEDR %.3f / currentEDR %.3f（倍数），色域 %s", pe, ce, sc.traitCollection.displayGamut == UIDisplayGamutP3 ? "P3" : "sRGB");
              // PR3a 策略抽象（评审稿 v3 §2.2 / §4 PR3a）：同一屏读数发布进 HDR 策略快照——iOS 在此直连发布；PR4 起换屏 / 拨 HDR 开关后的
              //   重探重发布由 s2a_hdr_tick_host 的 250ms tick 接管（下方 hdr_tick_ios，Mac 侧同一发布在 front_mac.m 的 tick 经它走）
              s2a_hdr_screen_publish(pe, ce);
              // PR4 开机预挂（评审稿 §4 PR4 ③）：识别 Switch 2 游戏即进层（赶在首查询前——Bananza 首查询即定窗口，nxnative dm_hdr_boot 形态）；
              //   层簿记进快照后立即应用 PQ 配置（赶在首 present 前）——这里直调 apply_to(l)（mk 在 front_metal_layer 的 dispatch_once 里，
              //   不能走会再调 front_metal_layer 的包装，重入死锁）
              if (s2a_platform_ounce()) nvn2_hdr_preattach(pe > 1.0, hdr_now_ms());
              s2a_hdr_layer_apply_to(l); }
            if (v != s2a_ios_root_view()) LOG("直连：出图层挂在外接屏（下一行的「根视图」尺寸是外接屏窗口的视图）");
            [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) { (void)n; if (g_relayout_due) { g_relayout_due = 0; s2a_direct_relayout(); } }];   // 补后台期间欠的重摆
            // PR4 层状态机接线（评审稿 §4 PR4 ②，iOS 等效通知集）：主线程 250ms tick 加进 NSRunLoopCommonModes（触屏 / 菜单不停）轮询屏读数——
            //   headroom（currentEDR）无专用通知，轮询即「headroom 变化也 post」的等效；外接屏接断通知 = 屏消失 / 恢复分支的触发器
            //   （回退主屏 / 置无效 → can=0 持稳后排空切回，恢复重探重发布——不对已消失屏做 present 假设）
            NSTimer *hdr_t = [NSTimer timerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) { (void)t; hdr_tick_ios(); }];
            [[NSRunLoop mainRunLoop] addTimer:hdr_t forMode:NSRunLoopCommonModes];
            [NSNotificationCenter.defaultCenter addObserverForName:UIScreenDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) { (void)n; hdr_tick_ios(); }];
            [NSNotificationCenter.defaultCenter addObserverForName:UIScreenDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) { (void)n; hdr_tick_ios(); }];
        };
        if (NSThread.isMainThread) mk(); else dispatch_sync(dispatch_get_main_queue(), mk);
        if (g_layer) LOG("直连：出图层已挂到根视图 %.0fx%.0f 像素（宽高比 %.3f，居中；宽屏第二层%s）", g_layer.drawableSize.width, g_layer.drawableSize.height,
                         g_layer.drawableSize.width / g_layer.drawableSize.height, s2a_widen_game() ? "开" : out_wide() ? "关、直连第三层整套放宽" : out_tall() ? "关、直连第三层上下加高" : "关");
        else LOG("【红】直连：根视图还没建好，没挂上出图层");
    });
    layer_session_check();
    return (__bridge void *)g_layer;
}
// PR4 层状态机的 iOS tick 触发（评审稿 §4 PR4 ②④）：读出图层所在屏的 EDR 读数交 s2a_hdr_tick_host（重探发布 + 层 tick + 层配置应用，实现在文件尾）。
//   快照屏消失（视图不在窗口上 / 外接屏断开——UIScreenDidDisconnectNotification）→ 回退主屏（同 PR1 探针的兜底口径）；主屏也不在 → 读数 0
//   （can=0 → 500ms 持稳后排空切回 SDR——不对已消失屏做 present 假设）；恢复后下一 tick 重探重发布（层簿记不回卷，排空语义不受扰动）。
static void hdr_tick_ios(void) {
    UIView *v = g_layer ? present_view() : nil;
    UIScreen *sc = v.window.screen ?: UIScreen.mainScreen;
    double pe = 0, ce = 0;
    if (@available(iOS 16.0, *)) { if (sc) { pe = sc.potentialEDRHeadroom; ce = sc.currentEDRHeadroom; } }
    s2a_hdr_tick_host(pe, ce);
}
#else
// Mac：启动器起的游戏窗口（macos/front_mac.m）有出图层就画上去；无头局（mac_run / 门禁）没有窗口，返回 NULL，直连层按无窗口只计数、照常服务截图
//   跨层可选符号按名字取（dlsym 一次，同 3e21c138；弱引用在 dylib 单独链接时找不到符号）
//   层的 device / 像素格式同 iOS 分支（通用模式由 NxGal 初始化时设，直连不起 NxGal：不设则 layer.residencySet 为空，直连层上屏登记常驻集抛异常）
//   多线程取层（出图线程、游戏线程 nvn_be_has_window / nvn_be_display_hz、主线程 HDR tick）：配置块在锁里只进一次。
//   已配置的层号原子发布，快路径只比原子号、不读强引用 set——两个线程同时给静态强引用赋值时，系统 objc_storeStrong 在 retain 期间
//   往变量里临时写哨兵 0x400000000000bad0，另一线程把它当旧值 release 即崩（Mac 0.6.9 出图线程首帧取层）。
//   配置块里 s2a_hdr_layer_apply 会重入本函数：层号先于它发布，重入走快路径、不再拿锁
void *front_metal_layer(void) {
    static void *(*f)(void); static dispatch_once_t once; static CAMetalLayer *set; static _Atomic(void *) set_id; static os_unfair_lock mu = OS_UNFAIR_LOCK_INIT;
    dispatch_once(&once, ^{ f = (void *(*)(void))dlsym(RTLD_DEFAULT, "s2a_mac_front_layer"); });
    CAMetalLayer *l = f ? (__bridge CAMetalLayer *)f() : nil;
    if (l && (__bridge void *)l != atomic_load_explicit(&set_id, memory_order_acquire)) {
        os_unfair_lock_lock(&mu);
        if ((__bridge void *)l != atomic_load_explicit(&set_id, memory_order_relaxed)) {
            l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = !fb_readback(); set = l;
            atomic_store_explicit(&set_id, (__bridge void *)l, memory_order_release);
            LOG("直连：出图层取 Mac 游戏窗口的层 %.0fx%.0f 像素", l.drawableSize.width, l.drawableSize.height);
            s2a_hdr_layer_apply();   // PR4：取用即按快照层态应用配置（§4 PR4 ①——创建点；预挂过的 Switch 2 游戏直接 PQ，否则显式 sRGB tag 的 S0）。
                                     // 层号已先发布：包装内再调 front_metal_layer 走快路径、不重入本块。首个取用可能在 present 线程（present_now 首帧）——
                                     // 与上面同块的 l.device / l.pixelFormat 直设同一口径（先于任何 drawable 的属性配置），applied 态用原子防并发重算
        }
        os_unfair_lock_unlock(&mu);
    }
    return (__bridge void *)l;
}
#endif

// ---------- PR4：层配置应用与 tick 接线（评审稿 v3 §2.3 S0/S1 行 + 注意事项① + §4 PR4 ①②③④；声明在 s2a_hdr_screen_publish 后）----------
//   把快照层态（hdr_layer_config：S0 = BGRA8Unorm + 显式 sRGB tag；S1 = RGB10A2Unorm + kCGColorSpaceITUR_2100_PQ + wantsExtendedDynamicRangeContent；
//   CAEDRMetadata 默认不挂——两态都不写它）应用到一块出图层。注意事项①（层复用路径）：从 PQ 层切回的层保留 PQ 色彩空间——S0 必须显式重设 sRGB tag
//   （设 NULL 则无色彩匹配、字节直进屏幕原生 primaries，P3 屏过饱和）；创建点（iOS mk() / Mac 取用）与切回点（tick 后）走同一份配置——策略单源。
//   调用方口径：iOS mk() 主线程、Mac 取用首帧可能 present 线程（先于任何 drawable 的属性直设，同块内 l.device / pixelFormat 先例）；
//   applied 态原子持有，跨线程并发 apply 最坏重复一次幂等赋值。色彩空间对象进程内各建一次常驻（层属性 retain，量级 = 转换次数）。
static void s2a_hdr_layer_apply_to(CAMetalLayer *l) {
    if (!l || nvn2_hdr_layer_sw() == HDR_OFF) return;   // 缺省（玩家 off 且 S4 关）不碰出图层（不设色彩空间标签）：层属性与改前逐项相同。PR-B 终稿：门读层视角开关——S4 开而玩家 off 时层配置照跑（出图层持 PQ 是 S4 的生效面；vi 答询不经这里）
    hdr_snapshot s; nvn2_hdr_snapshot_probe(&s);
    hdr_layer_cfg c = hdr_layer_config(s.layer_pq);
    static _Atomic int applied = -1;
    int was = atomic_exchange(&applied, (int)c.pf);
    if (was == (int)c.pf) return;
    static CGColorSpaceRef cs_pq, cs_srgb;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cs_pq = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);    // BT.2100 PQ（§2.3 S1 行）
        cs_srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);          // 显式 sRGB tag（§2.3 注意事项①）
    });
    if (c.pf == HDR_LPF_PQ10) {
        l.pixelFormat = MTLPixelFormatRGB10A2Unorm;
        l.colorspace = cs_pq;
#if !TARGET_OS_TV
        l.wantsExtendedDynamicRangeContent = YES;   // tvOS SDK 没有这个属性
#endif
        LOG("直连：出图层切 PQ（RGB10A2Unorm + kCGColorSpaceITUR_2100_PQ + wantsExtendedDynamicRangeContent；CAEDRMetadata 默认不挂，PR8+ 实验项）");
    } else {
        l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.colorspace = cs_srgb;
#if !TARGET_OS_TV
        l.wantsExtendedDynamicRangeContent = NO;
#endif
        if (was >= 0) LOG("直连：出图层切回 SDR（BGRA8Unorm + 显式 sRGB tag——从 PQ 层切回的层保留 PQ 色彩空间，必须显式重设，评审稿 §2.3 注意事项①）");
    }
}
#if TARGET_OS_IPHONE
static void s2a_hdr_layer_apply(void) { s2a_hdr_layer_apply_to(g_layer); }   // 直接用已建的层：不经 front_metal_layer（它带换局重摆检查 layer_session_check，归渲染线程取层时做）
#else
static void s2a_hdr_layer_apply(void) { s2a_hdr_layer_apply_to((__bridge CAMetalLayer *)front_metal_layer()); }
#endif
// PR4 开机预挂（评审稿 §4 PR4 ③）：识别 Switch 2 游戏即进层（赶在首查询前——Bananza 首查询即定窗口，nxnative dm_hdr_boot 形态）。
//   识别接缝按 s2a_platform_ounce 弱符号方向（nvn2.c 同款：原生层 svc_mem.c 强定义、单测缺省 0）；进层簿记在 nvn2_hdr_preattach（存储侧，
//   SDR 屏 / s1_ready 未过 / off / 已在层内各自让位）。层配置的应用由调用方做（iOS mk 直调 apply_to；Mac 首个 tick 的 apply / 直连取用补）。
void s2a_hdr_preattach(double potential_edr) {
    if (!s2a_platform_ounce()) return;
    nvn2_hdr_preattach(potential_edr > 1.0, hdr_now_ms());
}
// PR4 层状态机接线（评审稿 §4 PR4 ②④）：主线程 250ms tick（NSRunLoopCommonModes——拖窗 / 开菜单 / 触屏不停）与换屏通知共用的一个入口——
//   重探发布（读数变了才发，发布不回卷层簿记）+ 首探开机预挂 + 层 tick（can 滞回 500ms、进层即时）+ 层配置应用（进层即 PQ、排空完成切回
//   S0 + 显式 sRGB tag）。快照屏消失（调用方读数给 0）→ can=0 持稳后排空切回——不对已消失屏做 present 假设；恢复后下一 tick 重探重发布。
//   headroom 波动不切层（快照环境读数照刷，peak 步进吸收——§2.3 第 5 条）。front_mac.m 经 dlsym 调（不链直连层的构建里没有它，缺了就
//   退回 PR3a 的一次性行为）；iOS 的 hdr_tick_ios 直调。
void s2a_hdr_tick_host(double potential_edr, double current_headroom) {
    static double p_last = -1.0, c_last = -1.0;
    if (potential_edr != p_last || current_headroom != c_last) {   // 读数变了（换屏 / HDR 开关 / 净空波动 / 屏消失恢复）才重发布
        p_last = potential_edr; c_last = current_headroom;
        s2a_hdr_screen_publish(potential_edr, current_headroom);
    }
    if (nvn2_hdr_layer_sw() == HDR_OFF) return;   // 缺省（玩家 off 且 S4 关）：层状态机不动、不取层（Mac 通用模式的窗口层也不碰）。PR-B 终稿：门读层视角开关——S4 开而玩家 off 时层状态机照跑（出图层持 PQ；vi 答询照玩家开关，不在此层）
    static _Atomic int pre;
    if (!atomic_exchange(&pre, 1)) s2a_hdr_preattach(potential_edr);   // 首探即预挂（赶在首查询前）；非 Switch 2 / 层视角 off / SDR 屏在预挂链路里各自让位（S4 开而玩家 off 也预挂——S4 只让出图层持 PQ）
    nvn2_hdr_tick(nvn2_hdr_layer_sw(), potential_edr > 1.0, hdr_now_ms());   // raw can = potentialEDR>1.0（调用方口径）；sw = 层视角开关（玩家设置优先，S4 只在玩家没开时补 auto）
    s2a_hdr_layer_apply();
}

// PR1 探针（HDR 全链路评审稿 v2 §4 PR1 ③）Mac 无头局（mac_run / 门禁跑局，Mac 分支注释：没有前端窗口）补的一条（wave-6 G1 日志通道修复，随 PR4 波次携带）：
//   窗口版 probe 在 macos/front_mac.m screen_edr_note（窗口所在屏），但那份文件只在 S2A_MAC_FRONTEND=1 的前端构建编进宿主
//   （launchers/macos/build.mk），跑局的无头构建里没有它——11-19 G1：第四波只把窗口版从 fprintf(stderr) 改走 LOG，
//   通道对了、代码没编进跑局宿主，host.log 里探针行一行没有。这里在直连开局（direct_paths）按主屏读一次：
//   一次性、只读不改变判定（PR4 起换屏 / 拨 HDR 开关后的重探重发布由 s2a_hdr_tick_host 的 250ms tick 接管）。启动器起的前端窗口局
//   （s2a_mac_front_layer 在）让位给 front_mac.m 的窗口版（同一行文）；iOS 在挂层处已打自己的读数行（UIScreen，文案不同），这里不掺和。
//   G1 日志通道（评审稿 v3 §4 G1 第 3 条）：走 LOG（→ s2a_log_line → s2a_log_out(g_s2a_logfd) = os.c 构造期 dup 的原始
//   stderr = run_mac.sh 重定向的 host.log；不走 fprintf(stderr)——C# 侧 StdErrAdapter dup2 管道接管 fd 2 后会掉进管道）。
//   无头宿主不链 AppKit（AppKit 系框架在 S2A_MAC_FRONTEND=1 的块里，launchers/macos/build.mk），屏能力读数只有 AppKit
//   NSScreen 一家（NSScreen.h），按名字现取：dlopen 一次 + objc 运行时（同上面 Mac 分支 s2a_mac_front_layer 的按名取法）。
//   取不到（AppKit 拉不起来 / 主屏为空）打红行如实说，不硬凑读数。
#if !TARGET_OS_IPHONE
static void headless_edr_note(void) {
    static int noted; if (noted) return; noted = 1;
    if (dlsym(RTLD_DEFAULT, "s2a_mac_front_layer")) return;   // 前端窗口局：front_mac.m 窗口版探针已打同一行文，让位
    void *ak = dlopen("/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", RTLD_LAZY | RTLD_LOCAL);
    Class nsc = ak ? objc_getClass("NSScreen") : NULL;   // 无头局没有窗口，按主屏读（同 front_mac.m 窗口还没上屏时的兜底）
    id sc = nsc ? ((id (*)(Class, SEL))objc_msgSend)((id)nsc, sel_registerName("mainScreen")) : NULL;
    if (!sc) { LOG("【红】直连：无头局屏能力探针取不到主屏（AppKit %s），屏态这局没在档——换有屏环境重跑", ak ? "在、主屏为空" : "拉不起来"); return; }
    id cs = ((id (*)(id, SEL))objc_msgSend)(sc, sel_registerName("colorSpace"));
    CGColorSpaceRef cg = cs ? ((CGColorSpaceRef (*)(id, SEL))objc_msgSend)(cs, sel_registerName("CGColorSpace")) : NULL;
    CFStringRef raw = cg ? CGColorSpaceGetName(cg) : NULL;
    char rawname[96] = "-"; if (raw) CFStringGetCString(raw, rawname, sizeof rawname, kCFStringEncodingUTF8);
    id loc = cs ? ((id (*)(id, SEL))objc_msgSend)(cs, sel_registerName("localizedName")) : NULL;
    const char *loc8 = loc ? ((const char *(*)(id, SEL))objc_msgSend)(loc, sel_registerName("UTF8String")) : NULL;
    double potential = ((double (*)(id, SEL))objc_msgSend)(sc, sel_registerName("maximumPotentialExtendedDynamicRangeColorComponentValue"));
    LOG("窗口所在屏能力探针（无头局无窗口，按主屏读）：potentialEDR %.3f / currentEDR %.3f / referenceEDR %.3f（倍数），P3 %s（%s），backing %.2f，色彩空间 %s",
        potential,
        ((double (*)(id, SEL))objc_msgSend)(sc, sel_registerName("maximumExtendedDynamicRangeColorComponentValue")),
        ((double (*)(id, SEL))objc_msgSend)(sc, sel_registerName("maximumReferenceExtendedDynamicRangeColorComponentValue")),
        strstr(rawname, "P3") ? "是" : "否", rawname,
        ((double (*)(id, SEL))objc_msgSend)(sc, sel_registerName("backingScaleFactor")),
        loc8 ?: "-");
    if (potential <= 1.0)   // 评审稿 §2.5（同 front_mac.m）：判成 SDR 但屏其实支持 HDR 的错配，日志一行人话指引、给下一步
        LOG("当前屏按 SDR 处理（potentialEDR ≤ 1.0）。若该屏支持 HDR：查①系统设置→显示器→HDR 开关（常见成因）②显示器 OSD 的 HDR/色域档 ③线缆/扩展坞（直连 USB-C DP Alt、HBR2+）④EDID 是否宣告 HDR（本机实测先例：宣告 PQ 而非 HLG）");
}
#endif

// ---------- 对外入口（原生层 wire.c / App 壳 / 服务层调）----------
void *h_nvnBootstrapLoader(const char *name);   // graphics/nvn/gfx/nvn/hle_nvn.c（host/hle.h 的 HLE_FORCE 让它外部可见）
// 着色器库 / 表 / 管线清单的位置：游戏第一次调 nvnBootstrapLoader 时定（不是重定位时：iOS 上 S2A_GAME_DATA 由 host_main.c 在装载之后才设缺省，
//   重定位时读到的是空，直连层退回自己的缺省路径找不到映射表直接 FATAL——10-24 真机首局）。直连层读这些变量在 Metal 初始化时，晚于这里
static char g_dp_gd[1100]; static int g_dp_from_gd;   // 首局着色器库路径是不是由游戏数据目录推出来的、推的根（多程序合集切换按它换到 programs/<n>）
static void direct_paths(void) {
    char p[1300]; const char *gd = getenv("S2A_GAME_DATA");
#if !TARGET_OS_IPHONE
    headless_edr_note();   // PR1/G1 屏能力探针（见上）：Mac 无头局直连开局按主屏读一行；有前端窗口的启动器局让位给 front_mac.m 窗口版
#endif
    // 外置着色器包（shader_overlay.h）：没显式给 DM_MSL_DIR / DM_NVN_SHMAP 时，文档目录 gal/<程序号>/shaders/direct 有本链号的库就用它（更新着色器包不必重签），否则照旧包内
    if (!getenv("DM_MSL_DIR") && !getenv("DM_NVN_SHMAP")) {
        char ov[1100]; int r = s2a_shader_overlay_direct(ov, sizeof ov, s2a_npdm_program_id(), DMSH_CHAIN);
        if (r > 0) { setenv("DM_MSL_DIR", ov, 1); snprintf(p, sizeof p, "%s/shader_map.tsv", ov); setenv("DM_NVN_SHMAP", p, 1); LOG("直连：用文档目录里的外置着色器包 %s", ov); }
        else if (r < 0) LOG("直连：文档目录里的外置着色器包 %s 没有本链号 b-%s 的库或映射表，用包内的", ov, DMSH_CHAIN);
    }
    int had = getenv("DM_MSL_DIR") || getenv("DM_NVN_SHMAP");   // 显式给的（或外置着色器包）：多程序切换不动它（s2a_direct_program_switch 报红）
    if (gd && *gd && !had) { g_dp_from_gd = 1; strlcpy(g_dp_gd, gd, sizeof g_dp_gd); }
    if (gd && *gd) {   // 直连着色器库随资源包走（同通用着色器，S2A_GAME_DATA/shaders/；§12.3）：库目录 = <DM_MSL_DIR>/b-<链版本>，c1_full / cb_extent / pso_manifest 在映射表旁边
        snprintf(p, sizeof p, "%s/shaders/direct", gd); setenv("DM_MSL_DIR", p, 0);
        snprintf(p, sizeof p, "%s/shaders/direct/shader_map.tsv", gd); setenv("DM_NVN_SHMAP", p, 0);
    } else if (!getenv("DM_MSL_DIR") || !getenv("DM_NVN_SHMAP")) LOG("【红】直连：没有 S2A_GAME_DATA，也没给 DM_MSL_DIR / DM_NVN_SHMAP，着色器库找不到");
    // 录像（S2A_DIRECT_REC=<起始帧>:<帧数>，缺省关）：测试设备 devicectl 不支持录屏，改由 App 自己按帧存图——直连层的连续截图 DM_NVN_SHOT
    //   （上屏同一段编码），存到 <文档目录>/rec/f_<帧号>.png，执行器 --pull=rec 拉回后合成视频。每帧要等 GPU 并写盘，录像局帧率会降，功耗局不开
    { const char *r = getenv("S2A_DIRECT_REC"), *ds = getenv("DM_NVN_SHOT"); long a = 0, n = 0;
      if (r && *r && !(ds && *ds) && sscanf(r, "%ld:%ld", &a, &n) == 2 && a >= 0 && n > 0 && n <= 4096) {
          char dir[1100]; s2a_docs_path(dir, sizeof dir, "rec"); mkdir(dir, 0755);   // 文档根：Mac 局 S2A_DOCS = 局目录，iOS = $HOME/Documents
          size_t cap = (size_t)n * 12 + sizeof dir + 32; char *v = malloc(cap), *q = v;
          if (v) {
              for (long i = 0; i < n; i++) q += snprintf(q, cap - (size_t)(q - v), "%s%ld", i ? "," : "", a + i);
              snprintf(q, cap - (size_t)(q - v), ":%s/f_%%d.png", dir);
              setenv("DM_NVN_SHOT", v, 1); free(v);
              LOG("直连：录像存帧 第 %ld 帧起 %ld 帧 → %s", a, n, dir);
          }
      } else if (r && *r) LOG("【红】直连：S2A_DIRECT_REC=%s 看不懂（要 <起始帧>:<帧数>，帧数 1..4096），不录", r);
    }
    // 诊断截图（DM_NVN_SHOT）缺省走上屏同一段编码（带游戏的裁剪框）：截到的就是玩家看到的画面；不设时直连层原样拷整张窗口贴图，掌机档只占左上 1280×720
    { int sw = s2a_screen_w(), sh = s2a_screen_h(); snprintf(p, sizeof p, "%dx%d", sw > 0 ? sw : 1920, sh > 0 ? sh : 1080); setenv("DM_NVN_SHOT_OUT", p, 0); }
    // 本机见过的管线组合（直连层下次起局预建用）与兜底现编产物：<文档根>/wire/<程序号>/。文档根 = S2A_DOCS，否则 $HOME/Documents（iOS 沙盒）；
    //   11-07 前这里固定拼 $HOME/Documents：Mac 宿主不在沙盒，落到用户文稿夹，新签名程序首次 opendir 被隐私授权框挡住、Hades 白屏 90 秒
    s2a_docs_path(p, sizeof p, "wire"); mkdir(p, 0755);
    s2a_docs_path(p, sizeof p, "wire/%016llX", (unsigned long long)s2a_npdm_program_id()); mkdir(p, 0755);
    { char q[1200]; snprintf(q, sizeof q, "%s/shaders", p); setenv("DM_MSL_FALLBACK", q, 0); }   // 兜底现编产物（着色器线 s2a_dsh_compile 写 <键>_b.metal / .json，§12.3）：库里没有的键再到这里找
    { const char *c = getenv("S2A_COLD_START");   // 冷启动：兜底目录是首启没有的东西，清掉（host/dsh_cold.inc；通用模式的 NxColdStart 在直连模式不跑）
      if (c && !strcmp(c, "1")) { uint64_t b = 0, n = dsh_cold_clear(getenv("DM_MSL_FALLBACK"), &b);
          LOG("直连冷启动：已清兜底目录 %s %llu 个文件 %llu KB", getenv("DM_MSL_FALLBACK"), (unsigned long long)n, (unsigned long long)(b >> 10)); } }
    strlcat(p, "/pso_user.tsv", sizeof p); setenv("DM_PSO_USER", p, 0);
    LOG("直连：着色器库 %s，映射表 %s", getenv("DM_MSL_DIR"), getenv("DM_NVN_SHMAP"));
}
static void fail_watch_start(void);   // 直连异常看门（见下）
static void *direct_bootstrap(const char *name) {
    static pthread_once_t once = PTHREAD_ONCE_INIT, watch = PTHREAD_ONCE_INIT;
    pthread_once(&once, direct_paths);
    pthread_once(&watch, fail_watch_start);
    return h_nvnBootstrapLoader(name);
}
// 多程序合集（runtime/include/s2a_programs.h）：进程内重启切到序号 n（0 = 根）后、新局起之前由原生层调（host_main.c switch_program_modules，按名 dlsym）。
//   序号 n 的游戏数据在 <游戏数据>/programs/<n>/（同服务层 Application.ProgramDir、Windows 出包机出包 programs/<n>），着色器库、映射表、每款参数都随它换：
//   DM_MSL_DIR / DM_NVN_SHMAP 改到那里、每款参数重读、直连层重读映射表并重开库容器（nvn_be_reload_direct_library）。
//   首局路径是显式给的（DM_MSL_DIR / DM_NVN_SHMAP、外置着色器包）→ 不知道别的序号放在哪，报红不换；首局没走过直连（没调过 direct_paths）→ 回 0 不动。
//   回 1 = 换了、0 = 不用换、-1 = 报红没换
int nvn_be_reload_direct_library(void);   // graphics/nvn/gfx/nvn/nvn_mtl.m
int s2a_direct_program_switch(int n) {
    if (!g_dp_from_gd) { if (getenv("DM_MSL_DIR") || getenv("DM_NVN_SHMAP")) { LOG("【红】多程序合集：着色器库路径是显式给的（DM_MSL_DIR / DM_NVN_SHMAP 或外置着色器包），切到序号 %d 不换库，用的仍是 %s", n, getenv("DM_MSL_DIR") ?: "-"); return -1; } return 0; }
    char gd[1200], p[1300];
    if (n > 0) snprintf(gd, sizeof gd, "%s/programs/%d", g_dp_gd, n); else strlcpy(gd, g_dp_gd, sizeof gd);
    snprintf(p, sizeof p, "%s/shaders/direct", gd); setenv("DM_MSL_DIR", p, 1);
    snprintf(p, sizeof p, "%s/shaders/direct/shader_map.tsv", gd); setenv("DM_NVN_SHMAP", p, 1);
    (void)s2a_nvn_prof(); memset(&g_prof, 0, sizeof g_prof); prof_load(gd);   // 先让首次的 once 走过，再按新目录重读
    int r = nvn_be_reload_direct_library();
    LOG("多程序合集：图形层换到序号 %d：着色器库 %s（%s）", n, getenv("DM_MSL_DIR"), r > 0 ? "已重读映射表、重开库容器" : "映射表路径没变，不重读");
    return r > 0 ? 1 : 0;
}
// 直连模式下装载器解析游戏导入的 nvnBootstrapLoader 时调一次（重定位期）：查每款参数、换切栈桩，回取址入口（第一次被游戏调到时再定路径）
void *s2a_nvn_bootstrap_entry(void) {
    static int done; if (done) return (void *)direct_bootstrap;
    done = 1;
    s2a_nvn_prof();
    big_wrap();
    return (void *)direct_bootstrap;
}
// ---------- Vulkan 款（上游 dm/gfx/vk/hle_vk.m，MIT，10-24 原样搬入 graphics/nvn/gfx/vk/）----------
// 游戏 main 直接导入 vk*（哈迪斯、超级马力欧银河 1 / 2）：Vulkan 加载器 subsdk0 与 NVIDIA 驱动 subsdk1 不装（款配置 modules），
//   装载器重定位时把 vk* 导入解析到这里（runtime/src/gfxmode.c s2a_direct_lookup），vkGet*ProcAddr 也从这里取。
//   hle_vk 的 VKB 函数（登记在 __DATA,swvkb）经 S2Arm 切栈桩跑在宿主栈上（同 NVN 的 big_wrap），其余命令录制留在游戏栈
void *dm_vk_lookup(const char *name);   // graphics/nvn/gfx/vk/hle_vk.m
__attribute__((weak)) int s2a_console_mode(void) { return -1; }   // 强定义在 runtime/src/os.c（平台壳运行中设的档位，-1 = 没设过）；只链图形库的单测用这份
// 档位同服务层 Host.Docked：1 主机 / 0 掌机。运行中切过（设置页 / 档位梯子 runtime/src/qladder.c 调 s2a_console_mode_set）就按切后的，否则按起局 S2A_DOCKED
//   （11-06 优雅降级：原先只读起局环境，局中切档后游戏换了分辨率、这里问到的最大分辨率与 Vulkan 表面尺寸还是旧档位的）
int dm_opmode(void) { int m = s2a_console_mode(); if (m >= 0) return m; const char *d = getenv("S2A_DOCKED"); return d && !strcmp(d, "1"); }
// vi 层裁切（上游 hle_vi.c）：游戏调 nn::vi::SetLayerCrop(layer, x, y, w, h)，真机 SDK 把它推到系统窗口（Android perform SET_CROP），上屏时交给显示层的矩形就是它；
//   NVN 的 nvnWindowInitialize 只记内部裁切、不推系统窗口，上屏也不带矩形（11-01 读织梦岛 SDK 8.2.0 机器码：WindowInitialize 0x429b30 不调后端设裁切，
//   QueuePresentTexture 0x42a280 → 后端 queue 只 queueBuffer；SetLayerCrop 0x3b9c70 = perform(3, {x, y, x+w, y+h})，宽或高为 0 传 NULL 清空）。
//   nvnWindowSetCrop 推的是同一个系统窗口裁切，所以两者谁最后写算谁：nvnWindowSetCrop 调 dm_vi_layer_crop_clear 让位（hle_nvn.c）。
//   织梦岛掌机档设 0,0 1280x720（1920x1080 窗口贴图只画左下 1280x720），超级马力欧银河（Vulkan）掌机档同理
int32_t h_vi_set_layer_crop(void *layer, int32_t x, int32_t y, int32_t w, int32_t h);   // 状态与实现在 graphics/nvn/gfx/nvn/hle_nvn.c（各平台都编；安卓不编本文件）
// 直连模式下装载器解析游戏导入的 nn::vi 函数时问这里（runtime/src/gfxmode.c s2a_direct_lookup，重定位期）；没接的名字回 NULL，照原路径解析到 sdk
void *s2a_vi_entry(const char *name) {
    if (name && !strcmp(name, "_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii")) return (void *)h_vi_set_layer_crop;
    return nvn2_vi_entry(name);   // Switch 2：显示层整个由宿主答（nvn2.c；Switch 1 回 NULL，照原路径进 sdk）
}
static int vk_big(const char *name) {
    static const char *const *sec; static unsigned long sz;
    if (!sec) sec = (const char *const *)getsectiondata(&_mh_execute_header, "__DATA", "swvkb", &sz);
    for (unsigned long i = 0; sec && i < sz / sizeof *sec; i++) if (!strcmp(sec[i], name)) return 1;
    return 0;
}
static void *vk_export(const char *name);
static void *a_get_proc(void *obj, const char *name) { (void)obj; return vk_export(name); }   // vkGetInstanceProcAddr / vkGetDeviceProcAddr：交出的指针同样要套切栈桩
typedef int32_t (*create_instance_fn)(const void *, const void *, void *);
static int32_t a_create_instance(const void *ci, const void *alloc, void *inst) {   // 第一次进 Vulkan：定着色器库路径、起直连异常看门（同 NVN 的 direct_bootstrap）
    static pthread_once_t once = PTHREAD_ONCE_INIT, watch = PTHREAD_ONCE_INIT;
    pthread_once(&once, direct_paths);
    pthread_once(&watch, fail_watch_start);
    return ((create_instance_fn)dm_vk_lookup("vkCreateInstance"))(ci, alloc, inst);
}
void s2a_snap_handout_id(uint64_t v, const char *id) __attribute__((weak));   // runtime/src/snapshot.c（登记见 hle_nvn.c）
static void *vk_export_raw(const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr") || !strcmp(name, "vkGetDeviceProcAddr")) return sw_xstack_wrap(name, (void *)a_get_proc);
    if (!strcmp(name, "vkCreateInstance")) return sw_xstack_wrap(name, (void *)a_create_instance);
    void *fn = dm_vk_lookup(name);
    return fn && vk_big(name) ? sw_xstack_wrap(name, fn) : fn;
}
// 交给游戏的每个 vk* 地址（导入解析与运行中 vkGet*ProcAddr 都走这里）按「vk:export:<名>」登记身份：切栈桩按本进程第几个被套编号，
//   另一个进程里同一个函数的桩号不同（或还没套过、那个桩号空着），恢复时游戏内存里拿着的旧地址按身份换成本进程的（vk_snap.inc vk_snap_resolve）
static void *vk_export(const char *name) {
    if (!name || strncmp(name, "vk", 2)) return NULL;
    void *p = vk_export_raw(name);
    if (p && s2a_snap_handout_id) { char id[128]; snprintf(id, sizeof id, "vk:export:%s", name); s2a_snap_handout_id((uint64_t)(uintptr_t)p, id); }
    return p;
}
// 直连模式下装载器解析游戏的 vk* 导入（重定位期）：回实现地址，没有回 NULL（装载器照原路径找，找不到报红）
void *s2a_vk_entry(const char *name) {
    static int done; if (!done) { done = 1; s2a_nvn_prof(); LOG("直连：Vulkan 款，vk* 导入解析到 graphics/nvn/gfx/vk（直连层 hle_vk）"); }
    return vk_export(name);
}

// 直连模式下装载器解析 egl* / gl* 导入（runtime/src/gfxmode.c：OpenGL 款，以及在 GL 里画一部分画面的 NVN 款）：回实现地址（重活已套切栈桩），没有回 NULL。
void *gl_entry(const char *name);   // graphics/nvn/gfx/gl/gl_core.m：查表 + 重活套切栈桩，只回真实现（没实现的核心名由 s2a_gl_stub 在装载器找遍之后给）
// 游戏第一次真正进 EGL（eglGetDisplay，graphics/nvn/gfx/gl/egl.m）时定着色器库路径（DM_MSL_DIR = <游戏数据>/shaders/direct，GL 库在其下 gl/）、起直连异常看门：
//   同 NVN 的 direct_bootstrap、Vulkan 的 vkCreateInstance。不在重定位期做：iOS 上 S2A_GAME_DATA 由 host_main.c 在装载之后才设，
//   重定位期读到空会误报「着色器库找不到」（10-25 测试设备2 数码宝贝 GL 直连首包）
void s2a_gl_first_use(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT, watch = PTHREAD_ONCE_INIT;
    pthread_once(&once, direct_paths);
    pthread_once(&watch, fail_watch_start);
}
void *s2a_gl_entry(const char *name) {
    static int prof, hit; if (!prof) { prof = 1; s2a_nvn_prof(); }
    void *fn = gl_entry(name);
    // 第一个真解析到的名字才记：层里没有的名字（马车8 的 glslc*）也会问到这里
    if (fn && !hit) { hit = 1; LOG("直连：egl* / gl* 导入解析到 graphics/nvn/gfx/gl（首个 %s）", name); }
    return fn;
}
// 装载器找遍直连层 / 游戏模块 / libc / romfs NRO 都没定义的 gl* 导入（runtime/src/gfxmode.c s2a_direct_stub）：核心名给安全空实现，其余回 NULL
void *gl_stub(const char *name);   // graphics/nvn/gfx/gl/gl_core.m
void *s2a_gl_stub(const char *name) { return gl_stub(name); }

// 直连帧数 / 每帧绘制数：NVN 层与 Vulkan 层各自计数，一局只有一层出帧，相加即本局的数（只数 NVN 时 Vulkan 款起局 90 秒就被看门判「没出首帧」）
void dm_vk_stats(uint64_t *frames, uint64_t *draws, uint64_t *passes);   // graphics/nvn/gfx/vk/hle_vk.m
void dm_vk_bad_draws(uint64_t *feat, uint64_t *err);
void dm_gl_stats(uint64_t *frames, uint64_t *draws, uint64_t *passes);   // graphics/nvn/gfx/gl/gl_core.m
void dm_gl_bad_draws(uint64_t *feat, uint64_t *err);
// 前端层（Vulkan、OpenGL）出的帧 / 绘制 / 遍 / 丢弃：NVN 层之外各自计数，一局只有一层出帧，相加即本局的数（只数 NVN 时 Vulkan 款起局 90 秒就被看门判「没出首帧」）
typedef struct { uint64_t frames, draws, passes, feat, err; } fe_t;
static fe_t fe_stats(void) {
    fe_t r; uint64_t f, d, p, a, b;
    dm_vk_stats(&r.frames, &r.draws, &r.passes); dm_vk_bad_draws(&r.feat, &r.err);
    dm_gl_stats(&f, &d, &p); dm_gl_bad_draws(&a, &b); r.frames += f; r.draws += d; r.passes += p; r.feat += a; r.err += b;
    return r;
}
static uint64_t direct_frames(void) { return dm_nvn_frame_count() + fe_stats().frames; }
unsigned long long s2a_direct_frame_count(void) { return s2a_gfx_mode() == S2A_GFX_DIRECT ? direct_frames() : 0; }
// 这台设备能不能跑直连（runtime/src/gfxmode.c s2a_direct_packaged 判直连起手时问）：直连回放层现在全走 Metal 4（MTL4CommandQueue 等，没有就开局即终止），
//   所以 = 系统有 Metal 4 且 GPU 支持 Metal 4 家族。不按系统版本整体拦：Metal 3 直连路径（着色器线 WP5）接上后，这里改成「Metal 3 或 Metal 4 能跑」
int mtl_tab_selftest(char *why, size_t n) __attribute__((weak_import));   // nvn_mtl.m 的 Metal 3 路径（#279 D3-3）：只看在不在，不调用
int s2a_direct_device_ok(void) {
    static int ok = -1;
    if (ok < 0) {
        ok = 0;
        // 直连回放层有 Metal 4 / Metal 3 两条路（graphics/nvn/gfx/nvn/nvn_be.h，#277 D3）：Metal 4 家族 GPU（系统 26 起）走 Metal 4；其余走 Metal 3，
        //   要 GPU 在 Metal 3 家族（A13 / M1 起）且有旧队列的驻留集（macOS 15 / iOS 18 起，等于构建的部署下限 tools/toolchain.env）。两条都不行才不直连起手
        id<MTLDevice> d = MTLCreateSystemDefaultDevice(); int m4 = 0, m3 = 0;
        if (@available(macOS 26.0, iOS 26.0, *)) m4 = d && [d supportsFamily:MTLGPUFamilyMetal4];
        // Metal 3 路径在不在本构建里：问它自己导出的符号（参数表 D3-3 mtl_tab_selftest，叠在编码器 D3-2 之上，它在 = 整条路在）。弱引用、没链进来为空：
        //   路径没合入时没有 Metal 4 的设备照旧不直连起手，不靠合入顺序（10-25 维护者）
        if (@available(macOS 15.0, iOS 18.0, *)) m3 = mtl_tab_selftest && d && [d supportsFamily:MTLGPUFamilyMetal3];
        ok = m4 || m3;
        // 读数逐项写出（红行，诊断定位卡只摘红行）：玩家诊断里两台 A13（苹果文档属 Metal 3 家族）也被判不能直连，手上没有 A13 实机，靠下一份诊断这行定因
        if (!ok) {
            NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion; int f3 = d && [d supportsFamily:MTLGPUFamilyMetal3];
            LOG("【红】直连：这台设备既没有 Metal 4 也没有 Metal 3 路径，本局不直连起手（读数：设备 %s，系统 %ld.%ld，Metal 4 家族 %d，Metal 3 家族 %d，本构建 Metal 3 路径 %s）",
                d ? d.name.UTF8String : "无", (long)v.majorVersion, (long)v.minorVersion, m4, f3, mtl_tab_selftest ? "在" : "不在");
        }
    }
    return ok;
}
static _Atomic int g_suspended;
void s2a_direct_suspend_gate(int on) {
    if (s2a_gfx_mode() == S2A_GFX_DIRECT) { atomic_store(&g_suspended, on); dm_gfx_suspend(on); }
}
void s2a_direct_suspend(int on) {
    s2a_direct_suspend_gate(on);
    if (on && s2a_gfx_mode() == S2A_GFX_DIRECT) dm_gfx_suspend_drain();
}
int s2a_direct_guest_start_hold(void) {
    if (s2a_gfx_mode() != S2A_GFX_DIRECT) return 0;
    atomic_store(&g_suspended, 1);
    return dm_gfx_guest_start_hold();
}

// ---------- 直连异常（docs/设计_双轨.md §11.3 / §11.5）----------
// 五个入口：NVN 未实现函数被调（hle_nvn.c nvn_unimpl_c）、GPU 命令缓冲报错（nvn_mtl.m 两处 cb.error）、崩溃信号（runtime/src/crash.c，只打现场）、
//   看门（起局 90 秒没首帧 / 出帧后停 45 秒 / 画面检查：游戏的绘制被持续丢掉，host/pic_watch.inc；后台时不计）。
// 10-26 维护者改定：运行中不切换——出错只在出错那一处兜底（跳过或补上），这里按原因类别各打一行【红】（同一类每局只打一次），直连继续跑；
//   不写 fail、不弹窗、不收局。兼容（通用）路线改成出包时按款定（docs/设计_双轨.md §11.5）。开发局 tools/dev_run.sh 按【红】行判红
static pthread_mutex_t g_anom_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_anom_seen[16][64]; static int g_anom_n;   // 已打过的原因类别（原因里第一个「：」或「（」之前那段）
void s2a_direct_fail(const char *why) {
    if (s2a_gfx_mode() != S2A_GFX_DIRECT) return;
    if (!why) why = "-";
    char k[64]; size_t n = strlen(why);   // 「：」「（」是多字节 UTF-8，不能用 strcspn 的字节集合：逐位比整串
    for (const char *q = why; *q; q++) if (!strncmp(q, "：", strlen("：")) || !strncmp(q, "（", strlen("（"))) { n = (size_t)(q - why); break; }
    if (n >= sizeof k) n = sizeof k - 1;
    memcpy(k, why, n); k[n] = 0;
    pthread_mutex_lock(&g_anom_mu);
    int seen = 0; for (int i = 0; i < g_anom_n; i++) if (!strcmp(g_anom_seen[i], k)) { seen = 1; break; }
    if (!seen && g_anom_n < 16) snprintf(g_anom_seen[g_anom_n++], sizeof *g_anom_seen, "%s", k);
    pthread_mutex_unlock(&g_anom_mu);
    if (!seen) LOG("【红】直连异常（不切换，继续跑）：%s", why);
}
// GPU 安全停的宿主一侧（11-07 Switch2 线 84；判定与停提交在 nvn_mtl.m mtl_gpu_cb_error / mtl_gpu_halt，这里只在置位后调一次）。
//   10-31 起的做法是「挂死 → 派到别的线程走优雅收局」，可判定只看 localizedDescription，Metal 4 + 中文系统一次也没命中（11-07 测试设备2 金刚：照常提交、连续挂死、整机卡死）；
//   收局本身也要停显卡、等 GPU。现在提交在 nvn_mtl.m 那道门（gpu_go）当场就停，这里：
//   iOS —— 冻游戏线程、用 UIKit 原生视图盖一张中英双语错误页（app.m s2a_ios_gpu_stopped，不经 Metal），停在错误页，不收局（收局要等 GPU）；玩家从后台划掉再开。
//   Mac —— 同原先，转到别的线程优雅退出（s2a_host_shutdown 不能在 Metal 的完成回调线程里直接调；它停显卡时 nvn_be_drain 见安全停不再交 / 等）
void s2a_gpu_safe_stop(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    LOG("【红】GPU 挂死安全停：这局停在这里，不再往驱动提交（%s）", why ? why : "-");
#if TARGET_OS_IPHONE
    void (*page)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_ios_gpu_stopped");
    if (page) { page(why ? why : "-"); return; }
    LOG("【红】GPU 挂死安全停：iOS 壳没有错误页入口，改为收局");
#endif
    char *w = strdup(why ? why : "-");
    // 退出码 86（同 gal_core.m 命令缓冲出错）：作业层（pack/jobs/build.py）按它告诉玩家这局是被安全停下的，不显示「操作完成」（#1257）
    void (*code)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "s2a_host_exit_code");
    if (code) code(86);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        void (*down)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_host_shutdown");
        char m[300]; snprintf(m, sizeof m, "GPU 挂死安全停（%.200s）", w);
        if (down) down(m); else _exit(86);
    });
}
// GPU 卡住暂停的宿主一侧（11-13；判据在 nvn_mtl.m gpu_frame_stuck：连续 3 帧各过 3 秒）：iOS 盖「继续玩 / 退出」页（app.m s2a_ios_gpu_stuck），
//   Mac 弹同样两个按钮（macos/front_mac.m s2a_mac_gpu_stuck）；两边都先冻游戏线程，不再往 GPU 送新活。选继续 → s2a_gpu_stuck_resume。
//   哪边都没有入口（单测 / 无界面）：只记一行、照常跑（不停局）
void mtl_gpu_stuck_resume(void);
void s2a_gpu_stuck_resume(void) { mtl_gpu_stuck_resume(); }
void s2a_gpu_stuck(const char *why) {
    LOG("【红】GPU 卡住：%s → 暂停游戏，等玩家选继续或退出", why ? why : "-");
    void (*page)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, TARGET_OS_IPHONE ? "s2a_ios_gpu_stuck" : "s2a_mac_gpu_stuck");
    if (page) { page(why ? why : "-"); return; }
    LOG("【黄】GPU 卡住：宿主没有暂停页入口，照常继续");
    mtl_gpu_stuck_resume();
}
// nvn_mtl.m：命令缓冲执行报错（判挂死在 nvn_mtl.m mtl_gpu_cb_error，这里只打直连异常红行）。后台时（iOS 不许后台提交）的报错不算
void s2a_direct_gpu_error(const char *desc) {
    if (atomic_load(&g_suspended)) { LOG("直连：后台时的命令缓冲报错不判失败（%s）", desc ? desc : "-"); return; }
    char w[256]; snprintf(w, sizeof w, "GPU 命令缓冲报错：%s", desc ? desc : "-"); s2a_direct_fail(w);
}
#include "host/pic_watch.inc"
void nvn_be_stats(uint64_t *passes, uint64_t *draws);
void mtl_bad_draws(uint64_t *feat, uint64_t *err);
static void *fail_watch(void *arg) {
    (void)arg;
    pw_state pw = {0}; uint64_t pd = 0, pf = 0, pe = 0; int base = 0;   // 画面检查（host/pic_watch.inc）：上一秒的累计，base = 有没有可比的上一秒
    unsigned long long last = 0; double idle = 0, wait_first = 0;
    for (;;) {
        sleep(1);
        // 客机冻结（Mac 失焦暂停 / iOS 后台 / 重启屏障，runtime/src/thread.c s2a_guest_frozen）按设计不出帧，不算停住
        //   （11-15 v0.4.2 候选一局：窗口失焦冻结 45 秒被报「出帧停住 45 秒」假红；停帧救场 runtime/src/ap_host.c 早已同口径）
        //   宿主失焦 / 睡眠同样不算（D-T324，Mac 0.6.7：窗口失焦但没冻结——失焦暂停关——游戏收到失焦通知自己停画 53 秒，被记成「出帧停住 45 秒」直连失败）。
        //   外因判定与出图停住看门同一条（ap_host.c s2a_ap_stall_external：冻结 / 焦点=0 / 睡眠=1 / 系统小程序开着 任一成立；取不到的符号记 -1、不冒充外因，t_ap_host ⑥d 测）
        //   系统小程序开着（键盘 / 错误弹窗 / 手柄设置）同样不算（D-Z96G，iPad 0.6.9：输名字 53 秒被记成「出帧停住 45 秒」）
        static int (*frozen)(void), (*focused)(void), (*sleeping)(void), (*applet)(void), (*external)(int, int, int, int); static int looked;
        if (!looked) { frozen = (int (*)(void))dlsym(RTLD_DEFAULT, "s2a_guest_frozen"); focused = (int (*)(void))dlsym(RTLD_DEFAULT, "s2a_host_focused");
                       sleeping = (int (*)(void))dlsym(RTLD_DEFAULT, "s2a_host_sleeping"); applet = (int (*)(void))dlsym(RTLD_DEFAULT, "s2a_applet_open");
                       external = (int (*)(int, int, int, int))dlsym(RTLD_DEFAULT, "s2a_ap_stall_external"); looked = 1; }
        { int fz = frozen ? frozen() : -1, fo = focused ? focused() : -1, sl = sleeping ? sleeping() : -1, ap = applet ? applet() : -1;
          if (atomic_load(&g_suspended) || (external ? external(fz, fo, sl, ap) : fz == 1)) { idle = 0; base = 0; continue; } }
        unsigned long long f = direct_frames();
        if (!f) {
            if (++wait_first != 90) continue;   // 只在第 90 秒报一次、打一次现场，之后照常等首帧
            // 首帧前卡死留现场（出图停住看门首帧后才武装）：先打游戏线程表 + 宿主线程栈，同出图停住看门（runtime/src/ap_host.c）
            void (*td)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "s2a_thread_dump");
            int (*hd)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "s2a_dump_threads");
            LOG("直连：起局 90 秒没出首帧，游戏线程表 + 宿主线程栈：");
            if (td) td();
            if (hd) hd("直连：起局 90 秒没出首帧");
            s2a_direct_fail("起局 90 秒没出首帧"); continue;
        }
        { uint64_t p, d, fe, er; char w[200]; nvn_be_stats(&p, &d); mtl_bad_draws(&fe, &er);
          fe_t x = fe_stats(); d += x.draws; fe += x.feat; er += x.err;   // Vulkan / OpenGL 款的绘制 / 丢弃在前端层（NVN 款恒 0）
          if (base && pw_push(&pw, d - pd, fe - pf, er - pe, w, sizeof w)) s2a_direct_fail(w);
          pd = d; pf = fe; pe = er; base = 1; }
        if (f != last) { last = f; idle = 0; continue; }
        if (++idle >= 45) { char w[96]; snprintf(w, sizeof w, "出帧停住 45 秒（停在第 %llu 帧）", f); s2a_direct_fail(w); idle = 0; }
    }
}
static void fail_watch_start(void) { pthread_t t; if (!pthread_create(&t, NULL, fail_watch, NULL)) pthread_detach(t); else LOG("【红】直连：异常看门线程起不来"); }

// ---------- 自动玩家信号（tests/autoplay/autoplay_host.h）：直连模式下由这里给，graphics/metal 的 s2a_ap_draws / nxgal_screenshot 见直连就转过来 ----------
// 每帧绘制数：直连层累计绘制数（nvn_be_stats）按上屏帧取差；在每帧上屏通知里更新
void nvn_be_stats(uint64_t *passes, uint64_t *draws);
static _Atomic uint64_t g_ap_draws, g_ap_prev, g_ap_passes, g_ap_pprev;   // 每帧渲染遍数同法（普查极速模式的第二信号，10-30）
static void ap_draws_tick(void) { uint64_t p, d; nvn_be_stats(&p, &d); d += fe_stats().draws; atomic_store(&g_ap_draws, d - atomic_exchange(&g_ap_prev, d)); atomic_store(&g_ap_passes, p - atomic_exchange(&g_ap_pprev, p)); }
uint32_t s2a_direct_ap_draws(void) { uint64_t d = atomic_load(&g_ap_draws); return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d; }
uint32_t s2a_direct_ap_passes(void) { uint64_t d = atomic_load(&g_ap_passes); return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d; }
// 最近一帧截图（BGRA8）：直连层只给「下一次上屏时抓一张」的异步回调（mtl_grab_next_present，只在有出图层时抓）。
// 这里缓存最近抓到的一张：每次调用先交出缓存、再预约下一张——自动玩家拿到的是最近一两帧内的画面（它本来就按秒级节奏看图）
void mtl_grab_next_present(void (^cb)(const uint8_t *bgra, size_t w, size_t h, size_t row));
// 满屏内容判据（nvn_mtl.m mtl_fit_judge_shot）在这里、取图方的线程上判，不在上屏线程（10-30 d2r 慢帧 2.4%：接缝判据跑在上屏线程）：
//   回调里只记这张是不是被挑中（令牌），取图时用取走的那份拷贝判
uint64_t mtl_grab_fit_token(void); int mtl_fit_judge_shot(uint64_t tok, const uint8_t *p, size_t w, size_t h, size_t row);
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_shot; static int32_t g_shot_w, g_shot_h, g_shot_cap; static _Atomic int g_shot_armed; static uint64_t g_shot_tok;   // g_shot_tok：g_shot 这张的判据令牌（0 = 不判）
static void shot_arm(void) {
    if (dm_gfx_session_stopping()) return;
    if (atomic_exchange(&g_shot_armed, 1)) return;
    mtl_grab_next_present(^(const uint8_t *px, size_t w, size_t h, size_t row) {   // 原始 BGRA8 字节，只在回调里有效：拷一次进缓存（不再经 CGImage 再画一遍）
        if (px) {
            int32_t need = (int32_t)(w * h * 4);
            pthread_mutex_lock(&g_shot_mu);
            if (need > g_shot_cap) { uint8_t *b = realloc(g_shot, (size_t)need); if (b) { g_shot = b; g_shot_cap = need; } }
            if (need <= g_shot_cap) { if (row == w * 4) memcpy(g_shot, px, (size_t)need); else for (size_t y = 0; y < h; y++) memcpy(g_shot + y * w * 4, px + y * row, w * 4);
                g_shot_w = (int32_t)w; g_shot_h = (int32_t)h; g_shot_tok = mtl_grab_fit_token(); }
            pthread_mutex_unlock(&g_shot_mu);
        }
        atomic_store(&g_shot_armed, 0);
    });
}
int32_t s2a_direct_screenshot(void *out, int32_t cap, int32_t *w, int32_t *h) {
    shot_arm();
    pthread_mutex_lock(&g_shot_mu);
    int32_t need = g_shot_w * g_shot_h * 4;
    if (w) *w = g_shot_w; if (h) *h = g_shot_h;
    uint64_t tok = 0; size_t sw = (size_t)g_shot_w, sh = (size_t)g_shot_h;
    if (need > 0 && out && cap >= need) { memcpy(out, g_shot, (size_t)need); tok = g_shot_tok; g_shot_tok = 0; }
    pthread_mutex_unlock(&g_shot_mu);
    if (tok) mtl_fit_judge_shot(tok, out, sw, sh, sw * 4);   // 判的是交给调用方的这份拷贝（判据只读，不改字节）
    return need;   // 0 = 还没抓到过（同 nxgal_screenshot「还没出图」）
}

// ---------- 直连 frames.tsv（tests/真机契约.md §3；通用模式由 graphics/metal/cs/NxFrames.cs 写，不经这里）----------
// 直连模式下 C# 渲染循环不起，frames.tsv 原先一行都没有（10-24 动森直连局 acnh_1024204947_2），记分板帧率 / 慢帧格没数据。
// 计时取宿主侧回调，不读直连层的 DM_NVN_TSV：每次上屏直连层先调 dm_guest_frame_tick（上面，记上屏时刻与累计绘制 / 遍数），
//   再在拿到 8 次上屏之前那一次的屏上时刻后调 dm_perf_present（这里的强定义）——此刻本次是第 c 次上屏，回报的是第 c − 8 次；
//   第 n 次上屏显示的是第 n − 1 帧的 GPU 工作（nvn_be_set_frame 的帧号口径），这一帧此时早已做完，GPU 时间用 nvn_be_gpu_ms 取。
// 列只写拿得到的：frame / wall_us / disp_us / gpu_us / draw_n / pass_n。alloc_b、gc*、rb_us、gpuq_us、gc_pause_us 直连层没有，整列不出现
//   （tools/frame_gate.py 缺列判「未测」；写 0 会被当成真读数判绿）。屏上间隔 −1（被系统丢）或 0（没读数）时 disp_us 留空。
//   首行是起点（frame 1、wall_us 0，同契约）；最后 8 次上屏没有屏上读数，不写
double nvn_be_gpu_ms(uint64_t f);
double dm_vk_gpu_ms(uint64_t n);   // graphics/nvn/gfx/vk/hle_vk.m   // graphics/nvn/gfx/nvn/nvn_mtl.m：第 f 帧 GPU 毫秒（没做完 / 没读数 < 0）
#define FTN 64
// 分配 / GC 列（帧率能耗线）：C# 在直连局起局时登记采样钩子（graphics/metal/cs/NxFrames.cs StartDirect），每次上屏当场取 6 个累计量存进环，
//   写第 n 行时取第 n 次上屏那份（不取写行时刻的：已领先 8 次上屏）。依次：托管累计分配、gen0 / gen1 / gen2 回收次数、GC 累计暂停（微秒）。
//   不出 alloc_gpu_b：本线程是直连层的上屏线程，没有托管分配，记出来恒近 0 会被当成「编码线程零分配」的真读数。
//   钩子同时在 gen0 次数变了时往 host.log 打一行 [gc]（格式同通用模式）。没登记（C# 没起）= 这几列整列不出现；某行没采到 = 留空
static void (*g_ft_sample)(uint64_t c, int64_t *o);
void s2a_direct_frame_sample_set(void (*f)(uint64_t c, int64_t *o)) { g_ft_sample = f; }
static int g_ft_gc;   // 表头带分配 / GC 列（ft_open 时按是否登记定）
static struct { uint64_t t_ns, draws, passes; int64_t gc[5]; int has_gc; } g_ft[FTN];
static uint64_t g_ft_c;   // 已上屏次数（上屏线程独写）
static int g_ft_fd = -2;
static uint64_t g_ft_last;
static int g_ft_pace_last;
static void ft_present(void) {
    uint64_t p, d; nvn_be_stats(&p, &d); fe_t x = fe_stats(); p += x.passes; d += x.draws;   // Vulkan / OpenGL 款的绘制 / 遍数在前端层（NVN 款恒 0）
    uint64_t c = ++g_ft_c; g_ft[c % FTN].t_ns = now_ns(); g_ft[c % FTN].draws = d; g_ft[c % FTN].passes = p;
    void (*f)(uint64_t, int64_t *) = g_ft_sample; g_ft[c % FTN].has_gc = f != NULL;
    if (f) f(c, g_ft[c % FTN].gc);
}
// frames.tsv 落盘不在上屏 / 提交线程上；缓冲满时丢行计数，不阻塞上屏。
// 退出先关闭接收并唤醒 writer，join 等它按队列顺序排空后才返回；不把空队列等待当写盘锁。
#define FT_BUF (1u << 20)
static char g_ftb[FT_BUF]; static size_t g_ftb_n; static uint64_t g_ftb_drop;
static pthread_mutex_t g_ftb_mu = PTHREAD_MUTEX_INITIALIZER, g_fte_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ftb_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_ft_thread; static int g_ft_started, g_ft_stop;
static char g_ftw[FT_BUF];   // 只由 writer 使用；退出方 join 完成后才能取缓冲
static int ft_take_write(int wait) {
    pthread_mutex_lock(&g_ftb_mu);
    while (wait && !g_ftb_n && !g_ft_stop) pthread_cond_wait(&g_ftb_cv, &g_ftb_mu);
    size_t n = g_ftb_n; memcpy(g_ftw, g_ftb, n); g_ftb_n = 0;
    pthread_mutex_unlock(&g_ftb_mu);
    for (size_t o = 0; o < n;) { ssize_t w = write(g_ft_fd, g_ftw + o, n - o); if (w <= 0) break; o += (size_t)w; }
    return (int)n;
}
static void *ft_writer(void *a) {
    (void)a; pthread_setname_np("s2a-frames-tsv");
    while (ft_take_write(1)) {}   // stop 且队列为空才结束；已接收的行按原顺序落盘
    return NULL;
}
static void ft_exit_flush(void) {
    pthread_mutex_lock(&g_fte_mu);   // 重复 / 并发退出不能同时 join 同一个 writer
    if (g_ft_fd < 0) { pthread_mutex_unlock(&g_fte_mu); return; }
    pthread_mutex_lock(&g_ftb_mu);
    g_ft_stop = 1;
    pthread_cond_broadcast(&g_ftb_cv);
    pthread_mutex_unlock(&g_ftb_mu);
    if (g_ft_started) {
        int e = pthread_join(g_ft_thread, NULL);
        if (e) { LOG("【红】直连：frames.tsv 等待写盘线程失败（%d）", e); pthread_mutex_unlock(&g_fte_mu); return; }
        g_ft_started = 0;
    }
    ft_take_write(0);   // 也覆盖未启 writer 的对照；此时不会再接收新行
    if (g_ftb_drop) LOG("【红】直连：frames.tsv 写盘跟不上，丢了 %llu 行（缓冲 %u 字节）", (unsigned long long)g_ftb_drop, FT_BUF);
    pthread_mutex_unlock(&g_fte_mu);
}
static void ft_put(const char *b, size_t n) {
    pthread_mutex_lock(&g_ftb_mu);
    if (!g_ft_stop && g_ftb_n + n <= FT_BUF) { memcpy(g_ftb + g_ftb_n, b, n); g_ftb_n += n; pthread_cond_signal(&g_ftb_cv); }
    else if (!g_ft_stop) g_ftb_drop++;
    pthread_mutex_unlock(&g_ftb_mu);
}
static void ft_open(void) {
    g_ft_fd = -1;
    const char *p = getenv("S2A_FRAMES_TSV"); if (!p || !*p || s2a_gfx_mode() != S2A_GFX_DIRECT) return;
    char path[1200];
    if (p[0] == '/') strlcpy(path, p, sizeof path);
    else { const char *d = getenv("S2A_DOCS");   // 同 NxColdStart.Doc：相对名落文档根（Mac 局 S2A_DOCS = 局目录；Mac 没给就按当前目录）
        if (TARGET_OS_IPHONE || (d && *d)) s2a_docs_path(path, sizeof path, "%s", p); else strlcpy(path, p, sizeof path);
    }
    g_ft_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_ft_fd < 0) { LOG("【红】直连：frames.tsv 打不开（%s）", path); return; }
    g_ft_gc = g_ft_sample != NULL;
    static const char head[] = "frame\twall_us\tdisp_us\tgpu_us\tdraw_n\tpass_n", head_gc[] = "\talloc_b\tgc0\tgc1\tgc2\tgc_pause_us";
    (void)!write(g_ft_fd, head, sizeof head - 1);
    if (g_ft_gc) (void)!write(g_ft_fd, head_gc, sizeof head_gc - 1);
    (void)!write(g_ft_fd, "\n", 1);
    { pthread_t th; if (pthread_create(&th, NULL, ft_writer, NULL) == 0) { g_ft_thread = th; g_ft_started = 1; atexit(ft_exit_flush); }
      else { LOG("【红】直连：frames.tsv 写盘线程起不来，不写"); close(g_ft_fd); g_ft_fd = -1; return; } }
    LOG("直连：frames.tsv → %s（列 frame / wall_us / disp_us / gpu_us / draw_n / pass_n%s；读回列直连层没有，不出现）", path,
        g_ft_gc ? " / alloc_b / gc0-2 / gc_pause_us" : "；分配 / GC 列没有登记采样钩子，不出现");
}
static void ft_row(uint64_t n, double iv_ms) {
    char b[320]; int o = 0;
    uint64_t wall = n > 1 ? (g_ft[n % FTN].t_ns - g_ft[(n - 1) % FTN].t_ns) / 1000 : 0;
    o += snprintf(b + o, sizeof b - (size_t)o, "%llu\t%llu\t", (unsigned long long)n, (unsigned long long)wall);
    if (n > 1 && iv_ms > 0) o += snprintf(b + o, sizeof b - (size_t)o, "%llu", (unsigned long long)(iv_ms * 1000 + 0.5));
    b[o++] = '\t';
    double g = n > 1 ? nvn_be_gpu_ms(n - 1) : -1;
    if (g < 0) { double v = dm_vk_gpu_ms(n); if (v >= 0) g = v; }   // Vulkan 款：第 n 次上屏那一帧各次提交的 GPU 时间之和（hle_vk.m；NVN 款恒 −1）
    if (g >= 0) o += snprintf(b + o, sizeof b - (size_t)o, "%llu", (unsigned long long)(g * 1000 + 0.5));
    uint64_t dn = n > 1 ? g_ft[n % FTN].draws - g_ft[(n - 1) % FTN].draws : g_ft[n % FTN].draws, pn = n > 1 ? g_ft[n % FTN].passes - g_ft[(n - 1) % FTN].passes : g_ft[n % FTN].passes;
    o += snprintf(b + o, sizeof b - (size_t)o, "\t%llu\t%llu", (unsigned long long)dn, (unsigned long long)pn);
    if (g_ft_gc) {   // alloc_b / gc_pause_us 按相邻两次上屏取差（起点行 0）；gc0-2 写累计次数（同通用模式）。本行或上一行没采到 = 本行这几格留空
        const int64_t *g = g_ft[n % FTN].gc, *q = g_ft[(n - 1) % FTN].gc; int cur = g_ft[n % FTN].has_gc, prev = n == 1 || g_ft[(n - 1) % FTN].has_gc;
        if (cur && prev) o += snprintf(b + o, sizeof b - (size_t)o, "\t%lld\t%lld\t%lld\t%lld\t%lld", n > 1 ? (long long)(g[0] - q[0]) : 0LL,
            (long long)g[1], (long long)g[2], (long long)g[3], n > 1 ? (long long)(g[4] - q[4]) : 0LL);
        else o += snprintf(b + o, sizeof b - (size_t)o, "\t\t\t\t\t");
    }
    b[o++] = '\n';
    ft_put(b, (size_t)o);
}
void dm_perf_present(double iv_ms) {   // 直连层 hle_nvn.c 弱符号（它的 diag.c 强定义写 perf.txt，S2Arm 没搬那个文件）
    if (g_ft_fd == -2) ft_open();
    if (g_ft_fd < 0 || g_ft_c <= 8) return;
    uint64_t n = g_ft_c - 8;
    // 首次回报的是第 2 次上屏（直连层从第 10 次上屏起回报）：先补起点行；每局末清 g_ft_last。
    if (!g_ft_last && n > 1) ft_row(1, 0);
    if (n <= g_ft_last) return;
    ft_row(n, iv_ms); g_ft_last = n;
}

// 直连局的「帧节拍」行（tools/game_fps.py 取游戏自己要的帧率；通用模式由 graphics/loop/RenderLoop.cs 打，直连下它不起）：
//   直连层按游戏设的交换间隔给目标帧率（nvn_game_fps：30 / 60），每次上屏比一次，变了打一行，格式同 RenderLoop.FrameLine
int nvn_game_fps(void);   // graphics/nvn/gfx/nvn/hle_nvn.c
static void ft_pace(void) {
    int fps = nvn_game_fps();
    if (fps <= 0 || fps == g_ft_pace_last) return;
    g_ft_pace_last = fps; LOG("[gal] 帧节拍：交换间隔 %d，每帧 %.1f ms（%d 帧/秒）", 60 / fps, 1000.0 / fps, fps);
}

int dm_vk_shutdown_session(void);
int dm_gl_shutdown_session(void);
int dm_nvn_shutdown_session(void);
int nvn_be_present_shutdown(void);
// Internal frontend/resource stage. The dedicated graphics_session.c archive
// member owns the production lifecycle and its strong host-tracking dependency.
int dm_gfx_retire_frontends(void) {
    if (!dm_gfx_session_stopping()) return -EBUSY;
    // External swapchain textures are also referenced by the shared present
    // queue. That queue must become idle before either frontend drops owners.
    int e = nvn_be_present_shutdown(); if (e) return e;
    e = dm_gl_shutdown_session(); if (e) return e;
    e = dm_vk_shutdown_session(); if (e) return e;
    return dm_nvn_shutdown_session();
}
// Graphics session begin (graphics_session.c, every session including the first): re-register the process-lifetime callbacks the native layer's session reset
//   cleared (runtime/src/direct_host.c s2a_direct_host_session_reset) — the census summary (dsh_miss.inc, a constructor) and the Metal wait dump / resource report
//   (registered once at device setup). nvn_dirty's fault and pre-write hooks re-register themselves with the session's first pool (nvn_dirty.c g_hooks_on)
void dm_gfx_begin_host_state(void) {
    s2a_direct_summary_register(dsh_summary);
    { void mtl_begin_session_hooks(void); mtl_begin_session_hooks(); }
}
void dm_gfx_retire_host_state(void) {
    // Late compilation has no caller left after the guest and backend drain.
    // Cold keys never drawn still own their copied control sections here.
    pthread_mutex_lock(&g_dsh_pend_mu);
    for (int i = 0; i < DSH_PEND_BUCKETS; i++) {
        dsh_pend *entry = g_dsh_pend[i];
        while (entry) { dsh_pend *next = entry->next; free(entry); entry = next; }
        g_dsh_pend[i] = NULL;
    }
    atomic_store(&g_dsh_pend_n, 0);
    pthread_mutex_unlock(&g_dsh_pend_mu);
    atomic_store(&g_dsh_ok, 0); atomic_store(&g_dsh_failed, 0); atomic_store(&g_dsh_skipped, 0);
    atomic_store(&g_dsh_reused, 0); atomic_store(&g_dsh_typed, 0); atomic_store(&g_dsh_bad, 0);
    mtl_grab_next_present(nil);
    pthread_mutex_lock(&g_shot_mu); free(g_shot); g_shot = NULL; g_shot_w = g_shot_h = g_shot_cap = 0; pthread_mutex_unlock(&g_shot_mu);
    atomic_store(&g_shot_armed, 0); g_wprep = NULL; g_suspended = 0;
    pthread_mutex_lock(&g_anom_mu); memset(g_anom_seen, 0, sizeof g_anom_seen); g_anom_n = 0; pthread_mutex_unlock(&g_anom_mu);
    if (g_ft_fd >= 0) close(g_ft_fd); g_ft_fd = -2; g_ft_c = g_ft_last = 0; g_ft_pace_last = g_ft_gc = 0; g_ft_sample = NULL;
    memset(g_ft, 0, sizeof g_ft); g_ap_draws = g_ap_prev = 0;
}
// 标称帧率（自动玩家折算路线帧数，runtime/src/ap_host.c s2a_nominal_fps_set）：出图实际跟的节拍，三种直连款各取自己的出图路径——
//   GL 款 eglSwapBuffers 按 目标帧率 / eglSwapInterval 等拍（egl.m gl_present_fps）；Vulkan 款 vkAcquireNextImageKHR 按目标帧率排（hle_vk.m，不看游戏的 present mode）；
//   NVN 款按 present_itv（hle_nvn.c nvn_present_fps：帧步进 hold / display 模式 / 这一帧退回游戏间隔）。一局只有一层出帧，先问 GL、再问 Vulkan、都没出过帧就是 NVN
int nvn_present_fps(void); int gl_present_fps(void); int nvn_be_fps_target(void);   // graphics/nvn/gfx/nvn/hle_nvn.c、graphics/nvn/gfx/gl/egl.m、graphics/nvn/gfx/nvn/nvn_mtl.m
static int present_fps(void) {
    int f = gl_present_fps(); if (f > 0) return f;
    uint64_t vf, vd, vp; dm_vk_stats(&vf, &vd, &vp); if (vf) return nvn_be_fps_target();
    return nvn_present_fps();
}
