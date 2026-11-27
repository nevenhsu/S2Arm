// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// ReadTexels 读 GPU 内容（graphics/nvn/replay/rp_read_texels.inc 的 rt_gpu）接真后端 + lavapipe，始终开 validation 与同步校验。
// 旁路段的开 / 交、取贴图、提交锁、上一个栅栏以受控替身代（核心里的这几件由 t_nvn_replay 的替身后端用例覆盖）；拷贝、提交、等完、格式判断都是真的。
// 期望值：上传进去的字节 / 通道清成的颜色 / 上传的 BC1 原块，不经被测实现算。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_plat.h"
#include "nvn_vk_format.h"
#include "backend_vulkan_d24_readback_spv.h"

static nvn_gpu_tex g_src; static uint64_t g_sig, g_last; static int g_side_begins, g_side_commits, g_locks, g_unlocks, g_in_sub;
static char g_un_last[64]; static int g_un_n;
static nvn_gpu_buf g_t2b_dst;
static unsigned g_t2b_rows;
enum { DS_ROW = 20, DS_SLICE = 48, DS_OFFSET = 8, DS_OUT = 112, DS_ADDR = 0x19000 };
uint64_t nvn_be_last_fence(void) { return g_last; }
int rp_in_submit(void) { return g_in_sub; }
nvn_gpu_cmd rp_side_begin(const char *label) { (void)label; g_side_begins++; return begin_cmd(); }
static void side_done(void *u, const nvn_gpu_done *d) { (void)u; if (d->status != NVN_GPU_DONE_OK) g_fail++; }
uint64_t rp_side_commit(nvn_gpu_cmd c) {
    nvn_gpu_err e = {0}; const uint64_t f = ++g_sig; g_side_commits++;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, f, side_done, NULL, &e); CHECK(OK(rc), "side commit: %d %s", rc.code, e.why);
    g_last = f; return OK(rc) ? f : 0;
}
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) { (void)t; (void)c; return g_src; }
int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off) {
    (void)c;
    if (!g_t2b_dst || addr != DS_ADDR || len != 2 * DS_SLICE) return 0;
    *buf = g_t2b_dst;
    *off = DS_OFFSET;
    return 1;
}
int rp_post_gpu_write_rows(uint64_t addr, uint64_t bytes, uint64_t row, uint64_t image, uint32_t rows, uint32_t slices) {
    if (addr != DS_ADDR || bytes != 12 || row != DS_ROW || image != DS_SLICE || rows != 2 || slices != 2) return 0;
    g_t2b_rows++;
    return 1;
}
void nvn_hle_submit_lock(void) { g_locks++; }
void nvn_hle_submit_unlock(void) { g_unlocks++; }
uint64_t nvn_unimpl_count(const char *cls, const char *name) { (void)cls; (void)name; return 0; }
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) { (void)site; printf("unimplemented %s/%s\n", cls, name); snprintf(g_un_last, sizeof g_un_last, "%s", name); return ++g_un_n; }
#include "rp_read_texels.inc"

static uint64_t commit_wait(nvn_gpu_cmd c, const char *what) {   // 夹具自己准备内容的段（上传 / 通道清色）：与旁路段共用一条递增的提交值
    nvn_gpu_err e = {0}; const uint64_t f = ++g_sig;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, f, side_done, NULL, &e); CHECK(OK(rc), "%s commit: %d %s", what, rc.code, e.why);
    rc = nvn_gpu_fence_wait(f, 5000000000ull, &e); CHECK(OK(rc), "%s wait: %d %s", what, rc.code, e.why);
    g_last = f; return f;
}
static tex_t fake_tex(const nvn_gpu_tex_desc *td) { tex_t t; memset(&t, 0, sizeof t); t.serial = 90; t.d = td->d; nvn_tex_layout(&t.d, &t.L); return t; }
static void run_array_upload(void) {   // 二维数组 4×4×3：上传第 1、2 层 → 读第 1、2 层的 (1,1) 起 2×2，行距 16、片距 48（带 pad）
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 4; td.d.depth = 3; td.d.levels = 1;
    td.usage = NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e); if (!OK(rc)) { CHECK(0, "array image: %d %s", rc.code, e.why); return; }
    uint8_t src[2][64]; for (int z = 0; z < 2; z++) for (int i = 0; i < 64; i++) src[z][i] = (uint8_t)(0x20 + z * 0x40 + i);
    nvn_gpu_cmd c = begin_cmd(); NVNcopyRegion up = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, g_src, 0, 1, &up, src[0], 16, 64); nvn_gpu_tex_upload(c, g_src, 0, 2, &up, src[1], 16, 64); commit_wait(c, "array upload");
    tex_t t = fake_tex(&td); uint8_t dst[160]; memset(dst, 0xEE, sizeof dst);
    NVNcopyRegion r = {1, 1, 0, 2, 2, 2};
    const int b0 = g_side_begins, c0 = g_side_commits;
    rt_gpu(&t, 0, 1, 2, &r, 8, 2, 1, 1, 4, 4, dst, 16, 48);
    CHECK(g_side_begins == b0 + 1 && g_side_commits == c0 + 1 && g_locks == g_unlocks && g_un_n == 0, "array: one side segment, lock balanced, no unimpl (%s)", g_un_last);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++)
        CHECK(dst[z * 48 + y * 16 + x] == src[z][(y + 1) * 16 + 4 + x], "array z=%d y=%d byte=%d: %u != %u", z, y, x, dst[z * 48 + y * 16 + x], src[z][(y + 1) * 16 + 4 + x]);
    CHECK(dst[8] == 0xEE && dst[15] == 0xEE && dst[32] == 0xEE && dst[96] == 0xEE, "array: padding / slice gap untouched");
    nvn_gpu_tex_release(g_src);
}
static void run_clear(void) {   // GPU 写的内容：8×8 RGBA8 当渲染目标、通道清成 (1, 0, 51/255, 1) → 读回每个纹素 ff 00 33 ff
    g_src = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(g_src);
    nvn_gpu_pass_desc p; memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[1] = 0.0f; p.color[0].clear.f[2] = 51.0f / 255.0f; p.color[0].clear.f[3] = 1.0f; p.w = 8; p.h = 8; p.layers = 1;
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_pass_begin(c, &p); nvn_gpu_pass_end(c); commit_wait(c, "clear");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = 8; td.d.depth = td.d.levels = 1;
    tex_t t = fake_tex(&td); uint8_t dst[256]; memset(dst, 0xEE, sizeof dst);
    NVNcopyRegion r = {0, 0, 0, 8, 8, 1};
    rt_gpu(&t, 0, 0, 1, &r, 32, 8, 1, 1, 8, 8, dst, 32, 256);
    int bad = 0; for (int i = 0; i < 64; i++) bad += dst[i * 4] != 0xff || dst[i * 4 + 1] != 0 || dst[i * 4 + 2] != 0x33 || dst[i * 4 + 3] != 0xff;
    CHECK(!bad && g_un_n == 0, "clear: %d texels differ from ff 00 33 ff (first %02x %02x %02x %02x)", bad, dst[0], dst[1], dst[2], dst[3]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(g_src);
}
static void run_3d(void) {   // 三维 4×4×4：上传第 1、2 片 → 读这两片整片（copy_tex_buf 的 layer 在三维上是片号）
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_3D; td.d.format = NVN_FORMAT_RGBA8; td.d.width = td.d.height = td.d.depth = 4; td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e); if (!OK(rc)) { CHECK(0, "3D image: %d %s", rc.code, e.why); return; }
    uint8_t src[2][64]; for (int z = 0; z < 2; z++) for (int i = 0; i < 64; i++) src[z][i] = (uint8_t)(0x30 + z * 0x40 + i);
    nvn_gpu_cmd c = begin_cmd(); NVNcopyRegion up = {0, 0, 0, 4, 4, 1};
    nvn_gpu_tex_upload(c, g_src, 0, 1, &up, src[0], 16, 64); nvn_gpu_tex_upload(c, g_src, 0, 2, &up, src[1], 16, 64); commit_wait(c, "3D upload");
    tex_t t = fake_tex(&td); uint8_t dst[128]; memset(dst, 0xEE, sizeof dst);
    NVNcopyRegion r = {0, 0, 1, 4, 4, 2};
    rt_gpu(&t, 0, 1, 2, &r, 16, 4, 1, 1, 4, 4, dst, 16, 64);
    CHECK(!memcmp(dst, src[0], 64) && !memcmp(dst + 64, src[1], 64) && g_un_n == 0, "3D slices 1..2 read back the uploaded bytes");
    nvn_gpu_tex_release(g_src);
}
static void run_bc1(void) {   // BC1 4×4：上传一个块 → 读回就是这个块的 8 字节（不是解码后的像素）
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_RGBA_DXT1; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1; td.usage = NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e); if (!OK(rc)) { CHECK(0, "BC1 image: %d %s", rc.code, e.why); return; }
    uint8_t block[8] = {0x3f, 0xf8, 0, 0, 0xe4, 0xe4, 0xe4, 0xe4};
    nvn_gpu_cmd c = begin_cmd(); NVNcopyRegion r = {0, 0, 0, 4, 4, 1}; nvn_gpu_tex_upload(c, g_src, 0, 0, &r, block, 8, 8); commit_wait(c, "BC1 upload");
    tex_t t = fake_tex(&td); uint8_t dst[24]; memset(dst, 0xEE, sizeof dst);
    rt_gpu(&t, 0, 0, 1, &r, 8, 1, 4, 4, 4, 4, dst, 8, 8);
    CHECK(!memcmp(dst, block, 8) && dst[8] == 0xEE && g_un_n == 0, "BC1 raw block read back");
    nvn_gpu_tex_release(g_src);
}
static void run_formats(void) {   // 后端的 READBACK 位：拷得出 NVN 字节的有，深度模板合一 / D24 没有；没有的格式一段都不开，时间线照常
    CHECK(nvn_gpu_format_caps(NVN_FORMAT_RGBA8) & NVN_GPU_FMT_READBACK, "RGBA8 has READBACK");
    CHECK(nvn_gpu_format_caps(NVN_FORMAT_RGBA_DXT1) & NVN_GPU_FMT_READBACK, "BC1 has READBACK");
    CHECK(!(nvn_gpu_format_caps(NVN_FORMAT_DEPTH24_STENCIL8) & NVN_GPU_FMT_READBACK), "D24S8 has no READBACK (depth face only)");
    CHECK(!(nvn_gpu_format_caps(NVN_FORMAT_DEPTH24) & NVN_GPU_FMT_READBACK), "D24 has no READBACK (upper 8 bits undefined)");
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D; td.d.format = NVN_FORMAT_DEPTH24_STENCIL8; td.d.width = td.d.height = 4; td.d.depth = td.d.levels = 1;
    tex_t t = fake_tex(&td); uint8_t dst[64]; memset(dst, 0xEE, sizeof dst); NVNcopyRegion r = {0, 0, 0, 4, 4, 1};
    const int b0 = g_side_begins, n0 = g_un_n;
    rt_gpu(&t, 0, 0, 1, &r, 16, 4, 1, 1, 4, 4, dst, 16, 64);
    int untouched = 1; for (size_t i = 0; i < sizeof dst; i++) untouched &= dst[i] == 0xEE;
    CHECK(g_side_begins == b0 && g_un_n == n0 + 1 && !strcmp(g_un_last, "read_texels_gpu_format") && untouched && nvn_gpu_fence_failed() == 0,
          "D24S8 refused before any segment (%s), dst untouched, timeline healthy", g_un_last);
    g_un_n = n0;
}
static void run_substituted_depth(uint32_t fmt) {
    const int stencil = fmt == NVN_FORMAT_DEPTH24_STENCIL8;
    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = fmt,
                                .width = 4, .height = 4, .depth = 3, .levels = 1},
                           .usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED};
    nvn_gpu_err e = {0};
    nvn_gpu_rc rc = nvn_gpu_tex_create(&td, &g_src, &e);
    CHECK(OK(rc) && g_src, "D24 fmt=0x%x create: %d %s", fmt, rc.code, e.why);
    if (!OK(rc)) return;
    const uint32_t caps = nvn_gpu_format_caps(fmt);
    CHECK((caps & (NVN_GPU_FMT_SUBST | NVN_GPU_FMT_READBACK)) == (NVN_GPU_FMT_SUBST | NVN_GPU_FMT_READBACK),
          "D24 fmt=0x%x substituted storage advertises exact readback (caps=0x%x)", fmt, caps);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_view views[2] = {0};
    for (unsigned z = 0; z < 2; z++) {
        const view_t v = {.set_levels = 1, .nlevels = 1, .set_layers = 1, .min_layer = z + 1, .nlayers = 1};
        rc = nvn_gpu_view_create(g_src, &v, &views[z], &e);
        CHECK(OK(rc) && views[z], "D24 view z=%u: %d %s", z + 1, rc.code, e.why);
        if (!views[z]) continue;
        nvn_gpu_pass_desc pass = {.depth = {.view = views[z], .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE},
                                  .clear_depth = z ? 1.0f : 0.0f, .clear_stencil = z ? 0x5a : 0xa5,
                                  .w = 4, .h = 4, .layers = 1};
        if (stencil) pass.stencil = pass.depth;
        nvn_gpu_pass_begin(c, &pass);
        const nvn_gpu_clear cl = {.color = -1, .depth = 1, .stencil = stencil, .stencil_mask = 0xff,
                                  .depth_value = z ? 0.25f : 0.5f, .stencil_value = z ? 0xc3 : 0x3c,
                                  .x = 2, .y = 1, .w = 1, .h = 2};
        nvn_gpu_clear_rect(c, &cl);
        nvn_gpu_pass_end(c);
    }
    // 图像传输只送 [0,1] 内的值；NaN 在独立的计算缓冲用例中覆盖。
    nvn_gpu_tex float_src = NULL;
    if (!stencil) {
        float_src = make_tex(4, 1, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_SAMPLED);
        if (float_src) {
            const uint32_t depths[] = {0x3f7fffffu, 0x33000000u, 0, 0x33000001u};
            const NVNcopyRegion src = {0, 0, 0, 3, 1, 1}, dst = {1, 2, 0, 3, 1, 1};
            const NVNcopyRegion src2 = {3, 0, 0, 1, 1, 1}, dst2 = {3, 2, 0, 1, 1, 1};
            nvn_gpu_tex_upload(c, float_src, 0, 0, NULL, depths, sizeof depths, sizeof depths);
            nvn_gpu_copy_tex(c, float_src, 0, 0, &src, g_src, 0, 2, &dst);
            nvn_gpu_copy_tex(c, float_src, 0, 0, &src2, g_src, 0, 1, &dst2);
        }
    }
    commit_wait(c, "D24 render");

    uint8_t seed[DS_OUT], expected[DS_OUT], read[DS_OUT];
    memset(seed, 0xee, sizeof seed); memcpy(expected, seed, sizeof seed); memcpy(read, seed, sizeof seed);
    for (unsigned z = 0; z < 2; z++) for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 3; x++) {
        uint32_t depth = x == 1 ? (z ? 0x400000u : 0x800000u) : (z ? 0xffffffu : 0);
        if (!stencil && y && z) depth = x == 0 ? 0xfffffeu : 0;
        if (!stencil && y && !z && x == 2) depth = 1;
        const uint8_t s = stencil ? (x == 1 ? (z ? 0xc3 : 0x3c) : (z ? 0x5a : 0xa5)) : 0;
        uint8_t *p = expected + DS_OFFSET + z * DS_SLICE + y * DS_ROW + x * 4;
        p[0] = (uint8_t)depth; p[1] = (uint8_t)(depth >> 8); p[2] = (uint8_t)(depth >> 16); p[3] = s;
    }
    tex_t t = fake_tex(&td);
    const NVNcopyRegion r = {1, 1, 1, 3, 2, 2};
    const uint32_t real_storage_limit = g_dev.properties.limits.maxStorageBufferRange;
    if (stencil) g_dev.properties.limits.maxStorageBufferRange = 19;  // 模拟低上限，强制每行一次并重用两块暂存
    const int before = g_un_n;
    g_t2b_dst = make_buf(DS_OUT, NVN_GPU_MEM_SHARED, seed);
    nvn_gpu_buf observed = make_buf(DS_OUT, NVN_GPU_MEM_SHARED, NULL);
    if (g_t2b_dst && observed) {
        c = begin_cmd();
        const p_copy_b2t op = {.tex = &t, .r = r, .src = DS_ADDR, .row_stride = DS_ROW, .image_stride = DS_SLICE};
        g_t2b_rows = 0;
        rp_op_copy_t2b(c, &op, &op);
        nvn_gpu_copy_buf(c, g_t2b_dst, 0, observed, 0, DS_OUT);   // 同段的 GPU 消费者必须看到已转换的字节
        commit_wait(c, "D24 CopyT2B");
        const uint8_t *copy = buf_cpu(observed);
        unsigned matched = 0;
        for (unsigned i = 0; i < DS_OUT; i++) matched += copy[i] == expected[i];
        CHECK(matched == DS_OUT && g_un_n == before && g_t2b_rows == 1,
              "D24 fmt=0x%x CopyT2B bytes=%u/%u unimpl=%d (%s)", fmt, matched, DS_OUT, g_un_n - before, g_un_last);
        printf("D24 fmt=0x%x CopyT2B bytes=%u/%u\n", fmt, matched, DS_OUT);
    }
    const int before_read = g_un_n;
    rt_gpu(&t, 0, 1, 2, &r, 12, 2, 1, 1, 4, 4, read + DS_OFFSET, DS_ROW, DS_SLICE);
    unsigned matched = 0;
    for (unsigned i = 0; i < DS_OUT; i++) matched += read[i] == expected[i];
    CHECK(matched == DS_OUT && g_un_n == before_read, "D24 fmt=0x%x ReadTexels bytes=%u/%u unimpl=%d (%s)",
          fmt, matched, DS_OUT, g_un_n - before_read, g_un_last);
    printf("D24 fmt=0x%x ReadTexels bytes=%u/%u\n", fmt, matched, DS_OUT);

    g_dev.properties.limits.maxStorageBufferRange = real_storage_limit;
    nvn_gpu_buf_release(observed, NULL, NULL);
    nvn_gpu_buf_release(g_t2b_dst, NULL, NULL);
    g_t2b_dst = NULL;
    for (unsigned i = 0; i < 2; i++) nvn_gpu_view_release(views[i]);
    nvn_gpu_tex_release(float_src);
    nvn_gpu_tex_release(g_src);
}

static void run_d24_pack_shader(void) {
    // 写 SSBO 不受深度图 [0,1] 限制，可直接验证 NaN、Inf 和越界浮点的转换。
    enum { N = 251 };
    uint32_t input[N + (N + 3) / 4] = {0}, expected[N], actual[N];
    const uint32_t edges[] = {0, 0x80000000u, 0x33000000u, 0x33000001u, 0x3e800000u, 0x3f000000u,
                             0x3f000001u, 0x3f7fffffu, 0x3f800000u, 0x3fa00000u, 0xbf800000u,
                             0x7fc00000u, 0x7f800000u, 0xff800000u};
    memcpy(input, edges, sizeof edges);
    uint32_t rng = 0x489c7731u;
    for (unsigned i = sizeof edges / sizeof edges[0]; i < N; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        input[i] = (rng & 0x7fffffu) | ((100 + i % 27) << 23);   // 2^-27 到 1 之间的确定性样本
    }
    uint8_t *stencil = (uint8_t *)(input + N);
    for (unsigned i = 0; i < N; i++) stencil[i] = (uint8_t)(i + 1);
    memcpy(expected, input, sizeof expected);
    nvn_vk_depth_f32_to_24(expected, N);
    for (unsigned i = 0; i < N; i++) expected[i] |= (uint32_t)stencil[i] << 24;

    const nvn_gpu_layout_ent ents[] = {
        {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_R, .binding = 0, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .binding = 1, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
    };
    const nvn_gpu_layout layout = {.ent = ents, .nent = 2, .push_bytes = 3 * sizeof(uint32_t)};
    const nvn_gpu_shader_src source = {.kind = NVN_GPU_BLOB_SPIRV, .stage = NVN_GPU_SH_CS, .entry = "main",
                                      .blob = nvk_d24_readback_spv, .bytes = sizeof nvk_d24_readback_spv, .layout = &layout};
    nvn_gpu_shader shader = NULL;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_shader_load(&source, &shader, &err)) && shader, "D24 pack shader load: %s", err.why);
    if (!shader) return;
    const nvn_gpu_stage_desc stage = {.sh = shader};
    CHECK(OK(nvn_gpu_cpipe_get(&stage, NVN_GPU_PIPE_SYNC, &pipe, &err)) && pipe, "D24 pack pipeline: %s", err.why);
    nvn_gpu_buf src = make_buf(sizeof input, NVN_GPU_MEM_SHARED, input);
    nvn_gpu_buf dst = make_buf(sizeof actual, NVN_GPU_MEM_SHARED, NULL);
    if (pipe && src && dst) for (int with_stencil = 1; with_stencil >= 0; with_stencil--) {
        nvn_gpu_cmd c = begin_cmd();
        const nvn_gpu_binding b[] = {
            {.ent = 0, .cls = NVN_GPU_RES_SSBO, .u.b = {.buf = src, .len = sizeof input}},
            {.ent = 1, .cls = NVN_GPU_RES_SSBO, .u.b = {.buf = dst, .len = sizeof actual}},
        };
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
        const uint32_t params[] = {N, 1, (uint32_t)with_stencil};
        nvn_gpu_push(c, NVN_GPU_SH_CS, 0, params, sizeof params);
        nvn_gpu_dispatch(c, pipe, (N + 63) / 64, 1, 1, NULL, 0);
        commit_wait(c, "D24 pack shader");
        memcpy(actual, buf_cpu(dst), sizeof actual);
        unsigned match_full = 0, match_depth = 0;
        for (unsigned i = 0; i < N; i++) {
            match_full += actual[i] == expected[i];
            match_depth += actual[i] == (expected[i] & 0xffffffu);
        }
        CHECK(with_stencil ? match_full == N : match_full == 0 && match_depth == N,
              "D24 pack/stencil=%d: packed %u depth-only %u of %d", with_stencil, match_full, match_depth, N);
        printf("D24 pack/stencil=%d packed=%u/%d depth-only=%u/%d\n", with_stencil, match_full, N, match_depth, N);
    }
    nvn_gpu_buf_release(src, NULL, NULL);
    nvn_gpu_buf_release(dst, NULL, NULL);
    nvn_gpu_shader_release(shader);
}

int main(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { printf("device failed: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-replay-read-texels-test";
    const uint32_t saved_mask = nvn_vk_format_depth_subst();
    nvn_vk_format_set_depth_subst(0);   // lavapipe 的原生 D24 作为既有拒绝对照
    if (!open_backend()) return 1;
    run_array_upload(); run_clear(); run_3d(); run_bc1(); run_formats();
    uint64_t st[4]; nvn_be_read_texels_gpu_stats(st);
    CHECK(st[0] == 4 && g_locks == g_unlocks && g_locks == 4, "4 reads counted (%llu), locks %d/%d (the D24S8 refusal happens before the lock)", (unsigned long long)st[0], g_locks, g_unlocks);
    nvn_gpu_err e = {0}; CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close: %d %s", e.rc.code, e.why);
    nvn_vk_format_set_depth_subst(NVN_VK_DEPTH_SUBST_X8D24 | NVN_VK_DEPTH_SUBST_D24S8);
    if (!open_backend()) return 1;
    g_sig = g_last = 0;
    run_substituted_depth(NVN_FORMAT_DEPTH24);
    run_substituted_depth(NVN_FORMAT_DEPTH24_STENCIL8);
    run_d24_pack_shader();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close substituted: %d %s", e.rc.code, e.why);
    nvn_vk_format_set_depth_subst(saved_mask);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("%s replay ReadTexels GPU: array / clear / 3D / BC1 / formats, failures=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
