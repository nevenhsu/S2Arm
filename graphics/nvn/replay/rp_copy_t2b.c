// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 贴图到缓冲的录制回放：GPU 拷到池镜像，段成功后才回写真实写过的行。
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_unimpl.h"
#include "nvn_fmt_info.h"
#include <stdint.h>

static int mul64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b && a > UINT64_MAX / b) return 0;
    *out = a * b; return 1;
}
void rp_op_copy_t2b(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec) {
    const tex_t *t = o->tex;
    if (!c) return;   // 本段未建立，提交本身已失败。
    if (!t) { nvn_unimpl("tex", "t2b_no_texture", rec); return; }
    nvn_texdesc d = t->d;
    rp_fold_1d_desc(&d);
    int target = d.target, is3d = target == NVN_TEXTURE_TARGET_3D;
    if (target != NVN_TEXTURE_TARGET_2D && target != NVN_TEXTURE_TARGET_2D_ARRAY && !is3d &&
        target != NVN_TEXTURE_TARGET_CUBEMAP && target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
        nvn_unimpl("tex", "t2b_target", rec); return;
    }
    if (t->d.samples > 1 || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) ||
        (o->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT))) {
        nvn_unimpl("tex", "t2b_mode", rec); return;
    }
    nvn_fmt_info fi; int bw = 0, bh = 0, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_info_of((uint32_t)t->d.format, &fi) || !bpb || bw <= 0 || bh <= 0 || fi.expand) {
        nvn_unimpl("tex", "t2b_format", rec); return;
    }
    const uint32_t exact_subst = NVN_GPU_FMT_SUBST | NVN_GPU_FMT_READBACK;
    if (((fi.depth || fi.stencil) && (nvn_gpu_format_caps((uint32_t)t->d.format) & exact_subst) != exact_subst) ||
        ((bw > 1 || bh > 1) && !(nvn_gpu_format_caps((uint32_t)t->d.format) & NVN_GPU_FMT_SAMPLE))) {
        nvn_unimpl("tex", "t2b_format", rec); return;
    }
    uint32_t lv = o->has_view && o->view.set_levels ? o->view.base_level : 0;
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    if (lv >= levels || lv >= 32 || !d.width || !d.height) { nvn_unimpl("tex", "t2b_bounds", rec); return; }
    uint32_t w = d.width >> lv, h = d.height >> lv;
    if (!w) w = 1;
    if (!h) h = 1;
    uint32_t depth = is3d ? d.depth >> lv : target == NVN_TEXTURE_TARGET_2D ? 1 :
                     target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : d.depth;
    if (is3d && !depth) depth = 1;
    uint32_t l0 = !is3d && o->has_view && o->view.set_layers ? o->view.min_layer : 0;
    int32_t nz = o->r.depth ? o->r.depth : 1;
    if (o->r.xoffset < 0 || o->r.yoffset < 0 || o->r.zoffset < 0 || o->r.width <= 0 || o->r.height <= 0 || nz <= 0 ||
        (uint64_t)o->r.xoffset + (uint32_t)o->r.width > w || (uint64_t)o->r.yoffset + (uint32_t)o->r.height > h ||
        (uint64_t)l0 + (uint32_t)o->r.zoffset + (uint32_t)nz > depth ||
        o->r.xoffset % bw || o->r.yoffset % bh ||
        ((uint32_t)o->r.width % (uint32_t)bw && (uint64_t)o->r.xoffset + (uint32_t)o->r.width != w) ||
        ((uint32_t)o->r.height % (uint32_t)bh && (uint64_t)o->r.yoffset + (uint32_t)o->r.height != h)) {
        nvn_unimpl("tex", "t2b_bounds", rec); return;
    }
    uint64_t blocks = ((uint64_t)(uint32_t)o->r.width + (uint32_t)bw - 1) / (uint32_t)bw;
    uint32_t rows = (uint32_t)(((uint64_t)(uint32_t)o->r.height + (uint32_t)bh - 1) / (uint32_t)bh);
    uint64_t bytes, min_img, total;
    if (!mul64(blocks, (uint64_t)bpb, &bytes)) { nvn_unimpl("tex", "t2b_bad_stride", rec); return; }
    uint64_t row = o->row_stride ? o->row_stride : bytes;
    if (row < bytes || row % (uint64_t)bpb || row / (uint64_t)bpb > UINT32_MAX / (uint32_t)bw ||
        !mul64(row, rows, &min_img)) { nvn_unimpl("tex", "t2b_bad_stride", rec); return; }
    uint64_t img = o->image_stride ? o->image_stride : min_img;
    if (img < min_img || img / row > UINT32_MAX / (uint32_t)bh || !mul64(img, (uint32_t)nz, &total) ||
        o->src > UINT64_MAX - total) { nvn_unimpl("tex", "t2b_bad_stride", rec); return; }
    nvn_gpu_tex gpu = rp_tex_resolve(t, c);
    if (!gpu) return;
    nvn_gpu_buf buf = NULL; uint64_t off = 0;
    if (!rp_buf_resolve(c, o->src, total, &buf, &off)) { nvn_unimpl("buf", "t2b_destination_outside_pools", rec); return; }
    if ((off & 3) || off % (uint64_t)bpb || (nz > 1 && ((img & 3) || img % (uint64_t)bpb))) {
        nvn_unimpl("tex", "t2b_alignment", rec); return;
    }
    if (!nvn_gpu_buf_cpu(buf)) { nvn_unimpl("buf", "t2b_mirror_not_cpu_visible", rec); return; }
    if (!rp_post_gpu_write_rows(o->src, bytes, row, img, rows, (uint32_t)nz)) { nvn_unimpl("buf", "t2b_write_registration", rec); return; }
    NVNcopyRegion r = o->r; r.zoffset = 0; r.depth = 1;
    for (int32_t z = 0; z < nz; z++) {
        uint32_t layer = l0 + (uint32_t)o->r.zoffset + (uint32_t)z;
        nvn_gpu_copy_tex_buf(c, gpu, lv, layer, &r, buf, off + (uint64_t)z * img, row, img);
    }
}
