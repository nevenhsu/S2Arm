// 开发浮层目标帧率（runtime/src/hud.c hud_target，#619 审计 F-hud-cap-above-game）单测：桩出图形模式与前端回报的游戏帧率，读 s2a_hud_text 的「目标 N」。
// 期望手写，不跑被测实现：目标 = 实际生效周期 max(游戏间隔周期, 1/上限) 对应的帧率 = min(游戏帧率, 上限)；游戏不锁（GL 交换间隔 0）时 = 上限；
//   上限只认整串 0/30/60/120（同 graphics/nvn nvn_fps.c），非法值不进浮层；两者都没有 = 「未取得」（记 0）
#include "../src/native.h"
#include "../include/s2a_hud.h"
#include "../include/s2a_gfx_mode.h"
#include <math.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
_Atomic uint64_t g_s2a_hud_ft[2048], g_s2a_hud_fn;   // 上屏时间戳环（生产在 ap_host.c）：空环 = 慢帧「未测」，不影响目标
static int g_mode = S2A_GFX_DIRECT, g_gl = -1, g_nv = 60;
int s2a_gfx_mode(void) { return g_mode; }
const char *s2a_gfx_mode_brief(void) { return "单测"; }
int s2a_widen_full(void) { return 0; }
int s2a_widen_game(void) { return 0; }
int s2a_energy_snap(s2a_esnap *s) { (void)s; return 1; }
static uint64_t g_gpu_alloc;   // 浮层「GPU 已分配」（生产在 nvn_mtl.m）：0 = 没有直连层
uint64_t s2a_gpu_alloc_bytes(void) { return g_gpu_alloc; }
double s2a_gpu_busy_rate(const s2a_esnap *a, const s2a_esnap *b) { (void)a; (void)b; return -1; }   // 桩：上面采样恒失败，走不到这里
int nvn_gl_game_fps(void) { return g_gl; }   // 浮层按名字取（dlsym），这里给强定义当 GL 前端
int nvn_game_fps(void) { return g_nv; }
static int g_dir_w, g_dir_h;
void s2a_direct_out_size(int *w, int *h) { if (w) *w = g_dir_w; if (h) *h = g_dir_h; }
// 固定内存采样结果，验证成功和失败都不会因 Metal HUD 请求被隐藏。
static int g_mem_ok = 1;
kern_return_t task_info(task_name_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task; (void)flavor; (void)count;
    if (!g_mem_ok) return KERN_FAILURE;
    ((task_vm_info_t)info)->phys_footprint = 123 * 1048576ULL;
    return KERN_SUCCESS;
}
static int fails;
static void check_display(const char *label, const s2a_hud_host *host, int frames, int mem_ok) {
    char text[4096];
    g_mem_ok = mem_ok;
    atomic_store(&g_s2a_hud_fn, 0);
    if (frames) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        atomic_store(&g_s2a_hud_ft[0], mach_absolute_time() * tb.numer / tb.denom);
        atomic_store(&g_s2a_hud_fn, 1);
    }
    s2a_hud_text(text, sizeof text, host);
    const char *fps = frames ? "帧率 1 / 目标 " : "帧率 0 / 目标 ";
    const char *mem = mem_ok ? "内存 123 MB / 上限 " : "内存 读取失败 / 上限 ";
    int ok = strstr(text, fps) && strstr(text, mem) && strstr(text, "慢帧 未测（近 10 秒不到两帧）");
    if (host && host->metal_hud_requested) ok = ok && strstr(text, "Metal HUD 已请求");
    else ok = ok && !strstr(text, "Metal HUD 已请求");
    printf("%s：期望 FPS、内存与慢帧口径显示 → %s\n", label, ok ? "显示" : "缺失");
    if (!ok) { printf("✗ t_hud：%s，输出：%s\n", label, text); fails++; }
}
static int target(void) {
    char t[4096]; s2a_hud_host h = {.thermal = "单测", .build_id = "t_hud", .battery_rate_pm = NAN, .battery_level = -1};
    s2a_hud_text(t, sizeof t, &h);
    const char *p = strstr(t, "目标 "); int v = -1;
    if (p && !strncmp(p + strlen("目标 "), "未取得", strlen("未取得"))) v = 0; else if (p) sscanf(p, "目标 %d", &v);
    return v;
}
int main(void) {
    static const struct { int mode; const char *cap; int gl, nv, want; const char *what; } c[] = {
        {S2A_GFX_DIRECT, "120", 0, 60, 120, "GL 交换间隔 0 + 上限 120 → 120（不是 nvn_game_fps 缺省 60）"},
        {S2A_GFX_DIRECT, "30", 60, 60, 30, "GL 交换间隔 1 + 上限 30 → 30"},
        {S2A_GFX_DIRECT, "120", 60, 60, 60, "GL 交换间隔 1 + 上限 120 → 60（上限不解除原生锁）"},
        {S2A_GFX_DIRECT, "60", 30, 60, 30, "GL 交换间隔 2 + 上限 60 → 30"},
        {S2A_GFX_DIRECT, NULL, 0, 60, 0, "GL 交换间隔 0 + 不限 → 未取得"},
        {S2A_GFX_DIRECT, "0", 0, 60, 0, "GL 交换间隔 0 + S2A_FPS=0 → 未取得"},
        {S2A_GFX_DIRECT, "30abc", 0, 60, 0, "GL 交换间隔 0 + 非法 30abc → 未取得（不按 atoi 读成 30）"},
        {S2A_GFX_DIRECT, "120", -1, 60, 60, "非 GL 直连（NVN 回 60）+ 上限 120 → 60"},
        {S2A_GFX_DIRECT, "30", -1, 60, 30, "非 GL 直连（NVN 回 60）+ 上限 30 → 30"},
        {S2A_GFX_DIRECT, "120", -1, 30, 30, "非 GL 直连（NVN 回 30）+ 上限 120 → 30"},
        {S2A_GFX_GENERIC, "120", 0, 60, 120, "通用模式（读不到游戏帧率）+ 上限 120 → 120"},
        {S2A_GFX_GENERIC, NULL, 0, 60, 0, "通用模式 + 不限 → 未取得"},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        g_mode = c[i].mode; g_gl = c[i].gl; g_nv = c[i].nv;
        if (c[i].cap) setenv("S2A_FPS", c[i].cap, 1); else unsetenv("S2A_FPS");
        int got = target(); printf("%s 期望 %d → 真结果 %d\n", c[i].what, c[i].want, got); if (got != c[i].want) fails++;
    }
    s2a_hud_host h = {.thermal = "单测", .build_id = "t_hud", .battery_rate_pm = NAN, .battery_level = -1};
    check_display("官方 HUD 未请求，1 帧 / 内存可读", &h, 1, 1);
    h.metal_hud_requested = 1;
    check_display("官方 HUD 已请求，1 帧 / 内存可读", &h, 1, 1);
    check_display("官方 HUD 已请求，空帧环 / 内存读取失败", &h, 0, 0);
    check_display("宿主信息缺失，空帧环 / 内存读取失败", NULL, 0, 0);
    // 两本账（11-13）：期望手写——桩的实占 123 MB = 0.12 GB → 「0.1」；GPU 已分配 5.5 GiB = 5905580032 字节 → 简版「5.5 GB」、开发浮层「5632 MB」；
    //   没有直连层（0）时简版只写实占「0.1 GB」、不出 GPU
    {
        char b[256], t[4096]; g_mem_ok = 1; s2a_hud_host hh = {.thermal = "单测", .build_id = "t_hud", .battery_rate_pm = NAN, .battery_level = -1};
        g_gpu_alloc = 5905580032ull; s2a_hud_brief(b, sizeof b); s2a_hud_text(t, sizeof t, &hh);
        int ok1 = strstr(b, "进程 0.1 GB · GPU 5.5 GB") != NULL, ok2 = strstr(t, "GPU 已分配 5632 MB") != NULL;
        printf("GPU 已分配 5.5 GiB：简版期望含「进程 0.1 GB · GPU 5.5 GB」→ %s；开发浮层期望含「GPU 已分配 5632 MB」→ %s\n", b, ok2 ? "含" : "不含");
        if (!ok1 || !ok2) fails++;
        g_gpu_alloc = 0; s2a_hud_brief(b, sizeof b);
        int ok3 = strstr(b, "· 0.1 GB") && !strstr(b, "GPU");
        printf("没有直连层：简版期望只写「0.1 GB」→ %s\n", b); if (!ok3) fails++;
    }
    {   // 只看帧率（#2207），没补帧：期望手写——帧环里 3 帧都在 1 秒内 → 「3 帧」
        char b[64]; mach_timebase_info_data_t tb; mach_timebase_info(&tb); uint64_t now = mach_absolute_time() * tb.numer / tb.denom;
        int s2a_hud_brief_fps(char *, size_t);
        for (int i = 0; i < 3; i++) atomic_store(&g_s2a_hud_ft[i], now - (uint64_t)i * 1000000ull);
        atomic_store(&g_s2a_hud_fn, 3); s2a_hud_brief_fps(b, sizeof b);
        printf("只看帧率：帧环 3 帧（1 秒内），期望「3 帧」→ %s\n", b); if (strcmp(b, "3 帧")) fails++;
    }
    {   // 补帧（11-14）：补出的帧真上了屏就算进屏上帧率。期望手写：1 秒内补出 24 帧、游戏帧环里 1 帧 → 「屏上 25 帧（游戏 1）」；没补帧时不出「屏上」（上一段已核）
        void s2a_hud_mid_frame(void); char b[256];
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        atomic_store(&g_s2a_hud_ft[0], mach_absolute_time() * tb.numer / tb.denom); atomic_store(&g_s2a_hud_fn, 1);
        for (int i = 0; i < 24; i++) s2a_hud_mid_frame();
        s2a_hud_brief(b, sizeof b); int ok = strstr(b, "屏上 25 帧（游戏 1）") != NULL;
        printf("补帧：补出 24 帧 + 游戏 1 帧，简版期望含「屏上 25 帧（游戏 1）」→ %s\n", b); if (!ok) fails++;
        // 只看帧率（#2207）：同一帧环，期望「屏上 25 帧（游戏 1）」且不带内存（没有「GB」、没有「·」）
        int s2a_hud_brief_fps(char *, size_t); s2a_hud_brief_fps(b, sizeof b); ok = !strcmp(b, "屏上 25 帧（游戏 1）");
        printf("只看帧率（补帧）：期望「屏上 25 帧（游戏 1）」→ %s\n", b); if (!ok) fails++;
    }
    {   // 直连出图尺寸（nvn_be_present_out_size → s2a_direct_out_size）：0x0 未取得，1920x1080 掌机 / 2048x1152 主机如实显示
        char t[4096]; s2a_hud_host hh = {.thermal = "单测", .build_id = "t_hud", .battery_rate_pm = NAN, .battery_level = -1};
        g_mode = S2A_GFX_DIRECT;
        g_dir_w = 0; g_dir_h = 0; s2a_hud_text(t, sizeof t, &hh);
        int ok0 = strstr(t, "画幅 未取得出图尺寸") != NULL;
        printf("直连出图 0x0：期望含「画幅 未取得出图尺寸」→ %s\n", ok0 ? "是" : "否"); if (!ok0) fails++;
        g_dir_w = 1920; g_dir_h = 1080; s2a_hud_text(t, sizeof t, &hh);
        int ok1 = strstr(t, "画幅 1.778（1920x1080）") != NULL;
        printf("直连出图 1920x1080：期望含「画幅 1.778（1920x1080）」→ %s\n", ok1 ? "是" : "否"); if (!ok1) fails++;
        g_dir_w = 2048; g_dir_h = 1152; s2a_hud_text(t, sizeof t, &hh);
        int ok2 = strstr(t, "画幅 1.778（2048x1152）") != NULL;
        printf("直连出图 2048x1152：期望含「画幅 1.778（2048x1152）」→ %s\n", ok2 ? "是" : "否"); if (!ok2) fails++;
    }
    if (fails) printf("✗ t_hud：%d 项不符\n", fails); else printf("✓ t_hud：浮层目标 = min(游戏帧率, 上限)，GL 不锁时 = 上限，非法上限不进浮层，全过\n");
    return fails != 0;
}
