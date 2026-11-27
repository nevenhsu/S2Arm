// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实回放清除实现 + Vulkan 后端；仅资源查找换成受控纹理，读回全部像素。
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
#include "rp_exec.h"
#include "rp_res.h"

static nvn_gpu_tex g_image;
static nvn_gpu_view g_views[8]; static int g_nviews, g_writes, g_unimpl;
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) { (void)t; (void)c; return g_image; }
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) {
    (void)t; (void)c; nvn_gpu_view out = NULL; nvn_gpu_err e = {0};
    nvn_gpu_rc r = nvn_gpu_view_create(g_image, v, &out, &e);
    CHECK(OK(r), "replay view：%d %s", r.code, e.why);
    if (out && g_nviews < 8) g_views[g_nviews++] = out;
    return out;
}
void rp_view_pin_begin(void) {}
void rp_view_pin_end(void) {}
void rp_tex_mark_gpu_written(const tex_t *t) { (void)t; g_writes++; }
void rp_dirty_all(void) {}
uint64_t nvn_unimpl_count(const char *cls, const char *name) { (void)cls; (void)name; return 0; }
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site) {
    (void)site; printf("unimplemented %s/%s\n", cls, name); return ++g_unimpl;
}

static void release_clear_case(nvn_gpu_view all, nvn_gpu_buf rb) {
    for (int i = 0; i < g_nviews; i++) nvn_gpu_view_release(g_views[i]);
    g_nviews = g_writes = g_unimpl = 0;
    nvn_gpu_view_release(all);
    nvn_gpu_tex_release(g_image);
    nvn_gpu_buf_release(rb, NULL, NULL);
}

static int clear_case_wait(nvn_gpu_cmd cmd, uint64_t signal) {
    nvn_gpu_err err = {0};
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(cmd, signal, on_done, NULL, &err);
    if (OK(rc)) rc = nvn_gpu_fence_wait(signal, 5000000000ull, &err);
    CHECK(OK(rc), "CLEAR_TEX case %llu completion: %s", (unsigned long long)signal, err.why);
    return OK(rc);
}

static void clear_layers(uint32_t target, uint32_t depth, const char *label, uint64_t signal) {
    const int volume = target == NVN_TEXTURE_TARGET_3D;
    const uint32_t levels = volume ? 3 : 2;
    nvn_gpu_tex_desc td = {.d = {.target = target, .format = NVN_FORMAT_RGBA8,
                                .width = 8, .height = 8, .depth = depth, .levels = levels},
                           .usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED};
    nvn_gpu_err err = {0};
    const nvn_gpu_rc made = nvn_gpu_tex_create(&td, &g_image, &err);
    CHECK(OK(made), "%s texture: %s", label, err.why);
    if (!OK(made)) return;
    nvn_gpu_view all = make_view(g_image);
    nvn_gpu_buf rb = make_buf((uint64_t)levels * depth * 8 * 8 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    const float bg[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    nvn_gpu_clear_view(cmd, all, bg, NULL, 0);
    tex_t t = {.d = td.d, .serial = signal};
    const unsigned first = depth == 12 ? 6 : 2;
    const unsigned partial = depth == 12 ? 8 : 4;
    p_clear_tex op = {.tex = &t, .has_view = 1, .view = {
        .set_levels = 1, .base_level = volume ? 2 : 1, .nlevels = 1,
        .set_layers = 1, .min_layer = first - 1, .nlayers = 3,
    }, .r = {0, 0, volume ? 0 : 1, volume ? 2 : 4, volume ? 2 : 4, 2},
        .mask = 15, .value_type = CLEAR_TEX_FLOAT, .c.f = {1, 0, 0, 1}};
    rp_op_clear_tex(cmd, &op, &op);
    op.view.base_level = 1;
    op.view.min_layer = volume ? 2 : partial - 1;
    op.r = (NVNcopyRegion){1, 1, 1, 2, 2, volume ? 2 : 1};
    op.mask = 2;
    op.c.f[1] = 1;
    rp_op_clear_tex(cmd, &op, &op);
    if (volume) {
        // XY 全覆盖但只清一片：若误用传输清除，会覆盖整个 mip。
        op.r = (NVNcopyRegion){0, 0, 3, 4, 4, 1};
        op.mask = 15;
        op.c.f[1] = 0;
        rp_op_clear_tex(cmd, &op, &op);
    }
    const int rejected = g_unimpl;
    op.r = (NVNcopyRegion){0, 0, volume ? 4 : (int)depth, 4, 4, 1};
    rp_op_clear_tex(cmd, &op, &op);
    CHECK(rejected == 0 && g_unimpl == rejected + 1, "%s valid clears rejected=%d, bounds rejected=%d",
          label, rejected, g_unimpl - rejected);

    uint64_t offset = 0;
    for (uint32_t mip = 0; mip < levels; mip++) {
        const uint32_t w = 8u >> mip, slices = volume ? depth >> mip : depth;
        const NVNcopyRegion plane = {0, 0, 0, w, w, 1};
        for (uint32_t z = 0; z < slices; z++, offset += w * w * 4)
            nvn_gpu_copy_tex_buf(cmd, g_image, mip, z, &plane, rb, offset, w * 4, w * w * 4);
    }
    if (clear_case_wait(cmd, signal)) {
        const uint8_t *data = buf_cpu(rb);
        unsigned index = 0, bad = 0;
        for (uint32_t mip = 0; mip < levels; mip++) {
            const uint32_t w = 8u >> mip, slices = volume ? depth >> mip : depth;
            for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < w; y++) for (uint32_t x = 0; x < w; x++, index++) {
                const int red = volume ? mip == 2 || (mip == 1 && z == 3) : mip == 1 && z >= first && z < first + 2;
                const int green = mip == 1 && (volume ? z == 1 || z == 2 : z == partial) &&
                                  x >= 1 && x < 3 && y >= 1 && y < 3;
                const uint8_t expected[] = {red ? 255 : 64, red ? 0 : green ? 255 : 128, red ? 0 : 191, 255};
                if (memcmp(data + index * 4, expected, sizeof expected)) {
                    if (!bad) printf("%s first mismatch mip=%u slice=%u xy=%u,%u\n", label, mip, z, x, y);
                    bad++;
                }
            }
        }
        CHECK(bad == 0, "%s texture clear pixels=%u/%u", label, index - bad, index);
        printf("CLEAR_TEX %s pixels=%u/%u rejected=%d\n", label, index - bad, index, rejected);
    }
    release_clear_case(all, rb);
}

static void clear_reinterpreted(uint64_t signal) {
    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                .width = 4, .height = 4, .depth = 1, .levels = 1},
                           .usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_VIEW_FMT | NVN_GPU_TEX_STORAGE};
    nvn_gpu_err err = {0};
    const nvn_gpu_rc made = nvn_gpu_tex_create(&td, &g_image, &err);
    CHECK(OK(made), "mutable clear texture: %s", err.why);
    if (!OK(made)) return;
    nvn_gpu_view all = make_view(g_image);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    const float bg[4] = {0, 0, 0, 1};
    nvn_gpu_clear_view(cmd, all, bg, NULL, 0);
    tex_t t = {.d = td.d, .serial = signal};
    p_clear_tex op = {.tex = &t, .has_view = 1, .view = {.set_format = 1, .format = NVN_FORMAT_RGBA8_SRGB},
                      .r = {0, 0, 0, 4, 4, 1}, .mask = 15, .value_type = CLEAR_TEX_FLOAT,
                      .c.f = {0.5f, 0, 0, 1}};
    rp_op_clear_tex(cmd, &op, &op);
    op.view.format = NVN_FORMAT_RGBA8UI;
    op.r = (NVNcopyRegion){1, 0, 0, 2, 3, 1};
    op.mask = 2;
    op.value_type = CLEAR_TEX_UINT;
    op.c.u[1] = 37;
    rp_op_clear_tex(cmd, &op, &op);
    const int rejected = g_unimpl;
    op.view.format = NVN_FORMAT_R8;
    op.r = (NVNcopyRegion){0, 0, 0, 4, 4, 1};
    op.mask = 1;
    op.c.u[0] = 255;
    rp_op_clear_tex(cmd, &op, &op);
    CHECK(rejected == 0 && g_unimpl == rejected + 1, "compatible format rejected=%d, cross-class rejected=%d",
          rejected, g_unimpl - rejected);
    nvn_gpu_copy_tex_buf(cmd, g_image, 0, 0, NULL, rb, 0, 16, 64);
    if (clear_case_wait(cmd, signal)) {
        const uint8_t *data = buf_cpu(rb);
        unsigned good = 0;
        for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) {
            const uint8_t expected[] = {188, x >= 1 && x <= 2 && y < 3 ? 37 : 0, 0, 255};
            good += !memcmp(data + (y * 4 + x) * 4, expected, sizeof expected);
        }
        CHECK(good == 16, "sRGB and UINT view clear pixels=%u/16 first red=%u", good, data[0]);
        printf("CLEAR_TEX format-view pixels=%u/16 rejected=%d\n", good, rejected);
    }
    release_clear_case(all, rb);
}

static void clear_view_not_declared(uint64_t signal) {
    nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                                .width = 4, .height = 4, .depth = 1, .levels = 1},
                           .usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED};
    nvn_gpu_err err = {0};
    const nvn_gpu_rc made = nvn_gpu_tex_create(&td, &g_image, &err);
    CHECK(OK(made), "immutable clear texture: %s", err.why);
    if (!OK(made)) return;
    nvn_gpu_view all = make_view(g_image);
    nvn_gpu_buf rb = make_buf(64, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    const float bg[4] = {0, 0, 0, 1};
    nvn_gpu_clear_view(cmd, all, bg, NULL, 0);
    tex_t t = {.d = td.d, .serial = signal};
    p_clear_tex op = {.tex = &t, .has_view = 1, .view = {.set_format = 1, .format = NVN_FORMAT_RGBA8_SRGB},
                      .r = {0, 0, 0, 4, 4, 1}, .mask = 15, .value_type = CLEAR_TEX_FLOAT,
                      .c.f = {0.5f, 0, 0, 1}};
    rp_op_clear_tex(cmd, &op, &op);
    CHECK(g_unimpl == 1 && g_writes == 0, "immutable texture format change: rejected=%d writes=%d", g_unimpl, g_writes);
    nvn_gpu_copy_tex_buf(cmd, g_image, 0, 0, NULL, rb, 0, 16, 64);
    if (clear_case_wait(cmd, signal)) {
        const uint8_t *data = buf_cpu(rb);
        const uint8_t expected[] = {0, 0, 0, 255};
        unsigned good = 0;
        for (unsigned i = 0; i < 16; i++) good += !memcmp(data + i * 4, expected, sizeof expected);
        CHECK(good == 16, "unavailable view must preserve base pixels: %u/16", good);
        printf("CLEAR_TEX unavailable-view preserved=%u/16 rejected=%d\n", good, g_unimpl);
    }
    release_clear_case(all, rb);
}

int main(void) {
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { printf("device failed: %s\n", why); return 2; }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "nvn-replay-clear-test";
    if (!open_backend()) return 1;
    nvn_gpu_tex_desc td = {0}; td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 8; td.d.depth = 3; td.d.levels = 2; td.usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED;
    nvn_gpu_err e = {0}; nvn_gpu_rc r = nvn_gpu_tex_create(&td, &g_image, &e);
    if (!OK(r)) { printf("texture failed: %d %s\n", r.code, e.why); return 1; }
    nvn_gpu_view all = make_view(g_image); nvn_gpu_buf rb = make_buf(3 * 4 * 4 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); float bg[4] = {0.25f, 0.5f, 0.75f, 1};
    nvn_gpu_clear_view(c, all, bg, NULL, 0);
    tex_t t = {0}; t.d = td.d; t.serial = 1;
    p_clear_tex p = {0}; p.tex = &t; p.has_view = 1; p.view.set_levels = 1; p.view.base_level = 1;
    p.r.zoffset = 1; p.r.depth = 1; p.r.width = p.r.height = 4; p.mask = 15; p.value_type = CLEAR_TEX_FLOAT; p.c.f[0] = p.c.f[3] = 1;
    rp_op_clear_tex(c, &p, &p);
    p.r.zoffset = 2; p.r.xoffset = 1; p.r.yoffset = 1; p.r.width = 2; p.r.height = 2; p.mask = 2; p.c.f[1] = 1;
    rp_op_clear_tex(c, &p, &p);
    for (uint32_t layer = 0; layer < 3; layer++) nvn_gpu_copy_tex_buf(c, g_image, 1, layer, NULL, rb, 64 * layer, 16, 64);
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e); CHECK(OK(r), "commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e); CHECK(OK(r), "wait：%d %s", r.code, e.why);
    uint8_t *pixels = buf_cpu(rb);
    for (int layer = 0; layer < 3; layer++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        const uint8_t *q = pixels + layer * 64 + (y * 4 + x) * 4;
        int green = layer == 2 && x >= 1 && x < 3 && y >= 1 && y < 3;
        uint8_t expected[4] = {layer == 1 ? 255 : 64, layer == 1 ? 0 : green ? 255 : 128, layer == 1 ? 0 : 191, 255};
        CHECK(memcmp(q, expected, 4) == 0, "layer %d xy %d,%d = %u,%u,%u,%u expected %u,%u,%u,%u", layer, x, y,
              q[0], q[1], q[2], q[3], expected[0], expected[1], expected[2], expected[3]);
    }
    CHECK(g_writes == 2 && g_unimpl == 0, "writes %d unimpl %d", g_writes, g_unimpl);
    release_clear_case(all, rb);
    clear_layers(NVN_TEXTURE_TARGET_3D, 8, "3D", 2);
    clear_layers(NVN_TEXTURE_TARGET_CUBEMAP, 6, "CUBE", 3);
    clear_layers(NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, "CUBE_ARRAY", 4);
    clear_reinterpreted(5);
    clear_view_not_declared(6);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "close：%d %s", e.rc.code, e.why);
    s2a_vk_device_destroy(&g_dev, 1);
    printf("%s replay CLEAR_TEX: 2D array, 3D, cubes and format views, failures=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
