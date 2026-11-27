// NVN interface layer: takes over the nvnBootstrapLoader imported by main; the NVN driver in sdk is not executed at all.
// All 523 NVN names return counting thunks (nvn_thunks.S), and the thunks forward to the implementations here:
//   - those with semantics (device / memory pools / textures and samplers / queue / window / sync / events / command buffer memory) are implemented per NVN semantics, and the objects are real;
//   - those that only change GPU state and have no CPU-visible result (state objects, bindings, draws, clears...) are recorded as commands and replayed at submit;
//   - the rest (the 307 that main only loads but never calls + those without a prototype in the header) go to nvn_unimpl_entry: reports a failure and prints the name when called.
// GPU address = CPU address (memory pools use the real memory given by the game, also built as zero-copy MTLBuffer). Commands are recorded into the command memory given by the game and replayed at submit into a
// Metal command buffer (nvn_mtl.m): buffer copies / uniform updates go through blit (GPU ordering), event value writes / counters are written after the GPU completes;
// sync object = submit fence value, and waits are released only after the GPU completes.
// Every implementation gets a compile-time signature check against the prototypes in nvn_protos.h (NVNboolean parameters are received as uint32_t, only the low 8 bits are looked at).
// Environment switches: DM_NVN_FRAMES=N prints the call table per frame for the first N frames (default 3); DM_NVN_TSV=path writes the per-frame call table and timing to a TSV (DM_NVN_TSV_CALLS=0 writes timing only); DM_SLOWFRAME=1 writes only the per-frame breakdown of dropped frames to the log; DM_NVN_LOG=1 logs object creation.
#include "../../host/hle.h"
#include "nvn_protos.h"
#include "nvn_count.h"
#include "nvn_tex.h"
#include "nvn_mtl.h"
#include "nvn_pace.h"
#include "nvn_fps.h"
#include "nvn_fstep.h"
#include "nvn_vpace.h"
#include "../../host/front.h"
#include "nvn_dirty.h"
#include "nvn_program_pins.h"
#include "nvn2.h"
#include "nvn_key.h"   // Switch 1 着色器键的控制段口径（第 2 代置零两段易变字段）
#include "nvn2_cmd.h"   // NVN2 推常量 / DLSS 的记录格式与对象布局
#include "nvn_hmap.h"   // 虚拟池按池指针查（g_vph）
#include "nvn_drvscan.h"   // nvn_drv_nvrm_recode：NvColorFormat 换成游戏自带驱动的编码
#include <dlfcn.h>
#include "../../../../runtime/include/s2a_lowva.h"   // #1143：安卓的虚拟池预留记进低区账（vpool_lv）
#include <mach/mach_time.h>
#include <CommonCrypto/CommonDigest.h>
#include "../../../../runtime/src/snapfmt.h"   // 即时存档：布局指纹登记（hle_snap_fps）与按字段转的读入（snap_fread）
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#if TARGET_OS_OSX
#define DM_MEM_CAP_DEFAULT_GB 24
#else
#define DM_MEM_CAP_DEFAULT_GB 0
#endif
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/thread_policy.h>
#include <assert.h>
#include <stdatomic.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#if defined(__ANDROID__) || defined(__OHOS__)
#include "s2a_vp_alias.h"
#include "../runtime/vm/backing.h"
// svc_mem.c 的锁序断言与背板引用入口（只在 Android 有定义）。
int s2a_mem_g_mu_held(void);
void s2a_mem_lockorder_vp_enter(void);
// #48 A3/A4（android/native/system_tuning.c，只在 Android 有定义）：省电档对最终周期取
//   max(原周期, 33.3 ms)；登记最终周期给 Surface.setFrameRate 刷新率投票。
uint64_t s2a_android_limit_frame_period(uint64_t period_ns);
void s2a_android_present_note(uint64_t period_ns);
// android/native/gpu_turbo.c：每次请求上屏时登记一帧，供安卓 GPU 提频判定。
void s2a_android_frame_note(void);
#endif

// ---------- Slot table, thunks, unimplemented stubs ----------
typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[NVN_NFUNC];                 // The thunk accesses it by i*16; the layout must not change
_Static_assert(sizeof(nvn_slot_t) == 16, "the trampolines are addressed in 16-byte steps");
extern void *nvn_thunk[NVN_NFUNC], *nvn_thunk_fast[NVN_NFUNC];
// Call counts (the counting trampolines nvn_thunk, nvn_thunks.S) only when asked for: DM_NVN_COUNT=1, or the per-frame table DM_NVN_TSV with its call rows (DM_NVN_TSV_CALLS=0 writes only
// timing). Each count is an atomic add on a cache line every recording thread shares; otherwise the game gets the uncounted trampolines nvn_thunk_fast and the per-frame call lines are
// not written (ported from nxnative e250bb8b, MIT: there the trampolines of nvnBufferGetAddress / BindUniformBuffer took about 25 times the time of the functions they lead to)
static int nvn_counting(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_COUNT"), *c = getenv("DM_NVN_TSV_CALLS"); v = (e && atoi(e)) || (getenv("DM_NVN_TSV") && !(c && !atoi(c))); }
    return v;
}
extern const char *nvn_names[NVN_NFUNC];
extern void nvn_unimpl_entry(void);
extern void nvn_prof_entry(void);
extern void nvn_big_entry(void);
void *nvn_big_real[NVN_NFUNC];            // the real implementation of functions that run on the big host stack (nvn_entry.S nvn_big_entry)
void *nvn_real[NVN_NFUNC];               // DM_NVN_PROF: the real implementation called by the timing wrapper
_Atomic uint64_t nvn_prof_ticks[NVN_NFUNC]; // DM_NVN_PROF: cumulative time of each function (cntvct counts)
static uint8_t g_unimpl_seen[NVN_NFUNC];
// S2Arm 诊断（11-13 金手指堆偏移）：DM_NVN_PROF=1 + S2A_NVN_QLOG=<文件>：计时包装在调用后记每次「尺寸 / 对齐 / 整数」类查询的
//   名字、游戏返回地址、返回值与入参（GetInteger 记写回的值），与 Mac 上跑游戏自带驱动时同一调用点的答复对比（runtime/src/nvos_trace.c 调用点陷入）
int g_nvn_qlog; static int g_qlog_fd = -1;
void s2a_where(uint64_t a, char *out, size_t n) __attribute__((weak));   // runtime/src/crash.c：地址 → 模块+偏移；弱声明：安卓新层诊断链接不带 crash.c，取不到就写原始地址
void nvn_qlog_c(uint64_t i, uint64_t lr, uint64_t ret, uint64_t in1, uint64_t in2) {
    if (i >= NVN_NFUNC || g_qlog_fd < 0) return;
    const char *n = nvn_names[i];
    int integer = strstr(n, "GetInteger") != NULL;
    if (!strstr(n, "Size") && !strstr(n, "Alignment") && !strstr(n, "Granularity") && !integer && !strstr(n, "GetMemory")) return;
    char w[64], line[256]; if (s2a_where) s2a_where(lr, w, sizeof w); else snprintf(w, sizeof w, "%llx", (unsigned long long)lr);
    long long v = integer && in2 ? (long long)*(const int *)in2 : (long long)ret;
    int k = snprintf(line, sizeof line, "%s\t%s\t%llx\t%llx\t%lld\n", n, w, (unsigned long long)in1, (unsigned long long)in2, v);
    if (k > 0) (void)!write(g_qlog_fd, line, (size_t)k);
}

static const char *nvn2_red_why(const char *name);   // 「NVN2 入口全量」段：仍是未实现桩的 NVN2 入口为什么做不了（没有就是 NULL）
uint64_t nvn_unimpl_c(nvn_slot_t *slot, uint64_t x0, uint64_t x1, uint64_t x2) {
    unsigned i = (unsigned)(slot - nvn_slot);
    if (!g_unimpl_seen[i]++) { const char *why = nvn2_red_why(nvn_names[i]);
        LOG("【红】 NVN unimplemented function called: %s (x0=%#llx x1=%#llx x2=%#llx)%s%s", nvn_names[i], x0, x1, x2, why ? "：" : "", why ? why : "");
        void s2a_direct_fail(const char *why); char w[160]; snprintf(w, sizeof w, "NVN 未实现函数被调用：%s", nvn_names[i]); s2a_direct_fail(w); }   // S2Arm：§11.3 直连异常（打红行，不切换）
    return 0;
}

typedef struct { const char *name; void *fn; } nvn_impl;
#if defined(__ANDROID__) || defined(__OHOS__)
#define NVN_IMPL_SECTION __attribute__((used, section("swnvn")))
#else
#define NVN_IMPL_SECTION __attribute__((used, section("__DATA,swnvn")))
#endif
#define NVN(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                             \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \
    NVN_IMPL_SECTION static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)
// Newer interfaces without prototypes: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \
    static ret h_##name(__VA_ARGS__);                                                             \
    NVN_IMPL_SECTION static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)

static int g_log;
static _Atomic uint64_t g_serial_n;
uint64_t nvn_new_serial(void) { return ++g_serial_n; }
uint64_t nvn_serial_now(void) { return atomic_load(&g_serial_n); }   // 即时存档
// 帧回放（nvn_frame_rec.inc）：录像里的贴图 / 程序都带录时的序号；回放进程自己新建的对象（宿主内部程序、清屏用的贴图）从 1 编号，会和它们撞上，
//   按序号查到别的对象（异度3 f5500：渲染目标的序号查回一个宿主内部程序，tex_extent_at 抛 NvProg parentTexture 异常、回放中止）。回放先把计数器抬到 floor 以上
void nvn_serial_floor(uint64_t floor) { uint64_t c = g_serial_n; while (c < floor && !atomic_compare_exchange_weak(&g_serial_n, &c, floor)) {} }
static unsigned g_nsem;                          // Number of semantic implementations (filled by nvn_init_slots)
#define OLOG(...) do { if (g_log) LOG("[nvn] " __VA_ARGS__); } while (0)

static void nvn_init_slots(void) {
    static int done; if (done) return; done = 1;
    g_log = getenv("DM_NVN_LOG") != NULL;
    unsigned long sz;
    const nvn_impl *im = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
    unsigned nim = im ? (unsigned)(sz / sizeof *im) : 0, hit = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) nvn_slot[i].fn = (void *)nvn_unimpl_entry;
    for (unsigned k = 0; k < nim; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], im[k].name)) i++;
        if (i == NVN_NFUNC) die("NVN implementation %s is not in main's name table", im[k].name);
        nvn_slot[i].fn = im[k].fn; hit++;
    }
    g_nsem = hit;
}

// 即时存档恢复（跨版本换算交出的宿主指针）：本版里这个 NVN 函数名对应的 thunk 地址（counting = 计数版），没有 = 0
uint64_t nvn_snap_proc(const char *name, int counting) {
#if defined(__ANDROID__) || defined(__OHOS__)
    (void)name; (void)counting; return 0;
#else
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (!strcmp(nvn_names[i], name)) return (uint64_t)(uintptr_t)(counting ? nvn_thunk[i] : nvn_thunk_fast[i]);
    return 0;
#endif
}
static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) {
#if defined(__ANDROID__) || defined(__OHOS__)
            return nvn_slot[i].fn;   // Android 直接返回强类型实现，避免 Apple 汇编 thunk
#else
            { void s2a_snap_handout_id(uint64_t, const char *) __attribute__((weak)); int c = nvn_counting(); void *fn = c ? nvn_thunk[i] : nvn_thunk_fast[i];
              int s2a_snap_mode(void) __attribute__((weak));
              if (s2a_snap_handout_id && s2a_snap_mode && s2a_snap_mode()) { char id[128]; snprintf(id, sizeof id, "nvn:%s:%c", name, c ? 'c' : 'f'); s2a_snap_handout_id((uint64_t)(uintptr_t)fn, id); }
              return fn; }   // 即时存档：登记交给游戏的宿主指针
#endif
        }
    LOG("【红】 NVN address lookup of an unknown name: %s", name ? name : "(null)");
    return NULL;
}

// nvnBootstrapLoader is imported by main (defined in sdk); after it is forcibly taken over, the NVN driver in sdk is no longer referenced
HLE_FORCE("nvnBootstrapLoader", "the NVN graphics API is replaced wholesale with a host null device (Metal behind it); the SDK's NVN driver does not run",
          void *, h_nvnBootstrapLoader, const char *name) { return nvn_lookup(name); }

// ---------- Boolean parameters: Switch callers do not extend them, only look at the low 8 bits ----------
#define B(x) ((x) & 0xff)

#include "nvn_obj.h"
#include "nvn_nvx.h"

// ---------- Device ----------
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_x)); }   // 连 NVX 预留区间一起清（nvn_nvx.h）
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceBuilderGetFlags, int, const NVNdeviceBuilder *b) { return (int)((const dev_builder_t *)b)->flags; }
// 窗口原点 / 深度模式是「每个设备」的状态（nvnDeviceSetWindowOriginMode / SetDepthMode 都带 device），后端（视口翻转、绕序、上屏翻转、顶点着色器入口）只有一份全局。
// 后端跟「当前设备」：第一个初始化的设备；之后谁的队列提交 / 上屏就切到谁（nvnQueueSubmitCommands / nvnQueuePresentTexture，持 g_sub_mu 切，编码中途不变）。
// 第二个设备只初始化（带 NVN 缺省：左下原点、near = -w）不抢当前设备——11-03 弹丸论破V3：主设备开局设左上 + near=0，开局约 11 秒游戏再初始化一个设备、不设模式，
//   旧代码 DeviceInitialize 直接把全局改回左下：此后主设备的绘制视口全翻、上屏再翻，直接投影上屏的 3D 正、渲染目标当纹理采样再合成的界面层上下颠倒。
static NVNdevice *_Atomic g_cur_dev;
extern int s2a_nvn_window_origin_mode, s2a_nvn_depth_mode;
static void dev_apply(const dev_t_ *o) {
    s2a_nvn_window_origin_mode = (int)o->origin; s2a_nvn_depth_mode = (int)o->depth_mode;
    nvn_be_set_origin_lower_left(o->origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT); nvn_be_set_depth_zero(o->depth_mode != 0);
}
static int dev_ok(const NVNdevice *d) { return d && ((const dev_t_ *)d)->magic == MAGIC('D', 'E', 'V', 0); }
static void dev_follow(NVNdevice *d) {   // 持 g_sub_mu 调：后端切到这个设备的约定
    if (!dev_ok(d) || atomic_load(&g_cur_dev) == d) return;
    NVNdevice *was = atomic_exchange(&g_cur_dev, d); dev_apply((const dev_t_ *)d);
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("[nvn] backend follows device %p (was %p): window origin %s, depth mode %s", (void *)d, (void *)was,
        ((const dev_t_ *)d)->origin ? "upper left" : "lower left", ((const dev_t_ *)d)->depth_mode ? "near=0" : "near=-w");
}
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    o->origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;   // NVN's default; every game so far switched to upper-left right away, Link's Awakening keeps it
    o->depth_mode = NVN_DEPTH_MODE_NEAR_IS_MINUS_W;   // NVN's default depth mode: near = -w (GL)
    NVNdevice *cur = atomic_load(&g_cur_dev);
    if (!dev_ok(cur) || cur == d) { atomic_store(&g_cur_dev, d); dev_apply(o); }
    else LOG("[nvn] another NVN device %p initialized while %p is current: it keeps its own window origin / depth mode, the backend switches when its queue submits", (void *)d, (void *)cur);
    nvn2_device_init();   // Switch 2: NVN2's GOB order and log line (nvn2.c); nothing on Switch 1
    LOG("[nvn] DeviceInitialize %p flags=%#x", (void *)d, o->flags);   // 地址常打（一局一两行）：金手指指针链落点对照用
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; NVNdevice *e = d; atomic_compare_exchange_strong(&g_cur_dev, &e, NULL); }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)nvn_lookup(name);
}

// Device properties: values measured on Tegra X1 (nnheaders nvn_DeviceConstantsNX.h, obtained by calling nvnDeviceGetInteger item by item on SMO)
static const int g_devinfo[] = {
    // API version: 55.15 here, 53.313 for SDK 10 and older (nvn_api below; the profile's device_api overrides): SMBW (SDK 16) requires major == 55 and minor >= 15 (main 0x5a7b48); Super Mario 3D World + Bowser's Fury (SDK 10) requires major == 53 and minor >= 313 (main+0x711c88),
    // and on a mismatch it creates no device and leaves the device pointer null, which crashes in nnMain right after. DM_NVN_API=major.minor overrides
    [0x00] = 55, [0x01] = 15,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32,
};
// The NVN API version the game's own driver would report follows its SDK: games check it and create no device on a mismatch. SDK 10 and older are API 53 (Super Mario 3D World 10.4,
// Age of Calamity 9.6, Link's Awakening 8, Odyssey 7.3, Three Houses 6.5); SDK 11 and later take 55.15
__attribute__((weak)) int dm_sdk_major(void) { return 0; }   // loader.c; graphics unit tests have no loader
__attribute__((weak)) int dm_glslc_gpu_code(int *major, int *minor) { *major = *minor = 0; return 0; }   // hle_glslc.c: the game's own GLSLC's GPU code version
// host_adapt.m: what the game's own driver in its sdk module reports (nvn_drvscan.h). Preferred over the SDK guess: within one SDK line the minor differs
// (Raiden V, SDK 6.5, wants exactly 53.310; SDK 8.2 drivers say 53.312, 10.4 53.313, 11.4 53.314, 12.3 55.8, 13.3 55.12, 15 and later 55.15)
__attribute__((weak)) int dm_nvn_driver_api(int *major, int *minor) { *major = *minor = 0; return 0; }
// The same driver's value for one device-info item when that case returns a constant (host_adapt.m; 0 = not known). Used for the GPU code version range 0x2E–0x31:
//   Mario + Rabbids Sparks of Hope (SDK 13.3.5, driver says 1.5–1.16) checks each of its 1120 shaders against it, and with the table's SMO-era 1.14 it refused the compute shader
//   compute_quarterdepth_minz_groundheight, left its resource null and crashed at main+0x963c28 ("no program bound before the draw" just before)
__attribute__((weak)) int dm_nvn_driver_item(int w, int *out) { (void)w; (void)out; return 0; }
static void nvn_api(int api[2]) {
#ifdef DM_GAME_NVN_API_MAJOR
    api[0] = DM_GAME_NVN_API_MAJOR; api[1] = DM_GAME_NVN_API_MINOR; LOG("[nvn] device API %d.%d (game profile device_api)", api[0], api[1]);
#else
    if (dm_nvn_driver_api(&api[0], &api[1])) { LOG("[nvn] device API %d.%d (the game's own driver)", api[0], api[1]); return; }
    int m = dm_sdk_major(); api[0] = DM_SDK_OLD(m) ? 53 : g_devinfo[0]; api[1] = DM_SDK_OLD(m) ? 313 : g_devinfo[1]; LOG("[nvn] device API %d.%d (SDK %d)", api[0], api[1], m);
#endif
}
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    if (nvn2_on() && nvn2_device_integer(w, out)) { OLOG("DeviceGetInteger %#x -> %d (NVN2)", w, *out); return; }   // Switch 2: NVN2's own values (nvn2.c); unhandled items fall to the X1 table
    // 0x5F separate sampler bindings were not measured on SMO: use 32 as a fallback, and prefer the game's driver when recognized; 0x60/0x61 report a failure and return 0
    // 0x67 (SDK 19): FINAL FANTASY TACTICS - The Ivalice Chronicles reads it before every program it loads and passes it as the alignment of its copies of the shader's control section and code
    // (main+0x49800; its allocator raises anything below 8 to 8). Not measured on hardware: 256, the alignment of shader code in a memory pool; the host reads shaders by key, so any value works
    if (w == 0x67) { *out = 256; return; }
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) { LOG("【红】 nvnDeviceGetInteger unknown item %#x, returning 0", w); *out = 0; return; }
    *out = g_devinfo[w];
    if (w == NVN_DEVICE_INFO_SEPARATE_SAMPLER_BINDINGS_PER_STAGE) { int dv; if (dm_nvn_driver_item((int)w, &dv)) *out = dv; }
    // 0x2E / 0x30, the newest GPU code version the driver runs: the table's 1.14 is SMO's SDK 7 driver, while each SDK's driver runs the code of the GLSLC in the same SDK. A game that compiles
    // at run time checks its GLSLC's version against this range first (Catherine: Full Body, SDK 8.2.1: its GLSLC writes 1.15, main+0x330dcc refused it, no built-in program was compiled
    // and the first uniform lookup read a null program, main+0x3316dc), so the host reports at least the version of the GLSLC the game carries
    if (w >= 0x2E && w <= 0x31) { int dv; if (dm_nvn_driver_item((int)w, &dv)) *out = dv; }   // the game's own driver's range first (see dm_nvn_driver_item)
    if (w == 0x2E || w == 0x30) {
        int gv[2], mj = g_devinfo[0x2E], mn = g_devinfo[0x30], dv; if (dm_nvn_driver_item(0x2E, &dv)) mj = dv; if (dm_nvn_driver_item(0x30, &dv)) mn = dv;   // the newest version as reported so far
        if (dm_glslc_gpu_code(&gv[0], &gv[1]) && (gv[0] > mj || (gv[0] == mj && gv[1] > mn))) *out = w == 0x2E ? gv[0] : gv[1];
    }
    if (w <= 1) { static int api[2] = {-1, -1}; if (api[0] < 0) { nvn_api(api); const char *e = getenv("DM_NVN_API"); if (e) sscanf(e, "%d.%d", &api[0], &api[1]); } *out = api[w]; }
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
// Android 直连的统一编译请求需要这两个真实设备事实：整帧生效，跨模块只读。
int s2a_nvn_window_origin_mode = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;   // NVN 默认
int s2a_nvn_depth_mode = NVN_DEPTH_MODE_NEAR_IS_MINUS_W;              // NVN 默认
int s2a_nvn_rast_flip = 1;   // 当前提交队列的正面光栅翻转（见 nvnQueueInitialize）；没建过队列 = 同「建队列时左下角」（NVN 设备缺省原点）。后端 nvn_mtl.m 按它定绕序
// 只改这个设备；它是当前设备（或还没有当前设备）才同步给后端（见 dev_follow）
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; NVNdevice *c = atomic_load(&g_cur_dev); if (!dev_ok(c) || c == d) { atomic_store(&g_cur_dev, d); dev_apply((const dev_t_ *)d); } LOG("[nvn] window origin %s (device %p)", m ? "upper left (as D3D / Metal)" : "lower left (GL)", (void *)d); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; NVNdevice *c = atomic_load(&g_cur_dev); if (!dev_ok(c) || c == d) { atomic_store(&g_cur_dev, d); dev_apply((const dev_t_ *)d); } LOG("[nvn] depth mode %s (device %p)", m ? "near=0 (as D3D / Metal)" : "near=-w (GL)", (void *)d); }
NVN(nvnDeviceGetWindowOriginMode, NVNwindowOriginMode, const NVNdevice *d) { return ((const dev_t_ *)d)->origin; }
NVN(nvnDeviceGetDepthMode, NVNdepthMode, const NVNdevice *d) { return ((const dev_t_ *)d)->depth_mode; }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }

// ---------- Memory pools ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) {
    NVNdevice *dev = ((pool_builder_t *)b)->dev;
    memset(b, 0, sizeof(pool_builder_x)); ((pool_builder_t *)b)->dev = dev;   // 连 NVX 原生句柄 / 预留地址一起清（nvn_nvx.h，同真机 0x4c8c50）
    ((pool_builder_t *)b)->flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
}
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; ((pool_builder_x *)b)->native = 0; }   // 清句柄：与 SetNativeHandleNVX 互斥（20.5 0x4c8c70）
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
NVN(nvnMemoryPoolBuilderGetDevice, const NVNdevice *, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->dev; }
NVN(nvnMemoryPoolBuilderGetMemory, void *, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->mem; }
NVN(nvnMemoryPoolBuilderGetSize, size_t, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->size; }
NVN(nvnMemoryPoolBuilderGetFlags, NVNmemoryPoolFlags, const NVNmemoryPoolBuilder *b) { return (NVNmemoryPoolFlags)((const pool_builder_t *)b)->flags; }
// ---------- GPU 地址空间 ----------
// 真机 NVN 的缓冲区地址是 GPU 虚拟地址：驱动按大页（64 KB）对齐把池映射进 GPU 地址空间，与池存储的 CPU 地址无关。直连层原先把 CPU 存储指针直接当缓冲区地址给游戏，只有 4 KB 对齐；
//   按「缓冲区地址 64 KB 对齐」做子分配的游戏会多耗对齐空隙。十三机兵：128 MB + 4 KB 的池存储（CPU 0x710f5ba000），在缓冲区地址上按 64 KB 对齐切 64 / 48 / 16 MB，
//   直连多耗 24 KB，16 MB 那块拿到空，顺推区起点 0 → main+0x2f9ca0 空指针崩；通用模式真驱动给同一个池的缓冲区地址是 0x2ba8b0000（11-01 原生崩溃线实测）。
// 现在每个池在下面这个窗口里另分一段 64 KB 对齐、段间隔 64 KB 的 GPU 地址：产出点（GetBufferAddress / BufferGetAddress / TextureGetTextureAddress）给它；
//   收地址的 NVN 入口、以及回放时从游戏内存里读出的地址（nvn_mtl.m：Deferred 缓冲绑定、游戏自存的存储缓冲地址、地址解析兜底）用 nvn_ga2ca 换回 CPU 地址。
//   录制与回放内部一律 CPU 地址，不变；窗口外的地址原样过（宿主自己录的命令给的就是 CPU 地址），换算可重复做。
#define GVA_LO 0xC000000000ull     // 39 位客体地址空间之上、40 位（Maxwell GPU 虚拟地址宽度）之内
#define GVA_HI 0x10000000000ull
#define GVA_ALIGN 0x10000ull
typedef struct { uint64_t g, c, n; } gva_ent;   // GPU 起点、CPU 起点、字节数；按 g 升序
static gva_ent *g_gva; static int g_ngva, g_cgva; static pthread_rwlock_t g_gva_lk = PTHREAD_RWLOCK_INITIALIZER;
static uint64_t gva_alloc(uint64_t c, uint64_t n) {
    uint64_t need = (n + GVA_ALIGN - 1) & ~(GVA_ALIGN - 1), g = GVA_LO; int at;
    if (c && c < GVA_HI && c + n > GVA_LO) { LOG("【红】 池存储 CPU %#llx+%#llx 落进 GPU 地址窗口：换算会认错，这个池的缓冲区地址退回 CPU 地址", (unsigned long long)c, (unsigned long long)n); return 0; }
    pthread_rwlock_wrlock(&g_gva_lk);
    for (at = 0; at < g_ngva && g + need + GVA_ALIGN > g_gva[at].g; at++) g = (g_gva[at].g + g_gva[at].n + 2 * GVA_ALIGN - 1) & ~(GVA_ALIGN - 1);   // 第一个放得下（含间隔）的空档
    if (g + need > GVA_HI || (g_ngva == g_cgva && !(g_gva = realloc(g_gva, (size_t)(g_cgva = g_cgva ? g_cgva * 2 : 64) * sizeof *g_gva)))) {
        pthread_rwlock_unlock(&g_gva_lk); LOG("【红】 GPU 地址窗口放不下 %#llx 字节的池（已有 %d 段）：这个池的缓冲区地址退回 CPU 地址", (unsigned long long)n, g_ngva); return 0; }
    memmove(g_gva + at + 1, g_gva + at, (size_t)(g_ngva - at) * sizeof *g_gva); g_gva[at] = (gva_ent){g, c, n}; g_ngva++;
    pthread_rwlock_unlock(&g_gva_lk);
    return g;
}
static void gva_free(uint64_t g) {
    pthread_rwlock_wrlock(&g_gva_lk);
    for (int i = 0; i < g_ngva; i++) if (g_gva[i].g == g) { memmove(g_gva + i, g_gva + i + 1, (size_t)(g_ngva - i - 1) * sizeof *g_gva); g_ngva--; break; }
    pthread_rwlock_unlock(&g_gva_lk);
}
// 池的 GPU 地址段扩到 n 字节（D-6VG6：池上伸出池尾的缓冲对象，尾部的缓冲区地址也要换得回 CPU 地址）。不碰下一段才扩（段间留的 64 KB 间隔够大多数情况）；
//   碰了就不扩、返回 0（调用方记一行）
static int gva_extend(uint64_t g, uint64_t n) {
    int ok = 0;
    pthread_rwlock_wrlock(&g_gva_lk);
    for (int i = 0; i < g_ngva; i++) if (g_gva[i].g == g) {
        if (n <= g_gva[i].n) ok = 1;
        else if (i + 1 >= g_ngva || g + n < g_gva[i + 1].g) { g_gva[i].n = n; ok = 1; }
        break; }
    pthread_rwlock_unlock(&g_gva_lk);
    return ok;
}
uint64_t nvn_ga2ca(uint64_t a) {
    if (a < GVA_LO || a >= GVA_HI) return a;
    uint64_t r = a; int lo = 0, hi;
    pthread_rwlock_rdlock(&g_gva_lk);
    hi = g_ngva - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_gva[m].g <= a) lo = m + 1; else hi = m - 1; }   // hi = 最后一个起点 ≤ a 的段
    if (hi >= 0 && a - g_gva[hi].g <= g_gva[hi].n && g_gva[hi].c) r = g_gva[hi].c + (a - g_gva[hi].g);   // c = 0：虚拟池没拿到预留段   // 段尾（a = 起点 + 字节数）也认：游戏常给区间末端
    pthread_rwlock_unlock(&g_gva_lk);
    // 换不了的报一次，带前后相邻的池段（GPU 起点 + 字节数、CPU 起点；c = 0 = 虚拟池没拿到预留段）：看得出是落在两池之间的间隔、池尾之外还是已释放的池（D-BSX8 只有地址、判不了）
    static _Atomic int once;
    if (r == a && !once++) {
        char pv[96] = "无", nx[96] = "无";
        pthread_rwlock_rdlock(&g_gva_lk);
        if (hi >= 0) snprintf(pv, sizeof pv, "%#llx+%#llx（CPU %#llx）", (unsigned long long)g_gva[hi].g, (unsigned long long)g_gva[hi].n, (unsigned long long)g_gva[hi].c);
        if (hi + 1 < g_ngva) snprintf(nx, sizeof nx, "%#llx+%#llx（CPU %#llx）", (unsigned long long)g_gva[hi + 1].g, (unsigned long long)g_gva[hi + 1].n, (unsigned long long)g_gva[hi + 1].c);
        int ng = g_ngva; pthread_rwlock_unlock(&g_gva_lk);
        LOG("【红】[nvn] GPU 地址 %#llx 不在任何池里（池已释放 / 游戏算错 / 虚拟池没有预留段）：原样交给回放（只报一次）；前一段 %s、后一段 %s，共 %d 段", (unsigned long long)a, pv, nx, ng);
    }
    return r;
}
int nvn_ga_unmapped(uint64_t a) { return a >= GVA_LO && a < GVA_HI; }   // nvn_ga2ca 之后仍在 GPU 地址窗口里 = 换不成 CPU 地址（CPU 地址不会落进窗口，见 gva_alloc），宿主上没有映射
// 单点查询：给定 CPU 地址，返回所在池的 {CPU 起点, 字节数}；不在任何池返回 0。
// 池表可能上千项（BotW 约 1760），快照接口有 cap 截断，不能用来判定“是否在池里”——那会把
// 排在 cap 之后的池误判成“不可证明”。这里直接对按 GPU 起点排序的表做一次线性定位（观测到的
// 池数最多几百到两千，单次查询几微秒；只在 compute 的 size=0 绑定与缓冲长度登记里用）。
int nvn_gva_pool_of(uint64_t cpu_addr, uint64_t *cpu_base, uint64_t *bytes) {
    if (!cpu_base || !bytes) return 0;
    pthread_rwlock_rdlock(&g_gva_lk);
    for (int i = 0; i < g_ngva; i++) {
        const uint64_t c = g_gva[i].c, n = g_gva[i].n;
        if (!c || !n || cpu_addr < c || cpu_addr - c >= n) continue;
        *cpu_base = c; *bytes = n;
        pthread_rwlock_unlock(&g_gva_lk);
        return 1;
    }
    pthread_rwlock_unlock(&g_gva_lk);
    return 0;
}

// 单点查询（GPU 地址入口）：把 GPU 地址换回 CPU 地址后复用上面的查询。
int nvn_gva_extent(uint64_t gva, uint64_t *cpu_base, uint64_t *bytes) {
    return nvn_gva_pool_of(nvn_ga2ca(gva), cpu_base, bytes);
}

int nvn_gva_snapshot(uint64_t *out, int cap) {
    pthread_rwlock_rdlock(&g_gva_lk);
    int n = g_ngva < cap ? g_ngva : cap;
    for (int i = 0; i < n; i++) { out[3 * i] = g_gva[i].g; out[3 * i + 1] = g_gva[i].c; out[3 * i + 2] = g_gva[i].n; }
    pthread_rwlock_unlock(&g_gva_lk);
    return n;
}
void nvn_gva_restore(const uint64_t *t, int n) {
    pthread_rwlock_wrlock(&g_gva_lk);
    gva_ent *e = n ? malloc((size_t)n * sizeof *e) : NULL;
    if (n && !e) n = 0;
    for (int i = 0; i < n; i++) e[i] = (gva_ent){t[3 * i], t[3 * i + 1], t[3 * i + 2]};
    free(g_gva); g_gva = e; g_ngva = g_cgva = n;
    pthread_rwlock_unlock(&g_gva_lk);
}
// 驱动给出 GPU 地址的地方（缓冲 Initialize、池 / 缓冲 / 纹理取地址）都记一笔：原始全局内存（着色器从内存读出地址再访问）只够得着游戏手里有地址的池，
//   宿主的原始池表只收这些池（nvn_mtl.m gmem_map，同上游参考实现（MIT）pool_mark_addressed）。同线程连续同一个池只记一次
static void pool_mark_addressed(const pool_t *o) { static __thread const uint8_t *last; if (o && o->mem && o->mem != last) { last = o->mem; nvn_be_pool_addressed(o->mem, o->size); } }
static inline uint64_t pool_ga(const pool_t *o) { pool_mark_addressed(o); return o->gva ? o->gva : (uint64_t)(uintptr_t)o->mem; }   // 池的缓冲区地址（游戏看到的）
#define GA(a) nvn_ga2ca(a)

static uint8_t *vpool_reserve(const pool_t *o); static void vpool_release(const pool_t *o);   // sparse virtual pools, see MapVirtual
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size; o->nvmap = ((const pool_builder_x *)b)->native;   // native：构建器给的 nvmap 句柄（SetNativeHandleNVX），MemoryPoolGetNativeHandleNVX 回它
    ((pool_x *)p)->bl_va = ((const pool_builder_x *)b)->res_bl; ((pool_x *)p)->sp_va = ((const pool_builder_x *)b)->res_sp;   // 预留的块线性 / 特殊映射地址：只读回（MemoryPoolGet{BlockLinear,Special}AddressNVX）
    if (((const pool_builder_x *)b)->native) LOG("【红】 MemoryPoolInitialize: 池 %p 由 nvmap 句柄 %d 建（SetNativeHandleNVX）：本层不导入 nvmap 内存，池没有存储", (void *)p, ((const pool_builder_x *)b)->native);
    if (((const pool_builder_x *)b)->res_pitch) { static int once; if (!once++) LOG("【红】 MemoryPoolInitialize: 池 %p 预留了线性 GPU 地址 %#llx（SetPitchAddressNVX），本层 GPU 地址自己分配、不按预留值", (void *)p, (unsigned long long)((const pool_builder_x *)b)->res_pitch); }
    if (!o->mem && (o->flags & 0x200)) o->mem = vpool_reserve(o);   // every virtual pool: its own address range, a page table over physical pools (see MapVirtual)
    else if (o->mem) { nvn_be_pool_add(o->mem, o->size); nvn_dirty_add_range(o->mem, o->size); }
    if (!o->mem && !(o->flags & 0x200)) LOG("【红】 MemoryPoolInitialize: non-virtual pool has no storage flags=%#x size=%#llx", o->flags, o->size);
    // Switch 2（NVN2）：池建在 gsv GPU 可访问内存上，那段内存的 GPU 虚拟地址就是它的 CPU 虚拟地址，缓冲区地址 = 存储的 CPU 地址；游戏会拿 GetBufferAddress 的结果加偏移直接用 CPU 读写
    //   （#351：给窗口地址时 CPU 读 0xC086D00000 一带非法地址崩）。所以 NVN2 不另分窗口地址，pool_ga 回存储地址，nvn_ga2ca 对它原样过。窗口只给 Switch 1（大页对齐语义，见上）
    o->gva = !nvn2_on() && (o->mem || (o->flags & 0x200)) ? gva_alloc((uint64_t)(uintptr_t)o->mem, o->size) : 0;   // 虚拟池的 CPU 侧是它自己的预留段（预留失败时没有，GPU 地址照给：真机初始化即有）
    if (getenv("DM_NVN_VLOG")) { LOG("[nvn] MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, (unsigned long long)o->size, o->flags); void dm_backtrace(uint64_t fp); dm_backtrace((uint64_t)__builtin_frame_address(0)); }
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    return 1;
}
static void cmdshadow_pool_gone(const uint8_t *mem, size_t n);   // command buffer section: recordings in the pool's memory are released
// 先核魔数（审计 P1-190，同 #25 缓冲收尾）：游戏可能 Finalize 没 Initialize 过 / 已 Finalize 过的池，那时 gva / mem / flags 是垃圾或已释放，不能用
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) { if (((pool_t *)p)->magic != MAGIC('P', 'O', 'O', 'L')) return;
    if (((pool_t *)p)->gva) { gva_free(((pool_t *)p)->gva); ((pool_t *)p)->gva = 0; }
    if (((pool_t *)p)->flags & 0x200) vpool_release((pool_t *)p);
    if (((pool_t *)p)->mem && !(((pool_t *)p)->flags & 0x200)) {   // A virtual pool's runs were registered and are dropped by vpool_release
 nvn_dirty_remove_range(((pool_t *)p)->mem); nvn_be_pool_del(((pool_t *)p)->mem); cmdshadow_pool_gone(((pool_t *)p)->mem, ((pool_t *)p)->size); } nvn_be_alias_drop_pool(p); ((pool_t *)p)->magic = 0; }
static void ft_flush(const uint8_t *a, uint64_t n);
// CPU_UNCACHED 池的刷新也照样发布、不做空操作（11-01 维护者定：按语义对齐，不按字面对齐）：真机上它是空操作，是因为真机 CPU 与 GPU 共享缓存语义、GPU 直接看得到 CPU 写；
//   我方贴图在 Metal 侧另有一份，刷新是「CPU 写过这段」唯一的显式信号，照真机做成空操作会在关了脏页上传的配置里漏发布（贴图停在旧内容）
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {   // see ft_add
    const pool_t *o = (const pool_t *)p;
    if (o->magic != MAGIC('P', 'O', 'O', 'L') || !o->mem) return;   // 没初始化成功的池（审计 P1-190 余项，同池收尾）：mem 是残值，不刷
    ft_flush(o->mem + off, n);
#if defined(__ANDROID__) || defined(__OHOS__)
    // 后端可选：内容变化通知（安卓直连用它推进资源账本的内容版本）。
    // 只在安卓直连编；苹果宿主不链该后端，弱引用在 Mach-O 上会变成未定义符号。
    {
        extern void s2a_vk_adapter_pool_written(const void *, uint64_t, uint64_t) __attribute__((weak));
        if (s2a_vk_adapter_pool_written) s2a_vk_adapter_pool_written(o->mem, (uint64_t)off, (uint64_t)n);
    }
#endif
}
// Unified memory: the GPU's writes are already in the bytes the CPU reads, so there is nothing to invalidate. nvnBufferInvalidateMappedRange goes through here too
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
// 空池（D-B3PR，Drag x Drive 040041601D83A000，Mac 0.6.9 三局必现）：游戏用空池建了 0x170 字节的缓冲（BufferInitialize 报红回 0），
//   随即拿空池查标志位，宿主 nvnMemoryPoolGetFlags 直接读 pool->flags，读 0x4 崩在宿主里。真驱动这里同样会解引用，游戏为什么给空池还没定
//   （NVN2 语义待核）；宿主先不陪着崩：报红一次带游戏调用位置（下一份诊断据此定根因），回 0 / NULL
static int pool_null(const void *p, const char *who, uint64_t ra) {
    if (p) return 0;
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) { module_t *m = module_of((void *)ra);
        LOG("【红】 %s：池是空指针（游戏从 %s+%#llx 调）：回 0，不解引用", who, m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra); }
    return 1;
}
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { if (pool_null(p, "nvnMemoryPoolGetBufferAddress", (uint64_t)__builtin_return_address(0))) return 0; return pool_ga((const pool_t *)p); }

// ---------- Descriptor pools ----------
// One ID space per pool. In NVN a texture or sampler ID indexes the pool the command buffer has bound (SetTexturePool / SetSamplerPool), and two pools may use the same IDs. The replay keeps
// one table for each kind, so every pool gets a base in it, in creation order: registrations go to base + ID, and binds recorded on a command buffer get the base of the pool that command
// buffer last bound (bind_handles). A game with one pool of each kind is unchanged (base 0).
// Fire Emblem: Three Houses: a 12,544-entry texture pool and a 3,856-entry sampler pool for the scene, and 4,096-entry ones its 2D UI command buffer binds. Both fill IDs from 256 up; the scene's
// default 8×8 white texture overwrote the UI's textures, and the title menu's labels, the name entry and other menus drew as plain bars or not at all. The scene also registers each draw's
// textures into fresh IDs of a ring in its large pool (8,729 and up a few seconds in); with the table's capacity taken from the last pool alone, those all read as empty slots and the screen was black
enum { NDPOOL_INIT = 16 };   // 初始容量；按需增长，容量不是合法池上限（审计 B1）
static struct dpool_base { const void *pool; int base, cap; } *g_tpb, *g_spb;   // cap：这一段编号的容量（建池时按 256 取整）；pool = NULL = 已退休、编号段可给新池
static int g_cap_tpb, g_cap_spb, g_ntpb, g_nspb, g_tnext, g_snext;
static pthread_mutex_t g_dpool_mu = PTHREAD_MUTEX_INITIALIZER;
static int dpool_grow(struct dpool_base **t, int *cap) {   // g_dpool_mu held
    int ncap = *cap ? *cap * 2 : NDPOOL_INIT;
    struct dpool_base *nt = (struct dpool_base *)realloc(*t, (size_t)ncap * sizeof **t);
    if (!nt) return -1;
    memset(nt + *cap, 0, (size_t)(ncap - *cap) * sizeof **t);
    *t = nt; *cap = ncap; return 0;
}
static int dpool_add(struct dpool_base **t, int *cap, int *nt, int *next, const void *pool, int n, const char *kind) {   // → base, -1 = accurate failure
    pthread_mutex_lock(&g_dpool_mu);
    int i = 0; while (i < *nt && (*t)[i].pool != pool) i++;
    int need = (n + 255) & ~255;
    if (i == *nt) {   // 新池：先找已退休、容量够的编号段复用（取最小的够用段），没有才在表尾另开一段
        int r = -1; for (int k = 0; k < *nt; k++) if (!(*t)[k].pool && (*t)[k].cap >= need && (r < 0 || (*t)[k].cap < (*t)[r].cap)) r = k;
        if (r >= 0) { (*t)[r].pool = pool; int b = (*t)[r].base; pthread_mutex_unlock(&g_dpool_mu); return b; }
    }
    if (i == *nt) {
        if (*nt == *cap && dpool_grow(t, cap) != 0) {
            pthread_mutex_unlock(&g_dpool_mu);
            LOG("【红】 %s pool table growth failed (out of memory) at %d pools", kind, *nt);
            return -1;
        }
        (*t)[i] = (struct dpool_base){pool, *next, need}; (*nt)++; *next += need;
        if (i) LOG("[nvn] pool #%d (%s, %d entries): numbered from %d in the host table, separate from the earlier pools", i + 1, kind, n, (*t)[i].base);
    }
    int b = (*t)[i].base; pthread_mutex_unlock(&g_dpool_mu); return b;
}
// 池销毁：退休它的编号段，之后新建的池复用（dpool_add 取最小的够用段）。贴图池另把这一段的登记清掉（释放缓存的视图）。
// 原先编号只退休不复用（担心旧命令仍引用）：11-02 神之天平 外传（010052A01E9C0000）每帧新建 / 销毁贴图池（272 项），1800 帧建到第 26746 个，
//   宿主登记表按最大编号长到约 1370 万项（每项一份贴图对象快照 + 视图），常驻 1537 → 5292 MB 被内存哨兵杀。
//   复用安全：编号在回放提交时当场解析（tex_by_id 在 nvnQueueSubmitCommands 里跑），NVN 要求游戏等 GPU 用完才销毁池——销毁时没有在跑的回放还引用这段；
//   之后提交引用已销毁池的命令在真机也是未定义（池内存被重用就读到新描述符），同此
static void dpool_finalize(struct dpool_base *t, int nt, const void *pool, int tex) {
    pthread_mutex_lock(&g_dpool_mu);
    int base = -1, cap = 0;
    for (int i = 0; i < nt; i++) if (t[i].pool == pool) { t[i].pool = NULL; base = t[i].base; cap = t[i].cap; break; }
    pthread_mutex_unlock(&g_dpool_mu);
    if (tex && base >= 0) nvn_be_unreg_tex_range(base, cap);   // 采样器项是参数值、不持 GPU 对象，复用时被新登记覆盖，不必清
}
static int dpool_base(const struct dpool_base *t, int nt, const void *pool) { for (int i = 0; i < nt; i++) if (t[i].pool == pool) return t[i].base; return 0; }
int nvn_dpool_base(const void *pool, int tex) { return tex ? dpool_base(g_tpb, g_ntpb, pool) : dpool_base(g_spb, g_nspb, pool); }   // the replay's bound pools (nvn_mtl.m state_op)
// The pools each command buffer last bound are kept in its own host state (cmd_t tex_base / samp_base). They used to be a 64-entry list of command buffers that was never cleared:
// Dark Souls Remastered binds one of 12 sampler pools in more than 64 command buffers, and every later one read its samplers through the first pool's numbering
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; OLOG("TexturePool n=%d", n);
    if (dpool_add(&g_tpb, &g_cap_tpb, &g_ntpb, &g_tnext, tp, n, "texture") < 0) return 0;
    nvn_be_set_tex_pool_n(g_tnext);
    return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { dpool_finalize(g_tpb, g_ntpb, tp, 1); }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    nvn_be_reg_tex(dpool_base(g_tpb, g_ntpb, tp) + id, (const tex_t *)t, (const view_t *)v);
}
// Image descriptors live in the same pool index space as texture descriptors, and image handles are pool indices (nvnDeviceGetImageHandle; BindImages below), so an image
// registers into the same table. Hyrule Warriors: Age of Calamity registers the images its compute shaders write in the first battle (earlier games registered only textures)
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    nvn_be_reg_tex(dpool_base(g_tpb, g_ntpb, tp) + id, (const tex_t *)t, (const view_t *)v);
}
// 采样器的缺省参数（nvnSamplerBuilderSetDefaults 与下面池初始化的保留项共用；与游戏自带驱动 SetDefaults 写的逐项一致）
static void samp_defaults(samp_par *o) {
    memset(o, 0, sizeof *o);
    o->minf = NVN_MIN_FILTER_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_max = 1000.f; o->aniso = 1.f;
}
// 池的第 0、1 号：游戏自带的 NVN 驱动在 SamplerPoolInitialize 里自己往池内存写两个采样器描述符（本机 6 款 sdk 反汇编一致，SDK 最老到最新）：
//   缺省参数、寻址 S / T / R 改 CLAMP_TO_EDGE，第 0 项各向异性 1.0、第 1 项 2.0；TexturePoolInitialize 不写任何描述符。游戏会直接拿采样器号 0 采样
//   （11-14 安卓 8E 010045B018EC2000：句柄 贴图 2127 | 采样器 0，片元 OpImageSampleImplicitLod；不登记时一局跳过 5315 笔）。
//   HLE 替换了驱动，就在这里照样登记；游戏之后自己登记这两个号照常覆盖（同真机上改写池内存）。容量不到的池只登记放得下的
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; OLOG("SamplerPool n=%d", n);
    if (dpool_add(&g_spb, &g_cap_spb, &g_nspb, &g_snext, sp, n, "sampler") < 0) return 0;
    int base = dpool_base(g_spb, g_nspb, sp);
    samp_par r; samp_defaults(&r); r.ws = r.wt = r.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
    for (int id = 0; id < 2 && id < n; id++) { r.aniso = id ? 2.f : 1.f; nvn_be_reg_samp(base + id, &r); }
    return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { dpool_finalize(g_spb, g_nspb, sp, 0); }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { nvn_be_reg_samp(dpool_base(g_spb, g_nspb, sp) + id, &((const samp_t *)s)->p); }
// straight from a builder, without a sampler object (FINAL FANTASY TACTICS - The Ivalice Chronicles registers its samplers this way)
NVN(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool *sp, int id, const NVNsamplerBuilder *b) { nvn_be_reg_samp(dpool_base(g_spb, g_nspb, sp) + id, &((const samp_builder_t *)b)->p); }
// Unreal Engine reads a descriptor pool's size back at device setup (Octopath Traveler 0)
NVN(nvnTexturePoolGetMemoryPool, const NVNmemoryPool *, const NVNtexturePool *tp) { return (const NVNmemoryPool *)((const dpool_t *)tp)->pool; }
NVN(nvnTexturePoolGetMemoryOffset, ptrdiff_t, const NVNtexturePool *tp) { return (ptrdiff_t)((const dpool_t *)tp)->off; }
NVN(nvnTexturePoolGetSize, int, const NVNtexturePool *tp) { return ((const dpool_t *)tp)->n; }
NVN(nvnSamplerPoolGetMemoryPool, const NVNmemoryPool *, const NVNsamplerPool *sp) { return (const NVNmemoryPool *)((const dpool_t *)sp)->pool; }
NVN(nvnSamplerPoolGetMemoryOffset, ptrdiff_t, const NVNsamplerPool *sp) { return (ptrdiff_t)((const dpool_t *)sp)->off; }
NVN(nvnSamplerPoolGetSize, int, const NVNsamplerPool *sp) { return ((const dpool_t *)sp)->n; }

// ---------- Textures ----------
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
// 诊断（#1969 / D-7HMX：一款游戏传进来 0x47a，不在格式表里，那张贴图当了渲染目标、深度附件建不出、3D 主画面黑）：不认的格式记一次是游戏哪里设的（模块 + 偏移）
//   和这个生成器此刻的描述；格式表认不认按 nvn_tex_layout 的每块字节数（同 GetStorageSize 的判法），前 4 种不同的值各记一次
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) {
    TB(b)->d.format = f;
    nvn_texdesc d = TB(b)->d; d.width = d.width ? d.width : 1; d.height = d.height ? d.height : 1; d.depth = d.depth ? d.depth : 1; d.levels = d.levels ? d.levels : 1;
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    if (L.bpb) return;
    static uint32_t seen[4]; static int nseen; for (int i = 0; i < nseen; i++) if (seen[i] == (uint32_t)f) return;
    if (nseen >= 4) return;
    seen[nseen++] = (uint32_t)f;
    uint64_t ra = (uint64_t)__builtin_return_address(0); module_t *m = module_of((void *)ra);
    LOG("【红】 TextureBuilderSetFormat: format %#x is not an NVN format we know; set from %s+%#llx (builder so far: target %d flags %#x %ux%ux%u levels %u samples %d)",
        (unsigned)f, m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra,
        TB(b)->d.target, TB(b)->d.flags, TB(b)->d.width, TB(b)->d.height, TB(b)->d.depth, TB(b)->d.levels, TB(b)->d.samples);
}
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    // byte 0 (low nibble block height, high nibble block depth, log2 GOBs) | byte 1's low nibble (width alignment, log2 GOBs) << 8, + 1 (one int: tex_t must stay <= 0x80; #676)
    TB(b)->d.pkg_layout = l ? (l->layout[0] | (l->layout[1] & 0xf) << 8) + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
NVN(nvnTextureBuilderGetDevice, const NVNdevice *, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->dev; }
NVN(nvnTextureBuilderGetMemoryPool, const NVNmemoryPool *, const NVNtextureBuilder *b) { return (const NVNmemoryPool *)((const tex_builder_t *)b)->pool; }
NVN(nvnTextureBuilderGetMemoryOffset, ptrdiff_t, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->off; }
NVN(nvnTextureBuilderGetPackagedTextureData, const void *, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->pkg_data; }
NVN(nvnTextureBuilderGetPackagedTextureLayout, NVNboolean, const NVNtextureBuilder *b, NVNpackagedTextureLayout *layout) {
    const tex_builder_t *o = (const tex_builder_t *)b;
    uint32_t bits = 0;
    if (o->has_layout) { memcpy(&bits, o->layout, sizeof bits); bits &= 0xfff; }
    memcpy(layout->layout, &bits, sizeof bits);   // Only the three layout nibbles are defined; preserve the reserved bytes.
    return o->has_layout;
}
NVN(nvnTextureBuilderGetSparseTileLayout, void, const NVNtextureBuilder *b, NVNtextureSparseTileLayout *layout) {
    uint32_t tile[4]; nvn_tex_sparse_layout(&((const tex_builder_t *)b)->d, tile);
    memcpy(layout, tile, sizeof tile);
}
NVN(nvnTextureBuilderGetViewOffset, ptrdiff_t, const NVNtextureBuilder *b, const NVNtextureView *v) {
    nvn_texdesc d = ((const tex_builder_t *)b)->d;
    const view_t *view = (const view_t *)v;
    nvn_texlayout layout; uint64_t offset = 0;
    int target = d.target, view_target = view->set_target ? view->target : target;
    int layered = view->set_layers && view->min_layer > 0 &&
        (target == NVN_TEXTURE_TARGET_1D_ARRAY || target == NVN_TEXTURE_TARGET_2D_ARRAY ||
         target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY || target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY);
    if (layered) {
        if (target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
            target = d.target = NVN_TEXTURE_TARGET_2D_ARRAY; d.depth *= 6;
        }
        uint32_t layers = view->min_layer;
        if (view_target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) layers *= 6;
        if (view_target == NVN_TEXTURE_TARGET_1D_ARRAY) d.height = layers; else d.depth = layers;
        nvn_tex_layout(&d, &layout); offset = layout.size;
    }
    if (view->set_levels && view->base_level > 0) {
        // A layer prefix has already selected the array stride. Its mip prefix is within one layer.
        if (layered) {
            if (target == NVN_TEXTURE_TARGET_1D_ARRAY) { d.target = NVN_TEXTURE_TARGET_1D; d.height = 1; }
            else if (target == NVN_TEXTURE_TARGET_2D_ARRAY) { d.target = NVN_TEXTURE_TARGET_2D; d.depth = 1; }
            else if (target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY) { d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; d.depth = 1; }
        }
        d.levels = view->base_level;
        nvn_tex_layout(&d, &layout); offset += layout.size;
    }
    // 审计缺口族 32（立方体数组的层）取证：这里把立方体数组视图的起始层乘 6（按立方体计），TextureGetViewOffset 与采样按面计，两处口径不一。
    //   哪种对要看游戏真设的值，打前 4 次（集中回归收证据后统一成一条规则）
    if ((view_target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY || ((const tex_builder_t *)b)->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) && view->set_layers) {
        static int n; if (n++ < 4) LOG("[nvn] 立方体数组视图偏移（TextureBuilderGetViewOffset）：贴图深度 %u、视图目标 %d、起始层 %u、层数 %u → 偏移 %llu（起始层按立方体 ×6 算）",
                                       ((const tex_builder_t *)b)->d.depth, view_target, view->min_layer, view->nlayers, (unsigned long long)offset); }
    return (ptrdiff_t)offset;
}
NVN(nvnTextureBuilderGetSwizzle, void, const NVNtextureBuilder *b, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *bl, NVNtextureSwizzle *a) {
    const tex_builder_t *o = (const tex_builder_t *)b;
    unsigned swz = o->set_swz ? o->swz : 2u | 3u << 3 | 4u << 6 | 5u << 9;
    *r = (NVNtextureSwizzle)(swz & 7); *g = (NVNtextureSwizzle)((swz >> 3) & 7);
    *bl = (NVNtextureSwizzle)((swz >> 6) & 7); *a = (NVNtextureSwizzle)((swz >> 9) & 7);
}
NVN(nvnTextureBuilderGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtextureBuilder *b) {
    return ((const tex_builder_t *)b)->stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
}
static uint32_t texture_raw_storage_class(const nvn_texdesc *d) {
    if (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) return 1;
    switch (d->format) {
    case NVN_FORMAT_NONE: return 0;
    case NVN_FORMAT_STENCIL8: case NVN_FORMAT_DEPTH16: return 3;
    case NVN_FORMAT_DEPTH24: case NVN_FORMAT_DEPTH24_STENCIL8: return 4;
    case NVN_FORMAT_DEPTH32F_STENCIL8: return 5;
    default: return 2;
    }
}
NVN(nvnTextureBuilderGetRawStorageClass, uint32_t, const NVNtextureBuilder *b) {
    return texture_raw_storage_class(&((const tex_builder_t *)b)->d);
}
static uint64_t g_layout_logged;
// S2Arm 诊断（S2A_NVN_QLOG 开着时）：贴图尺寸 / 对齐查询记下描述（目标、格式、标志、宽高深、级数、采样数）与我方答复，与 nvn_qlog_c 的行按顺序对上
static void qlog_tex(const char *what, const nvn_texdesc *d, size_t v) {
    if (!g_nvn_qlog || g_qlog_fd < 0) return;
    char line[200]; int k = snprintf(line, sizeof line, "#tex\t%s\ttarget %d\tformat %#x\tflags %#x\t%ux%ux%u\tlevels %u\tsamples %d\t%zu\n",
                                     what, d->target, d->format, d->flags, d->width, d->height, d->depth, d->levels, d->samples, v);
    if (k > 0) (void)!write(g_qlog_fd, line, (size_t)k);
}
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] packaged texture %ux%ux%u levels %u format %#x target %d layout bytes %02x%02x%02x%02x%02x%02x%02x%02x → block height log2=%u (from layout byte 0) size %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) { const nvn_texdesc *d = &((const tex_builder_t *)b)->d;   // 带上描述：哪种目标、什么标志、多大（#1969：一张不认格式的贴图后来当了深度附件）
        LOG("【红】 TextureBuilderGetStorageSize unknown format %#x (target %d flags %#x %ux%ux%u levels %u samples %d)", d->format, d->target, d->flags, d->width, d->height, d->depth, d->levels, d->samples); }
    log_layout((const tex_builder_t *)b, &L);
    qlog_tex("size", &((const tex_builder_t *)b)->d, L.size);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    qlog_tex("align", &((const tex_builder_t *)b)->d, L.align);
    return L.align;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
// Textures the CPU fills in place (profile tex_flush_upload): Tears of the Kingdom streams its textures (TexToGo, already in the GPU's block-linear layout) straight into
// CPU-cached memory pools and publishes each with nvnMemoryPoolFlushMappedRange (9,436 calls at boot), without a packaged layout or WriteTexels. They stayed GPU-only
// textures with content 0, and the G-buffer's base colour came out black. Textures that can hold such data (not render targets, not linear, no packaged layout, in a pool
// the CPU can write) are registered by address range; a flush that covers part of one marks it CPU-written (nvn_be_tex_cpu_written: uploaded from guest memory, re-uploaded
// when its pages change). Upstream had this off by default, since a flushed range could cover an image the GPU writes and the upload would replace its content. In S2Arm it is on by default
// (host/game_tsv.h, game.py; 86d8f7c1f): the upload keeps what the GPU wrote (nvn_mtl.m NvPkg pgpu / gpw_mark: bytes a copy, clear, attachment or image wrote after the CPU are not re-uploaded), and a game.tsv can still turn it off
typedef struct { const uint8_t *mem; uint64_t size; tex_t t; uint64_t stamp; const void *span; } flush_tex;   // stamp: dirty-page generation at registration (tex_dirty_upload); span: its tracked range (nvn_dirty_span), so a check needs no search over every pool
static flush_tex *g_ft; static size_t g_nft, g_cft; static pthread_mutex_t g_ft_mu = PTHREAD_MUTEX_INITIALIZER;
static void ft_add(const tex_t *t) {
    // Buffer textures are views onto the pool buffer the GPU reads directly (tex_mtl in nvn_mtl.m); an upload would push Age of Calamity's 1,048,576-texel R32F one through tex_desc, past Metal's 2D limit
    // COMPRESSIBLE is a memory-compression hint, not a data format (upstream reference implementation, MIT: SD GUNDAM G GENERATION CROSS RAYS sets it on every texture it creates and writes
    // plain texels): such textures are registered too, or the CPU writes that follow stay invisible. A CPU_NO_ACCESS pool keeps the exclusion: there the compression tags may really be there.
    // A virtual (sparse) pool is left out whatever its CPU flags (upstream reference implementation, MIT, 6121fd75): its addresses are no memory the CPU writes or flushes (its Map is NULL) and no
    // range nvn_dirty tracks, so an untracked span read as dirty at once and every texture in one, a render target too, came out CPU-written at its first use. Its pages are followed by the page table (vpool_*)
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !t->mem || !t->pool || t->has_layout || t->d.target == NVN_TEXTURE_TARGET_BUFFER || (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) ||
        (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return;
    pthread_mutex_lock(&g_ft_mu);
    if (g_nft == g_cft && !(g_ft = realloc(g_ft, (g_cft = g_cft ? 2 * g_cft : 1024) * sizeof *g_ft))) die("flush_tex allocation failed");
    uint64_t st = nvn_dirty_now(); if (DM_GAME_TEX_DIRTY_UPLOAD) nvn_dirty_protect(t->mem, t->L.size);   // see nvn_ft_written
    g_ft[g_nft++] = (flush_tex){t->mem, t->L.size, *t, st, nvn_dirty_span(t->mem, t->L.size)};
    pthread_mutex_unlock(&g_ft_mu);
}
static void ft_del(uint64_t serial) {
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !serial) return;
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == serial) { g_ft[i] = g_ft[--g_nft]; break; }
    pthread_mutex_unlock(&g_ft_mu);
}
// Profile tex_dirty_upload: Tears of the Kingdom fills some textures from the CPU after creating them and publishes them with no NVN flush at all (os::FlushDataCache runs
// natively; its 121-layer ASTC terrain material arrays, 42 MB each). They stayed GPU-only textures with content 0, which ASTC decodes as the error colour, and the cave came out magenta.
// ft_add write-protects a registered texture's pages, and on its first resolution (tex_mtl, submit thread) a write since then makes it CPU-written like a flushed one
void nvn_be_tex_cpu_inferred(const tex_t *t);   // nvn_mtl.m / replay/rp_res.c：脏页 / 刷新范围推断的 CPU 写，与显式 WriteTexels 区分（cpu_key_gpu_write）
int nvn_ft_written(const tex_t *t) {
    if (!DM_GAME_TEX_DIRTY_UPLOAD || !t->serial) return 0;
    int r = 0; pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == t->serial) {
        if (nvn_dirty_since_span(g_ft[i].span, g_ft[i].mem, g_ft[i].size, g_ft[i].stamp)) { r = 1; g_ft[i] = g_ft[--g_nft]; }
        break; }
    pthread_mutex_unlock(&g_ft_mu);
    if (r) nvn_be_tex_cpu_inferred(t);
    return r;
}
// Later writes (tex_dirty_upload): a texture first resolved before the CPU filled it stays a GPU-only texture, and the first-use check above never runs for it again.
// Once per submission, when guest memory has changed since the last look, the registered textures whose pages were written since registration become CPU-written
// (tex_mtl_ in nvn_mtl.m then drops the GPU-only texture and uploads it). Tears of the Kingdom fills its 5120x5120 BC4 roughness atlas from the CPU after first binding it,
// as tiles stream in. A texture the GPU wrote (render target, shader-written image, copy / clear destination) may share pages with data the CPU writes: if the GPU-written bytes are
// still unchanged in guest memory (the write ledger, nvn_be_tex_gpu_owned), it leaves the list without an upload, which would replace the GPU's content with older bytes.
// Not by the host texture's usage: every uncompressed texture that is not packaged is made render-target capable (11-05 Ocarina of Time on Switch 2: glyphs the CPU drew into
// a texture after it was first sampled were never uploaded, and the title screen's button text was missing)
// Each poll looks at no more than FT_POLL_BYTES of registered textures, going on where the last one stopped; any guest write starts another pass over the whole list.
// Checking every texture at every submission cost TOTK 0.4 ms of submit CPU per frame on the Mac; this way a texture is picked up a few submissions later at most
enum { FT_POLL_BYTES = 128 << 20 };
int nvn_be_tex_gpu_owned_since(uint64_t serial, uint64_t stamp);   // nvn_mtl.m / 安卓 nvn_be_android.c
// 录到一次「GPU 写进这个池里的贴图」（渲染目标、清贴图、拷进贴图 / 降采样；着色器图像在 nvn_mtl.m 解析时记）：池里 CPU 写的非压缩 2D 贴图从此不再按首用取存储
//   （nvn_mtl.m tex_cpu_content / cpu_unc_drop；上游参考实现（MIT）75e5ddd8）。声明不放 nvn_obj.h：那是直连出库阶段键的输入
void nvn_be_pool_gpu_written(const void *pool);   // nvn_mtl.m / 安卓 nvn_be_android.c
void nvn_ft_poll(void) {
    if (!DM_GAME_TEX_DIRTY_UPLOAD) return;
    static uint64_t gen; static size_t cur, left;
    static tex_t *hit; static size_t chit; size_t nhit = 0;
    static uint64_t *hst; static size_t chit2;   // 各命中的登记时刻：只看这之后写过的页（nvn_be_tex_gpu_owned_since）
    uint64_t now = nvn_dirty_now(), scanned = 0;
    pthread_mutex_lock(&g_ft_mu);
    if (now != gen) { gen = now; left = g_nft; }   // guest memory changed: every entry gets one more look
    while (left && g_nft && scanned < FT_POLL_BYTES) {
        if (cur >= g_nft) cur = 0;
        scanned += g_ft[cur].size; left--;
        if (nvn_dirty_since_span(g_ft[cur].span, g_ft[cur].mem, g_ft[cur].size, g_ft[cur].stamp)) {
            if (nhit == chit && !(hit = realloc(hit, (chit = chit ? 2 * chit : 64) * sizeof *hit))) die("nvn_ft_poll allocation failed");
            if (nhit == chit2 && !(hst = realloc(hst, (chit2 = chit2 ? 2 * chit2 : 64) * sizeof *hst))) die("nvn_ft_poll allocation failed");
            hst[nhit] = g_ft[cur].stamp; hit[nhit++] = g_ft[cur].t; g_ft[cur] = g_ft[--g_nft];
        } else cur++;
    }
    pthread_mutex_unlock(&g_ft_mu);
    static uint64_t nup, nkept;   // under g_sub_mu (the submit path); first lines, then at powers of two
    for (size_t i = 0; i < nhit; i++) {   // outside g_ft_mu: the ownership check takes the ledger's lock
        // 多重采样贴图（2D_MULTISAMPLE / _ARRAY）的采样只在宿主 GPU 上，客体内存里排不出（nvn_deswizzle 不支持，GPU 解交错也不支持）：按客体内存重传会换上一张
        //   内容未定义的新贴图（#1499 被囚禁的掌心 iPhone：警告页之后整屏黑，日志「deswizzle failed format 0x29 target 5」）。它只能是 GPU 画出来的，一律留着
        const tex_t *t = &hit[i];
        const int msaa = t->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || t->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
        int kept = msaa || nvn_be_tex_gpu_owned_since(t->serial, hst[i]); uint64_t k = kept ? ++nkept : ++nup;
        if (k <= 8 || !(k & (k - 1))) LOG("[nvn] texture ser=%llu %ux%ux%u format %#x flags %#x: pages written after its first use, %s (%llu so far)", (unsigned long long)t->serial,
                                          t->d.width, t->d.height, t->d.depth, t->d.format, t->d.flags, msaa ? "kept as the GPU wrote it (multisample: its samples are not in guest memory)" : kept ? "kept as the GPU wrote it (its GPU-written bytes are unchanged in guest memory)" : "uploaded from guest memory", (unsigned long long)k);
        if (!kept) nvn_be_tex_cpu_inferred(t);
    }
}
static void ft_flush(const uint8_t *a, uint64_t n) {
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !a || !n) return;
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft;)
        if (g_ft[i].mem < a + n && a < g_ft[i].mem + g_ft[i].size) { nvn_be_tex_cpu_inferred(&g_ft[i].t); g_ft[i] = g_ft[--g_nft]; } else i++;
    pthread_mutex_unlock(&g_ft_mu);
}
int nvn_be_tex_from_gl(tex_t *t, uint32_t gl_name);   // nvn_mtl.m：按 GL 纹理名接存储与描述（声明不放 nvn_obj.h：那是直连出库阶段键的输入，宿主改动不该让各款直连库重出）
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    // Re-initializing a texture object that is still live (without a Finalize first): release the host texture of the old serial first. SMBW re-initializes its transient render targets on the same object every frame and never Finalizes them;
    //   previously every init created a new private MTLTexture and the old one stayed in the serial table forever, so GPU memory grew about 13 MB per frame (killed by the memory watchdog at 24 GB)
    if (o->magic == MAGIC('T', 'E', 'X', 0) && o->serial) { static uint64_t n; if (!n++) LOG("[nvn] TextureInitialize on a texture object that is still live (no Finalize): releasing the old host texture (first case ser=%llu)", (unsigned long long)o->serial); DM_BLOCK_BEGIN(); nvn_be_tex_release(o); DM_BLOCK_END(); }
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8); o->set_swz = s->set_swz; o->stencil = s->stencil; o->swz = s->swz;
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    o->serial = nvn_new_serial(); nvn_be_tex_init(o);
    if (s->gl_name && nvn_be_tex_from_gl(o, s->gl_name)) nvn_tex_layout(&o->d, &o->L);   // 按 GL 纹理建：描述换成 GL 的，存储是 GL 那张（nvn_mtl.m）
    {   // DM_NVN_TEXINFO=WxH also logs every texture of that size as it is initialized (render targets included): which ones share storage, and how their descriptions differ.
        //   DM_NVN_TEXINFO=any (or 0x0) drops the size filter: every texture is logged and the line says all=1 (still capped at the first 200)
        static int ti = -1, any; static uint32_t tw, th;
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; if (e && (!strcmp(e, "any") || (ti && !tw && !th))) { ti = 1; any = 1; } }
        if (ti && (any || (o->d.width == tw && o->d.height == th))) { static uint64_t n; if (n++ < 200) LOG("[nvn] TEXINFO init all=%d ser=%llu mem=%p pool=%p(mem %p size %#llx flags %#x) off=%#llx target=%d format=%#x flags=%#x samples=%d %ux%ux%u levels=%u stride=%u size=%#llx",
            any, (unsigned long long)o->serial, (void *)o->mem, (void *)o->pool, o->pool ? (void *)o->pool->mem : NULL, o->pool ? (unsigned long long)o->pool->size : 0ull, o->pool ? o->pool->flags : 0,
            (unsigned long long)o->off, o->d.target, o->d.format, o->d.flags, o->d.samples, o->d.width, o->d.height, o->d.depth, o->d.levels, o->d.stride, (unsigned long long)o->L.size); }
    }
    // See SetPackagedTextureData. Linear textures are not re-uploaded as packaged: they are created on the pool buffer (tex_linear), whatever the pool's CPU access, and read what was copied in
    if (s->pkg_data && o->mem) {
        memcpy(o->mem, s->pkg_data, o->L.size);
        if (!(o->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) nvn_be_tex_cpu_written(o);
    }
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    ft_add(o);
    return 1;
}
// 释放幂等（原生线 #1014）：serial 是 release 的唯一身份，首次释放后清掉；重复 Finalize 直接早退。
NVN(nvnTextureFinalize, void, NVNtexture *t) {
    if (TX(t)->magic != MAGIC('T', 'E', 'X', 0) || !TX(t)->serial) { TX(t)->magic = 0; return; }   // 没初始化过的对象 serial 是垃圾（审计 P1-190）：只认魔数 + serial
    ft_del(TX(t)->serial);
    DM_BLOCK_BEGIN(); nvn_be_tex_release(TX(t)); DM_BLOCK_END();
    TX(t)->serial = 0; TX(t)->magic = 0;
}
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
// ---- GL 互操作名字：TextureInitialize 按它接 GL 纹理的存储与描述（nvn_be_tex_from_gl）；Get 读回构建器上记的名字 ----
NVN(nvnTextureBuilderSetGLTextureName, void, NVNtextureBuilder *b, uint32_t name) { TB(b)->gl_name = name; }
NVN(nvnTextureBuilderGetGLTextureName, uint32_t, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->gl_name; }
// ---- 稀疏贴图：本项目不支持 sparse tiling（Builder 的 sparse 布局一律按普通纹理建），查询时把输出结构清零并点名一次，
//      不返回「看起来像成功」的假布局 ----
// 同 nvnTextureBuilderGetSparseTileLayout，按贴图建时的描述（tex_t.d）算：4 个 32 位字段（平铺 mip 数、页宽、页高、页深，graphics/nvn/docs/builder_queries.md）。
//   #962：原先填零报红，一款 Switch 2 游戏按这里的页大小算 MapVirtual 的请求长度，9 个 0 字节请求全被 vm_remap 拒（KERN_INVALID_ARGUMENT），随后在 main 里读空指针
NVN(nvnTextureGetSparseTileLayout, void, const NVNtexture *t, NVNtextureSparseTileLayout *layout) {
    uint32_t tile[4]; nvn_tex_sparse_layout(&CTX(t)->d, tile);
    memcpy(layout, tile, sizeof tile);
}
// ---- 调试 ID：NVN 没有 SetDebugID，驱动侧自行给。本层用对象的初始化序号（tex_t.serial，单调、每对象唯一、跨拷贝稳定）当调试 ID ----
NVN(nvnTextureGetDebugID, uint64_t, const NVNtexture *t) { return CTX(t)->serial; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->d.stride; }   // the row stride of a linear texture as built (0 for block-linear ones)
// The queries below are used by Engage (Unity) at startup: it calls MemoryPoolMap on "the pool the texture / buffer lives in", and with a stub returning 0 it dereferenced a null pool and crashed
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->d.levels; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->d.samples; }
NVN(nvnTextureGetMemoryPool, NVNmemoryPool *, const NVNtexture *t) { return (NVNmemoryPool *)CTX(t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }
// The GPU address of the texture's storage (DRAGON QUEST XI S, Unreal Engine 4.18, asks for it at start): pool address + offset, the same address space as nvnMemoryPoolGetBufferAddress
NVN(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) { const tex_t *o = CTX(t); return o->pool ? (NVNtextureAddress)(pool_ga(o->pool) + o->off) : 0; }
// NVX extension: the texture described as an NvRmSurface (88 bytes), so NvRm hardware (VIC / the video decoder) can write its storage directly. Link's Awakening (Grezzo, SDK 8.2)
// plays its opening movie through an Android OMX player in subsdk1 that looks this name up and imports every output texture with nvgr_import_nvrmsurface; with the name unknown it skips
// the import, leaves the buffer slots unset, and NvBlit later dereferences one (black screen, then SIGSEGV at sdk+0x3ee80c). Fields as the game's own NVN driver fills them (sdk+0x4299a0):
// cleared, then width / height, the NvColorFormat at +8 (its low byte is bits per pixel), the layout at +16 (1 pitch with the stride at +20; 3 block-linear with kind 0xFE at +32, the block
// height at +36 and NvRmSurfaceComputePitch's pitch, which is the row bytes rounded up to a 64-byte GOB), the pool's nvmap handle at +24, the offset in the pool at +28, and at +56
// NvRmSurfaceComputeSize (pitch × rows, rows rounded up to 4 for pitch and to the block height for block-linear; its chip-capability adjustments for YUV formats are not reproduced).
// The pool has no nvmap handle (the NVN driver does not run): the service layer opens one over the pool's guest memory (S2A_NvMapFromGuest), once per pool. Host1x stays live in direct
// mode, so VIC writes land in the pool (s2a_track_write stamps their pages in the pool's write tracking).
// Exporting the texture is the game saying hardware other than the GPU will write its storage, so a block-linear one is marked CPU-written here (nvn_be_tex_cpu_written: uploaded from guest
// memory, re-uploaded when its own bytes change; what the game's GPU work wrote since is kept, nvn_mtl.m NvPkg pgpu), whatever its pool's CPU access or its flags. Before (11-03), only the
// CPU-write fallback (ft_add / nvn_ft_poll) could make it so, and that leaves out CPU_NO_ACCESS pools, COMPRESSIBLE textures and host render targets: Suikoden I&II HD's VP9 opening movie
// (VIC → NVN texture) decoded every frame but never reached the Metal texture (0 re-uploads in 7 s). Linear textures are made on the pool buffer (tex_linear) and see VIC's writes already.
// host_adapt.m: the RGBA8 NvColorFormat of the game's own driver (nvn_drvscan.h nvn_drv_nvrm_rgba8_scan; 0 = not recognised). The table below is in the 6.4–10.9 encoding
// (Link's Awakening, SDK 8.2); nvn_drv_nvrm_recode moves it to the game's, which its VIC library requires (DRAGON QUEST X Offline, SDK 13.3: NvBlit refused the old one)
__attribute__((weak)) uint64_t dm_nvn_driver_nvrm_rgba8(void) { return 0; }
static uint64_t nvrm_color(int f) {   // NVNformat → NvColorFormat, the 6.4–10.9 driver's table; formats it does not list get 0
    switch (f) {
    case NVN_FORMAT_R8: return 0xE09200408ull;
    case NVN_FORMAT_RG8: return 0xE08440710ull;
    case NVN_FORMAT_RGBA8: case NVN_FORMAT_RGBA8_SRGB: return 0x100532120ull;
    case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_RGBX8: return 0x10A532120ull;
    case NVN_FORMAT_RGB565: return 0x10A881210ull;
    case NVN_FORMAT_BGRA8: case NVN_FORMAT_BGRA8_SRGB: return 0x100D12120ull;
    default: return 0;
    }
}
static uint64_t nvrm_color_game(int f) {
    static uint64_t drv; static int looked;
    if (!looked) { drv = dm_nvn_driver_nvrm_rgba8(); looked = 1;
        if (drv) LOG("[nvn] NvRmSurfaceNVX: the game's driver encodes RGBA8 as %#llx, colour formats recoded to it", (unsigned long long)drv);
        else LOG("【红】 nvnTextureGetNvRmSurfaceNVX: the game's driver's NvColorFormat encoding not recognised, the host table's 6.4–10.9 encoding is used"); }
    return nvn_drv_nvrm_recode(nvrm_color(f), drv);
}
static int32_t pool_nvmap(pool_t *p) {
    // 句柄 0：NvRmSurface 的 hMem 为 0，游戏的 nvgr_import_nvrmsurface 随后 NvMap GetId / FromId 失败、导入失败（010045B018EC2000 的影片播放器不查返回值，照样 NvBlit 那块已释放的缓冲：sdk 内 SIGSEGV）。
    //   三支都还没有实现（虚拟池的存储是物理池的页、没有单一 nvmap 句柄），按铁律报红；仍回 0，不另开句柄
    if (!p) { LOG("【红】 nvnTextureGetNvRmSurfaceNVX: 纹理没有内存池，NvRmSurface 的 nvmap 句柄给 0（游戏导入会失败）"); return 0; }
    if (p->flags & 0x200) { LOG("【红】 nvnTextureGetNvRmSurfaceNVX: 池 %p 是虚拟池（flags %#x 尺寸 %#llx），存储是物理池的页、本层不给虚拟池开 nvmap 句柄，句柄给 0（游戏导入会失败）", (void *)p, p->flags, (unsigned long long)p->size); return 0; }
    if (!p->mem) { LOG("【红】 nvnTextureGetNvRmSurfaceNVX: 池 %p 池没有存储（flags %#x 尺寸 %#llx），句柄给 0（游戏导入会失败）", (void *)p, p->flags, (unsigned long long)p->size); return 0; }
    int32_t h = __atomic_load_n(&p->nvmap, __ATOMIC_ACQUIRE);
    if (h) return h;
    static int (*mk)(uint64_t, uint32_t); static int looked;
#if defined(__ANDROID__) || defined(__OHOS__)
    // 安卓：服务层 NativeAOT 与本文件静态并入同一个 libs2arm_android.so，链接带 --exclude-libs,ALL，导出不进动态表，
    //   dlsym(RTLD_DEFAULT) 恒为 NULL（010045B018EC2000：每局句柄 0，影片播放器 NvBlit 已释放的缓冲崩在 sdk 内）。
    //   改成链接期弱引用：出包的单库里由归档解析（core-link 审计：归档定义的导出在库里仍未解析就失败）；不带服务层的门禁链接（gfx_plat/gate.sh，-z defs）里为空，走下面的报红
    { extern int S2A_NvMapFromGuest(uint64_t address, uint32_t size) __attribute__((weak)); if (!looked) { mk = S2A_NvMapFromGuest; looked = 1; } }
#else
    if (!looked) { mk = (int (*)(uint64_t, uint32_t))dlsym(RTLD_DEFAULT, "S2A_NvMapFromGuest"); looked = 1; }
#endif
    if (!mk || p->size > UINT32_MAX) { static int once; if (!once++) LOG("【红】 nvnTextureGetNvRmSurfaceNVX: no nvmap handle for pool %p size %#llx (%s)", (void *)p, (unsigned long long)p->size, mk ? "pool over 4 GB" : "no service layer: S2A_NvMapFromGuest missing"); return 0; }
    h = mk((uint64_t)(uintptr_t)p->mem, (uint32_t)p->size);
    int32_t z = 0;
    if (!__atomic_compare_exchange_n(&p->nvmap, &z, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return z;   // another thread opened one first: use that (this one stays unused)
    OLOG("NvRmSurfaceNVX: pool %p mem %p size %#llx → nvmap %d", (void *)p, (void *)p->mem, (unsigned long long)p->size, h);
    return h;
}
NVN_NOPROTO(nvnTextureGetNvRmSurfaceNVX, void, const NVNtexture *t, const void *unused, void *out) {
    (void)unused; const tex_t *o = CTX(t); uint8_t *s = (uint8_t *)out;
    memset(s, 0, 88);
    uint32_t w = o->d.width, h = o->d.height, lay, pitch, kind = 0, bh = 0, hm, off = (uint32_t)o->off; uint64_t cf = nvrm_color_game(o->d.format), size;
    uint32_t row = ((uint32_t)(cf & 0xff) * w + 7) >> 3;
    if (o->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) { lay = 1; pitch = o->d.stride; size = (uint64_t)pitch * ((h + 3) & ~3u); }
    else { lay = 3; kind = 0xFE; bh = o->L.tileH; pitch = (row + 63) & ~63u; uint32_t rh = 8u << bh; size = (uint64_t)pitch * ((h + rh - 1) / rh * rh); }
    hm = (uint32_t)pool_nvmap(o->pool);
    memcpy(s, &w, 4); memcpy(s + 4, &h, 4); memcpy(s + 8, &cf, 8); memcpy(s + 16, &lay, 4); memcpy(s + 20, &pitch, 4); memcpy(s + 24, &hm, 4); memcpy(s + 28, &off, 4);
    memcpy(s + 32, &kind, 4); memcpy(s + 36, &bh, 4); memcpy(s + 56, &size, 8);
    if (!cf) { static int once; if (!once++) LOG("【红】 nvnTextureGetNvRmSurfaceNVX: format %#x has no NvColorFormat (the driver reports 0 too)", o->d.format); }
    if (lay == 3) nvn_be_tex_cpu_written(o);   // VIC / the video decoder writes it from now on (see above)
}
// The size of a texture's ZCull save area (SaveZCullData / RestoreZCullData). The host does no ZCull and never touches the area, but games allocate it from their own pools, and NVN
// does not report 0. Cuphead (Unity 2017.4) allocates one per depth target (main+0x1948060) from its pool allocator, which gives a zero-size request the offset of a live block; its
// map of allocations then loses that block (the zero-size one's size overwrites it, and freeing the zero-size one deletes it), and the block's own free later pushes a garbage-sized
// range onto the free list (main+0x1945b10). Buffers allocated after that overlap live ones: copies read staging bytes other copies had overwritten, and sprites broke into slivers,
// went missing, or flickered every fourth frame (the upload ring's period). The hardware's formula is not known; this one grows with the texture: 8 bytes per 32x32 tile and layer,
// at least 256
static size_t zcull_size(const nvn_texdesc *d) {
    uint64_t tiles = (uint64_t)((d->width + 31) / 32) * ((d->height + 31) / 32) * (d->depth ? d->depth : 1);
    return tiles ? (size_t)((tiles * 8 + 0xff) & ~0xffull) : 0x100;
}
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { return zcull_size(&CTX(t)->d); }
static unsigned tex_swz(const tex_t *t) { return t->set_swz ? t->swz : 2u | 3u << 3 | 4u << 6 | 5u << 9; }   // the channel mapping: identity R G B A when TextureBuilderSetSwizzle was not called
// The mapping counts: BotW and Odyssey rebuild layout captures on the same storage with a new mapping, and replace the registered descriptor only if the two compare different (BotW main+0xec33f0)
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem && tex_swz(x) == tex_swz(y);
}
NVN(nvnTextureGetRawStorageClass, uint32_t, const NVNtexture *t) { return texture_raw_storage_class(&CTX(t)->d); }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewCompare, NVNboolean, const NVNtextureView *a, const NVNtextureView *b) {
    const view_t *x = (const view_t *)a, *y = (const view_t *)b;
    if (x->set_levels != y->set_levels || x->set_layers != y->set_layers ||
        x->set_format != y->set_format || x->set_swizzle != y->set_swizzle ||
        x->set_dsmode != y->set_dsmode || x->set_target != y->set_target) return 0;
    if (x->set_levels && (x->base_level != y->base_level || x->nlevels != y->nlevels)) return 0;
    if (x->set_layers && (x->min_layer != y->min_layer || x->nlayers != y->nlayers)) return 0;
    if (x->set_format && x->format != y->format) return 0;
    if (x->set_swizzle) for (int i = 0; i < 4; i++) if (x->swz[i] != y->swz[i]) return 0;
    if (x->set_dsmode && x->dsmode != y->dsmode) return 0;
    if (x->set_target && x->target != y->target) return 0;
    return 1;
}
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    if (w->set_layers && (o->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY || (w->set_target && w->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY))) {   // 缺口族 32 取证（见 TextureBuilderGetViewOffset）
        static int n; if (n++ < 4) LOG("[nvn] 立方体数组视图偏移（TextureGetViewOffset）：贴图深度 %u、视图目标 %d、起始层 %u、层数 %u → 偏移 %llu（起始层按面算）",
                                       o->d.depth, w->set_target ? w->target : o->d.target, w->min_layer, w->nlayers, (unsigned long long)off); }
    return (ptrdiff_t)off;
}
// CPU texture read (NVN ReadTexels): for packaged textures the pool memory is the ground truth (de-layout and read directly); GPU-written textures are read back from the MTLTexture (nvn_mtl.m)
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    nvn_be_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

// ---------- Samplers ----------
#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_defaults(SB(b));
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) {   // Retain the query value; Metal has no LOD snap (mip switches are not snapped to the step)
    SB(b)->lod_snap = v;
    if (v != 0.0f) { static int n; if (!n++) LOG("【红】 SamplerBuilderSetLodSnap %.3f: Metal has no LOD snap, mip switches are not snapped to this step (audit gap family 31)", v); }
}
NVN(nvnSamplerBuilderGetDevice, const NVNdevice *, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->dev; }
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *lo = o->lod_min; *hi = o->lod_max;
}
NVN(nvnSamplerBuilderGetLodBias, float, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->p.lod_bias; }
NVN(nvnSamplerBuilderGetCompare, void, const NVNsamplerBuilder *b, NVNcompareMode *m, NVNcompareFunc *f) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *m = (NVNcompareMode)o->cmp_mode; *f = (NVNcompareFunc)o->cmp_func;
}
NVN(nvnSamplerBuilderGetBorderColor, void, const NVNsamplerBuilder *b, float *c) { memcpy(c, ((const samp_builder_t *)b)->p.border, 16); }
NVN(nvnSamplerBuilderGetBorderColori, void, const NVNsamplerBuilder *b, int *c) { memcpy(c, ((const samp_builder_t *)b)->p.border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui, void, const NVNsamplerBuilder *b, uint32_t *c) { memcpy(c, ((const samp_builder_t *)b)->p.border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy, float, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->p.aniso; }
NVN(nvnSamplerBuilderGetReductionFilter, NVNsamplerReduction, const NVNsamplerBuilder *b) { return (NVNsamplerReduction)((const samp_builder_t *)b)->p.reduction; }
NVN(nvnSamplerBuilderGetLodSnap, float, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->p.lod_snap; }
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->pad = 0; o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- Programs ----------
#if defined(__ANDROID__) || defined(__OHOS__)
static void prog_pin_release(uint64_t serial, const void *owner);
#endif
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d;
#if defined(__ANDROID__) || defined(__OHOS__)
    prog_pin_release(((prog_t *)p)->serial, p);   // 不 Finalize 直接重新 Initialize 时先解旧 serial 的 pin
#endif
    memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); return 1;
}
static void pk_drop(const void *p);
#if defined(__ANDROID__) || defined(__OHOS__)
extern void s2a_source_drop_pending(uint64_t prog_serial) __attribute__((weak));   // android/native/direct/source_store.c
#endif
NVN(nvnProgramFinalize, void, NVNprogram *p) {
#if defined(__ANDROID__) || defined(__OHOS__)
    prog_pin_release(((prog_t *)p)->serial, p);
    if (s2a_source_drop_pending) s2a_source_drop_pending(((prog_t *)p)->serial);   // SetShaders 自持的控制段随程序释放（#1622：原先 256 格环满了静默挤掉）
#endif
    nvn_be_prog_release((prog_t *)p); ((prog_t *)p)->magic = 0; pk_drop(p);
}
// a program's per-stage keys (side table: guest NVNprogram address → host object; bit i of ok = stage i computed)
typedef struct { uint8_t key[6][8]; uint8_t stage[6]; uint8_t ok; } prog_keys_t;
static pthread_mutex_t g_pk_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableDictionaryRef g_pk;   // a dedicated table (keys not shared with other side tables)
static prog_keys_t *pk_get(const void *p, int make) {
    pthread_mutex_lock(&g_pk_mu);
    if (!g_pk) g_pk = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    prog_keys_t *k = (prog_keys_t *)CFDictionaryGetValue(g_pk, p);
    if (!k && make) { k = calloc(1, sizeof *k); memset(k->stage, 0xff, sizeof k->stage); CFDictionarySetValue(g_pk, p, k); }
    pthread_mutex_unlock(&g_pk_mu);
    return k;
}
// 按序号的程序快照表（S2Arm 11-01，勇者斗恶龙 XI S「PSO CommandMemory OOM」）：OP_BIND_PROGRAM 只录程序身份 {prog, serial, stages}（nvn_obj.h p_bind_program_id），
//   程序内容（prog_t 拷贝）与各阶段键 / 阶段序号按序号在这里存一份。真机 NVN 绑定时只把着色器地址编进命令流；旧记录每条带 184 字节快照，游戏按真机体积给的命令内存被用满。
// 序号只在 SetShaders（nvnProgramSetShaders 换新序号）时新增，同一序号的程序内容不再变，所以一个序号一份快照就够；键可能后来才算出（prog_key 补算、安卓绑定时补算），只合并变好的部分。
// 只增不删：命令缓冲可以录一次、提交很多次，游戏也可能隔很多帧才提交同一段命令内存，引用某序号的记录何时回放完宿主不知道（维护者 11-01：不许环形 / 先进先出淘汰）。
//   条目数 ≤ 绑定过（安卓：设置过）的程序序号数，增长等于程序 SetShaders 次数；每条 sizeof(psnap_ent) + 128 B（键组 64 B + 槽位与扩容旧表摊销），
//   安卓的 psnap_ent 另含 6 个源快照 pin 引用（slot+seq+key+sha，352 B），苹果平台没有这段（条目仍 120 B，与改动前一致）。到 2 的幂打一行条目数
// 线程：录制线程写（g_ps_mu 串行），回放线程无锁读：条目整份建好后才以 release 写进槽位，扩容建新表整体发布、旧表不回收（读端可能正在探测，旧表合计小于新表）；
//   条目里的快照写入后不再变，回放层可长期持有它的指针（nvn_mtl.m prog_live）；键变好时换一份新的键组发布，旧键组不回收（每序号至多 12 次：6 个键位 + 6 个阶段位各变一次）
typedef struct { prog_t snap; _Atomic(const prog_keys_t *) ks;
#if defined(__ANDROID__) || defined(__OHOS__)
                 struct nvn_program_pins pins;   // 该 serial 的源快照引用（g_ps_mu 保护）；苹果平台不占这份内存
#endif
} psnap_ent;
typedef struct { uint64_t cap; _Atomic(psnap_ent *) slot[]; } psnap_tab;
static pthread_mutex_t g_ps_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(psnap_tab *) g_ps_tab; static uint64_t g_ps_n;
static uint64_t ps_hash(uint64_t s) { s *= 0x9e3779b97f4a7c15ull; return s ^ s >> 29; }
static psnap_ent *ps_find(psnap_tab *t, uint64_t serial) {
    if (!t) return NULL;
    for (uint64_t i = ps_hash(serial) & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
        psnap_ent *e = atomic_load_explicit(&t->slot[i], memory_order_acquire);
        if (!e || e->snap.serial == serial) return e;
    }
}
static void ps_place(psnap_tab *t, psnap_ent *e) {
    uint64_t i = ps_hash(e->snap.serial) & (t->cap - 1);
    while (atomic_load_explicit(&t->slot[i], memory_order_relaxed)) i = (i + 1) & (t->cap - 1);
    atomic_store_explicit(&t->slot[i], e, memory_order_release);
}
// 存入或合并：p 是绑定 / 设置当时的程序对象（只读它的 nstage、sd、serial），keys / ok / stage 是那时已知的各阶段键、已算出的位、阶段序号（0xff = 认不出）
static void prog_snap_put(const prog_t *p, const uint8_t keys[6][8], uint8_t ok, const uint8_t stage[6]) {
    pthread_mutex_lock(&g_ps_mu);
    psnap_tab *t = atomic_load_explicit(&g_ps_tab, memory_order_relaxed);
    psnap_ent *e = ps_find(t, p->serial);
    const prog_keys_t *old = e ? atomic_load_explicit(&e->ks, memory_order_relaxed) : NULL;
    prog_keys_t nk; if (old) nk = *old; else { memset(&nk, 0, sizeof nk); memset(nk.stage, 0xff, sizeof nk.stage); }
    int better = !old;
    for (int i = 0; i < 6; i++) {
        if ((ok >> i & 1) && !(nk.ok >> i & 1)) { memcpy(nk.key[i], keys[i], 8); nk.ok |= (uint8_t)(1u << i); better = 1; }
        if (stage[i] != 0xff && nk.stage[i] == 0xff) { nk.stage[i] = stage[i]; better = 1; }
    }
    if (better) {
        prog_keys_t *k = malloc(sizeof *k); *k = nk;
        if (e) atomic_store_explicit(&e->ks, k, memory_order_release);
        else {
            e = calloc(1, sizeof *e); e->snap = *p; e->snap.magic = MAGIC('P', 'R', 'O', 'G');
            for (int st = 0; st < 6; st++) e->snap.sd[st].control = NULL;   // 控制段随游戏的程序走（与旧 prog_live 的快照一致：缺映射的键报红、不现译）
            atomic_store_explicit(&e->ks, k, memory_order_relaxed);
            if (!t || (g_ps_n + 1) * 2 > t->cap) {
                uint64_t cap = t ? t->cap * 2 : 1024;
                psnap_tab *n = calloc(1, sizeof *n + cap * sizeof n->slot[0]); n->cap = cap;
                if (t) for (uint64_t i = 0; i < t->cap; i++) { psnap_ent *o = atomic_load_explicit(&t->slot[i], memory_order_relaxed); if (o) ps_place(n, o); }
                atomic_store_explicit(&g_ps_tab, n, memory_order_release); t = n;   // 旧表不回收（见上）
            }
            ps_place(t, e);
            if (++g_ps_n >= 1024 && !(g_ps_n & (g_ps_n - 1))) LOG("[nvn] program snapshot table: %llu serials (about %llu KB at %llu B/serial, grows with ProgramSetShaders calls, never evicted)", (unsigned long long)g_ps_n, (unsigned long long)(g_ps_n * (sizeof(psnap_ent) + 128) / 1024), (unsigned long long)(sizeof(psnap_ent) + 128));
        }
    }
    pthread_mutex_unlock(&g_ps_mu);
}
// 回放侧（nvn_obj.h bind_program_rec、nvn_mtl.m prog_live）：序号的快照，指针长期有效（条目不回收）；keys / ok / stage 可为空。没有这个序号返回空
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    psnap_ent *e = ps_find(atomic_load_explicit(&g_ps_tab, memory_order_acquire), serial);
    if (!e) return NULL;
    const prog_keys_t *k = atomic_load_explicit(&e->ks, memory_order_acquire);
    if (keys) memcpy(keys, k->key, sizeof k->key);
    if (ok) *ok = k->ok;
    if (stage) memcpy(stage, k->stage, sizeof k->stage);
    return &e->snap;
}
#if defined(__ANDROID__) || defined(__OHOS__)
// 源快照的 program 生命周期 pin：身份用 content_sha（不按 prog_t* 指针），引用记在 serial 的快照条目上。
// SetShaders 换代 / program Finalize 时统一解 pin；同一 serial 的重复 Bind 去重。
extern int s2a_source_pin_key(const uint8_t key[8], struct s2a_source_pin_ref *out) __attribute__((weak));
extern int s2a_source_unpin_ref(const struct s2a_source_pin_ref *ref) __attribute__((weak));
static void prog_pin_add(uint64_t serial, const void *owner, const struct s2a_source_pin_ref *ref) {
    int added = 0;
    pthread_mutex_lock(&g_ps_mu);
    psnap_ent *e = ps_find(atomic_load_explicit(&g_ps_tab, memory_order_relaxed), serial);
    if (e) added = nvn_program_pins_add(&e->pins, owner, ref) == 1;
    pthread_mutex_unlock(&g_ps_mu);
    // 重复 / 已释放 / 没有 serial 条目：撤销刚拿到的 pin，保持计数平衡且不挂到已释放 serial。
    if (!added && s2a_source_unpin_ref) (void)s2a_source_unpin_ref(ref);
}
// SetShaders 的 serial 归属：prog_snap_put 建好条目后立刻归原件 o。
// 拷贝对象先 Bind 时，prog_source_pin 只会加 pin，不会把 owner 从原件改成拷贝地址。
static void prog_pin_set_owner(uint64_t serial, const void *owner) {
    pthread_mutex_lock(&g_ps_mu);
    psnap_ent *e = ps_find(atomic_load_explicit(&g_ps_tab, memory_order_relaxed), serial);
    if (e) nvn_program_pins_set_owner(&e->pins, owner);
    pthread_mutex_unlock(&g_ps_mu);
}
static void prog_pin_release(uint64_t serial, const void *owner) {
    struct s2a_source_pin_ref refs[NVN_PROGRAM_PIN_MAX];
    int n = 0;
    pthread_mutex_lock(&g_ps_mu);
    psnap_ent *e = ps_find(atomic_load_explicit(&g_ps_tab, memory_order_relaxed), serial);
    if (e) n = nvn_program_pins_release(&e->pins, owner, refs);
    pthread_mutex_unlock(&g_ps_mu);
    if (s2a_source_unpin_ref)
        for (int i = 0; i < n; i++) (void)s2a_source_unpin_ref(&refs[i]);
}
#if defined(__ANDROID__) || defined(__OHOS__)
// 源捕获诊断：计算源整段是零时，用这三行区分「没映射 / 还没写 / 地址语义错」。
// budget 每进程有限条，但计算阶段（stage>=4）永远打印。
static _Atomic int g_src_diag_budget = 24;
static void src_diag(const char *when, int slot, int stage, const uint8_t key[8], const void *data,
                     uint32_t ncode, const void *ctl, int key_ok) {
    void *mb = NULL; uint64_t mo = 0;
    int mapped = (data && ncode) ? nvn_be_addr((uint64_t)(uintptr_t)data, ncode, &mb, &mo) : 0;
    // 关键窗口是「头之后」：0x30 字节头只有 magic 有效，指令从 +0x30 起（图形/计算同一规则）。
    const uint8_t *ins = (const uint8_t *)data + 0x30;
    uint32_t ins_bytes = ncode > 0x30 ? ncode - 0x30 : 0;
    uint64_t w[4] = {0, 0, 0, 0};
    if (mapped && ins_bytes >= 32) memcpy(w, ins, 32);
    int ins_zero = mapped && ins_bytes >= 32 && !w[0] && !w[1] && !w[2] && !w[3];
    // 只打印可疑行：头后指令全零 / 阶段未知 / 计算阶段；再加前 24 条样例。
    if (!ins_zero && stage != 5 && stage >= 0 && atomic_fetch_add(&g_src_diag_budget, -1) <= 0) return;
    uint32_t cmagic = 0;
    if (ctl) memcpy(&cmagic, ctl, 4);
    uint32_t hmagic = 0;
    if (mapped && ncode >= 4) memcpy(&hmagic, data, 4);
    uint64_t key0 = 0; if (key) memcpy(&key0, key, 8);
    // 头/前缀布局取证：计算看 code[0..0x100]，图形看 code[0x30..0x90]，各自只打第一份；
    // 同一份还打 control 的 0x700..0x760（code 长度、c1 起点、线程组、共享内存等字段）。
    if (mapped && ncode >= 0x100 && ctl) {
        static _Atomic int cs_dumped, gfx_dumped;
        if (stage == 5 && atomic_fetch_add(&cs_dumped, 1) == 0) {
            char line[1024]; size_t o = 0;
            for (unsigned k = 0; k < 32; k++) {
                uint32_t v; memcpy(&v, (const uint8_t *)data + k * 4, 4);
                o += (size_t)snprintf(line + o, sizeof line - o, "%08x ", v);
            }
            line[o ? o - 1 : 0] = 0;
            LOG("source cs prefix(0x100): %s", line);
            o = 0;
            for (unsigned k = 0; k < 24; k++) {
                uint32_t v; memcpy(&v, (const uint8_t *)ctl + 0x700 + k * 4, 4);
                o += (size_t)snprintf(line + o, sizeof line - o, "%08x ", v);
            }
            line[o ? o - 1 : 0] = 0;
            LOG("source cs control(0x700..0x760): %s", line);
        }
        if (stage == 0 && atomic_fetch_add(&gfx_dumped, 1) == 0) {
            char line[1024]; size_t o = 0;
            for (unsigned k = 0; k < 24; k++) {
                uint32_t v; memcpy(&v, (const uint8_t *)data + 0x30 + k * 4, 4);
                o += (size_t)snprintf(line + o, sizeof line - o, "%08x ", v);
            }
            line[o ? o - 1 : 0] = 0;
            LOG("source gfx prefix(0x30..0xa0): %s", line);
            o = 0;
            for (unsigned k = 0; k < 24; k++) {
                uint32_t v; memcpy(&v, (const uint8_t *)ctl + 0x700 + k * 4, 4);
                o += (size_t)snprintf(line + o, sizeof line - o, "%08x ", v);
            }
            line[o ? o - 1 : 0] = 0;
            LOG("source gfx control(0x700..0x760): %s", line);
        }
    }
    // 计算阶段：整段扫一遍，给出「非零字节数 + 第一个非零字节偏移 + 头部 4 字」，
    // 用来区分「整段没写」和「指令在别的偏移」。
    uint32_t nz = 0, first_nz = 0xFFFFFFFFu, hdr[4] = {0, 0, 0, 0};
    if (stage == 5 && mapped && ncode) {
        const uint8_t *p8 = (const uint8_t *)data;
        uint32_t scan = ncode < (64u << 10) ? ncode : (64u << 10);
        for (uint32_t i = 0; i < scan; i++) if (p8[i]) { if (first_nz == 0xFFFFFFFFu) first_nz = i; nz++; }
        for (unsigned k = 0; k < 4 && (k + 1) * 4 <= scan; k++) memcpy(&hdr[k], p8 + k * 4, 4);
        // 第一个非零字节在头之后的位置：指令流真实起点 = 它按 4 字节向下对齐的位置。
        if (first_nz == 0x0) {
            for (uint32_t i = 0x30; i < scan; i++) if (p8[i]) { first_nz = i; break; }
        }
    }
    LOG("source %s: slot=%d stage=%d key=%016llx keyok=%d addr=0x%llx ncode=%u mapped=%d "
        "hmagic=0x%08x cmagic=0x%08x ins_zero=%d ins=%016llx %016llx %016llx %016llx "
        "scan={nz=%u first_nz=0x%x hdr=%08x %08x %08x %08x}",
        when, slot, stage, (unsigned long long)key0, key_ok, (unsigned long long)(uintptr_t)data, ncode,
        mapped, hmagic, cmagic, ins_zero, (unsigned long long)w[0], (unsigned long long)w[1],
        (unsigned long long)w[2], (unsigned long long)w[3],
        nz, first_nz, hdr[0], hdr[1], hdr[2], hdr[3]);
}
#endif
static void prog_source_pin(uint64_t serial, const void *owner, const uint8_t key[8]) {
    if (!key) return;
    // host 侧先按 key 前缀去重：重复 Bind 不进入 source_store 的 g_mu，也不做全表扫描。
    pthread_mutex_lock(&g_ps_mu);
    psnap_ent *e = ps_find(atomic_load_explicit(&g_ps_tab, memory_order_relaxed), serial);
    int have = e && nvn_program_pins_has_key(&e->pins, key);
    int released = e && e->pins.released;
    pthread_mutex_unlock(&g_ps_mu);
    if (have || released || !s2a_source_pin_key) return;
    struct s2a_source_pin_ref ref;
    if (s2a_source_pin_key(key, &ref) == 0) prog_pin_add(serial, owner, &ref);
}
void s2a_source_log_line(const char *line) { LOG("【红】%s", line); }
#endif
// 帧录（nvn_frame_rec.inc）：程序的各阶段键只在 SetShaders 那一刻算得出（控制段之后可能被游戏释放），录像要整张表带走，回放原样放回
typedef struct { uint64_t p; prog_keys_t k; } pk_rec;
static void pk_save1(const void *key, const void *val, void *ctx) { pk_rec r = {(uint64_t)(uintptr_t)key, *(const prog_keys_t *)val}; fwrite(&r, sizeof r, 1, (FILE *)ctx); }
int s2a_hle_pk_save(FILE *f) {
    pthread_mutex_lock(&g_pk_mu); uint64_t n = g_pk ? (uint64_t)CFDictionaryGetCount(g_pk) : 0;
    fwrite(&n, 8, 1, f); if (n) CFDictionaryApplyFunction(g_pk, pk_save1, f);
    pthread_mutex_unlock(&g_pk_mu);
    // 按序号的程序快照表随后：命令内存里的 OP_BIND_PROGRAM 只有序号，回放要靠它取程序内容
    pthread_mutex_lock(&g_ps_mu); psnap_tab *t = atomic_load_explicit(&g_ps_tab, memory_order_relaxed);
    fwrite(&g_ps_n, 8, 1, f);
    if (t) for (uint64_t i = 0; i < t->cap; i++) { psnap_ent *e = atomic_load_explicit(&t->slot[i], memory_order_relaxed);
        if (e) { fwrite(&e->snap, sizeof e->snap, 1, f); fwrite(atomic_load_explicit(&e->ks, memory_order_relaxed), sizeof(prog_keys_t), 1, f); } }
    pthread_mutex_unlock(&g_ps_mu); return (int)n;
}
// 读档换键（nvn_mtl.m 实现；安卓与不链 nvn_mtl.m 的单测没有它 = 原样）：存档里的键是存档那时的包的键，包更新成第 2 代后，第 1 代键按包内别名表 key_alias.tsv 换成第 2 代。
//   只看别名表、不重算：控制段在存档后可能已被游戏复用，按当前字节算出的键不可信。回 1 = 换了
// 弱链接[nvntest]：nvn_be_shader_key_canon（graphics/nvn/gfx/nvn/nvn_mtl.m；不链它的图形单测按这行放行未定义）
int nvn_be_shader_key_canon(uint8_t key[8]) __attribute__((weak));
static void pk_canon(prog_keys_t *k) {
    if (!nvn_be_shader_key_canon) return;
    for (int st = 0; st < 6; st++) if (k->ok >> st & 1) nvn_be_shader_key_canon(k->key[st]);
}
int s2a_hle_pk_load(FILE *f) {
    uint64_t n = 0, ns = 0; if (fread(&n, 8, 1, f) != 1) return -1;
    for (uint64_t i = 0; i < n; i++) { pk_rec r; if (fread(&r, sizeof r, 1, f) != 1) return -1; pk_canon(&r.k); prog_keys_t *k = pk_get((const void *)(uintptr_t)r.p, 1); *k = r.k; }
    if (fread(&ns, 8, 1, f) != 1) return -1;   // 11-01 前的录像没有快照表（它的 OP_BIND_PROGRAM 也是旧格式）：重录
    for (uint64_t i = 0; i < ns; i++) { prog_t snap; prog_keys_t k; if (snap_fread(&snap, sizeof snap, 1, f, "nvn.prog") != 1 || fread(&k, sizeof k, 1, f) != 1) return -1; pk_canon(&k); prog_snap_put(&snap, k.key, k.ok, k.stage); }
    return (int)n;
}
static void pk_drop(const void *p) {
    pthread_mutex_lock(&g_pk_mu);
    prog_keys_t *k = g_pk ? (prog_keys_t *)CFDictionaryGetValue(g_pk, p) : NULL;
    if (k) { CFDictionaryRemoveValue(g_pk, p); free(k); }
    pthread_mutex_unlock(&g_pk_mu);
}
#if defined(__ANDROID__) || defined(__OHOS__)
// S2Arm: hand each program stage to the platform's direct shader survey (Android compiles a missing bundle at SetShaders and writes it for the engine). Undefined on platforms without it.
extern void dm_shader_survey(const uint8_t key[8], uint64_t code, const void *control) __attribute__((weak));
// S2Arm Android 统一生产链的输入侧：保存有界自有的 code/control 快照（其他平台无此实现）。
extern int s2a_source_keep(const uint8_t key[8], uint32_t array_index, const void *code, uint32_t code_bytes,
                           const void *control, uint32_t control_bytes) __attribute__((weak));
// F4.2：control 只在 SetShaders 当次有效；key 还算不出来（code 未映射）时先自持 control，
// Bind 时用当时的 code 补算 key。全部弱绑定：其他平台没有实现时行为不变。
extern int s2a_source_hold_pending(uint64_t prog_serial, uint32_t stage, const void *control,
                                   uint32_t control_bytes) __attribute__((weak));
extern int s2a_source_pending_control_copy(uint64_t prog_serial, uint32_t stage, void *dst, uint32_t cap,
                                           uint32_t *n) __attribute__((weak));
extern int s2a_source_has(const uint8_t key[8]) __attribute__((weak));
extern void s2a_source_drop_pending_stage(uint64_t prog_serial, uint32_t stage) __attribute__((weak));   // 源已存好：释放该阶段的自持控制段

// Control +0x20 是该控制段的自描述长度：>=0x880 用 0x880；SDK 10 的 0x850 段只用它自己的长度。
static uint32_t prog_control_len(const void *control) { return nvn_key_ctl_len(control); }   // 与键同一口径（nvn_key.h）

// keys_hash 的“给定字节”版本：同一 control‖code 顺序，供 Bind 时用自持的 control 补算 key。
static int keys_hash_parts(const void *control, uint32_t nctl, const void *code, uint32_t ncode, uint8_t out[8]) {
    if (!control || !code || !nctl || !ncode) return 0;
    if (nctl > NVN_KEY_CTL_MAX) return 0;
    uint8_t md[CC_SHA256_DIGEST_LENGTH], kc[NVN_KEY_CTL_MAX];
    memcpy(kc, control, nctl); nvn_key_ctl_zero(kc, nctl);   // 第 2 代键（nvn_key.h）；安卓没有旧包，不做第 1 代兜底
    CC_SHA256_CTX hc;
    CC_SHA256_Init(&hc);
    CC_SHA256_Update(&hc, kc, nctl);
    CC_SHA256_Update(&hc, code, ncode);
    CC_SHA256_Final(md, &hc);
    memcpy(out, md, 8);
    return 1;
}

// NVN 阶段序号（0 vs / 1 fs / 2 gs / 3 tcs / 4 tes / 5 cs），从**调用方保证可读**的
// control/code 字节读定；认不出返回 -1。SetShaders 时 control 有效、code 在已映射池时算；
// Bind 时只用自持的 pending control + 当时已映射的 code，绝不重新解引用 guest control。
static int prog_stage_index_parts(const void *control, uint32_t nctl, const void *code, uint32_t ncode) {
    if (!control || !code || nctl < 0x708 || ncode < 4 || ncode >= (1u << 24)) return -1;
    const uint8_t *cd = (const uint8_t *)code;
    static const uint8_t gfx_magic[4] = { 0x78, 0x56, 0x34, 0x12 };
    if (memcmp(cd, gfx_magic, 4)) return 5;   // 不是图形头 = compute
    if (ncode < 0x34) return -1;               // 图形头还没到 stage 字段
    uint32_t w; memcpy(&w, cd + 0x30, 4);
    int t = (int)((w >> 10) & 0xf);
    if (t == 0 && (w & 0x1f) == 1) t = 1;
    switch (t) { case 1: return 0; case 5: return 1; case 4: return 2; case 2: return 3; case 3: return 4; default: return -1; }
}
#endif

// 着色器控制段 / 代码区地址可用：不在最低 64 KB 里（任何布局下那里都没有游戏内存；空地址加小偏移都落在这里，11-16 D-XDQS 代码区 = 0x100），且虚拟池里已映射（nvn_host_readable）。
//   不按游戏地址空间核权限：单测 / 回放工具的着色器在宿主自己的内存里，按地址空间核会误判、整批丢键
int nvn_host_readable(uint64_t a, uint64_t n);   // 本文件后面定义
// 弱链接[nvntest]：s2a_mem_mapped（游戏映射表，runtime/src/svc_mem.c；NVN 单测不链原生层，链接行按这行放行未定义，runtime/build.mk 自动收）
int s2a_mem_mapped(uint64_t addr, uint64_t size) __attribute__((weak));
// [a, a + n) 逐页真读得到（vm_read_overwrite 读不到只回错、不让进程崩；同 graphics/nvn/host/dsh_miss.inc dsh_bytes_ok）
static int pages_readable(uint64_t a, uint64_t n) {
#if defined(__APPLE__)
    uint64_t e = a + n, pg = (uint64_t)vm_page_size;
    if (e < a) return 0;
    for (uint64_t q = a & ~(pg - 1); q < e; q += pg) {
        uint8_t b; vm_size_t got = 0; uint64_t at = q < a ? a : q;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)at, 1, (vm_address_t)&b, &got) != KERN_SUCCESS || got != 1) return 0;
    }
#else
    (void)a; (void)n;
#endif
    return 1;
}
// 不在虚拟池里的地址原先一律当可读（#713 D-ZBQS 0.5.1：Switch 2 款着色器代码区落在游戏窗口里游戏没映射的一页，prog_key → CC_SHA256 SIGBUS）：
//   游戏映射表里有就照常读（常态，零额外开销）；映射表里没有再逐页真读一次，读不到才不算键（本阶段按「不在着色器表」报，不崩）。
//   映射表没有、但宿主自己的内存（单测 / 回放工具）逐页读得到，照常读，不误丢键
static int shader_bytes_readable(uint64_t a, uint64_t n) {
    if (a < 0x10000 || !nvn_host_readable(a, n)) return 0;
    if (s2a_mem_mapped && !s2a_mem_mapped(a, n) && !pages_readable(a, n)) {
        static _Atomic int said; if (atomic_fetch_add(&said, 1) < 8) LOG("【红】 shader bytes %#llx +%#llx are not mapped by the game and not readable: no key, the stage is reported missing", (unsigned long long)a, (unsigned long long)n);
        return 0; }
    return 1;
}
// sha256(ctl[0, nctl) ‖ code[0, ncode))[:8]（code 为 NULL 只哈希控制段）
static void keys_sha(const void *ctl, uint32_t nctl, const void *code, uint32_t ncode, uint8_t out[8]) {
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc);
    CC_SHA256_Update(&hc, ctl, nctl);
    if (code) CC_SHA256_Update(&hc, code, ncode);
    CC_SHA256_Final(md, &hc); memcpy(out, md, 8);
}
// 旧包兜底的取舍（nvn_mtl.m 实现；安卓与不链 nvn_mtl.m 的单测没有它 = 只用第 2 代键）：v2 / v1 = 同一份字节的第 2 / 1 代键，回 1 = 用 v1
// 弱链接[nvntest]：nvn_be_shader_key_pick（graphics/nvn/gfx/nvn/nvn_mtl.m；不链它的图形单测按这行放行未定义）
int nvn_be_shader_key_pick(const uint8_t v2[8], const uint8_t v1[8]) __attribute__((weak));
// mapped = 1: computed only when the code area is in a registered memory pool (at SetShaders: Mario sets some programs with an unmapped address first and fills it in before the first draw)
static int keys_hash(const NVNshaderData *sd, int mapped, uint8_t out[8]) {
    uint32_t ncode = 0; void *mb; uint64_t mo;
    if (!sd->control) return 0;
    // S2Arm: a control pointer that is no user address (above 2^47: Diablo III once had float data here, 0x3f80000000000000, and crashed reading it) is reported with the
    // program's state and not read; the stage then has no key and is reported missing from the map instead of crashing
    if ((uint64_t)(uintptr_t)sd->control >= (1ull << 47)) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("【红】 shader control section pointer %p is not an address (code %#llx): the program object was overwritten", sd->control, (unsigned long long)sd->data); return 0; }
    if (!shader_bytes_readable((uint64_t)(uintptr_t)sd->control, 0x880)) { static _Atomic int nc; if (atomic_fetch_add(&nc, 1) < 8) LOG("【红】 shader control section %p is not readable: no key, the stage is reported missing", sd->control); return 0; }
    // Switch 2 (NVN2 control section, nvn2.h): the key rule of nvn2_key. Keyed at the first use (prog_key, mapped = 0), never at SetShaders: on Pokopia 4 of 6 code areas hashed at
    //   SetShaders differed from the code at the first draw (pokopia-native, MIT)
    if (nvn2_on() && nvn2_ctl(sd->control)) {
        if (mapped) return 0;
        uint32_t n2 = nvn2_code_len(sd->control);
        if (sd->data && n2 && n2 < (1u << 24) && !shader_bytes_readable(sd->data, n2)) { static _Atomic int n2r; if (atomic_fetch_add(&n2r, 1) < 8) LOG("【红】 shader code area %#llx +%#x is not readable (NVN2 control %p): no key, the stage is reported missing", (unsigned long long)sd->data, n2, sd->control); return 0; }
        return nvn2_key(sd->control, sd->data && n2 && n2 < (1u << 24) ? (const void *)(uintptr_t)sd->data : NULL, out);   // as the Switch 1 rule below at mapped = 0
    }
    // 控制段 / 代码区读之前核可读（11-16 D-XDQS：代码区地址是游戏拿空缓冲地址加 0x100 算的 0x100，宿主读它段错误）：读不了就不算键，本阶段按「不在着色器表」报，不崩
    memcpy(&ncode, (const uint8_t *)sd->control + 0x704, 4);
    int code = sd->data && ncode && ncode < (1u << 24);
    if (code && !shader_bytes_readable(sd->data, ncode)) { static _Atomic int nd; if (atomic_fetch_add(&nd, 1) < 8) LOG("【红】 shader code area %#llx +%#x is not readable (control %p): no key, the stage is reported missing", (unsigned long long)sd->data, ncode, sd->control); return 0; }
    if (mapped && !(code && nvn_be_addr(sd->data, ncode, &mb, &mo))) return 0;
    // Control +0x20 holds the section's own length: sections of at least 0x880 bytes (all of Mario's, Engage's, Dread's and 13 Sentinels') use the first 0x880; SDK 10's 3D World has 0x850-byte ones followed by other data, so only their own length is used (shader_split.py ctl_len)
    //   the rule and the zeroing of two volatile spans: nvn_key.h
    uint32_t nctl = nvn_key_ctl_len(sd->control);
    const void *cd = code ? (const void *)(uintptr_t)sd->data : NULL;
    uint8_t kc[NVN_KEY_CTL_MAX]; memcpy(kc, sd->control, nctl); nvn_key_ctl_zero(kc, nctl);
    keys_sha(kc, nctl, cd, ncode, out);   // 第 2 代键
    // 旧包兜底：两段易变字段非零时第 1、2 代键不同；当前包的着色器表没有第 2 代、却有第 1 代（或设备兜底目录里有旧版本按第 1 代现编的），这一阶段就用第 1 代键，
    //   之后的查库 / 现编 / 首用复核（prog_key_live）都用它。规则只看包表与兜底目录、不随本局现编变，同一份字节每次算出同一个键（nvn_mtl.m nvn_be_shader_key_pick）
    if (nvn_be_shader_key_pick && !nvn_key_ctl_stable(sd->control, nctl)) {
        uint8_t v1[8]; keys_sha(sd->control, nctl, cd, ncode, v1);
        if (nvn_be_shader_key_pick(out, v1)) memcpy(out, v1, 8);
    }
    return 1;
}
// S2Arm #674: the key a Switch 2 stage would have with the code in place at SetShaders, only to start its library loading early. keys_hash leaves NVN2 stages unkeyed there
//   (the code can still change before the first draw, which keys them), and with no key at SetShaders the prefetch was never asked: every library of a Switch 2 game was loaded on the
//   submit thread at its first draw. A key computed from code that changes later is not in the package's map, so the prefetch drops it (nvn_be_shader_prefetch: no stage)
static int keys_hash_early(const NVNshaderData *sd, uint8_t out[8]) {
    void *mb; uint64_t mo;
    if (!sd->control || (uint64_t)(uintptr_t)sd->control >= (1ull << 47) || !nvn2_on() || !nvn2_ctl(sd->control)) return 0;
    uint32_t n2 = nvn2_code_len(sd->control);
    if (!sd->data || !n2 || n2 >= (1u << 24) || !nvn_be_addr(sd->data, n2, &mb, &mo)) return 0;
    return nvn2_key(sd->control, (const void *)(uintptr_t)sd->data, out);
}
// 首用算键的读数（11-18：4ff1884da 的修复走没走到，原先没有一行能数）：Switch 2 游戏每个阶段都在首用才算键，读的是 SetShaders 时自持的控制段（ctl_intern）；
//   魔数 / 版本不对 = 首用时读到的不是 NVN2 控制段（#1520 卡比修复前 98 支就是这样）。g_ctl_held / g_ctl_uniq：SetShaders 自持控制段的次数 / 去重后份数
// 局内实际用到的着色器（shader_seen.tsv，放在 pso_user.tsv 旁边，同 nvn_mtl.m pso_user_path：DM_PSO_USER 所在目录，否则存档目录 DM_SAVE 的上一级）：
//   每个绘制 / 派发第一次给某一阶段算出键时记一行「键<TAB>阶段」，只记键、不记着色器内容；同一个键只记一次（起局先读已有的，跨局累加）。
//   随诊断回收（ios/diagcollect.m、pack/jobs/library.py 同收 pso_user.tsv 那段），与同行公开清单合成「玩家会用到的着色器」名单：
//   出包只编会用到的那一部分之前，先要知道这份名单有多大、各局稳不稳（11-21 编批库方案）。写不了就整局不记，不影响游戏
#define SEEN_CAP (1u << 17)   // 不同着色器数的上限（一款大游戏局内见到的在 1 万上下）；满了不再记，不扩容
static pthread_mutex_t g_seen_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_seen_tab; static uint32_t g_seen_n; static FILE *g_seen_f; static int g_seen_state;   // 0 没开过；1 在记；-1 不记
static int seen_add(uint64_t v) {   // 开放寻址；0 留作空位（真有全零键时记成 1，只影响去重，不影响写出的键）
    uint64_t h = v ? v : 1; uint32_t i = (uint32_t)(h * 0x9E3779B97F4A7C15ull >> 47) & (SEEN_CAP - 1);
    while (g_seen_tab[i]) { if (g_seen_tab[i] == h) return 0; i = (i + 1) & (SEEN_CAP - 1); }
    if (g_seen_n >= SEEN_CAP / 2) return 0;
    g_seen_tab[i] = h; g_seen_n++; return 1;
}
static void seen_open_locked(void) {
    g_seen_state = -1;
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE"), *base = u && *u ? u : save && *save ? save : NULL;
    if (!base) return;
    char path[4096]; const char *slash = strrchr(base, '/');
    if (!slash || (size_t)(slash - base) + sizeof "/shader_seen.tsv" > sizeof path) return;
    snprintf(path, sizeof path, "%.*s/shader_seen.tsv", (int)(slash - base), base);
    if (!(g_seen_tab = calloc(SEEN_CAP, sizeof *g_seen_tab))) return;
    FILE *r = fopen(path, "r"); char line[64]; unsigned long long v;
    if (r) { while (fgets(line, sizeof line, r)) if (sscanf(line, "%16llx", &v) == 1) seen_add(v); fclose(r); }
    if (!(g_seen_f = fopen(path, "a"))) return;
    g_seen_state = 1;
    LOG("[nvn] 局内用到的着色器记到 shader_seen.tsv（只记键与阶段）：已有 %u 支", g_seen_n);
}
static void seen_note(const uint8_t key[8], unsigned st) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | key[i];   // 与 dsh_miss.inc 的键写法同序（逐字节 %02x）
    pthread_mutex_lock(&g_seen_mu);
    if (!g_seen_state) seen_open_locked();
    if (g_seen_state > 0 && seen_add(v)) { fprintf(g_seen_f, "%016llx\t%u\n", (unsigned long long)v, st); fflush(g_seen_f); }
    pthread_mutex_unlock(&g_seen_mu);
}
static _Atomic uint64_t g_ctl_held, g_ctl_uniq, g_n2_late, g_n2_late_bad;
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]) {
    if (st >= p->nstage) return 0;
    prog_keys_t *k = pk_get(p, 1);
    if (!(k->ok >> st & 1)) {
        int got = keys_hash(&p->sd[st], 0, k->key[st]); k->ok |= (uint8_t)(1u << st);
        if (nvn2_on() && p->sd[st].control && (uint64_t)(uintptr_t)p->sd[st].control < (1ull << 47)) {
            int ok = nvn2_ctl(p->sd[st].control); uint64_t c = atomic_fetch_add(&g_n2_late, 1) + 1, bad = ok ? atomic_load(&g_n2_late_bad) : atomic_fetch_add(&g_n2_late_bad, 1) + 1;
            if (!(c & (c - 1)) || !ok) { static _Atomic int said_bad; if (ok || atomic_fetch_add(&said_bad, 1) < 4)
                LOG("[nvn] Switch 2 着色器首用算键 %llu 次（控制段 NVN2 魔数 / 版本对 %llu、不对 %llu）；SetShaders 自持控制段 %llu 次（去重后 %llu 份）", (unsigned long long)c,
                    (unsigned long long)(c - bad), (unsigned long long)bad, (unsigned long long)atomic_load(&g_ctl_held), (unsigned long long)atomic_load(&g_ctl_uniq)); }
        }
        // S2Arm D-WQPH（11-15，Mac 奥日2 1.2.0：5 局每局 18–136 支「not in the shader map」，现编汇总成功 / 失败 / 跳过全 0）：SetShaders 时代码区还没进已登记的内存池
        //   （keys_hash mapped=1 不算键），那支没进首遇现编的待编表；到这里首用才算出键，dm_shader_miss_late 对 Switch 1 只认待编表里有的键，于是不在库里的整支丢、不现编。
        //   这里补登记（与 SetShaders 同一个入口：库里有就直接返回，没有就拷控制段进待编表 / 后台预编），控制段刚被 keys_hash 读过。NVN2 照旧走 dm_shader_miss_late 自己的首用登记
        if (got && !(nvn2_on() && nvn2_ctl(p->sd[st].control))) {
            static _Atomic uint64_t n; uint64_t c = atomic_fetch_add(&n, 1) + 1;
            if (!(c & (c - 1))) LOG("[nvn] shaders keyed at first use (code not in a registered memory pool at SetShaders): %llu so far; handed to the first-use compile like SetShaders' keys", (unsigned long long)c);
            dm_shader_miss(k->key[st], p->sd[st].data, p->sd[st].control);
        }
    }
    // 记局内用到的着色器：画 / 派发用到这一阶段时（键可能早在 SetShaders 时就算好了），按键去重（seen_note）；没算出键（全零）不记。
    //   不在 prog_keys_t 上加「已记」位：它原样进快照（gfx_progkeys.bin，s2a_hle_pk_save / load），加字段就改了快照格式
    { static const uint8_t zero[8]; if (memcmp(k->key[st], zero, 8)) seen_note(k->key[st], st); }
    memcpy(key, k->key[st], 8);
    return 1;
}
// S2Arm: a stage's key from the bytes there now (prog_key returns the one computed at SetShaders): equal to it only while the game still holds the same control section and code (nvn_mtl.m pso_fout_variant)
int prog_key_live(const prog_t *p, unsigned st, uint8_t key[8]) { return st < p->nstage && keys_hash(&p->sd[st], 0, key); }
// 首遇等待诊断（#1001，DM_NVN_FU_SETAT=1，缺省关）：按序号记 SetShaders 的时刻，nvn_mtl.m 的首遇等待行报「登记到这次绘制隔了多久」，
//   看两级建管线（SetShaders 时先建基础管线）有没有提前量。直接映射表 64K 项（1 MB，开了才分配）：同槽后来的序号顶掉先前的，查到序号对不上算没记到
enum { FU_SETAT_N = 1 << 16 };
static _Atomic uint64_t (*g_fu_setat)[2];
static int fu_setat_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_FU_SETAT"); on = e && *e && strcmp(e, "0"); } return on; }
static void fu_setat_note(uint64_t serial) {
    if (!fu_setat_on()) return;
    if (!atomic_load_explicit((_Atomic(void *) *)&g_fu_setat, memory_order_acquire)) {
        void *n = calloc(FU_SETAT_N, sizeof *g_fu_setat), *z = NULL;
        if (!n || !atomic_compare_exchange_strong((_Atomic(void *) *)&g_fu_setat, &z, n)) free(n);
    }
    _Atomic uint64_t *e = g_fu_setat ? g_fu_setat[serial & (FU_SETAT_N - 1)] : NULL; if (!e) return;
    atomic_store_explicit(&e[0], 0, memory_order_relaxed);
    atomic_store_explicit(&e[1], now_ns(), memory_order_relaxed);
    atomic_store_explicit(&e[0], serial, memory_order_release);
}
uint64_t nvn_prog_set_ns(uint64_t serial) {
    _Atomic uint64_t (*t)[2] = atomic_load_explicit((_Atomic(void *) *)&g_fu_setat, memory_order_acquire); if (!t) return 0;
    _Atomic uint64_t *e = t[serial & (FU_SETAT_N - 1)];
    if (atomic_load_explicit(&e[0], memory_order_acquire) != serial) return 0;
    uint64_t ns = atomic_load_explicit(&e[1], memory_order_relaxed);
    return atomic_load_explicit(&e[0], memory_order_acquire) == serial ? ns : 0;
}
#if !defined(__ANDROID__) && !defined(__OHOS__)
// 控制段只在 SetShaders 这次调用里保证有效（游戏之后可以复用那块内存），键却有留到首用（prog_key）才算的：Switch 2 阶段一律首用才算（代码区到首次绘制前还会变），
//   Switch 1 阶段在代码区还不在已登记内存池时也是。原先首用时读游戏的控制段指针，读到的可能已是别的数据：
//   0.5.7 iPhone 奥日2（Switch 1）+0x700 成了一个地址、+0x704 成了 0x76，键对不上库、代码长度取成 118 字节，现编越界；
//   一款 Switch 2 游戏 108 支现编失败里 98 支的「控制段」连 NVN2 魔数都没有，宿主按 Switch 1 硬读（#1520 当成「新版控制段没认出」）。
//   这里按内容去重拷一份自持，首用算键、首遇现编、补齐 / 输出类型变体都读它。同一份只存一份、不释放：总量以不同着色器数为界，程序快照也可能还引用着它。
//   （DM_NVN_SHDUMP 诊断档一直整份拷控制段，所以开转存复现不出来；安卓走自己的自持控制段 s2a_source_hold_pending）
// 弱链接[nvntest]：s2a_snap_conv_fread（runtime/src/snapshot.c 提供，经 snapfmt.h snap_fread 用：快照布局变了按字段转；图形单测不链宿主）
// 弱链接[nvntest]：s2a_snap_handout_blob（runtime/src/snapshot.c 提供；图形单测不链宿主，链接行按这行放行未定义）
void s2a_snap_handout_blob(const void *p, uint32_t n) __attribute__((weak));
typedef struct ctl_own { struct ctl_own *next; uint32_t n; uint8_t md[CC_SHA256_DIGEST_LENGTH]; uint8_t bytes[0x880]; } ctl_own;
static const void *ctl_intern(const void *ctl, uint32_t n) {
    enum { B = 4096 }; static ctl_own *tab[B]; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(ctl, n, md);
    uint32_t h; memcpy(&h, md, 4); h %= B;
    pthread_mutex_lock(&mu);
    ctl_own *e = tab[h];
    while (e && (e->n != n || memcmp(e->md, md, sizeof md))) e = e->next;
    int fresh = 0;
    if (!e && (e = calloc(1, sizeof *e))) { e->n = n; memcpy(e->md, md, sizeof md); memcpy(e->bytes, ctl, n); e->next = tab[h]; tab[h] = e; atomic_fetch_add(&g_ctl_uniq, 1); fresh = 1; }
    atomic_fetch_add(&g_ctl_held, 1);   // 0x880 以内没拷的部分是 0
    pthread_mutex_unlock(&mu);
    // 这份拷贝的地址会写进游戏内存里的程序对象：即时存档按内容身份登记（整 0x880 字节，含补零），恢复时重建、游戏里的旧地址换成新的（runtime/src/snapshot.c）
    if (fresh && s2a_snap_handout_blob) s2a_snap_handout_blob(e->bytes, (uint32_t)sizeof e->bytes);
    return e ? e->bytes : ctl;
}
// 要首用才算键的这一阶段，控制段该自持多少字节（0 = 不自持：已在 SetShaders 算出键、控制段指针不是地址或读不了）
static uint32_t ctl_hold_len(const NVNshaderData *sd, int keyed) {
    if (!sd->control || (uint64_t)(uintptr_t)sd->control >= (1ull << 47)) return 0;
    uint64_t a = (uint64_t)(uintptr_t)sd->control;
    if (nvn2_on() && shader_bytes_readable(a, 8) && nvn2_ctl(sd->control))
        return shader_bytes_readable(a, NVN2_CTL_COPY) ? NVN2_CTL_COPY : shader_bytes_readable(a, 0xd8) ? 0xd8 : 0;
    return !keyed && shader_bytes_readable(a, 0x880) ? 0x880 : 0;
}
#endif
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("【红】 ProgramSetShaders stage count %d", n); return 0; }
    NVNshaderData sdc[6]; for (int i = 0; i < n; i++) { sdc[i] = sd[i]; sdc[i].data = GA(sd[i].data); } sd = sdc;   // 代码区地址换回 CPU（见 GPU 地址空间）
#if defined(__ANDROID__) || defined(__OHOS__)
    uint64_t old_serial = o->serial;   // 旧阶段的 pin 等到新阶段存好/ pin 好之后再解，换 SetShaders 期间不留空窗
#endif
    nvn_be_prog_release(o); o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    fu_setat_note(o->serial);
    // The keys are computed here (see prog_key in nvn_obj.h): the control section is only guaranteed valid during this call; the code area lives in a memory pool and stays valid while the program is in use
    static int dump = -1; if (dump < 0) dump = getenv("DM_NVN_SHDUMP") != NULL;
    prog_keys_t *k = pk_get(p, 1);
    k->ok = 0;
    memset(k->stage, 0xff, sizeof k->stage);
#if defined(__ANDROID__) || defined(__OHOS__)
    struct s2a_source_pin_ref pinrefs[NVN_PROGRAM_PIN_MAX];
    int npin = 0;
#endif
    uint8_t pre_keys[6][8]; memset(pre_keys, 0, sizeof pre_keys); (void)pre_keys;   // 各阶段的键（SetShaders 算出的或首用才定的早键），给 nvn_be_pair_prefetch
    for (int i = 0; i < n; i++) {
        if (keys_hash(&sd[i], 1, k->key[i])) { k->ok |= (uint8_t)(1u << i); dm_shader_miss(k->key[i], sd[i].data, sd[i].control); nvn_be_shader_prefetch(k->key[i]); memcpy(pre_keys[i], k->key[i], 8); }   // S2Arm: a key missing from the map is translated on the device now (graphics/nvn/host_adapt.m)
        else { uint8_t early[8]; if (keys_hash_early(&sd[i], early)) { nvn_be_shader_prefetch(early); memcpy(pre_keys[i], early, 8); } }   // S2Arm #674: stages keyed at first use
#if !defined(__ANDROID__) && !defined(__OHOS__)
        // 首用才算键的阶段（Switch 2 一律；Switch 1 代码区还不在已登记内存池时），控制段先自持一份（ctl_intern）
        { uint32_t hl = ctl_hold_len(&sd[i], k->ok >> i & 1); if (hl) o->sd[i].control = ctl_intern(sd[i].control, hl); }
#endif
#if defined(__ANDROID__) || defined(__OHOS__)   // Android 直连的阶段 / 源快照 / 按 serial 记 key；Apple 回放不读这些，保持 main 原行为
        // 阶段只在 control 仍有效、code 已映射的本次调用内读定；code 未映射时留给 Bind
        // 用自持 control + 当时的 code 补算，Bind 不碰 guest control。
        if (sd[i].control && (uintptr_t)sd[i].control < (1ull << 47)) {
            uint32_t ncode = 0; memcpy(&ncode, (const uint8_t *)sd[i].control + 0x704, 4);
            void *mb; uint64_t mo;
            if (ncode && ncode < (1u << 24) && sd[i].data && (uintptr_t)sd[i].data < (1ull << 47) &&
                nvn_be_addr(sd[i].data, ncode, &mb, &mo)) {
                int st = prog_stage_index_parts(sd[i].control, prog_control_len(sd[i].control),
                                                (const void *)(uintptr_t)sd[i].data, ncode);
                if (st >= 0) k->stage[i] = (uint8_t)st;
            }
        }
        {
            uint32_t ncode = 0;
            if (sd[i].control && (uintptr_t)sd[i].control < (1ull << 47))
                memcpy(&ncode, (const uint8_t *)sd[i].control + 0x704, 4);
            int st_res = (k->stage[i] == 0xff) ? -1 : k->stage[i];
            src_diag("setshaders", (int)i, st_res, k->key[i], (const void *)(uintptr_t)sd[i].data, ncode,
                     sd[i].control, (int)((k->ok >> i) & 1));
        }
        if (dm_shader_survey) dm_shader_survey(k->key[i], sd[i].data, sd[i].control);   // S2Arm: direct runtime bundle survey (Android)
        if (sd[i].control && (uintptr_t)sd[i].control < (1ull << 47)) {
            uint32_t nctl = prog_control_len(sd[i].control);
            // F4.2：control 只在本次调用内有效，先自持；key 已算出时立刻存完整源。
            // 代码尚未映射（key 没算出来）时留给 Bind 用当时的 code + 这份 control 补算，不在这里存零键。
            if (s2a_source_hold_pending)
                (void)s2a_source_hold_pending(o->serial, (uint32_t)i, sd[i].control, nctl);
            if (s2a_source_keep && sd[i].data && ((k->ok >> i) & 1)) {
                uint32_t ncode = 0; memcpy(&ncode, (const uint8_t *)sd[i].control + 0x704, 4);
                if (ncode && ncode < (1u << 24)) {
                    if (s2a_source_keep(k->key[i], (uint32_t)i, (const void *)(uintptr_t)sd[i].data, ncode,
                                        sd[i].control, nctl) == 0 && k->stage[i] != 0xff && s2a_source_drop_pending_stage)
                        s2a_source_drop_pending_stage(o->serial, (uint32_t)i);   // 源已存好、键与阶段都已算出：刚自持的控制段冗余（Bind 不会再要）
                    // 立刻 pin 住；先不挂到 psnap_ent（该条目还没建），循环后统一记账，避免 N5 的额外 prog_keys。
                    if (s2a_source_pin_key && npin < NVN_PROGRAM_PIN_MAX) {
                        int dup = 0;
                        for (int j = 0; j < npin; j++)
                            if (!memcmp(pinrefs[j].key, k->key[i], 8)) { dup = 1; break; }
                        if (!dup) {
                            struct s2a_source_pin_ref ref;
                            if (s2a_source_pin_key(k->key[i], &ref) == 0) pinrefs[npin++] = ref;
                        }
                    }
                }
            }
        }
#endif
        // DM_NVN_SHDUMP (debugging): keep a copy of the control section so the replay layer can save shaders missing from the map (never freed)
        if (dump && sd[i].control) { size_t cl = nvn2_on() && nvn2_ctl(sd[i].control) ? NVN2_CTL_COPY : 0x880; void *c = calloc(1, 0x880); memcpy(c, sd[i].control, cl); o->sd[i].control = c; }   // NVN2: the section is only 0xe0 bytes
    }
#if defined(__ANDROID__) || defined(__OHOS__)
    prog_snap_put(o, k->key, k->ok, k->stage);   // 安卓：设置时就按序号存（游戏可能按值复制程序对象再绑定，控制段在本次调用后可能失效）
    prog_pin_set_owner(o->serial, o);             // 该 serial 的 pin 归属固定为原件，拷贝先 Bind 不抢 owner
    for (int i = 0; i < npin; i++) prog_pin_add(o->serial, o, &pinrefs[i]);
    prog_pin_release(old_serial, o);   // 新阶段已经 pin 好：旧阶段不再被当前 program 引用，源快照可淘汰
#endif
#if !defined(__ANDROID__) && !defined(__OHOS__)
    nvn_be_pair_prefetch((const uint8_t (*)[8])pre_keys, n);   // 这对顶点 / 片元的随包拉取基础管线先在后台取（nvn_mtl.m），首遇直接用拉取管线画
#endif
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}

// ---------- Sync / events ----------
// 别的队列（GL 层）的栅栏：GPU 上让此后的 NVN 提交等它 / CPU 上等它（is_ext = 0：不是别的队列的栅栏）；定义在 nvnQueueSubmitCommands 前（要 g_sub_mu）
static void sync_ext_queue_wait(const sync_t *s);
static int sync_ext_wait(const sync_t *s, uint64_t timeout, int *is_ext);
NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; ((sync_x *)s)->rm = 0; return 1; }
// S2Arm (10-30, Metroid Dread, census c1029c: "NVN address lookup of an unknown name: nvnSyncInitializeFromNvRmSyncNVX", then a crash in subsdk0 on the null it got): a sync made from
//   another engine's NvRmSync (on the Switch a host1x syncpoint fence, e.g. the video decoder's). Here those engines are Ryujinx services that finish on the CPU before they hand the
//   fence over, so the sync starts signaled (fenced at 0: nvnSyncWait / QueueWaitSync return at once). The third argument is the NvRmSync; its layout is not used. First call logged
NVN_NOPROTO(nvnSyncInitializeFromNvRmSyncNVX, NVNboolean, NVNsync *s, NVNdevice *d, const void *rm) {
    (void)d; static int once; if (!once++) { uint32_t w[2] = {0, 0}; if (rm) memcpy(w, rm, sizeof w); LOG("[nvn] SyncInitializeFromNvRmSyncNVX: first call, NvRmSync words %#x %#x; the sync starts signaled", w[0], w[1]); }
    *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; ((sync_x *)s)->rm = NVN_SYNC_RM; sync_publish((sync_t *)s, 0);   // rm：真机置类型 5（20.5 0x4d0720），Sync{Get,Set}SyncpointsNVX 只认它
    return 1;
}
// S2Arm (10-31, 直连缺陷表第 4 名：蓝精灵 Dreams、塞尔达传说 智慧的再现、密特罗德 Dread，「NVN address lookup of an unknown name: nvnSyncGetNvRmSyncNVX」):
//   the reverse of InitializeFromNvRmSyncNVX: the game hands an NVN sync to another engine (vi / the video decoder) as an NvRmSync (host1x syncpoint fence: id, value).
//   Those engines are Ryujinx services here and have no host1x syncpoint for our GPU work, so the work is waited for on the CPU first (the sync's fence, until it is reached) and the fence
//   handed out is the invalid syncpoint (id 0xFFFFFFFF, Ryujinx NvFence.InvalidSyncPointId), which every consumer treats as "nothing to wait for". A sync with no fence yet exports the same
// 审计 #1534 P1-47：原先最多等 1 秒，没等到也照样导出「已触发」并回真，消费方（vi / 视频解码）会读还没画完的图。现在等到栅栏真的完成才导出
//   （同 nvnQueueFinish：nvn_be_wait 不设 1 秒上限，只在会话收尾或 1 小时封顶时放弃；GL 栅栏同样等到为止）；没等到（会话收尾）报红、不写 rm、回假，不报完成。
//   普查 11-04 485 份 host.log 里 6 份调过这个入口，「1 s 没等到」0 次：等得到的调用行为不变
NVN_NOPROTO(nvnSyncGetNvRmSyncNVX, NVNboolean, const NVNsync *s, void *rm) {
    uint64_t fence = 0; int has = s && sync_published((const sync_t *)s, &fence), ext = 0;
    if (has && !sync_ext_wait((const sync_t *)s, UINT64_MAX, &ext) && ext) { static int n; if (n++ < 4) LOG("【红】 SyncGetNvRmSyncNVX: GL fence not reached (session ending); not exported"); return 0; }
    if (has && fence && !nvn_be_wait(fence, UINT64_MAX)) { static int n; if (n++ < 4) LOG("【红】 SyncGetNvRmSyncNVX: GPU fence %llu not reached (session ending); not exported", (unsigned long long)fence); return 0; }
    if (rm) { const uint32_t w[2] = {0xFFFFFFFFu, 0}; memcpy(rm, w, sizeof w); }
    static int once; if (!once++) LOG("[nvn] SyncGetNvRmSyncNVX: first call (fence %llu%s); exported as the invalid syncpoint (already signaled)", (unsigned long long)fence, has ? "" : ", none yet");
    return 1;
}
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
NVN(nvnSyncSetDebugLabel, void, NVNsync *s, const char *l) { (void)s; (void)l; }
// Fence = "all commands previously submitted to the queue have completed": record the fence value of the most recent submit
static void xq_lock(void), xq_unlock(void);   // 提交锁 g_sub_mu（定义在 nvnQueueSubmitCommands 前）
// 这条队列有停在跨队列等待上的工作（nvn_mtl.m nvn_xqueue.inc）时，栅栏要等那部分回放提交后才算：欠着，由后端回放那部分时发布
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) {
    (void)c; (void)f; xq_lock(); int owed = nvn_be_xq_fence(q, s); if (!owed) sync_publish((sync_t *)s, nvn_be_last_fence()); xq_unlock();
}
// A sync object that was never fenced waits until timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
// A GPU-side wait on a sync: the host has a single Metal queue that executes submissions in order, so commands before the fence are always done first and nothing needs waiting. A sync not fenced yet (fenced later by another thread) is not waited for here; logged once
//   来自 GL 的同步对象（nvnSyncInitializeFromFencedGLSync）在别的队列上：此后提交的 NVN 工作在 GPU 上等 GL 层共享事件到那个提交序号（nvn_be_queue_wait_event，跨队列真等）
// 审计 #1534 P1-23 被动采样（只计数、不改行为）：QueueWaitSync 次数、其中同步对象还没打栅栏的次数，进帧钩子里的「queue sync waits」汇总行（每 300 帧，0 也打）
uint64_t g_nvn_qws_n, g_nvn_qws_unfenced;   // 非 static：单测 t_nvn_audit1534_probes 直接读
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) {
    __atomic_fetch_add(&g_nvn_qws_n, 1, __ATOMIC_RELAXED);
    // 审计 #1646 P1-23：没打栅栏的同步对象，宿主不等（单 Metal 队列，等也无从挂到 GPU 上），所以不回成功：回假、报红一次（11-07 质量线：最近约 300 份 host.log
    //   的 P1-23 探针「on a sync not fenced yet」全是 0，没有款走到这一支；不在 CPU 上阻塞等别的线程打栅栏——同一线程「先等后打」会死锁，真机上等的是 GPU）
    if (!sync_published((const sync_t *)s, NULL)) {
        if (__atomic_fetch_add(&g_nvn_qws_unfenced, 1, __ATOMIC_RELAXED) == 0)
            LOG("【红】[nvn] QueueWaitSync: the sync object has no fence yet; the host cannot wait for it, returns false (first case: queue %p, sync %p; audit #1646 P1-23)", (void *)q, (const void *)s);
        return 0;
    }
    { void *owner = NULL; if (nvn_be_xq_owed(s, &owner)) { xq_lock(); nvn_be_xq_wait_queue(q, owner); xq_unlock(); } }   // 等别的队列欠着的栅栏：这条队列之后的提交排在那条队列停住的工作后面
    sync_ext_queue_wait((const sync_t *)s); return 1;
}
// ZCull data: the host does no ZCull, so there is nothing to save / restore
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
// 没打过栅栏的同步对象：立即回 FAILED(3)，不论超时。11-19 按真机驱动核定（一款 UE4 游戏随包的 NVN 驱动：nvnSyncInitialize 把类型字段置 0，nvnSyncWait 先置结果 3、
//   按类型跳表，类型 0 直达收尾返回 3；nvnCommandBufferFenceSync 录制时不碰同步对象，提交后才有类型）。原先回 ALREADY_SIGNALED：诊断 #343862（010097100EDD6000，
//   3 台 23 次崩）UE4 粒子实例计数回读先 Clear 栅栏（Finalize + Initialize）、把拷贝与栅栏排给 RHI 线程，工作线程以超时 0 轮询，拿到 0 当成数据已就绪，
//   去锁还没建出的暂存缓冲（+0x18 空）读 0x28 崩。下面两款的约束照样满足：都是立即返回，DOOM Eternal 只在 TIMEOUT 时睡下再查。
// 以下为旧注释（「回已触发」的依据是推断，真机实为 FAILED）：A sync never fenced counts as signaled: ALREADY_SIGNALED at once, whatever the timeout. 11-06（DOOM Eternal 起局 90 秒无首帧，c1106_rj3）：渲染线程在
//   main+0x2c2cac0 以超时 0 轮询每帧两个同步对象里的「上一个」，回 TIMEOUT_EXPIRED 就睡 50 µs 再查（svc 0xb 21 万次）；两个槽只 SyncInitialize、要提交后才
//   QueueFenceSync，帧开头先等的槽 1 第一次一定没打过栅栏——游戏在真机上能跑，真机对「初始化过、没打栅栏」的同步对象就不回超时（同 InitializeFromNvRmSyncNVX
//   「一开始就已触发」）。下面怒之铁拳4 的约束（带长超时等没围栏的对象，必须立刻返回）照样满足。原注释：A sync never fenced returns TIMEOUT_EXPIRED at once. 11-01 原生崩溃线：缺口族 40（34a54ae83）把这里改成「睡到别的线程围栏或超时、封顶 1 小时」，
//   依据是「按超时重试的游戏会空转」的推断（缺口扫描原文标「推断，待实测」），没有一款游戏的读数；怒之铁拳4 开局就在一个没围栏的同步对象上带长超时 SyncWait，
//   主线程睡在这里 120 秒无首帧（10-29 普查立刻返回时直连通过）。立刻返回是普查全部款跑过的行为
// 栅栏打在有停住工作的队列上（欠着，nvn_mtl.m nvn_xqueue.inc）：先等它发布（别的线程提交了那笔信号、停住的部分回放），按游戏给的超时；无限等时 100 ms 还没发布就把那条队列
//   停住的工作强制放行回放（报红），不让 CPU 等死在一个可能只由本线程之后才提交的信号上
// 只读计数（单测按事件判、不量墙钟，t_nvn_glsync ⑦）：nvnSyncWait 进入带超时（> 0）阻塞等待的次数。只增不减，不改行为
static _Atomic uint64_t g_syncwait_blocks;
uint64_t s2a_nvn_syncwait_blocks(void) { return atomic_load_explicit(&g_syncwait_blocks, memory_order_relaxed); }
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    uint64_t fence;
    { void *owner = NULL;
      if (nvn_be_xq_owed(s, &owner)) {
          uint64_t first = timeout < 100000000ull ? timeout : 100000000ull;
          if (first) atomic_fetch_add_explicit(&g_syncwait_blocks, 1, memory_order_relaxed);
          if (!nvn_be_xq_owed_wait(s, first)) {
              if (timeout <= first) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
              xq_lock(); nvn_be_xq_flush(owner, "the CPU waits on a fence taken behind it (SyncWait, 100 ms)"); xq_unlock();
          } } }
    if (!sync_published((const sync_t *)s, &fence)) return NVN_SYNC_WAIT_RESULT_FAILED;   // 没打过栅栏：同真机驱动回 3（见上）
    { int ext = 0; if (sync_ext_wait((const sync_t *)s, 0, &ext)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
      if (ext) { if (timeout) atomic_fetch_add_explicit(&g_syncwait_blocks, 1, memory_order_relaxed);
                 return sync_ext_wait((const sync_t *)s, timeout, &ext) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED; } }
    if (nvn_be_wait(fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    if (timeout) atomic_fetch_add_explicit(&g_syncwait_blocks, 1, memory_order_relaxed);
    return nvn_be_wait(fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventBuilderGetStorage, const NVNmemoryPool *, const NVNeventBuilder *b, int64_t *off) {
    const ev_builder_t *o = (const ev_builder_t *)b; *off = o->off; return (const NVNmemoryPool *)o->pool;
}
NVN(nvnEventBuilderGetMemoryPool, const NVNmemoryPool *, const NVNeventBuilder *b) { return (const NVNmemoryPool *)((const ev_builder_t *)b)->pool; }
NVN(nvnEventBuilderGetMemoryOffset, int64_t, const NVNeventBuilder *b) { return ((const ev_builder_t *)b)->off; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("【红】 EventInitialize has no storage"); return 0; }
    *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off), s->pool, s->off}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetMemoryPool, const NVNmemoryPool *, const NVNevent *e) { return (const NVNmemoryPool *)((const ev_t *)e)->pool; }
NVN(nvnEventGetMemoryOffset, int64_t, const NVNevent *e) { return ((const ev_t *)e)->off; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v; else *e->val = v; nvn_be_event_written(e->val); }   // 写后复查在等这个值的 GPU 等待（nvn_mtl.m ev_wait；安卓 nvn_be_android.c）
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

// ---------- Queue ----------
#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder2)); QB(b)->dev = dev;   // queue_builder2：连 NVN2 的主队列 / 光追内存一起清（nvn2_cmd.h）
    QB(b)->priority = 100;   // The memory queries report only explicitly configured values.
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; QB(b)->set_cmd = 1; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; QB(b)->set_comp = 1; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; QB(b)->set_ctl = 1; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; QB(b)->set_flush = 1; }
NVN(nvnQueueBuilderGetDevice, const NVNdevice *, const NVNqueueBuilder *b) { return ((const queue_builder_t *)b)->dev; }
NVN(nvnQueueBuilderGetFlags, int, const NVNqueueBuilder *b) { return (int)((const queue_builder_t *)b)->flags; }
NVN(nvnQueueBuilderGetCommandMemorySize, NVNboolean, const NVNqueueBuilder *b, int *n) { const queue_builder_t *o = (const queue_builder_t *)b; *n = (int)o->cmd_sz; return o->set_cmd; }
NVN(nvnQueueBuilderGetComputeMemorySize, NVNboolean, const NVNqueueBuilder *b, int *n) { const queue_builder_t *o = (const queue_builder_t *)b; *n = (int)o->comp_sz; return o->set_comp; }
NVN(nvnQueueBuilderGetControlMemorySize, NVNboolean, const NVNqueueBuilder *b, int *n) { const queue_builder_t *o = (const queue_builder_t *)b; *n = (int)o->ctl_sz; return o->set_ctl; }
NVN(nvnQueueBuilderGetCommandFlushThreshold, NVNboolean, const NVNqueueBuilder *b, int *n) { const queue_builder_t *o = (const queue_builder_t *)b; *n = (int)o->flush; return o->set_flush; }
NVN(nvnQueueBuilderGetMemorySize, int, const NVNqueueBuilder *b) { return (int)((const queue_builder_t *)b)->mem_sz; }
NVN(nvnQueueBuilderGetMemory, void *, const NVNqueueBuilder *b) { return ((const queue_builder_t *)b)->mem; }
NVN(nvnQueueBuilderGetQueuePriority, int, const NVNqueueBuilder *b) { return ((const queue_builder_t *)b)->priority; }
// Queue memory: the sum of the command + compute + control blocks rounded to 4 KiB (the host does not use it; the game only needs to allocate this size)
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    size_t cmd = o->set_cmd ? o->cmd_sz : 65536, comp = o->set_comp ? o->comp_sz : 262144, ctl = o->set_ctl ? o->ctl_sz : 16384;
    // S2Arm（11-13 金手指堆偏移根修）：照真驱动算（奥德赛 SDK 驱动 sdk+0x412f60 反汇编、Mac 上跑真驱动核过 0xA9000）：命令 + 控制原样，
    //   计算内存不原样加，按每 0x800 一个记录（0x600 + 0x200）折算；另加设备常数部分：片段互锁（标志位 0 没设：(0x80×2×4 + (0xff | 1<<10)) & ~0xff = 0x800）、
    //   固定 0x7b00（非调试设备）、zcull（标志位 1 没设：先按 0x1000 取整再加 0x1000）。设备字段取自 Tegra X1 真驱动（+0x70 = 0x80、+0x78 = 2、
    //   +0x7c = 1、zcull 大小 1 / 对齐 0x1000）。原先 命令 + 计算 + 控制 少 0x9000，游戏从自己堆里少分配，此后对象整体前移
    size_t w = (o->flags & 1) ? 0 : ((0x80u * 2u * 4u + (0xffu | (1u << 10))) & ~(size_t)0xff);
    w += 0x7b00;
    if (!(o->flags & 2)) w = ((w + 0xfff) & ~(size_t)0xfff) + 0x1000;
    size_t n = comp / 0x800;
    w = ((w + 0xff) & ~(size_t)0xff) + n * 0x800;
    w = (w + 0xfff) & ~(size_t)0xfff;
    return cmd + ctl + w;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
// 队列内存记账：NVN 的三个 GetTotal*MemoryUsed 报的是「这个队列当前被放进来的内存」，
// 来源就是 builder 上 SetQueueMemory / SetControlMemorySize / SetComputeMemorySize 给的值；Reset 之后重新从 0 累计，
// 所以这里在 Initialize 时把 builder 的三项抄进队列对象，Reset 清零即可，不需要额外的分配器记账。
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    // 正面判定的「光栅翻转」位按建队列那一刻的窗口原点定：游戏自带驱动（极品飞车 SDK 10 / 暗黑3 SDK 8 的 sdk 模块同一段代码）只在 QueueInitialize 的队列初值里写一次
    //   Maxwell 3D Y_CONTROL（方法 0x4EB，立即数 0x801004EB = TriangleRastFlip、0x800004EB = 0），条件是设备 +0x1988（SetWindowOriginMode 存的原点）不等于左上角；
    //   SetWindowOriginMode / SetDepthMode 只存设备字段、不再写它。所以先建队列、后设左上角的游戏（极品飞车）光栅翻转仍开着，正面方向与先设原点的（暗黑3）相反
    const dev_t_ *dv = dev_ok(o->dev) ? (const dev_t_ *)o->dev : NULL;
    uint32_t flip = !dv || dv->origin != NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    *(queue_t *)q = (queue_t){MAGIC('Q', 'U', 'E', 'U'), o->dev, 0, o->mem_sz, o->set_ctl ? o->ctl_sz : 0, o->set_comp ? o->comp_sz : 0, flip};
    ((queue_x *)q)->timeout_ns = 0; ((queue_x *)q)->timeslice_us = 0;   // NVX 记下的超时 / 时间片（nvn_nvx.h）
    s2a_nvn_rast_flip = (int)flip;
    LOG("[nvn] QueueInitialize %p: window origin at queue creation %s, front-face raster flip %s", (void *)q, flip ? "lower left" : "upper left", flip ? "on" : "off");
    return 1;
}
NVN(nvnQueueGetTotalCommandMemoryUsed, size_t, NVNqueue *q) { return (size_t)((const queue_t *)q)->cmd_bytes; }
NVN(nvnQueueGetTotalControlMemoryUsed, size_t, NVNqueue *q) { return (size_t)((const queue_t *)q)->ctl_bytes; }
NVN(nvnQueueGetTotalComputeMemoryUsed, size_t, NVNqueue *q) { return (size_t)((const queue_t *)q)->comp_bytes; }
NVN(nvnQueueResetMemoryUsageCounts, void, NVNqueue *q) {
    queue_t *o = (queue_t *)q; o->cmd_bytes = o->ctl_bytes = o->comp_bytes = 0;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { ((queue_t *)q)->magic = 0; }
// 审计 1101 18:00 P2-6：本层有 GPU 执行错误记录（nvnDeviceWaitForError 交出的那份）时仍按无错误回——如实上报会让轮询的游戏走进自己的出错分支，
//   没有现场读数前不改语义，只在「有记录却报无错」时打一次红线，作为定语义的读数
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info);
#if !defined(__ANDROID__) && !defined(__OHOS__)   // 错误记录是 Metal 后端的（nvn_be_wait_error，同 nvnDeviceWaitForError 的条件）
    { static uint64_t seen; static int once; if (nvn_be_wait_error(&seen, 0) && !once++) LOG("【红】 [nvn] QueueGetError: the host has a recorded GPU execution error but reports GPU_NO_ERROR (queue error reporting not implemented; first case)"); }
#endif
    return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
static void queue_finish(void);   // 持提交锁（g_sub_mu，定义在 nvnQueueSubmitCommands 前）
NVN(nvnQueueFinish, void, NVNqueue *q) { xq_lock(); nvn_be_xq_flush(q, "QueueFinish on it"); xq_unlock(); queue_finish(); }   // 停在跨队列等待上的工作先放行（nvn_xqueue.inc）

// ---------- Command buffer: records go to host shadow blocks; the game's command memory is only each recording's identity and anchor ----------
// The game's command memory used to hold the host records themselves, and GetCommandMemoryUsed reported host record bytes, 5-10 times the real device's commands. Games that read
// the count were misled (Mario Tennis Aces sizes a copy target by the source's Used and ran its 16 MB pool dry, main+0x62aac; Tears of the Kingdom, GitHub issue #61, gives each
// model's prerecorded lists slots of the measured size and our records overran into the next slot; Fire Emblem: Three Houses takes chunk start + Used as the GPU's ring position;
// Breath of the Wild sums the prerecorded sizes into each model's resource table). Reporting a smaller count while still writing the game's memory is no fix: the game packs the
// next recording right behind that count and our bytes would land on it. So:
//   - every recording (BeginRecording .. EndRecording) writes its records into host blocks of its own (sblk, chained by OP_JUMP); the handle is its first record's host address,
//     which replay, CallCommands and CopyCommands follow as before. The game's memory is not written, except the one word a real BindTexture command has there (rec_bind_tex1);
//   - a device cursor walks the game's memory as the real driver would: each record advances it by dev_bytes(op, size), an estimate of the NVN device command. Used / Free / Size
//     and the out-of-memory callback (its size included) all count device bytes, so the callback comes when the real driver's would;
//   - a recording remembers the device intervals it took in the game's memory. When the device cursor later runs over one of them, the game is re-recording that memory, and
//     the old recording is dead as on the device, where its commands were just overwritten: its host blocks are released (sh_overwrite). Granting memory alone (AddCommandMemory)
//     is not that signal: Tears of the Kingdom grants max(size, 512) for a slot of `size` bytes, overlapping the next slots, and only writes its own.
#define CB(c) ((cmd_t *)(c))
// GetCommandMemorySize 的读数：游戏最近一次 AddCommandMemory 给的段长，只在 AddCommandMemory 时变（真机语义），录制中不变。存在 NVNcommandBuffer 0xA0 里 cmd_t 之后：
//   cmd_t 在 nvn_obj.h（direct 阶段键的输入，改了全部款冷出库），这一项只有本文件读写。原先 Size = Used + Free 现算，录制中会变：写记录时 cur 先进、used 后进，
//   换段时跳转先计入 used 再清零，借宿主段后 used 不清零而 Free 换成宿主段剩余。大乱斗（01006A800016E000）在 libc++ introsort（main+0x3850850）里按 Size 给命令缓冲排序，
//   比较前后不一致 → 不查边界的扫描越过数组头，main+0x38509ec 读 0x50 崩
typedef struct { cmd_t c; uint64_t seg_size; } cmd_seg_t;
_Static_assert(sizeof(cmd_seg_t) <= sizeof(NVNcommandBuffer), "cmd_seg_t exceeds the reserved size of NVNcommandBuffer");
#define CB_SEG(c) (&((cmd_seg_t *)(c))->seg_size)
// used 由录制线程写、可被别的线程读（GetCommandMemoryUsed / Free）：一次原子写，读侧一次原子读拿快照，不出撕裂值
static void used_set(cmd_t *o, uint64_t v) { __atomic_store_n(&o->used, v, __ATOMIC_RELAXED); }
// Host bytes kept at the end of a block after every record: one JUMP to the next block
#define OP_RESERVE 16
// Device bytes of one command: what Used / Free / Size and the out-of-memory callback count. An estimate per NVN method on Maxwell (a method is a 4-byte header plus 4 bytes per
// data word): a draw sets the primitive, first / count and launches (indexed: plus the index buffer address and type), a program binds a code address and registers per stage,
// a render target set writes address / format / size per target, a texture copy or clear a source / destination pair and a launch. Records not listed carry what the device
// command carries (addresses, parameters, inline data such as UpdateUniformBuffer's) and count their own size; the listed ones carry host-only data (texture snapshots, object
// pointers) and count well under it. The estimate only decides when the game is called back for more memory and how much it reserves by Used; nothing is written there and
// replay never depends on it, so erring low is the safe side. BindTexture is 0x18: its handle sits 0x10 into the device command (rec_bind_tex1).
// END: the driver's EndRecording writes only control memory and takes no command memory, on NVN1 as on NVN2 (#1904: Breath of the Wild's own NVN1 driver, run offline, reads 0);
//   an empty recording hangs off a 1-byte anchor to be released by (sh_anchor_empty)
// DEV_CTL: the device writes no command memory for it. 纹理池 / 采样器池绑定在真机驱动里是控制内存的 16 字节令牌（类型 0x13 / 0x14，满了发 OUT_OF_CONTROL_MEMORY），
//   不进命令内存、不发 OUT_OF_COMMAND_MEMORY（游戏自带驱动反汇编，SDK 4.4.1 与更新一代 sdk 同形）。按记录大小算会在游戏还没给命令内存时回调：
//   Outlast 2（0100DE70085E8000，UE4）建 RHI 时只给控制内存就 BeginRecording、绑两个池，之后才建回调要用的命令内存环，回调读到未初始化的环指针崩在 main+0xf3a574
#define DEV_CTL 0xFFFFu
// 状态对象绑定（Blend / Color / ChannelMask / Multisample / Polygon / DepthStencil，OP_BIND_STATE）：记录里是宿主自己的状态快照（4 + 0x18 字节，连头 40），
//   不是设备命令；按记录大小算就成了每条 40 设备字节。UE4 给每个 PSO 预先分好定长的命令内存（按真机驱动的用量），六种状态一叠就被我们估满，
//   OUT_OF_COMMAND_MEMORY 回调里 UE4 断言「PSO CommandMemory OOM Event」中止（11-14 诊断 D-HSQE，0.3.10，选完存档即闪退）。
//   真机一条状态绑定是几条寄存器方法；按上面「宁可估低」取一条方法的下限 8 字节（4 字节方法头 + 1 个数据字）
#define DEV_STATE 8
static const uint16_t g_dev_bytes[OP_COUNT_] = {
    [OP_END] = 8, [OP_CALL] = 16, [OP_SET_RT] = 96, [OP_BIND_PROGRAM] = 64, [OP_BIND_TEX1] = 24, [OP_DRAW] = 32, [OP_DRAW_IDX] = 48, [OP_DRAW_TEX] = 64,
    [OP_DISPATCH] = 48, [OP_COPY_B2T] = 64, [OP_COPY_T2B] = 64, [OP_COPY_T2T] = 64, [OP_CLEAR_TEX] = 48, [OP_SET_TEX_POOL] = DEV_CTL, [OP_SET_SAMP_POOL] = DEV_CTL, [OP_BIND_STATE] = DEV_STATE,
};
// 按入口取驱动真值的记录（带索引绘制、DrawMeshTasks、NVN2 的顶点属性 / 顶点流状态；DEV_EXACT 包住那一条记录）不走上表的估算：入口在录这一条前设好 op 与字节，录完清零（0 = 查表）
static __thread uint32_t t_dev_op, t_dev_bytes;
#define DEV_EXACT(op, bytes, ...) do { t_dev_op = (op); t_dev_bytes = (bytes); __VA_ARGS__; t_dev_bytes = 0; } while (0)
static uint32_t dev_bytes(uint32_t op, uint32_t size) {
    if (t_dev_bytes && op == t_dev_op) return t_dev_bytes;
    if (op == OP_END) return 0;   // 驱动的 EndRecording 只写控制内存，不推进 cmd[0]（NVN1 / NVN2 同；sh_anchor_empty 给空录制留释放锚点）
    uint32_t d = op < OP_COUNT_ ? g_dev_bytes[op] : 0; return d == DEV_CTL ? 0 : d ? d : size; }
#define DEV_LEND (16u << 10)   // device bytes of a host-lent window (the game gave no memory): the callback is asked again only after this many

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_seg_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) { CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
NVN(nvnCommandBufferGetMemoryCallback, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->cb; }
NVN(nvnCommandBufferGetMemoryCallbackData, void *, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->cb_data; }
// Diagnostic DM_NVN_CMDSTAT=1: every 300 frames log one line of command recording usage -- host and device bytes / records per frame, how large a command memory segment the game
// gives each time, how many out-of-memory callbacks and host-lent windows per frame, the device bytes of segment tails skipped, the live recordings and their host blocks, and the
// count and host / device bytes of each command type. Counts only; does not change recording / replay
static struct { int on; uint64_t bytes, dev, recs, grants, grant_bytes, grant_min, grant_max, cbs, lends, op_n[64], op_b[64], op_d[64], frame0, waste; } g_cs = { .on = -1 };
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t dm_nvn_frame_count(void);
static void sh_live(uint64_t *recs, uint64_t *bytes);
static int cs_on(void) { if (g_cs.on < 0) g_cs.on = getenv("DM_NVN_CMDSTAT") != NULL; return g_cs.on; }
static void cs_op(uint32_t op, uint32_t size) { if (!cs_on() || op >= 64) return; pthread_mutex_lock(&g_cs_mu); g_cs.op_n[op]++; g_cs.op_b[op] += size; g_cs.op_d[op] += op == OP_JUMP ? 0 : dev_bytes(op, size); pthread_mutex_unlock(&g_cs_mu); }
static void cs_dump(void) {   // Called once per frame (at present)
    if (!cs_on()) return; uint64_t f = dm_nvn_frame_count(); if (f - g_cs.frame0 < 300) return;
    uint64_t lr = 0, lb = 0; sh_live(&lr, &lb);
    pthread_mutex_lock(&g_cs_mu); double n = (double)(f - g_cs.frame0); char top[900]; int k = 0; uint8_t used[64] = {0};
    for (int r = 0; r < 8; r++) { int bi = -1; for (int i = 0; i < 64; i++) if (!used[i] && g_cs.op_b[i] && (bi < 0 || g_cs.op_b[i] > g_cs.op_b[bi])) bi = i; if (bi < 0) break; used[bi] = 1;
        k += snprintf(top + k, sizeof top - k, " op %d: %.0f × %.0f bytes (device %.0f) = %.1f KB;", bi, g_cs.op_n[bi] / n, (double)g_cs.op_b[bi] / (double)g_cs.op_n[bi], (double)g_cs.op_d[bi] / (double)g_cs.op_n[bi], g_cs.op_b[bi] / n / 1024.0); }
    LOG("[nvn] command memory up to frame %llu: recorded per frame %.1f KB host / %.1f KB device / %.0f records; segments from the game per frame %.2f, mean %.1f KB each (min %.1f / max %.1f); out-of-memory callbacks per frame %.2f; host-lent windows per frame %.3f; segment tails skipped per frame %.1f KB; live recordings %llu in %.1f MB of host blocks | per frame%s",
        (unsigned long long)f, g_cs.bytes / n / 1024.0, g_cs.dev / n / 1024.0, g_cs.recs / n, g_cs.grants / n, g_cs.grants ? g_cs.grant_bytes / (double)g_cs.grants / 1024.0 : 0, g_cs.grant_min / 1024.0, g_cs.grant_max / 1024.0, g_cs.cbs / n, g_cs.lends / n, g_cs.waste / n / 1024.0, (unsigned long long)lr, lb / 1048576.0, top);
    int on = g_cs.on; memset(&g_cs, 0, sizeof g_cs); g_cs.on = on; g_cs.frame0 = f; pthread_mutex_unlock(&g_cs_mu);
}
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { op_hdr h = {op, size}; memcpy(p, &h, sizeof h); if (op) cs_op(op, size); }

// Host blocks: power-of-two classes of 2^SH_CLS0 .. 2^(SH_CLS0 + SH_NCLS - 1) bytes carved from SH_CHUNK mmap'd chunks. Chunks are page aligned and hold nothing else, so the
// frame recording saves them as they are and the replay maps them back at their addresses (nvn_frame_rec.inc, s2a_hle_cmdshadow_list). A recording's first block is as large
// as the command buffer's previous recording took (cmd_t hint), so a list recorded again fits one block; a longer one goes on in blocks twice as large, up to the largest class.
#define SH_CLS0 8
#define SH_NCLS 13
#define SH_CHUNK (2u << 20)
// Release index: a recording's device intervals hang in a hash of 4 KB guest pages (one node per page an interval touches). Breath of the Wild keeps thousands of prerecorded
// lists alive and Splatoon 3 records thousands in a frame while loading, so a write looks up only the pages it covers; a range wider than the table walks the table once
#define SH_PAGE_SHIFT 12
#define SH_NBKT (1u << 16)
// Released recordings wait SH_QUAR_FRAMES presented frames (or until more than SH_QUAR_CAP bytes wait) before their blocks are reused: the replay keeps record pointers within a
// submission and the game only re-records memory the GPU is done with, so this is a margin, not a requirement
#define SH_QUAR_FRAMES 2
#define SH_QUAR_CAP (256ull << 20)
typedef struct sblk { struct sblk *next; uint32_t cls, pad; } sblk;   // block header; records follow
typedef struct { uintptr_t lo, hi; } siv;
typedef struct snode { struct snode *next, **pprev; struct srec *r; uintptr_t lo, hi, page; } snode;
typedef struct srec {
    struct srec *prev, *next;   // the list of all recordings (g_sh_all), or the quarantine queue (next only)
    sblk *blk, *last;           // host blocks in order
    siv *iv, ivs[2];            // device intervals in the game's memory (iv = ivs until more are needed)
    uint32_t niv, civ, nnodes, open;
    snode *nodes;               // index nodes, set at EndRecording
    uintptr_t glo;              // start of the piece in the command buffer's current window
    uint64_t bytes, rbytes, frame;   // host bytes of the blocks / of the records; frame of release
    uint8_t *begin;             // its handle (first record), registered in g_shh once ended
    uintptr_t dev0;             // 首记录在游戏命令内存里的设备地址（真机上的句柄值）；借宿主窗口录的为 0（没有可靠地址）
} srec;
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
static sblk *g_sh_free[SH_NCLS];
typedef struct { uint8_t *mem; uint32_t cls, carved; } schunk;
static schunk *g_sh_chunk; static int g_sh_nchunk, g_sh_cchunk, g_sh_carving[SH_NCLS];   // g_sh_carving: index + 1 of the chunk a class carves from
static snode *g_sh_bkt[SH_NBKT];
static srec g_sh_all = { .prev = &g_sh_all, .next = &g_sh_all };
// 句柄登记（#768338 安全阀，g_sh_mu 下读写）：结束了的录制的句柄 → 1 活着 / 2 已被覆盖、还在隔离区（宿主块完好）；隔离期满块被复用时删掉。
//   内容共用的常驻副本（sh_intern）一直是 1。回放的 CALL 按它核目标（nvn_cmd_call_target）
static nvn_hmap g_shh = {.klen = 8};
// 即时存档：录制中的这条属于哪个游戏命令缓冲（cmd_t，游戏地址），恢复时改写它的 rec。旁挂表、只在存档模式登记：关着时录制结构不多一个字段
static nvn_hmap g_snap_rec_owner = {.klen = 8}; static pthread_mutex_t g_snap_rec_mu = PTHREAD_MUTEX_INITIALIZER;
// 弱链接[nvntest]：s2a_snap_handout_id（runtime/src/snapshot.c 提供；图形单测不链宿主，链接行按这行放行未定义）
// 弱链接[nvntest]：s2a_snap_mode（runtime/src/snapsw.c 提供；同上）
static int hle_snap_saving(void) { static int v = -1; int s2a_snap_saving(void) __attribute__((weak)); if (v < 0) v = s2a_snap_saving ? s2a_snap_saving() : 0; return v; }
static void shh_set(const uint8_t *h, uintptr_t st) { if (!h) return; uintptr_t k = (uintptr_t)h; if (st) nvn_hm_put(&g_shh, &k, (void *)st); else nvn_hm_del(&g_shh, &k); }
// #768338 根修：CALL 按设备地址解析（真机上句柄就是命令在游戏内存里的地址）。g_shd：句柄 → 录制时首记录的设备地址，块被复用前一直留着
//   （同一块再被别的录制当开头用时覆盖；条目数以宿主块的开头位置数为上限）；g_shl：设备地址 → 此刻占着它的活录制句柄（录制死了就删）。
//   长期列表 A 调用每帧在同一块内存里重录的 B：真机上 B 的句柄不变，A 跑的是此刻那块内存里的新内容；宿主每次重录换新句柄，靠这两张表从 A 记的旧句柄找到新录制
static nvn_hmap g_shd = {.klen = 8}, g_shl = {.klen = 8};
static void shd_end(const srec *r) {   // g_sh_mu held
    if (!r->dev0 || !r->begin) return;
    uintptr_t k = (uintptr_t)r->begin, d = r->dev0;
    nvn_hm_put(&g_shd, &k, (void *)d); nvn_hm_put(&g_shl, &d, (void *)k);
}
static void shd_kill(const srec *r) {   // g_sh_mu held：只删仍指向这条录制的项（同地址已有更新的录制就不动）
    if (!r->dev0 || !r->begin) return;
    uintptr_t d = r->dev0; if ((uintptr_t)nvn_hm_get(&g_shl, &d) == (uintptr_t)r->begin) nvn_hm_del(&g_shl, &d);
}
static srec *g_sh_qhead, *g_sh_qtail; static uint64_t g_sh_qbytes;
static uint64_t g_sh_nlive, g_sh_live_bytes, g_sh_unanchored;
typedef struct { uint64_t h; uint32_t len; struct sblk *b; } sintern;
static sintern *g_sh_it; static size_t g_sh_itcap, g_sh_itn; static uint64_t g_sh_shared_n;   // interned recordings (sh_intern)
static uint32_t sh_hash(uintptr_t page) { return (uint32_t)((page * 0x9E3779B97F4A7C15ull) >> 48) & (SH_NBKT - 1); }
static void sh_live(uint64_t *recs, uint64_t *bytes) { pthread_mutex_lock(&g_sh_mu); *recs = g_sh_nlive; *bytes = g_sh_live_bytes; pthread_mutex_unlock(&g_sh_mu); }
// Live recordings (begun and not released), for diagnostics and the unit tests
uint64_t s2a_hle_cmdshadow_live(void) { uint64_t r, b; sh_live(&r, &b); return r; }
uint64_t s2a_hle_cmdshadow_live_bytes(void) { uint64_t r, b; sh_live(&r, &b); return b; }
uint64_t s2a_hle_cmdshadow_shared(void) { pthread_mutex_lock(&g_sh_mu); uint64_t n = g_sh_shared_n; pthread_mutex_unlock(&g_sh_mu); return n; }   // 单测：按内容共用了几次
// 帧录（nvn_frame_rec.inc）：命令记录都在宿主块里（游戏窗口外），录像按原地址另存，回放映射回原地址
int s2a_hle_cmdshadow_list(void (*cb)(void *ctx, const void *mem, size_t size), void *ctx) {
    pthread_mutex_lock(&g_sh_mu); int n = 0; size_t pg = (size_t)getpagesize();
    for (int i = 0; i < g_sh_nchunk; i++) if (g_sh_chunk[i].carved) { cb(ctx, g_sh_chunk[i].mem, (((size_t)g_sh_chunk[i].carved << (SH_CLS0 + g_sh_chunk[i].cls)) + pg - 1) & ~(pg - 1)); n++; }
    pthread_mutex_unlock(&g_sh_mu); return n;
}
// 弱链接[nvntest]：s2a_cmd_arena_map（命令记录区，runtime/src/vawin.c；NVN 单测不链原生层，链接行按这行放行未定义，runtime/build.mk 自动收）
// 弱链接[nvntest]：s2a_cmd_arena_unmap（同上）
// 弱链接[nvntest]：s2a_cmd_arena_has（同上；帧录 nvn_frame_rec.inc 也用）
// Chunks of a process that records frames (S2A_FRAME_REC) come from the host's command-record area (runtime/src/vawin.c s2a_cmd_arena_map): a frame replay is another process and
//   maps them back at their addresses, and the area is the one range both processes hold free at the same place. A chunk from mmap(NULL) sits wherever the kernel put it, and the
//   replay process's own libraries / heaps / Metal often hold that address already (7df29fdfc: every replay red). Other processes have no area: mmap(NULL) as before
void *s2a_cmd_arena_map(uint64_t len) __attribute__((weak));
void s2a_cmd_arena_unmap(void *p, uint64_t len) __attribute__((weak));
static void *sh_chunk_map(void) {
    void *t = s2a_cmd_arena_map ? s2a_cmd_arena_map(SH_CHUNK) : NULL; if (t) return t;
    void *m = mmap(NULL, SH_CHUNK, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, S2A_VMTAG_FD(S2A_VMTAG_HOST), 0);
    return m == MAP_FAILED ? NULL : m;
}
static void sh_chunk_unmap(void *m) { if (s2a_cmd_arena_unmap) s2a_cmd_arena_unmap(m, SH_CHUNK); else munmap(m, SH_CHUNK); }
static sblk *sh_blk_get(size_t want, size_t need) {   // g_sh_mu held; a block of at least need bytes (header included), as close to want as the classes allow
    if (want < need) want = need;
    int c = 0; while (c < SH_NCLS - 1 && ((size_t)1 << (SH_CLS0 + c)) < want) c++;
    if (((size_t)1 << (SH_CLS0 + c)) < need) die("NVN command recording: a %zu-byte record is larger than the largest host block", need);
    sblk *b = g_sh_free[c];
    if (b) g_sh_free[c] = b->next;
    else {
        int k = g_sh_carving[c] - 1; uint32_t per = SH_CHUNK >> (SH_CLS0 + c);
        if (k < 0 || g_sh_chunk[k].carved == per) {
            if (g_sh_nchunk == g_sh_cchunk) { int cc = g_sh_cchunk ? g_sh_cchunk * 2 : 64; schunk *t = realloc(g_sh_chunk, sizeof *t * (size_t)cc); if (!t) die("NVN command recording: host block table growth failed"); g_sh_chunk = t; g_sh_cchunk = cc; }
            void *m = sh_chunk_map();
            if (!m) die("NVN command recording: mmap of a %u-byte host chunk failed (errno %d, %d chunks)", SH_CHUNK, errno, g_sh_nchunk);
            g_sh_chunk[g_sh_nchunk] = (schunk){m, (uint32_t)c, 0}; k = g_sh_nchunk++; g_sh_carving[c] = k + 1;
        }
        b = (sblk *)(g_sh_chunk[k].mem + ((size_t)g_sh_chunk[k].carved++ << (SH_CLS0 + c)));
    }
    b->next = NULL; b->cls = (uint32_t)c; return b;
}
static void sh_index(srec *r) {   // g_sh_mu held
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->niv; i++) n += (uint32_t)(((r->iv[i].hi - 1) >> SH_PAGE_SHIFT) - (r->iv[i].lo >> SH_PAGE_SHIFT) + 1);
    r->nodes = malloc(sizeof *r->nodes * n); if (!r->nodes) die("NVN command recording: index nodes for %u pages", n);
    r->nnodes = n; n = 0;
    for (uint32_t i = 0; i < r->niv; i++)
        for (uintptr_t pg = r->iv[i].lo >> SH_PAGE_SHIFT; pg <= (r->iv[i].hi - 1) >> SH_PAGE_SHIFT; pg++) {
            snode *d = &r->nodes[n++], **h = &g_sh_bkt[sh_hash(pg)];
            *d = (snode){*h, h, r, r->iv[i].lo, r->iv[i].hi, pg}; if (*h) (*h)->pprev = &d->next;
            __atomic_store_n(h, d, __ATOMIC_RELEASE);
        }
}
static void sh_free_rec(srec *r) {   // g_sh_mu held: blocks back to their classes
    shh_set(r->begin, 0);
    for (sblk *b = r->blk, *nx; b; b = nx) { nx = b->next; b->next = g_sh_free[b->cls]; g_sh_free[b->cls] = b; }
    if (r->iv != r->ivs) free(r->iv);
    free(r);
}
static void sh_pump(void) {   // g_sh_mu held: reuse the blocks of recordings released long enough ago
    uint64_t f = dm_nvn_frame_count();
    while (g_sh_qhead && (g_sh_qhead->frame + SH_QUAR_FRAMES <= f || g_sh_qbytes > SH_QUAR_CAP)) {
        srec *r = g_sh_qhead; g_sh_qhead = r->next; if (!g_sh_qhead) g_sh_qtail = NULL;
        g_sh_qbytes -= r->bytes; sh_free_rec(r);
    }
}
void s2a_hle_cmdshadow_quarantine_flush(void) {   // 单测：隔离区里的录制立即放回块池（同隔离期满）
    pthread_mutex_lock(&g_sh_mu);
    while (g_sh_qhead) { srec *r = g_sh_qhead; g_sh_qhead = r->next; g_sh_qbytes -= r->bytes; sh_free_rec(r); }
    g_sh_qtail = NULL; pthread_mutex_unlock(&g_sh_mu);
}
static void sh_kill(srec *r) {   // g_sh_mu held: the recording is dead; unindex it and queue its blocks for reuse
    for (uint32_t i = 0; i < r->nnodes; i++) { snode *d = &r->nodes[i]; __atomic_store_n(d->pprev, d->next, __ATOMIC_RELEASE); if (d->next) d->next->pprev = d->pprev; }
    free(r->nodes); r->nodes = NULL; r->nnodes = 0;
    r->prev->next = r->next; r->next->prev = r->prev;
    g_sh_nlive--; g_sh_live_bytes -= r->bytes;
    r->frame = dm_nvn_frame_count(); r->next = NULL; g_sh_qbytes += r->bytes; shh_set(r->begin, 2); shd_kill(r);
    if (g_sh_qtail) g_sh_qtail->next = r; else g_sh_qhead = r;
    g_sh_qtail = r;
}
// Release every ended recording with a device interval overlapping [lo, hi) (g_sh_mu held)
static void sh_kill_range(uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    uintptr_t p0 = lo >> SH_PAGE_SHIFT, p1 = (hi - 1) >> SH_PAGE_SHIFT; int all = p1 - p0 >= SH_NBKT;
    for (uintptr_t i = all ? 0 : p0; all ? i < SH_NBKT : i <= p1; i++) {
        snode **h = &g_sh_bkt[all ? (uint32_t)i : sh_hash(i)];
        for (snode *d = *h; d; ) {
            if ((all || d->page == i) && d->lo < hi && d->hi > lo) { sh_kill(d->r); d = *h; continue; }   // the kill unlinks all its nodes: walk the bucket again
            d = d->next;
        }
    }
}
// The device cursor is about to cover [lo, hi) of the game's memory: recordings there are being overwritten. Lock-free look at the buckets first: most writes land on
// pages no live recording touches (a game re-records memory only after the recording there is dead to it, so no other thread indexes these pages meanwhile)
static void sh_overwrite(uintptr_t lo, uintptr_t hi) {
    uintptr_t p0 = lo >> SH_PAGE_SHIFT, p1 = (hi - 1) >> SH_PAGE_SHIFT; int any = p1 - p0 >= SH_NBKT;
    for (uintptr_t p = p0; !any && p <= p1; p++) any = __atomic_load_n(&g_sh_bkt[sh_hash(p)], __ATOMIC_ACQUIRE) != NULL;
    if (!any) return;
    pthread_mutex_lock(&g_sh_mu); sh_kill_range(lo, hi); pthread_mutex_unlock(&g_sh_mu);
}
// 解析 CALL 目标（回放与前瞻扫描共用）：0 = 照 h 执行（活录制，或不在命令记录区：单测手拼的列表等不判）；2 = h 已死，同一设备地址上有新录制 *to（跳到它，同真机）；
//   1 = 已被覆盖、还在隔离区、同地址没有新录制（块完好，照常执行，探针记一行）；-1 = 落在命令记录区却不是任何录制的句柄、也查不到同地址的活录制（跳过，记红）
int nvn_cmd_call_resolve(uint64_t h, uint64_t *to) {
    uintptr_t k = (uintptr_t)h; int r = 0; *to = h;
    pthread_mutex_lock(&g_sh_mu);
    uintptr_t st = (uintptr_t)nvn_hm_get(&g_shh, &k);
    if (st != 1) {
        uintptr_t d = (uintptr_t)nvn_hm_get(&g_shd, &k), live = d ? (uintptr_t)nvn_hm_get(&g_shl, &d) : 0;
        if (live && live != k) { *to = live; r = 2; }
        else if (st == 2) r = 1;
        else for (int i = 0; i < g_sh_nchunk; i++) if (k >= (uintptr_t)g_sh_chunk[i].mem && k < (uintptr_t)g_sh_chunk[i].mem + SH_CHUNK) { r = -1; break; }
    }
    pthread_mutex_unlock(&g_sh_mu);
    return r;
}
int nvn_cmd_call_target(uint64_t h) { uint64_t to; int r = nvn_cmd_call_resolve(h, &to); return r == 2 ? 0 : r; }   // 旧口径（只判不跳转）
// A memory pool is finalized: the recordings in its memory can no longer run on the device (also where a game drops a whole command pool instead of re-recording it)
static void cmdshadow_pool_gone(const uint8_t *mem, size_t n) {
    if (!mem || !n) return;
    pthread_mutex_lock(&g_sh_mu); sh_kill_range((uintptr_t)mem, (uintptr_t)mem + n); pthread_mutex_unlock(&g_sh_mu);
}
static void sh_shutdown(void) {   // session end: every recording and chunk goes
    pthread_mutex_lock(&g_sh_mu);
    for (srec *r = g_sh_all.next, *nx; r != &g_sh_all; r = nx) { nx = r->next; free(r->nodes); if (r->iv != r->ivs) free(r->iv); free(r); }
    for (srec *r = g_sh_qhead, *nx; r; r = nx) { nx = r->next; if (r->iv != r->ivs) free(r->iv); free(r); }
    for (int i = 0; i < g_sh_nchunk; i++) sh_chunk_unmap(g_sh_chunk[i].mem);
    free(g_sh_chunk); g_sh_chunk = NULL; g_sh_nchunk = g_sh_cchunk = 0;
    free(g_shh.keys); free(g_shh.vals); nvn_hm_init(&g_shh, 8);
    free(g_shd.keys); free(g_shd.vals); nvn_hm_init(&g_shd, 8); free(g_shl.keys); free(g_shl.vals); nvn_hm_init(&g_shl, 8);
    memset(g_sh_carving, 0, sizeof g_sh_carving); memset(g_sh_free, 0, sizeof g_sh_free); memset(g_sh_bkt, 0, sizeof g_sh_bkt);
    g_sh_all.next = g_sh_all.prev = &g_sh_all; g_sh_qhead = g_sh_qtail = NULL; g_sh_qbytes = g_sh_nlive = g_sh_live_bytes = g_sh_unanchored = 0; free(g_sh_it); g_sh_it = NULL; g_sh_itcap = g_sh_itn = 0; g_sh_shared_n = 0;
    pthread_mutex_unlock(&g_sh_mu);
}
// The open recording leaves the command buffer's current window: note the device bytes it took there. In a host-lent window it took none of the game's memory; it hangs off
// the byte where the game's memory ran out (vanc) and dies with whatever overwrites that byte
static void sh_piece(cmd_t *o) {
    srec *r = o->rec; if (!r || o->gcur <= r->glo) return;
    siv v = o->gvirt ? (siv){o->vanc, o->vanc + 1} : (siv){r->glo, o->gcur};
    if (!v.lo) return;
    if (r->niv && r->iv[r->niv - 1].hi >= v.lo && r->iv[r->niv - 1].lo <= v.lo) { if (v.hi > r->iv[r->niv - 1].hi) r->iv[r->niv - 1].hi = v.hi; return; }   // contiguous / the same anchor
    if (r->niv == r->civ) {
        siv *t = malloc(sizeof *t * r->civ * 2); if (!t) die("NVN command recording: interval list growth failed");
        memcpy(t, r->iv, sizeof *t * r->niv); if (r->iv != r->ivs) free(r->iv); r->iv = t; r->civ *= 2;
    }
    r->iv[r->niv++] = v;
}
// The game gave no memory (no callback, a callback that added nothing, an empty pool) or a copy outran the target: go on in a host-lent window of device bytes. The device
// cursor only counts there; the game is asked again when the window is used up.
// 审计 #1534 P1-99：Used 只数游戏最近一次给的那段内存（Size 就是它的长度），借窗口不占游戏内存：借的那一刻这段算用尽（没写到的尾巴跳过，同 Free = 0），
//   之后借窗口里的记录不再加 Used。原先 Used 接着累加，可以大于 Size，Free 被夹成 0，三个读数对不上（Used + Free ≠ Size）
static void cmd_lend(cmd_t *o, uint32_t dv) {
    sh_piece(o);
    used_set(o, __atomic_load_n(CB_SEG(o), __ATOMIC_RELAXED));
    if (!o->gvirt && o->gend) {
        o->vanc = o->gcur > o->gstart ? o->gcur - 1 : o->gstart;
        if (cs_on() && o->gend > o->gcur) { pthread_mutex_lock(&g_cs_mu); g_cs.waste += o->gend - o->gcur; pthread_mutex_unlock(&g_cs_mu); }
    }
    o->gvirt = 1; o->gstart = o->gcur = 0; o->gend = dv > DEV_LEND ? dv : DEV_LEND;
    if (o->rec) o->rec->glo = 0;
    static _Atomic unsigned total; unsigned t = ++total;
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.lends++; pthread_mutex_unlock(&g_cs_mu); }
    if (t == 1 || t % 1024 == 0) LOG("【黄】[nvn] command memory: the game's memory does not cover the next %u device bytes (cb=%p), recording goes on in a host-lent window (%u times so far, frame %llu)", dv, (void *)o->cb, t, (unsigned long long)dm_nvn_frame_count());
}
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    const pool_t *pl = (const pool_t *)p; cmd_t *o = CB(c);
    { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ng; if (cblog && (++ng <= 60 || ng % 500 == 0)) LOG("[nvn] AddCommandMemory #%llu cmd=%p pool %p +%#lx length %#zx = [%p, %p) (recording %d)", (unsigned long long)ng, (void *)c, (const void *)pl, (long)off, n,
        pl && pl->mem ? (void *)(pl->mem + off) : NULL, pl && pl->mem ? (void *)(pl->mem + off + n) : NULL, o->recording); }   // 段的宿主地址：和崩溃地址 / 映射边界对
    // Nothing given (the sorceress's advanced stats page: the game's callback cannot allocate and passes an empty pool): a host-lent window. Any length counts, as on the device:
    //   Borderlands 3 (Unreal Engine 4) gives exactly the bytes the callback asked for (40, a real pool and offset), and anything short makes the next record ask again
    __atomic_store_n(CB_SEG(c), (uint64_t)n, __ATOMIC_RELAXED);   // Size（GetCommandMemorySize）= 游戏这次给的 n，空池 / 0 字节借宿主窗口时也照记：宿主窗口不是游戏加的命令内存
    if (!pl || !n || !pl->mem) { if (pl && n && !pl->mem) LOG("【红】 AddCommandMemory: pool has no CPU storage"); cmd_lend(o, 0); return; }
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.grants++; g_cs.grant_bytes += n; if (!g_cs.grant_min || n < g_cs.grant_min) g_cs.grant_min = n; if (n > g_cs.grant_max) g_cs.grant_max = n; if (!o->gvirt && o->gend > o->gcur) g_cs.waste += o->gend - o->gcur; pthread_mutex_unlock(&g_cs_mu); }
    sh_piece(o);
    o->gvirt = 0; o->gstart = o->gcur = (uintptr_t)(pl->mem + off); o->gend = o->gstart + n; o->vanc = 0;
    used_set(o, 0);   // GetCommandMemoryUsed counts from the start of the memory the game last added, across recordings (see there)
    if (o->rec) o->rec->glo = o->gcur;
    pthread_mutex_lock(&g_sh_mu); sh_pump(); pthread_mutex_unlock(&g_sh_mu);
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t n) { (void)m; CB(c)->ctl_size = n; }   // 主机不用控制内存，只记大小给 GetControlMemorySize / Free
// The recording goes on in a new host block (its first, or the last one is full): linked from the old one by a JUMP in the room kept for it
static void sh_grow(cmd_t *o, uint32_t need) {
    srec *r = o->rec; size_t want = r->last ? (size_t)2 << (SH_CLS0 + r->last->cls) : (o->hint ? o->hint : 1024);
    pthread_mutex_lock(&g_sh_mu); sh_pump();
    sblk *b = sh_blk_get(want, need + sizeof(sblk)); size_t bs = (size_t)1 << (SH_CLS0 + b->cls);
    if (r->last) r->last->next = b; else r->blk = b;
    r->last = b; r->bytes += bs; g_sh_live_bytes += bs;
    pthread_mutex_unlock(&g_sh_mu);
    uint8_t *start = (uint8_t *)(b + 1);
    if (o->cur) { put_hdr(o->cur, OP_JUMP, 16); memcpy(o->cur + 8, &start, 8); r->rbytes += 16; }
    o->cur = start; o->end = (uint8_t *)b + bs;
}
// Get host space for one record and write its header; its device bytes advance the device cursor. Not enough device bytes left: call back into the game per the NVN convention
// (event OUT_OF_COMMAND_MEMORY, the device bytes needed), twice at most, then a host-lent window. cb_ok = 0 (CopyCommands) lends at once instead of calling back once the game
// has given memory: Mario Tennis Aces's callback for a copy target calls through a null pointer (main+0x771678); with device counting a copy needs no more than the source's Used,
// which is what the game sizes the target by, so only an estimate off from the game's own arithmetic could get there
// 录制外命令与录制重入的读数（#372 诊断 D-EHK5：竞技场发黑那局只记了首例「command 15 … dropped」，丢了多少、丢在哪个命令缓冲上都不知道）：按命令号计数，
//   前 8 例带命令缓冲地址与录制状态，与 BeginRecording 重入的地址对得上就是同一条录制被打断；每 300 帧有新增时打一行汇总
static uint64_t g_frame;   // 定义在下面（帧计数），这里先声明
static _Atomic uint64_t g_rec_out_op[64], g_rec_out_n, g_rec_reenter_n, g_rec_end_idle_n;   // end_idle：EndRecording 落在没在录制的命令缓冲上（#202d02：每帧 Begin / End 一样多却每帧一次重入，看是不是同一缓冲嵌套）
static void rec_out_summary(uint64_t frame) {
    static uint64_t said_out, said_re, said_ei; uint64_t n = atomic_load(&g_rec_out_n), re = atomic_load(&g_rec_reenter_n), ei = atomic_load(&g_rec_end_idle_n);
    if (n == said_out && re == said_re && ei == said_ei) return;
    said_out = n; said_re = re; said_ei = ei; char by[512] = ""; size_t at = 0;
    for (int i = 0; i < 64 && at < sizeof by - 24; i++) { uint64_t c = atomic_load(&g_rec_out_op[i]); if (c) at += (size_t)snprintf(by + at, sizeof by - at, "%s%d×%llu", at ? "、" : "", i, (unsigned long long)c); }
    LOG("[nvn] 录制外命令到第 %llu 帧：丢了 %llu 条（按命令号：%s；号见 nvn_obj.h OP_*，15 = BIND_STATE）；BeginRecording 重入 %llu 次、EndRecording 落在没在录制的缓冲上 %llu 次", (unsigned long long)frame, (unsigned long long)n, at ? by : "无", (unsigned long long)re, (unsigned long long)ei);
}
static uint8_t *cmd_alloc_cb(cmd_t *o, uint32_t op, uint32_t size, int cb_ok) {
    size = (size + 7) & ~7u;
    srec *r = o->rec;
    if (!r) {   // Outside BeginRecording .. EndRecording no handle can reach the records (NVN does not allow it): taken and dropped
        static __thread uint8_t *scratch; static __thread uint32_t nscratch;
        if (size > nscratch) { uint8_t *t = realloc(scratch, size); if (!t) die("NVN command recording: scratch of %u bytes", size); scratch = t; nscratch = size; }
        uint64_t k = atomic_fetch_add(&g_rec_out_n, 1) + 1; atomic_fetch_add(&g_rec_out_op[op < 64 ? op : 63], 1);
        if (k <= 8) LOG("【红】 [nvn] command %u recorded outside BeginRecording / EndRecording on command buffer %p (recording state %d, frame %llu): dropped (case %llu of the first 8; totals in the 「录制外命令」 line)",
                        op, (void *)o, (int)o->recording, (unsigned long long)g_frame, (unsigned long long)k);
        put_hdr(scratch, op, size); return scratch;
    }
    uint32_t dv = dev_bytes(op, size);
    for (int tries = 0; o->gcur + dv > o->gend; tries++) {
        if (!o->cb || tries == 2 || (!cb_ok && (o->gend || o->gvirt))) { cmd_lend(o, dv); break; }   // a copy into a buffer never given memory still asks: its window anchors the recording
        if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.cbs++; pthread_mutex_unlock(&g_cs_mu); }
        { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ncb;
          if (cblog && (++ncb <= 40 || ncb % 1000 == 0)) LOG("[nvn] command memory callback #%llu cmd=%p need %u device bytes (free %lld)", (unsigned long long)ncb, (void *)o, dv, (long long)(o->gend - o->gcur)); }
        o->cb((NVNcommandBuffer *)o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, dv, o->cb_data);
    }
    if (dv && !o->gvirt) sh_overwrite(o->gcur, o->gcur + dv);   // 不占命令内存的记录（DEV_CTL）不盖任何设备区间
    o->gcur += dv; if (!o->gvirt) used_set(o, o->used + dv);   // a host-lent window takes none of the game's memory (cmd_lend)
    if (!o->cur || o->cur + size + OP_RESERVE > o->end) sh_grow(o, size + OP_RESERVE);
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.bytes += size; g_cs.dev += dv; g_cs.recs++; pthread_mutex_unlock(&g_cs_mu); }
    uint8_t *p = o->cur; o->cur += size; r->rbytes += size; put_hdr(p, op, size);
    if (o->recording == 2) { o->begin = p; o->recording = 1; r->dev0 = o->gvirt ? 0 : o->gcur - dv; }   // The recording's first record is its handle（dev0：它在游戏命令内存里的位置，借宿主窗口时没有）
    return p;
}
static uint8_t *cmd_alloc(cmd_t *o, uint32_t op, uint32_t size) { return cmd_alloc_cb(o, op, size, 1); }
// The recording's end (or an abandoned one): its last piece noted, indexed by its device intervals; a recording with none (the game never gave memory) can never be released
// 没占设备字节的录制（NVN2 的 END 不占字节：空录制、只有控制令牌的录制）也要能被释放：挂在一个字节上，随覆盖它的重录或内存池销毁一起死。
//   取游标前一字节（前一条录制的尾巴，紧接着的下一条录制不会写到它）；段首没有前一字节，取段尾。从没给过内存的录制没有可挂的字节，照旧留到局末
static void sh_anchor_empty(cmd_t *o, srec *r) {
    if (r->niv) return;
    uintptr_t a = o->gvirt ? o->vanc : o->gcur > o->gstart ? o->gcur - 1 : o->gend ? o->gend - 1 : 0;
    if (a) { r->iv[0] = (siv){a, a + 1}; r->niv = 1; }
}
// Recordings that take none of the game's command memory and hold only control-memory records (texture / sampler pool binds) and their END are interned by content
//   (#783). Nothing the device cursor writes ever covers them, so they hung off a 1-byte anchor; one first in a fresh segment anchored at the segment's last byte, a tail
//   the game never writes, and none was ever released: Ocarina of Time on Switch 2 ends ~10 such lists a frame (9 of them empty), thousands piled on one byte, ~10 MB of
//   host blocks a minute. These records carry only the pool pointer and are fixed once ended, so every recording with the same bytes replays the same from one permanent
//   copy, and the copies are as many as the distinct contents. Empty recordings (only END) share one copy in every mode. Other records are not interned: some keep host
//   addresses inside their own recording, which a copy would leave pointing at the old block
#define SH_INTERN_MAX 256
static uint64_t sh_fnv(const uint8_t *p, size_t n) { uint64_t h = 0xcbf29ce484222325ull; while (n--) { h ^= *p++; h *= 0x100000001b3ull; } return h; }
static int sh_intern(cmd_t *o, srec *r) {   // g_sh_mu held: 1 = r dropped, handle now the shared copy
    sblk *b = r->blk; if (!b || b != r->last || o->begin != (uint8_t *)(b + 1) || o->cur < o->begin) return 0;
    size_t len = (size_t)(o->cur - o->begin); if (!len || len > SH_INTERN_MAX) return 0;
    if (r->niv && len != 8) return 0;   // takes device bytes and is not empty: released by overwrite as before
    for (const uint8_t *p = o->begin; p < o->cur; ) {   // only pool binds and END
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < sizeof h || p + h.size > o->cur || (h.op != OP_END && h.op != OP_SET_TEX_POOL && h.op != OP_SET_SAMP_POOL) || (r->niv && h.op != OP_END)) return 0;
        p += h.size;
    }
    uint64_t hv = sh_fnv(o->begin, len); size_t i; sblk *copy = NULL;
    if (g_sh_itcap) for (i = hv & (g_sh_itcap - 1); g_sh_it[i].b; i = (i + 1) & (g_sh_itcap - 1))
        if (g_sh_it[i].h == hv && g_sh_it[i].len == len && !memcmp(g_sh_it[i].b + 1, o->begin, len)) { copy = g_sh_it[i].b; break; }
    if (!copy) {
        if ((g_sh_itn + 1) * 2 > g_sh_itcap) {   // grow (load <= 1/2) and rehash
            size_t nc = g_sh_itcap ? g_sh_itcap * 2 : 64; sintern *t = calloc(nc, sizeof *t); if (!t) return 0;
            for (size_t k = 0; k < g_sh_itcap; k++) if (g_sh_it[k].b) { size_t j = g_sh_it[k].h & (nc - 1); while (t[j].b) j = (j + 1) & (nc - 1); t[j] = g_sh_it[k]; }
            free(g_sh_it); g_sh_it = t; g_sh_itcap = nc;
        }
        copy = sh_blk_get(sizeof(sblk) + len, sizeof(sblk) + len); copy->next = NULL; memcpy(copy + 1, o->begin, len);
        for (i = hv & (g_sh_itcap - 1); g_sh_it[i].b; i = (i + 1) & (g_sh_itcap - 1)) {}
        g_sh_it[i] = (sintern){hv, (uint32_t)len, copy}; g_sh_itn++; shh_set((const uint8_t *)(copy + 1), 1);
    }
    r->prev->next = r->next; r->next->prev = r->prev; g_sh_nlive--; g_sh_live_bytes -= r->bytes;
    b->next = g_sh_free[b->cls]; g_sh_free[b->cls] = b;   // nothing points into it yet: the handle is handed out below
    if (r->iv != r->ivs) free(r->iv);
    free(r);
    o->begin = (uint8_t *)(copy + 1); o->cur = o->end = NULL; g_sh_shared_n++;
    return 1;
}
static void sh_end(cmd_t *o) {
    srec *r = o->rec; sh_piece(o);
    uint64_t h = r->rbytes + OP_RESERVE + sizeof(sblk); o->hint = (uint32_t)(h < 256 ? 256 : h > (1u << 20) ? (1u << 20) : h);
    pthread_mutex_lock(&g_sh_mu);
    if (sh_intern(o, r)) { pthread_mutex_unlock(&g_sh_mu); o->rec = NULL; return; }
    if (hle_snap_saving()) { pthread_mutex_lock(&g_snap_rec_mu); nvn_hm_del(&g_snap_rec_owner, &r); pthread_mutex_unlock(&g_snap_rec_mu); }
    sh_anchor_empty(o, r); r->open = 0; r->begin = o->begin; shh_set(r->begin, 1); shd_end(r);
    uint64_t un = 0; if (r->niv) sh_index(r); else un = ++g_sh_unanchored;
    pthread_mutex_unlock(&g_sh_mu);
    if (un && (un == 1 || un % 1024 == 0)) LOG("【黄】[nvn] command recording without any of the game's memory: its host blocks stay until the session ends (%llu so far)", (unsigned long long)un);
    o->rec = NULL;
}
// A recording starts with its first command, as in NVN: no BEGIN record, so an empty recording takes only its END. Games size command memory by GetCommandMemoryUsed
// (nvn_obj.h draw_rec): Breath of the Wild sums one scratch recording per mesh and per shape and render pass into each model's prerecorded commands
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) { uint64_t k = atomic_fetch_add(&g_rec_reenter_n, 1) + 1; if (k <= 8) LOG("【红】 BeginRecording re-entered on command buffer %p (frame %llu, case %llu of the first 8)", (void *)o, (unsigned long long)g_frame, (unsigned long long)k); if (o->rec) sh_end(o); }
    srec *r = calloc(1, sizeof *r); if (!r) die("NVN command recording: no memory for a recording");
    r->iv = r->ivs; r->civ = 2; r->open = 1; r->glo = o->gcur;
    pthread_mutex_lock(&g_sh_mu); r->prev = g_sh_all.prev; r->next = &g_sh_all; g_sh_all.prev->next = r; g_sh_all.prev = r; g_sh_nlive++; pthread_mutex_unlock(&g_sh_mu);
    o->rec = r; o->cur = o->end = NULL; o->begin = NULL;
    if (hle_snap_saving()) { pthread_mutex_lock(&g_snap_rec_mu); nvn_hm_put(&g_snap_rec_owner, &r, o); pthread_mutex_unlock(&g_snap_rec_mu); }
    o->recording = 2;   // 2: recording, no command yet (cmd_alloc sets begin)
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (!o->recording) { uint64_t k = atomic_fetch_add(&g_rec_end_idle_n, 1) + 1; if (k <= 8) LOG("【红】 EndRecording on command buffer %p that is not recording (frame %llu, case %llu of the first 8)", (void *)o, (unsigned long long)g_frame, (unsigned long long)k); }
    cmd_alloc(o, OP_END, 8);
    if (o->rec) sh_end(o);
    o->recording = 0;
    return (NVNcommandHandle)o->begin;
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
// One combined texture handle, laid out as the hardware's command: its device command sits at the device cursor, and the handle 0x10 into it, where an engine that patches its
// recorded commands in place writes a new one (Luigi's Mansion 3 takes the command memory base + GetCommandMemoryUsed before each such bind, main+0x7c8964, and stores a
// handle there + 0x10 every frame, main+0xb7d0d4). So the handle is also written there, the one word of the game's memory a record writes, inside the 0x18 device bytes the
// command takes; the record keeps that address (BIND_TEX1_AT) and replay reads the handle from it (nvn_obj.h bind_rec). In a host-lent window there is no such word
static void rec_bind_tex1(cmd_t *o, int32_t st, int32_t slot, uint64_t h, int32_t tb, int32_t sb, int cb_ok) { st = nvn2_stage_host(st);
    uint8_t *p = cmd_alloc_cb(o, OP_BIND_TEX1, 8 + sizeof(p_bind_tex1) + 8, cb_ok);
    uintptr_t g = o->rec && !o->gvirt ? o->gcur - dev_bytes(OP_BIND_TEX1, 0) + 0x10 : 0;
    p_bind_tex1 r = {st, slot | (g ? BIND_TEX1_AT : 0), h, tb, sb}; uint64_t ga = g;
    memcpy(p + 8, &r, sizeof r); memcpy(p + 8 + sizeof r, &ga, 8);
    if (g) memcpy((void *)g, &h, 8);
}
// Copy: the recording's records are copied into this buffer now (NVN: the caller may reuse the source memory right after the call); its JUMPs are followed, its END is not
//   copied. Call keeps only the handle and runs the source as it is at submit time. Both were recorded as a CALL (缺口横向扫描 11-01 族 41): a game reusing the copied
//   memory had the new contents replayed, or a stale record walked. A texture bind takes the handle the source's device word holds now and gets a word of its own
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    for (int i = 0; i < n; i++) {
        const uint8_t *q = (const uint8_t *)(uintptr_t)h[i];
        for (unsigned k = 0; q && k < (1u << 22); k++) {
            op_hdr r; memcpy(&r, q, sizeof r);
            if (r.op == OP_END) break;
            if (r.op == OP_JUMP) { memcpy(&q, q + 8, 8); continue; }
            if (r.size < sizeof r || (r.size & 7)) { LOG("【红】 CopyCommands: record %u of handle %#llx has size %u: copy stopped", k, (unsigned long long)h[i], r.size); break; }
            if (r.op == OP_BIND_TEX1 && r.size >= sizeof r + sizeof(p_bind_tex1)) {
                p_bind_tex1 b; memcpy(&b, q + sizeof r, sizeof b); uint64_t hh = b.h;
                if (b.slot & BIND_TEX1_AT) { uint64_t g; memcpy(&g, q + sizeof r + sizeof b, 8); memcpy(&hh, (const void *)(uintptr_t)g, 8); }
                rec_bind_tex1(CB(c), b.stage, b.slot & ~BIND_TEX1_AT, hh, b.tbase, b.sbase, 0);
            } else { uint8_t *p = cmd_alloc_cb(CB(c), r.op, r.size, 0); memcpy(p + sizeof r, q + sizeof r, r.size - sizeof r); }
            q += r.size;
        }
    }
}
// A GPU wait on a sync inside a command buffer (was a no-op; 缺口族 40): recorded, and at replay a sync fenced by another queue (the GL layer, InitializeFromFencedGLSync) makes the
//   queue wait for that fence on the GPU (nvn_be_queue_wait_event, as nvnQueueWaitSync does). A sync fenced on this queue needs nothing: the host's one queue runs in order
NVN(nvnCommandBufferWaitSync, void, NVNcommandBuffer *c, const NVNsync *s) {
    uint8_t *p = cmd_alloc(CB(c), OP_WAIT_SYNC, 16); memcpy(p + 8, &s, 8);
}
// GPU 等事件（审计 1101 P1-23 原先不录不等；11-05 极品飞车 热力追踪 每局必经、出帧停 45 秒）：录成 OP_SIGNAL_EVENT + NVN_EV_WAIT，回放时队列在 GPU 上等到事件满足条件（nvn_mtl.m ev_wait）
NVN(nvnCommandBufferWaitEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventWaitMode m, uint32_t v) {
    uint8_t *p = cmd_alloc(CB(c), OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = (uint32_t)m | NVN_EV_WAIT;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
    nvn_be_event_gpu_recorded(val);   // 录制时就告诉后端：这个事件由 GPU 侧信号写（别的队列先等它时不在 GPU 上等，nvn_mtl.m ev_wait ⑤）
}
// A fence recorded in a command buffer: the replay fences the sync with the submission that reaches this point (OP_FENCE_SYNC). Unreal Engine's NVN RHI fences its command buffers this way (Octopath Traveler 0); Pokémon Legends: Z-A fences once a frame this way and waits on it
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer *c, NVNsync *s, NVNsyncCondition cond, int flags) {
    (void)cond; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), OP_FENCE_SYNC, 16); memcpy(p + 8, &s, 8);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), OP_REPORT_COUNTER, 24); uint64_t tt = t; a = GA(a); memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), OP_COPY_B2B, 32); uint64_t nn = n; src = GA(src); dst = GA(dst);
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// uniform update: the data is copied into command memory at record time (NVN semantics: the caller may modify the original data after recording) and written into the buffer at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), OP_UPDATE_UB, sz);
    uint64_t dst = GA(buf + off), nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h; for replay see nvn_mtl.m ----------
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// Binding records with arrays are allocated by the actual count: prefix fields + n elements (the replay side reads only the first n). Previously they were recorded at fixed length for the maximum count (bind vertex buffers 272 bytes, bind textures / samplers 280 bytes,
// while there are usually only 1–2 in practice), and the empty tail took 40–50% of the command bytes per frame; the advanced stats page of the level 88 sorceress recorded 12.5 MB per frame, using up the game's own command memory pool (about 105 segments x 128 KB per frame)
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
// Draws carry indirect / multi-draw fields only when used. A single-instance
// indexed draw also omits implicit instance arguments: 0x20 bytes with its header.
// NVN 四边形条带（NVN_DRAW_PRIMITIVE_QUAD_STRIP = 8，Metal 没有）就地改成三角形条带（5）：第 i 个四边形 (2i, 2i+1, 2i+3, 2i+2) 正好是三角形条带第 2i、2i+1 两个三角形
//   （同一组顶点、同一片覆盖面）；四边形条带不画末尾落单的那个顶点，三角形条带会多画一个三角形 → CPU 知道个数的把个数向下取偶（间接绘制的个数在 GPU 上，原样）。
//   11-06 无人深空测试设备1：这类绘制原先在回放里整笔丢掉（「primitive type 8 not implemented」），录到 8 笔
// NVN 多边形（NVN_DRAW_PRIMITIVE_POLYGON = 9，Metal 没有）就地改成三角形扇（6）：凸多边形 v0..vn-1 = 以 v0 为扇心的 (v0, vi, vi+1)，覆盖面相同
//   （上游参考实现同样处理：Ryujinx.Graphics.Vulkan EnumConversion Polygon → TriangleFan）；扇形之后各条路径（展开、间接转换）照旧。
//   #635：0100F43008C44000 Mac 局 16 笔「primitive type 9 not implemented」整笔丢掉（曾被当成曲面细分，日志里 hull / domain 都是「-」）
static void prim_quad_strip(int32_t *prim, int32_t *count) { if (*prim == 8) { *prim = 5; if (*count > 0) *count &= ~1; } else if (*prim == 9) *prim = 6; }
static void rec_draw(NVNcommandBuffer *c, p_draw d) { prim_quad_strip(&d.prim, &d.count); size_t n = d.ind || d.mdi_cnt || d.tfc ? sizeof d : offsetof(p_draw, ind); memcpy(rec(c, OP_DRAW, n), &d, n); }
static void rec_draw_idx(NVNcommandBuffer *c, p_draw_idx d) {
    prim_quad_strip(&d.prim, &d.count);
    if (!d.ind && !d.mdi_cnt && d.instances == 1 && !d.base_instance) {
        p_draw_idx_single s = {d.prim, d.itype, d.count, d.base_vertex, d.ibuf};
        memcpy(rec(c, OP_DRAW_IDX, sizeof s), &s, sizeof s);
    } else {
        size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw_idx, ind);
        memcpy(rec(c, OP_DRAW_IDX, n), &d, n);
    }
}
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
// Records with textures snapshot the texture objects (0x80 bytes each, after the record struct) and replay reads the snapshots: an NVN command no longer depends on the API object once recorded, and the game may finalize, free or reuse it before submitting.
// Unity (Engage) does so when it changes scenes; only the object pointer used to be recorded, and replay read other data (a texture 1869367126 high, a Metal assertion abort).
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
#define REC_SNAP(c, OP, T, K) ((T *)rec((c), (OP), SNAP_OFF(T) + (size_t)(K) * sizeof(tex_t)))
static const tex_t *tex_snap(void *rec_, size_t off, int k, const NVNtexture *t, uint64_t ra) {
    if (!t) return NULL;
    tex_t *s = (tex_t *)((uint8_t *)rec_ + off) + k;
    memcpy(s, t, sizeof *s);
    // The object is invalid at record time (Unity finalizes and re-initializes the same render target object between recording and submitting): keep the object pointer as before and read it at submit, when it is usually valid again; replay still checks the magic
    if (s->magic != MAGIC('T', 'E', 'X', 0)) {
        static int red;
        if (red++ < 4) {   // the first 4 log the call site and the start of the object, to tell what the game passed
            module_t *m = module_of((void *)ra); const uint64_t *q = (const uint64_t *)t;
            LOG("【红】 texture object invalid while recording (uninitialized or already finalized): %p index %d, call site %s+%#llx, object start %llx %llx %llx %llx %llx %llx; read again at submit", (const void *)t, k,
                m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra,
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        }
        return (const tex_t *)t;
    }
    return s;
}
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("【红】 SetRenderTargets count %d", n); n = n < 0 ? 0 : 8; }
    // 按实际内容录（nvn_obj.h p_set_rt_c）：只录 n 个槽，非空纹理才占指针与快照，有视图才录视图
    p_set_rt t = {.n = n};
    for (int i = 0; i < n; i++) { t.color[i] = ct ? (const tex_t *)ct[i] : NULL; copy_view(&t.cview[i], &t.has_cview[i], cv ? cv[i] : NULL); }
    t.depth = (const tex_t *)dt; copy_view(&t.dview, &t.has_dview, dv);
    p_set_rt_c *h = set_rt_put(rec(c, OP_SET_RT, set_rt_size(&t)), &t);
    const tex_t **pt = (const tex_t **)(h + 1); uint32_t k = set_rt_ntex(h);
    for (uint32_t i = 0; i < k; i++) { pt[i] = tex_snap(h, set_rt_snap_off(h), (int)i, (const NVNtexture *)pt[i], (uint64_t)__builtin_return_address(0)); if (pt[i]) nvn_be_pool_gpu_written(pt[i]->pool); }
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
}
// Clears of integer targets (Paper Mario: The Thousand-Year Door): recorded as floats, which both replay paths convert to the attachment's integer type (the load action's MTLClearColor,
// clear_color_tri); exact up to 2^24 in magnitude, and 0xffffffff rounds to 2^32, which converts back to it
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int index, const int *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int index, const uint32_t *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("【红】 SetViewports first=%d n=%d", first, n); return; }
    // 按实际个数录（同 RECN 上方说明）：定长 400 字节时 Mario Tennis Aces 每画一次设一次视口，1 MB 命令内存用满后回调游戏，而那块缓冲没注册溢出处理 → 跳空指针（11-01）
    p_viewports *r = RECN(c, OP_VIEWPORTS, p_viewports, v, n); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    // 只录程序身份（见 nvn_obj.h p_bind_program_id）；程序内容与键按序号进快照表（prog_snap_put），回放用 bind_program_rec 还原
    const prog_t *o = (const prog_t *)p;
    stages = nvn2_stage_bits(stages);   // NVN2 任务 / 网格位 0x40 / 0x80 → 宿主阶段 3 / 4 的位（nvn2_cmd.h）
    *REC(c, OP_BIND_PROGRAM, p_bind_program_id) = (p_bind_program_id){o, o ? o->serial : 0, stages, 0};
    if (!o) return;
    prog_t snap; memcpy(&snap, o, sizeof snap);
    uint8_t keys[6][8], kok = 0, stage[6]; memset(keys, 0, sizeof keys); memset(stage, 0xff, sizeof stage);
    prog_keys_t *k = pk_get(p, 0);
    if (k) { memcpy(keys, k->key, sizeof keys); kok = k->ok; memcpy(stage, k->stage, sizeof stage); }
    prog_snap_put(&snap, keys, kok, stage);
#if defined(__ANDROID__) || defined(__OHOS__)   // Android 直连：按 serial 补 key、记阶段、补存源快照（Apple 回放不读，不在 Apple 上付扫表成本）
    (void)nvn_prog_snap_get(snap.serial, keys, &kok, stage);   // 合并后的最好已知（游戏按值复制的程序对象指针不同、序号相同）
    {
        // 绑定当时把 code/control 存进源快照。SetShaders 时代码可能还没登记进已映射内存池，
        // 那条路径的 keys_hash(mapped=1) 会跳过（暗黑3 第一条 draw 的 FS 就是这样）。F4.2：
        // control 优先用 SetShaders 自持的那份；key 缺失时用当时的 code + 自持 control 补算，
        // 不再依赖“Bind 时 guest control 仍有效”这个假设。
        if (s2a_source_keep) {
            int completed = 0;
            for (uint32_t i = 0; i < snap.nstage && i < 6; i++) {
                const NVNshaderData *sd = &snap.sd[i];
                if (!sd->data || (uintptr_t)sd->data >= (1ull << 47)) continue;
                uint8_t pctl[0x880];   // 自描述 control 的最大长度
                const uint8_t *ctl = NULL;
                uint32_t nctl = 0;
                {
                    uint32_t pn = 0;
                    if (s2a_source_pending_control_copy && s2a_source_pending_control_copy(snap.serial, i, pctl, sizeof pctl, &pn) &&
                        pn >= 0x760 && pn <= sizeof pctl) {
                        ctl = pctl; nctl = pn;
                    }
                }
                if (!ctl || nctl < 0x708) continue;   // 只用 SetShaders 自持的 control；guest control 已可能失效
                uint32_t ncode = 0; memcpy(&ncode, ctl + 0x704, 4);
                if (!ncode || ncode >= (1u << 24)) continue;
                void *mb; uint64_t mo;
                int mapped = nvn_be_addr(sd->data, ncode, &mb, &mo);
                if (stage[i] == 0xff && mapped) {
                    int st = prog_stage_index_parts(ctl, nctl, (const void *)(uintptr_t)sd->data, ncode);
                    if (st >= 0) { stage[i] = (uint8_t)st; completed = 1; }
                }
                if (((kok >> i) & 1) == 0) {
                    // key 在 SetShaders 没算出来：用当时的 code + 自持 control 补算（与 keys_hash 同一字节顺序）。
                    if (!mapped) continue;   // 代码仍不在已映射池：保持缺失
                    uint8_t key[8];
                    if (!keys_hash_parts(ctl, nctl, (const void *)(uintptr_t)sd->data, ncode, key)) continue;
                    memcpy(keys[i], key, 8);
                    kok |= (uint8_t)(1u << i);
                    completed = 1;
                    static _Atomic int noted;
                    if (atomic_fetch_add(&noted, 1) < 8)
                        LOG("[nvn] bind: stage %u key completed from the control held at SetShaders (code was unmapped then)", i);
                } else if (s2a_source_has && s2a_source_has(keys[i])) {
                    prog_source_pin(snap.serial, p, keys[i]);
                    if (stage[i] != 0xff && s2a_source_drop_pending_stage) s2a_source_drop_pending_stage(snap.serial, i);   // 源已在、阶段已知：自持控制段冗余（阶段没认出时 Bind 还要靠它补）
                    continue;   // 已有完整快照：不重读 guest control
                }
                src_diag("bind", (int)i, (stage[i] == 0xff) ? -1 : stage[i], keys[i],
                         (const void *)(uintptr_t)sd->data, ncode, ctl, (int)((kok >> i) & 1));
                if (s2a_source_keep(keys[i], i, (const void *)(uintptr_t)sd->data, ncode, ctl, nctl) == 0 && stage[i] != 0xff && s2a_source_drop_pending_stage)
                    s2a_source_drop_pending_stage(snap.serial, i);   // 源已存好、阶段已知：自持控制段冗余
                prog_source_pin(snap.serial, p, keys[i]);
            }
            if (completed) {
                if (k) memcpy(k->stage, stage, sizeof stage);
                prog_snap_put(&snap, keys, kok, stage);
            }
        }
    }
#endif
}
// S2Arm: the replay's snapshot of a program the game finalized or reused after binding gets the keys recorded at bind time (nvn_mtl.m prog_live)
void prog_keys_put(const prog_t *p, const uint8_t keys[6][8], uint8_t ok) { prog_keys_t *k = pk_get(p, 1); memcpy(k->key, keys, sizeof k->key); k->ok = ok; }
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = REC(c, OP_BIND_STATE, p_bind_state); r->kind = kind; memcpy(r->bytes, st, n); }
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
// 设备字节：NVN2 按驱动真值（SDK 22 的 sdk 反汇编 cmd[0] 的推进），NVN1 仍按记录大小（NVN1 驱动这两条比宿主记录还大，四个属性 68、两个流 72，不往上调）。
//   属性：一条方法 = 8 字节头 + 每个属性 4 字节，个数补成偶数。宿主记录多 first / n 两个字段，一律多报 8（四个属性 32 → 24）。
//   流：步长一条方法（8 + 每个流 4，补成偶数）+ 除数一条方法（8 + 前 m 个流各 4，补成偶数；m = 最后一个除数非 0 的流号 + 1，没有则 0）。两个流、除数全 0 时 32 → 24。
//   游戏会用这个读数定堆：一款体育游戏给每个 shape 录最多 23 种顶点状态（属性 + 流 + 每个流一次 BindVertexBuffer），把各次 GetCommandMemoryUsed 加进 shape 的预录命令大小，
//   再算进 GPU 块的提示大小，按 4 KB 进位后从按真机校准的模型堆里取；多报让块多占一页，模型堆放不下块描述，比赛中途拿空内存池建的 buffer 崩溃
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16) { LOG("【红】 BindVertexAttribState n=%d", n); return; }
    uint32_t drv = nvn2_on() ? 8 + 4 * (uint32_t)(n + (n & 1)) : 0;
    DEV_EXACT(OP_BIND_VATTR, drv, p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n));
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("【红】 BindVertexStreamState n=%d", n); return; }
    uint32_t drv = 0;
    if (nvn2_on()) {
        int m = 0; for (int i = 0; i < n; i++) if (((const vs_st *)st)[i].divisor) m = i + 1;
        drv = 16 + 4 * (uint32_t)(n + (n & 1)) + 4 * (uint32_t)(m + (m & 1));
    }
    DEV_EXACT(OP_BIND_VSTREAM, drv, p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n));
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first + n > 16) { LOG("【红】 BindVertexBuffers first=%d n=%d", first, n); return; }
    NVNbufferRange rc[16]; for (int i = 0; i < n; i++) rc[i] = (NVNbufferRange){GA(rg[i].address), rg[i].size}; rg = rc;
    if (n == 1 && rg[0].size <= UINT32_MAX) {
        *REC(c, OP_BIND_VBUF, p_bind_vbuf_single) = (p_bind_vbuf_single){first, (uint32_t)rg[0].size, rg[0].address};
        return;
    }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) { st = nvn2_stage_host(st);
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, GA(a), n};
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { st = nvn2_stage_host(st);
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, first + i, GA(rg[i].address), rg[i].size};
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) { st = nvn2_stage_host(st);
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, GA(a), n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { st = nvn2_stage_host(st);
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, GA(rg[i].address), rg[i].size};
}
// Deferred 版（nvn.h 没有原型，签名同非 Deferred 版，与已实现的 BindTexturesDeferred 同一约定）：GPU 执行这条命令时才读 rg 指向的缓冲范围，
//   录制后游戏还可以改那块内存。逐个缓冲录一条 p_bind_buf：stage 带 BIND_BUF_DEFER，addr 存游戏那条 NVNbufferRange 的地址，回放进状态时再读（nvn_mtl.m state_op）
static void bind_bufs_deferred(NVNcommandBuffer *c, int op, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { st = nvn2_stage_host(st);
    if (n < 0 || n > 16) { LOG("【红】 deferred buffer bind count %d", n); return; }
    for (int i = 0; i < n; i++) *REC(c, op, p_bind_buf) = (p_bind_buf){(int32_t)st | BIND_BUF_DEFER, first + i, (uint64_t)(uintptr_t)(rg + i), 0};
}
NVN_NOPROTO(nvnCommandBufferBindUniformBuffersDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { bind_bufs_deferred(c, OP_BIND_UBO, st, first, n, rg); }
NVN_NOPROTO(nvnCommandBufferBindStorageBuffersDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { bind_bufs_deferred(c, OP_BIND_SSBO, st, first, n, rg); }
static p_bind_handles *bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) { st = nvn2_stage_host(st);
    if (n < 0 || n > 32) { LOG("【红】 bind handle count %d", n); return NULL; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; r->defer = 0; r->src = 0; memcpy(r->h, h, 8 * n);
    {   // this command buffer may have bound a pool other than the first: move its IDs to that pool's part of the table (handle: texture ID in bits 0-19, sampler ID from bit 20)
        int tb = CB(c)->tex_base, sb = CB(c)->samp_base;
        for (int i = 0; i < n && (tb || sb); i++) {
            uint64_t v = r->h[i], tid = v & 0xfffff, sid = v >> 20;
            if (op == OP_BIND_SAMP) { r->h[i] = (sid + (uint64_t)sb) << 20; }
            else { tid += (uint64_t)tb; if (sid && op == OP_BIND_TEX) sid += (uint64_t)sb; r->h[i] = tid | sid << 20; }   // texture ID 0 is that pool's first entry, moved like any other
        }
    }
    return r;
}
// Image (writable texture): handle = texture pool index (GetImageHandle returns it as-is; RegisterImage writes the same table as RegisterTexture)
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
// Separate texture binds: on hardware they fill their own handle table (c2 0x168 + 8 slot), apart from BindTexture's, so they go to slots 128 + n (NVN_SEPTEX_SLOT0)
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateTextureHandle h) { uint64_t v; memcpy(&v, &h, 8); bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + i, 1, &v); }   // single (Engage): as the plural version
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { rec_draw(c, (p_draw){pr, first, n, 1, 0, 0}); }
// DrawTexture (S2Arm 10-30: Bare Knuckle IV called it and was "unimplemented"): replayed by nvn_mtl.m draw_tex. The region struct is opaque in our header: 4 floats x0 y0 x1 y1
NVN(nvnCommandBufferDrawTexture, void, NVNcommandBuffer *c, NVNtextureHandle h, const NVNdrawTextureRegion *dst, const NVNdrawTextureRegion *src) {
    p_draw_tex d = {.handle = h}; if (dst) memcpy(d.dst, dst, sizeof d.dst); if (src) memcpy(d.src, src, sizeof d.src);
    *REC(c, OP_DRAW_TEX, p_draw_tex) = d;
}
// Indirect: the {count, instances, first, base instance} record at ind is usually written by the GPU earlier in the frame (Hyrule Warriors: Age of Calamity's first battle), so the replay
// issues a Metal indirect draw from the pool buffer instead of reading it at record or replay time (nvn_mtl.m draw)
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind) { rec_draw(c, (p_draw){pr, 0, 0, 1, 0, GA(ind)}); }
// NVN2 任务 / 网格绘制（11-05 时之笛进 3D 场景）：count 个工作组从 first 起——有任务着色器时是任务工作组，没有时是网格工作组。录成图元 DM_PRIM_MESH_TASKS 的绘制（nvn2_cmd.h），
//   回放 nvn_geometry.inc mesh_tasks_draw；间接版的 {工作组数, 起始号} 由 GPU 写在 ind，回放时在 GPU 上换成 Metal 的间接网格参数（nvn_mtl.m ixc_run），不在录制 / 回放时读
// 设备字节按驱动真值 12：NVN2 驱动的 DrawMeshTasks 只写一条方法头加 2 个数据字（cmd[0] 进 0xc，SDK 22 的 sdk 反汇编），表里 OP_DRAW 的 32 多报 20。
//   游戏会用这个读数定堆：一款体育游戏录一笔 DrawMeshTasks 量网格材质的预录命令，加进 GPU 块的提示大小，按 4 KB 进位后从按真机校准的模型堆里取；
//   连同顶点属性 / 顶点流状态的多报（见 nvnCommandBufferBindVertexAttribState），提示越过 4 KB 进位 0xc 字节，块多占一页，随后 1016 字节的块描述配不到，模型拿空内存池建 buffer，比赛中途 memcpy 到空地址崩溃
NVN_NOPROTO(nvnCommandBufferDrawMeshTasks, void, NVNcommandBuffer *c, int first, int count) {
    DEV_EXACT(OP_DRAW, 12, rec_draw(c, (p_draw){DM_PRIM_MESH_TASKS, first, count, 1, 0, 0}));
}
NVN_NOPROTO(nvnCommandBufferDrawMeshTasksIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) { rec_draw(c, (p_draw){DM_PRIM_MESH_TASKS, 0, 0, 1, 0, GA(ind)}); }
// 三个带索引绘制入口共用。drv_bytes：游戏自带驱动该入口实际写入命令内存的字节（NVN2：反汇编各入口推进 cmd[0] 的立即数，SDK 20.5 / 21.4 / 22 与 Sports Resort 的 sdk 一致；
//   NVN1 同值：#1904 帖主在 ARM64 解释器里跑旷野之息自带的 NVN1 驱动，DrawElements 24、BaseVertex 48 / 72、Instanced 44 / 48、EndRecording 0，与这组逐项相同）。
//   游戏靠 GetCommandMemoryUsed 定大小：Sports Resort main+0x114660 录一笔 DrawElementsInstanced 量每个着色器程序的预录命令，算进 MatBlock 的 backing；按 Maxwell 的 48 算时
//   Car00 / Marinebike00 / Building00 越过 4 KB 进位，按真机校准的局部 heap 放不下管理物件，MatBlock 失败后拿空内存池建 buffer 崩溃。
//   NVN1 原先仍按 Maxwell 估算（一律 48 + END 8）：旷野之息按它给模型预录绘制定资源堆，东格鲁德遗迹石像 18 个网格多报 180 字节。#1904（1.9.0，iPad）读档时这个模型的资源堆 120 字节登记节点分配失败（main+0x155a7a4），多报是嫌疑；
//   本地只有 1.8.2（同一段代码在 main+0x1446604），同一存档修前修后都能进场景，没复现出因果
static void draw_elements(NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv, int bi, int inst, uint32_t drv_bytes) {
    DEV_EXACT(OP_DRAW_IDX, drv_bytes, rec_draw_idx(c, (p_draw_idx){pr, it, n, bv, bi, inst, GA(ib)}));
}
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    draw_elements(c, pr, it, n, ib, bv, bi, inst, 44 + (bv || bi ? 4 : 0));
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // The thunk is a tail jump (br), so the return address here is the call site in the game
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
}
// Indirect: the three group counts at ind are read by the GPU when the dispatch runs (nvn_mtl.m dispatch)
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) {
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){0, 0, 0, (uint64_t)__builtin_return_address(0), GA(ind)};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; CB(c)->tex_base = dpool_base(g_tpb, g_ntpb, p); }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; CB(c)->samp_base = dpool_base(g_spb, g_nspb, p); }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_B2T, p_copy_b2t, 1); o->src = GA(src); o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
    o->row_stride = CB(c)->copy_row_stride; o->image_stride = CB(c)->copy_image_stride;
    if (o->tex) nvn_be_pool_gpu_written(o->tex->pool);
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_T2B, p_copy_b2t, 1); o->src = GA(dst); o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
    o->row_stride = CB(c)->copy_row_stride; o->image_stride = CB(c)->copy_image_stride;
}
// Clear a texture region with the given value (float / signed / unsigned integer; mask = channel write mask)
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->value_type = CLEAR_TEX_FLOAT; memcpy(o->c.f, rgba, 16); if (o->tex) nvn_be_pool_gpu_written(o->tex->pool);
}
NVN(nvnCommandBufferClearTexturei, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const int *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->value_type = CLEAR_TEX_INT; memcpy(o->c.i, rgba, 16); if (o->tex) nvn_be_pool_gpu_written(o->tex->pool);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->value_type = CLEAR_TEX_UINT; memcpy(o->c.u, rgba, 16); if (o->tex) nvn_be_pool_gpu_written(o->tex->pool);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2); o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, (uint64_t)__builtin_return_address(0)); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, (uint64_t)__builtin_return_address(0));
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
    if (o->dst) nvn_be_pool_gpu_written(o->dst->pool);
}
// NVN stores these as 32-bit object fields; setters do not record commands or consume command memory.
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { CB(c)->copy_row_stride = (uint32_t)st; }
NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t st) { CB(c)->copy_image_stride = (uint32_t)st; }
NVN(nvnCommandBufferGetCopyRowStride, ptrdiff_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->copy_row_stride; }
NVN(nvnCommandBufferGetCopyImageStride, ptrdiff_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->copy_image_stride; }
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

// ---------- State objects: packed in place (nvn_obj.h) ----------
#define ST(T, x) ((T *)(x))
// 高级混合位的缺省（#344）：按 SDK 机器码——SetDefaults 一条 str 写 8 字节常量 0x0102_0102_110c_0000，同一份 SDK 的 GetAdvancedPremultipliedSrc 取第 18 位、
//   GetAdvancedNormalizedDst 取第 19 位（都为 1）、GetAdvancedOverlap 取 +2 半字低 2 位（0 = UNCORRELATED）、GetAdvancedMode 取第 8–13 位（0 = NONE）。
//   7 份自有游戏 SDK 一致（怪猎 / 暗黑2 名字表反查 SetDefaults = sdk+0x473c70；动森 / 塞尔达 / 丝之歌 / 异度3 / 暗黑3 机器码逐字节相同；马车8 老 SDK 同一常量），10-25 直连线读出。
//   这几位只影响 Get 读回：渲染器不模拟高级混合，管线键里也清掉了（nvn_mtl.m），不改画面
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, .adv_premul = 1, .adv_normdst = 1}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
// Advanced blend modes (NV_blend_equation_advanced: multiply, screen, overlay, ...) have no Metal blend state; mode 0 (none, which Luigi's Mansion 3 sets on every blend state) is the default
NVN(nvnBlendStateSetAdvancedMode, void, NVNblendState *s, NVNblendAdvancedMode mode) {
    ST(blend_st, s)->adv_mode = mode;
    static int once; if (mode && !once++) LOG("【红】 nvnBlendStateSetAdvancedMode %d: advanced blend modes are not implemented, the state blends with its blend functions", mode);
}

// 高级混合的其余三个开关（overlap / premultiplied src / normalized dst）：NVN 语义上是「配合高级模式的修饰位」，
// 渲染器没有模拟高级模式，所以这里只保住状态与查询结果（写入后可读回、与其它 blend 字段互不别名），
// 不放行任何「已实现」的假象：真正用到非零 adv_mode 时上面那条【红】照旧点名。
NVN(nvnBlendStateSetAdvancedOverlap, void, NVNblendState *s, NVNblendAdvancedOverlap overlap) { ST(blend_st, s)->adv_overlap = (unsigned)overlap & 3u; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc, void, NVNblendState *s, unsigned p) { ST(blend_st, s)->adv_premul = p ? 1u : 0u; }
NVN(nvnBlendStateSetAdvancedNormalizedDst, void, NVNblendState *s, unsigned n) { ST(blend_st, s)->adv_normdst = n ? 1u : 0u; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms2)); ST(ms_st, s)->enable = 1; }   // ms2：连 NVN2 的覆盖截断一起清（nvn2_cmd.h）
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)n; (void)v; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->samples; }
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->enable; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("【红】 vertex attribute offset %td exceeds the packed range", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }
// Getters: Monster Hunter Rise reads its stream states back when it builds a draw (about 24,000 calls in its character creation)
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return (ptrdiff_t)((const vs_st *)s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return ((const vs_st *)s)->divisor; }
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { const va_st *o = (const va_st *)s; if (f) *f = (NVNformat)o->fmt; if (off) *off = (ptrdiff_t)o->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return ((const va_st *)s)->stream; }

static _Atomic uint64_t g_sub_wall_ns, g_sub_cpu_ns, g_sub_instr;   // Wall clock / this thread's CPU time / this thread's retired instruction count for submit (excluding present) (taken away per frame)
static _Atomic uint64_t g_cum_sub_instr, g_cum_sub_cpu_ns;   // Cumulative versions of the above (the "translation" summary row every 300 frames takes the difference; on a real device read through the log port: instructions per draw / submit CPU per frame)
extern int thread_selfcounts(int type, void *buf, size_t nbytes);   // This thread's cumulative {instructions, cycles} (libSystem private interface, present on both Mac and iOS)
// Supplementary readings for the submit path (on a real device Metal 4 instruction count is -18% while submit CPU time is +49%; need to tell apart kernel mode, execution efficiency, or frequency throttling):
// this thread's cycle count (the second item of the same interface), kernel-mode CPU time (THREAD_BASIC_INFO), wall clock
static void thr_counts(uint64_t *instr, uint64_t *cyc) {
#if defined(__ANDROID__) || defined(__OHOS__)
    *instr = 0; *cyc = 0;   // 未接，不填假值
#else
    uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); *instr = c[0]; *cyc = c[1];
#endif
}
static uint64_t thr_sys_ns(void) {
#if defined(__ANDROID__) || defined(__OHOS__)
    return 0;   // Android 内核态 CPU 时间采样未接
#else
    thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT; mach_port_t me = mach_thread_self(); uint64_t r = 0;
    if (thread_info(me, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS) r = (uint64_t)bi.system_time.seconds * 1000000000ull + (uint64_t)bi.system_time.microseconds * 1000ull;
    mach_port_deallocate(mach_task_self(), me); return r;
#endif
}
static _Atomic uint64_t g_cum_sub_cyc, g_cum_sub_sys_ns, g_cum_sub_wall_ns;
// Where the submit thread's CPU ran (11-06 Switch2 线 71): the process-wide share on performance cores (power line, task_ptime) cannot tell whether the submit thread itself
// was on them. thread_selfcounts kind 4 (THSC_TIME_CPI_PER_PERF_LEVEL) gives this thread's {instructions, cycles, user, system time in mach units} per performance level,
// level 0 = hw.perflevel0 = the performance cores (checked on a Mac: a `taskpolicy -b` process counts only into level 1, at 2.67 GHz). Same cost as kind 1 (about 125 ns)
static _Atomic uint64_t g_cum_sub_lv_t[2], g_cum_sub_lv_cyc[2];   // submit CPU time (mach units) and cycles on level 0 / level 1
typedef struct { uint64_t ins, cyc, ut, st; } thsc_lv;
static void thr_counts_lv(thsc_lv lv[2]) {
#if defined(__ANDROID__) || defined(__OHOS__)
    memset(lv, 0, 2 * sizeof *lv);   // 未接，不填假值
#else
    memset(lv, 0, 2 * sizeof *lv); thread_selfcounts(4, lv, 2 * sizeof *lv);
#endif
}
// The thread that submits is the frame's critical path, whatever its guest role (11-06 Switch2 线 71). runtime/src/thread.c gives guest threads other than the guest main thread
// USER_INITIATED on an iPhone (2 performance cores); a game that submits from such a thread runs its whole translation + encoding on the efficiency cores once the kernel drops the
// acquire thread's real-time declaration (Zelda Ocarina, Duo 11-06 out_1106181717: present thread QoS 0x19, real-time policy not in effect, 0% of process CPU on performance cores,
// mean clock 1.49 GHz, submit CPU 13.8 ms a frame). Raise the submitting thread to USER_INTERACTIVE: checked at the first submit and every 256th after (the kernel may revoke a
// real-time declaration later, and the thread then falls back to its QoS). A real-time thread reads QoS unspecified and refuses a QoS change (EPERM): left as it is.
// Where every guest thread is USER_INTERACTIVE already (a Mac) this changes nothing. DM_NVN_RENDER_QOS=0 leaves the thread alone (A/B on a device)
#if !defined(__ANDROID__) && !defined(__OHOS__)
#include "../../../../runtime/include/s2a_qos.h"   // 全进程线程调度档一张表：提交线程 = S2A_QOS_SUBMIT
static int render_qos_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_RENDER_QOS"); v = !(e && !strcmp(e, "0")); } return v; }
static _Atomic uint32_t g_sub_qos_seen;   // the submit thread's QoS as last checked (summary line)
#endif
static void render_thread_qos(void) {
#if !defined(__ANDROID__) && !defined(__OHOS__)
    static __thread uint32_t n;
    if (n++ & 255) return;
    qos_class_t q = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &q, &rp);
    if (q != s2a_qos(S2A_QOS_SUBMIT) && q != QOS_CLASS_UNSPECIFIED && render_qos_on()) {
        int rc = s2a_qos_set_self(S2A_QOS_SUBMIT); qos_class_t g = QOS_CLASS_UNSPECIFIED; pthread_get_qos_class_np(pthread_self(), &g, &rp);
        static _Atomic int nl; if (atomic_fetch_add(&nl, 1) < 8)
            LOG("%s[nvn] submit thread %#lx QoS %#x → %#x (rc %d, now %#x): the submitting thread is the frame's critical path (s2a_qos.h S2A_QOS_SUBMIT; DM_NVN_RENDER_QOS=0 leaves it)", rc || g != s2a_qos(S2A_QOS_SUBMIT) ? "【红】 " : "",
                (unsigned long)(uintptr_t)pthread_self(), (unsigned)q, (unsigned)s2a_qos(S2A_QOS_SUBMIT), rc, (unsigned)g);
        q = g;
    }
    atomic_store_explicit(&g_sub_qos_seen, (uint32_t)q, memory_order_relaxed);
#endif
}
static int frame_rec(void);
static _Atomic uint64_t g_nvn_ns;   // Host time spent in submit / present this frame (replay encoding + present), used by the timing instrumentation

// ---------- Lifecycle: dm_gfx_suspend (called before iOS goes to the background; equivalent to Switch suspend) ----------
// on=1 only closes the entry gate and must not wait: UIKit's scene-update callback has a 10 second watchdog.
// dm_gfx_suspend_drain runs on the lifecycle worker with iOS background-task time: wait for calls already inside the gate, then drain GPU work.
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_susp_cv = PTHREAD_COND_INITIALIZER;
static int g_susp, g_inflight_api;
static _Atomic int g_session_stopping;
int dm_gfx_session_stopping(void) { return atomic_load(&g_session_stopping); }
__thread _Atomic int dm_host_work;   // see hle.h (defined here: the graphics unit tests compile this file but not hle_thread.c)
static void gfx_enter(void) { pthread_mutex_lock(&g_susp_mu); int blk = g_susp; if (blk) DM_BLOCK_BEGIN(); while (g_susp && !dm_gfx_session_stopping()) pthread_cond_wait(&g_susp_cv, &g_susp_mu); g_inflight_api++; pthread_mutex_unlock(&g_susp_mu); if (blk) DM_BLOCK_END(); dm_host_work++; }
static void gfx_leave(void) { dm_host_work--; pthread_mutex_lock(&g_susp_mu); g_inflight_api--; pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu); }
void dm_gfx_enter(void) { gfx_enter(); }   // the same gate for the Vulkan layer's submits and presents (dm/gfx/vk)
void dm_gfx_leave(void) { gfx_leave(); }
__attribute__((weak)) void dm_vk_drain(void) {}   // dm/gfx/vk/hle_vk.m: wait for the Vulkan layer's queue (the graphics unit tests do not link it)
__attribute__((weak)) void dm_gl_drain(void) {}   // dm/gfx/gl/hle_gl.m: the same for the GL layer
__attribute__((weak)) void nvn_be_background_purge(int bg) { (void)bg; }   // nvn_mtl.m 切后台瘦身（图形单测不链）
void (*dm_gfx_test_drain)(void);   // t_nvn_suspend: exercise the real gate / wait without acquiring a Metal device slot
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&g_susp_mu);
    g_susp = on != 0;
    if (!on) nvn_be_background_purge(0);   // 回前台：放开闸门前先把后台标成可清除的贴图标回来（被清掉的登记重新上传）
    if (!on) pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
    if (!on) LOG("[nvn] graphics resumed");
}
void dm_gfx_suspend_drain(void) {
    pthread_mutex_lock(&g_susp_mu);
    while (g_susp && g_inflight_api) pthread_cond_wait(&g_susp_cv, &g_susp_mu);
    int suspended = g_susp;
    pthread_mutex_unlock(&g_susp_mu);
    if (!suspended) { LOG("【红】[nvn] graphics suspend drain reached after the gate reopened; drain skipped"); return; }
    if (dm_gfx_test_drain) dm_gfx_test_drain();
    else { dm_vk_drain(); dm_gl_drain(); nvn_be_drain(); }   // Vulkan and GL first: presents on the NVN queue wait for their frames
    LOG("[nvn] graphics suspended: in-flight GPU work drained");
    if (!dm_gfx_test_drain) nvn_be_background_purge(1);   // 切后台瘦身：GPU 已排空，把可重建的私有贴图标成可清除（nvn_mtl.m）
}
int dm_gfx_guest_start_hold(void) {
    pthread_mutex_lock(&g_susp_mu);
    g_susp = 1;
    int inflight = g_inflight_api;
    pthread_mutex_unlock(&g_susp_mu);
    if (inflight) {
        LOG("【红】重启首帧屏障：直连图形 API 仍有 %d 个在途调用，gate 已关闭，不能在未排空时启动 guest", inflight);
        return -1;
    }
    LOG("[nvn] graphics guest-start gate closed: no in-flight API calls");
    return 0;
}
// One submission at a time: the Metal replay keeps one command buffer and one upload batch for the submission in progress (nvn_mtl.m g_cb). Fire Emblem: Three Houses initializes two queues and
// submits on both from two threads; the second submission arrived while the first was replaying and the host stopped with "submit re-entered" at the ninth frame. NVN orders work on different queues only
// through syncs, so replaying the two queues' submissions in lock order is one valid order
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER;
// QueueFinish 与提交互斥（同 QueueWaitEvent / 帧录上屏）：上传批属于提交线程（nvn_mtl.m up_owner_check），Finish 要先冲掉本线程的上传批再等 GPU。
//   原先不持锁：上一个提交线程在 Finish 里判了「是提交线程」之后，另一条游戏线程开始新提交、改了提交线程，up_flush 的归属检查当场 abort
//   （11-02 满屏线 马里奥和索尼克东京 2020 Mac 局 3 崩 1：nx-100 调 up_flush）；两条线程同时碰同一个上传批本身就是竞争，检查只是把它抓出来
//   锁只罩「冲上传批 + 取栅栏」两步，等 GPU 放锁（等 GPU 期间别的线程照常提交；直连线读码确认没有持 g_sub_mu 调 Finish 的路径）。
//   没有分段实现的后端（安卓 nvn_be_compat.h）走弱定义：持锁做完整 Finish
__attribute__((weak)) uint64_t nvn_be_finish_publish(void) { nvn_be_finish(); return 0; }
__attribute__((weak)) void nvn_be_finish_wait(uint64_t fence) { (void)fence; }
static void xq_lock(void) { pthread_mutex_lock(&g_sub_mu); }
static void xq_unlock(void) { pthread_mutex_unlock(&g_sub_mu); }
// 提交锁给回放核心（graphics/nvn/replay/rp_entry.c）：ReadTexels 读 GPU 内容要开一段旁路段，开段 / 交段与栅栏值分配都归提交线程，
//   和 nvnQueueSubmitCommands 串行。苹果后端不调它（nvn_mtl.m 的读回走自己的旧队列），对苹果没有行为变化
void nvn_hle_submit_lock(void) { xq_lock(); }
void nvn_hle_submit_unlock(void) { xq_unlock(); }
static void queue_finish(void) { pthread_mutex_lock(&g_sub_mu); uint64_t f = nvn_be_finish_publish(); pthread_mutex_unlock(&g_sub_mu); nvn_be_finish_wait(f); }
int nvn_frec_on(void); void nvn_frec_submit(const uint64_t *h, int n); void nvn_frec_present(const tex_t *t, double period, const int crop[4]);   // 帧录（nvn_frame_rec.inc）
static NVNqueue *g_err_queue;   // 最近一次提交的队列（nvnDeviceWaitForError 报错时填进错误信息）
// ---------- NVN ⇄ GL 同步对象（火影忍者 终极风暴羁绊 0100D010190A8000：GL 画、NVN 读，双向栅栏） ----------
// 签名依据：游戏 sdk 里这两个名字的跳板按「第一个空闲寄存器」装目标地址（sdk+0x4530f0 用 x2 → 2 个参数；sdk+0x453120 用 x4 → 4 个参数，同表 QueueFenceSync x4 / QueueWaitSync x2 /
//   DeviceInitialize x2 对得上），真实现在系统固件驱动里、游戏包里没有；参数类型照 main 的调用点（main+0x8fd0dc：x0 = NVNsync*，x1 = 存 GL 同步句柄的 8 字节；main+0x8fc4c0：x0 = NVNsync*，
//   x1 = NVNdevice*，x2 = glFenceSync 的返回值，x3 = 4 字节输出）。返回值两处调用都不读，按 NVN 惯例回 NVNboolean（没有反汇编依据）。
#if !defined(__ANDROID__) && !defined(__OHOS__)
uint64_t gl_sync_from_event(void *ev, uint64_t val); int gl_sync_event(uint64_t h, void **ev, uint64_t *val, int *committed);   // gfx/gl/gl_draw.m
static void sync_ext_queue_wait(const sync_t *s) {
    void *ev; uint64_t v; if (!sync_ext(s, &ev, &v)) return;
    pthread_mutex_lock(&g_sub_mu); nvn_be_queue_wait_event(ev, v); pthread_mutex_unlock(&g_sub_mu);   // 与提交互斥：等待排在本线程此后的提交之前
}
static int sync_ext_wait(const sync_t *s, uint64_t timeout, int *is_ext) {
    void *ev; uint64_t v; *is_ext = sync_ext(s, &ev, &v); return *is_ext && nvn_be_event_wait(ev, v, timeout);
}
// NVN 同步对象 → GL 同步对象：GL 侧等 NVN 层提交栅栏的共享事件到这个同步对象的栅栏号（glClientWaitSync 在 CPU 上等，glWaitSync 编码进 GL 命令缓冲在 GPU 上等）。
//   同步对象来自 GL（InitializeFromFencedGLSync）时直接转回那个 GL 栅栏。还没围栏：回假、不写（没有可等的东西）
NVN_NOPROTO(nvnSyncCreateGLSync, NVNboolean, const NVNsync *s, uint64_t *gl) {
    uint64_t fence; void *ev; uint64_t v;
    if (!s || !gl || !sync_published((const sync_t *)s, &fence)) { static int n; if (n++ < 4) LOG("【红】 nvnSyncCreateGLSync: the sync object has no fence yet; no GL sync created"); return 0; }
    if (!sync_ext((const sync_t *)s, &ev, &v)) { ev = nvn_be_fence_event(); v = fence; }
    uint64_t h = gl_sync_from_event(ev, v); if (!h) return 0;
    *gl = h; return 1;
}
// GL 同步对象 → NVN 同步对象：之后 nvnQueueWaitSync 在 GPU 上、nvnSyncWait 在 CPU 上等 GL 层共享事件到那个提交序号。
//   status（4 字节，按 NVNsyncWaitResult）：初始化当下用 0 超时查一次 GL 栅栏的结果——已完成 ALREADY_SIGNALED、还没完成 TIMEOUT_EXPIRED（即「查询，没等」）、句柄无效 FAILED（回假）。
//   GL 栅栏所在的命令缓冲还没提交（游戏没 glFlush）时照样建，NVN 侧要等到 GL 下次提交；报红一次
NVN_NOPROTO(nvnSyncInitializeFromFencedGLSync, NVNboolean, NVNsync *s, NVNdevice *d, const void *glsync, int *status) {
    (void)d; void *ev; uint64_t v; int committed;
    if (!s || !gl_sync_event((uint64_t)(uintptr_t)glsync, &ev, &v, &committed)) { if (status) *status = NVN_SYNC_WAIT_RESULT_FAILED; LOG("【红】 nvnSyncInitializeFromFencedGLSync: GL sync %p is not a GL sync object", glsync); return 0; }
    if (!committed) { static int once; if (!once++) LOG("【红】 nvnSyncInitializeFromFencedGLSync: the GL fence is not flushed yet (no glFlush); NVN waits until GL's next submit"); }
    *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0, NULL, 0}; ((sync_x *)s)->rm = 0; sync_publish_ext((sync_t *)s, 0, ev, v);
    if (status) *status = nvn_be_event_wait(ev, v, 0) ? NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    return 1;
}
// ---------- nvnDeviceWaitForError（DOUBLE DRAGON Ⅳ 010001B005E5C000 / Snipperclips 0100704000B3A000 的加载器按名取） ----------
// 签名与语义照 DD4 自带的 NVN 驱动（subsdk4+0x835cc，名字表第 209 项）：int (NVNdevice *device, uint64_t timeout_ns, 错误信息 *info)。
//   超时按纳秒除以 1000 交给 NvRmGpuDeviceWaitForError（阻塞，以微秒计）；没等到错误回 0、不写 info；等到错误回 1，info 头 4 字节写 1、+8 写出错的 NVNqueue*（驱动按通道号在设备的 256 个队列槽里找，
//   找不到写 0）；NvRm 回状态 2 时原样回 2（本层不会出现）。本层的「GPU 错误」= 本层命令缓冲执行报错（每次报错算一次，不重复报）；出错队列取最近一次提交的队列（本层所有队列共用一条 Metal 队列）
NVN_NOPROTO(nvnDeviceWaitForError, int, NVNdevice *d, uint64_t timeout_ns, void *info) {
    (void)d; static uint64_t seen;
    if (!nvn_be_wait_error(&seen, timeout_ns)) return 0;
    if (info) { const uint32_t kind = 1; NVNqueue *q = g_err_queue; memcpy(info, &kind, 4); memcpy((uint8_t *)info + 8, &q, 8); }
    return 1;
}
#else
static void sync_ext_queue_wait(const sync_t *s) { (void)s; }   // Android 没有 GL 层：不会有别的队列的栅栏
static int sync_ext_wait(const sync_t *s, uint64_t timeout, int *is_ext) { (void)s; (void)timeout; *is_ext = 0; return 0; }
#endif
// nvnInstallGlobalDebugCallback（密特罗德 究极4 010019A01E2F2000 的加载器按名取）：游戏自带 sdk 导出的这个函数整个就是一条 ret（究极4 sdk+0x4b06e0、火影 sdk+0x451310，
//   符号大小 4 字节）——零售驱动里全局调试回调不装、不调，什么都不做。本层照做；参数个数从一条 ret 定不出，按（回调, 用户数据）收
NVN_NOPROTO(nvnInstallGlobalDebugCallback, void, const void *callback, void *data) { (void)callback; (void)data; }
// ---------- 驱动 GetProcAddress 名字全集里的 NVX / 私有入口（11-03 按族补齐，替代逐款补名字） ----------
// 来源：游戏自带 NVN 驱动的名字解析（SDK 20.5 = 0100A00019DE0000 sdk 镜像，nvnDeviceGetProcAddress 0x4c3730 经指针 0xb836c8 尾调 0x4c79d0：名字以 nvn 开头就在 533 项有序表里二分；
//   查不到或不以 nvn 开头就进 0x4c6f60 的 strcmp 链，链上 32 个名字，最后一项 nvnInstallGlobalDebugCallback）。下文「20.5 0x…」均为该镜像里的函数地址，
//   6.4（010002C00C270000）与 10.9（01002B9012E28000）同名函数语义一致（字段偏移不同）。全表见 nvn_drv_names.txt，tests/nvn_drv_names_test.sh 守差集为空。
// 起因：大乱闘スマッシュブラザーズ SPECIAL（01006A800016E000）首帧前取 nvnSetExternalDebuggerEnabledNVX 拿到 NULL 不判空就调，崩在 pc 0（lr main+0x37fd300）。
// 本层没有 host1x / nvmap 通道 / GPU 通道这些真机对象：开关类只记下参数；查询类回本层的等价值（写在每条上）；拦截一族按「本层不让别的层接管分派」实现。

// 20.5 0x4c7580：经全局函数指针 0xb836c0 尾调，缺省目标 0x4c7bd0 是一条 ret——零售驱动里这个开关什么都不做（参数一个，按布尔收）。本层同样不改任何行为，只记下
static uint32_t g_ext_dbg_en;
NVN_NOPROTO(nvnSetExternalDebuggerEnabledNVX, void, uint32_t on) {
    g_ext_dbg_en = B(on); static int once; if (!once++) LOG("[nvn] SetExternalDebuggerEnabledNVX(%u)：零售驱动的缺省实现是空函数，本层只记下", g_ext_dbg_en);
}
// 20.5 0x4c73a0：把参数存进驱动全局（0xc364d8）。那是 GL 驱动交给 NVN 的接口对象（虚表 +0 是否可用、+0x10 按 GL 纹理名建纹理、+0x20 建 GL 栅栏），
//   被 MemoryPoolBuilderSetStorage 附近、TextureInitialize（构建器带 GL 纹理名时）、SyncCreateGLSync、SyncInitializeFromFencedGLSync 读。
//   本层的 GL 是宿主自己的实现，GL ⇄ NVN 互操作走宿主 GL 层（nvnSyncCreateGLSync 等已接），用不到游戏这侧的虚表：只记下指针、首次点名
static const void *g_gl_interop;
NVN_NOPROTO(nvnSetGLInteropNVX, void, const void *iface) {
    g_gl_interop = iface; static int once; if (!once++) LOG("[nvn] SetGLInteropNVX(%p)：GL 互操作走宿主 GL 层，游戏侧接口对象只记下不调用", iface);
}
// 20.5 0x4cee30：超时（纳秒）除以 10^6 换成毫秒交给 GPU 通道的超时设置（0x4d6648 → 0x65ddf0），回值即通道调用的返回码（0 = 成功）。
//   本层没有 GPU 通道（提交在宿主线程同步执行）：记在队列上、回 0
NVN_NOPROTO(nvnQueueSetTimeoutNVX, int, NVNqueue *q, uint64_t timeout_ns) {
    if (q) ((queue_x *)q)->timeout_ns = timeout_ns;
    static int once; if (!once++) LOG("[nvn] QueueSetTimeoutNVX(%p, %llu ns)：本层没有 GPU 通道，只记下", (void *)q, (unsigned long long)timeout_ns);
    return 0;
}
// 20.5 0x4cced0：时间片（微秒）按通道参数换算后交给 GPU 通道（0x4d66ac），无条件回 1。本层记在队列上、回 1
NVN_NOPROTO(nvnQueueSetTimesliceNVX, NVNboolean, NVNqueue *q, uint32_t timeslice_us) {
    if (q) ((queue_x *)q)->timeslice_us = timeslice_us;
    static int once; if (!once++) LOG("[nvn] QueueSetTimesliceNVX(%p, %u us)：本层没有 GPU 通道，只记下", (void *)q, timeslice_us);
    return 1;
}
// 20.5 0x4c5e70 / 0x4c5e80：设备构建器 +8 存个数、+0x10 存区间数组指针；Get 把指针写进出参、回个数。只存不用（设备初始化才用它们划 GPU 地址空间）。
//   本层的 GPU 地址窗口是宿主自己分的（gva_alloc），不按游戏预留的区间：记下读回，首次带非零个数时报红
NVN_NOPROTO(nvnDeviceBuilderSetReservedAddressRangesNVX, void, NVNdeviceBuilder *b, int n, const void *ranges) {
    ((dev_builder_x *)b)->nres = n; ((dev_builder_x *)b)->res = ranges;
    static int once; if (n && !once++) LOG("【红】 DeviceBuilderSetReservedAddressRangesNVX：游戏预留了 %d 段 GPU 地址区间，本层 GPU 地址自己分配、不避让这些区间", n);
}
NVN_NOPROTO(nvnDeviceBuilderGetReservedAddressRangesNVX, int, const NVNdeviceBuilder *b, const void **ranges) {
    if (ranges) *ranges = ((const dev_builder_x *)b)->res;
    return ((const dev_builder_x *)b)->nres;
}
// 20.5 0x4c8c80 / 0x4c8c90：池构建器 +0x20 存 nvmap 句柄，Set 同时清掉存储指针和大小（+0x10 / +0x18）；SetStorage（0x4c8c70）反过来清句柄——两者互斥。本层照做
NVN_NOPROTO(nvnMemoryPoolBuilderSetNativeHandleNVX, void, NVNmemoryPoolBuilder *b, int32_t h) {
    pool_builder_x *o = (pool_builder_x *)b; o->b.mem = NULL; o->b.size = 0; o->native = h;
}
NVN_NOPROTO(nvnMemoryPoolBuilderGetNativeHandleNVX, int32_t, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_x *)b)->native; }
// 20.5 0x4c8ce0 / 0x4c8d10 / 0x4c8d20 / 0x4c8cf0：池构建器 +0x28 / +0x30 / +0x38 存预留的线性（pitch）/ 块线性 / 特殊映射 GPU 地址，GetReservedAddresses 依次写进三个出参。
//   SetDefaults（0x4c8c50）清零三者——本层 SetDefaults 整个清零，同效
NVN_NOPROTO(nvnMemoryPoolBuilderSetPitchAddressNVX, void, NVNmemoryPoolBuilder *b, uint64_t a) { ((pool_builder_x *)b)->res_pitch = a; }
NVN_NOPROTO(nvnMemoryPoolBuilderSetBlockLinearAddressNVX, void, NVNmemoryPoolBuilder *b, uint64_t a) { ((pool_builder_x *)b)->res_bl = a; }
NVN_NOPROTO(nvnMemoryPoolBuilderSetSpecialAddressNVX, void, NVNmemoryPoolBuilder *b, uint64_t a) { ((pool_builder_x *)b)->res_sp = a; }
NVN_NOPROTO(nvnMemoryPoolBuilderGetReservedAddressesNVX, void, const NVNmemoryPoolBuilder *b, uint64_t *pitch, uint64_t *bl, uint64_t *sp) {
    const pool_builder_x *o = (const pool_builder_x *)b; *pitch = o->res_pitch; *bl = o->res_bl; *sp = o->res_sp;
}
// 20.5 0x4c9050：回池对象 +0x58 的 nvmap 句柄（普通池是驱动初始化时分的，带 SetNativeHandleNVX 建的池是导入的那个）。
//   本层：构建器给过句柄就回它（MemoryPoolInitialize 存进 nvmap），否则按需开一个（与 nvnTextureGetNvRmSurfaceNVX 同一个，pool_nvmap）；开不出（虚拟池、没有服务层）回 0，pool_nvmap 已报红
NVN_NOPROTO(nvnMemoryPoolGetNativeHandleNVX, int32_t, const NVNmemoryPool *p) {
    int32_t h = __atomic_load_n(&((const pool_t *)p)->nvmap, __ATOMIC_ACQUIRE);   // 构建器给的（没有存储，pool_nvmap 会直接回 0）或已经开过的
    return h ? h : pool_nvmap((pool_t *)p);
}
// 20.5 0x4c8d30 / 0x4c8d40：回池对象 +0x20 / +0x28，即池的块线性映射 / 特殊映射的 GPU 地址（真机初始化时按构建器预留的地址或自己分）。
//   本层贴图不按 GPU 地址取、没有这两种映射：构建器预留过就回预留值（MemoryPoolInitialize 存进 bl_va / sp_va），否则回 0 并首次报红——不是真机值
NVN_NOPROTO(nvnMemoryPoolGetBlockLinearAddressNVX, uint64_t, const NVNmemoryPool *p) {
    uint64_t a = ((const pool_x *)p)->bl_va; static int once; if (!a && !once++) LOG("【红】 MemoryPoolGetBlockLinearAddressNVX：本层没有块线性映射，池 %p 回 0（真机回驱动分的地址）", (const void *)p);
    return a;
}
NVN_NOPROTO(nvnMemoryPoolGetSpecialAddressNVX, uint64_t, const NVNmemoryPool *p) {
    uint64_t a = ((const pool_x *)p)->sp_va; static int once; if (!a && !once++) LOG("【红】 MemoryPoolGetSpecialAddressNVX：本层没有特殊映射，池 %p 回 0（真机回驱动分的地址）", (const void *)p);
    return a;
}
// 20.5 0x4d0820 / 0x4d0850 / 0x4d08c0：只认同步对象类型 5（只有 InitializeFromNvRmSyncNVX 0x4d0720 置 5，同时把 NvRmSync 里至多 4 个 (id, 值) 存在 +0x10）。
//   GetSyncpointsCount(sync, int *n)：类型 5 且 n 非空 → *n = 个数、回 1，否则回 0；GetSyncpoints(sync, n, 出参) / SetSyncpoints(sync, n, 入参)：类型 5、数组非空、n 等于存的个数 → 逐对拷、回 1，否则回 0。
//   本层：普通 NVN 同步对象同真机回 0。NvRmSync 来的同步对象本层不解析 NvRmSync（视为已到达，见上 InitializeFromNvRmSyncNVX），按 1 对 syncpoint 交出无效 syncpoint (0xFFFFFFFF, 0)
//   ——与 nvnSyncGetNvRmSyncNVX 交出的约定相同，消费方都当「不用等」；Set 照收个数校验、回 1，新值同样视为已到达（假设：交进来的是别的引擎已完成的栅栏）
NVN_NOPROTO(nvnSyncGetSyncpointsCountNVX, NVNboolean, const NVNsync *s, int *n) {
    if (!n || ((const sync_x *)s)->rm != NVN_SYNC_RM) return 0;
    *n = 1; return 1;
}
NVN_NOPROTO(nvnSyncGetSyncpointsNVX, NVNboolean, const NVNsync *s, int n, uint32_t *pairs) {
    if (((const sync_x *)s)->rm != NVN_SYNC_RM || !pairs || n != 1) return 0;
    pairs[0] = 0xFFFFFFFFu; pairs[1] = 0; return 1;
}
NVN_NOPROTO(nvnSyncSetSyncpointsNVX, NVNboolean, NVNsync *s, int n, const uint32_t *pairs) {
    if (((const sync_x *)s)->rm != NVN_SYNC_RM || !pairs || n != 1) return 0;
    static int once; if (!once++) LOG("[nvn] SyncSetSyncpointsNVX：syncpoint (%#x, %u) 视为已到达", pairs[0], pairs[1]);
    return 1;
}
// 20.5 0x4c31a0：(命令缓冲, 源纹理, 源区域, 目标纹理, 目标区域)。只读源区域的 x、y、宽、高（+0 / +4 / +0xc / +0x10）和目标区域的 x、y，深度固定 1、不带视图、mip 0，
//   交给拷贝引擎（0x4c1660）。本层没有单独的拷贝引擎：按同样的 2D 区域走普通的纹理到纹理拷贝（同一条录制 / 回放路径），标志 0
NVN_NOPROTO(nvnCommandBufferCopyTextureToTextureWithCopyEngineNVX, void, NVNcommandBuffer *c, const NVNtexture *src, const NVNcopyRegion *sr, const NVNtexture *dst, const NVNcopyRegion *dr) {
    NVNcopyRegion s = {sr->xoffset, sr->yoffset, 0, sr->width, sr->height, 1}, d = {dr->xoffset, dr->yoffset, 0, sr->width, sr->height, 1};
    h_nvnCommandBufferCopyTextureToTexture(c, src, NULL, &s, dst, NULL, &d, 0);
}

// ---------- NVN2（Switch 2）：推常量、DLSS（记录格式与对象布局见 nvn2_cmd.h；回放 nvn_mtl.m / nvn_dlss.inc）----------
// 推常量：(命令缓冲, 槽, 偏移, 字节数, 数据)，数据录制时拷进命令内存。火纹 Fortune's Weave 每帧都推（11-03 真机），翻译出的着色器读 c[24]（1169 处绑定）
static void push_const(NVNcommandBuffer *c, int compute, int slot, int offset, int size, const void *data) {
    unsigned s, off, n;
    if (!data || !nvn2_push_range(slot, offset, size, &s, &off, &n)) return;
    p_push_const *o = (p_push_const *)rec(c, OP_PUSH_CONST, sizeof *o + n);
    o->compute = (uint8_t)compute; o->slot = (uint8_t)s; o->offset = (uint16_t)off; o->size = n; memcpy(o + 1, data, n);
}
NVN_NOPROTO(nvnCommandBufferPushGraphicsConstants, void, NVNcommandBuffer *c, int slot, int offset, int size, const void *data) { push_const(c, 0, slot, offset, size, data); }
NVN_NOPROTO(nvnCommandBufferPushComputeConstants, void, NVNcommandBuffer *c, int slot, int offset, int size, const void *data) { push_const(c, 1, slot, offset, size, data); }
// DLSS 网络构建器 / 网络 / 参数：游戏分配的对象，按驱动布局读写（Get 读回同一处）。宿主的放大器是 MetalFX 时域放大（回放时按需建），不要游戏的存储：存储大小回 0
//   （驱动按输出尺寸、预设算的那块是 DLSS 网络自己的显存；火纹拿到 0 后 SetStorage(NULL, 0) 照常初始化）。Initialize 回 1（真机：后端句柄非 0）
enum { DLSS_HOST_MAX_SCALE = 3 };   // MetalFX 时域放大的倍数上限（supportedInputContentMaxScale，M5 Max 实测 3）：GetOptimalSettings 的最小渲染尺寸按它算
NVN_NOPROTO(nvnDlssNetworkBuilderSetDefaults, void, void *b) { nvn2_dlss_builder_defaults(b); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetDevice, void, void *b, NVNdevice *d) { nvn2_putp(b, DLB_DEVICE, d); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetDevice, NVNdevice *, const void *b) { return (NVNdevice *)nvn2_getp(b, DLB_DEVICE); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetFlags, void, void *b, uint32_t f) { nvn2_put32(b, DLB_FLAGS, f); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetFlags, uint32_t, const void *b) { return nvn2_get32(b, DLB_FLAGS); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetOutputResolution, void, void *b, int w, int h) { nvn2_put32(b, DLB_OUT_W, (uint32_t)w); nvn2_put32(b, DLB_OUT_H, (uint32_t)h); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetOutputResolution, void, const void *b, int *w, int *h) { if (w) *w = (int)nvn2_get32(b, DLB_OUT_W); if (h) *h = (int)nvn2_get32(b, DLB_OUT_H); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetMethod, void, void *b, int m) { nvn2_put32(b, DLB_METHOD, (uint32_t)m); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetMethod, int, const void *b) { return (int)nvn2_get32(b, DLB_METHOD); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetPreset, void, void *b, int p) { nvn2_put32(b, DLB_PRESET, (uint32_t)p); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetPreset, int, const void *b) { return (int)nvn2_get32(b, DLB_PRESET); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetMaxEvaluationEntries, void, void *b, int n) { nvn2_put32(b, DLB_MAXENT, (uint32_t)n); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetMaxEvaluationEntries, int, const void *b) { return (int)nvn2_get32(b, DLB_MAXENT); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetStorage, void, void *b, void *mem, size_t n) { nvn2_putp(b, DLB_MEM, mem); memcpy((uint8_t *)b + DLB_MEMSZ, &n, 8); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetMemory, void *, const void *b) { return (void *)nvn2_getp(b, DLB_MEM); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetMemorySize, size_t, const void *b) { size_t n; memcpy(&n, (const uint8_t *)b + DLB_MEMSZ, 8); return n; }
NVN_NOPROTO(nvnDlssNetworkBuilderGetStorageSize, size_t, const void *b) { (void)b; return 0; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetTextureIDs, void, void *b, const NVNtexturePool *pool, const int *ids) { nvn2_putp(b, DLB_POOL, pool); if (ids) memcpy((uint8_t *)b + DLB_IDS, ids, 32 * 4); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetTexturePool, const NVNtexturePool *, const void *b) { return (const NVNtexturePool *)nvn2_getp(b, DLB_POOL); }
NVN_NOPROTO(nvnDlssNetworkBuilderGetOptimalSettings, void, const void *b, int *ow, int *oh, int *xw, int *xh, int *nw, int *nh) {
    int o[6]; nvn2_dlss_optimal((int)nvn2_get32(b, DLB_OUT_W), (int)nvn2_get32(b, DLB_OUT_H), (int)nvn2_get32(b, DLB_METHOD), DLSS_HOST_MAX_SCALE, o);
    int *out[6] = {ow, oh, xw, xh, nw, nh}; for (int i = 0; i < 6; i++) if (out[i]) *out[i] = o[i];
    LOG("[nvn] DLSS 最佳渲染尺寸：输出 %ux%u 方法 %u → 最佳 %dx%d、最大 %dx%d、最小 %dx%d（宿主 MetalFX 时域放大，上限 %d 倍）", nvn2_get32(b, DLB_OUT_W), nvn2_get32(b, DLB_OUT_H),
        nvn2_get32(b, DLB_METHOD), o[0], o[1], o[2], o[3], o[4], o[5], DLSS_HOST_MAX_SCALE);
}
NVN_NOPROTO(nvnDlssNetworkInitialize, NVNboolean, void *n, const void *b) {
    memset(n, 0, DLN_SIZE);
    nvn2_putp(n, DLN_DEVICE, nvn2_getp(b, DLB_DEVICE)); nvn2_putp(n, DLN_POOL, nvn2_getp(b, DLB_POOL)); memcpy((uint8_t *)n + DLN_IDS, (const uint8_t *)b + DLB_IDS, 32 * 4);
    nvn2_putp(n, DLN_HANDLE, n); nvn2_put32(n, DLN_MAXENT, nvn2_get32(b, DLB_MAXENT)); nvn2_put32(n, DLN_METHOD, nvn2_get32(b, DLB_METHOD)); nvn2_put32(n, DLN_PRESET, nvn2_get32(b, DLB_PRESET));
    nvn2_put32(n, DLN_OUT_W, nvn2_get32(b, DLB_OUT_W)); nvn2_put32(n, DLN_OUT_H, nvn2_get32(b, DLB_OUT_H)); nvn2_put32(n, DLN_FLAGS, nvn2_get32(b, DLB_FLAGS));
    nvn2_put32(n, DLN_MAX_W, nvn2_get32(b, DLB_OUT_W)); nvn2_put32(n, DLN_MAX_H, nvn2_get32(b, DLB_OUT_H));   // Resize 的上限（nvnDlssNetworkResize）
    LOG("[nvn] DLSS 网络 %p：输出 %ux%u 方法 %u 预设 %u 标志 %#x 条目 %u（宿主按 MetalFX 时域放大回放）", n, nvn2_get32(b, DLB_OUT_W), nvn2_get32(b, DLB_OUT_H), nvn2_get32(b, DLB_METHOD),
        nvn2_get32(b, DLB_PRESET), nvn2_get32(b, DLB_FLAGS), nvn2_get32(b, DLB_MAXENT));
    return 1;
}
NVN_NOPROTO(nvnDlssNetworkFinalize, void, void *n) { memset(n, 0, DLN_SIZE); }
NVN_NOPROTO(nvnDlssParametersSetDefaults, void, void *p) { nvn2_dlss_params_defaults(p); }
// 贴图对（贴图, 视图）、整数对、浮点对：按 nvn2_cmd.h 的偏移存取
NVN_NOPROTO(nvnDlssParametersSetInputColor, void, void *p, const NVNtexture *t, const NVNtextureView *v) { nvn2_putp(p, DLP_COLOR, t); nvn2_putp(p, DLP_COLOR + 8, v); }
NVN_NOPROTO(nvnDlssParametersSetOutputColor, void, void *p, const NVNtexture *t, const NVNtextureView *v) { nvn2_putp(p, DLP_OUTPUT, t); nvn2_putp(p, DLP_OUTPUT + 8, v); }
NVN_NOPROTO(nvnDlssParametersSetMotionVectors, void, void *p, const NVNtexture *t, const NVNtextureView *v) { nvn2_putp(p, DLP_MOTION, t); nvn2_putp(p, DLP_MOTION + 8, v); }
NVN_NOPROTO(nvnDlssParametersSetDepth, void, void *p, const NVNtexture *t, const NVNtextureView *v) { nvn2_putp(p, DLP_DEPTH, t); nvn2_putp(p, DLP_DEPTH + 8, v); }
NVN_NOPROTO(nvnDlssParametersSetBiasCurrentColorMask, void, void *p, const NVNtexture *t, const NVNtextureView *v) { nvn2_putp(p, DLP_BIAS, t); nvn2_putp(p, DLP_BIAS + 8, v); }
NVN_NOPROTO(nvnDlssParametersSetExposureTexture, void, void *p, const NVNtexture *t, const NVNtextureView *v) { nvn2_putp(p, DLP_EXPOSURE, t); nvn2_putp(p, DLP_EXPOSURE + 8, v); }
NVN_NOPROTO(nvnDlssParametersSetInputSubrectangleSize, void, void *p, int x, int y) { nvn2_put32(p, DLP_INSIZE, (uint32_t)x); nvn2_put32(p, DLP_INSIZE + 4, (uint32_t)y); }
NVN_NOPROTO(nvnDlssParametersGetInputSubrectangleSize, void, const void *p, int *x, int *y) { if (x) *x = (int)nvn2_get32(p, DLP_INSIZE); if (y) *y = (int)nvn2_get32(p, DLP_INSIZE + 4); }
NVN_NOPROTO(nvnDlssParametersSetInputColorSubrectangleBase, void, void *p, int x, int y) { nvn2_put32(p, DLP_BASE_COLOR, (uint32_t)x); nvn2_put32(p, DLP_BASE_COLOR + 4, (uint32_t)y); }
NVN_NOPROTO(nvnDlssParametersGetInputColorSubrectangleBase, void, const void *p, int *x, int *y) { if (x) *x = (int)nvn2_get32(p, DLP_BASE_COLOR); if (y) *y = (int)nvn2_get32(p, DLP_BASE_COLOR + 4); }
NVN_NOPROTO(nvnDlssParametersSetDepthSubrectangleBase, void, void *p, int x, int y) { nvn2_put32(p, DLP_BASE_DEPTH, (uint32_t)x); nvn2_put32(p, DLP_BASE_DEPTH + 4, (uint32_t)y); }
NVN_NOPROTO(nvnDlssParametersGetDepthSubrectangleBase, void, const void *p, int *x, int *y) { if (x) *x = (int)nvn2_get32(p, DLP_BASE_DEPTH); if (y) *y = (int)nvn2_get32(p, DLP_BASE_DEPTH + 4); }
NVN_NOPROTO(nvnDlssParametersSetMotionVectorSubrectangleBase, void, void *p, int x, int y) { nvn2_put32(p, DLP_BASE_MOTION, (uint32_t)x); nvn2_put32(p, DLP_BASE_MOTION + 4, (uint32_t)y); }
NVN_NOPROTO(nvnDlssParametersGetMotionVectorSubrectangleBase, void, const void *p, int *x, int *y) { if (x) *x = (int)nvn2_get32(p, DLP_BASE_MOTION); if (y) *y = (int)nvn2_get32(p, DLP_BASE_MOTION + 4); }
NVN_NOPROTO(nvnDlssParametersSetBiasCurrentColorSubrectangleBase, void, void *p, int x, int y) { nvn2_put32(p, DLP_BASE_BIAS, (uint32_t)x); nvn2_put32(p, DLP_BASE_BIAS + 4, (uint32_t)y); }
NVN_NOPROTO(nvnDlssParametersGetBiasCurrentColorSubrectangleBase, void, const void *p, int *x, int *y) { if (x) *x = (int)nvn2_get32(p, DLP_BASE_BIAS); if (y) *y = (int)nvn2_get32(p, DLP_BASE_BIAS + 4); }
NVN_NOPROTO(nvnDlssParametersSetOutputSubrectangleBase, void, void *p, int x, int y) { nvn2_put32(p, DLP_BASE_OUTPUT, (uint32_t)x); nvn2_put32(p, DLP_BASE_OUTPUT + 4, (uint32_t)y); }
NVN_NOPROTO(nvnDlssParametersGetOutputSubrectangleBase, void, const void *p, int *x, int *y) { if (x) *x = (int)nvn2_get32(p, DLP_BASE_OUTPUT); if (y) *y = (int)nvn2_get32(p, DLP_BASE_OUTPUT + 4); }
NVN_NOPROTO(nvnDlssParametersSetJitterOffset, void, void *p, float x, float y) { nvn2_putf(p, DLP_JITTER, x); nvn2_putf(p, DLP_JITTER + 4, y); }
NVN_NOPROTO(nvnDlssParametersGetJitterOffset, void, const void *p, float *x, float *y) { if (x) *x = nvn2_getf(p, DLP_JITTER); if (y) *y = nvn2_getf(p, DLP_JITTER + 4); }
NVN_NOPROTO(nvnDlssParametersSetMotionVectorScale, void, void *p, float x, float y) { nvn2_putf(p, DLP_MVSCALE, x); nvn2_putf(p, DLP_MVSCALE + 4, y); }
NVN_NOPROTO(nvnDlssParametersGetMotionVectorScale, void, const void *p, float *x, float *y) { if (x) *x = nvn2_getf(p, DLP_MVSCALE); if (y) *y = nvn2_getf(p, DLP_MVSCALE + 4); }
NVN_NOPROTO(nvnDlssParametersSetPreExposure, void, void *p, float v) { nvn2_putf(p, DLP_PREEXP, v); }
NVN_NOPROTO(nvnDlssParametersGetPreExposure, float, const void *p) { return nvn2_getf(p, DLP_PREEXP); }
NVN_NOPROTO(nvnDlssParametersSetHistoryReset, void, void *p, uint32_t r) { ((uint8_t *)p)[DLP_RESET] = (uint8_t)B(r); }
NVN_NOPROTO(nvnDlssParametersGetHistoryReset, NVNboolean, const void *p) { return ((const uint8_t *)p)[DLP_RESET]; }
NVN_NOPROTO(nvnDlssParametersSetEvaluationIndex, void, void *p, int i) { nvn2_put32(p, DLP_INDEX, (uint32_t)i); }
NVN_NOPROTO(nvnDlssParametersGetEvaluationIndex, int, const void *p) { return (int)nvn2_get32(p, DLP_INDEX); }
// 叠加调试四项（DLSS 自己画的调试层：调试层 / 全屏 / 标 NaN / 抖动）：宿主的放大器没有这些层，只记值
NVN_NOPROTO(nvnDlssParametersSetOverlayDebugLayer, void, void *p, int v) { nvn2_put32(p, DLP_OVERLAY, (uint32_t)v); }
NVN_NOPROTO(nvnDlssParametersSetOverlayFullScreen, void, void *p, int v) { nvn2_put32(p, DLP_OVERLAY + 4, (uint32_t)v); }
NVN_NOPROTO(nvnDlssParametersSetOverlayShowNans, void, void *p, int v) { nvn2_put32(p, DLP_OVERLAY + 8, (uint32_t)v); }
NVN_NOPROTO(nvnDlssParametersSetOverlayJitterDebug, void, void *p, int v) { nvn2_put32(p, DLP_OVERLAY + 12, (uint32_t)v); }
// 求值：录进命令缓冲（真机同样是录制命令、不是设备调用），参数与六张贴图此刻取定
NVN_NOPROTO(nvnCommandBufferEvaluateDlss, void, NVNcommandBuffer *c, const void *net, const void *par) {
    p_dlss *o = REC_SNAP(c, OP_DLSS, p_dlss, DLT_N);
    for (int k = 0; k < DLT_N; k++) {
        o->tex[k] = tex_snap(o, SNAP_OFF(p_dlss), k, (const NVNtexture *)nvn2_getp(par, nvn2_dlss_tex_at[k]), (uint64_t)__builtin_return_address(0));
        copy_view(&o->view[k], &o->has_view[k], (const NVNtextureView *)nvn2_getp(par, nvn2_dlss_tex_at[k] + 8));
    }
    o->reset = ((const uint8_t *)par)[DLP_RESET];
    o->jitter[0] = nvn2_getf(par, DLP_JITTER); o->jitter[1] = nvn2_getf(par, DLP_JITTER + 4);
    o->mv_scale[0] = nvn2_getf(par, DLP_MVSCALE); o->mv_scale[1] = nvn2_getf(par, DLP_MVSCALE + 4); o->pre_exposure = nvn2_getf(par, DLP_PREEXP);
    o->in_w = (int32_t)nvn2_get32(par, DLP_INSIZE); o->in_h = (int32_t)nvn2_get32(par, DLP_INSIZE + 4);
    for (int k = 0; k < 5; k++) { o->base[k][0] = (int32_t)nvn2_get32(par, DLP_BASE_COLOR + 8 * k); o->base[k][1] = (int32_t)nvn2_get32(par, DLP_BASE_COLOR + 8 * k + 4); }
    o->out_w = (int32_t)nvn2_get32(net, DLN_OUT_W); o->out_h = (int32_t)nvn2_get32(net, DLN_OUT_H); o->flags = nvn2_get32(net, DLN_FLAGS); o->method = (int32_t)nvn2_get32(net, DLN_METHOD);
    o->index = (int32_t)nvn2_get32(par, DLP_INDEX); o->network = (uint64_t)(uintptr_t)net;
}
// 20.5 0x4d4510：参数是一个结构体：+0 必须是 1（接口版本）、+8 是操作号 0..6，否则 +4 写 1（失败）返回；合法时做 CUDA 侧的池 / 同步互操作。
//   本层没有 CUDA：所有操作都按「不支持」在 +4 写 1，首次报红
NVN_NOPROTO(nvnCUDAInteropFunction, void, void *args) {
    uint32_t ver = 0, op = 0, fail = 1; if (args) { memcpy(&ver, args, 4); memcpy(&op, (uint8_t *)args + 8, 4); memcpy((uint8_t *)args + 4, &fail, 4); }
    static int once; if (!once++) LOG("【红】 nvnCUDAInteropFunction（版本 %u 操作 %u）：本层没有 CUDA，回失败", ver, op);
}
// 拦截一族（20.5 才有；6.4 / 10.9 没有）：让别的层（调试 / 中间件）接管 NVN 分派。真机流程：InitializeNVX(表) 引用计数 +1、把驱动当前的 0x10b0 字节函数表拷进调用方的表、回 1（0x4c7590）；
//   SetProcAddressNVX(表, 名, 函数, 原函数出参) 名字不以 nvn 开头或查不到回 0，否则写进表、出参给驱动自己的实现、回 1（0x4c7610）；TryAcquireNVX(表) 没人占用（或就是它）时把表里非空项
//   装进全局分派、回 1，被别人占着回 0（0x4c7750）；OwnedNVX 比较占用者（0x4c7910）；ReleaseNVX 是占用者才清掉并恢复原表、回 1，否则 0（0x4c7930）；FinalizeNVX 引用计数 -1、回 1（0x4c79a0）；
//   SetDispatchResetCallbackNVX(表, 回调, 旧回调出参) 存回调、回 1（0x4c7710）。
//   本层按「不让别的层接管分派」实现（11-03 派活要求「没有拦截者」）：TryAcquire 回 0（真机在没人占用时回 1——这是本层与真机唯一的不同，首次报红）；于是 Owned / Release 恒 0；
//   Initialize 回 1 但不写调用方的表（表的大小随 SDK 变，本层的名字表又比它长，写不得）；SetProcAddress 对认得的名字回 1、原函数出参给本层的实现、表不写；
//   回调照存照读（本层不会重置分派，所以从不调用它）
static void *g_icpt_reset_cb;
NVN_NOPROTO(nvnInterceptionInitializeNVX, NVNboolean, void *table) {
    static int once; if (!once++) LOG("[nvn] InterceptionInitializeNVX(%p)：本层不让别的层接管分派，调用方的表不写", table);
    return 1;
}
NVN_NOPROTO(nvnInterceptionSetProcAddressNVX, NVNboolean, void *table, const char *name, void *fn, void **orig) {
    (void)table; (void)fn;
    if (!name || strncmp(name, "nvn", 3)) return 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) { if (orig) *orig = nvn_lookup(name); return 1; }
    return 0;
}
NVN_NOPROTO(nvnInterceptionSetDispatchResetCallbackNVX, NVNboolean, void *table, void *cb, void **old) {
    (void)table; if (old) *old = g_icpt_reset_cb; g_icpt_reset_cb = cb; return 1;
}
NVN_NOPROTO(nvnInterceptionTryAcquireNVX, NVNboolean, void *table) {
    static int once; if (!once++) LOG("【红】 InterceptionTryAcquireNVX(%p)：本层不让别的层接管 NVN 分派，回 0（真机没人占用时回 1）", table);
    return 0;
}
NVN_NOPROTO(nvnInterceptionOwnedNVX, NVNboolean, const void *table) { (void)table; return 0; }
NVN_NOPROTO(nvnInterceptionReleaseNVX, NVNboolean, void *table) { (void)table; return 0; }
NVN_NOPROTO(nvnInterceptionFinalizeNVX, NVNboolean, void) { return 1; }
// 两个不带 nvn 前缀的私有入口（同样在 0x4c6f60 的 strcmp 链上，按名取得到）：
// H3ikr9ix（6.4 / 10.9 / 20.5 都有，20.5 0x4c73b0）：在 533 项名字表里找 "TexturePoolRegisterTexture"，把驱动内部表（0xb82608）的这一项换成参数给的函数——即替换驱动自己的
//   纹理注册实现。本层不让外来代码替换自己的实现：不换，首次报红（参数个数从反汇编定：一个函数指针，无返回值）
NVN_NOPROTO(H3ikr9ix, void, void *fn) {
    static int once; if (!once++) LOG("【红】 NVN 私有入口 H3ikr9ix(%p)：真机用它替换驱动的 TexturePoolRegisterTexture 实现，本层不替换", fn);
}
// yK1NUEJH（只有 20.5，0x4c7450）：(位置, 偏移, 源, 字节数)。位置是 {池指针, 池内偏移} 两个 8 字节；按池的 CPU 映射（同 nvnMemoryPoolMap 0x4c8f80 的判法）
//   在 映射 + 池内偏移 + 偏移 处按 4 字节为单位写入 字节数/4 个字，池带 CPU 缓存标志（+0x10 第 2 位）时再刷新。本层照做：写进池存储、按 MemoryPoolFlushMappedRange 发布
NVN_NOPROTO(yK1NUEJH, void, const void *loc, ptrdiff_t off, const void *src, size_t n) {
    const pool_t *p; int64_t base; memcpy(&p, loc, 8); memcpy(&base, (const uint8_t *)loc + 8, 8);
    size_t words = (n >> 2) << 2;
    if (!p || !p->mem || !words) return;
    memcpy(p->mem + base + off, src, words); ft_flush(p->mem + base + off, words);
}
static void vsh_sync(void);   // virtual pool section (copies of pages that cannot be aliased)
// 每段回放之前（nvn_mtl.m nvn_xqueue.inc xq_piece）：一次提交可能分几段回放，停住的队列也可能在别的队列的提交里接着回放，所以按「这一段是哪条队列的」设
void nvn_hle_queue_enter(void *q) {
    dev_follow(((queue_t *)q)->dev);   // 后端约定跟这个队列的设备（见 g_cur_dev）
    s2a_nvn_rast_flip = (int)((queue_t *)q)->rast_flip;   // 绕序跟这条队列（见 nvnQueueInitialize）
    vsh_sync();   // copies of non-aliasable virtual pool pages: what the CPU wrote behind them since (hle_nvn.c vsh_*)
}
void nvn_hle_piece_begun(void) { nvn_ft_poll(); }
static void rt_work_tick(int who);   // 提交 / 编码线程的实时声明（DM_RT_SUBMIT，见 set_time_constraint_comp 之后）
NVN(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *h) {
    gfx_enter();
    rt_work_tick(1);
    if (dm_gfx_session_stopping()) { gfx_leave(); return; }
    pthread_mutex_lock(&g_sub_mu);
    if (dm_gfx_session_stopping()) { pthread_mutex_unlock(&g_sub_mu); gfx_leave(); return; }
    render_thread_qos();
    thsc_lv lv0[2]; thr_counts_lv(lv0);
    uint64_t t0 = now_ns(), c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), i0, y0, s0 = thr_sys_ns(); thr_counts(&i0, &y0);
    ((queue_t *)q)->submits++; g_err_queue = q;
    nvn_frec_submit((const uint64_t *)h, n);
    // 按 NVN 跨队列事件顺序回放（nvn_mtl.m nvn_xqueue.inc）：等别的队列还没回放的信号时，这条队列余下的命令停住，信号回放后再接着回放；每段回放前走 nvn_hle_queue_enter / nvn_hle_piece_begun
    nvn_be_submit(q, (const uint64_t *)h, n);
    pthread_mutex_unlock(&g_sub_mu);
    uint64_t dw = now_ns() - t0;
    atomic_fetch_add(&g_nvn_ns, dw); atomic_fetch_add(&g_sub_wall_ns, dw);
    uint64_t i1, y1; thr_counts(&i1, &y1); uint64_t dc = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0, di = i1 - i0;
    atomic_fetch_add(&g_cum_sub_cyc, y1 - y0); atomic_fetch_add(&g_cum_sub_sys_ns, thr_sys_ns() - s0); atomic_fetch_add(&g_cum_sub_wall_ns, dw);
    atomic_fetch_add(&g_sub_cpu_ns, dc); atomic_fetch_add(&g_cum_sub_cpu_ns, dc);
    atomic_fetch_add(&g_sub_instr, di); atomic_fetch_add(&g_cum_sub_instr, di);
    { thsc_lv lv1[2]; thr_counts_lv(lv1);
      for (int k = 0; k < 2; k++) { atomic_fetch_add(&g_cum_sub_lv_t[k], (lv1[k].ut + lv1[k].st) - (lv0[k].ut + lv0[k].st)); atomic_fetch_add(&g_cum_sub_lv_cyc[k], lv1[k].cyc - lv0[k].cyc); } }
    gfx_leave();
}

// Commands that only affect GPU state / only execute on the GPU: the null device only counts them (the thunk has already counted)
static void nvn_nop(void) {}
static const char *const g_nops[] = {
    "nvnCommandBufferSetDepthBounds",
    "nvnCommandBufferSetShaderScratchMemory",
    "nvnCommandBufferPushDebugGroupDynamic",
    "nvnCommandBufferPopDebugGroup",
    "nvnCommandBufferInsertDebugMarkerDynamic",
    "nvnCommandBufferInsertDebugMarker",   // 13 Sentinels
    "nvnCommandBufferPushDebugGroup",      // Super Mario 3D World + Bowser's Fury (NVN 53)
    "nvnCommandBufferPushDebugGroupStatic",
    "nvnCommandBufferPopDebugGroupId",
    "nvnCommandBufferInsertDebugMarkerStatic",
    "nvnCommandBufferSetStencilCullCriteria",
    "nvnCommandBufferResolveDepthBuffer",
    // NVN2 (Switch 2): the new names Pokopia calls (pokopia-recomp nvn2_abi.md §2, checked there in assembly). Debug groups per engine: (cmdbuf, u32 domain[, const char *]);
    //   the recording mode (cmdbuf, int) tunes how the GPU front end runs the commands, not what they produce (the host replays every command in order) (reference port 8dd0c98a, MIT).
    //   QueueBuilderSetPrimaryQueue used to be here; 11-04 it stores the queue (QueueBuilderGetPrimaryQueue reads it back, 「NVN2 入口全量」段)
    "nvnCommandBufferPushDebugGroupGraphics",
    "nvnCommandBufferPushDebugGroupCompute",
    "nvnCommandBufferPopDebugGroupGraphics",
    "nvnCommandBufferPopDebugGroupCompute",
    "nvnCommandBufferSetRecordingMode",
    // NVN2 计算重叠模式（命令缓冲, int）：控制令牌 0x27，队列支持时才发一条计算方法、数据 = (模式 == 0)，决定后一段计算能否与前面的工作重叠执行（kartnative nvn2.md，MIT）。
    //   宿主回放的计算编码器前后都是队列级全屏障（nvn_mtl.m m4_end / main_barrier_begin），从不重叠：相当于一直按「不重叠」执行，结果与任一模式相同、只少了并行
    "nvnCommandBufferSetComputeOverlapMode",
    // NVN2 几何处理模式（命令缓冲, int）：硬件前端怎么排几何工作的提示，不改结果；宿主按提交顺序回放（金刚每帧每个命令缓冲各调一次，11-04；the reference port 777a5f6c 同样不生效）
    "nvnCommandBufferSetGeometryProcessingMode",
    // 以下 5 个 nvn.h 没有原型。按 void 处理的依据：带原型的 Set / Insert / Override / Bind 函数 194 个里 192 个返回 void，例外只有 Program 的
    //   SetShaders / SetSubroutineLinkage（可失败的程序设置）。都是调试 / 硬件提示，主机没有可观察效果：
    "nvnCommandBufferInsertCheckpointStatic",          //   GPU 崩溃排查的检查点（同 InsertDebugMarker；设备侧的检查点开关见 #354）
    "nvnCommandBufferInsertCheckpointDynamic",
    "nvnCommandBufferOverrideSubtileSize",             //   分块缓存的子块尺寸提示（同 SetTiledCacheTileSize 一类）
    "nvnCommandBufferSetControlMemoryCallbackEnabled", //   同 SetCommandMemoryCallbackEnabled：主机不会耗尽控制内存，不会回调
    // NVN2 入口全量（11-04）：只改硬件排程 / 调试、不改结果的几条（驱动地址 = 火纹 SDK 22 sdk）：
    "nvnCommandBufferSetGraphicsWorkloadPriority",     //   图形工作负载优先级（cmd, int）：写调度方法 + 控制令牌，决定 GPU 内抢占先后；宿主按提交顺序回放
    "nvnCommandBufferSetComputeWorkloadPriority",      //   同上，计算（控制令牌 0x6b）
    "nvnCommandBufferSetResourceOccupancy",            //   (cmd, int 资源, float 比例)：按 SM 数换算后记控制令牌 0x6c，限制一类工作占多少 SM；不改结果
    "nvnCommandBufferInsertDebugMarkerGraphics",       //   (cmd, u32 域, const char *)：按引擎分的调试标记，同 InsertDebugMarker
    "nvnCommandBufferInsertDebugMarkerCompute",
    "nvnCommandBufferSetRepresentativeFragmentTest",   //   代表片元测试（cmd, bool）：只给「片元着色器只做可见性」的绘制用，硬件「可以」丢掉多余片元，不丢也合规（同 Vulkan VK_NV_representative_fragment_test）
    "nvnCommandBufferBindDeviceMessageBuffer",         //   给着色器绑消息缓冲（cmd, 阶段, 槽, 缓冲）：宿主翻译的着色器不发消息，绑了也没人写（消息缓冲见「NVN2 入口全量」段）
    "nvnCommandBufferBindDeviceMessageBufferDeferred",
    "nvnCommandBufferSetZCullZF32CompressionEnable",   //   ZCull 的 F32 深度压缩提示（主机不做 ZCull，同 SetStencilCullCriteria）   //   签名不带目标、只作用于当前深度缓冲（硬件压缩深度的就地解压）；主机深度是不压缩的普通 MTLTexture，无事可做   // S2Arm: the stencil cull (ZCull's stencil half) acceptance test: a hardware early-reject hint, same as the ZCull save / restore above
                                                //   (the host does no ZCull); Diablo III calls it, and as unimplemented it failed the direct run on its first draw pass (10-25 M5 run z_d3)
};
// 带栈上参数的 NVN2 入口（HDR 显示元数据：十个 float / 十个出参指针，第 9、10 个在调用方栈上）：不能换栈（大宿主栈 nvn_big_entry、计时包装 nvn_prof_entry、
//   安卓 xstack 都换了 sp，栈上参数就丢了），只能在游戏线程栈上直接跑。实现本身只读写构建器 / 窗口对象，栈用量很小
static int nvn_stack_args(const char *n) {
    return !strcmp(n, "nvnWindowBuilderSetHdrDisplayMetadata") || !strcmp(n, "nvnWindowBuilderGetHdrDisplayMetadata") ||
           !strcmp(n, "nvnWindowSetHdrDisplayMetadata") || !strcmp(n, "nvnWindowGetHdrDisplayMetadata");
}
#if !defined(__ANDROID__) && !defined(__OHOS__)
// 留在游戏栈上的 NVN 实现（命令录制 nvnCommandBuffer* 等，每帧上万次）套叶子桩（11-17，同 runtime/src/xstack.c 叶子名单）：游戏 sp 不是 16 字节对齐时
//   （Horizon 不查、苹果芯片查），宿主实现开头一压栈就 SIGBUS；叶子桩 sp 对齐直接尾跳，不对齐先对齐、调完恢复。
//   不套叶子桩：未实现桩 / 大栈入口 / 计时入口（靠跳板给的 x16 = 槽地址找实现，叶子桩会改写 x16；未实现桩与计时入口自己在汇编里对齐，
//   大栈入口切栈前不碰 sp、切到对齐的宿主栈）。带栈上参数的四个（挪 sp 会读错参数）改指 nvn_stackarg_entry：对齐时尾跳，不对齐时连栈参一起挪到对齐处再调。
//   host_adapt.m big_wrap 直连起用时调一次；幂等。leaf = sw_xstack_leaf（不链运行时的单测不调）
extern void nvn_prof_entry(void);
extern void nvn_stackarg_entry(void);
void *nvn_sa_real[NVN_NFUNC];   // 带栈参入口的真实现（nvn_entry.S nvn_stackarg_entry 按槽号取）
void nvn_leaf_wrap(void *(*leaf)(const char *, void *)) {
    static int done; if (done || !leaf) return; done = 1;
    unsigned n = 0, sa = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) {
        void *f = nvn_slot[i].fn;
        if (f == (void *)nvn_unimpl_entry || f == (void *)nvn_big_entry || f == (void *)nvn_prof_entry) continue;
        if (nvn_stack_args(nvn_names[i])) { nvn_sa_real[i] = f; nvn_slot[i].fn = (void *)nvn_stackarg_entry; sa++; continue; }
        nvn_slot[i].fn = leaf(nvn_names[i], f); n++;
    }
    LOG("[nvn] 留在游戏栈上的 %u 个实现套叶子桩、%u 个带栈参的经对齐入口（游戏 sp 不对齐时先对齐再调）", n, sa);
}
#endif
__attribute__((constructor)) static void nvn_nops_register(void) {
    // Empty implementations overwrite the unimplemented stubs after the slot table is initialized; a misspelled name (not in main's name table) reports a failure and aborts at startup
    nvn_init_slots();
    for (unsigned k = 0; k < sizeof g_nops / sizeof *g_nops; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], g_nops[k])) i++;
        if (i == NVN_NFUNC) die("NVN empty implementation %s is not in main's name table", g_nops[k]);
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry) die("NVN %s has an implementation and is also listed as an empty implementation", g_nops[k]);
        nvn_slot[i].fn = (void *)nvn_nop;
    }
    unsigned nnop = sizeof g_nops / sizeof *g_nops;
    unsigned nbig = 0;
#if !defined(__ANDROID__) && !defined(__OHOS__)
    // Implemented functions other than command recording (nvnCommandBuffer*, tens of thousands per frame, where the host only appends records) run on the big host stack: submit / present / object creation do Metal work, and a game thread's stack may be too small (see nvn_entry.S nvn_big_entry)
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && nvn_slot[i].fn != (void *)nvn_nop && strncmp(nvn_names[i], "nvnCommandBuffer", 16) && !nvn_stack_args(nvn_names[i])) {
            nvn_big_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_big_entry; nbig++;
        }
    // DM_NVN_PROF=1: implemented functions (except unimplemented stubs and present: present includes the display-link wait and is counted separately) get a timing wrapper, used to break down the time spent in the "translation layer recording phase"
    { const char *q = getenv("S2A_NVN_QLOG"); if (q && *q && getenv("DM_NVN_PROF")) { g_qlog_fd = open(q, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644); g_nvn_qlog = g_qlog_fd >= 0; } }
    if (getenv("DM_NVN_PROF"))
        for (unsigned i = 0; i < NVN_NFUNC; i++)
            if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && strcmp(nvn_names[i], "nvnQueuePresentTexture") && !nvn_stack_args(nvn_names[i])) { nvn_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_prof_entry; }
#else
    // Android：同 Apple「非命令录制实现跑大宿主栈」语义走 xstack 切栈桩（xs_entry 见 sp 不在
    //   宿主栈就切到本线程 pthread 栈再调实现；客机线程槽在 thread_main / guest_main 已登记）。
    //   但本 .so 的链接域里拿不到 sw_xstack_wrap（它在运行时侧），且构造器时机早于运行时注入——
    //   包装挪到 s2a_nvn_set_xwrap：direct_session.c 在选择事务冻结为 direct 时把
    //   sw_xstack_wrap 函数指针注进来。未注入 = 宿主单测/诊断路径，保持裸实现如实可用。
#endif
    LOG("[nvn] null device: %u names, %u implemented (%u semantic + %u empty, %u of them on the large host stack), the other %u go to the red-reporting stub",
        NVN_NFUNC, g_nsem + nnop, g_nsem, nnop, nbig, NVN_NFUNC - g_nsem - nnop);
}

#if defined(__ANDROID__) || defined(__OHOS__)
// Android 大宿主栈注入点：direct_session.c 在选择事务冻结为 direct、把 nvnBootstrapLoader
//   交给客机前调一次（本 .so 链不到 xstack，sw_xstack_wrap 只能以函数指针进来）。
//   幂等：整表只包一次（重包会把桩再喂给包装器叠桩）。未注入时 nvn_lookup 照发裸实现——
//   宿主单测/诊断路径用。
static int g_nvn_xwrapped;
void s2a_nvn_set_xwrap(void *(*w)(const char *, void *)) {
    if (!w || g_nvn_xwrapped) return;
    g_nvn_xwrapped = 1;
    unsigned n = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && nvn_slot[i].fn != (void *)nvn_nop && strncmp(nvn_names[i], "nvnCommandBuffer", 16) && !nvn_stack_args(nvn_names[i])) {
            void *nw = w(nvn_names[i], nvn_slot[i].fn);
            if (nw != nvn_slot[i].fn) { nvn_slot[i].fn = nw; n++; }
        }
    LOG("[nvn] %u non-command-buffer implementations now enter through the host-stack stub", n);
}
#endif

// ---------- Window / present ----------
#define WB(b) ((win_builder_t *)(b))
static void nvn2_win_builder_defaults(NVNwindowBuilder *b);   // 「NVN2 入口全量」段
#define NAT_ITV_OFF 0xf14   // 原生窗口对象里的交换间隔（见 nat_itv）
#define NAT_MAGIC 0x5f776e64u   // 原生窗口对象头「dnw_」
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1;
    if (nvn2_on()) nvn2_win_builder_defaults(b); }   // NVN2 字段在 0x40 之后：只有 Switch 2 的构建器有那么大
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
#if defined(__ANDROID__)
// 安卓平台件 android/native/nvn/guest_window_crop.c 提供（原生窗口裁剪交直连上屏，#1335）。只有安卓有这个实现：
//   苹果的 iOS / Mac 链接不放行弱未定义（iOS 全量编译链不过），所以整段只在安卓编
extern void nvn_plat_guest_window_watch(void *) __attribute__((weak));
#endif
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) {
    WB(b)->native = w;
#if defined(__ANDROID__)
    if (nvn_plat_guest_window_watch) nvn_plat_guest_window_watch(w);
#endif
}
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("【红】 WindowBuilderSetTextures count %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
// 游戏要的交换间隔超出真机上限（nvnDeviceGetInteger MAX_PRESENT_INTERVAL = 5，见上方设备属性表 [0x46]）：当 0（不等垂直同步，出图仍受出图节拍 60 / 120 Hz 封顶）。
//   11-14 一款游戏的公开帧率补丁把 SetPresentInterval 的参数改成 1000（模拟器补丁，作者按「模拟器关垂直同步、间隔被忽略」写的；Ryujinx 无界模式同样当 0，
//   compat/patches/0107）：原先照单收下，每次出图等 1000 个垂直同步，画面停在开头几帧、声音照常（iPhone 实机，帧率开关一关就好）。0–5 照旧
#define NVN_ITV_MAX 5
static int itv_in(int v) {
    if (v >= 0 && v <= NVN_ITV_MAX) return v;
    static int said; if (!said++) LOG("[nvn] 游戏要的交换间隔 %d 超出真机范围 0–%d（多为帧率补丁写的「不等垂直同步」），按 0 处理：不等垂直同步、由出图节拍封顶", v, NVN_ITV_MAX);
    return 0;
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = itv_in(v); }
NVN(nvnWindowBuilderGetDevice, const NVNdevice *, const NVNwindowBuilder *b) { return ((const win_builder_t *)b)->dev; }
NVN(nvnWindowBuilderGetNativeWindow, NVNnativeWindow, const NVNwindowBuilder *b) { return ((const win_builder_t *)b)->native; }
NVN(nvnWindowBuilderGetPresentInterval, int, const NVNwindowBuilder *b) { return ((const win_builder_t *)b)->interval; }
NVN(nvnWindowBuilderGetNumTextures, int, const NVNwindowBuilder *b) { return ((const win_builder_t *)b)->ntex; }
NVN(nvnWindowBuilderGetNumActiveTextures, int, const NVNwindowBuilder *b) { return ((const win_builder_t *)b)->nactive; }
NVN(nvnWindowBuilderGetTexture, NVNtexture *, const NVNwindowBuilder *b, int i) {
    const win_builder_t *o = (const win_builder_t *)b;
    return i >= 0 && i < o->ntex ? o->tex[i] : NULL;
}
static void nvn2_win_init(NVNwindow *w, const NVNwindowBuilder *b);
static void acq_lease_reset(const win_t *o);
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); acq_lease_reset(o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    o->nactive = s->nactive > 0 ? (s->nactive < s->ntex ? s->nactive : s->ntex) : s->ntex;   // 未显式设过就按全部纹理可用
    memcpy(o->tex, s->tex, sizeof o->tex);
    // SDK initializes the crop to the full first window texture (see docs/window_queries.md).
    if (o->ntex > 0 && o->tex[0]) { o->crop[2] = (int)CTX(o->tex[0])->d.width; o->crop[3] = (int)CTX(o->tex[0])->d.height; }
    for (int i = 0; i < o->ntex; i++) if (o->tex[i]) nvn_be_xv_window_tex(CTX(o->tex[i])->serial);
    // 上屏翻转按建窗口那一刻的原点定（真机驱动：SetWindowOriginMode 只存设备字段，绘制随它变；窗口图像的行序在建窗口时定下）。先建窗口、后设左上角的游戏
    //   （01002EF01A316000：窗口建在缺省左下角之后才设左上）绘制按左上、上屏仍翻；原先上屏跟当前原点走，整幅画面上下颠倒（诊断里 342 款只有它先建窗口后改原点）
    // 上屏翻转按窗口建时的原点（土豆兄弟：先建窗口、后改原点）：取当前设备的原点。不读构建器里的设备指针——SetDefaults 会留下构建器内存里原有的值，
    //   没调过 SetDevice 的构建器那里可能是野指针（t_nvn2_procs 的窗口构建器，dev_ok 读它的魔数段错误，main 红 8dd24685f）；NVN 游戏只有一个设备
    { const NVNdevice *wd = atomic_load(&g_cur_dev); o->present_ll = !dev_ok(wd) || ((const dev_t_ *)wd)->origin != NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT; }
    if (nvn2_on()) nvn2_win_init(w, b);   // NVN2：获取超时、色彩空间、HDR 元数据从构建器带过来（「NVN2 入口全量」段）
    // 先认对象头标记「dnw_」（系统原生窗口对象开头 4 字节，11-13 一款游戏实测），认得出再读 +0xF14：单测 / 别的写法给的原生窗口指针不碰
    int dnw = o->native && *(const volatile uint32_t *)o->native == NAT_MAGIC, nat0 = dnw ? *(volatile int32_t *)((char *)o->native + NAT_ITV_OFF) : -1;
    o->nat = nat0 == 1; if (o->nat) *(volatile int32_t *)((char *)o->native + NAT_ITV_OFF) = o->interval;   // 同真机驱动：建窗口时把间隔写进原生窗口（见 nat_itv）
    LOG("[nvn] WindowInitialize %p native %p（%s交换间隔同步%s，原值 %d） swap chain %d textures, interval %d, first %dx%d format %#x", (void *)w, o->native, dnw ? "" : "对象头不认、", o->nat ? "开" : "关", nat0, o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowFinalize, void, NVNwindow *w) { acq_lease_reset((const win_t *)w); }
NVN(nvnWindowGetNativeWindow, NVNnativeWindow, const NVNwindow *w) { return ((const win_t *)w)->native; }
// 原生窗口的交换间隔（11-13）：真机驱动 nvnWindowSetPresentInterval / 建窗口时把间隔写进系统原生窗口对象（构建器 SetNativeWindow 给的那个，+0xF14，
//   sdk 里的显示层代码就读写这一处），出图时按它走。直连层不走系统显示层，原先只记在自己的 win_t 里：公开库常见的帧率金手指（指针链落到原生窗口 +0xF14 写 1）
//   写成功却不生效（一款游戏开着仍 30 帧）。这里照驱动的做法双向同步：设间隔时写进去，每次出图读回来，被别人改了（金手指）就跟着改。
//   只在建窗口时这一处是系统缺省值 1 时才开（别的 SDK 版本这里若不是交换间隔，不写它）
// 游戏改交换间隔的时刻（now_ns，与回放核心上屏用的同一单调时钟）：上屏时随间隔一起交给后端（nvn_be_present_target），「分时」按它分段
static _Atomic uint64_t g_itv_set_ns;
static volatile int32_t *nat_itv(const win_t *o) { return o->nat && o->native ? (volatile int32_t *)((char *)o->native + NAT_ITV_OFF) : NULL; }
static void nat_itv_sync(win_t *o) {
    volatile int32_t *p = nat_itv(o); if (!p) return;
    int v = itv_in(*p);   // 真机范围 0–5（原先只认 0–4，5 合法却被丢）；超界同 itv_in 按 0
    if (v != o->interval) {
        const uint64_t t = now_ns(); atomic_store(&g_itv_set_ns, t);
        LOG("[nvn] 原生窗口交换间隔被改成 %d（游戏设的 %d；金手指等直接写系统窗口），出图跟它（单调时钟 %.3f 秒，这个窗口已上屏 %llu 帧）", v, o->interval, (double)t / 1e9, (unsigned long long)o->presents);
        o->interval = v;
    }
}
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) { nat_itv_sync((win_t *)w); return ((const win_t *)w)->interval; }   // 原生窗口被改过（金手指）先读回来，同驱动   // Link's Awakening reads it back every frame
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) {
    win_t *o = (win_t *)w; v = itv_in(v);
    if (o->interval != v) {
        const uint64_t t = now_ns(); atomic_store(&g_itv_set_ns, t);
        LOG("[nvn] the game sets present interval %d → %d（单调时钟 %.3f 秒，这个窗口已上屏 %llu 帧）", o->interval, v, (double)t / 1e9, (unsigned long long)o->presents);
    }
    o->interval = v; volatile int32_t *p = nat_itv(o); if (p) *p = v;
}
// 显示层裁切（nn::vi::SetLayerCrop，说明见 graphics/nvn/host_adapt.m s2a_vi_entry）：上屏取窗口贴图的区域。nvnWindowSetCrop 写的是同一个系统窗口裁切，谁最后写算谁
static pthread_mutex_t g_vi_mu = PTHREAD_MUTEX_INITIALIZER; static int g_vi_crop[4];
int32_t h_vi_set_layer_crop(void *layer, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)layer; int v[4] = {x, y, w, h}; if (w <= 0 || h <= 0 || x < 0 || y < 0) v[0] = v[1] = v[2] = v[3] = 0;   // 宽或高为 0：SDK 传 NULL = 清空（整张）
    pthread_mutex_lock(&g_vi_mu); int ch = memcmp(g_vi_crop, v, sizeof v); memcpy(g_vi_crop, v, sizeof v); pthread_mutex_unlock(&g_vi_mu);
    if (ch) LOG("[nvn] 游戏设显示层裁切 %d,%d %dx%d%s", v[0], v[1], v[2], v[3], v[2] ? "（上屏按它取窗口贴图的区域）" : "（清空：上屏整张）");
    return 0;
}
void dm_vi_layer_crop(int c[4]) { pthread_mutex_lock(&g_vi_mu); memcpy(c, g_vi_crop, sizeof g_vi_crop); pthread_mutex_unlock(&g_vi_mu); }
static void dm_vi_layer_crop_clear(void) { pthread_mutex_lock(&g_vi_mu); memset(g_vi_crop, 0, sizeof g_vi_crop); pthread_mutex_unlock(&g_vi_mu); }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w;
    // A zero dimension restores the full texture, including x/y, rather than storing a zero rectangle.
    if ((!ww || !hh) && o->ntex > 0 && o->tex[0]) { x = y = 0; ww = (int)CTX(o->tex[0])->d.width; hh = (int)CTX(o->tex[0])->d.height; }
    int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
    dm_vi_layer_crop_clear();   // 真机 SDK 把它推到系统窗口，盖掉之前 nn::vi::SetLayerCrop 设的（host_adapt.m：谁最后写算谁）
}
NVN(nvnWindowGetCrop, void, const NVNwindow *w, NVNrectangle *crop) {
    const win_t *o = (const win_t *)w;
    crop->x = o->crop[0]; crop->y = o->crop[1];
    crop->width = o->crop[2]; crop->height = o->crop[3];
}
static void trow(uint64_t f, const char *name, double v);
static void set_time_constraint(uint64_t period_ns);
static void rt_comp_tick(uint64_t period_ns);
static double g_rt_comp_ms;   // Currently declared real-time computation amount (ms; 0 = not declared)
static double g_rt_period_ms;
static _Atomic mach_port_t g_rt_port;   // the acquire thread that holds the time-constraint declaration (set_time_constraint_comp); the 300-frame status line reads it
static int g_rt_off;   // 本局放弃了实时声明（被系统降级两次，rt_demote.h）：之后不再声明，取图线程走标准策略 + USER_INTERACTIVE
static void sleep_until_ns(uint64_t dl);
// 只读计数（单测按事件判、不量墙钟，t_fps_limit）：本线程进入节拍等待的次数——sleep_until_ns 真睡了，或取图里去等显示链接拍。
//   单测在上屏前后各读一次，判「上屏只入队、不承担限帧等待」；只增不减，不改行为
static _Thread_local uint64_t t_pace_waits;
uint64_t s2a_nvn_pace_waits_self(void) { return t_pace_waits; }
// 只读计数（同上，t_fps_limit）：取图里节拍器做过决定的次数、最近一次用的节拍周期（ns，已含用户帧率上限 nvn_fps_period_ns）。只增 / 只记最近，不改行为
static _Atomic uint64_t g_pace_dec_n, g_pace_dec_period;
void s2a_nvn_pace_stats(uint64_t *n, uint64_t *period_ns) { *n = atomic_load_explicit(&g_pace_dec_n, memory_order_relaxed); *period_ns = atomic_load_explicit(&g_pace_dec_period, memory_order_relaxed); }
static uint64_t present_period(const win_t *o);
static uint64_t rt_period_ns(const win_t *o, uint64_t period);
static int present_itv(const win_t *o);
static int link_vsyncs(const win_t *o);
__attribute__((weak)) void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = 0; *ns = 0; }   // The host's hle_thread.c provides the ground truth; 0 in graphics unit tests
__attribute__((weak)) uint32_t dm_guest_work_qos(unsigned *count) { if (count) *count = 0; return QOS_CLASS_UNSPECIFIED; }
__attribute__((weak)) void dm_thread_lv_stats(uint64_t out[2][2][2]) { memset(out, 0, 8 * sizeof(uint64_t)); }   // nvn_rec.h：编码 / 上屏线程按性能档；没有 Metal 后端（单测 / 安卓）为 0
__attribute__((weak)) void dm_yield_top(char *out, size_t n) { if (n) snprintf(out, n, "-"); }   // hle_thread.c: the threads that yielded most since the last call; none in graphics unit tests
__attribute__((weak)) void dm_thread_cpu(char *out, size_t n, double secs) { (void)secs; if (n) snprintf(out, n, "-"); }   // hle_thread.c: guest-thread CPU by Switch priority band since the last call
void nvn_be_res_stats(char *out, size_t n);
void nvn_be_barrier_stats(uint64_t *skip, uint64_t *full);
extern uint32_t nvn_be_present_qos(void);
static uint64_t g_frame;
static uint64_t g_acq_wait_ns, g_pq_wait_ns;   // Accumulated display-link wait / wait for the present queue to return inside acquire, recorded per frame at present (there can be several acquires within one frame)
static uint64_t g_fs_start, g_fs_wait;   // frame step (fstep_tick): when the first acquire after a present returned (the frame's start, 0 = none yet), and its pacing wait
uint64_t dm_nvn_frame_count(void) { return g_frame; }
extern uint64_t g_t0_ns;   // Host process start point (main.c; same base as the UIMSG timestamps)
// The thread that presented last owns the frame pacing below. Another thread acquiring a swapchain texture (a loader drawing one frame of its
// own) does not sleep out the pacer: it only waits until the present thread has processed this texture's previous present. 10-31 TMNT (0100BB8017F24000):
// the game's "acquire if this frame has none yet" (main+0xfabf0, flag at device+0x18) is not atomic; the main thread sits ~16 ms inside acquire
// with the flag still unset, the video-context loader sees it unset and acquires too, and slept 32-37 ms here; meanwhile the main thread presented
// and reset the shared render-target set, so the loader read a null target at main+0xfb124 (4/4 before, 0/3 after; timeline in docs/坑记录.md)
// 11-02 牧场物语（0100C2D00EBD4000）：游戏主线程取图、渲染线程上屏，原先「最近上屏的线程」当节拍主人——取图线程从来不是它，全程不睡节拍，
//   标题页 140–200 帧 / 秒、按 A 菜单出不来（约 2 帧 / 秒时一按就出；3000 次取图 2999 次不在上屏线程）。节拍主人改为「出帧的取图线程」：
//   上屏时看被上屏的那张是哪个线程取的（按图号记在 g_acq_thr），连续 PACE_SWITCH 次上屏都来自同一个取图线程才换主人——
//   TMNT 加载线程偶尔取一张不会当上主人（保留上面 10-31 的修复），取图与上屏分在两个线程的引擎在取图线程上睡节拍（同真机：取图阻塞到有图放出）
enum { PACE_SWITCH = 3 };
static _Atomic(uintptr_t) g_present_thr;   // 节拍主人（出帧的取图线程；上屏线程同时就是取图线程时即它）
static _Atomic(uintptr_t) g_acq_thr[4];    // 交换链各图号最近一次的取图线程
static uintptr_t g_pace_cand; static int g_pace_run;   // 上屏侧：候选主人与它连续出帧的次数（只在上屏线程里改）
// #141：取图撞上还在屏上的那张时等下一次呈现翻走（present 存 shown 后广播）。取图线程等的是别的线程的呈现；
//   同线程撞图或等翻走超时后，优先认领另一张未被租用的活跃图；公共尾部仍按最终下标等它的 ticket / avail，返回时那张图才真正归还。
//   租约放在窗口旁挂表，不改游戏内 win_t 布局；与 shown / 翻走广播共用 g_flip_mu，候选扫描与呈交换图原子串行。
static pthread_mutex_t g_flip_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_flip_cv = PTHREAD_COND_INITIALIZER;
typedef struct { const void *w; uint32_t leased; } acq_lease_t;
static acq_lease_t g_acq_lease[64];
static int acq_lease_slot_locked(const win_t *o) {
    unsigned at = (unsigned)(((uintptr_t)o >> 4) & 63);
    for (unsigned n = 0; n < 64; n++) {
        acq_lease_t *e = &g_acq_lease[(at + n) & 63];
        if (e->w == o) return (int)((at + n) & 63);
        if (!e->w) { e->w = o; return (int)((at + n) & 63); }
    }
    return -1;
}
static void acq_lease_reset(const win_t *o) {
    pthread_mutex_lock(&g_flip_mu);
    for (int i = 0; i < 64; i++) if (g_acq_lease[i].w == o) { g_acq_lease[i].w = NULL; g_acq_lease[i].leased = 0; }
    pthread_mutex_unlock(&g_flip_mu);
}
static void acq_lease_force(const win_t *o, int idx) {
    if (idx < 0 || idx >= 4) return;
    pthread_mutex_lock(&g_flip_mu); int slot = acq_lease_slot_locked(o);
    if (slot >= 0) g_acq_lease[slot].leased |= 1u << idx;
    pthread_mutex_unlock(&g_flip_mu);
}
static void acq_lease_release_locked(const win_t *o, int idx) {
    if (idx < 0 || idx >= 4) return;
    int slot = acq_lease_slot_locked(o); if (slot >= 0) g_acq_lease[slot].leased &= ~(1u << idx);
}
static int acquire_claim_idx(win_t *o, int preferred, int visible, int nact) {
    int pick = -1;
    pthread_mutex_lock(&g_flip_mu); int slot = acq_lease_slot_locked(o);
    if (slot >= 0) {
        uint32_t *leased = &g_acq_lease[slot].leased;
        if (preferred >= 0 && preferred < nact && preferred != visible && !(*leased & (1u << preferred))) pick = preferred;
        for (int pass = 0; pick < 0 && pass < 2; pass++) {
            for (int i = 0; i < nact; i++) {
                if (i == visible || i == preferred || (*leased & (1u << i))) continue;
                if (!pass && o->ticket[i]) continue;
                pick = i; *leased |= 1u << i; break;
            }
        }
        if (pick >= 0) *leased |= 1u << pick;
        if (pick >= 0 && pick != preferred) {
            static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
            if (k <= 20 || !(k & (k - 1))) LOG("[nvn] WindowAcquireTexture 撞上已占用 / 在屏图 %d：改交已归还的活跃图 %d，保持 SUCCESS，累计 %llu 次", preferred, pick, (unsigned long long)k);
        }
    }
    pthread_mutex_unlock(&g_flip_mu);
    return pick;
}
// 只读计数（单测按事件判、不量墙钟，t_nvn_win_shown）：[0] 进入 #141 翻走等待的次数，[1] 借帧线程立即交出的次数，[2] 没等就交出还在屏上那张的次数。只增不减，不改行为
static _Atomic uint64_t g_acq_cnt[3];
void s2a_nvn_acq_counts(uint64_t c[3]) { for (int i = 0; i < 3; i++) c[i] = atomic_load_explicit(&g_acq_cnt[i], memory_order_relaxed); }
static void acquire_visible_legacy(win_t *o, int idx, int nact, const char *why) {
    atomic_fetch_add_explicit(&g_acq_cnt[2], 1, memory_order_relaxed);
    static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k <= 20 || !(k & (k - 1))) LOG("【红】 [nvn] WindowAcquireTexture 交出了还在屏上的那张：图 %d 是最近一次呈现的、还没被下一次呈现翻走（真机不会交出；原因：%s），累计 %llu 次；活跃 %d / %d 张、间隔 %d、已呈现 %llu 次",
                                       idx, why, (unsigned long long)k, nact, o->ntex, o->interval, (unsigned long long)o->presents);
}
// 审计 #1534 P1-24：按活跃张数轮换（NVN 语义：SetNumActiveTextures 之后只轮换前 nactive 张，未设时 = 全部）。11-04 普查 485 份日志里超级马力欧兄弟 惊奇
//   （010015100B514000）设 2 of 3，原先仍按 3 张轮换、交出被排除的第 3 张。活跃张数为 0（窗口没有纹理，或 SetNumActiveTextures(0)）没有可交的纹理：回 NATIVE_ERROR
// 取图时定、上屏时用的「这一帧是不是显示链路的节拍控的」（nvn_pace_follow）：按窗口对象放一张旁挂小表，不放进 win_t——
//   win_t 在游戏内存里、登记了即时存档的布局指纹（nvn.win），加字段就改了布局。表按窗口地址直接映射，撞了槽或读档后表是空的 = 0（照旧按节拍器走一个周期）
static struct { _Atomic(const void *) w; _Atomic int follow; } g_pace_follow[8];
static inline unsigned pace_follow_slot(const void *w) { return (unsigned)(((uintptr_t)w >> 4) & 7); }
static void pace_follow_set(const win_t *o, int follow) {
    unsigned k = pace_follow_slot(o);
    atomic_store_explicit(&g_pace_follow[k].follow, follow, memory_order_relaxed);
    atomic_store_explicit(&g_pace_follow[k].w, (const void *)o, memory_order_release);
}
static int pace_follow_take(const win_t *o) {   // 用一次就清：两次上屏之间没取图（取图不走这条路的调用方）不沿用上一帧的
    unsigned k = pace_follow_slot(o);
    if (atomic_load_explicit(&g_pace_follow[k].w, memory_order_acquire) != (const void *)o) return 0;
    return atomic_exchange_explicit(&g_pace_follow[k].follow, 0, memory_order_relaxed);
}
// 即时存档恢复后，窗口对象里的节拍时刻（avail[] / pace）从现在重新起算。它们是存档那个进程读的单调钟（开机以来的纳秒），跟着游戏内存进快照、
//   原样回来。同一次开机里恢复，这些时刻已经过去，取图不用等、节拍器也照「落后一个周期以上」重新起拍，清不清结果一样；设备重启后单调钟从零起，
//   它们成了几十小时以后，取图里「睡到可取用时刻」（sleep_until_ns）一睡不醒：游戏画完恢复后的第一帧就停住，声音照常
//   （11-23 iPad：存档时开机 61.1 小时，重启后第 536 秒读档，出帧线程停在 mach_wait_until，截止 = 开机后 219956 秒）。
//   窗口对象没有登记表，读档那一刻找不到它们：读档只抬一个代数，窗口在这一代里第一次取图 / 上屏时清。旁挂小表记「这一代清过的窗口」，
//   理由同 g_pace_follow（win_t 在游戏内存里、登记了即时存档的布局指纹，不加字段）。没读过档的进程代数是 0，进来就返回
static _Atomic uint64_t g_win_clock_gen;
static struct { _Atomic(const void *) w; _Atomic uint64_t gen; } g_win_clock[8];
static pthread_mutex_t g_win_clock_mu = PTHREAD_MUTEX_INITIALIZER;   // 清和登记串行：取图、上屏可以在两条线程上，后到的要等先到的清完才读
void hle_snap_win_clock_restored(void) { atomic_fetch_add_explicit(&g_win_clock_gen, 1, memory_order_release); }   // hle_snap_restore 调；单测直接调
static void win_clock_fresh(win_t *o) {
    uint64_t g = atomic_load_explicit(&g_win_clock_gen, memory_order_acquire); if (!g) return;
    for (int k = 0; k < 8; k++) if (atomic_load_explicit(&g_win_clock[k].w, memory_order_acquire) == (const void *)o && atomic_load_explicit(&g_win_clock[k].gen, memory_order_acquire) == g) return;
    pthread_mutex_lock(&g_win_clock_mu);
    int at = -1, empty = -1;
    for (int k = 0; k < 8; k++) { const void *w = atomic_load_explicit(&g_win_clock[k].w, memory_order_relaxed); if (w == (const void *)o) { at = k; break; } if (!w && empty < 0) empty = k; }
    if (at < 0) at = empty >= 0 ? empty : (int)(((uintptr_t)o >> 4) & 7);   // 表满（8 个以上活窗口交替用）：按地址挤掉一个，被挤的下次再清一遍（多一帧不受节拍限制）
    if (atomic_load_explicit(&g_win_clock[at].w, memory_order_relaxed) != (const void *)o || atomic_load_explicit(&g_win_clock[at].gen, memory_order_relaxed) != g) {
        static double back = -1; if (back < 0) { const char *e = getenv("DM_TEST_SNAP_CLOCK_BACK"); back = e ? atof(e) : 0; if (back < 0) back = 0; }
        // 测试开关 DM_TEST_SNAP_CLOCK_BACK=<秒>：把读回来的时刻往后拨这么多，等于「单调钟比存档时小了这么多秒」（设备重启过）。产品里不设。
        //   下面紧接着就清零，所以在这份代码上它只改日志里那个读数（确认恢复路径看到了未来的时刻）；要复现卡死，去掉清零那一行再跑
        if (back > 0) { uint64_t d = (uint64_t)(back * 1e9); for (int i = 0; i < 4; i++) if (o->avail[i]) o->avail[i] += d; if (o->pace) o->pace += d; }
        uint64_t now = now_ns(), mx = o->pace; for (int i = 0; i < 4; i++) if (o->avail[i] > mx) mx = o->avail[i];
        if (mx) LOG("即时存档恢复：窗口 %p 的节拍时刻是存档进程的钟，最晚的一个%s %.3f 秒，清掉、从现在重新起拍", (void *)o, mx > now ? "在这之后" : "已过去", (mx > now ? mx - now : now - mx) / 1e9);   // 全 0 = 读档后才建的窗口，不报
        memset(o->avail, 0, sizeof o->avail); o->pace = 0;
        // 先清、再写窗口、最后写代数（都带 release）：快路径先读窗口再读代数，读到「是我、是这一代」时清零一定已经可见（空槽 / 同一窗口新一代 / 挤掉别人三种都成立）
        atomic_store_explicit(&g_win_clock[at].w, (const void *)o, memory_order_release); atomic_store_explicit(&g_win_clock[at].gen, g, memory_order_release);
    }
    pthread_mutex_unlock(&g_win_clock_mu);
}
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *s, int *idx) {
    win_t *o = (win_t *)w;
    win_clock_fresh(o);   // 读档后第一次取图前清
    int nact = __atomic_load_n(&o->nactive, __ATOMIC_RELAXED);
    if (nact <= 0) { static int n; if (n++ < 4) LOG("【红】 [nvn] WindowAcquireTexture: the window has no active texture (%d of %d): no texture handed out", nact, o->ntex); *idx = -1; return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR; }
    *idx = (int)((unsigned)__atomic_fetch_add(&o->next, 1, __ATOMIC_RELAXED) % (unsigned)nact);
    // 借一帧的线程（有节拍主人、本线程不是它：加载线程画一帧过场 / 视频这类）取图一律立即返回，不进下面的翻走等待、也不等 ticket。
    //   11-19 #9ef296（0100BB8017F24000，0.3.10–0.5.8、Mac 与 iPad 共 9 件 17 次崩）：游戏「这帧还没取图就取一张」（device+0x18 判 < 0）不是原子的，
    //   主线程在取图里睡节拍时加载线程也来取图；它在取图里只要阻塞到主线程那次呈现之后（0.3.10 是 ticket 等待，0.4.x 起是 #141 翻走等待），
    //   主线程呈现完已清空共享的渲染目标绑定，加载线程醒来读到空、在 main+0xfafa4 读 +0x40 崩。23 次视频加载对位：崩的 17 次全撞图阻塞，不崩的 6 次全没撞。
    //   撞上在屏那张也照交（这一帧画进还在屏上的那张，最多闪一帧；阻塞则必崩）。翻走等待只留给还没有节拍主人的开局阶段
    uintptr_t pt0 = atomic_load_explicit(&g_present_thr, memory_order_relaxed);
    if (pt0 && pt0 != (uintptr_t)pthread_self()) {
        int sh = __atomic_load_n(&o->shown, __ATOMIC_RELAXED);
        acq_lease_force(o, *idx);
        if (*idx < 4) atomic_store_explicit(&g_acq_thr[*idx], (uintptr_t)pthread_self(), memory_order_relaxed);
        atomic_fetch_add_explicit(&g_acq_cnt[1], 1, memory_order_relaxed);
        { static _Atomic uint64_t bn; uint64_t bk = atomic_fetch_add(&bn, 1) + 1;
          if (bk <= 8 || !(bk & (bk - 1))) LOG("[nvn] WindowAcquireTexture：非出帧线程借一帧，立即交出图 %d（%s），累计 %llu 次", *idx, sh && *idx == sh - 1 ? "是屏上那张" : "不在屏上", (unsigned long long)bk); }
        if (s) sync_publish((sync_t *)s, nvn_be_last_fence());
        return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
    }
    {   // #141（伊苏 8 闪退）：真机只交已归还的图——最近一次呈现的那张还在屏上（等下一次呈现翻走才归还），绝不会被取到。
        //   撞上就等下一次呈现把它翻走再交（真机的取图阻塞点：全链在等显示时等下一张放还，与下面的节拍等待、ticket 等待各管一段）。
        //   活跃 1 张不适用：真机也没有第二张可交，取图等它自己那笔呈现处理完（下面的 ticket 等待）即是。
        //   等的是别人的呈现，上限 500 ms：超时不能把仍在屏上的 drawable 交回去继续写；改交另一张已归还的活跃图，没有才保持旧行为并报红。
        //   单张活跃 / 截图门没有可替换的确定性路径，保留原行为。截图门（DM_DETCLOCK）不适用：定时等 + 超时兜底是墙上时钟竞态，门图确定性优先。
        //   交出还在屏上那张的次数（单张 / 门图）：前 20 次逐次报，之后每到 2 的幂报一次（带累计），修前修后比这个数
        int sh = __atomic_load_n(&o->shown, __ATOMIC_RELAXED);
        if (sh && *idx == sh - 1) {
            int waited = 0;
            // 节拍主人（出帧的取图线程）或最近一次呈现那张的取图线程就是本线程时，下一次呈现由本线程自己发出，
            //   等它等于等自己：不进入等待；直接从已归还的活跃图里认领另一张，绝不交出在屏 drawable。
            //   「最近一次呈现的线程」按被上屏那张的取图线程记（g_acq_thr，1:1 引擎里取图与上屏同线程）。
            uintptr_t self = (uintptr_t)pthread_self();
            int mine = atomic_load_explicit(&g_present_thr, memory_order_relaxed) == self ||
                       (sh - 1 >= 0 && sh - 1 < 4 && atomic_load_explicit(&g_acq_thr[sh - 1], memory_order_relaxed) == self);
            const char *why = nact < 2 ? "只有 1 张活跃" : dm_detclock_enabled() ? "截图门确定性时钟" : mine ? "本线程负责下一次呈现，不能等自己" : "等满 500 ms 上限没被翻走";   // 交出时红行带上走的是哪一支
            if (nact >= 2 && !dm_detclock_enabled() && !mine) {
                // 上限 500 ms（原 2 个呈现周期 ≈ 33 ms）：D-9EQT（0100BB8017F24000，Mac）这款取图线程连取两张、上屏在别的线程，那笔呈现慢过 33 ms 就兜底交出在屏那张，
                //   崩局 3/3 紧随其后（游戏按图号取的对象为空，读 +0x40），不崩的 17 局 0 次。真机这里一直阻塞到翻走；500 ms 只兜「真没有呈现方」的局，超时不交出在屏图。
                uint64_t bound = 500000000ull; (void)present_period;
                atomic_fetch_add_explicit(&g_acq_cnt[0], 1, memory_order_relaxed);
                struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += (long)(bound % 1000000000ull); dl.tv_sec += (time_t)(bound / 1000000000ull);
                if (dl.tv_nsec >= 1000000000L) { dl.tv_nsec -= 1000000000L; dl.tv_sec++; }
                uint64_t w0 = now_ns(); DM_BLOCK_BEGIN();
                pthread_mutex_lock(&g_flip_mu);
                while (__atomic_load_n(&o->shown, __ATOMIC_RELAXED) == sh && !dm_gfx_session_stopping())
                    if (pthread_cond_timedwait(&g_flip_cv, &g_flip_mu, &dl) == ETIMEDOUT) break;
                int flip = __atomic_load_n(&o->shown, __ATOMIC_RELAXED) != sh;
                pthread_mutex_unlock(&g_flip_mu);
                DM_BLOCK_END();
                __atomic_fetch_add(&g_pq_wait_ns, now_ns() - w0, __ATOMIC_RELAXED);   // 翻走等待计入取图内的呈现队列等待（@pq_wait_ms；原子加：与取图线程并发的另一取图线程也会累计）
                if (flip) { waited = 1; static _Atomic uint64_t wn; uint64_t wk = atomic_fetch_add(&wn, 1) + 1;
                    if (wk <= 20 || !(wk & (wk - 1))) LOG("[nvn] WindowAcquireTexture 撞上还在屏上的那张：图 %d 等了 %.1f ms 等到下一次呈现翻走才交出（真机语义），累计 %llu 次",
                                                           *idx, (now_ns() - w0) / 1e6, (unsigned long long)wk); } }
            if (!waited && (nact < 2 || dm_detclock_enabled()))
                acquire_visible_legacy(o, *idx, nact, why); } }
    if (nact >= 2 && !dm_detclock_enabled()) {
        int visible = __atomic_load_n(&o->shown, __ATOMIC_RELAXED) - 1;
        int pick = acquire_claim_idx(o, *idx, visible, nact);
        if (pick < 0) {
            acquire_visible_legacy(o, *idx, nact, "没有未被租用的已归还活跃图，保持旧行为");
            acq_lease_force(o, *idx);
        } else *idx = pick;
    } else acq_lease_force(o, *idx);
    if (*idx >= 0 && *idx < 4) atomic_store_explicit(&g_acq_thr[*idx], (uintptr_t)pthread_self(), memory_order_relaxed);
    // Vsync blocks here (real-device NVN: when all textures in the swapchain are waiting for display, wait for the next one to be released), not in present -- the game often calls present while holding a lock
    uint64_t at = *idx < 4 ? o->avail[*idx] : 0, now = dm_detclock_enabled() ? guest_ns() : now_ns(), wait = at > now ? at - now : 0, pace_ns = 0;
    if (!dm_detclock_enabled()) {   // Register the acquire thread as a time-constraint thread (no longer registered only when it "arrived early": it must also declare itself when it is constantly behind in the hot state)
        static uint64_t rt_period; uint64_t period = present_period(o), rtp = rt_period_ns(o, period);
        atomic_fetch_add_explicit(&g_pace_dec_n, 1, memory_order_relaxed); atomic_store_explicit(&g_pace_dec_period, period, memory_order_relaxed);
        if (rtp && rt_period != rtp) { rt_period = rtp; set_time_constraint(rtp); }
        rt_comp_tick(rtp);
        uint64_t p0 = now_ns();
        int follow = 0;   // 这一帧是不是显示链路的节拍控的（等到了节拍）：上屏时节拍器跟着它重新起拍，不再自己走一个周期（nvn_pace_follow）
        if (nvn_be_link_active()) { t_pace_waits++; if (nvn_be_link_wait_tick(link_vsyncs(o), period ? 2 * period : 33333333ull)) follow = 1; else if (wait) sleep_until_ns(at); }   // DM_PRESENT_LINK: pacing fully follows the display link (wait for the present interval's display-link ticks when running fast, go immediately when behind); our own 60 Hz clock is only the fallback when no display-link tick arrives
        else if (wait) sleep_until_ns(at);
        pace_follow_set(o, follow);
        pace_ns = now_ns() - p0;
    } else {   // DM_DETCLOCK_PACE=1 (used by the screenshot gate): while the guest clock advances per frame, each frame's wall clock is no shorter than one step -- frame count and in-game time are deterministic, and the background loading thread has at least one step of wall clock available per frame;
               // When the machine is fully loaded frames only get slower and the background only gets more wall clock, so the picture captured at "the Nth frame after the message" is independent of load
        static int pace = -1; static uint64_t step, last;
        if (pace < 0) { const char *e = getenv("DM_DETCLOCK_PACE"), *d = getenv("DM_DETCLOCK"); step = d ? strtoull(d, 0, 0) : 0; pace = e && atoi(e) && step; }
        if (pace) { if (last && now_ns() < last + step) sleep_until_ns(last + step); last = now_ns(); }
    }
    g_acq_wait_ns += wait; trow(g_frame, "@late_ms", wait ? (now_ns() - at) / 1e6 : 0);
    // The previous present of this texture has not yet been processed by the present thread (display cannot keep up, a whole swapchain round behind): wait for it to be returned, then hand it to the game to draw
    uint64_t tk = *idx < 4 ? o->ticket[*idx] : 0;
    if (tk) { uint64_t w0 = now_ns(); nvn_be_present_wait(tk); g_pq_wait_ns += now_ns() - w0; }
    if (s) sync_publish((sync_t *)s, nvn_be_last_fence());   // The swapchain texture becomes available once the commits before this point have completed
    if (!g_fs_start) { g_fs_start = now_ns(); g_fs_wait = pace_ns; }   // the frame starts when the first acquire after a present returns: on a display-link tick when on time
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
// The older API (NVN 53, Super Mario 3D World + Bowser's Fury): the queue waits for the texture on the GPU and no sync object is handed out. The host replays in submission order, so later commands already come after earlier presents; the same as WindowAcquireTexture without a sync object
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *idx) {
    (void)q; return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS ? (NVNqueueAcquireTextureResult)0 : (NVNqueueAcquireTextureResult)1;
}

// Frame boundary: on each present, print / write to disk the call count of each NVN function for this frame
static uint64_t g_prev[NVN_NFUNC];
static FILE *g_tsv;
static void dump_frame(void) {
    static int inited, nframes = 3, calls = 1;
    nvn_be_tex_tick(g_frame);   // DM_NVN_ALLOCPROF: texture init / release summary every 300 frames
    if (g_frame % 300 == 0) rec_out_summary(g_frame);   // 录制外命令 / 录制重入有新增才打（不论开没开调用计数）
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_NVN_FRAMES"); if (e) nframes = atoi(e);
        const char *c = getenv("DM_NVN_TSV_CALLS"); if (c) calls = atoi(c);   // 0 = write only the @ timing rows (pulling files from a real device has a size limit, and the call-count rows take about 2/3)
        static char tp[1024]; const char *t = nvn_diag_path(getenv("DM_NVN_TSV"), tp, sizeof tp); if (t && !(g_tsv = fopen(t, "w"))) LOG("【红】 cannot open %s", t);   // iOS: relative path under Documents
        if (g_tsv) fprintf(g_tsv, "# kind: nvn-per-frame  value = call count when name is an NVN function name; names starting with @ are timing probes (ms / count)\nframe\tname\tvalue\n");
    }
    if (nvn_counting()) {   // the call counts exist only with the counting trampolines (nvn_counting)
        uint64_t total = 0; unsigned nfn = 0;
        char line[8192]; size_t o = 0; line[0] = 0;
        for (unsigned i = 0; i < NVN_NFUNC; i++) {
            uint64_t c = atomic_load(&nvn_slot[i].cnt), d = c - g_prev[i];
            g_prev[i] = c;
            if (!d) continue;
            total += d; nfn++;
            if (g_tsv && calls) fprintf(g_tsv, "%llu\t%s\t%llu\n", g_frame, nvn_names[i], d);
            if (g_frame <= (uint64_t)nframes && o < sizeof line - 96) o += snprintf(line + o, sizeof line - o, " %s=%llu", nvn_names[i] + 3, d);
        }
        if (g_frame <= (uint64_t)nframes) LOG("[nvn] %s %llu: %u functions %llu calls |%s", g_frame ? "frame" : "init", g_frame, nfn, total, line);
        else if (g_frame % 300 == 0) LOG("[nvn] frame %llu: %u functions %llu calls", g_frame, nfn, total);
    } else if (!g_frame) LOG("[nvn] call counts off (DM_NVN_COUNT=1 turns them on)");
    if (g_tsv) fflush(g_tsv);   // the @ timing rows too, with or without call counts
    if (g_frame && g_frame % 300 == 0) {   // 审计 #1534 P1-23 采样：累计值，0 也打；首次、变了、或满 3000 帧打一行
        static uint64_t lf, l0, l1; static int any; uint64_t n0 = __atomic_load_n(&g_nvn_qws_n, __ATOMIC_RELAXED), n1 = __atomic_load_n(&g_nvn_qws_unfenced, __ATOMIC_RELAXED);
        if (!any || n0 != l0 || n1 != l1 || g_frame - lf >= 3000) { any = 1; lf = g_frame; l0 = n0; l1 = n1;
            LOG("[nvn] queue sync waits up to frame %llu: QueueWaitSync %llu, on a sync not fenced yet %llu (audit #1534 P1-23 probe)", (unsigned long long)g_frame, (unsigned long long)n0, (unsigned long long)n1); }
    }
    g_frame++; cs_dump();
    // Every 300 frames read resident memory and hand it to the graphics layer to print the memory readout line (nvn_be_mem_stat, low frequency, capped). Memory watchdog: when resident memory exceeds the cap, log a failure and exit on its own. On by default only on the Mac host (24 GB: runaway host memory
    // drags the whole machine into a kernel panic, which happened twice with other programs on the same machine), or set for tests with DM_MEM_CAP_GB=<integer>; the player's iOS app has no cap (exiting before the system kills it is pointless and only adds another kind of crash)
    if (g_frame % 300 == 0) { task_vm_info_data_t vi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) { unsigned long long mb = (unsigned long long)(vi.phys_footprint >> 20); void nvn_be_mem_stat(unsigned long long, unsigned long long); nvn_be_mem_stat(g_frame, mb);
            static long cap = -1; if (cap < 0) { const char *e = getenv("DM_MEM_CAP_GB"); cap = e ? atol(e) : DM_MEM_CAP_DEFAULT_GB; }
            if (cap > 0 && mb > (unsigned long long)cap * 1024) { LOG("【红】 memory watchdog: resident %llu MB over the limit of %ld GB (frame %llu), exiting", mb, cap, (unsigned long long)g_frame); _exit(86); } } }
    dm_guest_frame_tick();   // Deterministic guest clock (DM_DETCLOCK, host hle_os.c): each present advances a fixed step; a no-op when not enabled
}
// ---------- Timing instrumentation (per-frame rows written to DM_NVN_TSV: frame / @metric / value; every 300 frames a summary of p50 / p95 / max) ----------
#define TWIN 300
static double g_tw[6][TWIN]; static unsigned g_tn[6];
static double g_pres_iv[TWIN]; static unsigned g_presn;   // Present interval (ms; -1 = dropped by the system, 0 = no reading)
static double g_pres_exp[TWIN], g_pres_exp_last;   // how long that frame should have stayed on screen (ms, pres_expect_ms); the last one for slow_emit
// The hold (60 Hz vsyncs) each present was given, by present number. On-screen intervals are read 8 presents late, so each is judged against the holds of its own presents:
// with the frame step at 2 vsyncs every frame stays 33.3 ms, which is not a dropped frame
static uint8_t g_hold_ring[16];
static double pres_expect_ms(uint64_t n) {   // the interval between presents n - 1 and n shows frame n - 1: the display link holds it for its own present's hold, the Mac pacer spaces present n by n's, so the larger of the two
    int k = g_hold_ring[n % 16] > g_hold_ring[(n - 1) % 16] ? g_hold_ring[n % 16] : g_hold_ring[(n - 1) % 16];
    double held = (k ? k : 1) * 1000.0 / 60, tgt = 1000.0 * nvn_fps_period(1.0 / nvn_be_fps_target());   // 游戏原生 30 或用户上限 30 都不是慢帧
    return held > tgt ? held : tgt;
}
__attribute__((weak)) double dm_brightness_now(void) { return -1; }
__attribute__((weak)) void dm_perf_present(double iv_ms) { (void)iv_ms; }   // Default (unit tests); strong definition in the host's diag.c: always-on performance record perf.txt
uint64_t dm_nvn_sub_cpu_ns(void) { return atomic_load(&g_cum_sub_cpu_ns); }   // Cumulative CPU nanoseconds of submit (excluding present)
// 声音调度迟到探针（11-14 卡顿诊断）：累加在公用核心 runtime/src/ap_host.c（s2a_aud_late_note，C# 声音渲染线程静态导入），@aud_wait_ms 每帧取走清零；
//   没链公用核心的图形单测取 0（弱定义）
__attribute__((weak)) uint64_t s2a_aud_late_take(void) { return 0; }
__attribute__((weak)) int dm_thermal_now(void) { return -1; }   // Strong definitions: iOS ios/app.m, Mac runtime/src/host_main.c (both NSProcessInfo, audit P1-80); -1 only in hosts/tests that link neither
__attribute__((weak)) int dm_low_power_now(void) { return 0; }   // Low Power Mode (iOS front_ios.m); off on the Mac
// Per-frame ring for the long-frame log (GPU time only arrives 8 frames late, so store first and judge later)
#define LFN 16
typedef struct { uint64_t f; double fr, nv, t_s, make_ms, up_mb, up_ms, sw_ms, sc_ms; unsigned pso, lib, up, rup; char stl[1024]; } lf_rec;   // sw / sc: submit wall / submit-thread CPU; stl: upload stall breakdown (nvn_be_stall_take)
__attribute__((weak)) void nvn_be_stall_take(char *out, size_t n) { if (n) out[0] = 0; }   // Upload stall breakdown for the long-frame line; strong definition in the Metal backend (nvn_mtl.m), none on Android: empty
static lf_rec g_lf[LFN];
__attribute__((weak)) void dm_longframe_note(const char *what) { LOG("[longframe] %s", what); }   // Default (graphics unit tests); the strong definition in the host's diag.c adds scene context and writes perf.txt
extern void nvn_be_create_ns(uint64_t *lib_ns, uint64_t *pso_ns); extern uint64_t nvn_be_upload_bytes(void); extern void nvn_be_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
static const char *const g_tname[6] = {"frame time", "frame work", "NVN submit+present", "GPU", "translation layer recording", "translation layer submit"};
static void tpush(int k, double v) { g_tw[k][g_tn[k]++ % TWIN] = v; }
static int dcmp(const void *a, const void *b);
// GPU 忙累计毫秒（逐帧 GPU 时间之和，晚 8 帧入账）：runtime/src/energy.c 的「[能耗] 近 N 秒」行与下面 power 行按差算 ms/s。是时间、不是能耗
//   （10-30 iOS 功耗测量口径修法 5：task_gpu_utilisation / 资源联盟的 GPU 能耗在真机一直读 0）
static double g_gpu_busy_ms;
double s2a_gpu_busy_ms(void) { return g_gpu_busy_ms; }
// 入账到哪儿的 GPU 时钟（已入账各帧最晚 GPU 结束，毫秒）：GPU 忙晚 8 帧、在 CPU 的帧边界入账，CPU 一顿（慢帧）顿前的 GPU 时间就挤进下一个墙钟窗口，
//   HUD 按墙钟算的「GPU 忙」跳过 1000 ms/s（#647：GPU 只有一条时间线，独占时间不可能超过 1 秒每秒）。按这个时钟差当分母，入账节奏不再进读数
static double g_gpu_clk_ms;
double s2a_gpu_clock_ms(void) { return g_gpu_clk_ms; }
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
// Power over the window since the last summary, for the whole process: CPU energy (task_energy in nJ, Apple silicon), CPU time (100% = one core) and the share of it on performance cores,
// wakeups per second (interrupt / out of platform idle), GPU time per second (task_gpu_utilisation), thermal state, and the guest threads that yielded most (hle_thread.c);
// then render-target traffic per frame (attachment load / store bytes, passes that load and store everything, stores saved by DM_NVN_UNWRITTEN_DC) and guest-thread CPU by Switch priority
static void power_line(uint64_t frame) {
#if defined(__ANDROID__) || defined(__OHOS__)
    (void)frame;   // Android 功耗/能效采样未接；不输出假的 0 统计
    return;
#else

    void nvn_be_handle_wait_stats(uint64_t s[6]); static uint64_t phw[6]; uint64_t hw[6]; nvn_be_handle_wait_stats(hw);
    LOG("[nvn] CPU uniform-handle waits up to frame %llu: waits %llu, total %.3f ms, conservative %llu (all %llu, collision %llu), cancelled %llu (since previous report)", frame,
        (unsigned long long)(hw[0] - phw[0]), (hw[1] - phw[1]) / 1e6, (unsigned long long)(hw[2] - phw[2]),
        (unsigned long long)(hw[3] - phw[3]), (unsigned long long)(hw[4] - phw[4]), (unsigned long long)(hw[5] - phw[5])); memcpy(phw, hw, sizeof phw);
    static uint64_t pt, pe, pcpu, pp, piw, ppw, pg, pf, pld, pst, pfu, pudc; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    task_power_info_v2_data_t pw; mach_msg_type_number_t n = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&pw, &n) != KERN_SUCCESS) return;
    uint64_t t = now_ns(), cpu = (pw.cpu_energy.total_user + pw.cpu_energy.total_system) * tb.numer / tb.denom, p = pw.task_ptime * tb.numer / tb.denom;
#if defined(__arm64__)
    uint64_t e = pw.task_energy;
#else
    uint64_t e = 0;
#endif
    uint64_t iw = pw.cpu_energy.task_interrupt_wakeups, idw = pw.cpu_energy.task_platform_idle_wakeups, g = (uint64_t)(g_gpu_busy_ms * 1e6);   // GPU：命令缓冲忙的时间（纳秒累计；task_gpu_utilisation 真机读 0）
    if (pt && t > pt) { double s = (t - pt) / 1e9; char yt[192]; dm_yield_top(yt, sizeof yt);
        double nf = frame > pf ? (double)(frame - pf) : 1;   // per frame: the fair comparison when one build holds the frame rate better than another
        LOG("[nvn] power up to frame %llu: CPU energy %.2f W, %.1f mJ per frame (%.1f fps); CPU %.0f%% (100 = one core), %.0f%% of it on performance cores; wakeups %.0f/s (out of platform idle %.0f); GPU busy %.1f ms/s (time, not energy); thermal %d; top yielders: %s", frame,
            (e - pe) / 1e9 / s, (e - pe) / 1e6 / nf, nf / s, (cpu - pcpu) / 1e7 / s, cpu > pcpu ? 100.0 * (double)(p - pp) / (double)(cpu - pcpu) : 0, (iw - piw) / s, (idw - ppw) / s, (g - pg) / 1e6 / s, dm_thermal_now(), yt);
        char rs[1200]; nvn_be_res_stats(rs, sizeof rs); static uint64_t pbs, pbf; uint64_t bs, bf; nvn_be_barrier_stats(&bs, &bf);
        uint64_t nvn_be_cut_count(void), nvn_be_copy_cut_count(void), nvn_be_pass_cut_count(void), nvn_be_blit_splits(void); void nvn_be_ubd_stats(uint64_t *, uint64_t *); static uint64_t pcut, pccut, ppcut, pbsp, pud, puc; uint64_t cut = nvn_be_cut_count(), ccut = nvn_be_copy_cut_count(), pcutn = nvn_be_pass_cut_count(), bsp = nvn_be_blit_splits(), ud, uc; nvn_be_ubd_stats(&ud, &uc);
        void nvn_be_ubp_stats(uint64_t *, uint64_t *, uint64_t *); static uint64_t ppm, ppr, ppf; uint64_t pm, pr, pf_; nvn_be_ubp_stats(&pm, &pr, &pf_);
        void nvn_be_lc_stats(uint64_t *, uint64_t *); static uint64_t plt, pll; uint64_t lt, ll; nvn_be_lc_stats(&lt, &ll);
        void nvn_be_ubp_why(uint64_t w[5]); static uint64_t pw5[5]; uint64_t w5[5]; nvn_be_ubp_why(w5);
        uint64_t ld, st, fu, udc = nvn_be_udc_saved_bytes(); nvn_be_bw(&ld, &st, &fu);
        LOG("[nvn] residency set up to frame %llu: %s; draw-pass entry barriers full %llu / saved %llu; split submits %llu (%llu after buffer → texture copies, %llu inside a draw pass); passes broken by copies / buffer updates %llu; in-pass uniform updates deferred %llu, passes ended to land them for a read %llu, partly covered blocks read from memory %llu (cached %llu, refused %llu: other GPU writes %llu, earlier submission running %llu, copy or fill queued %llu, size %llu, unreadable %llu); waiting clears taken by a later pass %llu, landed in passes of their own %llu; "
            "attachments per frame load %.0f MB / store %.0f MB, load-all store-all passes %.0f, saved by not storing unwritten attachments %.1f MB", frame, rs, (unsigned long long)(bf - pbf), (unsigned long long)(bs - pbs),
            (unsigned long long)(cut - pcut), (unsigned long long)(ccut - pccut), (unsigned long long)(pcutn - ppcut), (unsigned long long)(bsp - pbsp), (unsigned long long)(ud - pud), (unsigned long long)(uc - puc), (unsigned long long)(pm - ppm), (unsigned long long)(pr - ppr), (unsigned long long)(pf_ - ppf), (unsigned long long)(w5[1] - pw5[1]), (unsigned long long)(w5[2] - pw5[2]), (unsigned long long)(w5[3] - pw5[3]), (unsigned long long)(w5[0] - pw5[0]), (unsigned long long)(w5[4] - pw5[4]), (unsigned long long)(lt - plt), (unsigned long long)(ll - pll),
            (ld - pld) / 1048576.0 / nf, (st - pst) / 1048576.0 / nf, (fu - pfu) / nf, (udc - pudc) / 1048576.0 / nf); pbs = bs; pbf = bf; pcut = cut; pccut = ccut; ppcut = pcutn; pbsp = bsp; pud = ud; puc = uc; ppm = pm; ppr = pr; ppf = pf_; memcpy(pw5, w5, sizeof pw5); plt = lt; pll = ll;
            { void nvn_be_handle_wait_stats(uint64_t s[6]); static uint64_t phw[6]; uint64_t hw[6]; nvn_be_handle_wait_stats(hw);   // PR #324：CPU 解码句柄前等已提交写入者的次数 / 耗时 / 保守等（全部页 / 槽冲突）/ 被取消
              LOG("[nvn] CPU uniform-handle waits up to frame %llu: %llu waits, %.1f ms total, conservative %llu (all-pages %llu, slot collision %llu), cancelled %llu", frame, (unsigned long long)(hw[0] - phw[0]), (hw[1] - phw[1]) / 1e6,
                  (unsigned long long)(hw[2] - phw[2]), (unsigned long long)(hw[3] - phw[3]), (unsigned long long)(hw[4] - phw[4]), (unsigned long long)(hw[5] - phw[5])); memcpy(phw, hw, sizeof phw); }
        char tc[400]; dm_thread_cpu(tc, sizeof tc, s);
        LOG("[nvn] threads up to frame %llu: CPU %s", frame, tc); }
    else { char yt[192], rs[900], tc[400]; dm_yield_top(yt, sizeof yt); nvn_be_res_stats(rs, sizeof rs); dm_thread_cpu(tc, sizeof tc, 1); }   // the first call only takes the baselines
    pt = t; pe = e; pcpu = cpu; pp = p; piw = iw; ppw = idw; pg = g; pf = frame; nvn_be_bw(&pld, &pst, &pfu); pudc = nvn_be_udc_saved_bytes();
#endif
}
// Who takes the CPU (11-01 frame-pacing research, conclusion 1): four games' slow frames rise with machine load, and the logs could not tell "another process takes the CPU"
//   from "our own threads oversubscribe". Every 300-frame window prints one line next to the frame-interval line (same window):
//   Slow frames here are intervals > 1.5 × the period (same rule as the five-question frame-rate cell); "> one period" counts ~half of all frames at 60 fps (jitter around 16.7 ms).
//   machine busy = host_processor_info (user + system + nice) tick delta / total tick delta over all cores; this process = getrusage CPU time delta / (wall × cores); others = machine − this.
//   Reading: ≥ 90% of frames slow → a steady cadence below the target (11-01 Zelda: 60 fps on, it renders 30, slow 299 / 300 with 58% of 18 cores idle), checked first —
//   contention only makes some frames slow; others high + slow frames → another process takes the CPU (game mode / background); this process high, others low + slow frames → our own threads oversubscribe
//   (compile / prewarm / worker threads). On by default (one host_processor_info + getrusage per window); S2A_LOAD_LOG=0 turns it off. No machine-wide numbers (iOS may refuse) → no line
#if !defined(__ANDROID__) && !defined(__OHOS__)   // host_processor_info / getrusage 份额是苹果侧读数；安卓的 tsummary 不走这里（同 power_line）
static void load_line(uint64_t frame, unsigned slow, unsigned n) {
    static int on = -1; if (on < 0) { const char *e = getenv("S2A_LOAD_LOG"); on = !(e && !strcmp(e, "0")); }
    if (!on) return;
    static uint64_t pb, pt, ps, pw; natural_t nc = 0; processor_info_array_t a = NULL; mach_msg_type_number_t cnt = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &nc, &a, &cnt) != KERN_SUCCESS || !nc) return;
    processor_cpu_load_info_data_t *c = (processor_cpu_load_info_data_t *)a; uint64_t busy = 0, tot = 0;
    for (natural_t i = 0; i < nc; i++) { uint64_t u = (uint64_t)c[i].cpu_ticks[CPU_STATE_USER] + c[i].cpu_ticks[CPU_STATE_SYSTEM] + c[i].cpu_ticks[CPU_STATE_NICE]; busy += u; tot += u + c[i].cpu_ticks[CPU_STATE_IDLE]; }
    vm_deallocate(mach_task_self(), (vm_address_t)a, cnt * sizeof(integer_t));
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    uint64_t self = (uint64_t)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); uint64_t wall = (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
    if (pw && tot > pt && wall > pw) {
        double all = 100.0 * (double)(busy - pb) / (double)(tot - pt), me = 100.0 * (double)(self - ps) * 1000.0 / ((double)(wall - pw) * nc), oth = all - me < 0 ? 0 : all - me;
        LOG("[nvn] load up to frame %llu: machine busy %.0f%%, this process %.0f%%, other processes %.0f%% (%u cores); slow frames (> 1.5 periods) %u / %u%s", frame, all, me, oth, (unsigned)nc, slow, n,
            slow * 10 >= n * 9 ? " <- almost every frame slow: the game renders below the target rate (e.g. 60 fps on, it draws 30), not CPU contention"
            : slow * 100 > n && oth > 25 ? " <- another process takes the CPU (game mode / background)" : slow * 100 > n && me > 70 ? " <- own threads oversubscribe (compile / prewarm / workers)" : "");
    }
    pb = busy; pt = tot; ps = self; pw = wall;
}
#endif
void s2a_fps_slots_note(char *buf, size_t n); void nvn_be_rt_window(uint32_t *w, uint32_t *h, uint32_t *fmt);   // 定义见下（弱默认）
static void tsummary(uint64_t frame) {
#if defined(__ANDROID__) || defined(__OHOS__)
    (void)frame;   // Android 的 Apple 线程/QoS/功耗诊断未接；不输出伪造读数
    return;
#else

    char line[1024]; size_t o = 0;
    for (int k = 0; k < 6; k++) {
        unsigned n = g_tn[k] < TWIN ? g_tn[k] : TWIN; if (!n) continue;
        double v[TWIN]; memcpy(v, g_tw[k], n * sizeof *v); qsort(v, n, sizeof *v, dcmp);
        o += snprintf(line + o, sizeof line - o, " %s p50 %.2f / p95 %.2f / max %.2f ms (%u frames);", g_tname[k], v[n / 2], v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], v[n - 1], n);
    }
    LOG("[nvn] timing up to frame %llu:%s", frame, line);
    { uint32_t rw, rh, rf; nvn_be_rt_window(&rw, &rh, &rf); char sl[300] = ""; s2a_fps_slots_note(sl, sizeof sl);   // 11-02：动态分辨率有没有把渲染尺寸抬上去
      LOG("[nvn] render targets up to frame %llu: largest %ux%u (Metal format %u)%s%s", frame, rw, rh, rf, sl[0] ? "; frame-rate table slots " : "", sl); }
    double tgt = 1000.0 / nvn_be_fps_target();   // milliseconds one frame should take: 16.7 at 60 fps, 33.3 when the game itself asks for 30 (the over-time / dropped-frame counts below use it)
    { unsigned n = g_tn[0] < TWIN ? g_tn[0] : TWIN, over = 0; double v[TWIN];   // frame interval gets its own line (M1 baseline): number of frames longer than one frame period in this window and p99
      if (n) { memcpy(v, g_tw[0], n * sizeof *v); qsort(v, n, sizeof *v, dcmp); for (unsigned i = 0; i < n; i++) over += v[i] > tgt;
        LOG("[nvn] frame interval up to frame %llu: >%.1fms %u / %u frames, p99 %.2f ms", frame, tgt, over, n, v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1]);
        unsigned slow = 0; for (unsigned i = 0; i < n; i++) slow += v[i] > 1.5 * tgt;   // slow frame = longer than 1.5 periods (the five-question slow-frame rule); "> one period" is jitter at 60 fps (~half the frames)
        load_line(frame, slow, n); } }
    { unsigned n = g_presn < TWIN ? g_presn : TWIN, k = 0, drop = 0, discard = 0; double v[TWIN];   // on-screen present: dropped frame = on screen longer than its hold × 1.5 (pres_expect_ms) or discarded
      for (unsigned i = 0; i < n; i++) { double x = g_pres_iv[i]; if (x < 0) { drop++; discard++; } else if (x > 0) { v[k++] = x; drop += x > g_pres_exp[i] * 1.5; } }
      task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT; double fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vc) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1;
      double br = dm_brightness_now();   // footprint / brightness are carried on the on-screen present row: does not depend on the tick-entry observation gate (camp runs have it too)
      if (k) { qsort(v, k, sizeof *v, dcmp); LOG("[nvn] on-screen up to frame %llu: %u frames, dropped %u (discarded %u), p99 %.2f ms; footprint %.0f MiB; brightness %.2f; thermal %d", frame, k + discard, drop, discard, v[(k * 99) / 100 < k ? (k * 99) / 100 : k - 1], fp, br, dm_thermal_now()); }
      g_presn = 0; }
    power_line(frame);
    { static uint64_t prev[4], pf; uint64_t v[4]; nvn_be_vbuf_stats(&v[0], &v[1], &v[2], &v[3]);
      if (pf && frame > pf) { double nf = (double)(frame - pf);
          LOG("[nvn] vertex bindings up to frame %llu: per frame address lookups %.0f / reused %.0f, vertex table writes %.0f / skipped %.0f", frame,
              (v[0] - prev[0]) / nf, (v[1] - prev[1]) / nf, (v[2] - prev[2]) / nf, (v[3] - prev[3]) / nf); }
      memcpy(prev, v, sizeof prev); pf = frame; }
    { static uint64_t pi, pc, pd, pf; uint64_t ci = atomic_load(&g_cum_sub_instr), cc = atomic_load(&g_cum_sub_cpu_ns), ps_, dr_; nvn_be_stats(&ps_, &dr_);   // translation layer
      if (pf && frame > pf && dr_ > pd) LOG("[nvn] translation up to frame %llu: %.0f instructions per draw, submit per frame %.2f M instructions / CPU %.3f ms, %.0f draws per frame", frame,
          (double)(ci - pi) / (double)(dr_ - pd), (ci - pi) / 1e6 / (double)(frame - pf), (cc - pc) / 1e6 / (double)(frame - pf), (double)(dr_ - pd) / (double)(frame - pf));
      { void nvn_be_cost_stats(uint64_t v[5]); static uint64_t pv[5]; uint64_t v[5]; nvn_be_cost_stats(v);   // where the per-draw cost goes (10-31 Monster Hunter Rise submit thread)
        if (pf && frame > pf) { double nf = (double)(frame - pf);
            LOG("[nvn] submit costs up to frame %llu: per frame render passes %.1f, texture uploads %.1f (%.0f KB, %.3f ms on the submit thread), shadow-write pages noted %.0f", frame,
                (v[0] - pv[0]) / nf, (v[1] - pv[1]) / nf, (v[2] - pv[2]) / 1024.0 / nf, (v[3] - pv[3]) / 1e6 / nf, (v[4] - pv[4]) / nf); }
        memcpy(pv, v, sizeof pv); }
      { static uint64_t py, pk, pw; uint64_t cy = atomic_load(&g_cum_sub_cyc), ck = atomic_load(&g_cum_sub_sys_ns), cw = atomic_load(&g_cum_sub_wall_ns);
        if (pf && frame > pf && ci > pi && cy > py) { double nf = (double)(frame - pf), cyc = (double)(cy - py), cpu = (double)(cc - pc);
          qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp);
          uint64_t aw = 0, ag = 0; int an = 0; void nvn_be_alloc_stats(uint64_t *, uint64_t *, int *); nvn_be_alloc_stats(&aw, &ag, &an);
          static uint64_t pyn, pyns; uint64_t yn = 0, yns = 0; dm_main_yield_stats(&yn, &yns);
          static uint64_t plt[2], plc[2]; static mach_timebase_info_data_t ltb; if (!ltb.denom) mach_timebase_info(&ltb);   // the submit thread per performance level (thr_counts_lv)
          double lt[2], lc[2]; for (int k = 0; k < 2; k++) { uint64_t t_ = atomic_load(&g_cum_sub_lv_t[k]), c_ = atomic_load(&g_cum_sub_lv_cyc[k]); lt[k] = (double)(t_ - plt[k]) * ltb.numer / ltb.denom; lc[k] = (double)(c_ - plc[k]); plt[k] = t_; plc[k] = c_; }
          LOG("[nvn] translation detail up to frame %llu: submit per frame %.2f M cycles (%.2f cycles per instruction), CPU %.3f ms of which kernel %.3f ms, wall clock %.3f ms; mean clock %.2f GHz (cycles / CPU time); this thread's QoS %#x; allocator pools %d, GPU waits so far %llu; real-time declared computation %.2f ms; main thread yields per frame %.0f / %.3f ms; submit thread QoS %#x, its submit CPU %.0f%% on performance cores (perflevel0 %.2f GHz, perflevel1 %.2f GHz)", frame,
              cyc / 1e6 / nf, cyc / (double)(ci - pi), cpu / 1e6 / nf, (double)(ck - pk) / 1e6 / nf, (double)(cw - pw) / 1e6 / nf, cpu > 0 ? cyc / cpu : 0, (unsigned)qc, an, (unsigned long long)aw, g_rt_comp_ms, (double)(yn - pyn) / nf, (double)(yns - pyns) / 1e6 / nf,
              (unsigned)atomic_load_explicit(&g_sub_qos_seen, memory_order_relaxed), lt[0] + lt[1] > 0 ? 100.0 * lt[0] / (lt[0] + lt[1]) : 0, lt[0] > 0 ? lc[0] / lt[0] : 0, lt[1] > 0 ? lc[1] / lt[1] : 0); pyn = yn; pyns = yns; }
          if (pf && frame > pf) { uint64_t tv[2][2][2]; static uint64_t ptv[2][2][2]; dm_thread_lv_stats(tv); double w[2][2], nf = (double)(frame - pf);   // 编码线程 / 上屏线程按性能档（nvn_rec.h thr_lv_add）
            static mach_timebase_info_data_t ltb; if (!ltb.denom) mach_timebase_info(&ltb);
            for (int t = 0; t < 2; t++) for (int k = 0; k < 2; k++) w[t][k] = (double)(tv[t][k][0] - ptv[t][k][0]) * ltb.numer / ltb.denom;
            LOG("[nvn] thread cores up to frame %llu: encode thread %.2f ms a frame, %.0f%% on performance cores; present thread %.2f ms a frame, %.0f%% on performance cores (s2a_qos.h table)", frame,
                (w[0][0] + w[0][1]) / 1e6 / nf, w[0][0] + w[0][1] > 0 ? 100.0 * w[0][0] / (w[0][0] + w[0][1]) : 0, (w[1][0] + w[1][1]) / 1e6 / nf, w[1][0] + w[1][1] > 0 ? 100.0 * w[1][0] / (w[1][0] + w[1][1]) : 0);
            memcpy(ptv, tv, sizeof ptv); }
        if (frame % 300 == 0) {   // the declaration lives on the acquire thread (g_rt_port); this line runs on the submitting thread, and reading its own policy printed "not in effect" for a
            //   declaration that was in place (11-07 / 11-13: twice read as the cause of the submitting thread's efficiency cores). Before any declaration: this thread, as before
            mach_port_t rp = atomic_load(&g_rt_port), th = rp ? rp : pthread_mach_thread_np(pthread_self());
            unsigned wn = 0; uint32_t wq = dm_guest_work_qos(&wn); thread_extended_info_data_t xi; mach_msg_type_number_t xc = THREAD_EXTENDED_INFO_COUNT; int okx = thread_info(th, THREAD_EXTENDED_INFO, (thread_info_t)&xi, &xc) == KERN_SUCCESS;
            thread_time_constraint_policy_data_t tp; mach_msg_type_number_t tc = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def = 0; int rq = thread_policy_get(th, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tp, &tc, &def) == KERN_SUCCESS;   // def = 1: the real-time policy is not in effect (never set / revoked)
            LOG("[nvn] thread policy: %s real-time computation %.2f ms / period %.2f ms, real-time policy %s, current priority %d (base %d, policy %d); %u worker threads QoS %#x; present QoS %#x", rp ? "acquire thread" : "this thread (no declaration yet)",
                g_rt_comp_ms, g_rt_period_ms, !rq ? "unreadable (thread gone?)" : def ? "not in effect" : "in effect", okx ? xi.pth_curpri : -1, okx ? xi.pth_priority : -1, okx ? xi.pth_policy : -1, wn, wq, nvn_be_present_qos()); }
        py = cy; pk = ck; pw = cw; }
      pi = ci; pc = cc; pd = dr_; pf = frame; }
    uint64_t up, re, fa, pr; nvn_be_pkg_stats(&up, &re); nvn_dirty_stats(&fa, &pr);
    { uint64_t gw, gk, gp; nvn_be_pkg_gpu_stats(&gw, &gk, &gp);
      LOG("[nvn] packaged textures: first uploads %llu, re-uploads on pool content change %llu, skipped as the written pages held the same texture bytes %llu; GPU writes into them %llu, CPU changes left under later GPU writes %llu, partial re-uploads %llu; write-protection faults %llu, pages unprotected before host writes %llu; missed %llu (cumulative)", up, re, nvn_be_pkg_same(), gw, gk, gp, fa, pr, nvn_be_pkg_miss()); }
    memset(g_tn, 0, sizeof g_tn);

#endif
}
// DM_SLOWFRAME=1 (test switch, read over the log port on a real device): per-frame timings are first stored in a ring of the last 16 frames; dropped frames are judged 8 frames late (GPU / on-screen present times are back by then),
// and only for dropped frames is one line "[slowframe]\t<frame number>\t@<name>=<value>..." written to the log, plus one denominator line every 300 frames. No per-frame file is written: the log tee fsyncs every line, and writing line by line would disturb the readings
#define SRN 16
static struct { uint64_t f; int n; const char *k[64]; double v[64]; } g_sr[SRN];
static int g_slow = -1;
static int slow_on(void) { if (g_slow < 0) g_slow = getenv("DM_SLOWFRAME") != NULL; return g_slow; }
static int frame_rec(void) { return g_tsv || slow_on(); }
static void trow(uint64_t f, const char *name, double v) {
    if (g_tsv) fprintf(g_tsv, "%llu\t%s\t%.3f\n", f, name, v);
    if (slow_on()) { __typeof__(g_sr[0]) *r = &g_sr[f % SRN]; if (r->f != f) { r->f = f; r->n = 0; } if (r->n < 64) { r->k[r->n] = name; r->v[r->n++] = v; } }
}
static double srow(uint64_t f, const char *name, double dflt) {
    __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
    if (r->f == f) for (int i = 0; i < r->n; i++) if (!strcmp(r->k[i], name)) return r->v[i];
    return dflt;
}
// Dropped-frame definition: on-screen present interval > the frame's hold (pres_expect_ms: 16.7 ms at 60 fps, 33.3 ms at 30 or with the frame step at 2 vsyncs) ×1.5 or −1; without a window (no on-screen present time) fall back to the present call interval
static void slow_emit(uint64_t f) {
    static uint64_t judged, slow;
    double pm = srow(f, "@presented_ms", 0), fr = srow(f, "@frame_ms", 0), lim = (g_pres_exp_last > 0 ? g_pres_exp_last : 1000.0 / nvn_be_fps_target()) * 1.5;   // called right after that frame's interval was recorded
    int has = nvn_be_has_window(), bad = has ? (pm > lim || pm < 0) : fr > lim;
    if (has ? pm != 0 : fr != 0) judged++;
    if (bad) {
        slow++;
        char b[4096]; size_t o = snprintf(b, sizeof b, "[slowframe]\t%llu", f);
        __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
        for (int i = 0; i < r->n && o < sizeof b - 64; i++) o += snprintf(b + o, sizeof b - o, "\t%s=%.3f", r->k[i], r->v[i]);
        double m = 0; for (uint64_t p = f >= 3 ? f - 3 : 0; p < f; p++) { double x = srow(p, "@frame_ms", 0); if (x > m) m = x; }
        snprintf(b + o, sizeof b - o, "\t@prev3_max_frame_ms=%.3f", m);
        fprintf(stderr, "%s\n", b);
    }
    if (f % 300 == 0) fprintf(stderr, "[slowframe]\t#\tup to frame %llu\tjudged=%llu\tslow=%llu\tcriterion=%s\n", f, judged, slow, has ? "on-screen interval" : "present call interval (no window)");
}
// Declaration for the time-constraint (real-time) thread: period = present period, constraint = period, computation = the CPU time this thread actually needs per frame.
// The computation used to be hard-coded at 2 ms, while the game main thread (game logic + recording + submit replay) takes about 10 ms per frame when hot - a 5× gap between declared and actual:
// a real-time thread that exceeds its declared computation gets demoted by the system, and the performance controller also sets frequency from the declaration (this declaration is how the system learns the frame deadline and the workload).
// DM_RT_COMP=<ms> fixes the declaration; default auto = every 120 frames take the larger of 1.25 × the p90 and 1.1 × the peak of this thread's per-frame CPU time and re-declare if it differs from the declared value by > 15% (capped at 0.9 × period).
// DM_RT=0 leaves the acquire thread on the standard policy (its QoS only), for power A/B: the real-time declaration is how the performance controller learns the frame deadline, and it may hold the clock up for it
static int rt_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_RT"); v = !(e && !strcmp(e, "0")); if (!v) LOG("[nvn] DM_RT=0: the present thread gets no time-constraint policy (QoS only)"); } return v; }
static void set_time_constraint_comp(uint64_t period_ns, uint64_t comp_ns) {
    if (!rt_on() || g_rt_off) return;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    if (comp_ns < 1000000ull) comp_ns = 1000000ull; if (comp_ns > period_ns * 9 / 10) comp_ns = period_ns * 9 / 10;
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(period_ns * tb.denom / tb.numer), .computation = (uint32_t)(comp_ns * tb.denom / tb.numer),
        .constraint = (uint32_t)(period_ns * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) { g_rt_comp_ms = comp_ns / 1e6; g_rt_period_ms = period_ns / 1e6; atomic_store(&g_rt_port, pthread_mach_thread_np(pthread_self())); }
    // 读回内核实际生效的值（10-30：xnu 收到声明后 computation < constraint / 2 会直接抬到 constraint / 2，60 Hz 即 8.33 ms；读回才知道真正交给调度器的是多少）
    thread_time_constraint_policy_data_t g = {0}; mach_msg_type_number_t gc = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t gd = 0;
    int got = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&g, &gc, &gd) == KERN_SUCCESS && !gd;
    LOG("[nvn] present thread time-constraint policy (period %.3f ms, computation %.3f ms) %s; kernel now holds period %.3f / computation %.3f / constraint %.3f ms%s", period_ns / 1e6, comp_ns / 1e6, kr == KERN_SUCCESS ? "set" : "failed to set",
        got ? g.period * (double)tb.numer / tb.denom / 1e6 : 0, got ? g.computation * (double)tb.numer / tb.denom / 1e6 : 0, got ? g.constraint * (double)tb.numer / tb.denom / 1e6 : 0, got ? "" : " (read back failed)");
}
static int rt_comp_fixed_ms(void) { static int v = -2; if (v == -2) { const char *e = getenv("DM_RT_COMP"); v = e && *e && strcmp(e, "auto") ? atoi(e) : -1; } return v; }   // −1 = auto
// The fixed value, or in auto mode the computation declared so far (auto's starting value of 2 ms only the first time): a reset to 2 ms at each period change under-declared the thread until the next p90
static void set_time_constraint(uint64_t period_ns) { int f = rt_comp_fixed_ms(); set_time_constraint_comp(period_ns, f > 0 ? (uint64_t)f * 1000000ull : g_rt_comp_ms > 0 ? (uint64_t)(g_rt_comp_ms * 1e6) : 2000000ull); }
// Called every frame at acquire: records this thread's CPU time since the last call; in auto mode re-declares based on p90
#include "rt_demote.h"
// 每 30 帧核一次本线程是不是还在实时档（被降级的对策见 rt_demote.h）；取图线程上调，只看自己
static void rt_demote_tick(uint64_t period_ns) {
#if defined(__ANDROID__) || defined(__OHOS__)
    (void)period_ns;   // 「实时声明被降级回落优先级 0」是 XNU 的行为；安卓 / 鸿蒙走 mach 兼容层（android/native/nvn/compat/mach），没有标准 / 优先策略，不做
#else
    static int k; static uint64_t last_ns[2];   // 最近两次处理降级的时刻（判「60 秒内第几次」）
    if (!rt_on() || g_rt_off || !g_rt_comp_ms || ++k % 30) return;
    // 测试钩子（缺省关）：DM_RT_TEST_DEMOTE=<第几次检查>[,<第几次>]——到点先把本线程改成标准策略，模拟系统降级（真降级要每帧超出申报，内核把申报至少抬到半个周期，局里逼不出来）
    { static int hooks[2] = {-1, -1}; if (hooks[0] == -1) { const char *e = getenv("DM_RT_TEST_DEMOTE"); hooks[0] = hooks[1] = 0; if (e && *e) sscanf(e, "%d,%d", &hooks[0], &hooks[1]); }
      if (k / 30 == hooks[0] || k / 30 == hooks[1]) { thread_standard_policy_data_t tsp = {0}; thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY, (thread_policy_t)&tsp, THREAD_STANDARD_POLICY_COUNT);
        LOG("[nvn] 测试钩子：第 %d 次检查前把取图线程改成标准策略（模拟系统降级，DM_RT_TEST_DEMOTE）", k / 30); } }
    thread_extended_info_data_t xi; mach_msg_type_number_t xc = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO, (thread_info_t)&xi, &xc) != KERN_SUCCESS) return;
    uint64_t now = now_ns(); int recent = last_ns[0] && now - last_ns[0] < 60000000000ull;
    int a = rt_demote_action(1, xi.pth_policy != POLICY_TIMESHARE, recent);
    if (a == RT_DEMOTE_NONE) return;
    last_ns[0] = now;
    if (a == RT_DEMOTE_REDECLARE) {
        LOG("【红】[nvn] 取图线程的实时声明被系统降级（当前优先级 %d、基础 %d、策略 %d，申报计算量 %.2f ms / 周期 %.2f ms）：当场重新声明",
            xi.pth_curpri, xi.pth_priority, xi.pth_policy, g_rt_comp_ms, g_rt_period_ms);
        double c = g_rt_comp_ms; g_rt_comp_ms = 0; set_time_constraint_comp(period_ns, (uint64_t)(c * 1e6)); return;
    }
    // 改标准策略后再抬优先级：线程一旦用 thread_policy_set 设过策略，pthread_set_qos_class_self_np 就返回 EPERM（本机小程序实测：实时 → 标准后设
    //   USER_INTERACTIVE 回 1、QoS 仍 0）；THREAD_PRECEDENCE_POLICY 仍可用，importance 相对任务基础优先级，按读回的基础优先级算到 47（USER_INTERACTIVE 那一档）
    mach_port_t me = pthread_mach_thread_np(pthread_self());
    thread_standard_policy_data_t sp = {0};
    kern_return_t kr = thread_policy_set(me, THREAD_STANDARD_POLICY, (thread_policy_t)&sp, THREAD_STANDARD_POLICY_COUNT);
    thread_extended_info_data_t yi; mach_msg_type_number_t yc = THREAD_EXTENDED_INFO_COUNT; int base = thread_info(me, THREAD_EXTENDED_INFO, (thread_info_t)&yi, &yc) == KERN_SUCCESS ? yi.pth_priority : 31;
    thread_precedence_policy_data_t pp = {.importance = base < 47 ? 47 - base : 0};
    kern_return_t kp = thread_policy_set(me, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&pp, THREAD_PRECEDENCE_POLICY_COUNT);
    yc = THREAD_EXTENDED_INFO_COUNT; int after = thread_info(me, THREAD_EXTENDED_INFO, (thread_info_t)&yi, &yc) == KERN_SUCCESS ? yi.pth_priority : -1;
    g_rt_off = 1; g_rt_comp_ms = 0;
    LOG("【红】[nvn] 取图线程的实时声明 60 秒内第二次被系统降级（当前优先级 %d、策略 %d）：本局不再声明，改标准策略并抬到优先级 47（标准策略 %s，基础 %d → 抬后 %d、precedence %s），"
        "免得降级后回落到优先级 0、整个进程被搬到能效核", xi.pth_curpri, xi.pth_policy, kr == KERN_SUCCESS ? "已设" : "设置失败", base, after, kp == KERN_SUCCESS ? "已设" : "设置失败");
#endif
}
static void rt_comp_tick(uint64_t period_ns) {
    rt_demote_tick(period_ns);
    if (rt_comp_fixed_ms() > 0 || !period_ns || !g_rt_comp_ms) return;
    static uint64_t last; static double win[120]; static int n;
    uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); if (!last) { last = c; return; }
    win[n++] = (double)(c - last) / 1e6; last = c;
    if (n < 120) return;
    double v[120]; memcpy(v, win, sizeof v); qsort(v, 120, sizeof *v, dcmp); n = 0;
    // Also the window's peak: every frame that ran past the declaration stalled the thread for 30–60 ms. Dark Souls Remastered's render thread needs 1.9–2.0 ms a frame with one or
    // two frames in 120 near 4.4 ms; declared at 2.5 ms (p90 × 1.25) it stalled about once a second, at 30 fps and at 60; declared above the peak it held 60 fps at the same CPU energy
    double p90 = v[107], comp = fmax(p90 * 1.25, v[119] * 1.1), cap = period_ns * 0.9 / 1e6; if (comp > cap) comp = cap; if (comp < 1) comp = 1;
    if (comp > g_rt_comp_ms * 1.15 || comp < g_rt_comp_ms * 0.85) set_time_constraint_comp(period_ns, (uint64_t)(comp * 1e6));
}
// 提交 / 编码线程的实时声明（DM_RT_SUBMIT，按位：1 提交线程、2 编码线程 dm-encode（nvn_rec.h，iOS 缺省开的延后编码），3 两条；iOS 缺省 3、Mac 缺省 0，DM_RT_SUBMIT=0 关）：
//   iPhone 两款凉机 ABBA（30 帧锁定档与 60 帧档各一）帧率 / p95 / 功耗无回归才改的缺省；Mac 没量过、照旧关
//   实时约束只声明在取图线程上；回放 + 提交的那条线程（游戏渲染线程）每帧 8–19 ms，
//   只有 QoS 0x21、没有帧截止时间，性能控制器按普通线程给核给频率。0400486024DEC000 一台测试手机上：这条线程 0% 在性能核、能效核 1.28 GHz，每帧 CPU 19.1 ms、
//   墙钟 20.3 ms > 16.7，帧率 53。这里给它同样的声明（周期 = 取图线程声明的帧周期，计算量按这条线程每帧 CPU 的口径同 rt_comp_tick：120 帧窗口取
//   p90 × 1.25 与峰值 × 1.1 的大者，封顶周期的九成；变动超过 15% 才重报）。与取图线程是同一条线程时不另报。
//   要和功耗一起读：10-30 帧间隔工作组 all 档同样把线程推上性能核，60 帧不变而整机 1.69 → 2.47 W
static _Thread_local struct { uint64_t frame, c0; double win[120], decl; int n; } t_rts;
static void rt_work_tick(int who) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_RT_SUBMIT"); on = e ? atoi(e) & 3 : (TARGET_OS_IPHONE ? 3 : 0); if (on) LOG("[nvn] DM_RT_SUBMIT=%d: the time-constraint policy also goes to the %s", on, on == 3 ? "submit and encode threads" : on == 1 ? "submit thread" : "encode thread"); }
    if (!(on & who) || g_rt_period_ms <= 0 || atomic_load(&g_rt_port) == pthread_mach_thread_np(pthread_self())) return;
    uint64_t f = dm_nvn_frame_count(), c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    if (!t_rts.frame) { t_rts.frame = f; t_rts.c0 = c; return; }
    if (f == t_rts.frame) return;
    t_rts.win[t_rts.n++] = (double)(c - t_rts.c0) / 1e6 / (double)(f - t_rts.frame); t_rts.frame = f; t_rts.c0 = c;
    if (t_rts.n < 120) return;
    double v[120]; memcpy(v, t_rts.win, sizeof v); qsort(v, 120, sizeof *v, dcmp); t_rts.n = 0;
    double period = g_rt_period_ms, comp = fmax(v[107] * 1.25, v[119] * 1.1), cap = period * 0.9; if (comp > cap) comp = cap; if (comp < 1) comp = 1;
    double eff = fmax(comp, period / 2);   // 内核把小于约束一半的计算量抬到一半（读回已证）：按生效值判变动，免得 3 → 4 ms 这类没变化的重报
    if (t_rts.decl > 0 && eff <= t_rts.decl * 1.15 && eff >= t_rts.decl * 0.85) return;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    thread_time_constraint_policy_data_t pol = {.period = (uint32_t)(period * 1e6 * tb.denom / tb.numer), .computation = (uint32_t)(comp * 1e6 * tb.denom / tb.numer),
        .constraint = (uint32_t)(period * 1e6 * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    thread_time_constraint_policy_data_t g = {0}; mach_msg_type_number_t gc = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t gd = 0;
    int got = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&g, &gc, &gd) == KERN_SUCCESS && !gd;
    if (kr == KERN_SUCCESS) t_rts.decl = eff;
    LOG("[nvn] %s thread time-constraint policy (period %.3f ms, computation %.3f ms; its CPU a frame p90 %.2f / peak %.2f ms) %s; kernel now holds computation %.3f / constraint %.3f ms%s",
        who == 1 ? "submit" : "encode", period, comp, v[107], v[119], kr == KERN_SUCCESS ? "set" : "failed to set", got ? g.computation * (double)tb.numer / tb.denom / 1e6 : 0, got ? g.constraint * (double)tb.numer / tb.denom / 1e6 : 0, got ? "" : " (read back failed)");
}
void dm_rt_encode_tick(void) { rt_work_tick(2); }   // nvn_rec.h 编码线程每处理完一块调
static void sleep_until_ns(uint64_t dl) {   // absolute time on the monotonic clock
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = now_ns(); if (dl <= now) return;
    t_pace_waits++;
    DM_BLOCK_BEGIN(); mach_wait_until(mach_absolute_time() + (dl - now) * tb.denom / tb.numer); DM_BLOCK_END();
}
// Present is fixed at 60 Hz: the adaptive mechanism stepped down at frame 60 in 24/24 real-device logs and never came back up, so for this phase 60 fps is the hard floor.
// DM_PRESENT_HZ=0 (uncapped frame rate) serves only as an explicit performance profiling switch: some of the game's UI logic counts frames (with the same save, uncapped gives
// 4181 SaveIndicatorPanel entries and capped at 60 fps gives 0, which matches the ground truth), so uncapped is a semantically wrong way to run and must not be used as a criterion / cross-check / default. Unit tests set it directly with nvn_present_config
// 降档原因（runtime/src/fps.c：0 无、1 热状态 ≥ 严重、回到一般后稳 30 秒恢复、2 低电量）；弱定义 = 没有帧率表模块的宿主（单测）不降档
__attribute__((weak)) int s2a_fps_cap_reason(void) { return 0; }
__attribute__((weak)) uint64_t nvn_be_args_patched(void) { return 0; }   // nvn_mtl.m: argument blocks patched (only uniform slots written again); other backends none
// runtime/src/fps.c：帧率表键页槽的当前值（每 300 帧那行带出）；Metal 后端 nvn_mtl.m：本窗口最大渲染目标。别的后端 / 单测替身没有就报空 / 0
__attribute__((weak)) void s2a_fps_slots_note(char *buf, size_t n) { (void)n; buf[0] = 0; }
__attribute__((weak)) void nvn_be_rt_window(uint32_t *w, uint32_t *h, uint32_t *fmt) { *w = *h = *fmt = 0; }
static int g_hz = -1, g_display_mode;
static _Atomic int g_game_itv = 1;   // when pacing by the game's interval, the interval the game currently sets (in game mode 2 = 30 fps; nvn_game_fps reports it to the graphics layer, which sets the display link from it)
void nvn_present_config(int hz, int display_mode) { g_hz = hz; g_display_mode = display_mode; }
// 社区帧率补丁跟随（runtime/src/cpatch.c fps_link，11-13 补丁优先）：局中开 / 关目标 > 60 帧的补丁时改出图节拍；还没定节拍（g_hz < 0）时不动，起局读 DM_PRESENT_HZ
void nvn_present_hz(int hz) { if (hz > 0 && g_hz > 0 && hz != g_hz) { LOG("[nvn] present tick %d Hz → %d Hz（社区帧率补丁）", g_hz, hz); g_hz = hz; } }
static _Atomic uint64_t g_itv2_since;   // start of the game's current run of intervals >= 2 (now_ns; 0 = it is 1 now)
// The frame rate the game itself asks for: 30 in game mode when the present interval has been >= 2 for a full second (the display link follows at 30 Hz, nvn_be_fps_target), otherwise 60; back to 60 at once when the interval returns to 1. Intervals of 3 or more (20 fps or less) have no matching display-link mode and run at 30.
// The one-second hold: Mario 1.2.1 picks the interval from the previous frame's time (main+0xcfd24), setting 2 for the frame after a slow one and then 1 again (123 times in two minutes on an iPhone). Following that frame by frame would flap the display-link mode;
//   a single interval-2 frame is instead kept on screen for its own period by the display-link callback, and the next acquire waits as many ticks (nvn_be_link_wait_tick, nvn_mtl.m), without changing the mode
// Frame step (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3, Breath of the Wild 1.9.0): the game no longer picks an interval. The host hands it each frame's step in whole vsyncs
// (fstep_tick, nvn_fstep.h) and presents follow the hold k it decides with it
__attribute__((weak)) uint8_t *dm_fps_state;   // loader.c; graphics unit tests have no loader
__attribute__((weak)) void dm_fps_lock_tick(void) {}   // hle_fpslock.c: engine frame limits in game memory (Monster Hunter Rise); graphics unit tests have no host
// S2Arm 视频按片源帧率出图（nvn_vpace.h）：Ryujinx Nvdec 每解一帧调 s2a_video_frame（services/aot/Compose.cs 挂 NvdecDevice.S2AFrameDecoded）；
//   放 ≤ 30 帧视频时出图封顶 30、显示链接 30 Hz（帧步款经 fps_step_cap，其余款经 present_itv / nvn_game_fps）。S2A_VIDEO_PACE=0 关
static nvn_vpace g_vid; static pthread_mutex_t g_vid_mu = PTHREAD_MUTEX_INITIALIZER; static int g_vid_off = -1; static int g_vid_cap;
static uint64_t g_vid_t, g_vid_f, g_vid_d; static double g_vid_dpf = -1; static int g_vid_busy_said;   // 每帧绘制数（每 ≥ 0.25 秒按绘制数 ÷ 出图数重算一次，g_vid_mu 下）
void s2a_video_frame(void) { uint64_t t = now_ns(); pthread_mutex_lock(&g_vid_mu); nvn_vpace_frame(&g_vid, t); pthread_mutex_unlock(&g_vid_mu); }
// 带片源时间戳的一帧（Switch 2 解码器，nvn2_codec.c cv_emit）：片源帧率按时间戳判，见 nvn_vpace.h
void s2a_video_frame_pts(int64_t pts_ns) { uint64_t t = now_ns(); pthread_mutex_lock(&g_vid_mu); nvn_vpace_frame_pts(&g_vid, t, pts_ns); pthread_mutex_unlock(&g_vid_mu); }
static int video_cap(void) {
    if (g_vid_off < 0) { const char *e = getenv("S2A_VIDEO_PACE"); g_vid_off = e && !strcmp(e, "0"); }
    if (g_vid_off) return 0;
    uint64_t t = now_ns(), ps, dr; nvn_be_stats(&ps, &dr);
    pthread_mutex_lock(&g_vid_mu);
    if (t - g_vid_t >= 250000000ull) { uint64_t f = g_frame; if (g_vid_t && f > g_vid_f) g_vid_dpf = (double)(dr - g_vid_d) / (double)(f - g_vid_f); g_vid_t = t; g_vid_f = f; g_vid_d = dr; }
    int was = g_vid_cap, on = g_vid_cap = nvn_vpace_cap(&g_vid, t, g_vid_dpf), playing = g_vid.on; double dpf = g_vid_dpf;
    int say_busy = playing && !on && !g_vid_busy_said && dpf > VP_DRAW_MAX; if (say_busy) g_vid_busy_said = 1;
    pthread_mutex_unlock(&g_vid_mu);
    if (on != was) LOG("[nvn] 视频按片源帧率出图：%s（每帧绘制 %.0f）", on ? "在放 ≤ 30 帧视频，出图封顶 30" : "视频停了或游戏在照常渲染，出图恢复", dpf);
    if (say_busy) LOG("[nvn] 视频按片源帧率出图：在解 ≤ 30 帧视频，但游戏每帧绘制 %.0f（> %d，视频只是画面里的一块），不封顶", dpf, VP_DRAW_MAX);
    return on;
}
// 档位梯子压回 30 帧档（runtime/src/fps.c s2a_fps_hold → 降档原因 4，#1766）：出图同放视频一路封顶 30（至少隔 2 拍、显示链接 30 Hz）。
//   梯子只撤社区帧率补丁；有的补丁改的设定游戏只在起局读一次（一款 Unity 游戏读到 vSyncCount 1、起局设交换间隔 1）：撤了补丁游戏逻辑回到按 30 帧算，
//   交换间隔却还是 1，出图照 60 → 两倍速。30 帧档就按 30 出图；游戏自己设间隔 2 的款本来就是 30，不变
static int ladder30(void) { return s2a_fps_cap_reason() == 4; }
static int cap30(void) { return video_cap() || ladder30(); }   // 先算 video_cap：它顺带记视频开始 / 结束的日志
static int fps_step_cap(void) {
    if (!dm_fps_state) return 0;
    uint32_t b = __atomic_load_n((uint32_t *)dm_fps_state, __ATOMIC_RELAXED); float t; memcpy(&t, &b, sizeof t);
    return t <= 0 ? 0 : __atomic_load_n((uint32_t *)(dm_fps_state + 4), __ATOMIC_RELAXED) || t <= 30 || video_cap() ? 30 : 60;
}
static int fstep_on(void) { return fps_step_cap() != 0; }   // the host decides the steps in whole vsyncs
static nvn_fstep g_fs;   // present thread only
static nvn_fgpu g_fg;   // the GPU time at kmin and at the current hold (nvn_fstep_gpu_slowed)
// Each frame's critical path starts where its submits began: the present call less the submits' wall time that frame (the DM_NVN_TSV @call_ms − @sub_wall_ms), kept per frame until its GPU work is done
static struct { uint64_t f, t; } g_cpath[16];
static _Atomic int g_fs_k;   // the hold presentation follows (0 = not decided yet)
// The display link runs at 30 Hz at a 30 cap, except while the game fixes its own step at 1 (BotW's VFR override)
int nvn_game_fps(void) { int c = fps_step_cap(); if (c) return c == 30 && atomic_load(&g_fs_k) == 1 && fstep_on() ? 60 : c; if (g_hz > 0 && cap30()) return 30; uint64_t s = atomic_load(&g_itv2_since); return g_hz > 0 && !g_display_mode && s && now_ns() - s >= 1000000000ull ? 30 : 60; }
static void present_cfg(const win_t *o) {
    if (g_hz >= 0) return;
    // The interval mode comes from the game profile (present_interval, default game): whether frames follow the present interval the game sets depends on whether the game's logic counts frames, which differs per game. Environment variables override it, and can override the fixed 60 Hz.
    int scr = nvn_be_display_hz();
    const char *e = getenv("DM_PRESENT_HZ"); g_hz = e ? atoi(e) : 60;
    if (!g_hz) LOG("[nvn] [WARNING] DM_PRESENT_HZ=0: frame rate uncapped, frame pacing unlike the device, UI logic may drift (for profiling only, not for verdicts / cross-checks)");
    // Cut (intentional deviation from the original game's behavior): with DM_PRESENT_INTERVAL=display the present interval set by the game is not followed and one frame is presented per display-link tick,
    // with a fixed hard floor of 60 fps; the logic stays at 25 ticks, and the client's frame-counted UI logic is unaffected -
    // with the same save and the same input, the UI message streams of the two modes match message by message. Proof (before character select): r9 game / display 60 ×3 / display 120 ×2, 6 groups in total, uicmp consistent
    // (the save indicator is exempted as a timing race), and the timing differences of key messages are within game mode's own noise band; proof after entering a run is still missing, pending action script coverage
    const char *m = getenv("DM_PRESENT_INTERVAL"); g_display_mode = m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY;
    const char *src = m ? "DM_PRESENT_INTERVAL 环境变量" : DM_GAME_PRESENT_DISPLAY ? "推导（引擎标记 + L3 判定）或显式 game.tsv" : "缺省（game）";
    LOG("[nvn] present tick %d Hz, interval mode %s（依据：%s；游戏设 interval %d；%s）", g_hz, g_display_mode ? "display（每个显示节拍一帧）" : "game（按游戏设的间隔）",
        src, o->interval, scr > 0 ? "window, fixed 60 Hz" : "no window, fixed 60 Hz");
}
// S2Arm 帧率表（runtime/src/fps.c，10-26）：每帧在出图入口（nvn_be_set_frame 之后）调一次，写动态步长；返回 1 = 这一帧出图间隔退回游戏自己设的（如塞尔达预渲染过场）。没有帧率表 / 单测里是这个弱定义
__attribute__((weak)) int s2a_fps_present_hook(void) { return 0; }
__attribute__((weak)) int s2a_fps60_display_source(void) { return 0; }   // runtime/src/caps.c；没链能力清单的宿主 / 图形单测没有清单来源
__attribute__((weak)) void s2a_qp_frame(uint32_t draws, int video, int cutscene, int game_itv) { (void)draws; (void)video; (void)cutscene; (void)game_itv; }   // runtime/src/qprompt_host.c；图形单测没有宿主
static _Atomic int g_fps_game_itv;   // 这一帧按游戏自己设的间隔（不走 display 模式）
// 社区帧率补丁改在 sdk 里 NVN 驱动的交换间隔（D-36F4：真机上游戏照旧调 nvnWindowSetPresentInterval(2)，被补丁改过的驱动按 1 交帧）：
//   直连层接管了 NVN，sdk 里那段驱动代码不执行，游戏传的 2 原样进出图节拍、补丁开着仍 30 帧。补丁开着时 runtime/src/cpatch.c 经这里把间隔封到补丁意图（1；FPSLocker 配置同理，#1517：真机由 NX-FPS 插件锁游戏设的间隔），
//   关掉 / 停帧救场时回 0（不封，跟游戏）。只封出图节拍用的间隔；游戏读回的 nvnWindowGetPresentInterval 仍是它自己设的（同真机：补丁改的是驱动交帧，不改游戏的变量）
static _Atomic int g_itv_cap;
void nvn_present_itv_cap(int cap) {
    if (cap < 0) cap = 0;
    if (atomic_exchange(&g_itv_cap, cap) != cap) LOG("[nvn] 出图间隔上限 %d（%s）", cap, cap ? "社区帧率补丁开着（改 sdk 里驱动交帧的一类，或 FPSLocker 配置：真机 NX-FPS 插件锁游戏设的间隔），直连层按补丁意图交帧" : "不封，跟游戏设的间隔");
}
static int game_itv(const win_t *o) {
    int v = o->interval > 0 ? o->interval : 1, cap = atomic_load(&g_itv_cap);
    return cap > 0 && v > cap ? cap : v;
}
static int present_itv(const win_t *o) {   // vsyncs per frame that presentation follows
    int c = fps_step_cap(), k = atomic_load(&g_fs_k);
    if (c) return k && fstep_on() ? k : 60 / c;
    int itv = g_display_mode && !atomic_load(&g_fps_game_itv) ? 1 : game_itv(o);
    return itv < 2 && g_hz > 0 && cap30() ? 2 : itv;   // 放 ≤ 30 帧视频 / 档位梯子 30 帧档：至少隔 2 拍（60 Hz 节拍下 30 帧）
}
// 显示链等几个 60 Hz vsync（nvn_be_link_wait_tick 的口径；0 = 一个真实链接拍）：出图节拍被放开到 60 Hz 以上（g_hz，社区帧率补丁经 DM_PRESENT_HZ / nvn_present_hz）时，
//   游戏间隔按 g_hz 折算，与 present_period 同一口径。原先直接传游戏间隔，链路永远按 60 Hz 等：节拍 120 Hz、间隔 1 仍只出 60 帧（11-13 实机：每 600 拍只交 300 帧，
//   GPU 只忙 54% 也一样）。补丁在机器上做的是把交换间隔改成 0（不等垂直同步），这里间隔 1 在 120 Hz 下即等一个真实 120 Hz 拍。宿主帧步（fps_step_cap）自有 60 Hz 口径，不折算
static int link_vsyncs(const win_t *o) {
    int itv = present_itv(o);
    return g_hz > 60 && !fps_step_cap() ? itv * 60 / g_hz : itv;
}
// S2Arm 标称帧率（自动玩家折算路线帧数；host_adapt.m 每次上屏报给原生层 s2a_nominal_fps_set）：出图实际跟的节拍 = 节拍频率 / present_itv，任意线程可调。
//   与 nvn_game_fps（显示链接的频率）只差在 display 模式这一帧退回游戏间隔时（帧率表运行中关 / 过热封顶，g_fps_game_itv）：显示链接仍 60 Hz、出图却隔 interval 拍一帧，这里按游戏间隔给。
//   游戏间隔那一路同 nvn_game_fps 的 1 秒保持：单帧的间隔 2（马力欧按上一帧耗时挑间隔）不算换节拍，路线一步的折算不会被一帧的抖动翻倍
int nvn_present_fps(void) {
    int c = fps_step_cap(), hz = g_hz > 0 ? g_hz : 60;
    if (c) { int k = atomic_load(&g_fs_k); return hz / (k && fstep_on() ? k : 60 / c); }
    if (g_hz > 0 && cap30()) { int itv = atomic_load(&g_game_itv); return hz / (itv > 2 && !g_display_mode ? itv : 2); }
    if (g_display_mode && !atomic_load(&g_fps_game_itv)) return hz;
    uint64_t s = atomic_load(&g_itv2_since); int itv = atomic_load(&g_game_itv);
    return s && itv > 1 && now_ns() - s >= 1000000000ull ? hz / itv : hz;
}
static uint64_t present_period(const win_t *o) {
    present_cfg(o);
    int was = atomic_load(&g_game_itv), want = game_itv(o);
    if (want != was) {
        atomic_store(&g_game_itv, want);
        // 奇数间隔在 30 Hz 链路上没有整刻度（间隔 3 = 1.5 刻度，原先取整成 2 刻度 67 ms）：只有偶数间隔计入「降到 30 Hz」的连续段，
        //   奇数间隔立即回 60 Hz 口径（11-04 金刚真机间隔 2↔3 来回切、帧时间全是 33.3 ms 倍数）
        if (want < 2 || want % 2) atomic_store(&g_itv2_since, 0);
        else if (was < 2 || was % 2) atomic_store(&g_itv2_since, now_ns() | 1);   // | 1: the start is never 0
    }
    int itv = present_itv(o), hz = g_hz;
    // 上限作用于最终周期，不能把基准 60 Hz 降到 30 后再乘 interval（interval 2 会误成 15 帧）。
    uint64_t period = nvn_fps_period_ns(hz > 0 ? (uint64_t)itv * 1000000000ull / (uint64_t)hz : 0);
#if defined(__ANDROID__) || defined(__OHOS__)
    // #48 A4：系统省电档对最终周期取 max(原周期, 33.3 ms)；A3：登记最终周期给刷新率投票。
    period = s2a_android_limit_frame_period(period);
    s2a_android_present_note(period);
#endif
    return period;
}
// The period the acquire thread declares to the scheduler (set_time_constraint): the present period, except that a hold the frame step raised for late frames keeps the cap's deadline.
// Declaring the longer hold let the performance controller move the game to the efficiency cores at half the clock (TOTK on an iPhone: 3.4 → 1.5 GHz and 65% → 5% on performance
// cores within 10 frames of a step to 30), where 60 fps no longer fits and every trial back to it fails. A step the game fixes itself (BotW's VFR override) is declared as it is
static uint64_t rt_period_ns(const win_t *o, uint64_t period) {
    int c = fps_step_cap();
    if (!period || !c || !fstep_on() || __atomic_load_n((uint32_t *)(dm_fps_state + 0x24), __ATOMIC_RELAXED)) return period;
    int kmin = 60 / c;
    return present_itv(o) > kmin ? (uint64_t)kmin * 1000000000ull / (uint64_t)g_hz : period;
}
// Whether the CPU runs below its speed for reasons other than heat (nvn_fstep_next's slow, which allows probes): under 30% of the process's CPU time on performance cores in
// each of the last two windows of 30 presents, on a system with two core types, at thermal state nominal or fair, Low Power Mode off. A game that waits out a long hold is parked
// on the efficiency cores (TOTK on an iPhone: 6% on performance cores at 1.6 GHz, thermal 0), where the cost the frame step measures never fits k − 1 again; one window alone
// dips that low on a Mac at 30 fps now and then (26% once in 70 s, 97% around it)
static int g_fs_pshare = -1;   // the last window's share in percent (−1 = not measured yet)
static int fstep_cpu_slowed(uint64_t presents) {
    static int levels = -1, slowed, low_was; static uint64_t pc, pp;
    if (levels < 0) { size_t n = sizeof levels; if (sysctlbyname("hw.nperflevels", &levels, &n, 0, 0)) levels = 1; }
    if (levels < 2 || presents % 30) return levels >= 2 && slowed;
    task_power_info_v2_data_t pw; mach_msg_type_number_t n = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&pw, &n) != KERN_SUCCESS) return slowed;
    uint64_t c = pw.cpu_energy.total_user + pw.cpu_energy.total_system, p = pw.task_ptime;   // both in mach time units, as in power_line
    if (pc && c > pc) g_fs_pshare = (int)(100 * (p - pp) / (c - pc));
    pc = c; pp = p;
    int low = g_fs_pshare >= 0 && g_fs_pshare < 30;
    slowed = low && low_was && dm_thermal_now() <= 1 && !dm_low_power_now(); low_was = low;
    return slowed;
}
// Whether late frames may raise the hold above the cap's vsyncs (profile fps_hold, DM_FPS_HOLD=on / off). Off for a game whose updates do not wait for the presents and step
// by the cap whatever the hold (Age of Calamity): holding frames there only paces the screen, and an iPhone lowers the GPU clock to the longer hold, so frames got slower and
// the hold never came back (battle GPU 10 → 41 ms at the same draws, 20 fps at thermal state 0); off, each frame is shown as soon as it is ready
static int fstep_hold(void) {
    static int h = -1;
    if (h < 0) {
        const char *e = getenv("DM_FPS_HOLD"); h = e ? strcmp(e, "off") != 0 : DM_GAME_FPS_HOLD;
        if (!h) LOG("[nvn] frame step: frames are not held longer than the cap's vsyncs (%s)", e ? "DM_FPS_HOLD" : "game profile fps_hold");
    }
    return h;
}
// Once per present, before this frame's period is taken: the time since the previous frame's start (shown) and the frame's cost decide this frame's hold and the next frame's step.
// Cost = max(CPU: the time between the frames' starts without the pacing wait, GPU: the latest finished frame's GPU time), in vsyncs
static void fstep_tick(const win_t *o) {
    static uint64_t prev, t0, v0, fixed_sum, held[FSTEP_MAX + 1]; static int nlog;
    const double v = 1e9 / 60;
    uint64_t st = g_fs_start ? g_fs_start : now_ns(), pw = g_fs_wait; g_fs_start = g_fs_wait = 0;
    double shown = prev ? (st - prev) / v : 0, gpu = 0; prev = st;
    // The frame's work: its critical path, from its submits' start to its GPU work's end (latest frame with the GPU done). 11-02 direct line, job8 timestamps: the submits are
    //   committed in pieces (sub_cut, about 12 a frame), so the GPU starts 1.5 ms after the CPU and the two overlap; at hold 3, CPU start → GPU end was 27 ms (fits hold 2), while
    //   the start-to-start time less the pacing wait counted the submit thread's waits on the GPU too (35 ms), so a failed probe stepped up to 3 (20 fps) and hold 2 never fitted again.
    //   Whether a frame was late is still read from the screen (shown); without a critical path yet (first frames, a frame without GPU work) the old measure stands in
    double cpath = 0;
    for (uint64_t f = g_frame; f-- > 0 && g_frame - f <= 8;) {
        double g = nvn_be_gpu_ms(f); if (g < 0) continue;
        gpu = g * 1e6 / v; uint64_t g0, g1;
        if (g_cpath[f % 16].f == f && nvn_be_gpu_span(f, &g0, &g1) && g1 > g_cpath[f % 16].t) cpath = (g1 - g_cpath[f % 16].t) / v;
        break;
    }
    double cpu = shown - pw / v, cost = cpath > 0 ? (cpath > gpu ? cpath : gpu) : cpu > gpu ? cpu : gpu;
    int c = fps_step_cap(), kmin = 60 / c, kmax = !fstep_hold() ? kmin : kmin >= 2 ? 2 : FSTEP_MAX, k0 = g_fs.k, kmin0 = g_fs.kmin, probe0 = g_fs.probe;
    int fixed = (int)__atomic_load_n((uint32_t *)(dm_fps_state + 0x24), __ATOMIC_RELAXED);
    uint64_t probes0 = g_fs.probes, failed0 = g_fs.failed;
    // A screen whose rate is not a multiple of 60 (144, 165 Hz) shows the link's 1/60 s ticks on their nearest refreshes (nvn_mtl.m link_range), so a frame on time can show for one
    // refresh more than its hold: a cheap 50 ms frame shows 54.5 ms at 165 Hz, which counted as late kept the hold at 20 fps for good
    int hz = front_screen_max_fps(); g_fs.shown_slack = hz > 60 && hz % 60 && nvn_be_link_active() ? 60.0 / hz : 0;
    int cslow = fstep_cpu_slowed(o->presents), gslow = nvn_fstep_gpu_slowed(&g_fg, gpu, k0 ? k0 : kmin, kmin, probe0) && dm_thermal_now() <= 1 && !dm_low_power_now();   // same gates as the CPU's
    // On-screen verdict for the step-up window (11-06 BotW, test device 1: start-to-start late 26–30 of 120 frames, presented late 6–7, and the hold went to 30 fps):
    //   frame P stays steps[P] vsyncs, so presented(P+1) − presented(P) against it. Read 8 presents late (the presented callback has arrived by then, as for @presented_ms);
    //   no presented time (no window, dropped by the system, not arrived) leaves it unknown and the start-to-start time counts
    static uint8_t steps[16];
    if (o->presents > 9) {
        uint64_t pn = o->presents - 8; double a_ = nvn_be_presented_time(pn), b_ = nvn_be_presented_time(pn - 1); int k_ = steps[(pn - 1) % 16];
        if (a_ > 0 && b_ > 0 && k_) g_fs.onscreen = (a_ - b_) * 60 > k_ + FSTEP_LATE + g_fs.shown_slack ? 2 : 1;
    }
    int n = nvn_fstep_next(&g_fs, shown, cost, kmin, kmax, fixed, cslow || gslow);
    steps[o->presents % 16] = (uint8_t)n;
    __atomic_store_n((uint32_t *)(dm_fps_state + 0x20), (uint32_t)n, __ATOMIC_RELAXED); atomic_store(&g_fs_k, g_fs.k);
    held[g_fs.k]++; if (fixed) fixed_sum += (uint64_t)n;   // the stubs count only the host's steps
    static int fixed_was, nfix;
    if (fixed != fixed_was && nfix < 40) { nfix++; if (fixed) LOG("[nvn] frame step: the game fixes its own step at %d vsyncs at present %llu", fixed, (unsigned long long)o->presents); else LOG("[nvn] frame step: the game hands the step back to the host at present %llu", (unsigned long long)o->presents); }
    fixed_was = fixed;
    if (k0 && g_fs.k != k0 && nlog < 40) {
        char why[160], held[48] = ""; int w = snprintf(why, sizeof why, "probe");
        if (cslow) w += snprintf(why + w, sizeof why - w, ", CPU %d%% on performance cores", g_fs_pshare);
        if (gslow) w += snprintf(why + w, sizeof why - w, ", GPU %.2f× its time at hold %d", g_fg.cur / g_fg.ref, kmin);
        if (nvn_fstep_same(&g_fs)) snprintf(why + w, sizeof why - w, ", cost %.2f× the cost that held hold %d", g_fs.ccur / g_fs.cref, kmin);
        if (g_fs.k > k0 && g_fs.cref > 0) snprintf(held, sizeof held, "; hold %d held at %.2f", kmin, g_fs.cref);   // what a probe compares the longer hold's cost with
        LOG("[nvn] frame step: hold %d → %d vsyncs at present %llu (%s; cost %.2f vsyncs, GPU %.2f%s)%s", k0, g_fs.k, (unsigned long long)o->presents,
            fixed ? "the game's own step" : kmin0 > FSTEP_MAX ? "the game's own step ended" : kmin != kmin0 ? "the cap changed" : g_fs.k > k0 ? "3 late frames" : g_fs.probes != probes0 ? why : "trial", cost, gpu, held, ++nlog == 40 ? "; further changes only counted below" : "");
    }
    if (probe0 == 1 && g_fs.probe != 1 && !fixed && nlog < 40)   // whether the controller raised the clock for the shorter deadline
        LOG("[nvn] frame step: the probe %s at present %llu (CPU %d%% on performance cores)%s", g_fs.probe == 2 ? "held" : "failed", (unsigned long long)o->presents, g_fs_pshare, ++nlog == 40 ? "; further changes only counted below" : "");
    else if (probe0 == 2 && !g_fs.probe && g_fs.failed != failed0 && nlog < 40)
        LOG("[nvn] frame step: the run at hold %d after the probe lasted under %.0f s, counted as a failed probe (CPU %d%% on performance cores)%s", kmin, FSTEP_HOLD_MAX / 60, g_fs_pshare, ++nlog == 40 ? "; further changes only counted below" : "");
    if (o->presents % 1800 == 0) {   // game time against real time (1.000 = normal speed; lower when a device cannot hold 20 fps or after long stalls)
        uint64_t vs = __atomic_load_n((uint64_t *)(dm_fps_state + 0x30), __ATOMIC_RELAXED) + fixed_sum, t = now_ns();
        if (t0) LOG("[nvn] frame step: game time %.2f s in %.2f s (%.3f of real time); held 1 / 2 / 3 vsyncs %llu / %llu / %llu frames, %llu late, %llu vsyncs paid back, %.0f dropped, %llu up / %llu down (%llu probes, %llu failed trials)",
                    (vs - v0) / 60.0, (t - t0) / 1e9, (vs - v0) / 60.0 / ((t - t0) / 1e9), (unsigned long long)held[1], (unsigned long long)held[2], (unsigned long long)held[3],
                    (unsigned long long)g_fs.late, (unsigned long long)g_fs.paid, g_fs.dropped, (unsigned long long)g_fs.ups, (unsigned long long)g_fs.downs, (unsigned long long)g_fs.probes, (unsigned long long)g_fs.failed);
        t0 = t; v0 = vs; memset(held, 0, sizeof held);
    }
}
// 首遇现编 / 首传贴图的帧（档位梯子不把这类一次性卡顿算慢帧，runtime/src/qladder_host.c）：有就记成 2、每次取减 1——
//   梯子在上屏回调里取，现编可能落在这次上屏之前或之后一帧，记两次上屏覆盖这一前一后
static _Atomic int g_frame_hitch;
int s2a_frame_hitch_take(void) { int v = atomic_load(&g_frame_hitch); while (v > 0 && !atomic_compare_exchange_weak(&g_frame_hitch, &v, v - 1)) {} return v > 0; }
NVN(nvnQueuePresentTexture, void, NVNqueue *q, NVNwindow *w, int idx) {
    if (dm_gfx_session_stopping()) return;
    win_t *o = (win_t *)w; o->presents++; nat_itv_sync(o);
    win_clock_fresh(o);   // 读档后没取图先上屏（读档前已取在手里的那张）同样先清，不然节拍器拿旧目标排槽
    if (q) { pthread_mutex_lock(&g_sub_mu); nvn_be_xq_flush(q, "a present on it"); pthread_mutex_unlock(&g_sub_mu); }   // 上屏排在这条队列的工作后面：停在跨队列等待上的部分先放行（nvn_xqueue.inc）
    nvn_be_set_present_lower_left(o->present_ll);   // 这扇窗口建时的原点定上屏翻转（见 nvnWindowInitialize）
    if (q && dev_ok(((queue_t *)q)->dev) && atomic_load(&g_cur_dev) != ((queue_t *)q)->dev) { pthread_mutex_lock(&g_sub_mu); dev_follow(((queue_t *)q)->dev); pthread_mutex_unlock(&g_sub_mu); }   // 上屏翻转跟上屏队列的设备
    {   // 节拍主人：被上屏这张的取图线程（取图线程未知 = 上屏线程自己）；连续 PACE_SWITCH 次同一个才换
        uintptr_t by = idx >= 0 && idx < 4 ? atomic_load_explicit(&g_acq_thr[idx], memory_order_relaxed) : 0; if (!by) by = (uintptr_t)pthread_self();
        if (by == g_pace_cand) { if (g_pace_run < PACE_SWITCH) g_pace_run++; } else { g_pace_cand = by; g_pace_run = 1; }
        uintptr_t cur = atomic_load_explicit(&g_present_thr, memory_order_relaxed);
        if (!cur || (cur != by && g_pace_run >= PACE_SWITCH)) {
            if (cur && cur != by) LOG("[nvn] frame pacing owner: thread %#lx → %#lx (the thread acquiring the presented textures, %d presents in a row)", (unsigned long)cur, (unsigned long)by, g_pace_run);
            atomic_store_explicit(&g_present_thr, by, memory_order_relaxed); } }
    dm_fps_lock_tick();
    if (dm_fps_state) { static int was; int c = fps_step_cap(); if (c != was) { LOG("[nvn] frame step cap %d → %d fps at present %llu%s", was, c, (unsigned long long)o->presents, c == 30 && was == 60 ? " (prerendered movie)" : ""); was = c; } }
    if (fstep_on()) fstep_tick(o);
    static uint64_t prev_in, prev_out, prev_pass, prev_draw;
    present_cfg(o);
    {
        // Test switch DM_TEST_PRESENT_LAG=every N presents,milliseconds[,presents in a row][;…]: sleep that long before the Nth, 2Nth… present (and the ones after it in the run), imitating an occasional slow frame on a phone (Mario 1.2.1 then sets present interval 2; checks that the display link holds frames by interval),
        // or with a run of 3 a hitch that steps the frame step up ("1,10;1200,30,3": 10 ms more on every frame, and 30 more on three in a row every 1,200). Not set in the product
        static int lag_k = -1, lag_n[4], lag_ms[4], lag_run[4];
        if (lag_k < 0) {
            const char *e = getenv("DM_TEST_PRESENT_LAG"); lag_k = 0;
            while (e && *e && lag_k < 4) {
                int n = 0, ms = 0, r = 1, c = sscanf(e, "%d,%d,%d", &n, &ms, &r);
                if (c < 2 || n <= 0) break;
                lag_n[lag_k] = n; lag_ms[lag_k] = ms; lag_run[lag_k] = c == 3 && r > 1 ? r : 1;
                LOG("DM_TEST_PRESENT_LAG: every %d presents, sleep an extra %d ms (%d presents in a row)", n, ms, lag_run[lag_k]); lag_k++;
                if ((e = strchr(e, ';'))) e++;
            }
        }
        int lag = 0; for (int i = 0; i < lag_k; i++) if (o->presents % (uint64_t)lag_n[i] < (uint64_t)lag_run[i]) lag += lag_ms[i];
        if (lag) usleep((useconds_t)lag * 1000);
    }
    gfx_enter();
    uint64_t t0 = now_ns();
    int crop[4]; dm_vi_layer_crop(crop); if (!crop[2] || !crop[3]) memcpy(crop, o->crop, sizeof crop);   // 上屏区域 = 系统窗口裁切：游戏最后设的显示层裁切，没设就是窗口自己的
    if (idx >= 0 && idx < o->ntex && idx < 4) {
        pthread_mutex_lock(&g_sub_mu);
        if (dm_gfx_session_stopping()) { pthread_mutex_unlock(&g_sub_mu); gfx_leave(); return; }
        if (q && dev_ok(((queue_t *)q)->dev) && atomic_load(&g_cur_dev) != ((queue_t *)q)->dev) dev_follow(((queue_t *)q)->dev);
        nvn_be_present_prepare(CTX(o->tex[idx]));
        if (nvn_frec_on()) nvn_frec_present(CTX(o->tex[idx]), present_period(o) / 1e9, crop);   // 帧录：持提交锁落盘，这一帧之后的提交等它
        pthread_mutex_unlock(&g_sub_mu);
    }
    nvn_be_present_target(o->interval, atomic_load(&g_itv_set_ns));   // 游戏此刻申请的交换间隔（只给「分时」分段记账，不改上屏节拍）
    if (idx >= 0 && idx < o->ntex && idx < 4) o->ticket[idx] = nvn_be_present(CTX(o->tex[idx]), present_period(o) / 1e9, crop);   // enqueue only, return immediately
    if (idx >= 0 && idx < o->ntex) __atomic_store_n(&o->shown, idx + 1, __ATOMIC_RELAXED);   // 屏上换成这张：上一张归还（AcquireTexture 的 #141 报红看它）
    pthread_mutex_lock(&g_flip_mu); acq_lease_release_locked(o, idx); pthread_cond_broadcast(&g_flip_cv); pthread_mutex_unlock(&g_flip_mu);   // 撞上还在屏上那张的取图（#141）等这次呈现翻走；每帧一次锁内广播，无等者时开销可不计
    gfx_leave();
    atomic_fetch_add(&g_nvn_ns, now_ns() - t0);
    uint64_t f = g_frame, ps, dr; nvn_be_stats(&ps, &dr);
    if (nvn_real[0] || nvn_real[1]) {   // DM_NVN_PROF: record phase = time of all implemented functions except submit; submit phase = QueueSubmitCommands
        static unsigned isub = ~0u; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        if (isub == ~0u) for (isub = 0; isub < NVN_NFUNC && strcmp(nvn_names[isub], "nvnQueueSubmitCommands"); isub++) {}
        uint64_t rec = 0, sub = 0;
        static uint64_t acc[NVN_NFUNC];
        for (unsigned i = 0; i < NVN_NFUNC; i++) { uint64_t t = atomic_exchange(&nvn_prof_ticks[i], 0); acc[i] += t; if (i == isub) sub = t; else rec += t; }
        if ((g_frame + 1) % TWIN == 0) {   // every 300 frames list the 5 most time-consuming functions
            char line[1024]; size_t o = 0;
            for (int k = 0; k < 5; k++) { unsigned best = 0; for (unsigned i = 1; i < NVN_NFUNC; i++) if (acc[i] > acc[best]) best = i;
                if (!acc[best]) break; o += snprintf(line + o, sizeof line - o, " %s %.2f ms;", nvn_names[best] + 3, acc[best] * tb.numer / tb.denom / 1e6); acc[best] = 0; }
            LOG("[nvn] translation layer top 5 by time (sum over the last 300 frames):%s", line); memset(acc, 0, sizeof acc);
        }
        trow(g_frame, "@rec_ms", rec * tb.numer / tb.denom / 1e6); trow(g_frame, "@submit_ms", sub * tb.numer / tb.denom / 1e6);
        tpush(4, rec * tb.numer / tb.denom / 1e6); tpush(5, sub * tb.numer / tb.denom / 1e6);
    }
    if (prev_in) {
        double fr = (t0 - prev_in) / 1e6, wk = (t0 - prev_out) / 1e6, nv = atomic_exchange(&g_nvn_ns, 0) / 1e6;
        tpush(0, fr); tpush(1, wk); tpush(2, nv);
        trow(f, "@frame_ms", fr); trow(f, "@work_ms", wk); trow(f, "@nvn_ms", nv); trow(f, "@itv", (double)present_itv(o));
        // Slow-frame attribution (taken only when DM_NVN_TSV is on): this frame's present-thread CPU time (−1 if the thread changed), CPU of all threads in the process, acquire wait accumulated per frame, present call time (same base as the GPU / on-screen present times: ms since process start)
        trow(f, "@wait_ms", g_acq_wait_ns / 1e6); trow(f, "@pq_wait_ms", g_pq_wait_ns / 1e6);
        trow(f, "@aud_wait_ms", s2a_aud_late_take() / 1e6);   // 本帧窗里声音渲染线程节拍迟到累计（ms，s2a_aud_late_note 探针）；随 DM_SLOWFRAME 的慢帧行一并打出
        if (frame_rec()) {
            static pthread_t pth; static uint64_t pcpu, pproc;
            uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); pthread_t th = pthread_self();
            trow(f, "@cpu_ms", pcpu && pthread_equal(th, pth) ? (c - pcpu) / 1e6 : -1); pth = th; pcpu = c;
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            uint64_t pc = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
            if (pproc) trow(f, "@proc_cpu_ms", (pc - pproc) / 1e6); pproc = pc;
            // 每帧能耗（照 d2r-metal 4b059cfc，MIT）：task_energy = 内核按进程记的能量（纳焦，Apple 芯片），task_gpu_utilisation = 记在本进程名下的 GPU 时间（纳秒）。
            //   iPhone 上 GPU 降频到刚好填满一帧，@gpu_ms 量不出开销差，要看能量；改动 A/B 用「每帧毫焦 + 掉帧率」
            { static uint64_t pnj, pgpu; task_power_info_v2_data_t pi; mach_msg_type_number_t pn = TASK_POWER_INFO_V2_COUNT;
              if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&pi, &pn) == KERN_SUCCESS) {
#if defined(__arm64__)
                  if (pnj) trow(f, "@proc_mj", (pi.task_energy - pnj) / 1e6); pnj = pi.task_energy;
#endif
                  if (pgpu) trow(f, "@gpu_util_ms", (pi.gpu_energy.task_gpu_utilisation - pgpu) / 1e6); pgpu = pi.gpu_energy.task_gpu_utilisation; } }
            trow(f, "@call_ms", (t0 - g_t0_ns) / 1e6);
        }
        trow(f, "@passes", (double)(ps - prev_pass)); trow(f, "@draws", (double)(dr - prev_draw));
        static uint64_t pl, pst, pf, pudc; uint64_t l, st, fu; nvn_be_bw(&l, &st, &fu);
        trow(f, "@rt_load_mb", (l - pl) / 1048576.0); trow(f, "@rt_store_mb", (st - pst) / 1048576.0); trow(f, "@pass_full_rw", (double)(fu - pf));
        uint64_t udc = nvn_be_udc_saved_bytes(); trow(f, "@unwritten_dc_mb", (udc - pudc) / 1048576.0);
        pl = l; pst = st; pf = fu; pudc = udc;
        static uint64_t pae, par; uint64_t ae_, ar_; nvn_be_args_stats(&ae_, &ar_);
        trow(f, "@args_enc", (double)(ae_ - pae)); trow(f, "@args_reuse", (double)(ar_ - par)); pae = ae_; par = ar_;
        static uint64_t pap; uint64_t ap_ = nvn_be_args_patched(); trow(f, "@args_patch", (double)(ap_ - pap)); pap = ap_;
        static uint64_t puc, pus; uint64_t uc_, us_; nvn_be_use_stats(&uc_, &us_);
        static uint64_t pdo; uint64_t do_ = nvn_be_ds_omit(); trow(f, "@ds_omit", (double)(do_ - pdo)); pdo = do_;
        trow(f, "@use_call", (double)(uc_ - puc)); trow(f, "@use_skip", (double)(us_ - pus)); puc = uc_; pus = us_;
        static uint64_t pfa, pre_; uint64_t fa_, pr_, up_, re_; nvn_dirty_stats(&fa_, &pr_); nvn_be_pkg_stats(&up_, &re_);
        trow(f, "@faults", (double)(fa_ - pfa)); trow(f, "@reuploads", (double)(re_ - pre_)); pfa = fa_; pre_ = re_;
        static uint64_t pmc, pmp, pmn; uint64_t mc_, mpg_, mn_; nvn_dirty_mp_stats(&mc_, &mpg_, &mn_);
        trow(f, "@mprotect_calls", (double)(mc_ - pmc)); trow(f, "@mprotect_pages", (double)(mpg_ - pmp)); trow(f, "@mprotect_ms", (mn_ - pmn) / 1e6); pmc = mc_; pmp = mpg_; pmn = mn_;
        double up_ms = nvn_be_upload_ns_take() / 1e6; trow(f, "@upload_ms", up_ms);
        trow(f, "@upload_gpu_ms", nvn_be_upload_gpu_ns_take() / 1e6);
        // Accounting: number of synchronous creations on the submit thread this frame + a timestamp on the same clock as UIMSG (offline, frames are split into front end / loading / in-run by the loading screen intervals)
        static uint64_t plib, ppso; uint64_t lib_, pso_; nvn_be_create_stats(&lib_, &pso_);
        trow(f, "@lib_new", (double)(lib_ - plib)); trow(f, "@pso_new", (double)(pso_ - ppso));
        {   // long-frame log: this frame's items are first recorded into the ring; whether to print is decided 8 frames later, once the GPU time has arrived (see below)
            static uint64_t plns, ppns, pup, prup, pub; uint64_t lns, pns, up, rup, ub; nvn_be_create_ns(&lns, &pns); nvn_be_pkg_stats(&up, &rup); ub = nvn_be_upload_bytes();
            lf_rec *r = &g_lf[f % LFN]; r->f = f; r->fr = fr; r->nv = nv; r->t_s = (now_ns() - g_t0_ns) / 1e9; r->pso = (unsigned)(pso_ - ppso); r->lib = (unsigned)(lib_ - plib);
            nvn_be_stall_take(r->stl, sizeof r->stl); r->make_ms = ((lns - plns) + (pns - ppns)) / 1e6; r->up = (unsigned)(up - pup); r->rup = (unsigned)(rup - prup); r->up_mb = (ub - pub) / 1048576.0; r->up_ms = up_ms;
            plns = lns; ppns = pns; pup = up; prup = rup; pub = ub;
            if (r->pso || r->lib || r->up) atomic_store(&g_frame_hitch, 2);   // 档位梯子（runtime/src/qladder.c）：这一帧有首遇现编 / 首传贴图，记到它和下一次上屏
        }
        plib = lib_; ppso = pso_;
        trow(f, "@t_s", (now_ns() - g_t0_ns) / 1e9);
        trow(f, "@drawable_ms", nvn_be_drawable_ns_take() / 1e6);
        uint64_t sw = atomic_exchange(&g_sub_wall_ns, 0); g_cpath[f % 16].f = f; g_cpath[f % 16].t = t0 - sw;
        uint64_t sc = atomic_exchange(&g_sub_cpu_ns, 0); g_lf[f % LFN].sw_ms = sw / 1e6; g_lf[f % LFN].sc_ms = sc / 1e6;
        trow(f, "@sub_wall_ms", sw / 1e6); trow(f, "@sub_cpu_ms", sc / 1e6); trow(f, "@sub_instr", (double)atomic_exchange(&g_sub_instr, 0));
    } else atomic_store(&g_nvn_ns, 0);
    g_acq_wait_ns = g_pq_wait_ns = 0;
    if (f >= 8) { double g = nvn_be_gpu_ms(f - 8); if (g >= 0) { tpush(3, g); trow(f - 8, "@gpu_ms", g); g_gpu_busy_ms += g; uint64_t a, b; if (nvn_be_gpu_span(f - 8, &a, &b) && b / 1e6 > g_gpu_clk_ms) g_gpu_clk_ms = b / 1e6; }   // GPU time is taken 8 frames late (already completed)
        { lf_rec *r = &g_lf[(f - 8) % LFN];   // long-frame log (on by default, DM_LONGFRAME=0 turns it off): a line is printed only when the whole frame > 20 ms or GPU > 16.7 ms, and it is clearly above recent conditions
          static int on = -1; if (on < 0) { const char *e = getenv("DM_LONGFRAME"); on = !(e && !strcmp(e, "0")); }
          // "Long" is relative to recent conditions: when running steadily at 30 fps (menus / windowless host) or under sustained overload, every frame is > 20 ms; those are not long frames and must not flood the log - a frame has to be 40% above the sliding mean of the last 16 frames to count
          static double ef = 16.7, eg = 8; int lf = r->fr > 20.0 && r->fr > 1.4 * ef, lg = g > 16.7 && g > 1.3 * eg;
          if (r->f == f - 8) { ef += ((r->fr < 3 * ef ? r->fr : 3 * ef) - ef) / 16; eg += ((g < 3 * eg ? g : 3 * eg) - eg) / 16; }
          if (on && r->f == f - 8 && f - 8 > 0 && (lf || lg)) { char b[2048];
              snprintf(b, sizeof b, "frame %llu (%.2f s) whole frame %.1f ms, submit+present %.1f ms, GPU %.1f ms; new pipelines %u + shader libraries %u, %.1f ms in all; texture uploads %u first + %u re-uploads, %.1f MB / %.1f ms; submit wall %.1f ms cpu %.1f ms; stages %s",
                       (unsigned long long)r->f, r->t_s, r->fr, r->nv, g, r->pso, r->lib, r->make_ms, r->up, r->rup, r->up_mb, r->up_ms, r->sw_ms, r->sc_ms, r->stl[0] ? r->stl : "-");
              // Visibility: during the time a long frame "comes out late", what stays on screen is the previous frame; if the previous frame was a loading screen (or this frame itself is one), all the player sees is the loading screen staying a bit longer, with no motion interrupted
              dm_longframe_note(b); } }
        uint64_t g0, g1; if (frame_rec() && nvn_be_gpu_span(f - 8, &g0, &g1)) { trow(f - 8, "@gpu_beg_ms", (g0 - g_t0_ns) / 1e6); trow(f - 8, "@gpu_end_ms", (g1 - g_t0_ns) / 1e6); } }
    // 撑不住就问玩家（runtime/src/qprompt.c）：每帧喂本帧绘制数、在放视频、过场标记（满屏判据结果由宿主侧自己读 mtl_fit_last）
    s2a_qp_frame((uint32_t)(dr - prev_draw), g_vid.on, dm_fps_state && __atomic_load_n((uint32_t *)(dm_fps_state + 4), __ATOMIC_RELAXED), atomic_load(&g_game_itv));
    prev_in = t0; prev_pass = ps; prev_draw = dr;
    dump_frame();
    if (g_frame % TWIN == 0) tsummary(g_frame);
    nvn_be_set_frame(g_frame);
#if defined(__ANDROID__) || defined(__OHOS__)
    s2a_android_frame_note();   // 每帧一次（与上一行同点）：安卓侧量实际出图帧率，喂 GPU 提频判定（android/native/gpu_turbo.c；OHOS 为空桩）
#endif
    atomic_store(&g_fps_game_itv, s2a_fps_present_hook() | (g_display_mode && s2a_fps_cap_reason() != 0));   // | 不用 ||：钩子每帧都要调（动态步长）。 S2Arm 帧率表：每帧只在这里调一次（present_period 一帧被调 3 次：取图 / 入队 / 节拍器，直连线 10-26 审出），present_itv 只读结果
    // Real-device semantics: present only enqueues and returns immediately; this frame's display time is scheduled by the pacer, and the texture can be acquired again only at "display time + one period" (blocking in AcquireTexture)
    uint64_t period = present_period(o), now = dm_detclock_enabled() ? guest_ns() : now_ns();
    g_hold_ring[o->presents % 16] = (uint8_t)present_itv(o);
    nvn_pacer pc = {o->pace};
    uint64_t slot = now;   // 显示链路控了节奏的帧：从这次上屏起显示，节拍跟着它重新起拍（nvn_pace_follow）；否则照旧按节拍器排槽
    if (pace_follow_take(o)) nvn_pace_follow(&pc, now); else slot += nvn_pace(&pc, now, period);
    o->pace = pc.target;
    if (idx >= 0 && idx < 4) o->avail[idx] = period ? slot + period : 0;
    static uint64_t prev_slot; if (prev_slot && period) trow(f, "@display_ms", (slot - prev_slot) / 1e6); prev_slot = slot;   // display interval between two adjacent frames on screen (this is the one to look at for dropped frames)
    // Actual presentation interval (windowed mode): taken 8 presents late (the presented callback has arrived by then), as the difference between the n-th and the (n−1)-th; −1 = discarded
    if (o->presents > 9) { uint64_t n = o->presents - 8; double a_ = nvn_be_presented_time(n), b_ = nvn_be_presented_time(n - 1);
        if (a_ < 0) trow(f - 8, "@presented_ms", -1); else if (a_ > 0 && b_ > 0) trow(f - 8, "@presented_ms", (a_ - b_) * 1e3);
        g_pres_exp[g_presn % TWIN] = g_pres_exp_last = pres_expect_ms(n);
        g_pres_iv[g_presn++ % TWIN] = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0);
        { double iv_ = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0); nvn_be_ub_present(iv_); }   // same-process alternating measurement (DM_NVN_UB) tallies on-screen present intervals per phase; when off it is just one atomic read
        dm_perf_present(a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0));   // on-screen present intervals are also kept in a separate ring; tsummary reports dropped frames every 300 frames (M1 criterion)
        if (a_ > 0) trow(f - 8, "@pres_at_ms", a_ * 1e3 - g_t0_ns / 1e6); }   // on-screen present time (ms since process start; presentedTime shares a time base with mach_absolute_time)
    if (f >= 8 && slow_on()) slow_emit(f - 8);
    prev_out = now_ns();
}

// ---------- State-only interfaces used by newer-SDK games (SMBW): buffer objects, pool / sampler / texture queries, views, color state, device and window ----------
// They only read and write the object itself and record no commands. Where the host has no matching capability (texture channel swizzle, depth / stencil sampling mode, sampler reduction, virtual mapping) the first occurrence logs one line and the call returns normally.
#define ONCE_LOG(...) do { static int once_; if (!once_++) LOG("[nvn] " __VA_ARGS__); } while (0)

// Buffer object = pool + offset + size; the GPU address and the CPU mapping are both the pool's host pointer + offset (same convention as MemoryPoolGetBufferAddress)
typedef struct { NVNdevice *dev; const pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; const pool_t *pool; int64_t off; uint64_t size; } buf_t;             FITS(buf_t, NVNbuffer);
#define BB(b) ((buf_builder_t *)(b))
#define BF(b) ((const buf_t *)(b))
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { NVNdevice *d = BB(b)->dev; memset(b, 0, sizeof(buf_builder_t)); BB(b)->dev = d; }
// ---------- 缓冲长度登记（NVN 的 storage 绑定可能只给地址、size=0） ----------
// 真机 GPU 不按描述符长度检查访问，Vulkan 却必须给一个真实窗口。nvnBuffer 对象本身就是
// “池 + 偏移 + 长度”的事实来源，这里按 CPU 地址登记活缓冲的长度，供回放侧按“地址所在缓冲”
// 求精确长度；登记/注销都在 BufferInitialize/Finalize，查不到再由调用方退回池尾近似。
// 表是按起点有序的区间树（nvn_buf_reg.h），登记 / 注销 / 查询都是 O(log 活缓冲数)：11-07 原无序数组线性扫描让时之笛（每帧大量建删缓冲）
//   游戏线程 96% 耗在 BufferInitialize / Finalize，室外 30 → 5–6 帧（坑记录 11-07）。
#include "nvn_buf_reg.h"
static nvn_breg g_breg = {.rng = 0x9e3779b9u};
// 锁：锁内只有一次 O(log n) 树操作（百纳秒级），读者（安卓回放查询）也少 → 自旋几十次再让出的轻量锁（nvn_breg_lock），两端同一份：
//   macOS / iOS 的 pthread_rwlock 一争用就进内核排队交接（时之笛采样 37.6% 在 __psynch_rw_wrlock），4 线程建删比单线程慢约 60 倍；
//   pthread_mutex 在 Mac 上 4 线程耗时比抖到 2–10；苹果的 os_unfair_lock 共用文件不能直接包含（分层规则 R-02）
static _Atomic int g_breg_lk;
#define BREG_LOCK() nvn_breg_lock(&g_breg_lk)
#define BREG_UNLOCK() nvn_breg_unlock(&g_breg_lk)
// 只在有查询方时登记：后端自报（nvn_be_buf_extent_consumer）——安卓 Vulkan 回放（nvn_vk_mrt.c 弱链接 nvn_buf_extent 求 size=0 绑定长度）报 1，
//   Metal 路线（iOS / Mac）不查这张表、报 0、永不登记（11-07 时之笛修完树之后，Duo 上 5 个工作线程仍各有约 40% 样本在建删缓冲里抢这把锁）。
//   S2A_NVN_BUF_REG=1 强制打开：宿主单测（t_nvn_bufreg_rate）借此在 Mac 上守安卓那条路径。第一次建删时定下，之后不变。
int nvn_be_buf_extent_consumer(void);   // nvn_mtl.m 0 / 安卓 nvn_be_android.c 1
static int breg_on(void) {
    static _Atomic int v = -1; int r = atomic_load_explicit(&v, memory_order_relaxed);
    if (r < 0) { const char *e = getenv("S2A_NVN_BUF_REG"); r = nvn_be_buf_extent_consumer() || (e && e[0] == '1'); atomic_store_explicit(&v, r, memory_order_relaxed); }
    return r;
}
static void buf_reg_put(uint64_t base, uint64_t size) {
    if (!base || !size || !breg_on()) return;
    BREG_LOCK();
    nvn_breg_put(&g_breg, base, size);   // 内存不够时该起点不进表：查询退回池尾近似
    BREG_UNLOCK();
}
static void buf_reg_del(uint64_t base) {
    if (!breg_on()) return;
    BREG_LOCK();
    nvn_breg_del(&g_breg, base);
    BREG_UNLOCK();
}
// 查询：cpu_addr 落在某个活缓冲里时返回 {缓冲起点, 该缓冲总长度}（1 = 命中）；多个活缓冲重叠时取起点最近（最具体）的那个（声明在 nvn_obj.h）。
int nvn_buf_extent(uint64_t cpu_addr, uint64_t *base, uint64_t *size) {
    if (!cpu_addr || !base || !size) return 0;
    BREG_LOCK();
    int hit = nvn_breg_find(&g_breg, cpu_addr, base, size);
    BREG_UNLOCK();
    return hit;
}
// 登记表累计走过的 split / merge 步数（nvn_breg.steps）：单测 t_nvn_bufreg_rate.m 按 N / 4N 的步数比判建删复杂度（不用墙钟比，见 nvn_buf_reg.h）
uint64_t nvn_buf_reg_steps(void) {
    BREG_LOCK();
    uint64_t s = g_breg.steps;
    BREG_UNLOCK();
    return s;
}

NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { BB(b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) { BB(b)->pool = (const pool_t *)p; BB(b)->off = off; BB(b)->size = n; }
NVN(nvnBufferBuilderGetDevice, const NVNdevice *, const NVNbufferBuilder *b) { return ((const buf_builder_t *)b)->dev; }
NVN(nvnBufferBuilderGetMemoryPool, const NVNmemoryPool *, const NVNbufferBuilder *b) { return (const NVNmemoryPool *)((const buf_builder_t *)b)->pool; }
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *buf, const NVNbufferBuilder *b) {
    const buf_builder_t *bb = (const buf_builder_t *)b;
    if (!bb->pool || (!bb->pool->mem && !(bb->pool->flags & 0x200))) { uint64_t ra = (uint64_t)__builtin_return_address(0); module_t *m = module_of((void *)ra);   // 调用位置：D-B3PR 空池从游戏哪一步来
        LOG("【红】 BufferInitialize: pool has no storage (pool %p flags %#x offset %#llx size %#llx; called from %s+%#llx)", (const void *)bb->pool, bb->pool ? bb->pool->flags : 0, (unsigned long long)bb->off, (unsigned long long)bb->size,
            m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra); return 0; }
    // 缓冲对象只是「池 + 偏移 + 大小」的视图，建时不分配、真机驱动也不在这里查：越过池尾本身不是错，读写越过池尾才是（10-26 塞尔达按 + 开系统菜单：
    //   0x24000 的池上建 0x40000 的缓冲，菜单画面完整、局到 MARK；原先报红让能玩格判红）。黄字提示，访问时的越界检查另做
    //   D-6VG6（11-25 旷野之息拍照：0x195000 的池上建 0x1b0000 的缓冲，第二张存完 GPU 缺页）：访问越过池尾在真机上落在池存储之后同一处内存，这边原先没有任何后备——
    //   池的 Metal 缓冲只盖到池尾、GPU 地址段也只到池尾。建缓冲对象时把两者都扩到缓冲对象的末尾（nvn_be_pool_cover / gva_extend），整段都可寻址
    if ((uint64_t)bb->off + bb->size > bb->pool->size) {
        uint64_t need = (uint64_t)bb->off + bb->size;
        LOG("【黄】 BufferInitialize: buffer object extends past the end of its pool (offset %#llx size %#llx pool %#llx); the pool's backing is extended to cover it", (unsigned long long)bb->off, (unsigned long long)bb->size, (unsigned long long)bb->pool->size);
        if (bb->pool->mem && !(bb->pool->flags & 0x200)) {
            nvn_be_pool_cover(bb->pool->mem, bb->pool->size, need);
            if (bb->pool->gva && !gva_extend(bb->pool->gva, need)) LOG("【红】 BufferInitialize: the pool's GPU address range cannot grow to %#llx (the next pool's range starts too close): buffer addresses past the pool end stay unresolved", (unsigned long long)need);
        }
    }
    *(buf_t *)buf = (buf_t){MAGIC('B', 'U', 'F', 0), bb->pool, bb->off, bb->size};
    if (bb->pool->mem) buf_reg_put((uint64_t)(uintptr_t)(bb->pool->mem) + (uint64_t)bb->off, bb->size);
    pool_mark_addressed(bb->pool);   // 缓冲一建好，它的地址就可能被游戏算出来（池地址 + 偏移）
    OLOG("BufferInitialize %p pool=%p off=%#llx size=%#llx", (void *)buf, (const void *)bb->pool, (unsigned long long)bb->off, (unsigned long long)bb->size);
    return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *buf) {
    // 先核魔数：游戏可能 Finalize 没 Initialize 过的对象，那时 pool 字段是垃圾，不能解引用
    if (BF(buf)->magic == MAGIC('B', 'U', 'F', 0) && BF(buf)->pool && BF(buf)->pool->mem)
        buf_reg_del((uint64_t)(uintptr_t)(BF(buf)->pool->mem) + (uint64_t)BF(buf)->off);
    // 已 Finalize：写专用魔数（不是 0），池 / 偏移照留。真驱动 Finalize 后对象里存的地址还在，游戏 Finalize 后再取地址照样拿到原值（11-16 D-XDQS：
    //   010051F0207B2000 iPad 对魔数 0 的缓冲取地址，我们回空、游戏加 0x100 当着色器代码地址，宿主算着色器键时读 0x100 崩）。buf_dead 据此区分「已 Finalize」与「从没初始化成功」
    if (BF(buf)->magic == MAGIC('B', 'U', 'F', 0)) ((buf_t *)buf)->magic = MAGIC('B', 'U', 'F', 'x');
    else if (BF(buf)->magic != MAGIC('B', 'U', 'F', 'x')) ((buf_t *)buf)->magic = 0;   // 再 Finalize 一次：保持「已 Finalize」
}
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *buf, const char *s) { (void)buf; (void)s; }
// 初始化没成功的缓冲（BufferInitialize 回了 0：池没有存储，11-14 D-MDVK 签名丢了内存上限权限、堆申请失败）：游戏常不看返回值接着用，
//   对象里没有魔数、池字段是空或残值。Map / GetAddress 原先直接解引用池，宿主自己崩在 h_nvnBufferMap（读 0x8）。认出来报红、回空，让真因（缺存储）留在日志上
static inline int pool_cpu_none(const pool_t *o);
static int buf_dead(const NVNbuffer *buf, const char *who) {
    const buf_t *b = BF(buf);
    if (b->magic == MAGIC('B', 'U', 'F', 0) && b->pool && (b->pool->mem || pool_cpu_none(b->pool))) return 0;
    // 已 Finalize、池还活着：同真驱动回存着的地址（报红一次，便于核是哪款在 Finalize 后还用缓冲）
    if (b->magic == MAGIC('B', 'U', 'F', 'x') && b->pool && b->pool->magic == MAGIC('P', 'O', 'O', 'L') && (b->pool->mem || pool_cpu_none(b->pool))) {
        static _Atomic int nf; if (atomic_fetch_add(&nf, 1) < 4) LOG("【红】 %s：缓冲 %p 已 Finalize 仍在用，池还活着：同真驱动回它存着的地址 / 映射", who, (const void *)buf);
        return 0; }
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("【红】 %s：缓冲 %p 没初始化成功或池没有存储（魔数 %#x、池 %p）：回空，不解引用", who, (const void *)buf, b->magic, (const void *)b->pool);
    return 1;
}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *buf) { if (buf_dead(buf, "nvnBufferGetAddress")) return 0; return (NVNbufferAddress)(pool_ga(BF(buf)->pool) + BF(buf)->off); }
// CPU 不可访问的池（NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS）：映射给 NULL，同真驱动。宿主内部一律直接用 pool->mem，不经这两个入口。
//   11-02 EA SPORTS FC 26（01004FF021942000）：引擎算拷贝源 = nvnBufferGetAddress(A) + (CPU 指针 − nvnBufferMap(A))，CPU 不可访问的池它自己把 CPU 指针记 0
//   （NRO+0x67a84 查池标志位 0），只有 Map 也给 0 时才得回 GPU 地址；原先 Map 照给存储指针，算成「GPU 地址 − CPU 地址」（0x4ddcf80000），回放中止
static _Atomic uint64_t g_map_none[2];   // 映射 CPU 不可访问的池给了 NULL 的次数：[0] 池 / [1] 缓冲（第一次与每 1000 次打一行，核哪些款走到这条）
// 虚拟池同样给 NULL：真驱动的虚拟池类 Map 就是 `mov x0, xzr; ret`，缓冲 Map 转调池 Map、NULL 原样返回（11-03 无人深空 SDK 静态核：
//   驱动 GetProcAddress 表槽 271 → 池对象 vtable +0x28；虚拟池 vtable 0xcbcf18 的 +0x28 = 0x4bb1b0；普通池 0x4b9af0 查标志位 0，置位给 NULL）
static inline int pool_cpu_none(const pool_t *o) { return o && (o->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)); }
static void map_none_note(int k, const pool_t *o) {
    uint64_t n = atomic_fetch_add(&g_map_none[k], 1) + 1;
    if (n == 1 || n % 1000 == 0) LOG("[nvn] %s on a pool without CPU access (flags %#x): NULL, as the NVN driver (%llu so far)", k ? "nvnBufferMap" : "nvnMemoryPoolMap", o->flags, (unsigned long long)n);
}
NVN(nvnBufferMap, void *, const NVNbuffer *buf) { if (buf_dead(buf, "nvnBufferMap")) return NULL; if (pool_cpu_none(BF(buf)->pool)) { map_none_note(1, BF(buf)->pool); return NULL; } return BF(buf)->pool->mem + BF(buf)->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *buf) { return (size_t)BF(buf)->size; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *buf) { return (NVNmemoryPool *)BF(buf)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *buf) { return (ptrdiff_t)BF(buf)->off; }
// 调试 ID：NVN 没有 SetDebugID，驱动侧自定。本层用对象地址（对象存活期间唯一、稳定），与 nvnTextureGetDebugID 用序号的差别是缓冲区没有初始化序号
NVN(nvnBufferGetDebugID, uint64_t, const NVNbuffer *buf) { return (uint64_t)(uintptr_t)buf; }

// ---- Sampler：整数 / 无符号整数边界色（给整数纹理用的边界值），与已有浮点边界色共用 samp_par.border ----
NVN(nvnSamplerBuilderSetBorderColori, void, NVNsamplerBuilder *b, const int *c) {
    samp_par *p = &((samp_builder_t *)b)->p;
    for (int i = 0; i < 4; i++) p->border[i] = (float)c[i];
}
NVN(nvnSamplerBuilderSetBorderColorui, void, NVNsamplerBuilder *b, const uint32_t *c) {
    samp_par *p = &((samp_builder_t *)b)->p;
    for (int i = 0; i < 4; i++) p->border[i] = (float)c[i];
}
NVN(nvnSamplerGetBorderColorui, void, const NVNsampler *s, uint32_t *c) {
    const samp_par *p = &((const samp_t *)s)->p;
    for (int i = 0; i < 4; i++) c[i] = (uint32_t)(p->border[i] < 0.0f ? 0.0f : p->border[i] + 0.5f);
}
NVN(nvnSamplerGetDebugID, uint64_t, const NVNsampler *s) { return (uint64_t)(uintptr_t)s; }
// A buffer object is a view of its pool: a flush through it publishes CPU-written textures under that range exactly like the pool's flush (ft_flush). Empty before, so a texture the game
//   wrote through a buffer mapping and flushed there stayed a GPU-only texture with its old content (t_nvn_poolflush; gap scan 11-01 family 42)
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { if (buf_dead(buf, "nvnBufferFlushMappedRange")) return; h_nvnMemoryPoolFlushMappedRange((const NVNmemoryPool *)BF(buf)->pool, BF(buf)->off + off, n); }
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { if (buf_dead(buf, "nvnBufferInvalidateMappedRange")) return; h_nvnMemoryPoolInvalidateMappedRange((const NVNmemoryPool *)BF(buf)->pool, BF(buf)->off + off, n); }

// Memory pool queries
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { if (pool_null(p, "nvnMemoryPoolGetFlags", (uint64_t)__builtin_return_address(0))) return (NVNmemoryPoolFlags)0; return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { const pool_t *o = (const pool_t *)p; if (pool_null(p, "nvnMemoryPoolMap", (uint64_t)__builtin_return_address(0))) return NULL; if (pool_cpu_none(o)) { map_none_note(0, o); return NULL; } return o->mem; }   // CPU 不可访问的池给 NULL（见 nvnBufferMap）
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { if (pool_null(p, "nvnMemoryPoolGetSize", (uint64_t)__builtin_return_address(0))) return 0; return (size_t)((const pool_t *)p)->size; }
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *s) { (void)p; (void)s; }
// Debug labels: only for NVN debugging tools, unused by the host
NVN(nvnWindowSetDebugLabel, void, NVNwindow *w, const char *s) { (void)w; (void)s; }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *p, const char *s) { (void)p; (void)s; }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *p, const char *s) { (void)p; (void)s; }
NVN(nvnQueueSetDebugLabel, void, NVNqueue *q, const char *s) { (void)q; (void)s; }
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p, const char *s) { (void)p; (void)s; }
// ---- 本层不建模的三项程序特性：按「不支持就明说」处理，不返回成功兜底 ----
// 子程序链接（GPGPU subroutine linkage）：直连层的着色器是出包时离线翻译成整支函数的，没有运行时可链接的子程序表，
// 所以这里如实返回 0（失败）并点名一次，让游戏/日志能看出是这一项没支持，而不是静默画错
NVN(nvnProgramSetSubroutineLinkage, NVNboolean, NVNprogram *p, int n, const NVNsubroutineLinkageMapPtr *m) {
    (void)p; static int once;
    if (!once++) LOG("【红】 ProgramSetSubroutineLinkage %d: subroutine linkage is not supported (shaders are translated whole at pack time)", n);
    (void)m; return 0;
}
// 逐样本着色（sample shading）：只改光栅化阶段的采样密度，本层不建模；记一行，行为仍按每像素一次
NVN_NOPROTO(nvnProgramSetSampleShading, void, NVNprogram *p, float f) {
    (void)p; static int once; if (!once++) LOG("[nvn] ProgramSetSampleShading %.3f: sample-rate shading is not modelled (per-pixel shading is used)", f);
}
// 子块尺寸（tiled texture 的子块，配合 sparse / tiled 存储）：本层不做 tiled 存储，如实报 0 并点名
NVN_NOPROTO(nvnProgramGetSubtileSize, int, const NVNprogram *p) {
    (void)p; static int once; if (!once++) LOG("[nvn] ProgramGetSubtileSize: tiled texture storage is not used, reporting 0");
    return 0;
}
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }
// Virtual pool (flags 0x200): a GPU address range with no storage; MapVirtual maps pages of physical pools into it. A request is 0x28 bytes: {physical pool, physical offset, virtual offset, size, storage class}.
// Every virtual pool is a page table, whatever its size (below). It used to be so only for one larger than the application pool; a smaller one was made an alias of one physical pool
//   (storage = physical base + physical offset − virtual offset of its first request) and refused any later request with another base. UE4 keeps a small virtual pool per streamed texture
//   and maps it page by page out of a shared physical pool, mip tail first: Torchlight III refused 299 requests in one run (its streamed mips never arrived: flat ground, flat ships), 164 of
//   its aliases started before the physical pool (negative offsets: the texture's first levels read someone else's memory), and Trine 5 built a buffer on a virtual pool not yet mapped,
//   which had no storage yet: BufferInitialize failed and BufferGetAddress read a null pool (11-03, eight games with the same red line)
typedef struct { NVNmemoryPool *phys; int64_t poff, voff; uint64_t size; int32_t storage_class, _pad; } map_req;
_Static_assert(sizeof(map_req) == 0x28, "NVNmappingRequest");
#if defined(__ANDROID__) || defined(__OHOS__)   // 安卓：回放层（graphics/nvn/replay）按「缓冲地址就是宿主内存」读写，虚拟池照旧占一段宿主地址、物理页别名进去（安卓 39 位地址空间够用）；苹果宿主见 #else 一支（#1143）
// True sparse virtual pools (Monster Hunter Rise: RE Engine's GPU heap is one 64 GB virtual pool, built from separately allocated 4 MB physical pools mapped at offsets of their own; boot maps 45 of them
// over the first 180 MB). At Initialize every virtual pool gets an address range of its own (reserved, inaccessible),
// and every mapping request vm_remaps the physical pool's pages over its part of that range: the virtual addresses then read and write the same memory as the physical pool, as the GPU page table does
// on hardware, and buffer / texture addresses stay CPU addresses. Each run of mapped pages is registered with the replay as a pool of its own (nvn_be_pool_add), rebuilt when a request joins or splits runs.
// Unmapping puts inaccessible pages back. The reservation is the pool's size, at most 64 GB on the Mac and 1 GB on iOS.
// NVN maps at 4 KB, the host aliases at its 16 KB page: a request that is not 16 KB aligned (Monster Hunter Rise's ASTC 6x6 textures, whose mip tail starts 0x81000 into the texture),
// or one beyond the reservation, cannot be aliased. So every request is also kept in a page table at 4 KB (the physical memory behind each virtual page, and the mapping generation
// that set it), and the host reads textures of a sparse pool through it (nvn_sparse_copy): such a texture is uploaded from what the table points at, with unmapped pages read as 0,
// and uploaded again only when a mapping in its range changes or the CPU writes a physical page behind it (nvn_sparse_since; the physical pools are dirty-tracked). The game fills
// its sparse textures (flag 0x20, 32 GB into the pool) that way: it writes the tiles on the CPU into 4 MB physical pools, then maps them; no GPU copy writes them
enum { VPT_LEAF = 512 };   // a page-table leaf covers 2 MB
typedef struct { const uint8_t *phys; uint64_t gen; } vpte;
// One per virtual pool, allocated at Initialize and freed at Finalize. run: the mapped runs, grown as needed (a texture's pool usually has 1–3; Monster Hunter Rise's heap hundreds)
struct vpool { const pool_t *pool; uint8_t *base; uint64_t len, size, top; int nrun; struct vrun { uint64_t lo, hi; } *run; vpte **pt;
    int nsh, csh; struct vshp { uint64_t off, stamp; } *sh;   // copied host pages (see vsh_*): offset in the pool, dirty stamp of the last copy
    int lv;   // 预留记进了低区账（vpool_lv_note；帧录虚拟池区里取的不在低区、不记）
    struct vlog_ent { uint64_t off, len; } *log; _Atomic uint64_t nlog;   // 映射变更日志（马车世界缺口 ①，照上游参考实现（MIT）hle_nvn.c vlog）：每次映射请求 [off, off + len) 进环（VLOG_N），nlog 是总条数；有人问才建（nvn_sparse_log_seq）
    _Atomic uint64_t gen; uint64_t id;   // gen：本池最近一次映射请求的全局代号（vpt_set）；id：建池序号（全局递增，池对象地址复用时区分新旧池）。GPU 写的稀疏贴图按它们跟映射（nvn_mtl.m gstex_*）
#if defined(__ANDROID__) || defined(__OHOS__)
    s2a_vp_alias_table alias;   // dst -> 背板源；vpool_map / vpool_release 维护（#1217 §2.8）
#endif
};   // len: reserved; size: the pool's; top: highest mapped end so far (logged per 128 MB)
// Where the virtual pools are found: by pool pointer in a hash table (texture lookups ask per texture whether its pool is one: nvn_sparse_pool), by address in an array sorted by base
//   (nvn_host_readable). A UE4 game keeps a pool per streamed texture: Torchlight III created 1140 in one run, hundreds alive at once; the old table had 4 slots.
//   Both change only under g_vp_mu (Initialize / Finalize); the hash table also under g_vph_lk for writing, so nvn_sparse_pool reads it under g_vph_lk alone. Whoever holds g_vp_mu reads both freely
static nvn_hmap g_vph = {.klen = 8};
static pthread_rwlock_t g_vph_lk = PTHREAD_RWLOCK_INITIALIZER;
static struct vpool **g_vpa; static size_t g_nvpa, g_cvpa;
static pthread_mutex_t g_vp_mu = PTHREAD_MUTEX_INITIALIZER;
// 锁序（#1217 原生层裁定第 1 条）：NVN 可以 g_vp_mu → svc_mem g_mu，反向禁止。
// 代码依据（interface owner 回合 #6567620762 ①）：
//   - android/native/runtime/fault/thread_android.inc:426：停世界只对 guest PC 发 SIGURG 停车，
//     SVC / 原生回调里的线程照跑（「Host lock owners keep progressing」）。
//   - runtime/src/svc_mem.c:1086：backing_begin_locked 先 GMU_UNLOCK 放掉 g_mu，之后才等 writers 停稳；
//     所以持 g_vp_mu 的 NVN 线程去取 g_mu 最多等到改表一方放锁，不会和停世界成环。
// 渲染 / GPU 回调线程不调 s2a_mem_backing_alias_ref / unref，不会在持 g_vp_mu 的路径上取 g_mu。
// 反向（持 g_mu 再取 g_vp_mu）由下面的 Debug 断言拦住。
// Debug 构建下取 g_vp_mu 前问一次 svc_mem 当前线程是否持着 g_mu；是则直接 abort。
// g_vph_lk 只在持 g_vp_mu 时写、nvn_sparse_pool 单独读，读写都不再取别的锁：g_vp_mu → g_vph_lk 一个方向
static void vp_lock(void) {
#if defined(__ANDROID__) || defined(__OHOS__)
    s2a_mem_lockorder_vp_enter();
#endif
    pthread_mutex_lock(&g_vp_mu);
}
static void vp_unlock(void) { pthread_mutex_unlock(&g_vp_mu); }
static _Atomic uint64_t g_vp_gen;   // bumped per mapping request
static struct vpool *vpool_of(const pool_t *o) { return nvn_hm_get(&g_vph, &o); }   // caller holds g_vp_mu (or g_vph_lk for reading)
static size_t vpa_find(uint64_t a) {   // caller holds g_vp_mu; → index of the last pool whose base ≤ a, or g_nvpa when none
    size_t lo = 0, hi = g_nvpa;
    while (lo < hi) { size_t m = (lo + hi) / 2; if ((uint64_t)(uintptr_t)g_vpa[m]->base <= a) lo = m + 1; else hi = m; }
    return lo ? lo - 1 : g_nvpa;
}
static void vpool_publish(struct vpool *v) {   // caller holds g_vp_mu
    if (g_nvpa == g_cvpa) { size_t c = g_cvpa ? 2 * g_cvpa : 64; struct vpool **a = realloc(g_vpa, c * sizeof *a); if (!a) die("virtual pool table allocation failed"); g_vpa = a; g_cvpa = c; }
    size_t i = g_nvpa; while (i && g_vpa[i - 1]->base > v->base) { g_vpa[i] = g_vpa[i - 1]; i--; }
    g_vpa[i] = v; g_nvpa++;
    pthread_rwlock_wrlock(&g_vph_lk); nvn_hm_put(&g_vph, &v->pool, v); pthread_rwlock_unlock(&g_vph_lk);   // filled before it is visible
}
static void vpool_unpublish(struct vpool *v) {   // caller holds g_vp_mu
    pthread_rwlock_wrlock(&g_vph_lk); nvn_hm_del(&g_vph, &v->pool); pthread_rwlock_unlock(&g_vph_lk);
    size_t i = vpa_find((uint64_t)(uintptr_t)v->base);
    if (i < g_nvpa && g_vpa[i] == v) { memmove(g_vpa + i, g_vpa + i + 1, (g_nvpa - i - 1) * sizeof *g_vpa); g_nvpa--; }
}
// 弱链接[nvntest]：s2a_vp_arena_map（虚拟池区，runtime/src/vawin.c；NVN 单测不链原生层，链接行按这行放行未定义，runtime/build.mk 自动收）
// 弱链接[nvntest]：s2a_vp_arena_has（同上）
void *s2a_vp_arena_map(uint64_t len) __attribute__((weak)); int s2a_vp_arena_has(const void *p, uint64_t n) __attribute__((weak));
static void vpool_unmap_range(uint8_t *base, uint64_t len) {   // 虚拟池区里的盖回不可访问（区不还系统），其余照常 munmap
    if (s2a_vp_arena_has && s2a_vp_arena_has(base, len)) mmap(base, len, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0); else munmap(base, len);
}
static struct vpool *vpool_new(const pool_t *o, uint8_t *a, uint64_t len) {   // 建页表、登记（录时 vpool_reserve、回放 s2a_hle_vpool_load 共用）
    struct vpool *v = calloc(1, sizeof *v);
    if (!v || !(v->pt = calloc((o->size + (2u << 20) - 1) >> 21 ? (o->size + (2u << 20) - 1) >> 21 : 1, sizeof *v->pt))) die("sparse pool page table allocation failed");
    static _Atomic uint64_t ids; v->pool = o; v->base = a; v->len = len; v->size = o->size; v->id = atomic_fetch_add(&ids, 1) + 1;
    vp_lock(); vpool_publish(v); vp_unlock();
    return v;
}
static _Atomic uint64_t g_vsh_pages;   // copied host pages alive in all virtual pools (vsh_*)
// S2A_VPOOL_HOLE_MB=<n>：宿主最大空洞只有 n MB（大于它的预留一律当 errno 12 失败），在 Mac 上复现真机低区被切碎（11-06 FC26 真机最大空洞 511 MB）；缺省不限
static void *vpool_mmap(uint64_t len) {
    static uint64_t hole = ~0ull; if (hole == ~0ull) { const char *e = getenv("S2A_VPOOL_HOLE_MB"); hole = e && *e ? strtoull(e, NULL, 0) << 20 : 0; }
    if (hole && len > hole) { errno = ENOMEM; return MAP_FAILED; }
    return mmap(NULL, len, PROT_NONE, MAP_ANON | MAP_PRIVATE, S2A_VMTAG_FD(S2A_VMTAG_VPOOL), 0);   // 带标签：低区按标签合计认得出（#7c1a0f）
}
// 虚拟池的整段预留记进低区账（#1143：马车世界 iPhone 开局 11 个池预留 1368 MB，宿主 3 GB 一分钟内耗光，原先全算「拦不到的」）。
//   只记账、不设限（不走 acquire）：预留失败照旧逐次减半；账本取不到（单测 / 回放 / 安卓）就不记
static const s2a_lowva_api *vpool_lv(int *me) {
    static const s2a_lowva_api *lv; static int user, tried; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!tried) { tried = 1;
        const s2a_lowva_api *(*get)(void) = (const s2a_lowva_api *(*)(void))dlsym(RTLD_DEFAULT, "s2a_lowva"); const s2a_lowva_api *l = get ? get() : NULL;
        if (l && l->size >= sizeof(s2a_lowva_api) && l->version == S2A_LOWVA_VERSION) { lv = l; user = l->user("Switch 2 虚拟池预留", NULL, NULL); } }
    pthread_mutex_unlock(&mu);
    *me = user; return lv;
}
static uint8_t *vpool_reserve(const pool_t *o) {   // → the reserved range of a virtual pool, NULL when no address space is left
    if (!(o->flags & 0x200)) return NULL;
    // 1 GB on iOS: Monster Hunter Rise's buffers stay under 0x20100000 through its opening, its textures (32 GB in) are read through the page table, and 4 GB guest + 1 GB stays near the 5 GB
    //   proven on a phone. The Mac takes the same 1 GB while it reproduces the iPhone's address limits (S2A_IPHONE_VA, on by default: runtime/src/vawin.c), 64 GB otherwise; and a reservation
    //   that fails falls back to 1 GB before giving up (10-26 Monster Hunter Rise on the Mac with the iPhone limits: 64 GB could not be reserved, the pool fell back to one alias, the
    //   non-contiguous mappings that followed were refused and the first texture upload read unbacked memory: SIGBUS in pkg_upload)
    uint64_t cap = 1ull << 30;
#if !TARGET_OS_IPHONE
    { const char *e = getenv("S2A_IPHONE_VA"); if (e && *e == '0') cap = 64ull << 30; }
#endif
    uint64_t len = o->size < cap ? o->size : cap;
    void *a = s2a_vp_arena_map ? s2a_vp_arena_map(len) : NULL;   // 帧录进程：从虚拟池区取，回放进程同址放得回（runtime/src/vawin.c）
    int arena = a != NULL;
    if (!a) a = vpool_mmap(len);
    // 占不到就逐次减半（按 2 MB 页表粒度取整）再占，直到 16 MB：超出预留的映射走「超出预留、只进页表」那支，预留内的地址照常换得回宿主地址
    //   （11-06 FC26 真机：宿主低区被切碎、最大空洞 511 MB，512 MB 的池原先整池放弃，GPU 地址换不回宿主地址，开赛 SIGSEGV；原先只有大于 1 GB 的退一档到 1 GB）
    if (a == MAP_FAILED) { int er = errno; uint64_t want = len;
        while (a == MAP_FAILED && len > (16ull << 20)) { len = (len >> 1) & ~((2ull << 20) - 1); a = vpool_mmap(len); }
        if (a != MAP_FAILED) LOG("[nvn] virtual pool %p: %#llx bytes of address space not available (errno %d), reserving %#llx instead", (const void *)o, (unsigned long long)want, er, (unsigned long long)len); }
    if (a == MAP_FAILED) { LOG("【红】 virtual pool %p: cannot reserve %#llx bytes of address space (errno %d)", (const void *)o, (unsigned long long)len, errno); return NULL; }
    struct vpool *nv = vpool_new(o, a, len); vp_lock(); size_t live = g_nvpa; vp_unlock();
    if (!arena) { int me; const s2a_lowva_api *lv = vpool_lv(&me); if (lv && me > 0) { lv->note(me, (int64_t)len); nv->lv = 1; } }
    static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;   // a UE4 game makes one per streamed texture: the first ones and every 1000th
    if (k <= 8 || k % 1000 == 0) LOG("[nvn] virtual pool %p (%#llx bytes, flags %#x): own address range %p+%#llx, physical pages are remapped into it (%llu so far, %zu alive)", (const void *)o, (unsigned long long)o->size, o->flags, a, (unsigned long long)len, (unsigned long long)k, live);
    return a;
}
static void vpool_release(const pool_t *o) {
    vp_lock(); struct vpool *v = vpool_of(o);
    if (v) { vpool_unpublish(v);
        if (!rp_buf_alias_set((uintptr_t)v->base, v->len, 0)) die("cannot release virtual buffer aliases");
        for (int k = 0; k < v->nrun; k++) nvn_be_pool_del(v->base + v->run[k].lo); vpool_unmap_range(v->base, v->len);
        if (v->lv) { int me; const s2a_lowva_api *lv = vpool_lv(&me); if (lv && me > 0) lv->note(me, -(int64_t)v->len); }
#if defined(__ANDROID__) || defined(__OHOS__)
        // 拆池必须早于 s2a_mem_session_reset：祖先先是 vpool_release 再 reset，否则 unref 会下溢。
        s2a_vp_alias_clear(&v->alias);
#endif
        for (uint64_t l = 0; l < (v->size + (2u << 20) - 1) >> 21; l++) free(v->pt[l]);
        atomic_fetch_sub(&g_vsh_pages, (uint64_t)v->nsh);
        free(v->pt); free(v->run); free(v->sh); free(v->log); free(v); }
    vp_unlock();
}
// Mapped runs of a sparse pool: [lo, hi) sorted, disjoint, adjacent runs merged. Caller holds g_vp_mu. A request adds at most one run (a map) or splits one in two (an unmap)
static void vrun_set(struct vpool *v, uint64_t lo, uint64_t hi, int mapped) {
    struct vrun *nl = malloc(((size_t)v->nrun + 2) * sizeof *nl); int nn = 0; uint64_t mlo = lo, mhi = hi;
    if (!nl) die("virtual pool run table allocation failed");
    for (int k = 0; k < v->nrun; k++) {
        uint64_t a = v->run[k].lo, b = v->run[k].hi;
        if (b < lo || a > hi || (!mapped && (b == lo || a == hi))) { nl[nn++] = (struct vrun){a, b}; continue; }   // untouched (an unmap leaves adjacent runs alone)
        nvn_be_pool_del(v->base + a);
        if (mapped) { if (a < mlo) mlo = a; if (b > mhi) mhi = b; continue; }   // joins the new run
        if (a < lo) { nl[nn++] = (struct vrun){a, lo}; nvn_be_pool_add(v->base + a, lo - a); }   // what the unmap leaves on either side
        if (b > hi) { nl[nn++] = (struct vrun){hi, b}; nvn_be_pool_add(v->base + hi, b - hi); }
    }
    if (mapped) { nl[nn++] = (struct vrun){mlo, mhi}; nvn_be_pool_add(v->base + mlo, mhi - mlo); }
    for (int a = 1; a < nn; a++) for (int b = a; b > 0 && nl[b].lo < nl[b - 1].lo; b--) { struct vrun t = nl[b]; nl[b] = nl[b - 1]; nl[b - 1] = t; }
    free(v->run); v->run = nl; v->nrun = nn;
}
// Page table of a sparse pool (see above). Caller holds g_vp_mu
static const vpte *vpt_get(const struct vpool *v, uint64_t o) { const vpte *l = v->pt[o >> 21]; return l ? &l[(o >> 12) & (VPT_LEAF - 1)] : NULL; }
enum { VLOG_N = 4096 };
static void vpt_set(struct vpool *v, uint64_t voff, uint64_t size, const uint8_t *phys) {
    uint64_t gen = atomic_fetch_add(&g_vp_gen, 1) + 1; atomic_store(&v->gen, gen);
    if (v->log) { v->log[atomic_load_explicit(&v->nlog, memory_order_relaxed) % VLOG_N] = (struct vlog_ent){voff, size}; atomic_fetch_add_explicit(&v->nlog, 1, memory_order_release); }   // 先写条目再推进计数（读者按计数取）
    for (uint64_t o = 0; o < size; o += 0x1000) {
        vpte **l = &v->pt[(voff + o) >> 21];
        if (!*l && !phys) continue;   // never mapped: stays unmapped
        if (!*l && !(*l = calloc(VPT_LEAF, sizeof **l))) die("sparse pool page table allocation failed");
        (*l)[((voff + o) >> 12) & (VPT_LEAF - 1)] = (vpte){phys ? phys + o : NULL, gen};
    }
}
// Host pages of a virtual pool that cannot be aliased (审计 #1592 P1-112). NVN maps at 4 KB, the host aliases at its page (16 KB on Apple systems): a request whose virtual offset, size or
//   physical address is not on a host page used to live in the page table only. Textures read it there (nvn_sparse_copy), but a vertex / index / uniform / storage buffer, an indirect
//   record or a copy at such an address resolved to no Metal buffer: the draw read zeros or a dummy while the mapping had returned success (11 games of the 11-01..11-06 census map so, most
//   of them a whole pool out of a physical pool that is 4 KB but not 16 KB aligned). Such a host page is now made plain memory in the reservation, filled from the page table (each 4 KB from the
//   physical page behind it, an unmapped one 0), and joins the runs like an alias, so every buffer path addresses it. A copy is made
//   - on first use as a buffer (nvn_sparse_shadow from the Metal backend's address lookup, nvn_host_readable for host reads): a pool only textures use pays nothing;
//   - at once for a page already addressable (an alias or a copy) that an unaligned request changes: an alias would keep showing the old physical page.
// Kept current: a mapping request over it refills it; a CPU write to a physical page behind it is caught by the dirty tracker and refilled at the next queue submit (vsh_sync).
//   An aligned request over it replaces it with an alias (vsh_drop). Not carried back: a GPU write into it stays in the copy (said once). Caller holds g_vp_mu throughout
enum { VSH_CAP_PAGES = 16384 };   // 256 MB of copies at a 16 KB page: past it a buffer at such an address stays unaddressable (red)
static int vrun_has(const struct vpool *v, uint64_t lo, uint64_t hi) { for (int k = 0; k < v->nrun; k++) if (lo >= v->run[k].lo && hi <= v->run[k].hi) return 1; return 0; }
static int vsh_find(const struct vpool *v, uint64_t off) { for (int i = 0; i < v->nsh; i++) if (v->sh[i].off == off) return i; return -1; }
static void vsh_fill(struct vpool *v, int i, uint64_t pg) {   // protect the physical pages first, then stamp, then copy: a write before the copy is in it, one after it is newer than the stamp
    struct vshp *s = &v->sh[i]; uint8_t *d = v->base + s->off;
    for (uint64_t o = 0; o < pg; o += 0x1000) { const vpte *e = s->off + o < v->size ? vpt_get(v, s->off + o) : NULL; if (e && e->phys) nvn_dirty_protect(e->phys, 0x1000); }
    s->stamp = nvn_dirty_now();
    for (uint64_t o = 0; o < pg; o += 0x1000) { const vpte *e = s->off + o < v->size ? vpt_get(v, s->off + o) : NULL; if (e && e->phys) memcpy(d + o, e->phys, 0x1000); else memset(d + o, 0, 0x1000); }
}
static int vsh_make(struct vpool *v, uint64_t off, uint64_t pg) {   // the host page at off becomes a copy (its run is the caller's: vrun_set)
    if (vsh_find(v, off) >= 0) return 1;
    if (v->nsh == v->csh) { int c = v->csh ? 2 * v->csh : 16; struct vshp *t = realloc(v->sh, (size_t)c * sizeof *t); if (!t) die("virtual pool copied-page table allocation failed"); v->sh = t; v->csh = c; }
    if (mmap(v->base + off, pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) { LOG("【红】 virtual pool %p: host page %#llx cannot be made a copy (errno %d): a buffer there stays unaddressable", (const void *)v->pool, (unsigned long long)off, errno); return 0; }
#if defined(__ANDROID__) || defined(__OHOS__)
    if (!rp_buf_alias_set((uintptr_t)v->base + off, pg, 0)) die("cannot detach copied buffer page");
    s2a_vp_alias_remove(&v->alias, off, off + pg);   // an alias it replaces: the record goes (as an unmap)
#endif
    v->sh[v->nsh++] = (struct vshp){off, 0}; vsh_fill(v, v->nsh - 1, pg);
    uint64_t n = atomic_fetch_add(&g_vsh_pages, 1) + 1; static _Atomic uint64_t made; uint64_t k = atomic_fetch_add(&made, 1) + 1;
    if (k == 1 || k % 1000 == 0) LOG("[nvn] virtual pool %p: host page %#llx is not aliasable (a mapping in it is not on a host page): buffers read a copy of it filled from the page table (%llu made, %llu alive; GPU writes there stay in the copy)",
                                     (const void *)v->pool, (unsigned long long)off, (unsigned long long)k, (unsigned long long)n);
    return 1;
}
static void vsh_drop(struct vpool *v, uint64_t lo, uint64_t hi) {   // an aligned request maps over these pages: aliases (or inaccessible pages) again
    int j = 0; for (int i = 0; i < v->nsh; i++) if (v->sh[i].off < lo || v->sh[i].off >= hi) v->sh[j++] = v->sh[i];
    atomic_fetch_sub(&g_vsh_pages, (uint64_t)(v->nsh - j)); v->nsh = j;
}
// → 1 when every host page of [o, o+n) is addressable, made so here where it can be; pages up to o+want are made too while they can be (a binding found by its first byte)
static int vsh_cover(struct vpool *v, uint64_t o, uint64_t n, uint64_t want) {
    uint64_t pg = (uint64_t)getpagesize(), lo = o & ~(pg - 1), must = (o + (n ? n : 1) + pg - 1) & ~(pg - 1), hi = (o + (want > n ? want : n ? n : 1) + pg - 1) & ~(pg - 1), need = 0;
    if (must > v->len) return 0;   // beyond the reservation: no host address to give
    if (hi > v->len) hi = v->len;
    for (uint64_t p = lo; p < hi; p += pg) {   // first check them: a page with nothing mapped behind it stays unaddressable (on hardware it faults); past what must be covered the range just ends there
        if (vrun_has(v, p, p + pg)) continue;
        int any = 0; for (uint64_t q = p; q < p + pg && !any; q += 0x1000) { const vpte *e = q < v->size ? vpt_get(v, q) : NULL; any = e && e->phys; }
        if (!any) { if (p < must) return 0; hi = p; break; }
        need++;
    }
    if (need && atomic_load(&g_vsh_pages) + need > VSH_CAP_PAGES) { static _Atomic int said; if (!atomic_fetch_add(&said, 1)) LOG("【红】 virtual pool %p: %#llx+%#llx would take copied pages past %d: the buffer stays unaddressable (first case)", (const void *)v->pool, (unsigned long long)o, (unsigned long long)n, VSH_CAP_PAGES); return 0; }
    uint64_t rlo = 0; int open = 0;
    for (uint64_t p = lo; p <= hi; p += pg) {
        int mk = p < hi && !vrun_has(v, p, p + pg);
        if (mk && !vsh_make(v, p, pg)) { if (open) vrun_set(v, rlo, p, 1); return 0; }
        if (mk && !open) { rlo = p; open = 1; }
        if (!mk && open) { vrun_set(v, rlo, p, 1); open = 0; }
    }
    return 1;
}
static void vsh_sync(void) {   // queue submit: refill the copies whose physical pages the CPU wrote since their last fill
    if (!atomic_load_explicit(&g_vsh_pages, memory_order_relaxed)) return;
    uint64_t pg = (uint64_t)getpagesize(); vp_lock();
    for (size_t k = 0; k < g_nvpa; k++) { struct vpool *v = g_vpa[k];
        for (int i = 0; i < v->nsh; i++) { int dirty = 0;
            for (uint64_t q = v->sh[i].off; q < v->sh[i].off + pg && !dirty; q += 0x1000) { const vpte *e = q < v->size ? vpt_get(v, q) : NULL; dirty = e && e->phys && nvn_dirty_since(e->phys, 0x1000, v->sh[i].stamp); }
            if (dirty) vsh_fill(v, i, pg); } }
    vp_unlock();
}
int nvn_sparse_shadow(uint64_t a, uint64_t n, uint64_t want) {   // Metal backend: an address no pool holds; → 1 when it now is (a virtual pool's pages made copies)
    vp_lock(); size_t k = vpa_find(a); struct vpool *v = k < g_nvpa ? g_vpa[k] : NULL; int ok = 0;
    if (v && a < (uint64_t)(uintptr_t)v->base + v->len) ok = vsh_cover(v, a - (uint64_t)(uintptr_t)v->base, n, want);
    vp_unlock(); return ok;
}
static int vpool_map(struct vpool *v, const map_req *r) {   // caller holds g_vp_mu; → 0 on failure
    // 0 字节：没有页要挂也没有页要撤，照成功（#962：原先交给 vm_remap，回 KERN_INVALID_ARGUMENT，整次 MapVirtual 判失败，游戏随后读空指针）
    if (!r->size) return 1;
    uint64_t pg = (uint64_t)getpagesize();
    if (r->voff < 0 || (uint64_t)r->voff + r->size > v->size) { LOG("【红】 virtual pool %p: request at %#llx+%#llx is beyond the pool's %#llx", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)v->size); return 0; }
    if (((uint64_t)r->voff | r->size | (uint64_t)r->poff) & 0xFFF) { LOG("【红】 virtual pool %p: request %#llx+%#llx (physical offset %#llx) is not 4 KB aligned", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)r->poff); return 0; }
    const pool_t *ph = (const pool_t *)r->phys;
    if (ph && (!ph->mem || r->poff < 0 || (uint64_t)r->poff + r->size > ph->size)) { LOG("【红】 virtual pool %p: physical pool %p (%#llx bytes, storage %p) cannot supply %#llx+%#llx", (const void *)v->pool, (const void *)ph, (unsigned long long)ph->size, (void *)ph->mem, (unsigned long long)r->poff, (unsigned long long)r->size); return 0; }
    vpt_set(v, (uint64_t)r->voff, r->size, ph ? ph->mem + r->poff : NULL);
    vm_address_t to = (vm_address_t)(v->base + r->voff);
    // Not aliased (see above): kept in the page table; host pages of it already addressable become copies of the table (vsh_*), the others when a buffer first uses them
    // 别名表（s2a_vp_alias_add / remove）依赖这里的对齐：只有 [voff, voff+size) 与源地址都按宿主页
    // 对齐（且不越界）的请求才会走到下面的 ref / vm_remap / remove / add；4K 对齐但 16K 不对齐的
    // NVN 请求在这里只进页表。若删掉这个早退，别名表入口的对齐断言会直接 die（调用方违约）。
    if ((uint64_t)r->voff + r->size > v->len || ((uintptr_t)to | r->size | (uint64_t)r->voff) & (pg - 1) || (ph && (((uintptr_t)ph->mem + (uintptr_t)r->poff) & (pg - 1)))) {
        int beyond = (uint64_t)r->voff + r->size > v->len;
        static _Atomic uint64_t n; if (!atomic_fetch_add(&n, 1)) LOG("[nvn] virtual pool %p: a request %s (%s %#llx+%#llx) is kept in the page table: textures read it there, %s (later cases are not logged)",
                                                                   (const void *)v->pool, beyond ? "beyond the reservation" : "not aligned to the host page", r->phys ? "map" : "unmap", (unsigned long long)r->voff, (unsigned long long)r->size,
                                                                   beyond ? "buffers past the reservation have no host address" : "buffers read copies of its host pages");
        if (v->nrun || v->nsh) {
            uint64_t lo = (uint64_t)r->voff & ~(pg - 1), hi = ((uint64_t)r->voff + r->size + pg - 1) & ~(pg - 1); if (hi > v->len) hi = v->len;
            for (uint64_t q = lo; q < hi; q += pg) { int i = vsh_find(v, q); if (i >= 0) { vsh_fill(v, i, pg); continue; } if (vrun_has(v, q, q + pg) && vsh_make(v, q, pg)) vrun_set(v, q, q + pg, 1); }
        }
        return 1;
    }
    vsh_drop(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size);
    if (!r->phys) {   // unmap: inaccessible pages again
        if (mmap((void *)to, r->size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) { LOG("【红】 virtual pool unmap %#lx+%#llx failed (errno %d)", (unsigned long)to, (unsigned long long)r->size, errno); return 0; }
#if defined(__ANDROID__) || defined(__OHOS__)
        // 旧别名已被这张不可访问的映射覆盖：先切记录并 unref，源页若已死在这里打洞。
        if (!rp_buf_alias_set((uintptr_t)v->base + r->voff, r->size, 0)) die("cannot detach virtual buffer alias");
        s2a_vp_alias_remove(&v->alias, (uint64_t)r->voff, (uint64_t)r->voff + r->size);
#endif
        vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 0); return 1;
    }
    uintptr_t from = (uintptr_t)ph->mem + (uintptr_t)r->poff;
#if defined(__ANDROID__) || defined(__OHOS__)
    // 长寿别名入口核活（owner Q1-A）：源落在背板区就整段 ref；源页已死回 -EFAULT，
    // 保留今天私有占位上 mremap 也失败的响亮失败（不静默建一个读到零的别名）。
    int refd = 0;
    int ref = s2a_mem_backing_alias_ref((uint64_t)from, r->size);
    if (ref < 0) {
        LOG("【红】 virtual pool %p: mapping source %#lx+%#llx %s (errno %d)", (const void *)v->pool,
            (unsigned long)from, (unsigned long long)r->size, ref == -EFAULT ? "page is dead" : "backing ref failed", -ref);
        return 0;
    }
    refd = ref == 1;
#endif
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &to, r->size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, FALSE, &cur, &max, VM_INHERIT_NONE);
#if defined(__ANDROID__) || defined(__OHOS__)
    // 不论成败都切旧记录：mach.h 的 vm_remap 失败时也会在 dst 补私有 PROT_NONE，
    // 旧别名已经没了，不能留着对旧源页的引用。
    if (!rp_buf_alias_set((uintptr_t)v->base + r->voff, r->size, 0)) die("cannot detach virtual buffer alias");
    s2a_vp_alias_remove(&v->alias, (uint64_t)r->voff, (uint64_t)r->voff + r->size);
#endif
    if (kr != KERN_SUCCESS || to != (vm_address_t)(v->base + r->voff)) {
#if defined(__ANDROID__) || defined(__OHOS__)
        if (refd) s2a_mem_backing_alias_unref((uint64_t)from, r->size);
#endif
        LOG("【红】 virtual pool remap %#lx → %p (%#llx bytes) failed: %d", (unsigned long)from, (void *)(v->base + r->voff), (unsigned long long)r->size, kr); return 0;
    }
#if defined(__ANDROID__) || defined(__OHOS__)
    if (refd && s2a_vp_alias_add(&v->alias, (uint64_t)r->voff, (uint64_t)r->voff + r->size, (uint64_t)from)) {
        s2a_mem_backing_alias_unref((uint64_t)from, r->size);
        // 记录没登记成功就不能留着一个无 ref 的 dst 别名（源页之后被打洞会静默读零）；
        // 换回不可访问占位，调用方按映射失败处理。
        if (mmap((void *)to, r->size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED)
            die("virtual pool alias table allocation failed and dst restore failed");
        LOG("【红】 virtual pool %p: alias range table allocation failed", (const void *)v->pool);
        return 0;
    }
#endif
    if (!rp_buf_alias_set(to, r->size, from)) {
        if (mmap((void *)to, r->size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED)
            die("virtual buffer alias registration failed and dst restore failed");
        s2a_vp_alias_remove(&v->alias, (uint64_t)r->voff, (uint64_t)r->voff + r->size);
        vpt_set(v, (uint64_t)r->voff, r->size, NULL);
        vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 0);
        LOG("【红】 virtual pool %p: buffer alias registration failed", (const void *)v->pool);
        return 0;
    }
    mprotect((void *)to, r->size, PROT_READ | PROT_WRITE);   // the source may be write-protected by the dirty tracker at this moment; the alias itself is plain memory
    vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 1);
    uint64_t end = (uint64_t)r->voff + r->size;
    if (end >> 27 > v->top >> 27) LOG("[nvn] virtual pool %p: mapped up to %#llx (%d runs)", (const void *)v->pool, (unsigned long long)end, v->nrun);
    if (end > v->top) v->top = end;
    return 1;
}
#else   // 苹果宿主
// True sparse virtual pools (Monster Hunter Rise: RE Engine's GPU heap is one 64 GB virtual pool, built from separately allocated 4 MB physical pools mapped at offsets of their own; boot maps 45 of them
// over the first 180 MB). A virtual pool is GPU address space only: the game never reads it on the CPU (its Map is NULL), so it takes no host address space. It used to reserve its size
//   of host address space (at most 1 GB on iOS) and vm_remap physical pages into it (#1143: a Switch 2 racing game on an iPhone made over a hundred virtual pools, each took up to 1 GB, the host's
//   3 GB of address space ran out within a minute (errno 12) and the shader library mapped after them failed).
// Now its address is a number in a window no host or guest memory uses (VPF_LO..VPF_HI), handed out per pool at Initialize; buffer / texture addresses are that number + offset, as before.
// Every mapping request goes into a page table at 4 KB (the physical memory behind each virtual page, and the mapping generation that set it): textures read through it (nvn_sparse_copy),
//   uploaded again only when a mapping in their range changes or the CPU writes a physical page behind them (nvn_sparse_since; the physical pools are dirty-tracked). The game fills its sparse
//   textures (flag 0x20, 32 GB into the pool) that way: it writes the tiles on the CPU into 4 MB physical pools, then maps them; no GPU copy writes them.
// Buffers: a run of virtual pages with consecutive physical memory behind it is registered with the replay as an alias of the physical pool's own Metal buffer (nvn_be_pool_alias: the same
//   bytes for the GPU and the CPU, at any 4 KB offset; the host-page alignment the remapping needed is gone, and with it the copies a 4 KB but not 16 KB aligned request took). A buffer
//   spanning pages of different physical memory gets a copy filled from the page table when first used (vsh_*). The host reads pool bytes through nvn_host_ptr
#define VPF_LO 0x10000000000ull    // above the Switch 1 GPU address window (GVA_HI) and every guest / host address of an Apple or Android host (iOS and 39-bit Android cannot map there)
#define VPF_HI 0x40000000000ull    // 3 TB of virtual pools alive at once
#define VPF_GAP (2ull << 20)       // pools are 2 MB aligned with a 2 MB gap: an address past a pool's end is in no pool
enum { VPT_LEAF = 512 };   // a page-table leaf covers 2 MB
typedef struct { const uint8_t *phys; uint64_t gen; } vpte;
// One per virtual pool, allocated at Initialize and freed at Finalize. run: the runs of consecutive physical memory, grown as needed (a texture's pool usually has 1–3; Monster Hunter Rise's heap hundreds)
struct vpool { const pool_t *pool; uint8_t *base; uint64_t len, size, top; int nrun; struct vrun { uint64_t lo, hi; const uint8_t *phys; } *run; vpte **pt;
    struct vlog_ent { uint64_t off, len; } *log; _Atomic uint64_t nlog;   // 映射变更日志（马车世界缺口 ①，照上游参考实现（MIT）hle_nvn.c vlog）：每次映射请求 [off, off + len) 进环（VLOG_N），nlog 是总条数；有人问才建（nvn_sparse_log_seq）
    int nsh, csh; struct vshb { uint64_t off, len, stamp; uint8_t *mem; } *sh;   // copies (see vsh_*): virtual offset and length, dirty stamp of the last fill, host memory (its replay buffer owns it)
    _Atomic uint64_t gen; uint64_t id;   // gen：本池最近一次映射请求的全局代号（vpt_set）；id：建池序号（全局递增，池对象地址复用时区分新旧池）。GPU 写的稀疏贴图按它们跟映射（nvn_mtl.m gstex_*）
};   // base: the pool's address (a number, no host memory behind it); len: its share of the window (size rounded up to 2 MB); top: highest mapped end so far (logged per 128 MB)
// Where the virtual pools are found: by pool pointer in a hash table (texture lookups ask per texture whether its pool is one: nvn_sparse_pool), by address in an array sorted by base
//   (nvn_host_ptr, nvn_sparse_shadow, the window's free gaps). A UE4 game keeps a pool per streamed texture: Torchlight III created 1140 in one run, hundreds alive at once; the old table had 4 slots.
//   Both change only under g_vp_mu (Initialize / Finalize); the hash table also under g_vph_lk for writing, so nvn_sparse_pool reads it under g_vph_lk alone. Whoever holds g_vp_mu reads both freely
static nvn_hmap g_vph = {.klen = 8};
static pthread_rwlock_t g_vph_lk = PTHREAD_RWLOCK_INITIALIZER;
static struct vpool **g_vpa; static size_t g_nvpa, g_cvpa;
static pthread_mutex_t g_vp_mu = PTHREAD_MUTEX_INITIALIZER;
// 锁序（#1217 原生层裁定第 1 条）：NVN 可以 g_vp_mu → svc_mem g_mu，反向禁止。
// 代码依据（interface owner 回合 #6567620762 ①）：
//   - android/native/runtime/fault/thread_android.inc:426：停世界只对 guest PC 发 SIGURG 停车，
//     SVC / 原生回调里的线程照跑（「Host lock owners keep progressing」）。
//   - runtime/src/svc_mem.c:1086：backing_begin_locked 先 GMU_UNLOCK 放掉 g_mu，之后才等 writers 停稳；
//     所以持 g_vp_mu 的 NVN 线程去取 g_mu 最多等到改表一方放锁，不会和停世界成环。
// 渲染 / GPU 回调线程不调 s2a_mem_backing_alias_ref / unref，不会在持 g_vp_mu 的路径上取 g_mu。
// 反向（持 g_mu 再取 g_vp_mu）由下面的 Debug 断言拦住。
// Debug 构建下取 g_vp_mu 前问一次 svc_mem 当前线程是否持着 g_mu；是则直接 abort。
// g_vph_lk 只在持 g_vp_mu 时写、nvn_sparse_pool 单独读，读写都不再取别的锁：g_vp_mu → g_vph_lk 一个方向
// 回放层的池表锁（nvn_mtl.m g_mu）在它之后：g_vp_mu → 回放 g_mu（登记 / 注销别名与副本）
static void vp_lock(void) {
#if defined(__ANDROID__) || defined(__OHOS__)
    s2a_mem_lockorder_vp_enter();
#endif
    pthread_mutex_lock(&g_vp_mu);
}
static void vp_unlock(void) { pthread_mutex_unlock(&g_vp_mu); }
static _Atomic uint64_t g_vp_gen;   // bumped per mapping request
static struct vpool *vpool_of(const pool_t *o) { return nvn_hm_get(&g_vph, &o); }   // caller holds g_vp_mu (or g_vph_lk for reading)
static size_t vpa_find(uint64_t a) {   // caller holds g_vp_mu; → index of the last pool whose base ≤ a, or g_nvpa when none
    size_t lo = 0, hi = g_nvpa;
    while (lo < hi) { size_t m = (lo + hi) / 2; if ((uint64_t)(uintptr_t)g_vpa[m]->base <= a) lo = m + 1; else hi = m; }
    return lo ? lo - 1 : g_nvpa;
}
static struct vpool *vpool_holding(uint64_t a) {   // caller holds g_vp_mu; → the pool whose [base, base + size) holds a
    size_t k = vpa_find(a); struct vpool *v = k < g_nvpa ? g_vpa[k] : NULL;
    return v && a - (uint64_t)(uintptr_t)v->base < v->size ? v : NULL;
}
// The first gap of the window that holds len bytes (pools sorted by base; a gap of VPF_GAP before and after); NULL when none. Caller holds g_vp_mu
static uint8_t *vpf_find(uint64_t len) {
    uint64_t g = VPF_LO;
    for (size_t i = 0; i < g_nvpa; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_vpa[i]->base, e = b + g_vpa[i]->len;
        if (e + VPF_GAP <= g || b >= VPF_HI) continue;   // behind the candidate, or a frame recording's pool outside the window (s2a_hle_vpool_load)
        if (g + len + VPF_GAP <= b) break;
        g = e + VPF_GAP;
    }
    return g >= VPF_LO && g + len <= VPF_HI ? (uint8_t *)(uintptr_t)g : NULL;
}
static int vpf_overlaps(uint64_t a, uint64_t len) {   // caller holds g_vp_mu
    for (size_t i = 0; i < g_nvpa; i++) { uint64_t b = (uint64_t)(uintptr_t)g_vpa[i]->base; if (a < b + g_vpa[i]->len && b < a + len) return 1; }
    return 0;
}
static void vpool_publish(struct vpool *v) {   // caller holds g_vp_mu
    if (g_nvpa == g_cvpa) { size_t c = g_cvpa ? 2 * g_cvpa : 64; struct vpool **a = realloc(g_vpa, c * sizeof *a); if (!a) die("virtual pool table allocation failed"); g_vpa = a; g_cvpa = c; }
    size_t i = g_nvpa; while (i && g_vpa[i - 1]->base > v->base) { g_vpa[i] = g_vpa[i - 1]; i--; }
    g_vpa[i] = v; g_nvpa++;
    pthread_rwlock_wrlock(&g_vph_lk); nvn_hm_put(&g_vph, &v->pool, v); pthread_rwlock_unlock(&g_vph_lk);   // filled before it is visible
}
static void vpool_unpublish(struct vpool *v) {   // caller holds g_vp_mu
    pthread_rwlock_wrlock(&g_vph_lk); nvn_hm_del(&g_vph, &v->pool); pthread_rwlock_unlock(&g_vph_lk);
    size_t i = vpa_find((uint64_t)(uintptr_t)v->base);
    if (i < g_nvpa && g_vpa[i] == v) { memmove(g_vpa + i, g_vpa + i + 1, (g_nvpa - i - 1) * sizeof *g_vpa); g_nvpa--; }
}
// 建页表、登记（建池 vpool_reserve：a = NULL，在窗口里找一段；帧回放 s2a_hle_vpool_load：a = 录时的地址，与活着的池重叠就不建）→ NULL：窗口满 / 重叠
static struct vpool *vpool_new(const pool_t *o, uint8_t *a, uint64_t len) {
    struct vpool *v = calloc(1, sizeof *v);
    if (!v || !(v->pt = calloc((o->size + (2u << 20) - 1) >> 21 ? (o->size + (2u << 20) - 1) >> 21 : 1, sizeof *v->pt))) die("sparse pool page table allocation failed");
    static _Atomic uint64_t ids; v->pool = o; v->len = len; v->size = o->size; v->id = atomic_fetch_add(&ids, 1) + 1;
    vp_lock();
    if (!a) a = vpf_find(len); else if (vpf_overlaps((uint64_t)(uintptr_t)a, len)) a = NULL;
    if (a) { v->base = a; vpool_publish(v); }
    vp_unlock();
    if (!a) { free(v->pt); free(v); return NULL; }
    return v;
}
static _Atomic uint64_t g_vsh_bytes;   // copies alive in all virtual pools (vsh_*)
static uint8_t *vpool_reserve(const pool_t *o) {   // → the address of a virtual pool, NULL when the window has no room for it
    if (!(o->flags & 0x200)) return NULL;
    uint64_t len = o->size ? (o->size + VPF_GAP - 1) & ~(VPF_GAP - 1) : VPF_GAP;
    struct vpool *v = vpool_new(o, NULL, len);
    vp_lock(); size_t live = g_nvpa; vp_unlock();
    if (!v) { LOG("【红】 virtual pool %p: %#llx bytes do not fit in the virtual pool address window any more (%zu pools alive)", (const void *)o, (unsigned long long)o->size, live); return NULL; }
    static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;   // a UE4 game makes one per streamed texture: the first ones and every 1000th
    if (k <= 8 || k % 1000 == 0) LOG("[nvn] virtual pool %p (%#llx bytes, flags %#x): address %p (no host memory: buffers alias the physical pools through its page table) (%llu so far, %zu alive)",
                                     (const void *)o, (unsigned long long)o->size, o->flags, (void *)v->base, (unsigned long long)k, live);
    return v->base;
}
static void vpool_release(const pool_t *o) {
    vp_lock(); struct vpool *v = vpool_of(o);
    if (v) { vpool_unpublish(v);
        for (int k = 0; k < v->nrun; k++) nvn_be_pool_del(v->base + v->run[k].lo);
        for (int i = 0; i < v->nsh; i++) { nvn_be_pool_del(v->base + v->sh[i].off + 1); atomic_fetch_sub(&g_vsh_bytes, v->sh[i].len); }   // a copy's memory goes with its replay buffer
        for (uint64_t l = 0; l < (v->size + (2u << 20) - 1) >> 21; l++) free(v->pt[l]);
        free(v->pt); free(v->run); free(v->sh); free(v->log); free(v); }
    vp_unlock();
}
// A run's registration with the replay: an alias of the physical pool's buffer over [base + lo, base + hi). It finds none when the physical memory is no registered pool or crosses the
//   chunks of one longer than a Metal buffer may be (nvn_be_pool_add): a buffer there then gets a copy (nvn_sparse_shadow)
static void vrun_reg(struct vpool *v, const struct vrun *r) {
    if (nvn_be_pool_alias((uint64_t)(uintptr_t)v->base + r->lo, r->hi - r->lo, (uint64_t)(uintptr_t)r->phys)) return;
    static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k <= 4 || !(k & (k - 1))) LOG("【黄】 virtual pool %p: run %#llx+%#llx (physical %p) is in no single registered pool: buffers there read a copy (%llu so far)",
                                      (const void *)v->pool, (unsigned long long)r->lo, (unsigned long long)(r->hi - r->lo), (const void *)r->phys, (unsigned long long)k);
}
// Runs of a sparse pool: [lo, hi) sorted, disjoint, each with consecutive physical memory behind it (phys at lo); a neighbour joins only when its physical memory continues it.
//   Map (phys) or unmap (NULL) [lo, hi): runs it overlaps are cut (what is left on either side registered again), the new run joins its continuing neighbours. Caller holds g_vp_mu
static void vrun_set(struct vpool *v, uint64_t lo, uint64_t hi, const uint8_t *phys) {
    struct vrun *nl = malloc(((size_t)v->nrun + 3) * sizeof *nl); char *fresh = malloc((size_t)v->nrun + 3); int nn = 0;
    if (!nl || !fresh) die("virtual pool run table allocation failed");
    struct vrun m = {lo, hi, phys};
    for (int k = 0; k < v->nrun; k++) {
        struct vrun r = v->run[k];
        if (r.hi <= lo || r.lo >= hi) {
            if (phys && r.hi == lo && r.phys + (r.hi - r.lo) == phys) { nvn_be_pool_del(v->base + r.lo); m.lo = r.lo; m.phys = r.phys; continue; }   // continues into the new run
            if (phys && r.lo == hi && phys + (hi - lo) == r.phys) { nvn_be_pool_del(v->base + r.lo); m.hi = r.hi; continue; }
            nl[nn] = r; fresh[nn++] = 0; continue;   // untouched
        }
        nvn_be_pool_del(v->base + r.lo);
        if (r.lo < lo) { struct vrun l = {r.lo, lo, r.phys}; if (phys && l.phys + (lo - l.lo) == phys) { m.lo = l.lo; m.phys = l.phys; } else { nl[nn] = l; fresh[nn++] = 1; } }
        if (r.hi > hi) { struct vrun h = {hi, r.hi, r.phys + (hi - r.lo)}; if (phys && phys + (hi - lo) == h.phys) m.hi = h.hi; else { nl[nn] = h; fresh[nn++] = 1; } }
    }
    if (phys) { nl[nn] = m; fresh[nn++] = 1; }
    for (int a = 1; a < nn; a++) for (int b = a; b > 0 && nl[b].lo < nl[b - 1].lo; b--) { struct vrun t = nl[b]; nl[b] = nl[b - 1]; nl[b - 1] = t; char f = fresh[b]; fresh[b] = fresh[b - 1]; fresh[b - 1] = f; }
    for (int a = 0; a < nn; a++) if (fresh[a]) vrun_reg(v, &nl[a]);
    free(fresh); free(v->run); v->run = nl; v->nrun = nn;
}
// Page table of a sparse pool (see above). Caller holds g_vp_mu
static const vpte *vpt_get(const struct vpool *v, uint64_t o) { const vpte *l = v->pt[o >> 21]; return l ? &l[(o >> 12) & (VPT_LEAF - 1)] : NULL; }
enum { VLOG_N = 4096 };
static void vpt_set(struct vpool *v, uint64_t voff, uint64_t size, const uint8_t *phys) {
    uint64_t gen = atomic_fetch_add(&g_vp_gen, 1) + 1; atomic_store(&v->gen, gen);
    if (v->log) { v->log[atomic_load_explicit(&v->nlog, memory_order_relaxed) % VLOG_N] = (struct vlog_ent){voff, size}; atomic_fetch_add_explicit(&v->nlog, 1, memory_order_release); }   // 先写条目再推进计数（读者按计数取）
    for (uint64_t o = 0; o < size; o += 0x1000) {
        vpte **l = &v->pt[(voff + o) >> 21];
        if (!*l && !phys) continue;   // never mapped: stays unmapped
        if (!*l && !(*l = calloc(VPT_LEAF, sizeof **l))) die("sparse pool page table allocation failed");
        (*l)[((voff + o) >> 12) & (VPT_LEAF - 1)] = (vpte){phys ? phys + o : NULL, gen};
    }
}
// Copies of a virtual pool's pages (审计 #1592 P1-112): a buffer whose range spans virtual pages of different physical memory is no alias of one physical pool. It gets plain host memory
//   filled from the page table (each 4 KB from the physical page behind it), registered with the replay at its virtual address, on first use (nvn_sparse_shadow from the Metal backend's
//   address lookup, nvn_host_ptr for host reads): a pool only textures use pays nothing. Every page of the range must be mapped; past it the copy runs on to the last mapped page up to
//   what the lookup wants (a binding found by its first byte gets its whole range), unmapped pages in between read 0. Kept current: a CPU write to a physical page behind it is caught by the dirty tracker and refilled at the next queue submit (vsh_sync); a mapping
//   request over it drops it (the next use sees the new mapping). Not carried back: a GPU write into it stays in the copy. Caller holds g_vp_mu throughout
enum { VSH_CAP_MB = 256 };   // past it a buffer at such an address stays unaddressable (red)
static int vsh_at(const struct vpool *v, uint64_t lo, uint64_t hi) { for (int i = 0; i < v->nsh; i++) if (lo >= v->sh[i].off && hi <= v->sh[i].off + v->sh[i].len) return i; return -1; }
static void vsh_fill(struct vpool *v, struct vshb *s) {   // protect the physical pages first, then stamp, then copy: a write before the copy is in it, one after it is newer than the stamp
    for (uint64_t o = 0; o < s->len; o += 0x1000) { const vpte *e = vpt_get(v, s->off + o); if (e && e->phys) nvn_dirty_protect(e->phys, 0x1000); }
    s->stamp = nvn_dirty_now();
    for (uint64_t o = 0; o < s->len; o += 0x1000) { const vpte *e = vpt_get(v, s->off + o); if (e && e->phys) memcpy(s->mem + o, e->phys, 0x1000); else memset(s->mem + o, 0, 0x1000); }
}
static void vsh_drop(struct vpool *v, uint64_t lo, uint64_t hi) {   // copies [lo, hi) overlaps go
    int j = 0;
    for (int i = 0; i < v->nsh; i++) {
        if (v->sh[i].off + v->sh[i].len <= lo || v->sh[i].off >= hi) { v->sh[j++] = v->sh[i]; continue; }
        nvn_be_pool_del(v->base + v->sh[i].off + 1); atomic_fetch_sub(&g_vsh_bytes, v->sh[i].len);
    }
    v->nsh = j;
}
// → index of a copy holding [o, o+n), made here when none does; -1 when a page of it has nothing mapped behind it (on hardware the GPU faults there) or no copy can be made
static int vsh_cover(struct vpool *v, uint64_t o, uint64_t n, uint64_t want) {
    if (!n) n = 1;
    uint64_t lo = o & ~0xFFFull, must = (o + n + 0xFFF) & ~0xFFFull, hi = (o + (want > n ? want : n) + 0xFFF) & ~0xFFFull;
    if (o >= v->size || must > ((v->size + 0xFFF) & ~0xFFFull)) return -1;
    if (hi > ((v->size + 0xFFF) & ~0xFFFull)) hi = (v->size + 0xFFF) & ~0xFFFull;
    uint64_t last = must;   // past what must be covered: on to the last mapped page up to want; unmapped pages between read 0 (sparse memory reads 0 where nothing is mapped)
    for (uint64_t q = lo; q < hi; q += 0x1000) { const vpte *e = vpt_get(v, q); if (e && e->phys) { if (q + 0x1000 > last) last = q + 0x1000; } else if (q < must) return -1; }
    hi = last;
    int i = vsh_at(v, lo, hi); if (i >= 0) return i;
    if (atomic_load(&g_vsh_bytes) + (hi - lo) > ((uint64_t)VSH_CAP_MB << 20)) {
        static _Atomic int said; if (!atomic_fetch_add(&said, 1)) LOG("【红】 virtual pool %p: a copy of %#llx+%#llx would take copies past %d MB: the buffer stays unaddressable (first case)", (const void *)v->pool, (unsigned long long)lo, (unsigned long long)(hi - lo), VSH_CAP_MB);
        return -1; }
    uint64_t pg = (uint64_t)getpagesize(), ml = (hi - lo + pg - 1) & ~(pg - 1);
    uint8_t *mem = mmap(NULL, ml, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) { LOG("【红】 virtual pool %p: no memory for a copy of %#llx+%#llx (errno %d): a buffer there stays unaddressable", (const void *)v->pool, (unsigned long long)lo, (unsigned long long)(hi - lo), errno); return -1; }
    if (v->nsh == v->csh) { int c = v->csh ? 2 * v->csh : 16; struct vshb *t = realloc(v->sh, (size_t)c * sizeof *t); if (!t) die("virtual pool copy table allocation failed"); v->sh = t; v->csh = c; }
    struct vshb *s = &v->sh[v->nsh]; *s = (struct vshb){lo, hi - lo, 0, mem}; vsh_fill(v, s);
    // registered under its address + 1 (a run's key is its address: unmapping one never drops the other); the replay buffer owns the memory from here, on failure too
    if (!nvn_be_pool_add_at((uint64_t)(uintptr_t)v->base + lo, hi - lo, mem, ml, v->base + lo + 1)) { LOG("【红】 virtual pool %p: the copy of %#llx+%#llx could not be registered: a buffer there stays unaddressable", (const void *)v->pool, (unsigned long long)lo, (unsigned long long)(hi - lo)); return -1; }
    v->nsh++; uint64_t tot = atomic_fetch_add(&g_vsh_bytes, hi - lo) + (hi - lo);
    static _Atomic uint64_t made; uint64_t k = atomic_fetch_add(&made, 1) + 1;
    if (k == 1 || k % 1000 == 0) LOG("[nvn] virtual pool %p: %#llx+%#llx spans virtual pages of different physical memory: buffers read a copy of it filled from the page table (%llu made, %llu KB alive; GPU writes there stay in the copy)",
                                     (const void *)v->pool, (unsigned long long)lo, (unsigned long long)(hi - lo), (unsigned long long)k, (unsigned long long)(tot >> 10));
    return v->nsh - 1;
}
static void vsh_sync(void) {   // queue submit: refill the copies whose physical pages the CPU wrote since their last fill
    if (!atomic_load_explicit(&g_vsh_bytes, memory_order_relaxed)) return;
    vp_lock();
    for (size_t k = 0; k < g_nvpa; k++) { struct vpool *v = g_vpa[k];
        for (int i = 0; i < v->nsh; i++) { int dirty = 0;
            for (uint64_t q = v->sh[i].off; q < v->sh[i].off + v->sh[i].len && !dirty; q += 0x1000) { const vpte *e = vpt_get(v, q); dirty = e && e->phys && nvn_dirty_since(e->phys, 0x1000, v->sh[i].stamp); }
            if (dirty) vsh_fill(v, &v->sh[i]); } }
    vp_unlock();
}
int nvn_sparse_shadow(uint64_t a, uint64_t n, uint64_t want) {   // Metal backend: an address no pool holds; → 1 when it now is (a copy of a virtual pool's pages)
    vp_lock(); struct vpool *v = vpool_holding(a); int ok = v && vsh_cover(v, a - (uint64_t)(uintptr_t)v->base, n, want) >= 0;
    vp_unlock(); return ok;
}
static int vpool_map(struct vpool *v, const map_req *r) {   // caller holds g_vp_mu; → 0 on failure
    // 0 字节：没有页要挂也没有页要撤，照成功（#962：原先交给 vm_remap，回 KERN_INVALID_ARGUMENT，整次 MapVirtual 判失败，游戏随后读空指针）
    if (!r->size) return 1;
    if (r->voff < 0 || (uint64_t)r->voff + r->size > v->size) { LOG("【红】 virtual pool %p: request at %#llx+%#llx is beyond the pool's %#llx", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)v->size); return 0; }
    if (((uint64_t)r->voff | r->size | (uint64_t)r->poff) & 0xFFF) { LOG("【红】 virtual pool %p: request %#llx+%#llx (physical offset %#llx) is not 4 KB aligned", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)r->poff); return 0; }
    const pool_t *ph = (const pool_t *)r->phys;
    if (ph && (!ph->mem || r->poff < 0 || (uint64_t)r->poff + r->size > ph->size)) { LOG("【红】 virtual pool %p: physical pool %p (%#llx bytes, storage %p) cannot supply %#llx+%#llx", (const void *)v->pool, (const void *)ph, (unsigned long long)ph->size, (void *)ph->mem, (unsigned long long)r->poff, (unsigned long long)r->size); return 0; }
    const uint8_t *phys = ph ? ph->mem + r->poff : NULL;
    vpt_set(v, (uint64_t)r->voff, r->size, phys);
    vsh_drop(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size);   // copies over it are stale: made again from the new mapping when used
    vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, phys);
    uint64_t end = (uint64_t)r->voff + r->size;
    if (phys && end >> 27 > v->top >> 27) LOG("[nvn] virtual pool %p: mapped up to %#llx (%d runs)", (const void *)v->pool, (unsigned long long)end, v->nrun);
    if (phys && end > v->top) v->top = end;
    return 1;
}
#endif
NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p, int n, const NVNmappingRequest *rq) {
    pool_t *o = (pool_t *)p; const map_req *r = (const map_req *)rq;
    vp_lock(); struct vpool *v = vpool_of(o); int ok = v != NULL;
    for (int i = 0; v && i < n && ok; i++) ok = vpool_map(v, &r[i]);
    vp_unlock();
    if (!v) LOG("【红】 MapVirtual: pool %p (flags %#x) is not a virtual pool with an address range (%s): %d requests refused", (void *)o, o->flags, o->flags & 0x200 ? "none could be given at Initialize" : "not virtual", n);
    return (NVNboolean)ok;
}

// Samplers: the getters read the parameters in the object. Reduction (min / max of the texels a filtered sample covers) maps to the Metal 26 sampler reduction mode (nvn_mtl.m samp_by_id) on an Apple10 GPU; elsewhere the shader's device-made emulation variant reduces at explicit LODs (nvn_mtl.m se_want, translator/dmsh_red.py)
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { SB(b)->reduction = (uint8_t)r; }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *l) { (void)s; (void)l; }
#define SP(s) (&((const samp_t *)(s))->p)
NVN(nvnSamplerGetMinMagFilter, void, const NVNsampler *s, NVNminFilter *mi, NVNmagFilter *ma) { *mi = (NVNminFilter)SP(s)->minf; *ma = (NVNmagFilter)SP(s)->magf; }
NVN(nvnSamplerGetWrapMode, void, const NVNsampler *s, NVNwrapMode *a, NVNwrapMode *b, NVNwrapMode *c) { *a = (NVNwrapMode)SP(s)->ws; *b = (NVNwrapMode)SP(s)->wt; *c = (NVNwrapMode)SP(s)->wr; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *s, float *lo, float *hi) { *lo = SP(s)->lod_min; *hi = SP(s)->lod_max; }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *s) { return SP(s)->lod_bias; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *s, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)SP(s)->cmp_mode; *f = (NVNcompareFunc)SP(s)->cmp_func; }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *s, float *c) { memcpy(c, SP(s)->border, 16); }
// 整数版读回（#355 只交了无符号版）：同 #355 的存法——整数边框色按 float 存在 border[4]，这里四舍五入回整数
NVN(nvnSamplerGetBorderColori, void, const NVNsampler *s, int *c) { for (int i = 0; i < 4; i++) c[i] = (int)lrintf(SP(s)->border[i]); }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *s) { return SP(s)->aniso; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *s) { return (NVNsamplerReduction)SP(s)->reduction; }
// whether two samplers have the same state (games use it to deduplicate sampler pool entries)
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) {
    const samp_par *x = SP(a), *y = SP(b);
    return x->minf == y->minf && x->magf == y->magf && x->ws == y->ws && x->wt == y->wt && x->wr == y->wr && x->cmp_mode == y->cmp_mode && x->cmp_func == y->cmp_func && x->reduction == y->reduction &&
           x->lod_min == y->lod_min && x->lod_max == y->lod_max && x->lod_bias == y->lod_bias && x->aniso == y->aniso && x->lod_snap == y->lod_snap && !memcmp(x->border, y->border, sizeof x->border);
}

// Texture builder / texture / view
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = d; }
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    tex_builder_t *o = (tex_builder_t *)b; o->set_swz = 1; o->swz = (uint16_t)((r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9);
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureBuilderSetSwizzle not identity (%d %d %d %d): remapped with a Metal view when sampling", (int)r, (int)g, (int)bl, (int)a);
}
// STENCIL: the texture reads the stencil of its depth-stencil storage, as a stencil-mode view does (tex_by_id in nvn_mtl.m). Unreal Engine 4.27 makes its stencil shader resources this way: a second texture
// on the depth buffer's storage, built in stencil mode (DRAGON QUEST VII Reimagined's character occlusion mask, edbfc7a088be545e, tests stencil bits 0x1 and 0x84 of two of them; read as depth, the
// bits followed the depth values and the hero was drawn as a dithered silhouette inside a dithered square)
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) { TB(b)->stencil = m == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL; }
// 存储类别：真驱动恒为 0x100 | 硬件 kind 低 8 位，从不为 0（11-03 无人深空 SDK 静态核：GetProcAddress 表第 417 项 sdk+0x4b6750、第 460 项 sdk+0x4b7d10，
//   各分支都汇到 `mov w8, #0x100; bfxil w8, w0, #0, #8`；kind 来自 NvRm 分配器 sdk+0x5d1990，格式不支持时它给 0，驱动照样回 0x100）。宿主没有硬件 kind，回 0x100。
//   原先回 0：无人深空把它记进上传记录（main+0x16ef450 rec+0x1c），为 0 就当「CPU 可写」走 nvnMemoryPoolMap 虚拟池 + memcpy（main+0x16e4240），
//   虚拟池 Map 真机也是 NULL，写到 0x80015600 崩；非 0 走游戏自己按页表拷贝的路（main+0x1858540）。值本身游戏只判非 0、原样填进 MapVirtual 请求，宿主不读
enum { STORAGE_CLASS_HOST = 0x100 };
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { (void)b; return STORAGE_CLASS_HOST; }
// Used by Bowser's Fury: the texture's channel swizzle (set by TextureBuilderSetSwizzle at build time, identity R G B A when unset) and storage class (as for the builder)
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    unsigned w = tex_swz(CTX(t));
    if (r) *r = (NVNtextureSwizzle)(w & 7); if (g) *g = (NVNtextureSwizzle)(w >> 3 & 7); if (b) *b = (NVNtextureSwizzle)(w >> 6 & 7); if (a) *a = (NVNtextureSwizzle)(w >> 9 & 7);
}
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { (void)t; return STORAGE_CLASS_HOST; }   // 同 builder
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { return zcull_size(&TB(b)->d); }   // as for the texture (zcull_size)
// Packaged texture data in CPU memory (not written into the pool by the game): NVN copies it into the texture storage at TextureInitialize (the CPU cannot write a compressible pool; the driver copies). 13 Sentinels creates all its textures this way
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->d.format; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->d.target; }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *s) { (void)t; (void)s; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { const view_t *w = (const view_t *)v; *base = w->base_level; *n = w->nlevels; return w->set_levels; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { const view_t *w = (const view_t *)v; *t = (NVNtextureTarget)w->target; return w->set_target; }
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) { VW(v)->set_dsmode = 1; VW(v)->dsmode = (uint8_t)m; }   // STENCIL: the replay binds a stencil-only view (tex_by_id)
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    view_t *w = VW(v); w->set_swizzle = 1; w->swz[0] = (uint8_t)r; w->swz[1] = (uint8_t)g; w->swz[2] = (uint8_t)bl; w->swz[3] = (uint8_t)a;
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureViewSetSwizzle not identity (%d %d %d %d): remapped with a Metal view when sampling", (int)r, (int)g, (int)bl, (int)a);
}

// Color state: alpha test function, logic op (the fields already exist in color_st)
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { ST(color_st, s)->alpha_func = (uint32_t)f; }
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) { ST(color_st, s)->logic_op = (uint32_t)op; }
// Getters and the rest of the multisample state: Pokémon Legends: Z-A (Trinity, SDK 19) reads its state objects and builders back when it builds its pipeline states; the stub returned
// whatever was left in x0 (the object's address), so a blend enable or depth test read back as true, a texture builder's width as an address
NVN(nvnColorStateGetBlendEnable, NVNboolean, const NVNcolorState *s, int i) { return i >= 0 && i < 8 && (((const color_st *)s)->blend_en >> i & 1); }
NVN(nvnColorStateGetAlphaTest, NVNalphaFunc, const NVNcolorState *s) { return (NVNalphaFunc)((const color_st *)s)->alpha_func; }
NVN(nvnColorStateGetLogicOp, NVNlogicOp, const NVNcolorState *s) { return (NVNlogicOp)((const color_st *)s)->logic_op; }
NVN(nvnChannelMaskStateGetChannelMask, void, const NVNchannelMaskState *s, int i, NVNboolean *r, NVNboolean *g, NVNboolean *b, NVNboolean *a) {
    uint32_t m = i >= 0 && i < 8 ? ((const chmask_st *)s)->mask >> (4 * i) & 0xf : 0;
    if (r) *r = (m & 1) != 0; if (g) *g = (m & 2) != 0; if (b) *b = (m & 4) != 0; if (a) *a = (m & 8) != 0;
}
NVN(nvnBlendStateGetAdvancedMode, NVNblendAdvancedMode, const NVNblendState *s) { return (NVNblendAdvancedMode)((const blend_st *)s)->adv_mode; }
NVN(nvnBlendStateGetAdvancedOverlap, NVNblendAdvancedOverlap, const NVNblendState *s) { return (NVNblendAdvancedOverlap)((const blend_st *)s)->adv_overlap; }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc, NVNboolean, const NVNblendState *s) { return (NVNboolean)(((const blend_st *)s)->adv_premul ? 1u : 0u); }
NVN(nvnBlendStateGetAdvancedNormalizedDst, NVNboolean, const NVNblendState *s) { return (NVNboolean)(((const blend_st *)s)->adv_normdst ? 1u : 0u); }
NVN(nvnBlendStateGetBlendTarget, int, const NVNblendState *s) { return (int)((const blend_st *)s)->target; }
NVN(nvnBlendStateGetBlendFunc, void, const NVNblendState *s, NVNblendFunc *sr, NVNblendFunc *dr, NVNblendFunc *sa, NVNblendFunc *da) {
    const blend_st *o = (const blend_st *)s;
    if (sr) *sr = (NVNblendFunc)o->src_rgb; if (dr) *dr = (NVNblendFunc)o->dst_rgb; if (sa) *sa = (NVNblendFunc)o->src_a; if (da) *da = (NVNblendFunc)o->dst_a;
}
NVN(nvnBlendStateGetBlendEquation, void, const NVNblendState *s, NVNblendEquation *er, NVNblendEquation *ea) {
    const blend_st *o = (const blend_st *)s; if (er) *er = (NVNblendEquation)o->eq_rgb; if (ea) *ea = (NVNblendEquation)o->eq_a;
}
NVN(nvnPolygonStateGetCullFace, NVNface, const NVNpolygonState *s) { return (NVNface)((const poly_st *)s)->cull; }
NVN(nvnPolygonStateGetFrontFace, NVNfrontFace, const NVNpolygonState *s) { return (NVNfrontFace)((const poly_st *)s)->front; }
NVN(nvnPolygonStateGetPolygonMode, NVNpolygonMode, const NVNpolygonState *s) { return (NVNpolygonMode)((const poly_st *)s)->mode; }
NVN(nvnPolygonStateGetPolygonOffsetEnables, NVNpolygonOffsetEnable, const NVNpolygonState *s) { return (NVNpolygonOffsetEnable)((const poly_st *)s)->offset_en; }
NVN(nvnDepthStencilStateGetDepthTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dt; }
NVN(nvnDepthStencilStateGetDepthWriteEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dw; }
NVN(nvnDepthStencilStateGetDepthFunc, NVNdepthFunc, const NVNdepthStencilState *s) { return (NVNdepthFunc)((const ds_st *)s)->df; }
NVN(nvnDepthStencilStateGetStencilTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->st; }
NVN(nvnDepthStencilStateGetStencilFunc, NVNstencilFunc, const NVNdepthStencilState *s, NVNface f) { const ds_st *o = (const ds_st *)s; return (NVNstencilFunc)(f == NVN_FACE_BACK ? o->sf_b : o->sf_f); }
NVN(nvnDepthStencilStateGetStencilOp, void, const NVNdepthStencilState *s, NVNface f, NVNstencilOp *fail, NVNstencilOp *zfail, NVNstencilOp *zpass) {
    const ds_st *o = (const ds_st *)s; int b = f == NVN_FACE_BACK;
    if (fail) *fail = (NVNstencilOp)(b ? o->fail_b : o->fail_f); if (zfail) *zfail = (NVNstencilOp)(b ? o->zfail_b : o->zfail_f); if (zpass) *zpass = (NVNstencilOp)(b ? o->zpass_b : o->zpass_f);
}
// Multisample state: recorded only (the replay uses the sample count, multisample enable and alpha to coverage)
NVN(nvnMultisampleStateSetAlphaToCoverageDither, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c_dither = B(e) != 0; }
NVN(nvnMultisampleStateSetRasterSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->raster_samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetCoverageModulationMode, void, NVNmultisampleState *s, NVNcoverageModulationMode m) { ST(ms_st, s)->cov_mod = (uint8_t)m; }
NVN(nvnMultisampleStateSetCoverageToColorEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->cov2color_en = B(e) != 0; }
NVN(nvnMultisampleStateSetCoverageToColorOutput, void, NVNmultisampleState *s, int i) { ST(ms_st, s)->cov2color_out = (uint8_t)i; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_grid = B(e) != 0; }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c; }
NVN(nvnMultisampleStateGetAlphaToCoverageDither, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c_dither; }
NVN(nvnMultisampleStateGetRasterSamples, int, NVNmultisampleState *s) { return ((const ms_st *)s)->raster_samples; }
NVN(nvnMultisampleStateGetCoverageModulationMode, NVNcoverageModulationMode, const NVNmultisampleState *s) { return (NVNcoverageModulationMode)((const ms_st *)s)->cov_mod; }
NVN(nvnMultisampleStateGetCoverageToColorEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_en; }
NVN(nvnMultisampleStateGetCoverageToColorOutput, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_out; }
NVN(nvnMultisampleStateGetSampleLocationsEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_en; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_grid; }
// Builder and pool queries
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return (size_t)((const buf_builder_t *)b)->size; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return (ptrdiff_t)((const buf_builder_t *)b)->off; }
NVN(nvnTextureBuilderGetTarget, NVNtextureTarget, const NVNtextureBuilder *b) { return (NVNtextureTarget)((const tex_builder_t *)b)->d.target; }
NVN(nvnTextureBuilderGetWidth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.width; }
NVN(nvnTextureBuilderGetHeight, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.height; }
NVN(nvnTextureBuilderGetDepth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.depth; }
NVN(nvnTextureBuilderGetLevels, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.levels; }
NVN(nvnTextureBuilderGetFormat, NVNformat, const NVNtextureBuilder *b) { return (NVNformat)((const tex_builder_t *)b)->d.format; }
NVN(nvnTextureBuilderGetSamples, int, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->d.samples; }
NVN(nvnTextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)((const tex_builder_t *)b)->d.stride; }
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) { const view_t *w = (const view_t *)v; if (f) *f = (NVNformat)w->format; return w->set_format; }
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *v, NVNtextureDepthStencilMode *m) { const view_t *w = (const view_t *)v; if (m) *m = (NVNtextureDepthStencilMode)w->dsmode; return w->set_dsmode; }
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const view_t *w = (const view_t *)v; NVNtextureSwizzle *o[4] = {r, g, b, a};
    for (int c = 0; c < 4; c++) if (o[c]) *o[c] = (NVNtextureSwizzle)(w->set_swizzle ? w->swz[c] : NVN_TEXTURE_SWIZZLE_R + c);
    return w->set_swizzle;
}
NVN(nvnTextureGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtexture *t) { return ((const tex_t *)t)->stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
// Queue priority: the host has one Metal queue
NVN(nvnQueueBuilderSetQueuePriority, void, NVNqueueBuilder *b, int prio) { QB(b)->priority = prio; }
// Transform feedback (vertex outputs captured into buffers): Pokémon Legends: Z-A skins its characters this way. A vertex-only program writes skinned positions and packed normals / tangents
// under rasterizer discard, and the character draws read those buffers as vertex streams. Recorded here and run at replay by the shader's main0_tf entry point, a vertex function that stores the
// captured outputs into the bound buffers (dmsh_build.py vs_tf, the layout from the program's control section; nvn_mtl.m tf_* for the replay)
NVN(nvnCommandBufferBindTransformFeedbackBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { *REC(c, OP_TF_BIND, p_tf_bind) = (p_tf_bind){i, 0, GA(a), n}; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *r) {
    for (int i = 0; i < n; i++) *REC(c, OP_TF_BIND, p_tf_bind) = (p_tf_bind){first + i, 0, GA(r[i].address), r[i].size};
}
NVN(nvnCommandBufferBeginTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){0, 0, GA(a)}; }
NVN(nvnCommandBufferEndTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){1, 0, GA(a)}; }
NVN(nvnCommandBufferPauseTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){2, 0, GA(a)}; }
NVN(nvnCommandBufferResumeTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){3, 0, GA(a)}; }
void nvn_be_bad_draw_feat(void);
NVN(nvnCommandBufferDrawTransformFeedback, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNbufferAddress a) { rec_draw(c, (p_draw){.prim = m, .count = 1, .instances = 1, .tfc = a ? GA(a) : 1}); }   // as many vertices as were captured (replay: nvn_geometry.inc tfd_args)

// Device: combined texture handle = texture id | sampler id << 20 (same encoding as the separate handles)
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp) { (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20); }
// Handle for texel fetches only (Monster Hunter Rise): a fetch reads the texture descriptor and no sampler, so the handle is the texture ID with sampler ID 0
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int tex) { (void)d; return (NVNtextureHandle)(uint64_t)(uint32_t)tex; }
// Counter timestamps are 19.2 MHz GPU ticks (nvn_mtl.m gpu_ticks: ns × 12 / 625); convert back to ns. They used to be returned as is, about 52 times smaller than GetCurrentTimestampInNanoseconds, so subtracting one from the other gave garbage
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) { (void)d; return c->timestamp * 625 / 12; }
// Current GPU time (ns): the same clock as the counter timestamps -- the guest clock (guest_ns, nvn_mtl.m gpu_ticks)
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice *d) { (void)d; return guest_ns(); }
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
// 整数版本的 fast clear 颜色：与浮点版本同一条规则——只是给 Tegra 硬件的提示，宿主的清屏值由每条 clear 命令自己带
NVN(nvnDeviceRegisterFastClearColori, NVNboolean, NVNdevice *d, const int *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
NVN(nvnDeviceRegisterFastClearColorui, NVNboolean, NVNdevice *d, const uint32_t *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }   // 无符号版同上（10-25 批量补齐）
// ---- 调试接口（只保状态与查询，不改渲染行为）----
NVN(nvnDeviceSetDebugLabel, void, NVNdevice *d, const char *label) {
    dev_t_ *o = (dev_t_ *)d;
    if (o && o->magic == MAGIC('D', 'E', 'V', 0)) o->dbg_label = label;   // 只记指针，不接管所有权
    OLOG("DeviceSetDebugLabel %s", label ? label : "(null)");
}
// 调试域号：宿主不分区（没有域隔离），给单调唯一号即可，供游戏把它们自己的调试分组对上
NVN(nvnDeviceGenerateDebugDomainId, NVNdebugDomainId, const NVNdevice *d, const char *name) {
    (void)d; static _Atomic int next; int id = ++next;
    OLOG("DeviceGenerateDebugDomainId %s = %d", name ? name : "(null)", id);
    return id;
}
// 外部调试器：本层没有 attach 通道（Metal 调试器不在这个接口下），恒报「没接」
NVN(nvnDeviceIsExternalDebuggerAttached, NVNboolean, const NVNdevice *d) { (void)d; return 0; }
// 延迟 finalize：宿主一直是立即 finalize（Finalize 当场生效），所以这里没有待办，走一次空实现
NVN(nvnDeviceApplyDeferredFinalizes, void, NVNdevice *d, int n) {
    (void)d; static int once; if (!once++) LOG("[nvn] DeviceApplyDeferredFinalizes %d: finalizes are immediate, nothing deferred", n);
}
// 调试数据库：本层不维护对象注册表，遍历时一次都不回调（空库），不假装有对象
NVN(nvnDeviceWalkDebugDatabase, void, const NVNdevice *d, NVNdebugObjectType type, void (*cb)(void *, void *), void *ud) {
    (void)d; (void)type; (void)cb; (void)ud; static int once;
    if (!once++) LOG("[nvn] DeviceWalkDebugDatabase: the host keeps no debug object database, no entries are reported");
}
// 自动检查点 / 调用栈等捕获开关：只影响调试器抓现场，不影响渲染与同步，按空实现登记（各自点名一次）
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsEnable, void, NVNdevice *d, NVNboolean en) { static int once; if (!once++) LOG("[nvn] nvnDeviceSetAutomaticCheckpointsEnable: debug-only setting, ignored"); }
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsSamplingInterval, void, NVNdevice *d, int ns) { static int once; if (!once++) LOG("[nvn] nvnDeviceSetAutomaticCheckpointsSamplingInterval: debug-only setting, ignored"); }
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsByDebugGroup, void, NVNdevice *d, NVNboolean en) { static int once; if (!once++) LOG("[nvn] nvnDeviceSetAutomaticCheckpointsByDebugGroup: debug-only setting, ignored"); }
NVN_NOPROTO(nvnDeviceSetCheckpointCallStacksEnable, void, NVNdevice *d, NVNboolean en) { static int once; if (!once++) LOG("[nvn] nvnDeviceSetCheckpointCallStacksEnable: debug-only setting, ignored"); }
NVN_NOPROTO(nvnDeviceSetCheckpointMaxCallStacksDepth, void, NVNdevice *d, int n) { static int once; if (!once++) LOG("[nvn] nvnDeviceSetCheckpointMaxCallStacksDepth: debug-only setting, ignored"); }
NVN_NOPROTO(nvnDeviceSetAutomaticCheckpointsMask, void, NVNdevice *d, uint64_t mask) { static int once; if (!once++) LOG("[nvn] nvnDeviceSetAutomaticCheckpointsMask: debug-only setting, ignored"); }
// Same as the color one: a fast clear value is only a hint to the Tegra hardware (Tears of the Kingdom registers its depth clear at startup)
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float v) { (void)d; (void)v; return 1; }
// The size of the driver's cache for shaders it compiles from intermediate code (The Great Ace Attorney Chronicles sets 10 at startup); the host compiles nothing at run time
NVN(nvnDeviceSetIntermediateShaderCache, void, NVNdevice *d, int n) { (void)d; (void)n; }

// Window: number of active swapchain textures. WindowAcquireTexture rotates through the first nactive (NVN semantics; 审计 #1534 P1-24, see there); clamped to the SetTextures count
NVN(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *b, int n) { WB(b)->nactive = n; ONCE_LOG("WindowBuilderSetNumActiveTextures %d", n); }
NVN_NOPROTO(nvnWindowSetNumActiveTextures, void, NVNwindow *w, int n) {
    win_t *o = (win_t *)w;
    __atomic_store_n(&o->nactive, n > 0 ? (n < o->ntex ? n : o->ntex) : 0, __ATOMIC_RELAXED);   // 夹到 SetTextures 的个数：多出来的纹理没有交换链映像
    ONCE_LOG("WindowSetNumActiveTextures %d of %d (acquire rotates through the active ones)", n, o->ntex);
}
// 窗口纹理数 / 当前活跃纹理数（NVN 语义：前者是 SetTextures 给的个数，后者是 SetNumActiveTextures 生效后被轮换的个数，未设时为前者）
NVN_NOPROTO(nvnWindowGetNumTextures, int, const NVNwindow *w) { return ((const win_t *)w)->ntex; }
NVN_NOPROTO(nvnWindowGetNumActiveTextures, int, const NVNwindow *w) { return ((const win_t *)w)->nactive; }

// Command buffer memory usage, in device bytes (dev_bytes; records themselves are in host blocks and take none of the game's memory); the host does not use control memory, always 0;
// the memory callback switch is ignored. SMBW asks for this value after recording each material setup handle, and on 0 (with control memory also 0) treats the recording as
// empty and marks the handle invalid (from main+0x4bb6c it clears bit 0 of the material entry +0x50): every command counts at least 8 bytes.
// As in NVN, the count runs from the start of the command memory the game last added (AddCommandMemory), not from BeginRecording: Fire Emblem: Three Houses adds one 16 MB chunk of a ring to its main
// command buffer and records every frame into it; its memory callback (main+0x9ca960) takes chunk start + used as the position the GPU has reached and frees the ring up to there when the frame's fence
// signals. Counted per recording, the position stayed at one frame's size (about 18 KB), the ring never freed past it, and after the first 16 MB (about 15 s) the callback waited for ring space forever
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { return (size_t)__atomic_load_n(&((const cmd_t *)c)->used, __ATOMIC_RELAXED); }
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { (void)c; return 0; }
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return (size_t)((const cmd_t *)c)->ctl_size; }
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { return (size_t)((const cmd_t *)c)->ctl_size; }   // 已用恒 0
// Size and free bytes of the command memory the game last added (DRAGON QUEST XI S, Unreal Engine 4.18): as in NVN, size = used + free. Size is the length the game last passed to
//   AddCommandMemory (cmd_seg_t, stable through recording); free = size - used from one snapshot of used (device bytes, = the device cursor's room in the game's memory),
//   0 in a host-lent window past the game's memory (used = size there, cmd_lend)
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return (size_t)__atomic_load_n(CB_SEG(c), __ATOMIC_RELAXED); }
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) {
    size_t sz = h_nvnCommandBufferGetCommandMemorySize(c), u = h_nvnCommandBufferGetCommandMemoryUsed(c); return sz > u ? sz - u : 0; }
NVN_NOPROTO(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }

// nvnProgramSetShadersExt (NVN 55): each stage is 0x18 bytes = NVNshaderData{data, control} + one 8-byte extension field (filled from SMBW main 0x1074ff0).
// The host identifies stages by the control section and does not use the extension field: convert to plain NVNshaderData and hand it to SetShaders.
typedef struct { NVNbufferAddress data; const void *control; uint64_t ext; } shader_data_ext;
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const shader_data_ext *sd) {
    if (n < 0 || n > 6) { LOG("【红】 ProgramSetShadersExt stage count %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) { plain[i].data = sd[i].data; plain[i].control = sd[i].control; if (sd[i].ext) ONCE_LOG("ProgramSetShadersExt extension field nonzero (%#llx): unused by the host", (unsigned long long)sd[i].ext); }
    return h_nvnProgramSetShaders(p, n, plain);
}

// NVN2 用构建器建程序：SetDefaults → SetDevice → SetShaders(数量, NVNshaderData[]) → nvnProgramInitializeFromBuilder（金刚 Donkey Kong Bananza 11-04：
//   开局建一个两阶段图形程序，没接时程序没建成，用它的绘制全是空操作）。阶段同 NVN 的 16 字节 {data, control}，所以从构建器建程序 =
//   ProgramInitialize + ProgramSetShaders。构建器只记调用方的数组指针（游戏在建程序前一直留着它的栈帧），同 NVN 其他构建器。
//   做法取自 参考实现（MIT）dm/gfx/nvn/hle_nvn.c（777a5f6c），代码自写。
//   11-04 入口全量：构建器改按驱动布局存（nvn2_cmd.h PB_*，SetDefaults 0x660d40 不动设备、两个递归深度缺省 2），其余 Set / Get 照驱动的单条 str / ldr
NVN_NOPROTO(nvnProgramBuilderSetDefaults, void, void *b) { nvn2_prog_builder_defaults(b); }
NVN_NOPROTO(nvnProgramBuilderSetDevice, void, void *b, NVNdevice *d) { nvn2_putp(b, PB_DEVICE, d); }
NVN_NOPROTO(nvnProgramBuilderGetDevice, NVNdevice *, const void *b) { return (NVNdevice *)nvn2_getp(b, PB_DEVICE); }
NVN_NOPROTO(nvnProgramBuilderSetShaders, void, void *b, int n, const NVNshaderData *sd) { nvn2_put32(b, PB_NSHADER, (uint32_t)n); nvn2_putp(b, PB_SHADERS, sd); }
NVN_NOPROTO(nvnProgramBuilderGetShaderCount, int, const void *b) { return (int)nvn2_get32(b, PB_NSHADER); }
NVN_NOPROTO(nvnProgramBuilderGetShaders, const NVNshaderData *, const void *b) { return (const NVNshaderData *)nvn2_getp(b, PB_SHADERS); }
NVN_NOPROTO(nvnProgramBuilderSetSampleShading, void, void *b, int v) { nvn2_put32(b, PB_SAMPLE_SHADING, (uint32_t)v); }
NVN_NOPROTO(nvnProgramBuilderGetSampleShading, int, const void *b) { return (int)nvn2_get32(b, PB_SAMPLE_SHADING); }
NVN_NOPROTO(nvnProgramBuilderSetDebugDataHashes, void, void *b, int n, const void *h) { nvn2_put32(b, PB_DBG_N, (uint32_t)n); nvn2_putp(b, PB_DBG, h); }
NVN_NOPROTO(nvnProgramBuilderGetDebugDataHashes, const void *, const void *b) { return nvn2_getp(b, PB_DBG); }   // 驱动只回指针（0x+20），个数没有 Get
NVN_NOPROTO(nvnProgramBuilderSetRayTracingModules, void, void *b, int n, const void *m) { nvn2_put32(b, PB_NRT, (uint32_t)n); nvn2_putp(b, PB_RT, m); }
NVN_NOPROTO(nvnProgramBuilderGetRayTracingModuleCount, int, const void *b) { return (int)nvn2_get32(b, PB_NRT); }
NVN_NOPROTO(nvnProgramBuilderGetRayTracingModuleData, const void *, const void *b) { return nvn2_getp(b, PB_RT); }
NVN_NOPROTO(nvnProgramBuilderSetSubroutineLinkage, void, void *b, int n, const void *m) { nvn2_put32(b, PB_NSUB, (uint32_t)n); nvn2_putp(b, PB_SUB, m); }
NVN_NOPROTO(nvnProgramBuilderGetSubroutineLinkageCount, int, const void *b) { return (int)nvn2_get32(b, PB_NSUB); }
NVN_NOPROTO(nvnProgramBuilderGetSubroutineLinkage, const void *, const void *b) { return nvn2_getp(b, PB_SUB); }
NVN_NOPROTO(nvnProgramBuilderSetMaxTraceRecursionDepth, void, void *b, int v) { nvn2_put32(b, PB_MAX_TRACE, (uint32_t)v); }
NVN_NOPROTO(nvnProgramBuilderGetMaxTraceRecursionDepth, int, const void *b) { return (int)nvn2_get32(b, PB_MAX_TRACE); }
NVN_NOPROTO(nvnProgramBuilderSetMaxCallableRecursionDepth, void, void *b, int v) { nvn2_put32(b, PB_MAX_CALLABLE, (uint32_t)v); }
NVN_NOPROTO(nvnProgramBuilderGetMaxCallableRecursionDepth, int, const void *b) { return (int)nvn2_get32(b, PB_MAX_CALLABLE); }
NVN_NOPROTO(nvnProgramBuilderSetProgramRayTracingFlags, void, void *b, int f) { nvn2_put32(b, PB_RT_FLAGS, (uint32_t)f); }
NVN_NOPROTO(nvnProgramBuilderGetProgramRayTracingFlags, int, const void *b) { return (int)nvn2_get32(b, PB_RT_FLAGS); }
// 建程序：光追模块（光追程序）宿主做不了 → 回失败（不假装建成）；子程序链接走 ProgramSetSubroutineLinkage（宿主报红、整段翻译）；
//   逐样本着色宿主不强制（Metal 只在着色器读样本号时逐样本跑）→ 首次报红，程序照建（按像素着色，画面少了逐样本的细节）
NVN_NOPROTO(nvnProgramInitializeFromBuilder, NVNboolean, NVNprogram *p, const void *b) {
    int n = (int)nvn2_get32(b, PB_NSHADER), nrt = (int)nvn2_get32(b, PB_NRT), nsub = (int)nvn2_get32(b, PB_NSUB);
    if (n < 0 || n > 6) { LOG("【红】 ProgramInitializeFromBuilder stage count %d", n); return 0; }
    if (nrt) { static int once; if (!once++) LOG("【红】 ProgramInitializeFromBuilder：%d 个光追模块（光追程序），宿主没有光追管线，回失败", nrt); return 0; }
    if (nvn2_get32(b, PB_SAMPLE_SHADING)) { static int once; if (!once++) LOG("【红】 ProgramInitializeFromBuilder：逐样本着色 %u 宿主不强制，按像素着色", nvn2_get32(b, PB_SAMPLE_SHADING)); }
    if (!h_nvnProgramInitialize(p, (NVNdevice *)nvn2_getp(b, PB_DEVICE)) || (n && !h_nvnProgramSetShaders(p, n, (const NVNshaderData *)nvn2_getp(b, PB_SHADERS)))) return 0;
    if (nsub) h_nvnProgramSetSubroutineLinkage(p, nsub, (const NVNsubroutineLinkageMapPtr *)nvn2_getp(b, PB_SUB));
    return 1;
}

// ---------- NVN2 入口全量（11-04）：游戏自带 NVN2 驱动按名可取的 767 个入口（graphics/nvn/gfx/nvn/nvn2_drv_names.txt）里此前没实现的 142 个 ----------
// 每个入口的语义取自驱动本身（地址 = 火纹 SDK 22 sdk 模块，graphics/nvn/tools/nvn2_procs.py 出表；对象布局在 nvn2_cmd.h）。按类处理：
//   ① 构建器 / 对象的 Set / Get：照驱动布局存取（驱动多是一条 str / ldr）；
//   ② 录制类命令：能接现有记录的接（延后推常量 → OP_PUSH_CONST_PTR），纯硬件提示做空操作（g_nops，写明依据），宿主做不到的状态在「改成非缺省」时首次报红；
//   ③ 宿主没有对应物的整类功能（网格任务、光追、设备生成命令、协作向量、脏块图计算、原始描述符写入…）保持未实现桩：调用即报红并标直连异常，原因见 nvn2_red_why

// 脏块图参数（纯数据对象，照驱动布局，Set / Get 0x64eXXX 一族）。只有 ComputeDirtyTileMap 用它，那条命令宿主做不了（见 nvn2_red_why），参数照样能设能读
NVN_NOPROTO(nvnDirtyTileMapParametersSetDefaults, void, void *p) { nvn2_dtm_defaults(p); }
NVN_NOPROTO(nvnDirtyTileMapParametersSetTexture, void, void *p, const NVNtexture *t, const NVNtextureView *v, const void *region) {
    nvn2_putp(p, DTM_TEX, t); nvn2_putp(p, DTM_VIEW, v); memcpy((uint8_t *)p + DTM_REGION, region, 24); }
NVN_NOPROTO(nvnDirtyTileMapParametersGetTexture, void, const void *p, const NVNtexture **t, const NVNtextureView **v, void *region) {
    if (t) *t = (const NVNtexture *)nvn2_getp(p, DTM_TEX); if (v) *v = (const NVNtextureView *)nvn2_getp(p, DTM_VIEW); if (region) memcpy(region, (const uint8_t *)p + DTM_REGION, 24); }
NVN_NOPROTO(nvnDirtyTileMapParametersSetInputs, void, void *p, int a, int b, int c, int d) {
    nvn2_put32(p, DTM_IN_A, (uint32_t)a); nvn2_put32(p, DTM_IN_B, (uint32_t)b); nvn2_put32(p, DTM_IN_C, (uint32_t)c); nvn2_put32(p, DTM_IN_D, (uint32_t)d); }
NVN_NOPROTO(nvnDirtyTileMapParametersGetInputs, void, const void *p, int *a, int *b, int *c, int *d) {
    if (a) *a = (int)nvn2_get32(p, DTM_IN_A); if (b) *b = (int)nvn2_get32(p, DTM_IN_B); if (c) *c = (int)nvn2_get32(p, DTM_IN_C); if (d) *d = (int)nvn2_get32(p, DTM_IN_D); }
NVN_NOPROTO(nvnDirtyTileMapParametersSetOutputs, void, void *p, uint64_t a, int b, int c, uint64_t d) {
    memcpy((uint8_t *)p + DTM_OUT0, &a, 8); nvn2_put32(p, DTM_OUT1, (uint32_t)b); nvn2_put32(p, DTM_OUT2, (uint32_t)c); memcpy((uint8_t *)p + DTM_OUT3, &d, 8); }
NVN_NOPROTO(nvnDirtyTileMapParametersGetOutputs, void, const void *p, uint64_t *a, int *b, int *c, uint64_t *d) {
    if (a) memcpy(a, (const uint8_t *)p + DTM_OUT0, 8); if (b) *b = (int)nvn2_get32(p, DTM_OUT1); if (c) *c = (int)nvn2_get32(p, DTM_OUT2); if (d) memcpy(d, (const uint8_t *)p + DTM_OUT3, 8); }
NVN_NOPROTO(nvnDirtyTileMapParametersSetConstantColorTable, void, void *p, const void *t, int n, int c, uint64_t d) {
    nvn2_putp(p, DTM_CC, t); nvn2_put32(p, DTM_CC_N, (uint32_t)n); nvn2_put32(p, DTM_CC_C, (uint32_t)c); memcpy((uint8_t *)p + DTM_CC_D, &d, 8); }
NVN_NOPROTO(nvnDirtyTileMapParametersGetConstantColorTable, void, const void *p, const void **t, int *n, int *c, uint64_t *d) {
    if (t) *t = nvn2_getp(p, DTM_CC); if (n) *n = (int)nvn2_get32(p, DTM_CC_N); if (c) *c = (int)nvn2_get32(p, DTM_CC_C); if (d) memcpy(d, (const uint8_t *)p + DTM_CC_D, 8); }
NVN_NOPROTO(nvnDirtyTileMapParametersSetDepthStencilMask, void, void *p, uint32_t dm, uint32_t sm) { nvn2_put32(p, DTM_DSMASK, dm); nvn2_put32(p, DTM_DSMASK + 4, sm); }
NVN_NOPROTO(nvnDirtyTileMapParametersGetDepthStencilMask, void, const void *p, uint32_t *dm, uint32_t *sm) { if (dm) *dm = nvn2_get32(p, DTM_DSMASK); if (sm) *sm = nvn2_get32(p, DTM_DSMASK + 4); }
// 扩展足迹：四个布尔，驱动按字节存（strb）；调用方不扩展窄参数，只取低 8 位（同 B()）
NVN_NOPROTO(nvnDirtyTileMapParametersSetExpandFootprint, void, void *p, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint8_t *f = (uint8_t *)p + DTM_FOOT; f[0] = (uint8_t)B(a); f[1] = (uint8_t)B(b); f[2] = (uint8_t)B(c); f[3] = (uint8_t)B(d); }
NVN_NOPROTO(nvnDirtyTileMapParametersGetExpandFootprint, void, const void *p, uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d) {
    const uint8_t *f = (const uint8_t *)p + DTM_FOOT; if (a) *a = f[0]; if (b) *b = f[1]; if (c) *c = f[2]; if (d) *d = f[3]; }

// 窗口构建器 / 窗口的 NVN2 字段（nvn2_cmd.h win_builder2 / win2）。获取超时：宿主取图不会超时（交换链图按上屏节奏轮转、总能取到），只记下读回；
//   色彩空间：宿主上屏是 SDR（sRGB），非 0（缺省以外）在 WindowInitialize 首次报红；HDR 元数据是给显示器做色调映射的说明，宿主按 SDR 上屏不用它，只记下读回
#define WB2(b) ((win_builder2 *)(b))
#define W2(w) ((win2 *)(w))
static void nvn2_win_builder_defaults(NVNwindowBuilder *b) {
    WB2(b)->acquire_timeout = -1; WB2(b)->color_space = 0;
    memcpy(WB2(b)->hdr_display, nvn2_hdr_display_def, sizeof nvn2_hdr_display_def); memcpy(WB2(b)->hdr_content, nvn2_hdr_content_def, sizeof nvn2_hdr_content_def);
}
static void nvn2_win_init(NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder2 *s = (const win_builder2 *)b;
    W2(w)->acquire_timeout = s->acquire_timeout; W2(w)->color_space = s->color_space;
    memcpy(W2(w)->hdr_display, s->hdr_display, sizeof s->hdr_display); memcpy(W2(w)->hdr_content, s->hdr_content, sizeof s->hdr_content);
    if (s->color_space) {   // PR1 探针：红日志扩为结构化一行（M0 判读字段：color_space + hdr_display[10] + hdr_content[2]），触发条件与节流不变
        static int once; if (!once++)
            LOG("【红】 WindowInitialize：色彩空间 %d（非缺省），宿主按 SDR（sRGB）上屏，宽色域 / HDR 的颜色会偏；HDR 元数据 display=[%g %g %g %g %g %g %g %g %g %g] content=[%g %g]",
                s->color_space, s->hdr_display[0], s->hdr_display[1], s->hdr_display[2], s->hdr_display[3], s->hdr_display[4], s->hdr_display[5], s->hdr_display[6], s->hdr_display[7], s->hdr_display[8], s->hdr_display[9], s->hdr_content[0], s->hdr_content[1]);
    }
}
NVN_NOPROTO(nvnWindowBuilderSetAcquireTimeout, void, NVNwindowBuilder *b, int64_t ns) { WB2(b)->acquire_timeout = ns; }
NVN_NOPROTO(nvnWindowBuilderGetAcquireTimeout, int64_t, const NVNwindowBuilder *b) { return ((const win_builder2 *)b)->acquire_timeout; }
NVN_NOPROTO(nvnWindowBuilderSetColorSpace, void, NVNwindowBuilder *b, int cs) { WB2(b)->color_space = cs; }
NVN_NOPROTO(nvnWindowBuilderGetColorSpace, int, const NVNwindowBuilder *b) { return ((const win_builder2 *)b)->color_space; }
NVN_NOPROTO(nvnWindowBuilderSetHdrContentMetadata, void, NVNwindowBuilder *b, float max_cll, float max_fall) { WB2(b)->hdr_content[0] = max_cll; WB2(b)->hdr_content[1] = max_fall; }
NVN_NOPROTO(nvnWindowBuilderGetHdrContentMetadata, void, const NVNwindowBuilder *b, float *max_cll, float *max_fall) {
    if (max_cll) *max_cll = ((const win_builder2 *)b)->hdr_content[0]; if (max_fall) *max_fall = ((const win_builder2 *)b)->hdr_content[1]; }
NVN_NOPROTO(nvnWindowGetAcquireTimeout, int64_t, const NVNwindow *w) { return ((const win2 *)w)->acquire_timeout; }
NVN_NOPROTO(nvnWindowSetAcquireTimeout, void, NVNwindow *w, int64_t ns) { W2(w)->acquire_timeout = ns; }
NVN_NOPROTO(nvnWindowSetHdrContentMetadata, void, NVNwindow *w, float max_cll, float max_fall) { W2(w)->hdr_content[0] = max_cll; W2(w)->hdr_content[1] = max_fall; }
NVN_NOPROTO(nvnWindowGetHdrContentMetadata, void, const NVNwindow *w, float *max_cll, float *max_fall) {
    if (max_cll) *max_cll = ((const win2 *)w)->hdr_content[0]; if (max_fall) *max_fall = ((const win2 *)w)->hdr_content[1]; }
// 显示元数据：十个 float（三原色 xy、白点 xy、最暗、最亮）。Switch 侧按 AAPCS64：前 8 个在 s0..s7，第 9、10 个各占调用方栈上一个 8 字节槽（驱动读 [sp] / [sp, #8]）；
//   Apple 的栈参数按自然对齐紧排，所以第 9 个声明成 double（占 [sp] 的 8 字节，取低 4 字节即那个 float），第 10 个 float 就落在 [sp, #8]。
//   出参版十个指针：x1..x7 + 栈上 [sp] / [sp, #8] / [sp, #16]，指针本来就是 8 字节，两边一致。这四个不能换栈（nvn_stack_args）
static inline float stack_f(double d) { float f; memcpy(&f, &d, 4); return f; }
static void hdr_put(float *o, float a, float b, float c, float d, float e, float f, float g, float h, double i, float j) {
    o[0] = a; o[1] = b; o[2] = c; o[3] = d; o[4] = e; o[5] = f; o[6] = g; o[7] = h; o[8] = stack_f(i); o[9] = j; }
static void hdr_get(const float *s, float *a, float *b, float *c, float *d, float *e, float *f, float *g, float *h, float *i, float *j) {
    float *o[10] = {a, b, c, d, e, f, g, h, i, j}; for (int k = 0; k < 10; k++) if (o[k]) *o[k] = s[k]; }
NVN_NOPROTO(nvnWindowBuilderSetHdrDisplayMetadata, void, NVNwindowBuilder *w, float a, float b, float c, float d, float e, float f, float g, float h, double i, float j) {
    hdr_put(WB2(w)->hdr_display, a, b, c, d, e, f, g, h, i, j); }
NVN_NOPROTO(nvnWindowBuilderGetHdrDisplayMetadata, void, const NVNwindowBuilder *w, float *a, float *b, float *c, float *d, float *e, float *f, float *g, float *h, float *i, float *j) {
    hdr_get(((const win_builder2 *)w)->hdr_display, a, b, c, d, e, f, g, h, i, j); }
NVN_NOPROTO(nvnWindowSetHdrDisplayMetadata, void, NVNwindow *w, float a, float b, float c, float d, float e, float f, float g, float h, double i, float j) {
    hdr_put(W2(w)->hdr_display, a, b, c, d, e, f, g, h, i, j); }
NVN_NOPROTO(nvnWindowGetHdrDisplayMetadata, void, const NVNwindow *w, float *a, float *b, float *c, float *d, float *e, float *f, float *g, float *h, float *i, float *j) {
    hdr_get(((const win2 *)w)->hdr_display, a, b, c, d, e, f, g, h, i, j); }

// 多重采样状态：覆盖截断（驱动字 0 第 5 位，0x667e40 一族）。宿主回放不做覆盖截断：打开时首次报红（按不截断画）
NVN_NOPROTO(nvnMultisampleStateSetCoverageTruncationEnable, void, NVNmultisampleState *s, uint32_t e) {
    ((ms2 *)s)->cov_trunc = B(e) != 0;
    if (B(e)) { static int once; if (!once++) LOG("【红】 MultisampleStateSetCoverageTruncationEnable：宿主不做覆盖截断，按不截断画"); } }
NVN_NOPROTO(nvnMultisampleStateGetCoverageTruncationEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms2 *)s)->cov_trunc; }
// 采样位置网格（0x667dd0）：n = max(采样数, 光栅采样数)，网格宽 × 高 × n = 16：n ≤ 1 → 1×1、2 → 2×4、4 → 2×2、8 → 1×2、16 → 1×1；其他 n 驱动不写出参
NVN(nvnMultisampleStateGetSampleLocationsGrid, void, NVNmultisampleState *s, int *w, int *h) {
    const ms_st *o = (const ms_st *)s; int n = o->samples > o->raster_samples ? o->samples : o->raster_samples, gw, gh;
    switch (n) { case 0: case 1: case 16: gw = 1; gh = 1; break; case 2: gw = 2; gh = 4; break; case 4: gw = 2; gh = 2; break; case 8: gw = 1; gh = 2; break; default: return; }
    if (w) *w = gw; if (h) *h = gh;
}

// 队列构建器：主队列（驱动 +0x18）、光追内存大小（+0x38，+0x27 记「设过」，Get 回这个标志）。主队列此前是空操作，Get 要读回就改成记下；
//   宿主所有队列的提交按序进同一个 Metal 队列，主次关系不改执行
#define QB2(b) ((queue_builder2 *)(b))
NVN_NOPROTO(nvnQueueBuilderSetPrimaryQueue, void, NVNqueueBuilder *b, NVNqueue *q) { QB2(b)->primary = q; }
NVN_NOPROTO(nvnQueueBuilderGetPrimaryQueue, NVNqueue *, const NVNqueueBuilder *b) { return ((const queue_builder2 *)b)->primary; }
NVN_NOPROTO(nvnQueueBuilderSetRayTracingMemorySize, void, NVNqueueBuilder *b, int n) { QB2(b)->rt_mem = n; QB2(b)->rt_set = 1; }
NVN_NOPROTO(nvnQueueBuilderGetRayTracingMemorySize, NVNboolean, const NVNqueueBuilder *b, int *n) { const queue_builder2 *o = (const queue_builder2 *)b; if (n) *n = o->rt_mem; return o->rt_set; }
// 队列已用的光追内存（驱动读队列 +0x2838）：宿主不跑光追，没有用掉的光追内存，回 0 如实
NVN_NOPROTO(nvnQueueGetTotalRayTracingMemoryUsed, uint64_t, const NVNqueue *q) { (void)q; return 0; }

// 设备命令缓冲（GPU 自己生成命令用的存储描述）：对象照驱动布局（nvn2_cmd.h DCB_*）。句柄 = 存储池的 GPU 地址 + 池内偏移（0x6568b0）。
//   用它的 Begin / End / SubmitDeviceCommands 宿主做不了（见 nvn2_red_why），对象本身能建能读
NVN_NOPROTO(nvnDeviceCommandBufferSetDefaults, void, void *b) { memset((uint8_t *)b + DCB_POOL, 0, DCB_SIZE - DCB_POOL); }
NVN_NOPROTO(nvnDeviceCommandBufferSetDevice, void, void *b, NVNdevice *d) { nvn2_putp(b, DCB_DEVICE, d); }
NVN_NOPROTO(nvnDeviceCommandBufferSetStorage, void, void *b, const NVNmemoryPool *p, int64_t off, uint64_t size) {
    nvn2_putp(b, DCB_POOL, p); memcpy((uint8_t *)b + DCB_OFF, &off, 8); memcpy((uint8_t *)b + DCB_SIZE_, &size, 8); }
NVN_NOPROTO(nvnDeviceCommandBufferSetCommandMemorySize, void, void *b, uint64_t n) { memcpy((uint8_t *)b + DCB_CMD, &n, 8); }
NVN_NOPROTO(nvnDeviceCommandBufferSetComputeMemorySize, void, void *b, uint64_t n) { memcpy((uint8_t *)b + DCB_COMP, &n, 8); }
NVN_NOPROTO(nvnDeviceCommandBufferGetHandle, uint64_t, const void *b) {
    const pool_t *p = (const pool_t *)nvn2_getp(b, DCB_POOL); int64_t off; memcpy(&off, (const uint8_t *)b + DCB_OFF, 8);
    return p ? pool_ga(p) + (uint64_t)off : 0; }
// 设备命令句柄的内存大小（0x6570b0）：句柄低 2 位 = 2 的才是设备命令句柄，大小在第 40 位起（按 4 字节对齐）；宿主的命令句柄是记录段地址（低位 0）→ 回 0，同驱动对普通句柄
NVN_NOPROTO(nvnDeviceGetCommandHandleMemorySize, uint64_t, const NVNdevice *d, uint64_t h) { (void)d; return (h & 3) == 2 ? (h >> 38) & 0x3fffffc : 0; }
// 统一缓冲区句柄（无绑定 UBO：着色器从内存读到它当地址用）：照驱动公式（0x673630：高位 0x1e0000000000 + 参数 2 << 41，取第 45 位以上，低位 = 地址 / 16）。
//   怎么解这个句柄是着色器翻译的事（不归本层）
NVN_NOPROTO(nvnDeviceGetUniformBufferHandle, uint64_t, const NVNdevice *d, uint64_t addr, uint64_t n) {
    (void)d; return ((0x1e0000000000ull + (n << 41)) & 0xffffe00000000000ull) | (addr >> 4); }
NVN_NOPROTO(nvnDeviceGetFlags, int, const NVNdevice *d) { return (int)((const dev_t_ *)d)->flags; }   // 驱动 ldr w0, [x0, #8]：设备构建器给的标志

// 设备消息缓冲（着色器往 CPU 发调试消息用）：对象照驱动布局（nvn2_cmd.h DMB_*）。存储头 {12, 容量, 已写到}，Reset 写 {12, 容量, 0}；Begin 把游标放在 12、
//   终点 = 已写到（0 时取头的第一个字 = 12，即空）；GetNextMessage 游标到终点就回 0。宿主翻译出的着色器不发消息（Ampere 的消息指令不翻），头只有 Reset 写，
//   所以读出来恒为空——与真机「着色器没发消息」一致。终点越过 12（有人往里写了消息）时宿主不解析，报红回 0
static uint8_t *dmb_mem(const void *b) {
    const pool_t *p = (const pool_t *)nvn2_getp(b, DMB_POOL); return p && p->mem ? p->mem + (int32_t)nvn2_get32(b, DMB_OFF) : NULL; }
static void dmb_reset(const void *b) { uint8_t *m = dmb_mem(b); if (!m) return; uint32_t h[3] = {DMB_HDR, nvn2_get32(b, DMB_CAP), 0}; memcpy(m, h, 12); }
NVN_NOPROTO(nvnDeviceMessageBufferSetDefaults, void, void *b) { memset((uint8_t *)b + DMB_POOL, 0, DMB_SIZE - DMB_POOL); }
NVN_NOPROTO(nvnDeviceMessageBufferSetDevice, void, void *b, NVNdevice *d) { nvn2_putp(b, DMB_DEVICE, d); }
NVN_NOPROTO(nvnDeviceMessageBufferSetStorage, void, void *b, const NVNmemoryPool *p, int off, int size) { nvn2_putp(b, DMB_POOL, p); nvn2_put32(b, DMB_OFF, (uint32_t)off); nvn2_put32(b, DMB_CAP, (uint32_t)size); }
NVN_NOPROTO(nvnDeviceMessageBufferReset, void, void *b) { dmb_reset(b); }
NVN_NOPROTO(nvnDeviceMessageBufferBeginMessages, void, void *b) {
    uint8_t *m = dmb_mem(b); nvn2_putp(b, DMB_BASE, m); nvn2_put32(b, DMB_CUR, DMB_HDR);
    uint32_t h[3] = {DMB_HDR, 0, 0}; if (m) memcpy(h, m, 12);
    nvn2_put32(b, DMB_END, h[2] ? h[2] : h[0]);
}
NVN_NOPROTO(nvnDeviceMessageBufferEndMessages, void, void *b) { memset((uint8_t *)b + DMB_BASE, 0, 16); }
NVN_NOPROTO(nvnDeviceMessageBufferGetNextMessage, NVNboolean, void *b, void *out, void *data, uint64_t size) {
    (void)out; (void)data; (void)size;
    if (nvn2_get32(b, DMB_CUR) >= nvn2_get32(b, DMB_END)) return 0;
    static int once; if (!once++) LOG("【红】 DeviceMessageBufferGetNextMessage：消息缓冲里有 %u 字节宿主没写过的消息，宿主不解析，回「没有」", nvn2_get32(b, DMB_END) - nvn2_get32(b, DMB_CUR));
    return 0;
}
// 命令版重置（立即版 / 延后版同义）：真机由 GPU 按序写头。宿主在录制时由 CPU 写同样的 {12, 容量, 0}：宿主着色器从不写消息，头的内容在任何时刻都只可能是这个值，
//   早写晚写游戏读到的一样
NVN_NOPROTO(nvnCommandBufferResetDeviceMessageBuffer, void, NVNcommandBuffer *c, const void *b) { (void)c; dmb_reset(b); }
NVN_NOPROTO(nvnCommandBufferResetDeviceMessageBufferDeferred, void, NVNcommandBuffer *c, const void *b) { (void)c; dmb_reset(b); }

// DLSS：构建器读回纹理号（0x38 起 32 个 int，同 SetTextureIDs）；网络改尺寸（金刚 SDK 20.5 0x5dcc30：新宽高都不超过初始化时的上限才改输出分辨率并重算，回 1；否则回 0）。
//   宿主上限 = Initialize 时的输出分辨率（DLN_MAX_W / H）；回放按每条求值记录里的输出尺寸建放大器，改了尺寸下一条求值就按新尺寸放
NVN_NOPROTO(nvnDlssNetworkBuilderGetTextureIDs, void, const void *b, int *ids) { if (ids) memcpy(ids, (const uint8_t *)b + DLB_IDS, 32 * 4); }
NVN_NOPROTO(nvnDlssNetworkResize, NVNboolean, void *n, int w, int h) {
    if (w > (int)nvn2_get32(n, DLN_MAX_W) || h > (int)nvn2_get32(n, DLN_MAX_H)) return 0;
    nvn2_put32(n, DLN_OUT_W, (uint32_t)w); nvn2_put32(n, DLN_OUT_H, (uint32_t)h);
    LOG("[nvn] DLSS 网络 %p 改输出分辨率 %dx%d（上限 %ux%u）", n, w, h, nvn2_get32(n, DLN_MAX_W), nvn2_get32(n, DLN_MAX_H));
    return 1;
}

// 快速清除值登记（按序号的版本）：真机把清除值登记进设备的快速清除表、回表里的序号（格式不支持快速清除回 -1，0x691060）；不带序号的版本回「成功」。
//   宿主的清除没有快速 / 慢速之分，登记不改结果（同 NVN1 那几个回 1）：本层记一张 16 项的表，同值同格式回同一序号，满了回 -1（同真机表满）
typedef struct { uint32_t kind, fmt, v[4]; } fc_ent;
static int fast_clear_index(uint32_t kind, uint32_t fmt, const void *v, size_t n) {
    static fc_ent t[16]; static int nt; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    fc_ent e = {kind, fmt, {0}}; if (v) memcpy(e.v, v, n);
    pthread_mutex_lock(&mu); int r = -1;
    for (int i = 0; i < nt && r < 0; i++) if (!memcmp(&t[i], &e, sizeof e)) r = i;
    if (r < 0 && nt < 16) { t[nt] = e; r = nt++; }
    pthread_mutex_unlock(&mu); return r;
}
NVN_NOPROTO(nvnDeviceRegisterFastClearColorIndex, int, NVNdevice *d, const float *c, NVNformat f) { (void)d; return fast_clear_index(0, (uint32_t)f, c, 16); }
NVN_NOPROTO(nvnDeviceRegisterFastClearColoriIndex, int, NVNdevice *d, const int *c, NVNformat f) { (void)d; return fast_clear_index(1, (uint32_t)f, c, 16); }
NVN_NOPROTO(nvnDeviceRegisterFastClearColoruiIndex, int, NVNdevice *d, const uint32_t *c, NVNformat f) { (void)d; return fast_clear_index(1, (uint32_t)f, c, 16); }   // 驱动与 Colori 同一函数
NVN_NOPROTO(nvnDeviceRegisterFastClearDepthIndex, int, NVNdevice *d, float v) { (void)d; return fast_clear_index(2, 0, &v, 4); }
NVN_NOPROTO(nvnDeviceRegisterFastClearStencilIndex, int, NVNdevice *d, int v, int x) { (void)d; (void)x; return fast_clear_index(3, 0, &v, 4); }
NVN_NOPROTO(nvnDeviceRegisterFastClearStencil, NVNboolean, NVNdevice *d, int v) { return h_nvnDeviceRegisterFastClearStencilIndex(d, v, 0) >= 0; }   // 驱动：调 Index 版、回「序号 ≥ 0」

// NVX 通用地址（strcmp 链里的三个，20.5 起）：池构建器请求把池放在指定 GPU 地址（驱动存构建器 +0x28），池回自己的 GPU 地址（驱动读池 +0x18，即 GetBufferAddress 那个）。
//   宿主按自己的地址窗口分池地址（pool_ga），不按请求放：请求非 0 时首次报红；读回的是池实际的地址
NVN_NOPROTO(nvnMemoryPoolBuilderSetGenericAddressNVX, void, NVNmemoryPoolBuilder *b, uint64_t a) {
    (void)b; if (a) { static int once; if (!once++) LOG("【红】 MemoryPoolBuilderSetGenericAddressNVX(%#llx)：宿主不按请求的 GPU 地址放池，池地址按宿主窗口分", (unsigned long long)a); } }
NVN_NOPROTO(nvnMemoryPoolGetGenericAddressNVX, uint64_t, const NVNmemoryPool *p) { return pool_ga((const pool_t *)p); }
// 程序的调试数据哈希（0x660de0：程序带调试信息时回其哈希表指针，否则 0）：宿主程序不带驱动的调试数据库，回 0（= 没有调试数据）
NVN_NOPROTO(nvnProgramGetDebugDataHashesNVX, const void *, const NVNprogram *p) { (void)p; return NULL; }

// 延后推常量：数据指针录下，回放（提交时）再读（真机：控制内存里一条令牌、队列处理时读，nvn2_cmd.h p_push_const_ptr）
static void push_const_ptr(NVNcommandBuffer *c, int compute, int slot, int offset, int size, const void *data) {
    unsigned s, off, n;
    if (!data || !nvn2_push_range(slot, offset, size, &s, &off, &n)) return;
    p_push_const_ptr *o = (p_push_const_ptr *)rec(c, OP_PUSH_CONST_PTR, sizeof *o);
    o->compute = (uint8_t)compute; o->slot = (uint8_t)s; o->offset = (uint16_t)off; o->size = n; o->data = (uint64_t)(uintptr_t)data;
}
NVN_NOPROTO(nvnCommandBufferPushGraphicsConstantsDeferred, void, NVNcommandBuffer *c, int slot, int offset, int size, const void *data) { push_const_ptr(c, 0, slot, offset, size, data); }
NVN_NOPROTO(nvnCommandBufferPushComputeConstantsDeferred, void, NVNcommandBuffer *c, int slot, int offset, int size, const void *data) { push_const_ptr(c, 1, slot, offset, size, data); }

// 宿主做不了的状态：缺省值（= 宿主本来的行为）不录、不报；改成非缺省时首次报红，照常画（真机这几条都只是往命令流里写一个方法，不影响别的状态）
#define NVN2_RED_IF(cond, ...) do { if (cond) { static int once_; if (!once_++) LOG("【红】 " __VA_ARGS__); } } while (0)
// 计算 / 拷贝引擎开关（0x688d50 / 0x64fd30：写一个使能方法）：缺省开；关掉后真机跳过之后的计算 / 拷贝，宿主照做 → 关时报红
NVN_NOPROTO(nvnCommandBufferSetComputeEnable, void, NVNcommandBuffer *c, uint32_t on) { (void)c; NVN2_RED_IF(!B(on), "CommandBufferSetComputeEnable(false)：宿主不跳过之后的计算派发"); }
NVN_NOPROTO(nvnCommandBufferSetCopyEnable, void, NVNcommandBuffer *c, uint32_t on) { (void)c; NVN2_RED_IF(!B(on), "CommandBufferSetCopyEnable(false)：宿主不跳过之后的拷贝"); }
// 保守光栅化（模式, 膨胀）：模式 0 = 关（驱动按模式拼方法值，0 时只剩膨胀位）。Apple GPU 的 Metal 没有保守光栅化
NVN_NOPROTO(nvnCommandBufferSetConservativeRasterMode, void, NVNcommandBuffer *c, int mode, int dilate) {
    (void)c; NVN2_RED_IF(mode != 0, "CommandBufferSetConservativeRasterMode(%d, %d)：Metal 没有保守光栅化，按普通光栅化画", mode, dilate); }
// 浮点混合夹取：打开时浮点渲染目标的混合结果夹到 [0, 1]；Metal 的浮点目标混合不夹 → 打开时报红
NVN_NOPROTO(nvnCommandBufferSetFloatBlendClamp, void, NVNcommandBuffer *c, uint32_t on) { (void)c; NVN2_RED_IF(B(on), "CommandBufferSetFloatBlendClamp(true)：Metal 浮点目标混合不夹取"); }
// 排他裁剪（矩形内的片元丢弃）：矩形只在打开时生效，Set 两条只记不录；打开时报红（宿主照画矩形内的片元）
NVN_NOPROTO(nvnCommandBufferEnableExclusiveScissor, void, NVNcommandBuffer *c, uint32_t on) { (void)c; NVN2_RED_IF(B(on), "CommandBufferEnableExclusiveScissor(true)：Metal 没有排他裁剪，矩形内照画"); }
NVN_NOPROTO(nvnCommandBufferSetExclusiveScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { (void)c; (void)x; (void)y; (void)w; (void)h; }
NVN_NOPROTO(nvnCommandBufferSetExclusiveScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) { (void)c; (void)first; (void)n; (void)r; }
// 可变着色率：着色率图（贴图, 视图；空 = 关）绑上才有降率，宿主按全速率着色（画面不缺内容、只多算）→ 绑非空时报红；调色板模式非 0 报红；调色板本身只是数据
NVN_NOPROTO(nvnCommandBufferBindShadingRateImage, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v) {
    (void)c; (void)v; NVN2_RED_IF(t != NULL, "CommandBufferBindShadingRateImage：宿主没有可变着色率，按全速率着色"); }
NVN_NOPROTO(nvnCommandBufferSetShadingRatePaletteMode, void, NVNcommandBuffer *c, int mode) { (void)c; NVN2_RED_IF(mode != 0, "CommandBufferSetShadingRatePaletteMode(%d)：宿主没有可变着色率，按全速率着色", mode); }
NVN_NOPROTO(nvnCommandBufferShadingRateImagePalette, void, NVNcommandBuffer *c, int first, int n, const void *p) { (void)c; (void)first; (void)n; (void)p; }

// 仍是未实现桩的 NVN2 入口与原因（调用即报红并标直连异常，nvn_unimpl_c 打出原因）。按名前缀匹配
static const struct { const char *name; const char *why; } g_nvn2_red[] = {
    {"nvnCommandBufferDrawMeshTasks3D", "三维工作组的网格绘制：宿主只接了一维（DrawMeshTasks / DrawMeshTasksIndirect，起始号是缓冲槽 29 的一个 u32），三维起始号与工作组形状没有接口；时之笛（11-05 第十一版）没调"},
    {"nvnCommandBufferMultiDrawMeshTasks", "多条间接网格绘制：要按 GPU 写的条数（Count 版）逐条画、每条带自己的起始号，宿主没做（单条 DrawMeshTasksIndirect 已做）；时之笛（11-05 第十一版）没调"},
    {"nvnCommandBufferDispatchRays", "光追：宿主没有光追管线（加速结构格式、光追着色器都没有翻译）"},
    {"nvnCommandBufferBindAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnCommandBufferBuildAccelerationStructures", "光追：同 DispatchRays"},
    {"nvnCommandBufferCompactAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnCommandBufferCopyAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnCommandBufferRelocateAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnCommandBufferGetAccelerationStructureVisualizationData", "光追：同 DispatchRays"},
    {"nvnDeviceCalculateAccelerationStructureMemoryRequirements", "光追：同 DispatchRays（回大小会让游戏以为能建加速结构）"},
    {"nvnDeviceCompactAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnDeviceCopyAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnDeviceRelocateAccelerationStructure", "光追：同 DispatchRays"},
    {"nvnProgramGetShaderGroupHandle", "光追：光追程序建不成（ProgramInitializeFromBuilder 带光追模块回失败），没有着色器组句柄"},
    {"nvnCommandBufferBeginDeviceCommands", "设备生成命令：GPU 写硬件命令流再执行，宿主回放的是自己的记录，不执行硬件命令流"},
    {"nvnCommandBufferEndDeviceCommands", "设备生成命令：同 BeginDeviceCommands"},
    {"nvnCommandBufferSubmitDeviceCommands", "设备生成命令：同 BeginDeviceCommands"},
    {"nvnCommandBufferComputeDirtyTileMap", "脏块图：GPU 按块比较贴图内容写出脏块图，宿主没有这套计算（参数对象能设能读）"},
    {"nvnCommandBufferConvertCooperativeVectorMatrix", "协作向量（张量核矩阵布局转换）：Metal 没有对应布局"},
    {"nvnDeviceConvertCooperativeVectorMatrix", "协作向量：同上"},
    {"nvnDeviceGetCooperativeVectorMatrixSize", "协作向量：同上（回大小会让游戏以为能用）"},
    {"nvnCommandBufferSetComputeEnableConditional", "条件开关计算：按 GPU 内存里的值决定，宿主回放没有这类条件状态"},
    {"nvnCommandBufferSetCopyEnableConditional", "条件开关拷贝：同上"},
    {"nvnCommandBufferSetProgramSubroutines", "子程序：着色器整段翻译，没有子程序表"},
    {"nvnCommandBufferSetProvokingVertexMode", "平面着色取哪个顶点：Metal 没有可设的激发顶点，宿主不重排索引"},
    {"nvnMultisampleStateGetSampleLocations", "可编程采样位置：宿主不存不回放（SetSampleLocations 空操作），读不回游戏设的值"},
    {"nvnProgramGetScratchBytesPerWarp", "要按驱动从着色器控制段算每 warp 的暂存字节，宿主没有这份控制段解析"},
    {"nvnProgramGetDeviceComputeMemorySize", "设备生成命令用的计算内存：同 BeginDeviceCommands"},
    {"nvnSamplerPoolWriteDescriptorTableData", "直接写硬件采样器描述符（TSC 原始字节）：宿主的描述符池不是硬件格式"},
    {"nvnTexturePoolWriteDescriptorTableData", "直接写硬件贴图描述符（TIC 原始字节）：宿主的描述符池不是硬件格式"},
};
static const char *nvn2_red_why(const char *name) {
    for (unsigned k = 0; k < sizeof g_nvn2_red / sizeof *g_nvn2_red; k++) if (!strncmp(name, g_nvn2_red[k].name, strlen(g_nvn2_red[k].name))) return g_nvn2_red[k].why;
    return NULL;
}

// ---------- Commands used by newer-SDK games (SMBW): most map onto existing record formats, the replay layer is unchanged ----------
// Singular / combined forms → plural / separate forms; state the replay layer does not have yet (conditional render, depth bounds, ...) is logged once and drawing continues (better to over-draw than to drop).
#include "../tex/nvn_deswizzle.h"

NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {   // the source runs as it is at submit time (Copy above takes its records now)
    for (int i = 0; i < n; i++) { uint8_t *p = cmd_alloc(CB(c), OP_CALL, 16); memcpy(p + 8, &h[i], 8); }
}
// Combined texture handle (texture id | sampler id << 20) is recorded into the texture slot as is: the replay layer takes the texture from the low 20 bits and the sampler from the high bits (nvn_mtl.m NVN_RB_SAMPT)
// One combined handle: its device word is kept in the game's command memory, where Luigi's Mansion 3 patches it after recording (rec_bind_tex1)
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) { rec_bind_tex1(CB(c), st, i, (uint64_t)h, CB(c)->tex_base, CB(c)->samp_base, 1); }
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); }
// "Deferred" binding: the GPU reads the handles h points to when it executes this command, not at recording. The per-texture binding of The Legend of Zelda: Breath of the Wild's engine (main+0xec2e64) always uses it,
//   passing the address of the handle field in the texture sampler object with a count of 1; command blocks are recorded once and submitted repeatedly, and the handle is written / replaced afterwards. The host used to copy the handles at recording (mostly 0),
//   so every texture of the deferred lighting composite became the dummy texture 0 and the whole 3D picture was black. The address is recorded and the replay layer reads it when it reads the record (bh_fresh)
NVN_NOPROTO(nvnCommandBufferBindTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { st = nvn2_stage_host(st);
    if (n < 0 || n > 32) { LOG("【红】 bind handle count %d", n); return; }
    p_bind_handles *r = rec(c, OP_BIND_TEX, offsetof(p_bind_handles, h) + 8 * (size_t)n + sizeof(p_bind_pool_bases));
    r->stage = st; r->first = first; r->n = n; r->defer = 1; r->src = (uint64_t)(uintptr_t)h;
    // rec zeroes h; the source is not consumed until replay. Keep the recording-time pools with this bind.
    p_bind_pool_bases bases = {CB(c)->tex_base, CB(c)->samp_base};
    memcpy(r->h + n, &bases, sizeof bases);
}
// 其余句柄类的 Deferred 版（10-25 批量补齐；nvn.h 没有原型，签名同各自的非 Deferred 版，做法同上面的 BindTexturesDeferred）：录游戏指针，回放读时再取句柄（nvn_mtl.m bh_fresh）。
//   回放的 bh_fresh 对纹理 ID（低 20 位）和采样器 ID（20 位起）都加录制时的池基址；按 bind_handles 的口径，图像只加纹理基址、独立采样器只加采样器基址，所以不加的那个基址存 0
static void bind_handles_deferred(NVNcommandBuffer *c, int op, NVNshaderStage st, int first, int n, const void *h, int32_t tbase, int32_t sbase) { st = nvn2_stage_host(st);
    if (n < 0 || n > 32) { LOG("【红】 bind handle count %d", n); return; }
    p_bind_handles *r = rec(c, op, offsetof(p_bind_handles, h) + 8 * (size_t)n + sizeof(p_bind_pool_bases));
    r->stage = st; r->first = first; r->n = n; r->defer = 1; r->src = (uint64_t)(uintptr_t)h;
    p_bind_pool_bases bases = {tbase, sbase};
    memcpy(r->h + n, &bases, sizeof bases);
}
NVN_NOPROTO(nvnCommandBufferBindImagesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles_deferred(c, OP_BIND_IMG, st, first, n, h, CB(c)->tex_base, 0);
}
NVN_NOPROTO(nvnCommandBufferBindSeparateTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles_deferred(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + first, n, h, CB(c)->tex_base, CB(c)->samp_base);
}
NVN_NOPROTO(nvnCommandBufferBindSeparateSamplersDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles_deferred(c, OP_BIND_SAMP, st, first, n, h, 0, CB(c)->samp_base);
}
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNimageHandle h) { uint64_t v = (uint64_t)h; bind_handles(c, OP_BIND_IMG, st, i, 1, &v); }
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { NVNbufferRange r = {a, n}; h_nvnCommandBufferBindVertexBuffers(c, i, 1, &r); }

// Draws
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) { draw_elements(c, pr, it, n, ib, 0, 0, 1, 24); }
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv) { draw_elements(c, pr, it, n, ib, bv, 0, 1, bv ? 72 : 48); }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n, int bi, int inst) {
    rec_draw(c, (p_draw){pr, first, n, inst, bi});
}
// Indirect, as DrawArraysIndirect: the record at ind is read by the GPU (nvn_mtl.m draw)
// Indirect-count multi-draws (Monster Hunter Rise's GPU-driven scene: compute culling writes the draw records and a u32 count, then up to max draws are made from them; 1–49 per call,
// stride 20, the records and counts in its sparse virtual pool). Metal has no draw count read by the GPU, so each multi-draw becomes max ordinary indirect draws that carry the count's address
// and their index; the replay masks their records on the GPU, zeros past the count (nvn_mtl.m mdx_run). Both used to be empty, and the whole scene stayed undrawn.
// A call above MDI_INLINE draws records the range once (OP_MULTI_DRAW, nvn2_cmd.h; upstream reference implementation, MIT, c7ba23df: MIO's GPU-driven scene, maxima 4,594 and 8,192, lost
//   every draw past the first 1,024 when the call was cut there). A backend that does not replay that record (nvn_be_multi_draw_range 0: Android) keeps the old cut
int nvn_be_multi_draw_range(void);   // nvn_mtl.m 1 / 安卓 nvn_be_android.c 0
enum { MDI_INLINE = 1024 };
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_INLINE && nvn_be_multi_draw_range()) { *REC(c, OP_MULTI_DRAW, p_multi_draw) = (p_multi_draw){GA(ind), GA(cnt), GA(ib), stride, max, pr, it, 0}; return; }
    if (max > MDI_INLINE) { ONCE_LOG("【红】 indirect-count multi-draw of up to %d draws: only the first %d are made", max, MDI_INLINE); max = MDI_INLINE; }
    for (int i = 0; i < max; i++) rec_draw_idx(c, (p_draw_idx){pr, it, 0, 0, 0, 1, GA(ib), GA(ind + (uint64_t)i * (uint64_t)stride), GA(cnt), i});
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_INLINE && nvn_be_multi_draw_range()) { *REC(c, OP_MULTI_DRAW, p_multi_draw) = (p_multi_draw){GA(ind), GA(cnt), 0, stride, max, pr, -1, 0}; return; }
    if (max > MDI_INLINE) { ONCE_LOG("【红】 indirect-count multi-draw of up to %d draws: only the first %d are made", max, MDI_INLINE); max = MDI_INLINE; }
    for (int i = 0; i < max; i++) rec_draw(c, (p_draw){pr, 0, 0, 1, 0, GA(ind + (uint64_t)i * (uint64_t)stride), GA(cnt), i});
}
#if defined(__ANDROID__) || defined(__OHOS__)
// Whether the host can read n bytes at a guest address: outside a sparse virtual pool always; inside one only where pages are mapped (nvn_mtl.m reads multi-draw counts there)
int nvn_host_readable(uint64_t a, uint64_t n) {
    int ok = 1; vp_lock();
    size_t k = vpa_find(a); struct vpool *v = k < g_nvpa ? g_vpa[k] : NULL;   // reservations do not overlap: only the last one starting at or below a can hold it
    if (v && a < (uint64_t)(uintptr_t)v->base + v->len) {
        uint64_t o = a - (uint64_t)(uintptr_t)v->base; ok = 0;
        for (int r = 0; r < v->nrun; r++) if (o >= v->run[r].lo && o + n <= v->run[r].hi) { ok = 1; break; }
        if (!ok) ok = vsh_cover(v, o, n, n);   // pages not aliasable: copies of the page table (vsh_*)
    }
    vp_unlock(); return ok;
}
const uint8_t *nvn_host_ptr(uint64_t a, uint64_t n) { return nvn_host_readable(a, n) ? (const uint8_t *)(uintptr_t)a : NULL; }   // 安卓：虚拟池地址就是宿主地址
#else
// The host bytes behind n bytes at a buffer address: the address itself outside the virtual pools; inside one the physical memory behind it when one run holds the range, else a copy
//   of its pages (vsh_*); NULL where a page has nothing mapped (nvn_mtl.m reads indirect arguments, counts and index buffers this way). A virtual pool's address is no host memory
const uint8_t *nvn_host_ptr(uint64_t a, uint64_t n) {
    const uint8_t *r = (const uint8_t *)(uintptr_t)a; vp_lock();
    struct vpool *v = vpool_holding(a);
    if (v) { uint64_t o = a - (uint64_t)(uintptr_t)v->base; r = NULL;
        for (int k = 0; k < v->nrun && !r; k++) if (o >= v->run[k].lo && o + n <= v->run[k].hi) r = v->run[k].phys + (o - v->run[k].lo);
        if (!r) { int i = vsh_cover(v, o, n, n); if (i >= 0) r = v->sh[i].mem + (o - v->sh[i].off); } }
    vp_unlock(); return r;
}
// Whether the bytes at a may be read at that address itself: not inside a virtual pool (its address is no host memory)
int nvn_host_readable(uint64_t a, uint64_t n) { (void)n; vp_lock(); int r = !vpool_holding(a); vp_unlock(); return r; }
#endif
// Textures of a sparse pool, read through its page table (see the sparse pools above): whether a pool is one, a copy of a texture's bytes (unmapped pages read as 0; → bytes not mapped),
// whether its content may have changed since an upload (a mapping in its range newer than gen, or a physical page behind it written after the dirty-page stamp), and the write
// protection of the physical pages behind it (taken before an upload, as for any packaged texture)
static struct vpool *vpool_at(const void *pool, uint64_t a) {
    const pool_t *o = pool; struct vpool *v = vpool_of(o);   // caller holds g_vp_mu
    return v && a >= (uint64_t)(uintptr_t)v->base && a < (uint64_t)(uintptr_t)v->base + v->size ? v : NULL;
}
// 帧录 / 帧回放（nvn_frame_rec.inc vpool.bin）：虚拟池的地址段与页表。贴图 / 缓冲地址 = 池基址 + 偏移，写在游戏内存里，回放要同址重建。
//   录：活着的每个虚拟池 {池对象, 基址, 窗口长, 大小, 段数} + 段 {虚拟偏移, 长, 物理地址}（页表里虚拟、物理都连续的页合成一段）；
//   放：同址建池（地址只是一个数、不占宿主内存；早先版本录的池地址是宿主预留，照样当数用），页表照段写回，各段照录时那样登记成回放层的别名
#if !defined(__ANDROID__) && !defined(__OHOS__)   // 帧录放只在苹果宿主
typedef struct { uint64_t pool, base, len, size, nseg; } vp_rec_hd;
typedef struct { uint64_t voff, size, phys; } vp_rec_seg;
int s2a_hle_vpool_save(FILE *f) {   // → 存了几个，-1 写失败
    vp_lock(); int n = 0, ok = 1; uint64_t cnt = g_nvpa; ok = fwrite(&cnt, 8, 1, f) == 1;
    for (size_t i = 0; ok && i < g_nvpa; i++) {
        struct vpool *v = g_vpa[i]; size_t ns = 0, cs = 0; vp_rec_seg *sg = NULL;
        for (uint64_t l = 0; l < (v->size + (2u << 20) - 1) >> 21; l++) { const vpte *lf = v->pt[l]; if (!lf) continue;
            for (int k = 0; k < VPT_LEAF; k++) { if (!lf[k].phys) continue; uint64_t vo = (l << 21) + ((uint64_t)k << 12), ph = (uint64_t)(uintptr_t)lf[k].phys;
                if (ns && sg[ns - 1].voff + sg[ns - 1].size == vo && sg[ns - 1].phys + sg[ns - 1].size == ph) { sg[ns - 1].size += 0x1000; continue; }
                if (ns == cs) { cs = cs ? cs * 2 : 64; vp_rec_seg *t = realloc(sg, cs * sizeof *sg); if (!t) die("frame recording: virtual pool segment table"); sg = t; }
                sg[ns++] = (vp_rec_seg){vo, 0x1000, ph}; } }
        vp_rec_hd hd = {(uint64_t)(uintptr_t)v->pool, (uint64_t)(uintptr_t)v->base, v->len, v->size, ns};
        ok = fwrite(&hd, sizeof hd, 1, f) == 1 && (!ns || fwrite(sg, sizeof *sg, ns, f) == ns); free(sg); n++;
    }
    vp_unlock(); return ok ? n : -1;
}
int s2a_hle_vpool_load(FILE *f) {   // → 重建了几个，-1 读失败
    uint64_t cnt = 0; if (fread(&cnt, 8, 1, f) != 1) return -1;
    int n = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        vp_rec_hd hd; if (fread(&hd, sizeof hd, 1, f) != 1) return -1;
        vp_rec_seg *sg = hd.nseg ? malloc(hd.nseg * sizeof *sg) : NULL; if (hd.nseg && (!sg || fread(sg, sizeof *sg, hd.nseg, f) != hd.nseg)) { free(sg); return -1; }
        uint8_t *base = (uint8_t *)(uintptr_t)hd.base; const pool_t *o = (const pool_t *)(uintptr_t)hd.pool;
        vp_lock(); int dup = vpool_of(o) != NULL; vp_unlock(); if (dup) { free(sg); continue; }
        struct vpool *v = vpool_new(o, base, hd.len);
        if (!v) { LOG("【红】 回放：虚拟池 %p 地址 %p+%#llx 与活着的虚拟池重叠，引用它的贴图 / 缓冲回放不对", (const void *)o, base, (unsigned long long)hd.len); free(sg); continue; }
        vp_lock();
        for (uint64_t k = 0; k < hd.nseg; k++) {
            vpt_set(v, sg[k].voff, sg[k].size, (const uint8_t *)(uintptr_t)sg[k].phys);
            vrun_set(v, sg[k].voff, sg[k].voff + sg[k].size, (const uint8_t *)(uintptr_t)sg[k].phys);
        }
        vp_unlock(); free(sg); n++;
    }
    return n;
}
#endif
int nvn_sparse_pool(const void *pool) {   // called per texture lookup: the hash table's read lock only (see g_vph)
    const pool_t *o = pool; pthread_rwlock_rdlock(&g_vph_lk); int r = vpool_of(o) != NULL; pthread_rwlock_unlock(&g_vph_lk); return r;
}
uint64_t nvn_sparse_gen(void) { return atomic_load(&g_vp_gen); }
// 一个虚拟池的映射代号与建池序号（nvn_mtl.m gstex_*：GPU 写的稀疏贴图在本池映射变了时才同步块；序号变了 = 池已收尾、地址被新池复用）；不是虚拟池 → 0
void nvn_sparse_pool_gen(const void *pool, uint64_t *gen, uint64_t *id) {
    const pool_t *o = pool; pthread_rwlock_rdlock(&g_vph_lk); struct vpool *v = vpool_of(o);
    *gen = v ? atomic_load(&v->gen) : 0; *id = v ? v->id : 0; pthread_rwlock_unlock(&g_vph_lk);
}
// a 起 64 KB 背后的物理内存：16 个 4 KB 页是一整段连续物理内存时给它的地址，否则 0。同一物理内存背后的虚拟块共用一个 Metal 堆页（nvn_stex.inc，移植自 kartnative，MIT）
uint64_t nvn_sparse_backing(const void *pool, uint64_t a) {
    vp_lock(); struct vpool *v = vpool_at(pool, a); const uint8_t *p0 = NULL;
    if (v) { uint64_t o = a - (uint64_t)(uintptr_t)v->base; const vpte *e = (o & 0xFFF) || o + 0x10000 > v->size ? NULL : vpt_get(v, o); p0 = e ? e->phys : NULL;
        for (unsigned i = 1; p0 && i < 16; i++) { const vpte *f = vpt_get(v, o + i * 0x1000ull); if (!f || f->phys != p0 + i * 0x1000) p0 = NULL; } }
    vp_unlock(); return (uint64_t)(uintptr_t)p0;
}
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n) {
    vp_lock(); struct vpool *v = vpool_at(pool, a); uint64_t miss = 0;
    if (!v) { vp_unlock(); memset(dst, 0, n); return n; }
    for (uint64_t p = 0, o = a - (uint64_t)(uintptr_t)v->base; p < n;) {
        uint64_t at = o + p, k = 0x1000 - (at & 0xFFF); if (k > n - p) k = n - p;
        const vpte *e = at < v->size ? vpt_get(v, at) : NULL;
        if (e && e->phys) memcpy((uint8_t *)dst + p, e->phys + (at & 0xFFF), k); else { memset((uint8_t *)dst + p, 0, k); miss += k; }
        p += k;
    }
    vp_unlock(); return miss;
}
// Runs of consecutive physical pages behind [a, a+n): fn on each (nonzero stops the walk and is returned), or 1 at the first page mapped after gen (gen ~0 = do not look)
static int vpt_walk(const void *pool, uint64_t a, uint64_t n, uint64_t gen, int (*fn)(const uint8_t *, uint64_t, uint64_t), uint64_t arg) {
    vp_lock(); struct vpool *v = vpool_at(pool, a); int r = 0;
    if (!v) { vp_unlock(); return 1; }
    const uint8_t *rs = NULL, *re = NULL; uint64_t o = a - (uint64_t)(uintptr_t)v->base;
    for (uint64_t at = o & ~0xFFFull; at < o + n && at < v->size && !r; at += 0x1000) {
        const vpte *e = vpt_get(v, at);
        if (!e) { at |= (2u << 20) - 1; at -= 0xFFF; continue; }   // a leaf never mapped: skip its 2 MB
        if (gen != ~0ull && e->gen > gen) { r = 1; break; }
        if (!e->phys) continue;
        if (rs && e->phys == re) { re += 0x1000; continue; }
        if (rs) r = fn(rs, (uint64_t)(re - rs), arg);
        rs = e->phys; re = rs + 0x1000;
    }
    if (!r && rs) r = fn(rs, (uint64_t)(re - rs), arg);
    vp_unlock(); return r;
}
static int run_written(const uint8_t *p, uint64_t n, uint64_t stamp) { return nvn_dirty_since(p, n, stamp); }
static int run_protect(const uint8_t *p, uint64_t n, uint64_t unused) { (void)unused; nvn_dirty_protect(p, n); return 0; }
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) { return vpt_walk(pool, a, n, gen, run_written, stamp); }
// 映射变更日志（马车世界缺口 ①）：稀疏贴图每次提交原先逐页走整段页表（nvn_sparse_since），上游读数这一项占提交线程一半。现在贴图记下日志序号和背后的物理段，
//   之后只问「序号以来有没有和我重叠的映射请求」：没有就只查物理段写没写过（nvn_mtl.m pkg 检查）。
// 本池日志的当前序号（第一次问时建日志：之前的请求不在日志里，调用方此时整段走一遍再记序号）；不是虚拟池 → 0
uint64_t nvn_sparse_log_seq(const void *pool) {
    vp_lock(); struct vpool *v = vpool_of(pool); uint64_t n = 0;
    if (v) { if (!v->log && !(v->log = calloc(VLOG_N, sizeof *v->log))) die("sparse pool change log allocation failed"); n = atomic_load_explicit(&v->nlog, memory_order_acquire); }
    vp_unlock(); return n;
}
// *seq 以来本池有没有和 [a, a + n)（宿主地址）重叠的映射请求：1 = 有，或者日志已转过一圈 / 不是虚拟池 / 没建日志（都按有算，调用方整段走）；0 = 没有，*seq 推进到当前
int nvn_sparse_log_hit(const void *pool, uint64_t *seq, uint64_t a, uint64_t n) {
    vp_lock(); struct vpool *v = vpool_of(pool); int hit = 1;
    if (v && v->log) {
        uint64_t end = atomic_load_explicit(&v->nlog, memory_order_acquire), lo = a - (uint64_t)(uintptr_t)v->base;
        if (end - *seq <= VLOG_N) { hit = 0;
            for (uint64_t k = *seq; k < end && !hit; k++) { const struct vlog_ent *e = &v->log[k % VLOG_N]; hit = e->off < lo + n && lo < e->off + e->len; }
            if (!hit) *seq = end; }
    }
    vp_unlock(); return hit;
}
// [a, a + n) 背后连续物理页的段（物理地址、字节数），最多 max 段：段数；多于 max / 不是虚拟池 → -1（调用方不缓存，照旧整段走）
int nvn_sparse_runs(const void *pool, uint64_t a, uint64_t n, uint64_t (*out)[2], int max) {
    vp_lock(); struct vpool *v = vpool_at(pool, a); int k = 0;
    if (!v) { vp_unlock(); return -1; }
    const uint8_t *rs = NULL, *re = NULL; uint64_t o = a - (uint64_t)(uintptr_t)v->base;
    for (uint64_t at = o & ~0xFFFull; at < o + n && at < v->size && k >= 0; at += 0x1000) {
        const vpte *e = vpt_get(v, at);
        if (!e) { at |= (2u << 20) - 1; at -= 0xFFF; continue; }
        if (!e->phys) continue;
        if (rs && e->phys == re) { re += 0x1000; continue; }
        if (rs) { if (k == max) k = -1; else { out[k][0] = (uint64_t)(uintptr_t)rs; out[k][1] = (uint64_t)(re - rs); k++; } }
        rs = e->phys; re = rs + 0x1000;
    }
    if (k >= 0 && rs) { if (k == max) k = -1; else { out[k][0] = (uint64_t)(uintptr_t)rs; out[k][1] = (uint64_t)(re - rs); k++; } }
    vp_unlock(); return k;
}
int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n) {   // every page of [a, a+n) has memory behind it
    vp_lock(); struct vpool *v = vpool_at(pool, a); int ok = v != NULL;
    for (uint64_t o = v ? (a - (uint64_t)(uintptr_t)v->base) & ~0xFFFull : 0, end = v ? a - (uint64_t)(uintptr_t)v->base + n : 0; ok && o < end; o += 0x1000) { const vpte *e = o < v->size ? vpt_get(v, o) : NULL; ok = e && e->phys; }
    vp_unlock(); return ok;
}
void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n) { vpt_walk(pool, a, n, ~0ull, run_protect, 0); }
// Metal 稀疏贴图（nvn_mtl.m sp_upload）：[a, a+n) 按 64 KB 页出位图（bits 第 k 位 = 第 k 页 16 个 4 KB 小页全有物理页），一次加锁走完；
//   没映射过的 2 MB 叶整块跳过。返回有映射的页数。a 须 64 KB 对齐（稀疏贴图存储按页对齐）
uint64_t nvn_sparse_map_bits(const void *pool, uint64_t a, uint64_t n, uint8_t *bits) {
    uint64_t np = (n + 0xFFFF) >> 16, got = 0; memset(bits, 0, (size_t)((np + 7) >> 3));
    vp_lock(); struct vpool *v = vpool_at(pool, a);
    if (!v) { vp_unlock(); return 0; }
    uint64_t o0 = a - (uint64_t)(uintptr_t)v->base;
    for (uint64_t k = 0; k < np; k++) {
        uint64_t o = o0 + (k << 16); int ok = 1;
        for (uint64_t q = 0; q < 0x10000 && ok; q += 0x1000) {
            const vpte *e = o + q < v->size ? vpt_get(v, o + q) : NULL;
            if (!e) { ok = 0; uint64_t leaf_end = ((o + q) | ((2u << 20) - 1)) + 1; uint64_t kk = (leaf_end - o0) >> 16; if (kk > k + 1) k = kk - 1; break; }   // 向下取整：跨叶边界的那页照样逐个查   // 整个 2 MB 叶没映射：跳到叶尾
            ok = e->phys != NULL;
        }
        if (ok) { bits[k >> 3] |= (uint8_t)(1u << (k & 7)); got++; }
    }
    vp_unlock(); return got;
}
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind) {
    rec_draw_idx(c, (p_draw_idx){pr, it, 0, 0, 0, 1, GA(ib), GA(ind)});
}

// Viewport / scissor / depth range: single and multiple forms
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) { float v[4] = {(float)x, (float)y, (float)w, (float)h}; h_nvnCommandBufferSetViewports(c, 0, 1, v); }
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetScissor(c, r[0], r[1], r[2], r[3]);
    if (first + n > 1) ONCE_LOG("SetScissors first=%d n=%d: the host uses only index 0", first, n);
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetDepthRange(c, r[0], r[1]);
    if (first + n > 1) ONCE_LOG("SetDepthRanges first=%d n=%d: the host uses only index 0", first, n);
}

// GPU value write: ReportValue = write a 32-bit value to the address when this point is reached (the replay layer's event write, applied on completion)
NVN(nvnCommandBufferReportValue, void, NVNcommandBuffer *c, uint32_t v, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), OP_SIGNAL_EVENT, 24);
    volatile uint32_t *val = (volatile uint32_t *)(uintptr_t)GA(a); uint32_t mm = NVN_EVENT_SIGNAL_MODE_WRITE;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// Clear a buffer to a 32-bit value: one OP_CLEAR_BUF record, filled in order within the submission at replay (fillBuffer when all bytes match, otherwise copied from a pattern buffer)
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t v) { *REC(c, OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){GA(a), n, v, 0}; }

// State the replay layer does not have yet: logged once (conditional rendering is treated as "always draw")
// Render enable (profile render_cond, see OP_RENDER_COND in nvn_mtl.m): recorded and evaluated at replay; without the key everything draws, as before
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t en) {
    if (DM_GAME_RENDER_COND) *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){(en & 0xff) ? 2 : 3, 0, 0};
    else if (!(en & 0xff)) ONCE_LOG("【红】 SetRenderEnable(false): the host draws anyway for now");
}
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) {
    if (DM_GAME_RENDER_COND) *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){(int)m & 1, 0, GA(a)};
    else ONCE_LOG("【红】 SetRenderEnableConditional: the host draws anyway for now");
}
// Fire Emblem: Three Houses turns depth clamp on for its scenes; ignored, the ground right under its close-up battle cameras was clipped away and the lighting pass painted the sky's fog colour there
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_DEPTH_CLAMP, p_depth_clamp)->on = (en & 0xff) != 0; }
// Absolum (FNA3D's NVN driver) keeps the lower-left window origin and sets viewport 0's swizzle every frame (NEGATIVE_Y: clip y negated, how a GL-heritage renderer flips its picture).
// Ignored, its whole picture came out upside down. The replay applies it to the viewport (draw in nvn_mtl.m)
NVN(nvnCommandBufferSetViewportSwizzles, void, NVNcommandBuffer *c, int first, int n, const NVNviewportSwizzle *s) {
    if (n < 0 || first < 0 || first + n > 16) { LOG("【红】 SetViewportSwizzles first=%d n=%d", first, n); return; }
    p_vp_swizzle *r = REC(c, OP_VP_SWIZZLE, p_vp_swizzle); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) for (int k = 0; k < 4; k++) r->sw[i][k] = (uint8_t)((s[4 * i + k] ^ 2 * k) & 7);
}
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float r) { *REC(c, OP_ALPHA_REF, p_alpha_ref) = (p_alpha_ref){r, 0}; }   // the replay keeps it for the alpha-tested fragment variants (nvn_mtl.m alpha_ref_bind)
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) { (void)c; if (w != 1.0f) ONCE_LOG("SetLineWidth %.2f: Metal has only 1-pixel lines", w); }
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float s) { (void)c; (void)s; ONCE_LOG("SetPointSize: the point size comes from the shader"); }
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t en, int idx) { *REC(c, OP_PRIM_RESTART, p_prim_restart) = (p_prim_restart){(en & 0xff) != 0, (uint32_t)idx}; }
NVN(nvnCommandBufferSetPatchSize, void, NVNcommandBuffer *c, int sz) {
    *REC(c, OP_SET_PATCH_SIZE, p_set_patch_size) = (p_set_patch_size){sz};
}
// Rasterizer discard: draws run their vertex stage only (with transform feedback, the capture); recorded, and at replay such a draw rasterizes nothing (nvn_mtl.m)
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_RAST_DISCARD, p_rast_discard)->on = (en & 0xff) != 0; }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) { (void)c; if ((m & 0xffff) != 0xffff) ONCE_LOG("SetSampleMask %#x: ignored by the host for now", m); }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; if (en & 0xff) ONCE_LOG("SetConservativeRasterEnable on: ignored by the host for now"); }
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }        // Tegra tiled cache hint, no Metal equivalent
// A bound target's contents may be discarded: they are undefined afterwards, so keeping them is correct (The Great Ace Attorney Chronicles discards colour target 0)
NVN(nvnCommandBufferDiscardColor, void, NVNcommandBuffer *c, int i) { (void)c; (void)i; }
NVN(nvnCommandBufferDiscardDepthStencil, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN_NOPROTO(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; (void)en; }   // color compression hints
NVN_NOPROTO(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int cls, const int *t) { (void)c; (void)cls; (void)t; }
// Resolve a multisample texture into a single-sample one (whole texture): recorded as a texture copy with DM_COPY_RESOLVE; replay uses Metal's multisample resolve (copy_t2t in nvn_mtl.m)
static void rec_downsample(NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv, uint64_t ra) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2);
    o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, ra); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, ra);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv);
    if (o->dst) nvn_be_pool_gpu_written(o->dst->pool);
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = DM_COPY_RESOLVE;
}
NVN(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, (uint64_t)__builtin_return_address(0)); }
NVN(nvnCommandBufferDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, (uint64_t)__builtin_return_address(0));
}
// 分块下采样（TiledDownsample）：与 Downsample 同签名、结果相同（多重采样解析到单采样），「分块」只是硬件做法；走同一条解析拷贝
NVN(nvnCommandBufferTiledDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, (uint64_t)__builtin_return_address(0)); }
NVN(nvnCommandBufferTiledDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, (uint64_t)__builtin_return_address(0));
}
// Metal 没有对应能力的光栅状态：保守光栅化膨胀、子像素精度偏置、覆盖调制表（混合采样）、细分级别（回放层还没有细分管线）。
//   接受调用、不判直连失败，首次打一行【红】点名（分类工具按 not implemented 记「部分」）
NVN(nvnCommandBufferSetConservativeRasterDilate, void, NVNcommandBuffer *c, float d) { (void)c; static int once; if (d != 0 && !once++) LOG("【红】 nvnCommandBufferSetConservativeRasterDilate %g: conservative rasterization is not implemented (Metal has none)", d); }
NVN(nvnCommandBufferSetSubpixelPrecisionBias, void, NVNcommandBuffer *c, int x, int y) { (void)c; static int once; if (!once++ && (x || y)) LOG("【红】 nvnCommandBufferSetSubpixelPrecisionBias %d,%d: subpixel precision bias is not implemented (Metal has none)", x, y); }
NVN(nvnCommandBufferBindCoverageModulationTable, void, NVNcommandBuffer *c, const float *t) { (void)c; (void)t; static int once; if (!once++) LOG("【红】 nvnCommandBufferBindCoverageModulationTable: coverage modulation (mixed samples) is not implemented (Metal has none)"); }
NVN(nvnCommandBufferSetInnerTessellationLevels, void, NVNcommandBuffer *c, const float *l) { (void)c; (void)l; static int once; if (!once++) LOG("【红】 nvnCommandBufferSetInnerTessellationLevels: tessellation is not implemented in the replay"); }
NVN(nvnCommandBufferSetOuterTessellationLevels, void, NVNcommandBuffer *c, const float *l) { (void)c; (void)l; static int once; if (!once++) LOG("【红】 nvnCommandBufferSetOuterTessellationLevels: tessellation is not implemented in the replay"); }

// CPU texture write (NVN WriteTexels): texture storage lives in pool memory in Tegra block-linear layout; decode the whole level → patch the region → re-encode.
// Write protection is lifted and the range marked dirty first, so the replay layer re-uploads it the next time it is used
// Non-3D textures: only the bytes inside the region are written, by block-linear address, without decoding the whole level. Metroid Dread's minimap (main+0x160e80) calls WriteTexels once per texel (a 1×1 region);
// previously every call decoded, patched and re-encoded the whole level, and after a few minutes the main thread spent whole frames there (frames stopped from 175 s). Returns −1 when the region is out of range or the format unsupported, falling back to the whole-level path
static int write_region_bl(const tex_t *t, unsigned level, unsigned layer, const NVNcopyRegion *r, const uint8_t *src, size_t row, int bw, int bh, int bpb) {
    uint32_t row_bytes, rows, slices, gbpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &gbpb) || (int)gbpb != bpb || r->xoffset < 0 || r->yoffset < 0) return -1;
    uint32_t x0 = (uint32_t)(r->xoffset / bw) * (uint32_t)bpb, y0 = (uint32_t)(r->yoffset / bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh);
    if (x0 + wb > row_bytes || y0 + hb > rows) return -1;
    uint32_t lb = nvn_tex_level_gobs(&t->d, level) * 64; if (lb < row_bytes) lb = row_bytes;   // the row as laid out (padded in a sparse texture's tiled levels)
    for (size_t y = 0; y < hb; y++)
        for (size_t x = 0; x < wb;) {   // 16-byte runs are contiguous within a GOB
            uint32_t bx = x0 + (uint32_t)x; size_t n = 16 - bx % 16; if (n > wb - x) n = wb - x;
            memcpy(t->mem + base + nvn_bl_offset(bx, y0 + (uint32_t)y, 0, lb, rows, th, td), src + y * row + x, n); x += n;
        }
    return 0;
}
static void write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (dm_gfx_session_stopping()) return;
    if (!t->mem) { LOG("【红】 WriteTexels: texture has no storage"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { ONCE_LOG("【红】 WriteTexels: unknown format %#x", t->d.format); return; }
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    if (!is3d) {
        size_t wb0 = (size_t)((r->width + bw - 1) / bw) * bpb, row0 = row ? row : wb0, img0 = img ? img : row0 * (size_t)((r->height + bh - 1) / bh);
        dm_guest_write_prepare(t->mem, t->L.size);
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = !write_region_bl(t, level, layer0 + (unsigned)(r->zoffset + z), r, src + (size_t)z * img0, row0, bw, bh, bpb);
        if (ok) { nvn_be_tex_cpu_written(t); return; }
    }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    uint8_t *lin = malloc(lp * lr * lz); if (!lin) die("WriteTexels allocation failed");
    dm_guest_write_prepare(t->mem, t->L.size);
    // when the region is exactly the whole level (2D) every texel is overwritten, so the old content need not be decoded first (movies write whole frames; saves one deswizzle per 1080p frame)
    int whole = !is3d && !r->xoffset && !r->yoffset && (uint32_t)r->width == lw && (uint32_t)r->height == lh;
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)(r->zoffset + z);
        if (((is3d && z == 0) || !is3d) && !whole) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { ONCE_LOG("【红】 WriteTexels deswizzle failed (format %#x target %d)", t->d.format, t->d.target); free(lin); return; }
        uint8_t *dst = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0) + (size_t)(r->yoffset / bh) * lp + (size_t)(r->xoffset / bw) * bpb;
        for (size_t y = 0; y < hb; y++) memcpy(dst + y * lp, src + (size_t)z * img + y * row, wb);
        if (!is3d || z == nz - 1) nvn_swizzle(&t->d, level, layer, lin, lp, t->mem);
    }
    free(lin);
    nvn_be_tex_cpu_written(t);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) { write_texels(CTX(t), (const view_t *)v, r, src, 0, 0); }
// with the source's row and image strides in bytes (0 = tightly packed, as WriteTexels)
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    write_texels(CTX(t), (const view_t *)v, r, src, (size_t)row, (size_t)img);
}
// The movie decoder's native-texture output (DecoderMode 2, upstream the reference port host/hle_movie.c, which S2Arm does not carry: nothing here calls it, S2Arm decodes movies in Ryujinx Nvdec): a whole frame into level 0 of the client texture, as WriteTexels does; returns the texture format, or −1 when the object is not a texture
// work the host finished on the CPU: the sync object counts as signalled at once (fence 0 is always complete)
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }   // unified memory + dirty page tracking: nothing to flush

int nvn_be_request_stop(void);
int nvn_be_shutdown_session(void);
int nvn_be_drain_session(void);
void nvn_be_begin_session(void);
void nvn_dirty_reset_session(void);
int dm_nvn_request_stop(void) {
    atomic_store(&g_session_stopping, 1);
    pthread_mutex_lock(&g_susp_mu); g_susp = 0;
    pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu);
    return nvn_be_request_stop();
}
static void pk_free_session(const void *key, const void *value, void *ctx) { (void)key; (void)ctx; free((void *)value); }
int dm_nvn_shutdown_session(void) {
    if (!dm_gfx_session_stopping()) return -EBUSY;
    pthread_mutex_lock(&g_susp_mu); int busy = g_inflight_api; pthread_mutex_unlock(&g_susp_mu);
    if (busy) { LOG("【红】重启：仍有 %d 个直连图形调用", busy); return -EBUSY; }
    // Virtual mappings own Metal pool buffers; finalize them before clearing
    // the backend's remaining resource owners and before native remaps memory.
    int e = nvn_be_drain_session(); if (e) return e;
    for (;;) { vp_lock(); const pool_t *vp = g_nvpa ? g_vpa[g_nvpa - 1]->pool : NULL; vp_unlock(); if (!vp) break; vpool_release(vp); }
    e = nvn_be_shutdown_session(); if (e) return e;
    if (g_pk) { CFDictionaryApplyFunction(g_pk, pk_free_session, NULL); CFRelease(g_pk); g_pk = NULL; }
    sh_shutdown();
    free(g_ft); g_ft = NULL; g_nft = g_cft = 0;
    pthread_mutex_lock(&g_snap_rec_mu); nvn_hm_clear(&g_snap_rec_owner, NULL); pthread_mutex_unlock(&g_snap_rec_mu);   // 即时存档：录制归属指向上一局的游戏对象
    free(g_tpb); free(g_spb); g_tpb = g_spb = NULL;   // 池表按需增长（#1209），整表释放，下个会话从初始容量重来
    g_cap_tpb = g_cap_spb = g_ntpb = g_nspb = g_tnext = g_snext = 0;
    g_frame = 0; g_hz = -1; g_display_mode = 0; g_game_itv = 1;
    // 当前设备指向上一局游戏内存里的设备结构：那片内存随会话撤掉，新局 nvnDeviceInitialize 读它的标记就 SIGBUS（11-06 索尼克×夏特切到序号 1，
    //   h_nvnDeviceInitialize+0x44 读 g_cur_dev 0x75c61bf928）。同程序进程内重启同理，新局重新登记
    atomic_store(&g_cur_dev, NULL);
    memset(&g_fs, 0, sizeof g_fs); memset(&g_fg, 0, sizeof g_fg);
    g_fs_k = g_itv2_since = g_fps_game_itv = 0;
    nvn_dirty_reset_session();
    return 0;
}
void dm_nvn_begin_session(void) {
    nvn_be_begin_session();
    atomic_store(&g_session_stopping, 0);
}

// ---------- 即时存档：hle 层的宿主状态存取（逐项理由见报告 15 节） ----------
//   描述符池基址表、帧号、上屏裁切、CPU 后填贴图的序号、命令影子（每条录制、三张句柄表、块账、空闲链表头）。
//   命令记录区里的块已按原地址放回，块指针 / 设备地址 / 句柄在新进程里同址有效；录制对象（srec）在宿主堆上，按存下的字段重建并重新建索引，录制中的改写所属 cmd_t.rec
static int hs_w(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }
static int hs_r(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n ? 0 : -1; }
static int hs_hm_save(FILE *f, nvn_hmap *m) {
    uint32_t n = 0; for (uint32_t i = 0; i < m->cap; i++) n += m->vals[i] != NULL;
    int e = hs_w(f, &n, 4);
    for (uint32_t i = 0; i < m->cap; i++) if (m->vals[i]) { uint64_t k, v = (uint64_t)(uintptr_t)m->vals[i]; memcpy(&k, m->keys + (size_t)i * 8, 8); e |= hs_w(f, &k, 8) | hs_w(f, &v, 8); }
    return e;
}
static int hs_hm_load(FILE *f, nvn_hmap *m) {
    uint32_t n; if (hs_r(f, &n, 4)) return -1;
    for (uint32_t i = 0; i < n; i++) { uint64_t k, v; if (hs_r(f, &k, 8) || hs_r(f, &v, 8)) return -1; nvn_hm_put(m, &k, (void *)(uintptr_t)v); }
    return (int)n;
}
int hle_snap_save(FILE *f) {
    int e = 0; uint32_t mg = 0x31534C48u;   // "HLS1"
    e |= hs_w(f, &mg, 4);
    pthread_mutex_lock(&g_dpool_mu);
    e |= hs_w(f, &g_ntpb, 4) | hs_w(f, g_tpb, sizeof *g_tpb * (size_t)g_ntpb) | hs_w(f, &g_nspb, 4) | hs_w(f, g_spb, sizeof *g_spb * (size_t)g_nspb) | hs_w(f, &g_tnext, 4) | hs_w(f, &g_snext, 4);
    pthread_mutex_unlock(&g_dpool_mu);
    e |= hs_w(f, &g_frame, 8);
    pthread_mutex_lock(&g_vi_mu); e |= hs_w(f, g_vi_crop, sizeof g_vi_crop); pthread_mutex_unlock(&g_vi_mu);
    pthread_mutex_lock(&g_ft_mu); uint64_t nft = g_nft; e |= hs_w(f, &nft, 8); for (size_t i = 0; i < g_nft; i++) e |= hs_w(f, &g_ft[i].t.serial, 8); pthread_mutex_unlock(&g_ft_mu);
    pthread_mutex_lock(&g_sh_mu);
    e |= hs_w(f, g_sh_free, sizeof g_sh_free) | hs_w(f, &g_sh_nchunk, 4) | hs_w(f, g_sh_chunk, sizeof *g_sh_chunk * (size_t)g_sh_nchunk) | hs_w(f, g_sh_carving, sizeof g_sh_carving);
    uint32_t nr = 0, nopen = 0; for (srec *r = g_sh_all.next; r != &g_sh_all; r = r->next) { nr++; nopen += r->open; }
    e |= hs_w(f, &nr, 4);
    for (srec *r = g_sh_all.next; r != &g_sh_all; r = r->next) {
        uint64_t q[11] = {(uint64_t)(uintptr_t)r->blk, (uint64_t)(uintptr_t)r->last, r->niv, r->open, r->glo, r->bytes, r->rbytes, r->frame, (uint64_t)(uintptr_t)r->begin, r->dev0, 0};
        if (r->open) { pthread_mutex_lock(&g_snap_rec_mu); q[10] = (uint64_t)(uintptr_t)nvn_hm_get(&g_snap_rec_owner, &r); pthread_mutex_unlock(&g_snap_rec_mu); }
        e |= hs_w(f, q, sizeof q) | hs_w(f, r->iv, sizeof *r->iv * r->niv);
    }
    e |= hs_hm_save(f, &g_shh) | hs_hm_save(f, &g_shd) | hs_hm_save(f, &g_shl);
    pthread_mutex_unlock(&g_sh_mu);
    LOG("即时存档：hle 层 贴图池 %d / 采样器池 %d、帧号 %llu、CPU 后填贴图 %llu、录制 %u 条（录制中 %u）、句柄表 %u/%u/%u", g_ntpb, g_nspb, (unsigned long long)g_frame,
        (unsigned long long)nft, nr, nopen, g_shh.n, g_shd.n, g_shl.n);
    return e;
}
static uint64_t *g_snap_ft_ser; static uint64_t g_snap_nft;   // 恢复时：CPU 后填贴图的序号（nvn_snap.inc 按地址表找到对象后再登记）
int hle_snap_restore(FILE *f) {
    uint32_t mg; if (hs_r(f, &mg, 4) || mg != 0x31534C48u) return -1;
    hle_snap_win_clock_restored();
    pthread_mutex_lock(&g_dpool_mu);
    int nt, ns; int e = hs_r(f, &nt, 4);
    while (!e && g_cap_tpb < nt) e |= dpool_grow(&g_tpb, &g_cap_tpb) ? -1 : 0;
    e |= e ? -1 : hs_r(f, g_tpb, sizeof *g_tpb * (size_t)nt); g_ntpb = nt;
    e |= hs_r(f, &ns, 4);
    while (!e && g_cap_spb < ns) e |= dpool_grow(&g_spb, &g_cap_spb) ? -1 : 0;
    e |= e ? -1 : hs_r(f, g_spb, sizeof *g_spb * (size_t)ns); g_nspb = ns;
    e |= hs_r(f, &g_tnext, 4) | hs_r(f, &g_snext, 4);
    pthread_mutex_unlock(&g_dpool_mu);
    if (e) return -1;
    e |= hs_r(f, &g_frame, 8);
    pthread_mutex_lock(&g_vi_mu); e |= hs_r(f, g_vi_crop, sizeof g_vi_crop); pthread_mutex_unlock(&g_vi_mu);
    e |= hs_r(f, &g_snap_nft, 8); g_snap_ft_ser = malloc(8 * (g_snap_nft ? g_snap_nft : 1)); e |= hs_r(f, g_snap_ft_ser, 8 * g_snap_nft);
    if (e) return -1;
    pthread_mutex_lock(&g_sh_mu);
    int nch; e |= hs_r(f, g_sh_free, sizeof g_sh_free) | hs_r(f, &nch, 4);
    if (!e && nch > g_sh_cchunk) { g_sh_chunk = realloc(g_sh_chunk, sizeof *g_sh_chunk * (size_t)nch); g_sh_cchunk = nch; }
    e |= e ? -1 : hs_r(f, g_sh_chunk, sizeof *g_sh_chunk * (size_t)nch); g_sh_nchunk = nch;
    e |= hs_r(f, g_sh_carving, sizeof g_sh_carving);
    // 快照只导出每块已切出的部分（s2a_hle_cmdshadow_list，按页取整）；块的其余部分 B 里还没映射，接着切会落空：按块补映射尾部（只映射未导出的那段，不碰写回的内容）
    int ntail = 0;
    for (int i = 0; !e && i < g_sh_nchunk; i++) {
        size_t pg = (size_t)getpagesize(), done = g_sh_chunk[i].carved ? (((size_t)g_sh_chunk[i].carved << (SH_CLS0 + g_sh_chunk[i].cls)) + pg - 1) & ~(pg - 1) : 0;
        if (done >= SH_CHUNK) continue;
        uint8_t *t = g_sh_chunk[i].mem + done;
        if (mmap(t, SH_CHUNK - done, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != (void *)t) { LOG("【红】即时存档恢复：命令块 %p 尾部 +%#zx 映射失败 errno %d", (void *)t, SH_CHUNK - done, errno); e = -1; }
        else ntail++;
    }
    if (ntail) LOG("即时存档恢复：命令块补映射未导出的尾部 %d 块", ntail);
    uint32_t nr = 0, nopen = 0, nidx = 0; e |= hs_r(f, &nr, 4);
    for (uint32_t k = 0; k < nr && !e; k++) {
        uint64_t q[11]; if (hs_r(f, q, sizeof q)) { e = -1; break; }
        srec *r = calloc(1, sizeof *r); if (!r) die("即时存档恢复：录制对象内存不足");
        r->blk = (sblk *)(uintptr_t)q[0]; r->last = (sblk *)(uintptr_t)q[1]; r->niv = (uint32_t)q[2]; r->open = (uint32_t)q[3]; r->glo = (uintptr_t)q[4];
        r->bytes = q[5]; r->rbytes = q[6]; r->frame = q[7]; r->begin = (uint8_t *)(uintptr_t)q[8]; r->dev0 = (uintptr_t)q[9]; void *owner = (void *)(uintptr_t)q[10];
        if (r->niv <= 2) { r->iv = r->ivs; r->civ = 2; } else { r->iv = malloc(sizeof *r->iv * r->niv); r->civ = r->niv; }
        if (hs_r(f, r->iv, sizeof *r->iv * r->niv)) { e = -1; break; }
        r->prev = g_sh_all.prev; r->next = &g_sh_all; g_sh_all.prev->next = r; g_sh_all.prev = r; g_sh_nlive++; g_sh_live_bytes += r->bytes;
        if (r->open) { if (owner) { ((cmd_t *)owner)->rec = r;
                if (hle_snap_saving()) { pthread_mutex_lock(&g_snap_rec_mu); nvn_hm_put(&g_snap_rec_owner, &r, owner); pthread_mutex_unlock(&g_snap_rec_mu); } }   // 恢复局里再存时要能找到归属
            nopen++; }   // 录制中：游戏命令缓冲里存的是 A 的宿主指针，改成新对象
        else if (r->niv) { sh_index(r); nidx++; }
    }
    int a = e ? -1 : hs_hm_load(f, &g_shh), b = e ? -1 : hs_hm_load(f, &g_shd), c = e ? -1 : hs_hm_load(f, &g_shl);
    pthread_mutex_unlock(&g_sh_mu);
    LOG("%s即时存档恢复：hle 层 贴图池 %d / 采样器池 %d、帧号 %llu、录制 %u 条（录制中 %u、重建索引 %u）、句柄表 %d/%d/%d", e || a < 0 || b < 0 || c < 0 ? "【红】" : "",
        nt, ns, (unsigned long long)g_frame, nr, nopen, nidx, a, b, c);
    return e || a < 0 || b < 0 || c < 0 ? -1 : 0;
}
// 恢复：CPU 后填贴图按序号找到游戏里的贴图对象重新登记（lookup 由 nvn_snap.inc 给：序号 → 贴图对象地址）
int hle_snap_ft_restore(const void *(*lookup)(uint64_t ser)) {
    int n = 0, miss = 0;
    for (uint64_t i = 0; i < g_snap_nft; i++) { const tex_t *t = lookup(g_snap_ft_ser[i]); if (t && t->serial == g_snap_ft_ser[i]) { ft_add(t); n++; } else miss++; }
    free(g_snap_ft_ser); g_snap_ft_ser = NULL; g_snap_nft = 0;
    LOG("%s即时存档恢复：CPU 后填贴图重新登记 %d 个（找不到 %d）", miss ? "【黄】" : "", n, miss); return n;
}

// 即时存档：游戏内存里放着宿主定义的 NVN 对象（nvn_obj.h，游戏分配的对象内存里直接是这些结构）+ 本文件按整块存的表：布局指纹。
//   首字段是 magic 的对象带上 magic 值：换版本布局变了时恢复处按它在游戏内存里找到对象原地转（runtime/src/snapshot.c conv_guest_objs）
void hle_snap_fps(snap_fp_cb cb) {
    cb("nvn.tex", SNAP_FP_OBJ(tex_t, MAGIC('T', 'E', 'X', 0))); cb("nvn.cmd", SNAP_FP_OBJ(cmd_t, MAGIC('C', 'M', 'D', 'B'))); cb("nvn.sync", SNAP_FP_OBJ(sync_t, MAGIC('S', 'Y', 'N', 'C'))); cb("nvn.pool", SNAP_FP_OBJ(pool_t, MAGIC('P', 'O', 'O', 'L')));
    cb("nvn.prog", SNAP_FP_OBJ(prog_t, MAGIC('P', 'R', 'O', 'G'))); cb("nvn.samp", SNAP_FP_OBJ(samp_t, MAGIC('S', 'M', 'P', 0))); cb("nvn.view", SNAP_FP_TYPE(view_t)); cb("nvn.win", SNAP_FP_OBJ(win_t, MAGIC('W', 'I', 'N', 'D')));
    cb("nvn.queue", SNAP_FP_OBJ(queue_t, MAGIC('Q', 'U', 'E', 'U'))); cb("nvn.dev", SNAP_FP_OBJ(dev_t_, MAGIC('D', 'E', 'V', 0))); cb("nvn.ev", SNAP_FP_OBJ(ev_t, MAGIC('E', 'V', 'N', 'T'))); cb("nvn.dpool", SNAP_FP_TYPE(dpool_t));
    cb("hle.dpool_base", SNAP_FP_TYPE(struct dpool_base)); cb("hle.schunk", SNAP_FP_TYPE(schunk)); cb("hle.sblk", SNAP_FP_TYPE(sblk));
}
