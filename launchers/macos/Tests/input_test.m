#include <stdarg.h>
// Tests the actual Mac input adapter, including atomic cross-thread snapshots.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "../../../macos/ns2_relay.h"
#include "../../../macos/xbox_gip_relay.h"
#include "../../../ios/input_config.h"
#import "../../../macos/front_mac.m"
#include "../../../macos/imu_burst.h"
#import "../../../macos/input_guide.m"
#import "../../../macos/controller_view.m"

static BOOL guide_preview;
void s2a_host_shutdown(const char *why) { (void)why; if (guide_preview) exit(0); abort(); }
void s2a_clock_catchup(void) {}   // 游戏日历补时（runtime/src/os.c，Mac 唤醒通知调）在这里不测
void s2a_heartbeat_mac(void) {}   // Mac 心跳（macos/heartbeat_mac.c，前端 10 秒定时器调）在这里不测
int s2a_playtime_flush(void) { return 0; }   // 游玩时间落盘（runtime/src/playtime.c，同一个 10 秒定时器调）在这里不测
int s2a_caps_fps_fast(void) { return 0; }   // runtime/src/caps.c：同局 A/B 红的款压回时不给再试（11-24 #2201），这里不测
void s2a_ql_retry60(void) {}   // 档位梯子「再试 60 帧」（runtime/src/qladder_host.c，前端横幅按钮调）在这里不测
// 即时快照（runtime/src/snapsw.c、snapshot.c）在这里不测：开关关着，菜单不出这两项
int s2a_snap_logging(void) { return 0; }
int s2a_snap_dir_for(const char *gid, char *out, size_t n) { (void)gid; (void)out; (void)n; return -1; }
int s2a_snap_exists(const char *dir) { (void)dir; return 0; }
int s2a_snap_fits_inplace(const char *dir) { (void)dir; return -1; }   // 快照落点预检（runtime/src/snapshot.c）：本单测不恢复
int s2a_snap_relaunch(const char *dir, int rollback) { (void)dir; (void)rollback; return -1; }   // Mac 原地重开（runtime/src/snaprelaunch.c）
int s2a_snap_save_changed(const char *dir) { (void)dir; return -1; }
int s2a_snap_request_save(const char *dir) { (void)dir; return -1; }
void s2a_snap_on_done(void (*cb)(int, const char *)) { (void)cb; }
int s2a_host_restart(void (*done)(int, const char *, void *), void *ctx) { (void)done; (void)ctx; return -1; }   // 进程内重启（runtime/src/session.c）在这里不测
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // 宿主单调时钟（runtime/src/os.c）：前台焦点计时用（#965 起 front_mac.m 引用），给真时钟
// 生命周期桩（接口 v2.15，缺口扫描族 1）：记下前端报的焦点 / 睡眠、客机冻结与时钟原因 3 的开关次数
static int stub_focus = 1, stub_suspend, stub_sleep, stub_freeze_on, stub_freeze_off, stub_clock3_on, stub_clock3_off;
void s2a_host_focus_set(int focused) { stub_focus = focused; }
int s2a_focus_suspend(void) { return stub_suspend; }
void s2a_host_sleep_set(int sleeping) { stub_sleep = sleeping; }
void s2a_guest_freeze(int on) { if (on) stub_freeze_on++; else stub_freeze_off++; }
void guest_clock_freeze_for(int reason, int on) { if (reason == 3) { if (on) stub_clock3_on++; else stub_clock3_off++; } }
static int width, height;
static int gfx_mode = S2A_GFX_GENERIC, layer_calls;
static void *notified_layer, *published_at_notify;
int s2a_gfx_mode(void) { return gfx_mode; }
void nxgal_window_set_layer(void *layer) {
    layer_calls++; notified_layer = layer; published_at_notify = s2a_mac_front_layer();
}
void nxgal_window_set_size(int32_t w, int32_t h) { width = w; height = h; }
static int fit_w, fit_h;
int s2a_screen_fit(int pw, int ph) { fit_w = pw; fit_h = ph; return 0; }   // 宽屏跟窗口比例（runtime/src/widen.c）：只记前端交出的像素尺寸
int s2a_hud_on(void) { return 0; }   // 开发浮层（runtime/src/hud.c）在这里不测，关着
int s2a_hud_text(char *out, size_t n, const s2a_hud_host *h) { (void)h; return snprintf(out, n, "%s", ""); }
int s2a_hud_brief(char *out, size_t n) { return snprintf(out, n, "%s", ""); }   // 帧率浮层（macos/perf_brief.m，#2207）在这里不测
int s2a_hud_brief_fps(char *out, size_t n) { return snprintf(out, n, "%s", ""); }
void s2a_hud_brief_set(int on) { (void)on; }
// 存档原子提交判定（runtime/src/direct_prompt.c s2a_save_atomic_poll，判定本身由 runtime/tests/t_direct_prompt.c 测）：这里只给读数，测 Mac 横幅接线
static int fake_atomic;
int s2a_save_atomic_poll(void) { int bits = fake_atomic; fake_atomic = 0; return bits; }

// 鼠标左键当触屏的画面矩形（front_mac.m touch_picture）：渲染层这一帧 / 宽屏状态由单测给
static int fake_frame_w, fake_frame_h, fake_widen_game, fake_widen_full, fake_screen_w = 1920, fake_screen_h = 1080;
int nxgal_present_frame(int32_t *w, int32_t *h, float *ax, float *ay, int32_t *stretched) {
    *w = fake_frame_w; *h = fake_frame_h; *ax = 16; *ay = 9; *stretched = 0; return fake_frame_w > 0;
}
int s2a_widen_game(void) { return fake_widen_game; }
int s2a_widen_full(void) { return fake_widen_full; }
int s2a_screen_w(void) { return fake_screen_w; }
int s2a_screen_h(void) { return fake_screen_h; }
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); } }
static uint64_t g_fake_mouse_ns;   // #2131 鼠标时间窗的假时钟
static uint64_t fake_mouse_now(void) { return g_fake_mouse_ns; }
static void expectf(int ok, const char *fmt, ...) { if (!ok) { va_list a; va_start(a, fmt); fputs("FAIL: ", stderr); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a); exit(1); } }
static NSEvent *key_event(NSEventType type, unsigned short code, BOOL repeat) {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0
        windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:repeat keyCode:code];
}
static BOOL physical_keys[128];
static void poll_stale(void) { for (int n = 0; n < KEY_UP_POLLS; n++) release_stale_keys(); sample_input(); }   // 采样连续报松开满 KEY_UP_POLLS 次才删键
// 排空主队列（NS2 输入事件化验证）：主队列块由主线程 runloop 跑，尾部再排一个标记块，标记跑到 = 之前入队的全跑完
static void drain_main_queue(void) {
    __block int drained = 0;
    dispatch_async(dispatch_get_main_queue(), ^{ drained = 1; });
    for (int i = 0; i < 200 && !drained; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    expect(drained, "main queue drains on the main runloop");
}
static bool key_state(CGEventSourceStateID state, CGKeyCode code) {
    expect(state == kCGEventSourceStateCombinedSessionState, "query current login session");
    return code < 128 && physical_keys[code];
}

static void test_key_repeat(void) {
    config = default_config(); reset_input(); focused = YES;
    read_key_state = key_state;
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSZeroRect];
    unsigned short arrows[] = {123, 126, 124, 125};
    for (int i = 0; i < 4; i++) {
        int32_t sticks[4]; uint32_t bit = 1u << (12 + i);
        physical_keys[arrows[i]] = YES;
        [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], NO)];
        for (int n = 0; n < 1200; n++) [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], YES)];
        expect(s2a_vpad_read(sticks) == bit, "long hold retains direction");
        [view keyUp:key_event(NSEventTypeKeyUp, arrows[i], NO)];
        expect(s2a_vpad_read(sticks) == 0, "release clears repeated direction");
        [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], YES)];
        expect(s2a_vpad_read(sticks) == 0, "late repeat cannot restore released direction");

        physical_keys[arrows[i]] = YES;
        [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], NO)];
        for (int n = 0; n < 1200; n++) { release_stale_keys(); sample_input(); }
        expect(s2a_vpad_read(sticks) == bit, "polling preserves a long physical hold");
        physical_keys[arrows[i]] = NO; // 故意不发送 keyUp，验证下一次采样能恢复
        poll_stale();
        expect(s2a_vpad_read(sticks) == 0, "polling recovers missing arrow release");
    }
    int32_t sticks[4];
    physical_keys[14] = physical_keys[13] = YES;
    [view keyDown:key_event(NSEventTypeKeyDown, 14, NO)];
    [view keyDown:key_event(NSEventTypeKeyDown, 13, NO)];
    physical_keys[13] = NO;
    poll_stale();
    expect(s2a_vpad_read(sticks) == 1 && sticks[1] == 0, "releasing stick direction preserves another held button");
    physical_keys[14] = NO;
    poll_stale();
    expect(s2a_vpad_read(sticks) == 0, "polling recovers missing ordinary key release");

    [keys addObject:@56]; modifiers = 1; physical_keys[60] = YES;
    poll_stale();
    expect(s2a_vpad_read(sticks) == 2 && modifiers == 1, "right shift preserves canonical modifier");
    physical_keys[60] = NO;
    poll_stale();
    expect(s2a_vpad_read(sticks) == 0 && modifiers == 0, "missing modifier release clears chords");

    physical_keys[123] = YES;
    S2AMacDelegate *delegate = [S2AMacDelegate new];
    [view keyDown:key_event(NSEventTypeKeyDown, 123, NO)];
    [delegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification object:nil]];
    [view keyDown:key_event(NSEventTypeKeyDown, 123, NO)];
    [view keyDown:key_event(NSEventTypeKeyDown, 123, YES)];
    expect(s2a_vpad_read(sticks) == 0 && keys.count == 0, "focus loss and queued repeat leave input neutral");
    [delegate windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:nil]];
    [view keyUp:key_event(NSEventTypeKeyUp, 123, NO)];
    [view keyDown:key_event(NSEventTypeKeyDown, 123, YES)];
    expect(s2a_vpad_read(sticks) == 0, "unfocused press then focused release and late repeat remain neutral");
    [view keyDown:key_event(NSEventTypeKeyDown, 123, NO)];
    [view keyUp:key_event(NSEventTypeKeyUp, 123, NO)];
    expect(s2a_vpad_read(sticks) == 0, "fresh press and release still work after focus returns");
    physical_keys[123] = NO;
    poll_input(); // 同时编译、链接并执行生产定时采样入口
    puts("Mac keyboard: four arrows x 1200 repeats and polls, lost keyUp, chords and focus passed");
}
// 审计 P2-13 界面半边：判定（只提示一次）挪到 runtime/src/direct_prompt.c 与 iOS 共用（P2-45）；这里测生产 pollPrompt 拿到非 0 就亮横幅、为 0 不亮。期望手算
static void test_save_atomic_warning(void) {
    S2AMacDelegate *delegate = [S2AMacDelegate new]; S2APromptBanner *banner = [S2APromptBanner new]; delegate.banner = banner;
    fake_atomic = 0; [delegate pollPrompt]; expect(banner.hidden, "atomic saves do not warn");
    fake_atomic = 2; [delegate pollPrompt]; expect(!banner.hidden, "non-atomic save volume warns");
    puts("Mac save safety: non-atomic save volume shows banner passed");
}
// 审计 P1-59：服务层报局域网发现起不来（S2A_LanState 非 0）时，Mac 窗口给玩家一句说明，只提示一次；为 0 不提示。期望手算
static int fake_lan;
static int fake_lan_state(void) { return fake_lan; }
static void test_lan_warning(void) {
    lan_state = fake_lan_state; lan_warned = NO;
    fake_lan = 0; expect(lan_warning() == nil, "LAN available does not warn");
    fake_lan = 1; NSString *missing = lan_warning();
    expect([missing containsString:@"这台 Mac"] && [missing containsString:@"本地联机"], "missing discovery explains this Mac cannot find other devices");
    expect(lan_warning() == nil, "LAN warning is shown only once");
    lan_warned = NO; fake_lan = 2; NSString *failed = lan_warning();
    expect([failed containsString:@"没能启动"], "start failure says discovery failed to start");
    lan_warned = NO; fake_lan = 3;
    expect([lan_warning() isEqualToString:missing], "missing discovery wins over a later start failure");
    puts("Mac LAN: unavailable discovery warns the player once passed");
}
// 缺口扫描族 1 / 审计 P2-64：Mac 失焦按游戏的失焦暂停开关冻结客机、回焦点放开；收尾路径只报失焦不冻结。期望手算（计数）
static void test_focus_suspend(void) {
    stub_freeze_on = stub_freeze_off = stub_clock3_on = stub_clock3_off = 0; focus_frozen = NO;
    stub_suspend = 1;
    display_focus_publish(NO);
    expect(stub_focus == 0 && stub_freeze_on == 1 && stub_clock3_on == 1, "focus loss with suspend requested freezes guest and clock");
    display_focus_publish(NO);
    expect(stub_freeze_on == 1, "repeated focus loss does not freeze twice");
    display_focus_publish(YES);
    expect(stub_focus == 1 && stub_freeze_off == 1 && stub_clock3_off == 1, "focus regained releases guest and clock");
    stub_suspend = 0;
    display_focus_publish(NO); display_focus_publish(YES);
    expect(stub_freeze_on == 1 && stub_freeze_off == 1 && stub_focus == 1, "game that disabled suspend keeps running when unfocused");
    stub_suspend = 1;
    display_focus_publish_ex(NO, NO);
    expect(stub_focus == 0 && stub_freeze_on == 1, "shutdown publishes focus loss without freezing");
    display_focus_publish(NO); display_focus_publish_ex(NO, NO);
    expect(stub_freeze_on == 2 && stub_freeze_off == 2 && stub_clock3_off == 2, "shutdown releases a focus freeze");
    int saved_pause_off = g_focus_pause_off;
    g_focus_pause_off = 1; stub_suspend = 1;
    display_focus_publish(NO);
    expect(stub_focus == 1 && stub_freeze_on == 2 && stub_clock3_on == 2 && dm_focus_now() == 0,
           "pause-off keeps guest focus active and skips freeze while tracking display focus");
    display_focus_publish_ex(NO, NO);
    expect(stub_focus == 0 && stub_freeze_on == 2,
           "pause-off shutdown still publishes focus loss without freezing");
    g_focus_pause_off = saved_pause_off;
    stub_suspend = 0; display_focus_publish(YES);
    puts("Mac lifecycle: focus loss freezes only when the game asks, regains and shutdown release passed");
}
static void test_untrusted_key_state(void) {   // 审计 P1-57：系统状态误报「已松开」时不丢按住的键，也不永久停掉核对
    config = default_config(); reset_input(); focused = YES;
    read_key_state = key_state;
    memset(physical_keys, 0, sizeof physical_keys);
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSZeroRect];
    unsigned short arrows[] = {123, 126, 124, 125};
    for (int i = 0; i < 4; i++) {
        int32_t sticks[4]; uint32_t bit = 1u << (12 + i);
        [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], NO)];   // 系统一直误报松开
        for (int n = 0; n < 1200; n++) { [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], YES)]; poll_input(); }
        expect(s2a_vpad_read(sticks) == bit, "repeats keep a hold through a false released state");
        // 循环最后一下是「重复（计数清零）→ 采样（计数 1）」，再采 KEY_UP_POLLS - 2 次到 KEY_UP_POLLS - 1，差一次不删
        for (int n = 0; n < KEY_UP_POLLS - 2; n++) poll_input();
        expect(s2a_vpad_read(sticks) == bit, "one poll short of the debounce keeps the hold");
        poll_input();
        expect(s2a_vpad_read(sticks) == 0, "sustained released state without repeats drops the key");
        [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], YES)];
        expect(s2a_vpad_read(sticks) == bit, "a later repeat restores a key dropped by polling");
        [view keyUp:key_event(NSEventTypeKeyUp, arrows[i], NO)];
        expect(s2a_vpad_read(sticks) == 0, "keyUp clears the restored key");
        [view keyDown:key_event(NSEventTypeKeyDown, arrows[i], YES)];
        expect(s2a_vpad_read(sticks) == 0, "a late repeat after keyUp stays released");
    }
    // 核对不会被永久关掉：误报之后，真正丢了 keyUp 的键仍按采样收回
    int32_t sticks[4]; physical_keys[14] = YES;
    [view keyDown:key_event(NSEventTypeKeyDown, 14, NO)];
    physical_keys[14] = NO; poll_stale();
    expect(s2a_vpad_read(sticks) == 0, "polling still recovers a lost keyUp after earlier mismatches");
    reset_input();
    puts("Mac keyboard: false released state keeps holds, repeat restores, polling stays on passed");
}
// 审计 P1-34：修饰键只走 flagsChanged、没有自动重复可恢复；系统按键重复关掉时普通键同理。
//   采样连续误报松开满防抖被删后，系统恢复报按下就加回（含修饰位）；真正的松开照旧清；本视图没收到过的按下不从系统状态新增。期望手算（缺省映射 Shift=B 位 1、Control=LS 位 4、←=LEFT 位 12）
static NSEvent *flags_event(unsigned short code, NSEventModifierFlags flags) {
    return [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:flags timestamp:0
        windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:code];
}
static void test_modifier_false_release(void) {
    config = default_config(); reset_input(); focused = YES;
    read_key_state = key_state;
    memset(physical_keys, 0, sizeof physical_keys);
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSZeroRect];
    int32_t sticks[4];
    physical_keys[56] = YES;
    [view flagsChanged:flags_event(56, NSEventModifierFlagShift)];
    expect(s2a_vpad_read(sticks) == (1u << 1) && modifiers == 1, "shift press maps to B");
    physical_keys[56] = NO; poll_stale();
    expect(s2a_vpad_read(sticks) == 0 && modifiers == 0, "sustained false release drops shift");
    physical_keys[56] = YES; poll_input();
    expect(s2a_vpad_read(sticks) == (1u << 1) && modifiers == 1, "system reporting shift held again restores it");
    physical_keys[56] = NO; [view flagsChanged:flags_event(56, 0)];
    expect(s2a_vpad_read(sticks) == 0 && modifiers == 0 && !stale_keys.count, "real flagsChanged release clears shift");
    physical_keys[56] = YES; poll_stale();
    expect(s2a_vpad_read(sticks) == 0 && modifiers == 0, "a press this view never received is not added from system state");
    physical_keys[56] = NO;

    physical_keys[62] = YES;
    [view flagsChanged:flags_event(62, NSEventModifierFlagControl)];
    expect(s2a_vpad_read(sticks) == (1u << 4) && modifiers == 2, "right control maps to canonical LS");
    physical_keys[62] = NO; poll_stale();
    expect(s2a_vpad_read(sticks) == 0 && [stale_keys containsObject:@59], "false release drops right control");
    physical_keys[62] = YES; [view flagsChanged:flags_event(62, NSEventModifierFlagControl)];
    expect(s2a_vpad_read(sticks) == (1u << 4) && ![stale_keys containsObject:@59] && !key_up_polls[@59].intValue, "fresh modifier press clears the stale record");
    physical_keys[62] = NO; [view flagsChanged:flags_event(62, 0)];
    expect(s2a_vpad_read(sticks) == 0 && !keys.count && !stale_keys.count, "right control release clears everything");

    physical_keys[123] = YES;
    [view keyDown:key_event(NSEventTypeKeyDown, 123, NO)];
    physical_keys[123] = NO; poll_stale();
    expect(s2a_vpad_read(sticks) == 0, "ordinary key dropped by a false release");
    physical_keys[123] = YES; poll_input();
    expect(s2a_vpad_read(sticks) == (1u << 12), "ordinary key restores without auto-repeat when system reports it held");
    physical_keys[123] = NO; [view keyUp:key_event(NSEventTypeKeyUp, 123, NO)];
    reset_input();
    puts("Mac keyboard: modifiers and non-repeating keys survive a false released state passed");
}
// 审计 P2-34：鼠标按下只在 mouseUp 删除，松开被别处吃掉时一直按着。现在按系统当前按下的鼠标键核对，与键盘同一套防抖与恢复。
//   期望手算（缺省映射 右键 = ZL 位 8）
static NSUInteger fake_mouse_buttons;
// NSEvent 的 mouseEventWithType 造出的右键按钮号是 0；经 CGEvent 造才带真实按钮号 1
static NSEvent *right_mouse_event(CGEventType type) {
    CGEventRef cg = CGEventCreateMouseEvent(NULL, type, CGPointZero, kCGMouseButtonRight);
    NSEvent *e = [NSEvent eventWithCGEvent:cg]; CFRelease(cg); return e;
}
static NSUInteger mouse_buttons_state(void) { return fake_mouse_buttons; }
// R5：键盘区原始键态（HID usage 位图 + 修饰键字段；服务层 InputPump 照 s2a_kb_read 这份写键盘区共享内存）。
//   键码表对拍：W=13→0x1A、F5=96→0x3E、Esc=53→0x29、小键盘Enter=76→0x58、Z=6→0x1D；修饰键 shift 56→0xE1、
//   ctrl 59→0xE0、option 58→0xE2（0xE0–0xE2 落在位图第 4 个 64 位段）+ 字段位（Shift=2、Control=1、LeftAlt=4）；
//   CapsLock 只走 flagsChanged（位图 0x39 + 字段 bit8）；失焦发全零、按住的键不丢；keyUp 清位；粘键核对删键跟着清；复位全清
static void test_keyboard_hid(void) {
    config = default_config(); reset_input(); focused = YES;
    read_key_state = key_state; memset(physical_keys, 0, sizeof physical_keys);
    uint64_t k[4]; uint32_t m;
    s2a_kb_read(k, &m);
    expect(k[0] == 0 && k[1] == 0 && k[2] == 0 && k[3] == 0 && m == 0, "keyboard bitmap starts empty");
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSZeroRect];
    [view keyDown:key_event(NSEventTypeKeyDown, 13, NO)];   // W
    s2a_kb_read(k, &m);
    expect(k[0] == (1UL << 0x1A) && !k[1] && !k[2] && !k[3] && m == 0, "W lands on its HID usage bit");
    [view keyDown:key_event(NSEventTypeKeyDown, 96, NO)];   // F5
    s2a_kb_read(k, &m);
    expect((k[0] & (1UL << 0x3E)) && (k[0] & (1UL << 0x1A)), "F5 lands beside W");
    [view keyDown:key_event(NSEventTypeKeyDown, 76, NO)];   // 小键盘 Enter
    s2a_kb_read(k, &m);
    expect((k[1] & (1UL << (0x58 - 64))) && (k[0] & (1UL << 0x3E)), "keypad Enter lands on usage 0x58");
    [view keyUp:key_event(NSEventTypeKeyUp, 13, NO)];
    s2a_kb_read(k, &m);
    expect(!(k[0] & (1UL << 0x1A)) && (k[0] & (1UL << 0x3E)), "keyUp clears only its own bit");
    [view keyDown:key_event(NSEventTypeKeyDown, 53, NO)];   // Esc（窗口非全屏、没锁鼠标：照常进键盘区）
    s2a_kb_read(k, &m);
    expect((k[0] & (1UL << 0x29)) != 0, "Esc lands on usage 0x29");   // 位号 ≥ 32 的单项判空要显式 != 0：int 截断会把高位清零
    [view flagsChanged:flags_event(56, NSEventModifierFlagShift)];
    s2a_kb_read(k, &m);
    expect((k[3] & (1UL << (0xE1 & 63))) && m == (1u << 1), "shift lands on usage 0xE1 and the Shift field bit");
    // flagsChanged 的 modifierFlags 反映全量状态（与真机一致），合成事件按累积传
    [view flagsChanged:flags_event(59, NSEventModifierFlagShift | NSEventModifierFlagControl)];
    [view flagsChanged:flags_event(58, NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption)];
    s2a_kb_read(k, &m);
    expect((k[3] & (1UL << (0xE0 & 63))) && (k[3] & (1UL << (0xE2 & 63))) && m == ((1u << 1) | 1u | (1u << 2)),
           "control and option land with their usages and field bits");
    [view flagsChanged:flags_event(56, NSEventModifierFlagControl | NSEventModifierFlagOption)];   // 松 shift：其余还在
    [view flagsChanged:flags_event(59, NSEventModifierFlagOption)];
    [view flagsChanged:flags_event(58, 0)];
    s2a_kb_read(k, &m);
    expect(!(k[3] & (7UL << 32)) && m == 0, "releasing modifiers clears bitmap and field");
    [view flagsChanged:flags_event(57, NSEventModifierFlagCapsLock)];
    s2a_kb_read(k, &m);
    expect((k[0] & (1UL << 0x39)) && m == (1u << 8), "CapsLock lands on usage 0x39 and the CapsLock field bit");
    [view flagsChanged:flags_event(57, 0)];
    s2a_kb_read(k, &m);
    expect(!(k[0] & (1UL << 0x39)) && m == 0, "CapsLock release clears both");
    focused = NO; sample_input();
    s2a_kb_read(k, &m);
    expect(!k[0] && !k[1] && !k[2] && !k[3] && m == 0, "unfocused publishes an all-zero bitmap");
    focused = YES; sample_input();
    s2a_kb_read(k, &m);
    expect((k[0] & (1UL << 0x29)) && (k[1] & (1UL << (0x58 - 64))), "refocus re-publishes held keys, like pads");
    [view keyDown:key_event(NSEventTypeKeyDown, 6, NO)];   // Z
    physical_keys[6] = YES;   // 粘键核对的 is_down 桩：Z 还按着，先不清
    poll_stale();
    s2a_kb_read(k, &m);
    expect((k[0] & (1UL << 0x1D)), "Z lands on usage 0x1D while the system still reports it held");
    physical_keys[6] = NO; poll_stale();   // 系统报松开满防抖次数 → 删键（keyUp 被别的响应者吃掉的兜底），位图跟着清
    s2a_kb_read(k, &m);
    expect(!(k[0] & (1UL << 0x1D)), "stale-key reconciliation clears the bitmap without a keyUp");
    reset_input();
    s2a_kb_read(k, &m);
    expect(!k[0] && !k[1] && !k[2] && !k[3] && m == 0, "reset clears the keyboard bitmap");
    puts("Mac keyboard: HID usage bitmap, modifiers, caps, focus and reconciliation passed");
}
static void test_mouse_lost_release(void) {
    config = default_config(); reset_input(); focused = YES;
    read_key_state = key_state; read_mouse_buttons = mouse_buttons_state;
    memset(physical_keys, 0, sizeof physical_keys);
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSZeroRect];
    NSEvent *down = right_mouse_event(kCGEventRightMouseDown), *up = right_mouse_event(kCGEventRightMouseUp);
    int32_t sticks[4];
    fake_mouse_buttons = 1u << 1;
    [view rightMouseDown:down];
    for (int n = 0; n < 1200; n++) poll_input();
    expect(s2a_vpad_read(sticks) == (1u << 8), "held right button survives polling");
    fake_mouse_buttons = 0;
    for (int n = 0; n < KEY_UP_POLLS - 1; n++) poll_input();
    expect(s2a_vpad_read(sticks) == (1u << 8), "one poll short of the debounce keeps the button");
    poll_input();
    expect(s2a_vpad_read(sticks) == 0, "lost mouseUp is released by polling");
    fake_mouse_buttons = 1u << 1; poll_input();
    expect(s2a_vpad_read(sticks) == (1u << 8), "system reporting the button held again restores it");
    fake_mouse_buttons = 0; [view rightMouseUp:up];
    expect(s2a_vpad_read(sticks) == 0 && !stale_mouse.count && !mouse_up_polls.count, "real mouseUp clears the button");
    fake_mouse_buttons = 1u << 1; for (int n = 0; n < KEY_UP_POLLS; n++) poll_input();
    expect(s2a_vpad_read(sticks) == 0, "a click this view never received is not added from system state");
    fake_mouse_buttons = 0;
    reset_input();
    puts("Mac mouse: lost mouseUp released by polling, false release restores passed");
}
static void inspect_guide(NSView *view, int *buttons, int *editors) {
    if ([view isKindOfClass:S2AControllerButton.class]) (*buttons)++;
    if ([view isKindOfClass:NSTextField.class] && [(NSTextField *)view isEditable]) (*editors)++;
    if ([view isKindOfClass:NSPopUpButton.class]) (*editors)++;
    for (NSView *child in view.subviews) inspect_guide(child, buttons, editors);
}
static BOOL guide_has_label(NSView *view, NSString *value) {
    if ([view isKindOfClass:NSTextField.class] && [[(NSTextField *)view stringValue] isEqual:value]) return YES;
    for (NSView *child in view.subviews) if (guide_has_label(child, value)) return YES;
    return NO;
}
static void *read_snapshots(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        int32_t s[4]; uint32_t b = s2a_vpad_read(s);
        if (s[0] != (int32_t)b || s[1] != (int32_t)b || s[2] != (int32_t)b || s[3] != (int32_t)b) return (void *)1;
    }
    return NULL;
}
// Event payload only: the adapter's real mouseMoved/sample/read path is exercised.
@interface TestMotion : NSObject
@property double deltaX, deltaY;
@end
@implementation TestMotion
@end

static CGError association_error;
static int association_calls, cursor_hides, cursor_shows;
static boolean_t association_connected;
static CGError test_associate(boolean_t connected) {
    association_calls++; association_connected = connected; return association_error;
}
static void test_cursor_visibility(BOOL visible) { if (visible) cursor_shows++; else cursor_hides++; }
static NSEvent *capture_key_event(NSEventType type, unsigned short code, BOOL repeat) {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0
        windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:repeat keyCode:code];
}
static void test_capture_held_key(BOOL system_state_matches) {
    config = default_config(); reset_input(); focused = YES;
    physical_keys[13] = system_state_matches;
    S2AMetalView *view = [S2AMetalView new];
    S2AMacDelegate *delegate = [S2AMacDelegate new];
    int32_t sticks[4];
    NSEvent *click = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:1 clickCount:1 pressure:1];
    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:2 clickCount:1 pressure:0];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 13, NO)];
    expect(s2a_vpad_read(sticks) == 0 && sticks[1] == 32767, "W held before capture");
    for (int capture = 0; capture < 2; capture++) {
        [view mouseDown:click];
        expect(mouse_captured && s2a_vpad_read(sticks) == 0 && sticks[1] == 32767,
               "capture preserves held W and first click never sends Y");
        [view mouseUp:up];
        for (int n = 0; n < 1200; n++) {
            [view keyDown:capture_key_event(NSEventTypeKeyDown, 13, YES)]; poll_input();
        }
        expect(s2a_vpad_read(sticks) == 0 && sticks[1] == 32767,
               "held W survives capture and repeated keyDown/poll cycles");
        [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
        expect(!mouse_captured && s2a_vpad_read(sticks) == 0 && sticks[1] == 32767,
               "Esc releases mouse without clearing held W");
        [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    }
    [view mouseDown:click]; [view mouseUp:up];
    association_error = kCGErrorFailure;
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
    expect(!mouse_captured && mouse_release_pending && s2a_vpad_read(sticks) == 0 && sticks[1] == 32767,
           "failed mouse release preserves held W while waiting for retry");
    association_error = kCGErrorSuccess;
    [view mouseDown:click]; [view mouseUp:up];
    expect(mouse_captured && !mouse_release_pending && s2a_vpad_read(sticks) == 0 && sticks[1] == 32767,
           "release retry and recapture preserve W without sending Y");
    [delegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification object:nil]];
    expect(!mouse_captured && keys.count == 0 && s2a_vpad_read(sticks) == 0 && !sticks[1],
           "focus loss releases capture and clears held W");
    [delegate windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:nil]];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 13, YES)]; poll_input();
    expect(s2a_vpad_read(sticks) == 0 && !sticks[1], "repeat after focus loss cannot restore W");
    physical_keys[13] = NO;
    [view keyUp:capture_key_event(NSEventTypeKeyUp, 13, NO)];
    reset_input();
    puts("Mac mouse: held W across capture/Esc/recapture, first-click Y isolation and focus passed");
}

// 全屏 Esc（玩家反馈进了全屏回不到窗口）：期望按规则手写——Esc 没映射：全屏下按一次就退全屏（鼠标锁着一并释放），不发游戏键；不是全屏不退；
//   Esc 映射了 PLUS：短按照常发 PLUS、1 秒内松开不退；按住满 1 秒退全屏一次、PLUS 随之松开
static BOOL fake_fullscreen; static int fake_toggles;
static BOOL fake_window_fullscreen(NSWindow *w) { (void)w; return fake_fullscreen; }
static void fake_fullscreen_toggle(NSWindow *w) { (void)w; fake_toggles++; fake_fullscreen = !fake_fullscreen; }
static void run_loop_for(double seconds) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]]; }
static void test_fullscreen_esc(void) {
    window_fullscreen = fake_window_fullscreen; fullscreen_toggle = fake_fullscreen_toggle;
    S2AMetalView *view = [S2AMetalView new]; int32_t sticks[4];
    config = default_config(); reset_input(); focused = YES;
    expect(!esc_mapped(), "default keyboard leaves Esc unmapped");
    fake_fullscreen = NO; fake_toggles = 0;
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)]; [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    expect(fake_toggles == 0, "windowed Esc does not toggle full screen");
    fake_fullscreen = YES; config = @{@"keyboard": @{@"A": @{@"code": @14}}, @"mouse": @{}, @"gamepad": @{}, @"axes": @{}, @"deadzone": @0.15, @"mouseOptions": @{@"mode": @"camera"}};
    set_mouse_capture(YES);
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
    expect(fake_toggles == 1 && !fake_fullscreen && !mouse_captured && s2a_vpad_read(sticks) == 0, "unmapped Esc leaves full screen at once and releases the mouse");
    [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    config = @{@"keyboard": @{@"PLUS": @{@"code": @53}}, @"mouse": @{}, @"gamepad": @{}, @"axes": @{}, @"deadzone": @0.15, @"mouseOptions": @{@"mode": @"camera"}};
    reset_input(); fake_fullscreen = YES; fake_toggles = 0;
    physical_keys[53] = YES; [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
    expect(s2a_vpad_read(sticks) == (1u << 10) && fake_toggles == 0, "mapped Esc short press sends PLUS and stays full screen");
    run_loop_for(0.4); physical_keys[53] = NO; [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    run_loop_for(0.9);
    expect(fake_toggles == 0 && fake_fullscreen && s2a_vpad_read(sticks) == 0, "released before 1 s: no full-screen change");
    physical_keys[53] = YES; [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
    run_loop_for(1.2);
    expect(fake_toggles == 1 && !fake_fullscreen && s2a_vpad_read(sticks) == 0, "mapped Esc held 1 s leaves full screen once and releases PLUS");
    physical_keys[53] = NO; [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    window_fullscreen = window_fullscreen_now; fullscreen_toggle = window_fullscreen_toggle;
    reset_input(); config = default_config();
    puts("Mac full screen: Esc exits when unmapped, short press to game / hold 1 s exits when mapped");
}
static void test_capture_key_order(void) {
    config = @{@"keyboard": @{@"PLUS": @{@"code": @53}, @"A": @{@"code": @14}},
               @"mouse": @{}, @"gamepad": @{}, @"axes": @{}, @"deadzone": @0.15,
               @"mouseOptions": @{@"mode": @"camera"}};
    reset_input(); focused = YES;
    S2AMetalView *view = [S2AMetalView new]; int32_t sticks[4];
    set_mouse_capture(YES);
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
    expect(!mouse_captured && s2a_vpad_read(sticks) == 0, "captured Esc only releases, never sends mapped PLUS");
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, YES)];
    expect(s2a_vpad_read(sticks) == 0, "held Esc repeat after release cannot send mapped PLUS");
    [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, NO)];
    expect(s2a_vpad_read(sticks) == (1u << 10), "fresh uncaptured Esc sends mapped PLUS");
    [view keyUp:capture_key_event(NSEventTypeKeyUp, 53, NO)];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 14, NO)];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 14, YES)];
    expect(s2a_vpad_read(sticks) == 1, "repeat preserves an already held game button");
    [view keyUp:capture_key_event(NSEventTypeKeyUp, 14, NO)];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 14, YES)];
    expect(s2a_vpad_read(sticks) == 0, "late repeat cannot restore a released game button");
    set_mouse_capture(YES); focused = NO; // Defensive release must precede both keyDown filters.
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 53, YES)];
    expect(!mouse_captured && !mouse_release_pending, "Esc release precedes unfocused and repeat rejection");
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 14, NO)];
    [view keyDown:capture_key_event(NSEventTypeKeyDown, 14, YES)];
    focused = YES; sample_input();
    expect(s2a_vpad_read(sticks) == 0, "unfocused events cannot reappear on focus return");
    reset_input(); config = default_config();
    puts("Mac mouse: Esc mapping, release ordering, repeat and focus regression passed");
}

// 审计 P2-63：多个控制器时取最近有输入的扩展手柄；没有扩展手柄档的（遥控器等）跳过。假对象只答 active_pad 用到的两个方法
@class FakeController;
@interface FakePad : NSObject
@property double lastEventTimestamp;
@property(copy) void (^valueChangedHandler)(GCExtendedGamepad *, GCControllerElement *);
@property(weak) FakeController *controller;
@end
@implementation FakePad
@end
@interface FakeController : NSObject
@property(strong) FakePad *extendedGamepad;
@property(strong) dispatch_queue_t handlerQueue;
@property(strong) id motion;
@end
@implementation FakeController
@end
static FakeController *fake_controller(FakePad *pad) { FakeController *c = [FakeController new]; c.extendedGamepad = pad; pad.controller = c; return c; }
@interface FakeProSensor : NSObject
@property BOOL sensorsRequireManualActivation, hasRotationRate, hasGravityAndUserAcceleration, sensorsActive;
@property GCRotationRate rotationRate;
@property GCAcceleration gravity, userAcceleration, acceleration;
@end
@implementation FakeProSensor
@end
static void test_pro_sensor_control(void) {
    FakePad *pad = [FakePad new]; FakeController *c = fake_controller(pad);
    FakeProSensor *m = [FakeProSensor new]; m.sensorsRequireManualActivation = YES; m.hasRotationRate = YES; c.motion = m;
    atomic_store(&g_motion_wanted, 1);
    sample_motion((GCExtendedGamepad *)pad);
    expect(m.sensorsActive, "Pro public sensor control is requested even without separated gravity / usable calibrated samples");
    expect(g_motion_known == 1, "rotation without separated gravity gives an acceleration-only sample (DualSense), ranked below the Pro raw HID path in s2a_pad_read_motion");
    atomic_store(&g_motion_wanted, 0); sample_motion(nil);
    expect(!m.sensorsActive, "motion disable releases public Pro sensor request");
    sample_motion(nil);
    GCController *old = pad_owner[1]; pad_owner[1] = (GCController *)c;
    atomic_store(&g_motion_wanted, 1); sample_slot_motion(NULL);
    expect(m.sensorsActive && g_slot_motion_known[1] == 1, "P2 sensor control stays independent of calibrated samples; an acceleration-only sample still reaches P2 (DualSense)");
    atomic_store(&g_motion_wanted, 0); sample_slot_motion(NULL);
    expect(!m.sensorsActive, "P2 motion disable releases its public request");
    pad_owner[1] = old;
    puts("Mac Pro motion: public activation independent of calibrated sample capability passed");
}
static void test_active_pad(void) {
    FakePad *old = [FakePad new], *recent = [FakePad new]; old.lastEventTimestamp = 10; recent.lastEventTimestamp = 20;
    NSArray *list = @[fake_controller(nil), fake_controller(old), fake_controller(recent)];
    expect((id)active_pad(list) == recent, "picks the extended gamepad with the latest input, skipping a remote first in the list");
    old.lastEventTimestamp = 30;
    expect((id)active_pad(list) == old, "switches when the other pad gets newer input");
    expect(active_pad(@[fake_controller(nil)]) == nil && active_pad(@[]) == nil, "no extended gamepad: none");
}

// A2 事件驱动输入：Mac 导出 s2a_vpad_read_ex，sample_input 同步写 runtime/src/pad.c，InputPump 才能从 Sleep(16)
//   换成 s2a_pad_wait。期望手算：keyDown 后 read_ex 立即看到 A 位并带 host 时间戳；keyUp 在 50ms deadline 前唤醒等待线程
//   （旧实现没有 pad 通知，等待只能跑满 deadline）。
static void test_event_input(void) {
    typedef void (*read_ex_fn)(s2a_pad_snapshot *);
    read_ex_fn read_ex = (read_ex_fn)dlsym(RTLD_DEFAULT, "s2a_vpad_read_ex");
    expect(read_ex != NULL, "Mac exports s2a_vpad_read_ex");
    config = default_config(); reset_input(); focused = YES;
    s2a_pad_set_active(1);
    S2AMetalView *view = [S2AMetalView new];
    [view keyDown:key_event(NSEventTypeKeyDown, 14, NO)];   // 缺省映射 E = Switch A（位 0）
    s2a_pad_snapshot down; read_ex(&down);
    expect((down.buttons & 1u) != 0 && down.host_timestamp_us != 0, "event snapshot carries the key and its host timestamp");
    { s2a_pad_latency clear; s2a_pad_get_latency(&clear, 1); }   // 清掉前面用例留下的样本，下面只看这一次提交
    uint64_t before_commit = s2a_pad_now_us();
    s2a_pad_record_commit(&down);
    uint64_t after_commit = s2a_pad_now_us();

    uint64_t observed;
    { s2a_pad_snapshot snap; read_ex(&snap); observed = snap.notification_seq; }
    __block int woke = -1;
    dispatch_semaphore_t started = dispatch_semaphore_create(0), done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        woke = s2a_pad_wait(observed, s2a_pad_now_us() + 5000000);   // 截止放到 5 秒：判它为什么返回，不判用了多久（不受机器负载影响）
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_signal(started);
    [NSThread sleepForTimeInterval:0.02];   // 让等待线程先进入条件变量；即使没进，序号变化也会让它立即返回
    [view keyUp:key_event(NSEventTypeKeyUp, 14, NO)];
    expect(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0, "pad wait completes after key up");
    expect(woke == 1, "key up wakes pad wait because the notification sequence changed, not because the deadline passed");

    // 延迟 = 提交时刻 − 按键的 host 时间戳：提交前后各取一次时钟，记下的值必须夹在两者之间（只依赖时间戳之差，不设绝对上限）
    s2a_pad_latency latency; s2a_pad_get_latency(&latency, 1);
    expect(latency.samples == 1 && latency.host_total_us == latency.host_max_us &&
           latency.host_max_us >= before_commit - down.host_timestamp_us && latency.host_max_us <= after_commit - down.host_timestamp_us,
           "committed event latency equals commit time minus the key's host timestamp");

    // 负对照（维护者要求）：事件驱动只发变化；长按 / 摇杆持续推时，没有新事件的那几拍 read_ex 仍必须返回完整的按住状态。
    [keys addObject:@14]; [keys addObject:@13]; sample_input();   // E = A，W = LS_UP
    uint64_t held_seq;
    { s2a_pad_snapshot snap; read_ex(&snap); held_seq = snap.notification_seq; }
    expect(s2a_pad_wait(held_seq, s2a_pad_now_us() + 20000) == 0, "idle wait times out without new events");
    for (int i = 0; i < 5; i++) {
        s2a_pad_snapshot held; read_ex(&held);
        expect((held.buttons & 1u) != 0 && held.axes[1] == 32767, "held key and stick persist across idle reads");
    }
    [keys removeObject:@14]; [keys removeObject:@13]; sample_input();

    // 鼠标位移仍由 HID 读取侧消费一次（旧 s2a_vpad_read 与 read_ex 共用）
    config = @{@"keyboard": @{}, @"mouse": @{}, @"gamepad": @{}, @"axes": @{}, @"deadzone": @0.15,
               @"mouseOptions": @{@"mode": @"camera", @"sensitivityX": @1, @"sensitivityY": @1}};
    apply_mouse_options(); mouse_captured = YES; focused = YES;
    TestMotion *motion = [TestMotion new]; motion.deltaX = 2;
    S2AMetalView *motionView = [S2AMetalView new];
    mouse_now = fake_mouse_now; g_fake_mouse_ns = 500000000ull;
    [motionView mouseMoved:(NSEvent *)motion];
    s2a_pad_snapshot moved; read_ex(&moved);
    expectf(moved.axes[2] == 1454, "read_ex applies the mouse window (3000 × 16 / 33 = 1454): %d", moved.axes[2]);
    g_fake_mouse_ns += 40000000; read_ex(&moved);
    expect(moved.axes[2] == 0, "read_ex centers once the window has passed");
    mouse_now = mouse_clock; reset_input();
    mouse_captured = NO;
    config = default_config(); focused = YES; s2a_pad_set_active(0); reset_input();
    puts("Mac input: event-driven snapshot, wait wake, idle-hold persistence and latency recording passed");
}

// 维护方评审 11-09：手柄断开时前端切到 TOUCH 源，但 pad.c 里 SYSTEM 源一直 connected，selected() 仍选 SYSTEM
//   并丢掉 TOUCH 更新，断开瞬间按住的键卡住、键盘失灵。期望手算：SYSTEM 按 A（0x1）→ 断开后键盘按 B（0x2）→
//   快照 source = TOUCH、buttons = 0x2。负对照：去掉 publish_pad_state 里的 SYSTEM 断开 → 快照仍是 SYSTEM / 0x1。
static void test_pad_disconnect(void) {
    typedef void (*read_ex_fn)(s2a_pad_snapshot *);
    read_ex_fn read_ex = (read_ex_fn)dlsym(RTLD_DEFAULT, "s2a_vpad_read_ex");
    expect(read_ex != NULL, "Mac exports s2a_vpad_read_ex");
    int32_t sticks[4] = {0};
    focused = YES; s2a_pad_set_active(0); s2a_pad_set_active(1);
    publish_pad_state(1, 1u, sticks);
    s2a_pad_snapshot snap; read_ex(&snap);
    expect(snap.source == S2A_PAD_SYSTEM && snap.buttons == 1u, "gamepad press is selected on SYSTEM source");
    publish_pad_state(0, 1u << 1, sticks);
    read_ex(&snap);
    expect(snap.source == S2A_PAD_TOUCH && snap.buttons == (1u << 1), "keyboard still works after gamepad disconnect");
    expect(!s2a_pad_has_controller(), "disconnected gamepad is no longer the selected controller");
    s2a_pad_set_active(0); reset_input();
    puts("Mac pad: disconnect returns to touch source and keeps keyboard input passed");
}

// 本地多手柄槽位（s2a_slot_plan 是纯逻辑，用假身份指针测，不碰 GameController）：
//   按连接顺序占槽、断开清槽（槽位不压实，玩家号不跳）、重连回原槽、超过 4 只的第 5 只丢弃。
//   apply_slots 与 sample_input 的写回同构：整表重建，只有本轮还在的才写回。
static void apply_slots(const void **owners, const void *const *live, const int *slot_of, int count, int max) {
    for (int i = 0; i < max; i++) owners[i] = NULL;
    for (int j = 0; j < count; j++) if (slot_of[j] >= 0) owners[slot_of[j]] = live[j];
}
static void test_pad_slots(void) {
    int ids[5] = {1, 2, 3, 4, 5};
    const void *a = &ids[0], *b = &ids[1], *c = &ids[2], *d = &ids[3], *e = &ids[4];
    const void *owners[4] = {NULL, NULL, NULL, NULL};
    const void *live[5]; int slot_of[5];

    live[0] = a;
    expect(s2a_slot_plan(owners, live, 1, 4, slot_of) == 1 && slot_of[0] == 0, "one pad takes slot 0 and reports one pad");
    apply_slots(owners, live, slot_of, 1, 4);

    live[1] = b;
    expect(s2a_slot_plan(owners, live, 2, 4, slot_of) == 2 && slot_of[0] == 0 && slot_of[1] == 1,
        "second pad joins after the first in connection order");
    apply_slots(owners, live, slot_of, 2, 4);

    live[2] = c;
    expect(s2a_slot_plan(owners, live, 3, 4, slot_of) == 3 && slot_of[2] == 2, "third pad takes slot 2");
    apply_slots(owners, live, slot_of, 3, 4);

    // 第一只断开：槽 0 空出来，第二、三只留在原槽（不压实，玩家号不跳）
    expect(s2a_slot_plan(owners, live + 1, 2, 4, slot_of) == 3 && slot_of[0] == 1 && slot_of[1] == 2,
        "unplugging the first clears slot 0 and leaves the others where they were");
    apply_slots(owners, live + 1, slot_of, 2, 4);

    // 重连回原槽：中间没插别的，第一只回到槽 0
    expect(s2a_slot_plan(owners, live, 3, 4, slot_of) == 3 && slot_of[0] == 0,
        "a pad that comes back reclaims its original slot");
    apply_slots(owners, live, slot_of, 3, 4);

    // 第一只又断开、新的第四只接上：占最小空槽（槽 0），不是追加到末尾
    const void *bcd[3] = {b, c, d};
    expect(s2a_slot_plan(owners, bcd, 3, 4, slot_of) == 3 && slot_of[0] == 1 && slot_of[1] == 2 && slot_of[2] == 0,
        "a new pad takes the lowest free slot");
    apply_slots(owners, bcd, slot_of, 3, 4);

    // 超过 4 只：第五只 -1，不参与
    const void *five[5] = {a, b, c, d, e};
    expect(s2a_slot_plan(owners, five, 5, 4, slot_of) == 4 && slot_of[4] == -1, "a fifth pad is ignored");
    // 全断开：回到只有槽 0（键盘 / 鼠标）的那一位
    expect(s2a_slot_plan(owners, live, 0, 4, slot_of) == 1, "no pads: back to one slot for keyboard and mouse");
}

// #1264（D-7VG4 现场）：兜底手柄 a（PowerA，顺序表认不到它）先占槽 0；游戏中途连上 DualSense b 进槽 1；面板把 b 登记成 P1（顺序表 rank 0）。
//   期望手算：钉住后 b 在槽 0、a 被挤到槽 1；再关掉 a，b 仍在槽 0（单人游戏读玩家 1 就是它）。没登记的手柄不受钉住影响、照旧留原槽
static void test_pad_slot_pin(void) {
    int ids[3] = {1, 2, 3};
    const void *a = &ids[0], *b = &ids[1], *c = &ids[2];
    const void *owners[4] = {a, b, NULL, NULL};
    int slot_of[3];
    const void *ab[2] = {a, b}; int rank_ab[2] = {S2A_PAD_ORDER_UNLISTED, 0};
    s2a_slot_pin(owners, ab, rank_ab, 2, 4);
    expect(s2a_slot_plan(owners, ab, 2, 4, slot_of) == 2 && slot_of[1] == 0 && slot_of[0] == 1,
        "a pad the panel lists as P1 takes slot 0; the unlisted one moves to slot 1");
    apply_slots(owners, ab, slot_of, 2, 4);
    const void *only_b[1] = {b}; int rank_b[1] = {0};
    s2a_slot_pin(owners, only_b, rank_b, 1, 4);
    expect(s2a_slot_plan(owners, only_b, 1, 4, slot_of) == 1 && slot_of[0] == 0, "turning the old pad off leaves the listed P1 in slot 0");
    // 没登记的两只（a 槽 1、c 槽 2）+ 登记 P1 的 b 已在槽 0：钉住什么都不动
    const void *owners2[4] = {b, a, c, NULL};
    const void *bac[3] = {b, a, c}; int rank_bac[3] = {0, S2A_PAD_ORDER_UNLISTED, S2A_PAD_ORDER_UNLISTED};
    s2a_slot_pin(owners2, bac, rank_bac, 3, 4);
    expect(owners2[0] == b && owners2[1] == a && owners2[2] == c && owners2[3] == NULL, "pinning leaves pads already in place and unlisted pads alone");
    // 登记成 P2（rank 1）、P1 不在：钉在槽 1，不提前到槽 0（同原机：玩家号不跳）
    const void *owners3[4] = {NULL, NULL, NULL, NULL};
    const void *only_c[1] = {c}; int rank_c[1] = {1};
    s2a_slot_pin(owners3, only_c, rank_c, 1, 4);
    expect(s2a_slot_plan(owners3, only_c, 1, 4, slot_of) == 2 && slot_of[0] == 1, "a listed P2 stays in slot 1 when P1 is absent");
    // 顺序表槽 0 是 S2A_PAD_ORDER_EMPTY（P1 暂时断开）、槽 1 是 Xbox：Xbox 仍拿 rank 1、不匹配 empty
    s2a_pad_order sparse = {0};
    sparse.count = 2;
    sparse.entry[0].ns2_player = S2A_PAD_ORDER_EMPTY;
    sparse.entry[1].ns2_player = -1; snprintf(sparse.entry[1].name, sizeof sparse.entry[1].name, "Xbox"); sparse.entry[1].index = 0;
    expect(s2a_pad_order_rank(&sparse, -1, "Xbox", 0) == 1, "empty slot 0 preserves rank 1 for P2");
    expect(s2a_pad_order_rank(&sparse, -1, "", 0) == S2A_PAD_ORDER_UNLISTED, "empty slot entry does not match any live controller");
}

// 只有通用物理描述的手柄（没有扩展手柄档）：按标准键名从 physicalInputProfile 读、回调装在 valueDidChangeHandler 上。
//   用系统自带的虚拟简易手柄（controllerWithMicroGamepad：只有 A / X / Menu / 方向键）当这种手柄。期望手算：
//   它的类别是「Siri Remote」，认不出类型按缺省「自动」照字母（pad.c s2a_pad_abxy_order）：系统 A = Switch A，默认配置第 0 位（1）；
//   方向键上是第 13 位（8192）；没按任何键是 0
static void test_profile_only_pad(void) {
    config = default_config();
    GCController *c = [GCController controllerWithMicroGamepad];
    expect(c.extendedGamepad == nil && pad_has_profile_buttons(c), "micro-only controller counts as a profile-only pad");
    install_pad_handler(c);
    expect(c.physicalInputProfile.valueDidChangeHandler != nil && c.handlerQueue == dispatch_get_main_queue(), "profile handler installed on the main queue (#1819)");
    uint32_t buttons = 99; int32_t sticks[4];
    sample_profile_state(c, config, &buttons, sticks);
    expect(buttons == 0, "nothing pressed: no buttons");
    [c.microGamepad.buttonA setValue:1];
    sample_profile_state(c, config, &buttons, sticks);
    expect(buttons == 1, "unrecognized pad, automatic layout: profile button A maps to Switch A by its letter");
    s2a_pad_layout_set(S2A_PAD_LAYOUT_SWITCH); sample_profile_state(c, config, &buttons, sticks);
    expect(buttons == 2, "Switch layout: profile button A (south) maps to Switch B by position");
    s2a_pad_layout_set(S2A_PAD_LAYOUT_AUTO);
    [c.microGamepad.buttonA setValue:0]; [c.microGamepad.dpad setValueForXAxis:0 yAxis:1];
    sample_profile_state(c, config, &buttons, sticks);
    expect(buttons == (1u << 13), "profile dpad up maps to Switch Up");
    puts("Mac profile-only pad: handler and standard-name reading passed");
}

// 手柄按键布局（11-11）：系统自带的虚拟扩展手柄（controllerWithExtendedGamepad，类别认不出 = 按 Xbox / MFi 那种排法：buttonA 在南、Y 在北）。
//   期望手算（默认配置 东/南/北/西 = Switch A/B/X/Y = 第 0/1/2/3 位）：
//   自动 / Xbox 布局：照字母，按 buttonA → Switch A（1）、按 buttonY → Switch Y（8）；
//   Switch 布局：按位置，buttonA 在南 → Switch B（2）、buttonY 在北 → Switch X（4）；切换后下一次读即按新布局，不用重开
static void test_pad_layout(void) {
    config = default_config();
    GCController *c = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad *pad = c.extendedGamepad;
    uint32_t buttons; int32_t sticks[4];
    int layouts[3] = {S2A_PAD_LAYOUT_AUTO, S2A_PAD_LAYOUT_SWITCH, S2A_PAD_LAYOUT_XBOX};
    uint32_t want_a[3] = {1, 2, 1}, want_y[3] = {8, 4, 8};
    for (int i = 0; i < 3; i++) {
        s2a_pad_layout_set(layouts[i]);
        [pad.buttonA setValue:1]; sample_pad_state(pad, config, &buttons, sticks);
        expect(buttons == want_a[i], "pad layout: button A lands on the expected Switch button");
        [pad.buttonA setValue:0]; [pad.buttonY setValue:1]; sample_pad_state(pad, config, &buttons, sticks);
        expect(buttons == want_y[i], "pad layout: button Y lands on the expected Switch button");
        [pad.buttonY setValue:0];
    }
    s2a_pad_layout_set(S2A_PAD_LAYOUT_AUTO);
    puts("Mac pad layout: automatic / Switch / Xbox take effect on the next read passed");
}

// 同一份四键配置在主线程与事件回调路径生效，原始键态始终只映射一次。
static void test_shared_face_map(void) {
    GCController *c = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad *pad = c.extendedGamepad;
    GCControllerButtonInput *face[] = {pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY};
    config = default_config(); pad_slot_owner[0] = c;
    atomic_store(&input_muted, 0); atomic_store(&kbd_buttons_snap, 0);
    for (int i = 0; i < 4; i++) atomic_store(&kbd_sticks_snap[i], 0);
    int codes[] = {0xe4, 0xb1, 0x4e};
    uint32_t expected[3][4] = {{1, 2, 4, 8}, {2, 1, 8, 4}, {4, 8, 1, 2}};
    int checks = 0;
    for (int layout = 0; layout < 3; layout++) {
        g_face_map = codes[layout];
        for (int key = 0; key < 4; key++) {
            [face[key] setValue:1];
            uint32_t sampled; int32_t sticks[4];
            sample_pad_state(pad, face_mapping_cfg(config), &sampled, sticks);
            uint32_t raw = controller_face_bits(c);
            expect(mapped_face_buttons(c, raw, sampled) == expected[layout][key], "shared face map: physical key reaches agreed Switch button");
            pad_event_fast(c);
            expect(s2a_vpad_read_pad(0, sticks) == expected[layout][key], "main-player face map: event callback publishes the same mapped button");
            pad_slot_owner[0] = nil;
            for (int slot = 1; slot < 4; slot++) {
                pad_slot_owner[slot] = c;
                pad_event_fast(c);
                expect(s2a_vpad_read_pad(slot, sticks) == expected[layout][key], "all four players use the same shared face map");
                pad_slot_owner[slot] = nil;
            }
            pad_slot_owner[0] = c;
            [face[key] setValue:0]; pad_event_fast(c);
            expect(s2a_vpad_read_pad(0, sticks) == 0, "releasing a remapped key clears its target");
            checks += 6;
        }
    }
    expect(mapped_face_buttons(nil, 1u << 2, (1u << 7)) == ((1u << 7) | 4), "Switch channel mapping preserves shoulders and normal Nintendo face labels");
    expect(mapped_face_buttons(c, 0, 1) == 1, "a non-face input bound to A keeps its game action");
    g_face_map = 0xb1;
    expect(mapped_face_buttons(nil, 1, 0) == 1, "Switch controllers always keep their native face layout");
    int valid = 0;
    for (int code = 0; code < 256; code++) if (s2a_face_valid(code)) {
        valid++;
        for (unsigned bits = 0; bits < 16; bits++) expect((s2a_face_apply(bits | (1u << 20), code) & ~15u) == (1u << 20), "all permutations preserve non-face keys");
    }
    expect(valid == 24, "exactly the 24 permutations are accepted");
    NSString *file = [NSTemporaryDirectory() stringByAppendingPathComponent:[@"s2arm-face-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    setenv("S2A_PAD_FACE_FILE", file.UTF8String, 1);
    [@"{\"schema\":1,\"profiles\":{\"other\":177}}" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    pad_face_refresh(YES);
    expect(controller_face_map(c) == 0xb1, "game host reads the launcher's legacy document as one shared map");
    [@"{\"schema\":2,\"mapping\":228}" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    pad_face_refresh(YES);
    expect(controller_face_map(c) == 0xe4, "restoring defaults takes effect live without restarting the game");
    expect(controller_face_map(nil) == 0xe4, "a document without a Nintendo entry keeps Switch pads on their labels");
    // Player picked the Xbox layout for Switch pads: target A/B/X/Y <- source B/A/Y/X (iOS s2a_pad_abxy_order swap) = 1|0<<2|3<<4|2<<6 = 177.
    [@"{\"schema\":2,\"mapping\":228,\"nintendo\":177}" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    pad_face_refresh(YES);
    expect(controller_face_map(nil) == 0xb1, "the player's Nintendo choice reaches the Switch channel");
    expect(controller_face_map(c) == 0xe4, "the Nintendo choice leaves other controllers on the shared map");
    expect(mapped_face_buttons(nil, 1u << 1, 0) == 1u, "Xbox layout on a Switch pad: bottom B confirms as A");
    expect(mapped_face_buttons(nil, 1u << 0, 0) == 2u, "Xbox layout on a Switch pad: right A cancels as B");
    expect(mapped_face_buttons(nil, 1u << 3, 0) == 4u && mapped_face_buttons(nil, 1u << 2, 0) == 8u, "Xbox layout on a Switch pad swaps X and Y");
    [@"{\"schema\":2,\"mapping\":177,\"nintendo\":0}" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    pad_face_refresh(YES);
    expect(controller_face_map(nil) == 0xe4 && controller_face_map(c) == 0xe4, "an invalid Nintendo entry falls back to platform defaults");
    [NSFileManager.defaultManager removeItemAtPath:file error:nil];
    setenv("S2A_PAD_FACE_FILE", "/nonexistent/s2arm-face.json", 1); pad_face_refresh(YES);
    pad_slot_owner[0] = nil;
    printf("Mac shared face mapping: %d input checks, 24 permutations x 16 states, live save/reset passed\n", checks);
}

static void test_controller_face_overlay(void) {
    s2a_face_overlay_hold hold = {0};
    expect(!s2a_face_overlay_step(&hold, 0, 1, 1), "overlay chord starts hidden");
    expect(!s2a_face_overlay_step(&hold, 999999999, 1, 1), "overlay does not open before one second");
    expect(s2a_face_overlay_step(&hold, 1000000000, 1, 1), "one-second L3/R3 chord opens the overlay");
    expect(s2a_face_overlay_step(&hold, 4000000000, 1, 1), "held chord toggles only once");
    s2a_face_overlay_step(&hold, 4100000000, 0, 1);
    s2a_face_overlay_step(&hold, 4200000000, 1, 1);
    expect(!s2a_face_overlay_step(&hold, 5200000000, 1, 1), "the next fresh chord closes the overlay");
    s2a_face_overlay_step(&hold, 6000000000, 1, 0);
    expect(!s2a_face_overlay_step(&hold, 8000000000, 1, 1), "focus gain cannot activate a chord held while inactive");
    s2a_face_overlay_step(&hold, 8100000000, 0, 1);
    s2a_face_overlay_step(&hold, 8200000000, 1, 1);
    expect(s2a_face_overlay_step(&hold, 9200000000, 1, 1), "release rearms the chord");
    expect(!s2a_face_overlay_step(&hold, 9300000000, 0, 0), "disconnect hides the overlay");
    S2AControllerFaceOverlay *view = [[S2AControllerFaceOverlay alloc] initWithFrame:NSMakeRect(0, 0, 268, 180)];
    view.family = @"playstation"; view.mapping = 0xb1;
    expect([view.rows isEqual:@[@"△ → X", @"□ → Y", @"× → B", @"○ → A"]], "PS overlay shows the exact physical-to-Switch map in diamond order");
    view.family = @"xbox";
    expect([view.rows isEqual:@[@"Y → X", @"X → Y", @"A → B", @"B → A"]], "Xbox overlay shows only its own four physical buttons");
    view.mapping = 0xe4;
    expect([view.rows isEqual:@[@"Y → Y", @"X → X", @"A → A", @"B → B"]], "default mapping is reflected in the overlay too");
    expect([view hitTest:NSMakePoint(30, 30)] == nil, "small overlay leaves game mouse input available");
    view.family = @"playstation"; view.mapping = 0xb1; view.hidden = NO;
    NSString *output = NSProcessInfo.processInfo.environment[@"S2A_SETTINGS_SHOTS"];
    if (output) {
        NSBitmapImageRep *bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
        [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[output stringByAppendingPathComponent:@"game-face-overlay.png"] atomically:YES];
    }
    puts("Mac controller face overlay: 9 chord timing cases, PS/Xbox/default rows and mouse passthrough passed");
}

// 分玩家键位（槽 i = 玩家 i+1，InputSettingsSheet 按玩家保存）：玩家 2–4 没给自己的映射（S2A_INPUT_CONFIG_P2..P4 没给 / 读不懂）
//   就照玩家 1 的全局那份，行为与原来完全一致；给了就槽位各照各的采样。期望手算（虚拟手柄 buttonA 自动布局按「东」上报，
//   默认配置目标 A ← 东键 → 按下读 Switch A 第 0 位）：玩家 2 的映射把目标 B 绑到东键（"B": "east"）→
//   同一只手柄按同一个键，玩家 1 读到 Switch A（1）、玩家 2 读到 Switch B（2）
static void test_per_player_config(void) {
    config = default_config();
    player_config_maps[0] = player_config_maps[1] = player_config_maps[2] = nil;
    expect(slot_cfg(0) == config && slot_cfg(1) == config && slot_cfg(2) == config && slot_cfg(3) == config && slot_cfg(4) == config,
        "no per-player maps: every slot uses the global config");
    NSDictionary *p2 = @{@"gamepad": @{@"B": @"east"}, @"mouse": @{}, @"axes": @{}, @"deadzone": @0.15, @"mouseOptions": @{@"mode": @"camera"}};
    player_config_maps[0] = p2;   // 玩家 2（槽 1）
    expect(slot_cfg(0) == config, "slot 0 stays on the player 1 config");
    expect(slot_cfg(1) == p2 && slot_cfg(2) == config && slot_cfg(3) == config, "slot 1 uses player 2's map, other slots fall back");
    GCController *c = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad *pad = c.extendedGamepad;
    uint32_t buttons; int32_t sticks[4];
    [pad.buttonA setValue:1];   // 东键（自动布局：buttonA 按东上报）
    sample_pad_state(pad, config, &buttons, sticks);
    expect(buttons == 1, "player 1 map: east button reads Switch A");
    sample_pad_state(pad, p2, &buttons, sticks);
    expect(buttons == 2, "player 2 map: east button reads Switch B through her own binding");
    [pad.buttonA setValue:0];
    player_config_maps[0] = nil;
    expect(slot_cfg(1) == config, "cleared player 2 map: back to the global config");
    puts("Mac per-player configs: slot_cfg routing and per-player sampling passed");
}


// 手柄事件回调：handlerQueue 必须是主队列（#1819：真机手柄状态由 GameController 在主线程改写，回调在别的队列读同一份 → Recursive or concurrent mutation 杀进程）；
//   没占到槽的回调直接跳过（快速路径不碰没排进槽位表的手柄），没有新事件时 120Hz 定时器仍是回退采样，长按不会因为「只发变化」而丢。
static void test_pad_handler_install(void) {
    FakePad *pad = [FakePad new];
    FakeController *controller = fake_controller(pad);
    install_pad_handler((GCController *)controller);
    expect(controller.handlerQueue == dispatch_get_main_queue(), "pad handler runs on the main queue, where GameController mutates the device state (#1819)");
    expect(pad.valueChangedHandler != nil, "gamepad valueChangedHandler installed");
    config = default_config(); keys = [NSMutableSet set]; mouse = [NSMutableSet set]; focused = YES;
    read_key_state = key_state; memset(physical_keys, 0, sizeof physical_keys); physical_keys[14] = YES;
    int32_t sticks[4];
    [keys addObject:@14]; sample_input();
    expect(s2a_vpad_read(sticks) == 1, "120Hz fallback publishes a held state without a callback");
    pad.valueChangedHandler((GCExtendedGamepad *)pad, nil);   // 没占到槽（FakeController 不在系统列表里）：快速路径跳过，不发布也不崩
    expect(s2a_vpad_read(sticks) == 1, "unslotted callback is a no-op");
    [keys removeObject:@14]; sample_input();
    expect(s2a_vpad_read(sticks) == 0, "release is visible after a sample");
    [keys addObject:@14]; poll_input();
    expect(s2a_vpad_read(sticks) == 1, "120Hz fallback publishes a held state without a callback");
    for (int i = 0; i < 120; i++) { poll_input(); pad.valueChangedHandler((GCExtendedGamepad *)pad, nil); }
    expect(s2a_vpad_read(sticks) == 1, "held state persists across the 120Hz fallback and no-op callbacks");
    physical_keys[14] = NO;
    reset_input();
    puts("Mac pad: handler installs on the main queue and 120Hz fallback keeps held state passed");
}

// 手柄快速路径（延迟管线调研·输入：GCController 权衡解除）。期望手算（缺省映射：手柄 A=东=位 0、B=南=位 1；键盘 Shift=B 位 1；E=A 位 0）：
//   ① 回调队列 = 主队列（#1819：系统在主线程改写真机手柄状态，回调读它必须同一条线程；A2 曾挂专用队列，红）；
//   ② 快速路径合成槽 0：键盘快照（位 1）+ 手柄 A（位 0）一起发布 = 3；松手后只剩键盘位（回调读当前整份状态，不是只发变化）；
//   ③ 已删（11-22 #1819）：原例「主线程忙时事件经专用队列照样发布」的前提不成立——系统处理真机手柄事件本身排在主线程，主线程忙时回调不会来；
//   ④ 玩家 2（槽 1）事件只写自己的槽位原子量并唤醒泵（改前：sample_input 采不到不在系统列表里的虚拟手柄 → 槽 1 恒 0，红）；
//   ⑤ 失焦静音：快速路径发零。
//   系统虚拟手柄（controllerWithExtendedGamepad）不在 GCController.controllers 里，槽位镜像由测试照 sample_input
//   写回同构放进去（同 test_pad_slots 的 apply_slots 注释）；setValue 只改读数不触发回调（本机实测，改前改后同一测法），
//   事件 = 照 GameController 的投递方式经 handlerQueue 调回调块本身。
static void slot_fake_pad(int slot, GCController *c) {
    pthread_mutex_lock(&g_compose_mu); pad_slot_owner[slot] = c; pthread_mutex_unlock(&g_compose_mu);
}
static void test_pad_fast_path(void) {
    typedef void (*read_ex_fn)(s2a_pad_snapshot *);
    read_ex_fn read_ex = (read_ex_fn)dlsym(RTLD_DEFAULT, "s2a_vpad_read_ex");
    expect(read_ex != NULL, "Mac exports s2a_vpad_read_ex");
    config = default_config(); keys = [NSMutableSet set]; mouse = [NSMutableSet set]; focused = YES;
    read_key_state = key_state; memset(physical_keys, 0, sizeof physical_keys);
    s2a_pad_set_active(0); s2a_pad_set_active(1);   // 清上一局留下的源选择
    reset_input();

    // ① 回调队列 = 主队列（#1819）
    GCController *pad0 = [GCController controllerWithExtendedGamepad];
    install_pad_handler(pad0);
    expect(pad0.handlerQueue == dispatch_get_main_queue(), "pad handler stays on the main queue (#1819)");
    { GCController *pp = [GCController controllerWithExtendedGamepad]; pp.handlerQueue = dispatch_queue_create("t.other", DISPATCH_QUEUE_SERIAL);
      pp.extendedGamepad.valueChangedHandler = nil; install_pad_handler(pp);
      expect(pp.handlerQueue == dispatch_get_main_queue(), "a handler queue set elsewhere is moved back to the main queue (#1819)"); }
    sample_input();   // 刷新键盘 / 鼠标快照与静音标记（此刻全零）
    slot_fake_pad(0, pad0);

    // ② 快速路径合键盘快照：Shift（位 1）+ 手柄 A（位 0）
    [keys addObject:@56]; modifiers = 1; physical_keys[60] = YES;
    sample_input(); slot_fake_pad(0, pad0);   // sample_input 整表重建镜像（虚拟手柄不在列表里会被清掉）：重放回去
    s2a_pad_snapshot snap; read_ex(&snap);
    expect(snap.buttons == 2, "keyboard shift alone maps to B");
    [pad0.extendedGamepad.buttonA setValue:1];
    pad0.extendedGamepad.valueChangedHandler(pad0.extendedGamepad, pad0.extendedGamepad.buttonA);
    read_ex(&snap);
    expect(snap.buttons == 3, "fast path composes the keyboard snapshot with the pad: B|A");
    [pad0.extendedGamepad.buttonA setValue:0];
    pad0.extendedGamepad.valueChangedHandler(pad0.extendedGamepad, pad0.extendedGamepad.buttonA);
    read_ex(&snap);
    expect(snap.buttons == 2, "release clears the pad bit and keeps the keyboard bit");
    [keys removeObject:@56]; modifiers = 0; physical_keys[60] = NO; sample_input(); slot_fake_pad(0, pad0);
    read_ex(&snap);
    expect(snap.buttons == 0, "keyboard release is visible after a sample");

    // ④ 玩家 2（槽 1）：事件只写自己的槽位原子量并唤醒泵
    GCController *pad1 = [GCController controllerWithExtendedGamepad];
    install_pad_handler(pad1);
    slot_fake_pad(1, pad1);
    [pad1.extendedGamepad.buttonB setValue:1];
    read_ex(&snap); uint64_t seq1 = snap.notification_seq;
    dispatch_async(pad1.handlerQueue, ^{ pad1.extendedGamepad.valueChangedHandler(pad1.extendedGamepad, pad1.extendedGamepad.buttonB); });
    run_loop_for(0.05);   // 改前排主队列：跑 runloop 才执行（红）；改后排专用队列：早执行完
    read_ex(&snap);
    expect(snap.buttons == 0, "player 2 events do not leak into the player 1 snapshot");
    int32_t ps[4];
    expect(s2a_vpad_read_pad(1, ps) == 2, "player 2 slot atomics carry the pad B bit");
    expect(snap.notification_seq != seq1, "player 2 event wakes the HID pump");
    [pad1.extendedGamepad.buttonB setValue:0];
    dispatch_async(pad1.handlerQueue, ^{ pad1.extendedGamepad.valueChangedHandler(pad1.extendedGamepad, pad1.extendedGamepad.buttonB); });
    run_loop_for(0.05);

    // ⑤ 失焦静音：快速路径发零
    focused = NO; input_mute_refresh(); sample_input(); slot_fake_pad(0, pad0);
    [pad0.extendedGamepad.buttonA setValue:1];
    pad0.extendedGamepad.valueChangedHandler(pad0.extendedGamepad, pad0.extendedGamepad.buttonA);
    read_ex(&snap);
    expect(snap.buttons == 0, "muted fast path publishes zeros");
    focused = YES; input_mute_refresh(); sample_input();
    read_ex(&snap);
    expect(snap.buttons == 0, "refocus keeps input neutral until the next event");
    [pad0.extendedGamepad.buttonA setValue:0];
    reset_input();
    puts("Mac pad: main-queue fast path, player 2 slot and mute passed");
}

// 关了「切到别的窗口时暂停」（S2A_FOCUS_PAUSE=0，#1606 ④）时失焦：手柄照常操作游戏，键盘 / 鼠标清空。期望手算（缺省映射：手柄 A = 位 0；键盘 Shift = B = 位 1）：
//   ① 缺省（暂停开）失焦：手柄 A 也发零（同 test_pad_fast_path ⑤）；
//   ② 暂停关失焦：键盘按着 Shift、手柄按 A → 只剩手柄位 = 1（改前：失焦一律静音 = 0，红）；pad.c 仍 active；
//   ③ 暂停关、回到焦点：键盘位回来 = 3
static void test_pad_unfocused_when_pause_off(void) {
    typedef void (*read_ex_fn)(s2a_pad_snapshot *);
    read_ex_fn read_ex = (read_ex_fn)dlsym(RTLD_DEFAULT, "s2a_vpad_read_ex");
    config = default_config(); keys = [NSMutableSet set]; mouse = [NSMutableSet set];
    read_key_state = key_state; memset(physical_keys, 0, sizeof physical_keys);
    reset_input(); s2a_pad_set_active(0); s2a_pad_set_active(1);
    GCController *pad0 = [GCController controllerWithExtendedGamepad];
    install_pad_handler(pad0);
    s2a_pad_snapshot snap;
    int saved = g_focus_pause_off;
    for (int off = 0; off <= 1; off++) {
        g_focus_pause_off = off;
        focused = NO; input_mute_refresh(); s2a_pad_set_active(pads_live());
        [keys addObject:@56]; modifiers = 1; physical_keys[60] = YES;   // 失焦时还按着 Shift（别的 App 里按的也一样）
        sample_input(); slot_fake_pad(0, pad0);
        [pad0.extendedGamepad.buttonA setValue:1];
        pad0.extendedGamepad.valueChangedHandler(pad0.extendedGamepad, pad0.extendedGamepad.buttonA);
        read_ex(&snap);
        if (!off) expect(snap.buttons == 0, "pause on (default): unfocused pad is muted");
        else expect(snap.buttons == 1, "pause off: unfocused pad keeps playing, keyboard cleared (#1606)");
        if (off) {
            focused = YES; input_mute_refresh(); s2a_pad_set_active(1); sample_input(); slot_fake_pad(0, pad0);
            pad0.extendedGamepad.valueChangedHandler(pad0.extendedGamepad, pad0.extendedGamepad.buttonA);
            read_ex(&snap);
            expect(snap.buttons == 3, "pause off, focus back: keyboard and pad both count");
        }
        [pad0.extendedGamepad.buttonA setValue:0];
        pad0.extendedGamepad.valueChangedHandler(pad0.extendedGamepad, pad0.extendedGamepad.buttonA);
        [keys removeObject:@56]; modifiers = 0; physical_keys[60] = NO;
        focused = YES; input_mute_refresh(); s2a_pad_set_active(1); sample_input(); reset_input();
    }
    g_focus_pause_off = saved;
    puts("Mac pad: pause-off keeps the pad live when unfocused, keyboard cleared passed");
}

// 延迟读数（--pad-latency-probe 手动跑，不进门禁）：事件投递 → 回调执行 → pad.c 发布的毫秒读数，
//   同一测法改前改后各跑一遍（改前 = 负对照构建）。设备无关的判据在 test_pad_fast_path（截止 200ms < 忙 300ms），
//   这里只报数：主线程忙 0 / 100 / 300 / 1000ms 各 3 轮，dispatch→回调执行的耗时（排主队列的改前 ≈ 忙的时长）、
//   回调体自身耗时（快速路径采样 + 发布）与发布后的按键位。
static void pad_latency_probe(void) {
    typedef void (*read_ex_fn)(s2a_pad_snapshot *);
    read_ex_fn read_ex = (read_ex_fn)dlsym(RTLD_DEFAULT, "s2a_vpad_read_ex");
    expect(read_ex != NULL, "Mac exports s2a_vpad_read_ex");
    config = default_config(); keys = [NSMutableSet set]; mouse = [NSMutableSet set]; focused = YES;
    read_key_state = key_state; memset(physical_keys, 0, sizeof physical_keys);
    s2a_pad_set_active(0); s2a_pad_set_active(1); reset_input();
    GCController *pad = [GCController controllerWithExtendedGamepad];
    install_pad_handler(pad);
    __block uint64_t exec_us = 0, exec_end_us = 0;
    int cases[] = {0, 100, 300, 1000};
    for (int round = 0; round < 3; round++)
        for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
            [pad.extendedGamepad.buttonA setValue:0];
            pad.extendedGamepad.valueChangedHandler(pad.extendedGamepad, pad.extendedGamepad.buttonA);
            focused = YES; input_mute_refresh(); sample_input();
            slot_fake_pad(0, pad);
            s2a_pad_snapshot snap; read_ex(&snap);   // 清 pending 时间戳
            [pad.extendedGamepad.buttonA setValue:1];
            exec_us = exec_end_us = 0;
            uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
            dispatch_async(pad.handlerQueue, ^{
                exec_us = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
                pad.extendedGamepad.valueChangedHandler(pad.extendedGamepad, pad.extendedGamepad.buttonA);
                exec_end_us = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
            });
            uint64_t spin = t0 + (uint64_t)cases[i] * 1000;
            while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000 < spin) {}   // 主线程忙转：不进 runloop、不排主队列
            run_loop_for(0.2);   // 改前排主队列：等 runloop 才执行；改后排专用队列：早执行完
            for (int n = 0; n < 1000 && !exec_end_us; n++) usleep(1000);
            s2a_pad_snapshot after; read_ex(&after);
            printf("pad-latency busy=%4dms round=%d dispatch→exec %.2fms exec-body %.3fms buttons=%u\n",
                   cases[i], round, (exec_us - t0) / 1000.0, (exec_end_us - exec_us) / 1000.0, after.buttons);
        }
    [pad.extendedGamepad.buttonA setValue:0];
    reset_input();
}

// 11-04 维护者定：鼠标左键点画面 = 触屏（对齐 Ryujinx TouchScreenManager）。期望手写：窗口 1600×720 点、无窗口缩放 1；
//   出图 16:9 → 画面在 x 160..1440（左右各 160 黑边），触点空间 1280×720，所以视图 (480, 360) → 触点 (320, 360)。
//   ① 左键映射成 Y（缺省映射）、模式「仅按键」：不写触点、照发 Y；② 左键没映射、模式「仅按键」：按下 Start (320,360)、拖到 (800,200) 跟点 (640,200)、抬起 End、之后 0 个；
//   ③ 点黑边 (100,360)：不写；④ 两次读之间按下又抬起：先 Start、下一拍 End；⑤ 直连模式（出图比例 16:9）同一矩形；⑥ 视角模式、左键没映射：点一下锁鼠标、不写触点；
//   ⑦ 按着时失焦：补一帧 End
static NSEvent *left_event(NSEventType type, NSPoint view_point, double view_h) {
    return [NSEvent mouseEventWithType:type location:NSMakePoint(view_point.x, view_h - view_point.y)   // 视图翻转（isFlipped），窗口坐标左下原点
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:1 clickCount:1 pressure:1];
}
static int read_touch(int32_t *x, int32_t *y, uint32_t *attr) { int32_t xy[2] = {-1, -1}; uint32_t a = 99; int n = s2a_vpad_read_touch(xy, &a); *x = xy[0]; *y = xy[1]; *attr = a; return n; }
static void test_mouse_touch(void) {
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSMakeRect(0, 0, 1600, 720)];
    int32_t x, y, sticks[4]; uint32_t a; NSPoint in = NSMakePoint(480, 360);
    fake_frame_w = 1280; fake_frame_h = 720; gfx_mode = S2A_GFX_GENERIC;
    NSMutableDictionary *c = [default_config() mutableCopy]; c[@"mouseOptions"] = @{@"mode": @"off", @"sensitivityX": @1, @"sensitivityY": @1};
    config = c; reset_input(); focused = YES; while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(read_touch(&x, &y, &a) == 0 && s2a_vpad_read(sticks) == (1u << 3), "① 左键映射成 Y：不写触点、照发 Y");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)];
    c[@"mouse"] = @{@"ZL": @1}; config = c; reset_input(); while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    int n = read_touch(&x, &y, &a);
    expect(n == 1 && x == 320 && y == 360 && a == 1 && s2a_vpad_read(sticks) == 0, "② 左键没映射：按下 = Start (320,360)、不发键");
    [view mouseDragged:left_event(NSEventTypeLeftMouseDragged, NSMakePoint(800, 200), 720)];
    n = read_touch(&x, &y, &a);
    expect(n == 1 && x == 640 && y == 200 && a == 0, "② 拖动跟点 (640,200)");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, NSMakePoint(800, 200), 720)];
    n = read_touch(&x, &y, &a);
    expect(n == 1 && x == 640 && y == 200 && a == 2, "② 抬起 = End");
    expect(read_touch(&x, &y, &a) == 0, "② End 之后 0 个触点");
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, NSMakePoint(100, 360), 720)];
    expect(read_touch(&x, &y, &a) == 0, "③ 点黑边：不写触点");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, NSMakePoint(100, 360), 720)];
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)]; [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)];
    int n1 = read_touch(&x, &y, &a); uint32_t a1 = a; int n2 = read_touch(&x, &y, &a);
    expect(n1 == 1 && a1 == 1 && n2 == 1 && a == 2 && read_touch(&x, &y, &a) == 0, "④ 快点：先 Start、下一拍 End");
    gfx_mode = S2A_GFX_DIRECT; fake_frame_w = 0;
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    n = read_touch(&x, &y, &a);
    expect(n == 1 && x == 320 && y == 360 && a == 1, "⑤ 直连模式（出图 16:9）同一矩形：(320,360)");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)]; while (read_touch(&x, &y, &a)) {}
    gfx_mode = S2A_GFX_GENERIC; fake_frame_w = 1280;
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    S2AMacDelegate *delegate = [S2AMacDelegate new];
    [delegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification object:nil]];
    n = read_touch(&x, &y, &a); int n3 = read_touch(&x, &y, &a); uint32_t a3 = a;
    expect(n == 1 && n3 == 1 && a3 == 2 && read_touch(&x, &y, &a) == 0, "⑦ 按着时失焦：补一帧 End");
    focused = YES;
    c[@"mouseOptions"] = @{@"mode": @"camera", @"sensitivityX": @1, @"sensitivityY": @1}; config = c; reset_input(); while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(mouse_captured && read_touch(&x, &y, &a) == 0, "⑥ 视角模式：点一下锁鼠标、不写触点");
    set_mouse_capture(NO);
    config = default_config(); reset_input(); gfx_mode = S2A_GFX_GENERIC; fake_frame_w = 0;
    puts("Mac mouse touch: unmapped left click Start/drag/End, letterbox, quick tap, direct rect, focus End; mapped / camera left click no touch passed");
}

// 11-04 维护者定（开箱即用）：游戏声明了触屏配置（s2a_vpad_touch_configured）且玩家没改过键位 → 左键点画面当触屏（不发 Y、不锁鼠标）；
//   玩家改过键位 → 照他的设置；没声明 → 照旧（视角模式点一下锁鼠标、锁住后左键发 Y）。期望手写，画面同 test_mouse_touch：视图 (480,360) → 触点 (320,360)。
//   「没改过」用启动器标准映射写出的 JSON（Models.swift InputConfig.standard 编码：多 schema、modifiers 0、invert false、recenter true）验按值比较
static NSDictionary *launcher_standard_json(void) {
    NSMutableDictionary *c = [default_config() mutableCopy]; c[@"schema"] = @1;
    NSMutableDictionary *k = [NSMutableDictionary dictionary];
    for (NSString *n in c[@"keyboard"]) k[n] = @{@"code": c[@"keyboard"][n][@"code"], @"modifiers": @0};
    c[@"keyboard"] = k;
    NSMutableDictionary *a = [NSMutableDictionary dictionary];
    for (NSString *n in c[@"axes"]) a[n] = @{@"source": c[@"axes"][n][@"source"], @"invert": @NO};
    c[@"axes"] = a;
    c[@"mouseOptions"] = @{@"mode": @"camera", @"sensitivityX": @1.0, @"sensitivityY": @1.0, @"invertX": @NO, @"invertY": @NO, @"recenter": @YES};
    return [NSJSONSerialization JSONObjectWithData:[NSJSONSerialization dataWithJSONObject:c options:0 error:nil] options:0 error:nil];   // 过一遍 JSON，同启动器落盘再读
}
static void test_mouse_touch_declared(void) {
    S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSMakeRect(0, 0, 1600, 720)];
    int32_t x, y, sticks[4]; uint32_t a; NSPoint in = NSMakePoint(480, 360);
    fake_frame_w = 1280; fake_frame_h = 720; gfx_mode = S2A_GFX_GENERIC; association_error = kCGErrorSuccess;
    atomic_store(&touch_declared, -1);
    config = default_config(); reset_input(); focused = YES; while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(mouse_captured && read_touch(&x, &y, &a) == 0, "没声明 + 缺省键位：照旧锁鼠标、不写触点");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)];
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(read_touch(&x, &y, &a) == 0 && s2a_vpad_read(sticks) == (1u << 3), "没声明 + 缺省键位：锁住后左键照旧发 Y");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)]; set_mouse_capture(NO);
    expect(s2a_vpad_touch_configured(2) == 1, "声明配置：收下返回 1");
    config = launcher_standard_json(); reset_input(); while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    int n = read_touch(&x, &y, &a);
    expect(n == 1 && x == 320 && y == 360 && a == 1 && !mouse_captured && s2a_vpad_read(sticks) == 0, "声明配置 + 缺省键位（启动器标准映射）：左键 = 触点 Start (320,360)、不发 Y、不锁鼠标");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)];
    expect(read_touch(&x, &y, &a) == 1 && a == 2, "声明配置 + 缺省键位：抬起 = End");
    // #1987 维护方定（11-22）：「玩家改没改过」只看左键的绑定和鼠标模式，别的键改了不算（所有游戏的默认改了一个手柄键、某款单独改了个按键都照样让给触屏）
    NSMutableDictionary *changed = [launcher_standard_json() mutableCopy]; NSMutableDictionary *kb = [changed[@"keyboard"] mutableCopy];
    kb[@"A"] = @{@"code": @36, @"modifiers": @0}; changed[@"keyboard"] = kb;   // 玩家把 A 改到回车
    config = changed; reset_input(); while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    n = read_touch(&x, &y, &a);
    expect(n == 1 && a == 1 && !mouse_captured && s2a_vpad_read(sticks) == 0, "声明配置 + 只改了键盘键（A → 回车）：左键和鼠标模式没动，仍当触屏、不发 Y、不锁鼠标");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)]; while (read_touch(&x, &y, &a)) {}
    NSMutableDictionary *pad = [launcher_standard_json() mutableCopy]; NSMutableDictionary *gp = [pad[@"gamepad"] mutableCopy];
    gp[@"A"] = @"south"; gp[@"B"] = @"east"; pad[@"gamepad"] = gp;   // 所有游戏的默认只换了一组手柄键（A / B 对调）
    config = pad; reset_input(); while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    n = read_touch(&x, &y, &a);
    expect(n == 1 && a == 1 && !mouse_captured && s2a_vpad_read(sticks) == 0, "声明配置 + 默认只改了手柄键：左键仍当触屏");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)]; while (read_touch(&x, &y, &a)) {}
    NSMutableDictionary *rebound = [launcher_standard_json() mutableCopy];
    rebound[@"mouse"] = @{@"ZR": @0, @"ZL": @1};   // 左键改绑到 ZR（鼠标模式仍是转动视角）
    config = rebound; reset_input(); while (read_touch(&x, &y, &a)) {}
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(mouse_captured && read_touch(&x, &y, &a) == 0, "声明配置 + 左键改绑成别的键：不接管（视角模式锁鼠标、不写触点）");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)];
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(read_touch(&x, &y, &a) == 0 && s2a_vpad_read(sticks) == (1u << 9), "声明配置 + 左键改绑成 ZR：锁住后左键发 ZR（0x200）");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)]; set_mouse_capture(NO);
    NSMutableDictionary *off = [launcher_standard_json() mutableCopy];
    off[@"mouse"] = @{@"ZL": @1}; off[@"mouseOptions"] = @{@"mode": @"off", @"sensitivityX": @1.0, @"sensitivityY": @1.0, @"invertX": @NO, @"invertY": @NO, @"recenter": @YES};
    config = off; reset_input(); while (read_touch(&x, &y, &a)) {}   // 鼠标模式改成「不起作用」、左键没绑
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    n = read_touch(&x, &y, &a);
    expect(n == 1 && a == 1 && !mouse_captured && s2a_vpad_read(sticks) == 0, "声明配置 + 鼠标模式不是转动视角、左键没绑：按规则 ② 接管（当触屏）");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)]; while (read_touch(&x, &y, &a)) {}
    NSMutableDictionary *offBound = [off mutableCopy]; offBound[@"mouse"] = @{@"Y": @0, @"ZL": @1};
    config = offBound; reset_input(); while (read_touch(&x, &y, &a)) {}   // 鼠标模式改了、左键仍是 Y：玩家动过鼠标用法 → 照映射发 Y
    [view mouseDown:left_event(NSEventTypeLeftMouseDown, in, 720)];
    expect(read_touch(&x, &y, &a) == 0 && !mouse_captured && s2a_vpad_read(sticks) == (1u << 3), "声明配置 + 鼠标模式改成不起作用、左键仍绑 Y：按玩家设置发 Y、不写触点");
    [view mouseUp:left_event(NSEventTypeLeftMouseUp, in, 720)];
    atomic_store(&touch_declared, -1); config = default_config(); reset_input(); fake_frame_w = 0;
    puts("Mac mouse touch declared: declared + default left button/mode touches (other keys changed too); rebound left / changed mode follow player; undeclared keeps camera/Y passed");
}
static NSDictionary *frozen_original_config(void) {
    return @{@"keyboard": @{@"A": @{@"code": @14}, @"B": @{@"code": @56}, @"X": @{@"code": @49},
                            @"L": @{@"code": @12}, @"R": @{@"code": @5}, @"ZR": @{@"code": @15},
                            @"LS": @{@"code": @59}, @"RS": @{@"code": @8}, @"PLUS": @{@"code": @48},
                            @"MINUS": @{@"code": @46}, @"LEFT": @{@"code": @123}, @"UP": @{@"code": @126},
                            @"RIGHT": @{@"code": @124}, @"DOWN": @{@"code": @125},
                            @"LS_UP": @{@"code": @13}, @"LS_DOWN": @{@"code": @1},
                            @"LS_LEFT": @{@"code": @0}, @"LS_RIGHT": @{@"code": @2}},
             @"mouse": @{@"Y": @0, @"ZL": @1},
             @"gamepad": @{@"A": @"east", @"B": @"south", @"X": @"north", @"Y": @"west",
                           @"L": @"leftShoulder", @"R": @"rightShoulder", @"ZL": @"leftTrigger", @"ZR": @"rightTrigger",
                           @"LS": @"leftStick", @"RS": @"rightStick", @"PLUS": @"menu", @"MINUS": @"options",
                           @"LEFT": @"dpadLeft", @"UP": @"dpadUp", @"RIGHT": @"dpadRight", @"DOWN": @"dpadDown"},
             @"axes": @{@"LX": @{@"source": @"leftX"}, @"LY": @{@"source": @"leftY"},
                         @"RX": @{@"source": @"rightX"}, @"RY": @{@"source": @"rightY"}}, @"deadzone": @0.15,
             @"mouseOptions": @{@"mode": @"camera", @"sensitivityX": @1, @"sensitivityY": @1}};
}

// CPU/Foundation-only slice. Calls the actual Mac adapter above and actual iOS
// file adapter (linked from ios/input_config.m), without creating NSApp,
// a Metal layer, a game process, or querying an attached input device.
static void test_shared_input_config(void) {
    unsigned n = 0;
#define CONFIG_EXPECT(x, why) do { n++; expect((x), (why)); } while (0)
    s2a_input_config mac, ios;
    CONFIG_EXPECT(!mac_input_config_read(NULL, &mac) && !s2a_ios_input_config_read(NULL, &ios), "no file selects shared defaults");
    CONFIG_EXPECT(!memcmp(&mac, &ios, sizeof mac), "two platform readers return identical default tables");
    CONFIG_EXPECT([mapping_norm(default_config()) isEqual:mapping_norm(frozen_original_config())], "all original Mac defaults remain semantically identical");
    NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    CONFIG_EXPECT([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL], "make private config fixture directory");
    NSString *path = [directory stringByAppendingPathComponent:@"input.json"];
    NSDictionary *base = @{@"schema":@1, @"keyboard":@{@"A":@{@"code":@49,@"modifiers":@3},@"B":@{@"code":@56}},
        @"mouse":@{@"Y":@0}, @"gamepad":@{@"A":@"east"}, @"axes":@{@"LX":@{@"source":@"rightY",@"invert":@YES}},
        @"deadzone":@0.2, @"mouseOptions":@{@"mode":@"camera",@"sensitivityX":@2,@"sensitivityY":@0.5}};
    NSArray *changes = @[
        @{@"expected":@0},
        @{@"schema":@2,@"expected":@(S2A_INPUT_CONFIG_SCHEMA)},
        @{@"schema":@"1",@"expected":@(S2A_INPUT_CONFIG_SCHEMA)},
        @{@"schema":@YES,@"expected":@(S2A_INPUT_CONFIG_SCHEMA)},
        @{@"keyboard":@"invalid",@"expected":@(S2A_INPUT_CONFIG_FIELDS)},
        @{@"keyboard":@{@"A":@{@"code":@"49"}},@"expected":@(S2A_INPUT_CONFIG_BINDING)},
        @{@"keyboard":@{@"A":@{@"code":@128}},@"expected":@(S2A_INPUT_CONFIG_BINDING)},
        @{@"keyboard":@{@"A":@{@"code":@49},@"B":@{@"code":@49}},@"expected":@(S2A_INPUT_CONFIG_DUPLICATE)},
        @{@"mouse":@{@"Y":@5},@"expected":@(S2A_INPUT_CONFIG_BINDING)},
        @{@"gamepad":@{@"A":@"invalid"},@"expected":@(S2A_INPUT_CONFIG_BINDING)},
        @{@"axes":@{@"LX":@{@"source":@"leftX",@"invert":@"yes"}},@"expected":@(S2A_INPUT_CONFIG_BINDING)},
        @{@"axes":@{@"LX":@{@"source":@"leftX"},@"RX":@{@"source":@"leftX"}},@"expected":@(S2A_INPUT_CONFIG_DUPLICATE)},
        @{@"deadzone":@1,@"expected":@(S2A_INPUT_CONFIG_RANGE)}
    ];
    for (NSDictionary *change in changes) {
        NSMutableDictionary *fixture = [base mutableCopy];
        for (NSString *key in change) if (![key isEqual:@"expected"]) fixture[key] = change[key];
        NSData *data = [NSJSONSerialization dataWithJSONObject:fixture options:0 error:NULL];
        CONFIG_EXPECT([data writeToFile:path atomically:YES], "write synthetic JSON fixture");
        memset(&mac,0x5a,sizeof mac); memset(&ios,0x5a,sizeof ios);
        int expected = [change[@"expected"] intValue];
        int a = mac_input_config_read(path.fileSystemRepresentation,&mac), b = s2a_ios_input_config_read(path.fileSystemRepresentation,&ios);
        CONFIG_EXPECT(a == expected && b == expected, "same hand-declared parse/error result on both platform adapters");
        CONFIG_EXPECT(!memcmp(&mac,&ios,sizeof mac), "byte-identical portable tables including failure-not-published cases");
        if (!expected) {
            NSDictionary *converted = input_config_dictionary(&mac);
            CONFIG_EXPECT(mac.keyboard[0].code == 49 && mac.keyboard[0].modifiers == 3 && mac.keyboard[1].code == 56, "handwritten custom physicalkey table");
            CONFIG_EXPECT(mac.mouse[3] == 0 && mac.gamepad[0] == 0 && mac.axes[0].source == 3 && mac.axes[0].invert, "handwritten mouse/gamepad/axis sources");
            CONFIG_EXPECT(mac.deadzone == .2 && mac.sensitivity_x == 2 && mac.sensitivity_y == .5 && mac.recenter, "options and omitted recenter default");
            CONFIG_EXPECT([converted[@"keyboard"][@"A"][@"code"] isEqual:@49] && [converted[@"axes"][@"LX"][@"source"] isEqual:@"rightY"], "Mac dictionary consumes actual decoded fields");
            CONFIG_EXPECT([mapping_norm(converted) isEqual:mapping_norm(base)], "UI/native dictionary bridge preserves supplied config semantics");
        }
    }
    CONFIG_EXPECT([[@"{" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:path atomically:YES], "write malformed JSON");
    CONFIG_EXPECT(mac_input_config_read(path.fileSystemRepresentation,&mac) == S2A_INPUT_CONFIG_JSON && s2a_ios_input_config_read(path.fileSystemRepresentation,&ios) == S2A_INPUT_CONFIG_JSON, "malformed JSON same error code");
    NSString *missing = [directory stringByAppendingPathComponent:@"not-created.json"];
    CONFIG_EXPECT(mac_input_config_read(missing.fileSystemRepresentation,&mac) == S2A_INPUT_CONFIG_IO && s2a_ios_input_config_read(missing.fileSystemRepresentation,&ios) == S2A_INPUT_CONFIG_IO, "file I/O errors match");
    NSData *data = [NSJSONSerialization dataWithJSONObject:base options:0 error:NULL];
    CONFIG_EXPECT([data writeToFile:path atomically:YES], "restore own valid fixture only");
    char savedNumericLocale[128]; snprintf(savedNumericLocale,sizeof savedNumericLocale,"%s",setlocale(LC_NUMERIC,NULL));
    for (const char **name = (const char *[]){"fr_FR.UTF-8","de_DE.UTF-8",NULL}; *name; name++) {
        if (!setlocale(LC_NUMERIC,*name)) continue;
        CONFIG_EXPECT(!mac_input_config_read(path.fileSystemRepresentation,&mac) && !s2a_ios_input_config_read(path.fileSystemRepresentation,&ios), "both platform readers accept locale-independent JSON decimals");
        CONFIG_EXPECT(mac.deadzone == .2 && ios.sensitivity_y == .5 && !memcmp(&mac,&ios,sizeof mac), "same numeric table under comma-decimal locale");
    }
    CONFIG_EXPECT(setlocale(LC_NUMERIC,savedNumericLocale) != NULL,"restore test process numeric locale");
    const char *priorDocs = getenv("S2A_DOCS"), *priorPath = getenv("S2A_INPUT_CONFIG");
    NSString *savedDocs = priorDocs ? @(priorDocs) : nil, *savedPath = priorPath ? @(priorPath) : nil;
    setenv("S2A_DOCS",directory.fileSystemRepresentation,1); setenv("S2A_INPUT_CONFIG","input.json",1);
    CONFIG_EXPECT(!s2a_ios_input_config_load(), "iOS shell loads relative imported config under Documents");
    const s2a_input_config *active = s2a_ios_input_config_current();
    CONFIG_EXPECT(active && active->keyboard[0].code == 49 && active->axes[0].source == 3, "iOS published table is the parsed custom mapping");
    CONFIG_EXPECT([[@"{" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:path atomically:YES], "replace own fixture with invalid JSON");
    // 11-17 维护者：读不懂不再拒启——仍回错误码（调用方黄字说明），这局换成默认键位，不沿用上一份映射
    { s2a_input_config fallback; s2a_input_config_default(&fallback);
      const s2a_input_config *now = (s2a_ios_input_config_load() == S2A_INPUT_CONFIG_JSON) ? s2a_ios_input_config_current() : NULL;
      CONFIG_EXPECT(now && !memcmp(now, &fallback, sizeof fallback), "failed iOS startup load falls back to the default mapping, not the previous one"); }
    if (savedDocs) setenv("S2A_DOCS",savedDocs.UTF8String,1); else unsetenv("S2A_DOCS");
    if (savedPath) setenv("S2A_INPUT_CONFIG",savedPath.UTF8String,1); else unsetenv("S2A_INPUT_CONFIG");
    CONFIG_EXPECT([NSFileManager.defaultManager removeItemAtPath:directory error:NULL], "remove only synthetic fixture directory created by this test");
    printf("input_config_apple details: %u/%u checks (actual Mac/iOS readers on macOS, no UI/device/game)\n",n,n);
#undef CONFIG_EXPECT
}
int main(int argc, const char **argv) {
    setenv("S2A_PAD_ORDER_FILE", "/nonexistent/s2arm-pad-order.json", 1);   // 不读本机分配过的顺序表（#520 E）
    setenv("S2A_PAD_FACE_FILE", "/nonexistent/s2arm-face.json", 1);
    // 兜底手柄体感（11-16 第三方 Switch 模式手柄）：0x30 报告第 13 字节起 3 组 IMU，取最后一组（13 + 24）。期望手算：
    //   加速度 x 原始 4096 × 0.000244 = 0.999424 g → 按 Pro 轴向落到输出 y；角速度 x 原始 1000 × 0.06103 = 61.03 度/秒 → 输出 gyro_y；z 原样
    {   uint8_t r[64] = {0x30}; const uint8_t *last = r + 13 + 24; (void)last;
        r[13 + 24 + 0] = 0x00; r[13 + 24 + 1] = 0x10;   // ax = 4096
        r[13 + 24 + 4] = 0x00; r[13 + 24 + 5] = 0xF0;   // az = -4096
        r[13 + 24 + 6] = 0xE8; r[13 + 24 + 7] = 0x03;   // gx = 1000
        s2a_pad_motion m = {0};
        expect(s2a_pro_imu(r, 49, &m) && fabsf(m.acceleration_y - 0.999424f) < 1e-5f && fabsf(m.acceleration_x) < 1e-6f && fabsf(m.acceleration_z + 0.999424f) < 1e-5f
               && fabsf(m.gyro_y - 61.03f) < 1e-3f && fabsf(m.gyro_x) < 1e-6f, "HID pad 0x30 report IMU (last sample) maps like the Pro");
        expect(!s2a_pro_imu(r, 48, &m) && !s2a_pro_imu((const uint8_t[64]){0x21}, 64, &m), "HID pad IMU needs a whole 0x30 report");
    }
    // 补满采样（#2146，macos/imu_burst.h）：一包 3 组 IMU，交给游戏的条数 × 5 ms 必须等于真实流逝、时间戳严格单调。期望全按包间隔手算：
    //   15 ms 一包 → 每包 3 条；8 ms 一包 11 包（跨 80 ms）→ 首包 3 + 80/5 = 19 条（照单全收会是 33 条、幅度 ×1.7）；
    //   1 ms 后扎堆的包 → 0 条，15 ms 处的下一包补满 3 条；断流 1 秒 → 只补 3 条不追帐；取走时比「现在」早 40 ms 以上的丢掉。g[i].gyro_x = 组号作标记
    {   s2a_imu_burst b = {0}; s2a_pad_motion g[3] = {{.gyro_x = 0}, {.gyro_x = 1}, {.gyro_x = 2}}, out[S2A_IMU_BURST_CAP];
        const uint64_t t0 = 1000000;
        expect(s2a_imu_burst_push(&b, g, t0) == 3 && s2a_imu_burst_push(&b, g, t0 + 15000) == 3, "IMU burst: 15 ms packets give 3 samples each");
        int n = s2a_imu_burst_drain(&b, out, S2A_IMU_BURST_CAP, t0 + 15000), mono = n == 6;
        for (int i = 0; i < n; i++) mono = mono && out[i].host_timestamp_us == t0 - 10000 + 5000ull * (uint64_t)i;
        expect(mono, "IMU burst: 15 ms packets are 5 ms apart, newest = receive time");
        s2a_imu_burst_reset(&b); int total = 0; uint64_t last = 0; mono = 1;
        for (int p = 0; p <= 10; p++) {
            total += s2a_imu_burst_push(&b, g, t0 + 8000ull * (uint64_t)p);
            n = s2a_imu_burst_drain(&b, out, S2A_IMU_BURST_CAP, t0 + 8000ull * (uint64_t)p);
            for (int i = 0; i < n; i++) { mono = mono && (!last || out[i].host_timestamp_us == last + 5000); last = out[i].host_timestamp_us; }
        }
        expect(total == 19 && mono && last == t0 + 80000, "IMU burst: 8 ms packets give elapsed / 5 ms samples, strictly 5 ms apart");
        s2a_imu_burst_reset(&b);
        int k0 = s2a_imu_burst_push(&b, g, t0), k1 = s2a_imu_burst_push(&b, g, t0 + 1000), k2 = s2a_imu_burst_push(&b, g, t0 + 15000);
        s2a_imu_burst_drain(&b, out, S2A_IMU_BURST_CAP, t0 + 15000);
        int k3 = s2a_imu_burst_push(&b, g, t0 + 25000);
        n = s2a_imu_burst_drain(&b, out, S2A_IMU_BURST_CAP, t0 + 25000);
        expect(k0 == 3 && k1 == 0 && k2 == 3 && k3 == 2 && n == 2 && out[0].gyro_x == 1 && out[1].gyro_x == 2 &&
               out[0].host_timestamp_us == t0 + 20000 && out[1].host_timestamp_us == t0 + 25000, "IMU burst: bunched packet waits, partial packet keeps the newest groups");
        expect(s2a_imu_burst_push(&b, g, t0 + 1025000) == 3 && s2a_imu_burst_drain(&b, out, S2A_IMU_BURST_CAP, t0 + 1025000) == 3 &&
               out[0].host_timestamp_us == t0 + 1015000, "IMU burst: after a 1 s gap only the packet's own 15 ms is given");
        s2a_imu_burst_reset(&b); s2a_imu_burst_push(&b, g, t0); s2a_imu_burst_push(&b, g, t0 + 15000);
        expect(s2a_imu_burst_drain(&b, out, 2, t0 + 15000) == 2 && b.len == 4, "IMU burst: drain stops at max");
        expect(s2a_imu_burst_drain(&b, out, S2A_IMU_BURST_CAP, t0 + 15000 + 37000) == 1 && out[0].host_timestamp_us == t0 + 15000 && b.len == 0,
               "IMU burst: samples older than 40 ms are dropped, not replayed");
    }
    // 兜底手柄（macos/hid_pad.m，#934）：Switch 简易 HID 键号 1 = B（南）、2 = A（东）、3 = Y（西）、4 = X（北）、9 = −、10 = +；13 Home 不进键位
    expect(s2a_hid_pad_switch_button_bit(1) == (1u << 1) && s2a_hid_pad_switch_button_bit(2) == (1u << 0) &&
           s2a_hid_pad_switch_button_bit(3) == (1u << 3) && s2a_hid_pad_switch_button_bit(4) == (1u << 2), "HID pad face buttons follow Switch labels");
    expect(s2a_hid_pad_switch_button_bit(9) == (1u << 11) && s2a_hid_pad_switch_button_bit(10) == (1u << 10) &&
           s2a_hid_pad_switch_button_bit(13) == 0 && s2a_hid_pad_switch_button_bit(0) == 0, "HID pad minus / plus, Home and out of range");
    // 十字键 hat 0..7（0 = 上，顺时针），8 = 松开；位：左 12、上 13、右 14、下 15
    expect(s2a_hid_pad_hat_bits(0, 0, 7) == (1u << 13) && s2a_hid_pad_hat_bits(3, 0, 7) == ((1u << 15) | (1u << 14)) &&
           s2a_hid_pad_hat_bits(8, 0, 7) == 0 && s2a_hid_pad_hat_bits(1, 1, 8) == (1u << 13), "HID pad hat switch to d-pad bits");
    // 轴：0..65535 中点约 0；Y 反向（HID 向下为正）
    // 走不走 Switch Pro 协议那一支（#934 11-15）：名字带 Pro Controller 就走，任天堂原厂 0x057e 也走（macOS 27 上原厂一代 Pro 手柄系统认不出）；
    //   别的手柄名走通用那一支（hid_generic.h，runtime/tests/t_hid_generic.c）；原厂一代 Pro 系统没列出时照旧占位、不等
    expect(s2a_hidg_is_pro("Pro Controller") && s2a_hidg_is_pro("PowerA Wireless Pro Controller") &&
           !s2a_hidg_is_pro("Xbox Wireless Controller") && !s2a_hidg_is_pro(NULL), "HID pad Pro protocol branch by name");
    expect(s2a_hidg_route(1, 0x057e, 1, 0, 0) == S2A_HIDG_TAKE, "HID pad fallback takes Nintendo's own Pro Controller at once");
    expect(s2a_hid_pad_axis(0, 0, 65535, 0) == -32767 && s2a_hid_pad_axis(65535, 0, 65535, 0) == 32767 &&
           s2a_hid_pad_axis(0, 0, 65535, 1) == 32767 && abs(s2a_hid_pad_axis(32768, 0, 65535, 0)) <= 1, "HID pad axis normalisation");
    // 兜底手柄六轴跟启动设置「体感」走（#1383：关了体感，GameController 不认的 Pro 仍把陀螺仪交给游戏）。期望手写
    expect(s2a_hid_pad_motion_ok(1, 1000000, 900000) && !s2a_hid_pad_motion_ok(0, 1000000, 900000),
           "HID pad motion passes only while motion is enabled");
    expect(!s2a_hid_pad_motion_ok(1, 1000000, 0) && !s2a_hid_pad_motion_ok(1, 1000000, 800000) && s2a_hid_pad_motion_ok(1, 1000000, 800001),
           "HID pad motion needs a sample younger than 200 ms");
    setenv("S2A_MOTION", "0", 1);
    s2a_pad_set_motion_enabled(1);   // 游戏要六轴，但启动设置关了体感
    expect(!s2a_mac_hid_pad_motion_enabled(), "motion off in launch settings also turns off the HID fallback pad's motion");
    unsetenv("S2A_MOTION");
    s2a_pad_set_motion_enabled(1);
    expect(s2a_mac_hid_pad_motion_enabled(), "HID fallback pad motion is on again when the setting allows it");
    s2a_pad_set_motion_enabled(0);
    expect(!s2a_mac_hid_pad_motion_enabled(), "game stops asking for motion: HID fallback pad motion off");
    {   // Switch Pro 协议（#934 11-15：PowerA 简易模式量程不对、摇杆慢五六倍；改发子命令切 0x30 完整报告）。期望全部手算：
        //   摇杆：中心 2048、行程 ±1600 → 3648 满、448 满负、2848 半程（800 / 1600 × 32767 = 16383.5 → 16383）
        expect(s2a_pro_axis(2048, 2048, 1600, 1600) == 0 && s2a_pro_axis(3648, 2048, 1600, 1600) == 32767 && s2a_pro_axis(448, 2048, 1600, 1600) == -32767 &&
               s2a_pro_axis(2848, 2048, 1600, 1600) == 16383 && s2a_pro_axis(4095, 2048, 1600, 1600) == 32767, "Pro stick 12-bit normalisation by calibration");
        //   校准：两个 12 位值 (a, b) 打成 3 字节 = a 低 8 位 | a 高 4 位 + b 低 4 位 | b 高 8 位。左 = 上行程 (0x5A0, 0x5B0)、中心 (0x7F0, 0x810)、下行程 (0x5C0, 0x5D0)；
        //   右 = 中心 (0x800, 0x7E0)、下行程 (0x600, 0x610)、上行程 (0x620, 0x630)
        const uint8_t cal[18] = {0xA0, 0x05, 0x5B, 0xF0, 0x07, 0x81, 0xC0, 0x05, 0x5D, 0x00, 0x08, 0x7E, 0x00, 0x06, 0x61, 0x20, 0x06, 0x63};
        s2a_pro_cal l, r; int ok = s2a_pro_cal_parse(cal, &l, &r);
        expect(ok && l.calibrated && r.calibrated && l.cx == 0x7F0 && l.cy == 0x810 && l.xmax == 0x5A0 && l.ymax == 0x5B0 && l.xmin == 0x5C0 && l.ymin == 0x5D0 &&
               r.cx == 0x800 && r.cy == 0x7E0 && r.xmax == 0x620 && r.ymax == 0x630 && r.xmin == 0x600 && r.ymin == 0x610, "Pro factory stick calibration (left / right field orders differ)");
        uint8_t blank[18]; memset(blank, 0xFF, sizeof blank); s2a_pro_cal keep; s2a_pro_cal_default(&keep);
        expect(!s2a_pro_cal_parse(blank, &keep, &keep) && keep.cx == 2048 && !keep.calibrated, "Pro calibration left unwritten keeps the default");
        uint8_t bad_cal[18]; memcpy(bad_cal, cal, sizeof bad_cal); bad_cal[3] = 0x00; bad_cal[4] = 0x01;   // 左 X 中心改成 0x100，超出合理范围
        s2a_pro_cal bad_left, bad_right; s2a_pro_cal_default(&bad_left); s2a_pro_cal_default(&bad_right);
        expect(!s2a_pro_cal_parse(bad_cal, &bad_left, &bad_right) && !bad_left.calibrated && bad_left.cx == 2048,
               "Pro unreasonable factory calibration falls back to growing defaults");
        // 先来一帧 +1800 尖峰（0x7F0 + 0x708 = 0xDF8），再推到出厂满行程 +1440（0xD90）。
        // 改后：尖峰截到 +32767、行程仍 0x5A0；随后满行程也是 +32767。改前：尖峰把行程撑到 1800，满行程只有 1440/1800×32767 = 26213。
        const uint8_t spike_rep[12] = {0x30, 0, 0, 0, 0, 0, 0xF8, 0x0D, 0x81, 0x00, 0xE8, 0x7E};
        const uint8_t full_rep[12] = {0x30, 0, 0, 0, 0, 0, 0x90, 0x0D, 0x81, 0x00, 0xE8, 0x7E};
        uint32_t factory_bt = 0; int16_t factory_ax[4] = {0};
        expect(s2a_pro_parse(spike_rep, sizeof spike_rep, &l, &r, &factory_bt, factory_ax) == 1 &&
               factory_ax[0] == 32767 && l.xmax == 0x5A0 && l.xmin == 0x5C0,
               "Pro factory X calibration is fixed and never grows from a spike");
        expect(s2a_pro_parse(full_rep, sizeof full_rep, &l, &r, &factory_bt, factory_ax) == 1 &&
               factory_ax[0] == 32767 && l.xmax == 0x5A0,
               "Pro factory X full travel stays full after an earlier spike");
        // cx != cy：左 Y 中心 0x810 + 半行程 0x2D8 = 0xAE8，正确应为 16383；误用 X 中心 / 行程会得到约 23093。
        const uint8_t y_half_rep[12] = {0x30, 0, 0, 0, 0, 0, 0xF0, 0x8F, 0xAE, 0x00, 0xE8, 0x7E};
        expect(s2a_pro_parse(y_half_rep, sizeof y_half_rep, &l, &r, &factory_bt, factory_ax) == 1 &&
               factory_ax[1] == 16383 && l.ymax == 0x5B0 && l.ymin == 0x5D0,
               "Pro factory Y calibration uses Y center and travel when cx differs from cy");
        //   0x30 报告：A + ZR（字节 3 = 0x88）、+（字节 4 = 0x02）、上 + L（字节 5 = 0x42）；左摇杆 (3648, 2048) = 40 0E 80，右摇杆 (448, 3648) = C0 01 E4
        const uint8_t rep[12] = {0x30, 0, 0, 0x88, 0x02, 0x42, 0x40, 0x0E, 0x80, 0xC0, 0x01, 0xE4};
        s2a_pro_cal d; s2a_pro_cal_default(&d); uint32_t bt = 0; int16_t ax[4] = {0};
        expect(s2a_pro_parse(rep, sizeof rep, &d, &d, &bt, ax) == 1 && bt == ((1u << 0) | (1u << 9) | (1u << 10) | (1u << 13) | (1u << 6)) &&
               ax[0] == 32767 && ax[1] == 0 && ax[2] == -32767 && ax[3] == 32767, "Pro 0x30 report: buttons and sticks");
        uint8_t simple[12]; memcpy(simple, rep, sizeof simple); simple[0] = 0x3F;
        expect(s2a_pro_parse(simple, sizeof simple, &d, &d, &bt, ax) == 0, "Pro parser leaves the simple HID report (0x3F) to the element path");
        //   读不到出厂校准（#1264 PowerA）：缺省行程 1003、随读数放大。期望手算：+500 → 500 / 1003 × 32767 = 16334.6 → 16334；
        //   推到 +1200（超过 1003）→ 行程放大到 1200、满量程 32767；之后 +600 → 600 / 1200 × 32767 = 16383.5 → 16383；
        //   负向没动过：-700 → -700 / 1003 × 32767 = -22868.3 → -22868（行程两向分开记）
        s2a_pro_cal g; s2a_pro_cal_default(&g);
        int16_t g1 = s2a_pro_axis_grow(2548, g.cx, &g.xmax, &g.xmin), g2 = s2a_pro_axis_grow(3248, g.cx, &g.xmax, &g.xmin);
        int16_t g3 = s2a_pro_axis_grow(2648, g.cx, &g.xmax, &g.xmin), g4 = s2a_pro_axis_grow(1348, g.cx, &g.xmax, &g.xmin);
        expect(g1 == 16334 && g2 == 32767 && g.xmax == 1200 && g3 == 16383 && g4 == -22868 && g.xmin == 1003,
               "Pro stick without factory calibration: range starts at 1003 and grows to the farthest reading");
        expect(s2a_pro_player_lights(0) == 0x1 && s2a_pro_player_lights(1) == 0x3 && s2a_pro_player_lights(3) == 0xF, "Pro player lights: first n lit for player n");
        // Xbox 有线手柄 GIP 协议纯函数验证（#1406）
        expect(s2a_is_xbox_gip(0x045E, 0x02D1) == 1, "Xbox GIP: 0x045E:0x02D1 (Xbox One)");
        expect(s2a_is_xbox_gip(0x045E, 0x0B12) == 1, "Xbox GIP: 0x045E:0x0B12 (Xbox Series)");
        expect(s2a_is_xbox_gip(0x045E, 0x028E) == 0, "Xbox GIP: 0x045E:0x028E (Xbox 360 XUSB, 非 GIP) 返回 0");
        expect(s2a_usb_has_hid_child(0) == 0, "USB has HID child: 空句柄返回 0");
        expect(s2a_is_xbox_gip(0x045E, 0x9999) == 0, "Xbox GIP: 非 Xbox 手柄 PID 返回 0");
        expect(s2a_is_xbox_gip(0x054C, 0x0CE6) == 0, "Xbox GIP: 非微软 VID 返回 0");

        uint8_t ack_pkt[13];
        const uint8_t req_ack[4] = {0x02, 0x10, 0x05, 0x01};
        expect(s2a_gip_ack(req_ack, sizeof req_ack, ack_pkt) == 13 &&
               ack_pkt[0] == 0x01 && ack_pkt[1] == 0x20 && ack_pkt[2] == 0x05 && ack_pkt[5] == 0x02,
               "Xbox GIP ACK: 置有 ACK 标志时正确生成 13 字节应答包");
        const uint8_t no_ack[4] = {0x20, 0x00, 0x05, 0x01};
        expect(s2a_gip_ack(no_ack, sizeof no_ack, ack_pkt) == 0, "Xbox GIP ACK: 无 ACK 标志返回 0");

        const uint8_t gip_rep[18] = {
            0x20, 0x00, 0x01, 0x0E,
            0x14, 0x10,             // buttons: Xbox A (0x0010), Menu (+, 0x0004), LB (0x1000)
            0x00, 0x01,             // LT: 256 (> 200 -> ZL)
            0x00, 0x00,             // RT: 0
            0x00, 0x40,             // lx: 16384
            0x00, 0xC0,             // ly: -16384
            0x00, 0x00,             // rx: 0
            0x00, 0x00              // ry: 0
        };
        uint32_t gip_bt = 0; int16_t gip_ax[4] = {0};
        expect(s2a_gip_parse(gip_rep, sizeof gip_rep, &gip_bt, gip_ax) == 1, "Xbox GIP parse: valid 0x20 report");
        expect(gip_bt == ((1u << 0) | (1u << 10) | (1u << 6) | (1u << 8)), "Xbox GIP parse buttons: A, +, L, ZL");
        expect(gip_ax[0] == 16384 && gip_ax[1] == -16384 && gip_ax[2] == 0 && gip_ax[3] == 0, "Xbox GIP parse axes: lx, ly, rx, ry");
        uint8_t invalid_gip[18]; memcpy(invalid_gip, gip_rep, sizeof invalid_gip); invalid_gip[0] = 0x07;
        expect(s2a_gip_parse(invalid_gip, sizeof invalid_gip, &gip_bt, gip_ax) == 0, "Xbox GIP parse: non-0x20 report returns 0");

        // Xbox GIP 启动器中继协议与宿主接收端验证（不碰物理 USB）
        s2a_mac_hid_pad_clear_xbox_relay();
        s2a_xbox_gip_relay_packet rp;
        memset(&rp, 0, sizeof rp);
        rp.magic = S2A_XBOX_GIP_RELAY_MAGIC;
        rp.version = S2A_XBOX_GIP_RELAY_VERSION;
        rp.flags = S2A_XBOX_GIP_FLAG_CONNECTED;
        rp.slot = 0;
        rp.vid = 0x045E;
        rp.pid = 0x02D1;
        strlcpy(rp.name, "Xbox Relay Test", sizeof rp.name);
        expect(s2a_xbox_gip_relay_packet_valid(&rp) == 1, "Xbox GIP relay: valid packet");
        rp.magic = 0;
        expect(s2a_xbox_gip_relay_packet_valid(&rp) == 0 && s2a_mac_hid_pad_apply_xbox_relay(&rp, sizeof rp) == 0, "Xbox GIP relay: bad magic rejected");
        rp.magic = S2A_XBOX_GIP_RELAY_MAGIC;
        rp.slot = S2A_XBOX_GIP_RELAY_MAX;
        expect(s2a_xbox_gip_relay_packet_valid(&rp) == 0 && s2a_mac_hid_pad_apply_xbox_relay(&rp, sizeof rp) == 0, "Xbox GIP relay: out-of-range slot rejected");
        rp.slot = 0;

        // 刚连上未按键（buttons == 0）保持 joined=0，不占玩家位
        expect(s2a_mac_hid_pad_apply_xbox_relay(&rp, sizeof rp) == 1, "Xbox GIP relay: apply idle connected packet");
        s2a_hid_pad hp_out[S2A_HID_PAD_MAX];
        int hp_n = s2a_mac_hid_pad_read(hp_out);
        int found_relay = 0;
        for (int i = 0; i < hp_n; i++) if (!strcmp(hp_out[i].name, "Xbox Relay Test")) found_relay = 1;
        expect(!found_relay, "Xbox GIP relay: idle controller does not join before first button press");

        // 按下 A 键并推摇杆 → 立即加入玩家位并读到按键 / 摇杆
        rp.buttons = (1u << 0) | (1u << 6);
        rp.ax[0] = 12345; rp.ax[1] = -23456; rp.ax[2] = 1000; rp.ax[3] = -1000;
        expect(s2a_mac_hid_pad_apply_xbox_relay(&rp, sizeof rp) == 1, "Xbox GIP relay: apply active button packet");
        hp_n = s2a_mac_hid_pad_read(hp_out);
        found_relay = 0;
        for (int i = 0; i < hp_n; i++) {
            if (!strcmp(hp_out[i].name, "Xbox Relay Test")) {
                found_relay = 1;
                expect(hp_out[i].buttons == ((1u << 0) | (1u << 6)) && hp_out[i].lx == 12345 && hp_out[i].ly == -23456 && hp_out[i].vid == 0x045E,
                       "Xbox GIP relay: read returns relayed buttons, sticks and vid");
            }
        }
        expect(found_relay == 1, "Xbox GIP relay: joined controller listed in hid_pad_read");

        // 模拟两局游戏依次通过 socketpair 读取并在断开时自动清槽
        s2a_mac_hid_pad_clear_xbox_relay();
        for (int session = 0; session < 2; session++) {
            int sp[2] = {-1, -1};
            expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "Xbox GIP relay: socketpair");
            rp.flags = S2A_XBOX_GIP_FLAG_CONNECTED;
            rp.buttons = (session == 0) ? (1u << 0) : (1u << 1);
            rp.ax[0] = (int16_t)(session == 0 ? 5000 : -5000);
            expect(s2a_xbox_gip_write_all(sp[0], &rp, sizeof rp) == 0, "Xbox GIP relay: write packet to game fd");
            close(sp[0]);
            expect(s2a_mac_hid_pad_read_xbox_relay_fd(sp[1]) == 0, "Xbox GIP relay: read_fd drains and clears on EOF");
            close(sp[1]);
            hp_n = s2a_mac_hid_pad_read(hp_out);
            found_relay = 0;
            for (int i = 0; i < hp_n; i++) if (!strcmp(hp_out[i].name, "Xbox Relay Test")) found_relay = 1;
            expect(!found_relay, "Xbox GIP relay: EOF clears relay slot cleanly");
        }

        // Xbox GIP 异常自愈与重唤醒纯函数验证（重通告 0x02、递增 seq、IO 错误分类、空闲保活节流）
        const uint8_t ann_pkt[4] = {0x02, 0x20, 0x01, 0x1c};
        expect(s2a_xbox_gip_is_announce(ann_pkt, sizeof ann_pkt) == 1, "Xbox GIP: 0x02 packet recognized as announce");
        expect(s2a_xbox_gip_is_announce(gip_rep, sizeof gip_rep) == 0, "Xbox GIP: 0x20 input report is not announce");
        expect(s2a_xbox_gip_is_announce(ann_pkt, 3) == 0, "Xbox GIP: truncated 0x02 packet rejected");

        uint8_t seq = 254;
        expect(s2a_xbox_gip_next_seq(&seq) == 255 && seq == 255, "Xbox GIP seq: 254 -> 255");
        expect(s2a_xbox_gip_next_seq(&seq) == 1 && seq == 1, "Xbox GIP seq: 255 wraps to 1 (skips 0)");
        uint8_t pwr[5] = {0}, wak[5] = {0};
        s2a_xbox_gip_build_power(7, pwr);
        s2a_xbox_gip_build_wake(8, wak);
        expect(pwr[0] == 0x05 && pwr[1] == 0x20 && pwr[2] == 7 && pwr[3] == 0x01 && pwr[4] == 0x00, "Xbox GIP build_power packet");
        expect(wak[0] == 0x05 && wak[1] == 0x20 && wak[2] == 8 && wak[3] == 0x0f && wak[4] == 0x06, "Xbox GIP build_wake packet");

        uint32_t rec_errs = 0;
        expect(s2a_xbox_gip_classify_io_error(0xe000404fu, &rec_errs, 2) == S2A_XBOX_GIP_IO_RECOVER && rec_errs == 1, "Xbox GIP IO: 1st pipe stall recoverable");
        expect(s2a_xbox_gip_classify_io_error(0xe00002edu, &rec_errs, 2) == S2A_XBOX_GIP_IO_RECOVER && rec_errs == 2, "Xbox GIP IO: 2nd overrun recoverable");
        expect(s2a_xbox_gip_classify_io_error(0xe0004051u, &rec_errs, 2) == S2A_XBOX_GIP_IO_FATAL && rec_errs == 3, "Xbox GIP IO: exceeding max_recoverable turns fatal");
        expect(s2a_xbox_gip_classify_io_error(0, &rec_errs, 2) == S2A_XBOX_GIP_IO_OK && rec_errs == 0, "Xbox GIP IO: success resets recoverable error count");
        expect(s2a_xbox_gip_classify_io_error(0xe00002c0u, &rec_errs, 2) == S2A_XBOX_GIP_IO_FATAL, "Xbox GIP IO: kIOReturnNoDevice is immediately fatal");

        expect(s2a_xbox_gip_idle_should_rewake(5000, 1000, 1000, 10000, 10000) == 0, "Xbox GIP idle: below threshold does not rewake");
        expect(s2a_xbox_gip_idle_should_rewake(11000, 1000, 0, 10000, 10000) == 1, "Xbox GIP idle: past threshold triggers rewake");
        expect(s2a_xbox_gip_idle_should_rewake(15000, 1000, 11000, 10000, 10000) == 0, "Xbox GIP idle: throttled within rewake_interval");
        expect(s2a_xbox_gip_idle_should_rewake(21000, 1000, 11000, 10000, 10000) == 1, "Xbox GIP idle: triggers again after rewake_interval");
    }
    char relayPath[96];
    snprintf(relayPath, sizeof relayPath, "/tmp/s2arm-motion-%d.sock", getpid());
    struct sockaddr_un relayAddress = {.sun_family = AF_UNIX};
    strcpy(relayAddress.sun_path, relayPath);
    int relayListener = socket(AF_UNIX, SOCK_STREAM, 0);
    expect(bind(relayListener, (struct sockaddr *)&relayAddress, sizeof relayAddress) == 0, "motion relay bind");
    expect(listen(relayListener, 1) == 0, "motion relay listen");
    // 开局后才连手柄（11-11 实机：先开游戏、再在 S2Arm 连 Joy-Con 2，游戏一直收不到输入）：通道文件不在不连、出现后连；已接上 / 非 IDLE 不重复连
    char missingRelay[96];
    snprintf(missingRelay, sizeof missingRelay, "/tmp/s2arm-missing-%d.sock", getpid());
    expect(!s2a_mac_ns2_relay_due(NS2_STATUS_IDLE, missingRelay), "relay not due before the S2Arm connection program creates the socket");
    expect(s2a_mac_ns2_relay_due(NS2_STATUS_IDLE, relayPath), "relay due once the socket exists and the game never connected");
    expect(!s2a_mac_ns2_relay_due(NS2_STATUS_READY, relayPath) && !s2a_mac_ns2_relay_due(NS2_STATUS_CONNECTING, relayPath),
           "relay not reconnected while connected or connecting");
    expect(!s2a_mac_ns2_relay_due(NS2_STATUS_IDLE, NULL) && !s2a_mac_ns2_relay_due(NS2_STATUS_IDLE, ""), "no relay path: keyboard-only launch never connects");
    setenv("S2A_NS2_SOCKET", relayPath, 1);
    s2a_mac_ns2_focus(1);
    expect(s2a_mac_ns2_connect(), "Switch 2 Pro motion relay connects");
    int relayClient = accept(relayListener, NULL, NULL);
    s2a_pad_set_motion_enabled(1);
    s2a_mac_ns2_set_game_style(1 << 4);   // 游戏用 JoyconRight：单只右 Joy-Con 的六轴原样给（#520 A）
    ns2_relay_packet motionPacket = {
        .magic = NS2_RELAY_MAGIC, .version = NS2_RELAY_VERSION, .type = NS2_RELAY_INPUT,
        .ready = 1, .status = NS2_STATUS_READY, .motion_known = 1, .kind = NS2_KIND_JOYCON_R,
        .motion = {.known = 1, .host_timestamp_us = 1234, .gyro_x = 90, .acceleration_z = 1},
    };
    expect(send(relayClient, &motionPacket, sizeof motionPacket, 0) == sizeof motionPacket, "motion relay publishes");
    s2a_pad_motion ns2Motion = {0};
    for (int attempt = 0; attempt < 100 && !s2a_pad_read_motion(&ns2Motion); attempt++) usleep(1000);
    expect(ns2Motion.host_timestamp_us == 1234 && ns2Motion.gyro_x == 90 && ns2Motion.acceleration_z == 1,
            "Switch 2 Pro motion reaches shared Mac six-axis entry");
    {
        FakePad *p=[FakePad new]; FakeController *c=fake_controller(p);
        FakeProSensor *m=[FakeProSensor new]; m.sensorsRequireManualActivation=YES; m.hasRotationRate=YES; c.motion=m;
        sample_motion((GCExtendedGamepad *)p);
        expect(!m.sensorsActive,"NS2 lead transport does not activate an unrelated system motion source");
    }
    // #520 A：游戏不用单只样式（扮 Pro）时，单只右 Joy-Con 的六轴在游戏端转成横握视角：(ax, az) → (−az, ax)、(gx, gz) → (−gz, gx)
    s2a_mac_ns2_set_game_style(1 << 0);
    expect(s2a_pad_read_motion(&ns2Motion) && ns2Motion.gyro_x == 0 && ns2Motion.gyro_z == 90 && ns2Motion.acceleration_x == -1 && ns2Motion.acceleration_z == 0,
           "single right Joy-Con motion turns sideways when the game uses a Pro controller");
    // 按键 / 摇杆同理：通道收原始方向（SL 在扩展位 21）；游戏用 JoyconRight 原样给、SL 换成 Ryujinx SlRight，扮 Pro 时转横握、SL 当 L
    ns2_relay_packet rawPacket = motionPacket; rawPacket.state = (ns2_state){(1ull << 2) | (1ull << NS2_BUTTON_SL), 0, 0, 300, -400};
    expect(send(relayClient, &rawPacket, sizeof rawPacket, 0) == sizeof rawPacket, "raw single Joy-Con packet sent");
    ns2_state shaped; uint32_t side = 0;
    for (int attempt = 0; attempt < 100 && !(s2a_mac_ns2_read(&shaped, &side), shaped.buttons); attempt++) usleep(1000);
    expect(shaped.buttons == ((1ull << 0) | (1ull << 6)) && side == 0 && shaped.lx == -400 && shaped.ly == -300 && !s2a_mac_ns2_raw_now(),
           "game uses Pro: raw right Joy-Con becomes sideways (X -> A, SL -> L, stick (300,-400) -> (-400,-300))");
    s2a_mac_ns2_set_game_style(1 << 4);
    s2a_mac_ns2_read(&shaped, &side);
    expect(shaped.buttons == (1ull << 2) && side == (1u << 26) && shaped.rx == 300 && shaped.ry == -400 && s2a_mac_ns2_raw_now(),
           "game uses JoyconRight: raw keys and right stick pass through, SL becomes Ryujinx SlRight");
    s2a_mac_ns2_set_game_style(1 << 3);
    s2a_mac_ns2_read(&shaped, &side);
    expect(side == 0 && shaped.buttons == ((1ull << 0) | (1ull << 6)) && !s2a_mac_ns2_raw_now(), "game uses JoyconLeft with a right Joy-Con: not raw (sideways)");
    // 游戏不收 Pro、只收双 Joy-Con（InputPump 报 JoyconPair）：单只右 Joy-Con 照原始方向（真机双手模式只接一侧、直握；11-11 一款体育游戏的球类项目实机）
    s2a_mac_ns2_set_game_style(1 << 2);
    s2a_mac_ns2_read(&shaped, &side);
    expect(shaped.buttons == (1ull << 2) && side == (1u << 26) && s2a_mac_ns2_raw_now(), "game uses JoyconPair only: single Joy-Con stays raw (vertical)");
    expect(s2a_mac_ns2_single_style(NS2_KIND_JOYCON_L) == (1 << 3) && s2a_mac_ns2_single_style(NS2_KIND_JOYCON_R) == (1 << 4) &&
           !s2a_mac_ns2_single_style(NS2_KIND_PRO) && !s2a_mac_ns2_single_style(NS2_KIND_JOYCON_PAIR) && !s2a_mac_ns2_single_style(NS2_KIND_NONE),
           "single Joy-Con style hint: only one Joy-Con maps to JoyconLeft / JoyconRight");
    ns2_state pro = {(1ull << 6) | (1ull << NS2_BUTTON_SL), 1, 2, 3, 4};
    expect(s2a_mac_ns2_shape(NS2_KIND_PRO, 0, &pro) == 0 && pro.buttons == ((1ull << 6) | (1ull << NS2_BUTTON_SL)) && pro.lx == 1,
           "shape leaves the Pro controller untouched");
    s2a_mac_ns2_set_game_style(1 << 4);
    // 通道 v2 带型号（#520）：游戏端照收；菜单显示名跟着型号
    expect(s2a_mac_ns2_kind() == NS2_KIND_JOYCON_R && !strcmp(s2a_mac_ns2_kind_name(s2a_mac_ns2_kind()), "Joy-Con 2 (R)"),
           "relay v2 carries the controller kind to the game");
    expect(!strcmp(s2a_mac_ns2_kind_name(NS2_KIND_JOYCON_PAIR), "Joy-Con 2 (L + R)") && !strcmp(s2a_mac_ns2_kind_name(NS2_KIND_NONE), "Switch 手柄") && !strcmp(s2a_mac_ns2_kind_name(NS2_KIND_PRO), "Switch 2 Pro"),
           "kind display names: pair and not-connected fallback");
    // Joy-Con 2 拼成一对：只有右只（motion_left_known 0，同旧启动器）时一路；INPUT 包带左只（motion_left）时左右各一路，左在前
    s2a_pad_motion pairMotion[2] = {0};
    expect(s2a_pad_read_motion_pair(pairMotion) == 1 && pairMotion[0].gyro_x == 90, "Joy-Con 2 pair without left motion: one source (right), as before");
    ns2_relay_packet pairPacket = motionPacket; pairPacket.kind = NS2_KIND_JOYCON_PAIR; pairPacket.motion.host_timestamp_us = 2000;
    pairPacket.motion_left_known = 1;
    pairPacket.motion_left = (ns2_relay_motion_left){.acceleration_z = 1, .gyro_x = -90, .host_timestamp_us_lo = 1999};
    expect(send(relayClient, &pairPacket, sizeof pairPacket, 0) == sizeof pairPacket, "pair relay packet with left motion sent");
    int pairN = 0;
    for (int attempt = 0; attempt < 100 && (pairN = s2a_pad_read_motion_pair(pairMotion)) != 2; attempt++) usleep(1000);
    expect(pairN == 2 && pairMotion[0].gyro_x == -90 && pairMotion[0].host_timestamp_us == 1999 && pairMotion[1].gyro_x == 90 && pairMotion[1].host_timestamp_us == 2000,
           "Joy-Con 2 pair: left motion from motion_left, right from motion, left first");
    expect(s2a_pad_read_motion(&ns2Motion) && ns2Motion.gyro_x == 90, "single-source six-axis entry still reads the right Joy-Con");
    // Joy-Con 2 光学鼠标（INPUT 包 mouse / mouse_kind）：没有鼠标读数（旧启动器）时照样报连着 Joy-Con 2、known 0
    //   （一对 → 两侧）；有读数时 x / y / distance 原样、side 跟 mouse_kind；Pro 不报
    s2a_pad_mouse jcMouse[2] = {{0}};
    expect(s2a_pad_read_mouse_pair(jcMouse) == 2 && jcMouse[0].side == 0 && jcMouse[1].side == 1 && !jcMouse[0].known && !jcMouse[1].known,
           "Joy-Con 2 pair without mouse readings: both sides still reported as Joy-Con 2, no readings");
    ns2_relay_packet mousePacket = pairPacket; mousePacket.kind = NS2_KIND_JOYCON_L; mousePacket.mouse_kind = NS2_KIND_JOYCON_L;
    mousePacket.mouse = (ns2_relay_mouse){.x = -5, .y = 300, .surface = 7, .distance = 0};
    expect(send(relayClient, &mousePacket, sizeof mousePacket, 0) == sizeof mousePacket, "relay packet with mouse readings sent");
    for (int attempt = 0; attempt < 100 && !(s2a_pad_read_mouse_pair(jcMouse) && jcMouse[0].known); attempt++) usleep(1000);
    expect(jcMouse[0].known && jcMouse[0].side == 0 && jcMouse[1].side == 0xff && jcMouse[0].x == -5 && jcMouse[0].y == 300 && jcMouse[0].distance == 0 && jcMouse[0].host_timestamp_us != 0,
           "Joy-Con 2 mouse readings reach the game: left side, raw x / y / distance");
    // 拼成一对、两只都有鼠标：连接程序一包左一包右轮流（mouse_kind 标哪一只），宿主左右各存一份
    ns2_relay_packet pairMouse = pairPacket; pairMouse.kind = NS2_KIND_JOYCON_PAIR;
    pairMouse.mouse_kind = NS2_KIND_JOYCON_R; pairMouse.mouse = (ns2_relay_mouse){.x = 40, .y = 1};
    expect(send(relayClient, &pairMouse, sizeof pairMouse, 0) == sizeof pairMouse, "pair relay packet with right mouse sent");
    pairMouse.mouse_kind = NS2_KIND_JOYCON_L; pairMouse.mouse = (ns2_relay_mouse){.x = -7, .y = 2, .distance = 12};
    expect(send(relayClient, &pairMouse, sizeof pairMouse, 0) == sizeof pairMouse, "pair relay packet with left mouse sent");
    s2a_pad_mouse both[2] = {{0}};
    int bothN = 0;
    for (int attempt = 0; attempt < 100 && !((bothN = s2a_pad_read_mouse_pair(both)) == 2 && both[0].known && both[0].x == -7 && both[1].known); attempt++) usleep(1000);
    expect(bothN == 2 && both[0].side == 0 && both[0].x == -7 && both[0].distance == 12 && both[1].side == 1 && both[1].x == 40,
           "Joy-Con 2 pair: left and right mouse readings both reach the game (alternating packets)");
    ns2_relay_packet proPacket = motionPacket; proPacket.kind = NS2_KIND_PRO;
    expect(send(relayClient, &proPacket, sizeof proPacket, 0) == sizeof proPacket, "Pro relay packet sent");
    for (int attempt = 0; attempt < 100 && s2a_pad_read_mouse_pair(jcMouse); attempt++) usleep(1000);
    expect(!s2a_pad_read_mouse_pair(jcMouse) && !jcMouse[0].known && !jcMouse[1].known, "Pro controller: no Joy-Con 2 mouse");
    // 多手柄（#520 C）：通道包头 player 分玩家，玩家 0 = 上面的 Pro、玩家 1 = 另一只手柄；各占一个槽位、各读各的（与系统手柄共用 s2a_slot_plan）
    ns2_relay_packet second = proPacket; second.player = 1; second.state = (ns2_state){.buttons = 1ull << 0, .lx = 1000};
    expect(send(relayClient, &second, sizeof second, 0) == sizeof second, "player 2 relay packet sent");
    ns2_state p2 = {0}; ns2_kind p2kind = NS2_KIND_NONE;
    for (int attempt = 0; attempt < 100 && !s2a_mac_ns2_read_player(1, &p2, NULL, &p2kind); attempt++) usleep(1000);
    ns2_state p1 = {0}; s2a_mac_ns2_read_player(0, &p1, NULL, NULL);
    expect(p2.buttons == 1 && p2.lx == 1000 && p2kind == NS2_KIND_PRO && p1.buttons == 0, "player 2 packet lands in player 2 only, player 1 untouched");
    config = default_config(); reset_input(); focused = YES; sample_input();   // 缺省键位：east = A
    int32_t padSticks[4] = {0};
    uint32_t slot1 = s2a_vpad_read_pad(1, padSticks), slot0 = s2a_vpad_read_pad(0, padSticks);
    expect(s2a_vpad_pad_count() == 2 && slot1 != 0 && slot0 == 0, "two NS2 players: two slots, player 2's button only in slot 2");
    // 失焦：读数清零但两位玩家仍占槽（否则服务层每次切窗口都拆掉玩家 2 的格子）
    focused = NO; s2a_mac_ns2_focus(0); sample_input();
    expect(s2a_vpad_pad_count() == 2 && s2a_vpad_read_pad(1, padSticks) == 0, "unfocused: both players keep their slots, inputs zeroed");
    focused = YES; s2a_mac_ns2_focus(1);
    // 玩家 2 起照它那一格的样式（#520，s2a_mac_ns2_set_player_style；宿主照槽位转）：没报 → 单只 Joy-Con 转横握、SL / SR 不单独给；
    //   那一格是 JoyconLeft → 原样给、SL 单独给（同玩家 1）。游戏样式（玩家 1 那一格）不影响玩家 2
    ns2_relay_packet secondJc = second; secondJc.kind = NS2_KIND_JOYCON_L; secondJc.state = (ns2_state){.buttons = 1ull << NS2_BUTTON_SL};
    s2a_mac_ns2_set_game_style(1 << 3);
    expect(send(relayClient, &secondJc, sizeof secondJc, 0) == sizeof secondJc, "player 2 single Joy-Con packet sent");
    uint32_t p2side = 0;
    for (int attempt = 0; attempt < 100 && !(s2a_mac_ns2_read_player(1, &p2, &p2side, &p2kind) && p2kind == NS2_KIND_JOYCON_L); attempt++) usleep(1000);
    expect(p2kind == NS2_KIND_JOYCON_L && p2side == 0, "player 2 single Joy-Con, no slot style reported: sideways (the game style is player 1's slot only)");
    s2a_mac_ns2_set_player_style(1, 1 << 3);
    expect(s2a_mac_ns2_read_player(1, &p2, &p2side, &p2kind) && p2side == (1u << 24) && s2a_mac_ns2_player_raw(1),
           "player 2 single Joy-Con in a JoyconLeft slot: raw like player 1, SL reported on its own");
    s2a_mac_ns2_set_player_style(1, 1 << 0);
    expect(s2a_mac_ns2_read_player(1, &p2, &p2side, &p2kind) && p2side == 0 && !s2a_mac_ns2_player_raw(1), "negative: player 2 in a Pro slot is still sideways");
    s2a_mac_ns2_set_player_style(1, 0);
    // 拆开后 P1 是通道玩家 2（分配成 P1 的是另一只）：游戏靠鼠标那一路认 Joy-Con 2，照这只的型号报左侧在；负对照：没这位玩家、超范围
    s2a_pad_mouse leadMouse[2] = {{0}};
    expect(s2a_mac_ns2_read_mouse_player(1, leadMouse) == 1 && leadMouse[0].side == 0 && leadMouse[1].side == 0xff, "player 2 as P1: Joy-Con 2 left side reported to the game");
    expect(!s2a_mac_ns2_read_mouse_player(5, leadMouse) && !s2a_mac_ns2_read_mouse_player(NS2_RELAY_PLAYERS, leadMouse), "negative: absent / out-of-range player has no Joy-Con 2");
    // 负对照：玩家号超出 NS2_RELAY_PLAYERS 的包丢掉、通道不断；之后的包照收
    ns2_relay_packet bogus = second; bogus.player = NS2_RELAY_PLAYERS + 3; bogus.state = (ns2_state){.buttons = 1ull << 5};
    expect(send(relayClient, &bogus, sizeof bogus, 0) == sizeof bogus, "out-of-range player packet sent");
    second.state = (ns2_state){.buttons = 1ull << 1};
    expect(send(relayClient, &second, sizeof second, 0) == sizeof second, "player 2 packet after the bogus one sent");
    for (int attempt = 0; attempt < 100 && !(s2a_mac_ns2_read_player(1, &p2, NULL, NULL) && p2.buttons == (1ull << 1)); attempt++) usleep(1000);
    ns2_state none = {0};
    expect(p2.buttons == (1ull << 1) && s2a_mac_ns2_status() == NS2_STATUS_READY && !s2a_mac_ns2_read_player(NS2_RELAY_PLAYERS + 3, &none, NULL, NULL),
           "out-of-range player dropped without closing the relay");
    // 玩家 2 断开（ready 0）：只剩玩家 1 一格
    second.ready = 0;
    expect(send(relayClient, &second, sizeof second, 0) == sizeof second, "player 2 disconnect packet sent");
    for (int attempt = 0; attempt < 100 && s2a_mac_ns2_player_present(1); attempt++) usleep(1000);
    sample_input();
    expect(!s2a_mac_ns2_player_present(1) && s2a_mac_ns2_player_present(0) && s2a_vpad_pad_count() == 1, "player 2 leaves: back to one slot");
    s2a_mac_ns2_set_game_style(1 << 4);
    // Switch 2 Pro 输入事件化（11-12 延迟管线调研）：通道读线程对变化踢主队列采样（s2a_mac_ns2_input_changed → front_mac.m），
    //   不再等 120Hz 定时器拉取。这里全程没有定时器在跑：排空主队列后，槽位也应已被采样
    config = default_config(); keys = [NSMutableSet set]; mouse = [NSMutableSet set]; focused = YES;
    ns2_relay_packet buttonPacket = motionPacket;
    buttonPacket.state = (ns2_state){.buttons = 1, .lx = -32767};   // 实体 A + 左摇杆满左
    expect(send(relayClient, &buttonPacket, sizeof buttonPacket, 0) == sizeof buttonPacket, "relay button packet sent");
    uint32_t padButtons = 0; memset(padSticks, 0, sizeof padSticks);
    for (int attempt = 0; attempt < 100; attempt++) {
        drain_main_queue();   // 通道线程已入队的踢在这跑：没有定时器在跑，槽位也应已被采样
        padButtons = s2a_vpad_read_pad(0, padSticks);
        if (padButtons == 1 && padSticks[0] == -32767) break;
        usleep(1000);   // 通道线程还没读到这一包：下一轮再试
    }
    expect(padButtons == 1 && padSticks[0] == -32767, "NS2 button/stick change reaches the pad slot via the main-queue kick, without polling");
    focused = NO;   // 还原：后面的键鼠用例自带 focused / config 初始化
    // 旧版启动器（通道 v1）：游戏端断开、不猜字段，型号回到 NONE
    ns2_relay_packet oldPacket = motionPacket; oldPacket.version = 1;
    expect(send(relayClient, &oldPacket, sizeof oldPacket, 0) == sizeof oldPacket, "relay v1 packet sent");
    for (int attempt = 0; attempt < 100 && s2a_mac_ns2_status() != NS2_STATUS_DISCONNECTED; attempt++) usleep(1000);
    expect(s2a_mac_ns2_status() == NS2_STATUS_DISCONNECTED && s2a_mac_ns2_kind() == NS2_KIND_NONE,
           "relay version mismatch disconnects instead of guessing fields");
    s2a_pad_set_motion_enabled(0);
    s2a_mac_ns2_detach();
    close(relayClient);
    close(relayListener);
    unlink(relayPath);
    unsetenv("S2A_NS2_SOCKET");
    // 通道断开也会踢一次主队列采样（#771）：在这里排空，别让它留到后面的用例里按失焦发布全零
    for (int i = 0; i < 5; i++) { usleep(5000); drain_main_queue(); }
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--input-config-only")) { test_shared_input_config(); return 0; }
        if (argc == 2 && !strcmp(argv[1], "--pad-latency-probe")) { pad_latency_probe(); return 0; }
        if (argc >= 3 && !strcmp(argv[1], "--guide-render")) {
            [NSApplication sharedApplication];
            NSApp.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];
            S2AMacInputGuide *guide=[[S2AMacInputGuide alloc] initWithName:guide_text(@"只读手柄图 · 合成样本（非游戏）",@"Read-only controls · Synthetic fixture (not a game)")
                gameID:@"0100000000000001" mapping:default_config() gameWindow:nil];
            if (argc > 3) [guide selectTarget:@(argv[3])];
            NSView *view=guide.window.contentView; [view layoutSubtreeIfNeeded];
            NSBitmapImageRep *bitmap=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
            [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
            NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            expect([png writeToFile:@(argv[2]) atomically:YES], "write native read-only render");
            return 0;
        }
#ifdef S2A_GUIDE_TEST_HOST
        // Explicit synthetic-host build for control-port/UI integration checks.
        // No loader, game resources or guest execution is linked into this fixture.
        guide_preview = YES; s2a_front_run(NULL); return 0;
#endif
        if (argc == 2 && !strcmp(argv[1], "--guide-preview")) { guide_preview = YES; s2a_front_run(NULL); return 0; }
        test_shared_input_config();
        test_active_pad();
        test_pro_sensor_control();
        test_pad_slots();
    test_pad_slot_pin();
        // 同一个真实前端实现：通用 attach 先发布层，detach 先通知再清层；
        // 直连只发布/清层，不通知尚未初始化且不应初始化的通用 NxGal。
        CAMetalLayer *front = [CAMetalLayer layer];
        void *raw = (__bridge void *)front;
        front_layer_attach(front);
        expect(layer_calls == 1 && notified_layer == raw, "generic attaches original layer once");
        expect(published_at_notify == raw && s2a_mac_front_layer() == raw, "generic publishes layer before notifying");
        front_layer_detach();
        expect(layer_calls == 2 && notified_layer == NULL, "generic detaches layer once");
        expect(published_at_notify == raw && s2a_mac_front_layer() == NULL, "generic preserves detach ordering");
        gfx_mode = S2A_GFX_DIRECT;
        front_layer_attach(front);
        expect(layer_calls == 2 && s2a_mac_front_layer() == raw, "direct publishes layer without generic attach");
        front_layer_detach();
        expect(layer_calls == 2 && s2a_mac_front_layer() == NULL, "direct clears layer without generic detach");
        gfx_mode = S2A_GFX_GENERIC;
        config = default_config(); keys = [NSMutableSet set]; mouse = [NSMutableSet set]; focused = YES;
        test_key_repeat();
        test_event_input();
        test_pad_disconnect();
        test_pad_handler_install();
        test_pad_fast_path();
        test_pad_unfocused_when_pause_off();
        test_profile_only_pad();
    test_pad_layout();
    test_shared_face_map();
    test_controller_face_overlay();
        test_per_player_config();
        test_save_atomic_warning();
        test_lan_warning();
        test_focus_suspend();
        test_untrusted_key_state();
        test_modifier_false_release();
        test_keyboard_hid();
        test_mouse_lost_release();
        test_mouse_touch();
        test_mouse_touch_declared();
        setenv("S2A_UI_LANGUAGE", "en", 1);
        NSArray *guideRows = s2a_mac_guide_rows(config);
        expect([guideRows[0][1] isEqual:@"E"], "guide uses actual default A key");
        expect([guideRows[3][2] isEqual:@"Left click"], "guide describes actual mouse Y binding");
        expect([guideRows[0][3] isEqual:@"Right face button"], "guide includes human-readable controller mapping");
        [NSApplication sharedApplication];
        NSMutableDictionary *source=[config mutableCopy];
        source[@"keyboard"]=[config[@"keyboard"] mutableCopy];
        S2AMacInputGuide *visual=[[S2AMacInputGuide alloc] initWithName:@"Synthetic readonly fixture" gameID:@"0100000000000001" mapping:source gameWindow:nil];
        expect(!visual.window.visible, "native visual guide never opens automatically");
        int buttonCount=0, editors=0; inspect_guide(visual.window.contentView, &buttonCount, &editors);
        expect(buttonCount==24 && [NSSet setWithArray:s2a_mac_controller_targets()].count==24, "all 24 supported targets are selectable in the diagram");
        expect(editors==0, "native guide has no editable fields or mapping pickers");
        source[@"keyboard"][@"A"]=@{@"code":@36};
        [visual selectTarget:@"B"];
        expect([visual.selectedTarget isEqual:@"B"] && guide_has_label(visual.window.contentView,@"Shift"), "selection displays the corresponding frozen input");
        [visual selectTarget:@"A"];
        expect(guide_has_label(visual.window.contentView,@"E"), "external edits cannot relabel the running snapshot");
        [visual selectTarget:@"not-an-input"];
        expect([visual.selectedTarget isEqual:@"A"], "invalid selection does not change displayed target");
        expect([config[@"keyboard"][@"A"][@"code"] intValue]==14, "diagram selection never changes HID mapping");
        NSImage *statusIcon=s2a_mac_guide_status_image();
        expect(statusIcon.template && NSEqualSizes(statusIcon.size,NSMakeSize(22,18)) && statusIcon.TIFFRepresentation.length>0, "Switch status icon renders as an adaptive template");
        visual=nil;
        expect(![s2a_mac_guide_preference_key(@"0100000000000001") isEqual:s2a_mac_guide_preference_key(@"0100000000000002")], "guide preferences are keyed by game ID");
        expect(s2a_mac_guide_preference_key(@"../unknown") == nil, "unknown identity cannot share preferences");
        NSString *guideRoot = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        [NSFileManager.defaultManager createDirectoryAtPath:guideRoot withIntermediateDirectories:YES attributes:nil error:NULL];
        NSString *preference = [guideRoot stringByAppendingPathComponent:@"guide.json"];
        NSData *pref = [NSJSONSerialization dataWithJSONObject:@{@"schema":@1, @"gameId":@"0100000000000001", @"show":@NO} options:0 error:NULL];
        [pref writeToFile:preference atomically:YES];
        expect(!s2a_mac_guide_show_again(@"0100000000000001", preference), "native guide reads shared launcher preference");
        expect(s2a_mac_guide_show_again(@"0100000000000002", preference), "another game cannot inherit hidden guide");
        NSData *manifest = [NSJSONSerialization dataWithJSONObject:@{@"gameId":@"0100000000000001", @"gameName":@"Fixture A"} options:0 error:NULL];
        [manifest writeToFile:[guideRoot stringByAppendingPathComponent:@"manifest.json"] atomically:YES];
        setenv("S2A_GAME_DATA", guideRoot.UTF8String, 1); unsetenv("S2A_GAME_NAME"); unsetenv("S2A_GAME_ID");
        expect([s2a_mac_guide_identity()[@"name"] isEqual:@"Fixture A"], "packaged game metadata names the guide without a launcher override");
        setenv("S2A_GAME_NAME", "Fixture B", 1); setenv("S2A_GAME_ID", "0100000000000002", 1);
        expect([s2a_mac_guide_identity()[@"name"] isEqual:@"Fixture B"], "new launch identity replaces previous game name");
        [NSFileManager.defaultManager removeItemAtPath:guideRoot error:NULL];
        int32_t sticks[4];
        [keys addObject:@14]; sample_input(); expect(s2a_vpad_read(sticks) == 1, "E maps to Switch A");
        [keys addObject:@13]; sample_input(); s2a_vpad_read(sticks); expect(sticks[1] == 32767, "W moves left stick up");
        [mouse addObject:@1]; sample_input(); expect(s2a_vpad_read(sticks) & (1u << 8), "right mouse maps to ZL");
        focused = NO; sample_input(); expect(s2a_vpad_read(sticks) == 0 && !sticks[1], "focus loss clears input");
        reset_input(); focused = YES;
        associate_mouse = test_associate; set_mouse_visible = test_cursor_visibility;
        set_mouse_capture(YES);
        expect(mouse_captured && !association_connected && cursor_hides == 1, "capture disconnects and hides cursor");
        association_error = kCGErrorFailure; set_mouse_capture(NO);
        expect(!mouse_captured && mouse_release_pending && cursor_shows == 1, "failed release remains retryable and stops camera input");
        set_mouse_capture(NO);
        expect(association_calls == 3 && cursor_shows == 1, "release retry does not unhide twice");
        association_error = kCGErrorSuccess; set_mouse_capture(NO);
        expect(!mouse_release_pending && association_connected, "release retry restores cursor association");
        set_mouse_capture(NO); expect(association_calls == 4, "released cursor needs no extra OS call");
        association_error = kCGErrorFailure; set_mouse_capture(YES);
        expect(!mouse_captured && !mouse_release_pending && cursor_hides == 1, "failed capture does not hide cursor");
        association_error = kCGErrorSuccess;
        test_capture_key_order();
        test_fullscreen_esc();
        test_capture_held_key(YES);
        test_capture_held_key(NO);
        associate_mouse = CGAssociateMouseAndMouseCursorPosition; set_mouse_visible = cursor_visibility;
        S2AMetalView *motionView = [S2AMetalView new];
        mouse_captured = YES; // Handler precondition only; do not call the OS capture API in unit tests.
        TestMotion *motion = [TestMotion new]; motion.deltaX = 1; motion.deltaY = -1;
        // #2131 回中模式按 33 ms 时间窗换算：摇杆量 = 窗内位移和 × 16 / 33（假时钟，期望手算：1 点 = 1500）
        mouse_now = fake_mouse_now; g_fake_mouse_ns = 1000000000ull; reset_input();
        for (int i = 0; i < 8; i++) [motionView mouseMoved:(NSEvent *)motion];
        sample_input(); sample_input(); // 120 Hz frontend may publish twice before the HID read.
        g_fake_mouse_ns += 1000000; s2a_vpad_read(sticks);
        expectf(sticks[2] == 5818 && sticks[3] == 5818, "8 deltas inside the window: 12000 × 16 / 33 = 5818 (%d, %d)", sticks[2], sticks[3]);
        g_fake_mouse_ns += 5000000; s2a_vpad_read(sticks);
        expectf(sticks[2] == 5818, "a second HID read 5 ms later inside the window sees the same deflection (not consumed once): %d", sticks[2]);
        g_fake_mouse_ns += 30000000; s2a_vpad_read(sticks); expect(!sticks[2] && !sticks[3], "36 ms after the last movement the stick is centered");
        motion.deltaX = 4; motion.deltaY = 0; [motionView mouseMoved:(NSEvent *)motion];
        motion.deltaX = -4; [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expect(!sticks[2], "opposite mouse deltas cancel within the window");
        g_fake_mouse_ns += 40000000; motion.deltaX = 100; [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expect(sticks[2] == 32767, "mouse stick clamps only at HID read");
        [motionView mouseMoved:(NSEvent *)motion]; reset_input();
        s2a_vpad_read(sticks); expect(!sticks[2], "focus reset discards pending mouse movement");
        mouse_captured = NO; [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expect(!sticks[2], "uncaptured cursor does not rotate the camera");
        mouse_captured = YES; focused = NO; [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expect(!sticks[2], "unfocused mouse motion is ignored");
        focused = YES;
        {   // #2131 的现场：鼠标每 8 ms 一个 1 点事件，HID 每 5 ms 读一次，30 帧的游戏每 33 ms 只看最新一条。
            //   期望：游戏每帧看到的量都在 16 ms 的位移附近（1500 × 16 / 8 = 3000，窗内 4–5 个事件 → 2909–3636），不出现 0；
            //   原先（读完清零）游戏看到的是最后 5 ms 里有没有事件，0 和 1500 交替
            reset_input(); g_fake_mouse_ns = 2000000000ull; motion.deltaX = 1; motion.deltaY = 0;
            uint64_t next_ev = g_fake_mouse_ns, next_game = g_fake_mouse_ns + 40000000; int frames = 0, zero = 0, lo = 1 << 30, hi = 0;
            for (uint64_t t = g_fake_mouse_ns; frames < 30; t += 1000000) {
                g_fake_mouse_ns = t;
                if (t >= next_ev) { [motionView mouseMoved:(NSEvent *)motion]; next_ev += 8000000; }
                if ((t - 2000000000ull) % 5000000 == 0) s2a_vpad_read(sticks);   // InputPump 的 5 ms 一格
                if (t >= next_game) { frames++; zero += !sticks[2]; lo = MIN(lo, sticks[2]); hi = MAX(hi, sticks[2]); next_game += 33333333; }
            }
            expectf(!zero && lo >= 2909 && hi <= 3636, "steady mouse at 30 fps: no zero frames, every frame 2909..3636 (zero %d, %d..%d)", zero, lo, hi);
            g_fake_mouse_ns += 40000000; s2a_vpad_read(sticks); expect(!sticks[2], "steady mouse stops: centered within the window");
            reset_input();
        }
        mouse_now = mouse_clock;
        // 审计 P2-62：recenter = false（赛车预设）→ 停手后右摇杆停在原处；复位清零；recenter 缺省 / true → 下一拍回中。期望按 1500 × 灵敏度手算
        config = @{@"keyboard": @{}, @"mouse": @{}, @"gamepad": @{}, @"axes": @{}, @"deadzone": @0.15,
                   @"mouseOptions": @{@"mode": @"camera", @"sensitivityX": @1, @"sensitivityY": @1, @"recenter": @NO}};
        apply_mouse_options(); focused = YES; mouse_captured = YES;
        motion.deltaX = 2; motion.deltaY = 0; [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expect(sticks[2] == 3000, "no-recenter mouse moves the stick");
        s2a_vpad_read(sticks); expect(sticks[2] == 3000, "no-recenter stick holds after the mouse stops");
        [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expect(sticks[2] == 6000, "no-recenter stick accumulates further movement");
        reset_input(); s2a_vpad_read(sticks); expect(!sticks[2], "focus reset recenters a held stick");
        config = default_config(); apply_mouse_options();
        mouse_now = fake_mouse_now; g_fake_mouse_ns = 3000000000ull;
        [motionView mouseMoved:(NSEvent *)motion];
        s2a_vpad_read(sticks); expectf(sticks[2] == 1454, "default recenter moves the stick (3000 × 16 / 33 = 1454): %d", sticks[2]);
        g_fake_mouse_ns += 40000000; s2a_vpad_read(sticks); expect(!sticks[2], "default recenter returns to center after the mouse stops");
        mouse_now = mouse_clock;
        mouse_captured = NO; focused = YES;
        config = @{@"keyboard": @{@"A": @{@"code": @14, @"modifiers": @1}, @"B": @{@"code": @56}},
                   @"mouse": @{}, @"gamepad": @{}, @"axes": @{}, @"deadzone": @0.15};
        [keys addObjectsFromArray:@[@14, @56]]; modifiers = 1; sample_input();
        expect(s2a_vpad_read(sticks) == 1, "chord consumes standalone modifier binding");
        reset_input();
        pthread_t thread; expect(!pthread_create(&thread, NULL, read_snapshots, NULL), "create HID reader");
        for (int i = 0; i < 100000; i++) {   // publish 的序号快照只允许一个写者在 g_compose_mu 内：同生产代码的两条写线
            int32_t v = i % 32768, s[4] = {v, v, v, v};
            pthread_mutex_lock(&g_compose_mu);
            publish((uint32_t)v, s);
            pthread_mutex_unlock(&g_compose_mu);
        }
        void *result; pthread_join(thread, &result); expect(result == NULL, "HID reads coherent snapshots");
        S2AMetalView *view = [[S2AMetalView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
        view.wantsLayer = YES; view.layer = [CAMetalLayer layer]; [view layout];
        expect(width == 640 && height == 360, "view publishes native pixel size");
        expect(fit_w == 640 && fit_h == 360, "view hands the same pixel size to the widen screen (P1-83: the window's aspect follows)");
        puts("Mac input/guide: mappings, identity, snapshots and graphics-mode layer checks passed");
    }
    return 0;
}
