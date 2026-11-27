// 回放核心的资源池与绑定构造单测：与 t_nvn_replay.c 共用同一份替身（重命名它的 main 后 include）。
// 覆盖两块改动：
//   ① 贴图内容检查按「全局写代号 + 段号 + 映射代际」只查一次（rp_res.c）；
//   ② 绑定构造省重（rp_draw.c）：B1 整组沿用上一笔、B3 驱动段按内容命中、B2 换管线按布局立脏。
// 期望值都由用例自己写下的输入直接得出，并经 rp_perf_take / 替身计数核对；带负对照。
#define main replay_fixture_main
#define rp_shader_resolve fixture_shader_resolve
#define nvn_plat_flag fixture_plat_flag
#define nvn_dirty_since fixture_dirty_since
#define nvn_dirty_protect fixture_dirty_protect
#define nvn_gpu_cmd_commit fixture_gpu_cmd_commit
#define nvn_gpu_tex_upload_blocklinear fixture_upload_blocklinear
#include "t_nvn_replay.c"
#undef nvn_gpu_tex_upload_blocklinear
#undef nvn_gpu_cmd_commit
#undef nvn_dirty_protect
#undef nvn_dirty_since
#undef nvn_plat_flag
#undef rp_shader_resolve
#undef main
#include <stdio.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t heap_in_use(void) { malloc_statistics_t s; malloc_zone_statistics(malloc_default_zone(), &s); return s.size_in_use; }
#else
#include <malloc.h>
static size_t heap_in_use(void) { return (size_t)mallinfo2().uordblks; }
#endif
#include <sys/wait.h>
#include <unistd.h>

// 负对照开关：环境变量 T_NVN_RES_FLAGS_OFF=1 把三个省重开关都关掉（走改动前的旧行为）。
// 正常跑（不设变量）：三个开关缺省开，全绿；设了变量再跑：贴图纪元 / 整组沿用 / 驱动段命中三处必须变红。
static int res_flags_off(void) {
    static int v = -1; static unsigned k;
    if (v < 0 || !(++k & 255)) { const char *e = getenv("T_NVN_RES_FLAGS_OFF"); v = e && *e && *e != '0'; }
    return v;
}
static int g_bc_byte_equal;
int nvn_plat_flag(const char *name, int dflt) {
    if (g_bc_byte_equal && !strcmp(name, "nvk_tex_byte_equal")) return 1;
    if (res_flags_off() && (!strcmp(name, "nvk_bind_fast") || !strcmp(name, "nvk_drv_memo") || !strcmp(name, "nvk_tex_epoch"))) return 0;
    return fixture_plat_flag(name, dflt);
}

// 逐页比对的独立账：另记被调次数和它要比的页数（按替身的 4096 字节页），用来核 RP_PERF_TEX_CHECK_PAGES。
// 块内存（g_block_mem）按逐页代号答（同真实现：任一页代号 > stamp 即脏），其余内存照旧用替身的全局答法。
static uint64_t g_since_calls, g_since_pages;
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    g_since_calls++;
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr; g_since_pages += (a + len - 1) / PG - a / PG + 1;
    if (g_block_mem && a >= (uintptr_t)g_block_mem && a + len <= (uintptr_t)g_block_mem + BLOCK_TEST_PAGES * PG) {
        for (size_t k = (a - (uintptr_t)g_block_mem) / PG, e = (a + len - 1 - (uintptr_t)g_block_mem) / PG; k <= e; k++) if (g_block_gen[k] > stamp) return 1;
        return 0;
    }
    return fixture_dirty_since(addr, len, stamp);
}

// 程序切换：替身只会给 g_fprog；B2 要两个程序对象，这里在用例指定时换成另一个。
static struct rp_prog *g_alt_prog;
int rp_shader_resolve(const struct rp_shader_req *q, const struct rp_prog **out) {
    if (g_alt_prog) { *out = g_alt_prog; return 0; }
    return fixture_shader_resolve(q, out);
}
static void res_prog_min(void) {   // 最小程序：没有绑定项；用例再按需加布局项
    memset(&g_fprog, 0, sizeof g_fprog);
    for (int st = 0; st < RP_PROG_STAGES; st++) for (int i = 0; i < RP_PROG_ENT_MAX; i++) g_fprog.ent[st][i].src_reg2 = UINT16_MAX;
    g_fprog.sh[0] = &g_fsh[0]; g_fprog.sh[1] = &g_fsh[1]; g_fprog.support_bytes = 2608; g_fprog.color_out_mask = 0xffffffffu;
}

// ---------- ① 贴图内容纪元（rp_res.c）----------
// 同一段里连续用同一张 CPU 贴图：第二次只比全局写代号，不扫脏页（RP_PERF_TEX_EPOCH +1），上传次数不变。
// 换段（新一次提交）必重核一次；游戏写过（写代号推进）必须重传；WriteTexels 强制作废纪元。
// 负对照：不是被写跟踪的内存不走快路，照旧每次保守查。
static void t_res_tex_epoch(void) {
    res_fresh();
    nvn_be_sub_begin();
    memset(g_pmem, 0, sizeof g_pmem);
    memset(g_pmem, 0x11, 8 * 8 * 4);
    tex_t t = mk_tex(900, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    t.mem = g_pmem;                                    // 放在被写跟踪的测试内存里（nvn_dirty_span 认得它）
    uint64_t u0 = rp_res_uploads();
    uint64_t p[RP_PERF_N];
    rp_perf_take(p);                                   // 清计数
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 1);   // 首用：整张传一次
    rp_perf_take(p);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 1);   // 同段第二次：不传
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 1);                                               // 而且根本没扫脏页（纪元命中）
    // 换一次提交（段号变）：内容没脏，重核一次但不重传；同一段里下一次又走纪元快路
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); nvn_be_sub_begin();
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 1);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 0);                   // 换段必重核（负对照：没有跳过）
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 1);
    // 游戏写过：写代号推进，必须重传一次；之后同段再次使用回到纪元快路
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_pg_gen[0] = g_dirty_written;   // 真写同时推进被写页的代号（脏页日志按它出事件）
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 2);
    rp_perf_take(p);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 2);
    rp_perf_take(p);
    CHECK(p[RP_PERF_TEX_EPOCH] == 1);
    // WriteTexels（nvn_be_tex_cpu_written）：纪元清零 → 强制重传
    nvn_be_tex_cpu_written(&t);
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && rp_res_uploads() == u0 + 3);
    // 负对照：内存不在写跟踪范围（g_tmem）→ 没有纪元可依，照旧每次保守查，一次都不跳过
    tex_t u = mk_tex(901, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    memset(u.mem, 0x22, u.L.size);
    CHECK(rp_tex_resolve(&u, &g_fake_cmd) != NULL);
    uint64_t u1 = rp_res_uploads();
    rp_perf_take(p);
    for (int i = 0; i < 4; i++) CHECK(rp_tex_resolve(&u, &g_fake_cmd) != NULL);
    CHECK(p[RP_PERF_TEX_EPOCH] == 0);
    CHECK(rp_res_uploads() == u1);                     // 没脏：也不重传
    nvn_be_tex_release(&t); nvn_be_tex_release(&u);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
}

// ---------- ①b 无关写不该让贴图整张逐页比（复现 9000 的「取贴图视图」持锁段，修法：贴图核对接脏页日志）----------
// 同一段里 N 张已传过的 CPU 贴图（块内存，手动日志替身：fake_log_write 同时改内容、逐页代号、全局代号并追加事件）；
// 每笔绘制前游戏写一次同池里贴图之外的一页（常量缓冲那类），每笔把 N 张各取两次。可选：第 WD 笔另写贴图 3 的一页；
// wrap = 1 时第 WD 笔再追加 70 条无关事件（超过替身日志容量 64，读者判环绕）。
// 纪元键仍是全局写代号，所以每笔第一次取都「没命中」；修后没命中时读日志求交，不再逐页比。
enum { SC_N = 8, SC_D = 32, SC_WD = 10, SC_W = 128, SC_H = 128, SC_UBO = 200 };
struct scene { uint64_t miss, skip, pages, since, log_n, log_ev, fallback, up[SC_N], unarmed; };
// 加保护没落地（8E 真机影子核对：日志判净、逐页判脏）：首传贴图 4 时它的第 2 页正被另一线程解保护（nvn_dirty_page_armed 答 0），
//   那次写在首传读完内容之后才落、且不缺页（页还可写），日志里没有事件。1 = 写的代号晚于首传取的代号（逐页比看得见、日志看不见）；
//   2 = 代号不晚于首传取的代号（解保护在取代号之前推进了代号：两条路都看不见）。修法：上传时有页没重新只读，这次上传不算同步，下一次必重传。
static int g_sc_unarmed;
static struct scene tex_log_scene(int log_on, int write_tex3, int wrap) {
    struct scene r; memset(&r, 0, sizeof r);
    const int saved_flag = g_flag_buf_dirtylog;
    res_fresh();
    g_flag_buf_dirtylog = log_on; g_log_fake_on = 1; g_log_fake_bad = 0; g_log_head = 0; g_log_ident++; memset(g_log_unarmed, 0, sizeof g_log_unarmed);
    g_dirty_now = 100; ++g_dirty_map; g_block_unreliable = 0;
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    memset(g_block_gen, 0, sizeof g_block_gen);
    nvn_be_sub_begin();
    g_tp.mem = g_block_mem;
    g_tp.size = BLOCK_TEST_PAGES * PG;
    tex_t t[SC_N];
    for (int i = 0; i < SC_N; i++) {
        t[i] = mk_tex(950 + (uint64_t)i, 0, NVN_FORMAT_RGBA8, SC_W, SC_H, 1, 0);
        t[i].off = (int64_t)(i * t[i].L.size);
        t[i].mem = g_tp.mem + t[i].off;   // 各占 16 页：贴图 i 在第 16i..16i+15 页
        memset(t[i].mem, 0x30 + i, t[i].L.size);
        const size_t up = (size_t)i * (t[i].L.size / PG) + 2;
        if (g_sc_unarmed && i == 4) { g_log_unarmed[up] = 1; if (g_sc_unarmed == 2) g_block_gen[up] = g_dirty_now; }
        CHECK(rp_tex_resolve(&t[i], &g_fake_cmd) != NULL);   // 首用：整张传
        if (g_sc_unarmed && i == 4) {   // 并发的那次写在读完内容之后才落；页可写，不缺页、不进日志
            memset(t[i].mem + 2 * PG, 0x99, PG); if (g_sc_unarmed == 1) g_block_gen[up] = ++g_dirty_now;
            g_log_unarmed[up] = 0;       // 之后再加保护就能落地
        }
    }
    const size_t pages_per = t[0].L.size / PG;
    uint64_t p[RP_PERF_N]; rp_perf_take(p);
    g_since_calls = g_since_pages = 0;
    for (int d = 0; d < SC_D; d++) {
        fake_log_write(SC_UBO + (size_t)(d % 8), (uint8_t)d);                 // 无关写：贴图之外的一页
        if (write_tex3 && d == SC_WD) fake_log_write(3 * pages_per + 5, 0x77);   // 真写贴图 3 的一页
        if (wrap && d == SC_WD) for (int k = 0; k < 70; k++) fake_log_event(SC_UBO + 20, SC_UBO + 20);   // 日志环绕
        for (int rr = 0; rr < 2; rr++) for (int i = 0; i < SC_N; i++) {
            uint64_t u0 = rp_res_uploads();
            CHECK(rp_tex_resolve(&t[i], &g_fake_cmd) != NULL);
            r.up[i] += rp_res_uploads() - u0;
        }
    }
    rp_perf_take(p);
    r.miss = p[RP_PERF_TEX_EPOCH_MISS]; r.skip = p[RP_PERF_TEX_EPOCH]; r.pages = p[RP_PERF_TEX_CHECK_PAGES]; r.since = g_since_calls;
    r.log_n = p[RP_PERF_TEX_LOG_N]; r.log_ev = p[RP_PERF_TEX_LOG_EV]; r.fallback = p[RP_PERF_TEX_LOG_FALLBACK]; r.unarmed = p[RP_PERF_TEX_UNARMED];
    CHECK(r.pages == g_since_pages);                    // 页数计数与独立账一致
    for (int i = 0; i < SC_N; i++) nvn_be_tex_release(&t[i]);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_block_mem = NULL; g_log_fake_on = 0; g_flag_buf_dirtylog = saved_flag;
    printf("tex-epoch scene log=%d write=%d wrap=%d: miss=%llu skip=%llu pages=%llu since=%llu log_checks=%llu log_events=%llu fallback=%llu up3=%llu up_other=%llu\n",
           log_on, write_tex3, wrap, (unsigned long long)r.miss, (unsigned long long)r.skip, (unsigned long long)r.pages, (unsigned long long)r.since,
           (unsigned long long)r.log_n, (unsigned long long)r.log_ev, (unsigned long long)r.fallback, (unsigned long long)r.up[3],
           (unsigned long long)(r.up[0] + r.up[1] + r.up[2] + r.up[4] + r.up[5] + r.up[6] + r.up[7]));
    return r;
}
static void t_res_tex_epoch_unrelated(void) {
    const uint64_t ND = (uint64_t)SC_N * SC_D, PP = (uint64_t)SC_W * SC_H * 4 / PG;
    // 日志关（现状逐字）：每笔第一次取没命中、整张逐页比
    struct scene off = tex_log_scene(0, 0, 0);
    CHECK(off.miss == ND && off.skip == ND && off.since == ND && off.pages == ND * PP && off.log_n == 0 && off.fallback == 0);
    for (int i = 0; i < SC_N; i++) CHECK(off.up[i] == 0);
    // 日志开：没命中照计，但一页都不比；每次只读到那一条无关写事件；不重传
    struct scene on = tex_log_scene(1, 0, 0);
    CHECK(on.miss == ND && on.skip == ND && on.since == 0 && on.pages == 0 && on.log_n == ND && on.log_ev == ND && on.fallback == 0);
    for (int i = 0; i < SC_N; i++) CHECK(on.up[i] == 0);
    // 真写贴图 3：两条路判脏一致（只有贴图 3 重传一次）；日志路仍一页不比，写那一笔每张多读一条事件
    struct scene offw = tex_log_scene(0, 1, 0), onw = tex_log_scene(1, 1, 0);
    for (int i = 0; i < SC_N; i++) { CHECK(offw.up[i] == (i == 3)); CHECK(onw.up[i] == offw.up[i]); }
    CHECK(onw.pages == 0 && onw.since == 0 && onw.fallback == 0 && onw.log_ev == ND + SC_N);
    // 日志环绕：每张退回一次（重起游标 + 整张逐页比），之后回到日志路；判脏仍一致（贴图 3 的写不能漏）
    struct scene onx = tex_log_scene(1, 1, 1);
    for (int i = 0; i < SC_N; i++) CHECK(onx.up[i] == offw.up[i]);
    CHECK(onx.fallback == SC_N && onx.since == SC_N && onx.pages == SC_N * PP && onx.log_n == ND - SC_N);
    CHECK(off.unarmed == 0 && on.unarmed == 0 && onw.unarmed == 0);   // 负对照：加保护都落地时一次都不计、不多传
}
// 加保护没落地：两种代号先后 × 日志开关，贴图 4 都必须重传恰好一次（重传时页已重新只读，之后回到日志路 / 纪元，不再多传）；别的贴图不受影响。
static void t_res_tex_unarmed(void) {
    for (int v = 1; v <= 2; v++) for (int lg = 0; lg <= 1; lg++) {
        g_sc_unarmed = v;
        struct scene s = tex_log_scene(lg, 0, 0);
        g_sc_unarmed = 0;
        CHECK(s.up[4] == 1);
        CHECK(s.unarmed == 0);   // 计数在首传那次（rp_perf_take 之前）；重传那次已落地
        for (int i = 0; i < SC_N; i++) if (i != 4) CHECK(s.up[i] == 0);
        if (s.up[4] != 1) printf("  unarmed v=%d log=%d: 贴图 4 重传 %llu 次（期望 1）\n", v, lg, (unsigned long long)s.up[4]);
    }
}

// ---------- ② B1 整组沿用 + B3 驱动段按内容命中（rp_draw.c）----------
// 连续四笔同一程序、同一 base_instance：
//   ① 首次：三个阶段各发一次绑定（完整构造）；
//   ② 参数没变：三个阶段都直接沿用（BIND_FAST +3，一次 bind 都不发）；
//   ③ 换常量缓冲：只有顶点阶段重建；驱动段内容（存储缓冲表 / base_instance / 贴图句柄表）没变 → 命中上次的暂存（SLICE_HIT +2）；
//   ④ 换贴图：只有片元阶段重建（DRAW 阶段快路 +2）；贴图句柄表变了 → 驱动段必须重做（SLICE_HIT +0）。
static void t_res_bind_fast(void) {
    pass_fresh(); res_fresh();
    g_resolve_rc = g_cresolve_rc = 0; g_gdraws = 0;
    memset(g_pg_gen, 0, sizeof g_pg_gen); g_pg_tracked = 1; g_dirty_now = 100; g_dirty_written = 0;
    memset(g_pmem, 0, sizeof g_pmem); g_arena_used = 0;
    uint64_t base = (uint64_t)(uintptr_t)g_pmem;
    nvn_be_pool_add(g_pmem, sizeof g_pmem);
    res_prog_min();
    g_fprog.nent[0] = 4;
    g_fprog.ent[0][0].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][0].src_kind = NVN_RB_SUPPORT;
    g_fprog.ent[0][1].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][1].src_kind = NVN_RB_CBUF; g_fprog.ent[0][1].src_reg = 2;
    g_fprog.ent[0][2].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][2].src_kind = NVN_RB_DRV0;
    g_fprog.ent[0][3].cls = NVN_GPU_RES_UBO; g_fprog.ent[0][3].src_kind = NVN_RB_DRV2;   // 换常量缓冲时驱动段内容没变：两片都该命中
    g_fprog.nent[1] = 2;
    g_fprog.ent[1][0].cls = NVN_GPU_RES_TEX_SAMP; g_fprog.ent[1][0].src_kind = NVN_RB_TEX; g_fprog.ent[1][0].src_reg = 5;
    g_fprog.dtt[1][0] = NVN_TEXTURE_TARGET_2D + 1;
    g_fprog.ent[1][1].cls = NVN_GPU_RES_UBO; g_fprog.ent[1][1].src_kind = NVN_RB_DRV2;
    tex_t rtt = mk_tex(910, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    tex_t tx = mk_tex(911, 0x4000, NVN_FORMAT_RGBA8, 8, 8, 1, 0), ty = mk_tex(912, 0x5000, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_be_reg_tex(7, &tx, NULL); nvn_be_reg_tex(8, &ty, NULL); nvn_be_reg_samp(3, &sp);
    memset(g_pmem + PG + 128, 0x11, 64); memset(g_pmem + PG + 512, 0x22, 64);
    stream s = {0};
    put_set_rt(&s, &rtt, NULL);
    p_bind_vattr *va = put(&s, OP_BIND_VATTR, sizeof *va); va->n = 1; va_st a0 = {.fmt = 0x2A, .offset = 0, .stream = 1}; memcpy(&va->st[0], &a0, 4);
    p_bind_vstream *vs = put(&s, OP_BIND_VSTREAM, sizeof *vs); vs->n = 2; vs->st[1].stride = 16;
    p_bind_vbuf_single *vb = put(&s, OP_BIND_VBUF, sizeof *vb); vb->first = 1; vb->size = 64; vb->addr = base + 2 * PG + 32 - 0x1000;
    p_bind_handles *bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 7 | 3 << 20;
    p_bind_buf *ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 128; ub->size = 64;
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;   // ① 首笔
    size_t o1 = s.n;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;           // ② 什么都没变
    size_t o2 = s.n;
    ub = put(&s, OP_BIND_UBO, sizeof *ub); ub->stage = 0; ub->index = 2; ub->addr = base + PG + 512; ub->size = 64;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;           // ③ 换常量缓冲
    size_t o3 = s.n;
    bh = put(&s, OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 5; bh->n = 1; bh->h[0] = 8 | 3 << 20;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;           // ④ 换贴图
    end(&s);
    memset(g_bind_calls, 0, sizeof g_bind_calls);
    nvn_be_sub_begin();
    const op_hdr eh = {OP_END, 8}; uint8_t sv[8]; uint64_t pf[RP_PERF_N];
    memcpy(sv, s.b + o1, 8); memcpy(s.b + o1, &eh, 8);
    nvn_be_exec(s.b);                                                 // ①
    CHECK(g_gdraws == 1 && g_bind_calls[0] == 1 && g_bind_calls[1] == 1 && g_bind_calls[2] == 1);
    rp_perf_take(pf);
    memcpy(s.b + o1, sv, 8);
    memcpy(sv, s.b + o2, 8); memcpy(s.b + o2, &eh, 8);
    nvn_be_exec(s.b + o1);                                            // ②
    CHECK(g_gdraws == 2 && g_bind_calls[0] == 1 && g_bind_calls[1] == 1 && g_bind_calls[2] == 1);   // 一次 bind 都没发
    rp_perf_take(pf); CHECK(pf[RP_PERF_BIND_FAST] == 3);              // 三个阶段都整组沿用
    memcpy(s.b + o2, sv, 8);
    memcpy(sv, s.b + o3, 8); memcpy(s.b + o3, &eh, 8);
    rp_perf_take(pf);
    nvn_be_exec(s.b + o2);                                            // ③
    CHECK(g_gdraws == 3 && g_bind_calls[0] == 2 && g_bind_calls[1] == 1 && g_bind_calls[2] == 1);   // 只有顶点阶段重建
    rp_perf_take(pf);
    CHECK(pf[RP_PERF_BIND_FAST] == 2);                          // 没变的两阶段整组沿用
    CHECK(pf[RP_PERF_BIND_SLICE_HIT] == 2);                     // DRV0 / DRV2 内容没变：命中上次那片
    memcpy(s.b + o3, sv, 8);
    rp_perf_take(pf);
    nvn_be_exec(s.b + o3);                                            // ④
    CHECK(g_gdraws == 4 && g_bind_calls[0] == 2 && g_bind_calls[1] == 2 && g_bind_calls[2] == 1);   // 只有片元阶段重建
    rp_perf_take(pf);
    CHECK(pf[RP_PERF_BIND_FAST] == 2);                          // 没变的两阶段整组沿用
    CHECK(pf[RP_PERF_BIND_SLICE_HIT] == 0);                     // 贴图句柄表变了：驱动段必须重做
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&rtt); nvn_be_tex_release(&tx); nvn_be_tex_release(&ty); nvn_be_pool_del(g_pmem);
    pass_fresh();
}

// ---------- ③ B2 换管线按布局立脏（rp_draw.c）----------
// 用的是「换管线之后这一笔被裁剪掉」的路径：绘制没发出，rp_args_dirty 留在原地，正好核对换管线时立了什么脏。
//   ① 基线（程序 A）；
//   ② 换成程序对象 B（布局与 A 相同）：布局等同 → 一个阶段的绑定都不立脏；
//   ③ 把 B 的顶点布局改出差异：只有顶点阶段立脏，片元 / 几何不动。
static void t_res_pipe_layout(void) {
    static struct rp_prog prog2;
    pass_fresh(); res_fresh();
    g_resolve_rc = 0; g_alt_prog = NULL; g_gdraws = 0;
    res_prog_min();
    prog2 = g_fprog;
    prog2.sh[0] = &g_fsh[2];   // 另一个程序对象（着色器不同、布局相同）
    tex_t rtt = mk_tex(920, 0, NVN_FORMAT_RGBA8, 64, 36, 1, 0);
    stream s = {0};
    put_set_rt(&s, &rtt, NULL);
    p_draw *dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;
    size_t o1 = s.n;
    p_scissor *sc = put(&s, OP_SCISSOR, sizeof *sc); sc->x = 1000; sc->y = 0; sc->w = 10; sc->h = 10;   // 整个在目标外：这一笔画不出东西
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;
    size_t o2 = s.n;
    dn = put(&s, OP_DRAW, sizeof *dn); dn->prim = 4; dn->count = 3; dn->instances = 1;
    end(&s);
    nvn_be_sub_begin();
    const op_hdr eh = {OP_END, 8}; uint8_t sv[8];
    memcpy(sv, s.b + o1, 8); memcpy(s.b + o1, &eh, 8);
    nvn_be_exec(s.b);                                 // ① 基线：画成功，脏位清零
    CHECK(g_gdraws == 1 && rp_args_dirty[0] == 0 && rp_args_dirty[1] == 0);
    int d0 = g_gdraws;
    memcpy(s.b + o1, sv, 8);
    memcpy(sv, s.b + o2, 8); memcpy(s.b + o2, &eh, 8);
    g_alt_prog = &prog2;
    rp_pso_dirty = 1;                                 // 强制换一次管线（布局判定的用例）
    nvn_be_exec(s.b + o1);                            // ② 换程序对象、布局等同
    CHECK(g_gdraws == d0 && rp_args_dirty[0] == 0 && rp_args_dirty[1] == 0 && rp_args_dirty[2] == 0);
    memcpy(s.b + o2, sv, 8);
    prog2.nent[0] = 1; prog2.ent[0][0].cls = NVN_GPU_RES_UBO; prog2.ent[0][0].src_kind = NVN_RB_CBUF; prog2.ent[0][0].src_reg = 2;
    rp_pso_dirty = 1;
    nvn_be_exec(s.b + o2);                            // ③ 布局变了：只有顶点阶段立脏
    CHECK(g_gdraws == d0 && rp_args_dirty[0] == 1 && rp_args_dirty[1] == 0 && rp_args_dirty[2] == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_alt_prog = NULL; nvn_be_tex_release(&rtt);
    pass_fresh();
}

// ---------- ④ 贴图存储伸出内存池（rp_res.c tex_window / tex_src）----------
// 真机（丝之歌等多款 Unity 游戏，苹果端红行「storage = pool + offset 0xffffffffffffffff」）：窗口那张 1920x1080 贴图的存储偏移是 -1，
//   存储第一个字节落在池前一页，那页游戏没映射。用例照搬这个形态：池前一页 PROT_NONE，池内偏移 i 的字节 = pool_byte(i)（从不为 0）。
//   改前：判内容扫全段 / 上传反交织都读池前那一字节，段错误。改后：池外字节按 0（同参照实现「读没映射的页得 0」），池内照取；写跟踪只罩池内那段。
// 期望值不经被测实现：存储偏移 s 的字节 = s 在池外 ? 0 : pool_byte(s - 1)；第 0 行第 x 字节在 GOB 里的偏移按公开 GOB 公式（y = 0）= (x%64/32)·256 + (x%32/16)·32 + x%16。
static uint8_t pool_byte(uint64_t i) { return (uint8_t)(i % 251 + 1); }
static uint8_t store_byte(uint64_t s) { return s == 0 ? 0 : pool_byte(s - 1); }   // 偏移 -1：存储偏移 0 在池外
static uint64_t gob_x(uint32_t x) { return (uint64_t)(x % 64 / 32) * 256 + (x % 32 / 16) * 32 + x % 16; }
static void t_res_tex_before_pool(void) {
    nvn_be_sub_begin(); res_fresh();
    const size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED) return;
    CHECK(mprotect(m, pg, PROT_NONE) == 0);   // 池前一页：游戏没映射
    pool_t pool; memset(&pool, 0, sizeof pool); pool.mem = m + pg; pool.size = 2 * pg; pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    for (size_t i = 0; i < pool.size; i++) pool.mem[i] = pool_byte(i);
    // A：游戏用 CPU 写过（同真机 ser=3 被判 CPU 写过）→ 整张上传
    tex_t a = mk_tex(7300, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0); a.pool = &pool; a.off = -1; a.mem = pool.mem - 1;
    CHECK(a.L.size >= 512 && a.L.size <= pool.size);
    nvn_be_tex_cpu_written(&a);
    int n0 = g_nup;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == n0 + 1);
    CHECK(g_up[n0].pitch == 64 && g_up[n0].w == 16 && g_up[n0].h == 8);
    { int ok = 1; for (uint32_t x = 0; x < 64; x++) ok &= g_up[n0].first[x] == store_byte(gob_x(x));
      CHECK(ok); CHECK(g_up[n0].first[0] == 0 && g_up[n0].first[1] == pool_byte(0) && g_up[n0].first[16] == pool_byte(31) && g_up[n0].first[32] == pool_byte(255)); }
    CHECK(g_prot_addr == pool.mem && g_prot_len == a.L.size - 1);   // 写跟踪罩的是池内那段：池外字节恒为 0，不用跟踪
    nvn_be_tex_release(&a);
    // B：没标过 CPU 写：首用判「存储里有没有 CPU 数据」要扫存储，同样只扫池内那段（池内非 0 → 当 CPU 贴图上传）
    res_fresh();
    tex_t b = mk_tex(7301, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0); b.pool = &pool; b.off = -1; b.mem = pool.mem - 1;
    n0 = g_nup;
    CHECK(rp_tex_resolve(&b, &g_fake_cmd) != NULL && g_nup == n0 + 1 && g_up[n0].first[0] == 0 && g_up[n0].first[1] == pool_byte(0));
    nvn_be_tex_release(&b);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    munmap(m, 3 * pg);
}

// GPU 后端替身在入口内复制原始字节，再由 CPU 参考解排；两路均逐字对照手算 GOB。
// 池首和池尾各放一页 PROT_NONE，子进程让越界访问变成有名断言，而非中止整套。
static void t_res_tex_window_gpu_child(int tail) {
    int fail0 = g_fail; nvn_be_sub_begin(); res_fresh();
    const size_t pg = (size_t)getpagesize();
    uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED); if (m == MAP_FAILED) _exit(2);
    CHECK(mprotect(m, pg, PROT_NONE) == 0 && mprotect(m + 2 * pg, pg, PROT_NONE) == 0);
    pool_t pool = {0}; pool.mem = m + pg; pool.size = pg; pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    for (size_t i = 0; i < pg; i++) pool.mem[i] = pool_byte(i);
    tex_t a = mk_tex(7310 + (unsigned)tail * 2, 0, NVN_FORMAT_RGBA8, 16, 8, 1, 0);
    a.pool = &pool; a.off = tail ? (int64_t)pg - 256 : -1; a.mem = pool.mem + a.off; a.has_layout = 1;
    CHECK(a.L.size == 512); g_window_capture = 1; g_caps_extra &= ~NVN_GPU_CAP_GPU_DESWIZZLE;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_nup == 1);
    uint8_t cpu[512], expected[512]; memcpy(cpu, g_window_linear, sizeof cpu);
    for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 64; x++) {
        uint64_t s = (x / 32) * 256 + (y / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
        expected[y * 64 + x] = tail ? (s < 256 ? pool_byte(pg - 256 + s) : 0) : store_byte(s);
    }
    CHECK(!memcmp(cpu, expected, sizeof cpu)); nvn_be_tex_release(&a);
    res_fresh(); a.serial++; g_block_uploads = 0; g_caps_extra |= NVN_GPU_CAP_GPU_DESWIZZLE;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_block_uploads == 1 && g_nup == 0);
    uint8_t decoded[512]; CHECK(!nvn_deswizzle(&a.d, 0, 0, g_window_raw, decoded, 64));
    CHECK(!memcmp(decoded, expected, sizeof decoded)); CHECK(!memcmp(decoded, cpu, sizeof decoded));
    CHECK(g_prot_addr == pool.mem + (tail ? pg - 256 : 0) && g_prot_len == (tail ? 256 : 511));
    /* 在途 GPU 写落在池内窗口 → -2 延迟重试，不拿旧快照上传；按全长区间查会漏检（rp_pool_find 要求整段在池）。落地后窗口源照传 */
    extern uint64_t rp_buf_gpu_write_note(uint64_t, uint64_t, uint64_t *);
    extern void rp_buf_gpu_write_land(uint64_t, uint64_t, int);
    res_fresh(); a.serial++; g_block_uploads = 0; g_nup = 0; g_buf_cpu_auto = 1;
    nvn_be_pool_add(pool.mem, pg);
    nvn_gpu_buf mirror = NULL; uint64_t key = 0;
    CHECK(rp_buf_resolve(&g_fake_cmd, (uint64_t)(uintptr_t)pool.mem, pg, &mirror, NULL) && mirror && mirror->cpu);
    uint64_t gwid = rp_buf_gpu_write_note((uint64_t)(uintptr_t)pool.mem + (tail ? pg - 128 : 64), 64, &key);
    CHECK(gwid != 0);
    int un0 = nvn_unimpl_count("tex", "gpu_deswizzle_pending_pool_write");
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) == NULL && g_block_uploads == 0 && g_nup == 0);
    CHECK(nvn_unimpl_count("tex", "gpu_deswizzle_pending_pool_write") == (uint64_t)un0 + 1);
    /* B1：-2 路径不取窗口副本、不动日志游标：连拒多次堆内存不涨（旧实现每次重试泄一份 L.size 副本） */
    size_t h0 = heap_in_use();
    for (int i = 0; i < 8; i++) CHECK(rp_tex_resolve(&a, &g_fake_cmd) == NULL && g_block_uploads == 0);
    CHECK(heap_in_use() - h0 < 512);
    if (gwid) rp_buf_gpu_write_land(key, gwid, 1);
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_block_uploads == 1);
    nvn_be_tex_release(&a); nvn_be_pool_del(pool.mem); g_buf_cpu_auto = 0;
    /* 诊断开：快照取与上传同一份窗口源（池外为 0），跟踪 / 保护只罩池内窗口；快照内容不变 / 变各验一档 */
    res_fresh(); a.serial++; g_block_uploads = 0; g_flag_tdiag = 1; g_tdiag_n = 0; g_block_mem = pool.mem; g_span_len = pg; g_prot_n = 0;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_tdiag_n == 1 && strstr(g_tdiag_lines[0], "kind=first state=initial") && strstr(g_tdiag_lines[0], "raw_bytes=512"));
    for (int i = 0; i < g_prot_n && i < 8; i++) CHECK((uintptr_t)g_prot_log[i] >= (uintptr_t)pool.mem && (uintptr_t)g_prot_log[i] + g_prot_llen[i] <= (uintptr_t)pool.mem + pg);   // 保护只罩池内窗口
    const size_t dirty_pg = tail ? pg / PG - 1 : 0;   // 窗口覆盖的跟踪页（夹具页粒度 PG，系统页可以更大）
    g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[dirty_pg] = g_dirty_written;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_tdiag_n == 2 && strstr(g_tdiag_lines[1], "state=same"));
    pool.mem[tail ? pg - 1 : 0] ^= 0xff; g_dirty_written = g_dirty_now + 1; g_dirty_now += 2; g_block_gen[dirty_pg] = g_dirty_written;
    CHECK(rp_tex_resolve(&a, &g_fake_cmd) != NULL && g_tdiag_n == 3 && strstr(g_tdiag_lines[2], "state=changed"));
    g_flag_tdiag = 0; g_block_mem = NULL; g_span_len = BLOCK_TEST_PAGES * PG;
    nvn_be_tex_release(&a); nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK); munmap(m, 3 * pg);
    printf("GPU_WINDOW %s: CPU/GPU-contract bytes=512 failures=%d\n", tail ? "tail" : "before", g_fail - fail0); fflush(stdout);
    _exit(g_fail != fail0);
}
static void t_res_tex_window_gpu(void) {
    for (int tail = 0; tail < 2; tail++) {
        pid_t child = fork(); CHECK(child >= 0); if (child < 0) continue;
        if (!child) t_res_tex_window_gpu_child(tail);
        int status = 0; CHECK(waitpid(child, &status, 0) == child);
        if (WIFSIGNALED(status)) printf("GPU_WINDOW %s source access signal=%d\n", tail ? "tail" : "before", WTERMSIG(status));
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

// 只替代物理存储选择，实际资源表、脏页核对、pin 与视图重建仍走回放入口。
static unsigned g_bc1_snapshots;
static int fixture_bc1_replace(nvn_gpu_tex old, int opaque, nvn_gpu_tex *out) {
    nvn_gpu_tex_desc d = old->d;
    d.usage &= ~NVN_GPU_TEX_BC1_OPAQUE;
    if (opaque) d.usage |= NVN_GPU_TEX_BC1_OPAQUE;
    if (d.usage == old->d.usage) return 0;
    nvn_gpu_err err = {0};
    return nvn_gpu_ok(nvn_gpu_tex_create(&d, out, &err)) ? 1 : -1;
}

int nvn_gpu_tex_bc1_snapshot(nvn_gpu_cmd c, nvn_gpu_tex tex, const void *bytes, uint64_t size,
                            uint64_t serial, nvn_gpu_tex *out) {
    *out = NULL;
    CHECK(c && serial == 9390 && tex->d.d.width == 4 && tex->d.d.height == 4 && tex->d.d.levels == 1);
    uint32_t words[2] = {0};
    nvn_texlayout layout;
    nvn_tex_layout(&tex->d.d, &layout);
    CHECK(size == layout.size && nvn_deswizzle(&tex->d.d, 0, 0, bytes, words, sizeof words) == 0);
    const uint32_t selectors = words[1] & (words[1] >> 1) & UINT32_C(0x55555555);
    const int opaque = (words[0] & 0xffff) > (words[0] >> 16) || !selectors;
    g_bc1_snapshots++;
    return fixture_bc1_replace(tex, opaque, out) < 0 ? -1 : 1;
}

int nvn_gpu_tex_bc1_unwrap(nvn_gpu_cmd c, nvn_gpu_tex tex, nvn_gpu_tex *out) {
    CHECK(c);
    *out = NULL;
    return fixture_bc1_replace(tex, 0, out);
}

static void t_res_bc1_rebind(void) {
    res_fresh();
    g_caps_extra |= NVN_GPU_CAP_BC1_SNAPSHOT;
    g_bc1_snapshots = 0;
    const uint32_t opaque[] = {0x07e0f800, 0xe4e4e4e4}, punch[] = {0xf80007e0, 0xc0000000};
    tex_t t = mk_tex(9390, 0, NVN_FORMAT_RGBA_DXT1, 4, 4, 1, 0);
    t.mem = g_pmem;
    memset(t.mem, 0, t.L.size);
    memcpy(t.mem, opaque, sizeof opaque);
    nvn_be_reg_tex(3, &t, NULL);
    nvn_be_sub_begin();
    rp_view_pin_begin();
    nvn_gpu_view old = rp_view_by_id(3, &g_fake_cmd, NULL);
    nvn_gpu_view unregistered = rp_view_for(&t, NULL, &g_fake_cmd);
    CHECK(old && unregistered && (old->of->d.usage & NVN_GPU_TEX_BC1_OPAQUE));
    const int old_id = old ? old->rid : 0;
    const int unregistered_id = unregistered ? unregistered->rid : 0;
    const uint64_t gen = rp_tex_reg_gen();
    CHECK(g_bc1_snapshots == 1);

    // 只翻一个 BC1 选择码就须整图换回。热登记视图和按对象取视图都须取到新版。
    memcpy(t.mem, punch, sizeof punch);
    nvn_be_tex_cpu_written(&t);
    CHECK(rp_tex_cpu_written(t.serial) == 1);
    nvn_gpu_view next = rp_view_by_id(3, &g_fake_cmd, NULL);
    nvn_gpu_view next_unregistered = rp_view_for(&t, NULL, &g_fake_cmd);
    CHECK(next && next->rid != old_id && !(next->of->d.usage & NVN_GPU_TEX_BC1_OPAQUE));
    CHECK(next_unregistered && next_unregistered->rid != unregistered_id && next && next_unregistered->of == next->of);
    CHECK(g_bc1_snapshots == 2 && rp_tex_reg_gen() == gen + 1);
    CHECK(!was_view_released(old_id));
    rp_view_pin_end();
    CHECK(was_view_released(old_id));
    nvn_be_sub_commit();
    fake_complete(NVN_GPU_DONE_OK);

    memcpy(t.mem, opaque, sizeof opaque);
    nvn_be_tex_cpu_written(&t);
    CHECK(rp_tex_cpu_written(t.serial) == 1);
    nvn_be_sub_begin();
    nvn_gpu_tex packed = rp_tex_resolve(&t, &g_fake_cmd);
    CHECK(packed && (packed->d.usage & NVN_GPU_TEX_BC1_OPAQUE));
    nvn_gpu_tex plain = rp_tex_resolve_plain(&t, &g_fake_cmd);
    CHECK(plain && !(plain->d.usage & NVN_GPU_TEX_BC1_OPAQUE) && rp_tex_resolve(&t, &g_fake_cmd) == plain);
    CHECK(g_bc1_snapshots == 3);
    nvn_be_sub_commit();
    fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&t);
    g_caps_extra &= ~NVN_GPU_CAP_BC1_SNAPSHOT;
}

// 真写块的一处，跟踪记录整个 64 页块，随后同块内的写不再追加事件。
static void bc_neighbor_write(size_t chunk, size_t page, uint8_t v) {
    fake_log_event(chunk, chunk + 63);
    const uint64_t gen = ++g_dirty_now;
    for (size_t k = chunk; k < chunk + 64; k++) g_block_gen[k] = gen;
    g_block_mem[page * PG] = v;
}

static int g_bc_fail_pre;
static unsigned g_bc_failed_pre;
nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd c, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    if (g_bc_fail_pre && c->is_pre) {
        g_bc_fail_pre = 0;
        g_bc_failed_pre++;
        c->live = 0;
        if (err) snprintf(err->why, sizeof err->why, "fixture pre submit failure");
        nvn_gpu_done failed = {.status = NVN_GPU_DONE_FAILED, .why = "fixture pre submit failure"};
        if (done) done(user, &failed);
        return (nvn_gpu_rc){NVN_GPU_E_OOM};
    }
    return fixture_gpu_cmd_commit(c, signal, done, user, err);
}

static const void *g_bc_late_protect;
static uint8_t *g_bc_raw_capture;
static size_t g_bc_raw_size;
void nvn_dirty_protect(const void *addr, size_t len) {
    if (addr == g_bc_late_protect) {
        g_bc_late_protect = NULL;
        bc_neighbor_write(64, 64, 0x7c);  // 另一块在读取日志之后、重新只读之前变可写
    }
    fixture_dirty_protect(addr, len);
}

nvn_gpu_rc nvn_gpu_tex_upload_blocklinear(nvn_gpu_cmd c, nvn_gpu_tex tex, uint32_t level, uint32_t layer,
                                          const nvn_texlayout *layout, const void *src, uint64_t bytes) {
    if (g_bc_raw_capture) {
        CHECK(bytes == g_bc_raw_size);
        if (bytes == g_bc_raw_size) memcpy(g_bc_raw_capture, src, g_bc_raw_size);
    }
    return fixture_upload_blocklinear(c, tex, level, layer, layout, src, bytes);
}

static void t_res_bc_byte_equal(int enabled) {
    res_fresh();
    const int saved_flag = g_flag_buf_dirtylog;
    g_bc_byte_equal = enabled;
    g_flag_buf_dirtylog = g_log_fake_on = 1;
    g_log_fake_bad = 0;
    g_log_head = 0;
    g_log_ident++;
    g_block_mem = (uint8_t *)(((uintptr_t)g_block_arena + BLOCK_TEST_ALIGN - 1) & ~(uintptr_t)(BLOCK_TEST_ALIGN - 1));
    memset(g_block_mem, 0, BLOCK_TEST_PAGES * PG);
    memset(g_block_gen, 0, sizeof g_block_gen);
    memset(g_log_unarmed, 0, sizeof g_log_unarmed);
    g_tp.mem = g_block_mem;
    g_tp.size = BLOCK_TEST_PAGES * PG;
    nvn_be_pool_add(g_tp.mem, g_tp.size);
    uint64_t perf[RP_PERF_N];
    rp_perf_take(perf);

    // 4×4 BC7 的唯一个块占 16 字节，纹理起点与邻写在不同宿主页。
    tex_t t = mk_tex(9600, 12 * PG, NVN_FORMAT_BPTC_UNORM, 4, 4, 1, 0);
    t.mem = g_tp.mem + t.off;
    uint8_t expected[16];
    memset(expected, 0x25, sizeof expected);
    memcpy(t.mem, expected, sizeof expected);

    nvn_be_sub_begin();
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == 1);
    CHECK(g_up[0].img == sizeof expected && !memcmp(g_up[0].first, expected, sizeof expected));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);

    nvn_be_sub_begin();
    bc_neighbor_write(0, 10, 0x49);
    const uint64_t original = rp_res_uploads();
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
    CHECK(rp_res_uploads() == original);    // 仅分块假共享，纹理内容相同 → 零重传
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);

    nvn_be_sub_begin();
    bc_neighbor_write(0, 10, 0x4a);
    expected[7] ^= 0x51;
    t.mem[7] = expected[7];                 // 同块已可写，此写没有自己的日志事件
    const int backend_before = g_nup;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == backend_before + 1);
    CHECK(g_up[backend_before].img == sizeof expected && !memcmp(g_up[backend_before].first, expected, sizeof expected));
    const unsigned failed_before = g_bc_failed_pre;
    g_bc_fail_pre = 1;
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_bc_failed_pre == failed_before + 1);

    // 前置段失败后无额外写入，也必须重录最新纹理，而不能拿失败的快照跳过。
    nvn_be_sub_begin();
    const int retry = g_nup;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == retry + 1);
    CHECK(g_up[retry].img == sizeof expected && !memcmp(g_up[retry].first, expected, sizeof expected));
    // 提交返回成功但完成回执失败：同样不能相信已录入快照。
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_FAILED);
    nvn_be_sub_begin();
    const int async_retry = g_nup;
    CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == async_retry + 1);
    CHECK(g_up[async_retry].img == sizeof expected && !memcmp(g_up[async_retry].first, expected, sizeof expected));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&t);

    // 两个块都在同一张 512² BC7 源内。只读到第一块日志时第二块发生晚写：
    // 重新保护后必须从新源上传，该字节位于第一次比较的交集之外。
    tex_t spanning = mk_tex(9601, 12 * PG, NVN_FORMAT_BPTC_UNORM, 512, 512, 1, 0);
    spanning.mem = g_tp.mem + spanning.off;
    CHECK(spanning.L.size == 512u * 512u);
    memset(spanning.mem, 0x25, spanning.L.size);
    uint8_t joined[512 * 512];
    memset(joined, 0x25, sizeof joined);
    g_bc_raw_capture = joined;
    g_bc_raw_size = sizeof joined;
    g_caps_extra |= NVN_GPU_CAP_GPU_DESWIZZLE;
    nvn_be_sub_begin();
    CHECK(rp_tex_resolve(&spanning, &g_fake_cmd) != NULL);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_sub_begin();
    bc_neighbor_write(0, 10, 0x4b);
    g_bc_late_protect = spanning.mem;
    const uint64_t late = rp_res_uploads();
    CHECK(rp_tex_resolve(&spanning, &g_fake_cmd) != NULL && rp_res_uploads() == late + 1 && !g_bc_late_protect);
    CHECK(joined[(64 - 12) * PG] == 0x7c);
    joined[(64 - 12) * PG] = 0x25;
    int all_match = 1;
    for (size_t i = 0; i < sizeof joined; i++) if (joined[i] != 0x25) { all_match = 0; break; }
    CHECK(all_match);
    g_bc_raw_capture = NULL;
    g_caps_extra &= ~NVN_GPU_CAP_GPU_DESWIZZLE;
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&spanning);

    // BC1 走真实回放的可选完整上传入口。内容没变时不再次调用编码器。
    tex_t bc1 = mk_tex(9390, 12 * PG, NVN_FORMAT_RGBA_DXT1, 4, 4, 1, 0);
    bc1.mem = g_tp.mem + bc1.off;
    const uint32_t opaque[] = {0x07e0f800, 0xe4e4e4e4};
    memset(bc1.mem, 0, bc1.L.size);
    memcpy(bc1.mem, opaque, sizeof opaque);
    const unsigned snapshots = g_bc1_snapshots;
    g_caps_extra |= NVN_GPU_CAP_BC1_SNAPSHOT;
    nvn_be_sub_begin();
    CHECK(rp_tex_resolve(&bc1, &g_fake_cmd) != NULL && g_bc1_snapshots == snapshots + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_sub_begin();
    bc_neighbor_write(0, 10, 0x4c);
    CHECK(rp_tex_resolve(&bc1, &g_fake_cmd) != NULL && g_bc1_snapshots == snapshots + 1);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    // 同一个字节版本遇到在途 GPU 写，不能走相等快路。等它撤销后沿正常重试上传。
    nvn_be_sub_begin();
    uint64_t pool_key = 0;
    g_buf_cpu_auto = 1;
    const uint64_t pending = rp_buf_gpu_write_note((uintptr_t)bc1.mem, sizeof opaque, &pool_key);
    CHECK(pending != 0);
    bc_neighbor_write(0, 10, 0x4d);
    CHECK(rp_tex_resolve(&bc1, &g_fake_cmd) == NULL && g_bc1_snapshots == snapshots + 1);
    rp_buf_gpu_write_land(pool_key, pending, 0);
    CHECK(rp_tex_resolve(&bc1, &g_fake_cmd) != NULL && g_bc1_snapshots == snapshots + 2);
    g_buf_cpu_auto = 0;
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);

    // 换底作废旧日志身份并推进受影响页的代号；相等也照原路取客体。
    nvn_be_sub_begin();
    g_dirty_map++;
    g_log_ident++;
    bc_neighbor_write(0, 10, 0x4e);
    CHECK(rp_tex_resolve(&bc1, &g_fake_cmd) != NULL && g_bc1_snapshots == snapshots + 3);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    g_caps_extra &= ~NVN_GPU_CAP_BC1_SNAPSHOT;
    rp_perf_take(perf);
    if (enabled) {
        CHECK(perf[RP_PERF_TEX_BYTE_EQUAL] == 2 && perf[RP_PERF_TEX_BYTE_CHANGED] == 1);
        CHECK(perf[RP_PERF_TEX_BYTE_SKIPPED] == t.L.size + bc1.L.size);
    }
    nvn_be_tex_release(&bc1);
    nvn_be_pool_del(g_tp.mem);
    g_block_mem = NULL;
    g_log_fake_on = 0;
    g_flag_buf_dirtylog = saved_flag;
    g_bc_byte_equal = 0;
    res_fresh();
}

// SetRenderTargets marks its whole pool at record time. The unrelated sampler
// must still see ordinary CPU stores, both before its first use and later.
static void t_linear_in_render_pool(void) {
    pass_fresh();
    tex_t rt = mk_tex(901, 0, NVN_FORMAT_RGBA8, 16, 16, 1, 0);
    nvn_be_pool_gpu_written(rt.pool);
    stream commands = {0};
    put_set_rt(&commands, &rt, NULL);
    p_clear_color *clear = put(&commands, OP_CLEAR_COLOR, sizeof *clear);
    clear->index = 0; clear->mask = 15; clear->rgba[3] = 1;
    end(&commands);
    nvn_be_sub_begin(); nvn_be_exec(commands.b); nvn_be_sub_commit();
    fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_npd == 1 && nvn_be_tex_gpu_owned(rt.serial));

    const float exposure[2][2] = {{1.175f, 0.880f}, {0.625f, 1.25f}};
    const uint8_t color[2][4] = {{51, 102, 153, 255}, {16, 32, 64, 128}};
    for (int kind = 0; kind < 2; ++kind) {
        int fmt = kind == 0 ? NVN_FORMAT_RG32F : NVN_FORMAT_RGBA8;
        tex_t sample = mk_tex(902 + kind, 0x4000 + kind * 0x1000, fmt, 1, 1, 1, NVN_TEXTURE_FLAGS_LINEAR);
        sample.d.stride = 32;
        nvn_tex_layout(&sample.d, &sample.L);
        size_t bytes = kind == 0 ? sizeof exposure[0] : sizeof color[0];
        int first_upload = g_nup;
        for (int phase = 0; phase < 2; ++phase) {
            const void *expected = kind == 0 ? (const void *)exposure[phase] : (const void *)color[phase];
            memcpy(sample.mem, expected, bytes);
            g_dirty_written = ++g_dirty_now;
            nvn_be_sub_begin();
            CHECK(rp_tex_resolve(&sample, &g_fake_cmd) != NULL);
            nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
            CHECK(g_nup == first_upload + phase + 1);
            if (g_nup <= first_upload) continue;
            const uint8_t *actual = g_up[g_nup - 1].first;
            if (kind == 0) {
                float pixel[2]; memcpy(pixel, actual, sizeof pixel);
                printf("mixed pool RG32F phase=%d expected=(%.3f,%.3f) actual=(%.3f,%.3f)\n",
                       phase, exposure[phase][0], exposure[phase][1], pixel[0], pixel[1]);
            } else {
                printf("mixed pool RGBA8 phase=%d actual=(%u,%u,%u,%u)\n",
                       phase, actual[0], actual[1], actual[2], actual[3]);
            }
            CHECK(g_up[g_nup - 1].img == bytes && !memcmp(actual, expected, bytes));
        }
        // A GPU write takes ownership. Even a second texture object over this
        // storage must retain its GPU version after the guest page changes.
        nvn_be_sub_begin();
        nvn_gpu_tex original = rp_tex_resolve(&sample, &g_fake_cmd);
        rp_tex_mark_gpu_written(&sample);
        tex_t alias = sample;
        alias.serial += 100;
        memset(sample.mem, 0xc3, bytes);
        g_dirty_written = ++g_dirty_now;
        int before = g_nup;
        CHECK(rp_tex_resolve(&alias, &g_fake_cmd) == original);
        CHECK(rp_tex_resolve(&sample, &g_fake_cmd) == original);
        CHECK(nvn_be_tex_gpu_owned(alias.serial) && g_nup == before);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        nvn_be_tex_release(&alias);
        nvn_be_tex_release(&sample);
    }
    // Only eligible CPU-visible linear data enters dirty tracking. The regular
    // render target may take a one-time initial snapshot under its own rules.
    static const struct { int format, flags, pool_flags; } excluded[] = {
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, 0},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, 0},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_DISPLAY, 0},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_VIDEO_DECODE, 0},
        {NVN_FORMAT_DEPTH32F, NVN_TEXTURE_FLAGS_LINEAR, 0},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS},
        {NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR, NVN_MEMORY_POOL_FLAGS_VIRTUAL},
        {NVN_FORMAT_RGBA8, 0, 0},
    };
    for (size_t i = 0; i < sizeof excluded / sizeof excluded[0]; ++i) {
        pool_t pool = g_tp;
        if (excluded[i].pool_flags) pool.flags = NVN_MEMORY_POOL_FLAGS_GPU_CACHED | excluded[i].pool_flags;
        nvn_be_pool_gpu_written(&pool);
        tex_t t = mk_tex(1000 + i, 0x8000 + i * 0x1000, excluded[i].format, 1, 1, 1, excluded[i].flags);
        t.pool = &pool;
        memset(t.mem, 0x35, t.L.size);
        g_nup = 0;
        nvn_be_sub_begin();
        CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL);
        CHECK(!(g_last_usage & NVN_GPU_TEX_CPU_UPLOAD));
        if (excluded[i].flags && g_nup) CHECK(g_up[0].first[0] == 0);
        int before = g_nup;
        memset(t.mem, 0xb4, t.L.size); g_dirty_written = ++g_dirty_now;
        CHECK(rp_tex_resolve(&t, &g_fake_cmd) != NULL && g_nup == before);
        nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
        nvn_be_tex_release(&t);
        nvn_be_alias_drop_pool(&pool);
    }
    // Separate serial and description, with only a partial byte overlap.
    tex_t overlap = mk_tex(1100, 32, NVN_FORMAT_RG32F, 1, 1, 1, NVN_TEXTURE_FLAGS_LINEAR);
    memset(overlap.mem, 0x7e, overlap.L.size);
    g_nup = 0;
    nvn_be_sub_begin();
    CHECK(rp_tex_resolve(&overlap, &g_fake_cmd) != NULL);
    CHECK(!(g_last_usage & NVN_GPU_TEX_CPU_UPLOAD) && g_nup == 1 && g_up[0].first[0] == 0);
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&overlap);
    // A buffer write to the linear image's own bytes is still in flight.
    // The mirror is authoritative until retirement makes those bytes visible.
    nvn_be_pool_add(g_tmem, sizeof g_tmem);
    tex_t pending = mk_tex(1101, 0x18000, NVN_FORMAT_RG32F, 1, 1, 1, NVN_TEXTURE_FLAGS_LINEAR);
    nvn_be_sub_begin();
    int saved_auto = g_buf_cpu_auto;
    g_buf_cpu_auto = 1;
    nvn_gpu_buf mirror = NULL;
    uint64_t off = 0, key = 0, addr = (uintptr_t)pending.mem;
    CHECK(rp_buf_resolve(&g_fake_cmd, addr, 8, &mirror, &off) && mirror && mirror->cpu);
    uint64_t id = rp_buf_gpu_write_note(addr, 8, &key);
    CHECK(id != 0);
    int uploads = g_nup;
    CHECK(rp_tex_resolve(&pending, &g_fake_cmd) == NULL && g_nup == uploads);
    CHECK(nvn_unimpl_count("tex", "linear_pending_pool_write") == 1);
    if (mirror && mirror->cpu) memcpy(mirror->cpu + off, exposure[1], 8);
    if (id) rp_buf_gpu_write_land(key, id, 1);
    CHECK(rp_tex_resolve(&pending, &g_fake_cmd) != NULL && g_nup == uploads + 1 &&
          !memcmp(g_up[uploads].first, exposure[1], 8));
    nvn_be_sub_commit(); fake_complete(NVN_GPU_DONE_OK);
    nvn_be_tex_release(&pending);
    nvn_be_pool_del(g_tmem);
    g_buf_cpu_auto = saved_auto;
    nvn_be_tex_release(&rt);
    pass_fresh();
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--bc-byte-equal") || !strcmp(argv[1], "--bc-byte-equal-negative"))) {
        t_res_bc_byte_equal(!strcmp(argv[1], "--bc-byte-equal"));
        printf("BC source equality: %d checks, %d failures\n", g_checks, g_fail);
        return g_fail ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--dirtylog-off")) g_flag_buf_dirtylog = 0;   // 整套以开关关再跑一遍：贴图核对走逐页退回路径
    if (argc > 1 && !strcmp(argv[1], "--gpu-window")) { t_res_tex_window_gpu(); return g_fail ? 1 : 0; }
    t_res_bc1_rebind();
    if (argc > 1 && !strcmp(argv[1], "--bc1-rebind")) {
        printf("BC1 view rebind: %d checks, %d failures\n", g_checks, g_fail);
        return g_fail ? 1 : 0;
    }
    t_linear_in_render_pool();
    t_res_tex_epoch();
    t_res_bc_byte_equal(1);
    t_res_tex_epoch_unrelated();
    t_res_tex_unarmed();
    t_res_bind_fast();
    t_res_pipe_layout();
    t_res_tex_before_pool();
    t_res_tex_window_gpu();
    printf("t_nvn_res：%d 项检查，%d 项红\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
