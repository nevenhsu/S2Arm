// 回放后端契约 graphics/nvn/gfx/nvn/nvn_backend.h 的编译自洽单测（W1 D0）：只 include，不接任何实现、不链任何后端。
// 证明三件事：
//   1. 头文件单独可编（C11 与 Objective-C 各编一次，见 runtime/tests/mk/nvn_backend.mk），不依赖 Metal / Vulkan 头；
//   2. 句柄是互不相容的不透明指针（传错类型编译期就报）；
//   3. 签名表：每个契约函数的类型逐个钉死。改签名必须同时改这里和 NVN_GPU_CONTRACT_VERSION（两个后端同批改）。
//   4. 错误码在独立区间、与安卓会话 ABI 不相交；nvn_gpu_rc 不能当 int 用（.mk 另跑两条「必须编译失败」的探针）。
// 「头文件里没有 Metal / Vulkan 类型」由 .mk 里的去注释扫描判（注释里的 MTL* / Vk* 对照允许）。签名表与编译自洽不证明语义，语义见设计文档 §8 契约用例清单。
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nvn_backend.h"
#include "nvn_vsync.h"

#define SIG(fn, T) _Static_assert(__builtin_types_compatible_p(__typeof__(&fn), T), #fn " 签名与契约表不符")
#define DISTINCT(A, B) _Static_assert(!__builtin_types_compatible_p(A, B), #A " 与 " #B " 必须是不同的句柄类型")

_Static_assert(NVN_GPU_CONTRACT_VERSION == 26, "契约版本变了：同批更新本签名表");

// v24 冷环绑定描述：缓冲身份、访问区间和嵌套前缀按共用契约传递。
#define FIELD_TYPE(record, member, T) \
    _Static_assert(__builtin_types_compatible_p(__typeof__(((struct record *)0)->member), T), \
                   #record "." #member " 字段类型与契约表不符")
FIELD_TYPE(nvn_gpu_cold_prefix, buf, nvn_gpu_buf);
FIELD_TYPE(nvn_gpu_cold_prefix, binding, nvn_gpu_access_span);
FIELD_TYPE(nvn_gpu_cold_prefix, fragment_ent, uint32_t);
FIELD_TYPE(nvn_gpu_cold_prefix, byte, uint32_t);
FIELD_TYPE(nvn_gpu_cold_prefix, mask, uint32_t);
FIELD_TYPE(nvn_gpu_cold_domain, buf, nvn_gpu_buf);
FIELD_TYPE(nvn_gpu_cold_domain, binding, nvn_gpu_access_span);
FIELD_TYPE(nvn_gpu_cold_domain, first_word, uint32_t);
FIELD_TYPE(nvn_gpu_cold_domain, stride_words, uint32_t);
FIELD_TYPE(nvn_gpu_cold_domain, count, uint32_t);
FIELD_TYPE(nvn_gpu_cold_domain, fragment_ent, uint32_t);
FIELD_TYPE(nvn_gpu_cold_domain, prefix, struct nvn_gpu_cold_prefix);
#undef FIELD_TYPE

// 句柄：指针大小、两两不同
_Static_assert(sizeof(nvn_gpu_buf) == sizeof(void *), "句柄应为指针");
DISTINCT(nvn_gpu_buf, nvn_gpu_tex); DISTINCT(nvn_gpu_tex, nvn_gpu_view); DISTINCT(nvn_gpu_view, nvn_gpu_samp);
DISTINCT(nvn_gpu_samp, nvn_gpu_shader); DISTINCT(nvn_gpu_shader, nvn_gpu_pipe); DISTINCT(nvn_gpu_pipe, nvn_gpu_cmd);
DISTINCT(nvn_gpu_cmd, nvn_gpu_surface); DISTINCT(nvn_gpu_buf, nvn_gpu_view); DISTINCT(nvn_gpu_tex, nvn_gpu_samp);

// 阶段号 = NVN 阶段号；管线描述里的 5 个图形阶段在 CS 之前
_Static_assert(NVN_GPU_SH_VS == 0 && NVN_GPU_SH_FS == 1 && NVN_GPU_SH_GS == 2 && NVN_GPU_SH_TCS == 3 && NVN_GPU_SH_TES == 4 && NVN_GPU_SH_CS == 5, "阶段号须同 NVN");
_Static_assert(sizeof(((nvn_gpu_pipe_desc *)0)->st) / sizeof(nvn_gpu_stage_desc) == NVN_GPU_SH_CS, "图形阶段数");
// 契约结构直接嵌 NVN 记录（不另造中间枚举）：字段类型就是 nvn_obj.h 的类型
_Static_assert(__builtin_types_compatible_p(__typeof__(((nvn_gpu_pipe_desc *)0)->blend[0]), blend_st), "混合状态用 NVN 记录");
_Static_assert(__builtin_types_compatible_p(__typeof__(((nvn_gpu_pipe_desc *)0)->attr[0]), va_st), "顶点属性用 NVN 记录");
_Static_assert(__builtin_types_compatible_p(__typeof__(((nvn_gpu_tex_desc *)0)->d), nvn_texdesc), "贴图描述用 NVN 记录");
// 贴图描述的字节布局：回放夹具格式 §4.3（android/tests/replay_fixture/FORMAT.md）按这个逐字节写金样；末尾 2 字节是保留位（恒 0）
_Static_assert(sizeof(nvn_texdesc) == 40 && offsetof(nvn_gpu_tex_desc, usage) == 40 && offsetof(nvn_gpu_tex_desc, host_w) == 44 && offsetof(nvn_gpu_tex_desc, host_h) == 48
               && offsetof(nvn_gpu_tex_desc, swz) == 52 && offsetof(nvn_gpu_tex_desc, _rsv) == 54 && sizeof(((nvn_gpu_tex_desc *)0)->_rsv) == 2 && sizeof(nvn_gpu_tex_desc) == 56,
               "贴图描述的字节布局变了：回放夹具格式 §4.3 与已提交的金样同批改");
_Static_assert(__builtin_types_compatible_p(__typeof__(((nvn_gpu_dyn *)0)->ds), ds_st), "深度模板用 NVN 记录");
_Static_assert(sizeof(((nvn_gpu_dyn *)0)->vp) == 16 * 6 * sizeof(float), "视口记录同 p_viewports");
_Static_assert(sizeof(((nvn_gpu_err *)0)->why) >= 160, "why 至少容下安卓 s2a_record_result.why 的 160 字节");
// 能力位互不重叠且连续
enum { CAPS_OR = NVN_GPU_CAP_HOST_IMPORT | NVN_GPU_CAP_GPU_VA | NVN_GPU_CAP_TILE_MEMORY | NVN_GPU_CAP_SAMP_MINMAX | NVN_GPU_CAP_SAMP_LOD_BIAS
       | NVN_GPU_CAP_DEPTH_CLIP_CTL | NVN_GPU_CAP_GEOMETRY | NVN_GPU_CAP_TESSELLATION | NVN_GPU_CAP_XFB | NVN_GPU_CAP_INDIRECT_COUNT | NVN_GPU_CAP_LINKED_PIPE
       | NVN_GPU_CAP_TEX_BC | NVN_GPU_CAP_TEX_ASTC | NVN_GPU_CAP_GPU_DURATION | NVN_GPU_CAP_GPU_TIME_HOST | NVN_GPU_CAP_SHOWN_TIME
       | NVN_GPU_CAP_DYN_DEPTH_STENCIL | NVN_GPU_CAP_DYN_RASTER | NVN_GPU_CAP_BLIT_SCALED | NVN_GPU_CAP_GPU_DESWIZZLE | NVN_GPU_CAP_BLIT_DEPTH_STENCIL
       | NVN_GPU_CAP_NO_ATTACH_PASS | NVN_GPU_CAP_REPORT_TIMESTAMP | NVN_GPU_CAP_STORAGE_SRGB_ALIAS | NVN_GPU_CAP_DRAW_TEX | NVN_GPU_CAP_BC1_SNAPSHOT };
_Static_assert(CAPS_OR == (1u << 26) - 1, "能力位应为连续且不重叠的 26 位");

// 错误域（定稿 A7）：与安卓 native–NativeAOT 会话 ABI 不相交。会话 ABI 码值抄自 android/native/direct/s2a_direct.h
//   enum s2a_direct_result（#868 头 642ec4574 / android/868-land-1029@ac3d6ec23，DirectAbi.cs 同值）：OK 0、PENDING +1、ARGUMENT -1 … INTERNAL -9。
//   那边扩码时改这里的 S2A_ABI_MIN / S2A_ABI_MAX。D0 v1 的撞值（PENDING -7 = DRIVER、TIMEOUT -5 = STOPPED）就是这条要挡住的。
enum { S2A_ABI_MIN = -9, S2A_ABI_MAX = 1 };
#define OUTSIDE_ABI(c) _Static_assert((int)(c) < (int)S2A_ABI_MIN || (int)(c) > (int)S2A_ABI_MAX, #c " 与安卓会话 ABI 码值区间 [-9, 1] 相交")
#define IN_RANGE(c) _Static_assert((int)(c) <= (int)NVN_GPU_E_BASE && (int)(c) >= (int)NVN_GPU_E_BASE - 99, #c " 不在 nvn_gpu 错误区间 [-1099, -1000]")
#define ERR(c) OUTSIDE_ABI(c); IN_RANGE(c)
ERR(NVN_GPU_E_ARG); ERR(NVN_GPU_E_UNSUPPORTED); ERR(NVN_GPU_E_OOM); ERR(NVN_GPU_E_DEVICE); ERR(NVN_GPU_E_TIMEOUT); ERR(NVN_GPU_E_ORDER); ERR(NVN_GPU_E_PENDING);
ERR(NVN_GPU_E_COMPILE); ERR(NVN_GPU_E_LAYOUT); ERR(NVN_GPU_E_OUTDATED); ERR(NVN_GPU_E_REVOKED); ERR(NVN_GPU_E_FAILED); ERR(NVN_GPU_E_GPU_FAULT); ERR(NVN_GPU_E_STOPPED);
_Static_assert((int)NVN_GPU_E_LAST == (int)NVN_GPU_E_STOPPED && (int)NVN_GPU_E_ARG == (int)NVN_GPU_E_BASE - 1, "错误码连续：新码接在 E_LAST 后面并更新 E_LAST 与上面的 ERR 表");
_Static_assert((int)NVN_GPU_E_BASE - (int)NVN_GPU_E_LAST == 14, "错误码 14 个：增删时同批改 ERR 表");
// 返回值类型不是整数：不能与 int 互相赋值 / cast（.mk 另有两条编译失败探针实测这一点）
_Static_assert(!__builtin_types_compatible_p(nvn_gpu_rc, int) && sizeof(nvn_gpu_rc) == sizeof(int32_t), "nvn_gpu_rc 必须是包一层的结构体");
// 布局项：set / binding / 来源与 Metal aid 同在一项（定稿 A1）
_Static_assert(sizeof(nvn_gpu_layout_ent) == 24, "布局项含独立采样器来源槽");
_Static_assert(NVN_GPU_RES_N == 9, "资源类别 9 种：增删时同批改 caps.max_stage_res 的来源注释");

// 签名表（按头文件顺序）
SIG(nvn_gpu_ok, int (*)(nvn_gpu_rc));
SIG(nvn_gpu_open, nvn_gpu_rc (*)(const nvn_gpu_open_desc *, nvn_gpu_err *));
SIG(nvn_gpu_close, nvn_gpu_rc (*)(uint64_t, nvn_gpu_err *));
SIG(nvn_gpu_caps_get, void (*)(nvn_gpu_caps *));
SIG(nvn_gpu_format_caps, uint32_t (*)(uint32_t));
SIG(nvn_gpu_buf_wrap, nvn_gpu_rc (*)(void *, uint64_t, nvn_gpu_buf *, nvn_gpu_err *));
SIG(nvn_gpu_buf_create, nvn_gpu_rc (*)(uint64_t, int, const void *, nvn_gpu_buf *, nvn_gpu_err *));
SIG(nvn_gpu_buf_va, uint64_t (*)(nvn_gpu_buf));
SIG(nvn_gpu_buf_cpu, uint8_t *(*)(nvn_gpu_buf));
SIG(nvn_gpu_buf_release, void (*)(nvn_gpu_buf, nvn_gpu_retired_fn, void *));
SIG(nvn_gpu_buf_use, void (*)(nvn_gpu_cmd, nvn_gpu_buf));
SIG(nvn_gpu_buf_write, void (*)(nvn_gpu_cmd, nvn_gpu_buf, uint64_t, const void *, uint64_t));
SIG(nvn_gpu_arena_alloc, nvn_gpu_rc (*)(nvn_gpu_cmd, uint64_t, uint32_t, nvn_gpu_slice *, nvn_gpu_err *));
SIG(nvn_gpu_arena_fill, uint64_t (*)(nvn_gpu_cmd, const nvn_gpu_slice *, const void *, uint64_t));
SIG(nvn_gpu_arena_fill_wait, uint64_t (*)(uint64_t));
SIG(nvn_gpu_arena_fill_take, int (*)(uint64_t));
SIG(nvn_gpu_tex_create, nvn_gpu_rc (*)(const nvn_gpu_tex_desc *, nvn_gpu_tex *, nvn_gpu_err *));
SIG(nvn_gpu_tex_info_get, void (*)(nvn_gpu_tex, nvn_gpu_tex_info *));
SIG(nvn_gpu_tex_bc1_snapshot, int (*)(nvn_gpu_cmd, nvn_gpu_tex, const void *, uint64_t, uint64_t, nvn_gpu_tex *));
SIG(nvn_gpu_tex_bc1_unwrap, int (*)(nvn_gpu_cmd, nvn_gpu_tex, nvn_gpu_tex *));
SIG(nvn_gpu_tex_release, void (*)(nvn_gpu_tex));
SIG(nvn_gpu_view_create, nvn_gpu_rc (*)(nvn_gpu_tex, const view_t *, nvn_gpu_view *, nvn_gpu_err *));
SIG(nvn_gpu_view_create_color_attachment, nvn_gpu_rc (*)(nvn_gpu_tex, const view_t *, nvn_gpu_view *, nvn_gpu_err *));
SIG(nvn_gpu_view_create_buf, nvn_gpu_rc (*)(nvn_gpu_buf, uint64_t, uint64_t, uint32_t, nvn_gpu_view *, nvn_gpu_err *));
SIG(nvn_gpu_view_release, void (*)(nvn_gpu_view));
SIG(nvn_gpu_samp_create, nvn_gpu_rc (*)(const samp_par *, nvn_gpu_samp *, nvn_gpu_err *));
SIG(nvn_gpu_samp_release, void (*)(nvn_gpu_samp));
SIG(nvn_gpu_tex_upload, void (*)(nvn_gpu_cmd, nvn_gpu_tex, uint32_t, uint32_t, const NVNcopyRegion *, const void *, uint64_t, uint64_t));
SIG(nvn_gpu_tex_upload_blocklinear, nvn_gpu_rc (*)(nvn_gpu_cmd, nvn_gpu_tex, uint32_t, uint32_t, const nvn_texlayout *, const void *, uint64_t));
SIG(nvn_gpu_shader_load, nvn_gpu_rc (*)(const nvn_gpu_shader_src *, nvn_gpu_shader *, nvn_gpu_err *));
SIG(nvn_gpu_shader_write_end, uint64_t (*)(nvn_gpu_shader, unsigned, unsigned, unsigned, uint32_t));
SIG(nvn_gpu_shader_release, void (*)(nvn_gpu_shader));
SIG(nvn_gpu_pipe_get, nvn_gpu_rc (*)(const nvn_gpu_pipe_desc *, int, nvn_gpu_pipe *, nvn_gpu_err *));
SIG(nvn_gpu_pipe_lookup, int (*)(const nvn_gpu_pipe_desc *, nvn_gpu_pipe *));
SIG(nvn_gpu_cpipe_get, nvn_gpu_rc (*)(const nvn_gpu_stage_desc *, int, nvn_gpu_pipe *, nvn_gpu_err *));
SIG(nvn_gpu_cmd_begin, nvn_gpu_rc (*)(const nvn_gpu_cmd_desc *, nvn_gpu_cmd *, nvn_gpu_err *));
SIG(nvn_gpu_cmd_abort, void (*)(nvn_gpu_cmd));
SIG(nvn_gpu_pass_begin, void (*)(nvn_gpu_cmd, const nvn_gpu_pass_desc *));
SIG(nvn_gpu_pass_end, void (*)(nvn_gpu_cmd));
SIG(nvn_gpu_set_pipe, void (*)(nvn_gpu_cmd, nvn_gpu_pipe));
SIG(nvn_gpu_set_dyn, void (*)(nvn_gpu_cmd, const nvn_gpu_dyn *));
SIG(nvn_gpu_set_vbuf, void (*)(nvn_gpu_cmd, uint32_t, nvn_gpu_buf, uint64_t));
SIG(nvn_gpu_pool_create, nvn_gpu_rc (*)(const nvn_gpu_pool_desc *, nvn_gpu_pool_table *, nvn_gpu_err *));
SIG(nvn_gpu_view_texture, nvn_gpu_tex (*)(nvn_gpu_view));
SIG(nvn_gpu_pool_has_texture, int (*)(nvn_gpu_pool_table, nvn_gpu_tex));
SIG(nvn_gpu_pool_bind, nvn_gpu_rc (*)(nvn_gpu_cmd, nvn_gpu_stage, uint32_t, nvn_gpu_pool_table, nvn_gpu_err *));
SIG(nvn_gpu_pool_release, void (*)(nvn_gpu_pool_table));
SIG(nvn_gpu_bind, void (*)(nvn_gpu_cmd, nvn_gpu_stage, const nvn_gpu_binding *, uint32_t));
SIG(nvn_gpu_push, void (*)(nvn_gpu_cmd, nvn_gpu_stage, uint32_t, const void *, uint32_t));
SIG(nvn_gpu_draw, void (*)(nvn_gpu_cmd, const nvn_gpu_draw_args *));
SIG(nvn_gpu_dispatch, void (*)(nvn_gpu_cmd, nvn_gpu_pipe, uint32_t, uint32_t, uint32_t, nvn_gpu_buf, uint64_t));
SIG(nvn_gpu_copy_buf, void (*)(nvn_gpu_cmd, nvn_gpu_buf, uint64_t, nvn_gpu_buf, uint64_t, uint64_t));
SIG(nvn_gpu_copy_bufs_uniform, void (*)(nvn_gpu_cmd, const nvn_gpu_buf_copy *, uint32_t, uint32_t));
SIG(nvn_gpu_readback_enabled, int (*)(void));
SIG(nvn_gpu_readback_cmd_enabled, int (*)(nvn_gpu_cmd));
SIG(nvn_gpu_readback_record, uint64_t (*)(nvn_gpu_cmd, nvn_gpu_readback_span *, uint32_t));
SIG(nvn_gpu_fill_buf, void (*)(nvn_gpu_cmd, nvn_gpu_buf, uint64_t, uint64_t, uint32_t));
SIG(nvn_gpu_copy_buf_tex, void (*)(nvn_gpu_cmd, nvn_gpu_buf, uint64_t, uint64_t, uint64_t, nvn_gpu_tex, uint32_t, uint32_t, const NVNcopyRegion *));
SIG(nvn_gpu_copy_tex_buf, void (*)(nvn_gpu_cmd, nvn_gpu_tex, uint32_t, uint32_t, const NVNcopyRegion *, nvn_gpu_buf, uint64_t, uint64_t, uint64_t));
SIG(nvn_gpu_copy_tex, void (*)(nvn_gpu_cmd, nvn_gpu_tex, uint32_t, uint32_t, const NVNcopyRegion *, nvn_gpu_tex, uint32_t, uint32_t, const NVNcopyRegion *));
SIG(nvn_gpu_blit, void (*)(nvn_gpu_cmd, nvn_gpu_view, const NVNcopyRegion *, nvn_gpu_view, const NVNcopyRegion *, int));
SIG(nvn_gpu_clear_view, void (*)(nvn_gpu_cmd, nvn_gpu_view, const float *, const uint32_t *, int));
SIG(nvn_gpu_clear_rect, void (*)(nvn_gpu_cmd, const nvn_gpu_clear *));
SIG(nvn_gpu_draw_tex, void (*)(nvn_gpu_cmd, const nvn_gpu_draw_tex_desc *));
SIG(nvn_gpu_barrier, void (*)(nvn_gpu_cmd, uint32_t));
SIG(nvn_gpu_buf_access_span, int (*)(nvn_gpu_buf, uint64_t, uint64_t, nvn_gpu_access_span *));
SIG(nvn_gpu_view_access_span, int (*)(nvn_gpu_view, nvn_gpu_access_span *));
SIG(nvn_gpu_compute_to_graphics, void (*)(nvn_gpu_cmd));
SIG(nvn_gpu_timestamp, void (*)(nvn_gpu_cmd, uint32_t, uint32_t));
SIG(nvn_gpu_occlusion, void (*)(nvn_gpu_cmd, int32_t));
SIG(nvn_gpu_label, void (*)(nvn_gpu_cmd, const char *));
SIG(nvn_gpu_cmd_commit, nvn_gpu_rc (*)(nvn_gpu_cmd, uint64_t, nvn_gpu_done_fn, void *, nvn_gpu_err *));
SIG(nvn_gpu_fence_done, uint64_t (*)(void));
SIG(nvn_gpu_fence_failed, uint64_t (*)(void));
SIG(nvn_gpu_fence_wait, nvn_gpu_rc (*)(uint64_t, uint64_t, nvn_gpu_err *));
SIG(nvn_gpu_surface_attach, nvn_gpu_rc (*)(void *, nvn_gpu_surface *, nvn_gpu_err *));
SIG(nvn_gpu_surface_rebuild, nvn_gpu_rc (*)(nvn_gpu_surface, nvn_gpu_err *));
SIG(nvn_gpu_surface_gen, uint64_t (*)(nvn_gpu_surface));
SIG(nvn_gpu_surface_detach, nvn_gpu_rc (*)(nvn_gpu_surface, uint64_t, nvn_gpu_err *));
SIG(nvn_gpu_drawable_acquire, nvn_gpu_rc (*)(nvn_gpu_surface, uint64_t, void *, nvn_gpu_drawable *, nvn_gpu_err *));
SIG(nvn_gpu_drawable_release, void (*)(nvn_gpu_surface, const nvn_gpu_drawable *));
SIG(nvn_gpu_present, nvn_gpu_rc (*)(nvn_gpu_surface, const nvn_gpu_drawable *, const nvn_gpu_present_desc *, uint64_t, nvn_gpu_presented_fn, void *, nvn_gpu_err *));
SIG(nvn_gpu_stats, void (*)(nvn_gpu_stat_fn, void *, int));
// 平台节拍接口 nvn_vsync.h（定稿 #9，不属 nvn_gpu_*）
SIG(nvn_vsync_start, int (*)(uint32_t, nvn_vsync_fn, void *, char *, size_t));
SIG(nvn_vsync_stop, void (*)(void));
// #968 消费审查定稿 ①（10-30）：组合采样项 view 与 samp 同时装得下、互不重叠（原稿两者在同一个 union 里，只能放一个）
_Static_assert(offsetof(nvn_gpu_binding, u.ts.samp) >= offsetof(nvn_gpu_binding, u.ts.view) + sizeof(nvn_gpu_view), "TEX_SAMP 的 view 与 samp 重叠");
#define N_SIG 85   // 上面 SIG 的条数：nvn_gpu_* 83 + nvn_vsync_* 2（.mk 核对声明数）

int main(void) {
    // 运行期只做一件不依赖实现的事：典型调用点的结构体能按契约填出来（零初始化 + 字段可写），证明头文件里的类型完整可用
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.cfmt[0] = 0x25; pd.samples = 1; pd.st[NVN_GPU_SH_VS].nspec = 1; pd.st[NVN_GPU_SH_VS].spec[0] = (nvn_gpu_spec){1000, NVN_GPU_SPEC_BOOL, 0, 1};
    nvn_gpu_binding b = {.ent = 3, .cls = NVN_GPU_RES_VA, .u.va = 0x1000};
    nvn_gpu_binding ts = {.ent = 4, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {(nvn_gpu_view)(uintptr_t)0x10, (nvn_gpu_samp)(uintptr_t)0x20}};
    nvn_gpu_layout_ent le = {.set = 2, .cls = NVN_GPU_RES_UBO, .access = NVN_GPU_ACC_R, .src_kind = NVN_RB_SUPPORT, .binding = 5, .count = 1, .stages = 1u << NVN_GPU_SH_FS};
    nvn_gpu_rc rc = {NVN_GPU_E_PENDING};
    nvn_gpu_pass_desc ps = {.ncolor = 1, .w = 1280, .h = 720, .layers = 1};
    ps.color[0].load = NVN_GPU_LOAD_CLEAR; ps.color[0].store = NVN_GPU_STORE_STORE;
    int ok = pd.st[0].spec[0].id == 1000 && b.u.va == 0x1000 && ts.u.ts.view == (nvn_gpu_view)(uintptr_t)0x10 && ts.u.ts.samp == (nvn_gpu_samp)(uintptr_t)0x20 && le.binding == 5 && !nvn_gpu_ok(rc) && rc.code == -1007 && ps.color[0].load == NVN_GPU_LOAD_CLEAR && sizeof(nvn_gpu_dyn) > 0;
    printf("nvn_backend.h 契约 v%d：签名 %d 项、句柄 8 类、能力位 26 位、错误码 14 个（区间 [-1099,-1000]，与会话 ABI [-9,1] 不相交），编译期全部钉住；运行期填表 %s\n", NVN_GPU_CONTRACT_VERSION, N_SIG, ok ? "通过" : "失败");
    return ok ? 0 : 1;
}
