// 每款参数：读随包的 <游戏数据>/shaders/direct/game.tsv（出库时把选中的配置档拷进去：零配置推导档或手写档），不再编进宿主、不按程序号查表。
// 键与换算同 graphics/nvn/games/game.py header()；没写的键按 Switch 真实语义的缺省（10-25 维护者定「手写档只当测试夹具，运行时与出包都不依赖它」）：
//   render_cond on（条件渲染照 NVN 语义求值；关 = 忽略游戏的条件，动森把清屏包在条件里、不开每帧全黑）
//   gpu_timestamps segment（计数器时间戳取 GPU 实际起止，submission 是整次提交一个近似时刻）
//   draw_barriers full（绘制遍各等前面全部 GPU 活；game = 只按游戏自己的屏障排序，遍的顶点段可与前一遍的片元段重叠：上游参考实现（MIT）时之笛档 game，GPU 25→18 ms；DM_NVN_BARRIER 覆盖）
//   tex_flush_upload / tex_dirty_upload on（CPU 写进池的贴图要上传：Switch 上 CPU / GPU 同一份内存。只重传字节真变了的页，GPU 后写的页留着，见 nvn_mtl.m NvPkg ph / pgpu）
//   其余同 game.py：tex_dyn_slots on（10-31 改回 Switch 真实语义：10-25 关它是因马车8 马里奥全黑，10-31 main 上马车8 开着跑到 MARK、比赛画面正常、本路线 0 支动态句柄着色器；关着时这类着色器整支建不出、不画，脑锻炼因此进不了局），present_interval game、fps_hold on、vconv_range off、app_pool 0xCD500000、alpha_from_red / feedback_snap 空
// 档里显式写的值优先（含 off）；值看不懂报红、按缺省。纯 C、无副作用，host_adapt.m 与单测 graphics/nvn/tests/t_game_tsv.c 共用。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t app_pool;
    int present_display, present_display_derived, fps_hold, render_cond, tex_flush_upload, tex_dirty_upload, tex_dyn_slots, gpu_ts_segment, vconv_range, draw_barriers_game;
    char alpha_from_red[2048], feedback_snap[512], zero_skip[512];
} s2a_game_tsv;

static inline void s2a_game_tsv_defaults(s2a_game_tsv *g) {
    memset(g, 0, sizeof *g);
    g->app_pool = 0xCD500000ull; g->fps_hold = 1;
    g->render_cond = 1; g->tex_flush_upload = 1; g->tex_dirty_upload = 1; g->tex_dyn_slots = 1; g->gpu_ts_segment = 1;
}

// 一行「键<TAB>值…」：同 game.py，跳过空行与 # 开头，键 / 每个值去两端空白，值之间用 TAB 连；同一键取第一行
static inline int s2a_game_tsv_find(const char *text, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *ln = text; ln && *ln; ) {
        const char *e = strchr(ln, '\n'); size_t len = e ? (size_t)(e - ln) : strlen(ln);
        const char *p = ln; while (p < ln + len && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        if (p < ln + len && *p != '#') {
            const char *tab = memchr(p, '\t', (size_t)(ln + len - p)); const char *ke = tab ? tab : ln + len;
            while (ke > p && (ke[-1] == ' ' || ke[-1] == '\r')) ke--;
            if ((size_t)(ke - p) == kl && !memcmp(p, key, kl)) {
                size_t o = 0; const char *v = tab ? tab + 1 : ln + len;
                while (v < ln + len) {
                    const char *ve = memchr(v, '\t', (size_t)(ln + len - v)); if (!ve) ve = ln + len;
                    const char *a = v, *b = ve; while (a < b && (*a == ' ' || *a == '\r')) a++; while (b > a && (b[-1] == ' ' || b[-1] == '\r')) b--;
                    if (o && o + 1 < n) out[o++] = '\t';
                    size_t c = (size_t)(b - a); if (o + c >= n) c = n - 1 - o; memcpy(out + o, a, c); o += c;
                    v = ve + 1;
                }
                out[o] = 0; return 1;
            }
        }
        ln = e ? e + 1 : NULL;
    }
    return 0;
}

// 开关键：on / off（gpu_timestamps：segment / submission；present_interval：display / game；draw_barriers：game / full）。看不懂 → 写一行到 err、保持缺省
static inline void s2a_game_tsv_switch(const char *text, const char *key, const char *on, const char *off, int *dst, char *err, size_t en) {
    char v[64]; if (!s2a_game_tsv_find(text, key, v, sizeof v)) return;
    if (!strcmp(v, on)) *dst = 1; else if (!strcmp(v, off)) *dst = 0;
    else if (err && !*err) snprintf(err, en, "%s 的值「%s」看不懂（要 %s / %s），按缺省", key, v, on, off);
}
static inline void s2a_game_tsv_words(const char *text, const char *key, char *dst, size_t n) {   // 空白分隔的着色器键名单：TAB 换成空格
    if (!s2a_game_tsv_find(text, key, dst, n)) return;
    for (char *c = dst; *c; c++) if (*c == '\t') *c = ' ';
}

// 解析 game.tsv 全文到 *g（先填缺省）。err 收第一处看不懂的值（空 = 都认得）
static inline void s2a_game_tsv_parse(const char *text, s2a_game_tsv *g, char *err, size_t en) {
    s2a_game_tsv_defaults(g); if (err && en) *err = 0;
    if (!text) return;
    char v[64];
    if (s2a_game_tsv_find(text, "app_pool", v, sizeof v)) {
        char *end; unsigned long long x = strtoull(v, &end, 0);
        if (*v && !*end && x) g->app_pool = x; else if (err && !*err) snprintf(err, en, "app_pool 的值「%s」看不懂，按缺省", v);
    }
    s2a_game_tsv_switch(text, "present_interval", "display", "game", &g->present_display, err, en);
    s2a_game_tsv_switch(text, "present_interval_derived", "display", "game", &g->present_display_derived, err, en);
    s2a_game_tsv_switch(text, "fps_hold", "on", "off", &g->fps_hold, err, en);
    s2a_game_tsv_switch(text, "render_cond", "on", "off", &g->render_cond, err, en);
    s2a_game_tsv_switch(text, "tex_flush_upload", "on", "off", &g->tex_flush_upload, err, en);
    s2a_game_tsv_switch(text, "tex_dirty_upload", "on", "off", &g->tex_dirty_upload, err, en);
    s2a_game_tsv_switch(text, "tex_dyn_slots", "on", "off", &g->tex_dyn_slots, err, en);
    s2a_game_tsv_switch(text, "gpu_timestamps", "segment", "submission", &g->gpu_ts_segment, err, en);
    s2a_game_tsv_switch(text, "vconv_range", "on", "off", &g->vconv_range, err, en);
    s2a_game_tsv_switch(text, "draw_barriers", "game", "full", &g->draw_barriers_game, err, en);
    { char d[8]; if (!s2a_game_tsv_find(text, "tex_dirty_upload", d, sizeof d)) g->tex_dirty_upload = g->tex_flush_upload; }   // 同 game.py：没写时随 tex_flush_upload
    if (!g->tex_flush_upload) g->tex_dirty_upload = 0;   // 同 game.py：tex_dirty_upload 要 tex_flush_upload 开
    s2a_game_tsv_words(text, "alpha_from_red", g->alpha_from_red, sizeof g->alpha_from_red);
    s2a_game_tsv_words(text, "feedback_snap", g->feedback_snap, sizeof g->feedback_snap);
    s2a_game_tsv_words(text, "zero_skip", g->zero_skip, sizeof g->zero_skip);   // 零源跳过事实：<片元键>:<常量缓冲寄存器>:<向量>:<分量>，空白分隔（nvn_mtl.m zskip_facts_load；出库时 direct_lib.sh 从 games/zero_skip.tsv 按程序号补进）
}

// 读文件再解析；返回 1 = 读到文件，0 = 没有文件（全按缺省）
static inline int s2a_game_tsv_load(const char *path, s2a_game_tsv *g, char *err, size_t en) {
    FILE *f = fopen(path, "rb"); if (!f) { s2a_game_tsv_parse(NULL, g, err, en); return 0; }
    char *buf = NULL; size_t cap = 0, len = 0, r; char tmp[4096];
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (len + r + 1 > cap) { cap = (len + r + 1) * 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
        memcpy(buf + len, tmp, r); len += r;
    }
    fclose(f); if (buf) buf[len] = 0;
    s2a_game_tsv_parse(buf, g, err, en); free(buf); return 1;
}

// present_interval 决策函数（生产代码与单测共用）：返回最终的 present_display（1=display, 0=game）
// explicit_set: game.tsv 显式写了 present_interval（1=有显式值，0=无）
// explicit_display: 显式值是 display（1）还是 game（0），仅当 explicit_set=1 时有效
// derived_display: 推导值 present_interval_derived 是 display（1）还是 game（0）
// l3_result: L3 判定结果字符串（"ok" / "need_l3" / "untested" / NULL=无文件或不在表）
// 规则：显式优先；否则 推导 display 且 L3 不是 need_l3 → display（ok / untested / 无文件 / 不在表都放开）；否则 game
//   （维护者 10-29 07:0x 全能力出包「默认开、强信号才关」：未测不是关的理由；逻辑速度红走两条强信号——L3 表 need_l3、包内能力清单 fps60=0
//    经 s2a_present_fps60_gate 关掉，docs/能力卡_数据口径.md 第 7 节）
static inline int s2a_present_decide(int explicit_set, int explicit_display, int derived_display, const char *l3_result) {
    if (explicit_set) return explicit_display;  // 显式 present_interval 优先（L3 不覆盖）
    if (!derived_display) return 0;  // 推导 game → game
    // 推导 display：只有 L3 判定 need_l3（逻辑速度红、必须补时间步）才退回 game
    return l3_result && !strcmp(l3_result, "need_l3") ? 0 : 1;
}
// 60 帧开关（S2A_FPS60，同 runtime/src/fps.c fps_on_at_start：只认整串 "0" 为关）：关 = 回游戏原帧率，档里的 present_interval display（显式或推导）也不生效
// 11-11 维护者：60 帧改成玩家手动开（默认游戏原本的帧率）——放开出图间隔只在 S2A_FPS60=1 时；没设 / 0 / 别的值一律退回游戏自己的间隔
static inline int s2a_present_fps60_gate(int display, const char *fps60_env) { return display && fps60_env && !strcmp(fps60_env, "1"); }
