// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "rp_exec.h"
#include "rp_res.h"
#include "rp_state.h"
#include "nvn_unimpl.h"
#include "nvn_fmt_info.h"
#include "nvn_plat.h"
#include <math.h>
#include <string.h>

// 弱链接[replay]：nvn_gpu_view_create_color_attachment
nvn_gpu_rc nvn_gpu_view_create_color_attachment(nvn_gpu_tex, const view_t *, nvn_gpu_view *, nvn_gpu_err *) __attribute__((weak));

static int clear_color_format(uint32_t fmt, nvn_fmt_info *fi) {
    return nvn_fmt_info_of(fmt, fi) && !fi->depth && !fi->stencil && !fi->expand && fi->bw == 1 && fi->bh == 1;
}

static void clear_value(const p_clear_tex *o, int kind, nvn_gpu_clear *cl) {
    for (int k = 0; k < 4; k++) {
        double v = o->value_type == CLEAR_TEX_UINT ? (double)o->c.u[k] :
                   o->value_type == CLEAR_TEX_INT ? (double)o->c.i[k] : (double)o->c.f[k];
        if (kind == NVN_FMT_UINT) cl->value.u[k] = isnan(v) || v <= 0 ? 0u : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
        else if (kind == NVN_FMT_SINT) cl->value.i[k] = isnan(v) ? 0 : v <= -2147483648.0 ? INT32_MIN : v >= 2147483647.0 ? INT32_MAX : (int32_t)v;
        else cl->value.f[k] = (float)v;
    }
}

void rp_op_clear_tex(nvn_gpu_cmd c, const p_clear_tex *o, const void *rec) {
    if (!(o->mask & 15) || !o->r.width || !o->r.height) return;
    const tex_t *t = o->tex;
    if (!t || !c) { if (c) nvn_unimpl("tex", "clear_no_texture", rec); return; }
    nvn_texdesc d = t->d;
    rp_fold_1d_desc(&d);
    const int volume = d.target == NVN_TEXTURE_TARGET_3D;
    if (!volume && d.target != NVN_TEXTURE_TARGET_2D && d.target != NVN_TEXTURE_TARGET_2D_ARRAY &&
        d.target != NVN_TEXTURE_TARGET_CUBEMAP && d.target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
        nvn_unimpl("tex", "clear_target", rec); return;
    }
    if (t->d.samples > 1 || o->value_type > CLEAR_TEX_INT) { nvn_unimpl("tex", "clear_samples_or_value", rec); return; }
    nvn_fmt_info fi;
    if (!clear_color_format((uint32_t)t->d.format, &fi)) {
        nvn_unimpl("tex", "clear_format", rec); return;
    }
    const uint32_t fmt = o->has_view && o->view.set_format ? o->view.format : (uint32_t)t->d.format;
    const int reinterpret = fmt != (uint32_t)t->d.format;
    if (reinterpret) {
        nvn_fmt_info vf;
        if (!nvn_gpu_view_create_color_attachment || !clear_color_format(fmt, &vf) || fi.bytes != vf.bytes) {
            nvn_unimpl("tex", "clear_view_format", rec); return;
        }
        fi = vf;
    }
    uint32_t lv = o->has_view && o->view.set_levels ? o->view.base_level : 0;
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    if (lv >= levels || lv >= 32 || !d.width || !d.height) { nvn_unimpl("tex", "clear_level", rec); return; }
    uint32_t w = d.width >> lv, h = d.height >> lv;
    if (!w) w = 1;
    if (!h) h = 1;
    // 3D 的 z 是该级的深度坐标；立方体与数组的 z 叠加在视图的首层上。
    uint32_t l0 = !volume && o->has_view && o->view.set_layers ? o->view.min_layer : 0;
    uint32_t layers = volume ? (uint32_t)d.depth >> lv :
        d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? d.depth : rp_tex_layer_count(t);
    if (volume && !layers) layers = 1;
    int32_t nz = o->r.depth ? o->r.depth : 1;
    if (o->r.xoffset < 0 || o->r.yoffset < 0 || o->r.zoffset < 0 || o->r.width < 0 || o->r.height < 0 || nz < 0 ||
        (uint64_t)o->r.xoffset + o->r.width > w || (uint64_t)o->r.yoffset + o->r.height > h ||
        (uint64_t)l0 + o->r.zoffset + nz > layers || (uint64_t)l0 + o->r.zoffset + nz > 65536u) {
        nvn_unimpl("tex", "clear_bounds", rec); return;
    }
    unsigned active = (1u << fi.channels) - 1u, mask = (unsigned)o->mask & active;
    if (!mask) return;
    int whole = !o->r.xoffset && !o->r.yoffset && (uint32_t)o->r.width == w && (uint32_t)o->r.height == h && mask == active;
    int transfer = whole && !reinterpret && (!volume || (!o->r.zoffset && (uint32_t)nz == layers));
    if (!transfer && !(nvn_gpu_format_caps(fmt) & NVN_GPU_FMT_RENDER)) { nvn_unimpl("tex", "clear_not_renderable", rec); return; }
    nvn_gpu_tex storage = reinterpret ? rp_tex_resolve(t, c) : NULL;
    if (reinterpret && !storage) return;
    nvn_gpu_clear cl = {0}; cl.color = 0; cl.color_mask = (uint8_t)mask; cl.stencil_mask = 0xff;
    cl.x = o->r.xoffset; cl.y = o->r.yoffset; cl.w = (uint32_t)o->r.width; cl.h = (uint32_t)o->r.height;
    clear_value(o, fi.kind, &cl);
    int is_int = fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT;
    // Vulkan 传输清图只能清 3D 的整级；其余清除用逐片的二维附件。
    const int whole_volume = volume && transfer;
    const int32_t views = whole_volume ? 1 : nz;
    for (int32_t z = 0; z < views; z++) {
        view_t v = {0}; v.set_levels = 1; v.base_level = (uint8_t)lv; v.nlevels = 1;
        v.set_target = 1; v.target = whole_volume ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D;
        if (!whole_volume) {
            v.set_layers = 1; v.min_layer = (uint16_t)(l0 + (uint32_t)o->r.zoffset + (uint32_t)z); v.nlayers = 1;
        }
        rp_view_pin_begin();
        nvn_gpu_view gpu = NULL;
        if (reinterpret) {
            v.set_format = 1; v.format = fmt;
            nvn_gpu_err err = {0};
            if (!nvn_gpu_ok(nvn_gpu_view_create_color_attachment(storage, &v, &gpu, &err)) &&
                nvn_unimpl("tex", "clear_view_format", rec) == 1)
                nvn_plat_log(NVN_PLAT_LOG_RED, "clear_view_format", err.why);
        } else gpu = rp_view_for(t, &v, c);
        if (!gpu) { rp_view_pin_end(); return; }
        if (transfer) nvn_gpu_clear_view(c, gpu, cl.value.f, cl.value.u, is_int);
        else {
            nvn_gpu_pass_desc pass = {0}; pass.ncolor = 1; pass.w = w; pass.h = h; pass.layers = 1;
            pass.color[0].view = gpu; pass.color[0].load = whole ? NVN_GPU_LOAD_CLEAR : NVN_GPU_LOAD_LOAD;
            pass.color[0].store = NVN_GPU_STORE_STORE;
            if (whole) memcpy(&pass.color[0].clear, &cl.value, sizeof pass.color[0].clear);
            nvn_gpu_pass_begin(c, &pass);
            if (!whole) nvn_gpu_clear_rect(c, &cl);
            nvn_gpu_pass_end(c); rp_dirty_all();
        }
        if (reinterpret) nvn_gpu_view_release(gpu);
        rp_view_pin_end(); rp_tex_mark_gpu_written(t);
    }
}
