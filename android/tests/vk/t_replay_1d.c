// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实回放上传、清除、拷贝、附件、描述符与 Vulkan 采样。GLSL 匹配翻译器的 Dim2D 和独立数组位。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "nvn_deswizzle.h"
#include "nvn_mtl.h"
#include "nvn_tex.h"
#include "rp_exec.h"
#include "rp_res.h"

int rp_tm_on;
static nvn_gpu_cmd active;
static uint64_t fence_signal;
static uint32_t observed_layers;
extern __typeof__(nvn_gpu_pass_begin) __real_nvn_gpu_pass_begin;
void __wrap_nvn_gpu_pass_begin(nvn_gpu_cmd cmd, const nvn_gpu_pass_desc *pass) {
    observed_layers = pass->layers;
    __real_nvn_gpu_pass_begin(cmd, pass);
}
void rp_perf_add(int index, uint64_t value) { (void)index; (void)value; }
uint64_t rp_seg_serial(void) { return 1; }
uint64_t rp_sub_serial(void) { return 1; }
// Every recorded command is submitted once with the next value in rp_side_commit.
uint64_t rp_sub_fence(void) { return fence_signal + 1; }
int rp_fence_done(uint64_t value) { return nvn_gpu_fence_done() >= value; }
int rp_occl_want(void) { return 0; }
void rp_occl_pass_opened(int declared) { CHECK(!declared, "unexpected occlusion"); }
void rp_occl_pass_closed(void) {}
void rp_occl_pause(nvn_gpu_cmd c) { (void)c; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last_sub) {
    *last_sub = rp_sub_serial();
    return active;
}
static pthread_mutex_t submit_mu = PTHREAD_MUTEX_INITIALIZER;
void nvn_hle_submit_lock(void) { pthread_mutex_lock(&submit_mu); }
void nvn_hle_submit_unlock(void) { pthread_mutex_unlock(&submit_mu); }
uint64_t nvn_be_last_fence(void) { return fence_signal; }
int rp_in_submit(void) { return active != NULL; }
nvn_gpu_cmd rp_side_begin(const char *label) { (void)label; return begin_cmd(); }
uint64_t rp_side_commit(nvn_gpu_cmd cmd) {
    nvn_gpu_err err = {0};
    const uint64_t fence = ++fence_signal;
    const nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, fence, on_done, NULL, &err);
    CHECK(OK(rc), "1D submit: %s", err.why);
    return OK(rc) ? fence : 0;
}

enum { WIDTH = 8, LEVELS = 2, LAYERS = 4, PIXELS = 60, ZERO_PIXELS = 2, POOL_STRIDE = 32768 };
static uint8_t storage[2 * POOL_STRIDE];
static pool_t pool = {.mem = storage, .size = sizeof storage, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};

static float uploaded(unsigned id, unsigned mip, unsigned layer, unsigned x) {
    return 1.0f + 200 * id + 50 * mip + 10 * layer + x;
}

static tex_t guest_texture(unsigned id, int target, unsigned layers) {
    tex_t t = {.d = {.target = target, .format = NVN_FORMAT_R32F,
                    .width = WIDTH, .height = layers, .depth = 1, .levels = LEVELS,
                    .flags = NVN_TEXTURE_FLAGS_IMAGE},
               .serial = id, .pool = &pool, .off = (id - 1) * POOL_STRIDE,
               .mem = storage + (id - 1) * POOL_STRIDE, .has_layout = 1};
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size && t.L.size <= POOL_STRIDE && t.L.layers == layers, "guest layout id=%u", id);
    for (unsigned mip = 0; mip < LEVELS; mip++) {
        const unsigned w = WIDTH >> mip;
        for (unsigned layer = 0; layer < layers; layer++) {
            float row[WIDTH];
            for (unsigned x = 0; x < w; x++) row[x] = uploaded(id, mip, layer, x);
            CHECK(!nvn_swizzle(&t.d, mip, layer, row, w * sizeof *row, t.mem),
                  "seed guest texture id=%u mip=%u layer=%u", id, mip, layer);
        }
    }
    return t;
}

static void clear_texture(const tex_t *t, unsigned layer, unsigned first, unsigned width, float value) {
    p_clear_tex op = {.tex = t, .has_view = 1, .view = {
        .set_levels = 1, .base_level = 1, .nlevels = 1,
        .set_layers = 1, .min_layer = layer, .nlayers = 1,
        .set_target = 1, .target = t->d.target,
    }, .r = {first, 0, 0, width, 1, 1}, .mask = 1, .value_type = CLEAR_TEX_FLOAT};
    op.c.f[0] = value;
    rp_op_clear_tex(active, &op, &op);
}

static void clear_attachment(const tex_t *t) {
    p_set_rt rt = {.n = 1, .color = {t}, .has_cview = {1}, .cview = {{
        .set_levels = 1, .base_level = 1, .nlevels = 1,
        .set_layers = 1, .min_layer = 3, .nlayers = 1,
        .set_target = 1, .target = NVN_TEXTURE_TARGET_1D,
    }}};
    rp_pass_set_rt(active, &rt);
    uint32_t w = 0, h = 0;
    rp_pass_slot_size(0, &w, &h);
    CHECK(w == 4 && h == 1, "1D_ARRAY attachment mip 1 expected 4x1, got %ux%u", w, h);
    if (w != 4 || h != 1) return;
    const p_clear_color op = {.index = 0, .mask = 15, .rgba = {103, 0, 0, 1}};
    rp_pass_clear_color(active, &op, NULL);
    rp_pass_flush(active);
}

static void check_layered_attachment(const tex_t *t) {
    const p_set_rt rt = {.n = 1, .color = {t}, .has_cview = {1}, .cview = {{
        .set_levels = 1, .base_level = 1, .nlevels = 1,
        .set_layers = 1, .min_layer = 1, .nlayers = 3,
    }}};
    rp_pass_set_rt(active, &rt);
    uint32_t w = 0, h = 0;
    rp_pass_slot_size(0, &w, &h);
    if (w != 4 || h != 1) return;  // 无效尺寸由 clear_attachment 的断言报告。
    observed_layers = 0;
    rp_pass_want_layered(active, 1);
    rp_pass_prepare_draw(active, 15);
    CHECK(rp_pass_begin(active) && observed_layers == 3, "1D_ARRAY layered framebuffer expected 3 layers, got %u", observed_layers);
    rp_pass_flush(active);
    rp_pass_want_layered(active, 0);
}

static void copy(const tex_t *one, const tex_t *array) {
    static const float source[2][8] = {{104}, {105}};
    nvn_be_pool_add(source, sizeof source);
    const view_t array_view = {.set_levels = 1, .base_level = 1, .nlevels = 1,
                               .set_layers = 1, .min_layer = 1, .nlayers = 2};
    const p_copy_b2t upload = {.tex = array, .src = (uint64_t)(uintptr_t)source, .has_view = 1, .view = array_view,
                              .r = {0, 0, 0, 1, 1, 2}, .row_stride = 16, .image_stride = 32};
    rp_op_copy_b2t(active, &upload, &upload);
    nvn_be_pool_del(source);
    const p_copy_t2t copy = {.src = array, .dst = one, .has_sv = 1, .sv = array_view,
                            .sr = {0, 0, 1, 1, 1, 1}, .dr = {3, 0, 0, 1, 1, 1}};
    rp_op_copy_t2t(active, &copy, &copy);
}

static void check_read_texels(const tex_t *one, const tex_t *array) {
    if (!nvn_be_tex_gpu_owned(one->serial) || !nvn_be_tex_gpu_owned(array->serial)) {
        CHECK(0, "1D ReadTexels fixture needs GPU-authored contents");
        return;
    }
    float row[2][8];
    for (unsigned layer = 0; layer < 2; layer++)
        for (unsigned x = 0; x < 8; x++) row[layer][x] = -1;
    const NVNcopyRegion region = {0, 0, 0, 4, 1, 2};
    nvn_be_read_texels(array, 1, 1, &region, (uint8_t *)row, 16, sizeof row[0]);
    const float want[2][8] = {{104, 102, 102, 464, -1, -1, -1, -1},
                             {105, 472, 473, 474, -1, -1, -1, -1}};
    CHECK(!memcmp(row, want, sizeof row), "1D_ARRAY ReadTexels bytes or layer padding differ");
    const NVNcopyRegion one_region = {3, 0, 0, 1, 1, 1};
    float pixel = -1;
    nvn_be_read_texels(one, 0, 0, &one_region, (uint8_t *)&pixel, sizeof pixel, sizeof pixel);
    CHECK(pixel == 105, "1D ReadTexels after T2T expected 105 got %.0f", pixel);
}

static void store(const char *dir, const tex_t *one, const tex_t *array) {
    const view_t one_view = {.set_target = 1, .target = NVN_TEXTURE_TARGET_1D,
                             .set_levels = 1, .nlevels = 1};
    const view_t array_view = {.set_target = 1, .target = NVN_TEXTURE_TARGET_1D_ARRAY,
                               .set_levels = 1, .nlevels = 1,
                               .set_layers = 1, .min_layer = 1, .nlayers = 2};
    const nvn_gpu_layout_ent entries[] = {
        {.binding = 0, .cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.binding = 1, .cls = NVN_GPU_RES_IMAGE, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
    };
    const nvn_gpu_layout layout = {.ent = entries, .nent = 2};
    nvn_gpu_shader shader = NULL;
    nvn_gpu_pipe pipeline = mk_cpipe(dir, "replay_1d_store.comp", &layout, &shader);
    nvn_gpu_view a = rp_view_for(one, &one_view, active), b = rp_view_for(array, &array_view, active);
    CHECK(pipeline && a && b, "1D storage pipeline/views");
    if (pipeline && a && b) {
        const nvn_gpu_binding bind[] = {
            {.ent = 0, .cls = NVN_GPU_RES_IMAGE, .u.view = a},
            {.ent = 1, .cls = NVN_GPU_RES_IMAGE, .u.view = b},
        };
        nvn_gpu_set_pipe(active, pipeline);
        nvn_gpu_bind(active, NVN_GPU_SH_CS, bind, 2);
        nvn_gpu_dispatch(active, pipeline, 1, 1, 1, NULL, 0);
    }
    if (shader) nvn_gpu_shader_release(shader);
}

static void sample(const char *dir, nvn_gpu_view one, nvn_gpu_view array) {
    const nvn_gpu_layout_ent entries[] = {
        {.binding = 0, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.binding = 1, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.binding = 2, .cls = NVN_GPU_RES_SSBO, .access = NVN_GPU_ACC_W, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.binding = 3, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
        {.binding = 4, .cls = NVN_GPU_RES_TEX_SAMP, .access = NVN_GPU_ACC_R, .count = 1, .stages = 1u << NVN_GPU_SH_CS},
    };
    const nvn_gpu_layout layout = {.ent = entries, .nent = sizeof entries / sizeof entries[0]};
    nvn_gpu_shader shader = NULL;
    nvn_gpu_pipe pipeline = mk_cpipe(dir, "replay_1d.comp", &layout, &shader);
    const samp_par sp = {0};
    nvn_gpu_err err = {0};
    nvn_gpu_samp sampler = NULL;
    CHECK(pipeline && OK(nvn_gpu_samp_create(&sp, &sampler, &err)), "1D sampler/pipeline: %s", err.why);
    nvn_gpu_view zero_one = rp_view_by_id_decl(3, NVN_TEXTURE_TARGET_1D + 1, 0, 0, active, NULL);
    nvn_gpu_view zero_array = rp_view_by_id_decl(4, NVN_TEXTURE_TARGET_1D_ARRAY + 1, 0, 0, active, NULL);
    CHECK(zero_one && zero_array, "1D typed zero fallback views");
    const unsigned bytes = (PIXELS + ZERO_PIXELS) * sizeof(float);
    nvn_gpu_buf output = make_buf(bytes, NVN_GPU_MEM_SHARED, NULL);
    if (!pipeline || !sampler || !one || !array || !zero_one || !zero_array || !output) {
        nvn_gpu_cmd_abort(active);
    } else {
        const nvn_gpu_binding bind[] = {
            {.ent = 0, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {.view = one, .samp = sampler}},
            {.ent = 1, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {.view = array, .samp = sampler}},
            {.ent = 2, .cls = NVN_GPU_RES_SSBO, .u.b = {.buf = output, .len = bytes}},
            {.ent = 3, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {.view = zero_one, .samp = sampler}},
            {.ent = 4, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {.view = zero_array, .samp = sampler}},
        };
        nvn_gpu_set_pipe(active, pipeline);
        nvn_gpu_bind(active, NVN_GPU_SH_CS, bind, sizeof bind / sizeof bind[0]);
        nvn_gpu_dispatch(active, pipeline, 1, 1, 1, NULL, 0);
        const uint64_t pending = rp_sub_fence();
        CHECK(!rp_fence_done(pending), "1D pending fence completed before submit");
        const uint64_t fence = rp_side_commit(active);
        const int retired = fence && OK(nvn_gpu_fence_wait(fence, 5000000000ull, &err));
        CHECK(fence == pending && retired && rp_fence_done(fence), "1D sample completion: %s", err.why);
        const float *pixels = retired ? (const float *)buf_cpu(output) : NULL;
        unsigned next = 0, matches = 0;
        if (pixels) for (unsigned mip = 0; mip < LEVELS; mip++) {
            const unsigned width = WIDTH >> mip;
            for (unsigned id = 1; id <= 2; id++) {
                for (unsigned layer = 0; layer < (id == 1 ? 1 : LAYERS); layer++) {
                    for (unsigned x = 0; x < width; x++) {
                        float want = uploaded(id, mip, layer, x);
                        if (mip == 0 && id == 1 && x == 2) want = 106;
                        if (mip == 0 && id == 1 && x == 3) want = 105;
                        if (mip == 0 && id == 2 && layer == 2 && x == 2) want = 107;
                        if (mip == 1 && id == 1) want = 101;
                        if (mip == 1 && id == 2 && x == 0 && (layer == 1 || layer == 2)) want = 103 + layer;
                        if (mip == 1 && id == 2 && layer == 1 && x >= 1 && x < 3) want = 102;
                        if (mip == 1 && id == 2 && layer == 3) want = 103;
                        matches += pixels[next] == want;
                        CHECK(pixels[next] == want, "1D id=%u mip=%u layer=%u x=%u expected %.0f got %.0f",
                              id, mip, layer, x, want, pixels[next]);
                        next++;
                    }
                }
            }
        }
        CHECK(matches == PIXELS, "1D sampled %u/%d texels", matches, PIXELS);
        const int zero = pixels && pixels[PIXELS] == 0 && pixels[PIXELS + 1] == 0;
        CHECK(zero, "1D and 1D_ARRAY fallback texels must both be zero");
        printf("replay 1D sampled=%u/%d zero=%d/%d layered=%u\n", matches, PIXELS, zero ? ZERO_PIXELS : 0, ZERO_PIXELS, observed_layers);
    }
    active = NULL;
    if (sampler) nvn_gpu_samp_release(sampler);
    if (shader) nvn_gpu_shader_release(shader);
    if (output) nvn_gpu_buf_release(output, NULL, NULL);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc = (nvn_gpu_open_desc){.host_device = &g_dev, .app_label = "replay-1d"};
    if (!open_backend()) return 2;
    tex_t t = guest_texture(1, NVN_TEXTURE_TARGET_1D, 1);
    tex_t a = guest_texture(2, NVN_TEXTURE_TARGET_1D_ARRAY, LAYERS);
    nvn_be_reg_tex(1, &t, NULL);
    nvn_be_reg_tex(2, &a, NULL);
    nvn_be_set_tex_pool_n(8);
    active = begin_cmd();
    copy(&t, &a);
    clear_texture(&t, 0, 0, 4, 101);
    clear_texture(&a, 1, 1, 2, 102);
    clear_attachment(&a);
    check_layered_attachment(&a);
    store(argv[1], &t, &a);
    const uint64_t rejected = nvn_unimpl_count("tex", "clear_target");
    CHECK(rejected == 0, "1D clear_target rejected=%llu", (unsigned long long)rejected);
    nvn_gpu_view one = rp_view_by_id_decl(1, NVN_TEXTURE_TARGET_2D + 1, 0, 0, active, NULL);
    nvn_gpu_view array = rp_view_by_id_decl(2, NVN_TEXTURE_TARGET_2D_ARRAY + 1, 0, 0, active, NULL);
    CHECK(one && array, "1D descriptor views");
    sample(argv[1], one, array);
    check_read_texels(&t, &a);
    rp_pass_reset();
    rp_res_reset();
    nvn_gpu_err err = {0};
    CHECK(OK(nvn_gpu_close(5000000000ull, &err)), "close: %s", err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "destroy device");
    printf("replay 1D failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
