// 首次进游戏的加载页：进度模型（接口与口径见 runtime/include/s2a_loadprog.h）。
#include "native.h"
#include "s2a_loadprog.h"
#include "s2a_docs.h"   // 文档根唯一取法
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

#define S 1000000000ull
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_stage, g_first_run, g_go;            // 阶段、首次、为什么不再等
static _Atomic int g_skip;                        // 玩家点了「先开始玩」
static int g_ff;                                  // 首帧已到、但预热还在等（页面先不收，预热结束时再收）
static long g_done, g_total, g_compiled;         // g_compiled：这一局预热真现编的条数（直连层报，s2a_load_warm_compiled）
static int g_loading;                             // 完整页显示「载入中」（之后的启动、从管线归档载入为主）
static uint64_t g_t0, g_warm_t0, g_last_up;       // 起局、预热开始、已编数最近一次上涨
static double g_rate;                             // 已编 / 秒，按 1 秒窗口指数平滑
static uint64_t g_rate_t; static long g_rate_done;
static double g_frac;                             // 显示过的最大进度
static double g_eta_shown = -1; static uint64_t g_eta_t, g_slow_since;   // 显示的剩余秒数、它的时刻、新估计持续偏高的起点
static char g_pack[24];
static _Atomic int g_begun;                       // s2a_load_begin 调过 = 有加载页前端（iOS）；Mac / 安卓宿主没有

static double env_s(const char *k, double d) { const char *e = getenv(k); double v = e && *e ? atof(e) : d; return v > 0 ? v : d; }

void s2a_load_begin(uint64_t now, int first_run) {
    pthread_mutex_lock(&g_mu);
    g_stage = S2A_LOAD_DATA; g_first_run = first_run; g_go = S2A_LOAD_GO_NONE; g_ff = 0; atomic_store(&g_skip, 0);
    g_done = g_total = g_compiled = 0; g_loading = 0; g_t0 = now; g_warm_t0 = g_last_up = 0; g_rate = 0; g_rate_t = 0; g_rate_done = 0;
    g_frac = 0; g_eta_shown = -1; g_eta_t = g_slow_since = 0;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_begun, 1);
}

void s2a_load_user_skip(void) { atomic_store(&g_skip, 1); }
void s2a_load_warm_compiled(long n) { pthread_mutex_lock(&g_mu); if (n > g_compiled) g_compiled = n; pthread_mutex_unlock(&g_mu); }

static void mark_warm_done(void);
int s2a_load_warm_tick(uint64_t now, long done, long total) {
    // 全部编完（直连层保证 (total, total) 只发一次：首帧前编完，或先进了游戏、后台编完时）：管线已进系统缓存，记本包已完整预热
    if (total > 0 && done >= total) mark_warm_done();
    pthread_mutex_lock(&g_mu);
    if (g_stage >= S2A_LOAD_ENTER) { pthread_mutex_unlock(&g_mu); return 0; }   // 已在进游戏 / 页面已收起：后台那次完成通知只记标记，不动阶段
    if (g_stage == S2A_LOAD_DATA) { g_stage = S2A_LOAD_OPTIMIZE; g_warm_t0 = g_last_up = g_rate_t = now; g_rate_done = done; }
    if (total > 0) g_total = total;
    if (done > g_done) { g_done = done; g_last_up = now; }
    if (now - g_rate_t >= S) {   // 1 秒窗口的速率，指数平滑（新窗口占 0.3）
        double r = (double)(g_done - g_rate_done) / ((double)(now - g_rate_t) / S);
        g_rate = g_rate > 0 ? 0.7 * g_rate + 0.3 * r : r; g_rate_t = now; g_rate_done = g_done;
    }
    int go = 0, up = 0;
    // 编完按这一 tick 自己的参数判（done ≥ total）：直连层开头的 (0, n) 一定 n > 0（warm_announce / warm_prepare 只在有条目时发），done ≥ total 只出现在完成那一下——
    //   包括没有可预热的条目时的 (0, 0)（11-02 维护者 魔界战记5 首启：后台预热 0 条、0.6 ms 就收尾发了 (0, 0)，原先按「g_total 非 0」判，0 条永远不算编完，
    //   阶段停在「优化画面」、首帧来了也只记不收，之后再没有 tick、30 秒无进展护栏也跑不到，页面「已优化 0 / 0」卡死）
    // 简短启动画面（之后的启动）要不要换成完整加载页，看这一局实际要做什么（「之后的启动」只是上次的预测）：
    //   ① 真要现编一批：进入优化画面满 S2A_LOAD_UPGRADE_S（3）秒、现编已到 S2A_LOAD_UPGRADE_COMPILED（200）条（存进管线归档的只是一部分，或系统清了缓存）
    //      → 完整页「优化」，有进度、给「先开始玩」（11-23 #2133：一款 4.7 万条，第二次打开在只有小转圈的画面后面重编，玩家看着卡死）
    //   ② 几乎全从管线归档载入、却载入很久：满 S2A_LOAD_SLOW_S（10）秒仍没完 → 完整页「载入中」（同样有进度和「先开始玩」，不说「优化」）。
    //      11-26：原先只按 3 秒判，归档命中的载入也算「还要编」，大款（预热上万项，一款开放世界大作 3 万）每次启动都弹「为你的设备优化画面」，
    //      玩家以为每次都在重新优化（实际归档全命中，0.6.9 同样要载 8–10 秒，只是显示简短画面）
    //   「载入中」后面现编又够了（归档残缺）→ 改回「优化」
    { double el = (double)(now - g_warm_t0) / S; long need = (long)env_s("S2A_LOAD_UPGRADE_COMPILED", 200);
      if (!g_first_run && done < total) {
          if (el >= env_s("S2A_LOAD_UPGRADE_S", 3) && g_compiled >= need) { g_first_run = 1; g_loading = 0; up = 1; }
          else if (el >= env_s("S2A_LOAD_SLOW_S", 10)) { g_first_run = 1; g_loading = 1; up = 2; }
      } else if (g_loading && done < total && g_compiled >= need) { g_loading = 0; up = 3; } }
    if (done >= total) { g_go = S2A_LOAD_GO_DONE; go = 1; }
    else if (atomic_load(&g_skip)) { g_go = S2A_LOAD_GO_USER; go = 1; }
    else if (now - g_last_up >= (uint64_t)(env_s("S2A_LOAD_STALL_S", 30) * S)) { g_go = S2A_LOAD_GO_STALL; go = 1; }
    // 没有总时长上限（11-22 #502 玩家：「我没点就别进游戏，等是我的选择」）：编译还在往前走就一直等，玩家随时可点「先开始玩」；原先 30 秒到点硬进（11-06 马车8 页面看着卡住才加），
    //   那次的「看着卡住」由上面的无进展护栏和逐秒剩余时间兜住；直连层有前端时同样不再到点放行（nvn_mtl.m warm_prepare）
    if (go) g_stage = g_ff ? S2A_LOAD_DONE : S2A_LOAD_ENTER;   // 首帧早已到过（直连层先过首帧回调、后在首次提交里等预热）：预热一结束就收页
    int reason = g_go; long d = g_done, t = g_total, c = g_compiled;
    pthread_mutex_unlock(&g_mu);
    if (up == 1) LOG("加载页：之后的启动却要现编（已现编 %ld 条，已编 %ld / %ld），换成完整加载页「优化」（进度 + 先开始玩）", c, d, t);
    else if (up == 2) LOG("加载页：之后的启动从管线归档载入 %ld / %ld 条已超过 %.0f 秒（现编 %ld 条），换成完整加载页「载入中」（进度 + 先开始玩）", d, t, env_s("S2A_LOAD_SLOW_S", 10), c);
    else if (up == 3) LOG("加载页：「载入中」又现编到 %ld 条（归档不全），改显示「优化」", c);
    if (go && reason != S2A_LOAD_GO_DONE)
        LOG("加载页：不再等预热（%s），已编 %ld / %ld，剩下的转后台", reason == S2A_LOAD_GO_USER ? "玩家点了先开始玩" : "长时间没有进展", d, t);
    return go && reason != S2A_LOAD_GO_DONE ? 1 : 0;   // 全部编完：直连层自己正常收尾（返回 0 不转后台）
}

static void docs_path(char *out, size_t n, const char *name) {
    s2a_docs_path(out, n, "wire%s%s", name ? "/" : "", name ? name : "");
}
int s2a_load_first_run(const char *pack) {
    snprintf(g_pack, sizeof g_pack, "%s", pack && *pack ? pack : "-");
    char p[1200], w[64] = ""; docs_path(p, sizeof p, "warm_done");
    FILE *f = fopen(p, "r"); if (!f) return 1;
    int ok = fgets(w, sizeof w, f) != NULL; fclose(f);
    w[strcspn(w, " \t\r\n")] = 0;
    return !(ok && !strcmp(w, g_pack) && strcmp(g_pack, "-"));
}
// 首帧回调（ap_host.c s2a_frame_presented）在直连层首次提交之前就来了，预热等待发生在首次提交里：此时还在「优化画面」= 屏上其实没有画面，
//   页面不收、只记首帧已到，等预热结束（全部编完或不再等）再收。10-30 真机怪猎崛起冷首启：首帧回调 0.96 秒就收了页，之后预热等了 15 秒黑屏
void s2a_load_first_frame(uint64_t now) {
    (void)now;
    pthread_mutex_lock(&g_mu);
    if (g_stage == S2A_LOAD_OPTIMIZE) g_ff = 1; else g_stage = S2A_LOAD_DONE;
    pthread_mutex_unlock(&g_mu);
}
static void mark_warm_done(void) {
    static _Atomic int done; if (atomic_exchange(&done, 1) || !strcmp(g_pack, "-") || !g_pack[0]) return;
    char d[1200], p[1300], t[1400]; docs_path(d, sizeof d, NULL);
    if (mkdir(d, 0755) && errno != EEXIST) return;
    snprintf(p, sizeof p, "%s/warm_done", d); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) return;
    fprintf(f, "%s\n", g_pack);
    if (fclose(f) || rename(t, p)) LOG("【红】加载页：写 %s 失败（errno %d）", p, errno);
    else LOG("加载页：本包预热已全部编完，之后的启动只显示简短启动画面");
}

void s2a_load_snapshot(uint64_t now, s2a_load_view *v) {
    pthread_mutex_lock(&g_mu);
    v->stage = g_stage; v->first_run = g_first_run; v->loading = g_loading; v->done = g_done; v->total = g_total; v->go_reason = g_go;
    // 进度：准备数据 0–5%（按 10 秒渐近，不到 5%）、优化画面 5–95%（已编 / 总数）、进入游戏 95–100%
    double f = g_stage == S2A_LOAD_DATA ? 0.05 * (1.0 - 1.0 / (1.0 + (double)(now - g_t0) / (10.0 * S)))
             : g_stage == S2A_LOAD_OPTIMIZE ? 0.05 + 0.90 * (g_total ? (double)g_done / (double)g_total : 0)
             : g_stage == S2A_LOAD_ENTER ? 0.95 : 1.0;
    if (f > g_frac) g_frac = f;
    v->frac = g_frac;
    // 剩余时间：只在优化画面段、测了 ≥ 2 秒且速率 > 0 时给
    v->eta_s = -1;
    if (g_stage == S2A_LOAD_OPTIMIZE && g_rate > 0 && now - g_warm_t0 >= 2 * S && g_total > g_done) {
        double est = (double)(g_total - g_done) / g_rate;
        if (g_eta_shown < 0) g_eta_shown = est;
        else {
            double down = g_eta_shown - (double)(now - g_eta_t) / S;   // 按流逝时间往下走
            if (down < 0) down = 0;
            if (est > g_eta_shown * 1.25) { if (!g_slow_since) g_slow_since = now; }   // 实测变慢
            else g_slow_since = 0;
            g_eta_shown = g_slow_since && now - g_slow_since >= 3 * S ? est : (est < down ? est : down);
            if (g_slow_since && now - g_slow_since >= 3 * S) g_slow_since = 0;
        }
        g_eta_t = now;
        v->eta_s = (int)(g_eta_shown + 0.999);   // 不再按上限封顶（没有上限了，11-22 #502）：显示的就是按已编 / 总数和近段速率估的剩余时间
    }
    v->can_skip = g_stage == S2A_LOAD_OPTIMIZE && g_first_run && !atomic_load(&g_skip);   // 优化画面一开始就可点、一直可点（11-22 #502：等不等由玩家定；只有完整加载页有这个按钮）
    pthread_mutex_unlock(&g_mu);
}

void s2a_load_eta_text(int s, char *out, size_t n) {
    if (s < 0) snprintf(out, n, "正在估算剩余时间");
    else if (s < 5) snprintf(out, n, "不到 5 秒");
    else if (s < 60) snprintf(out, n, "%d 秒", s);   // 逐秒倒数（11-06 维护者：原先按 5 秒一档取整，看着像写死的）
    else if (s % 60) snprintf(out, n, "%d 分 %d 秒", s / 60, s % 60);
    else snprintf(out, n, "%d 分钟", s / 60);
}

// 没有加载页前端（没调过 s2a_load_begin：Mac / 安卓宿主）回 -1：直连层用自己的护栏（DM_PSO_PREPARE_STALL_MS / DM_PSO_PREPARE_MAX_MS），同弱定义
int s2a_front_warm_tick(long done, long total) { return atomic_load(&g_begun) ? s2a_load_warm_tick(now_ns(), done, total) : -1; }
void s2a_front_warm_compiled(long n) { if (atomic_load(&g_begun)) s2a_load_warm_compiled(n); }
void s2a_front_warm_released(void) {
    if (!atomic_load(&g_begun)) return;
    pthread_mutex_lock(&g_mu);
    int go = g_stage < S2A_LOAD_ENTER; long d = g_done, t = g_total;
    if (go) { g_go = S2A_LOAD_GO_HOST; g_stage = g_ff ? S2A_LOAD_DONE : S2A_LOAD_ENTER; }
    pthread_mutex_unlock(&g_mu);
    if (go) LOG("加载页：直连层已放行（清单全部领走、只剩在编的几条），已编 %ld / %ld，剩下的转后台", d, t);
}

// 前台激活门（见 s2a_loadprog.h）
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER; static int g_front;
void s2a_load_front_state(int st) {
    pthread_mutex_lock(&g_fmu); g_front = st; pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
}
int s2a_load_front_wait(void) {
    uint64_t t0 = now_ns(); int waited = 0;
    pthread_mutex_lock(&g_fmu);
    if (g_front == 1) { waited = 1; LOG("前台激活门：加载已做完，场景还没激活（系统弹框盖着？），等激活再起游戏主线程"); }
    while (g_front == 1) pthread_cond_wait(&g_fcv, &g_fmu);
    pthread_mutex_unlock(&g_fmu);
    int ms = waited ? (int)((now_ns() - t0) / 1000000ull) : 0;
    if (waited) LOG("前台激活门：场景已激活，起游戏主线程（等了 %d ms）", ms);
    return ms;
}

