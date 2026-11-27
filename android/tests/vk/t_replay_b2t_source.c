// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Reuse the compute fixture's shader/pool bindings; dispatch, mirror ownership,
// B2T replay, Vulkan transfers and all pixel reads use their production implementations.
#define main dispatch_barrier_main
#include "t_replay_dispatch_barrier.c"
#undef main

static tex_t texture;
static nvn_gpu_tex image;

nvn_gpu_tex rp_tex_resolve_plain(const tex_t *t, nvn_gpu_cmd c) {
    REQUIRE(t == &texture && c == active);
    return image;
}

static void check_pixels(const uint32_t *pixels, uint32_t size, int target, int gpu_source,
                         unsigned *control_errors, unsigned *source_errors) {
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            const int copied = target && x >= 1 && x < 3 && y >= 1 && y < 3;
            uint32_t want = 0x35353535;
            if (copied) {
                const uint32_t i = 4 * (y - 1) + (x - 1); // 2 texels + 2 words of row padding
                want = gpu_source ? 0x24240000 + 2 * i : 0x61610000 + i;
            }
            if (pixels[y * size + x] != want) {
                fprintf(stderr, "%s x=%u y=%u got=%08x want=%08x\n",
                        copied ? "source" : "untouched", x, y, pixels[y * size + x], want);
                if (copied) ++*source_errors;
                else ++*control_errors;
            }
        }
    }
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    const int gpu_source = !strcmp(argv[2], "gpu");
    REQUIRE(gpu_source || !strcmp(argv[2], "cpu"));
    struct s2a_vk_device device;
    char why[256];
    REQUIRE(!s2a_vk_device_create(&device, why, sizeof why));
    nvn_gpu_open_desc od = {.host_device = &device};
    GPU(nvn_gpu_open(&od, &err));
    rp_state_reset();
    nvn_unimpl_reset();
    prog.is_compute = 1;
    prog.nent[0] = 1;
    prog.ent[0][0] = (nvn_gpu_layout_ent){.cls = NVN_GPU_RES_SSBO, .src_kind = NVN_RB_SBUFP,
        .src_reg2 = UINT16_MAX, .access = NVN_GPU_ACC_RW, .count = 1, .stages = 1u << NVN_GPU_SH_CS};
    nvn_gpu_layout layout = {.ent = prog.ent[0], .nent = 1};
    prog.sh[0] = shader(argv[1], "bar_double.comp", &layout);
    uint32_t words[128];
    for (uint32_t i = 0; i < 64; i++) {
        words[i] = 0x12120000 + i;
        words[64 + i] = 0x35353535;
        uint32_t cpu_pixel = 0x61610000 + i;
        memcpy(guest + 2048 + 4 * i, &cpu_pixel, 4);
    }
    memcpy(guest, words, sizeof words);
    rp_S.ssbo[5][0].addr = (uintptr_t)guest;
    rp_S.ssbo[5][0].size = sizeof words;

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 8;
    td.d.depth = td.d.levels = 2;
    td.usage = NVN_GPU_TEX_SAMPLED;
    texture.serial = 1;
    texture.d = td.d;
    GPU(nvn_gpu_tex_create(&td, &image, &err));
    nvn_gpu_buf readback;
    GPU(nvn_gpu_buf_create(4 * 256, NVN_GPU_MEM_SHARED, NULL, &readback, &err));
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    for (uint32_t level = 0; level < 2; level++) {
        int size = 8 >> level;
        NVNcopyRegion region = {0, 0, 0, size, size, 1};
        for (uint32_t layer = 0; layer < 2; layer++)
            nvn_gpu_tex_upload(active, image, level, layer, &region, words + 64, size * 4, size * size * 4);
    }
    p_dispatch dispatch = {.x = 1, .y = 1, .z = 1};
    rp_dispatch(active, &dispatch, NULL);
    REQUIRE(nw == 1);
    p_copy_b2t op = {.tex = &texture, .has_view = 1, .r = {1, 1, 0, 2, 2, 1},
                     .row_stride = 16, .image_stride = 32};
    op.src = (uintptr_t)(guest + (gpu_source ? 256 : 2048));
    op.view.set_layers = op.view.set_levels = 1;
    op.view.min_layer = op.view.base_level = 1;
    rp_op_copy_b2t(active, &op, &op);
    REQUIRE(nvn_unimpl_kinds() == 0);
    for (uint32_t level = 0; level < 2; level++) {
        int size = 8 >> level;
        NVNcopyRegion region = {0, 0, 0, size, size, 1};
        for (uint32_t layer = 0; layer < 2; layer++)
            nvn_gpu_copy_tex_buf(active, image, level, layer, &region, readback, (2 * level + layer) * 256,
                                 size * 4, size * size * 4);
    }
    GPU(nvn_gpu_cmd_commit(active, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    rp_buf_gpu_write_land(wkey[0], wid[0], 1);
    uint32_t produced;
    memcpy(&produced, guest + 256, sizeof produced);
    REQUIRE(produced == 0x24240000);
    unsigned control_errors = 0, source_errors = 0;
    for (uint32_t level = 0; level < 2; level++)
        for (uint32_t layer = 0; layer < 2; layer++)
            check_pixels((const uint32_t *)(nvn_gpu_buf_cpu(readback) + (2 * level + layer) * 256),
                         8 >> level, level == 1 && layer == 1, gpu_source, &control_errors, &source_errors);
    printf("B2T %s: producer=%08x untouched_errors=%u/156 source_errors=%u/4\n",
           argv[2], produced, control_errors, source_errors);
    nvn_gpu_buf_release(readback, NULL, NULL);
    nvn_gpu_tex_release(image);
    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_shader_release(prog.sh[0]);
    GPU(nvn_gpu_close(5000000000ull, &err));
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    return control_errors || source_errors;
}
