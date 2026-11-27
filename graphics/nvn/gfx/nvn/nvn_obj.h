// NVN host-side object layout and command record format: shared by hle_nvn.c (interface layer, writes) and nvn_mtl.m (Metal replay layer, reads).
// Objects are stored in place in the NVN object memory allocated by the game, with size not exceeding the reserved size in nvn.h (FITS static assertion).
// State objects (Blend / Color / ... 4–0x18 bytes) are small structs the game copies by value; their layout is packed in place with bit-fields.
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn.h"
#include "nvn_tex.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " exceeds the reserved size of " #H)

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data;
                 const char *dbg_label;                     /* DeviceSetDebugLabel：只保查询/日志用，不改行为 */ } dev_t_;  FITS(dev_t_, NVNdevice);

typedef struct {
    NVNdevice *dev; uint32_t flags;
    uint8_t set_cmd, set_comp, set_ctl, set_flush;
    uint32_t cmd_sz, comp_sz, ctl_sz, flush; void *mem; uint32_t mem_sz;
    int priority;
} queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits;
                 uint64_t cmd_bytes, ctl_bytes, comp_bytes;
                 uint32_t rast_flip; } queue_t;   /* rast_flip：建队列那一刻设备不是左上角原点（游戏自带驱动在 QueueInitialize 里据此写 Y_CONTROL 的 TriangleRastFlip，之后不再改；见 hle_nvn.c） */   /* *_bytes：本队列当前被游戏放进来的命令 / 控制 / 计算内存（Initialize 时记，Reset 清零） */
FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; int32_t nvmap; uint64_t gva; } pool_t;  FITS(pool_t, NVNmemoryPool);   // nvmap: the pool's nvmap handle, opened on first nvnTextureGetNvRmSurfaceNVX (0 = none yet); gva: 池的 GPU 地址（hle_nvn.c gva_alloc，0 = 没分到，用 CPU 地址）
uint64_t nvn_ga2ca(uint64_t a);   // 游戏拿到的 GPU 地址 → 池存储的 CPU 地址；窗口外原样返回（hle_nvn.c）
int nvn_ga_unmapped(uint64_t a);   // 换算后仍是 GPU 地址（换不成 CPU 地址、宿主上没映射）：完成回调不能往这里写（hle_nvn.c）
int nvn_gva_snapshot(uint64_t *out, int cap);   // GPU 地址表按 {GPU 起点, CPU 起点, 字节数} 三元组写出，返回段数（帧录放 nvn_frame_rec.inc 存 state.bin 用）
// 缓冲长度查询：CPU 地址所在活 nvnBuffer 的 {起点, 总长度}（1 = 命中）。storage 绑定只给地址、
// size=0 时用它求精确长度；查不到再退回池尾近似。
int nvn_buf_extent(uint64_t cpu_addr, uint64_t *base, uint64_t *size);
// 单点查询：CPU 地址所在池的 {起点, 字节数}（1 = 命中）；GPU 地址入口先做 ga2ca。
int nvn_gva_pool_of(uint64_t cpu_addr, uint64_t *cpu_base, uint64_t *bytes);
int nvn_gva_extent(uint64_t gva, uint64_t *cpu_base, uint64_t *bytes);
void nvn_gva_restore(const uint64_t *t, int n);  // 回放时按快照重建（替换现有表）

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void *pkg_data;                // TextureBuilderSetPackagedTextureData: copied into the storage at TextureInitialize (13 Sentinels)
    uint32_t gl_name;                    // TextureBuilderSetGLTextureName: GL 互操作名字（Metal 路径不消费，只保查询）
    uint8_t set_swz : 1, stencil : 1; uint16_t swz;   // TextureBuilderSetSwizzle: r | g << 3 | b << 6 | a << 9 (NVN and Metal channel enums have the same values: 0 zero 1 one 2 R 3 G 4 B 5 A);
                                         // stencil: TextureBuilderSetDepthStencilMode STENCIL (the texture reads the stencil of its depth-stencil storage, as a stencil-mode view does)
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
// Field order packed to 0x80: newer SDKs (SMBW, NVN 55) allocate NVNtexture as 0x80 bytes (allocated from main 0x978c3c, initialized at 0x978cd4);
// the former 0x88 wrote past the object's end and corrupted the adjacent heap block's free list. has_layout sits in the alignment gap after d, layout and serial fill up to 0x80.
typedef struct {
    // set_swz / stencil / swz (as in tex_builder_t) take the alignment gap after d; the size is still 0x80
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, set_swz : 1, stencil : 1; uint16_t swz; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off; uint8_t layout[8];
    uint64_t serial;                     // Initialization serial: the replay layer uses it to find the MTLTexture in the host table (the game may copy the object by value, so object pointers cannot be stored)
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
_Static_assert(sizeof(tex_t) <= 0x80, "tex_t exceeds the newer SDKs' NVNtexture (0x80)");
// The newer SDK's (NVN 55, SMBW) NVNtextureView is only 16 bytes: the game keeps it in a 16-byte stack slot (main +0x5796c), and the former 28-byte record wrote out of bounds over the saved fp / lr,
//   so the function returned to 0x100000000 and crashed. Packed into 16 bytes: levels / layers / format / target + swizzle and depth-stencil mode (the last two only recorded for now)
typedef struct { uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, set_dsmode : 1, dsmode : 2;
                 uint8_t target, base_level, nlevels; uint16_t format, min_layer, nlayers; uint8_t swz[4]; uint16_t _rsv; } view_t;
FITS(view_t, NVNtextureView);
_Static_assert(sizeof(view_t) <= 16, "view_t exceeds the newer SDKs' NVNtextureView (16 bytes)");

// Sampler parameters (one copy shared by builder and object)
// The eight enums share one 32-bit word (every NVN value is < 16), so the 40 bytes fit after an 8-byte header in the 0x30-byte NVNsampler
typedef struct { uint32_t minf : 4, magf : 4, ws : 4, wt : 4, wr : 4, cmp_mode : 4, cmp_func : 4, reduction : 4; float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap; } samp_par;
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
// All state sits in bytes [8, 0x30): games deduplicate sampler pool entries by a CRC over the object from byte 8 (Super Mario Odyssey; with the filters and S / T wrap at
// bytes 4-7 samplers differing only in them collided, its cache kept reallocating and re-registering pool IDs that recorded draws still used: bloom read a stale LOD range, #100)
typedef struct { uint32_t magic, pad; samp_par p; } samp_t;                                   FITS(samp_t, NVNsampler);

// The per-stage shader keys are not kept in prog_t: the game's real NVNprogram may be smaller than the header's 0xC0 (growing prog_t corrupted the adjacent object in Mario); see prog_keys in hle_nvn.c
typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; } prog_t;     FITS(prog_t, NVNprogram);   // serial: table key for the replay layer's resolved result

// ext_ev / ext_val：来自别的队列的栅栏（nvnSyncInitializeFromFencedGLSync：GL 层的共享事件与提交序号）；NULL = 普通 NVN 栅栏
typedef struct { uint32_t magic, fenced; uint64_t fence; void *ext_ev; uint64_t ext_val; } sync_t;   FITS(sync_t, NVNsync);
// Publish fence then fenced. A plain store of fenced first lets another core observe "done" while fence is still 0;
// nvn_be_wait(0) returns immediately because the completed counter starts at 0. Release on fenced pairs with the acquire in sync_published.
static inline void sync_publish_ext(sync_t *s, uint64_t fence, void *ev, uint64_t v) {
    __atomic_store_n(&s->fence, fence, __ATOMIC_RELAXED);
    __atomic_store_n(&s->ext_ev, ev, __ATOMIC_RELAXED); __atomic_store_n(&s->ext_val, v, __ATOMIC_RELAXED);
    __atomic_store_n(&s->fenced, 1u, __ATOMIC_RELEASE);
}
static inline void sync_publish(sync_t *s, uint64_t fence) { sync_publish_ext(s, fence, NULL, 0); }   // 重新围栏：别的队列的栅栏作废
// 已发布且是别的队列的栅栏：*ev / *v 给出要等的共享事件与值
static inline int sync_ext(const sync_t *s, void **ev, uint64_t *v) {
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE)) return 0;
    *ev = __atomic_load_n(&s->ext_ev, __ATOMIC_RELAXED); *v = __atomic_load_n(&s->ext_val, __ATOMIC_RELAXED); return *ev != NULL;
}
// 1 and *fence set once a publish is visible. fence may be NULL when the caller only needs the flag.
static inline int sync_published(const sync_t *s, uint64_t *fence) {
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE)) return 0;
    if (fence) *fence = __atomic_load_n(&s->fence, __ATOMIC_RELAXED);
    return 1;
}
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; const pool_t *pool; int64_t off; } ev_t; FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval; NVNtexture *tex[4]; int32_t nactive; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, nactive, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; int32_t shown, nat, present_ll; } win_t;   // ticket[i]: ticket of the most recent present of image i; shown: image of the most recent present + 1 (0 = none yet; on the device it stays on screen until the next present flips, so it is not returned)；nat：原生窗口交换间隔同步开着（hle_nvn.c nat_itv）
FITS(win_t, NVNwindow);

// Command buffer: records are written into host blocks of each recording (hle_nvn.c, command buffer), handle = first record; a device cursor walks the command memory the game gives
// (AddCommandMemory) as the real driver would fill it, and when it runs out, the game is called back for more memory
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur;                        // Host write pointer in the open recording's last block
    uint64_t nvn_flag10;
                                         // +0x10 is left to the game: its inlined NVN header code writes byte 1 here after AddCommandMemory and before BeginRecording / CallCommands (an internal flag of real NVN; main+0x4da5c, main+0xb8dec etc.).
                                         // This used to be the low byte of end: rewritten to 01 it cut the segment end by up to 254 bytes, and SMBW's material setup handles (512-byte blocks at minimum) kept overflowing into host segments at the iPhone's addresses
    uint8_t *end;                        // End of the open recording's last host block
    uint8_t *begin;                      // First record of this recording (its handle); recording 2 = not written yet
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
    uint64_t used;                       // Device bytes since the game last added command memory, across recordings (returned by GetCommandMemoryUsed)
    int32_t tex_base, samp_base;         // Host-table bases of the texture / sampler pool this command buffer last bound (hle_nvn.c dpool_add; 0 = the first pool of its kind)
    uint32_t copy_row_stride, copy_image_stride; // CPU state, preserved across recordings and captured by each buffer/texture copy; 0 = tightly packed
    uint64_t ctl_size;                   // 游戏最近一次 AddControlMemory 给的控制内存大小（主机不用控制内存，只供 GetControlMemorySize / Free 读回）
    uintptr_t gstart, gcur, gend;        // Device cursor in the current window: the memory the game last added, or a host-lent window (gvirt: offsets from 0, none of the game's memory)
    uintptr_t vanc;                      // The last byte of the game's memory before a host-lent window, which a recording there hangs off (0 = none)
    struct srec *rec;                    // The open recording's host blocks and device intervals (NULL outside BeginRecording .. EndRecording)
    uint32_t hint, gvirt;                // hint: host bytes the previous recording took, the size of the next recording's first block (0 = none yet)
} cmd_t;                                                                                   FITS(cmd_t, NVNcommandBuffer);

// ---------- State objects (defaults follow the NVN / GL defaults) ----------
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3,
                        adv_mode : 6, adv_overlap : 2, adv_premul : 1, adv_normdst : 1; } blend_st;   // adv_*：NVN 高级混合状态，渲染器暂不模拟，只保状态与查询（见 hle_nvn.c）
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
// RGBX8 … RGBX32I (0x6D-0x78) as a vertex attribute format: X is padding that does not take part, so the attribute is the RGB format of the same component size and type
// (RGB8 … RGB32I, 0x19-0x24, same order): three components read at the attribute's offset, the fourth filled as for RGB (1). The element's fourth component only takes bytes in
// the stream's stride, which the game sets itself, so no later attribute moves. The Metal replay (nvn_mtl.m state_op) and the Android recorder (vk_record.c) take the bound
// formats through this; before it they had no vertex format and the attribute read the zero buffer (缺口族 43)
static inline uint32_t nvn_vattr_fmt_canon(uint32_t f) { return f >= 0x6D && f <= 0x78 ? f - (0x6D - 0x19) : f; }
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);

// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,                                             // OP_BEGIN is no longer recorded (hle_nvn.c BeginRecording)
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_CLEAR_BUF, OP_DEPTH_CLAMP, OP_RENDER_COND, OP_FENCE_SYNC, OP_TF_BIND, OP_TF_STATE, OP_RAST_DISCARD, OP_VP_SWIZZLE, OP_BIND_TEX1, OP_PRIM_RESTART, OP_SET_PATCH_SIZE, OP_DRAW_TEX, OP_WAIT_SYNC, OP_ALPHA_REF, OP_COUNT_
};
// Payload (immediately follows op_hdr)
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
// SetRenderTargets 的记录按实际内容编码（11-01，Mario Tennis Aces）：定长 p_set_rt（8 个颜色槽）+ n+1 份快照每条 504 字节，比真机 NVN 大 5–10 倍，
// 每帧 1 MB 命令内存被它和视口用满后回调游戏，游戏那块缓冲没注册溢出处理 → 跳空指针。紧凑记录（payload，紧跟 op_hdr，全部 8 字节对齐）：
//   p_set_rt_c 头 8B → 指针 8B × k（非空颜色按槽序，再深度；k = popcount(cmask) + 有深度）→ view_t 16B × v（有视图的颜色按槽序，再深度视图）→ tex_t 快照 128B × k（第 i 个指针的快照）
// 指针照旧指向记录内第 i 份快照，录制时对象无效则指向对象本身（tex_snap）。读端一律经 set_rt_rec 还原成 p_set_rt 再用
typedef struct { uint8_t n, cmask, vmask, dflags; uint32_t pad; } p_set_rt_c;   // n 颜色个数；cmask 第 i 位 = 颜色 i 非空；vmask 第 i 位 = 颜色 i 有视图；dflags 见下
enum { SET_RT_DEPTH = 1, SET_RT_DVIEW = 2 };
_Static_assert(sizeof(p_set_rt_c) == 8 && sizeof(view_t) == 16 && sizeof(tex_t) % 8 == 0, "SET_RT 紧凑记录各段须 8 字节对齐");
static inline uint32_t set_rt_ntex(const p_set_rt_c *h) { return (uint32_t)__builtin_popcount(h->cmask) + (h->dflags & SET_RT_DEPTH ? 1u : 0u); }
static inline uint32_t set_rt_nview(const p_set_rt_c *h) { return (uint32_t)__builtin_popcount(h->vmask) + (h->dflags & SET_RT_DVIEW ? 1u : 0u); }
static inline size_t set_rt_snap_off(const p_set_rt_c *h) { return sizeof *h + 8 * (size_t)set_rt_ntex(h) + sizeof(view_t) * (size_t)set_rt_nview(h); }
static inline size_t set_rt_payload(const p_set_rt_c *h) { return set_rt_snap_off(h) + sizeof(tex_t) * (size_t)set_rt_ntex(h); }
static inline p_set_rt_c set_rt_hdr(const p_set_rt *r) {   // 完整结构 → 紧凑头（n 超 8 照录，读端按 n 判非法，掩码只看前 8 槽）
    int n = r->n < 0 ? 0 : r->n; p_set_rt_c h = {.n = (uint8_t)n};
    for (int i = 0; i < n && i < 8; i++) { if (r->color[i]) h.cmask |= (uint8_t)(1u << i); if (r->has_cview[i]) h.vmask |= (uint8_t)(1u << i); }
    h.dflags = (uint8_t)((r->depth ? SET_RT_DEPTH : 0) | (r->has_dview ? SET_RT_DVIEW : 0));
    return h;
}
static inline size_t set_rt_size(const p_set_rt *r) { p_set_rt_c h = set_rt_hdr(r); return set_rt_payload(&h); }   // payload 字节数（不含 op_hdr）
// 写出头、指针与视图（快照区不动），返回头；写入端随后逐个快照并改写指针，手工拼记录的单测直接留对象指针
static inline p_set_rt_c *set_rt_put(void *a, const p_set_rt *r) {
    p_set_rt_c *h = (p_set_rt_c *)a; *h = set_rt_hdr(r);
    const tex_t **pt = (const tex_t **)(h + 1); view_t *pv = (view_t *)(pt + set_rt_ntex(h));
    for (int i = 0; i < h->n && i < 8; i++) { if (h->cmask >> i & 1) *pt++ = r->color[i]; if (h->vmask >> i & 1) *pv++ = r->cview[i]; }
    if (r->depth) *pt = r->depth;
    if (r->has_dview) *pv = r->dview;
    return h;
}
// 紧凑记录 → 完整 p_set_rt（未用槽为 0，同原定长记录）；size 为 op_hdr.size，记录短于头算出的长度时按无目标还原
static inline const p_set_rt *set_rt_rec(const void *a, uint32_t size, p_set_rt *t) {
    const p_set_rt_c *h = (const p_set_rt_c *)a; memset(t, 0, sizeof *t);
    if (size < 8 + sizeof *h || size - 8 < set_rt_payload(h)) return t;
    t->n = h->n;
    const tex_t *const *pt = (const tex_t *const *)(h + 1); const view_t *pv = (const view_t *)(pt + set_rt_ntex(h));
    for (int i = 0; i < h->n && i < 8; i++) { if (h->cmask >> i & 1) t->color[i] = *pt++; if (h->vmask >> i & 1) { t->cview[i] = *pv++; t->has_cview[i] = 1; } }
    if (h->dflags & SET_RT_DEPTH) t->depth = *pt;
    if (h->dflags & SET_RT_DVIEW) { t->dview = *pv; t->has_dview = 1; }
    return t;
}
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
typedef struct { int32_t mode, pad; uint64_t addr; } p_render_cond;   // mode 0 / 1: SetRenderEnableConditional (render if the 64-bit counters at addr and addr + 16 are equal / not equal), 2 / 3: SetRenderEnable(true / false)
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { float n, f; } p_depth_range;
typedef struct { uint32_t on, index; } p_prim_restart;
typedef struct { float ref; int32_t pad; } p_alpha_ref;   // SetAlphaRef: the alpha test's reference value (fragment slot 30 at draws whose pipeline has an alpha test, nvn_mtl.m alpha_ref_bind)
typedef struct { int32_t size; } p_set_patch_size;   // SetPatchSize: control points per patch (for primitive type 14 = patches)
typedef struct { int32_t on; } p_depth_clamp;   // SetDepthClamp: depth outside [near, far] is clamped instead of clipped (Metal's depth clip mode)
typedef struct { int32_t first, n; uint8_t sw[16][4]; } p_vp_swizzle;   // SetViewportSwizzles: per viewport the source of clip x, y, z, w (NVNviewportSwizzle) XOR the identity (0, 2, 4, 6), so 0 = unchanged
// snap / keys (S2Arm): the program and its per-stage keys as they were at bind time. NVN encodes the shaders into the command buffer when it is recorded, so a game may finalize or
// reuse the program before submitting; the replay reads prog only while it is still that program (magic and serial), the snapshot otherwise (nvn_mtl.m prog_live)
// stage_of[i]: sd[i] 的 NVN 阶段序号（0 vs / 1 fs / 2 gs / 3 tcs / 4 tes / 5 cs / 0xff 认不出），
// 在绑定当时从 code/control 读定。NVN 按阶段绑定（BindProgram(program, stage bits)），回放要把
// 各阶段最近一次绑定的 key 合起来才是这次 draw 的有效程序；只记本次调用的掩码会丢掉先绑的阶段。
// p_bind_program 是回放读的完整样子；命令流里只有 p_bind_program_id，读时一律经 bind_program_rec 还原。
typedef struct { const prog_t *prog; int32_t stages; uint8_t kok; uint8_t keys[6][8]; prog_t snap; uint8_t stage_of[6]; } p_bind_program;
// OP_BIND_PROGRAM 的载荷（S2Arm 11-01）：程序身份 = 游戏的程序对象 + 绑定当时的序号 + 阶段位，24 字节、整条 32 字节（旧记录带快照与键，载荷 184 字节、整条 192 字节）。
// 真机 NVN 绑定时只把着色器地址编进命令流；游戏按真机体积给命令内存，旧记录 5–10 倍大，勇者斗恶龙 XI S 开局 8 秒内存回调里 UE4 断言「PSO CommandMemory OOM」。
// 同一序号的程序内容不再变（SetShaders 换新序号），snap / keys / kok / stage_of 按序号存在宿主表（hle_nvn.c prog_snap_put，只增不删）
typedef struct { const prog_t *prog; uint64_t serial; int32_t stages, pad; } p_bind_program_id;
// 宿主按序号的程序快照表（hle_nvn.c）：返回的快照指针长期有效；没有该序号返回空。安卓直连核心可能不带 hle_nvn.c 链接，故弱引用
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) __attribute__((weak));
uint64_t nvn_prog_set_ns(uint64_t serial) __attribute__((weak));   // 首遇等待诊断（#1001，hle_nvn.c）：这个序号 SetShaders 的时刻（now_ns）；没开 DM_NVN_FU_SETAT / 没记到 = 0
// 还原完整记录：prog / stages 取自记录，snap / keys / kok / stage_of 按序号查表；表里没有（或载荷长度不对）时快照只带序号、键全缺、阶段 0xff
static inline const p_bind_program *bind_program_rec(const void *a, uint32_t size, p_bind_program *t) {
    memset(t, 0, sizeof *t); memset(t->stage_of, 0xff, sizeof t->stage_of);
    if (size - 8 != sizeof(p_bind_program_id)) return t;   // 不是本格式：stages 0，什么也不绑
    const p_bind_program_id *r = (const p_bind_program_id *)a;
    t->prog = r->prog; t->stages = r->stages; t->snap.serial = r->serial;
    const prog_t *s = nvn_prog_snap_get ? nvn_prog_snap_get(r->serial, t->keys, &t->kok, t->stage_of) : NULL;
    if (s) t->snap = *s;
    return t;
}
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind: 0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t first; uint32_t size; uint64_t addr; } p_bind_vbuf_single;
// One binding with a 32-bit byte count needs no array count or high size word.
// Other counts and larger ranges keep the original record, including 64-bit sizes.
static inline const p_bind_vbuf *bind_vbuf_rec(const void *a, uint32_t size, p_bind_vbuf *t) {
    if (size - 8 != sizeof(p_bind_vbuf_single)) return (const p_bind_vbuf *)a;
    const p_bind_vbuf_single *s = (const p_bind_vbuf_single *)a;
    *t = (p_bind_vbuf){.first = s->first, .n = 1, .r = {{s->addr, s->size}}};
    return t;
}
#define BIND_BUF_DEFER 0x100   // p_bind_buf.stage 的标记位：Bind*BuffersDeferred，addr = 游戏的 NVNbufferRange 地址，回放进状态时再读（nvn_mtl.m state_op）
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
// defer / src: BindTexturesDeferred reads src at replay via bh_fresh (nvn_mtl.m).
// Only deferred records append p_bind_pool_bases after h[n]; ordinary binds keep their existing size and layout.
typedef struct { int32_t stage, first, n, defer; uint64_t src; uint64_t h[32]; } p_bind_handles; // Separate texture / sampler handles
typedef struct { int32_t tbase, sbase; } p_bind_pool_bases;
// nvnCommandBufferBindTexture (one combined handle). An engine that patches its recorded commands in place writes a new handle into the device command: Luigi's Mansion 3 takes the
// command memory base + GetCommandMemoryUsed before each such bind (main+0x7c8964) and stores a handle there + 0x10 every frame (main+0xb7d0d4). The host keeps that word in the
// game's command memory (hle_nvn.c rec_bind_tex1): slot carries BIND_TEX1_AT and a uint64_t address of the word follows the struct; the handle is read there at replay.
// h is the handle as the game gives it (32 bits: texture ID | sampler ID << 20, all a patch writes); tbase / sbase are the numbering bases of the descriptor pools the command buffer
// had bound (hle_nvn.c bind_handles), added when it is read. Read it with bind_rec, which gives a p_bind_handles
#define BIND_TEX1_AT 0x10000
typedef struct { int32_t stage, slot; uint64_t h; int32_t tbase, sbase; } p_bind_tex1;
static inline const p_bind_handles *bind_rec(uint32_t op, const void *a, p_bind_handles *t) {
    if (op != OP_BIND_TEX1) return (const p_bind_handles *)a;
    const p_bind_tex1 *b = (const p_bind_tex1 *)a; uint64_t hh = b->h;
    if (b->slot & BIND_TEX1_AT) { uint64_t g; memcpy(&g, b + 1, 8); memcpy(&hh, (const void *)(uintptr_t)g, 8); }
    uint64_t tid = hh & 0xfffff, sid = (uint32_t)hh >> 20;
    t->stage = b->stage; t->first = b->slot & ~BIND_TEX1_AT; t->n = 1; t->defer = 0; t->src = 0;
    t->h[0] = (tid + (uint64_t)b->tbase) | (sid ? sid + (uint64_t)b->sbase : 0) << 20; return t;   // texture ID 0 is the pool's first entry, as any other (11-02 Story of Seasons: its depth buffer at ID 0)
}
// Texture slots of a stage: 0-127 nvnCommandBufferBindTexture's (combined handles), 128-255 the separate texture binds. The driver keeps the two in tables of their own (c2 0x20 + 8 r and
// 0x168 + 8 r, dm/gfx/tools/dmsh_build.py), which the shaders' reflection names as these slots, so a game may bind both kinds in the same frame (Monster Hunter Rise: BindTexture slot 0 beside separate slots 0-4)
#define NVN_SEPTEX_SLOT0 128
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
typedef struct { int32_t prim, first, count, instances, base_instance; uint64_t ind; uint64_t mdi_cnt; int32_t mdi_i, pad; uint64_t tfc; } p_draw;   // tfc != 0: DrawTransformFeedback, the vertex count is the bytes captured (the game's counter address, nvn_geometry.inc tfd_args)   // instances 0 counts as 1; ind != 0: DrawArraysIndirect, the arguments are read by the GPU from that address; mdi_cnt != 0: draw mdi_i of an indirect-count multi-draw (drawn only while mdi_i < the u32 at mdi_cnt)
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf, ind; uint64_t mdi_cnt; int32_t mdi_i, pad; } p_draw_idx;   // ind != 0: DrawElementsIndirect, {count, instances, first index, base vertex, base instance} read by the GPU from that address; mdi_cnt: as in p_draw
typedef struct { int32_t prim, itype, count, base_vertex; uint64_t ibuf; } p_draw_idx_single;
// A single-instance indexed draw omits the implicit instance count (1) and base
// instance (0). Replay reconstructs them; other draws retain their full arguments.
// Other short draw records stop before ind; draw_rec / draw_idx_rec zero their
// missing indirect fields. Always decode through these helpers before replay.
// Games size command memory by what nvnCommandBufferGetCommandMemoryUsed reports, so a plain draw must not take more of it than the real driver's commands do. Breath of the Wild
// records each mesh's draw into a scratch command buffer and reserves that many bytes per mesh for the model's prerecorded draws, in the heap its resource size table sizes:
// at 0x50 bytes per recorded draw (0x30 now, with its END) three models needed one 4 KB page more than their table entry, and loading them crashed (main+0x155a7a4)
static inline const p_draw *draw_rec(const void *a, uint32_t size, p_draw *t) { if (size - 8 >= sizeof *t) return (const p_draw *)a; memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
static inline const p_draw_idx *draw_idx_rec(const void *a, uint32_t size, p_draw_idx *t) {
    if (size - 8 >= sizeof *t) return (const p_draw_idx *)a;
    if (size - 8 == sizeof(p_draw_idx_single)) {
        const p_draw_idx_single *s = (const p_draw_idx_single *)a;
        *t = (p_draw_idx){.prim = s->prim, .itype = s->itype, .count = s->count, .base_vertex = s->base_vertex, .instances = 1, .ibuf = s->ibuf};
    } else { memset(t, 0, sizeof *t); memcpy(t, a, size - 8); }
    return t;
}
typedef struct { int32_t x, y, z; uint64_t caller, ind; } p_dispatch;
// DrawTexture (S2Arm 10-30, Bare Knuckle IV): a textured rectangle into the bound targets with the current blend state. handle = texture ID | sampler ID << 20; dst = window
//   coordinates x0 y0 x1 y1, src = texel coordinates x0 y0 x1 y1 (NVNdrawTextureRegion; Ryujinx DrawManager.DrawTexture gives the same fields: dst origin + size, src origin + texels per pixel)
typedef struct { uint64_t handle; float dst[4], src[4]; } p_draw_tex;   // caller: return address of the game call site (to locate the dispatcher); ind != 0: DispatchComputeIndirect, the group counts are read by the GPU from that address
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; uint32_t row_stride, image_stride; } p_copy_b2t; // B2T / T2B: strides at recording time
// A host-only bit in flags: DM_COPY_RESOLVE = this is a Downsample (multisample resolve), recorded as a texture copy so everything else treats it as one (reads the source, writes the destination)
#define DM_COPY_RESOLVE 0x40000000
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // Reused for T2B: src stores the destination buffer address
enum { CLEAR_TEX_FLOAT, CLEAR_TEX_UINT, CLEAR_TEX_INT };
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, value_type; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; int32_t i[4]; } c; } p_clear_tex;
typedef struct { uint64_t addr, size; uint32_t value, _pad; } p_clear_buf;   // buffer filled with a 32-bit value (ClearBuffer)
typedef struct { float rgba[4]; } p_blend_color;
typedef struct { int32_t index, pad; uint64_t addr, size; } p_tf_bind;   // BindTransformFeedbackBuffer(s): one record per buffer (index 0-3)
typedef struct { int32_t what, pad; uint64_t ctl; } p_tf_state;          // what: 0 begin, 1 end, 2 pause, 3 resume; ctl = the game's 32-byte control buffer
typedef struct { int32_t on; } p_rast_discard;

// Entry point of the Metal replay layer (nvn_mtl.m): replays a set of command handles into the current submission
void nvn_be_exec(const uint8_t *handle);
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value);   // in-order buffer fill within the submission (OP_CLEAR_BUF)
void nvn_be_exec_rest(const uint64_t *handles, int n);   // Tells which handles in this submission still follow the current handle (used by the pass lookahead across handles)
// Present: draw the swapchain texture to the front-end CAMetalLayer (only counts when there is no window)
void nvn_be_present_prepare(const tex_t *t);   // Under the front end's submit lock: initialize a texture first used by present before handing it to the present thread
void nvn_be_present_target(int interval, uint64_t set_ns);   // Before each present: the present interval the game asks for right now and when it set it (now_ns); logging only (per-target segments in the 5-second timing lines), never changes pacing
uint64_t nvn_be_present(const tex_t *t, double period_s, const int crop[4]);   // Enqueue a present and return the ticket immediately; period_s: minimum time this image is displayed (= present period, 0 = unlimited)
void nvn_be_present_out_size(int *w, int *h);   // Last presented source crop size in storage pixels (for HUD, matching gal_present.m nxgal_out_size)
void nvn_be_present_wait(uint64_t ticket);                    // Wait until the present with this ticket has been processed (blit committed to the GPU queue)
void mtl_present_flush(void);                              // Wait until all enqueued presents have been processed
void mtl_present_test_stall(uint64_t ns);                  // Unit test: extra pause in ns before each present on the present thread
int nvn_be_display_hz(void);                                   // Maximum refresh rate of the screen (Hz) when there is a window, 0 without a window
void nvn_be_set_tex_pool_n(int n);                            // Texture descriptor pool capacity: texture IDs outside the pool are treated as 'empty slots' (read 0)
// Timing instrumentation (shared by Mac / iOS): submissions are tagged with the frame number; GPU time = sum of GPUEnd−GPUStart over all of that frame's command buffers, readable only after all complete (otherwise −1)
void nvn_be_set_frame(uint64_t f);
void nvn_be_ub_present(double iv_ms);   // Same-process alternating measurement (DM_NVN_UB): the present thread reports the actual present interval every frame
double nvn_be_gpu_ms(uint64_t f);
void nvn_be_sub_index(const uint64_t *h, int n);   // Start of submission: build the store lookahead index in replay order (nvn_mtl.m)
void xv_test_counts(int *dc, int *viol);
int nvn_be_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);   // The frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns); returns 0 if not all complete or there is no GPU time
void nvn_be_stats(uint64_t *passes, uint64_t *draws);
void nvn_be_args_stats(uint64_t *enc, uint64_t *reuse);
void nvn_be_use_stats(uint64_t *call, uint64_t *skip);
uint64_t nvn_be_ds_omit(void);   // Cumulative number of passes with no depth attachment because the lookahead proved depth-stencil unused
uint64_t nvn_be_drawable_ns_take(void);   // Take and zero: cumulative ns spent waiting for nextDrawable in present   // Cumulative useResource calls actually issued / skipped by same-encoder dedup   // Cumulative argument buffer new-encode / reuse counts
void nvn_be_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);   // Cumulative attachment load / store bytes, cumulative number of full load-store passes
const char *nvn_diag_path(const char *p, char *buf, size_t n);   // Diagnostic output file path: on iOS a relative path goes under $HOME/Documents (the process cwd is not writable; the run executor pulls from Documents), directories created
uint64_t nvn_be_udc_saved_bytes(void);                                      // Write-back bytes actually saved by DM_NVN_UNWRITTEN_DC
int nvn_be_has_window(void);
int nvn_be_fps_target(void);                                   // target frame rate 30 / 60 (power-saving mode, DM_FPS, the game itself asking for 30)
int mtl_warm_wait(int timeout_ms);
int nvn_be_link_active(void);
int nvn_be_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // >0：游戏的60Hz交换间隔；0：一个实际显示链接拍（无原生锁且用户上限120）
double nvn_be_presented_time(uint64_t n);
   // Actual presentation time (seconds) of the n-th present (counted from 1); 0 = not ready, −1 = dropped
// Texture release (at Finalize)
void nvn_be_tex_init(const tex_t *t);
void nvn_be_tex_counters(uint64_t *init, uint64_t *rel);   // DM_NVN_ALLOCPROF: cumulative texture inits / releases
void nvn_be_tex_tick(uint64_t frame);                      // DM_NVN_ALLOCPROF: one summary line every 300 frames      // TextureInitialize: the serial belongs to a live texture object until nvn_be_tex_release
void nvn_be_tex_release(tex_t *t);
void nvn_be_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
uint64_t nvn_be_pkg_miss(void);
uint64_t nvn_be_pkg_same(void);
void nvn_be_pkg_gpu_stats(uint64_t *writes, uint64_t *kept, uint64_t *partial);
uint64_t nvn_be_upload_ns_take(void);   // Take and zero: time spent uploading packaged textures on the submit thread since last time   // DM_NVN_DIRTYCHECK diagnostic: number of missed detections   // Packaged textures: number of first uploads, number of re-uploads due to pool content changes
uint64_t nvn_be_upload_gpu_ns_take(void);   // Take and zero: GPU execution time of upload command buffers (sum of GPUEndTime − GPUStartTime)
uint64_t rp_sync_pipelines_created(void);   // 新回放层后端：前台成功创建的图形和计算管线，进程累计
// 进程累计。Metal 数提交时现场加载的库，新回放层数提交时取用的现编程序（含 worker 交付）；图形 + 计算管线只数前台现场创建。
void nvn_be_create_stats(uint64_t *lib, uint64_t *pso);
// CPU texture read: starting at level / layer0, written into dst with row / img byte strides
void nvn_be_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
// Descriptor pool registration (texture ID / sampler ID → object snapshot), program release
void nvn_be_reg_tex(int id, const tex_t *t, const view_t *v);
void nvn_be_unreg_tex_range(int base, int n);   // 贴图池销毁时清它那段编号的登记（hle_nvn.c dpool_finalize）
// textures written by WriteTexels (recorded by serial): the replay layer treats them like packaged textures, uploaded from guest memory and re-uploaded on dirty pages
void nvn_be_tex_cpu_written(const tex_t *t);
int nvn_be_tex_gpu_owned(uint64_t serial);                                  // its host texture is a render target or shader-writable: GPU content, never re-uploaded from guest memory
// A program's per-stage shader key (hle_nvn.c): the first 8 bytes of sha256(first 0x880 bytes of the control section (its own length when shorter; two volatile spans zeroed, nvn_key.h) + code area);
//   an old package keyed without the zeroing gets its old key (keys_hash). Computed at SetShaders (NVN lets the caller free the control section once it returns, and Unity does);
// a stage whose code area was not yet in any memory pool then is computed the first time it is asked for. Returns 0 = no such stage
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]);
void nvn_be_reg_samp(int id, const samp_par *p);
void nvn_be_prog_release(prog_t *p);
void nvn_be_shader_prefetch(const uint8_t key[8]);   // S2Arm: load a stage's shader library in the background (ProgramSetShaders), off the frame path
void nvn_be_pair_prefetch(const uint8_t (*keys)[8], int n);   // S2Arm: look up the program's vertex / fragment pair's package pulling base in the background (ProgramSetShaders)
// New serial (> 0, globally increasing)
uint64_t nvn_new_serial(void);
void nvn_serial_floor(uint64_t floor);   // 新序号从 floor 之后编（帧回放：避开录像里的序号）
