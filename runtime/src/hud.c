// 开发浮层（S2A_HUD=1，缺省关；iOS 设置页「开发浮层」、Mac 启动器开关都落到这个环境变量）：给手动测试的开发者看局内读数。
// 界面每 0.5 秒取诊断文字；无论是否请求 Metal HUD，都显示 S2Arm 自采的 FPS 与内存。
// 浮层画在系统界面层（UIView / NSView），不进游戏画面：截图、存帧、自动玩家看图都从 Metal 贴图读回（gal_present.m screenshot、
//   host_adapt.m s2a_direct_screenshot），不含视图叠层。读不到的项写明为什么读不到（如「iOS 不给」），不猜、不写空的「无」。
// 唯一新加的逐帧开销：浮层开着时，每上屏一帧记一个时间戳进 10 秒环（ap_host.c s2a_frame_presented）；关着时只读一个缓存的开关
#include "native.h"
#include "../include/s2a_hud.h"
#include "../include/s2a_native.h"
#include "../include/s2a_gfx_mode.h"
#include "../include/s2a_ui_text.h"   // 齿轮菜单「性能浮层」那一行：中文原宏 + _EN，S2A_UI 按界面语言取（#316）
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

// GPU 已分配（直连层 graphics/nvn nvn_mtl.m 强定义）：Metal 设备 currentAllocatedSize 减去零拷贝缓冲——贴图、我们自己分配的缓冲记在这里；
//   内存池的零拷贝缓冲就是游戏内存（已在进程实占里），Metal 按每个缓冲全长、重叠的块重复记，不算进来（#1827）。没有直连层 / 设备还没建 = 0（浮层就只写进程实占）
__attribute__((weak)) uint64_t s2a_gpu_alloc_bytes(void) { return 0; }
// 上屏时间戳环在 ap_host.c（逐帧路径只依赖它自己；只链 ap_host 的单测不用带上本文件）：写方只有上屏线程，界面线程按序号读，读到被覆盖的就停
enum { HR = 2048 };
extern _Atomic uint64_t g_s2a_hud_ft[HR], g_s2a_hud_fn;
#define g_ft g_s2a_hud_ft
#define g_fn g_s2a_hud_fn
static uint64_t mono_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }

// 声音：S2AOutputDiag.cs 的 10 秒汇报线程每次报一回各输出会话的累计真缺口（ms），另报一次本窗渲染丢周期（session = -1）；浮层显示各会话之和
static _Atomic int64_t g_gap_ms[8], g_skip; static _Atomic int g_audio_seen;
void s2a_hud_session_reset(void) {
    for (int i = 0; i < 8; i++) atomic_store(&g_gap_ms[i], 0);
    atomic_store(&g_skip, 0); atomic_store(&g_audio_seen, 0);
}
void s2a_hud_audio(int session, int64_t gap_ms_total, int64_t skipped) {
    if (session < 0) { atomic_fetch_add(&g_skip, skipped); return; }   // -1：渲染器整体的丢周期（不分会话）
    if (session >= 8) return;
    atomic_store(&g_gap_ms[session], gap_ms_total); atomic_store(&g_audio_seen, 1);
}

// 帧率：近 1 秒的上屏帧数与近 10 秒的慢帧占比。目标帧率能读到（直连的游戏节拍与 S2A_FPS 上限合成的实际生效帧率，见 hud_target）时慢帧 = 间隔 > 1.5 × 目标间隔；
//   读不到时按近 10 秒间隔的中位数算，行里写明口径
// 帧率上限：与 graphics/nvn nvn_fps.c nvn_fps_parse、通用层 FramePacingPolicy 同口径，只认整串 0 或 24–1000 的十进制整数（别的值那两处起局就报红退出），
//   不用 atoi（"30abc" 不能只进浮层）；浮层不链图形层，这里照抄一份
static int cap_fps(void) {
    const char *e = getenv("S2A_FPS"); int v = 0;
    if (!e || !*e) return 0;
    for (const char *p = e; *p; p++) { if (*p < '0' || *p > '9' || p - e >= 4) return 0; v = v * 10 + (*p - '0'); }
    return v >= 24 && v <= 1000 ? v : 0;
}
// 目标 = 实际生效周期 max(游戏间隔周期, 1/上限) 对应的帧率，即 min(游戏帧率, 上限)（#619 审计 F-hud-cap-above-game）。
//   游戏帧率：GL 前端回报它自己的（nvn_gl_game_fps，graphics/nvn/gfx/gl/egl.m：交换间隔折成帧率，0 = 交换间隔 0、游戏不锁垂直同步；-1 = 本局没走 GL），
//   其余直连前端取 nvn_game_fps；通用模式读不到游戏帧率。没有原生锁 / 读不到时目标就是上限（GL 交换间隔 0 + 上限 120 = 120，不是缺省 60）；上限也没有 = 0「未取得」
// 帧率被补丁 / 我方帧率表抬高了没有：runtime/src/fps.c 强定义覆盖；只链本文件的单测用缺省 1 = 当作抬高了（上限照常算进目标）
__attribute__((weak)) int s2a_fps_raised(void) { return 1; }
static int hud_target(void) {
    int cap = cap_fps(), game = 0;
    if (s2a_gfx_mode() == S2A_GFX_DIRECT) {
        int (*gl)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "nvn_gl_game_fps"), (*nv)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "nvn_game_fps");
        int g = gl ? gl() : -1; game = g >= 0 ? g : nv ? nv() : 0;
    }
    if (game > 0 && !s2a_fps_raised()) return game;   // 帧率没被抬高：上限不压游戏原生出图率（graphics/nvn/gfx/nvn/nvn_fps.c，11-25 #502），目标就是游戏帧率
    return game <= 0 ? cap : cap && cap < game ? cap : game;
}
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
// 60 帧降档原因（runtime/src/fps.c：热状态危急 / 低电量模式时自动按关生效）：弱定义缺省 0，fps.c 的强定义覆盖（单测只链本文件时用缺省）
__attribute__((weak)) int s2a_fps_cap_reason(void) { return 0; }
__attribute__((weak)) int s2a_ui_zh(void) { return 1; }   // 界面语言（s2a_ui_text.h）：iOS ios/app.m、Mac macos/front_mac.m 强定义；别的平台 / 只链本文件的单测按中文
static const char *cap_note(void) { int r = s2a_fps_cap_reason(); return r == 1 ? "　60 帧降档：热" : r == 2 ? "　60 帧降档：低电量" : r == 3 ? "　60 帧：同局 A/B 测原帧率" : r == 4 ? "　60 帧：档位梯子压回原帧率" : ""; }
static void line_fps(char *o, size_t n) {
    uint64_t end = atomic_load_explicit(&g_fn, memory_order_acquire), now = mono_ns();
    static uint64_t itv[HR]; int k = 0; uint64_t f1 = 0, prev = 0;
    for (uint64_t i = end; i > 0 && end - i < HR - 1; i--) {
        uint64_t t = atomic_load_explicit(&g_ft[(i - 1) % HR], memory_order_relaxed);
        if (!t || now - t > 10000000000ull) break;
        if (now - t <= 1000000000ull) f1++;
        if (prev) itv[k++] = prev - t;
        prev = t;
    }
    int tgt = hud_target();   // 有效目标同时决定显示和慢帧阈值
    char t[48]; if (tgt) snprintf(t, sizeof t, "%d", tgt); else snprintf(t, sizeof t, "未取得");
    char lead[160];
    snprintf(lead, sizeof lead, "帧率 %llu / 目标 %s%s", (unsigned long long)f1, t, cap_note());
    if (!k) { snprintf(o, n, "%s　慢帧 未测（近 10 秒不到两帧）", lead); return; }
    uint64_t lim; const char *how;
    if (tgt) { lim = 1500000000ull / (uint64_t)tgt; how = "> 1.5 倍目标间隔"; }
    else { static uint64_t s[HR]; memcpy(s, itv, sizeof(uint64_t) * (size_t)k); qsort(s, (size_t)k, sizeof *s, cmp_u64); lim = s[k / 2] * 3 / 2; how = "> 1.5 倍中位间隔"; }
    int slow = 0; for (int i = 0; i < k; i++) slow += itv[i] > lim;
    snprintf(o, n, "%s　慢帧 %.1f%%（近 10 秒 %d 帧，%s）", lead, 100.0 * slow / k, k, how);
}

#if TARGET_OS_IPHONE
#define NOT_GIVEN "iOS 不给"
#else
#define NOT_GIVEN "取不到"
#endif
// 整机功率：电池遥测 AppleSmartBattery / PowerTelemetryData 的 AccumulatedSystemLoad（毫瓦累加）÷ SystemLoadAccumulatorCount（采样次数），
//   与真机局整机读数同一口径（tools/score_cells.py 从 pwh.plist 读的就是它）。App 内经 IOKit 读注册表；沙盒不给 / 没电池（Mac mini）时写明原因。
//   注册表约每分钟才刷新一次（Spare-host 实测：约 60 次采样一刷），所以「累加值变了」才差分，两次刷新之间显示上一窗的均值
static void sys_power(char *o, size_t n) {
    // IOKit / CoreFoundation 都按名字取（宿主各链接配置不一定带这两个框架；取不到 = 读不到）
    typedef unsigned int io_obj;
    static struct {
        io_obj (*get_svc)(unsigned int, CFDictionaryRef); CFMutableDictionaryRef (*match)(const char *);
        CFTypeRef (*prop)(io_obj, CFStringRef, CFAllocatorRef, uint32_t);
        CFStringRef (*str)(CFAllocatorRef, const char *, CFStringEncoding); CFTypeID (*type)(CFTypeRef), (*dict_t)(void), (*num_t)(void);
        const void *(*get)(CFDictionaryRef, const void *); Boolean (*num)(CFNumberRef, CFNumberType, void *); void (*rel)(CFTypeRef);
    } f;
    static io_obj svc; static int state;   // 0 未试 1 有 -1 没有
    static CFStringRef k_tel, k_load, k_cnt;
    static int64_t l0, c0; static int have; static char shown[64];
    if (!state) {
        void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY), *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
        if (io && cf) {
            f.get_svc = (io_obj (*)(unsigned int, CFDictionaryRef))dlsym(io, "IOServiceGetMatchingService");
            f.match = (CFMutableDictionaryRef (*)(const char *))dlsym(io, "IOServiceMatching");
            f.prop = (CFTypeRef (*)(io_obj, CFStringRef, CFAllocatorRef, uint32_t))dlsym(io, "IORegistryEntryCreateCFProperty");
            f.str = (CFStringRef (*)(CFAllocatorRef, const char *, CFStringEncoding))dlsym(cf, "CFStringCreateWithCString");
            f.type = (CFTypeID (*)(CFTypeRef))dlsym(cf, "CFGetTypeID"); f.dict_t = (CFTypeID (*)(void))dlsym(cf, "CFDictionaryGetTypeID");
            f.num_t = (CFTypeID (*)(void))dlsym(cf, "CFNumberGetTypeID"); f.get = (const void *(*)(CFDictionaryRef, const void *))dlsym(cf, "CFDictionaryGetValue");
            f.num = (Boolean (*)(CFNumberRef, CFNumberType, void *))dlsym(cf, "CFNumberGetValue"); f.rel = (void (*)(CFTypeRef))dlsym(cf, "CFRelease");
        }
        if (f.get_svc && f.match && f.prop && f.str && f.type && f.dict_t && f.num_t && f.get && f.num && f.rel) {
            k_tel = f.str(NULL, "PowerTelemetryData", kCFStringEncodingUTF8); k_load = f.str(NULL, "AccumulatedSystemLoad", kCFStringEncodingUTF8);
            k_cnt = f.str(NULL, "SystemLoadAccumulatorCount", kCFStringEncodingUTF8);
            svc = f.get_svc(0, f.match("AppleSmartBattery"));
        }
        state = svc && k_tel && k_load && k_cnt ? 1 : -1;
        if (state < 0) LOG("开发浮层：读不到电池遥测（AppleSmartBattery），整机功率显示「" NOT_GIVEN "」");
    }
    int64_t ld = -1, ct = -1;
    if (state > 0) {
        CFTypeRef d = f.prop(svc, k_tel, NULL, 0);
        if (d && f.type(d) == f.dict_t()) {
            CFTypeRef a = f.get(d, k_load), c = f.get(d, k_cnt);
            if (!a || f.type(a) != f.num_t() || !f.num(a, kCFNumberSInt64Type, &ld)) ld = -1;
            if (!c || f.type(c) != f.num_t() || !f.num(c, kCFNumberSInt64Type, &ct)) ct = -1;
        }
        if (d) f.rel(d);
    }
    if (ld < 0 || ct < 0) { snprintf(o, n, "%s", NOT_GIVEN); return; }
    if (!have) { l0 = ld; c0 = ct; have = 1; snprintf(shown, sizeof shown, "测量中（遥测约每分钟一更）"); }
    else if (ct > c0) { snprintf(shown, sizeof shown, "%.2f W", (double)(ld - l0) / (double)(ct - c0) / 1000.0); l0 = ld; c0 = ct; }
    snprintf(o, n, "%s", shown);
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
// 插没插电（gfxmode.c 直连音频调度缺省按电源档分的强定义，11-14 卡顿诊断）：AppleSmartBattery 的 ExternalConnected。
//   与 sys_power 各持一份按名取（那边整机瓦数要遥测三键；这边只要外接电源一位，读不到时两边要给不同方向的兜底：浮层显示「取不到」，
//   这里没电池服务（桌面机）算非电池回实时，读得出但键坏按电池回 interactive 保 10-24 起的现状），互不影响
int s2a_mac_on_battery(void) {
    typedef unsigned int io_obj;
    static struct {
        io_obj (*get_svc)(unsigned int, CFDictionaryRef); CFMutableDictionaryRef (*match)(const char *);
        CFTypeRef (*prop)(io_obj, CFStringRef, CFAllocatorRef, uint32_t); CFStringRef (*str)(CFAllocatorRef, const char *, CFStringEncoding);
        CFTypeID (*type)(CFTypeRef), (*bool_t)(void), (*num_t)(void);
        Boolean (*bl)(CFBooleanRef); Boolean (*num)(CFNumberRef, CFNumberType, void *); void (*rel)(CFTypeRef);
    } f;
    static io_obj svc; static int tried;
    if (!tried) {
        tried = 1;
        void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY), *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
        if (io && cf) {
            f.get_svc = (io_obj (*)(unsigned int, CFDictionaryRef))dlsym(io, "IOServiceGetMatchingService");
            f.match = (CFMutableDictionaryRef (*)(const char *))dlsym(io, "IOServiceMatching");
            f.prop = (CFTypeRef (*)(io_obj, CFStringRef, CFAllocatorRef, uint32_t))dlsym(io, "IORegistryEntryCreateCFProperty");
            f.str = (CFStringRef (*)(CFAllocatorRef, const char *, CFStringEncoding))dlsym(cf, "CFStringCreateWithCString");
            f.type = (CFTypeID (*)(CFTypeRef))dlsym(cf, "CFGetTypeID"); f.bool_t = (CFTypeID (*)(void))dlsym(cf, "CFBooleanGetTypeID");
            f.num_t = (CFTypeID (*)(void))dlsym(cf, "CFNumberGetTypeID");
            f.bl = (Boolean (*)(CFBooleanRef))dlsym(cf, "CFBooleanGetValue"); f.num = (Boolean (*)(CFNumberRef, CFNumberType, void *))dlsym(cf, "CFNumberGetValue");
            f.rel = (void (*)(CFTypeRef))dlsym(cf, "CFRelease");
        }
        if (f.get_svc && f.match && f.prop && f.str && f.type && f.bool_t && f.num_t && f.bl && f.num && f.rel)
            svc = f.get_svc(0, f.match("AppleSmartBattery"));
    }
    if (!f.get_svc || !svc) return 0;   // 框架取不到 / 没电池服务（桌面机）：不算电池
    CFStringRef k = f.str(NULL, "ExternalConnected", kCFStringEncodingUTF8);
    CFTypeRef d = k ? f.prop(svc, k, NULL, 0) : NULL;
    if (k) f.rel(k);
    int on_battery = 1;   // 键读不出：按电池（interactive，10-24 起的现状），宁可保守
    if (d) {
        int64_t v = 1;
        if (f.type(d) == f.bool_t()) on_battery = !f.bl((CFBooleanRef)d);
        else if (f.type(d) == f.num_t() && f.num((CFNumberRef)d, kCFNumberSInt64Type, &v)) on_battery = !v;
        f.rel(d);
    }
    return on_battery;
}
#endif

// 能耗：本进程 CPU 能耗 + GPU 忙时间（s2a_energy_snap 差分；GPU 没有瓦数，读不到时写明）+ 整机功率（电池遥测）+ 系统发热档 + 电量与变化速率
static void line_energy(char *o, size_t n, const s2a_hud_host *h) {
    static s2a_esnap last; static int have; s2a_esnap s; char w[64] = "测量中";
    if (s2a_energy_snap(&s)) snprintf(w, sizeof w, NOT_GIVEN);
    else {
        if (have && s.wall > last.wall) {
            double sec = (s.wall - last.wall) / 1e9, cpu = (s.nj - last.nj) / 1e9 / sec, gms = s2a_gpu_busy_rate(&last, &s);   // GPU 只有忙的时间、没有瓦数（10-30 测量口径修法 5）
            if (gms >= 0) snprintf(w, sizeof w, "CPU %.2f W（GPU 忙 %.0f ms/s）", cpu, gms); else snprintf(w, sizeof w, "CPU %.2f W（GPU " NOT_GIVEN "）", cpu);
        }
        last = s; have = 1;
    }
    char sp[48]; sys_power(sp, sizeof sp);
    char b[64] = NOT_GIVEN;
    if (h && h->battery_level >= 0) {
        if (h->battery_rate_pm == h->battery_rate_pm && h->battery_rate_pm > -1000) snprintf(b, sizeof b, "%.0f%%（%+.1f%%/分）", h->battery_level * 100, h->battery_rate_pm);
        else snprintf(b, sizeof b, "%.0f%%（速率待变满 1%%）", h->battery_level * 100);
    }
    snprintf(o, n, "进程功率 %s\n整机功率 %s · 发热 %s\n电量 %s", w, sp, h && h->thermal ? h->thermal : NOT_GIVEN, b);
}

// 满屏：出图宽高比（通用与直连均取出图源矩形）对屏幕宽高比；宽屏层级
static void line_fit(char *o, size_t n, const s2a_hud_host *h) {
    void (*gsz)(int *, int *) = (void (*)(int *, int *))dlsym(RTLD_DEFAULT, s2a_gfx_mode() == S2A_GFX_DIRECT ? "s2a_direct_out_size" : "nxgal_out_size");
    int ow = 0, oh = 0; if (gsz) gsz(&ow, &oh);
    char a[64] = "未取得出图尺寸", sc[64] = "未取得屏幕尺寸";
    if (ow > 0 && oh > 0) snprintf(a, sizeof a, "%.3f（%dx%d）", (double)ow / oh, ow, oh);
    if (h && h->screen_w > 0 && h->screen_h > 0) snprintf(sc, sizeof sc, "%.3f", h->screen_w / h->screen_h);
    // 直连第三层缺省开、S2A_WIDEN_DIRECT=0 关（graphics/nvn/widen_glue.inc wd_on、host_adapt.m out_wide 同一判据）：包里有整套表但关掉时如实写，不报成「第三层」
    //   （10-26 测试设备2 马车8 直连：浮层写「第三层」、出图 1.777，核心其实只在观察 k 1.0000）
    const char *de = getenv("S2A_WIDEN_DIRECT"); int off3 = s2a_gfx_mode() == S2A_GFX_DIRECT && de && !strcmp(de, "0");
    const char *wl = s2a_widen_full() ? (!off3 ? "第三层" : s2a_widen_game() ? "第二层（直连第三层已关）" : "关（直连第三层已关）") : s2a_widen_game() ? "第二层" : "关";
    snprintf(o, n, "画幅 %s · 屏幕 %s\n宽屏 %s", a, sc, wl);
}

// 首遇现建：通用 = 管线缓存没命中现建（nxgal_pso_cache_stats 次数 / 累计，nxgal_pso_build_max_ns 最长一次）；直连 = 着色器现编次数（无耗时）
static void line_first(char *o, size_t n) {
    if (s2a_gfx_mode() == S2A_GFX_DIRECT) {
        void (*ds)(uint64_t *) = (void (*)(uint64_t *))dlsym(RTLD_DEFAULT, "s2a_dsh_stats");
        uint64_t d[5] = {0}; if (!ds) { snprintf(o, n, "首遇现建 未测（宿主未提供着色器计数）"); return; } ds(d);
        snprintf(o, n, "首遇现建 着色器现编 %llu 次（失败 %llu）　最长耗时 未提供", (unsigned long long)(d[0] + d[1]), (unsigned long long)d[1]);
        return;
    }
    void (*ps)(uint64_t *) = (void (*)(uint64_t *))dlsym(RTLD_DEFAULT, "nxgal_pso_cache_stats");
    uint64_t (*pm)(void) = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "nxgal_pso_build_max_ns");
    if (!ps) { snprintf(o, n, "首遇现建 未测（宿主未提供管线计数）"); return; }
    uint64_t p[4] = {0}; ps(p);
    char mx[24] = "未提供"; if (pm) snprintf(mx, sizeof mx, "%.1f ms", pm() / 1e6);
    snprintf(o, n, "首遇现建 管线 %llu 次（累计 %.0f ms）　最长 %s", (unsigned long long)p[1], p[2] / 1e6, mx);
}

static void line_audio(char *o, size_t n) {
    if (!atomic_load(&g_audio_seen)) { snprintf(o, n, "声音 待首个输出会话汇报（10 秒一报）"); return; }
    int64_t gap = 0; for (int i = 0; i < 8; i++) gap += atomic_load(&g_gap_ms[i]);
    snprintf(o, n, "声音 供数缺额 %lld ms · 丢周期 %lld（累计）", (long long)gap, (long long)atomic_load(&g_skip));
}

// 内存：本进程物理占用（phys_footprint）/ 系统上限（iOS = 占用 + os_proc_available_memory，即系统给本 App 的上限；Mac = 整机内存）
static void line_mem(char *o, size_t n) {
    task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT; double fp = -1, lim = -1;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) == KERN_SUCCESS) fp = vi.phys_footprint / 1048576.0;
#if TARGET_OS_IPHONE
    size_t av = os_proc_available_memory(); if (fp >= 0 && av) lim = fp + av / 1048576.0;
#else
    uint64_t m = 0; size_t sz = sizeof m; if (!sysctlbyname("hw.memsize", &m, &sz, NULL, 0)) lim = m / 1048576.0;
#endif
    char a[24] = "读取失败", b[24] = "未取得"; if (fp >= 0) snprintf(a, sizeof a, "%.0f MB", fp); if (lim > 0) snprintf(b, sizeof b, "%.0f MB", lim);
    uint64_t g = s2a_gpu_alloc_bytes();
    if (g) snprintf(o, n, "内存 %s / 上限 %s · GPU 已分配 %.0f MB（另一本账，撞上限报 OutOfMemory）", a, b, g / 1048576.0); else snprintf(o, n, "内存 %s / 上限 %s", a, b);
}

// 简版一行：近 1 秒上屏帧数 + 本进程物理内存占用（与开发浮层同源：上屏时间戳环、phys_footprint）
// 补帧：补出来的那一帧真上了屏的时刻（graphics/nvn/gfx/nvn/nvn_present_fi.inc fi_count_on 调）。浮层「帧」原先只数游戏帧，补帧开着时一直显示 30（11-14 手测）
#define MR 256
static _Atomic uint64_t g_mt[MR], g_mn;
__attribute__((visibility("default"))) void s2a_hud_mid_frame(void) { uint64_t i = atomic_fetch_add(&g_mn, 1); atomic_store_explicit(&g_mt[i % MR], mono_ns(), memory_order_relaxed); }
static uint64_t mids_last_second(uint64_t now) {
    uint64_t end = atomic_load(&g_mn), c = 0;
    for (uint64_t i = end; i > 0 && end - i < MR - 1; i--) { uint64_t t = atomic_load_explicit(&g_mt[(i - 1) % MR], memory_order_relaxed); if (!t || now - t > 1000000000ull) break; c++; }
    return c;
}
static uint64_t frames_last_second(uint64_t now) {
    uint64_t end = atomic_load_explicit(&g_fn, memory_order_acquire), f1 = 0;
    for (uint64_t i = end; i > 0 && end - i < HR - 1; i--) {
        uint64_t t = atomic_load_explicit(&g_ft[(i - 1) % HR], memory_order_relaxed);
        if (!t || now - t > 1000000000ull) break;
        f1++;
    }
    return f1;
}
int s2a_hud_brief(char *out, size_t n) {
    uint64_t now = mono_ns(), f1 = frames_last_second(now);
    task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    double gb = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) == KERN_SUCCESS ? vi.phys_footprint / 1073741824.0 : -1;
    char m[64]; snprintf(m, sizeof m, "%s", S2A_UI(S2A_UI_HUD_MEM_FAIL)); uint64_t g = s2a_gpu_alloc_bytes();
    if (gb >= 0 && g) snprintf(m, sizeof m, S2A_UI(S2A_UI_HUD_MEM_GPU), gb, g / 1073741824.0); else if (gb >= 0) snprintf(m, sizeof m, "%.1f GB", gb);
    uint64_t mid = mids_last_second(now);
    if (mid) return snprintf(out, n, S2A_UI(S2A_UI_HUD_BRIEF_FI), (unsigned long long)(f1 + mid), (unsigned long long)f1, m);
    return snprintf(out, n, S2A_UI(S2A_UI_HUD_BRIEF), (unsigned long long)f1, m);
}
// 只看帧率（#2207 Mac「显示」菜单）：同简版的帧数，不带内存
int s2a_hud_brief_fps(char *out, size_t n) {
    uint64_t now = mono_ns(), f1 = frames_last_second(now), mid = mids_last_second(now);
    if (mid) return snprintf(out, n, S2A_UI(S2A_UI_HUD_FPS_FI), (unsigned long long)(f1 + mid), (unsigned long long)f1);
    return snprintf(out, n, S2A_UI(S2A_UI_HUD_FPS), (unsigned long long)f1);
}

int s2a_hud_text(char *out, size_t n, const s2a_hud_host *h) {
    char l[8][512];
    int metal = h && h->metal_hud_requested;
    line_fps(l[0], sizeof l[0]);
    line_energy(l[1], sizeof l[1], h);
    line_fit(l[2], sizeof l[2], h);
    int direct = s2a_gfx_mode() == S2A_GFX_DIRECT;
    snprintf(l[3], sizeof l[3], "S2Arm · %s %s", direct ? "直连" : "通用", s2a_gfx_mode_brief());
    line_first(l[4], sizeof l[4]);
    line_audio(l[5], sizeof l[5]);
    line_mem(l[6], sizeof l[6]);
    snprintf(l[7], sizeof l[7], "包 %s%s", h && h->build_id && *h->build_id ? h->build_id : "未记录", metal ? " · Metal HUD 已请求" : "");
    return snprintf(out, n, "%s\n%s\n%s\n\n%s\n%s\n\n%s\n%s\n%s", l[3], l[7], l[0], l[1], l[2], l[4], l[5], l[6]);
}
