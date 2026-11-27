// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 独立录制流夹具：真实回放核心，后端替身记录清除、通道和顺序。
#define nvn_gpu_view_create fixture_view_create
#define main replay_fixture_main
#include "t_nvn_replay.c"
#undef main
#undef nvn_gpu_view_create

static view_t g_seen_view[16]; static int g_nseen_view;
nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex t, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (v && g_nseen_view < 16) g_seen_view[g_nseen_view++] = *v;
    return fixture_view_create(t, v, out, err);
}

static struct { int n, npd, ends, is_int; float f[4]; uint32_t u[4]; } g_cv;
void nvn_gpu_clear_view(nvn_gpu_cmd c, nvn_gpu_view v, const float rgba[4], const uint32_t u4[4], int is_int) {
    (void)c; CHECK(v != NULL); g_cv.n++; g_cv.npd = g_npd; g_cv.ends = g_pass_ends; g_cv.is_int = is_int;
    if (rgba) memcpy(g_cv.f, rgba, sizeof g_cv.f);
    if (u4) memcpy(g_cv.u, u4, sizeof g_cv.u);
}
static p_clear_tex *clear_op(stream *s, const tex_t *t, int x, int y, int w, int h, int mask) {
    p_clear_tex *p = put(s, OP_CLEAR_TEX, sizeof *p); p->tex = t;
    p->r.xoffset = x; p->r.yoffset = y; p->r.width = w; p->r.height = h; p->r.depth = 1;
    p->mask = mask; p->value_type = CLEAR_TEX_FLOAT;
    p->c.f[0] = 0.25f; p->c.f[1] = 0.5f; p->c.f[2] = 0.75f; p->c.f[3] = 1.0f;
    return p;
}
static void clear_fresh(void) { pass_fresh(); memset(&g_cv, 0, sizeof g_cv); g_nseen_view = 0; }
static void test_whole_and_order(void) {
    clear_fresh(); tex_t t = mk_tex(201, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; put_set_rt(&s, &t, NULL);
    p_clear_color *c = put(&s, OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 15; c->rgba[0] = 0.125f;
    clear_op(&s, &t, 0, 0, 8, 8, 15); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.npd == 1 && g_cv.ends == 1);
    CHECK(!g_cv.is_int && g_cv.f[0] == 0.25f && g_cv.f[2] == 0.75f && g_cv.f[3] == 1.0f);
    CHECK(g_pd[0].color[0].load == NVN_GPU_LOAD_CLEAR && g_pd[0].color[0].clear.f[0] == 0.125f);
    CHECK(nvn_unimpl_count("op", "OP_CLEAR_TEX") == 0);
}
static void test_partial(void) {
    clear_fresh(); tex_t t = mk_tex(202, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; clear_op(&s, &t, 2, 1, 3, 4, 5); end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 1 && g_pass_ends == 1 && g_ncr == 1);
    CHECK(g_pd[0].ncolor == 1 && g_pd[0].w == 8 && g_pd[0].h == 8 && g_pd[0].color[0].load == NVN_GPU_LOAD_LOAD && g_pd[0].color[0].store == NVN_GPU_STORE_STORE);
    CHECK(g_cr[0].d.x == 2 && g_cr[0].d.y == 1 && g_cr[0].d.w == 3 && g_cr[0].d.h == 4 && g_cr[0].d.color_mask == 5);
    CHECK(g_cr[0].d.value.f[0] == 0.25f && g_cr[0].d.value.f[2] == 0.75f && g_cr[0].npd == 1 && g_cr[0].ends == 0);
    CHECK(nvn_unimpl_count("op", "OP_CLEAR_TEX") == 0);
}
static void test_empty_and_bounds(void) {
    clear_fresh(); tex_t t = mk_tex(203, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream s = {0}; clear_op(&s, &t, 0, 0, 8, 8, 0); clear_op(&s, &t, 0, 0, 0, 8, 15);
    end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 0 && g_ncr == 0 && nvn_unimpl_kinds() == 0);
    clear_fresh(); stream b = {0}; clear_op(&b, &t, 7, 0, 2, 8, 15); end(&b); submit1(&b); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 0 && g_ncr == 0 && nvn_unimpl_kinds() > 0);
    CHECK(nvn_unimpl_count("op", "OP_CLEAR_TEX") == 0);
}
static void test_levels_layers_and_types(void) {
    clear_fresh(); tex_t t = mk_tex(204, 0, NVN_FORMAT_RGBA8, 8, 8, 2, 0);
    t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.depth = 4; nvn_tex_layout(&t.d, &t.L);
    stream s = {0}; p_clear_tex *p = clear_op(&s, &t, 0, 0, 4, 4, 15);
    p->has_view = 1; p->view.set_levels = 1; p->view.base_level = 1; p->view.set_layers = 1; p->view.min_layer = 1;
    p->r.zoffset = 1; p->r.depth = 2; end(&s); submit1(&s); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 2 && g_nseen_view == 2);
    CHECK(g_seen_view[0].base_level == 1 && g_seen_view[0].nlevels == 1 && g_seen_view[0].min_layer == 2 && g_seen_view[0].nlayers == 1);
    CHECK(g_seen_view[1].base_level == 1 && g_seen_view[1].min_layer == 3 && g_seen_view[1].nlayers == 1);
    CHECK(nvn_be_tex_gpu_owned(t.serial));
    clear_fresh(); tex_t u = mk_tex(205, 0, NVN_FORMAT_RGBA32UI, 4, 4, 1, 0);
    stream q = {0}; p = clear_op(&q, &u, 0, 0, 4, 4, 15); p->value_type = CLEAR_TEX_UINT;
    p->c.u[0] = 0xffffffffu; p->c.u[1] = 16777217u; p->c.u[2] = 3; p->c.u[3] = 4;
    end(&q); submit1(&q); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.is_int && g_cv.u[0] == 0xffffffffu && g_cv.u[1] == 16777217u);
    clear_fresh(); tex_t si = mk_tex(207, 0, NVN_FORMAT_RGBA32I, 4, 4, 1, 0);
    stream neg = {0}; p = clear_op(&neg, &si, 0, 0, 4, 4, 15); p->value_type = CLEAR_TEX_INT;
    p->c.i[0] = INT32_MIN; p->c.i[1] = -16777217; p->c.i[2] = INT32_MAX; p->c.i[3] = -1;
    end(&neg); submit1(&neg); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.is_int && g_cv.u[0] == 0x80000000u && g_cv.u[1] == (uint32_t)-16777217 && g_cv.u[2] == 0x7fffffffu && g_cv.u[3] == 0xffffffffu);
    clear_fresh(); tex_t cvt = mk_tex(208, 0, NVN_FORMAT_RGBA32UI, 4, 4, 1, 0);
    stream conv = {0}; p = clear_op(&conv, &cvt, 0, 0, 4, 4, 15); p->c.f[0] = -1; p->c.f[1] = 12.75f; p->c.f[2] = 1.0e30f;
    end(&conv); submit1(&conv); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 1 && g_cv.is_int && g_cv.u[0] == 0 && g_cv.u[1] == 12 && g_cv.u[2] == 0xffffffffu && g_cv.u[3] == 1);
    clear_fresh(); tex_t v = mk_tex(206, 0, NVN_FORMAT_RGBA8, 8, 8, 1, 0);
    stream bad = {0}; p = clear_op(&bad, &v, 0, 0, 8, 8, 15); p->has_view = 1; p->view.set_format = 1; p->view.format = NVN_FORMAT_R8;
    end(&bad); submit1(&bad); fake_complete(NVN_GPU_DONE_OK);
    CHECK(g_cv.n == 0 && g_npd == 0 && nvn_unimpl_count("tex", "clear_view_format") == 1 && !nvn_be_tex_gpu_owned(v.serial));
}
int main(void) {
    test_whole_and_order(); test_partial(); test_empty_and_bounds(); test_levels_layers_and_types();
    pass_fresh();
    printf("%s t_nvn_clear_tex：%d 项断言，%d 项失败\n", g_fail ? "✗" : "✓", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
