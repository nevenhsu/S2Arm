// 加载页进度模型单测（runtime/include/s2a_loadprog.h）：期望值按口径手算。每例 fork 一个子进程（模型的静态量从零开始），S2A_DOCS 指临时目录。
// 等事件的上限只当兜底（11-19：原先 1～5 秒，负载高时线程没跑到就判红 → 一律 30 秒，正常时一到就返回）
#include "../src/native.h"
#include "s2a_loadprog.h"
#include <sys/wait.h>
#include <unistd.h>

#define S 1000000000ull
uint64_t now_ns(void) { return 0; }
static int fails;
#define WANT(cond) do { if (!(cond)) { printf("    不符：%s（行 %d）\n", #cond, __LINE__); _exit(1); } } while (0)
#define CASE(name, body) do { char d[] = "/tmp/t_loadprog.XXXXXX"; if (!mkdtemp(d)) return 1; setenv("S2A_DOCS", d, 1); \
    pid_t p = fork(); if (!p) { body; _exit(0); } int st; waitpid(p, &st, 0); \
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { fails++; printf("✗ %s\n", name); } else printf("  ✓ %s\n", name); } while (0)
static s2a_load_view V(uint64_t t) { s2a_load_view v; s2a_load_snapshot(t, &v); return v; }
static int eq(int s, const char *want) { char b[64]; s2a_load_eta_text(s, b, sizeof b); return !strcmp(b, want); }

#include <pthread.h>
static _Atomic int g_gate_done;
static void *gate_waiter(void *x) { (void)x; s2a_load_front_wait(); g_gate_done = 1; return NULL; }
int main(void) {
    unsetenv("S2A_LOAD_STALL_S"); unsetenv("DM_PSO_PREPARE_MAX_MS");
    // 前台激活门（s2a_load_front_state / s2a_load_front_wait；11-19 系统弹框盖着时场景停在前台未激活）：期望按状态定义手写
    CASE("前台激活门：没设过状态（不走 UIKit 场景的路径）→ 不等", { alarm(5); WANT(s2a_load_front_wait() == 0); });
    CASE("前台激活门：前台未激活时加载已做完、起主线程前等着；200 ms 后仍在等，激活后 1 秒内放行", {
        alarm(60); s2a_load_front_state(1); pthread_t th; WANT(!pthread_create(&th, NULL, gate_waiter, NULL));
        usleep(200000); WANT(g_gate_done == 0);
        s2a_load_front_state(2); for (int i = 0; i < 3000 && !g_gate_done; i++) usleep(10000); WANT(g_gate_done == 1); pthread_join(th, NULL); });
    CASE("前台激活门：已激活 → 不等", { alarm(5); s2a_load_front_state(2); WANT(s2a_load_front_wait() == 0); });
    CASE("剩余时间文案逐秒：-1 估算中、3 不到 5 秒、42 → 42 秒、61 → 1 分 1 秒、80 → 1 分 20 秒、120 → 2 分钟", {
        WANT(eq(-1, "正在估算剩余时间")); WANT(eq(3, "不到 5 秒")); WANT(eq(42, "42 秒")); WANT(eq(61, "1 分 1 秒"));
        WANT(eq(80, "1 分 20 秒")); WANT(eq(120, "2 分钟")); });
    CASE("之后的启动要重编（#2133）：现编已 300 条（≥ 200）；优化画面开始后 2.9 秒仍是简短画面、不给先开始玩；满 3 秒还没编完 → 升成完整页「优化」、给先开始玩；之后不再降回", {
        s2a_load_begin(0, 0); WANT(s2a_load_warm_tick(1 * S, 0, 46995) == 0); s2a_load_warm_compiled(300);
        { s2a_load_view v = V(1 * S); WANT(v.first_run == 0 && v.can_skip == 0); }
        WANT(s2a_load_warm_tick(1 * S + 2900000000ull, 300, 46995) == 0); { s2a_load_view v = V(1 * S + 2900000000ull); WANT(v.first_run == 0 && v.can_skip == 0); }
        WANT(s2a_load_warm_tick(4 * S, 400, 46995) == 0); { s2a_load_view v = V(4 * S); WANT(v.first_run == 1 && v.loading == 0 && v.can_skip == 1 && v.stage == S2A_LOAD_OPTIMIZE); }
        WANT(s2a_load_warm_tick(5 * S, 46995, 46995) == 0); WANT(V(5 * S).first_run == 1); });
    CASE("之后的启动 3 秒内编完：一直是简短画面（不升级、不给先开始玩）", {
        s2a_load_begin(0, 0); WANT(s2a_load_warm_tick(1 * S, 0, 500) == 0);
        WANT(s2a_load_warm_tick(3 * S, 500, 500) == 0); { s2a_load_view v = V(3 * S); WANT(v.first_run == 0 && v.can_skip == 0 && v.stage == S2A_LOAD_ENTER); }
        WANT(s2a_load_warm_tick(9 * S, 500, 500) == 0); WANT(V(9 * S).first_run == 0); });
    CASE("阶段：起局 = 准备数据，第一次 tick = 优化画面，编完 = 进入游戏，首帧 = 收起", {
        s2a_load_begin(0, 1); WANT(V(1 * S).stage == S2A_LOAD_DATA);
        WANT(s2a_load_warm_tick(2 * S, 0, 100) == 0); WANT(V(2 * S).stage == S2A_LOAD_OPTIMIZE);
        WANT(s2a_load_warm_tick(3 * S, 100, 100) == 0); { s2a_load_view v = V(3 * S); WANT(v.stage == S2A_LOAD_ENTER && v.go_reason == S2A_LOAD_GO_DONE); }
        s2a_load_first_frame(4 * S); WANT(V(4 * S).stage == S2A_LOAD_DONE && V(4 * S).frac == 1.0); });
    CASE("首帧回调先于预热结束（直连层首次提交里等预热，10-30 真机冷首启黑屏 15 秒）：首帧时还在优化画面 → 不收页；编完那一 tick 直接收起；没预热的启动首帧照旧收起", {
        s2a_load_begin(0, 1); WANT(s2a_load_warm_tick(1 * S, 0, 100) == 0);
        s2a_load_first_frame(2 * S); WANT(V(2 * S).stage == S2A_LOAD_OPTIMIZE);
        WANT(s2a_load_warm_tick(3 * S, 60, 100) == 0); WANT(V(3 * S).stage == S2A_LOAD_OPTIMIZE);
        WANT(s2a_load_warm_tick(4 * S, 100, 100) == 0); WANT(V(4 * S).stage == S2A_LOAD_DONE);
        s2a_load_begin(10 * S, 1); s2a_load_first_frame(11 * S); WANT(V(11 * S).stage == S2A_LOAD_DONE); });
    CASE("没有可预热的条目（11-02 魔界战记5 首启卡在「已优化 0 / 0」）：完成 tick (0, 0) 先到 → 进入游戏，首帧 → 收起；首帧先到 → 收起，之后 (0, 0) 不再动阶段", {
        s2a_load_begin(0, 1); WANT(s2a_load_warm_tick(1 * S, 0, 0) == 0); { s2a_load_view v = V(1 * S); WANT(v.stage == S2A_LOAD_ENTER && v.go_reason == S2A_LOAD_GO_DONE); }
        s2a_load_first_frame(2 * S); WANT(V(2 * S).stage == S2A_LOAD_DONE);
        s2a_load_begin(10 * S, 1); s2a_load_first_frame(11 * S); WANT(V(11 * S).stage == S2A_LOAD_DONE);
        WANT(s2a_load_warm_tick(12 * S, 0, 0) == 0); WANT(V(12 * S).stage == S2A_LOAD_DONE); });
    CASE("直连层自己放行（11-06：清单领完只剩在编的几条 / 到上限，s2a_front_warm_released）：优化画面中 → 进入游戏、原因 HOST；首帧已到过 → 直接收起；已进入游戏后再通知不动阶段", {
        s2a_load_begin(0, 1); WANT(s2a_load_warm_tick(1 * S, 0, 100) == 0);
        s2a_front_warm_released(); { s2a_load_view v = V(2 * S); WANT(v.stage == S2A_LOAD_ENTER && v.go_reason == S2A_LOAD_GO_HOST); }
        s2a_front_warm_released(); WANT(V(2 * S).stage == S2A_LOAD_ENTER && V(2 * S).go_reason == S2A_LOAD_GO_HOST);
        s2a_load_first_frame(3 * S); WANT(V(3 * S).stage == S2A_LOAD_DONE);
        s2a_load_begin(10 * S, 1); WANT(s2a_load_warm_tick(11 * S, 0, 100) == 0); s2a_load_first_frame(12 * S); WANT(V(12 * S).stage == S2A_LOAD_OPTIMIZE);
        s2a_front_warm_released(); WANT(V(13 * S).stage == S2A_LOAD_DONE); });
    CASE("首帧已到、玩家点了先开始玩：那一 tick 回 1（转后台）并直接收起", {
        s2a_load_begin(0, 1); WANT(s2a_load_warm_tick(1 * S, 0, 100) == 0); s2a_load_first_frame(2 * S);
        s2a_load_user_skip(); WANT(s2a_load_warm_tick(3 * S, 10, 100) == 1); { s2a_load_view v = V(3 * S); WANT(v.stage == S2A_LOAD_DONE && v.go_reason == S2A_LOAD_GO_USER); } });
    CASE("匀速 100 条 / 秒、共 1000：测满 2 秒前不给剩余时间；3 秒时 7 秒、4 秒时 6 秒", {
        s2a_load_begin(0, 1);
        for (uint64_t t = 0; t <= 4 * S; t += S / 2) { s2a_load_warm_tick(t, (long)(t / (S / 100)), 1000);
            if (t == S) WANT(V(t).eta_s == -1);
            if (t == 3 * S) WANT(V(t).eta_s == 7);
            if (t == 4 * S) WANT(V(t).eta_s == 6); } });
    CASE("变慢到 10 条 / 秒：头 3 秒显示值只往下走，持续 3 秒后按实测往上修正（≥ 15 秒）", {
        s2a_load_begin(0, 1); int last = -1;
        for (uint64_t t = 0; t <= 4 * S; t += S / 2) { s2a_load_warm_tick(t, (long)(t / (S / 100)), 1000); last = V(t).eta_s; }
        WANT(last == 6);
        long d = 400;
        for (uint64_t t = 4 * S + S / 2; t <= 7 * S; t += S / 2) { d += 5; s2a_load_warm_tick(t, d, 1000);
            if (t % S == 0) { int e = V(t).eta_s; WANT(e <= last); last = e; } }
        int up = 0; for (uint64_t t = 7 * S + S / 2; t <= 9 * S; t += S / 2) { d += 5; s2a_load_warm_tick(t, d, 1000); if (t % S == 0 && V(t).eta_s >= 15) up = 1; }
        WANT(up); });
    CASE("进度只进不退：每 0.5 秒取一次快照，frac 不降；三段边界 5% / 95% / 100%", {
        s2a_load_begin(0, 1); double prev = 0;
        for (uint64_t t = 0; t <= 3 * S; t += S / 2) { double f = V(t).frac; WANT(f >= prev && f < 0.05); prev = f; }
        for (uint64_t t = 3 * S; t <= 13 * S; t += S / 2) { s2a_load_warm_tick(t, (long)((t - 3 * S) / (S / 100)), 1000); double f = V(t).frac; WANT(f >= prev); prev = f; }
        WANT(prev > 0.94 && prev <= 0.95); s2a_load_first_frame(14 * S); WANT(V(14 * S).frac == 1.0); });
    CASE("先开始玩一直可点（11-22 #502）：准备数据段不出；优化画面第一 tick 起就出、每 0.5 秒都在（含快编完剩不到 5 秒时）；点了下一次 tick 返回 1、原因 = 玩家；之后按钮收起", {
        s2a_load_begin(0, 1); WANT(!V(0).can_skip);
        for (uint64_t t = 0; t <= 10 * S; t += S / 2) { s2a_load_warm_tick(t, (long)(t / (S / 95)), 1000); WANT(V(t).can_skip); }   // 95 条 / 秒，10 秒时剩 50 条
        WANT(V(10 * S).eta_s < 5 && V(10 * S).can_skip);
        s2a_load_user_skip(); WANT(s2a_load_warm_tick(10 * S + S / 20, 950, 1000) == 1);
        { s2a_load_view v = V(11 * S); WANT(v.go_reason == S2A_LOAD_GO_USER && v.stage == S2A_LOAD_ENTER && !v.can_skip); } });
    CASE("之后的启动（简短启动画面没有按钮）：每条都现编时，优化画面前 3 秒不出「先开始玩」；满 3 秒还没编完升成完整页才出（#2133）；第 10 秒编完不再出", {
        s2a_load_begin(0, 0); for (uint64_t t = 0; t <= 10 * S; t += S / 2) { long d = (long)(t / (S / 100)); s2a_load_warm_compiled(d); s2a_load_warm_tick(t, d, 1000); WANT(V(t).can_skip == (t >= 3 * S && t < 10 * S)); } });
    // 11-26（一款开放世界大作 iPhone 0.6.10「每次进游戏都要为你的设备优化画面」）：之后的启动归档全命中，载入几万条也被当成「还要编」升成「优化」页。期望按口径手写：
    CASE("第二次起局归档全命中、载入 2.5 秒完（现编 0）：一直简短画面，不升级、待优化 0 项（不出「优化」也不出「载入中」）", {
        s2a_load_begin(0, 0); for (uint64_t t = 0; t <= 3 * S; t += S / 2) { long d = t >= 2 * S + S / 2 ? 30000 : (long)(t / (S / 12000)); s2a_load_warm_compiled(0); s2a_load_warm_tick(t, d, 30000);
            s2a_load_view v = V(t); WANT(v.first_run == 0 && v.loading == 0 && v.can_skip == 0); } });
    CASE("归档全命中但载入慢（3000 条、100 条 / 秒、现编 0）：满 3 秒仍简短（不再按 3 秒升「优化」）；9.5 秒仍简短；满 10 秒还没完 → 完整页「载入中」、给先开始玩", {
        s2a_load_begin(0, 0); for (uint64_t t = 0; t <= 12 * S; t += S / 2) { s2a_load_warm_compiled(0); s2a_load_warm_tick(t, (long)(t / (S / 100)), 3000);
            s2a_load_view v = V(t); if (t < 10 * S) WANT(v.first_run == 0 && v.can_skip == 0); else WANT(v.first_run == 1 && v.loading == 1 && v.can_skip == 1); } });
    CASE("「载入中」后现编到 200 条（归档不全）→ 改显示「优化」；现编 199 条时仍是「载入中」", {
        s2a_load_begin(0, 0); s2a_load_warm_tick(0, 0, 3000); s2a_load_warm_tick(10 * S, 1000, 3000); WANT(V(10 * S).loading == 1);
        s2a_load_warm_compiled(199); s2a_load_warm_tick(11 * S, 1100, 3000); WANT(V(11 * S).loading == 1 && V(11 * S).first_run == 1);
        s2a_load_warm_compiled(200); s2a_load_warm_tick(12 * S, 1200, 3000); WANT(V(12 * S).loading == 0 && V(12 * S).first_run == 1); });
    CASE("首次启动（完整页一开始就在）：不管现编多少都是「优化」，不显示「载入中」", {
        s2a_load_begin(0, 1); for (uint64_t t = 0; t <= 12 * S; t += S / 2) { s2a_load_warm_compiled(0); s2a_load_warm_tick(t, (long)(t / (S / 100)), 3000); WANT(V(t).loading == 0); } });
    CASE("护栏：已编数 30 秒没涨 → 返回 1、原因 = 无进展；29.5 秒时还在等", {
        s2a_load_begin(0, 1); s2a_load_warm_tick(0, 0, 1000); s2a_load_warm_tick(1 * S, 10, 1000);
        WANT(s2a_load_warm_tick(30 * S + S / 2, 10, 1000) == 0);
        WANT(s2a_load_warm_tick(31 * S, 10, 1000) == 1); WANT(V(31 * S).go_reason == S2A_LOAD_GO_STALL); });
    CASE("没有总时长上限（11-22 #502：编译还在往前走就等，到点不再硬进）：10 条 / 秒一直有进展，到第 600 秒每一 tick 都回 0、阶段仍是优化画面；DM_PSO_PREPARE_MAX_MS=8000 也不影响页面", {
        s2a_load_begin(0, 1); int r = 0;
        for (uint64_t t = 0; t <= 600 * S; t += S / 2) r |= s2a_load_warm_tick(t, (long)(t / (S / 10)), 100000);
        WANT(!r); WANT(V(600 * S).stage == S2A_LOAD_OPTIMIZE && V(600 * S).go_reason == S2A_LOAD_GO_NONE);
        setenv("DM_PSO_PREPARE_MAX_MS", "8000", 1); s2a_load_begin(0, 1); r = 0;
        for (uint64_t t = 0; t <= 60 * S; t += S / 2) r |= s2a_load_warm_tick(t, (long)(t / (S / 10)), 100000);
        WANT(!r); unsetenv("DM_PSO_PREPARE_MAX_MS"); });
    CASE("剩余时间按已编 / 总数和速率估、不再按上限封顶（11-22 #502）：10 条 / 秒、共 100000 条，第 10 秒剩 99900 条 → 9990 秒、第 20 秒 → 9980 秒（±1 秒取整）", {
        s2a_load_begin(0, 1); int e10 = -9; int e20 = -9;
        for (uint64_t t = 0; t <= 20 * S; t += S / 2) { s2a_load_warm_tick(t, (long)(t / (S / 10)), 100000);
            if (t == 10 * S) e10 = V(t).eta_s; if (t == 20 * S) e20 = V(t).eta_s; }
        WANT(e10 >= 9989 && e10 <= 9991); WANT(e20 >= 9979 && e20 <= 9981); });
    CASE("全部编完才进游戏：记下本包已完整预热，同包之后 = 非首次、换包 = 首次", {
        WANT(s2a_load_first_run("aaaa"));
        s2a_load_begin(0, 1); s2a_load_warm_tick(0, 0, 10); WANT(s2a_load_warm_tick(S, 10, 10) == 0); s2a_load_first_frame(2 * S);
        WANT(!s2a_load_first_run("aaaa")); WANT(s2a_load_first_run("bbbb")); });
    CASE("先开始玩后后台编完：首帧后迟到的 (N, N) 也记标记、不动阶段、回 0；没编完（只跳过）不记", {
        WANT(s2a_load_first_run("bbbb"));
        s2a_load_begin(0, 1); s2a_load_warm_tick(0, 0, 10); s2a_load_user_skip(); WANT(s2a_load_warm_tick(S, 3, 10) == 1); s2a_load_first_frame(2 * S);
        WANT(s2a_load_first_run("bbbb"));
        WANT(s2a_load_warm_tick(30 * S, 10, 10) == 0); WANT(V(30 * S).stage == S2A_LOAD_DONE); WANT(!s2a_load_first_run("bbbb")); });
    CASE("反证：没有出包指纹（\"-\"）永远算首次、也不记", {
        WANT(s2a_load_first_run("-")); s2a_load_begin(0, 1); s2a_load_warm_tick(0, 0, 10); s2a_load_warm_tick(S, 10, 10); s2a_load_first_frame(2 * S);
        WANT(s2a_load_first_run("-")); });
    CASE("没有加载页前端（没调过 s2a_load_begin）：s2a_front_warm_tick 回 -1；调过之后回 0 / 1", {
        WANT(s2a_front_warm_tick(0, 10) == -1); s2a_load_begin(0, 1); WANT(s2a_front_warm_tick(0, 10) == 0); });
    if (fails) { printf("✗ t_loadprog：%d 例不通\n", fails); return 1; }
    printf("✓ t_loadprog：27 例全过\n"); return 0;
}
