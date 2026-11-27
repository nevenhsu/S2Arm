// nvn_gpu_* 的 Vulkan 后端（W1 D8 第一段：接管已有设备 → 缓冲 → 写 / 填 / 拷 → 提交 / 等 → 读回 → release / abort / close）。
//
// 范围（本段只做契约用例 C01–C08 需要的原语；其余函数按契约明确返回 E_UNSUPPORTED，不静默空实现）：
//   · 设备：nvn_gpu_open 接管平台已建好的 struct s2a_vk_device（不另建、不拥有其寿命）；
//   · 缓冲：create / cpu / va / write / fill / copy / arena / use / release(retired)；
//   · 提交：cmd_begin / commit / abort / fence_done / fence_failed / fence_wait；
//   · 失败终止（A4）：第一个失败段的 signal = F，先发布 F 再以 FAILED 回调；F 之后不再执行；
//   · 发布顺序：GPU 完成 → done（核心回写）→ release 语义推进已完成值；fence_wait 以 acquire 语义读。
// 本段不做：贴图 / 视图 / 采样器 / 着色器 / 管线 / 通道 / 上屏（清屏、三角形、深度用例随后接，届时按同一契约补）。
//
// 诊断开关：nvk_diag_draw_mode（见 nvn_gpu_draw 上方），只包 vkCmdDraw* 八处，缺省 0 时行为不变。
//
// 线程模型：[提交] 线程录制并提交；后端自己的 worker 线程等栅栏、调 done / retired（[回调]）。
// 对象状态（refs / released）都由 g_mu 保护；回调一律在锁外调用。
//
// 已知与契约 v2 的接点（消费审查，见 docs/设计_回放核心共用.md / 工单 #784 评论）：
//   1. nvn_gpu_binding 的 union 只能放 view 或 samp，表达不了 TEX_SAMP 组合（安卓现有 COMBINED_IMAGE_SAMPLER）；
//   2. done.signal 的「0 = 不推进」与 commit 的「signal 严格递增」冲突；本实现按后者：signal = 0 直接 E_ARG，
//      因为失败终止用 F = 0 表示「没有失败」，无信号段一旦失败无法表达。等契约属主定稿后同批改。

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "nvn_backend.h"
#include "nvn_vk_cold_guard.h"
#include "nvn_refl.h"
#include "../../../graphics/nvn/gfx/tex/nvn_deswizzle.h"
#include "backend_vulkan_deswizzle_spv.h"
#include "backend_vulkan_bc1_astc_spv.h"
#include "nvn_unimpl.h"
#include "nvn_vk_report_ts.h"
#include "nvn_vk_land_auto.h"
#include <unistd.h>
#include "nvn_plat.h"   // 调试开关与一行日志（上屏段的调试色、GPU 时间读数）
#include "nvn_vk_format.h"
#include "nvn_pipe_flight.h"   // 配方级在途表（纯逻辑）：同配方合并成一份创建，不同配方不互相排队
#include "nvn_vk_texview.h"
#include "nvn_vk_bcdec.h"
#include "nvn_vk_pipe_once.h"     // 装缓存的双检去重（纯逻辑，宿单测用假创建驱动）
#include "nvn_vk_pipe_demand.h"
#include "nvn_vk_pipe_warm.h"     // 预热清单：warm 标记 / 命中计数的口径
#include "nvn_vk_pipe_diag.h"     // 计算管线现建慢行的文本（带阶段键与出库标记）
#include "nvn_vk_driver_cache.h"   // 驱动管线缓存的落盘文件层（见「驱动管线缓存」一节）
#include "nvn_fmt_info.h"
#include "nvn_vk_border.h"   // 没有 VK_EXT_custom_border_color 时边框色就近取内置三档（纯逻辑，宿主单测）
#include "nvn_fmt_expand.h"
#include "vk_device.h"
#include "spirv_storage.h"   // 顶点阶段的存储写检查（与直连接纳同一份，按 Vulkan 规范的 NonWritable 判）
#include "spirv_workgroup.h" // 最终 SPIR-V 的实际计算工作组尺寸（含 WorkgroupSize 的优先级）
#include "spirv_write_bound.h"
#include "spirv_compliance.h"   // #1865 统一装载闸：能力 / 扩展 / 存储类字节声明扫描
#include "../capprobe/capprobe.h"
#include "direct/sha256.h"
_Static_assert(NVN_GPU_CONTRACT_VERSION == 26, "核对 Vulkan 后端与新版 NVN GPU 契约");
#include "nvn_vk_dq.inc"   // 两级回放：从这里往下 vkCmd* 等名字走它的包装（调试开关 nvk_dq，缺省开）
// 宿主后端夹具可能不链回放布局；缺来源时能力关闭并明确报原因。
// 弱链接[androiddraw]：nvn_deswizzle_geom
// 弱链接[androiddraw]：nvn_deswizzle
// 弱链接[androiddraw]：nvn_tex_layout
// 弱链接[androiddraw]：nvn_tex_level_offset
// 弱链接[androiddraw]：nvn_tex_level_gobs
// 弱链接[androiddraw]：nvn_gob_switch2
extern void nvn_tex_layout(const nvn_texdesc *, nvn_texlayout *) __attribute__((weak));
extern int nvn_deswizzle_geom(const nvn_texdesc *, unsigned, unsigned, uint32_t *, uint32_t *, uint32_t *, uint64_t *, unsigned *, unsigned *, uint32_t *) __attribute__((weak));
extern int nvn_deswizzle(const nvn_texdesc *, unsigned, unsigned, const void *, void *, size_t) __attribute__((weak));
extern uint64_t nvn_tex_level_offset(const nvn_texdesc *, unsigned) __attribute__((weak));
extern uint32_t nvn_tex_level_gobs(const nvn_texdesc *, unsigned) __attribute__((weak));
extern int nvn_gob_switch2 __attribute__((weak));
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkShaderModule module; VkPipeline pipe; } g_deswizzle;
static uint64_t g_deswizzle_count, g_deswizzle_bytes, g_deswizzle_fallbacks;
static uint64_t g_deswizzle_reasons[5];
static int deswizzle_open(void);
static void deswizzle_close(void);
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; } g_d24_readback;
static int d24_readback_open(void);
static void d24_readback_close(void);
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; } g_bc1_astc;
static void bc1_astc_close(void);
static int bc1_astc_storage(uint32_t nvn_format, const nvn_gpu_tex_desc *d, VkImageType type,
                            uint32_t layers, VkImageCreateFlags flags, VkFormat *format);

#define VK_SEG_QUEUE_INIT 16
#define VK_ARENA_MIN (256u * 1024u)
#define VK_WAIT_SLICE_NS (50ull * 1000ull * 1000ull)

enum { STAT_BUFS = 0, STAT_SUBMITS, STAT_DONE_OK, STAT_DONE_FAILED, STAT_RETIRED, STAT_WRITES, STAT_FILLS, STAT_COPIES,
       STAT_LAND_BYTES, STAT_LAND_DIRECT, STAT_LAND_INCOMPATIBLE,
       STAT_LAND_LIMIT, STAT_LAND_BUSY, STAT_LAND_ALLOC, STAT_LAND_INVALIDATE, STAT_N };
static const char *const stat_names[STAT_N] = {
    "bufs_created", "submits", "segments_done", "segments_failed", "bufs_retired", "buf_writes", "buf_fills", "buf_copies",
    "land_staged_bytes", "land_direct_spans", "land_incompatible", "land_limit", "land_busy", "land_alloc_failure", "land_invalidate_failure",
};

struct nvn_gpu_buf_s {
    struct cold_ledger *cold_ledger;
    uint64_t cold_generation;
    uint64_t access_id;   // 供共用层认存储生命周期，不以可复用的指针充当身份
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    uint64_t bytes;
    uint64_t va;
    int mem_kind;
    VkMemoryPropertyFlags memory_flags;   // 实际分配的类型，回写按这个判断直接读或暂存
    int released;
    int refs;
    nvn_gpu_retired_fn retired;
    void *retired_user;
    struct nvn_gpu_buf_s *all_next;
    struct nvn_gpu_buf_s *retire_next;   // 退休队列就地链接，队列本身不再分配
};

struct vk_land_slot { nvn_gpu_buf buf; int busy; };

struct nvn_gpu_tex_s {
    uint64_t access_id;
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize mem_size; uint32_t mem_type;   // 这块显存向驱动要的大小和内存类型（还回 texmem 池时用）
    VkImageView view;
    VkFormat format, sample_format;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
    VkImageLayout layout;
    uint32_t w, h, depth, levels, layers, samples, nvn_format, target;
    nvn_texdesc original_desc;   // 原客体排布（含 packaged 宽对齐），不能从宿主图像反推
    uint32_t original_usage;
    int array_compatible;
    int cube_compatible;   // P0-6：二维数组建图时带了 CUBE_COMPATIBLE（方形、≥6 层、单采样、设备收这个格式 / 用途 / 标志），可以建立方体（数组）视图
    int view_fmt, released, refs, bc_decoded;
    VkFormat fmt_list[3]; uint8_t fmt_n;   // 格式列表模式（保 Adreno UBWC，见 tex_fmt_allowed）：view_fmt = 0 时视图只能取这几个格式；fmt_n = 0 = 没走列表
    uint8_t ext_usage;   // 建图带了 EXTENDED_USAGE（存储用途配本身不能存储的格式，见 NVN_GPU_CAP_STORAGE_SRGB_ALIAS）：每个视图按自己的格式剥用途
    uint64_t bc_shadow_mem_size;   // 实际影子缓冲分配，诊断时在 g_mu 下发布
    VkBuffer bc_sbuf; VkDeviceMemory bc_smem;   // bc_decoded 贴图的压缩原数据影子（GPU 缓冲，按级、按层 / 3D 切片、行紧排；见 bc_shadow_layout）
    uint8_t fb_sample;   // 同一通道里既当附件又被采样过（反馈环）：这张图以后当附件一律 GENERAL（见「图像布局」）
    uint16_t swz;   // 存储自带的通道映射（tex_builder_t.swz 打包法，0 = 恒等）：视图没自己设映射时用它
    // 反馈快照图（feedback_make 建的私有图）跨段复用（见 backend_vulkan_feedback.inc 的快照池）：
    //   fb_snap = 是快照图、fbk 是建它时的整份建图请求（池的形状键）；fb_ready = 用它的段已执行完（栅栏到），图已离开 UNDEFINED、GPU 不再碰它。
    uint8_t fb_snap, fb_ready;
    struct fb_key { uint32_t target, nvn_format, w, h, depth, levels, samples, usage; } fbk;
    struct nvn_gpu_tex_s *all_next;
};

struct nvn_gpu_view_s {
    VkImageView view;
    struct nvn_gpu_tex_s *tex;
    VkBufferView buffer_view;
    struct nvn_gpu_buf_s *buf;
    uint64_t buffer_off, buffer_bytes;
    VkFormat buffer_format;
    VkFormatFeatureFlags buffer_features;
    uint32_t buffer_caps;
    uint32_t nvn_format;   // 视图的有效 NVN 格式（可从贴图覆盖）；clear 的 is_int 按它判
    VkFormat format;       // 视图自己的原生格式：当附件时渲染通道按它声明（视图换了格式，硬件就按视图的格式写，比如经 sRGB 视图写线性存储）
    uint32_t base_level, levels, base_layer, layers;
    int released, refs;
    int swizzled;   // 通道映射非恒等：只能采样，不能当附件
    int p16_remap;  // #975：16 位打包格式在本设备走退回格式，通道映射里合进了格式自带的分量换位：不能当附件 / 存储图像
    VkImageViewType type;
    VkImageAspectFlags aspect;   // 这个视图取的面（深度模板格式的采样视图只取一个面）
    VkComponentMapping components; // 反馈快照保持采样视图的通道映射
    uint32_t att_layer;          // 当附件时取的层 / 3D 深度切片（视图没设层范围 = 0）
    VkImageView att;             // 附件形态（一级一层的 2D 视图），第一次当附件时建（view_attachment）
    VkImageView att_n; uint32_t att_nl;   // 分层通道的附件形态（起始层起 att_nl 层的 2D 阵列视图，一级），第一次当分层附件时建
    VkImageView img;             // 图像形态（恒等通道映射），第一次当存储图像时建：存储图像视图必须恒等映射（view_image_form）
    VkImageView depth_blit[2];   // 辅助深度模板拷贝的恒等单级单层采样视图，随本视图保活与释放
    // 反查表：这个视图当过附件的帧缓冲（帧缓冲缓存槽位 + 代 + 句柄）。视图销毁时只拆这几项，
    // 不必在全局 2048 项里线性扫一遍（真机上一帧 30 个视图销毁 × 扫描全表 × 每通道开通道要拿同一把锁，开通道里光等锁就 10 微秒）。
    struct { uint16_t idx; uint32_t gen; VkFramebuffer fb; } fbs[6];
    uint8_t nfbs, fbs_overflow;   // 溢出（同一个视图用在 6 个以上帧缓冲里）：销毁时退回全表扫
    struct nvn_gpu_view_s *all_next;
    struct nvn_gpu_view_s **all_pprev;   // 指向「指着我的那个指针」（链头 g.views，或前一个视图的 all_next）：摘链 O(1)，见 view_unlink_locked
};

struct nvn_gpu_shader_s {
    VkShaderModule mod;
    int missing_storage_format_feature;
    const char *missing_vertex_feature;   // #1072：顶点阶段写存储 / 写裁剪、剔除距离要的特性设备没启用（特性名）；NULL = 不缺
    nvn_gpu_stage stage;
    uint8_t precompiled;
    struct s2a_write_bound *write_bounds;  // immutable, indexed by the shader layout entry
    uint8_t stage_key[8], spirv_digest[32];  // 库阶段键与实际交给 Vulkan 的模块摘要
    uint8_t src_key[8];          // 装载时调用方给的阶段键（现编 / 出库都有，只作日志；stage_key 只在出库件上填）
    nvn_gpu_layout_ent *ents;
    VkFormat *tfmt;              // 与 ents 平行：TBUF / IMAGE_TBUF 项的 texel 视图格式（自 blob 反射）；非 texel 项 UNDEFINED
    uint32_t nent;
    uint32_t push_off, push_bytes;
    char entry[32];
    int released, refs;
    struct nvn_gpu_shader_s *all_next;
};

struct nvn_gpu_pipe_s {
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkDescriptorSetLayout dsl[4];
    struct dset_lay *dlay[4];   // 各组布局在布局表里的表项（#1096：结构相同的布局全后端只建一个，管线只借用、不销毁；表项归布局表所有，会话收尾才销毁）；dsl[set] == dlay[set]->dsl
    uint32_t nset;
    nvn_gpu_layout_ent *stage_ents[6];
    int16_t *stage_dyn[6];   // 动态 UBO 偏移（#1233）：与 stage_ents 同下标，>= 0 = 这一项是动态 UBO、值是它在所属组里的动态序号起点（按 binding 升序累加个数）；-1 = 静态；整条管线不用动态时为 NULL
    uint8_t ndyn[4];         // 每组的动态描述符个数
    uint8_t dyn_all;         // 整条管线布局里动态描述符总数（≤ 设备 maxDescriptorSetUniformBuffersDynamic 且 ≤ 32）；0 = 这条管线的 UBO 都是静态的
    VkFormat *stage_tfmt[6];
    int8_t texel_binds;   // 有没有纹素缓冲类的绑定项：0 还没数，1 有，-1 没有（见 bind_descriptor_groups）
    uint32_t stage_nent[6];
    uint32_t stage_push_off[6], stage_push_bytes[6];
    nvn_gpu_pipe_desc key;
    uint32_t actual_samples; // 主机实际管线样本数；key 保留请求以匹配预热与查找。
    uint8_t warm;          // 1 = 这份对象是预热带 / 预热线程建的，真实 draw 第一次命中时计数（见 nvn_gpu_pipe_get）
    uint64_t warm_tag;     // 管线预热清单的标签（槽号 + 槽代号，见 nvn_vk_pipe_warm.h）；0 = 没设过。原子读写，随对象销毁
    struct nvn_gpu_pipe_s *all_next;
    nvn_gpu_shader cshader;
    uint8_t lib_stages;  // 出包预编阶段掩码
    struct nvn_gpu_pipe_s *cnext;
    struct nvn_gpu_pipe_s *hnext;   // 图形管线的散列桶链（g_pipe_ht）
    uint32_t hash;                  // 建管线时的 pipe_hash(描述)：查表先比它、相同才逐字段比（不碰 key 那一大块内存）；重排桶也用它
};

struct nvn_gpu_samp_s {
    VkSampler sampler;
    VkSampler average_sampler; // 分离绑定无法核对配对视图时，按需创建同参数的平均采样器
    VkSamplerCreateInfo average_info;
    VkSamplerCustomBorderColorCreateInfoEXT average_border;
    int average_custom;
    VkSampler int_sampler;   // 给整数格式的视图用：最近点、不做各向异性、整数边界色（整数格式不能线性过滤）；参数本来就合规时与 sampler 是同一个
    uint8_t reduction;       // #976：sampler 带 min / max 归约（NVN_SAMPLER_REDUCTION_MIN / MAX；0 = 平均）。int_sampler 是最近点，不带归约
    int released, refs;
    uint32_t border_slots;   // 占用的自定义边框色配额（sampler 与 int_sampler 各算一个；内置三档为 0）
    struct nvn_gpu_samp_s *all_next;
};

struct prs_use;     // 整池组回收的本段取用记录（见 backend_vulkan_pool.inc 的 prs_*）
struct nvn_gpu_cmd_s;
static void prs_release(struct nvn_gpu_cmd_s *c, int executed);   // 段结束：还原反馈补丁、把回收的整池组放回（finish_cmd 最先一步）
static void prs_open(void);
static void prs_close(void);
// Pool tables are immutable; each slot holds a resource reference, including dummies.
struct nvn_gpu_pool_table_s {
    uint32_t kind, count;
    int released, refs;
    uint64_t uid;               // 建表时取的序号（全进程单调增、不复用）：备忘 / 回收的键用它而不是指针——表销毁后地址会被新表复用，按指针会误命中旧组
    void **objects;
    VkDescriptorImageInfo *images;
    // 图像池的「按图索引」（建表时一次建好，表不可变）：同一张 VkImage 的视图（按视图指针去重）串成链。
    // 反馈检测只需看和本通道附件同图的那几项，不必每笔绘制逐项扫整表（整池 8448 项）。没有索引（ikey == NULL，宿主夹具手造的表）退回逐项扫。
    uint32_t nuniq, idx_cap;
    void **uviews;      // 去重后的视图，nuniq 项
    uint32_t *unext;    // uviews[j] 的下一项（同图），UINT32_MAX 结尾
    uint64_t *ikey;     // 开放寻址表：VkImage 句柄（0 = 空槽），idx_cap 项（2 的幂）
    uint32_t *ihead;    // 对应槽的链头（uviews 下标）
    struct nvn_gpu_pool_table_s *next;
};
static void pool_destroy_locked(struct nvn_gpu_pool_table_s *table);
// 按图索引的查找（feedback.inc 在 pool.inc 之前被包含，所以放在这里）：同一张图的第一项（uviews 下标），没有 = UINT32_MAX；表有索引（ikey != NULL）才能调。
static inline uint32_t pool_hash64(uint64_t x, uint32_t cap) { return (uint32_t)((x * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1); }
static inline uint32_t pool_img_first(const struct nvn_gpu_pool_table_s *t, uint64_t img) {
    uint32_t g = pool_hash64(img, t->idx_cap);
    while (t->ikey[g]) { if (t->ikey[g] == img) return t->ihead[g]; g = (g + 1) & (t->idx_cap - 1); }
    return UINT32_MAX;
}
struct vk_ref { void *obj; int type; };
enum { REF_BUF = 1, REF_TEX, REF_VIEW, REF_SAMP, REF_POOL };

struct vk_staging { struct nvn_gpu_buf_s *buf; };

struct pass_rec { VkRenderPass rp; VkFramebuffer fb; };
// 本段里一张图当前在哪：第一次碰到按 GENERAL（段首约定），之后按本段记的（见「图像布局」）
struct cmd_lay { struct nvn_gpu_tex_s *t; VkImageLayout cur; };
enum { DESC_MEMO_N = 2048 };
struct desc_memo { uint64_t h1, h2; VkDescriptorSetLayout dsl; VkDescriptorSet set; };
struct dset_bin;
struct dset_use { struct dset_bin *bin; VkDescriptorSet set; VkDescriptorPool pool; };   // 本段从按布局回收的表项里取的一组（见「描述符组按布局回收」）；bin = 取用时的表项，放回时直接放回它
struct bc_temp { VkImage image; VkDeviceMemory memory; struct nvn_vk_bcdec_rec rec; struct bc_temp *next; };   // rec：缓冲 → BC 贴图录进段里的解码资源（#697）
enum { TS_PASS_MAX = 250 };   // 调试读数：一段里逐个计时的通道数上限
enum { OCC_CHUNK = 256, OCC_QUERY_SPARE_MAX = 65536 };   // 遮挡查询的续段槽按块给：一块 = 一个 OCC_CHUNK 槽的查询池，用到哪块才要哪块；OCC_QUERY_SPARE_MAX = 一个声明槽名下最多几个续段槽（保险丝：防失控，不是预期用量）

// 冗余状态消除的计数下标（见 nvn_gpu_cmd_s.es）
enum { ES_PIPE, ES_VP, ES_SC, ES_BC, ES_ST, ES_DB, ES_VB, ES_IB, ES_PUSH, ES_KINDS };
struct vk_segment {
    struct vk_segment *next;
    struct nvn_gpu_cmd_s *cmd;
    uint64_t signal;
    nvn_gpu_done_fn done;
    void *user;
    VkFence fence;
    VkCommandBuffer cb;
    int failed;
    int submitted;
    int executed;
    int embedded;   // cmd owns this record; finish_cmd frees it with cmd
    int internal;   // 后端自己的段（呈现）：不占时间线上的值，完成后只回收
    uint64_t wsi_seq;   // 呈现段的序号（>0）：worker 等到它的栅栏后记进 g.wsi_done，取图复用信号量前按它等（见 backend_vulkan_wsi.inc）
    nvn_gpu_rc rc;
    char why[192];
};

struct feedback_copy {
    struct feedback_copy *next;
    struct nvn_gpu_view_s *source;
    VkImageView view;
    struct nvn_gpu_tex_s *snap, *snap_src; // 快照图（本段 REF_TEX 保活到 fence）与它照的源图；同段同源复用，见 feedback_make
};

enum { LOSS_PROGRAMS_PER_SEGMENT = 16 };

struct nvn_gpu_cmd_s {
    struct vk_segment segment; // reserve before recording: commit cannot lose done to allocation failure
    int land_mode, land_observe, land_tracked, land_epoch;
    uint32_t land_eligible, land_direct;
    uint64_t land_eligible_bytes;
    struct vk_land_slot *land_slot;
    VkDeviceSize land_bytes, land_invalidate_bytes;
    const struct nvn_gpu_pipe_s *loss_programs[LOSS_PROGRAMS_PER_SEGMENT], *loss_last_pipe;
    uint8_t loss_nprograms, loss_programs_truncated;
    VkCommandBuffer cb;
    struct bc_time_chunk *bc_first_time, *bc_time;
    struct bc_temp *bc_temps;
    struct clr_pipe_temp *clear_pipes;   // 清除缓存满后的段自有管线，完成 / 放弃时回收
    struct dtx_pipe_temp *dtx_pipes;     // DrawTexture 管线缓存满后的段自有管线（backend_vulkan_draw_tex.inc），同上回收
    struct feedback_copy *feedback, *feedback_stop; // 本笔 [feedback, stop)，历史项活到 fence
    int feedback_on;
    int feedback_read_on;
    int pool_fb_on;              // nvk_pool_fb：1 = 整池数组里和「本笔写着的附件」重叠的视图也拷快照（旧行为，A/B 与回退诊断用）；缺省 0 = 不拷（见 feedback_add）
    uint16_t feedback_read_mask; // 颜色 bit0..7、深度模板物理附件 bit8：本物理通道 LOAD / NONE
    int recording;
    int in_pass;
    // 上一道全屏障之后录过「缓冲作目标」的传输：传输只在开头带屏障，绘制有渲染通道开头的外部依赖兜着，
    // 计算派发没有，所以派发前按它补一道（开段置 1：前置段 / 上一段的传输写，段末那道屏障只对主机可见）
    int xfer_dirty;
    nvn_gpu_rc sticky;
    char why[192];
    struct vk_ref *refs;
    uint32_t nrefs, refcap;
    uint64_t *refseen;               // 本段已引用句柄的备忘（#1194）：直接映射，槽里 = 句柄指针 | 类型；第一次引用时懒分配，随段释放
    uint64_t refseen_epoch;          // 备忘建表（或上次清空）时的 g_rel_epoch；与现在不一致就整表作废
    uint32_t refseen_hits;           // 本段备忘命中的次数（只给宿主用例读：nvn_gpu_vk_test_cmd_refseen_hits）
    struct vk_staging *staging;
    uint32_t nstaging, scap;
    struct nvn_gpu_buf_s *arena;
    uint64_t arena_used, arena_cap;
    VkCommandPool pool;
    VkDescriptorPool dpool;                  // 当前在分配的描述符池
    VkDescriptorPool *dpools_full;           // 已经分满的池：里面的组还被已录制的命令引用，段完成时一并销毁
    uint32_t ndpools_full, dpools_full_cap;
    struct dset_use *dsu;                    // 本段从按布局回收表取过的描述符组：段完成时整批放回（放弃的段改为还给池）
    uint32_t ndsu, dsu_cap, dsu_gen;         // dsu_gen：取第一组时的回收表代际（g_dset_gen）；放回时代际变了 = 表和池都已随会话销毁，不能再碰
    struct nvn_gpu_pipe_s *pipe;
    struct pass_rec *passes;
    struct desc_memo *memo;   // 这一段里写过的描述符组：内容 → 组（见 bind_descriptor_groups）；懒分配，随段释放
    uint32_t memo_n;
    uint32_t npasses, passcap;
    uint32_t pass_w, pass_h;
    uint32_t pass_ncolor, pass_layers;       // 当前通道的颜色槽数、层数（clear_rect 用）
    uint32_t pass_color_mask;                // 哪些颜色槽有附件（位 i = 槽 i；空位为 0）
    uint32_t pass_cfmt[8], pass_dsfmt, pass_samples;   // 当前通道各槽的 NVN 格式（空位 0）、深度模板格式、采样数（带掩码的清除建管线用）
    VkImageAspectFlags pass_ds_aspect;       // 当前通道深度模板附件有的面（0 = 没有该附件）
    VkQueryPool report_ts;
    uint64_t report_used, report_begin_ns;
    uint32_t report_count;
    VkQueryPool tsq;                         // 调试读数 nvk_gputime：段首尾各一个 GPU 时间戳，后面每个通道首尾各一个（没开就是空）
    struct { uint16_t w, h, draws; uint8_t ncol, ds; } tsp[TS_PASS_MAX];
    uint32_t ntsp;
    VkCommandBuffer init_cb;                 // 本池复用的前置命令缓冲；本段需要时录入图像转布局 / 时间戳重置
    struct nvn_gpu_tex_s **newimg;           // 上面那个命令缓冲转的图像（各持一份引用，段完成时归还；段没执行则放回待转表）
    uint32_t n_newimg;
    struct cmd_lay *lay;   // 本段内每张图当前的布局（段首一律 GENERAL，段末转回 GENERAL）
    uint32_t nlay, laycap;
    int opt;                        // 本段是否走对齐语义的布局（开段时读 debug.s2a.nvk_opt_layout，缺省 0）
    nvn_gpu_pass_desc pass_desc;    // 当前通道的原始描述（反馈环拆通道时按它重开）
    struct nvn_gpu_tex_s *pass_tex[9];   // 当前通道各附件的贴图（pass_natt 个；拆通道判断反馈环用）
    uint32_t pass_natt;
    nvn_gpu_dyn dyn;
    int has_dyn;
    struct { nvn_gpu_buf buf; uint64_t off; } vbuf[16];
    struct { nvn_gpu_binding b; } binds[6][64];   // 每阶段一张表，bind 只替换该阶段
    uint32_t nbinds[6];
    uint8_t pool_active;
    nvn_gpu_pool_table pool_binds[6][64];  // 各阶段布局项上的池快照，cmd_ref 保活
    VkDescriptorPool *pool_dpools; uint32_t pool_ndpools, pool_dpool_cap;
    struct prs_use *prs_use; uint32_t prs_n, prs_cap;   // 本段取过的回收整池组（段完成时还原反馈补丁并放回，见 prs_release）
    VkDescriptorSet desc_sets[4];   // 仅本命令段；旧组仍归本段的池所有，不就地覆盖在途绑定
    VkPipelineLayout desc_layout;
    uint32_t desc_dirty;
    uint32_t dyn_off[4][32];   // 动态 UBO 偏移（#1233）：每组每个动态描述符当前要给 vkCmdBindDescriptorSets 的偏移；换布局时清零，每笔绘制从 binds 里重算
    uint8_t dyn_dirty;         // 只有动态 UBO 的偏移变了（组内容没变）：这一笔不重写组，只用新偏移重绑
    // 描述符写入影子（开关 nvk_desc_skip，缺省开）：同一个组对象里，内容和上次真正写进去的一样就不再写。
    // 真机上一帧约 1030 次整组重建、每次 11 条描述符；其中大部分条目（贴图句柄、采样器、常量缓冲片）和上一笔相同。
    struct desc_shadow { uint64_t a, b, c; uint32_t type; uint16_t binding, elem; uint8_t valid; } dsh[4][32];
    VkDescriptorSet dsh_set[4];
    uint8_t dsh_ok[4];
    uint8_t desc_point;             // 当前这几组绑在哪个绑定点：0 还没绑，1 图形，2 计算

    uint8_t *push_data[6];        // 各阶段 push 暂存；vkCmdPushConstants 录制时拷贝入命令缓冲，段完成即放
    uint32_t push_capacity[6]; // 分配字节数，测试与管线切换共用
    uint8_t push_have[6];
    uint32_t push_gen[6];         // 各阶段 push 内容的代号：nvn_gpu_push 写进的字节与原先不同才加一（发命令时据此判断要不要重发）
    // 冗余状态消除（#708）：上次真正录进命令缓冲的绘制状态。只在发命令那一刻和它比，相同就不再录；
    // ok 位为 0 = 不知道命令缓冲里现在是什么（命令对象新建时全 0；通道开始、矩形清除、计算派发之后整份作废）。
    // 只管管线、动态状态、顶点 / 索引缓冲、push 常量；描述符组不在这里（#657）。
    struct {
        VkPipeline gpipe;                                  // 绑着的图形管线（空 = 不知道）
        uint8_t vp_ok, sc_ok, bc_ok, st_ok, db_ok, ib_ok;
        VkViewport vp; VkRect2D sc; float bc[4]; uint32_t st[2][3]; float db[3];
        uint16_t vb_ok; VkBuffer vb[16]; VkDeviceSize vboff[16];
        VkBuffer ib; VkDeviceSize ioff; VkIndexType it;
        uint8_t push_ok[6]; VkPipelineLayout push_layout[6]; uint32_t push_off[6], push_bytes[6], push_gen[6];
    } es;
    uint32_t es_count[ES_KINDS];   // 实际录了几次（测试读数：nvn_gpu_vk_test_emit_counts）
    // 遮挡查询（nvn_gpu_cmd_desc.occlusion_slots > 0 才有）：一段一个查询池，段开头整池重置；每个槽一段里只能用一次。
    // 后端自己拆通道（附件反馈快照）时，活动的查询在旧通道末尾结束，新通道里换一个「续段槽」接着数。续段槽不在上面那个池里，
    // 按块给（qchunk[k] = 第 k 块，一个 OCC_CHUNK 槽的查询池，用到哪块才要哪块）：同一段里这样拆几次不定（固定 64 个续段槽的版本在真机上有一个场景用完了，#1658），所以不定死一个数。
    // 读回时把续段槽的样本数并回它所属的声明槽（q_owner[j] = 第 j 个续段槽属于哪个声明槽）；调用方看到的槽数、槽号和读数都不变。
    // q_active = 活动查询的声明槽号；q_active_pool / q_active_idx = 它此刻所在的池与池内下标（声明槽就是 qpool 的第 q_active 个）。
    // q_used[槽] 的位：1 = 本段用过；2 = 这个查询续不下去了（保险丝到顶 / 分配或建池失败，拆通道照做，只是它不再续数）；
    //   4 = 读回时它名下有一段没读到；8 = 读回时它的声明槽那一段没读到。带 2 或 4 的槽按「未完成」处理（读 0，计入 nvn_unimpl），不拿部分和当结果。
    // 一个查询名下的续段槽在 q_owner 里是连续的（一个槽一段里只开一次、同一时刻只有一个活动查询），q_run_start = 当前这一串的起点。
    VkQueryPool qpool; uint32_t qcount, qmax_used; int32_t q_active; uint8_t *q_used; uint8_t pass_occl;
    VkQueryPool q_active_pool; uint32_t q_active_idx;
    VkQueryPool *qchunk; uint32_t qnchunk, qchunk_cap;   // 续段块表（按需增长）
    uint32_t *q_owner, qspare_used, qspare_cap, q_run_start;   // 续段槽归属表（按需增长）
    struct {
        nvn_gpu_pipe original, variant;
        struct nvn_gpu_cold_domain domain;
        nvn_gpu_draw_args draw;
        nvn_gpu_slice result;
        uint32_t prefix_value;
        uint64_t source_generation;
    } cold;
    VkDescriptorPool cold_pool;
    uint32_t cold_sets;
    uint64_t cold_barriers;
    struct cold_event *cold_events;
    unsigned cold_nevents, cold_cap;
    int cold_events_lost;
    struct nvn_gpu_cmd_s *live_next;
};

// 冗余状态消除：影子整份作废（之后每一项第一次用到都照发）
static void es_reset(struct nvn_gpu_cmd_s *c) { memset(&c->es, 0, sizeof c->es); }



// 每个 2 的幂区间分八档，0–7 ns 精确。固定内存，覆盖 uint64_t 全范围。
enum { PIPE_DURATION_SUB_BUCKETS = 8, PIPE_DURATION_BUCKETS = PIPE_DURATION_SUB_BUCKETS * 62 };
struct pipe_creation_counters {
    _Atomic uint64_t no_feedback, app_hit, driver_ns, wall_ns;
    _Atomic uint64_t duration[PIPE_DURATION_BUCKETS];
};

struct nvn_gpu_vk {
    struct s2a_vk_device *dev;
    struct nvn_vk_bcdec *bc;
    int bc1_astc_on, bc_time_on;
    uint32_t bc_time_bits;
    VkDevice d;
    VkPhysicalDevice phys;
    VkQueue queue;
    nvn_gpu_caps caps;
    VkPipeline cold_pipe;
    VkPipelineLayout cold_layout;
    VkDescriptorSetLayout cold_dsl;
    int cold_enabled, cold_failed, cold_writes_lost;
    uint64_t cold_epoch;
    _Atomic uint64_t cold_attempts, cold_armed, cold_pairs;
    _Atomic uint32_t cold_present_count;
    PFN_vkGetCalibratedTimestampsKHR report_calibrate;
    uint32_t report_bits;
    float report_period;
    int64_t report_corr_ticks;   // 自校修正（计数）：驱动校准对的 DEVICE 值与查询计数器零点不一致时从校准 GPU 值里减掉（见 report_self_calibrate）
    uint64_t report_corr_dev;    // 自校修正附带的不确定度（探针窗口半宽）
    pthread_t worker;
    int worker_started;
    int closing;
    int closed;
    _Atomic int stop;
    int busy;
    int commit_active;   // 正在 commit 的线程数：close 要等它归零才拆资源
    int pipe_calls_active;   // 已准入的图形 / 计算管线调用；close 锁住入口后等它们完整退出
    int close_active;    // 已有一个线程在跑 close，防止重复 join
    _Atomic int device_lost;
    uint64_t completed;
    uint64_t failed;
    uint64_t last_signal;
    uint64_t *submitted;
    uint32_t nsub, capsub;
    struct vk_segment *qhead, *qtail;
    struct vk_segment *active_segment;   // g_mu：已出队，worker 尚未处理完 Vulkan 等待
    _Atomic(struct vk_segment *) submitting;  // 正在向 Vulkan 提交的客体段。入队（g_mu）时清除
    int loss_reported;                   // g_mu：每个会话首份故障快照
    struct nvn_gpu_buf_s *rhead, *rtail;
    struct nvn_gpu_cmd_s *live;
    struct nvn_gpu_buf_s *bufs;
    struct nvn_gpu_tex_s *texs;
    struct nvn_gpu_view_s *views;
    struct nvn_gpu_samp_s *samps;
    struct nvn_gpu_pool_table_s *pools;
    uint32_t border_live;          // 存活的自定义边框色 VkSampler 个数，对照 dev->custom_border_max（g_mu 下改）
    struct nvn_gpu_shader_s *shaders;
    struct nvn_gpu_pipe_s *pipes;
    uint64_t stats[STAT_N];
    struct vk_land_slot land_slots[3];
    int land_readback, land_auto;
    _Atomic int land_reported;
    unsigned land_prior_live, land_live_count;
    struct la_trial land_trial;
    int vbuf_batch;                 // 会话内固定；debug.s2a.nvk_replay_cpu_cuts 缺省开启
    int fault_inject;
    uint64_t test_fail_signal;    // 测试钩子：让指定 signal 的段表现为 fence 失败
    uint64_t test_delay_signal;   // 测试钩子：让指定 signal 的段先睡 ms
    uint32_t test_delay_ms;
    uint64_t wsi_seq;    // 已提交的呈现段序号（g_submit_mu 下分配，与入队同序）
    uint64_t wsi_done;   // worker 已等到栅栏的最后一个呈现段序号（g_mu 下改；段按入队顺序处理，单调）
};

// Stop 可在后端还在 open 时报告；计数清零也走原子操作，不与 g 的 memset 共用内存。
static _Atomic enum nvk_pipe_feedback_mode g_pipe_feedback;
static struct pipe_creation_counters g_pipe_creation[NVK_PIPE_CREATION_BUCKETS];
static void pipe_creation_open(const struct s2a_vk_device *dev) {
    enum nvk_pipe_feedback_mode mode = NVK_PIPE_FEEDBACK_UNAVAILABLE;
    if (dev->pipeline_creation_feedback_enabled)
        mode = nvn_plat_flag("nvk_pipe_creation_feedback", 1) ? NVK_PIPE_FEEDBACK_ON : NVK_PIPE_FEEDBACK_OFF;
    for (unsigned i = 0; i < NVK_PIPE_CREATION_BUCKETS; i++) {
        struct pipe_creation_counters *c = &g_pipe_creation[i];
        atomic_store_explicit(&c->no_feedback, 0, memory_order_relaxed);
        atomic_store_explicit(&c->app_hit, 0, memory_order_relaxed);
        atomic_store_explicit(&c->driver_ns, 0, memory_order_relaxed);
        atomic_store_explicit(&c->wall_ns, 0, memory_order_relaxed);
        for (unsigned b = 0; b < PIPE_DURATION_BUCKETS; b++)
            atomic_store_explicit(&c->duration[b], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&g_pipe_feedback, mode, memory_order_release);
}

static struct nvn_gpu_vk g;
static _Atomic int g_open;   // 跨线程：close 置 0 后其他线程据此拒收
static uint64_t g_border_nearest;     // sampler_border_color_nearest 计数（samp_border_nearest_note）
static int g_border_nearest_said;     // 本局已打过边框色就近那一行：开会话时清零
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// 诊断读数（nvk_gputime 开着时每 300 次上屏打一行）：每帧新建了多少个驱动对象、写了多少次描述符、开了多少个查询。
enum { OC_RP, OC_FB, OC_CPOOL, OC_DPOOL, OC_DALLOC, OC_DWRITE, OC_QUERY, OC_VIEW, OC_N };
static uint64_t g_oc[OC_N], g_oc_total[OC_N];
static uint64_t g_acq_ns;   // 取窗口图（vkAcquireNextImageKHR）累计用时，随对象读数那一行打
#define OC(i) (__atomic_fetch_add(&g_oc[i], 1, __ATOMIC_RELAXED), __atomic_fetch_add(&g_oc_total[i], 1, __ATOMIC_RELAXED))
uint64_t nvn_vk_dbg_created(int which) { return which >= 0 && which < OC_N ? __atomic_load_n(&g_oc_total[which], __ATOMIC_RELAXED) : 0; }   // 用例读：会话以来新建的各类对象数（0 渲染通道 1 帧缓冲 …，见上面的枚举）
// ---- 通道对象缓存：渲染通道对象与帧缓冲跨帧保留 ----
// 原先每开一个通道都 vkCreateRenderPass + vkCreateFramebuffer、所在的段做完再销毁；一帧一百多个通道，目标就那么十几组，每帧重复建销。
// 做法照 d2r-mobile 安卓笔记 perf-round2 的发现 3：渲染通道对象按「附件描述」记（与具体图像无关，会话结束才销毁）；
// 帧缓冲按「渲染通道 + 各附件视图 + 尺寸」记，直到其中任何一个视图销毁。视图要等引用它的段都做完才会销毁（cmd_ref 保活），
// 所以那时用到这个帧缓冲的命令也都做完了，可以当场销毁。表满了的那一个照旧路走（记在段上、段做完销毁）。
// 调试开关 nvk_pass_cache=0 关掉（缺省开）。
static void pc_drop_view(VkImageView v);
static void pc_drop_view_fast(struct nvn_gpu_view_s *view, VkImageView v);   // 视图销毁：按反查表只拆它当过的帧缓冲
static void pc_fb_note_desc(const nvn_gpu_pass_desc *d, int slot, uint32_t gen, VkFramebuffer fb);   // 记「这些附件视图当过这个帧缓冲」
static void pc_destroy_all(void);
enum { PIPE_HT_N = 1024 };
static uint64_t g_desc_ns[4], g_desc_nw, g_desc_sets, g_desc_hit;   // 描述符四步：分配 / 构造写入项 / 写入 / 绑定；写入的描述符条数；重写的组数
static uint32_t g_max_dyn_ubo, g_max_dyn_ubo_dev;   // 设备 maxDescriptorSetUniformBuffersDynamic（开会话时读；契约头不动，只在本后端里存）；后一个是设备真值（宿主用例压低前者之后还原用）
// 动态 UBO 偏移的拆分掩码：偏移 & 掩码 = 低位，走 vkCmdBindDescriptorSets 的动态偏移（Vulkan 的动态偏移只有 32 位）；偏移 & ~掩码 = 高位，放进描述符。
// 生产恒为 0xffffffff（低 32 位 / 高 32 位）；只有宿主用例经 nvn_vk_dbg_dyn_split 压低它：宿主设备（lavapipe）单次分配上限 2 GiB，造不出 ≥ 4 GiB 的缓冲，
// 要让「高位非 0」真的读到数据，只能把拆分点从 32 位降到 20 位，同一段代码、同一套判据。
static uint64_t g_dyn_lo_mask = 0xffffffffull;
static uint64_t g_dyn_rebind, g_dyn_pipes[2], g_dyn_fast;   // 诊断：只换动态偏移、不重写组、只重绑的次数；用动态 UBO 的管线条数 / 建过的带 UBO 的管线条数；绑定表里「只换偏移」直接放过的项数（用例钉这条近路）
// 描述符组备忘开关 nvk_desc_memo（缺省开；0 = 关，回到「静态 UBO、不备忘」的老路径）。开 = 段内备忘 + 动态 UBO 偏移（备忘的键里没有偏移才有命中，见 pipe_build / write_binds）。
// 建管线时读一次（决定这条管线的 UBO 用不用动态偏移），绑定时按段读（bind_descriptor_groups 里有短缓存）；两处读到不一样也能正确：动态与否是管线自己的属性，备忘与否只是这一笔要不要查表。
static int desc_memo_flag(void) { return nvn_plat_flag("nvk_desc_memo", 1) != 0; }
// 整池数组组（NVN_RB_TEXP / SAMPP：贴图池 / 采样器池整表一个绑定，count = 池长 + 1）的备忘开关 nvk_pool_memo（缺省开；0 = 关，每笔脏组都新建整池组，A/B 与负对照用）。
// 整池组原先不进上面的备忘：每笔画的脏组都向驱动新建一个只装它的描述符池（maxSets = 1）、分一组、把整张表（真机一款 3D 游戏：贴图池 8448 项 + 采样器池 1280 项）重写一遍，
// 池快照不可变、本段持引用，指针相同 = 内容相同，所以「同一布局 + 同样的普通写入 + 同样的池快照」的组可以像别的组一样段内复用（键见 pool_set_mask，用法见 bind_descriptor_groups）。
static int pool_memo_flag(void) { return nvn_plat_flag("nvk_pool_memo", 1) != 0; }
static int g_pmemo_cur = -1; static uint32_t g_pmemo_k;   // 开关缓存：绑定时每 256 次重读一次（宿主用例经 nvn_gpu_vk_test_pool_memo_refresh 立即作废）
// 整池组回收开关 nvk_pool_rec（缺省开；0 = 关，每笔脏的整池组新建单组专用池、整表写、段完成销毁，A/B 与负对照用）。见 backend_vulkan_pool.inc 的 prs_*。
static int pool_rec_flag(void) { return nvn_plat_flag("nvk_pool_rec", 1) != 0; }
static int g_prs_cur = -1;   // 开关缓存：同上
// 回收总量按字节封顶（维护方在 #1905 定的口径，不按组数）：在用 + 空闲的回收组占用的描述符池，估算字节合计 ≤ 封顶（缺省 384 MB）；
// 超了先淘汰空闲串里最久没用的组，淘汰完还不够才退回老路径，不拦任何操作。空闲串不再另设「一半」的封顶：一帧里各段完成时，这一帧用到的整池组一起放回空闲串（真机重场景一帧用 170–187
// 组，估算约 204–224 MB），空闲封顶设成总封顶的一半（192 MB ≈ 160 组）就比一帧的用量还小，每帧放回时超出的 14–30 组被淘汰、下一帧又得新建
// （取组 + 放回 3.7–10.3 毫秒 / 帧）；总量封顶已经封住了内存。
// 每个描述符按 PRS_DESC_BYTES 字节估：一款 3D 游戏的整池组（贴图池 8448 项 + 采样器池 1280 项，
// 共约 9.7 千项）实测每组使进程内存涨约 0.85 MB（回收开 / 关两档 PSS 差 +271 MB，其中图形内存 +219 MB，现存 257 组），0.85 MB ÷ 9.7 千项 ≈ 88 字节 / 项。
// 这是估算，不是读数：别的驱动 / 别的 GPU 的每项开销会不同，封顶只是防止换一款表更大的游戏时一路涨到被系统杀掉。真机缺省包不能改封顶；调试包用开关 nvk_pool_rec_mb 压低（验淘汰）。
enum { PRS_DESC_BYTES = 88, PRS_GROUP_MIN_BYTES = 4096, PRS_CAP_MB = 384 };
static pthread_mutex_t g_prs_mu = PTHREAD_MUTEX_INITIALIZER;   // 保护回收表和下面的计数；回放线程只 trylock，碰上就走老路径
static uint32_t g_prs_total, g_prs_idle;   // 现存组（在用 + 空闲）/ 其中空闲（持 g_prs_mu）
static uint64_t g_prs_bytes, g_prs_idle_bytes;   // 现存组 / 其中空闲的估算字节（持 g_prs_mu）
static uint64_t g_prs_cap = (uint64_t)PRS_CAP_MB << 20, g_prs_idle_cap = (uint64_t)PRS_CAP_MB << 20;   // 总量封顶 / 空闲封顶（字节，持 g_prs_mu；会话开始按 nvk_pool_rec_mb 重置，缺省两者相等：空闲 ≤ 总量，总量封顶封住内存）
static uint64_t g_prs_seq;   // 放回空闲串的序号发号器（持 g_prs_mu）：越小越久没用，淘汰时全局取最小
static uint64_t g_prs_evict;   // 按字节封顶淘汰掉的空闲组，会话内累计（原子累加，读数不加锁）
static uint64_t g_prs_why[4];   // 没走回收的原因累计（原子）：碰锁或内存不够 / 写法不同 / 到上限 / 补丁太多
static uint64_t g_pool_uid;                         // 池快照序号发号器（持 g_mu）
enum { PN_NEED, PN_HIT, PN_NEW, PN_FB, PN_ELEMS, PN_RHIT, PN_RMISS, PN_RNEW, PN_RFALL, PN_RPATCH, PN_N };
// 回收（nvk_pool_rec）读数：PN_RHIT 取到键相同的空闲组（整池免写）/ PN_RMISS 取到键不同的空闲组（整表重写）/ PN_RNEW 向驱动新建 / PN_RFALL 没走回收（退回老路径）/ PN_RPATCH 反馈补丁写入的项数
static uint64_t g_pool_n[PN_N], g_pool_ns[3];   // 整池组读数：要的组数 / 段内备忘命中 / 新建（建池 + 分组）/ 因反馈拷贝在用而绕开备忘 / 整池写入的描述符项数 / 回收：取到键相同的空闲组 / 取到键不同的 / 向驱动新建 / 没走回收 / 反馈补丁写入项数；新建（建池 + 分组）与整池写入的纳秒（nvk_gputime 开着才计）
// 诊断累加器（g_desc_* / g_draw_ns）只增不清零：每个读者自带一份「上次读到的值」，读数 = 当前 − 上次。
// 近 300 帧读数（backend_vulkan_wsi.inc）与每 5 秒的逐笔读数（nvn_gpu_draw 末尾）原先各自对同一批累加器 exchange 清零，
// 互相取走对方窗口里的一部分，两边读数的每帧值、每笔值都偏小（nvk_gputime 开着时）。
// 64 位累加器只增不清零，不会溢出：纳秒累加器按每秒 1e9 算，约 584 年才回绕。
// 「上次读到的值」是各读者函数里的 static，与同一个函数里原有的 np / w_t0 / w_n 一样，只由调用这个函数的那一个线程读写，不加锁。
static inline uint64_t diag_delta(const uint64_t *g, uint64_t *last) {
    const uint64_t cur = __atomic_load_n(g, __ATOMIC_RELAXED), d = cur - *last;
    *last = cur;
    return d;
}
// 诊断读数（nvk_gputime 开着才计）：贴图 / 视图 / 帧缓冲的建与拆各用多久、各几次。
// 0 vkCreateImage，1 要显存并绑定，2 默认视图，3 帧缓冲，4 附件视图，5 拆视图，6 从帧缓冲表里摘（pc_drop_view），7 拆贴图（视图 + 图 + 还显存）
static uint64_t g_life_ns[8], g_life_n[8];
#define LIFE_T0 uint64_t lt_ = g_draw_tm ? now_ns() : 0
#define LIFE_ADD(i) do { if (g_draw_tm) { uint64_t ln_ = now_ns(); __atomic_fetch_add(&g_life_ns[i], ln_ - lt_, __ATOMIC_RELAXED); __atomic_fetch_add(&g_life_n[i], 1, __ATOMIC_RELAXED); lt_ = ln_; } } while (0)
static uint64_t now_ns(void);
static int g_draw_tm; static uint64_t g_draw_ns[3];   // 绘制入口分段计时：描述符 / 状态 / 顶点索引与绘制调用
static uint64_t g_feedback_stats[4]; // nvk_gputime：只读采样 draw / 快照 vkCmdCopyImage / 反馈重开次数 / 整池数组与写着的附件重叠而免拷的 draw 数，只增不清零
static uint64_t g_feedback_snap[2];  // nvk_gputime：反馈快照图 新建 / 同段同源复用 次数，只增不清零
// 反馈快照池（跨段复用快照图，见 backend_vulkan_feedback.inc）：放在 g_mu 下。数组按放回先后排，[0] 最久没用。
//   上限按字节（开关 nvk_feedback_pool_mb，缺省 256，0 = 关、回到每段新建），会话打开时读一次。
enum { FBPOOL_N = 256 };
static struct nvn_gpu_tex_s *g_fbpool[FBPOOL_N];
static uint32_t g_fbpool_n;
static uint64_t g_fbpool_bytes, g_fbpool_cap;
static uint64_t g_fbpool_cnt[3];   // 缺省常开、只计数（只增不清零）：快照图 新建 / 池命中 / 淘汰
void nvn_vk_fbpool_stat(uint64_t out[5]);   // 读数出口（backend_vulkan_feedback.inc）：新建 / 命中 / 淘汰 / 池里张数 / 池里字节
static uint64_t g_draw_log_ns = 5000000000ull;         // 逐笔读数（nvn_gpu_draw 末尾）的周期，缺省 5 秒；测试钩子 nvn_gpu_vk_test_diag_timing 可缩短
static void draw_tm_init(void) { if (!g_draw_tm) g_draw_tm = nvn_plat_flag("nvk_gputime", 0) != 0; }   // 与 GPU 时间戳同一个开关
// 回放线程取 g_mu（nvk_gputime 开着才计）：先 trylock，没拿到的记「真等了几次、等了多久」。持 g_mu 的别的线程（游戏工作线程销毁视图、
// 完成线程摘段）会把回放线程挡在这里；回放耗时里有多少是在这儿干等，看这一项。只用在回放线程独占的录制路径上（cmd_ref_at / 附件视图 / 建视图）。
static uint64_t g_gmu_n, g_gmu_wait_n, g_gmu_wait_ns;
static void gmu_lock_rt(void) {
    if (!g_draw_tm) { pthread_mutex_lock(&g_mu); return; }
    __atomic_fetch_add(&g_gmu_n, 1, __ATOMIC_RELAXED);
    if (pthread_mutex_trylock(&g_mu) == 0) return;
    const uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_mu);
    __atomic_fetch_add(&g_gmu_wait_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_gmu_wait_ns, now_ns() - t0, __ATOMIC_RELAXED);
}
static uint64_t g_pl_ns, g_pl_n, g_pl_steps, g_pl_cmp;   // 取图形管线：命中查找的累计纳秒 / 次数 / 链上走过的条数 / 其中逐字段比了的条数
// 图形管线的散列桶（见 pipe_hash）；持 g_mu。桶数 g_pipe_ht_n 是 2 的幂，管线数超过桶数就翻倍（装填因子 <= 1，见 pipe_ht_add）。
// 原先桶数固定 PIPE_HT_N = 1024：世界场景里管线数是它的好几倍，真机读数「取管线查找」每次比 7.3 条（每条一次缓存未命中，0.83 微秒 / 次、每帧 487 次，#1130）。
static struct nvn_gpu_pipe_s **g_pipe_ht;
static uint32_t g_pipe_ht_n, g_pipe_cnt;
#define CPIPE_BUCKETS 1024u
static struct nvn_gpu_pipe_s *g_cpipe_ht[CPIPE_BUCKETS];
// ---- 驱动管线缓存（#822）：后端建的每条管线都带同一个 VkPipelineCache；会话收尾把驱动给的数据写进 open_desc.cache_dir，
// 下一局开会话时读回来喂给驱动。真机每局起局二十多秒的同步建管线（每条二三十毫秒）是驱动每次从头编，带上缓存后重开一局按内容命中。
// 契约（nvn_backend.h open_desc.cache_dir）：NULL = 不落盘；后端有界维护、收尾时写、不逐帧写盘。文件层是旧回放用的 nvn_vk_driver_cache
// （魔数 + 版本 + SHA-256、临时文件 fsync 后原子替换、坏了当没有、64 MiB 封顶）。
// 缓存只是加速，不是前提：读 / 建 / 取数据 / 写，任何一步失败都只记一行日志，管线照旧能建（不拦用户）。
// 调试开关 nvk_pipe_cache=0 关掉（缺省开）：不建缓存、不读不写盘，管线照旧不带缓存建。
// 局中补写（#868，「收尾时写」之外的第二个时机）：被系统杀掉 / 崩掉的局走不到收尾，驱动编出来的结果就白丢了——所以建管线累计够多、隔得够久时，
// 在局中取一份驱动缓存交给后台线程落盘，每次落盘仍是整份原子替换（见下面 plc_save_midrun）。不是逐帧写盘：缺省最多 30 秒一次，数据没变不写。
static VkPipelineCache g_plc;                // VK_NULL_HANDLE = 没有（开关关了 / 驱动建不出）：建管线时照传，空句柄就是不带缓存
static char g_plc_dir[480];                  // 落盘目录副本；空 = 不落盘（只在本局内复用，收尾不写）
static uint8_t g_plc_uuid[VK_UUID_SIZE];     // 设备 + 驱动的管线缓存标识（文件名里带它，换驱动不互读）
static size_t g_plc_loaded;                  // 开会话时读到并交给驱动的字节数（0 = 没读到）
static uint64_t g_plc_rejected;              // 本会话里旧文件读不出 / 驱动不收、被丢掉的次数
static uint64_t g_plc_made;                  // 本会话经缓存建出的管线条数（图形 + 计算 + 清屏）
// ---- 局中补写的状态。触发：g_plc_made 比上次保存时多 ≥ N 条（nvk_plc_new，缺省 64），且头一次距开会话 ≥ F 毫秒（nvk_plc_first_ms，10000）、
// 之后距上次保存 ≥ E 毫秒（nvk_plc_every_ms，30000）；取到的数据大小与上次保存（或开局读入）的一致就不写。
// 分工：取数据（vkGetPipelineCacheData，真机约 0.5 ms）在触发的那条线程上做；写盘（临时文件 + fsync + 原子替换，真机十几毫秒、慢盘更久）在后台线程做，
// 后台线程只拿自己的缓冲区和路径，不碰任何 Vulkan 句柄、不碰会话全局。同一时刻最多一次写在飞（进程级、不排队：在飞时新的触发直接跳过）。
// 收尾：等在飞的写算进 close 的截止时间，到点没写完就放弃——不再写最终份、不再等它，旧文件原样；写失败 / 设备丢失只记日志。
static uint64_t g_plc_t_open;                // 开会话的时刻（单调时钟 ns）
static uint64_t g_plc_t_saved;               // 上一次局中保存（含「取了数据发现没变」）的时刻；0 = 本局还没有过。原子读写
static uint64_t g_plc_made_saved;            // 那一次的 g_plc_made。原子读写
static size_t g_plc_size_saved;              // 上一次落盘（或开局读入）的数据字节数：取到的数据与它一致就不写。持 g_plc_mu
static uint64_t g_plc_first_ns, g_plc_every_ns, g_plc_thr_new;   // 触发的三个阈值：开会话时读一次
// 护 g_plc 的销毁与 g_plc_size_saved（触发取数据 vs 收尾；触发用 trylock，拿不到就让给别人）。
// 缓存对象本身的并发不靠这把锁：创建缓存时没设 EXTERNALLY_SYNCHRONIZED = 实现内部同步（见 plc_open 的注释）；
// 这把锁保护的是我们自己的状态：g_plc 的销毁不能和取数据撞上、g_plc_size_saved 的读写要成对。
static pthread_mutex_t g_plc_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_plcw_busy;                      // 后台写在飞（进程级，跨会话；原子读写）
static int g_plcw_failed;                    // 最近一次后台写失败了：收尾时即使大小没变也再写一次。原子读写
static uint64_t g_plcw_started, g_plcw_ok, g_plcw_fail, g_plcw_same;   // 读数（进程累计，不随会话清零）：交给后台写的次数 / 成功 / 失败 / 与上次一致没写
static uint64_t g_plcw_get_ns;   // 触发线程上「查大小 + 取数据 + 拷字节」的累计纳秒（后台写盘不算；长帧归因读它）
static pthread_mutex_t g_submit_mu = PTHREAD_MUTEX_INITIALIZER; // 提交串行化：校验 signal、vkQueueSubmit、入队
static pthread_cond_t g_cv;
static int g_cv_ready;
static _Thread_local int g_in_callback;   // [回调] 线程：只许调 [任意] 函数

// ---------------------------------------------------------------- 小工具

static nvn_gpu_rc rc(int code) { nvn_gpu_rc r; r.code = code; return r; }
static int rc_is(nvn_gpu_rc r, int code) { return r.code == code; }

static nvn_gpu_rc fail(nvn_gpu_err *e, int code, const char *fmt, ...) {
    if (e) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->why, sizeof e->why, fmt, ap);
        va_end(ap);
        e->rc = rc(code);
    }
    return rc(code);
}

static void set_sticky(struct nvn_gpu_cmd_s *c, int code, const char *fmt, ...) {
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;   // 首错原文
    c->sticky = rc(code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->why, sizeof c->why, fmt, ap);
    va_end(ap);
}

static uint64_t add_sat(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void deadline_after(struct timespec *ts, uint64_t ns) {
    uint64_t t = add_sat(now_ns(), ns);
    ts->tv_sec = (time_t)(t / 1000000000ull);
    ts->tv_nsec = (long)(t % 1000000000ull);
}

// ---------------------------------------------------------------- 提交集合

static int submitted_has(struct nvn_gpu_vk *vk, uint64_t v) {
    uint32_t lo = 0, hi = vk->nsub;   // signal 严格递增，数组天然有序
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (vk->submitted[mid] == v) return 1;
        if (vk->submitted[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static void submitted_compact_locked(struct nvn_gpu_vk *vk) {
    uint32_t drop = 0;
    while (drop < vk->nsub && vk->submitted[drop] <= vk->completed) drop++;
    if (!drop) return;
    memmove(vk->submitted, vk->submitted + drop, (size_t)(vk->nsub - drop) * sizeof *vk->submitted);
    vk->nsub -= drop;
}

static int submitted_add(struct nvn_gpu_vk *vk, uint64_t v) {
    if (submitted_has(vk, v)) return 1;
    if (vk->nsub == vk->capsub) {
        uint32_t cap = vk->capsub ? vk->capsub * 2 : VK_SEG_QUEUE_INIT;
        uint64_t *p = realloc(vk->submitted, (size_t)cap * sizeof *p);
        if (!p) return 0;
        vk->submitted = p;
        vk->capsub = cap;
    }
    vk->submitted[vk->nsub++] = v;
    return 1;
}

// ---------------------------------------------------------------- 引用与退休

// 调用方持 g_mu。对象本身就是队列节点：分配失败不可能吞掉 retired。
static void queue_retire_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    b->retire_next = NULL;
    if (vk->rtail) vk->rtail->retire_next = b;
    else vk->rhead = b;
    vk->rtail = b;
    pthread_cond_broadcast(&g_cv);
}

static void destroy_pipe_objects(struct nvn_gpu_pipe_s *p);
static void dlay_close(void);
static void destroy_tex_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t);
static void destroy_view_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v);
static void destroy_samp_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_samp_s *s);
static void unref_locked(struct nvn_gpu_vk *vk, void *obj, int type);

// ---------- 图像布局 ----------
// 两个取值（开关 debug.s2a.nvk_opt_layout）：
//   · 0（缺省，旧行为）：图像常驻 VK_IMAGE_LAYOUT_GENERAL：通道附件按 GENERAL 进出，采样 / 存储按 GENERAL 绑，
//     传输在同一次调用里转去转回。
//   · 1：附件在通道里用 COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL，采样用
//     SHADER_READ_ONLY_OPTIMAL / DEPTH_STENCIL_READ_ONLY_OPTIMAL，存储图像与传输照旧 GENERAL / TRANSFER_*。
//     移动 GPU 只有附件布局下才开帧缓冲压缩与快速解析，GENERAL 下用不了：一帧几十个通道的固定成本主要花在这上面。
// 段与段之间的静止布局一律 GENERAL：每张图在本段的第一次迁移都从 GENERAL 出发，段末（commit 前）把本段动过的都转回
// GENERAL（lay_rest_at_commit）。这样命令缓冲里录的 oldLayout 只依赖本段自己的录制顺序，和「哪一段先提交」无关——
// 前置段 g_pre 是录制在后、提交在前，靠全局记布局遇到这种次序就记不准，旧实现因此一律钉 GENERAL。
// 新建的图像（UNDEFINED）仍由「创建之后的第一次提交」先于那一段转成 GENERAL（submit_cbs 的 init_cb）：
// 提交时才知道谁先跑，预知不了首次用途，所以新图首用比最优多一次转换。
// 正确性红线：GENERAL 不保证同 draw 附件反馈正确；重叠的采样视图在 draw 前复制到私有快照，
// 然后按 LOAD 重开（backend_vulkan_feedback.inc）。同图不重叠级 / 层仍共用整图布局，按原迁移路径处理。
// 声明了遮挡查询的通道（pass_desc.occlusion）中途不能拆（查询不能跨通道重开），这种通道附件一律 GENERAL。
static struct nvn_gpu_tex_s **g_newimg;   // g_mu
static uint32_t g_newimg_n, g_newimg_cap;
static int newimg_add_locked(struct nvn_gpu_tex_s *t) {
    if (g_newimg_n == g_newimg_cap) {
        uint32_t cap = g_newimg_cap ? g_newimg_cap * 2 : 64;
        struct nvn_gpu_tex_s **p = realloc(g_newimg, (size_t)cap * sizeof *p);
        if (!p) return 0;
        g_newimg = p; g_newimg_cap = cap;
    }
    g_newimg[g_newimg_n++] = t;
    return 1;
}
static void newimg_del_locked(struct nvn_gpu_tex_s *t) {
    for (uint32_t i = 0; i < g_newimg_n; i++) if (g_newimg[i] == t) { g_newimg[i] = g_newimg[--g_newimg_n]; return; }
}
static struct cmd_lay *lay_get(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, int make) {
    for (uint32_t i = 0; i < c->nlay; i++) if (c->lay[i].t == t) return &c->lay[i];
    if (!make) return NULL;
    if (c->nlay == c->laycap) {
        uint32_t cap = c->laycap ? c->laycap * 2 : 32;
        struct cmd_lay *p = realloc(c->lay, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        c->lay = p; c->laycap = cap;
    }
    c->lay[c->nlay].t = t;
    c->lay[c->nlay].cur = VK_IMAGE_LAYOUT_GENERAL;
    return &c->lay[c->nlay++];
}
static VkImageLayout lay_cur(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    struct cmd_lay *e = lay_get(c, t, 0);
    return e ? e->cur : VK_IMAGE_LAYOUT_GENERAL;
}
// 附件这一段用什么布局（只看这一张图与通道本身）
static VkImageLayout pass_att_layout(const nvn_gpu_pass_desc *d, const struct nvn_gpu_tex_s *t, int depth, int opt) {
    if (!opt || t->fb_sample || d->occlusion || (!depth && d->fb_fetch)) return VK_IMAGE_LAYOUT_GENERAL;
    return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
// 采样这张图要用哪个布局：深度模板只有 DEPTH_STENCIL_READ_ONLY 这一种只读布局
static VkImageLayout sample_layout_for(const struct nvn_gpu_tex_s *t) {
    if (!(t->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))) return VK_IMAGE_LAYOUT_GENERAL;
    return (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // 纯模板 S8 也是深度模板图像（#1581 O）
}
// 绑定项里的贴图（采样 / 存储图像；别的类别没有图）
static struct nvn_gpu_tex_s *bind_tex_of(const nvn_gpu_binding *b) {
    struct nvn_gpu_view_s *v = b->cls == NVN_GPU_RES_TEX ? b->u.view
                              : b->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view
                              : b->cls == NVN_GPU_RES_IMAGE ? b->u.view : NULL;
    return v ? v->tex : NULL;
}

// 命令段用完的命令池（带主缓冲与按需分配的前置缓冲）和描述符池留下来给后面的段用，不再每段新建、段完成就销毁。
// 真机取样（一款 3D 游戏，每帧 9 个命令段）：回放线程约 14% 的时间在驱动向内核要显存（kgsl_sharedmem_alloc → ioctl + mmap），
// 开命令段 nvn_gpu_cmd_begin 一项占 6%；每帧新建 9.2 个命令池、5.0 个描述符池。
// 段做完后：命令池、描述符池各放进一张空闲表；开段 / 要描述符池时先从表里取，取出来再重置（vkResetCommandPool / vkResetDescriptorPool）。
// 重置之后上一段分出去的描述符组句柄全部失效——段内的影子（desc_sets、备忘表）都随段对象一起没了，不会带到下一段。
// 表有上限（多出来的照旧销毁），会话收尾全部销毁。诊断开关 nvk_seg_reuse=0 关掉（同一局里对照用），缺省开。
enum { SEGPOOL_CMD_N = 32, SEGPOOL_DESC_N = 64 };
static struct { VkCommandPool pool; VkCommandBuffer cb, init_cb; } g_seg_cmd[SEGPOOL_CMD_N];
static VkDescriptorPool g_seg_desc[SEGPOOL_DESC_N];
static uint32_t g_seg_ncmd, g_seg_ndesc; static uint64_t g_seg_hit_cmd, g_seg_hit_desc;
static int g_seg_closed;   // 会话收尾已经清过表：之后再还回来的一律直接销毁（持 g_seg_mu 读写）
static pthread_mutex_t g_seg_mu = PTHREAD_MUTEX_INITIALIZER;
static int seg_reuse_on(void) { static int cur = -1; static uint32_t k; int c = __atomic_load_n(&cur, __ATOMIC_RELAXED); if (c < 0 || !(__atomic_add_fetch(&k, 1, __ATOMIC_RELAXED) & 63)) { c = nvn_plat_flag("nvk_seg_reuse", 1) != 0; __atomic_store_n(&cur, c, __ATOMIC_RELAXED); } return c; }
static int seg_take_cmd(VkCommandPool *pool, VkCommandBuffer *cb, VkCommandBuffer *init_cb) {
    int ok = 0;
    pthread_mutex_lock(&g_seg_mu);
    if (g_seg_ncmd) { g_seg_ncmd--; *pool = g_seg_cmd[g_seg_ncmd].pool; *cb = g_seg_cmd[g_seg_ncmd].cb; *init_cb = g_seg_cmd[g_seg_ncmd].init_cb; g_seg_hit_cmd++; ok = 1; }
    pthread_mutex_unlock(&g_seg_mu);
    // 重置放在取用的这一刻、由要用它的线程做：放回时（完成线程）就重置的话，两条线程会在驱动里抢同一把锁，
    // 真机上「交后端」从 0.4 涨到 1.5–2.2 毫秒 / 帧
    if (ok && vkResetCommandPool(g.d, *pool, 0) != VK_SUCCESS) { vkDestroyCommandPool(g.d, *pool, NULL); *pool = VK_NULL_HANDLE; *cb = VK_NULL_HANDLE; *init_cb = VK_NULL_HANDLE; ok = 0; }
    return ok;
}
static void seg_give_cmd(VkCommandPool pool, VkCommandBuffer cb, VkCommandBuffer init_cb) {   // 这一段已经做完（或被放弃），池里没有在用的命令缓冲
    if (!pool) return;
    int kept = 0;
    if (cb && seg_reuse_on()) {
        pthread_mutex_lock(&g_seg_mu);
        if (!g_seg_closed && g_seg_ncmd < SEGPOOL_CMD_N) { g_seg_cmd[g_seg_ncmd].pool = pool; g_seg_cmd[g_seg_ncmd].cb = cb; g_seg_cmd[g_seg_ncmd].init_cb = init_cb; g_seg_ncmd++; kept = 1; }
        pthread_mutex_unlock(&g_seg_mu);
    }
    if (!kept) vkDestroyCommandPool(g.d, pool, NULL);   // 池同时回收它的命令缓冲
}
static VkDescriptorPool seg_take_desc(void) {
    VkDescriptorPool p = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_seg_mu);
    if (g_seg_ndesc) { p = g_seg_desc[--g_seg_ndesc]; g_seg_hit_desc++; }
    pthread_mutex_unlock(&g_seg_mu);
    if (p && vkResetDescriptorPool(g.d, p, 0) != VK_SUCCESS) { vkDestroyDescriptorPool(g.d, p, NULL); p = VK_NULL_HANDLE; }   // 同上：取用时重置
    return p;
}
static void seg_give_desc(VkDescriptorPool p) {
    if (!p) return;
    int kept = 0;
    if (seg_reuse_on()) {
        pthread_mutex_lock(&g_seg_mu);
        if (!g_seg_closed && g_seg_ndesc < SEGPOOL_DESC_N) { g_seg_desc[g_seg_ndesc++] = p; kept = 1; }
        pthread_mutex_unlock(&g_seg_mu);
    }
    if (!kept) vkDestroyDescriptorPool(g.d, p, NULL);
}
// 同一张回收的账里再加两样（真机取样：池回收之后回放线程还有约 4.5% 在 nvn_gpu_buf_create → vkAllocateMemory、约 2.5% 在 vkCreateQueryPool）：
// · 命令段的暂存缓冲（arena 的块，多数是 VK_ARENA_MIN 那么大）：整个缓冲对象留下，大小相同的直接再用。它一直留在 g.bufs 链里，
//   会话收尾遍历那条链时一起销毁，所以清表只清计数。暂存区是先写后用的，旧内容不用清。
// · 遮挡查询池：槽数相同的再用；开段时照旧整池重置。
// 暂存缓冲这张表只留标准大小（VK_ARENA_MIN）的块。真机取样（一款 3D 游戏的世界场景，Adreno 840 调试包，开 nvk_flt 的诊断包）：
// 每帧用 24 个标准大小的块，表里却长期占着 28 个非标准大小的块（加载阶段留下的，之后再没有谁要同样的大小，每帧放回的非标准块 0 个），
// 当时表只有 32 个位置，只剩 4 个留给标准块：每帧 11.8 次没命中新建（vkAllocateMemory + 映射）、11.8 次表满销毁（vkFreeMemory）。
// 非标准大小的块不进表、照旧销毁；诊断开关 nvk_stg_std=0 退回旧行为（开会话时读一次，同一局里对照要重开会话），缺省开。
// 池镜像同步的暂存（nvn_gpu_buf_write）另有一条账，见 stg_chunk 一节：一笔写按标准块大小切开，切出来的标准块都走这张表回收，
// 表容量放大到 SEGPOOL_STG_BIG_N 个位置（原先 32 个）。
enum { SEGPOOL_STG_BIG_N = 256, SEGPOOL_Q_N = 16 };
static struct nvn_gpu_buf_s *g_seg_stg[SEGPOOL_STG_BIG_N]; static uint32_t g_seg_nstg;
static int g_seg_stg_std = 1;   // 只留标准大小（开会话时读 nvk_stg_std；持 g_seg_mu 读写）
// 暂存写读数（开会话清零，原子累加）：0 写缓冲笔数；1 写入字节；2 拷贝命令条数；3 一笔超过一个标准块的笔数；4 新建暂存块个数；5 新建暂存块字节
static uint64_t g_stg_wr[6];
static uint64_t g_seg_stg_hit, g_seg_stg_miss, g_seg_stg_kept, g_seg_stg_rej, g_seg_stg_full;   // 取用命中 / 没命中；放回留下 / 因非标准大小没留 / 因表满没留（持 g_seg_mu 读写；会话读数，不随 take 清零）
static struct { VkQueryPool q; uint32_t n; } g_seg_q[SEGPOOL_Q_N]; static uint32_t g_seg_nq;
static struct nvn_gpu_buf_s *seg_take_stg(uint64_t bytes) {
    struct nvn_gpu_buf_s *b = NULL;
    pthread_mutex_lock(&g_seg_mu);
    for (uint32_t i = 0; i < g_seg_nstg; i++) if (g_seg_stg[i]->bytes == bytes) { b = g_seg_stg[i]; g_seg_stg[i] = g_seg_stg[--g_seg_nstg]; break; }
    if (b) g_seg_stg_hit++; else g_seg_stg_miss++;
    pthread_mutex_unlock(&g_seg_mu);
    return b;
}
static int seg_give_stg(struct nvn_gpu_buf_s *b) {   // 1 = 留下了；0 = 没留，调用方照旧销毁
    int kept = 0;
    if (!seg_reuse_on() || !b->mapped) return 0;
    pthread_mutex_lock(&g_seg_mu);
    if (g_seg_closed) { /* 会话收尾已清过表：直接销毁 */ }
    else if (g_seg_stg_std && b->bytes != VK_ARENA_MIN) g_seg_stg_rej++;
    else if (g_seg_nstg >= SEGPOOL_STG_BIG_N) g_seg_stg_full++;
    else { g_seg_stg[g_seg_nstg++] = b; g_seg_stg_kept++; kept = 1; }
    pthread_mutex_unlock(&g_seg_mu);
    return kept;
}
static VkQueryPool seg_take_q(uint32_t n) {
    VkQueryPool q = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_seg_mu);
    for (uint32_t i = 0; i < g_seg_nq; i++) if (g_seg_q[i].n == n) { q = g_seg_q[i].q; g_seg_q[i] = g_seg_q[--g_seg_nq]; break; }
    pthread_mutex_unlock(&g_seg_mu);
    return q;
}
static void seg_give_q(VkQueryPool q, uint32_t n) {
    if (!q) return;
    int kept = 0;
    if (n && seg_reuse_on()) {
        pthread_mutex_lock(&g_seg_mu);
        if (!g_seg_closed && g_seg_nq < SEGPOOL_Q_N) { g_seg_q[g_seg_nq].q = q; g_seg_q[g_seg_nq].n = n; g_seg_nq++; kept = 1; }
        pthread_mutex_unlock(&g_seg_mu);
    }
    if (!kept) vkDestroyQueryPool(g.d, q, NULL);
}
static void seg_drain(void) {
    pthread_mutex_lock(&g_seg_mu);
    for (uint32_t i = 0; i < g_seg_ncmd; i++) vkDestroyCommandPool(g.d, g_seg_cmd[i].pool, NULL);
    for (uint32_t i = 0; i < g_seg_ndesc; i++) vkDestroyDescriptorPool(g.d, g_seg_desc[i], NULL);
    for (uint32_t i = 0; i < g_seg_nq; i++) vkDestroyQueryPool(g.d, g_seg_q[i].q, NULL);
    g_seg_ncmd = g_seg_ndesc = 0; g_seg_nq = 0; g_seg_nstg = 0; g_seg_closed = 1;   // 暂存缓冲在 g.bufs 链里，由收尾遍历销毁
    pthread_mutex_unlock(&g_seg_mu);
}

// ---- 描述符组按布局回收（#1096）
// 每笔绘制要的描述符组原先一组一次 vkAllocateDescriptorSets：真机世界场景每帧 2263 次、约 1.25 毫秒（驱动里每次还有一轮堆分配 / 释放），
// 段做完整池交回回收表、取用时再 vkResetDescriptorPool。这里按布局结构（每个 binding 的类型、数量、阶段逐条相同的布局算同一种）留一串空闲的组：
// alloc_sets 先取空闲的，取空了才向驱动要组：一个表项第一次要 1 组，之后每次要的翻倍（1、2、4、8），封顶 DSET_BATCH 组，多出的留在表里
// （不一上来就分一批：回收表曾按「每条管线的每一组」各记一项，世界场景里上万项大多只被用几次，每项预留 8 组让近十万个组躺在驱动里，真机 300 秒末 :game 进程 TOTAL PSS 多占约 330 MB，见 #1096）；
// 段完成（栅栏已发出：提交前整队清空过，记录必已做完）时
// 把它取过的组整批放回；放弃的段（finish_cmd(c, 0)：两级回放开着时它的记录可能还在队列里）不放回，走 vkFreeDescriptorSets
// （两级回放的包装里整队清空，录制线程不会还在读它）还给池。组从「增长池」分（带 FREE_DESCRIPTOR_SET 位，只增不重置；分满换新池），
// 会话收尾整池销毁。建池失败 / 登记表项时内存不够 / 取用记录放不下 / 碰上完成线程正持锁，一律退回老路径（段自己的池），不拦任何操作。
// 组一旦分出来就留在表里，一直复用到会话收尾，不做「空闲串超过在途峰值就还池」的清扫：真机（高通 Adreno 840）上带 FREE_DESCRIPTOR_SET 位的池
// 里 vkFreeDescriptorSets 之后再分，驱动不复用腾出来的那块显存，而是继续占新的，直到整个池涨到只用不还时的约 4 倍
// （一个 1024 组的池：只分不还 2.85 MB，还了再分一圈一圈涨到 11.4 MB）。清扫版本在稳态里每帧还给池 14 组、再向驱动要 14 组，
// 增长池（10–12 个）越涨越大，300 秒末 :game 进程 TOTAL PSS 多占约 190 MB，见 #1096。
// 总量用 g_dset_cap（缺省 DSET_MAX_SETS）封顶：在用的加空闲的组数到了上限，向驱动要组就拒绝、这一笔退回老路径（读数 dset_capped）。
// 回收的组里留着上一次写的描述符：本笔没写到的槽会一直指着上一段用过的视图 / 缓冲（老路径的新组里这些槽是未初始化的、不指向任何资源）。
// 那些资源之后可能被销毁，而销毁可以发生在这一组已经绑进某一段、那一段还没做完的时候：validation 层把它当违规（VUID-vkDestroyBuffer-buffer-00922，
// 或命令缓冲因「绑定的缓冲已销毁」失效），整套 vk 用例里 C204 臂 G 的旧写法（留着没重写的槽）偶发撞上过这两条。
// 所以每一项记一个「写法」sig：这一笔给这一组写了哪几条描述符（每条的 binding 与数组项号各取散列、求和，与顺序无关）。第一次取到组时定下；
// 之后写法不同的取用不从这一项取、走老路径（段自己的池，新组里没有残留）；写法相同的取用会把上一次写过的槽全部重写，不会留下旧描述符。
// （一种布局结构的写法由着色器声明的绑定决定、游戏必须全绑上，稳态下恒定，共用这个布局的各条管线写法相同；写法不同的只是回到回收前的做法，读数 dset_sigmiss 记次数。）
// 布局不再每条管线各建各的：结构相同（每个 binding 的类型、数量、阶段逐条相同）的 VkDescriptorSetLayout 全后端只建一个（布局表 struct dset_lay，见下），
// 句柄共用，不依赖「定义相同的布局互相兼容」那条规范例外：几条管线的管线布局里放的就是同一个句柄。布局只在会话收尾销毁（管线销毁只是不再用它，
// 建管线失败时也只是不再用），所以回收表不用跟着布局摘表。
// 调试开关 nvk_dset_rec（开会话时读一次，缺省 1；0 = 老路径，A/B 与负对照用）。
// 回收表项（struct dset_bin）：每个布局结构一项。第一次取这个布局的组时登记进 g_dset_bins（头插的单链表）、指针记在布局表项里（dset_lay.bin），
// 取用记录也带着它（dset_use.bin）：之后取组、放回都直接用这个指针，全程没有「按布局句柄查找」。链表只在读数 / 收尾时遍历；
// 表项数 = 本会话用到过的布局结构数，没有桶、没有上限，不会因为满了而退化。
// （走过的两条弯路：① 按布局句柄散列的定长表：世界场景里 8192 项挤在 1024 个桶里，平均链长约 8，每次取 / 放沿链走、每一跳一次缓存未命中，
// 真机取样（b19、直连）占回放线程 2.5%、全部取组路径 3.3%；② 每条管线的每一组各记一项：世界场景约 4300 条管线、1.2 万项，每项留着几组，
// 空闲的组近十万个躺在驱动里（真机 300 秒末 :game 进程 TOTAL PSS 多占 200–330 MB），而同一结构的所有管线完全可以从同一串里取、往同一串里放，见 #1096。）
// 表项在一个会话内只增不减（只在 dset_close 里销毁），指针在会话内恒有效；代际 g_dset_gen（每次开会话 +1）挡住「表已销毁、布局表项里还留着旧指针」。
enum { DSET_BATCH = 8, DSET_POOL_SETS = 1024, DSET_POOL_PER_TYPE = 4096, DSET_FREE_RUN = 256, DSET_MAX_SETS = 16384 };
struct dset_ent { VkDescriptorSet set; VkDescriptorPool pool; };
struct dset_bin {
    struct dset_bin *next; VkDescriptorSetLayout dsl;   // next：登记表（g_dset_bins）里的下一项；dsl：这一项的布局（向驱动要组时用）
    struct dset_ent *free; uint32_t nfree, cap;       // 空闲串（栈：后放回的先取）
    uint8_t batch;                                    // 下一次向驱动要几组（0 当作 1，每次翻倍、封顶 DSET_BATCH）
    uint8_t solo, dead;                               // solo：一批 DSET_BATCH 组放不进空池，改一组一组分；dead：一组也放不进空池，这个布局永远走老路径
    uint8_t sig_set;                                  // sig 定下了吗（第一次取到组时定）
    uint64_t sig;                                     // 这一项的组每次被写的「写法」：本笔写了哪几条描述符（binding + 数组项号，求和），见 dset_take
};
static pthread_mutex_t g_dset_mu = PTHREAD_MUTEX_INITIALIZER;   // 保护下面全部：回收表、增长池表、计数；对增长池的分配 / 释放也在它下面串行（Vulkan 要求池外部同步）
static struct dset_bin *g_dset_bins;   // 登记表：本会话登记过的全部表项（头插的单链表）
static uint32_t g_dset_nbins, g_dset_npools, g_dset_pools_cap;
static VkDescriptorPool *g_dset_pools, g_dset_cur;
static int g_dset_on, g_dset_closed = 1, g_dset_test_reject, g_dset_test_failreg;   // g_dset_test_failreg：用例注入，下 n 次登记表项当作内存不够（生产恒为 0，持 g_dset_mu 读写）
static uint32_t g_dset_gen;   // 回收表代际：每次开会话 +1（持 g_dset_mu 改）
static uint64_t g_dset_new, g_dset_reuse, g_dset_put, g_dset_freed, g_dset_taken;   // 开会话起累计：向驱动新分的组 / 从表里复用的组 / 段完成放回的组 / 放弃的段还给池的组 / 交给 alloc_sets 的组（复用 + 新分的第一组）
static uint64_t g_dset_busy;   // 回放线程取组时碰上锁被持着、退回老路径的次数（原子累加，不在锁里）
static uint64_t g_dset_sigmiss;   // 开会话起累计：这一笔的写法和表项里定下的不同、退回老路径的次数（持 g_dset_mu 改）
static uint64_t g_dset_capped;    // 开会话起累计：向驱动要组时在用的加空闲的组数已到 g_dset_cap、退回老路径的次数（持 g_dset_mu 改）
static uint32_t g_dset_cap = DSET_MAX_SETS;   // 总量上限（持 g_dset_mu 读写）：生产恒为 DSET_MAX_SETS，用例用 nvn_vk_dbg_dset_cap 改小，开会话时复位
// 布局表（#1096）：建管线时每一组的布局先按「结构」在这里找，找到就共用，没有才向驱动建一个。键 = 按 binding 升序排好的 (binding, 类型, 数量, 阶段) 数组
// （binding 编号不同、类型 / 数量 / 阶段任何一项不同都是不同的布局）；散列表按条数翻倍（装填因子 ≤ 1），不会因为满了而退化。
// 表项归布局表所有，会话收尾（所有管线销毁之后）才销毁布局；管线里只借用句柄。
struct dset_lb { uint32_t binding, type, count, stages; };
struct dset_lay {
    struct dset_lay *all_next, *hnext;   // all_next：全部表项（收尾销毁、翻倍重排用）；hnext：散列桶链
    uint64_t hash;
    VkDescriptorSetLayout dsl;
    struct dset_bin *bin; uint32_t bin_gen;   // 这个布局的回收表项（持 g_dset_mu 读写）；bin_gen != g_dset_gen = 回收表已随会话销毁，指针作废
    uint32_t nb;
    struct dset_lb b[];                  // nb 项，按 binding 升序
};
static pthread_mutex_t g_dlay_mu = PTHREAD_MUTEX_INITIALIZER;   // 保护布局表本身（散列表、链、计数）；表项里的 bin / bin_gen 归 g_dset_mu
static struct dset_lay **g_dlay_ht, *g_dlay_all;
static uint32_t g_dlay_ht_n, g_dlay_n;
static uint64_t g_dlay_ask, g_dlay_hit;   // 开会话起累计：建管线时取布局的次数 / 其中命中已有表项的次数（持 g_dlay_mu 改）
static uint64_t g_dlay_made, g_dlay_gone; // 进程内累计（不随会话清零）：向驱动建过的布局数 / 收尾时销毁的布局数；会话收尾后两者必须相等（持 g_dlay_mu 改）
// 恒等式（所有段都结束后、没有内存不足漏掉的组时）：taken == put + freed（交出去的组要么放回、要么还给池）；
// new == idle + freed（向驱动要来的每一组：在空闲串里 / 被放弃的段还给池，一组不多一组不少）。
static struct dset_bin *dset_bin_new(VkDescriptorSetLayout l) {   // 持 g_dset_mu；登记一项，NULL = 内存不够
    if (g_dset_test_failreg > 0) { g_dset_test_failreg--; return NULL; }
    struct dset_bin *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->dsl = l; b->next = g_dset_bins; g_dset_bins = b; g_dset_nbins++;
    return b;
}
static int dset_bin_push(struct dset_bin *b, VkDescriptorSet set, VkDescriptorPool pool) {   // 持 g_dset_mu；0 = 内存不够
    if (b->nfree == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 16;
        struct dset_ent *n = realloc(b->free, cap * sizeof *n);
        if (!n) return 0;
        b->free = n; b->cap = cap;
    }
    b->free[b->nfree].set = set; b->free[b->nfree].pool = pool; b->nfree++;
    return 1;
}
// 把同一个池的一串组还给池（持 g_dset_mu：对同一个池的分配 / 释放不能并发）
static void dset_free_run(VkDescriptorPool pool, const VkDescriptorSet *sets, uint32_t n) {
    if (n) (void)vkFreeDescriptorSets(g.d, pool, n, sets);
}
static int dset_new_pool(void) {   // 持 g_dset_mu
    VkDescriptorPoolSize ps[9] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DSET_POOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_SAMPLER, DSET_POOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DSET_POOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, DSET_POOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DSET_POOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DSET_POOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, DSET_POOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, DSET_POOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, DSET_POOL_PER_TYPE}};
    if (g_dset_npools == g_dset_pools_cap) {
        uint32_t cap = g_dset_pools_cap ? g_dset_pools_cap * 2 : 8;
        VkDescriptorPool *np = realloc(g_dset_pools, cap * sizeof *np);
        if (!np) return 0;
        g_dset_pools = np; g_dset_pools_cap = cap;
    }
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = DSET_POOL_SETS;
    dpi.poolSizeCount = 9;
    dpi.pPoolSizes = ps;
    VkDescriptorPool p = VK_NULL_HANDLE;
    OC(OC_DPOOL);
    if (vkCreateDescriptorPool(g.d, &dpi, NULL, &p) != VK_SUCCESS) return 0;
    g_dset_pools[g_dset_npools++] = p;
    g_dset_cur = p;
    return 1;
}
// 向驱动要一批组：第一个给调用方，其余留在该布局的空闲串里。要几组看表项的 batch：第一次 1 组，之后每次翻倍（1、2、4、8），封顶 DSET_BATCH，
// 所以只被用过几次的表项手里不会攥着一批用不上的组，常用的表项几次翻倍后就和一次要 8 组一样。
// 0 = 驱动不给、或已到总量上限（调用方退回老路径）。持 g_dset_mu
// 当前池分满（池耗尽 / 碎片）就换新池重分；新池（空池）也放不下一批，说明这个布局的组太大：改成一组一组分，一组也放不下就把这个布局标成 dead（永远走老路径，
// 不再每次都建一个新池）。总量上限是软的：到顶的那一次最多多分 DSET_BATCH - 1 组。
static int dset_refill(struct dset_bin *b, VkDescriptorSet *first, VkDescriptorPool *from) {
    if (b->dead) return 0;
    if (g_dset_new - g_dset_freed >= g_dset_cap) { g_dset_capped++; return 0; }   // 在用的加空闲的组数 = 向驱动要来的减去还给池的
    VkDescriptorSetLayout lay[DSET_BATCH]; VkDescriptorSet sets[DSET_BATCH];
    for (int i = 0; i < DSET_BATCH; i++) lay[i] = b->dsl;
    uint32_t want = b->solo ? 1 : b->batch ? b->batch : 1;
    int fresh = 0;   // 本次调用里用的是新建的池：分不出来就说明这一批（一组）本身超过了空池的容量
    for (int tries = 0; tries < 4; tries++) {
        if (!g_dset_cur) { if (!dset_new_pool()) return 0; fresh = 1; }
        VkDescriptorSetAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = g_dset_cur;
        ai.descriptorSetCount = want;
        ai.pSetLayouts = lay;
        OC(OC_DALLOC);
        const int inj = __atomic_load_n(&g_dset_test_reject, __ATOMIC_RELAXED);   // 用例注入（见 nvn_vk_dbg_dset_inject），生产恒为 0
        VkResult r = (inj == 2 || (inj == 1 && want > 1)) ? VK_ERROR_OUT_OF_POOL_MEMORY : vkAllocateDescriptorSets(g.d, &ai, sets);
        if (r == VK_SUCCESS) {
            *first = sets[0]; *from = g_dset_cur;
            for (uint32_t i = 1; i < want; i++) if (!dset_bin_push(b, sets[i], g_dset_cur)) break;   // 内存不够只会少留几组：留不下的随池一起销毁
            g_dset_new += want;
            if (!b->solo && want < DSET_BATCH) b->batch = (uint8_t)(2 * want);   // 下一次要的翻倍
            return 1;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return 0;
        if (fresh) {
            if (want > 1) { want = 1; b->solo = 1; continue; }   // 同一个空池，改一组一组分再试
            b->dead = 1;
            return 0;
        }
        g_dset_cur = VK_NULL_HANDLE;   // 当前池分满：它留在 g_dset_pools 里（组还有在用的），下一轮换新池
    }
    return 0;
}
// alloc_sets 的快路：1 = 取到了（已记进本段的取用记录），0 = 退回老路径。
// 用 trylock：完成线程在放回 / 还池时会持锁，回放线程是关键路径，碰上就直接走老路径（多一次驱动分配），不等锁。
// sig：这一笔给这一组写的「写法」（write_binds 求出，见上面的说明）。表项里还没定写法就用这一笔的；已经定了而这一笔不同，就不从这一项取：
// 表项里的组都只被这一种写法写过，换一种写法去写，没被重写的槽会留着上一次的描述符。
static int dset_take(struct nvn_gpu_cmd_s *c, struct nvn_gpu_pipe_s *p, uint32_t set, uint64_t sig, VkDescriptorSet *out) {
    int ok = 0;
    if (pthread_mutex_trylock(&g_dset_mu) != 0) { __atomic_fetch_add(&g_dset_busy, 1, __ATOMIC_RELAXED); return 0; }
    struct dset_bin *b = NULL;
    struct dset_lay *e = set < 4 ? p->dlay[set] : NULL;
    if (!g_dset_closed && e) {
        if (e->bin_gen != g_dset_gen) { e->bin = NULL; e->bin_gen = g_dset_gen; }   // 回收表是上个会话的：这个布局记的指针作废
        b = e->bin;
        if (!b) b = e->bin = dset_bin_new(e->dsl);   // 这个布局第一次取组：登记。内存不够（NULL）不记，这次退回老路径，下次再试
    }
    if (b && b->sig_set && b->sig != sig) { g_dset_sigmiss++; b = NULL; }   // 写法不同：不从这一项取（老路径）
    if (b && c->ndsu == c->dsu_cap) {   // 先给本段的取用记录留好位置，之后取到的组一定记得下来
        uint32_t cap = c->dsu_cap ? c->dsu_cap * 2 : 256;
        struct dset_use *n = realloc(c->dsu, cap * sizeof *n);
        if (n) { c->dsu = n; c->dsu_cap = cap; } else b = NULL;
    }
    if (b) {
        VkDescriptorSet s = VK_NULL_HANDLE; VkDescriptorPool from = VK_NULL_HANDLE;
        if (b->nfree) { s = b->free[--b->nfree].set; from = b->free[b->nfree].pool; g_dset_reuse++; ok = 1; }
        else ok = dset_refill(b, &s, &from);
        if (ok) {
            if (!b->sig_set) { b->sig = sig; b->sig_set = 1; }   // 第一次取到组：这一项的写法定为这一笔的
            if (!c->ndsu) c->dsu_gen = g_dset_gen;
            c->dsu[c->ndsu].bin = b; c->dsu[c->ndsu].set = s; c->dsu[c->ndsu].pool = from; c->ndsu++; *out = s; g_dset_taken++;
        }
    }
    pthread_mutex_unlock(&g_dset_mu);
    return ok;
}
// 段结束（finish_cmd）：执行过的段把取过的组整批放回各自表项的空闲串；放弃的段 / 空闲串放不下（内存不够）的，还给池
static void dset_release(struct nvn_gpu_cmd_s *c, int executed) {
    if (c->ndsu) {
        VkDescriptorSet run[DSET_FREE_RUN]; uint32_t nrun = 0; VkDescriptorPool run_pool = VK_NULL_HANDLE;
        pthread_mutex_lock(&g_dset_mu);
        const int live = !g_dset_closed && c->dsu_gen == g_dset_gen;   // 池已随会话销毁（或已换了新会话）：组跟着没了，不能再碰；锁里这个结论不会变
        for (uint32_t i = 0; i < c->ndsu; i++) {
            const struct dset_use *u = &c->dsu[i];
            if (!live) break;
            struct dset_bin *b = u->bin;
            if (executed && dset_bin_push(b, u->set, u->pool)) { g_dset_put++; continue; }
            if (nrun && (u->pool != run_pool || nrun == DSET_FREE_RUN)) { dset_free_run(run_pool, run, nrun); g_dset_freed += nrun; nrun = 0; }
            run_pool = u->pool; run[nrun++] = u->set;
        }
        if (nrun && live) { dset_free_run(run_pool, run, nrun); g_dset_freed += nrun; }
        pthread_mutex_unlock(&g_dset_mu);
    }
    free(c->dsu); c->dsu = NULL; c->ndsu = c->dsu_cap = 0;
}
static uint64_t dset_idle_total(void) {   // 空闲串里现有的组
    uint64_t n = 0;
    pthread_mutex_lock(&g_dset_mu);
    for (const struct dset_bin *b = g_dset_bins; b; b = b->next) n += b->nfree;
    pthread_mutex_unlock(&g_dset_mu);
    return n;
}
static void dset_open(void) {
    const int on = nvn_plat_flag("nvk_dset_rec", 1) != 0;
    pthread_mutex_lock(&g_dset_mu);
    __atomic_store_n(&g_dset_on, on, __ATOMIC_RELAXED); g_dset_closed = 0;
    g_dset_gen++;   // 新会话的表是空的：上个会话里管线记的表项指针全部作废
    g_dset_new = g_dset_reuse = g_dset_put = g_dset_freed = g_dset_taken = g_dset_sigmiss = g_dset_capped = 0; g_dset_cap = DSET_MAX_SETS;
    __atomic_store_n(&g_dset_busy, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_dset_mu);
}
static void dset_close(void) {   // 会话收尾：所有段都完成了、设备空闲；整池销毁（组随池一起没了）
    pthread_mutex_lock(&g_dset_mu);
    g_dset_closed = 1;
    for (struct dset_bin *b = g_dset_bins, *nx; b; b = nx) { nx = b->next; free(b->free); free(b); }
    g_dset_bins = NULL; g_dset_nbins = 0;
    for (uint32_t i = 0; i < g_dset_npools; i++) vkDestroyDescriptorPool(g.d, g_dset_pools[i], NULL);
    free(g_dset_pools); g_dset_pools = NULL; g_dset_npools = g_dset_pools_cap = 0; g_dset_cur = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_dset_mu);
}

// 贴图显存块的小池。动态分辨率的游戏每帧换渲染目标的宽高，每张新图都向驱动要一块显存、旧图拆的时候还回去：
// 一款 3D 游戏每帧 10 张时「要显存并绑定」1.24 毫秒 + 拆 0.41 毫秒（直接计时）。拆贴图时把显存块留在池里，
// 新图要的大小落在某块的 [块的 4/5, 块] 之间、内存类型相同就直接绑它（图已经拆了的显存块可以再绑给别的图）。
// 池按先进先出，最多 TEXMEM_N 块、TEXMEM_BYTES 字节，超了把最老的还给驱动；会话收尾全部还掉。
// 诊断开关 nvk_texmem=0 关掉（同一局里对照用），缺省开。
enum { TEXMEM_N = 48 };
#define TEXMEM_BYTES (256ull << 20)
static struct { VkDeviceMemory m; VkDeviceSize size; uint32_t type; } g_texmem[TEXMEM_N];
static uint32_t g_texmem_n; static VkDeviceSize g_texmem_bytes; static uint64_t g_texmem_hit, g_texmem_miss;
static pthread_mutex_t g_texmem_mu = PTHREAD_MUTEX_INITIALIZER;
static int texmem_on(void) { static int cur = -1; static uint32_t k; if (cur < 0 || !(++k & 63)) cur = nvn_plat_flag("nvk_texmem", 1) != 0; return cur; }
static VkDeviceMemory texmem_take(VkDeviceSize need, uint32_t type, VkDeviceSize *size) {
    VkDeviceMemory m = VK_NULL_HANDLE; int best = -1;
    pthread_mutex_lock(&g_texmem_mu);
    for (uint32_t i = 0; i < g_texmem_n; i++)
        if (g_texmem[i].type == type && g_texmem[i].size >= need && g_texmem[i].size - g_texmem[i].size / 5 <= need &&
            (best < 0 || g_texmem[i].size < g_texmem[best].size)) best = (int)i;
    if (best >= 0) { m = g_texmem[best].m; *size = g_texmem[best].size; g_texmem_bytes -= g_texmem[best].size;
        memmove(&g_texmem[best], &g_texmem[best + 1], (g_texmem_n - (uint32_t)best - 1) * sizeof g_texmem[0]); g_texmem_n--; g_texmem_hit++; }
    else g_texmem_miss++;
    pthread_mutex_unlock(&g_texmem_mu);
    return m;
}
static void texmem_give(VkDevice d, VkDeviceMemory m, VkDeviceSize size, uint32_t type) {
    if (!m) return;
    if (!size || size > TEXMEM_BYTES / 4 || !texmem_on()) { vkFreeMemory(d, m, NULL); return; }
    VkDeviceMemory out[TEXMEM_N + 1]; uint32_t nout = 0;
    pthread_mutex_lock(&g_texmem_mu);
    while (g_texmem_n && (g_texmem_n == TEXMEM_N || g_texmem_bytes + size > TEXMEM_BYTES)) {   // 最老的在前
        out[nout++] = g_texmem[0].m; g_texmem_bytes -= g_texmem[0].size;
        memmove(&g_texmem[0], &g_texmem[1], (g_texmem_n - 1) * sizeof g_texmem[0]); g_texmem_n--; }
    g_texmem[g_texmem_n].m = m; g_texmem[g_texmem_n].size = size; g_texmem[g_texmem_n].type = type; g_texmem_n++; g_texmem_bytes += size;
    pthread_mutex_unlock(&g_texmem_mu);
    for (uint32_t i = 0; i < nout; i++) vkFreeMemory(d, out[i], NULL);
}
static void texmem_drain(VkDevice d) {
    pthread_mutex_lock(&g_texmem_mu);
    for (uint32_t i = 0; i < g_texmem_n; i++) vkFreeMemory(d, g_texmem[i].m, NULL);
    g_texmem_n = 0; g_texmem_bytes = 0;
    pthread_mutex_unlock(&g_texmem_mu);
}

// 调用方持 g_mu。贴图 / 视图没有 retired 回调，最后一次引用结束即可销毁。
static void destroy_tex_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t) {
    newimg_del_locked(t);
    struct nvn_gpu_tex_s **pp = &vk->texs;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
    LIFE_T0;
    pc_drop_view(t->view); LIFE_ADD(6);
    if (t->view) vkDestroyImageView(vk->d, t->view, NULL);
    if (t->image) vkDestroyImage(vk->d, t->image, NULL);
    texmem_give(vk->d, t->memory, t->mem_size, t->mem_type);
    LIFE_ADD(7);
    if (t->bc_sbuf) vkDestroyBuffer(vk->d, t->bc_sbuf, NULL);
    if (t->bc_smem) vkFreeMemory(vk->d, t->bc_smem, NULL);
    free(t);
}

// ---- 视图全局链 vk->views：头插 + 反向指针，摘链 O(1) ----
// 原先 destroy_view_locked 摘链是从链头线性找自己（单向链）：世界场景现存视图 2.3 万个、每帧拆 56 个，整段在持 g_mu 时走；
// 真机取样（旧路径，世界阶段、去掉托底线程后 43303 个样本）：销毁视图的函数 9467 个 = 21.9%，其中这个摘链循环 9341 个 = 21.6%；两条游戏工作线程里它分别占各自样本的 65.2% / 71.1%（见 docs/坑记录.md）。
// all_pprev 指向「指着我的那个指针」，摘自己不再要找。诊断开关 nvk_ulist=0 退回旧的线性找（同一局里对照用；反向指针照旧维护，
// 运行中来回切也安全），缺省开。调用方持 g_mu。
static uint64_t g_dv_steps_total;   // 旧路径走过的节点数（会话累计；用例读，缺省路径恒为 0）
static int g_ulist_cur = -1, g_fbdrop_cur = -1;   // 开关缓存（-1 = 还没读）；每 256 次调用重读一次，用例钩子（nvn_vk_dbg_view_stat 5）可立刻作废
static int view_ulist_on(void) { static uint32_t k; if (g_ulist_cur < 0 || !(++k & 255)) g_ulist_cur = nvn_plat_flag("nvk_ulist", 1) != 0; return g_ulist_cur; }
static void view_link_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v) {
    v->all_next = vk->views;
    if (vk->views) vk->views->all_pprev = &v->all_next;
    v->all_pprev = &vk->views;
    vk->views = v;
}
static void view_unlink_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v) {
    struct nvn_gpu_view_s **pp;
    if (view_ulist_on()) {
        pp = v->all_pprev;
        if (!pp) return;   // 没链在表上（整表释放之后才会误调到）：不当成 O(N) 去找
    } else {
        uint64_t steps = 0;
        pp = &vk->views;
        while (*pp && *pp != v) { pp = &(*pp)->all_next; steps++; }
        __atomic_fetch_add(&g_dv_steps_total, steps, __ATOMIC_RELAXED);
        if (!*pp) return;
    }
    *pp = v->all_next;
    if (v->all_next) v->all_next->all_pprev = pp;
    v->all_next = NULL;
    v->all_pprev = NULL;
}
// 诊断读数（nvk_gputime 开着才计）：销毁视图的次数、整个函数的用时、其中摘链的用时；见 nvn_gpu_present 里的「视图销毁」那一行。
static uint64_t g_dv_ns[2], g_dv_n;

static void destroy_view_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v) {
    const uint64_t dv0 = g_draw_tm ? now_ns() : 0;
    view_unlink_locked(vk, v);
    const uint64_t dv1 = g_draw_tm ? now_ns() : 0;
    LIFE_T0;
    pc_drop_view_fast(v, v->att); pc_drop_view_fast(v, v->img); pc_drop_view_fast(v, v->view); LIFE_ADD(6);
    if (v->att) vkDestroyImageView(vk->d, v->att, NULL);
    if (v->att_n) vkDestroyImageView(vk->d, v->att_n, NULL);
    if (v->img) vkDestroyImageView(vk->d, v->img, NULL);
    for (unsigned i = 0; i < 2; i++) if (v->depth_blit[i]) vkDestroyImageView(vk->d, v->depth_blit[i], NULL);
    if (v->view) vkDestroyImageView(vk->d, v->view, NULL);
    LIFE_ADD(5);
    if (v->buffer_view) vkDestroyBufferView(vk->d, v->buffer_view, NULL);
    struct nvn_gpu_tex_s *t = v->tex;
    struct nvn_gpu_buf_s *b = v->buf;
    free(v);
    if (b) unref_locked(vk, b, REF_BUF);
    if (t) {
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0) destroy_tex_locked(vk, t);
    }
    if (g_draw_tm && dv0) {
        const uint64_t dv2 = now_ns();
        __atomic_fetch_add(&g_dv_ns[0], dv2 - dv0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_dv_ns[1], dv1 - dv0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_dv_n, 1, __ATOMIC_RELAXED);
    }
}

static void destroy_samp_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_samp_s *s) {
    struct nvn_gpu_samp_s **pp = &vk->samps;
    while (*pp && *pp != s) pp = &(*pp)->all_next;
    if (*pp) *pp = s->all_next;
    vk->border_live -= s->border_slots;
    if (s->average_sampler) vkDestroySampler(vk->d, s->average_sampler, NULL);
    if (s->int_sampler && s->int_sampler != s->sampler) vkDestroySampler(vk->d, s->int_sampler, NULL);
    if (s->sampler) vkDestroySampler(vk->d, s->sampler, NULL);
    free(s);
}

static int ref_released(void *obj, int type) {
    switch (type) {
    case REF_BUF: return ((struct nvn_gpu_buf_s *)obj)->released;
    case REF_TEX: return ((struct nvn_gpu_tex_s *)obj)->released;
    case REF_VIEW: return ((struct nvn_gpu_view_s *)obj)->released;
    case REF_SAMP: return ((struct nvn_gpu_samp_s *)obj)->released;
    case REF_POOL: return ((struct nvn_gpu_pool_table_s *)obj)->released;
    default: return 1;
    }
}

static void ref_bump(void *obj, int type) {
    switch (type) {
    case REF_BUF: ((struct nvn_gpu_buf_s *)obj)->refs++; break;
    case REF_TEX: ((struct nvn_gpu_tex_s *)obj)->refs++; break;
    case REF_VIEW: ((struct nvn_gpu_view_s *)obj)->refs++; break;
    case REF_SAMP: ((struct nvn_gpu_samp_s *)obj)->refs++; break;
    case REF_POOL: ((struct nvn_gpu_pool_table_s *)obj)->refs++; break;
    }
}

static void ref_drop(void *obj, int type) {
    switch (type) {
    case REF_BUF: if (((struct nvn_gpu_buf_s *)obj)->refs > 0) ((struct nvn_gpu_buf_s *)obj)->refs--; break;
    case REF_TEX: if (((struct nvn_gpu_tex_s *)obj)->refs > 0) ((struct nvn_gpu_tex_s *)obj)->refs--; break;
    case REF_VIEW: if (((struct nvn_gpu_view_s *)obj)->refs > 0) ((struct nvn_gpu_view_s *)obj)->refs--; break;
    case REF_SAMP: if (((struct nvn_gpu_samp_s *)obj)->refs > 0) ((struct nvn_gpu_samp_s *)obj)->refs--; break;
    case REF_POOL: if (((struct nvn_gpu_pool_table_s *)obj)->refs > 0) ((struct nvn_gpu_pool_table_s *)obj)->refs--; break;
    }
}

// 段内「已引用句柄」备忘（#1194）。回放核心每笔绘制都把整张绑定表交给 nvn_gpu_bind，表里每一项各 cmd_ref 一次：取 g_mu、查 released、
// 计数 +1、追加引用表，段完成时再逐项 unref 一遍。真机世界场景每帧 1.7 万次取锁（每笔 13–16 次），而相邻几百笔绑的是同一批句柄。
// 同一段里同一个句柄只要引用过一次，段就一直持有它的一份引用（引用表在段完成时才归还）——再引用多少次，都不改「它什么时候能销毁」。
// 所以第二次起可以直接返回，条件是：①本段已成功引用过它；②那之后没有任何句柄被 release 过（release 才会让 released 变真，
// 这是原路径里唯一会让同一个句柄第二次引用结果不同的事）。②用全局 release 代号判：每次 release（buf / tex / view / samp，都已持 g_mu）加一，
// 备忘记着建表时的代号，对不上就整表清空，之后的引用走原路径（release 之后仍在使用照旧报粘滞 E_ARG）。
// 备忘里的句柄本段都持着引用，不会被销毁，也就没有「地址复用给新句柄、误命中」的可能。直接映射：撞槽只是多走一次原路径。
// 没有用户开关，备忘恒开；宿主用例的对照档靠测试钩子 nvn_gpu_vk_test_ref_seen（文件末）把它关掉，让每次都走原路径。
enum { REF_SEEN_BITS = 10, REF_SEEN_N = 1 << REF_SEEN_BITS };
static uint64_t g_rel_epoch;                 // 有句柄被 release 的次数（持 g_mu 加；回放线程无锁读）
static int g_ref_seen = 1;                   // 恒 1；只有测试钩子会改（改的时候没有会话在录制）
static uint64_t g_ref_n, g_ref_hit;          // 读数（nvk_gputime 开着才计）：进 cmd_ref_at 的次数 / 备忘命中次数
static inline void rel_epoch_bump(void) { __atomic_fetch_add(&g_rel_epoch, 1, __ATOMIC_RELEASE); }   // 调用方持 g_mu，紧跟在 released = 1 之后
// 录制点（函数名 + 行号）随报错带出：「release 之后仍在使用」只在真机长局里出现（#937），
// 光凭一句话分不清是哪类句柄、哪条录制路径，普查报告里这一行就要能定位。
#define cmd_ref(c, obj, type) cmd_ref_at((c), (obj), (type), __func__, __LINE__)
// 已持一份引用后登记到本段；分配失败时就地归还这份引用并记 OOM。线程：[提交]。
// 池表用自己的一步式取引用（见 backend_vulkan_pool.inc 的 pool_cmd_ref），也走这里登记。
static int cmd_ref_append(struct nvn_gpu_cmd_s *c, void *obj, int type) {
    if (c->nrefs == c->refcap) {
        uint32_t cap = c->refcap ? c->refcap * 2 : 16;
        struct vk_ref *p = realloc(c->refs, (size_t)cap * sizeof *p);
        if (!p) {   // 记录失败视为 OOM：整段不提交；刚加的引用要当场归还
            pthread_mutex_lock(&g_mu);
            ref_drop(obj, type);
            pthread_mutex_unlock(&g_mu);
            set_sticky(c, NVN_GPU_E_OOM, "引用表分配失败");
            return 0;
        }
        c->refs = p;
        c->refcap = cap;
    }
    c->refs[c->nrefs].obj = obj;
    c->refs[c->nrefs].type = type;
    c->nrefs++;
    return 1;
}
static void cmd_ref_at(struct nvn_gpu_cmd_s *c, void *obj, int type, const char *fn, int line) {
    if (!obj) return;
    uint64_t tag = 0, ep = 0;
    unsigned slot = 0;
    if (g_ref_seen) {
        ep = __atomic_load_n(&g_rel_epoch, __ATOMIC_ACQUIRE);
        tag = (uint64_t)(uintptr_t)obj | (uint64_t)type;   // 句柄来自 malloc，16 字节对齐，低 4 位空着放类型（1..4）
        slot = (unsigned)(((uint32_t)((uintptr_t)obj >> 4) * 2654435761u) >> (32 - REF_SEEN_BITS));
        if (c->refseen) {
            if (c->refseen_epoch != ep) { memset(c->refseen, 0, REF_SEEN_N * sizeof *c->refseen); c->refseen_epoch = ep; }
            else if (c->refseen[slot] == tag) {
                c->refseen_hits++;
                if (g_draw_tm) { __atomic_fetch_add(&g_ref_n, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_ref_hit, 1, __ATOMIC_RELAXED); }
                return;
            }
        }
    }
    if (g_draw_tm) __atomic_fetch_add(&g_ref_n, 1, __ATOMIC_RELAXED);
    gmu_lock_rt();
    if (ref_released(obj, type)) {   // release 之后再录制使用 = 契约违规（粘滞 E_ARG）
        pthread_mutex_unlock(&g_mu);
        static const char *const names[] = {"?", "buf", "tex", "view", "samp"};
        set_sticky(c, NVN_GPU_E_ARG, "release 之后仍在使用句柄（%s %p，录制于 %s:%d）",
                   type >= REF_BUF && type <= REF_SAMP ? names[type - REF_BUF + 1] : names[0], obj, fn, line);
        return;
    }
    ref_bump(obj, type);
    pthread_mutex_unlock(&g_mu);
    if (!cmd_ref_append(c, obj, type)) return;
    if (g_ref_seen) {
        // 表的代号就是进函数时读到的 ep（进来时表已存在的话上面已经对齐到 ep）：进函数之后若有句柄被 release，代号已经变了，
        // 下一次进来会发现对不上、整表清空。这一项本身是持锁检查通过之后才记的（检查时它若已被 release，上面已经报错返回了）。
        if (!c->refseen) {
            c->refseen = calloc(REF_SEEN_N, sizeof *c->refseen);   // 分配不到就不备忘，每次走原路径，答案不变
            c->refseen_epoch = ep;
        }
        if (c->refseen) c->refseen[slot] = tag;
    }
}

static int fbpool_put_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t);   // backend_vulkan_feedback.inc
static void fbpool_drain_locked(struct nvn_gpu_vk *vk);
// 调用方持 g_mu。
static void unref_locked(struct nvn_gpu_vk *vk, void *obj, int type) {
    if (!obj) return;
    if (type == REF_BUF) {
        struct nvn_gpu_buf_s *b = obj;
        if (b->refs > 0) b->refs--;
        if (b->released && b->refs == 0) queue_retire_locked(vk, b);
        return;
    }
    if (type == REF_TEX) {
        struct nvn_gpu_tex_s *t = obj;
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0 && !fbpool_put_locked(vk, t)) destroy_tex_locked(vk, t);
        return;
    }
    if (type == REF_VIEW) {
        struct nvn_gpu_view_s *v = obj;
        if (v->refs > 0) v->refs--;
        if (v->released && v->refs == 0) destroy_view_locked(vk, v);
        return;
    }
    if (type == REF_POOL) {
        struct nvn_gpu_pool_table_s *t = obj;
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0) pool_destroy_locked(t);
        return;
    }
    if (type == REF_SAMP) {
        struct nvn_gpu_samp_s *s = obj;
        if (s->refs > 0) s->refs--;
        if (s->released && s->refs == 0) destroy_samp_locked(vk, s);
    }
}

static void unlink_buf_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    struct nvn_gpu_buf_s **pp = &vk->bufs;
    while (*pp && *pp != b) pp = &(*pp)->all_next;
    if (*pp) *pp = b->all_next;
    b->all_next = NULL;
}

static void retire_one(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    nvn_gpu_retired_fn cb = b->retired;
    void *user = b->retired_user;
    pthread_mutex_lock(&g_mu);
    unlink_buf_locked(vk, b);
    pthread_mutex_unlock(&g_mu);
    if (b->mapped && vk->d) vkUnmapMemory(vk->d, b->memory);
    if (b->buffer) vkDestroyBuffer(vk->d, b->buffer, NULL);
    if (b->memory) vkFreeMemory(vk->d, b->memory, NULL);
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->mapped = NULL;
    free(b->cold_ledger);
    free(b);
    pthread_mutex_lock(&g_mu);
    vk->stats[STAT_RETIRED]++;
    pthread_mutex_unlock(&g_mu);
    if (cb) {
        g_in_callback++;
        cb(user);
        g_in_callback--;
    }
}

// ---------------------------------------------------------------- 设备 / 能力

static uint32_t sample_bits_max(VkSampleCountFlags f) {
    uint32_t n = 0;
    for (uint32_t s = 1; s <= 64; s <<= 1)
        if (f & s) n = s;
    return n;
}

// 与 Metal 选择一致：只向下寻找，不把不支持的 2x 升成 4x。
static uint32_t msaa_pick(uint32_t want, VkSampleCountFlags available) {
    for (uint32_t n = want; n; n >>= 1) if (available & n) return n;
    return 0;
}

struct msaa_notice { uint32_t fmt, requested; struct msaa_notice *next; };
static pthread_mutex_t g_msaa_notice_mu = PTHREAD_MUTEX_INITIALIZER;
static struct msaa_notice *g_msaa_notices;
static void msaa_notice_once(uint32_t fmt, uint32_t requested, uint32_t actual) {
    int report = 1;
    pthread_mutex_lock(&g_msaa_notice_mu);
    for (struct msaa_notice *p = g_msaa_notices; p; p = p->next)
        if (p->fmt == fmt && p->requested == requested) { report = 0; break; }
    if (report) {
        struct msaa_notice *p = malloc(sizeof *p);
        if (p) { p->fmt = fmt; p->requested = requested; p->next = g_msaa_notices; g_msaa_notices = p; }
    }
    pthread_mutex_unlock(&g_msaa_notice_mu);
    if (report) { char msg[160]; snprintf(msg, sizeof msg, "msaa_fallback: 格式 %#x 请求 %u 重采样，改用 %u", fmt, requested, actual); nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", msg); }
}

// 管线没有图像对象，按附件格式的支持集合计算共同采样数；真实图像的用途可能更严格，绑定时再核实。
static int pipe_msaa_normalize(const nvn_gpu_pipe_desc *src, nvn_gpu_pipe_desc *dst) {
    *dst = *src;
    if (src->samples != 2 && src->samples != 4 && src->samples != 8 && src->samples != 16) return 1;
    uint32_t formats[10], count = 0;
    for (int i = 0; i < 8; i++) if (src->cfmt[i]) formats[count++] = src->cfmt[i];
    if (src->dfmt) formats[count++] = src->dfmt;
    if (src->sfmt && src->sfmt != src->dfmt) formats[count++] = src->sfmt;
    VkSampleCountFlags available = ~(VkSampleCountFlags)0;
    for (uint32_t i = 0; i < count; i++) {
        VkFormat f = nvn_vk_tex_format(formats[i]);
        if (f == VK_FORMAT_UNDEFINED) return 0;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (nvn_vk_format_is_depth(f) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkImageFormatProperties ip = {0};
        if (vkGetPhysicalDeviceImageFormatProperties(g.phys, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &ip) != VK_SUCCESS) return 0;
        available &= ip.sampleCounts;
    }
    if (!count) available = g.dev->properties.limits.framebufferNoAttachmentsSampleCounts;
    dst->samples = msaa_pick(src->samples, available);
    return dst->samples != 0;
}

static void caps_digest(const void *p, size_t n, uint8_t out[16]) {
    uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull;
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h1 = (h1 ^ b[i]) * 1099511628211ull;
        h2 = (h2 + b[i]) * 1469598103934665603ull;
    }
    memcpy(out, &h1, 8);
    memcpy(out + 8, &h2, 8);
}

// 只接受实际启用的扩展、队列时间戳及 CLOCK_MONOTONIC 校准域。
static int report_calibration(uint64_t t[2], uint64_t *deviation) {
    VkCalibratedTimestampInfoKHR info[2] = {
        { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR },
        { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
    };
    return g.report_calibrate && g.report_calibrate(g.d, 2, info, t, deviation) == VK_SUCCESS;
}

// 报告时间戳自校（11-19，Adreno 830 驱动 0x80320049 实测）：vkGetCalibratedTimestamps 的 DEVICE 值与 vkCmdWriteTimestamp 写出的
// 计数零点差一个常数（7318 万计数 ≈ 3.81 秒），按驱动校准对换算的每个样本都落在录制窗口之前，回放层全部记 timestamp result_unavailable。
// 这里提交一笔只写一个时间戳的探针段，提交前 / 完成后的主机时刻框住样本，取窗口中点与驱动换算之差当修正（连采 3 次取窗口最小的）。
// 差在合并不确定度内就信驱动（修正 0、行为同以前）；探针本身失败也保持以前的行为。
static void report_self_calibrate(void) {
    struct s2a_vk_device *d = g.dev;
    g.report_corr_ticks = 0; g.report_corr_dev = 0;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cb = VK_NULL_HANDLE; VkQueryPool qp = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = d->queue_family;
    VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qci.queryType = VK_QUERY_TYPE_TIMESTAMP; qci.queryCount = 1;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateCommandPool(g.d, &pci, NULL, &pool) != VK_SUCCESS) goto out;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool; ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.d, &ai, &cb) != VK_SUCCESS || vkCreateQueryPool(g.d, &qci, NULL, &qp) != VK_SUCCESS ||
        vkCreateFence(g.d, &fci, NULL, &fence) != VK_SUCCESS) goto out;
    int have = 0; int64_t best_corr = 0; uint64_t best_extra = 0, best_win = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) break;
        vkCmdResetQueryPool(cb, qp, 0, 1);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) break;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        pthread_mutex_lock(&g_submit_mu);
        uint64_t begin = now_ns();
        VkResult r = vkQueueSubmit(g.queue, 1, &si, fence);
        pthread_mutex_unlock(&g_submit_mu);
        if (r != VK_SUCCESS || vkWaitForFences(g.d, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) break;
        uint64_t end = now_ns();
        vkResetFences(g.d, 1, &fence);
        uint64_t res[2] = {0, 0}, cal[2], dev;
        if (vkGetQueryPoolResults(g.d, qp, 0, 1, sizeof res, res, sizeof res, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS ||
            !res[1] || !report_calibration(cal, &dev)) continue;
        int64_t corr; uint64_t extra;
        if (!s2a_report_ts_self_offset(res[0], cal[0], cal[1], g.report_bits, g.report_period, begin, end, dev, &corr, &extra)) continue;
        if (end - begin < best_win) { best_win = end - begin; best_corr = corr; best_extra = extra; have = 1; }
    }
    if (have && best_corr) {
        // 修正折成计数减在 GPU 一侧：换算结果同「主机时刻 + 修正」，而校准主机时刻仍是真实取值（换算里还要拿它核时序）。取整误差 ≤ 半个计数，并进不确定度。
        g.report_corr_ticks = (int64_t)llroundl((long double)best_corr / g.report_period);
        g.report_corr_dev = best_extra + (uint64_t)(g.report_period / 2 + 1);
        char line[200];
        snprintf(line, sizeof line, "report_ts 自校：驱动校准对与查询计数器零点差 %.3f 毫秒，换算加修正（探针窗口 %llu 微秒）",
                 (double)best_corr / 1e6, (unsigned long long)(best_win / 1000));
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
    }
out:
    if (fence) vkDestroyFence(g.d, fence, NULL);
    if (qp) vkDestroyQueryPool(g.d, qp, NULL);
    if (pool) vkDestroyCommandPool(g.d, pool, NULL);
}
static void report_timestamp_open(void) {
    struct s2a_vk_device *d = g.dev;
    if (!d->calibrated_timestamps || !d->properties.limits.timestampComputeAndGraphics ||
        !isfinite(d->properties.limits.timestampPeriod) || d->properties.limits.timestampPeriod <= 0) return;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->physical, &n, NULL);
    if (d->queue_family >= n) return;
    VkQueueFamilyProperties *q = calloc(n, sizeof *q);
    if (!q) return;
    vkGetPhysicalDeviceQueueFamilyProperties(d->physical, &n, q);
    uint32_t bits = d->queue_family < n ? q[d->queue_family].timestampValidBits : 0;
    free(q);
    if (!bits || bits > 64) return;
    int khr = d->calibrated_timestamps == 1;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR domains_fn =
        (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)vkGetInstanceProcAddr(d->instance,
            khr ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR" : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
    if (!domains_fn) return;
    n = 0;
    if (domains_fn(d->physical, &n, NULL) != VK_SUCCESS || !n || n > 64) return;
    VkTimeDomainKHR domains[64];
    if (domains_fn(d->physical, &n, domains) != VK_SUCCESS) return;
    int device = 0, mono = 0;
    for (uint32_t i = 0; i < n; i++) {
        device |= domains[i] == VK_TIME_DOMAIN_DEVICE_KHR;
        mono |= domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    }
    if (!device || !mono) return;
    g.report_calibrate = (PFN_vkGetCalibratedTimestampsKHR)vkGetDeviceProcAddr(d->device,
        khr ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT");
    uint64_t t[2], deviation;
    if (!report_calibration(t, &deviation)) { g.report_calibrate = NULL; return; }
    g.report_bits = bits;
    g.report_period = d->properties.limits.timestampPeriod;
    report_self_calibrate();
}

static void fill_caps(struct nvn_gpu_vk *vk, const nvn_gpu_open_desc *d) {
    const VkPhysicalDeviceProperties *p = &vk->dev->properties;
    const VkPhysicalDeviceLimits *l = &p->limits;
    nvn_gpu_caps *c = &vk->caps;
    memset(c, 0, sizeof *c);
    c->api = NVN_GPU_API_VULKAN;
    c->api_version = p->apiVersion;
    c->family = 0;
    c->vendor_id = p->vendorID;
    c->device_id = p->deviceID;
    snprintf(c->name, sizeof c->name, "%.63s", p->deviceName);
    c->max_color_att = l->maxColorAttachments;
    c->max_vertex_streams = l->maxVertexInputBindings;
    c->max_tex_dim = l->maxImageDimension2D;
    c->max_samples = sample_bits_max(l->framebufferColorSampleCounts & l->framebufferDepthSampleCounts);
    c->max_viewports = l->maxViewports;
    c->max_push_bytes = l->maxPushConstantsSize;
    c->min_ubo_align = (uint32_t)l->minUniformBufferOffsetAlignment;
    c->min_ssbo_align = (uint32_t)l->minStorageBufferOffsetAlignment;
    c->min_texel_align = (uint32_t)l->minTexelBufferOffsetAlignment;
    c->max_texel_elems = l->maxTexelBufferElements;
    c->max_ubo_range = l->maxUniformBufferRange;
    c->max_ssbo_range = l->maxStorageBufferRange;   // 描述符「范围」上限（不等于单次分配上限）
    // 单次分配上限来自 VkPhysicalDeviceMaintenance3Properties.maxMemoryAllocationSize（Vulkan 1.1）；
    // 以前这里错用了 maxStorageBufferRange，于是任何大于 128 MiB 的池镜像都建不出来。
    { VkPhysicalDeviceMaintenance3Properties m3; memset(&m3, 0, sizeof m3); m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
      VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2); p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &m3;
      vkGetPhysicalDeviceProperties2(vk->phys, &p2);
      c->max_buf_bytes = m3.maxMemoryAllocationSize ? m3.maxMemoryAllocationSize : (uint64_t)l->maxStorageBufferRange; }
    c->max_stage_res[NVN_GPU_RES_UBO] = l->maxPerStageDescriptorUniformBuffers;
    c->max_stage_res[NVN_GPU_RES_SSBO] = l->maxPerStageDescriptorStorageBuffers;
    c->max_stage_res[NVN_GPU_RES_TEX] = l->maxPerStageDescriptorSampledImages;
    c->max_stage_res[NVN_GPU_RES_SAMP] = l->maxPerStageDescriptorSamplers;
    uint32_t comb = l->maxPerStageDescriptorSampledImages < l->maxPerStageDescriptorSamplers
                        ? l->maxPerStageDescriptorSampledImages : l->maxPerStageDescriptorSamplers;
    c->max_stage_res[NVN_GPU_RES_TEX_SAMP] = comb;
    c->max_stage_res[NVN_GPU_RES_IMAGE] = l->maxPerStageDescriptorStorageImages;
    c->max_sets = l->maxBoundDescriptorSets;
    g_max_dyn_ubo_dev = g_max_dyn_ubo = l->maxDescriptorSetUniformBuffersDynamic;

    VkPhysicalDeviceFeatures f = vk->dev->core_features;   // 已启用记录，不是「设备支持」
    // debug 强制设备特性不能绕开 GS+遮挡探测门；缺记录或未通过保持未接。
    const char *gs_probe = s2a_capprobe_gs_occ_state ? s2a_capprobe_gs_occ_state() : NULL;
    const char *gs_verdict = gs_probe ? strrchr(gs_probe, ':') : NULL;
    int gs_enabled = f.geometryShader && gs_verdict && !strcmp(gs_verdict + 1, "supported");
    if (gs_enabled) c->caps |= NVN_GPU_CAP_GEOMETRY;
    if (gs_enabled && f.vertexPipelineStoresAndAtomics) {
        c->storage_stages |= 1u << NVN_GPU_SH_GS;
        c->atomic_stages |= 1u << NVN_GPU_SH_GS;
    }
    if (f.vertexPipelineStoresAndAtomics) c->storage_stages |= 1u << NVN_GPU_SH_VS;
    if (f.fragmentStoresAndAtomics) c->storage_stages |= 1u << NVN_GPU_SH_FS;
    c->storage_stages |= 1u << NVN_GPU_SH_CS;
    if (f.vertexPipelineStoresAndAtomics) c->atomic_stages |= 1u << NVN_GPU_SH_VS;
    if (f.fragmentStoresAndAtomics) c->atomic_stages |= 1u << NVN_GPU_SH_FS;
    c->atomic_stages |= 1u << NVN_GPU_SH_CS;

    // #1199：BC 不再按 textureCompressionBC 一刀切——原生全有或缺的格式能走 compute 解码就算支持。
    if (f.textureCompressionBC || vk->dev->bc_decode_mask) c->caps |= NVN_GPU_CAP_TEX_BC;
    if (f.textureCompressionASTC_LDR) c->caps |= NVN_GPU_CAP_TEX_ASTC;
    if (vk->bc_time_on || (vk->bc1_astc_on && !f.textureCompressionBC && f.textureCompressionASTC_LDR))
        c->caps |= NVN_GPU_CAP_BC1_SNAPSHOT;
    if (g_deswizzle.pipe) c->caps |= NVN_GPU_CAP_GPU_DESWIZZLE;
    c->caps |= NVN_GPU_CAP_BLIT_SCALED;   // vkCmdBlitImage；具体格式对行不行在 nvn_gpu_blit 里逐次按格式特性判
    c->caps |= NVN_GPU_CAP_BLIT_DEPTH_STENCIL; // 辅助绘制拷贝；具体格式仍须有采样与深度模板附件能力
    // 不声明 NVN_GPU_CAP_NO_ATTACH_PASS（11-15 撤回，#697）：零附件通道在 Mali-G710 r32p1 上让一款大作标题页 / 场景整片发红（d1c76bd63 起，
    //   关掉即好），根因待查；撤回后回放核心照 11-14 前把不挂目标的笔跳过。后端开零附件通道的能力本身保留（pass_begin / C90 照测）。
    // B2：设备 advertise VK_GOOGLE_display_timing 且 debug.s2a.display_timing=1（默认关）才有
    // 每帧实际上屏时刻可报——能力位如实反映 advertise∧启用，时刻取不到时按契约报 UNKNOWN。
    if (vk->dev->has_display_timing) c->caps |= NVN_GPU_CAP_SHOWN_TIME;
    if (vk->dev->depth_clip_control_enabled) c->caps |= NVN_GPU_CAP_DEPTH_CLIP_CTL;
    if (vk->dev->draw_indirect_count_enabled && vk->dev->draw_indirect_count &&
        vk->dev->draw_indexed_indirect_count && f.multiDrawIndirect && f.drawIndirectFirstInstance)
        c->caps |= NVN_GPU_CAP_INDIRECT_COUNT;
    // HOST_IMPORT / GPU_VA / TILE_MEMORY / SAMP_MINMAX / LOD_BIAS / DEPTH_CLIP_CTL / GEOMETRY / TESSELLATION /
    // XFB / LINKED_PIPE / GPU_DURATION / GPU_TIME_HOST / DYN_* / DESWIZZLE：
    // 本段没有实现或没实测生效，一律不置位（契约：后端不做近似）。

    if (vk->report_calibrate) c->caps |= NVN_GPU_CAP_REPORT_TIMESTAMP;
    if (vk->dev->properties.apiVersion >= VK_API_VERSION_1_1) c->caps |= NVN_GPU_CAP_STORAGE_SRGB_ALIAS;   // EXTENDED_USAGE / VkImageViewUsageCreateInfo 是 1.1 核心
    c->caps |= NVN_GPU_CAP_DRAW_TEX;   // v20：内置四边形管线（backend_vulkan_draw_tex.inc）只用 Vulkan 1.0 核心，恒有
    if (d->caps_record && d->caps_record_bytes) {
        caps_digest(d->caps_record, d->caps_record_bytes, c->caps_id);
    } else {
        nvn_gpu_caps tmp = *c;   // 摘要覆盖整份能力记录（含限值与能力位），caps_id 自己清零
        memset(tmp.caps_id, 0, sizeof tmp.caps_id);
        caps_digest(&tmp, sizeof tmp, c->caps_id);
    }
}

// ---------------------------------------------------------------- worker

static uint32_t g_occl_declared_miss;   // 宿主用例（nvn_gpu_vk_test_occl_declared_miss）：声明槽那一段读回时当作没回来；0 = 不干预

// 读 n 个遮挡查询的 64 位值和可用位（r[i][0] = 值，r[i][1] 非 0 = 可用）。段已完成，不等待；没可用的项值保持 0。返回没可用的个数。
static uint32_t occl_read(VkQueryPool pool, uint32_t first, uint32_t n, uint64_t (*r)[2]) {
    memset(r, 0, (size_t)n * sizeof *r);
    (void)vkGetQueryPoolResults(g.d, pool, first, n, (size_t)n * sizeof *r, r, sizeof *r, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    uint32_t miss = 0;
    for (uint32_t i = 0; i < n; i++) miss += r[i][1] == 0;
    return miss;
}

static void segment_done(struct vk_segment *s, int status, int code, const char *why) {
    if (!s->done) return;
    nvn_gpu_done d;
    memset(&d, 0, sizeof d);
    d.signal = s->signal;
    d.status = (uint32_t)status;
    d.rc = rc(code);
    d.why = why;
    d.time_kind = NVN_GPU_TIME_NONE;
    d.occlusion = NULL;
    d.nocclusion = 0;
    if (status != NVN_GPU_DONE_FAILED && s->cmd && s->cmd->land_slot) {
        d.readback = s->cmd->land_slot->buf->mapped;
        d.readback_bytes = s->cmd->land_bytes;
    }
    // 遮挡结果：段已完成，用过的槽都有结果；没用过的槽读作 0（不等待，取不到的项保持预置的 0）
    uint64_t *occ = NULL;
    if (status == NVN_GPU_DONE_OK && s->cmd && s->cmd->qpool && s->cmd->qmax_used && (occ = calloc(s->cmd->qcount, sizeof *occ))) {
        struct nvn_gpu_cmd_s *c = s->cmd;
        for (uint32_t i = 0; i < c->qmax_used; i++)
            if (c->q_used[i] & 1) {
                uint64_t r[1][2];
                if (occl_read(c->qpool, i, 1, r) || g_occl_declared_miss) c->q_used[i] |= 8;   // 声明槽那一段没读到：这个槽读 0；名下有续段的，续段也不再并进来（下面）
                occ[i] = (c->q_used[i] & 8) ? 0 : r[0][0];
            }
        // 续段槽（后端内部拆通道时接着数的那些）：样本数并回它们所属的声明槽，调用方只看到声明槽的总数；一块一次读完。
        // 一个槽名下的各段（声明槽那一段 + 每个续段）要全部读到才算数：有一段没读到，这个槽按未完成处理，不拿部分和当结果。
        for (uint32_t k = 0; k < c->qnchunk; k++) {
            const uint32_t first = k * OCC_CHUNK, left = c->qspare_used - first, n = left < OCC_CHUNK ? left : OCC_CHUNK;
            uint64_t r[OCC_CHUNK][2];
            (void)occl_read(c->qchunk[k], 0, n, r);
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t o = c->q_owner[first + i];
                if (!r[i][1] || (c->q_used[o] & 8)) {
                    if (!(c->q_used[o] & 4)) nvn_unimpl("gpu", "occlusion_continuation_part_missing", NULL);
                    c->q_used[o] |= 4;
                } else occ[o] += r[i][0];
            }
        }
        for (uint32_t i = 0; i < c->qmax_used; i++) if (c->q_used[i] & 6) occ[i] = 0;   // 未完成的槽（续不下去 / 有一段没读到）：读 0，不留部分和
        d.occlusion = occ; d.nocclusion = c->qcount;
    }
    nvn_gpu_timestamp_result timestamps[NVN_GPU_TIMESTAMP_SLOTS] = {{0}};
    if (status != NVN_GPU_DONE_FAILED && s->cmd && s->cmd->report_ts) {
        struct nvn_gpu_cmd_s *c = s->cmd;
        uint64_t calibration[2], deviation;
        // 换算拿不到的原因读数（报告时间戳 → 回放的 timestamp result_unavailable）：首次与 2 的幂次各打一行，带全部输入
        static _Atomic uint64_t ts_fail_n;
        if (report_calibration(calibration, &deviation)) {
            uint64_t end = now_ns();
            for (uint32_t i = 0; i < c->report_count; i++) {
                uint64_t result[2] = {0, 0};
                if (!(c->report_used & (UINT64_C(1) << i))) continue;
                VkResult qr = vkGetQueryPoolResults(g.d, c->report_ts, i, 1, sizeof result, result,
                        sizeof result, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                int why = qr != VK_SUCCESS ? 10 : !result[1] ? 11 :
                          s2a_report_ts_convert_why(result[0], (calibration[0] - (uint64_t)g.report_corr_ticks) & s2a_ts_valid_mask(g.report_bits), calibration[1],
                              g.report_bits, g.report_period, c->report_begin_ns, end, deviation + g.report_corr_dev, &timestamps[i].host_ns);
                timestamps[i].valid = why == 0;
                if (why) {
                    const uint64_t n = atomic_fetch_add(&ts_fail_n, 1) + 1;
                    if (!(n & (n - 1))) {
                        char line[360];
                        snprintf(line, sizeof line, "report_ts 换算拿不到（第 %llu 次）why=%d（10 查询失败 / 11 不可用 / 1 参数时序 / 2 窗口过半周期 / 3 越界 / 4 早于窗口 / 5 晚于窗口）"
                                 " raw=%llu gpu=%llu host=%llu begin=%llu end=%llu dev=%llu bits=%u period=%.4f qr=%d corr=%lld",
                                 (unsigned long long)n, why, (unsigned long long)result[0], (unsigned long long)calibration[0], (unsigned long long)calibration[1],
                                 (unsigned long long)c->report_begin_ns, (unsigned long long)end, (unsigned long long)deviation, g.report_bits, g.report_period, (int)qr, (long long)g.report_corr_ticks);
                        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
                    }
                }
            }
        } else {
            const uint64_t n = atomic_fetch_add(&ts_fail_n, 1) + 1;
            if (!(n & (n - 1))) { char line[120]; snprintf(line, sizeof line, "report_ts 换算拿不到（第 %llu 次）：校准时间戳调用失败", (unsigned long long)n); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line); }
        }
        d.timestamps = timestamps;
        d.ntimestamps = c->report_count;
    }
    g_in_callback++;
    s->done(s->user, &d);
    g_in_callback--;
    free(occ);
}

static void detach_live(struct nvn_gpu_cmd_s *c) {
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (*pp) *pp = c->live_next;
    c->live_next = NULL;
    pthread_mutex_unlock(&g_mu);
}

static void clr_cmd_free(struct nvn_gpu_cmd_s *c);   // backend_vulkan_clear.inc
static void dtx_cmd_free(struct nvn_gpu_cmd_s *c);   // backend_vulkan_draw_tex.inc

#include "backend_vulkan_bc_time.inc"
#include "backend_vulkan_land_auto.inc"

static void finish_cmd(struct nvn_gpu_cmd_s *c, int executed) {
    if (!c) return;
    // 放弃的段（abort / 带粘滞错误不提交）没走过 vkEndCommandBuffer 的清空：两级回放队列里可能还有它的记录（往上传片里填、引用它的暂存缓冲 / 管线），
    // 先整队清空再归还 / 销毁下面这些资源。执行过的段已经过了 GPU 完成，记录早就做完了
    if (!executed) vk_dq_drain();
    if (c->bc_first_time) bc_time_finish(c, 0);
    prs_release(c, executed);   // 回收的整池组：先把反馈补丁写回正典再放回（要在下面销毁快照视图、归还表引用之前：补丁的正典内容引用着表里的视图）
    // 布局按段跟踪（c->lay）：段不执行时命令缓冲里什么都没跑，没有要回滚的；t->layout 只做诊断。
    detach_live(c);   // 已提交的 cmd 在 commit 时已摘下，这里兜底
    if (c->cold_pool) vkDestroyDescriptorPool(g.d, c->cold_pool, NULL);
    free(c->cold_events);
    // 放弃 / 提交前失败的段仍可能有 DQ 待录命令引用快照；先排空录制，再销毁。
    if (c->feedback && !executed) vk_dq_drain();
    while (c->feedback) { // 必须先销毁快照视图，再归还下面持有的图像引用
        struct feedback_copy *f = c->feedback; c->feedback = f->next;
        if (f->view) vkDestroyImageView(g.d, f->view, NULL);
        // 段执行完（executed = 1 只在栅栏等到之后由完成线程传进来）：快照图已转出 UNDEFINED、GPU 不再用它，下面归还最后一份引用时可以放回快照池。
        // 段没执行：新建的图可能还是 UNDEFINED，不进池（fb_ready 保持 0，照旧销毁）。快照图同一时刻只属于一段，这一位只由持有它的段写。
        if (executed && f->snap) f->snap->fb_ready = 1;
        free(f);
    }
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->n_newimg; i++) {   // 段没执行：图像还是 UNDEFINED，放回待转表（放不回就留给下面的归还：没人再用它）
        if (!executed && !c->newimg[i]->released) newimg_add_locked(c->newimg[i]);
        unref_locked(&g, c->newimg[i], REF_TEX);
    }
    free(c->newimg); c->newimg = NULL; c->n_newimg = 0;
    for (uint32_t i = 0; i < c->nrefs; i++) unref_locked(&g, c->refs[i].obj, c->refs[i].type);
    if (c->land_tracked) {
        g.land_live_count--;
        if (c->land_epoch != g.land_trial.epoch) g.land_prior_live--;
    }
    if (c->land_slot) c->land_slot->busy = 0;   // GPU 已结束，done 已归还借用，或段已丢弃
    pthread_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < c->nstaging; i++) {
        struct nvn_gpu_buf_s *b = c->staging[i].buf;
        if (!b) continue;
        if (seg_give_stg(b)) continue;
        pthread_mutex_lock(&g_mu);
        unlink_buf_locked(&g, b);
        pthread_mutex_unlock(&g_mu);
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        if (b->buffer) vkDestroyBuffer(g.d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
    }
    for (uint32_t i = 0; i < c->npasses; i++) {
        if (c->passes[i].fb) vkDestroyFramebuffer(g.d, c->passes[i].fb, NULL);
        if (c->passes[i].rp) vkDestroyRenderPass(g.d, c->passes[i].rp, NULL);
    }
    for (uint32_t i = 0; i < c->pool_ndpools; i++) vkDestroyDescriptorPool(g.d, c->pool_dpools[i], NULL);
    free(c->pool_dpools);
    free(c->passes);
    free(c->lay);
    free(c->memo);
    for (int i = 0; i < 6; i++) free(c->push_data[i]);
    seg_give_desc(c->dpool);
    for (uint32_t i = 0; i < c->ndpools_full; i++) seg_give_desc(c->dpools_full[i]);
    free(c->dpools_full);
    dset_release(c, executed);   // 按布局回收的描述符组：执行过的段放回空闲串，放弃的段还给池（#1096）
    if (c->report_ts) vkDestroyQueryPool(g.d, c->report_ts, NULL);
    if (c->tsq) vkDestroyQueryPool(g.d, c->tsq, NULL);
        seg_give_q(c->qpool, c->qcount);
    for (uint32_t k = 0; k < c->qnchunk; k++) seg_give_q(c->qchunk[k], OCC_CHUNK);   // 续段块：同大小的池回收表里再用
    free(c->qchunk); free(c->q_owner);
    free(c->q_used);
    // 段内解码资源在 nvn_vk_bcdec.c 里销毁，那边的 vkDestroy* 不走两级回放的包装（不先清空录制队列）：
    // 放弃的段里录制线程可能还没录到用它们的命令，先清空再销毁（开关关着时只是一次比较）。
    if (c->bc_temps) vk_dq_drain();
    while (c->bc_temps) {
        struct bc_temp *b = c->bc_temps; c->bc_temps = b->next;
        nvn_vk_bcdec_rec_free(g.dev, &b->rec);
        if (b->image) vkDestroyImage(g.d, b->image, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
    }
    clr_cmd_free(c);   // 先解除旧命令的管线依赖，再把命令池交给另一线程复用
    dtx_cmd_free(c);
    seg_give_cmd(c->pool, c->cb, c->init_cb);
    free(c->refs);
    free(c->refseen);
    free(c->staging);
    free(c);
}

// 帧日志的 GPU 读数（android/native/nvn/nvn_vk_frame_log.c，#1501 D①）：每段报 GPU 时间戳跨度、上屏段完成时结一帧。
//   弱引用：只链后端的 vk 夹具没有帧日志，地址为空就不调。
void nvn_vk_frame_log_gpu_segment(uint64_t gpu_ns) __attribute__((weak));
void nvn_vk_frame_log_gpu_frame_end(void) __attribute__((weak));
// 同一份段读数也交给 ADPF（android/native/adpf_hint.c）：API 35 起随帧上报 GPU 时长。只链后端的 vk 夹具没有它，地址为空就不调
void s2a_adpf_gpu_segment(uint64_t gpu_ns) __attribute__((weak));
void s2a_adpf_gpu_frame_end(void) __attribute__((weak));
#include "backend_vulkan_device_lost.inc"

#ifdef S2A_LAND_READBACK_TEST
// The policy fixture drives full windows. Here only inject its requested mode to
// exercise switching with real outstanding submissions, borrowed slots and fences.
void nvn_gpu_vk_test_land_mode(int on) {
    pthread_mutex_lock(&g_mu);
    g.land_trial.mode = on;
    pthread_mutex_unlock(&g_mu);
}
// Called by the recording thread before commit or abort.
uint64_t nvn_gpu_vk_test_land_eligible_bytes(nvn_gpu_cmd c) {
    return c ? c->land_eligible_bytes : 0;
}
static VkResult (*g_land_test_gate)(uint64_t);
void nvn_gpu_vk_test_land_gate(VkResult (*gate)(uint64_t)) { g_land_test_gate = gate; }
#endif

static VkResult land_readback_invalidate(struct nvn_gpu_vk *vk, const struct nvn_gpu_cmd_s *c, uint64_t signal) {
    if (!c || !c->land_slot) return VK_SUCCESS;
#ifdef S2A_LAND_READBACK_TEST
    if (g_land_test_gate) {
        VkResult test = g_land_test_gate(signal);
        if (test != VK_SUCCESS) return test;
    }
#else
    (void)signal;
#endif
    const struct nvn_gpu_buf_s *b = c->land_slot->buf;
    if (b->memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return VK_SUCCESS;
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->memory, .size = c->land_invalidate_bytes,
    };
    // 暂存由 GPU 独占写，录制的 HOST_READ 可见性屏障和上方实际 fence wait 均已完成。
    return vkInvalidateMappedMemoryRanges(vk->d, 1, &range);
}

static void process_segment(struct nvn_gpu_vk *vk, struct vk_segment *s) {
    int failed = s->failed;
    nvn_gpu_rc code = s->rc;
    const char *why = s->why[0] ? s->why : "段未执行";

    if (!failed) {
        pthread_mutex_lock(&g_mu);
        uint64_t fail_sig = g.test_fail_signal, delay_sig = g.test_delay_signal;
        uint32_t delay_ms = g.test_delay_ms;
        pthread_mutex_unlock(&g_mu);
        if (delay_ms && delay_sig == s->signal) {
            struct timespec ts = {(time_t)(delay_ms / 1000), (long)(delay_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        VkResult r = VK_TIMEOUT;
        while (!vk->stop && !vk->device_lost) {
            r = vkWaitForFences(vk->d, 1, &s->fence, VK_TRUE, 50ull * 1000ull * 1000ull);
            if (r != VK_TIMEOUT) break;
            if (vk->land_auto && !atomic_load_explicit(&vk->land_reported, memory_order_relaxed)) {
                pthread_mutex_lock(&g_mu);
                land_auto_check_gap_locked(now_ns());
                pthread_mutex_unlock(&g_mu);
            }
        }
        // 测试钩子：真实栅栏必须先完成（否则 fence / command buffer 还在途，不能销毁），
        // 完成后再把它当成失败，走真实的失败终止路径。
        if (r == VK_SUCCESS && fail_sig && fail_sig == s->signal) r = (VkResult)VK_ERROR_UNKNOWN;
        if (r != VK_SUCCESS) {
            pthread_mutex_lock(&g_mu);
            vk->active_segment = NULL;
            if (r == VK_ERROR_DEVICE_LOST) { vk->device_lost = 1; fbpool_drain_locked(vk); }
            if (!s->internal && (!vk->failed || s->signal < vk->failed)) vk->failed = s->signal;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
            if (r == VK_ERROR_DEVICE_LOST) device_loss_report(vk, s, LOSS_OBSERVED);
            failed = 1;
            code = rc(r == VK_ERROR_DEVICE_LOST ? NVN_GPU_E_DEVICE : NVN_GPU_E_FAILED);
            why = r == VK_ERROR_DEVICE_LOST ? "设备丢失" : "等待栅栏失败";
        } else {
            // 栅栏完成后立即移出嫌疑范围。回调和时间戳读数可以稍后完成。
            pthread_mutex_lock(&g_mu);
            vk->active_segment = NULL;
            int terminal = vk->failed && s->signal >= vk->failed;
            pthread_mutex_unlock(&g_mu);
            bc_time_finish(s->cmd, 1);
            if (s->cmd && s->cmd->tsq && !s->internal) {   // 调试读数：这一段在 GPU 上首尾相隔多久；相邻两段之间 GPU 空了多久
                static double period; static uint64_t n, sum, mx, last_end, gap, t_first;
                if (period == 0) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g.phys, &pp); period = pp.limits.timestampPeriod > 0 ? pp.limits.timestampPeriod : 1; }
                uint64_t ts[2] = {0, 0};
                if (vkGetQueryPoolResults(vk->d, s->cmd->tsq, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
                    uint64_t d = (uint64_t)((double)(ts[1] - ts[0]) * period), b = (uint64_t)((double)ts[0] * period), e = (uint64_t)((double)ts[1] * period);
                    if (!n) t_first = b;
                    if (last_end && b > last_end) gap += b - last_end;
                    last_end = e; n++; sum += d; if (d > mx) mx = d;
                    if (nvn_vk_frame_log_gpu_segment) nvn_vk_frame_log_gpu_segment(d);
                    if (s2a_adpf_gpu_segment) s2a_adpf_gpu_segment(d);
                    {   // 逐通道：按尺寸归拢（同尺寸的算一类），每 400 段报耗时最多的 6 类
                        static struct { uint32_t key, n, draws, ds, n1; uint64_t ns, ns1; } bk[48]; static int nbk;
                        static uint64_t pts[2 * TS_PASS_MAX];
                        uint32_t np = s->cmd->ntsp;
                        if (np && vkGetQueryPoolResults(vk->d, s->cmd->tsq, 2, 2 * np, sizeof pts[0] * 2 * np, pts, sizeof pts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                            for (uint32_t i = 0; i < np; i++) { uint32_t key = (uint32_t)s->cmd->tsp[i].w << 16 | s->cmd->tsp[i].h; int k = 0;
                                while (k < nbk && bk[k].key != key) k++;
                                if (k == nbk) { if (nbk == 48) continue; bk[nbk++].key = key; bk[k].n = bk[k].draws = bk[k].ds = bk[k].n1 = 0; bk[k].ns = bk[k].ns1 = 0; }
                                bk[k].n++; bk[k].draws += s->cmd->tsp[i].draws; bk[k].ds += s->cmd->tsp[i].ds;
                                uint64_t dns = pts[2 * i + 1] >= pts[2 * i] ? (uint64_t)((double)(pts[2 * i + 1] - pts[2 * i]) * period) : 0;
                                bk[k].ns += dns;
                                if (s->cmd->tsp[i].draws <= 1) { bk[k].n1++; bk[k].ns1 += dns; }   // 0–1 笔通道：单独累计，看它们到底吃掉多少 GPU 时间
                            }
                        if (n == 400) { char l2[700]; size_t o = 0; l2[0] = 0;
                            for (int r = 0; r < 6 && r < nbk; r++) { int best = r; for (int k = r + 1; k < nbk; k++) if (bk[k].ns > bk[best].ns) best = k;
                                if (best != r) { __typeof__(bk[0]) t = bk[r]; bk[r] = bk[best]; bk[best] = t; }
                                o += (size_t)snprintf(l2 + o, sizeof l2 - o, "%s%ux%u：%u 个（带深度 %u、0–1 笔 %u 个耗 %.0f）%u 笔 %.0f 毫秒", r ? "；" : "", bk[r].key >> 16, bk[r].key & 0xffff, bk[r].n, bk[r].ds, bk[r].n1, (double)bk[r].ns1 / 1e6, bk[r].draws, (double)bk[r].ns / 1e6); if (o >= sizeof l2) break; }
                            char l3[800]; snprintf(l3, sizeof l3, "GPU 时间（按通道尺寸，同样这 400 段）：%s", l2); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", l3); nbk = 0; }
                    }
                    if (n == 400) { char line[300]; snprintf(line, sizeof line, "GPU 时间：%llu 段，段内首尾平均 %.2f 毫秒、最长 %.2f 毫秒；这些段前后共 %.0f 毫秒，其中 GPU 在段内 %.0f 毫秒、段与段之间空着 %.0f 毫秒",
                                                         (unsigned long long)n, (double)sum / 1e6 / (double)n, (double)mx / 1e6, (double)(e - t_first) / 1e6, (double)sum / 1e6, (double)gap / 1e6);
                                    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line); n = 0; sum = mx = gap = 0; }
                }
            }
            // 前面的段已经把时间线打进失败终态时，本段即使执行完也不许发布成功（A4）。
            if (terminal) {
                failed = 1;
                code = rc(NVN_GPU_E_FAILED);
                why = "时间线已失败，本段不发布成功";
            }
        }
    }

    char land_failure[112];
    if (!s->internal && !failed) {
        const VkResult r = land_readback_invalidate(vk, s->cmd, s->signal);
        if (r != VK_SUCCESS) {
            snprintf(land_failure, sizeof land_failure, "回写暂存失效操作失败（VkResult %d）", (int)r);
            pthread_mutex_lock(&g_mu);
            if (!vk->failed || s->signal < vk->failed) vk->failed = s->signal;
            vk->stats[STAT_LAND_INVALIDATE]++;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
            failed = 1;
            code = rc(NVN_GPU_E_FAILED);
            why = land_failure;
        }
    }

    if (s->internal) {   // 呈现段：没有回调、不动时间线
        if (nvn_vk_frame_log_gpu_frame_end) nvn_vk_frame_log_gpu_frame_end();   // 本帧各段都排在上屏段之前完成：在这里结一帧
        if (s2a_adpf_gpu_frame_end) s2a_adpf_gpu_frame_end();
        pthread_mutex_lock(&g_mu);
        // 栅栏已在本线程上等到（或设备已丢）：这一段对取图信号量的等待从此对主机可见地完成，取图可以复用那把信号量
        if (s->wsi_seq > vk->wsi_done) vk->wsi_done = s->wsi_seq;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    } else if (failed) {
        segment_done(s, NVN_GPU_DONE_FAILED, code.code, why);
        pthread_mutex_lock(&g_mu);
        vk->stats[STAT_DONE_FAILED]++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    } else {
        // 发布顺序：先 done（核心回写），done 返回后才推进已完成值。
        segment_done(s, NVN_GPU_DONE_OK, NVN_GPU_OK, NULL);
        pthread_mutex_lock(&g_mu);
        // Count work only after invalidate and the successful landing callback.
        if (s->cmd && s->cmd->land_observe && !vk->land_trial.reason &&
            s->cmd->land_epoch == vk->land_trial.epoch) {
            struct nvn_gpu_cmd_s *c = s->cmd;
            struct la_counts *l = &vk->land_trial.counts;
            l->bytes += c->land_eligible_bytes;
            l->spans += c->land_eligible;
            l->subs += c->land_eligible != 0;
            l->direct += c->land_direct;
            if (c->land_slot) {
                l->staged += c->land_eligible;
                l->staged_bytes += c->land_bytes;
            }
        }
        if (s->signal > 0 && s->signal > vk->completed) vk->completed = s->signal;
        submitted_compact_locked(vk);
        vk->stats[STAT_DONE_OK]++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    if (s->fence) vkDestroyFence(vk->d, s->fence, NULL);
    int embedded = s->embedded;
    finish_cmd(s->cmd, s->executed);
    if (!embedded) free(s);
}

static void *worker_main(void *arg) {
    struct nvn_gpu_vk *vk = arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (vk->land_auto && vk->land_trial.mode && !vk->land_trial.reason)
            land_auto_check_gap_locked(now_ns());
        if (vk->rhead) {
            struct nvn_gpu_buf_s *b = vk->rhead;
            vk->rhead = b->retire_next;
            if (!vk->rhead) vk->rtail = NULL;
            b->retire_next = NULL;
            pthread_mutex_unlock(&g_mu);
            retire_one(vk, b);
            pthread_mutex_lock(&g_mu);
            // 退休队列清空要广播：close 在 g_cv 上等 rhead 变空（它先销毁视图，视图放下引用后缓冲才进退休队列）。
            // 原来这里不发信号，close 只能睡到截止时间才醒——每次关会话白等满时限（测试里 5 秒，C208）。
            if (!vk->rhead) pthread_cond_broadcast(&g_cv);
            continue;
        }
        if (vk->qhead) {
            struct vk_segment *s = vk->qhead;
            vk->qhead = s->next;
            if (!vk->qhead) vk->qtail = NULL;
            vk->busy = 1;
            if (s->submitted) vk->active_segment = s;
            pthread_mutex_unlock(&g_mu);
            process_segment(vk, s);
            pthread_mutex_lock(&g_mu);
            vk->busy = 0;
            pthread_cond_broadcast(&g_cv);
            continue;
        }
        if (vk->stop) break;
        struct timespec ts;
        deadline_after(&ts, VK_WAIT_SLICE_NS);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

// ---------------------------------------------------------------- 驱动管线缓存（#822，见 g_plc 一节的总说明）

static void plc_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void plc_logf(int level, const char *fmt, ...) {
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nvn_plat_log(level, "gpu", b);
}

static double plc_ms(uint64_t from, uint64_t to) { return (double)(to - from) / 1e6; }

// 缓存目录还不存在就补上（最多再补一层父目录，和程序缓存 direct/program_cache.c 的 ensure_dir 同口径：目录通常是
// <状态目录>/direct-cache/nvn-vk，第一局可能谁都还没建过）。已存在 / 建不出都不在这里报错：写盘那一步会如实记下原因。
static void plc_ensure_dir(const char *dir) {
    if (mkdir(dir, 0700) == 0 || errno != ENOENT) return;
    char parent[sizeof g_plc_dir];
    snprintf(parent, sizeof parent, "%s", dir);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return;
    *slash = 0;
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) return;
    (void)mkdir(dir, 0700);
}

// 开会话时：读旧文件（有就喂给驱动）→ 建缓存。线程：与 open 同。任何失败都落到「没有缓存」，不让 open 失败。
static void plc_open(const char *dir) {
    g_plc = VK_NULL_HANDLE;
    g_plc_dir[0] = 0;
    g_plc_loaded = 0;
    g_plc_rejected = 0;
    g_plc_made = 0;
    g_plc_t_open = now_ns();
    __atomic_store_n(&g_plc_t_saved, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_plc_made_saved, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_plcw_failed, 0, __ATOMIC_RELAXED);
    g_plc_size_saved = 0;
    memcpy(g_plc_uuid, g.dev->properties.pipelineCacheUUID, sizeof g_plc_uuid);
    if (!nvn_plat_flag("nvk_pipe_cache", 1)) {
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：开关 nvk_pipe_cache=0，管线不带缓存建");
        return;
    }
    {   // 局中补写的阈值（缺省值见 g_plc_made 一节；条数至少 1，毫秒不取负）
        int n = nvn_plat_flag("nvk_plc_new", 64), f = nvn_plat_flag("nvk_plc_first_ms", 10000), e = nvn_plat_flag("nvk_plc_every_ms", 30000);
        g_plc_thr_new = n < 1 ? 1u : (uint64_t)n;
        g_plc_first_ns = (uint64_t)(f < 0 ? 0 : f) * 1000000ull;
        g_plc_every_ns = (uint64_t)(e < 0 ? 0 : e) * 1000000ull;
    }
    void *blob = NULL;
    size_t bytes = 0;
    if (!dir || !dir[0]) {
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：平台没给缓存目录，只在本局内复用，不落盘");
    } else if (strlen(dir) >= sizeof g_plc_dir) {
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：缓存目录路径太长，只在本局内复用，不落盘");
    } else {
        snprintf(g_plc_dir, sizeof g_plc_dir, "%s", dir);
        char why[96] = "";
        uint64_t t0 = now_ns();
        int r = nvk_driver_cache_read(g_plc_dir, g_plc_uuid, &blob, &bytes, why, sizeof why);
        if (r < 0) {   // 损坏 / 版本不符 / 超限：当没有，本局收尾会换成新的
            g_plc_rejected++;
            blob = NULL;
            bytes = 0;
            plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：旧文件不能用（%s），当作没有，本局收尾换成新的", why);
        } else if (r > 0) {
            plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：目录里还没有旧文件（头一局，或被清过）");
        } else {
            plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：读到旧文件 %zu 字节（%.1f ms）", bytes, plc_ms(t0, now_ns()));
        }
    }
    VkPipelineCacheCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    // 并发依据（P1）：flags 保持 0，即**不**设 VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT。
    // 规范里「宿主访问 pipelineCache 必须外部同步」这一条以「建缓存时设了该标志」为前提
    // （vkCreateGraphicsPipelines 的 VUID-vkCreateGraphicsPipelines-pipelineCache-02876；参考
    // https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateGraphicsPipelines.html）；
    // 不设该标志 = 实现必须自己同步宿主访问（缺省语义），所以不同配方的建管线可以并发拿同一个缓存，
    // 不需要一把全局创建锁。本后端只在设备策略兜底 debug.s2a.nvk_pipe_serial=1 时把创建重新串行化
    // （见 nvn_gpu_pipe_get）。vkGetPipelineCacheData / 收尾销毁仍只在 g_plc_mu 下串行——那是我们
    // 自己状态（销毁 vs 取数据、g_plc_size_saved）的互斥，不是缓存访问的规范要求。
    ci.initialDataSize = bytes;
    ci.pInitialData = bytes ? blob : NULL;
    VkResult vr = vkCreatePipelineCache(g.d, &ci, NULL, &g_plc);
    if (vr != VK_SUCCESS && bytes) {   // 驱动不收这份旧数据：丢掉它、用空缓存重建，别让旧文件挡住缓存
        g_plc_rejected++;
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：驱动不收旧数据（VkResult %d），改用空缓存", (int)vr);
        bytes = 0;
        g_plc = VK_NULL_HANDLE;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vr = vkCreatePipelineCache(g.d, &ci, NULL, &g_plc);
    }
    free(blob);
    if (vr != VK_SUCCESS) {
        g_plc = VK_NULL_HANDLE;
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：驱动建不出缓存（VkResult %d），管线照旧不带缓存", (int)vr);
        return;
    }
    g_plc_loaded = bytes;
    g_plc_size_saved = bytes;   // 盘上现有的、驱动也收下了的数据大小；驱动不收旧数据时 bytes 已清零 → 取到的任何数据都比它「新」，收尾会换掉旧文件
}

// ---- 局中补写（#868）：后台写盘线程 + 触发 ----

// 交给后台线程的一份活：数据缓冲区、落盘目录、缓存标识都是自己的副本——后台线程不碰任何 Vulkan 句柄，也不碰会话全局。
struct plcw_job {
    void *data;
    size_t size;
    char dir[sizeof g_plc_dir];
    uint8_t uuid[VK_UUID_SIZE];
    unsigned long long made;   // 触发时本局经缓存建管线的条数（只用于日志）
    double get_ms;             // 触发线程上花的毫秒：查大小 + 取数据 + 拷字节（只用于日志）
};

// 后台写盘线程（分离线程）：写一份、收尾、清「在飞」标记（最后一步，收尾那边靠它判断写完没有）。写失败只记日志，收尾时会再试一次。
static void *plcw_main(void *arg) {
    struct plcw_job *j = (struct plcw_job *)arg;
    char why[96] = "";
    uint64_t t0 = now_ns();
    plc_ensure_dir(j->dir);
    int w = nvk_driver_cache_write(j->dir, j->uuid, j->data, j->size, why, sizeof why);
    double ms = plc_ms(t0, now_ns());
    if (w == 0) {
        __atomic_store_n(&g_plcw_failed, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_plcw_ok, 1, __ATOMIC_RELAXED);
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：局中写出 %zu 字节（本局经缓存建管线 %llu 条；触发线程用时 %.1f ms，后台写盘 %.1f ms）", j->size, j->made, j->get_ms, ms);
    } else {
        __atomic_store_n(&g_plcw_failed, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_plcw_fail, 1, __ATOMIC_RELAXED);
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：局中写盘没写成（%s）。收尾时会再试一次，本局不受影响", why);
    }
    free(j->data);
    free(j);
    __atomic_store_n(&g_plcw_busy, 0, __ATOMIC_RELEASE);
    return NULL;
}

// 触发检查已过（条数够了、时间到了）：在这条线程上取一份驱动数据，交给后台线程写。线程：任意建管线的线程（可能还持着清屏管线的锁 g_clr_mu）。
// 拿不到 g_plc_mu（别的线程正在取数据，或收尾在销毁缓存）/ 上一次写还在飞 → 直接跳过，下次建管线再碰，不排队、不等。
// force = 1：不看条数 / 时间阈值（停局保留会话时用，见 nvn_gpu_vk_pipe_cache_flush），其余照旧：数据没变不写、写在飞就跳过。
static void plc_save_midrun_impl(int force) {
    if (__atomic_load_n(&g_plcw_busy, __ATOMIC_ACQUIRE)) return;
    if (pthread_mutex_trylock(&g_plc_mu) != 0) return;
    VkPipelineCache pc = g_plc;
    uint64_t t = now_ns(), made = __atomic_load_n(&g_plc_made, __ATOMIC_RELAXED), ts = __atomic_load_n(&g_plc_t_saved, __ATOMIC_RELAXED);
    int due = pc && g_plc_dir[0] && !g.device_lost &&
              (force || (made >= __atomic_load_n(&g_plc_made_saved, __ATOMIC_RELAXED) + g_plc_thr_new &&
                         (ts ? t - ts >= g_plc_every_ns : t - g_plc_t_open >= g_plc_first_ns)));
    int claimed = 0;   // 抢到「在飞」标记 = 这一刻没有别的写，也轮到本线程来写
    if (due) {
        int expect = 0;
        claimed = __atomic_compare_exchange_n(&g_plcw_busy, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
    if (!claimed) { pthread_mutex_unlock(&g_plc_mu); return; }
    // 到点了。基线先推进到现在：这次取不到数据 / 数据没变 / 起不了线程，都要等再多 N 条、再隔 E 才重试，别每建一条管线就来一遍。
    __atomic_store_n(&g_plc_made_saved, made, __ATOMIC_RELAXED);
    __atomic_store_n(&g_plc_t_saved, t, __ATOMIC_RELAXED);
    uint64_t tg0 = now_ns();   // 触发线程在这里花的时间从这儿算起：查大小 + 取数据 + 拷字节（带 fsync 的写盘不在这条线程上）
    size_t size = 0;
    VkResult vr = vkGetPipelineCacheData(g.d, pc, &size, NULL);
    void *buf = NULL;
    int started = 0;   // 后台线程已接手（之后「在飞」标记由它清）；没接手的路径在函数末尾自己清
    if (vr != VK_SUCCESS || size == 0) {
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：局中取不到驱动数据（VkResult %d，%zu 字节），这次不落盘", (int)vr, size);
    } else if (size == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&g_plcw_same, 1, __ATOMIC_RELAXED);
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：局中检查：本局经缓存建管线 %llu 条，数据 %zu 字节与上次保存一致，不写", (unsigned long long)made, size);
    } else {
        size_t cap = size + size / 4 + 4096;   // 留余量：取大小之后别的线程又建了管线，数据可能已经变大（VK_INCOMPLETE 的半份数据不能落盘）
        buf = malloc(cap);
        if (!buf) {
            plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：内存不够取数据（%zu 字节），这次不落盘", cap);
        } else {
            size_t got = cap;
            vr = vkGetPipelineCacheData(g.d, pc, &got, buf);
            if (vr != VK_SUCCESS || got == 0 || got > cap) {
                plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：局中取数据失败（VkResult %d），这次不落盘", (int)vr);
                free(buf);
                buf = NULL;
            } else if (got == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
                // 查询给的大小可以是上界（lavapipe 就比实际取到的大），上面按查询大小比不出「没变」：按实际取到的字节数再比一次
                __atomic_fetch_add(&g_plcw_same, 1, __ATOMIC_RELAXED);
                plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：局中检查：本局经缓存建管线 %llu 条，数据 %zu 字节与上次保存一致，不写", (unsigned long long)made, got);
            } else {
                size = got;
                struct plcw_job *j = (struct plcw_job *)calloc(1, sizeof *j);
                if (j) {
                    j->data = buf; j->size = size; j->made = (unsigned long long)made; j->get_ms = plc_ms(tg0, now_ns());
                    memcpy(j->dir, g_plc_dir, sizeof j->dir);
                    memcpy(j->uuid, g_plc_uuid, sizeof j->uuid);
                    pthread_attr_t at;
                    pthread_t th;
                    if (pthread_attr_init(&at) == 0) {
                        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                        pthread_attr_setstacksize(&at, 256 * 1024);
                        __atomic_fetch_add(&g_plcw_started, 1, __ATOMIC_RELAXED);   // 先记再起：后台线程一旦跑完，读数里「成功 + 失败」不会比「次数」多
                        started = pthread_create(&th, &at, plcw_main, j) == 0;
                        if (!started) __atomic_fetch_sub(&g_plcw_started, 1, __ATOMIC_RELAXED);
                        pthread_attr_destroy(&at);
                    }
                    if (started) {
                        g_plc_size_saved = size;
                        buf = NULL;   // 缓冲区和 j 都归后台线程了
                    } else {
                        free(j);
                    }
                }
                if (!started) plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：起不了后台写盘线程，这次不落盘");
            }
        }
    }
    free(buf);
    __atomic_fetch_add(&g_plcw_get_ns, now_ns() - tg0, __ATOMIC_RELAXED);   // 触发线程这一段的累计（后台写盘不算）
    if (!started) __atomic_store_n(&g_plcw_busy, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_plc_mu);
}

static void plc_save_midrun(void) { plc_save_midrun_impl(0); }

// 停局但会话保留到进程退出（#860：前端没停稳就不关后端，也就走不到 plc_close 的收尾写）：把驱动当前缓存取一份交后台线程写盘，
// 不销毁缓存、不等写完——写盘是临时文件 + 原子替换，进程中途退出旧文件原样。调用方线程只花取数据的时间（真机约 0.5 ms）。线程：任意。
void nvn_gpu_vk_pipe_cache_flush(void) {
    if (!g_open) return;
    plc_save_midrun_impl(1);
}

// 一条管线刚经缓存建好：记账；够触发局中补写的条数、时间也到了就试一次。线程：任意建管线的线程（图形 / 计算 / 清屏）。
static void plc_created(void) {
    if (!g_plc) return;
    uint64_t made = __atomic_add_fetch(&g_plc_made, 1, __ATOMIC_RELAXED);
    if (!g_plc_dir[0] || made < __atomic_load_n(&g_plc_made_saved, __ATOMIC_RELAXED) + g_plc_thr_new) return;
    uint64_t ts = __atomic_load_n(&g_plc_t_saved, __ATOMIC_RELAXED), t = now_ns();
    if (ts ? t - ts < g_plc_every_ns : t - g_plc_t_open < g_plc_first_ns) return;
    plc_save_midrun();
}

// 等在飞的后台写收尾，最多等到 deadline（now_ns 时钟）。返回 1 = 已经没有在飞的写，0 = 到点了还没写完。
static int plcw_wait(uint64_t deadline) {
    while (__atomic_load_n(&g_plcw_busy, __ATOMIC_ACQUIRE)) {
        if (now_ns() >= deadline) return 0;
        struct timespec ts = {0, 1000000};   // 1 ms 一查：写盘通常十几毫秒，不值得为它再配一套条件变量
        nanosleep(&ts, NULL);
    }
    return 1;
}

// 收尾时（所有段都完成之后、销毁管线之前）：把驱动当前的缓存数据写盘，再销毁缓存。线程：与 close 同。
// 先等在飞的局中写（等多久算进 close 的 deadline；到点没写完 = 放弃这次收尾写：两边写同一个临时文件，不能并行，也不拖 close）。
// 数据大小与上次保存 / 读入时一致就不重写；设备丢了不取数据；写失败只记日志（下一局照旧慢一次，本局不受影响）。
static void plc_close(uint64_t deadline) {
    pthread_mutex_lock(&g_plc_mu);   // 与局中触发互斥：拿到之后不会再有线程拿 g_plc 去取数据
    VkPipelineCache pc = g_plc;
    g_plc = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_plc_mu);
    if (!pc) return;
    unsigned long long made = (unsigned long long)__atomic_load_n(&g_plc_made, __ATOMIC_RELAXED);
    if (!plcw_wait(deadline)) {
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：close 时限内局中写盘还没写完，放弃收尾这次写（旧文件原样，下一局照常读）");
    } else if (g_plc_dir[0] && g.device_lost) {
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：设备已丢失，不取数据、不落盘");
    } else if (g_plc_dir[0]) {
        uint64_t t0 = now_ns();
        size_t size = 0;
        VkResult vr = vkGetPipelineCacheData(g.d, pc, &size, NULL);
        if (vr != VK_SUCCESS || size == 0) {
            plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：取不到驱动数据（VkResult %d，%zu 字节），不落盘", (int)vr, size);
        } else if (size == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
            plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：本局没有新增（%zu 字节，与上次保存 / 读入时一致），不重写；本局经缓存建管线 %llu 条", size, made);
        } else {
            void *buf = malloc(size);
            if (!buf) {
                plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：内存不够取数据（%zu 字节），不落盘", size);
            } else {
                size_t want = size;
                vr = vkGetPipelineCacheData(g.d, pc, &size, buf);
                uint64_t t1 = now_ns();
                if (vr == VK_SUCCESS && size > 0 && size <= want && size == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
                    // 查询大小可以是上界（同局中补写）：按实际取到的字节数再判一次「没变」
                    plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：本局没有新增（取到 %zu 字节，与上次保存 / 读入时一致），不重写；本局经缓存建管线 %llu 条", size, made);
                } else if (vr == VK_SUCCESS && size > 0 && size <= want) {
                    char why[96] = "";
                    plc_ensure_dir(g_plc_dir);
                    int w = nvk_driver_cache_write(g_plc_dir, g_plc_uuid, buf, size, why, sizeof why);
                    uint64_t t2 = now_ns();
                    if (w == 0)
                        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：写出 %zu 字节（读入时 %zu；本局经缓存建管线 %llu 条；取数据 %.1f ms，写盘 %.1f ms）",
                                 size, g_plc_loaded, made, plc_ms(t0, t1), plc_ms(t1, t2));
                    else
                        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：没写成（%s）。下一局照旧要慢一次，本局不受影响", why);
                } else {
                    plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：取数据失败（VkResult %d），不落盘", (int)vr);
                }
                free(buf);
            }
        }
    }
    vkDestroyPipelineCache(g.d, pc, NULL);
}

// ---------------------------------------------------------------- 打开 / 关闭 / 能力

nvn_gpu_rc nvn_gpu_open(const nvn_gpu_open_desc *d, nvn_gpu_err *err) {
    if (g_open) return fail(err, NVN_GPU_E_ORDER, "已有打开的后端");
    if (!d || !d->host_device) return fail(err, NVN_GPU_E_ARG, "host_device 为空");
    struct s2a_vk_device *dev = d->host_device;
    if (!dev->instance || !dev->physical || !dev->device || !dev->queue)
        return fail(err, NVN_GPU_E_ARG, "host_device 不是已建好的 Vulkan 设备上下文");

    memset(&g, 0, sizeof g);
    g.dev = dev;
    pipe_creation_open(dev);
    const int land_setting = nvn_plat_flag("nvk_land_readback", 0);
    g.land_auto = land_setting == 2;
    if (land_setting == 1 || g.land_auto) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        g.land_readback = s2a_vk_memory_type(dev, UINT32_MAX, want, NULL) != UINT32_MAX;
        if (!g.land_readback)
            nvn_plat_log(NVN_PLAT_LOG_INFO, "land-readback", "无主机可见且可缓存的显存类型，直接从镜像回写");
    }
    { int mb = nvn_plat_flag("nvk_feedback_pool_mb", 256); g_fbpool_cap = mb > 0 ? (uint64_t)mb << 20 : 0; }   // 反馈快照池上限（0 = 关）
    // 0 = direct A, 1 = guarded A/B, 2 = guarded A/A overhead control.
    const int cold_mode = nvn_plat_flag("nvk_cold_guard", 0);
    g.cold_enabled = cold_mode == 1 || cold_mode == 2 ? cold_mode : 0;
    g.vbuf_batch = nvn_plat_flag("nvk_replay_cpu_cuts", 1) != 0;
    {   // 诊断（nvk_mem_types，缺省 0）：打印显存类型表，判「有没有 HOST_CACHED 的可见类型」——
        // CPU 写映射内存与显卡抢带宽的那条路（见坑记录续 21）要不要改走 cached 暂存，取决于这张表。
        static int mt_on = -1; if (mt_on < 0) mt_on = nvn_plat_flag("nvk_mem_types", 0) != 0;
        if (mt_on) {
            const VkPhysicalDeviceMemoryProperties *mp = &dev->memory;
            for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
                char lb[200];
                snprintf(lb, sizeof lb, "显存类型 %u：标志 0x%x（DEVICE_LOCAL %d / HOST_VISIBLE %d / HOST_COHERENT %d / HOST_CACHED %d / LAZILY %d）、堆 %u",
                         i, (unsigned)mp->memoryTypes[i].propertyFlags,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0,
                         (unsigned)mp->memoryTypes[i].heapIndex);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", lb);
            }
        }
    }
    g.d = dev->device;
    vk_dq_init(g.d);
    g.phys = dev->physical;
    g.queue = dev->queue;
    if (!g_cv_ready) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   // 与 deadline_after 同一时钟
        if (pthread_cond_init(&g_cv, &ca) != 0) {
            pthread_condattr_destroy(&ca);
            return fail(err, NVN_GPU_E_DEVICE, "cond 初始化失败");
        }
        pthread_condattr_destroy(&ca);
        g_cv_ready = 1;
    }
    dset_open();   // 按布局回收的描述符组（#1096）：开关在开会话时读一次
    prs_open();    // 整池组回收（nvk_pool_rec）
    g_deswizzle_count = g_deswizzle_bytes = g_deswizzle_fallbacks = 0;
    memset(g_deswizzle_reasons, 0, sizeof g_deswizzle_reasons);
    if (nvn_plat_flag("nvk_gpu_deswizzle", 1) && !deswizzle_open())
        nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", "【红】GPU 解排不可用，本局采用 CPU 解排");
    bc_time_open();
    report_timestamp_open();
    fill_caps(&g, d);
    g_ref_n = g_ref_hit = 0;
    __atomic_store_n(&g_border_nearest_said, 0, __ATOMIC_RELAXED);   // 边框色就近那一行每局首个打一次
    if (pthread_create(&g.worker, NULL, worker_main, &g) != 0)
        return fail(err, NVN_GPU_E_DEVICE, "worker 线程创建失败");
    g.worker_started = 1;
    if (nvn_vk_format_depth_subst() && !d24_readback_open())
        nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", "D24 替换存储的回读转换管线创建失败");
    pthread_mutex_lock(&g_seg_mu); g_seg_closed = 0;
    g_seg_stg_std = nvn_plat_flag("nvk_stg_std", 1) != 0; g_seg_stg_hit = g_seg_stg_miss = g_seg_stg_kept = g_seg_stg_rej = g_seg_stg_full = 0;
    for (int i = 0; i < 6; i++) __atomic_store_n(&g_stg_wr[i], 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_seg_mu);
    plc_open(d->cache_dir);   // 缓存是加速不是前提：里面任何一步失败都落到「不带缓存」，不让 open 失败
    g_open = 1;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_caps_get(nvn_gpu_caps *out) {
    if (!out) return;
    if (!g_open) {
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&g_mu);
    *out = g.caps;
    pthread_mutex_unlock(&g_mu);
}

// 贴图 → 缓冲回读的格式判断（nvn_gpu_copy_tex_buf 与 nvn_gpu_format_caps 的 NVN_GPU_FMT_READBACK 共用，两边不会不一致）：
//   readback_refusal：copy_tex_buf 拒收的原因，NULL = 能拷。拒收是粘滞错误，整段提交失败、时间线进失败终态，
//     所以 ReadTexels 这类旁路读回必须先查 READBACK 位、在开段前挡住。
//   readback_exact：替换的 D24/D24S8 单独打包。原生深度模板仍只拷深度面，原生 D24 深度面的高 8 位未定义。
static const char *readback_refusal(uint32_t nvn_format) {
    nvn_fmt_info fi;
    if (nvn_fmt_info_of(nvn_format, &fi) && fi.expand) return "展开格式回读未实现";
    if (nvn_vk_format_substituted(nvn_format) && !g_d24_readback.pipe) return "D24 替换存储的回读转换不可用";
    return NULL;
}
static int readback_exact(uint32_t nvn_format) {
    nvn_fmt_info fi;
    if (readback_refusal(nvn_format) || !nvn_fmt_info_of(nvn_format, &fi)) return 0;
    if (nvn_vk_format_substituted(nvn_format)) return 1;
    if (fi.depth && fi.stencil) return 0;
    if (nvn_format == NVN_FORMAT_DEPTH24) return 0;
    return 1;
}
uint32_t nvn_gpu_format_caps(uint32_t nvn_format) {
    if (!g_open) return 0;
    nvn_fmt_info info;
    int expand = nvn_fmt_info_of(nvn_format, &info) && info.expand;
    VkFormat f = nvn_vk_tex_format(nvn_format);
    if (f == VK_FORMAT_UNDEFINED) return 0;
    int bcdec = 0;   // 这个块压缩格式在本设备上走计算解码
    if (nvn_format >= 0x41 && nvn_format <= 0x50 && (g.dev->bc_decode_mask >> (nvn_format - 0x41)) & 1u) {
        if (!nvn_vk_bcdec_route(nvn_format, NULL, &f, NULL, NULL, NULL)) return 0;
        bcdec = 1;
    }
    VkFormatProperties p;
    s2a_vk_format_properties(g.phys, f, &p);
    VkFormatFeatureFlags o = p.optimalTilingFeatures;
    uint32_t out = 0;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) out |= NVN_GPU_FMT_SAMPLE;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) out |= NVN_GPU_FMT_FILTER;
    if (o & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) out |= NVN_GPU_FMT_RENDER;
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) out |= NVN_GPU_FMT_BLEND;
    if (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) out |= NVN_GPU_FMT_STORAGE;
    if (nvn_vk_format_is_depth(f)) out |= NVN_GPU_FMT_DEPTH;
    if (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) out |= NVN_GPU_FMT_ATOMIC;
    if (o & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) out |= NVN_GPU_FMT_VERTEX;
    if (nvn_vk_format_substituted(nvn_format)) out |= NVN_GPU_FMT_SUBST;   // #398：D24 换 D32 存储
    if (readback_exact(nvn_format)) out |= NVN_GPU_FMT_READBACK;
    if (expand) {
        out |= NVN_GPU_FMT_SUBST;   // 存储被换成展开目标，逐字节拷贝要核心换算
        VkFormat sf = nvn_vk_format(nvn_format);
        VkFormatProperties sp;
        memset(&sp, 0, sizeof sp);
        if (sf != VK_FORMAT_UNDEFINED) s2a_vk_format_properties(g.phys, sf, &sp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) out &= ~NVN_GPU_FMT_VERTEX;
    }
    // 走计算解码的块压缩格式：能力是照「解码后的格式」查的，但它只能被采样——建图入口不收渲染 / 存储用途。
    // 把解码后格式的「可存储」原样报出去，核心会给带 IMAGE 标志的压缩贴图加存储用途，建图就被拒（#697：Mali 上 5 种格式的贴图全建不出来，背景全黑）。
    if (bcdec) out &= NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_FILTER | NVN_GPU_FMT_READBACK;   // 回读照样是原压缩块字节（copy_tex_buf → bc_readback）
    // 通道映射补分量位置的退回格式：存储字节的分量顺序不是 NVN 的，回读也就不是 NVN 字节（下一行连 READBACK 一起去掉）
    // #975：16 位打包格式走退回格式时，分量位置靠采样视图的通道映射补回；渲染 / 存储 / 顶点取数不经通道映射，不报这些能力。
    { uint8_t map[4]; if (nvn_vk_format_chanmap(nvn_format, map)) out &= NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_FILTER; }
    return out;
}

static void wsi_close_locked(void);   // backend_vulkan_wsi.inc
static void clr_close(void);          // backend_vulkan_clear.inc
static void dtx_close(void);          // backend_vulkan_draw_tex.inc
static void depth_blit_close(void);   // backend_vulkan_depth_blit.inc
static void cold_guard_close(void);
static void easu_close(void);   // backend_vulkan_easu.inc
nvn_gpu_rc nvn_gpu_close(uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!g_open) return rc(NVN_GPU_OK);
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许 close");
    pthread_mutex_lock(&g_mu);
    if (g.close_active) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ORDER, "另一个线程正在 close");
    }
    land_auto_exit_locked();
    g.close_active = 1;
    g.closing = 1;
    pthread_mutex_unlock(&g_mu);
    uint64_t deadline = add_sat(now_ns(), timeout_ns);

    // ① 丢弃未提交 cmd：原子摘链后由本线程独占销毁，避免与并发 abort 重复释放。
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s *list = g.live;
    g.live = NULL;
    pthread_mutex_unlock(&g_mu);
    while (list) {
        struct nvn_gpu_cmd_s *next = list->live_next;
        list->live_next = NULL;
        finish_cmd(list, 0);
        list = next;
    }

    // ②③④ 等在途段 / 退休队列 / 并发 commit / 管线调用全部结束。
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (!g.qhead && !g.busy && !g.rhead && g.commit_active == 0 && g.pipe_calls_active == 0) {
            wsi_close_locked();   // 在途段都完成了：还接着的面连同交换链一起收掉
            // 先销毁视图、放下其持有的底层缓冲引用，再由 worker 完成退休回调。
            while (g.pools) pool_destroy_locked(g.pools);
            while (g.views) destroy_view_locked(&g, g.views);
            if (!g.rhead) break;
        }
        uint64_t now = now_ns();
        if (now >= deadline) {
            g.close_active = 0;
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_TIMEOUT, "close 时限内未完成在途段 / 退休 / 提交 / 管线调用（保留资源，可再调一次）");
        }
        struct timespec ts;
        deadline_after(&ts, deadline - now);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_mu);
    g.stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g.worker, NULL);
    g.worker_started = 0;
    if (g.bc_time_on) {
        pthread_mutex_lock(&g_bc_time_mu);
        bc_time_log_window(now_ns(), 1);
        pthread_mutex_unlock(&g_bc_time_mu);
    }

    // 在途段都完成了、不会再有新管线：把驱动管线缓存写盘（失败只记日志）再销毁它。放在销毁管线之前，后面的拆除出事也不丢这一局的编译结果。
    // 在飞的局中写（#868）要先等它收尾，等多久算进这次 close 的截止时间；到点没写完就放弃收尾这次写，不拖 close。
    plc_close(deadline);

    // ⑤ 销毁所有仍活着的后端对象；已 release 的 retired 已由 worker 调完。
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_buf_s *b = g.bufs;
    g.bufs = NULL;
    g.live = NULL;
    pthread_mutex_unlock(&g_mu);
    while (b) {
        struct nvn_gpu_buf_s *next = b->all_next;
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        if (b->buffer) vkDestroyBuffer(g.d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b->cold_ledger);
        free(b);
        b = next;
    }
    clr_close();
    dtx_close();
    depth_blit_close();
    dset_close();   // 在途段都完成了：增长池整池销毁（组随池一起没了；布局只在会话收尾销毁，下面销毁完管线之后才轮到它，所以回收表不用跟着布局摘表）
    prs_close();    // 整池组回收的空闲组：连池销毁
    easu_close();
    cold_guard_close();
    struct nvn_gpu_pipe_s *p = g.pipes;
    g.pipes = NULL;
    free(g_pipe_ht); g_pipe_ht = NULL; g_pipe_ht_n = 0; g_pipe_cnt = 0;
    memset(g_cpipe_ht, 0, sizeof g_cpipe_ht);
    while (p) {
        struct nvn_gpu_pipe_s *pn = p->all_next;
        destroy_pipe_objects(p);
        p = pn;
    }
    dlay_close();   // 管线都销毁了：描述符组布局（布局表）此时才销毁
    struct nvn_gpu_shader_s *sh = g.shaders;
    g.shaders = NULL;
    while (sh) {
        struct nvn_gpu_shader_s *sn = sh->all_next;
        if (sh->mod) vkDestroyShaderModule(g.d, sh->mod, NULL);
        free(sh->ents);
        free(sh->tfmt);
        free(sh->write_bounds);
        free(sh);
        sh = sn;
    }
    struct nvn_gpu_samp_s *sp = g.samps;
    g.samps = NULL;
    g.border_live = 0;
    while (sp) {
        struct nvn_gpu_samp_s *sn = sp->all_next;
        if (sp->average_sampler) vkDestroySampler(g.d, sp->average_sampler, NULL);
        if (sp->int_sampler && sp->int_sampler != sp->sampler) vkDestroySampler(g.d, sp->int_sampler, NULL);
        if (sp->sampler) vkDestroySampler(g.d, sp->sampler, NULL);
        free(sp);
        sp = sn;
    }
    pc_destroy_all();
    struct nvn_gpu_view_s *vw = g.views;
    g.views = NULL;
    while (vw) {
        struct nvn_gpu_view_s *vn = vw->all_next;
        if (vw->att) vkDestroyImageView(g.d, vw->att, NULL);
        if (vw->att_n) vkDestroyImageView(g.d, vw->att_n, NULL);
        if (vw->view) vkDestroyImageView(g.d, vw->view, NULL);
        free(vw);
        vw = vn;
    }
    struct nvn_gpu_tex_s *tx = g.texs;
    g.texs = NULL;
    g_fbpool_n = 0; g_fbpool_bytes = 0;   // 池里的快照图都还挂在 g.texs 上，随下面一起销毁
    free(g_newimg); g_newimg = NULL; g_newimg_n = g_newimg_cap = 0;
    while (tx) {
        struct nvn_gpu_tex_s *tn = tx->all_next;
        if (tx->view) vkDestroyImageView(g.d, tx->view, NULL);
        if (tx->image) vkDestroyImage(g.d, tx->image, NULL);
        if (tx->memory) vkFreeMemory(g.d, tx->memory, NULL);
        if (tx->bc_sbuf) vkDestroyBuffer(g.d, tx->bc_sbuf, NULL);   // 现解贴图的压缩影子（同 destroy_tex_locked）
        if (tx->bc_smem) vkFreeMemory(g.d, tx->bc_smem, NULL);
        free(tx);
        tx = tn;
    }
    texmem_drain(g.d);
    seg_drain();
    if (g.bc) nvn_vk_bcdec_destroy(g.dev, g.bc);
    deswizzle_close();
    d24_readback_close();
    bc1_astc_close();
    free(g.submitted);
    g.submitted = NULL;
    g.nsub = g.capsub = 0;
    g.closed = 1;
    g.close_active = 0;
    g_open = 0;
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 缓冲

static int is_shared(int mem) { return mem == NVN_GPU_MEM_SHARED; }

static int session_accepting(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = !g.closing;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static int pipe_call_enter(nvn_gpu_err *err) {
    pthread_mutex_lock(&g_mu);
    int ok = g_open && !g.closing;
    if (ok) g.pipe_calls_active++;
    pthread_mutex_unlock(&g_mu);
    if (!ok) fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    return ok;
}

static void pipe_call_leave(void) {
    pthread_mutex_lock(&g_mu);
    if (g.pipe_calls_active > 0) g.pipe_calls_active--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

#if defined(S2A_BACKEND_PIPE_CALL_TEST)
static pthread_mutex_t g_pipe_test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pipe_test_cv = PTHREAD_COND_INITIALIZER;
static int g_pipe_test_hold, g_pipe_test_entered;

void nvn_vk_pipe_call_test_hold(int hold) {
    pthread_mutex_lock(&g_pipe_test_mu);
    g_pipe_test_hold = hold;
    if (hold) g_pipe_test_entered = 0;
    pthread_cond_broadcast(&g_pipe_test_cv);
    pthread_mutex_unlock(&g_pipe_test_mu);
}

int nvn_vk_pipe_call_test_wait(uint64_t timeout_ns) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)(timeout_ns / 1000000000ull);
    until.tv_nsec += (long)(timeout_ns % 1000000000ull);
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_pipe_test_mu);
    while (!g_pipe_test_entered) {
        if (pthread_cond_timedwait(&g_pipe_test_cv, &g_pipe_test_mu, &until) != 0) break;
    }
    int entered = g_pipe_test_entered;
    pthread_mutex_unlock(&g_pipe_test_mu);
    return entered;
}

static void pipe_call_test_gate(void) {
    pthread_mutex_lock(&g_pipe_test_mu);
    if (g_pipe_test_hold) {
        g_pipe_test_entered = 1;
        pthread_cond_broadcast(&g_pipe_test_cv);
        while (g_pipe_test_hold) pthread_cond_wait(&g_pipe_test_cv, &g_pipe_test_mu);
    }
    pthread_mutex_unlock(&g_pipe_test_mu);
}
#endif

// prefer_cached：主机可见的缓冲优先挑 HOST_CACHED 的显存类型（本机有 DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED，类型 6）。
// 暂存缓冲（每帧约 9 MB 常量缓冲 + 顶点镜像落盘）是 CPU 反复小段写的：写在缓存内存上比写在不缓存映射上快数倍，
// 且该类型带 coherent 位、不需要显式 flush（见坑记录续 21 的带宽争用读数）。
enum { BUF_COHERENT, BUF_PREFER_CACHED, BUF_READBACK_CACHED };
static nvn_gpu_rc buf_create_impl(uint64_t bytes, int mem, const void *init, int memory_policy, nvn_gpu_buf *out, nvn_gpu_err *err);
nvn_gpu_rc nvn_gpu_buf_create(uint64_t bytes, int mem, const void *init, nvn_gpu_buf *out, nvn_gpu_err *err) {
    // 开关 nvk_mem_cached：所有主机可见缓冲（含池镜像）也走 HOST_CACHED 类型。缺省跟随重叠档（nvk_sync_lag≠0 时开）：
    // 默认档 A/B 中性（绑定 5.2 / 回放 33.2 / GPU 忙 39.0–39.5），而重叠档里 CPU 直写镜像 550 KB/帧与显卡抢带宽，
    // 开了之后顶点流 22–25 → 17.9 毫秒、帧 p50 52 → 50（读数见坑记录续 23）。
    static int all_cached = -1; static unsigned k;
    if (all_cached < 0 || !(++k & 255)) all_cached = nvn_plat_flag("nvk_mem_cached", nvn_plat_flag("nvk_sync_lag", 0) != 0) != 0;
    return buf_create_impl(bytes, mem, init, all_cached, out, err);
}
static nvn_gpu_rc buf_create_impl(uint64_t bytes, int mem, const void *init, int memory_policy, nvn_gpu_buf *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    pthread_mutex_lock(&g_mu);
    int closing = g.closing;
    pthread_mutex_unlock(&g_mu);
    if (closing) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    // 不按 maxMemoryAllocationSize 预先拒：规范里它是提示（超过「可能」失败、不保证失败），Adreno 830 原厂驱动上
    //   1508 MiB 的分配实测成功，而上限报 1 GiB（11-14，#1070：1.47 GiB 的池镜像被这里拦下、整局黑屏）。真分配不出来时
    //   下面 vkAllocateMemory 照实报驱动的错误码。
    if (bytes == 0) return fail(err, NVN_GPU_E_ARG, "申请 0 字节");
    if (mem != NVN_GPU_MEM_SHARED && mem != NVN_GPU_MEM_DEVICE) return fail(err, NVN_GPU_E_ARG, "mem 非法");
    if (mem == NVN_GPU_MEM_DEVICE && init) return fail(err, NVN_GPU_E_ARG, "DEVICE 内存不能带 init");

    struct nvn_gpu_buf_s *b = calloc(1, sizeof *b);
    if (!b) return fail(err, NVN_GPU_E_OOM, "缓冲结构分配失败");
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    const int readback = memory_policy == BUF_READBACK_CACHED;
    if (readback) bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.d, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkCreateBuffer 失败（%llu 字节）", (unsigned long long)bytes);
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.d, b->buffer, &req);
    VkMemoryPropertyFlags want = is_shared(mem) ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = UINT32_MAX;
    if (readback) want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    else if (memory_policy == BUF_PREFER_CACHED && is_shared(mem))
        type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, want, NULL);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, readback ? NVN_GPU_E_UNSUPPORTED : NVN_GPU_E_OOM,
                    "找不到满足要求的显存类型（兼容类型位 0x%x）", req.memoryTypeBits);
    }
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g.d, &ai, NULL, &b->memory) != VK_SUCCESS) {
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkAllocateMemory 失败");
    }
    if (vkBindBufferMemory(g.d, b->buffer, b->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(g.d, b->memory, NULL);
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkBindBufferMemory 失败");
    }
    b->bytes = bytes;
    b->mem_kind = mem;
    b->memory_flags = g.dev->memory.memoryTypes[type].propertyFlags;
    if (is_shared(mem)) {
        if (vkMapMemory(g.d, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->mapped) != VK_SUCCESS) {
            vkDestroyBuffer(g.d, b->buffer, NULL);
            vkFreeMemory(g.d, b->memory, NULL);
            free(b);
            return fail(err, NVN_GPU_E_OOM, "vkMapMemory 失败");
        }
        if (init) memcpy(b->mapped, init, (size_t)bytes);
    }
    pthread_mutex_lock(&g_mu);
    if (g.closing || g.closed) {   // 与 close 的快照串行：close 开始后新建的对象不留在账上
        pthread_mutex_unlock(&g_mu);
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        vkDestroyBuffer(g.d, b->buffer, NULL);
        vkFreeMemory(g.d, b->memory, NULL);
        free(b);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    b->all_next = g.bufs;
    g.bufs = b;
    g.stats[STAT_BUFS]++;
    pthread_mutex_unlock(&g_mu);
    *out = b;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_buf_wrap(void *host, uint64_t bytes, nvn_gpu_buf *out, nvn_gpu_err *err) {
    (void)host;
    (void)bytes;
    if (out) *out = NULL;
    return fail(err, NVN_GPU_E_UNSUPPORTED, "本设备 / 本段未启用 HOST_IMPORT（VK_EXT_external_memory_host 未实测）");
}

uint64_t nvn_gpu_buf_va(nvn_gpu_buf b) { return b ? b->va : 0; }   // 未开 CAP_GPU_VA：恒 0
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b) { return b ? b->mapped : NULL; }

void nvn_gpu_buf_release(nvn_gpu_buf bh, nvn_gpu_retired_fn retired, void *user) {
    if (!bh) return;
    struct nvn_gpu_buf_s *b = bh;
    pthread_mutex_lock(&g_mu);
    if (!b->released) {
        b->released = 1;
        rel_epoch_bump();
        b->retired = retired;
        b->retired_user = user;
        if (b->refs == 0) queue_retire_locked(&g, b);
    }
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_buf_use(nvn_gpu_cmd ch, nvn_gpu_buf bh) {
    if (!ch || !bh) return;
    cmd_ref((struct nvn_gpu_cmd_s *)ch, bh, REF_BUF);
}

// ---------------------------------------------------------------- arena

static struct nvn_gpu_buf_s *new_staging(struct nvn_gpu_cmd_s *c, uint64_t cap) {
    nvn_gpu_err e;
    nvn_gpu_buf b = seg_reuse_on() ? seg_take_stg(cap) : NULL;
    static int cached_on = -1; if (cached_on < 0) cached_on = nvn_plat_flag("nvk_stage_cached", 1) != 0;
    if (!b) {
        if (!rc_is(buf_create_impl(cap, NVN_GPU_MEM_SHARED, NULL, cached_on, &b, &e), NVN_GPU_OK)) return NULL;
        __atomic_add_fetch(&g_stg_wr[4], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_stg_wr[5], cap, __ATOMIC_RELAXED);   // 新建的暂存块（表里没取到）
    }
    if (c->nstaging == c->scap) {
        uint32_t n = c->scap ? c->scap * 2 : 4;
        struct vk_staging *p = realloc(c->staging, (size_t)n * sizeof *p);
        if (!p) {
            nvn_gpu_buf_release(b, NULL, NULL);
            return NULL;
        }
        c->staging = p;
        c->scap = n;
    }
    c->staging[c->nstaging++].buf = b;
    return b;
}

nvn_gpu_rc nvn_gpu_arena_alloc(nvn_gpu_cmd ch, uint64_t bytes, uint32_t align, nvn_gpu_slice *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    memset(out, 0, sizeof *out);
    if (!ch) return fail(err, NVN_GPU_E_ARG, "cmd 为空");
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return fail(err, c->sticky.code, "%s", c->why);
    if (!g_open || g.closing) return fail(err, NVN_GPU_E_STOPPED, "会话已开始关闭");
    if (bytes == 0) return fail(err, NVN_GPU_E_ARG, "bytes 为 0");
    if (align == 0) align = 4;
    if (align & (align - 1)) return fail(err, NVN_GPU_E_ARG, "align 不是 2 的幂：%u", align);
    uint64_t off = (c->arena_used + align - 1) & ~(uint64_t)(align - 1);
    if (!c->arena || off + bytes > c->arena_cap) {
        uint64_t cap = bytes + align;
        if (cap < VK_ARENA_MIN) cap = VK_ARENA_MIN;
        struct nvn_gpu_buf_s *b = new_staging(c, cap);
        if (!b) return fail(err, NVN_GPU_E_OOM, "arena %llu 字节分配失败", (unsigned long long)cap);
        c->arena = b;
        c->arena_cap = cap;
        c->arena_used = 0;
        off = 0;
    }
    out->buf = c->arena;
    out->off = off;
    out->cpu = c->arena->mapped + off;
    out->va = 0;
    c->arena_used = off + bytes;
    return rc(NVN_GPU_OK);
}

// 暂存填充（契约见 nvn_backend.h）：两级回放开着时交录制线程按序拷（nvn_vk_dq.inc vk_dq_fill），关着就地拷。
// 段提交时 vkEndCommandBuffer 的包装先清空队列，所以 GPU 读上传片之前一定拷完；放弃段在 finish_cmd 开头整队清空。
uint64_t nvn_gpu_arena_fill(nvn_gpu_cmd ch, const nvn_gpu_slice *sl, const void *src, uint64_t bytes) {
    if (!sl || !sl->cpu || !src || !bytes) return 0;
    struct nvn_gpu_cmd_s *c = ch;
    if (!c) { memcpy(sl->cpu, src, (size_t)bytes); return 0; }
    return vk_dq_fill(sl->cpu, src, bytes);
}
uint64_t nvn_gpu_arena_fill_wait(uint64_t ticket) { return vk_dq_fill_wait(ticket); }
int nvn_gpu_arena_fill_take(uint64_t ticket) { return vk_dq_fill_take(ticket); }

// ---------------------------------------------------------------- 录制原语

// 保守的全局内存屏障：传输操作前后与段末各来一次（先保证正确，后面再按访问类型收窄）。
static void mem_barrier(struct nvn_gpu_cmd_s *c, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                        VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(c->cb, src_stage, dst_stage, 0, 1, &b, 0, NULL, 0, NULL);
}
static void full_barrier(struct nvn_gpu_cmd_s *c) {
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    c->xfer_dirty = 0;
    if (g.cold_enabled) c->cold_barriers++;
}


static int buf_range(const struct nvn_gpu_buf_s *b, uint64_t off, uint64_t bytes) {
    return b && off <= b->bytes && bytes <= b->bytes - off;
}

#include "backend_vulkan_cold_ledger.inc"

enum { LAND_SLOT_LIMIT = 64 << 20, LAND_SLOT_GRANULARITY = 1 << 20 };

static void land_stat(unsigned key, uint64_t amount) {
    pthread_mutex_lock(&g_mu);
    g.stats[key] += amount;
    pthread_mutex_unlock(&g_mu);
}

static struct vk_land_slot *land_slot_take(VkDeviceSize size) {
    struct vk_land_slot *slot = NULL;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < sizeof g.land_slots / sizeof g.land_slots[0]; i++) {
        struct vk_land_slot *candidate = &g.land_slots[i];
        if (candidate->busy) continue;
        if (candidate->buf && candidate->buf->bytes >= size) {
            slot = candidate;
            break;
        }
        if (!slot) slot = candidate;
    }
    if (slot) slot->busy = 1;
    pthread_mutex_unlock(&g_mu);
    if (!slot) {
        land_stat(STAT_LAND_BUSY, 1);
        return NULL;
    }
    if (slot->buf && slot->buf->bytes >= size) return slot;
    if (slot->buf) nvn_gpu_buf_release(slot->buf, NULL, NULL);
    const VkDeviceSize capacity = (size + LAND_SLOT_GRANULARITY - 1) & ~(VkDeviceSize)(LAND_SLOT_GRANULARITY - 1);
    nvn_gpu_err err = {0};
    const nvn_gpu_rc r = buf_create_impl(capacity, NVN_GPU_MEM_SHARED, NULL, BUF_READBACK_CACHED, &slot->buf, &err);
    if (!nvn_gpu_ok(r)) {
        pthread_mutex_lock(&g_mu);
        slot->busy = 0;
        g.stats[r.code == NVN_GPU_E_UNSUPPORTED ? STAT_LAND_INCOMPATIBLE : STAT_LAND_ALLOC]++;
        pthread_mutex_unlock(&g_mu);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "land-readback", err.why);
        return NULL;
    }
    if (g.land_auto) {
        pthread_mutex_lock(&g_mu);
        g.land_trial.counts.allocs++;
        pthread_mutex_unlock(&g_mu);
    }
    char message[128];
    snprintf(message, sizeof message, "暂存 %llu 字节，实际内存标志 0x%x，非一致粒度 %llu",
             (unsigned long long)slot->buf->bytes, (unsigned)slot->buf->memory_flags,
             (unsigned long long)g.dev->properties.limits.nonCoherentAtomSize);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "land-readback", message);
    return slot;
}

struct vk_land_region { nvn_gpu_buf src; VkBufferCopy region; };

static int compare_land_src(const void *a, const void *b) {
    const uintptr_t x = (uintptr_t)((const struct vk_land_region *)a)->src;
    const uintptr_t y = (uintptr_t)((const struct vk_land_region *)b)->src;
    return (x > y) - (x < y);
}

int nvn_gpu_readback_enabled(void) { return g_open && g.land_readback && !g.land_auto; }
int nvn_gpu_readback_cmd_enabled(nvn_gpu_cmd c) { return c && (c->land_mode || c->land_observe); }

uint64_t nvn_gpu_readback_record(nvn_gpu_cmd c, nvn_gpu_readback_span *spans, uint32_t count) {
    if (!nvn_gpu_readback_cmd_enabled(c) || c->in_pass || c->land_slot || !rc_is(c->sticky, NVN_GPU_OK)) return 0;

    const VkMemoryPropertyFlags direct = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkDeviceSize atom = g.dev->properties.limits.nonCoherentAtomSize;
    VkDeviceSize used = 0;
    uint32_t staged = 0, cached = 0;
    const uint32_t span_limit = LAND_SLOT_LIMIT / sizeof(struct vk_land_region);
    if (count > span_limit || !atom || atom > LAND_SLOT_LIMIT || (atom & (atom - 1))) {
        land_stat(STAT_LAND_LIMIT, 1);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        nvn_gpu_readback_span *s = &spans[i];
        s->readback_offset = UINT64_MAX;
        if (!s->src) continue;
        int use_direct = s->src->mapped && (s->src->memory_flags & direct) == direct;
#ifdef S2A_LAND_READBACK_TEST
        if (g_land_test_gate) use_direct = 0;   // A software ICD can expose only cached mappings.
#endif
        if (use_direct) {
            cached++;
            continue;
        }
        if (s->bytes == 0 || s->bytes > LAND_SLOT_LIMIT - used || !buf_range(s->src, s->offset, s->bytes)) {
            land_stat(STAT_LAND_LIMIT, 1);
            return 0;
        }
        s->readback_offset = used;
        used += s->bytes;
        staged++;
    }
    if (c->land_observe) {
        c->land_eligible = staged;
        c->land_eligible_bytes = used;
        c->land_direct = cached;
    }
    if (!c->land_mode) return 0;
    if (cached) land_stat(STAT_LAND_DIRECT, cached);
    if (!staged) return 0;

    struct vk_land_region region_stack[16];
    VkBufferCopy copy_stack[16];
    struct vk_land_region *regions = staged <= 16 ? region_stack : malloc((size_t)staged * sizeof *regions);
    VkBufferCopy *copies = staged <= 16 ? copy_stack : malloc((size_t)staged * sizeof *copies);
    if (!regions || !copies) {
        land_stat(STAT_LAND_ALLOC, 1);
        used = 0;
        goto out;
    }

    const VkDeviceSize invalidate_bytes = (used + atom - 1) & ~(atom - 1);
    struct vk_land_slot *slot = land_slot_take(invalidate_bytes);
    if (!slot) {
        used = 0;
        goto out;
    }
    c->land_slot = slot;
    c->land_bytes = used;
    c->land_invalidate_bytes = invalidate_bytes;

    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const nvn_gpu_readback_span *s = &spans[i];
        if (s->readback_offset == UINT64_MAX) continue;
        regions[n++] = (struct vk_land_region){s->src, {s->offset, s->readback_offset, s->bytes}};
    }
    qsort(regions, n, sizeof *regions, compare_land_src);
    for (uint32_t i = 0; i < n; i++)
        if (!i || regions[i].src != regions[i-1].src) cmd_ref(c, regions[i].src, REF_BUF);
    // 录制失败走本段粘滞错误和 FAILED 回调，已经拿到的槽随命令回收。
    if (!rc_is(c->sticky, NVN_GPU_OK)) goto out;
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    for (uint32_t first = 0; first < n;) {
        const nvn_gpu_buf src = regions[first].src;
        uint32_t end = first;
        do {
            copies[end - first] = regions[end].region;
            end++;
        } while (end < n && regions[end].src == src);
        vkCmdCopyBuffer(c->cb, src->buffer, slot->buf->buffer, end - first, copies);
        first = end;
    }
    // 映射读的可见性，以及本次传输读完成后，后续提交才能复写源镜像的执行依赖。
    mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_HOST_READ_BIT);
    land_stat(STAT_LAND_BYTES, used);
out:
    if (regions != region_stack) free(regions);
    if (copies != copy_stack) free(copies);
    return used;
}

// ---- stg_chunk：暂存写按标准块切开
// 池镜像同步（graphics/nvn/replay/rp_buf.c sync_write）在镜像正被显卡用着时，把游戏内存里的脏页一段一段交给 nvn_gpu_buf_write：
// 一笔 = 一段连续脏页，短的几 KB、长的几 MB。arena_alloc 为超过当前块余量的一笔新建一个 max(bytes + 4, 256 KB) 的块，
// 大于 256 KB 的块不是标准大小，回收表不收（nvk_stg_std），段完成就销毁；下一次又是一块新映射的内存，memcpy 每 4 KB 缺一页。
// 真机（Adreno 8 系，一款 3D 游戏的世界场景，R29 调试包，缺省开关）：每帧池镜像同步约 90 MB，其中约 34 MB 走暂存；主线程每秒约 20.8 万次缺页
// （96.6% 落在 memmove、93.7% 经 nvn_gpu_buf_write），内核态占该线程 CPU 21%、解析慢路尾约 12 毫秒 / 帧。
// 切法：一笔超过一个标准块就按 STG_CHUNK 切；当前块还剩 4 KB 以上先用满它。每一块都是标准大小，走回收表（SEGPOOL_STG_BIG_N 个位置）。
// STG_CHUNK = 标准块 - 4：arena_alloc 要 bytes + align（align 取 4）字节的块，整块用满时恰好等于 VK_ARENA_MIN，新建出来的就是标准块。
#define STG_CHUNK ((uint64_t)VK_ARENA_MIN - 4u)
static uint64_t arena_room(const struct nvn_gpu_cmd_s *c, uint32_t align) {   // 当前 arena 块在对齐之后还剩多少字节（没有块 = 0）
    if (!c->arena) return 0;
    const uint64_t off = (c->arena_used + align - 1) & ~(uint64_t)(align - 1);
    return off < c->arena_cap ? c->arena_cap - off : 0;
}

void nvn_gpu_buf_write(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, const void *src, uint64_t bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *b = bh;
    if (!b || (!src && bytes)) { set_sticky(c, NVN_GPU_E_ARG, "buf_write 句柄 / src 为空"); return; }
    if (!buf_range(b, off, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "buf_write 越界 off=%llu bytes=%llu size=%llu",
                                                   (unsigned long long)off, (unsigned long long)bytes, (unsigned long long)b->bytes); return; }
    if (bytes == 0) return;
    // 暂存按标准块切开（见 stg_chunk 一节）：一笔写要的暂存超过一个标准块时，整块新建的非标准大小块回收表不收，每次都是新的
    // 一段映射内存，memcpy 逐页缺页；切成标准块之后走回收表，页一直是驻留的。
    __atomic_add_fetch(&g_stg_wr[0], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_stg_wr[1], bytes, __ATOMIC_RELAXED);
    if (bytes > STG_CHUNK) __atomic_add_fetch(&g_stg_wr[3], 1, __ATOMIC_RELAXED);
    full_barrier(c);   // 一笔写只在第一条拷贝之前来一次：各条拷贝的目标段互不相交，彼此之间没有先后依赖
    for (uint64_t done = 0; done < bytes;) {
        uint64_t n = bytes - done;
        if (n > STG_CHUNK) {
            const uint64_t room = arena_room(c, 4);   // 当前块还剩 4 KB 以上先把它用满，不够就开新的标准块
            n = (room >= 4096 && room < STG_CHUNK) ? room : STG_CHUNK;
        }
        nvn_gpu_slice sl;
        nvn_gpu_err e;
        if (!rc_is(nvn_gpu_arena_alloc(ch, n, 4, &sl, &e), NVN_GPU_OK)) {
            set_sticky(c, e.rc.code, "buf_write 暂存：%s", e.why);
            return;
        }
        memcpy(sl.cpu, (const uint8_t *)src + done, (size_t)n);
        VkBufferCopy bc = {sl.off, off + done, n};
        vkCmdCopyBuffer(c->cb, sl.buf->buffer, b->buffer, 1, &bc);
        __atomic_add_fetch(&g_stg_wr[2], 1, __ATOMIC_RELAXED);
        done += n;
    }
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    cmd_ref(c, b, REF_BUF);
    cold_note_write(c, b, off, bytes, COLD_READ_TRANSFER, 1);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_WRITES]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_fill_buf(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t bytes, uint32_t value) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *b = bh;
    if (!b || !buf_range(b, off, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "fill_buf 越界"); return; }
    if ((off & 3) || (bytes & 3)) { set_sticky(c, NVN_GPU_E_ARG, "fill_buf 需 4 字节对齐 off=%llu bytes=%llu",
                                                  (unsigned long long)off, (unsigned long long)bytes); return; }
    if (bytes == 0) return;
    full_barrier(c);
    vkCmdFillBuffer(c->cb, b->buffer, off, bytes, value);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    cmd_ref(c, b, REF_BUF);
    cold_note_write(c, b, off, bytes, COLD_READ_TRANSFER, 1);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_FILLS]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_copy_buf(nvn_gpu_cmd ch, nvn_gpu_buf src, uint64_t soff, nvn_gpu_buf dst, uint64_t doff, uint64_t bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *s = src, *d = dst;
    if (!s || !d || !buf_range(s, soff, bytes) || !buf_range(d, doff, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "copy_buf 越界"); return; }
    if (bytes & 3) { set_sticky(c, NVN_GPU_E_ARG, "copy_buf 需 4 字节长度（本段实现限制）bytes=%llu", (unsigned long long)bytes); return; }
    if (bytes == 0) return;
    full_barrier(c);
    VkBufferCopy bc = {soff, doff, bytes};
    vkCmdCopyBuffer(c->cb, s->buffer, d->buffer, 1, &bc);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    cmd_ref(c, s, REF_BUF);
    cmd_ref(c, d, REF_BUF);
    cold_note_write(c, d, doff, bytes, 0, 0);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_COPIES]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_copy_bufs_uniform(nvn_gpu_cmd ch, const nvn_gpu_buf_copy *v, uint32_t n, uint32_t stages) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch || !n) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    for (uint32_t i = 0; i < n; i++) {
        if (!v[i].src || !v[i].dst || !buf_range(v[i].src, v[i].soff, v[i].bytes) || !buf_range(v[i].dst, v[i].doff, v[i].bytes)) { set_sticky(c, NVN_GPU_E_ARG, "copy_bufs 第 %u 段越界", i); return; }
        if (!v[i].bytes || (v[i].bytes & 3)) { set_sticky(c, NVN_GPU_E_ARG, "copy_bufs 第 %u 段长度 %llu 不是 4 的正整数倍", i, (unsigned long long)v[i].bytes); return; }
    }
    VkPipelineStageFlags dst = 0;
    if (stages & 1u << NVN_GPU_SH_VS) dst |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (stages & 1u << NVN_GPU_SH_FS) dst |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if ((stages & 1u << NVN_GPU_SH_GS) && g.dev->core_features.geometryShader) dst |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    if (!dst) { set_sticky(c, NVN_GPU_E_ARG, "copy_bufs 没给读的阶段（stages=%#x）", stages); return; }
    // 源可能刚被计算 / 传输 / 图形写过（GPU 写出的间接记录、主段里的镜像同步），目标是本段新分的暂存：一道屏障盖住两者
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    enum { RUN = 64 }; VkBufferCopy r[RUN];
    for (uint32_t i = 0; i < n;) {
        struct nvn_gpu_buf_s *s = v[i].src, *d = v[i].dst; uint32_t k = 0;
        while (i < n && k < RUN && v[i].src == s && v[i].dst == d) { r[k].srcOffset = v[i].soff; r[k].dstOffset = v[i].doff; r[k].size = v[i].bytes; k++; i++; }
        vkCmdCopyBuffer(c->cb, s->buffer, d->buffer, k, r);
        cmd_ref(c, s, REF_BUF); cmd_ref(c, d, REF_BUF);
        if (g.cold_enabled) for (uint32_t j = 0; j < k; j++)
            cold_note_write(c, d, r[j].dstOffset, r[j].size, 0, 0);
    }
    mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, dst, VK_ACCESS_UNIFORM_READ_BIT);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（同 nvn_gpu_copy_buf）
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_COPIES] += n;
    pthread_mutex_unlock(&g_mu);
}


// ---------------------------------------------------------------- 着色器 / 管线 / 通道 / 绘制（纹理三角形）

static VkShaderStageFlagBits vk_shader_stage(nvn_gpu_stage st) {
    switch (st) {
    case NVN_GPU_SH_VS: return VK_SHADER_STAGE_VERTEX_BIT;
    case NVN_GPU_SH_FS: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case NVN_GPU_SH_GS: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case NVN_GPU_SH_CS: return VK_SHADER_STAGE_COMPUTE_BIT;
    default: return 0;
    }
}

static VkPrimitiveTopology vk_topology(uint32_t prim) {
    switch (prim) {
    case 0x0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 0x4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 0x5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;        // NVN 邻接图元直接映射（回放核心已按设备开了 geometryShader 才放行）
    case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // 2 线环 / 7–9（回放核心已展开 / 换成扇）/ 14 补丁：未实现
    }
}

static VkBlendFactor vk_blend_factor(uint32_t f) {
    switch (f) {
    case 0x1: return VK_BLEND_FACTOR_ZERO;
    case 0x2: return VK_BLEND_FACTOR_ONE;
    case 0x3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_MAX_ENUM;
    }
}
static VkBlendOp vk_blend_op(uint32_t e) {
    switch (e) {
    case 0x1: return VK_BLEND_OP_ADD;
    case 0x2: return VK_BLEND_OP_SUBTRACT;
    case 0x3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x4: return VK_BLEND_OP_MIN;
    case 0x5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_MAX_ENUM;
    }
}
// 逐项表达 NVN 语义，不依赖与 Vulkan 枚举编号碰巧一致。
static VkLogicOp vk_logic_op(uint32_t op) {
    switch (op) {
    case NVN_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
    case NVN_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
    case NVN_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
    case NVN_LOGIC_OP_COPY: return VK_LOGIC_OP_COPY;
    case NVN_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
    case NVN_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
    case NVN_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
    case NVN_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
    case NVN_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
    case NVN_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
    case NVN_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
    case NVN_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
    case NVN_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
    case NVN_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
    case NVN_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
    case NVN_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
    default: return VK_LOGIC_OP_MAX_ENUM;
    }
}

static VkStencilOp vk_stencil_op(uint32_t op) {   // NVN_STENCIL_OP_*：1 KEEP … 8 DECR_WRAP
    switch (op) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkCompareOp vk_depth_func(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkDescriptorType vk_desc_type(uint8_t cls) {
    switch (cls) {
    case NVN_GPU_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case NVN_GPU_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case NVN_GPU_RES_TEX: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case NVN_GPU_RES_SAMP: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case NVN_GPU_RES_TEX_SAMP: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case NVN_GPU_RES_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case NVN_GPU_RES_TBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case NVN_GPU_RES_IMAGE_TBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

// ---------------- SPIR-V texel 反射（只读 blob；未知格式由绑定的 view 提供） ----------------
// 只解析 TBUF / IMAGE_TBUF 需要的最小指令子集：
//   OpDecorate(Binding/DescriptorSet) → OpVariable(UniformConstant) → OpTypePointer → (OpTypeArray) → OpTypeImage。
// 约定（v2 未定，报 PR 请契约属主复核）：
//   · OpTypeImage.Dim 必须为 Buffer（一维缓冲纹理只能映射成 texel buffer）；
//   · TBUF 收 Sampled=0/1（采样读），IMAGE_TBUF 收 Sampled=0/2（存储写）；
//   · ImageFormat 非 Unknown 按 SPIR-V 值映射；Unknown 留给绑定视图，不猜格式；
//   · 项与 blob 对不上 → E_LAYOUT（逐项核对的 A1 要求）；数值类 / 位宽推不出 → E_UNSUPPORTED。
enum { SPV_OP_CAPABILITY = 17, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_IMAGE = 25,
       SPV_OP_TYPE_SAMPLED_IMAGE = 27, SPV_OP_TYPE_ARRAY = 28, SPV_OP_TYPE_RUNTIME_ARRAY = 29,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_VARIABLE = 59, SPV_OP_IMAGE_TEXEL_POINTER = 60, SPV_OP_DECORATE = 71 };
enum { SPV_DEC_BINDING = 33, SPV_DEC_SET = 34 };
enum { SPV_DIM_BUFFER = 5, SPV_SC_UNIFORM_CONST = 0 };   // SPIR-V Dim：0-3 维度、4=Rect、5=Buffer

struct spv_id {
    uint8_t kind;          // 1 int 2 float 3 vector 4 image 5 array 7 pointer 8 variable 9 sampled_image
    uint8_t sgn, sampled, dim;
    uint8_t atom;          // variable: 着色器对它取过 OpImageTexelPointer（图像原子）
    uint32_t a;            // int/float: 位宽；vector: 分量类型 id；image: sampled 类型 id；array: elem id；ptr: pointee id；var: 指针类型 id
    uint32_t fmt;          // image: ImageFormat 操作数
};

// SPIR-V ImageFormat 操作数（非 Unknown）→ VkFormat
static VkFormat spv_img_fmt(uint32_t f) {
    switch (f) {
    case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 3: return VK_FORMAT_R32_SFLOAT;
    case 4: return VK_FORMAT_R8G8B8A8_UNORM;
    case 5: return VK_FORMAT_R8G8B8A8_SNORM;
    case 6: return VK_FORMAT_R32G32_SFLOAT;
    case 7: return VK_FORMAT_R16G16_SFLOAT;
    case 8: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 9: return VK_FORMAT_R16_SFLOAT;
    case 10: return VK_FORMAT_R16G16B16A16_UNORM;
    case 11: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 12: return VK_FORMAT_R16G16_UNORM;
    case 13: return VK_FORMAT_R8G8_UNORM;
    case 14: return VK_FORMAT_R16_UNORM;
    case 15: return VK_FORMAT_R8_UNORM;
    case 16: return VK_FORMAT_R16G16B16A16_SNORM;
    case 17: return VK_FORMAT_R16G16_SNORM;
    case 18: return VK_FORMAT_R8G8_SNORM;
    case 19: return VK_FORMAT_R16_SNORM;
    case 20: return VK_FORMAT_R8_SNORM;
    case 21: return VK_FORMAT_R32G32B32A32_SINT;
    case 22: return VK_FORMAT_R16G16B16A16_SINT;
    case 23: return VK_FORMAT_R8G8B8A8_SINT;
    case 24: return VK_FORMAT_R32_SINT;
    case 25: return VK_FORMAT_R32G32_SINT;
    case 26: return VK_FORMAT_R16G16_SINT;
    case 27: return VK_FORMAT_R8G8_SINT;
    case 28: return VK_FORMAT_R16_SINT;
    case 29: return VK_FORMAT_R8_SINT;
    case 30: return VK_FORMAT_R32G32B32A32_UINT;
    case 31: return VK_FORMAT_R16G16B16A16_UINT;
    case 32: return VK_FORMAT_R8G8B8A8_UINT;
    case 33: return VK_FORMAT_R32_UINT;
    case 34: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 35: return VK_FORMAT_R32G32_UINT;
    case 36: return VK_FORMAT_R16G16_UINT;
    case 37: return VK_FORMAT_R8G8_UINT;
    case 38: return VK_FORMAT_R16_UINT;
    case 39: return VK_FORMAT_R8_UINT;
    case 40: return VK_FORMAT_R64_UINT;
    case 41: return VK_FORMAT_R64_SINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// Vulkan 1.1 设备级 without-format 特性适用的格式；1..39 与规范列表对应。
static int texel_unknown_storage_supported(VkFormat f) {
    for (uint32_t i = 1; i <= 39; i++)
        if (spv_img_fmt(i) == f) return 1;
    return 0;
}

static nvn_gpu_rc spv_texel_formats(const void *blob, size_t bytes,
                                    const nvn_gpu_layout_ent *ents, uint32_t nent, VkFormat *tfmt,
                                    uint32_t *required, nvn_gpu_err *err) {
    const uint32_t *w = blob;
    if (bytes < 20 || w[0] != 0x07230203u) return fail(err, NVN_GPU_E_ARG, "SPIR-V 头非法");
    uint32_t bound = w[3];
    if (!bound || bound > 0x100000) return fail(err, NVN_GPU_E_ARG, "SPIR-V id 上界非法：%u", bound);
    struct spv_id *ids = calloc(bound, sizeof *ids);
    uint32_t *vset = malloc(bound * sizeof *vset), *vbind = malloc(bound * sizeof *vbind);
    if (!ids || !vset || !vbind) {
        free(ids); free(vset); free(vbind);
        return fail(err, NVN_GPU_E_OOM, "SPIR-V 反射暂存分配失败");
    }
    for (uint32_t i = 0; i < bound; i++) vset[i] = vbind[i] = UINT32_MAX;
    size_t pos = 5, n = bytes / 4;
    while (pos < n) {
        uint32_t w0 = w[pos], op = w0 & 0xFFFFu, wc = w0 >> 16;
        if (!wc || pos + wc > n) break;
        const uint32_t *o = w + pos + 1;
        switch (op) {
        case SPV_OP_CAPABILITY:
            if (wc >= 2 && o[0] == 55) *required |= NVN_GPU_ACC_R;
            if (wc >= 2 && o[0] == 56) *required |= NVN_GPU_ACC_W;
            break;
        case SPV_OP_TYPE_INT:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 1; t->a = o[1]; t->sgn = (uint8_t)o[2]; }
            break;
        case SPV_OP_TYPE_FLOAT:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 2; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_VECTOR:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 3; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_IMAGE:
            if (wc >= 9 && o[0] < bound) {
                struct spv_id *t = &ids[o[0]];
                t->kind = 4; t->a = o[1]; t->dim = (uint8_t)o[2]; t->sampled = (uint8_t)o[6]; t->fmt = o[7];
            }
            break;
        case SPV_OP_TYPE_SAMPLED_IMAGE:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 9; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_ARRAY:
        case SPV_OP_TYPE_RUNTIME_ARRAY:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 5; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_POINTER:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 7; t->a = o[2]; }
            break;
        case SPV_OP_VARIABLE:
            if (wc >= 4 && o[1] < bound && o[2] == SPV_SC_UNIFORM_CONST) {
                struct spv_id *t = &ids[o[1]];
                t->kind = 8; t->a = o[0];
            }
            break;
        case SPV_OP_IMAGE_TEXEL_POINTER:   // 结果类型 / 结果 / 图像（指向变量的指针）/ 坐标 / 样本：图像原子要格式的 STORAGE_TEXEL_BUFFER_ATOMIC
            if (wc >= 6 && o[2] < bound) ids[o[2]].atom = 1;
            break;
        case SPV_OP_DECORATE:
            if (wc >= 4 && o[0] < bound) {
                if (o[1] == SPV_DEC_BINDING) vbind[o[0]] = o[2];
                else if (o[1] == SPV_DEC_SET) vset[o[0]] = o[2];
            }
            break;
        default: break;
        }
        pos += wc;
    }
    nvn_gpu_rc r = rc(NVN_GPU_OK);
    for (uint32_t i = 0; i < nent; i++) {
        const nvn_gpu_layout_ent *e = &ents[i];
        if (e->cls != NVN_GPU_RES_TBUF && e->cls != NVN_GPU_RES_IMAGE_TBUF) continue;
        uint32_t v = UINT32_MAX;
        for (uint32_t j = 0; j < bound; j++)
            if (ids[j].kind == 8 && vset[j] == e->set && vbind[j] == e->binding) { v = j; break; }
        if (v == UINT32_MAX) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) 在 blob 里无对应 OpVariable", e->set, e->binding);
            break;
        }
        uint32_t t = ids[v].a, guard = 0;
        while (t < bound && (ids[t].kind == 7 || ids[t].kind == 5 || ids[t].kind == 9) && ++guard <= 16) t = ids[t].a;
        if (t >= bound || ids[t].kind != 4) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) 的类型链解不到 OpTypeImage", e->set, e->binding);
            break;
        }
        if (ids[t].dim != SPV_DIM_BUFFER) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Dim=%u 不是 Buffer", e->set, e->binding, ids[t].dim);
            break;
        }
        if (e->cls == NVN_GPU_RES_TBUF && ids[t].sampled > 1) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Sampled=%u 与 TBUF（采样读）不符", e->set, e->binding, ids[t].sampled);
            break;
        }
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && ids[t].sampled == 1) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Sampled=1 与 IMAGE_TBUF（存储写）不符", e->set, e->binding);
            break;
        }
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && !ids[t].fmt)
            *required |= e->access ? e->access & NVN_GPU_ACC_RW : NVN_GPU_ACC_RW;
        VkFormat f = ids[t].fmt ? spv_img_fmt(ids[t].fmt) : VK_FORMAT_UNDEFINED;
        if (ids[t].fmt && f == VK_FORMAT_UNDEFINED) {
            r = fail(err, NVN_GPU_E_UNSUPPORTED, "texel 布局项 (set %u, binding %u) 格式推不出（ImageFormat=%u）", e->set, e->binding, ids[t].fmt);
            break;
        }
        // 存储纹素缓冲上的原子：绑定视图的格式必须等于声明格式（写描述符时核），所以在这里按声明格式查一次设备的
        //   STORAGE_TEXEL_BUFFER_ATOMIC 就够；声明 Unknown 时证不出绑定格式能做原子，照实报不支持，不赌驱动
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && ids[v].atom) {
            VkFormatProperties fp; memset(&fp, 0, sizeof fp);
            if (f != VK_FORMAT_UNDEFINED) s2a_vk_format_properties(g.phys, f, &fp);
            if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT)) {
                r = fail(err, NVN_GPU_E_UNSUPPORTED, "texel 布局项 (set %u, binding %u) 做图像原子：格式 %d 没有 STORAGE_TEXEL_BUFFER_ATOMIC（ImageFormat=%u）",
                         e->set, e->binding, (int)f, ids[t].fmt);
                break;
            }
        }
        tfmt[i] = f;
    }
    free(ids);
    free(vset);
    free(vbind);
    return r;
}

static void destroy_shader_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_shader_s *sh) {
    struct nvn_gpu_shader_s **pp = &vk->shaders;
    while (*pp && *pp != sh) pp = &(*pp)->all_next;
    if (*pp) *pp = sh->all_next;
    if (sh->mod) vkDestroyShaderModule(vk->d, sh->mod, NULL);
    free(sh->ents);
    free(sh->tfmt);
    free(sh->write_bounds);
    free(sh);
}

// #1072 第 1 项：顶点前的阶段（VS / TCS / TES / GS）写存储要 vertexPipelineStoresAndAtomics（VUID-RuntimeSpirv-NonWritable-06341），
// 用到 ClipDistance / CullDistance 能力要 shaderClipDistance / shaderCullDistance。设备没启用时不交给驱动（不合法的 SPIR-V），
// 返回缺的特性名；建管线时按它报 UNSUPPORTED。存储写的判据（含解析不了按「要」算）与直连接纳是同一份 s2a_spirv_storage_need。
static uint64_t g_vfeat_missing[3];   // 缺特性被拦下的着色器数：顶点存储写 / 裁剪距离 / 剔除距离
static const char *spv_vertex_missing(const void *blob, size_t bytes, nvn_gpu_stage st) {
    const VkPhysicalDeviceFeatures *f = &g.dev->core_features;
    static const uint32_t model[5] = {0, 4, 3, 1, 2};   // NVN 阶段（VS / FS / GS / TCS / TES）→ SPIR-V ExecutionModel（Vertex 0 / Fragment 4 / Geometry 3 / TessControl 1 / TessEval 2）
    if (st != NVN_GPU_SH_FS && (unsigned)st < 5 && !f->vertexPipelineStoresAndAtomics &&
        s2a_spirv_storage_need(blob, bytes, model[st]) != 0) return "vertexPipelineStoresAndAtomics";
    if (f->shaderClipDistance && f->shaderCullDistance) return NULL;
    const unsigned char *b = blob; size_t n = bytes / 4;   // blob 不保证 4 字节对齐：逐字节取字
    for (size_t i = 5; i < n;) {
        uint32_t word = s2a_spv_word(b + i * 4), len = word >> 16, op = word & 0xffffu;
        if (!len || len > n - i) break;
        if (op == 17 && len >= 2) {   // OpCapability
            uint32_t cap = s2a_spv_word(b + (i + 1) * 4);
            if (cap == 32 && !f->shaderClipDistance) return "shaderClipDistance";
            if (cap == 33 && !f->shaderCullDistance) return "shaderCullDistance";
        } else if (op != 17 && op != 10 && op != 11 && op != 14 && op != 15 && op != 16) break;   // 能力只在模块开头（扩展 / 导入 / 寻址 / 入口 / 执行模式之前）
        i += len;
    }
    return NULL;
}

static uint64_t g_spv_devlimit_reject;                    // 本机上限（features / limits / apiVersion / 启用扩展）拒收计数（读数见 nvn_gpu_stats）
// 本机上限核对失败的统一出口：计数后按 fail 同法写 why（调用点信息在 fmt 里，owner 设备形态档按行比对）
static nvn_gpu_rc spv_dev_fail(nvn_gpu_err *e, const char *fmt, ...) {
    __atomic_fetch_add(&g_spv_devlimit_reject, 1, __ATOMIC_RELAXED);
    if (e) { va_list ap; va_start(ap, fmt); vsnprintf(e->why, sizeof e->why, fmt, ap); va_end(ap); e->rc = rc(NVN_GPU_E_UNSUPPORTED); }
    return rc(NVN_GPU_E_UNSUPPORTED);
}

static nvn_gpu_rc compute_workgroup_check(const nvn_gpu_shader_src *s, nvn_gpu_err *err) {
    const char *entry = s->entry && s->entry[0] ? s->entry : "main";
    // The driver must receive the exact entry whose execution mode we checked.
    if (strlen(entry) >= sizeof(((struct nvn_gpu_shader_s *)0)->entry))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "compute workgroup: entry name exceeds backend capacity");
    uint32_t local[3] = {0};
    const char *why = s2a_spirv_workgroup_size(s->blob, s->bytes, entry, local);
    if (why) return fail(err, NVN_GPU_E_UNSUPPORTED, "%s (entry=%s)", why, entry);
    const VkPhysicalDeviceLimits *limits = &g.dev->properties.limits;
    for (unsigned k = 0; k < 3; k++) {
        if (!local[k]) return fail(err, NVN_GPU_E_UNSUPPORTED, "compute workgroup: LocalSize[%u]=0 (entry=%s)", k, entry);
        if (local[k] > limits->maxComputeWorkGroupSize[k])
            return spv_dev_fail(err, "spv compliance: 本机 maxComputeWorkGroupSize[%u]=%u：LocalSize=%ux%ux%u（entry=%s 程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                        k, limits->maxComputeWorkGroupSize[k], local[0], local[1], local[2], entry,
                        s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    }
    uint32_t threads = 1;
    for (unsigned k = 0; k < 3; k++) {
        // Divide before multiplying: even a malformed three-dimensional product
        // cannot wrap around and appear to fit the device's invocation limit.
        if (local[k] > limits->maxComputeWorkGroupInvocations / threads)
            return spv_dev_fail(err, "spv compliance: 本机 maxComputeWorkGroupInvocations=%u：LocalSize=%ux%ux%u（entry=%s 程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                        limits->maxComputeWorkGroupInvocations, local[0], local[1], local[2], entry,
                        s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
        threads *= local[k];
    }
    // Workgroup 共享字节是最后一道保险：出库闸在 libpack_compute_v1 已核，本处同时盖住现编产物与绕过出库闸的路径。
    uint64_t shared = 0;
    const char *swhy = s2a_spv_workgroup_bytes(s->blob, s->bytes, &shared);
    if (swhy) return fail(err, NVN_GPU_E_UNSUPPORTED, "%s (entry=%s)", swhy, entry);
    if (shared > limits->maxComputeSharedMemorySize)
        return spv_dev_fail(err,
                    "spv compliance: 本机 maxComputeSharedMemorySize=%u：Workgroup 声明 %llu 字节（entry=%s 程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                    limits->maxComputeSharedMemorySize, (unsigned long long)shared, entry,
                    s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    return rc(NVN_GPU_OK);
}

// ---------- #1865 统一 SPIR-V 合规核对 ----------
// 装载期单点：库件 / 现编 / 缓存件全过。每条核对失败打一行（项名、SPIR-V 依据、本机值、程序键）。
// 「表外声明」（能力表 / 扩展表没映射的）两级处置：precompiled（出包预编 / 离线库件）按拒收——
// 该件无法自证合法，回退现编重产；现编 / 缓存件具名警告 + 计数、照常派发——
// 翻译器新增无害声明不该把画面打断（laze #492710 定稿）。
static uint64_t g_spv_unlisted_cap, g_spv_unlisted_ext;   // 现编件被放行的表外声明计数（读数见 nvn_gpu_stats）
static int spv_req_have(int req) {
    const VkPhysicalDeviceFeatures *f = &g.dev->core_features;
    switch (req) {
    case SPV_REQ_INT16: return f->shaderInt16;
    case SPV_REQ_GEOMETRY: return f->geometryShader;
    case SPV_REQ_TESSELLATION: return f->tessellationShader;
    case SPV_REQ_TG_POINTSIZE: return f->shaderTessellationAndGeometryPointSize;
    case SPV_REQ_IMAGE_GATHER_EXT: return f->shaderImageGatherExtended;
    case SPV_REQ_STORAGE_IMAGE_MS: return f->shaderStorageImageMultisample;
    case SPV_REQ_STORAGE_IMG_EXT_FMT: return f->shaderStorageImageExtendedFormats;
    case SPV_REQ_UBO_DYN: return f->shaderUniformBufferArrayDynamicIndexing;
    case SPV_REQ_SAMPLED_DYN: return f->shaderSampledImageArrayDynamicIndexing;
    case SPV_REQ_SBO_DYN: return f->shaderStorageBufferArrayDynamicIndexing;
    case SPV_REQ_IMG_DYN: return f->shaderStorageImageArrayDynamicIndexing;
    case SPV_REQ_CLIP_DIST: return f->shaderClipDistance;
    case SPV_REQ_CULL_DIST: return f->shaderCullDistance;
    case SPV_REQ_CUBE_ARRAY: return f->imageCubeArray;
    case SPV_REQ_MULTI_VIEWPORT: return f->multiViewport;
    case SPV_REQ_STORAGE_READ_WOF: return f->shaderStorageImageReadWithoutFormat;
    case SPV_REQ_STORAGE_WRITE_WOF: return f->shaderStorageImageWriteWithoutFormat;
    case SPV_REQ_DRAW_PARAMETERS: return g.dev->shader_draw_parameters_enabled;
    case SPV_REQ_API_1_1: return g.dev->properties.apiVersion >= VK_API_VERSION_1_1;
    case SPV_REQ_DESCRIPTOR_INDEXING: return g.dev->descriptor_indexing_enabled;
    case SPV_REQ_FLOAT16: case SPV_REQ_INT64: case SPV_REQ_FLOAT64: case SPV_REQ_INT8:
    case SPV_REQ_FLOAT_CONTROLS: case SPV_REQ_16BIT_STORAGE: case SPV_REQ_8BIT_STORAGE:
    case SPV_REQ_SBC_CLASS: case SPV_REQ_POST_DEPTH: case SPV_REQ_MULTIVIEW:
    case SPV_REQ_STENCIL_EXPORT: case SPV_REQ_SHADING_RATE:
    case SPV_REQ_VARIABLE_POINTERS: case SPV_REQ_BUFFER_DEVICE_ADDRESS:
        return 0;   // 设备从未启用对应扩展 / 特性（vk_device.c 的启用清单里没有）
    default: return 0;
    }
}
static nvn_gpu_rc spv_decl_reject_or_warn(const nvn_gpu_shader_src *s,
                                          int is_cap, const char *detail, nvn_gpu_err *err) {
    if (s->precompiled)
        return fail(err, NVN_GPU_E_UNSUPPORTED,
                    "spv compliance: unlisted %s %s（库件不能自证合法，退回现编；程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                    is_cap ? "capability" : "extension", detail,
                    s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    uint64_t n = __atomic_fetch_add(is_cap ? &g_spv_unlisted_cap : &g_spv_unlisted_ext, 1, __ATOMIC_RELAXED);
    if (n < 8) {
        char line[256];
        snprintf(line, sizeof line, "spv compliance: 现编件声明表外 %s %s，具名计数放行（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                 is_cap ? "capability" : "extension", detail,
                 s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "gpu", line);
    }
    return rc(NVN_GPU_OK);
}
static nvn_gpu_rc spv_compliance_check(const nvn_gpu_shader_src *s, nvn_gpu_err *err) {
    struct s2a_spv_decl d;
    const char *scan = s2a_spv_scan(s->blob, s->bytes, &d);
    if (scan) return fail(err, NVN_GPU_E_UNSUPPORTED, "%s", scan);
    // SPIR-V 版本 vs 本机 apiVersion 支持的最高 SPIR-V 环境（1.0→1.0 / 1.1→1.3 / 1.2→1.5 / ≥1.3→1.6）
    const uint32_t api = g.dev->properties.apiVersion;
    const uint32_t vmax = api >= VK_API_VERSION_1_3 ? 0x10600u : api >= VK_API_VERSION_1_2 ? 0x10500u :
                          api >= VK_API_VERSION_1_1 ? 0x10300u : 0x10000u;
    if (d.version > vmax)
        return spv_dev_fail(err,
                    "spv compliance: 本机 SPIR-V 版本上限 %#x（apiVersion %#x）：模块声明 %#x（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                    vmax, api, d.version,
                    s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    for (unsigned i = 0; i < d.n_caps; i++) {
        int req = s2a_spv_cap_req(d.caps[i]);
        if (req == SPV_REQ_CORE) continue;
        if (req == SPV_REQ_UNLISTED) {
            char detail[40]; snprintf(detail, sizeof detail, "%u", d.caps[i]);
            nvn_gpu_rc r = spv_decl_reject_or_warn(s, 1, detail, err);
            if (!nvn_gpu_ok(r)) return r;
            continue;
        }
        if (req == SPV_REQ_CLIP_DIST || req == SPV_REQ_CULL_DIST)
            continue;   // 这两项由 spv_vertex_missing 按阶段感知延迟打标（CS 不查、非 CS 建管线报红），不在此硬拒
        if (req == SPV_REQ_STORAGE_READ_WOF || req == SPV_REQ_STORAGE_WRITE_WOF)
            continue;   // 声明≠使用：无格式存储读写按实际访问延迟打标（missing_storage_format_feature），管线点拒——C60b/C60c 契约
        if (!spv_req_have(req))
            return spv_dev_fail(err,
                        "spv compliance: 本机缺 %s（OpCapability %u 声明；程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                        s2a_spv_req_name(req), d.caps[i],
                        s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    }
    for (unsigned i = 0; i < d.n_ext; i++) {
        int req = s2a_spv_ext_req(d.exts[i]);
        if (req == SPV_REQ_CORE) continue;
        if (req == SPV_REQ_UNLISTED) {
            nvn_gpu_rc r = spv_decl_reject_or_warn(s, 0, d.exts[i], err);
            if (!nvn_gpu_ok(r)) return r;
            continue;
        }
        if (!spv_req_have(req))
            return spv_dev_fail(err,
                        "spv compliance: 本机缺 %s（OpExtension %s 声明；程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                        s2a_spv_req_name(req), d.exts[i],
                        s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    }
    const VkPhysicalDeviceLimits *limits = &g.dev->properties.limits;
    if (d.has_push_bytes && d.push_bytes > limits->maxPushConstantsSize)
        return spv_dev_fail(err,
                    "spv compliance: 本机 maxPushConstantsSize=%u：PushConstant 声明 %llu 字节（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                    limits->maxPushConstantsSize, (unsigned long long)d.push_bytes,
                    s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    // 本机上限用量：每类描述符计数 vs 每阶段上限（max_stage_res 即 maxPerStageDescriptor* 的 cls 序投影）、
    //   set 号 vs maxBoundDescriptorSets、push 段总长 vs maxPushConstantsSize（与声明侧取大者判）
    if (s->layout) {
        uint32_t cnt[NVN_GPU_RES_N] = {0}; uint8_t maxset = 0;
        for (uint32_t i = 0; i < s->layout->nent; i++) {
            const nvn_gpu_layout_ent *e = &s->layout->ent[i];
            if (e->cls < NVN_GPU_RES_N) cnt[e->cls] += e->count ? e->count : 1;
            if (e->set > maxset) maxset = e->set;
        }
        for (uint32_t cls = 0; cls < NVN_GPU_RES_N; cls++) {
            uint32_t lim = g.caps.max_stage_res[cls];
            if (!lim) continue;   // TBUF / IMAGE_TBUF / VA 无对应每阶段描述符上限（行未填 = 0），不在本表核
            if (cnt[cls] > lim)
                return spv_dev_fail(err,
                            "spv compliance: 本机每阶段描述符类 %u 上限 %u：用 %u（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                            cls, lim, cnt[cls], s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
        }
        if ((uint32_t)maxset + 1 > g.caps.max_sets)
            return spv_dev_fail(err,
                        "spv compliance: 本机 maxBoundDescriptorSets=%u：用到 set %u（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                        g.caps.max_sets, maxset, s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
        uint64_t push_total = (uint64_t)s->layout->push_off + s->layout->push_bytes;
        if (push_total > limits->maxPushConstantsSize)
            return spv_dev_fail(err,
                        "spv compliance: 本机 maxPushConstantsSize=%u：push 段 %llu 字节（程序键 %02x%02x%02x%02x%02x%02x%02x%02x）",
                        limits->maxPushConstantsSize, (unsigned long long)push_total,
                        s->key[0], s->key[1], s->key[2], s->key[3], s->key[4], s->key[5], s->key[6], s->key[7]);
    }
    if (s->stage == NVN_GPU_SH_CS) {   // 计算工作组预检：LocalSize / 线程积 / Workgroup 共享字节（直读 SPIR-V）
        nvn_gpu_rc workgroup = compute_workgroup_check(s, err);
        if (!nvn_gpu_ok(workgroup)) return workgroup;
    }
    return rc(NVN_GPU_OK);
}

#include "nvn_vk_array_query.h"
// 数组采样层坐标改写（nvn_vk_array_query.h）的缺省值按设备定：规范要求驱动自己把层号夹到视图的层范围，合规驱动上这段改写结果不变、
//   只是每次数组采样多一次尺寸查询和一次夹取，所以只对实测需要的驱动缺省开。目前只有华为 Maleoon 一族（vendorID 0x19E5、驱动名以 Maleoon 开头；
//   实测 Maleoon 920，驱动名 "Maleoon 920"、driverID 9、驱动版本 3104880233）。nvk_array_query 显式设 0 / 1 仍能强制。
static int array_query_default(void) {
    return g.dev->properties.vendorID == 0x19E5u && !strncmp(g.dev->driver_name, "Maleoon", 7);
}
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!s || !s->blob || !s->bytes) return fail(err, NVN_GPU_E_ARG, "着色器 blob 为空");
    if (s->stage == NVN_GPU_SH_GS && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "GS 未启用或探测未通过");
    if (s->kind != NVN_GPU_BLOB_SPIRV) return fail(err, NVN_GPU_E_UNSUPPORTED, "只收 SPIR-V blob（kind=%d）", s->kind);
    if (s->bytes & 3) return fail(err, NVN_GPU_E_ARG, "SPIR-V 字节数不是 4 的倍数");
    // #1865 统一合规核对：能力 / 扩展声明、SPIR-V 版本、push 与描述符用量、计算工作组（含 Workgroup 字节）。
    //   原 Int16 行内拒与 CS 工作组预检都在表里；缺设备特性的按程序报 UNSUPPORTED、why 点名特性名。
    {
        nvn_gpu_rc compliance = spv_compliance_check(s, err);
        if (!nvn_gpu_ok(compliance)) return compliance;
    }
    struct nvn_gpu_shader_s *sh = calloc(1, sizeof *sh);
    if (!sh) return fail(err, NVN_GPU_E_OOM, "着色器结构分配失败");
    sh->stage = s->stage;
    memcpy(sh->src_key, s->key, sizeof sh->src_key);
    snprintf(sh->entry, sizeof sh->entry, "%s", s->entry && s->entry[0] ? s->entry : "main");
    if (s->layout) {
        sh->nent = s->layout->nent;
        sh->push_off = s->layout->push_off;
        sh->push_bytes = s->layout->push_bytes;
        if (sh->nent) {
            sh->ents = calloc(sh->nent, sizeof *sh->ents);
            if (!sh->ents) {
                free(sh);
                return fail(err, NVN_GPU_E_OOM, "布局拷贝分配失败");
            }
            memcpy(sh->ents, s->layout->ent, (size_t)sh->nent * sizeof *sh->ents);
        }
        int need_tfmt = 0;
        for (uint32_t i = 0; i < sh->nent; i++)
            if (sh->ents[i].cls == NVN_GPU_RES_TBUF || sh->ents[i].cls == NVN_GPU_RES_IMAGE_TBUF) need_tfmt = 1;
        if (need_tfmt) {
            sh->tfmt = calloc(sh->nent, sizeof *sh->tfmt);
            if (!sh->tfmt) {
                free(sh->ents);
                free(sh);
                return fail(err, NVN_GPU_E_OOM, "texel 格式表分配失败");
            }
            uint32_t required = 0;
            nvn_gpu_rc tr = spv_texel_formats(s->blob, s->bytes, sh->ents, sh->nent, sh->tfmt, &required, err);
            if (!rc_is(tr, NVN_GPU_OK)) {
                free(sh->tfmt);
                free(sh->ents);
                free(sh);
                return tr;
            }
            sh->missing_storage_format_feature =
                ((required & NVN_GPU_ACC_R) && !g.dev->core_features.shaderStorageImageReadWithoutFormat) ||
                ((required & NVN_GPU_ACC_W) && !g.dev->core_features.shaderStorageImageWriteWithoutFormat);
        }
    }
    sh->missing_vertex_feature = s->stage == NVN_GPU_SH_CS ? NULL : spv_vertex_missing(s->blob, s->bytes, s->stage);
    if (sh->missing_vertex_feature) {   // 具名报红：每种缺的特性头一次点名，之后只计数（读数见 nvn_gpu_stats 的 shader_missing_*）
        static int said[3];
        int k = !strcmp(sh->missing_vertex_feature, "vertexPipelineStoresAndAtomics") ? 0 : !strcmp(sh->missing_vertex_feature, "shaderClipDistance") ? 1 : 2;
        __atomic_fetch_add(&g_vfeat_missing[k], 1, __ATOMIC_RELAXED);
        if (!__atomic_exchange_n(&said[k], 1, __ATOMIC_RELAXED)) {
            char line[200];
            snprintf(line, sizeof line, "【红】设备没启用 %s：用到它的着色器（首个是阶段 %d）不交给驱动，用它的管线报 UNSUPPORTED、绘制跳过", sh->missing_vertex_feature, (int)s->stage);
            nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", line);
        }
    }
    // 没有启用能力时保留可查询的 shader 句柄，管线入口返回 UNSUPPORTED；
    // 不向驱动提交不合法 SPIR-V，以免 vkCreateShaderModule 先触发 validation。
    if (!sh->missing_storage_format_feature && !sh->missing_vertex_feature) {
        VkShaderModuleCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = s->bytes; ci.pCode = s->blob;
        uint32_t *array_code = NULL, array_words = 0, array_count = 0;
        if (nvn_plat_flag("nvk_array_query", array_query_default()) && s->bytes / 4 <= UINT32_MAX) {
            array_code = nvn_vk_array_query(s->blob, (uint32_t)(s->bytes / 4), &array_words, &array_count);
            if (array_code) {
                ci.pCode = array_code; ci.codeSize = (size_t)array_words * 4;
                static unsigned array_logs;
                if (__atomic_fetch_add(&array_logs, 1, __ATOMIC_RELAXED) < 16) {
                    char line[128]; snprintf(line, sizeof line, "array-query stage=%u samples=%u words=%u", (unsigned)s->stage, array_count, array_words);
                    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
                }
            }
        }
        if (g.cold_enabled && !s->precompiled && s->stage == NVN_GPU_SH_FS) {
            s2a_sha256 hash;
            s2a_sha256_init(&hash);
            s2a_sha256_update(&hash, ci.pCode, ci.codeSize);
            s2a_sha256_final(&hash, sh->spirv_digest);
        }
        VkResult module_result = vkCreateShaderModule(g.d, &ci, NULL, &sh->mod);
        if (module_result == VK_SUCCESS && s->precompiled) {
            sh->precompiled = 1;
            memcpy(sh->stage_key, s->key, sizeof sh->stage_key);
            s2a_sha256 hash;
            s2a_sha256_init(&hash);
            s2a_sha256_update(&hash, ci.pCode, ci.codeSize);
            s2a_sha256_final(&hash, sh->spirv_digest);
        }
        if (module_result == VK_SUCCESS && sh->stage == NVN_GPU_SH_CS && nvn_plat_flag("nvk_land_bound", 0))
            sh->write_bounds = s2a_spirv_write_bounds(ci.pCode, ci.codeSize, sh->entry, sh->ents, sh->nent);
        free(array_code);
        if (module_result != VK_SUCCESS) {
            free(sh->tfmt); free(sh->ents); free(sh);
            return fail(err, NVN_GPU_E_COMPILE, "vkCreateShaderModule 失败（stage %d）", s->stage);
        }
    }
    sh->refs = 1;   // 核心持有
    pthread_mutex_lock(&g_mu);
    sh->all_next = g.shaders;
    g.shaders = sh;
    pthread_mutex_unlock(&g_mu);
    *out = sh;
    return rc(NVN_GPU_OK);
}

int nvn_vk_cold_guard_mode(void) { return g.cold_enabled; }

int nvn_vk_cold_shader_digest(nvn_gpu_shader shader, uint8_t sha[32]) {
    if (!g.cold_enabled || !shader || shader->stage != NVN_GPU_SH_FS || !shader->mod) return 0;
    memcpy(sha, shader->spirv_digest, 32);
    return 1;
}

uint64_t nvn_gpu_shader_write_end(nvn_gpu_shader sh, unsigned index, unsigned set, unsigned binding, uint32_t groups_x) {
    if (!sh || !sh->write_bounds || index >= sh->nent || !groups_x ||
        sh->ents[index].set != set || sh->ents[index].binding != binding) return 0;
    struct s2a_write_bound b = sh->write_bounds[index];
    return (uint64_t)(groups_x - 1) * b.group_stride + b.end_first_group;
}

void nvn_gpu_shader_release(nvn_gpu_shader sh) {
    struct nvn_gpu_shader_s *s = sh;
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->released) {
        s->released = 1;
        if (s->refs > 0) s->refs--;
        if (s->refs == 0) destroy_shader_locked(&g, s);
    }
    pthread_mutex_unlock(&g_mu);
}

static VkRenderPass make_pipeline_pass(const nvn_gpu_pipe_desc *d) {
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    uint32_t natt = 0, ncol = 0;
    VkSampleCountFlagBits samples = (VkSampleCountFlagBits)(d->samples > 1 ? d->samples : 1);
    uint32_t nslot = 0;   // 颜色槽个数 = 最后一个有格式的槽 + 1；中间没格式的槽是空位（VK_ATTACHMENT_UNUSED）
    for (uint32_t i = 0; i < 8; i++) if (d->cfmt[i]) nslot = i + 1;
    for (uint32_t i = 0; i < nslot; i++) {
        if (!d->cfmt[i]) { col[ncol].attachment = VK_ATTACHMENT_UNUSED; col[ncol].layout = VK_IMAGE_LAYOUT_UNDEFINED; ncol++; continue; }
        VkFormat f = nvn_vk_tex_format(d->cfmt[i]);
        if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = f;
        att[natt].samples = samples;
        att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col[ncol].attachment = natt;
        col[ncol].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ncol++;
        natt++;
    }
    int has_depth = 0;
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    // 深度模板附件：深度格式优先，只挂模板（纯 STENCIL8，dfmt = 0）时按 sfmt 挂——与开通道时
    //   「深度视图，没有就模板视图」同一口径，否则管线的兼容通道少一个附件、和实际通道不兼容（#1581 O）
    const uint32_t dsfmt = d->dfmt ? d->dfmt : d->sfmt;
    if (dsfmt) {
        VkFormat f = nvn_vk_tex_format(dsfmt);
        if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = f;
        att[natt].samples = samples;
        att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref.attachment = natt;
        has_depth = 1;
        natt++;
    }
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = ncol;
    sp.pColorAttachments = ncol ? col : NULL;
    sp.pDepthStencilAttachment = has_depth ? &dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rp;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = natt;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    OC(OC_RP);
    if (vkCreateRenderPass(g.d, &rp, NULL, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

static int same_spec(const nvn_gpu_stage_desc *a, const nvn_gpu_stage_desc *b) {
    if (a->sh != b->sh || a->nspec != b->nspec) return 0;
    return a->nspec == 0 || memcmp(a->spec, b->spec, (size_t)a->nspec * sizeof *a->spec) == 0;
}

// 图形管线按描述散列成桶。原先在全部管线的一条链表里逐条比：一款 3D 游戏几百条管线、每帧取 513 次，
// 真机上管线数翻一倍「开通道与取管线」每帧就多 1.3 毫秒（同一局里两种写法的管线并存时量到的）。
// 实际交给 Vulkan 的图元重启：只在条带 / 扇上有意义（列表图元打开要 primitiveTopologyListRestart，这里不开）。
// 管线缓存的散列与相等判定都按它比：原始的 prim_restart 在列表图元上也会被置（rp_draw 按游戏状态填），
// 拿它比会给列表图元多编一条一模一样的管线；不比则条带上开 / 关重启共用一条管线，有一边画错。
static int eff_restart(const nvn_gpu_pipe_desc *d) {
    VkPrimitiveTopology t = vk_topology(d->prim);
    return d->prim_restart && (t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                               t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY ||
                               t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY);
}

// 散列把 same_pipe_desc 比的全部字段都算进来：着色器句柄、特化常量（只比到 nspec 条）、图元 / 格式 / 采样数 / patch_size、光栅丢弃 / 负一裁剪 /
// 窗口原点 / 深度夹取、颜色格式、通道重映射、混合、颜色状态、写掩码、多重采样、顶点属性与顶点流两张表、深度模板、多边形。原先的散列只算了其中一部分：
// 同着色器同状态、只差特化常量 / 顶点布局 / 通道重映射的管线全落在同一个桶里，要靠逐字段比才分得开。
// 散列只决定落在哪个桶，相等与否仍由 same_pipe_desc 定（same_pipe_desc 相等 ⇒ 这里喂进去的每个字节都相等 ⇒ 散列相等；反过来不要求）。
// 以后给 same_pipe_desc 加比对字段，要同时加进这里（android/tests/vk 的 C205 逐字段表与描述大小检查会红）。
// 按 8 字节一个字混合（原先字节逐个 FNV：每个字节一次相依的乘法，约 350 字节）。
static inline uint64_t pipe_mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (; n >= 8; b += 8, n -= 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    return h;
}
static uint32_t pipe_hash(const nvn_gpu_pipe_desc *d) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    uintptr_t sh[5]; for (int i = 0; i < 5; i++) sh[i] = (uintptr_t)d->st[i].sh;
    h = pipe_mix(h, sh, sizeof sh);
    for (int i = 0; i < 5; i++) {   // 特化常量：same_pipe_desc 只比前 nspec 条
        const uint32_t ns = d->st[i].nspec;
        h = pipe_mix(h, &ns, sizeof ns);
        if (ns && ns <= NVN_GPU_SPEC_MAX) h = pipe_mix(h, d->st[i].spec, (size_t)ns * sizeof d->st[i].spec[0]);
    }
    const uint32_t v[6] = {(uint32_t)d->prim, (uint32_t)d->dfmt, (uint32_t)d->sfmt, (uint32_t)d->samples, (uint32_t)d->patch_size,
                           (uint32_t)eff_restart(d)};   // #1102：same_pipe_desc 按实际生效的图元重启比
    h = pipe_mix(h, v, sizeof v);
    const uint8_t fl[4] = {d->rast_discard, d->clip_neg1, d->origin_ll, d->depth_clamp};
    h = pipe_mix(h, fl, sizeof fl);
    h = pipe_mix(h, d->cfmt, sizeof d->cfmt); h = pipe_mix(h, d->lmap, sizeof d->lmap);
    h = pipe_mix(h, d->blend, sizeof d->blend); h = pipe_mix(h, &d->color, sizeof d->color);
    h = pipe_mix(h, &d->chmask, sizeof d->chmask); h = pipe_mix(h, &d->ms, sizeof d->ms); h = pipe_mix(h, &d->ds, sizeof d->ds);
    h = pipe_mix(h, d->attr, sizeof d->attr); h = pipe_mix(h, d->stream, sizeof d->stream);
    {   // poly_st 的位域只占部分对象位；散列和相等判定都不能依赖填充位。
        const uint8_t poly[] = {d->poly.cull, d->poly.front, d->poly.mode, d->poly.offset_en};
        h = pipe_mix(h, poly, sizeof poly);
    }
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
    return (uint32_t)(h ^ h >> 32);
}
// 登记一条图形管线进散列表（持 g_mu）：管线数超过桶数（装填因子 > 1）就把桶数翻倍，按各管线存下的散列重排；分配失败沿用旧表
// （链变长；表一直建不出来时查找退回整表逐条比：慢，答案不变）。
static int g_pipe_ht_test;   // 只给用例：1 = 查找一律整表逐条比，2 = 散列表分配当失败（走「表建不出来」的退路），4 = 起始桶数 16（多翻几次倍），8 = 只有增长时分配当失败（第一张表照常建）
static void pipe_ht_add(struct nvn_gpu_pipe_s *p) {
    if (!g_pipe_ht_n || g_pipe_cnt + 1 > g_pipe_ht_n) {
        const uint32_t n = g_pipe_ht_n ? g_pipe_ht_n * 2 : (g_pipe_ht_test & 4) ? 16u : PIPE_HT_N;
        struct nvn_gpu_pipe_s **nt = ((g_pipe_ht_test & 2) || ((g_pipe_ht_test & 8) && g_pipe_ht_n)) ? NULL : calloc(n, sizeof *nt);
        if (nt) {
            for (uint32_t i = 0; i < g_pipe_ht_n; i++)
                for (struct nvn_gpu_pipe_s *q = g_pipe_ht[i], *nx; q; q = nx) {
                    nx = q->hnext;
                    const uint32_t b = q->hash & (n - 1);
                    q->hnext = nt[b]; nt[b] = q;
                }
            free(g_pipe_ht); g_pipe_ht = nt; g_pipe_ht_n = n;
        }
    }
    g_pipe_cnt++;
    p->hnext = NULL;
    if (!g_pipe_ht_n) return;
    const uint32_t b = p->hash & (g_pipe_ht_n - 1);
    p->hnext = g_pipe_ht[b]; g_pipe_ht[b] = p;
}

static int same_pipe_desc(const nvn_gpu_pipe_desc *a, const nvn_gpu_pipe_desc *b) {
    if (a->prim != b->prim || a->patch_size != b->patch_size || a->dfmt != b->dfmt || a->sfmt != b->sfmt ||
        a->samples != b->samples || a->rast_discard != b->rast_discard || a->clip_neg1 != b->clip_neg1 ||
        eff_restart(a) != eff_restart(b))
        return 0;
    for (int i = 0; i < 5; i++)
        if (!same_spec(&a->st[i], &b->st[i])) return 0;
    for (int i = 0; i < 8; i++)
        if (a->cfmt[i] != b->cfmt[i]) return 0;
    return memcmp(a->lmap, b->lmap, sizeof a->lmap) == 0 &&
           memcmp(a->blend, b->blend, sizeof a->blend) == 0 &&
           memcmp(&a->color, &b->color, sizeof a->color) == 0 &&
           memcmp(&a->chmask, &b->chmask, sizeof a->chmask) == 0 &&
           memcmp(&a->ms, &b->ms, sizeof a->ms) == 0 &&
           memcmp(a->attr, b->attr, sizeof a->attr) == 0 &&
           memcmp(a->stream, b->stream, sizeof a->stream) == 0 &&
           memcmp(&a->ds, &b->ds, sizeof a->ds) == 0 &&
           a->poly.cull == b->poly.cull && a->poly.front == b->poly.front &&
           a->poly.mode == b->poly.mode && a->poly.offset_en == b->poly.offset_en &&
           a->origin_ll == b->origin_ll && a->depth_clamp == b->depth_clamp;
}
// 用例读写（android/tests/vk）：stat 的 which：0 散列桶数、1 图形管线数、2 累计逐字段比了的条数、3 累计取管线命中次数、4 累计链上走过的条数；
// hash / same 是 pipe_hash / same_pipe_desc 的对外口子（「相等 ⇒ 散列相等」的性质用例用）；test_mode 的位见 g_pipe_ht_test。
uint64_t nvn_vk_dbg_pipe_stat(int which) {
    switch (which) {
    case 0: return g_pipe_ht_n;
    case 1: return g_pipe_cnt;
    case 2: return __atomic_load_n(&g_pl_cmp, __ATOMIC_RELAXED);
    case 3: return __atomic_load_n(&g_pl_n, __ATOMIC_RELAXED);
    case 4: return __atomic_load_n(&g_pl_steps, __ATOMIC_RELAXED);
    default: return 0;
    }
}
uint32_t nvn_vk_dbg_pipe_hash(const nvn_gpu_pipe_desc *d) { return pipe_hash(d); }
int nvn_vk_dbg_pipe_same(const nvn_gpu_pipe_desc *a, const nvn_gpu_pipe_desc *b) { return same_pipe_desc(a, b); }
void nvn_vk_dbg_pipe_test_mode(int m) { g_pipe_ht_test = m; }

// 创建管线的并发边界（P1：原先在 g_create_mu 下跑完整个 pipe_build，后台预热一条配方时，无关配方的
// 首次 draw 也得排队；拿锁后又不复检缓存，已排队的同配方还会再建一次）。现在的分工：
//   · 缓存查找 / 安装 / warm 计数在 g_mu 下（短锁，见 pipe_find_locked / pipe_install_locked）；
//   · pipe_build 在锁外做，不同配方真并行——依据是 VkPipelineCache 没设
//     VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED（不设 = 内部同步，见 plc_open 的注释）；
//   · 同配方并发需求由「在途表」合并成一份创建（见 pipe_inflight 一节）。
// g_create_mu 只在设备策略兜底 debug.s2a.nvk_pipe_serial=1 时使用（把创建重新串行化）；
// 缺省 0 = 不同配方并发建。release 包 debug.s2a.* 属性门关着，读不到 → 缺省行为。
static pthread_mutex_t g_create_mu = PTHREAD_MUTEX_INITIALIZER;
// 设备策略兜底开关：每建一条读一次（建管线本身 0.2–1 毫秒起，release 包读属性直接返回缺省，不值得缓存）。
static int pipe_serial_on(void) { return nvn_plat_flag("nvk_pipe_serial", 0) != 0; }
// 预热（W3）：t_warm_scope = 本线程的创建算「预建」（预热线程设 1，见 nvk_pipe_warm_build）。
// 预建对象打 warm 位；真实 draw 第一次命中它时计 warm 命中。计数都是进程累计，nvn_gpu_stats 报。
static _Thread_local int t_warm_scope, t_demand_worker;
static _Atomic uint64_t g_sync_pipelines;
void nvk_pipe_demand_worker_enter(void) { t_demand_worker = 1; }   // 专用后台线程，退出时 TLS 一并释放
uint64_t rp_sync_pipelines_created(void) { return atomic_load_explicit(&g_sync_pipelines, memory_order_relaxed); }
static void note_sync_pipeline(void) {
    if (!t_warm_scope && !t_demand_worker)
        atomic_fetch_add_explicit(&g_sync_pipelines, 1, memory_order_relaxed);
}
static uint64_t g_warm_hits, g_warm_created, g_pipe_dup;
// 在途表读数（见 pipe_inflight）：同配方需求合并等待的次数 / 表满（或停用）自己直接建的次数 /
// 认领后复检命中的次数（省掉一次重复建）。
// 在途表读数由 nvn_pipe_flight 内部计数（见 nvn_gpu_stats 的 pipe_inflight_* 项）

void nvn_gpu_pipe_warm_scope(int on) { t_warm_scope = on ? 1 : 0; }
uint64_t nvn_gpu_pipe_warm_hits(void) { return __atomic_load_n(&g_warm_hits, __ATOMIC_RELAXED); }
uint64_t nvn_gpu_pipe_warm_created(void) { return __atomic_load_n(&g_warm_created, __ATOMIC_RELAXED); }
static unsigned pipe_duration_bucket(uint64_t ns) {
    if (ns < PIPE_DURATION_SUB_BUCKETS) return (unsigned)ns;
    const unsigned exponent = 63u - (unsigned)__builtin_clzll(ns);
    const unsigned shift = exponent - 3;
    return PIPE_DURATION_SUB_BUCKETS * shift + (unsigned)(ns >> shift);
}

static uint64_t pipe_duration_bucket_end(unsigned bucket) {
    if (bucket < PIPE_DURATION_SUB_BUCKETS) return bucket;
    const unsigned shift = bucket / PIPE_DURATION_SUB_BUCKETS - 1;
    const uint64_t mantissa = PIPE_DURATION_SUB_BUCKETS + bucket % PIPE_DURATION_SUB_BUCKETS;
    return (mantissa << shift) | ((UINT64_C(1) << shift) - 1);
}

// 只从实际 vkCreate 调用回来时记录；不进对象命中路径，不在驱动调用两侧加锁。
static void pipe_creation_note(int compute, uint64_t wall_ns, const VkPipelineCreationFeedback *feedback) {
    const unsigned bucket = (t_warm_scope ? NVK_PIPE_GFX_WARM : NVK_PIPE_GFX_DRAW) + compute;
    struct pipe_creation_counters *c = &g_pipe_creation[bucket];
    atomic_fetch_add_explicit(&c->wall_ns, wall_ns, memory_order_relaxed);
    if (!feedback || !(feedback->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
        atomic_fetch_add_explicit(&c->no_feedback, 1, memory_order_relaxed);
        return;
    }
    if (feedback->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
        atomic_fetch_add_explicit(&c->app_hit, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->driver_ns, feedback->duration, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->duration[pipe_duration_bucket(feedback->duration)], 1, memory_order_relaxed);
}

enum nvk_pipe_feedback_mode nvn_vk_pipe_creation_stats(struct nvk_pipe_creation_stats out[NVK_PIPE_CREATION_BUCKETS]) {
    const enum nvk_pipe_feedback_mode mode = atomic_load_explicit(&g_pipe_feedback, memory_order_acquire);
    for (unsigned i = 0; i < NVK_PIPE_CREATION_BUCKETS; i++) {
        const struct pipe_creation_counters *c = &g_pipe_creation[i];
        uint64_t hist[PIPE_DURATION_BUCKETS];
        uint64_t valid = 0;
        for (unsigned b = 0; b < PIPE_DURATION_BUCKETS; b++) {
            hist[b] = atomic_load_explicit(&c->duration[b], memory_order_relaxed);
            valid += hist[b];
        }
        uint64_t p95 = 0;
        if (valid) {
            uint64_t rank = valid - valid / 20; // ceil(0.95 * valid)
            for (unsigned b = 0; b < PIPE_DURATION_BUCKETS; b++) {
                if (rank <= hist[b]) {
                    p95 = pipe_duration_bucket_end(b);
                    break;
                }
                rank -= hist[b];
            }
        }
        const uint64_t unavailable = atomic_load_explicit(&c->no_feedback, memory_order_relaxed);
        out[i] = (struct nvk_pipe_creation_stats){
            .created = valid + unavailable,
            .valid = valid,
            .no_feedback = unavailable,
            .app_hit = atomic_load_explicit(&c->app_hit, memory_order_relaxed),
            .driver_ns = atomic_load_explicit(&c->driver_ns, memory_order_relaxed),
            .driver_p95_le_ns = p95,
            .wall_ns = atomic_load_explicit(&c->wall_ns, memory_order_relaxed),
        };
    }
    return mode;
}

// 清单标签（管线预热清单把「这条管线对应哪一槽」存在对象上，换回同一条管线时只加计数）。后端管线局内不淘汰、
// 不复用指针（销毁只在会话收尾整批做），所以标签随对象生灭；读写不加锁（单个 64 位原子）。
uint64_t nvn_gpu_pipe_warm_tag(nvn_gpu_pipe p) { return p ? __atomic_load_n(&((struct nvn_gpu_pipe_s *)p)->warm_tag, __ATOMIC_RELAXED) : 0; }
void nvn_gpu_pipe_warm_tag_set(nvn_gpu_pipe p, uint64_t tag) { if (p) __atomic_store_n(&((struct nvn_gpu_pipe_s *)p)->warm_tag, tag, __ATOMIC_RELAXED); }

// 缓存里按配方找一条图形管线（调用方持 g_mu）。散列只取描述里一部分字段（都是 same_pipe_desc 比的），
// 相等与否仍由 same_pipe_desc 定。
static struct nvn_gpu_pipe_s *pipe_find_locked(const nvn_gpu_pipe_desc *d) {
    const uint32_t ph = pipe_hash(d);
    static int scan = -1; static uint32_t scan_n;   // 诊断开关 nvk_pipe_scan=1：走原来的整表逐条比（同一局里对比用），每 256 次取管线读一次
    if (scan < 0 || !(++scan_n & 255)) scan = nvn_plat_flag("nvk_pipe_scan", 0) != 0;
    const int walk_all = scan || !g_pipe_ht_n || (g_pipe_ht_test & 1);   // 表建不出来（内存不够）时退回整表逐条比：慢，答案不变
    // 计时只在 nvk_gputime 开着才取（#1130）。开关值 g_draw_tm 由绘制入口 / 每 300 帧的读数块刷新，这里只读
    struct timespec lk0 = {0, 0}; if (g_draw_tm) clock_gettime(CLOCK_MONOTONIC, &lk0);
    uint32_t steps = 0, cmp = 0;
    for (struct nvn_gpu_pipe_s *q = walk_all ? g.pipes : g_pipe_ht[ph & (g_pipe_ht_n - 1)]; q; q = walk_all ? q->all_next : q->hnext) {
        steps++;
        if (q->hash != ph) continue;   // 存下的散列不同一定不同：不去碰 q->key 那一大块（计算管线的 hash 恒为 0，整表逐条比时也靠它跳过）
        cmp++;
        if (same_pipe_desc(&q->key, d)) {
            if (g_draw_tm) { struct timespec lk1; clock_gettime(CLOCK_MONOTONIC, &lk1);   // 诊断读数：查找这一段用了多久、走了多少条、逐字段比了多少条
              __atomic_fetch_add(&g_pl_ns, (uint64_t)(lk1.tv_sec - lk0.tv_sec) * 1000000000ull + (uint64_t)lk1.tv_nsec - (uint64_t)lk0.tv_nsec, __ATOMIC_RELAXED); }
            __atomic_fetch_add(&g_pl_n, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_pl_steps, steps, __ATOMIC_RELAXED); __atomic_fetch_add(&g_pl_cmp, cmp, __ATOMIC_RELAXED);
            return q;
        }
    }
    return NULL;
}

// ---- 配方级在途表（P1：同配方合并成一份创建，不同配方不互相排队）----
// 表项是一次「配方 → 创建」的汇合点：建者占一项并真的去建；同配方的其它需求挂在这一项上等自己的结果，
// 不占全局锁、也不让无关配方排队。容量固定 128（同时最多 128 种配方在途）；满了不排队、这一次自己
// 直接建并计数（安装时双检兜底，不会出现两份同配方对象）。
// 线程 / 锁：只用 g_pi_mu 保护；它从不与 g_mu / g_create_mu 嵌套持有（先放缓存锁再来这里，发布结果前
//   也已放掉 g_mu），所以不会出现「建者持 g_mu 等表锁 / 等待者持表锁等 g_mu」的死锁；
//   pthread_cond_wait 期间释放 g_pi_mu。cond 在进程内首次使用时初始化（pthread_once）；任意一只初始化
//   失败就整张表停用（退回不合并的同步建：功能不受影响，只在读数上记 g_pi_full）。
// 生命周期：表项不拥有对象，只记一次创建的结果。成功对象在 g_mu 下装进 g.pipes，等待者醒来拿到的是
//   同一份已安装的指针；后端 close 之后不再有调用（close 的前提是前端静默 / 预热线程已 join，
//   与改动前相同，本表不延长会话生命期）。
// 逻辑全部在 nvn/nvn_pipe_flight.c（纯逻辑、宿主单测覆盖）；这里只做后端接线：配方键就是
// nvn_gpu_pipe_desc，同一配方的判据与缓存一致（same_pipe_desc）。
static nvk_pf g_pi;
static pthread_once_t g_pi_once = PTHREAD_ONCE_INIT;
static void pi_init(void) { nvk_pf_init(&g_pi, sizeof(nvn_gpu_pipe_desc)); }
static void pi_ensure(void) { pthread_once(&g_pi_once, pi_init); }
static int pi_eq(const void *a, const void *b) { return same_pipe_desc((const nvn_gpu_pipe_desc *)a, (const nvn_gpu_pipe_desc *)b); }

static void destroy_pipe_objects(struct nvn_gpu_pipe_s *p) {
    if (!p) return;
    if (p->pipe) vkDestroyPipeline(g.d, p->pipe, NULL);
    if (p->pass) vkDestroyRenderPass(g.d, p->pass, NULL);
    if (p->layout) vkDestroyPipelineLayout(g.d, p->layout, NULL);
    // 描述符组布局（p->dsl / p->dlay）归布局表所有，会话收尾由 dlay_close 销毁，这里只借用过、不销毁
    for (int i = 0; i < 6; i++) { free(p->stage_ents[i]); free(p->stage_tfmt[i]); free(p->stage_dyn[i]); }
    free(p);
}

// 取一组的布局（#1096）：先按结构在布局表里找，找到就共用，没有才向驱动建一个。b[0..nb) 是这一组的全部绑定（nb ≤ 32），顺序随意，这里按 binding 升序排好再比。
// 返回表项（句柄在 ->dsl）；NULL = 失败：*oom = 1 是主机内存不够，0 是驱动不给建。调用方持有的只是借用，不销毁。
static struct dset_lay *dlay_get(const VkDescriptorSetLayoutBinding *b, uint32_t nb, int *oom) {
    struct dset_lb k[32];
    uint64_t h = 1469598103934665603ull ^ nb;
    *oom = 0;
    for (uint32_t i = 0; i < nb; i++) {
        const struct dset_lb x = {b[i].binding, (uint32_t)b[i].descriptorType, b[i].descriptorCount, (uint32_t)b[i].stageFlags};
        uint32_t j = i;
        while (j && k[j - 1].binding > x.binding) { k[j] = k[j - 1]; j--; }
        k[j] = x;
    }
    for (uint32_t i = 0; i < nb; i++) {
        h = (h ^ k[i].binding) * 1099511628211ull; h = (h ^ k[i].type) * 1099511628211ull;
        h = (h ^ k[i].count) * 1099511628211ull; h = (h ^ k[i].stages) * 1099511628211ull;
    }
    h ^= h >> 29;
    pthread_mutex_lock(&g_dlay_mu);
    g_dlay_ask++;
    for (struct dset_lay *e = g_dlay_ht_n ? g_dlay_ht[h & (g_dlay_ht_n - 1)] : NULL; e; e = e->hnext)
        if (e->hash == h && e->nb == nb && memcmp(e->b, k, nb * sizeof k[0]) == 0) { g_dlay_hit++; pthread_mutex_unlock(&g_dlay_mu); return e; }
    if (g_dlay_n >= g_dlay_ht_n) {   // 装填因子到 1：桶数翻倍重排（分配不到就沿用旧表：链会长一点，答案不变；一张表都没有时才算失败）
        const uint32_t n = g_dlay_ht_n ? g_dlay_ht_n * 2 : 16;
        struct dset_lay **nt = calloc(n, sizeof *nt);
        if (nt) {
            for (struct dset_lay *e = g_dlay_all; e; e = e->all_next) { e->hnext = nt[e->hash & (n - 1)]; nt[e->hash & (n - 1)] = e; }
            free(g_dlay_ht); g_dlay_ht = nt; g_dlay_ht_n = n;
        } else if (!g_dlay_ht_n) { pthread_mutex_unlock(&g_dlay_mu); *oom = 1; return NULL; }
    }
    struct dset_lay *e = calloc(1, sizeof *e + nb * sizeof k[0]);
    if (!e) { pthread_mutex_unlock(&g_dlay_mu); *oom = 1; return NULL; }
    VkDescriptorSetLayoutBinding sb[32];
    for (uint32_t i = 0; i < nb; i++) {
        memset(&sb[i], 0, sizeof sb[i]);
        sb[i].binding = k[i].binding; sb[i].descriptorType = (VkDescriptorType)k[i].type;
        sb[i].descriptorCount = k[i].count; sb[i].stageFlags = (VkShaderStageFlags)k[i].stages;
    }
    VkDescriptorSetLayoutCreateInfo li;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = nb;
    li.pBindings = nb ? sb : NULL;
    if (vkCreateDescriptorSetLayout(g.d, &li, NULL, &e->dsl) != VK_SUCCESS) { free(e); pthread_mutex_unlock(&g_dlay_mu); return NULL; }
    e->hash = h; e->nb = nb; memcpy(e->b, k, nb * sizeof k[0]);
    e->hnext = g_dlay_ht[h & (g_dlay_ht_n - 1)]; g_dlay_ht[h & (g_dlay_ht_n - 1)] = e;
    e->all_next = g_dlay_all; g_dlay_all = e; g_dlay_n++; g_dlay_made++;
    pthread_mutex_unlock(&g_dlay_mu);
    return e;
}
// 会话收尾（所有管线都已销毁、设备空闲）：销毁全部布局，布局表清空
static void dlay_close(void) {
    pthread_mutex_lock(&g_dlay_mu);
    for (struct dset_lay *e = g_dlay_all, *nx; e; e = nx) { nx = e->all_next; vkDestroyDescriptorSetLayout(g.d, e->dsl, NULL); g_dlay_gone++; free(e); }
    g_dlay_all = NULL; free(g_dlay_ht); g_dlay_ht = NULL; g_dlay_ht_n = g_dlay_n = 0; g_dlay_ask = g_dlay_hit = 0;
    pthread_mutex_unlock(&g_dlay_mu);
}

// 建一套图形管线对象（只碰驱动与 VkPipelineCache，不装缓存、不碰 g.pipes 表）。
// 线程：在 g_mu / g_pi_mu 之外执行，不同配方的多条线程（[提交] / [后台] 预热 / 其它提交线程）会同时进来。
// 并发只读清单（P1 复查过，全部只读或由各自对象同步；除布局表（在 g_dlay_mu 下）外，这里不写任何共享状态）：
//   · d->st[st].sh 指向的 struct nvn_gpu_shader_s：mod / entry / nent / ents / tfmt / push_off / push_bytes /
//     missing_storage_format_feature 都是 nvn_gpu_shader_load 装载期写死的（ents / tfmt 就是「描述符布局缓存」
//     本身，建管线只读它、往自己的 p->stage_* 里拷）。refs / released 是唯一会被并发写的字段，但只在 g_mu 下
//     由 nvn_gpu_shader_release 与 install_link（装缓存时加引用）改，本函数不读写它们。
//     生命期不在本函数内建立：调用方必须保证 desc 里各阶段的着色器在整个调用期间有效（核心 / 预热 owner 的
//     程序借用合同）；安装成功后再由管线持有一份引用。
//   · g.dev / g.caps / g_plc：会话打开后到 close 前不变（g_plc 由驱动内部同步，见 plc_open）；
//     close 的前提是前端已静默、预热线程已 join，不再有在建的管线。
//   · 渲染通道 / 管线布局 / 管线对象都是本函数新建的私有对象，不共享。描述符 set layout 取自布局表（dlay_get，#1096）：
//     表与表里的布局归会话，全程在 g_dlay_mu 下读写（叶子锁，持着它不取别的锁），这里只借用、不销毁。
// 不读的：g.pipes / g_pipe_ht（装缓存时才在 g_mu 下碰，见 pipe_install_locked）、通道对象缓存 g_pc_*（那是
//   通道录制路径的缓存，跟建管线无关）。以后往这里加共享可写状态必须先加锁或改成构建期私有，不能裸加。
// 装缓存由 pipe_install_locked 做（g_mu 下双检 + 着色器引用）。
static const char *pool_pipeline_preflight(const struct nvn_gpu_pipe_s *, const nvn_gpu_pipe_desc *, const struct nvn_gpu_shader_s *);
// 建管线的 Vulkan 调用失败：VkResult 原名带进 why（回放层按「返回码 + 原文」分类，rp_draw.c rp_pfail_kind），返回码按结果分——
// 内存类（主存 / 显存 / 池）= NVN_GPU_E_OOM（暂时性，回放层退避重试）；设备丢失 = NVN_GPU_E_DEVICE（终态）；其余用调用方给的确定性码。
static const char *pipe_vk_res_name(VkResult r) {
    switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VkResult";
    }
}
static int pipe_vk_res_code(VkResult r, int fixed) {
    if (r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_POOL_MEMORY) return NVN_GPU_E_OOM;
    if (r == VK_ERROR_DEVICE_LOST) return NVN_GPU_E_DEVICE;
    return fixed;
}
static nvn_gpu_rc pipe_build(const nvn_gpu_pipe_desc *d, struct nvn_gpu_pipe_s **out, nvn_gpu_err *err) {
    *out = NULL;
    const nvn_gpu_pipe_desc *requested = d;
    nvn_gpu_pipe_desc normalized;
    if (!pipe_msaa_normalize(d, &normalized)) return fail(err, NVN_GPU_E_UNSUPPORTED, "msaa_pipeline_samples: 附件格式没有共同支持的采样数");
    d = &normalized;
    // NVN POINT=0 / LINE=1 / FILL=2；完整 poly 已由 same_pipe_desc 纳入缓存键。
    if (d->poly.mode > 2) return fail(err, NVN_GPU_E_ARG, "未知多边形填充模式 %u", (unsigned)d->poly.mode);
    if (d->poly.mode != 2 && !g.dev->core_features.fillModeNonSolid)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "非填充多边形需要已启用的 fillModeNonSolid");
    // 片元阶段可以没有：只写深度的管线（深度预通道、阴影图），颜色附件上没有输出
    if (!d->st[NVN_GPU_SH_VS].sh)
        return fail(err, NVN_GPU_E_ARG, "图形管线至少需要顶点阶段");
    for (int st = 0; st < 5; st++)
        if (d->st[st].sh && d->st[st].sh->missing_storage_format_feature)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "设备未启用无格式存储图像所需特性");
    for (int st = 0; st < 5; st++)
        if (d->st[st].sh && d->st[st].sh->missing_vertex_feature)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "阶段 %d 的着色器要设备特性 %s，设备没启用（#1072）", st, d->st[st].sh->missing_vertex_feature);
    if (d->st[NVN_GPU_SH_GS].sh && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "GS 未启用或探测未通过");
    if (d->st[NVN_GPU_SH_GS].sh && d->prim != 0 && d->prim != 1 && d->prim != 4)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "GS 非原生图元未实现");
    for (int i = NVN_GPU_SH_TCS; i < 5; i++)
        if (d->st[i].sh) return fail(err, NVN_GPU_E_UNSUPPORTED, "细分阶段：本段未实现");

    struct merged { uint8_t set; uint16_t binding; uint8_t cls; uint16_t count; uint32_t stages; int16_t dbase; } m[64];   // dbase：动态 UBO 在所属组里的动态序号起点，-1 = 静态
    uint32_t nm = 0, maxset = 0;
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh) continue;
        for (uint32_t i = 0; i < sh->nent; i++) {
            const nvn_gpu_layout_ent *e = &sh->ents[i];
            if (e->cls == NVN_GPU_RES_VA)
                return fail(err, NVN_GPU_E_UNSUPPORTED,
                            "VA（设备地址）类别：契约未定非 bufferDeviceAddress 的消费方式，不报绿（请契约属主定夺）");
            if (vk_desc_type(e->cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM)
                return fail(err, NVN_GPU_E_UNSUPPORTED, "资源类别 %u：本段未实现", e->cls);
            uint32_t j = 0;
            for (; j < nm; j++)
                if (m[j].set == e->set && m[j].binding == e->binding) break;
            uint32_t stagebit = 1u << st;
            if (j == nm) {
                if (nm == 64) return fail(err, NVN_GPU_E_UNSUPPORTED, "契约：描述符项超过 64");
                m[nm++] = (struct merged){e->set, e->binding, e->cls, e->count ? e->count : 1, stagebit, -1};
            } else {
                if (m[j].cls != e->cls || m[j].count != (e->count ? e->count : 1))
                    return fail(err, NVN_GPU_E_LAYOUT, "set %u binding %u 的阶段布局冲突", e->set, e->binding);
                m[j].stages |= stagebit;
            }
            if (e->set > maxset) maxset = e->set;
        }
    }
    if (maxset >= 4) return fail(err, NVN_GPU_E_LAYOUT, "契约：set %u 超过四 set 上限", maxset);

    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    if (!p) return fail(err, NVN_GPU_E_OOM, "管线结构分配失败");
    p->nset = nm ? maxset + 1 : 0;
    // 动态 UBO 偏移（#1233）：备忘开着（nvk_desc_memo，缺省开）时，这条管线布局里的 UBO 个数不超过设备的动态 UBO 上限
    // （maxDescriptorSetUniformBuffersDynamic，按整条管线布局计；也不超过 32）就全部做成动态——
    // 描述符里只放缓冲和范围，偏移在绑定时给；这样「只是偏移不同」的两组内容完全一样，不用重写也不用另分一组，备忘才有命中。超限的管线保持静态（按设备能力取舍，不是 A/B 开关）。
    uint32_t dyn_n = 0;
    for (uint32_t i = 0; i < nm; i++) if (m[i].cls == NVN_GPU_RES_UBO) dyn_n += m[i].count;
    const int ubo_dyn = dyn_n && dyn_n <= g_max_dyn_ubo && dyn_n <= 32 && desc_memo_flag();
    if (dyn_n) __atomic_fetch_add(&g_dyn_pipes[1], 1, __ATOMIC_RELAXED);
    if (ubo_dyn) __atomic_fetch_add(&g_dyn_pipes[0], 1, __ATOMIC_RELAXED);
    for (uint32_t set = 0; set < p->nset; set++) {
        VkDescriptorSetLayoutBinding b[32];
        uint32_t nb = 0;
        if (ubo_dyn) {   // 本组每条动态 UBO 的组内动态序号：按 binding 升序累加个数（Vulkan 按这个顺序消费动态偏移）
            for (uint32_t i = 0; i < nm; i++) {
                if (m[i].set != set || m[i].cls != NVN_GPU_RES_UBO) continue;
                uint32_t base = 0;
                for (uint32_t j = 0; j < nm; j++) if (m[j].set == set && m[j].cls == NVN_GPU_RES_UBO && m[j].binding < m[i].binding) base += m[j].count;
                m[i].dbase = (int16_t)base;
                p->ndyn[set] = (uint8_t)(p->ndyn[set] + m[i].count);
            }
            p->dyn_all = (uint8_t)(p->dyn_all + p->ndyn[set]);
        }
        for (uint32_t i = 0; i < nm; i++) {
            if (m[i].set != set) continue;
            if (nb == 32) { free(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "契约：单 set 描述符项超过 32"); }
            memset(&b[nb], 0, sizeof b[nb]);
            b[nb].binding = m[i].binding;
            b[nb].descriptorType = m[i].dbase >= 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : vk_desc_type(m[i].cls);
            b[nb].descriptorCount = m[i].count;
            b[nb].stageFlags = 0;
            for (int st = 0; st < 5; st++)
                if (m[i].stages & (1u << st)) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);
            nb++;
        }
        int oom = 0;
        p->dlay[set] = dlay_get(b, nb, &oom);   // 结构相同的布局全后端共用一个（#1096）；布局归布局表，这里只借用
        if (!p->dlay[set]) {
            free(p);
            if (oom) return fail(err, NVN_GPU_E_OOM, "布局表项分配失败（set %u）", set);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateDescriptorSetLayout 失败（set %u）", set);
        }
        p->dsl[set] = p->dlay[set]->dsl;
    }
    { const char *limit = pool_pipeline_preflight(p, d, NULL); if (limit) { free(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "%s", limit); } }
    VkPushConstantRange pr[6];
    uint32_t npr = 0;
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh || !sh->push_bytes) continue;
        if ((sh->push_off | sh->push_bytes) & 3) {
            free(p);
            return fail(err, NVN_GPU_E_LAYOUT, "契约：阶段 %d push 范围 [%u,+%u) 未 4 字节对齐", st, sh->push_off, sh->push_bytes);
        }
        if (sh->push_off > g.caps.max_push_bytes || sh->push_bytes > g.caps.max_push_bytes - sh->push_off) {
            free(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "本机：阶段 %d push 范围 [%u,+%u) 超出设备上限 %u",
                        st, sh->push_off, sh->push_bytes, g.caps.max_push_bytes);
        }
        uint32_t k;
        for (k = 0; k < npr; k++)
            if (pr[k].offset == sh->push_off && pr[k].size == sh->push_bytes) {
                pr[k].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);
                break;
            }
        if (k == npr)
            pr[npr++] = (VkPushConstantRange){vk_shader_stage((nvn_gpu_stage)st), sh->push_off, sh->push_bytes};
    }
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = p->nset;
    pli.pSetLayouts = p->nset ? p->dsl : NULL;
    pli.pushConstantRangeCount = npr;
    pli.pPushConstantRanges = npr ? pr : NULL;
    { const VkResult vr = vkCreatePipelineLayout(g.d, &pli, NULL, &p->layout);
      if (vr != VK_SUCCESS) {
          free(p);
          return fail(err, pipe_vk_res_code(vr, NVN_GPU_E_UNSUPPORTED), "vkCreatePipelineLayout 失败（%s %d）", pipe_vk_res_name(vr), (int)vr);
      } }
    p->pass = make_pipeline_pass(d);
    if (!p->pass) {
        vkDestroyPipelineLayout(g.d, p->layout, NULL);
        free(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "管线 render pass 创建失败（附件格式 / 采样数）");
    }

    VkPipelineShaderStageCreateInfo stages[5];
    uint32_t ns = 0;
    for (int st = 0; st < 5; st++) {
        if (!d->st[st].sh) continue;
        memset(&stages[ns], 0, sizeof stages[ns]);
        stages[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[ns].stage = vk_shader_stage((nvn_gpu_stage)st);
        stages[ns].module = d->st[st].sh->mod;
        stages[ns].pName = d->st[st].sh->entry;
        ns++;
    }

    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    VkVertexInputBindingDivisorDescriptionEXT vdiv[16];   // #977：实例除数大于 1 的流
    uint32_t nvb = 0, nva = 0, ndiv = 0;
    for (int i = 0; i < 16; i++) {
        if (!d->attr[i].fmt) continue;
        VkFormat f = nvn_vk_format(nvn_vattr_fmt_canon(d->attr[i].fmt));   // RGBX → 同宽 RGB（X 不参与）
        if (f == VK_FORMAT_UNDEFINED) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "顶点属性格式 0x%x 无映射", d->attr[i].fmt);
            destroy_pipe_objects(p);
            return bad;
        }
        // 必须 CPU 解码的格式（32 位 UI2F/I2F）：本后端没有解码合成 binding，按映射出的整数
        // 格式绑定会把整数位模式当 float 静默读错值——明确报红，不返回成功兜底。
        {   // #975：走退回格式的 16 位打包格式，分量位置只在采样视图里补回；顶点取数按格式原样解，分量会错位
            uint8_t map[4];
            if (nvn_vk_format_chanmap(nvn_vattr_fmt_canon(d->attr[i].fmt), map)) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "顶点属性格式 0x%x：本设备走退回格式，顶点取数不经通道映射", d->attr[i].fmt);
                destroy_pipe_objects(p);
                return bad;
            }
        }
        if (nvn_vattr_must_cpu_decode(d->attr[i].fmt)) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED,
                                  "顶点属性格式 0x%x 需要 CPU 顶点解码（本后端未实现）", d->attr[i].fmt);
            destroy_pipe_objects(p);
            return bad;
        }
        uint32_t stream = d->attr[i].stream;
        int found = 0;
        for (uint32_t k = 0; k < nvb; k++) if (vb[k].binding == stream) found = 1;
        if (!found) {
            vb[nvb].binding = stream;
            vb[nvb].stride = d->stream[stream].stride;
            vb[nvb].inputRate = d->stream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            nvb++;
            // NVN 除数：0 = 逐顶点，1 = 每实例一次，n = 每 n 个实例一次（同 iOS stepRate = divisor）。
            // 大于 1 要 VK_EXT_vertex_attribute_divisor；没有或超过设备上限就不建，不静默退回每实例一次。
            uint32_t dv = d->stream[stream].divisor;
            if (dv > 1) {
                if (!g.dev->vertex_divisor_enabled || dv > g.dev->max_vertex_attrib_divisor) {
                    nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "顶点流 %u：实例除数 %u 要 VK_EXT_vertex_attribute_divisor（本设备%s，上限 %u）",
                                          stream, dv, g.dev->vertex_divisor_enabled ? "超上限" : "没有", g.dev->max_vertex_attrib_divisor);
                    destroy_pipe_objects(p);
                    return bad;
                }
                vdiv[ndiv].binding = stream;
                vdiv[ndiv].divisor = dv;
                ndiv++;
            }
        }
        va[nva].location = (uint32_t)i;
        va[nva].binding = stream;
        va[nva].format = f;
        va[nva].offset = d->attr[i].offset;
        nva++;
    }

    VkPipelineVertexInputStateCreateInfo vin;
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = nvb;
    vin.pVertexBindingDescriptions = vb;
    vin.vertexAttributeDescriptionCount = nva;
    vin.pVertexAttributeDescriptions = va;
    VkPipelineVertexInputDivisorStateCreateInfoEXT vdi;
    memset(&vdi, 0, sizeof vdi);
    vdi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    vdi.vertexBindingDivisorCount = ndiv;
    vdi.pVertexBindingDivisors = vdiv;
    if (ndiv) vin.pNext = &vdi;

    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = vk_topology(d->prim);
    // 重启只在条带 / 扇上有意义；列表图元打开需要 primitiveTopologyListRestart，这里忽略（见 eff_restart）
    ia.primitiveRestartEnable = eff_restart(d) ? VK_TRUE : VK_FALSE;
    if (ia.topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "图元 %u：本段只实现三角形", d->prim);
        destroy_pipe_objects(p);
        return bad;
    }

    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    // clip_neg1：裁剪深度 [-1, 1]（顶点着色器没做 near = -w 的深度改写，见 nvn_backend.h；只在 CAP_DEPTH_CLIP_CTL 时会是 1）
    VkPipelineViewportDepthClipControlCreateInfoEXT vp_dcc;
    memset(&vp_dcc, 0, sizeof vp_dcc);
    vp_dcc.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
    vp_dcc.negativeOneToOne = VK_TRUE;
    if (d->clip_neg1) {
        if (!g.dev->depth_clip_control_enabled) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "裁剪深度 [-1, 1]：设备没开 VK_EXT_depth_clip_control");
            destroy_pipe_objects(p);
            return bad;
        }
        vp.pNext = &vp_dcc;
    }

    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = d->poly.mode == 0 ? VK_POLYGON_MODE_POINT : d->poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = d->poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : d->poly.cull == 2 ? VK_CULL_MODE_BACK_BIT
                 : d->poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    // 正面绕序（契约 v6）：poly.front 是 y 朝上的规范化设备坐标里的绕序。Vulkan 在帧缓冲坐标（y 朝下）里判正面：
    // 视口不翻（原点左下，见 nvn_gpu_draw）时帧缓冲里看到的绕序与它相反；视口翻了（原点左上，负高度）时相同。
    { int ccw_ndc = d->poly.front != 0, ccw_fb = d->origin_ll ? !ccw_ndc : ccw_ndc;
      rs.frontFace = ccw_fb ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; }
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = d->depth_clamp ? VK_TRUE : VK_FALSE;   // 设备特性 depthClamp 是必需项（vk_device.c）
    rs.depthBiasEnable = (d->poly.offset_en & (1u << d->poly.mode)) ? VK_TRUE : VK_FALSE;   // NVN 点 / 线 / 填充偏移位分别为 1 / 2 / 4
    rs.rasterizerDiscardEnable = d->rast_discard ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(d->samples > 1 ? d->samples : 1);
    // #977：NVN 透明度转覆盖率（核心字段，不用扩展；同 iOS alphaToCoverageEnabled）。a2c_dither 的抖动样式由实现自定，Vulkan 不给开关。
    ms.alphaToCoverageEnable = d->ms.a2c ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (d->dfmt) {
        ds.depthTestEnable = d->ds.dt ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = d->ds.dw ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = vk_depth_func(d->ds.df);
        ds.depthBoundsTestEnable = VK_FALSE;
    }
    if (d->sfmt && d->ds.st) {   // 模板：比较函数与三种操作进管线；读掩码 / 写掩码 / 参考值是动态状态（apply_dyn）。通道没有模板附件时测试恒过，不开
        ds.stencilTestEnable = VK_TRUE;
        ds.front = (VkStencilOpState){vk_stencil_op(d->ds.fail_f), vk_stencil_op(d->ds.zpass_f), vk_stencil_op(d->ds.zfail_f), vk_depth_func(d->ds.sf_f), 0xff, 0xff, 0};
        ds.back = (VkStencilOpState){vk_stencil_op(d->ds.fail_b), vk_stencil_op(d->ds.zpass_b), vk_stencil_op(d->ds.zfail_b), vk_depth_func(d->ds.sf_b), 0xff, 0xff, 0};
    }

    VkLogicOp logic_op = vk_logic_op(d->color.logic_op);
    if (logic_op == VK_LOGIC_OP_MAX_ENUM) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_ARG, "逻辑操作 %u 非法", d->color.logic_op);
        destroy_pipe_objects(p);
        return bad;
    }
    int logic_enabled = d->color.logic_op != NVN_LOGIC_OP_COPY;
    if (logic_enabled && !g.dev->core_features.logicOp) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "逻辑操作 %u：设备未启用 logicOp", d->color.logic_op);
        destroy_pipe_objects(p);
        return bad;
    }
    VkPipelineColorBlendAttachmentState cb[8];
    uint32_t ncb = 0, nslot = 0;
    for (uint32_t i = 0; i < 8; i++) if (d->cfmt[i]) nslot = i + 1;
    for (uint32_t i = 0; i < nslot; i++) {
        memset(&cb[ncb], 0, sizeof cb[ncb]);
        if (!d->cfmt[i]) { ncb++; continue; }   // 空槽：占一个位置，不混合、不写
        // 整数格式的目标从不混合（硬件忽略它的混合状态；Vulkan 上整数附件开混合非法）
        nvn_fmt_info cfi; int is_int = nvn_fmt_info_of(d->cfmt[i], &cfi) && (cfi.kind == NVN_FMT_UINT || cfi.kind == NVN_FMT_SINT);
        if (!logic_enabled && ((d->color.blend_en >> i) & 1u) && !is_int) {
            const blend_st *b = &d->blend[i];
            if (b->adv_mode) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "高级混合（adv_mode）：本段未实现（target %u）", i);
                destroy_pipe_objects(p);
                return bad;
            }
            uint32_t fs[4] = {b->src_rgb, b->dst_rgb, b->src_a, b->dst_a};
            for (int k = 0; k < 4; k++)
                if (fs[k] >= 0x10 && fs[k] <= 0x13 && !g.dev->core_features.dualSrcBlend) {
                    nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "双源混合因子：设备未启用 dualSrcBlend（target %u）", i);
                    destroy_pipe_objects(p);
                    return bad;
                }
            VkBlendFactor sf = vk_blend_factor(b->src_rgb), df = vk_blend_factor(b->dst_rgb);
            VkBlendFactor sa = vk_blend_factor(b->src_a), da = vk_blend_factor(b->dst_a);
            VkBlendOp er = vk_blend_op(b->eq_rgb), ea = vk_blend_op(b->eq_a);
            if (sf == VK_BLEND_FACTOR_MAX_ENUM || df == VK_BLEND_FACTOR_MAX_ENUM || sa == VK_BLEND_FACTOR_MAX_ENUM ||
                da == VK_BLEND_FACTOR_MAX_ENUM || er == VK_BLEND_OP_MAX_ENUM || ea == VK_BLEND_OP_MAX_ENUM) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "混合因子 / 运算：target %u 未实现", i);
                destroy_pipe_objects(p);
                return bad;
            }
            cb[ncb].blendEnable = VK_TRUE;
            cb[ncb].srcColorBlendFactor = sf; cb[ncb].dstColorBlendFactor = df; cb[ncb].colorBlendOp = er;
            cb[ncb].srcAlphaBlendFactor = sa; cb[ncb].dstAlphaBlendFactor = da; cb[ncb].alphaBlendOp = ea;
        }
        uint32_t mask = (d->chmask.mask >> (i * 4)) & 0xF;   // 0 = 这个目标四个通道都不写（只写深度的绘制），不是「没设」
        cb[ncb].colorWriteMask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                 ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
        ncb++;
    }
    VkPipelineColorBlendStateCreateInfo cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    // COPY 保留原混合路径；其余操作取代混合。Vulkan 对 float / sRGB 附件自动跳过逻辑运算。
    cbs.logicOpEnable = logic_enabled ? VK_TRUE : VK_FALSE;
    cbs.logicOp = logic_op;
    cbs.attachmentCount = ncb;
    cbs.pAttachments = cb;

    VkDynamicState dyn[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                             VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dyns;
    memset(&dyns, 0, sizeof dyns);
    dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 7;
    dyns.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp;
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = ns;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dyns;
    gp.layout = p->layout;
    gp.renderPass = p->pass;
    gp.subpass = 0;
    VkPipelineCreationFeedback feedback = {0};
    VkPipelineCreationFeedbackCreateInfo feedback_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pNext = gp.pNext,
        .pPipelineCreationFeedback = &feedback,
    };
    const int want_feedback = atomic_load_explicit(&g_pipe_feedback, memory_order_relaxed) == NVK_PIPE_FEEDBACK_ON;
    if (want_feedback) gp.pNext = &feedback_info;
    const uint64_t pl_t0 = now_ns();   // 驱动编译管线是同步的：超过 3 毫秒就打一行（走路/进新场景时会集中出现）
    const VkResult gvr = vkCreateGraphicsPipelines(g.d, g_plc, 1, &gp, NULL, &p->pipe);
    const uint64_t wall_ns = now_ns() - pl_t0;
    pipe_creation_note(0, wall_ns, gvr == VK_SUCCESS && want_feedback ? &feedback : NULL);
    if (gvr != VK_SUCCESS) {
        nvn_gpu_rc bad = fail(err, pipe_vk_res_code(gvr, NVN_GPU_E_COMPILE), "vkCreateGraphicsPipelines 失败（%s %d）", pipe_vk_res_name(gvr), (int)gvr);
        destroy_pipe_objects(p);
        return bad;
    }
    plc_created();
    { // 原有长帧读数也含 plc_created 的局中补写；PSO feedback 的墙钟只量 vkCreate。
      static int slow = -1; if (slow < 0) slow = nvn_plat_flag("nvk_hitch_log", 0) != 0;
      const uint64_t dt = slow ? now_ns() - pl_t0 : 0;
      if (dt > 3000000ull) { char b[160]; snprintf(b, sizeof b, "图形管线编译 %.1f 毫秒", (double)dt / 1e6); nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe", b); } }
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh) continue;
        if (sh->precompiled) p->lib_stages |= 1u << st;
        p->stage_nent[st] = sh->nent;
        p->stage_push_off[st] = sh->push_off;
        p->stage_push_bytes[st] = sh->push_bytes;
        if (sh->nent) {
            p->stage_ents[st] = calloc(sh->nent, sizeof *p->stage_ents[st]);
            if (!p->stage_ents[st]) {
                destroy_pipe_objects(p);
                return fail(err, NVN_GPU_E_OOM, "图形管线阶段布局分配失败");
            }
            memcpy(p->stage_ents[st], sh->ents, (size_t)sh->nent * sizeof *p->stage_ents[st]);
            if (ubo_dyn) {   // 每一项的动态序号起点：和上面布局里的同一张 m 表对着取
                p->stage_dyn[st] = calloc(sh->nent, sizeof *p->stage_dyn[st]);
                if (!p->stage_dyn[st]) {
                    destroy_pipe_objects(p);
                    return fail(err, NVN_GPU_E_OOM, "图形管线动态偏移表分配失败");
                }
                for (uint32_t i = 0; i < sh->nent; i++) {
                    int16_t v = -1;
                    for (uint32_t j = 0; j < nm; j++) if (m[j].set == sh->ents[i].set && m[j].binding == sh->ents[i].binding) { v = m[j].dbase; break; }
                    p->stage_dyn[st][i] = v;
                }
            }
            if (sh->tfmt) {
                p->stage_tfmt[st] = calloc(sh->nent, sizeof *p->stage_tfmt[st]);
                if (!p->stage_tfmt[st]) {
                    destroy_pipe_objects(p);
                    return fail(err, NVN_GPU_E_OOM, "图形管线 texel 格式表分配失败");
                }
                memcpy(p->stage_tfmt[st], sh->tfmt, (size_t)sh->nent * sizeof *p->stage_tfmt[st]);
            }
        }
    }
    p->actual_samples = d->samples > 1 ? d->samples : 1;
    p->key = *requested;   // 以原请求查找：预热、在途表与缓存使用同一身份。
    note_sync_pipeline();
    *out = p;
    return rc(NVN_GPU_OK);
}

// ---- 装缓存：双检去重（并发建同一配方只留一份）----
// 双检的几步做成回调交给 nvk_pipe_once_adopt（纯逻辑、宿主单测用假创建驱动）：
//   find  → pipe_find_locked（调用方的锁下再查一次）
//   link  → 加着色器引用并挂进 g.pipes / g_pipe_ht
//   destroy → 已有同配方的另一份时销毁这份新建的（不能出现两份同配方管线）
static void *install_find(void *ctx, const void *key, size_t key_bytes) {
    (void)ctx; (void)key_bytes;
    return pipe_find_locked((const nvn_gpu_pipe_desc *)key);
}
static void install_link(void *ctx, void *fresh) {
    (void)ctx;
    struct nvn_gpu_pipe_s *p = fresh;
    for (int st = 0; st < 5; st++)
        if (p->key.st[st].sh) p->key.st[st].sh->refs++;   // 管线持有
    p->all_next = g.pipes;
    g.pipes = p;
    p->hash = pipe_hash(&p->key);
    pipe_ht_add(p);
    if (t_warm_scope) {   // 预热线程建的：打 warm 标记，等真实 draw 第一次命中
        p->warm = 1;
        __atomic_fetch_add(&g_warm_created, 1, __ATOMIC_RELAXED);
    }
}
static void install_destroy(void *ctx, void *fresh) {
    (void)ctx;
    destroy_pipe_objects(fresh);
}

// 装一条新建的图形管线（调用方持 g_mu）。返回交给调用方的对象：
//   · 建的过程中已有同配方的另一份（预热线程 / 另一个提交线程先装进去了）→ 销毁新建的那份、用既有的；
//   · 没有 → 装进缓存。
static struct nvn_gpu_pipe_s *pipe_install_locked(struct nvn_gpu_pipe_s *fresh) {
    nvk_pipe_once_ops ops = { NULL, install_find, install_link, install_destroy };
    void *out = NULL;
    int r = nvk_pipe_once_adopt(&ops, &fresh->key, sizeof fresh->key, fresh, &out);
    if (r < 0) {   // 不该发生（三个回调都在）：如实失败，调用方报红，不返回假成功
        destroy_pipe_objects(fresh);
        return NULL;
    }
    if (r > 0) __atomic_fetch_add(&g_pipe_dup, 1, __ATOMIC_RELAXED);
    return out;
}

// 真实 draw 第一次拿到预建对象：预热验收读数（每份只计一次）。预热线程自己再取同一条（重试路径）
// 不算命中，否则读数会虚高。调用方持 g_mu（warm 位由「缓存命中」与「等到在途创建」两条路一起改）。
static void pipe_hit_note_locked(struct nvn_gpu_pipe_s *hit) {
    if (hit->warm && !t_warm_scope) {
        hit->warm = 0;
        __atomic_fetch_add(&g_warm_hits, 1, __ATOMIC_RELAXED);
    }
}

// 只查缓存、不建（异步首用 / 预热「先看有没有」用）。线程：[提交] / [后台]。1 = 有（*out），0 = 没有。
// 不记 warm 命中（那是「真实 draw 第一次拿到预建对象」的口径，见 pipe_hit_note_locked）。
int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *d, nvn_gpu_pipe *out) {
    if (out) *out = NULL;
    if (!out || !d || !session_accepting()) return 0;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pipe_s *hit = pipe_find_locked(d);
    pthread_mutex_unlock(&g_mu);
    if (!hit) return 0;
    *out = hit;   // 后端缓存持有，核心不 release
    return 1;
}

// 取（或建）图形管线。线程：[提交] 与 [后台] 都可调（预热线程用 SYNC 建）。
// ASYNC / PROVISIONAL 的「先返回 PENDING、后台再建」本后端不内建：本层 v1 两种 mode 都按同步建处理（语义同 SYNC）。
// 后台建 + 可跳首用由其上安卓 demand 层（nvn_vk_pipe_demand.c）承接：它用 nvn_gpu_pipe_lookup 判命中、后台线程调 SYNC 建。
//
// 并发语义（P1 收口）：
//   ① 缓存命中：g_mu 短锁查表，命中即返回——建管线在飞时不占这把锁，不同配方的需求不会被串行化；
//   ② 缓存未命中：查「在途表」（g_pi_mu 短锁）。同配方有在途创建 → 挂上去等它（不重复建、不等全局锁）；
//      不同配方 → 各自认领各自建（pipe_build 在锁外，可真并行）；
//   ③ 认领后复检缓存（只拿 g_mu）：另一个线程可能刚把同配方装好（我们的第一次查询在它安装之前），
//      复检到就省掉这次创建；
//   ④ 建好后在 g_mu 下安装：pipe_install_locked 再双检一次，万一仍有同配方的另一份就销毁新建的那份、
//      用既有的（g_pipe_dup 计数）——正常路径下在途表已保证这里不会重复，这条是表满 / 表停用时的兜底；
//   ⑤ 失败原样返回；挂了等待者的那份由建者把同一个错误广播出去（不重试、不返回假成功）。
// 调用方契约：d->st[].sh 指向的着色器在整个调用期间必须有效（核心 / 预热 owner 的程序借用合同，见 pipe_build）。
// 设备策略：debug.s2a.nvk_pipe_serial=1 时创建重新串行化（见 ④ 前的注释），缺省 0 = 并发建。
static nvn_gpu_rc pipe_get_inner(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!d) return fail(err, NVN_GPU_E_ARG, "管线描述为空");

    // ① 缓存命中（短锁）
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pipe_s *hit = pipe_find_locked(d);
    if (hit) {
        pipe_hit_note_locked(hit);
        pthread_mutex_unlock(&g_mu);
        *out = hit;   // 后端缓存持有，核心不 release
        return rc(NVN_GPU_OK);
    }
    pthread_mutex_unlock(&g_mu);

    // ② 在途表：同配方合并成一份创建；不同配方互不等待（逻辑在 nvn_pipe_flight.c）
    pi_ensure();
    int handle = -1;
    const int role = nvk_pf_enter(&g_pi, d, pi_eq, &handle);   // 1 = 我是建者 / 2 = 等别人 / 0 = 表满或停用
    if (role == 2) {
        void *got = NULL;
        char gwhy[NVK_PF_WHY];
        const int code = nvk_pf_wait(&g_pi, handle, &got, gwhy, sizeof gwhy);
        if (code) {
            if (err) { err->rc = rc(code); memcpy(err->why, gwhy, sizeof err->why); }
            return rc(code);
        }
        // 等到的是预建/已建对象：与缓存命中同一口径记一次 warm 命中（warm 位只在 g_mu 下读写）
        if (got) {
            pthread_mutex_lock(&g_mu);
            pipe_hit_note_locked(got);
            pthread_mutex_unlock(&g_mu);
        }
        *out = got;   // 后端缓存持有，核心不 release
        return rc(NVN_GPU_OK);
    }
    const int own = (role == 1);   // 0 = 表满/停用：这一次不合并，自己建（安装双检兜底；full 由模块计数）

    // ③ 认领后复检缓存（只拿 g_mu 看一眼；锁顺序固定为「先缓存、后表」）
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pipe_s *late = pipe_find_locked(d);
    if (late) {
        pipe_hit_note_locked(late);
        pthread_mutex_unlock(&g_mu);
        if (own) {
            nvk_pf_note_late_hit(&g_pi);
            nvk_pf_publish(&g_pi, handle, late, 0, NULL);
        }
        *out = late;
        return rc(NVN_GPU_OK);
    }
    pthread_mutex_unlock(&g_mu);

    // ④ 建管线（锁外）。设备策略兜底：debug.s2a.nvk_pipe_serial=1 时把创建重新串行化——不同配方在
    // g_create_mu 上排队（回到「全局创建锁」的行为；同配方仍由在途表合并）。用于实测不能同缓存并发建的
    // 驱动；release 包 debug.s2a.* 属性门关着 → 读不到 → 缺省 0 = 并发建。
    struct nvn_gpu_pipe_s *fresh = NULL;
    nvn_gpu_rc br;
    if (pipe_serial_on()) {
        pthread_mutex_lock(&g_create_mu);
        br = pipe_build(d, &fresh, err);
        pthread_mutex_unlock(&g_create_mu);
    } else {
        br = pipe_build(d, &fresh, err);
    }
    if (!nvn_gpu_ok(br)) {
        if (own) nvk_pf_publish(&g_pi, handle, NULL, br.code, err ? err->why : NULL);   // 同配方的等待者拿到同一个错误
        return br;
    }

    // ⑤ 安装：g_mu 下双检（同配方已被别人装好 → 销毁新建的那份、用既有的，g_pipe_dup 计数）
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pipe_s *p = pipe_install_locked(fresh);
    pthread_mutex_unlock(&g_mu);
    if (!p) {
        nvn_gpu_rc oom = fail(err, NVN_GPU_E_OOM, "图形管线装缓存失败");
        if (own) nvk_pf_publish(&g_pi, handle, NULL, oom.code, err ? err->why : NULL);
        return oom;
    }
    if (own) nvk_pf_publish(&g_pi, handle, p, 0, NULL);
    *out = p;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!pipe_call_enter(err)) return rc(NVN_GPU_E_STOPPED);
#if defined(S2A_BACKEND_PIPE_CALL_TEST)
    pipe_call_test_gate();
#endif
    nvn_gpu_rc result = pipe_get_inner(d, mode, out, err);
    pipe_call_leave();
    return result;
}

static nvn_gpu_rc cpipe_build(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    (void)mode;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!cs || !cs->sh) return fail(err, NVN_GPU_E_ARG, "计算阶段为空");
    struct nvn_gpu_shader_s *sh = cs->sh;
    if (sh->stage != NVN_GPU_SH_CS) return fail(err, NVN_GPU_E_ARG, "阶段不是 CS");
    if (sh->missing_storage_format_feature)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "设备未启用无格式存储图像所需特性");
    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    if (!p) return fail(err, NVN_GPU_E_OOM, "计算管线结构分配失败");
    uint32_t maxset = 0;
    for (uint32_t i = 0; i < sh->nent; i++) {
        if (sh->ents[i].cls == NVN_GPU_RES_VA) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED,
                        "VA（设备地址）类别：契约未定非 bufferDeviceAddress 的消费方式，不报绿（请契约属主定夺）");
        }
        if (vk_desc_type(sh->ents[i].cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "资源类别 %u：本段未实现", sh->ents[i].cls);
        }
        if (sh->ents[i].set > maxset) maxset = sh->ents[i].set;
    }
    if (sh->nent && maxset >= 4) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_LAYOUT, "契约：set %u 超过四 set 上限", maxset); }
    p->nset = sh->nent ? maxset + 1 : 0;
    for (uint32_t set = 0; set < p->nset; set++) {
        VkDescriptorSetLayoutBinding b[32];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < sh->nent; i++) {
            if (sh->ents[i].set != set) continue;
            if (nb == 32) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "契约：单 set 描述符项超过 32"); }
            memset(&b[nb], 0, sizeof b[nb]);
            b[nb].binding = sh->ents[i].binding;
            b[nb].descriptorType = vk_desc_type(sh->ents[i].cls);
            b[nb].descriptorCount = sh->ents[i].count ? sh->ents[i].count : 1;
            b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            nb++;
        }
        int oom = 0;
        p->dlay[set] = dlay_get(b, nb, &oom);   // 同图形管线：结构相同的布局共用一个，只借用
        if (!p->dlay[set]) {
            destroy_pipe_objects(p);
            if (oom) return fail(err, NVN_GPU_E_OOM, "布局表项分配失败（set %u）", set);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateDescriptorSetLayout 失败（set %u）", set);
        }
        p->dsl[set] = p->dlay[set]->dsl;
    }
    { const char *limit = pool_pipeline_preflight(p, NULL, sh); if (limit) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "%s", limit); } }
    VkPushConstantRange cpr;
    uint32_t ncpr = 0;
    if (sh->push_bytes) {
        if ((sh->push_off | sh->push_bytes) & 3) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_LAYOUT, "契约：CS push 范围 [%u,+%u) 未 4 字节对齐", sh->push_off, sh->push_bytes);
        }
        if (sh->push_off > g.caps.max_push_bytes || sh->push_bytes > g.caps.max_push_bytes - sh->push_off) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "本机：CS push 范围 [%u,+%u) 超出设备上限 %u",
                        sh->push_off, sh->push_bytes, g.caps.max_push_bytes);
        }
        cpr = (VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, sh->push_off, sh->push_bytes};
        ncpr = 1;
    }
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = p->nset;
    pli.pSetLayouts = p->nset ? p->dsl : NULL;
    pli.pushConstantRangeCount = ncpr;
    pli.pPushConstantRanges = ncpr ? &cpr : NULL;
    if (vkCreatePipelineLayout(g.d, &pli, NULL, &p->layout) != VK_SUCCESS) {
        destroy_pipe_objects(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreatePipelineLayout 失败");
    }
    VkComputePipelineCreateInfo cp;
    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = sh->mod;
    cp.stage.pName = sh->entry;
    cp.layout = p->layout;
    VkPipelineCreationFeedback feedback = {0};
    VkPipelineCreationFeedbackCreateInfo feedback_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pNext = cp.pNext,
        .pPipelineCreationFeedback = &feedback,
    };
    const int want_feedback = atomic_load_explicit(&g_pipe_feedback, memory_order_relaxed) == NVK_PIPE_FEEDBACK_ON;
    if (want_feedback) cp.pNext = &feedback_info;
    const uint64_t pc_t0 = now_ns();   // 同图形管线：超过 3 毫秒打一行，用来对长帧
    VkResult cpvr = vkCreateComputePipelines(g.d, g_plc, 1, &cp, NULL, &p->pipe);
    const uint64_t wall_ns = now_ns() - pc_t0;
    pipe_creation_note(1, wall_ns, cpvr == VK_SUCCESS && want_feedback ? &feedback : NULL);
    if (cpvr != VK_SUCCESS) {
        destroy_pipe_objects(p);
        return fail(err, NVN_GPU_E_COMPILE, "vkCreateComputePipelines 失败（VkResult %d，%u 项资源）", (int)cpvr, sh->nent);
    }
    plc_created();
    {
      static int slowc = -1; if (slowc < 0) slowc = nvn_plat_flag("nvk_hitch_log", 0) != 0;
      const uint64_t dt = slowc ? now_ns() - pc_t0 : 0;
      if (dt > 3000000ull) { char b[200]; nvn_vk_compute_pipe_line(b, sizeof b, (double)dt / 1e6, sh->src_key, sh->precompiled, sh->spirv_digest); nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe", b); } }
    p->cshader = sh;   // 两条安装路径都保留实际计算阶段的身份
    if (sh->precompiled) p->lib_stages = 1u << NVN_GPU_SH_CS;
    p->stage_nent[NVN_GPU_SH_CS] = sh->nent;
    p->stage_push_off[NVN_GPU_SH_CS] = sh->push_off;
    p->stage_push_bytes[NVN_GPU_SH_CS] = sh->push_bytes;
    if (sh->nent) {
        p->stage_ents[NVN_GPU_SH_CS] = calloc(sh->nent, sizeof *p->stage_ents[NVN_GPU_SH_CS]);
        if (!p->stage_ents[NVN_GPU_SH_CS]) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_OOM, "计算管线阶段布局分配失败");
        }
        memcpy(p->stage_ents[NVN_GPU_SH_CS], sh->ents, (size_t)sh->nent * sizeof *sh->ents);
        if (sh->tfmt) {
            p->stage_tfmt[NVN_GPU_SH_CS] = calloc(sh->nent, sizeof *p->stage_tfmt[NVN_GPU_SH_CS]);
            if (!p->stage_tfmt[NVN_GPU_SH_CS]) {
                destroy_pipe_objects(p);
                return fail(err, NVN_GPU_E_OOM, "计算管线 texel 格式表分配失败");
            }
            memcpy(p->stage_tfmt[NVN_GPU_SH_CS], sh->tfmt, (size_t)sh->nent * sizeof *sh->tfmt);
        }
    }
    note_sync_pipeline();
    *out = p;
    return rc(NVN_GPU_OK);
}

// 计算管线只以 CS 身份为键（本路径尚不支持特化）。同键在途合并；完成后双检并安装。
static nvk_pf g_cpi;
static pthread_once_t g_cpi_once = PTHREAD_ONCE_INIT;
static void cpi_init(void) { nvk_pf_init(&g_cpi, sizeof(nvn_gpu_shader)); }
uint64_t nvn_vk_dbg_cpipe_waits(void) { uint64_t n = 0; pthread_once(&g_cpi_once, cpi_init); nvk_pf_stats(&g_cpi, &n, NULL, NULL, NULL, NULL); return n; }
static int cpi_eq(const void *a, const void *b) { nvn_gpu_shader x, y; memcpy(&x, a, sizeof x); memcpy(&y, b, sizeof y); return x == y; }
static uint32_t cpipe_bucket(nvn_gpu_shader sh) { return (uint32_t)(((uintptr_t)sh >> 4) & (CPIPE_BUCKETS - 1)); }
static struct nvn_gpu_pipe_s *cpipe_find_locked(nvn_gpu_shader sh) {
    for (struct nvn_gpu_pipe_s *p = g_cpipe_ht[cpipe_bucket(sh)]; p; p = p->cnext)
        if (p->cshader == sh) return p;
    return NULL;
}
static nvn_gpu_rc cpipe_get_inner(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!cs || !cs->sh) return fail(err, NVN_GPU_E_ARG, "计算阶段为空");
    if (cs->nspec) return fail(err, NVN_GPU_E_UNSUPPORTED, "计算特化常量尚未实现");
    if (!nvn_plat_flag("nvk_compute_warm", 0)) {
        nvn_gpu_pipe p = NULL; nvn_gpu_rc r = cpipe_build(cs, mode, &p, err);
        if (!nvn_gpu_ok(r)) return r;
        pthread_mutex_lock(&g_mu); cs->sh->refs++; p->all_next = g.pipes; g.pipes = p; pthread_mutex_unlock(&g_mu);
        *out = p; return r;
    }
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_pipe_s *hit = cpipe_find_locked(cs->sh);
    if (hit) pipe_hit_note_locked(hit);
    pthread_mutex_unlock(&g_mu);
    if (hit) { *out = hit; return rc(NVN_GPU_OK); }
    pthread_once(&g_cpi_once, cpi_init);
    int handle = -1, role = nvk_pf_enter(&g_cpi, &cs->sh, cpi_eq, &handle);
    if (role == 2) {
        void *got = NULL; char why[NVK_PF_WHY];
        int code = nvk_pf_wait(&g_cpi, handle, &got, why, sizeof why);
        if (code) { if (err) { err->rc = rc(code); memcpy(err->why, why, sizeof err->why); } return rc(code); }
        if (got) { pthread_mutex_lock(&g_mu); pipe_hit_note_locked(got); pthread_mutex_unlock(&g_mu); }
        *out = got; return rc(NVN_GPU_OK);
    }
    int own = role == 1;
    pthread_mutex_lock(&g_mu); hit = cpipe_find_locked(cs->sh);
    if (hit) pipe_hit_note_locked(hit);
    pthread_mutex_unlock(&g_mu);
    if (hit) {
        if (own) { nvk_pf_note_late_hit(&g_cpi); nvk_pf_publish(&g_cpi, handle, hit, 0, NULL); }
        *out = hit; return rc(NVN_GPU_OK);
    }
    nvn_gpu_pipe fresh = NULL;
    nvn_gpu_rc built = cpipe_build(cs, mode, &fresh, err);
    if (!nvn_gpu_ok(built)) {
        if (own) nvk_pf_publish(&g_cpi, handle, NULL, built.code, err ? err->why : NULL);
        return built;
    }
    pthread_mutex_lock(&g_mu);
    hit = cpipe_find_locked(cs->sh);
    if (!hit) {
        hit = fresh; cs->sh->refs++;
        hit->all_next = g.pipes; g.pipes = hit;
        uint32_t b = cpipe_bucket(cs->sh); hit->cnext = g_cpipe_ht[b]; g_cpipe_ht[b] = hit;
        if (t_warm_scope) { hit->warm = 1; __atomic_fetch_add(&g_warm_created, 1, __ATOMIC_RELAXED); }
    } else {
        pipe_hit_note_locked(hit);
        destroy_pipe_objects(fresh);
        __atomic_fetch_add(&g_pipe_dup, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_mu);
    if (own) nvk_pf_publish(&g_cpi, handle, hit, 0, NULL);
    *out = hit; return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!pipe_call_enter(err)) return rc(NVN_GPU_E_STOPPED);
#if defined(S2A_BACKEND_PIPE_CALL_TEST)
    pipe_call_test_gate();
#endif
    nvn_gpu_rc result = cpipe_get_inner(cs, mode, out, err);
    pipe_call_leave();
    return result;
}

// EXTENDED_USAGE 建的图：视图默认继承整张图的用途，sRGB 的采样 / 附件视图带着存储用途会被拒（格式不支持存储），
//   按视图自己的格式剥掉存储用途。不是这种图返回 NULL（pNext 不挂）。
static const void *view_usage_next(const struct nvn_gpu_tex_s *t, VkFormat vf, VkImageViewUsageCreateInfo *u) {
    if (!t->ext_usage) return NULL;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, vf, &fp);
    memset(u, 0, sizeof *u);
    u->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    u->usage = t->usage;
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u->usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT;
    return u;
}

// 附件形态的视图：一级一层的 2D 视图，深度模板格式两个面都在（对照 nvn_mtl.m attach @4928：附件 = 基底 + 视图设了才取的
// level / slice，没设就是首级首层）。采样用的视图可以是多级 / 多层 / 立方体 / 只取深度面，都不能直接挂到帧缓冲上：
// 第一次当附件时另建一个，随视图销毁。建不出来回空。
// nl = 所在通道的层数（nvn_gpu_pass_desc.layers）。分层通道（nl > 1，程序的顶点 / 几何阶段写层号）要起始层起 nl 层的 2D 阵列视图，
// 图元按层号落到对应层（3D 贴图：深度切片，要 2D_ARRAY_COMPATIBLE）；单层形态只有第 0 层，写到更高层的图元全丢（八方旅人调色 LUT 32 片只剩第 0 片）。
// 视图自己的层数不够 nl 时回空（回放核心按附件可给的层数定 nl，正常走不到）。
static VkImageView view_attachment(struct nvn_gpu_view_s *v, uint32_t nl) {
    struct nvn_gpu_tex_s *t = v->tex;
    if (v->p16_remap) return VK_NULL_HANDLE;   // #975：恒等形态会按退回格式的分量顺序写，调用方具名报红
    int image3d = t->target == NVN_TEXTURE_TARGET_3D && t->depth > 1;
    if (nl > 1) {
        if (v->layers < nl || (image3d && !t->array_compatible)) return VK_NULL_HANDLE;
        gmu_lock_rt();
        if (v->att_n && v->att_nl != nl) { pc_drop_view(v->att_n); vkDestroyImageView(g.d, v->att_n, NULL); v->att_n = VK_NULL_HANDLE; }
        if (!v->att_n) {
            VkImageViewCreateInfo ci;
            memset(&ci, 0, sizeof ci);
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ci.image = t->image;
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            ci.format = v->format ? v->format : t->format;
            ci.subresourceRange = (VkImageSubresourceRange){t->aspect, v->base_level, 1, image3d ? v->att_layer : v->base_layer, nl};
            VkImageViewUsageCreateInfo vu; ci.pNext = view_usage_next(t, ci.format, &vu);
            OC(OC_VIEW);
            { LIFE_T0; if (vkCreateImageView(g.d, &ci, NULL, &v->att_n) != VK_SUCCESS) v->att_n = VK_NULL_HANDLE; LIFE_ADD(4); }
            v->att_nl = v->att_n ? nl : 0;
        }
        VkImageView r = v->att_n;
        pthread_mutex_unlock(&g_mu);
        return r;
    }
    if (!v->swizzled && v->levels == 1 && v->layers == 1 && v->type == VK_IMAGE_VIEW_TYPE_2D && v->aspect == t->aspect) return v->view;
    gmu_lock_rt();
    if (!v->att && !(image3d && !t->array_compatible)) {
        VkImageViewCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = t->image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = v->format ? v->format : t->format;
        ci.subresourceRange = (VkImageSubresourceRange){t->aspect, v->base_level, 1, image3d ? v->att_layer : v->base_layer, 1};
        VkImageViewUsageCreateInfo vu; ci.pNext = view_usage_next(t, ci.format, &vu);
        OC(OC_VIEW);
        { LIFE_T0; if (vkCreateImageView(g.d, &ci, NULL, &v->att) != VK_SUCCESS) v->att = VK_NULL_HANDLE; LIFE_ADD(4); }
    }
    VkImageView r = v->att;
    pthread_mutex_unlock(&g_mu);
    return r;
}

// 图像形态的视图：存储图像视图必须用恒等通道映射（Vulkan 规定 image 视图不参与通道映射），而采样视图可能带着
// 通道映射（游戏设的，或没有 alpha 的格式把 a 钉成 ONE）。第一次当存储图像时另建一个，随视图销毁。
static VkImageView view_image_form(struct nvn_gpu_view_s *v) {
    if (v->p16_remap) return VK_NULL_HANDLE;   // #975：同 view_attachment
    if (!v->swizzled) return v->view;
    pthread_mutex_lock(&g_mu);
    if (!v->img) {
        VkImageViewCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = v->tex->image;
        ci.viewType = v->type;
        ci.format = v->format;
        ci.subresourceRange = (VkImageSubresourceRange){v->aspect, v->base_level, v->levels, v->base_layer, v->layers};
        VkImageViewUsageCreateInfo vu; ci.pNext = view_usage_next(v->tex, ci.format, &vu);
        OC(OC_VIEW);
        { LIFE_T0; if (vkCreateImageView(g.d, &ci, NULL, &v->img) != VK_SUCCESS) v->img = VK_NULL_HANDLE; LIFE_ADD(4); }
    }
    VkImageView r = v->img;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static pthread_mutex_t g_pc_mu = PTHREAD_MUTEX_INITIALIZER;
// 开关每 128 个通道重读一次（真机上同一局里来回切着比）；一个通道从头到尾用开头读到的那个值（g_pc_use，只在回放线程上动）
static int g_pc_use = 1;
static int pc_on(void) { static int on = -1; static uint32_t n; if (on < 0 || !(++n & 127)) on = nvn_plat_flag("nvk_pass_cache", 1) != 0; return on; }
struct pc_rp {
    uint32_t natt, ncol;
    int has_depth;
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8], dref;
    VkRenderPass rp;
    VkExtent2D granularity;
};
static struct pc_rp *g_pc_rp; static uint32_t g_pc_nrp, g_pc_rpcap;
// 帧缓冲缓存：真机上「开通道内部」的帧缓冲一段读到 7.7–12.8 微秒/通道、未命中 25–34 个/帧
// （未命中就只能 vkCreateFramebuffer，7–12 微秒一个）。表从 2048 加到 8192、探测 24 加到 32，专门压未命中率；
// 反查表（pc_fb_note_desc / pc_drop_view_fast）在容量变大后仍然是按视图定点拆，不做全表扫描。
enum { PC_FB_N = 8192, PC_FB_PROBE = 32 };
struct pc_fb { VkRenderPass rp; uint32_t natt, w, h, layers; VkImageView v[9]; VkFramebuffer fb; uint32_t gen; int tomb; };
static struct pc_fb g_pc_fb[PC_FB_N];
static VkRenderPass pc_pass(const VkRenderPassCreateInfo *ci, const VkAttachmentDescription *att, uint32_t natt,
                            const VkAttachmentReference *col, uint32_t ncol, const VkAttachmentReference *dref,
                            VkExtent2D *granularity) {
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!g_pc_use || natt > 9 || ncol > 8) {
        OC(OC_RP);
        if (vkCreateRenderPass(g.d, ci, NULL, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
        if (granularity) vkGetRenderAreaGranularity(g.d, pass, granularity);
        return pass;
    }
    struct pc_rp k; memset(&k, 0, sizeof k); k.natt = natt; k.ncol = ncol; k.has_depth = dref != NULL;
    memcpy(k.att, att, natt * sizeof *att); memcpy(k.col, col, ncol * sizeof *col); if (dref) k.dref = *dref;
    pthread_mutex_lock(&g_pc_mu);
    for (uint32_t i = 0; i < g_pc_nrp; i++) {
        struct pc_rp *entry = &g_pc_rp[i];
        if (memcmp(entry, &k, offsetof(struct pc_rp, rp))) continue;
        pass = entry->rp;
        if (granularity) {
            if (!entry->granularity.width || !entry->granularity.height)
                vkGetRenderAreaGranularity(g.d, pass, &entry->granularity);
            *granularity = entry->granularity;
        }
        pthread_mutex_unlock(&g_pc_mu);
        return pass;
    }
    OC(OC_RP);
    if (vkCreateRenderPass(g.d, ci, NULL, &pass) != VK_SUCCESS) { pthread_mutex_unlock(&g_pc_mu); return VK_NULL_HANDLE; }
    if (granularity) { vkGetRenderAreaGranularity(g.d, pass, &k.granularity); *granularity = k.granularity; }
    if (g_pc_nrp == g_pc_rpcap) { uint32_t cap = g_pc_rpcap ? g_pc_rpcap * 2 : 64; struct pc_rp *n = realloc(g_pc_rp, cap * sizeof *n); if (n) { g_pc_rp = n; g_pc_rpcap = cap; } }
    if (g_pc_nrp < g_pc_rpcap) { k.rp = pass; g_pc_rp[g_pc_nrp++] = k; }   // 记不下（内存不够）：这个对象留到会话结束也没人销毁；只在分配失败时发生
    pthread_mutex_unlock(&g_pc_mu);
    return pass;
}
static uint32_t g_fb_gen;   // 帧缓冲缓存槽的代：槽被复用时，视图反查表里记的旧引用会因代不同而被忽略
static uint64_t g_fb_hit, g_fb_miss, g_fb_create_ns;   // 读数（nvk_pass_prof）：帧缓冲命中 / 未命中 / 新建耗时
static VkFramebuffer pc_fb(const VkFramebufferCreateInfo *fi, int *cached, int *slot_out, uint32_t *gen_out) {
    VkFramebuffer fb = VK_NULL_HANDLE; *cached = 0; if (slot_out) *slot_out = -1; if (gen_out) *gen_out = 0;
    if (!g_pc_use || fi->attachmentCount > 9) { OC(OC_FB); return vkCreateFramebuffer(g.d, fi, NULL, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE; }
    if (!g_pc_use || fi->attachmentCount > 9) { OC(OC_FB); return vkCreateFramebuffer(g.d, fi, NULL, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE; }
    uint64_t h = (uint64_t)(uintptr_t)fi->renderPass * 0x9E3779B97F4A7C15ull ^ ((uint64_t)fi->width << 40 | (uint64_t)fi->height << 16 | fi->layers);
    for (uint32_t i = 0; i < fi->attachmentCount; i++) h = (h ^ (uint64_t)(uintptr_t)fi->pAttachments[i]) * 0x100000001B3ull;
    pthread_mutex_lock(&g_pc_mu);
    int slot = -1;
    for (uint32_t p = 0; p < PC_FB_PROBE; p++) {
        struct pc_fb *e = &g_pc_fb[(h + p) % PC_FB_N];
        if (!e->fb) { if (slot < 0) slot = (int)((h + p) % PC_FB_N); if (!e->tomb) break; continue; }
        if (e->rp == fi->renderPass && e->natt == fi->attachmentCount && e->w == fi->width && e->h == fi->height && e->layers == fi->layers &&
            !memcmp(e->v, fi->pAttachments, fi->attachmentCount * sizeof(VkImageView))) { fb = e->fb; *cached = 1; __atomic_fetch_add(&g_fb_hit, 1, __ATOMIC_RELAXED); pthread_mutex_unlock(&g_pc_mu); return fb; }
    }
    OC(OC_FB);
    __atomic_fetch_add(&g_fb_miss, 1, __ATOMIC_RELAXED);
    { struct timespec f0; clock_gettime(CLOCK_MONOTONIC, &f0); LIFE_T0; VkResult fr_ = vkCreateFramebuffer(g.d, fi, NULL, &fb); LIFE_ADD(3); if (fr_ != VK_SUCCESS) { pthread_mutex_unlock(&g_pc_mu); return VK_NULL_HANDLE; }
      struct timespec f1; clock_gettime(CLOCK_MONOTONIC, &f1);
      __atomic_fetch_add(&g_fb_create_ns, (uint64_t)(f1.tv_sec - f0.tv_sec) * 1000000000ull + (uint64_t)f1.tv_nsec - (uint64_t)f0.tv_nsec, __ATOMIC_RELAXED); }
    if (slot >= 0) { struct pc_fb *e = &g_pc_fb[slot]; memset(e, 0, sizeof *e); e->rp = fi->renderPass; e->natt = fi->attachmentCount; e->w = fi->width; e->h = fi->height; e->layers = fi->layers;
        memcpy(e->v, fi->pAttachments, fi->attachmentCount * sizeof(VkImageView)); e->fb = fb; e->gen = ++g_fb_gen; *cached = 1;
        if (slot_out) { *slot_out = slot; }
        if (gen_out) { *gen_out = e->gen; } }
    pthread_mutex_unlock(&g_pc_mu);
    return fb;
}
static uint64_t g_pc_scan_n;   // 全表扫的次数（会话累计；用例读：缺省档下视图销毁不该再扫）
static uint32_t pc_drop_view_n(VkImageView v) {   // 全表扫：拆掉含这个句柄的帧缓冲，返回拆了几个
    uint32_t n = 0;
    if (!v) return 0;
    __atomic_fetch_add(&g_pc_scan_n, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_pc_mu);
    for (uint32_t i = 0; i < PC_FB_N; i++) { struct pc_fb *e = &g_pc_fb[i]; if (!e->fb) continue;
        for (uint32_t k = 0; k < e->natt; k++) if (e->v[k] == v) { vkDestroyFramebuffer(g.d, e->fb, NULL); memset(e, 0, sizeof *e); e->tomb = 1; n++; break; } }
    pthread_mutex_unlock(&g_pc_mu);
    return n;
}
static void pc_drop_view(VkImageView v) { (void)pc_drop_view_n(v); }
// 视图销毁：先按反查表只拆它当过附件的那些帧缓冲（真机上一帧 30 个视图销毁，全表扫会和开通道抢同一把锁）。
// 反查表溢出过才退回全表扫。原先「反查表里一项也没有」（视图从没当过已缓存帧缓冲的附件，或前一次调用已经拆过、nfbs 被清零）也退回全表扫：
// 8192 项约 0.9 MB、每次约 25 微秒，世界场景每帧拆 56 个视图 × 至少一次，读数「从帧缓冲表里摘」1.48 毫秒/帧（nvk_fbdrop=0 时；缺省 0.39），还全在持 g_mu 的销毁路径上，
// 与本段「溢出才退回」的本意不符。反查表在每次缓存新帧缓冲时把全部附件视图都记过（pc_fb_note_desc），表是空的就不可能有它的句柄。
// 诊断开关 nvk_fbdrop：0 = 旧行为（表是空的也全表扫）；1 = 缺省；2 = 自检（每次之后再全表扫一遍，扫到残留报红并顺手拆掉）。
static int fbdrop_mode(void) { static uint32_t k; if (g_fbdrop_cur < 0 || !(++k & 255)) g_fbdrop_cur = nvn_plat_flag("nvk_fbdrop", 1); return g_fbdrop_cur; }
static uint64_t g_fbdrop_resid;   // 自检档扫到的残留帧缓冲数（用例读；缺省档恒为 0）
static void pc_drop_resid_check(VkImageView v) {
    uint32_t n = pc_drop_view_n(v);
    if (!n) return;
    __atomic_fetch_add(&g_fbdrop_resid, n, __ATOMIC_RELAXED);
    nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", "帧缓冲缓存里有反查表没记到的项（视图销毁时全表扫到残留，已拆）");
}
static void pc_drop_view_fast(struct nvn_gpu_view_s *view, VkImageView v) {
    if (!v) return;
    const int mode = fbdrop_mode();
    if (!view || view->fbs_overflow) { pc_drop_view(v); return; }
    if (!view->nfbs) {
        if (mode <= 0) pc_drop_view(v);
        else if (mode >= 2) pc_drop_resid_check(v);
        return;
    }
    pthread_mutex_lock(&g_pc_mu);
    for (uint8_t i = 0; i < view->nfbs; i++) {
        uint16_t idx = view->fbs[i].idx; if (idx >= PC_FB_N) continue;
        struct pc_fb *e = &g_pc_fb[idx];
        if (!e->fb || e->gen != view->fbs[i].gen || e->fb != view->fbs[i].fb) continue;
        vkDestroyFramebuffer(g.d, e->fb, NULL); memset(e, 0, sizeof *e); e->tomb = 1;
    }
    pthread_mutex_unlock(&g_pc_mu);
    view->nfbs = 0; view->fbs_overflow = 0;
    if (mode >= 2) pc_drop_resid_check(v);
}
// 记录「这个视图当过这个帧缓冲的附件」（反查表满 6 项就不再记，销毁时退回全表扫）
static void pc_fb_note(struct nvn_gpu_view_s *view, int slot, uint32_t gen, VkFramebuffer fb) {
    if (!view || slot < 0 || !gen || !fb) return;
    if (view->nfbs >= 6) { view->fbs_overflow = 1; return; }
    view->fbs[view->nfbs].idx = (uint16_t)slot; view->fbs[view->nfbs].gen = gen; view->fbs[view->nfbs].fb = fb; view->nfbs++;
}
static void pc_fb_note_desc(const nvn_gpu_pass_desc *d, int slot, uint32_t gen, VkFramebuffer fb) {
    if (!d) return;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) if (d->color[i].view) pc_fb_note(d->color[i].view, slot, gen, fb);
    if (d->depth.view) pc_fb_note(d->depth.view, slot, gen, fb);
    if (d->stencil.view && d->stencil.view != d->depth.view) pc_fb_note(d->stencil.view, slot, gen, fb);
}
static void pc_destroy_all(void) {
    pthread_mutex_lock(&g_pc_mu);
    for (uint32_t i = 0; i < PC_FB_N; i++) if (g_pc_fb[i].fb) vkDestroyFramebuffer(g.d, g_pc_fb[i].fb, NULL);
    memset(g_pc_fb, 0, sizeof g_pc_fb);
    for (uint32_t i = 0; i < g_pc_nrp; i++) vkDestroyRenderPass(g.d, g_pc_rp[i].rp, NULL);
    free(g_pc_rp); g_pc_rp = NULL; g_pc_nrp = g_pc_rpcap = 0;
    pthread_mutex_unlock(&g_pc_mu);
}
// #975：走退回格式的 16 位打包格式当颜色附件，硬件按退回格式的分量顺序写，与 NVN 位序不符：开通道前具名报红。
static int pass_p16_reject(struct nvn_gpu_cmd_s *c, const nvn_gpu_pass_desc *d) {
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        struct nvn_gpu_view_s *v = d->color[i].view;
        if (v && v->p16_remap) {
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "颜色附件 %u：16 位打包格式 0x%x 在本设备走退回格式，不能当渲染目标", i, v->nvn_format);
            return 1;
        }
    }
    return 0;
}
// 渲染通道的附件描述：布局按「这张图这一段用什么布局」定（见「图像布局」）。
// att_tex / att_lay 回填各附件的贴图与这一段用的布局；att_tex 可能重复（同一张图的不同级 / 层挂在不同槽）。
static VkRenderPass make_pass(const nvn_gpu_pass_desc *d, VkImageView fb_views[9], VkAttachmentDescription att[9],
                              VkAttachmentReference col[8], VkAttachmentReference *dref, uint32_t *natt_out, int *has_depth,
                              struct nvn_gpu_tex_s *att_tex[9], VkImageLayout att_lay[9], int opt, int feedback, uint16_t read_mask,
                              VkExtent2D *granularity) {
    int is_depth[9], is_read[9];
    uint32_t natt = 0, a = 0, ncol = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {   // 先归拢：真正挂上的颜色附件（按槽序），空槽不算附件
        struct nvn_gpu_view_s *v = d->color[i].view;
        if (!v) continue;
        att_tex[natt] = v->tex; is_depth[natt] = 0; is_read[natt] = (read_mask & (1u << i)) != 0; natt++;
    }
    if (d->depth.view || d->stencil.view) {   // 深度模板只算一个附件（描述里没给视图的那一面不关心）
        att_tex[natt] = (d->depth.view ? d->depth.view : d->stencil.view)->tex;
        is_depth[natt] = 1; is_read[natt] = (read_mask & (1u << 8)) != 0; natt++;
    }
    // 只读采样的 GENERAL 仅属于这个物理通道，不永久标记纹理 fb_sample。
    // 后续普通通道仍按 opt 选附件布局；同一图多级/层的布局一致性由下面的归并保证。
    for (uint32_t i = 0; i < natt; i++) att_lay[i] = is_read[i] ? VK_IMAGE_LAYOUT_GENERAL : pass_att_layout(d, att_tex[i], is_depth[i], opt);
    for (uint32_t i = 0; i < natt; i++)   // 同一张图的几个级 / 层挂在不同槽：布局必须一致，有一个要走 GENERAL 就都走 GENERAL
        for (uint32_t j = i + 1; j < natt; j++)
            if (att_tex[i] == att_tex[j] && att_lay[i] != att_lay[j]) att_lay[i] = att_lay[j] = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference stencil_ref;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        struct nvn_gpu_view_s *v = d->color[i].view;
        if (!v) { col[ncol].attachment = VK_ATTACHMENT_UNUSED; col[ncol].layout = VK_IMAGE_LAYOUT_UNDEFINED; ncol++; continue; }   // 颜色槽里的空位
        struct nvn_gpu_tex_s *t = v->tex;
        memset(&att[a], 0, sizeof att[a]);
        att[a].format = v->format ? v->format : t->format;   // 按视图的格式声明（对照 nvn_mtl.m attach @4883：视图换了格式就按那个格式的视图挂上去，管线格式跟着附件）
        att[a].samples = (VkSampleCountFlagBits)t->samples;
        switch (d->color[i].load) {
        case NVN_GPU_LOAD_CLEAR: att[a].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
        case NVN_GPU_LOAD_DONTCARE: att[a].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
        default: att[a].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; break;
        }
        att[a].initialLayout = att_lay[a];   // 附件在通道里用的布局：缺省 GENERAL；开 nvk_opt_layout 用附件专用布局
        // 逻辑通道可能在后续 draw 的反馈采样处拆开；内部边界必须保存附件。
        // DONTCARE 允许丢弃，并不要求丢弃，不能在尚未结束的逻辑通道提前丢内容。
        att[a].storeOp = feedback || d->color[i].store != NVN_GPU_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (read_mask & (1u << i)) {
            att[a].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[a].storeOp = VK_ATTACHMENT_STORE_OP_NONE_EXT;
        }
        att[a].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[a].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[a].finalLayout = att_lay[a];
        col[ncol].attachment = a;
        col[ncol].layout = att_lay[a];
        fb_views[a] = view_attachment(v, d->layers);
        if (!fb_views[a]) return VK_NULL_HANDLE;
        ncol++; a++;
    }
    *has_depth = 0;
    if (d->depth.view || d->stencil.view) {
        // 深度与模板是同一个附件的两个面，各按自己的 load / store 走；描述里没给视图的那一面不关心。
        // 只要有一面要保留原内容（LOAD），附件就必须从它现在的布局进来：从 UNDEFINED 进来等于两面都丢。
        struct nvn_gpu_view_s *v = d->depth.view ? d->depth.view : d->stencil.view;
        struct nvn_gpu_tex_s *t = v->tex;
        static const VkAttachmentLoadOp k_load[3] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE};
        memset(&att[a], 0, sizeof att[a]);
        att[a].format = t->format;
        att[a].samples = (VkSampleCountFlagBits)t->samples;
        att[a].loadOp = d->depth.view ? k_load[d->depth.load < 3 ? d->depth.load : 2] : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[a].stencilLoadOp = d->stencil.view ? k_load[d->stencil.load < 3 ? d->stencil.load : 2] : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[a].initialLayout = att_lay[a];
        att[a].storeOp = (feedback && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) || (d->depth.view && d->depth.store != NVN_GPU_STORE_DONTCARE) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[a].stencilStoreOp = (feedback && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) || (d->stencil.view && d->stencil.store != NVN_GPU_STORE_DONTCARE) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (read_mask & (1u << 8)) {
            // 同一物理深度模板附件的两面都只读；未声明的面也不能 DONT_CARE 写。
            att[a].loadOp = att[a].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[a].storeOp = att[a].stencilStoreOp = VK_ATTACHMENT_STORE_OP_NONE_EXT;
        }
        att[a].finalLayout = att_lay[a];
        dref->attachment = a;
        dref->layout = att_lay[a];
        fb_views[a] = view_attachment(v, d->layers);
        if (!fb_views[a]) return VK_NULL_HANDLE;
        *has_depth = 1;
        a++;
    }
    (void)stencil_ref;
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = ncol;
    sp.pColorAttachments = ncol ? col : NULL;
    sp.pDepthStencilAttachment = *has_depth ? dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rp;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = a;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    *natt_out = a;
    if (granularity) {
        if (!a || d->layers > 1 || d->occlusion || d->fb_fetch || read_mask || !feedback) granularity = NULL;
        for (uint32_t i = 0; granularity && i < a; i++) {
            const int depth = is_depth[i];
            if (att[i].samples != VK_SAMPLE_COUNT_1_BIT || att_tex[i]->target != NVN_TEXTURE_TARGET_2D ||
                ((!depth || d->depth.view) && (att[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || att[i].storeOp != VK_ATTACHMENT_STORE_OP_STORE)) ||
                (depth && d->stencil.view && (att[i].stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD || att[i].stencilStoreOp != VK_ATTACHMENT_STORE_OP_STORE)))
                granularity = NULL;
            for (uint32_t j = 0; granularity && j < i; j++)
                if (att_tex[i]->image == att_tex[j]->image) granularity = NULL;
        }
    }
    return pc_pass(&rp, att, a, col, ncol, *has_depth ? dref : NULL, granularity);
}

static VkRect2D pass_area_aligned(const nvn_gpu_pass_desc *d, VkExtent2D granularity) {
    const VkRect2D full = {{0, 0}, {d->w, d->h}};
    if (!d->area_w || !d->area_h || !granularity.width || !granularity.height) return full;
    int64_t x1 = (int64_t)d->area_x + d->area_w, y1 = (int64_t)d->area_y + d->area_h;
    uint32_t x0 = d->area_x < 0 ? 0 : (uint32_t)d->area_x, y0 = d->area_y < 0 ? 0 : (uint32_t)d->area_y;
    if (x1 > d->w) x1 = d->w;
    if (y1 > d->h) y1 = d->h;
    if (x1 <= x0 || y1 <= y0) return full;
    x0 -= x0 % granularity.width; y0 -= y0 % granularity.height;
    x1 = (x1 + granularity.width - 1) / granularity.width * granularity.width;
    y1 = (y1 + granularity.height - 1) / granularity.height * granularity.height;
    if (x1 > d->w) x1 = d->w; // 远端贴着帧缓冲边缘也符合粒度要求。
    if (y1 > d->h) y1 = d->h;
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// 开通道用的 VkRenderPassBeginInfo：清除值按附件下标排（空槽不占附件）、渲染区域按 desc 收缩并裁到帧缓冲里。
static void pass_begin_info(const nvn_gpu_pass_desc *d, VkRenderPass pass, VkFramebuffer fb, uint32_t natt, int has_depth,
                            VkRenderPassBeginInfo *bi, VkClearValue clear[9], VkExtent2D granularity) {
    memset(clear, 0, 9 * sizeof clear[0]);
    for (uint32_t i = 0, k = 0; i < d->ncolor && i < 8; i++) {   // 清除值按附件下标排：空槽不占附件
        if (!d->color[i].view) continue;
        memcpy(&clear[k++].color, &d->color[i].clear, sizeof clear[0].color);
    }
    if (has_depth) { clear[natt - 1].depthStencil.depth = d->clear_depth; clear[natt - 1].depthStencil.stencil = d->clear_stencil; }
    memset(bi, 0, sizeof *bi);
    bi->sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi->renderPass = pass;
    bi->framebuffer = fb;
    bi->renderArea = pass_area_aligned(d, granularity);
    bi->clearValueCount = natt;
    bi->pClearValues = clear;
}

// 记在段上的「段做完要销毁」的渲染通道 / 帧缓冲（没进缓存的才记）
static int pass_rec_add(struct nvn_gpu_cmd_s *c, VkRenderPass rp, VkFramebuffer fb) {
    if (!rp && !fb) return 1;
    if (c->npasses == c->passcap) {
        uint32_t cap = c->passcap ? c->passcap * 2 : 8;
        struct pass_rec *np = realloc(c->passes, cap * sizeof *np);
        if (!np) return 0;
        c->passes = np; c->passcap = cap;
    }
    c->passes[c->npasses++] = (struct pass_rec){rp, fb};
    return 1;
}

// 这张图是不是当前通道的附件（同一张图的任意级 / 层都算：Vulkan 的布局是整张图的）
static int pass_attached(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    if (!c->in_pass) return 0;
    for (uint32_t i = 0; i < c->pass_natt; i++) if (c->pass_tex[i] == t) return 1;
    return 0;
}

static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout to);   // 下面「贴图 / 视图 / 采样器」一节

static void pass_read_sync(struct nvn_gpu_cmd_s *c) {
    if (!c->feedback_read_mask) return;
    // NONE 没有隐式 store 写，须覆盖图形采样读到后续写的执行依赖。
    // 放在通道外：不能只改实际 pass 的依赖，否则不再兼容建管线的 pass 模板。
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

// 反馈环 / 通道内要改布局：当前通道的附件又要被采样（这张图这一段退回 GENERAL），或存储图像要 GENERAL 而它现在是只读布局。
// Vulkan 通道内不许改布局，所以在通道边界上重开：结束当前通道（附件内容按 finalLayout 留在显存里），
// 把要改的图在通道外迁好，再按 LOAD / STORE 重开同一组附件。只在 nvk_opt_layout=1 时才会走到；
// 开通道前能迁的都在 pass_begin / pass_flush_pending 里迁完了，这里只兜通道里新出现的绑定。
struct reopen_fix { struct nvn_gpu_tex_s *t; VkImageLayout want; };

// 进通道之前：本段里还停在「附件布局」、又不是本通道附件的图，一律先转成只读采样布局（在通道外转）。
// 这样通道里采样任何图都不用再迁布局，也顺带拿到 SHADER_READ_ONLY 的语义；下一段还要当附件时，
// 通道开头再按附件布局迁回来；提交前统一转回 GENERAL（lay_rest_at_commit）。
static void pass_flush_pending(struct nvn_gpu_cmd_s *c) {
    if (!c->opt) return;
    for (uint32_t i = 0; i < c->nlay; i++) {
        struct nvn_gpu_tex_s *t = c->lay[i].t;
        int attached = 0;
        for (uint32_t k = 0; k < c->pass_natt; k++) if (c->pass_tex[k] == t) { attached = 1; break; }
        if (attached) continue;
        if (c->lay[i].cur == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || c->lay[i].cur == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) image_barrier(c, t, sample_layout_for(t));
        if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    }
}

static int pass_reopen(struct nvn_gpu_cmd_s *c, const struct reopen_fix *fx, uint32_t nfx) {
    if (!c->in_pass || c->q_active >= 0) return 0;   // 查询不能跨通道；声明了 occlusion 的通道从开头就用整区
    nvn_gpu_pass_desc d = c->pass_desc;
    d.area_w = d.area_h = 0;
    for (uint32_t i = 0; i < d.ncolor && i < 8; i++) if (d.color[i].view) { d.color[i].load = NVN_GPU_LOAD_LOAD; d.color[i].store = NVN_GPU_STORE_STORE; }
    if (d.depth.view) { d.depth.load = NVN_GPU_LOAD_LOAD; d.depth.store = NVN_GPU_STORE_STORE; }
    if (d.stencil.view) { d.stencil.load = NVN_GPU_LOAD_LOAD; d.stencil.store = NVN_GPU_STORE_STORE; }
    if (c->tsq && c->ntsp < TS_PASS_MAX) { vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 3 + 2 * c->ntsp); c->ntsp++; }
    vkCmdEndRenderPass(c->cb);
    c->in_pass = 0;
    c->pass_occl = 0;
    pass_read_sync(c);
    VkImageView fb_views[9];
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    struct nvn_gpu_tex_s *att_tex[9]; VkImageLayout att_lay[9];
    uint32_t natt = 0;
    int has_depth = 0;
    g_pc_use = pc_on();
    if (pass_p16_reject(c, &d)) return 0;
    VkRenderPass pass = make_pass(&d, fb_views, att, col, &dref, &natt, &has_depth, att_tex, att_lay, c->opt, c->feedback_on, c->feedback_read_mask, NULL);
    if (!pass) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "通道重开：渲染通道创建失败"); return 0; }
    for (uint32_t i = 0; i < natt; i++) {   // 新布局（fb_sample 置位的附件变成 GENERAL），要在开新通道之前转完
        image_barrier(c, att_tex[i], att_lay[i]);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return 0; }
    }
    for (uint32_t i = 0; i < nfx; i++) {   // 通道里新出现、要换布局的图：也在通道外迁
        image_barrier(c, fx[i].t, fx[i].want);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return 0; }
    }
    VkFramebufferCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = pass;
    fi.attachmentCount = natt;
    fi.pAttachments = fb_views;
    fi.width = d.w;
    fi.height = d.h;
    fi.layers = d.layers ? d.layers : 1;
    int fb_cached = 0, fbsl = -1; uint32_t fbgen = 0;
    VkFramebuffer fb = pc_fb(&fi, &fb_cached, &fbsl, &fbgen);
    if (!fb) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); set_sticky(c, NVN_GPU_E_UNSUPPORTED, "通道重开：vkCreateFramebuffer 失败"); return 0; }
    pc_fb_note_desc(&d, fbsl, fbgen, fb);
    VkRenderPassBeginInfo bi;
    VkClearValue clear[9];
    pass_begin_info(&d, pass, fb, natt, has_depth, &bi, clear, (VkExtent2D){0});
    if (c->tsq && c->ntsp < TS_PASS_MAX) {
        c->tsp[c->ntsp].w = (uint16_t)d.w; c->tsp[c->ntsp].h = (uint16_t)d.h; c->tsp[c->ntsp].draws = 0;
        c->tsp[c->ntsp].ncol = (uint8_t)(has_depth ? natt - 1 : natt); c->tsp[c->ntsp].ds = (uint8_t)has_depth;
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 2 + 2 * c->ntsp);
    }
    vkCmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->in_pass = 1;   // 通道的其它状态（尺寸 / 格式 / 颜色槽 / 层数）与原来一致，不重设
    c->pass_occl = d.occlusion;
    c->pass_desc.area_w = c->pass_desc.area_h = 0;
    c->pass_natt = natt;
    for (uint32_t i = 0; i < natt; i++) c->pass_tex[i] = att_tex[i];
    if (!pass_rec_add(c, g_pc_use ? VK_NULL_HANDLE : pass, fb_cached ? VK_NULL_HANDLE : fb)) { set_sticky(c, NVN_GPU_E_OOM, "通道列表分配失败"); return 0; }
    return 1;
}

// 用真实 VkScissor 或清除矩形作最后一道检查。回放前端可能比实际对齐后的区域更保守；
// Vulkan API 调用之间即使出现内部重开，也只按最后一次实际开遍的范围判断。
static int pass_ensure_area(struct nvn_gpu_cmd_s *c, VkRect2D rect) {
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    if (!d->area_w || !d->area_h) return 1;
    const int64_t left = rect.offset.x < 0 ? 0 : rect.offset.x;
    const int64_t top = rect.offset.y < 0 ? 0 : rect.offset.y;
    int64_t right = (int64_t)rect.offset.x + rect.extent.width;
    int64_t bottom = (int64_t)rect.offset.y + rect.extent.height;
    if (right > d->w) right = d->w;
    if (bottom > d->h) bottom = d->h;
    if (right > left && bottom > top && left >= d->area_x && top >= d->area_y &&
        right <= (int64_t)d->area_x + d->area_w && bottom <= (int64_t)d->area_y + d->area_h) return 1;
    if (pass_reopen(c, NULL, 0)) return 1;
    if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_ORDER, "局部通道：需要整区，无法重开");
    return 0;
}

// 诊断读数（开关 debug.s2a.nvk_pass_prof）：开通道内部各步的耗时（纳秒累计 / 次数，每 512 个通道打一行）。
// 真机上一帧 112 个通道，「后端开通道」占 2.5 毫秒（每次 22 微秒），要看清是渲染通道查表、帧缓冲查表还是记账。
static uint64_t g_pbp_ns[6], g_pbp_n;
static void pass_prof(int i, uint64_t t0) {
    static int on = -1; static unsigned k;
    if (on < 0 || !(++k & 255)) on = nvn_plat_flag("nvk_pass_prof", 0) != 0;
    if (!on) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (i >= 0) g_pbp_ns[i] += now - t0;
    else if (++g_pbp_n >= 512) {
        char pl[300];
        snprintf(pl, sizeof pl, "开通道内部（每通道微秒）：渲染通道 %.2f / 附件迁移 %.2f / 帧缓冲 %.2f（命中 %llu / 未命中 %llu，未命中里新建共 %.0f 微秒）/ 开通道 %.2f / 引用与记账 %.2f（%llu 个，已缓存的渲染通道 %u 个）",
                 (double)g_pbp_ns[0] / 1e3 / (double)g_pbp_n, (double)g_pbp_ns[1] / 1e3 / (double)g_pbp_n, (double)g_pbp_ns[2] / 1e3 / (double)g_pbp_n,
                 (unsigned long long)__atomic_exchange_n(&g_fb_hit, 0, __ATOMIC_RELAXED), (unsigned long long)__atomic_exchange_n(&g_fb_miss, 0, __ATOMIC_RELAXED),
                 (double)__atomic_exchange_n(&g_fb_create_ns, 0, __ATOMIC_RELAXED) / 1e3,
                 (double)g_pbp_ns[3] / 1e3 / (double)g_pbp_n, (double)g_pbp_ns[4] / 1e3 / (double)g_pbp_n, (unsigned long long)g_pbp_n, g_pc_nrp);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass", pl);
        memset(g_pbp_ns, 0, sizeof g_pbp_ns); g_pbp_n = 0;
    }
}
void nvn_gpu_pass_begin(nvn_gpu_cmd ch, const nvn_gpu_pass_desc *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "pass_begin 时已在通道里"); return; }
    struct timespec pbp_ts; clock_gettime(CLOCK_MONOTONIC, &pbp_ts);
    uint64_t pbp_t = (uint64_t)pbp_ts.tv_sec * 1000000000ull + (uint64_t)pbp_ts.tv_nsec;
#define PBP_STEP(i) do { struct timespec z_; clock_gettime(CLOCK_MONOTONIC, &z_); uint64_t n_ = (uint64_t)z_.tv_sec * 1000000000ull + (uint64_t)z_.tv_nsec; pass_prof(i, pbp_t); pbp_t = n_; } while (0)
    es_reset(c);   // 冗余状态消除：新通道先保守地全部重发（#708）
    if (!d || !d->w || !d->h) { set_sticky(c, NVN_GPU_E_ARG, "pass_begin 参数非法（尺寸为 0）"); return; }   // 一个附件都没有也合法（NVN_GPU_CAP_NO_ATTACH_PASS）：帧缓冲按 w / h
    if (d->depth.view && d->stencil.view && d->depth.view != d->stencil.view) { set_sticky(c, NVN_GPU_E_ARG, "pass_begin 的深度与模板必须是同一个视图"); return; }
    uint32_t attachment_samples = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) if (d->color[i].view) {
        uint32_t n = d->color[i].view->tex->samples;
        if (attachment_samples && attachment_samples != n) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "msaa_attachment_mismatch: 颜色附件样本数不一致"); return; }
        attachment_samples = n;
    }
    if (d->depth.view || d->stencil.view) {
        uint32_t n = (d->depth.view ? d->depth.view : d->stencil.view)->tex->samples;
        if (attachment_samples && attachment_samples != n) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "msaa_attachment_mismatch: 深度与颜色附件样本数不一致"); return; }
    }
    VkImageView fb_views[9];
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    struct nvn_gpu_tex_s *att_tex[9]; VkImageLayout att_lay[9];
    uint32_t natt = 0;
    int has_depth = 0;
    g_pc_use = pc_on();
    if (pass_p16_reject(c, d)) return;
    VkExtent2D granularity = {0};
    VkRenderPass pass = make_pass(d, fb_views, att, col, &dref, &natt, &has_depth, att_tex, att_lay, c->opt, c->feedback_on,
                                  c->feedback_read_mask, d->area_w && d->area_h ? &granularity : NULL);
    PBP_STEP(0);
    if (!pass) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "渲染通道创建失败（附件格式 / 采样数）"); return; }
    // 附件先从现在的布局迁到这一段要用的布局：必须在 vkCmdBeginRenderPass 之前，通道内不许再迁
    for (uint32_t i = 0; i < natt; i++) {
        image_barrier(c, att_tex[i], att_lay[i]);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return; }
    }
    c->pass_natt = natt;
    for (uint32_t i = 0; i < natt; i++) c->pass_tex[i] = att_tex[i];
    pass_flush_pending(c);   // 本段里还停在附件布局、又不是本通道附件的图：先在通道外转成只读采样布局
    PBP_STEP(1);
    if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return; }
    VkFramebufferCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = pass;
    fi.attachmentCount = natt;
    fi.pAttachments = fb_views;
    fi.width = d->w;
    fi.height = d->h;
    fi.layers = d->layers ? d->layers : 1;
    int fb_cached = 0, fbsl = -1; uint32_t fbgen = 0;
    VkFramebuffer fb = pc_fb(&fi, &fb_cached, &fbsl, &fbgen);
    PBP_STEP(2);
    if (!fb) {
        if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL);
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "vkCreateFramebuffer 失败");
        return;
    }
    VkRenderPassBeginInfo bi;
    VkClearValue clear[9];
    pass_begin_info(d, pass, fb, natt, has_depth, &bi, clear, granularity);
    if (c->tsq && c->ntsp < TS_PASS_MAX) {
        c->tsp[c->ntsp].w = (uint16_t)d->w; c->tsp[c->ntsp].h = (uint16_t)d->h; c->tsp[c->ntsp].draws = 0; c->tsp[c->ntsp].ncol = (uint8_t)(has_depth ? natt - 1 : natt); c->tsp[c->ntsp].ds = (uint8_t)has_depth;
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 2 + 2 * c->ntsp);
    }
    vkCmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    PBP_STEP(3);
    c->in_pass = 1;
    c->pass_occl = d->occlusion;
    c->pass_w = d->w;
    c->pass_h = d->h;
    c->pass_ncolor = d->ncolor < 8 ? d->ncolor : 8;
    c->pass_color_mask = 0;
    memset(c->pass_cfmt, 0, sizeof c->pass_cfmt); c->pass_dsfmt = 0; c->pass_samples = 1;
    for (uint32_t i = 0; i < c->pass_ncolor; i++) if (d->color[i].view) {
        c->pass_color_mask |= 1u << i; c->pass_cfmt[i] = d->color[i].view->nvn_format; c->pass_samples = d->color[i].view->tex->samples; }
    if (d->depth.view || d->stencil.view) { struct nvn_gpu_tex_s *dt = (d->depth.view ? d->depth.view : d->stencil.view)->tex; c->pass_dsfmt = dt->nvn_format; c->pass_samples = dt->samples; }
    c->pass_layers = fi.layers;
    c->pass_ds_aspect = (d->depth.view ? d->depth.view->tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (d->stencil.view ? d->stencil.view->tex->aspect & VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    c->pass_desc = *d;
    c->pass_desc.area_x = bi.renderArea.offset.x;
    c->pass_desc.area_y = bi.renderArea.offset.y;
    c->pass_desc.area_w = c->pass_desc.area_h = 0;
    if (bi.renderArea.extent.width != d->w || bi.renderArea.extent.height != d->h) {
        c->pass_desc.area_w = bi.renderArea.extent.width;
        c->pass_desc.area_h = bi.renderArea.extent.height;
    }
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        if (!d->color[i].view) continue;
        cmd_ref(c, d->color[i].view, REF_VIEW);   // 附件 view（连带贴图）保活到本段完成
    }
    if (d->depth.view || d->stencil.view) {
        struct nvn_gpu_view_s *dsv = d->depth.view ? d->depth.view : d->stencil.view;
        cmd_ref(c, dsv, REF_VIEW);
    }
    pc_fb_note_desc(d, fbsl, fbgen, fb);
    // 记在段上的只有「段做完要销毁」的：没进缓存的帧缓冲、缓存关着时的渲染通道对象
    PBP_STEP(4);
    pass_prof(-1, 0);
    if (!pass_rec_add(c, g_pc_use ? VK_NULL_HANDLE : pass, fb_cached ? VK_NULL_HANDLE : fb)) { set_sticky(c, NVN_GPU_E_OOM, "通道列表分配失败"); return; }
#undef PBP_STEP
}

void nvn_gpu_pass_end(nvn_gpu_cmd ch) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "pass_end 时不在通道里"); return; }
    if (c->q_active >= 0) { vkCmdEndQuery(c->cb, c->q_active_pool, c->q_active_idx); c->q_active = -1; }   // 查询不跨通道（后端内部拆通道要接着数的，由 occl_spare_* 在新通道里续上）
    c->pass_occl = 0;
    vkCmdEndRenderPass(c->cb);
    c->in_pass = 0;
    c->pass_natt = 0;
    pass_read_sync(c);
    c->feedback_read_mask = 0;
    if (c->tsq && c->ntsp < TS_PASS_MAX) { vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 3 + 2 * c->ntsp); c->ntsp++; }
    // 附件在通道里用的布局由 make_pass 定（缺省全程 GENERAL，开 nvk_opt_layout 时用附件专用布局），通道内不迁移：
    // 出通道后本段第一次用到它时按需迁移（采样 / 存储 / 传输 / 再当附件），段末统一转回 GENERAL（lay_rest_at_commit）。
    // render pass / framebuffer 被已录制的命令引用，等 cmd 真正完成后再销毁（finish_cmd）。
}

// ---- 遮挡查询的续段（后端自己拆通道时用；目前只有附件反馈的快照，见 backend_vulkan_feedback.inc feedback_restart）----
// Vulkan 的同一个查询不能跨 VkRenderPass，而 NVN 的遮挡计数可以：后端拆通道时，活动查询在旧通道末尾结束（nvn_gpu_pass_end），
// 新通道里换一个续段槽接着数，读回时并回原槽（segment_done）。样本数是各段之和，不截断、不假报成功。
// 续段槽按块给（nvn_gpu_cmd_s.qchunk）：一块用满再要下一块，没有总数上限；一个声明槽名下最多 OCC_QUERY_SPARE_MAX 个（保险丝）。
// 要不到续段槽（保险丝到顶 / 分配或建池失败）：只这一个查询续不下去（q_used 的 2 位），拆通道照做、渲染不受影响，整段不失败；
// 它的读数按「未完成」处理（读 0，计入 nvn_unimpl("gpu", "occlusion_continuation_lost")），不留部分和——nvn_gpu_done 没有按槽失败的字段（#1658）。
// Ryujinx 的 BufferedQuery 每个查询最多 MaxQuerySegments = 64 段、超了显式失败而不是读回一个更小的「像样」的值，这里的「不假报」同它；
// 但 64 这个数不能照搬：固定 64 个续段槽的版本在真机上有一个场景用完了（#1658）。
// 开一个物理查询（声明槽与续段槽共用）。设备有精确计数就要精确值，没有就只保证「有样本通过时非 0」。
// 开关 debug.s2a.nvk_occl_fast（缺省 0 = 照能力位用 PRECISE 精确计数）：1 = 不带 PRECISE，走显卡的快速（可能非精确）计数。
// 精确计数在部分移动 GPU 上要逐个样本过一遍，本款每帧 400 多个遮挡查询、全在热通道里；NVN 的语义要精确值，先用读数看差多少。
static void occl_begin(struct nvn_gpu_cmd_s *c, VkQueryPool pool, uint32_t idx) {
    static int fast = -1;
    if (fast < 0) fast = nvn_plat_flag("nvk_occl_fast", 0) != 0;
    OC(OC_QUERY);
    vkCmdBeginQuery(c->cb, pool, idx, g.dev->core_features.occlusionQueryPrecise && !fast ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}
struct occl_cont { VkQueryPool pool; uint32_t idx; int fresh; };   // 一个续段槽：所在的块池、池内下标、是不是这一次新要的块（新块要在通道外整块重置）
static uint32_t g_occl_query_max = OCC_QUERY_SPARE_MAX;   // 一个声明槽名下最多几个续段槽；只有宿主用例（nvn_gpu_vk_test_occl_query_max）改小，免得为了碰到保险丝要画几万笔
static uint32_t g_occl_skip_begin;   // 宿主用例（nvn_gpu_vk_test_occl_skip_begin）：每段第 N 个续段槽（从 1 数）不开，让它读回时「没回来」；0 = 不干预
// 这个查询续不下去了：记下（q_used 的 2 位）、计入 nvn_unimpl，返回 0——调用方照常拆通道，只是这个查询不再续数。
static int occl_lost(struct nvn_gpu_cmd_s *c, int32_t slot) {
    c->q_used[slot] |= 2;
    nvn_unimpl("gpu", "occlusion_continuation_lost", NULL);
    return 0;
}
// 主机侧：为声明槽 slot 预留下一个续段槽（必要时把表扩一格、要一块新的查询池）。不录命令；要不到就返回 0（这个查询续不下去，已记下，不是粘滞错误）。
// 所有要先到手的东西都在改状态之前拿齐，失败时 qspare_used / q_owner / qchunk 原样不动。
static int occl_spare_reserve(struct nvn_gpu_cmd_s *c, int32_t slot, struct occl_cont *k) {
    const uint32_t j = c->qspare_used;
    k->fresh = 0;
    if (c->q_used[slot] & 2) return 0;
    if (j == 0 || c->q_owner[j - 1] != (uint32_t)slot) c->q_run_start = j;   // 换了查询：新的一串
    if (j - c->q_run_start >= g_occl_query_max) return occl_lost(c, slot);
    if (j == c->qspare_cap) {   // 归属表放不下了：翻倍
        const uint32_t ncap = c->qspare_cap ? c->qspare_cap * 2 : 64;
        uint32_t *t = realloc(c->q_owner, (size_t)ncap * sizeof *t);
        if (!t) return occl_lost(c, slot);
        c->q_owner = t; c->qspare_cap = ncap;
    }
    if (j % OCC_CHUNK == 0) {   // 当前块用满（或还没有块）：要下一块
        if (c->qnchunk == c->qchunk_cap) {
            const uint32_t ncap = c->qchunk_cap ? c->qchunk_cap * 2 : 4;
            VkQueryPool *t = realloc(c->qchunk, (size_t)ncap * sizeof *t);
            if (!t) return occl_lost(c, slot);
            c->qchunk = t; c->qchunk_cap = ncap;
        }
        VkQueryPool q = seg_reuse_on() ? seg_take_q(OCC_CHUNK) : VK_NULL_HANDLE;
        if (!q) {
            VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = OCC_CHUNK;
            if (vkCreateQueryPool(g.d, &qi, NULL, &q) != VK_SUCCESS) return occl_lost(c, slot);
        }
        c->qchunk[c->qnchunk++] = q;
        k->fresh = 1;
    }
    c->q_owner[j] = (uint32_t)slot;
    c->qspare_used = j + 1;
    k->pool = c->qchunk[j / OCC_CHUNK]; k->idx = j % OCC_CHUNK;
    return 1;
}
// 通道外（旧通道已结束、新通道还没开）：新要的块整块重置（重置只能在通道外；块里的槽用之前都在重置态，一块只重置这一次）。
static void occl_spare_reset(struct nvn_gpu_cmd_s *c, const struct occl_cont *k) {
    if (k->fresh) vkCmdResetQueryPool(c->cb, k->pool, 0, OCC_CHUNK);
}
// 新通道里：在预留好的续段槽上接着数，活动查询改指它（之后 end / pass_end 结束的就是这个槽）。
// 续段真正开出的次数（进程累计，nvn_gpu_stats 的 occl_cont_used）：没出错不等于续段走到了，真机读数要能直接看到它被用过。
//   第 1、10、100…… 次各打一行信息（带累计数），日志量有上限。
static uint64_t g_occl_cont_used;
static void occl_cont_note(void) {
    const uint64_t n = __atomic_add_fetch(&g_occl_cont_used, 1, __ATOMIC_RELAXED);
    uint64_t p = 1; while (p < n && p <= UINT64_MAX / 10) p *= 10;
    if (p != n) return;
    char b[160]; snprintf(b, sizeof b, "遮挡查询续段：反馈拆通道时活动查询换续段槽接着数，累计 %llu 次", (unsigned long long)n);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", b);
}
static void occl_spare_begin(struct nvn_gpu_cmd_s *c, int32_t slot, const struct occl_cont *k) {
    if (g_occl_skip_begin && c->qspare_used == g_occl_skip_begin) { c->q_active = -1; return; }   // 宿主用例：这个续段槽不开
    occl_begin(c, k->pool, k->idx);
    c->q_active = slot; c->q_active_pool = k->pool; c->q_active_idx = k->idx;
    occl_cont_note();
}

static int ensure_push_capacity(struct nvn_gpu_cmd_s *c, int st, uint32_t required) {
    if (required <= c->push_capacity[st]) return 1;
    if (required > g.caps.max_push_bytes) { set_sticky(c, NVN_GPU_E_ARG, "push 暂存超出设备上限"); return 0; }
    uint8_t *data = realloc(c->push_data[st], required);
    if (!data) { set_sticky(c, NVN_GPU_E_OOM, "push 暂存分配失败"); return 0; }
    memset(data + c->push_capacity[st], 0, required - c->push_capacity[st]);
    c->push_data[st] = data;
    c->push_capacity[st] = required;
    return 1;
}

void nvn_gpu_set_pipe(nvn_gpu_cmd ch, nvn_gpu_pipe ph) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_pipe_s *p = ph;
    if (!p) { set_sticky(c, NVN_GPU_E_ARG, "set_pipe 传了空管线"); return; }
    if (!c->in_pass && p->pass) { set_sticky(c, NVN_GPU_E_ORDER, "图形管线在通道外 set_pipe"); return; }
    if (c->in_pass && c->pass_natt && p->pass && p->actual_samples != c->pass_samples) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "msaa_pipeline_mismatch: 管线与附件样本数不一致"); return;
    }
    // Already allocated stage data survives pipe changes. Expand before the next push/dispatch.
    for (int st = 0; st <= NVN_GPU_SH_CS; st++) {
        uint64_t end = (uint64_t)p->stage_push_off[st] + p->stage_push_bytes[st];
        if (c->push_data[st] && end > UINT32_MAX) { set_sticky(c, NVN_GPU_E_ARG, "push 范围溢出"); return; }
        if (c->push_data[st] && !ensure_push_capacity(c, st, (uint32_t)end)) return;
    }
    if (c->pipe != ph && c->pool_active) { memset(c->pool_binds, 0, sizeof c->pool_binds); c->pool_active = 0; }
    c->pipe = ph;
}

void nvn_gpu_set_dyn(nvn_gpu_cmd ch, const nvn_gpu_dyn *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "set_dyn 在通道外"); return; }
    if (!d) { set_sticky(c, NVN_GPU_E_ARG, "set_dyn 为空"); return; }
    c->dyn = *d;
    c->has_dyn = 1;
}

void nvn_gpu_set_vbuf(nvn_gpu_cmd ch, uint32_t stream, nvn_gpu_buf bh, uint64_t off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (stream >= 16) { set_sticky(c, NVN_GPU_E_ARG, "顶点流 %u 越界", stream); return; }
    c->vbuf[stream].buf = bh;
    c->vbuf[stream].off = off;
    if (bh) cmd_ref(c, bh, REF_BUF);
}

// 先按地址查活视图表，再保活，绝不把旧 u.b 的 buffer 指针当作 view 解引用。
static int cmd_ref_texel_view(struct nvn_gpu_cmd_s *c, nvn_gpu_view handle, uint32_t cls) {
    VkFormatFeatureFlags need = cls == NVN_GPU_RES_TBUF ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT
                                                         : VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
    uint32_t cap = cls == NVN_GPU_RES_TBUF ? NVN_GPU_FMT_SAMPLE : NVN_GPU_FMT_STORAGE;
    gmu_lock_rt();
    struct nvn_gpu_view_s *v = g.views;
    while (v && v != handle) v = v->all_next;
    if (!v || v->released || !v->buffer_view || !(v->buffer_features & need) || !(v->buffer_caps & cap)) {
        pthread_mutex_unlock(&g_mu);
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "纹素缓冲绑定不是支持该用途的有效缓冲视图");
        return 0;
    }
    v->refs++;
    pthread_mutex_unlock(&g_mu);
    if (c->nrefs == c->refcap) {
        uint32_t cap = c->refcap ? c->refcap * 2 : 16;
        struct vk_ref *p = realloc(c->refs, (size_t)cap * sizeof *p);
        if (!p) {
            pthread_mutex_lock(&g_mu); unref_locked(&g, v, REF_VIEW); pthread_mutex_unlock(&g_mu);
            set_sticky(c, NVN_GPU_E_OOM, "引用表分配失败");
            return 0;
        }
        c->refs = p; c->refcap = cap;
    }
    c->refs[c->nrefs++] = (struct vk_ref){v, REF_VIEW};
    return 1;
}

// 不比较 union 的未用字节 / padding；缓冲偏移、范围及数组元素都是绑定身份的一部分。
static int binding_equal(const nvn_gpu_binding *a, const nvn_gpu_binding *b) {
    if (a->ent != b->ent || a->elem != b->elem || a->cls != b->cls) return 0;
    switch (a->cls) {
    case NVN_GPU_RES_UBO: case NVN_GPU_RES_SSBO:
        return a->u.b.buf == b->u.b.buf && a->u.b.off == b->u.b.off && a->u.b.len == b->u.b.len;
    case NVN_GPU_RES_TEX: case NVN_GPU_RES_IMAGE: case NVN_GPU_RES_TBUF: case NVN_GPU_RES_IMAGE_TBUF: return a->u.view == b->u.view;
    case NVN_GPU_RES_SAMP: return a->u.samp == b->u.samp;
    case NVN_GPU_RES_TEX_SAMP: return a->u.ts.view == b->u.ts.view && a->u.ts.samp == b->u.ts.samp;
    default: return 0;
    }
}

void nvn_gpu_bind(nvn_gpu_cmd ch, nvn_gpu_stage st, const nvn_gpu_binding *b, uint32_t n) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass && (!c->pipe || c->pipe->pass)) { set_sticky(c, NVN_GPU_E_ORDER, "bind 在通道外（非计算管线）"); return; }
    if (!c->pipe) { set_sticky(c, NVN_GPU_E_ORDER, "bind 前没有 set_pipe"); return; }
    if (!b && n) { set_sticky(c, NVN_GPU_E_ARG, "bind 表为空"); return; }
    if (n > 64) { set_sticky(c, NVN_GPU_E_ARG, "一个阶段一次绑定 %u 项，超过上限 64（原先多出来的被悄悄丢掉）", n); return; }
    if ((uint32_t)st > NVN_GPU_SH_CS) { set_sticky(c, NVN_GPU_E_ARG, "bind 阶段越界"); return; }
    if (c->desc_layout != c->pipe->layout) c->desc_dirty = 0xfu;
    else {
        uint32_t max = n > c->nbinds[st] ? n : c->nbinds[st];
        for (uint32_t i = 0; i < max; i++) {
            const nvn_gpu_binding *old = i < c->nbinds[st] ? &c->binds[st][i].b : NULL, *next = i < n ? &b[i] : NULL;
            if (old && next && binding_equal(old, next)) continue;
            if (old && next && c->pipe->stage_dyn[st] && old->cls == NVN_GPU_RES_UBO && next->cls == NVN_GPU_RES_UBO && old->ent == next->ent && old->elem == next->elem
                && old->ent < c->pipe->stage_nent[st] && c->pipe->stage_dyn[st][old->ent] >= 0 && old->u.b.buf == next->u.b.buf && old->u.b.len == next->u.b.len
                && ((old->u.b.off ^ next->u.b.off) & ~g_dyn_lo_mask) == 0) {   // 偏移的高位（缺省高 32 位）在描述符里（write_binds），不同就是内容变了，走脏组
                c->dyn_dirty = 1;   // 动态 UBO 只换了偏移的低 32 位：组内容没变，不重写，只用新偏移重绑（见 bind_descriptor_groups）
                __atomic_fetch_add(&g_dyn_fast, 1, __ATOMIC_RELAXED);
                continue;
            }
            if (old && old->ent < c->pipe->stage_nent[st]) c->desc_dirty |= 1u << c->pipe->stage_ents[st][old->ent].set;
            if (next && next->ent < c->pipe->stage_nent[st]) c->desc_dirty |= 1u << c->pipe->stage_ents[st][next->ent].set;
        }
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        if (b[i].ent < 64 && c->pool_binds[st][b[i].ent]) {
            set_sticky(c, NVN_GPU_E_LAYOUT, "普通绑定与池表使用相同布局项"); return;
        }
        if (c->pipe->stage_nent[st] && b[i].ent >= c->pipe->stage_nent[st]) {
            set_sticky(c, NVN_GPU_E_LAYOUT, "绑定项 %u 的 ent %u 越界", i, b[i].ent);
            return;
        }
        const nvn_gpu_layout_ent *e = c->pipe->stage_nent[st] ? &c->pipe->stage_ents[st][b[i].ent] : NULL;
        if (e && e->cls != b[i].cls) { set_sticky(c, NVN_GPU_E_LAYOUT, "绑定项 %u 类别 %u != 布局 %u", i, b[i].cls, e->cls); return; }
        struct nvn_gpu_tex_s *bt = bind_tex_of(&b[i]);
        if (bt && bt->samples == 1 && (bt->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || bt->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY)) {
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "msaa_sampled_single: 多重采样纹理降为单采样后不能作为多采样着色器输入"); return;
        }
        c->binds[st][count].b = b[i];
        if (b[i].cls == NVN_GPU_RES_TEX_SAMP) {
            if (!b[i].u.ts.view || !b[i].u.ts.samp) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "TEX_SAMP 绑定缺 view 或 samp");
                return;
            }
            cmd_ref(c, b[i].u.ts.view, REF_VIEW);
            cmd_ref(c, b[i].u.ts.samp, REF_SAMP);
        } else if (b[i].cls == NVN_GPU_RES_TEX && b[i].u.view) cmd_ref(c, b[i].u.view, REF_VIEW);
        else if (b[i].cls == NVN_GPU_RES_SAMP && b[i].u.samp) cmd_ref(c, b[i].u.samp, REF_SAMP);
        else if (b[i].cls == NVN_GPU_RES_IMAGE && b[i].u.view) cmd_ref(c, b[i].u.view, REF_VIEW);
        else if (b[i].cls == NVN_GPU_RES_TBUF || b[i].cls == NVN_GPU_RES_IMAGE_TBUF) {
            if (!cmd_ref_texel_view(c, b[i].u.view, b[i].cls)) return;
        }
        else if ((b[i].cls == NVN_GPU_RES_UBO || b[i].cls == NVN_GPU_RES_SSBO) && b[i].u.b.buf) cmd_ref(c, b[i].u.b.buf, REF_BUF);
        count++;
    }
    c->nbinds[st] = count;
}

// 测试读数：各类状态命令实际录了几次（下标同 ES_*：管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push）
void nvn_gpu_vk_test_emit_counts(nvn_gpu_cmd ch, uint32_t out[9]) {
    struct nvn_gpu_cmd_s *c = ch;
    for (int i = 0; i < 9; i++) out[i] = c && i < ES_KINDS ? c->es_count[i] : 0;
}

// 测试读数（#1194）：缓冲句柄现在被几段持有（refs）/ 一段命令的引用表现有几条。只给宿主用例读：句柄必须还活着（没 release 或还被段引用着）。
uint32_t nvn_gpu_vk_test_buf_refs(nvn_gpu_buf bh) {
    struct nvn_gpu_buf_s *b = bh;
    if (!b) return 0;
    pthread_mutex_lock(&g_mu);
    const uint32_t n = (uint32_t)b->refs;
    pthread_mutex_unlock(&g_mu);
    return n;
}
uint32_t nvn_gpu_vk_test_cmd_nrefs(nvn_gpu_cmd ch) { struct nvn_gpu_cmd_s *c = ch; return c ? c->nrefs : 0; }
// 这一段里 cmd_ref 命中备忘（直接返回）的次数：每次引用要么命中、要么追加一条引用表项（报错的除外），宿主用例拿它核恒等式，
// 也靠它看出「报错的那次被记进了备忘」（报错之后再引用同一句柄，命中数不许动）。
uint32_t nvn_gpu_vk_test_cmd_refseen_hits(nvn_gpu_cmd ch) { struct nvn_gpu_cmd_s *c = ch; return c ? c->refseen_hits : 0; }
// 对照档：0 = 备忘关（每次都走原路径），1 = 产品行为。只在没有会话在录制的时候改。
void nvn_gpu_vk_test_ref_seen(int on) { g_ref_seen = on != 0; }

// Test-only observation of the real allocation; never reads or writes push bytes.
uint32_t nvn_gpu_vk_test_push_capacity(nvn_gpu_cmd ch, nvn_gpu_stage st) {
    struct nvn_gpu_cmd_s *c = ch;
    return c && (uint32_t)st <= (uint32_t)NVN_GPU_SH_CS ? c->push_capacity[st] : 0;
}

// 只给宿主用例用：遮挡查询续段块的槽数（用例据此造「刚好一块 / 跨块」的场景，不把常量抄进用例）；
// 与一段最多几块的上限（n = 0 恢复缺省；返回改之前的值）：用例改成 2 块，不用画几万笔就能验「到顶显式失败」。
uint32_t nvn_gpu_vk_test_occl_chunk(void) { return OCC_CHUNK; }
uint32_t nvn_gpu_vk_test_occl_query_max(uint32_t n) { const uint32_t old = g_occl_query_max; g_occl_query_max = n ? n : OCC_QUERY_SPARE_MAX; return old; }
uint32_t nvn_gpu_vk_test_occl_skip_begin(uint32_t n) { const uint32_t old = g_occl_skip_begin; g_occl_skip_begin = n; return old; }
uint32_t nvn_gpu_vk_test_occl_declared_miss(uint32_t on) { const uint32_t old = g_occl_declared_miss; g_occl_declared_miss = on; return old; }

void nvn_gpu_push(nvn_gpu_cmd ch, nvn_gpu_stage st, uint32_t off, const void *data, uint32_t bytes) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if ((uint32_t)st > (uint32_t)NVN_GPU_SH_CS) { set_sticky(c, NVN_GPU_E_ARG, "push 阶段 %d 非法", (int)st); return; }
    if (!c->pipe) { set_sticky(c, NVN_GPU_E_ORDER, "push 前没有 set_pipe"); return; }
    if (!c->in_pass && c->pipe->pass) { set_sticky(c, NVN_GPU_E_ORDER, "push 在通道外（非计算管线）"); return; }
    if (!data && bytes) { set_sticky(c, NVN_GPU_E_ARG, "push data 为空"); return; }
    uint32_t po = c->pipe->stage_push_off[st], pb = c->pipe->stage_push_bytes[st];
    if (!pb) { set_sticky(c, NVN_GPU_E_ARG, "阶段 %d 未声明 push 范围", (int)st); return; }
    if (off < po || (uint64_t)(off - po) + bytes > pb) {
        set_sticky(c, NVN_GPU_E_ARG, "push [%u,+%u) 越出阶段 %d 声明范围 [%u,+%u)", off, bytes, (int)st, po, pb);
        return;
    }
    if (!bytes) return;
    uint64_t end = (uint64_t)po + pb;
    if (end > UINT32_MAX) { set_sticky(c, NVN_GPU_E_ARG, "push 范围溢出"); return; }
    if (!ensure_push_capacity(c, st, (uint32_t)end)) return;
    if (!c->push_have[st] || memcmp(c->push_data[st] + off, data, bytes)) { memcpy(c->push_data[st] + off, data, bytes); c->push_gen[st]++; }
    c->push_have[st] = 1;
}

// ---------------------------------------------------------------- 描述符池 / 绑定写入 / texel 视图

// 每条 cmd 一个池：64 set（缺扩容是已知静默错 C66 的复现对象，本段不修）。
// 一段命令的描述符池：一池 256 组，分满了换新池接着分（旧池留到段完成再销毁），一段里画多少笔都不设上限。
// 池的上限按规范可以被驱动放宽也可以严格执行，所以「分满」只认分配调用返回的池耗尽 / 碎片两种结果，不自己数。
enum { DPOOL_SETS = 256, DPOOL_PER_TYPE = 1024 };
static int new_dpool(struct nvn_gpu_cmd_s *c) {
    VkDescriptorPoolSize ps[9] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_SAMPLER, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, DPOOL_PER_TYPE}};
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = DPOOL_SETS;
    dpi.poolSizeCount = 9;
    dpi.pPoolSizes = ps;
    if (seg_reuse_on() && (c->dpool = seg_take_desc()) != VK_NULL_HANDLE) return 1;
    OC(OC_DPOOL);
    if (vkCreateDescriptorPool(g.d, &dpi, NULL, &c->dpool) != VK_SUCCESS) {
        c->dpool = VK_NULL_HANDLE;
        set_sticky(c, NVN_GPU_E_OOM, "描述符池创建失败");
        return 0;
    }
    return 1;
}
// 给一笔绘制 / 派发分配它的 n 组描述符。0 = 失败（已记粘滞错误）
// p / set：这一组属于哪条管线的第几组（dsl 就是 p->dsl + set；回收表的表项指针记在管线里，见 dset_take）；
// p 为 NULL = 这一组不属于任何管线（深度模板缩放自己的布局），不进回收，直接走下面老路径；sig：这一笔给这一组写的写法（p 为 NULL 时不看）
static int alloc_sets(struct nvn_gpu_cmd_s *c, const VkDescriptorSetLayout *dsl, uint32_t n, VkDescriptorSet *sets, struct nvn_gpu_pipe_s *p, uint32_t set, uint64_t sig) {
    if (n == 1 && p && __atomic_load_n(&g_dset_on, __ATOMIC_RELAXED) && dset_take(c, p, set, sig, sets)) return 1;   // 按布局回收（#1096）；取不到才走下面老路径
    for (int fresh = 0; fresh < 2; fresh++) {
        if (!c->dpool && !new_dpool(c)) return 0;
        VkDescriptorSetAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = c->dpool;
        ai.descriptorSetCount = n;
        ai.pSetLayouts = dsl;
        OC(OC_DALLOC);
        VkResult r = vkAllocateDescriptorSets(g.d, &ai, sets);
        if (r == VK_SUCCESS) return 1;
        if (fresh || (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)) {
            set_sticky(c, NVN_GPU_E_OOM, "vkAllocateDescriptorSets 失败（%d%s）", (int)r, fresh ? "，新池上也分不出" : "");
            return 0;
        }
        // 当前池分满：挪进「已满」表，下一轮在新池上重分这一笔
        if (c->ndpools_full == c->dpools_full_cap) {
            uint32_t cap = c->dpools_full_cap ? c->dpools_full_cap * 2 : 8;
            VkDescriptorPool *np = realloc(c->dpools_full, cap * sizeof *np);
            if (!np) { set_sticky(c, NVN_GPU_E_OOM, "描述符池表分配失败"); return 0; }
            c->dpools_full = np;
            c->dpools_full_cap = cap;
        }
        c->dpools_full[c->ndpools_full++] = c->dpool;
        c->dpool = VK_NULL_HANDLE;
    }
    return 0;
}

// 采样绑定在描述符里声明的布局必须等于这一笔执行时图像的实际布局：本段跟踪的 GENERAL / *READ_ONLY 二选一。
static VkImageView feedback_view(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v) {
    for (struct feedback_copy *f = c->feedback; f != c->feedback_stop; f = f->next)
        if (f->source == v) return f->view;
    return v ? v->view : VK_NULL_HANDLE;
}
static VkImageLayout bind_image_layout(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v) {
    if (v && feedback_view(c, v) != v->view) return VK_IMAGE_LAYOUT_GENERAL;
    return c->opt && v && v->tex ? lay_cur(c, v->tex) : VK_IMAGE_LAYOUT_GENERAL;
}
// 仅在分离绑定时按需建立平均采样器，组合绑定仍使用原归约采样器。
// s 已被命令引用保活；g_mu 同时保护创建、边框配额和销毁。
static VkSampler samp_separate_average(struct nvn_gpu_samp_s *s) {
    pthread_mutex_lock(&g_mu);
    if (!s->average_sampler && (!s->average_custom || g.border_live < g.dev->custom_border_max)) {
        if (vkCreateSampler(g.d, &s->average_info, NULL, &s->average_sampler) == VK_SUCCESS && s->average_custom) {
            g.border_live++;
            s->border_slots++;
        }
    }
    VkSampler sampler = s->average_sampler;
    pthread_mutex_unlock(&g_mu);
    return sampler;
}

// 把某阶段本笔绑定写进 wr/ii/bi；返回写入条数，出错置粘滞返回 -1。
// 缺成员的新类别（IMAGE / TBUF / IMAGE_TBUF）报 E_LAYOUT；老类别保持 NULL 句柄空资源写法不变。
// 只写 dirty 里的组（没变的组沿用上一笔的描述符集对象，不重写）。纹素缓冲的格式核对取决于管线，
// 组没变、管线换了也要核，所以这两类不在脏组里也走一遍核对，只是不写。
// sig[组]：把这一笔写进该组的每一条（binding 与数组项号）的散列加进去，求和与顺序无关；取组时按它挑回收表的表项（见 dset_take）。
static inline uint64_t dset_sig_mix(uint32_t binding, uint32_t elem) {
    uint64_t x = ((uint64_t)binding << 32 | elem) + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static int write_binds(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int st, uint32_t dirty,
                       const VkDescriptorSet *sets, VkWriteDescriptorSet *wr,
                       VkDescriptorImageInfo *ii, VkDescriptorBufferInfo *bi, uint64_t *sig) {
    int nw = 0;
    for (uint32_t i = 0; i < c->nbinds[st]; i++) {
        const nvn_gpu_binding *b = &c->binds[st][i].b;
        if (!p->stage_nent[st]) { set_sticky(c, NVN_GPU_E_LAYOUT, "阶段 %d 没有布局项却给了绑定", st); return -1; }
        const nvn_gpu_layout_ent *e = &p->stage_ents[st][b->ent];
        const int dynb = p->stage_dyn[st] && p->stage_dyn[st][b->ent] >= 0;   // 动态 UBO（#1233）：描述符里放偏移的高 32 位（一般为 0），低 32 位记进 dyn_off、绑定时给；两者相加还是原偏移
        if (dynb) {
            const uint32_t di = (uint32_t)p->stage_dyn[st][b->ent] + b->elem;
            if (e->set >= 4 || b->elem >= (e->count ? e->count : 1u) || di >= p->ndyn[e->set]) { set_sticky(c, NVN_GPU_E_LAYOUT, "动态 UBO 绑定项 ent %u 的数组下标 %u 越界", b->ent, b->elem); return -1; }
            c->dyn_off[e->set][di] = (uint32_t)(b->u.b.off & g_dyn_lo_mask);
        }
        const int live = (int)(dirty >> e->set & 1u);
        if (!live && e->cls != NVN_GPU_RES_TBUF && e->cls != NVN_GPU_RES_IMAGE_TBUF) continue;
        VkWriteDescriptorSet *w = &wr[nw];
        memset(w, 0, sizeof *w);
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w->dstSet = sets[e->set];
        w->dstBinding = e->binding;
        w->dstArrayElement = b->elem;
        w->descriptorCount = 1;
        w->descriptorType = dynb ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : vk_desc_type(e->cls);
        if (live) sig[e->set] += dset_sig_mix(e->binding, b->elem);
        VkDescriptorImageInfo *im = &ii[nw];
        VkDescriptorBufferInfo *bf = &bi[nw];
        switch (e->cls) {
        case NVN_GPU_RES_TEX:
            memset(im, 0, sizeof *im);
            im->imageView = feedback_view(c, b->u.view);
            im->imageLayout = bind_image_layout(c, b->u.view);
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_SAMP:
            memset(im, 0, sizeof *im);
            im->sampler = b->u.samp ? b->u.samp->sampler : VK_NULL_HANDLE;
            if (b->u.samp && b->u.samp->reduction) {
                im->sampler = samp_separate_average(b->u.samp);
                if (!im->sampler) {
                    set_sticky(c, NVN_GPU_E_UNSUPPORTED, "sampler_reduction_average_separate_create：无法建立平均采样器");
                    return -1;
                }
                nvn_unimpl("gpu", "sampler_reduce_average_separate_unknown_format", NULL);
            }
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_TEX_SAMP:
            if (!b->u.ts.view || !b->u.ts.samp) { set_sticky(c, NVN_GPU_E_LAYOUT, "TEX_SAMP 绑定缺 view 或 samp"); return -1; }
            memset(im, 0, sizeof *im);
            im->imageView = feedback_view(c, b->u.ts.view);
            { nvn_fmt_info vfi; int vint = nvn_fmt_info_of(b->u.ts.view->nvn_format, &vfi) && (vfi.kind == NVN_FMT_UINT || vfi.kind == NVN_FMT_SINT);
              im->sampler = vint ? b->u.ts.samp->int_sampler : b->u.ts.samp->sampler;
              if (!vint && b->u.ts.samp->reduction) {   // #976：做 min / max 归约的视图：格式要有 FILTER_MINMAX
                  VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, b->u.ts.view->format, &fp);
                  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT_EXT)) {
                      set_sticky(c, NVN_GPU_E_UNSUPPORTED, "sampler_reduction_view_format：视图格式 0x%x（Vulkan %d）没有 SAMPLED_IMAGE_FILTER_MINMAX，不能做 min / max 归约",
                                 b->u.ts.view->nvn_format, (int)b->u.ts.view->format);
                      return -1;
                  }
              } }
            im->imageLayout = bind_image_layout(c, b->u.ts.view);
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_IMAGE:
            if (!b->u.view) { set_sticky(c, NVN_GPU_E_LAYOUT, "IMAGE 绑定缺 view"); return -1; }
            if (b->u.view->p16_remap) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "IMAGE 绑定：16 位打包格式 0x%x 在本设备走退回格式，不能当存储图像", b->u.view->nvn_format); return -1; }
            memset(im, 0, sizeof *im);
            im->imageView = view_image_form(b->u.view);   // 带通道映射的视图不能当存储图像用：另建恒等形态
            im->imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // 存储图像一律 GENERAL（Vulkan 要求），迁移见 prepare_bind_layouts
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_UBO:
        case NVN_GPU_RES_SSBO:
            memset(bf, 0, sizeof *bf);
            bf->buffer = b->u.b.buf ? b->u.b.buf->buffer : VK_NULL_HANDLE;
            bf->offset = dynb ? (b->u.b.off & ~g_dyn_lo_mask) : b->u.b.off;
            bf->range = b->u.b.len;
            if (e->cls == NVN_GPU_RES_UBO && b->u.b.len > g.caps.max_ubo_range) {   // UBO 没有 WHOLE_SIZE 例外：超过范围上限只能准确拒绝
                set_sticky(c, NVN_GPU_E_ARG, "常量缓冲范围 %llu 超过 maxUniformBufferRange %llu", (unsigned long long)b->u.b.len, (unsigned long long)g.caps.max_ubo_range);
                return -1; }
            if (e->cls == NVN_GPU_RES_SSBO && b->u.b.len > g.caps.max_ssbo_range) {
                // 存储缓冲同样只能准确拒绝：改绑 VK_WHOLE_SIZE 并不豁免，有效范围（缓冲尾 − offset）照样受 maxStorageBufferRange 约束
                // （VUID-VkWriteDescriptorSet-descriptorType-00333，校验层原话「is VK_WHOLE_SIZE, but the effective range … is greater than
                // maxStorageBufferRange」）。回放核心已按上限夹好窗口（rp_draw.c sb_window），走到这里是调用方没夹。
                set_sticky(c, NVN_GPU_E_ARG, "存储缓冲范围 %llu 超过 maxStorageBufferRange %llu", (unsigned long long)b->u.b.len, (unsigned long long)g.caps.max_ssbo_range);
                return -1; }
            w->pBufferInfo = bf;
            break;
        case NVN_GPU_RES_TBUF:
        case NVN_GPU_RES_IMAGE_TBUF: {
            struct nvn_gpu_view_s *v = b->u.view; // 已在 bind 时核验并保活
            VkFormat f = p->stage_tfmt[st] ? p->stage_tfmt[st][b->ent] : VK_FORMAT_UNDEFINED;
            if (f != VK_FORMAT_UNDEFINED && f != v->buffer_format) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "texel 绑定视图格式与着色器声明不符");
                return -1;
            }
            if (e->cls == NVN_GPU_RES_IMAGE_TBUF && f == VK_FORMAT_UNDEFINED &&
                !texel_unknown_storage_supported(v->buffer_format)) {
                set_sticky(c, NVN_GPU_E_UNSUPPORTED, "纹素视图格式不支持无格式存储访问");
                return -1;
            }
            w->pTexelBufferView = &v->buffer_view;
            break;
        }
        default:
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "绑定类别 %u：本段未实现", e->cls);
            return -1;
        }
        if (live) nw++;
    }
    return nw;
}

// 带整池数组的组的键：各池绑定（绑定号, 池快照序号）的两路无序混合和（进段内备忘 / 回收的键）；own = 这一组里按（阶段, 条目）顺序遇到的第一张池快照（NULL = 这一组没有池绑定）。
struct pool_set_info { uint64_t k1, k2; nvn_gpu_pool_table own; };
static int pool_set_mask(struct nvn_gpu_cmd_s *, const struct nvn_gpu_pipe_s *, int, uint32_t *, struct pool_set_info *);
static int pool_alloc_set(struct nvn_gpu_cmd_s *, const struct nvn_gpu_pipe_s *, uint32_t, VkDescriptorSet *);
static int pool_write_sets(struct nvn_gpu_cmd_s *, const struct nvn_gpu_pipe_s *, int, uint32_t, uint32_t);
// 整池组回收（见 backend_vulkan_pool.inc 的 prs_*）。反馈拷贝在用时，这一笔整池（图像池）里要换成快照的项：
enum { PRS_PATCH_MAX = 64 };
struct prs_plan { uint32_t n; struct { uint32_t set, binding, idx; VkImageView view; VkDescriptorImageInfo canon; } e[PRS_PATCH_MAX]; };
static int prs_plan(struct nvn_gpu_cmd_s *, const struct nvn_gpu_pipe_s *, int, uint32_t, struct prs_plan *);
static int prs_take(struct nvn_gpu_cmd_s *, const struct nvn_gpu_pipe_s *, uint32_t, uint64_t, const struct pool_set_info *, VkDescriptorSet *, int *, uint32_t *);
static void prs_mark(struct nvn_gpu_cmd_s *, uint32_t, const struct pool_set_info *);
static int prs_apply(struct nvn_gpu_cmd_s *, const struct prs_plan *, uint32_t, const uint32_t *);
static int pool_check_layouts(struct nvn_gpu_cmd_s *, const struct nvn_gpu_pipe_s *, int);
// 脏组分配新对象，不能更新前一笔已经引用的组；跨阶段共用同一组时写全组。
static int bind_descriptor_groups(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute) {
    const uint8_t point = compute ? 2 : 1;
    if (c->desc_layout != p->layout || c->desc_point != point) {
        c->desc_layout = p->layout; c->desc_point = point; c->desc_dirty = 0xfu;
        memset(c->desc_sets, 0, sizeof c->desc_sets);
        memset(c->dyn_off, 0, sizeof c->dyn_off); c->dyn_dirty = 0;   // 换了布局全部重写重绑，动态偏移在下面从 binds 里重算
    }
    // A descriptor-free pipeline ignores bindings left by the previous pipeline.
    if (!p->nset) return 1;
    uint32_t pool_mask = 0; struct pool_set_info pinfo[4] = {{0, 0, NULL}, {0, 0, NULL}, {0, 0, NULL}, {0, 0, NULL}};   // 带整池数组的组；每组里各池绑定（绑定号, 池快照序号）的混合和，进备忘 / 回收的键
    if (c->pool_active && !pool_set_mask(c, p, compute, &pool_mask, pinfo)) return 0;
    uint32_t dirty = c->desc_dirty & ((1u << p->nset) - 1u);
    // 哪一组都没变、布局和绑定点也没换：命令缓冲里绑着的就是这几组，不用重写也不用重绑。
    // 纹素缓冲的格式核对随管线变（见 write_binds），有这类绑定的管线不走这条近路。
    struct nvn_gpu_pipe_s *pm = (struct nvn_gpu_pipe_s *)(uintptr_t)p;
    if (!pm->texel_binds) {
        int8_t has = -1;
        for (int st = 0; st <= NVN_GPU_SH_CS; st++) for (uint32_t i = 0; i < p->stage_nent[st]; i++)
            if (p->stage_ents[st][i].cls == NVN_GPU_RES_TBUF || p->stage_ents[st][i].cls == NVN_GPU_RES_IMAGE_TBUF) has = 1;
        pm->texel_binds = has;
    }
    if (!dirty && !c->dyn_dirty && p->texel_binds < 0) return 1;
    const uint8_t dynd = c->dyn_dirty;   // 只有动态 UBO 偏移变了：不重写组，下面只用新偏移重绑
    const int tm = g_draw_tm; const uint64_t t0 = tm ? now_ns() : 0;   // 诊断：这四步各用多久（nvk_gputime 开着才计）
    // 描述符组备忘（nvk_desc_memo，缺省开，0 = 关；照 d2r-mobile 安卓笔记 perf-round2 发现 4）：
    // 同一段里内容完全相同的组（同一布局、每条描述符的绑定号 / 类型 / 资源都相同）直接用上次写好的那一组，不再分配、不再写。
    // 一款 3D 游戏每帧重写 2800 组、1.13 万条描述符（分配 2.3 毫秒 + 写入 2.05 毫秒）。组归这一段的描述符池，段做完才销毁；
    // 内容相同就是资源相同，资源由首次写入那笔的引用保活到段完成。
    // UBO 用动态偏移时（pipe_build）描述符里没有偏移（高 32 位一般为 0），只换偏移的两组键相同，才会命中；静态 UBO 的管线偏移在键里，命中率低（#1233）。
    static int memo_cur = -1; static uint32_t memo_k;
    if (memo_cur < 0 || !(++memo_k & 255)) memo_cur = desc_memo_flag();
    // 整池数组组（pool_mask 里的组）：脏的整池组也进备忘（nvk_pool_memo，缺省开，0 = 关）。键 = 布局 + 这一笔写入的普通绑定 + 每个池绑定的（绑定号, 池快照指针）（pool_set_mask）；
    // 池快照建好就不可变、本段持引用到段完成，指针相同 = 整张表的内容相同，所以键相同的组直接用上次写好的那一组，不再建池、不再重写整张表。
    // 反馈拷贝在用时整池里的视图要换成拷贝（pool_write_sets），拷贝随反馈表变、不在键里：这一笔整体走不备忘的老路径。池组不脏时不影响别的组备忘。
    if (g_pmemo_cur < 0 || !(++g_pmemo_k & 255)) { g_pmemo_cur = pool_memo_flag(); g_prs_cur = pool_rec_flag(); }
    const uint32_t pool_dirty = dirty & pool_mask;
    const int pool_fb = pool_dirty && c->feedback != c->feedback_stop;
    const int memo = memo_cur && dirty && (!pool_dirty || (g_pmemo_cur && !pool_fb));
    // 整池组回收（nvk_pool_rec，见 backend_vulkan_pool.inc 的 prs_*）：反馈拷贝在用时先算「整池里要换成快照的项」（补丁），太多就这一笔整个走老路径。
    struct prs_plan plan; plan.n = 0;
    const int rec = g_prs_cur && pool_dirty && (!pool_fb || prs_plan(c, p, compute, pool_dirty, &plan));
    uint32_t rec_mask = 0, valid_mask = 0, uidx[4] = {0, 0, 0, 0};   // 回收取到的组 / 其中整池内容已就绪的组 / 它们在本段取用记录里的下标
    if (pool_dirty) {
        const uint64_t np = (uint64_t)__builtin_popcount(pool_dirty);
        __atomic_fetch_add(&g_pool_n[PN_NEED], np, __ATOMIC_RELAXED);
        if (pool_fb) __atomic_fetch_add(&g_pool_n[PN_FB], np, __ATOMIC_RELAXED);
    }
    uint32_t hit = 0;   // 备忘命中的组：不分配、不写
    // 先不分配：写入项里先放记号（认出每条属于哪一组），写完才知道每一组这一笔的「写法」（write_binds 的 sig），再按写法去回收表取组（见 dset_take）。
    // 备忘开着时命中的组连取都省了；备忘关着时取完组把记号换成真的组。
    VkDescriptorSet mark[4];
    for (uint32_t set = 0; set < 4; set++) mark[set] = (VkDescriptorSet)(uintptr_t)(0x5a0 + set);
    uint64_t sig[4] = {0, 0, 0, 0};
    const uint64_t t1 = tm ? now_ns() : 0;
    VkWriteDescriptorSet wr[6 * 64]; VkDescriptorImageInfo ii[6 * 64]; VkDescriptorBufferInfo bi[6 * 64];
    uint32_t nw = 0;
    for (int st = compute ? NVN_GPU_SH_CS : 0; st < (compute ? NVN_GPU_SH_CS + 1 : 5); st++) {
        int n = write_binds(c, p, st, dirty, mark, wr + nw, ii + nw, bi + nw, sig);
        if (n < 0) return 0;
        nw += (uint32_t)n;
    }
    uint64_t ta = 0;   // 诊断：取组用时（备忘开着时取组在下面的备忘块里，不单独计）
    if (!memo) {
        const uint64_t ta0 = tm ? now_ns() : 0;
        for (uint32_t set = 0; set < p->nset; set++)
            if (dirty & (1u << set)) {
                if (pool_mask & (1u << set)) {
                    int v = 0;
                    if (rec && prs_take(c, p, set, sig[set], pinfo + set, c->desc_sets + set, &v, uidx + set)) { rec_mask |= 1u << set; if (v) valid_mask |= 1u << set; }
                    else if (!pool_alloc_set(c, p, set, c->desc_sets + set)) return 0;
                }
                else if (!alloc_sets(c, p->dsl + set, 1, c->desc_sets + set, pm, set, sig[set])) return 0;
            }
        for (uint32_t i = 0; i < nw; i++) {   // 记号换成真的组（write_binds 只写脏组，记号一定落在 0..3）
            const uint32_t set = (uint32_t)((uintptr_t)wr[i].dstSet - 0x5a0);
            if (set < 4) wr[i].dstSet = c->desc_sets[set];
        }
        if (tm) ta = now_ns() - ta0;
    }
    if (memo) {
        uint64_t h1[4], h2[4];
        for (uint32_t set = 0; set < 4; set++) { h1[set] = 1469598103934665603ull ^ (uintptr_t)(set < p->nset ? p->dsl[set] : 0); h2[set] = 0x9E3779B97F4A7C15ull + set; }
        for (uint32_t i = 0; i < nw; i++) {
            const uint32_t set = (uint32_t)((uintptr_t)wr[i].dstSet - 0x5a0); if (set >= 4) continue;
            uint64_t v[5] = {(uint64_t)wr[i].dstBinding << 32 | wr[i].dstArrayElement, (uint64_t)wr[i].descriptorType, 0, 0, 0};
            if (wr[i].pImageInfo) { v[2] = (uint64_t)(uintptr_t)wr[i].pImageInfo->imageView; v[3] = (uint64_t)(uintptr_t)wr[i].pImageInfo->sampler; v[4] = (uint64_t)wr[i].pImageInfo->imageLayout; }
            else if (wr[i].pBufferInfo) { v[2] = (uint64_t)(uintptr_t)wr[i].pBufferInfo->buffer; v[3] = wr[i].pBufferInfo->offset; v[4] = wr[i].pBufferInfo->range; }
            else if (wr[i].pTexelBufferView) v[2] = (uint64_t)(uintptr_t)*wr[i].pTexelBufferView;
            for (int k = 0; k < 5; k++) { h1[set] = (h1[set] ^ v[k]) * 1099511628211ull; h2[set] = (h2[set] + v[k]) * 0xff51afd7ed558ccdull; h2[set] ^= h2[set] >> 29; }
        }
        for (uint32_t set = 0; set < 4; set++) if (pool_dirty >> set & 1) {   // 整池组：再把每个池绑定的（绑定号, 池快照序号）混进键
            h1[set] = (h1[set] ^ pinfo[set].k1) * 1099511628211ull;
            h2[set] = (h2[set] + pinfo[set].k2) * 0xff51afd7ed558ccdull; h2[set] ^= h2[set] >> 29;
        }
        if (!c->memo) c->memo = calloc(DESC_MEMO_N, sizeof *c->memo);
        for (uint32_t set = 0; set < p->nset; set++) {
            if (!(dirty & (1u << set))) continue;
            struct desc_memo *slot = NULL; int found = 0;
            if (c->memo) for (uint32_t q = 0; q < 16; q++) {
                struct desc_memo *m = &c->memo[(h1[set] + q) % DESC_MEMO_N];
                if (!m->set) { slot = m; break; }
                if (m->h1 == h1[set] && m->h2 == h2[set] && m->dsl == p->dsl[set]) { c->desc_sets[set] = m->set; found = 1; break; }
            }
            if (found) { hit |= 1u << set; continue; }
            if (pool_dirty >> set & 1) {
                int v = 0;
                if (rec && prs_take(c, p, set, sig[set], pinfo + set, c->desc_sets + set, &v, uidx + set)) { rec_mask |= 1u << set; if (v) valid_mask |= 1u << set; }   // 回收的整池组
                else if (!pool_alloc_set(c, p, set, c->desc_sets + set)) return 0;   // 整池组：单组专用池（和不备忘时同一条路）
            } else if (!alloc_sets(c, p->dsl + set, 1, c->desc_sets + set, pm, set, sig[set])) return 0;
            if (slot && c->memo_n < DESC_MEMO_N * 3 / 4) { slot->h1 = h1[set]; slot->h2 = h2[set]; slot->dsl = p->dsl[set]; slot->set = c->desc_sets[set]; c->memo_n++; }
        }
        uint32_t keep = 0;   // 命中的组不用写：把它们的写入项去掉，其余的换上真的组
        for (uint32_t i = 0; i < nw; i++) {
            const uint32_t set = (uint32_t)((uintptr_t)wr[i].dstSet - 0x5a0);
            if (set >= 4 || (hit >> set & 1)) continue;
            wr[keep] = wr[i]; wr[keep].dstSet = c->desc_sets[set]; keep++;
        }
        nw = keep;
        if (tm) __atomic_fetch_add(&g_desc_hit, (uint64_t)__builtin_popcount(hit), __ATOMIC_RELAXED);
        if (hit & pool_dirty) __atomic_fetch_add(&g_pool_n[PN_HIT], (uint64_t)__builtin_popcount(hit & pool_dirty), __ATOMIC_RELAXED);
    }
    // 只重写变过的条目（切换 nvk_desc_skip 时整份写）。影子按「组对象 + binding 槽」记：换了组对象就整组作废。
    {
        static int skip_cur = -1; static uint32_t skip_k;
        if (skip_cur < 0 || !(++skip_k & 255)) skip_cur = nvn_plat_flag("nvk_desc_skip", 1) != 0;
        if (skip_cur && !memo) {
            for (uint32_t set = 0; set < 4; set++) {
                if (!(c->desc_sets[set] && (c->dsh_ok[set] == 0 || c->dsh_set[set] != c->desc_sets[set]))) continue;
                memset(c->dsh[set], 0, sizeof c->dsh[set]); c->dsh_set[set] = c->desc_sets[set]; c->dsh_ok[set] = 1;
            }
            uint32_t keep = 0;
            for (uint32_t i = 0; i < nw; i++) {
                uint32_t set = 4;
                for (uint32_t s = 0; s < 4; s++) if (c->desc_sets[s] && wr[i].dstSet == c->desc_sets[s]) { set = s; break; }
                if (set >= 4 || wr[i].dstBinding >= 32) { wr[keep++] = wr[i]; continue; }
                struct desc_shadow *sh = &c->dsh[set][wr[i].dstBinding];
                struct desc_shadow nx; memset(&nx, 0, sizeof nx);
                nx.type = wr[i].descriptorType; nx.binding = (uint16_t)wr[i].dstBinding; nx.elem = (uint16_t)wr[i].dstArrayElement;
                if (wr[i].pImageInfo) { nx.a = (uint64_t)(uintptr_t)wr[i].pImageInfo->imageView; nx.b = (uint64_t)(uintptr_t)wr[i].pImageInfo->sampler; nx.c = (uint64_t)wr[i].pImageInfo->imageLayout; }
                else if (wr[i].pBufferInfo) { nx.a = (uint64_t)(uintptr_t)wr[i].pBufferInfo->buffer; nx.b = wr[i].pBufferInfo->offset; nx.c = wr[i].pBufferInfo->range; }
                else if (wr[i].pTexelBufferView) nx.a = (uint64_t)(uintptr_t)*wr[i].pTexelBufferView;
                // 影子一个 binding 只占一格，元素号也得比：数组里相邻两个元素描述符相同（空元素沿用首个
                // 有效视图、采样器共用首项）时，不比元素就会把后一个元素当「没变」跳过，新分配的组里它从没被写过。
                // 每条写入 descriptorCount 恒为 1（write_binds），所以按元素比就够。
                if (sh->valid && sh->elem == nx.elem && sh->a == nx.a && sh->b == nx.b && sh->c == nx.c && sh->type == nx.type) continue;
                nx.valid = 1; *sh = nx; wr[keep++] = wr[i];
            }
            nw = keep;
        }
    }
    const uint64_t t2 = tm ? now_ns() : 0;
    if (nw) { OC(OC_DWRITE); vkUpdateDescriptorSets(g.d, nw, wr, 0, NULL); }
    if (pool_dirty & ~hit) {   // 命中的整池组已经写好，只写这一笔新建的；回收的组里整池内容已就绪的（valid_mask）也不用写
        const uint32_t full = pool_dirty & ~hit & ~valid_mask;
        if (full && !pool_write_sets(c, p, compute, full, rec_mask)) return 0;   // 回收的组整表写正典（反馈视图改走补丁），其余照旧
        for (uint32_t set = 0; set < 4; set++) if ((rec_mask & ~valid_mask) >> set & 1) prs_mark(c, uidx[set], &pinfo[set]);   // 整表写完才记里面写着哪几张表
        if (rec_mask && plan.n && !prs_apply(c, &plan, rec_mask, uidx)) return 0;   // 反馈快照的补丁
    }
    const uint64_t t3 = tm ? now_ns() : 0;
    c->desc_dirty = 0; c->dyn_dirty = 0;
    if ((dirty || dynd) && p->nset) {
        uint32_t offs[32], no = 0;   // 动态偏移按（组号、组内动态序号）顺序拼成一串；每次绑定都把所有组的偏移一并给出
        if (p->dyn_all) for (uint32_t s = 0; s < p->nset && s < 4; s++) for (uint32_t k = 0; k < p->ndyn[s]; k++) offs[no++] = c->dyn_off[s][k];
        vkCmdBindDescriptorSets(c->cb, compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p->layout, 0, p->nset, c->desc_sets, no, no ? offs : NULL);
        if (tm && !dirty) __atomic_fetch_add(&g_dyn_rebind, 1, __ATOMIC_RELAXED);
    }
    if (tm) { const uint64_t t4 = now_ns();
        __atomic_fetch_add(&g_desc_ns[0], (t1 - t0) + ta, __ATOMIC_RELAXED); __atomic_fetch_add(&g_desc_ns[1], (t2 - t1) - ta, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_desc_ns[2], t3 - t2, __ATOMIC_RELAXED); __atomic_fetch_add(&g_desc_ns[3], t4 - t3, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_desc_nw, nw, __ATOMIC_RELAXED); __atomic_fetch_add(&g_desc_sets, (uint64_t)__builtin_popcount(dirty), __ATOMIC_RELAXED); }
    return 1;
}

// 绘制 / 派发前：把这一笔要采样 / 当存储图像用的图按本段的布局状态惰性迁移过去。
// 开通道时 pass_flush_pending 已经把「附件布局」都转成只读采样布局，所以通道里采样不需要迁移；
// 重叠采样先走快照；同图不重叠的采样仍要共享整图布局，退到 GENERAL。
// 存储图像要 GENERAL 而它现在是只读布局时，也在通道外迁移（见 pass_reopen）。
#include "backend_vulkan_feedback.inc"

static int prepare_bind_layouts(struct nvn_gpu_cmd_s *c, int compute) {
    if (!feedback_prepare(c, compute)) return 0;
    if (c->pool_active && !pool_check_layouts(c, c->pipe, compute)) return 0;
    if (!c->opt) return 1;
    const int st0 = compute ? NVN_GPU_SH_CS : 0, st1 = compute ? NVN_GPU_SH_CS + 1 : 5;
    struct reopen_fix fx[32]; uint32_t nfx = 0;
    for (int st = st0; st < st1; st++)
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            struct nvn_gpu_view_s *v = b->cls == NVN_GPU_RES_TEX ? b->u.view : b->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view : NULL;
            if (v && feedback_view(c, v) != v->view) continue;
            struct nvn_gpu_tex_s *t = bind_tex_of(b);
            if (!t) continue;
            const VkImageLayout cur = lay_cur(c, t);
            if (pass_attached(c, t)) {
                if (cur == VK_IMAGE_LAYOUT_GENERAL) continue;   // 这一段本来就 GENERAL：采样 / 存储都能用
                t->fb_sample = 1;                               // 反馈环：这张图这一段退回 GENERAL（以后当附件一律 GENERAL）
                if (nfx < 32) { fx[nfx].t = t; fx[nfx].want = VK_IMAGE_LAYOUT_GENERAL; nfx++; }
                continue;
            }
            const int storage = b->cls == NVN_GPU_RES_IMAGE;
            const VkImageLayout want = storage ? VK_IMAGE_LAYOUT_GENERAL : sample_layout_for(t);
            if (cur == want || cur == VK_IMAGE_LAYOUT_GENERAL) continue;   // GENERAL 采样 / 存储都合法，不动它
            if (nfx < 32) { fx[nfx].t = t; fx[nfx].want = want; nfx++; }
        }
    if (!nfx) return rc_is(c->sticky, NVN_GPU_OK);
    if (!c->in_pass) {   // 通道外（计算派发）：直接迁
        for (uint32_t i = 0; i < nfx; i++) image_barrier(c, fx[i].t, fx[i].want);
        return rc_is(c->sticky, NVN_GPU_OK);
    }
    if (!pass_reopen(c, fx, nfx)) {
        if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_ORDER, "通道里要改布局（反馈环 / 存储图像），通道又拆不开");
        return 0;
    }
    return rc_is(c->sticky, NVN_GPU_OK);
}

// 只用于诊断（debug.s2a.nvk_diag_draw_mode，缺省 0，玩家包恒为 0）：分清一帧慢在回放这一侧还是 GPU 这一侧。
//   1 = 状态、绑定、描述符照做，只是不发绘制调用（量回放一侧的帧率上限）；
//   2 = 每笔绘制调用发两遍（GPU 光栅量约翻倍，回放一侧几乎不变）。
// 两档画面都必然不对，不要用来判断画面；开关只在第一笔绘制时读一次，之后是缓存值。
static int g_diag_draw_mode = -1;
static inline int diag_draw_mode(void) {
    if (__builtin_expect(g_diag_draw_mode < 0, 0)) g_diag_draw_mode = nvn_plat_flag("nvk_diag_draw_mode", 0);
    return g_diag_draw_mode;
}
#define DIAG_DRAW(call) do { \
    if (__builtin_expect(dm != 1, 1)) { call; if (__builtin_expect(dm == 2, 0)) { call; } } \
} while (0)

void nvn_gpu_draw(nvn_gpu_cmd ch, const nvn_gpu_draw_args *a) {
    draw_tm_init();   // 诊断：这笔绘制各段用时（nvk_gputime 开着才计）
    const int dtm = g_draw_tm; const uint64_t dt0 = dtm ? now_ns() : 0;
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass || !c->pipe || !a) { set_sticky(c, NVN_GPU_E_ORDER, "draw 缺通道 / 管线 / 参数"); return; }
    if (c->tsq && c->ntsp < TS_PASS_MAX) c->tsp[c->ntsp].draws++;
    if (a->cnt) {
        if (!(g.caps.caps & NVN_GPU_CAP_INDIRECT_COUNT)) {
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_indirect_count：设备未启用 KHR count / multiDrawIndirect / firstInstance"); return;
        }
        uint32_t size = a->ibuf ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
        uint32_t stride = a->stride ? a->stride : size;
        uint64_t span = a->max_draws ? (uint64_t)(a->max_draws - 1) * stride + size : 0;
        if (!a->ind || (a->cnt_off & 3) || a->cnt_off > a->cnt->bytes || a->cnt->bytes - a->cnt_off < 4 ||
            (a->ind_off & 3) || (stride & 3) || stride < size ||
            a->max_draws > g.dev->properties.limits.maxDrawIndirectCount ||
            a->ind_off >= a->ind->bytes || span > a->ind->bytes - a->ind_off ||
            (a->ibuf && (a->itype > 2 || a->ioff >= a->ibuf->bytes ||
                         (a->ioff & ((1u << a->itype) - 1))))) {
            set_sticky(c, NVN_GPU_E_ARG, "draw_indirect_count：偏移、步长、最大跨度或设备上限非法"); return;
        }
        // GPU-produced count/commands are authoritative. The producer must obey Vulkan's device limit.
        cmd_ref(c, a->cnt, REF_BUF);
        if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    }
    if (a->prim != 0x0 && a->prim != 0x1 && a->prim != 0x3 && a->prim != 0x4 && a->prim != 0x5 && a->prim != 0x6) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "图元 %u 未实现（线环 / quads 等）", a->prim); return;
    }
    struct nvn_gpu_pipe_s *p = c->pipe;
    if (a->prim != p->key.prim) {
        set_sticky(c, NVN_GPU_E_ARG, "draw prim %u 与管线拓扑 %u 不一致", a->prim, p->key.prim);
        return;
    }
    VkRect2D sc = {{0, 0}, {c->pass_w, c->pass_h}};
    if (c->has_dyn && c->dyn.has_sc)
        sc = (VkRect2D){{c->dyn.sc[0], c->dyn.sc[1]}, {(uint32_t)c->dyn.sc[2], (uint32_t)c->dyn.sc[3]}};
    if (!pass_ensure_area(c, sc)) return;
    if (!prepare_bind_layouts(c, 0)) return;   // 采样 / 存储图的布局先到位（可能拆通道），再写描述符
    if (!bind_descriptor_groups(c, p, 0)) return;
    const uint64_t dt1 = dtm ? now_ns() : 0;
    // 以下各项只在和上次真正录进去的不同时才录（#708；影子见 nvn_gpu_cmd_s.es）
    for (int st = 0; st < 5; st++)
        if (c->push_have[st] && p->stage_push_bytes[st]) {
            if (c->es.push_ok[st] && c->es.push_layout[st] == p->layout && c->es.push_off[st] == p->stage_push_off[st] &&
                c->es.push_bytes[st] == p->stage_push_bytes[st] && c->es.push_gen[st] == c->push_gen[st]) continue;
            vkCmdPushConstants(c->cb, p->layout, vk_shader_stage((nvn_gpu_stage)st),
                               p->stage_push_off[st], p->stage_push_bytes[st], c->push_data[st] + p->stage_push_off[st]);
            c->es.push_ok[st] = 1; c->es.push_layout[st] = p->layout; c->es.push_off[st] = p->stage_push_off[st];
            c->es.push_bytes[st] = p->stage_push_bytes[st]; c->es.push_gen[st] = c->push_gen[st]; c->es_count[ES_PUSH]++;
        }
    if (c->has_dyn && (c->dyn.origin_ll != 0) != (p->key.origin_ll != 0)) { set_sticky(c, NVN_GPU_E_ARG, "绘制时的窗口原点（左下 = %d）与管线描述里的不一致", (int)c->dyn.origin_ll); return; }
    // 换游戏管线不动动态状态：所有游戏管线声明的动态状态是同一组（见建管线处的 dyn[7]），按规范切换时保留；
    // 只声明了一部分的管线（矩形清除的）用过之后整份影子作废（nvn_gpu_clear_rect）。
    if (!c->es.gpipe || c->es.gpipe != p->pipe) { vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe); c->es.gpipe = p->pipe; c->es_count[ES_PIPE]++; }
    VkViewport vp;
    if (c->has_dyn && c->dyn.nvp) {
        vp = (VkViewport){c->dyn.vp[0][0], c->dyn.vp[0][1], c->dyn.vp[0][2], c->dyn.vp[0][3], c->dyn.vp[0][4], c->dyn.vp[0][5]};
    } else {
        vp = (VkViewport){0, 0, (float)c->pass_w, (float)c->pass_h, 0.0f, 1.0f};
    }
    // NVN 窗口原点在左上时，裁剪空间 +y 落在图的上方（行号小的一边）；Vulkan 的 +y 朝下（行号大的一边）：用负高度的视口翻过来。
    // 原点在左下（NVN 默认）时两边的行号方向一致，不翻；那种图是自下而上存的，由呈现时翻（present_desc.flip_y）。
    if (c->has_dyn && !c->dyn.origin_ll) { vp.y += vp.height; vp.height = -vp.height; }
    // 浮点按位比（memcmp）：-0 与 +0、NaN 各按自己的位，宁可多发不漏发
    if (!c->es.vp_ok || memcmp(&c->es.vp, &vp, sizeof vp)) { vkCmdSetViewport(c->cb, 0, 1, &vp); c->es.vp = vp; c->es.vp_ok = 1; c->es_count[ES_VP]++; }
    if (!c->es.sc_ok || memcmp(&c->es.sc, &sc, sizeof sc)) { vkCmdSetScissor(c->cb, 0, 1, &sc); c->es.sc = sc; c->es.sc_ok = 1; c->es_count[ES_SC]++; }
    const float bc[4] = {c->dyn.blend_color[0], c->dyn.blend_color[1], c->dyn.blend_color[2], c->dyn.blend_color[3]};
    if (!c->es.bc_ok || memcmp(c->es.bc, bc, sizeof bc)) { vkCmdSetBlendConstants(c->cb, bc); memcpy(c->es.bc, bc, sizeof bc); c->es.bc_ok = 1; c->es_count[ES_BC]++; }
    // 模板的三个动态量：下标 0 = 正面、1 = 背面（NVN 的面位）。没发过动态状态时用 NVN 缺省（掩码全开、参考值 0）
    {
        uint32_t stv[2][3];
        for (int f = 0; f < 2; f++) {
            stv[f][0] = c->has_dyn ? c->dyn.st_vmask[f] & 0xffu : 0xffu;
            stv[f][1] = c->has_dyn ? c->dyn.st_wmask[f] & 0xffu : 0xffu;
            stv[f][2] = c->has_dyn ? c->dyn.st_ref[f] & 0xffu : 0u;
        }
        if (!c->es.st_ok || memcmp(c->es.st, stv, sizeof stv)) {
            for (int f = 0; f < 2; f++) {
                VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
                vkCmdSetStencilCompareMask(c->cb, face, stv[f][0]);
                vkCmdSetStencilWriteMask(c->cb, face, stv[f][1]);
                vkCmdSetStencilReference(c->cb, face, stv[f][2]);
            }
            memcpy(c->es.st, stv, sizeof stv); c->es.st_ok = 1; c->es_count[ES_ST]++;
        }
    }
    // 深度偏移（NVN SetPolygonOffsetClamp：factor 乘斜率、units 乘最小可分辨深度差、clamp 限幅）。管线没开偏移时这三个量不起作用，照发 0。
    {
        const p_poly_offset *po = &c->dyn.po;
        int on = c->has_dyn && (p->key.poly.offset_en & (1u << p->key.poly.mode));
        if (on && po->clamp != 0.0f && !g.dev->core_features.depthBiasClamp) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度偏移带限幅（clamp=%g），设备没有 depthBiasClamp", (double)po->clamp); return; }
        const float db[3] = {on ? po->units : 0.0f, on ? po->clamp : 0.0f, on ? po->factor : 0.0f};
        if (!c->es.db_ok || memcmp(c->es.db, db, sizeof db)) { vkCmdSetDepthBias(c->cb, db[0], db[1], db[2]); memcpy(c->es.db, db, sizeof db); c->es.db_ok = 1; c->es_count[ES_DB]++; }
    }
    const uint64_t dt2 = dtm ? now_ns() : 0;
    uint32_t changed_vbuf = 0;
    for (uint32_t s = 0; s < 16; s++) {
        if (!c->vbuf[s].buf) continue;
        VkBuffer vb = c->vbuf[s].buf->buffer;
        VkDeviceSize off = c->vbuf[s].off;
        if ((c->es.vb_ok >> s & 1) && c->es.vb[s] == vb && c->es.vboff[s] == off) continue;
        c->es.vb[s] = vb; c->es.vboff[s] = off; c->es.vb_ok |= (uint16_t)(1u << s);
        changed_vbuf |= 1u << s;
    }
    // 只发送连续的变化槽。位图仅低 16 位，按首槽右移后取反必非零，可数出这一段的连续 1。
    // 两级录制入队时复制数组，保活仍由 nvn_gpu_set_vbuf 按缓冲登记。
    while (changed_vbuf) {
        uint32_t first = (uint32_t)__builtin_ctz(changed_vbuf);
        uint32_t count = g.vbuf_batch ? (uint32_t)__builtin_ctz(~(changed_vbuf >> first)) : 1;
        vkCmdBindVertexBuffers(c->cb, first, count, c->es.vb + first, c->es.vboff + first);
        c->es_count[ES_VB]++;
        changed_vbuf &= ~(((1u << count) - 1) << first);
    }
    uint32_t inst = a->instances ? a->instances : 1;
    uint32_t draws = a->max_draws ? a->max_draws : 1;
    const int dm = diag_draw_mode();
    if (a->ibuf) {
        if (a->itype > 2) { set_sticky(c, NVN_GPU_E_ARG, "索引类型 %u 非法", a->itype); return; }
        VkIndexType it = a->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : a->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        VkBuffer ib = a->ibuf->buffer;
        VkDeviceSize ioff = a->ioff;
        uint32_t first = a->first;
        // getenv 只在 u8 索引这一种（极少数）绘制上读：u16 / u32 的每笔绘制不再付这份开销（#950 的本意），
        //   又不把测试开关冻成首次读到的值（缓存成静态量后 C42 中途设的强制展开不生效）
        const int force_expand = a->itype == 0 && getenv("S2A_VK_FORCE_INDEX_U8_EXPAND") != NULL;
        if (a->itype == 0 && (!g.dev->index_type_uint8_enabled || force_expand)) {
            if (a->ind) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "u8 间接索引无 CPU 展开回退"); return; }
            if (!a->ibuf->mapped || !a->count) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "u8 索引展开需要可映射索引缓冲"); return; }
            if (ioff > a->ibuf->bytes || (uint64_t)first + a->count > (a->ibuf->bytes - ioff)) {
                set_sticky(c, NVN_GPU_E_ARG, "u8 索引展开范围越界（ioff=%llu first=%u count=%u bytes=%llu）",
                           (unsigned long long)ioff, first, a->count, (unsigned long long)a->ibuf->bytes);
                return;
            }
            const uint8_t *src = a->ibuf->mapped + ioff + first;
            uint64_t need = 2ull * a->count;
            nvn_gpu_slice sl;
            nvn_gpu_err ae;
            if (!rc_is(nvn_gpu_arena_alloc(ch, need, 4, &sl, &ae), NVN_GPU_OK)) { set_sticky(c, ae.rc.code, "u8 索引展开暂存：%s", ae.why); return; }
            for (uint32_t i = 0; i < a->count; i++) ((uint16_t *)sl.cpu)[i] = src[i];
            ib = sl.buf->buffer;
            ioff = sl.off;
            it = VK_INDEX_TYPE_UINT16;
            first = 0;
        }
        {
            uint64_t isz = a->itype == 0 ? 1 : a->itype == 1 ? 2 : 4;
            // Native count reads firstIndex/indexCount from GPU commands, not stale direct-draw fields.
            if (!a->cnt && (a->itype != 0 || (g.dev->index_type_uint8_enabled && !force_expand))) {
                if (ioff > a->ibuf->bytes || (uint64_t)first + a->count > (a->ibuf->bytes - ioff) / isz) {
                    set_sticky(c, NVN_GPU_E_ARG, "索引范围越界（ioff=%llu first=%u count=%u bytes=%llu）",
                               (unsigned long long)ioff, first, a->count, (unsigned long long)a->ibuf->bytes);
                    return;
                }
            }
        }
        cmd_ref(c, a->ibuf, REF_BUF);
        if (a->cnt && !rc_is(c->sticky, NVN_GPU_OK)) return;
        if (!c->es.ib_ok || c->es.ib != ib || c->es.ioff != ioff || c->es.it != it) {
            vkCmdBindIndexBuffer(c->cb, ib, ioff, it);
            c->es.ib = ib; c->es.ioff = ioff; c->es.it = it; c->es.ib_ok = 1; c->es_count[ES_IB]++;
        }
        if (a->ind) {
            cmd_ref(c, a->ind, REF_BUF);
            if (a->cnt && !rc_is(c->sticky, NVN_GPU_OK)) return;
            uint32_t stride = a->stride ? a->stride : (uint32_t)sizeof(VkDrawIndexedIndirectCommand);
            if (a->cnt) {
                DIAG_DRAW(vkCmdDrawIndexedIndirectCount(c->cb, a->ind->buffer, a->ind_off, a->cnt->buffer, a->cnt_off, a->max_draws, stride));
            } else if (draws > 1) {
                for (uint32_t i = 0; i < draws; i++)
                    DIAG_DRAW(vkCmdDrawIndexedIndirect(c->cb, a->ind->buffer, a->ind_off + (uint64_t)i * stride, 1, stride));
            } else {
                DIAG_DRAW(vkCmdDrawIndexedIndirect(c->cb, a->ind->buffer, a->ind_off, 1, stride));
            }
        } else {
            DIAG_DRAW(vkCmdDrawIndexed(c->cb, a->count, inst, first, a->base_vertex, a->base_instance));
        }
    } else if (a->ind) {
        cmd_ref(c, a->ind, REF_BUF);
        if (a->cnt && !rc_is(c->sticky, NVN_GPU_OK)) return;
        uint32_t stride = a->stride ? a->stride : (uint32_t)sizeof(VkDrawIndirectCommand);
        if (a->cnt) {
            DIAG_DRAW(vkCmdDrawIndirectCount(c->cb, a->ind->buffer, a->ind_off, a->cnt->buffer, a->cnt_off, a->max_draws, stride));
        } else if (draws > 1) {
            for (uint32_t i = 0; i < draws; i++)
                DIAG_DRAW(vkCmdDrawIndirect(c->cb, a->ind->buffer, a->ind_off + (uint64_t)i * stride, 1, stride));
        } else {
            DIAG_DRAW(vkCmdDrawIndirect(c->cb, a->ind->buffer, a->ind_off, 1, stride));
        }
    } else {
        DIAG_DRAW(vkCmdDraw(c->cb, a->count, inst, a->first, a->base_instance));
    }
    if (dm != 1 && (a->ind ? !a->cnt || a->max_draws : a->count)) loss_note_program(c, p);
    if (dtm) { const uint64_t dt3 = now_ns(); __atomic_fetch_add(&g_draw_ns[0], dt1 - dt0, __ATOMIC_RELAXED); __atomic_fetch_add(&g_draw_ns[1], dt2 - dt1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_draw_ns[2], dt3 - dt2, __ATOMIC_RELAXED);
        // 每 5 秒一次：把后端逐笔的三段（描述符 / 状态 / 顶点索引与绘制调用）摊到每笔上打印，
        // 找到后端这笔 5 微秒里到底是谁（真机读数见坑记录续 27）。
        static uint64_t w_t0; static uint64_t w_n;
        static uint64_t l_draw[3], l_desc[4], l_dnw, l_dsets;   // 这个读者上次读到的累加器值（不清零共享累加器，见 diag_delta）
        w_n++;
        const uint64_t now = dt3;
        if (!w_t0) w_t0 = now;
        if (now - w_t0 >= __atomic_load_n(&g_draw_log_ns, __ATOMIC_RELAXED)) {
            const uint64_t a0 = diag_delta(&g_draw_ns[0], &l_draw[0]);
            const uint64_t a1 = diag_delta(&g_draw_ns[1], &l_draw[1]);
            const uint64_t a2 = diag_delta(&g_draw_ns[2], &l_draw[2]);
            const uint64_t d0 = diag_delta(&g_desc_ns[0], &l_desc[0]);
            const uint64_t d1 = diag_delta(&g_desc_ns[1], &l_desc[1]);
            const uint64_t d2 = diag_delta(&g_desc_ns[2], &l_desc[2]);
            const uint64_t d3 = diag_delta(&g_desc_ns[3], &l_desc[3]);
            const uint64_t dnw = diag_delta(&g_desc_nw, &l_dnw);
            const uint64_t dsets = diag_delta(&g_desc_sets, &l_dsets);
            char b2[320];
            snprintf(b2, sizeof b2, "后端逐笔（微秒/笔，共 %llu 笔）：入口 %.2f（描述符准备 %.2f / 更新 %.2f / 绑定 %.2f / 收尾 %.2f）、状态 %.2f、顶点索引与绘制 %.2f；描述符更新 %.2f 笔/笔（%.1f 套）",
                     (unsigned long long)w_n,
                     (double)(a0 + a1 + a2) / 1000.0 / (double)w_n, (double)d0 / 1000.0 / (double)w_n, (double)d1 / 1000.0 / (double)w_n,
                     (double)d2 / 1000.0 / (double)w_n, (double)d3 / 1000.0 / (double)w_n,
                     (double)a1 / 1000.0 / (double)w_n, (double)a2 / 1000.0 / (double)w_n,
                     (double)dnw / (double)w_n, (double)dsets / (double)w_n);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b2);
            w_t0 = now; w_n = 0;
        }
    }
    if (g.cold_enabled && dm != 1 && (a->ind ? !a->cnt || a->max_draws : a->count))
        for (unsigned stage = 0; stage < NVN_GPU_SH_CS; stage++)
            cold_track_shader_writes(c, p, stage, COLD_READ_GRAPHICS);
}

// 本段未实现的区域：一律粘滞 UNSUPPORTED / 返回 UNSUPPORTED，不空实现。
#define VK_NOT_YET(c, name) set_sticky((c), NVN_GPU_E_UNSUPPORTED, name "：D8 第一段未实现（随后接通道 / 管线）")

static int g_test_no_xfer_bar;   // 测试钩子 nvn_gpu_vk_test_xfer_barrier 置（文件末）
void nvn_gpu_dispatch(nvn_gpu_cmd ch, nvn_gpu_pipe ph, uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!ind && (!x || !y || !z)) return;   // 0 组：空操作，不录、不置错（对齐 Ryujinx）
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "dispatch 在通道内"); return; }
    es_reset(c);   // push 常量的范围按管线布局、不分绑定点：保守起见计算派发之后图形这边全部重发（#708）
    if (ph && c->pipe && ph != c->pipe) { set_sticky(c, NVN_GPU_E_ARG, "dispatch 管线与 bind 时的管线不一致"); return; }
    struct nvn_gpu_pipe_s *p = c->pipe;
    if (!p || p->pass) { set_sticky(c, NVN_GPU_E_ORDER, "dispatch 缺计算管线"); return; }
    if (ind && x | y | z) { set_sticky(c, NVN_GPU_E_ARG, "间接 dispatch 与计数不能同时给"); return; }
    if (ind) { if (!buf_range(ind, off, 12)) { set_sticky(c, NVN_GPU_E_ARG, "间接 dispatch 参数越界"); return; } }
    c->pipe = p;
    if (!prepare_bind_layouts(c, 1)) return;
    if (!bind_descriptor_groups(c, p, 1)) return;
    if (c->push_have[NVN_GPU_SH_CS] && p->stage_push_bytes[NVN_GPU_SH_CS])
        vkCmdPushConstants(c->cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           p->stage_push_off[NVN_GPU_SH_CS], p->stage_push_bytes[NVN_GPU_SH_CS],
                           c->push_data[NVN_GPU_SH_CS] + p->stage_push_off[NVN_GPU_SH_CS]);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    // 之前录过缓冲作目标的传输（核心的池镜像同步、缓冲写 / 填 / 拷贝、贴图→缓冲）：契约说之后的读看到新值（nvn_backend.h buf_write），
    // 传输自己只在开头带屏障，这里补一道再派发；连续派发之间不加（NVN 不保证无屏障的派发之间的顺序，游戏屏障另走 nvn_gpu_barrier）
    if (c->xfer_dirty && !g_test_no_xfer_bar) full_barrier(c);
    if (ind) {
        vkCmdDispatchIndirect(c->cb, ind->buffer, off);
        cmd_ref(c, ind, REF_BUF);
    } else {
        vkCmdDispatch(c->cb, x, y, z);
    }
    loss_note_program(c, p);
    if (g.cold_enabled) cold_track_shader_writes(c, p, NVN_GPU_SH_CS, COLD_READ_COMPUTE);
}
// ---------------------------------------------------------------- 贴图 / 视图 / 采样器

static uint32_t fmt_bpp(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB: return 1;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:   // 打包成一个 32 位字的格式
        return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;   // 压缩 / 打包格式先不算行宽：调用方按 E_UNSUPPORTED 处理
    }
}

// 图像有的面：纯模板 S8_UINT 只有模板面（#1581 O：原先一律带深度面，S8 的视图取成 DEPTH）
static VkImageAspectFlags fmt_aspect(VkFormat f) {
    if (!nvn_vk_format_is_depth(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return (nvn_vk_format_has_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (nvn_vk_format_has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static VkPipelineStageFlags layout_dst_stage(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:   // 通道开头的深度载入与颜色输出同序（任务约定）
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        // 采样只发生在着色器阶段；ALL_GRAPHICS 不含 tessellation 那几个可选位，设备没开该特性也不违规
        return VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}
static VkAccessFlags layout_dst_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;   // 载入要读、绘制要写
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}
static VkPipelineStageFlags layout_src_stage(VkImageLayout l) {   // 离开这个布局：等谁写完
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return layout_dst_stage(l);
    case VK_IMAGE_LAYOUT_GENERAL: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    default: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}
static VkAccessFlags layout_src_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_MEMORY_WRITE_BIT;
    default: return 0;
    }
}

// 录一次布局迁移：本段内跟踪，段首一律按 GENERAL 算（见「图像布局」）
static void image_barrier_from(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = (VkImageSubresourceRange){t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    b.srcAccessMask = layout_src_access(from);
    b.dstAccessMask = layout_dst_access(to);
    vkCmdPipelineBarrier(c->cb, layout_src_stage(from), layout_dst_stage(to), 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;   // 只做诊断：真正的「本段当前布局」在 c->lay 里
}

// 这张图当前有没有被绑成采样贴图 / 存储图像：采样描述符里声明的 imageLayout 跟着本段布局走，布局一变这些组就作废
static int tex_bound_for_image(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    for (int st = 0; st < 6; st++)
        for (uint32_t i = 0; i < c->nbinds[st]; i++) if (bind_tex_of(&c->binds[st][i].b) == t) return 1;
    return 0;
}

// 记录线程单线程编码（契约 [提交]）；布局按「本段内」跟踪：段首一律 GENERAL，段末转回 GENERAL。
static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout to) {
    struct cmd_lay *e = lay_get(c, t, 0);
    VkImageLayout from = e ? e->cur : VK_IMAGE_LAYOUT_GENERAL;
    if (from == to) return;   // 不用迁：不占表、不录屏障（旧行为下一切本来就都在 GENERAL）
    if (!e) { e = lay_get(c, t, 1); if (!e) { set_sticky(c, NVN_GPU_E_OOM, "布局跟踪表分配失败"); return; } }
    image_barrier_from(c, t, from, to);
    e->cur = to;
    if (c->opt && tex_bound_for_image(c, t)) c->desc_dirty = 0xfu;   // 采样描述符里的 imageLayout 作废，下一笔全重写
}

// 段末：本段动过的图一律转回 GENERAL（段与段之间的静止布局约定，见「图像布局」；只转真的改过的）
static void lay_rest_at_commit(struct nvn_gpu_cmd_s *c) {
    for (uint32_t i = 0; i < c->nlay; i++) {
        if (c->lay[i].cur == VK_IMAGE_LAYOUT_GENERAL) continue;
        image_barrier_from(c, c->lay[i].t, c->lay[i].cur, VK_IMAGE_LAYOUT_GENERAL);
        c->lay[i].cur = VK_IMAGE_LAYOUT_GENERAL;
    }
}

// 压缩格式（块 > 1）：区域可以按整块给到 mip 边缘之外（小于一块的 mip，例如 2×2，游戏照 NVN 给 4×4），
//   只要没超过边缘所在的那一整块，就裁到边（Vulkan 对压缩格式本来就要求区域到边为止）；超过整块照旧非法。
static void copy_region_block_clamp(const struct nvn_gpu_tex_s *t, uint32_t w, uint32_t h, NVNcopyRegion *rr) {
    nvn_fmt_info f;
    if (!nvn_fmt_info_of(t->nvn_format, &f) || (f.bw <= 1 && f.bh <= 1) || rr->xoffset < 0 || rr->yoffset < 0) return;
    uint32_t bw = f.bw > 1 ? f.bw : 1, bh = f.bh > 1 ? f.bh : 1;
    uint32_t aw = (w + bw - 1) / bw * bw, ah = (h + bh - 1) / bh * bh;
    if (rr->width > 0 && (uint32_t)rr->xoffset < w && (uint32_t)rr->xoffset + (uint32_t)rr->width > w &&
        (uint32_t)rr->xoffset + (uint32_t)rr->width <= aw) rr->width = (int)(w - (uint32_t)rr->xoffset);
    if (rr->height > 0 && (uint32_t)rr->yoffset < h && (uint32_t)rr->yoffset + (uint32_t)rr->height > h &&
        (uint32_t)rr->yoffset + (uint32_t)rr->height <= ah) rr->height = (int)(h - (uint32_t)rr->yoffset);
}

static int copy_region(const struct nvn_gpu_tex_s *t, uint32_t level, const NVNcopyRegion *r_in, VkOffset3D *off, VkExtent3D *ext) {
    uint32_t w = t->w >> level, h = t->h >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    NVNcopyRegion clamped; const NVNcopyRegion *r = r_in;
    if (r_in) { clamped = *r_in; copy_region_block_clamp(t, w, h, &clamped); r = &clamped; }
    if (t->target == NVN_TEXTURE_TARGET_3D) {
        if (r && (r->width > 0 || r->height > 0)) {
            uint32_t rz = (uint32_t)(r->depth > 0 ? r->depth : 1);
            if (r->xoffset < 0 || r->yoffset < 0 ||
                (uint32_t)r->xoffset + (uint32_t)r->width > w ||
                (uint32_t)r->yoffset + (uint32_t)r->height > h) return -1;
            *off = (VkOffset3D){r->xoffset, r->yoffset, r->zoffset};
            *ext = (VkExtent3D){(uint32_t)r->width, (uint32_t)r->height, rz};
        } else {
            *off = (VkOffset3D){0, 0, r ? r->zoffset : 0};
            *ext = (VkExtent3D){w, h, (uint32_t)(r && r->depth > 0 ? (uint32_t)r->depth : d)};
        }
        // 两个宽高分支都必须落在当前 mip 的深度内；减法避免区间末端溢出。
        if (off->z < 0 || (uint32_t)off->z >= d || ext->depth > d - (uint32_t)off->z) return -1;
        return 0;
    }
    if (r && r->width > 0 && r->height > 0) {
        if (r->xoffset < 0 || r->yoffset < 0 || (uint32_t)r->xoffset + (uint32_t)r->width > w ||
            (uint32_t)r->yoffset + (uint32_t)r->height > h || r->zoffset != 0 || r->depth > 1)
            return -1;
        *off = (VkOffset3D){r->xoffset, r->yoffset, 0};
        *ext = (VkExtent3D){(uint32_t)r->width, (uint32_t)r->height, 1};
    } else {
        *off = (VkOffset3D){0, 0, 0};
        *ext = (VkExtent3D){w, h, 1};
    }
    return 0;
}

static int check_copy_params(const struct nvn_gpu_tex_s *t, uint32_t level, const NVNcopyRegion *r,
                             uint64_t row_bytes, uint64_t image_bytes, uint32_t bpp,
                             VkOffset3D *off, VkExtent3D *ext) {
    if (!t || level >= t->levels) return -1;
    if (copy_region(t, level, r, off, ext)) return -1;
    if (!row_bytes || row_bytes < (uint64_t)ext->width * bpp) return -1;
    if (!image_bytes || image_bytes < row_bytes * ext->height) return -1;
    return 0;
}

// 建贴图（vkCreateImage + 要显存 + 建默认视图）在真机上单张 0.3–2 毫秒；走路/进新场景时会成批出现，
// 所以超过 3 毫秒的单独记一行，用来和「长帧」对时间（见坑记录续 34）。
// 保住 Adreno 的 UBWC 帧缓冲压缩：图带自由 MUTABLE_FORMAT 时 Adreno 会对它关掉 UBWC，渲染目标的带宽与 GPU 时间都上去
//   （外部参考 nx2android gfx_vk_tex.c ubwc_mode，MIT：惊奇世界地图 Adreno 830 GPU 每帧 11.8 → 8.7 ms，另一款 -35~40%）。
//   回放核心给每张贴图都要 NVN_GPU_TEX_VIEW_FMT（同一份存储会按别的格式看），所以按用途分三种：
//   · 存储用途（原始位读写要跨类格式，且这类图本来难有 UBWC）/ BC 解码 / 采样格式与本格式不同：照旧自由 MUTABLE；
//   · 深度 / 模板图：不开 MUTABLE（深度视图只能取本格式）；
//   · 其余颜色图：MUTABLE + VkImageFormatListCreateInfo 只列本格式、采样格式与 sRGB / UNORM 孪生（VK_KHR_image_format_list，见 vk_device.c）。
//   列表外的视图格式退回本格式建视图、记一次红行并计数（tex_view_fallbacks），不让整笔跳掉。
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// RGBA8 ↔ BGRA8（UNORM / sRGB）：同一块存储按另一种通道顺序看。列表里不放 BGRA（不确定 Adreno 还保不保 UBWC，而 RGBA8 是最常见的渲染目标），
//   用列表里 sRGB 属性相同的那个格式建视图、在通道映射里互换 R / B——采样结果与直接按 BGRA 看逐位相同。返回替代格式，不是这一对返回 UNDEFINED。
static VkFormat rb_swap_stand_in(const struct nvn_gpu_tex_s *t, VkFormat want) {
    const int want_bgra = want == VK_FORMAT_B8G8R8A8_UNORM || want == VK_FORMAT_B8G8R8A8_SRGB;
    const int want_rgba = want == VK_FORMAT_R8G8B8A8_UNORM || want == VK_FORMAT_R8G8B8A8_SRGB;
    if (!want_bgra && !want_rgba) return VK_FORMAT_UNDEFINED;
    const int srgb = want == VK_FORMAT_B8G8R8A8_SRGB || want == VK_FORMAT_R8G8B8A8_SRGB;
    const VkFormat use = want_bgra ? (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM)
                                   : (srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM);
    if (use == t->format || use == t->sample_format) return use;
    for (unsigned i = 0; i < t->fmt_n; i++) if (t->fmt_list[i] == use) return use;
    return VK_FORMAT_UNDEFINED;
}
static uint64_t g_tex_view_fallbacks;   // 列表模式下视图格式不在列表、退回本格式的次数（验证读数）
uint64_t nvn_vk_tex_view_fallbacks(void) { return g_tex_view_fallbacks; }
int nvn_vk_tex_fmt_list_active(void) { return g.dev && g.dev->image_format_list_enabled; }   // 测试用：本设备是否走格式列表模式
static int tex_fmt_allowed(const struct nvn_gpu_tex_s *t, VkFormat f) {
    if (f == t->sample_format || f == t->format || t->view_fmt) return 1;
    for (unsigned i = 0; i < t->fmt_n; i++) if (t->fmt_list[i] == f) return 1;
    return 0;
}

nvn_gpu_rc nvn_gpu_tex_create(const nvn_gpu_tex_desc *d, nvn_gpu_tex *out, nvn_gpu_err *err) {
    const uint64_t tc_t0 = now_ns();
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (!d) return fail(err, NVN_GPU_E_ARG, "描述为空");
    if (d->host_w || d->host_h) return fail(err, NVN_GPU_E_UNSUPPORTED, "宽屏宿主尺寸（host_w/h）：本段未实现");
    uint32_t target = (uint32_t)d->d.target;
    uint32_t depth = d->d.depth ? (uint32_t)d->d.depth : 1;
    uint32_t layers = 1;
    VkImageType itype = VK_IMAGE_TYPE_2D;
    VkImageViewType vtype = VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateFlags iflags = 0;
    int array_compat = 0;
    switch (target) {
    case NVN_TEXTURE_TARGET_2D:
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: break;
    case NVN_TEXTURE_TARGET_2D_ARRAY:
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
        layers = depth; vtype = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case NVN_TEXTURE_TARGET_3D:
        itype = VK_IMAGE_TYPE_3D; vtype = VK_IMAGE_VIEW_TYPE_3D;
        array_compat = 1;
        iflags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        break;
    case NVN_TEXTURE_TARGET_CUBEMAP:
        layers = 6; vtype = VK_IMAGE_VIEW_TYPE_CUBE;
        iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
        if (!g.dev->core_features.imageCubeArray)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "立方体数组需要 imageCubeArray 设备特性（未启用）");
        if (!depth || depth % 6) return fail(err, NVN_GPU_E_ARG, "立方体数组 depth=%u 必须是 6 的倍数", depth);
        layers = depth; vtype = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    default:
        return fail(err, NVN_GPU_E_UNSUPPORTED, "target %u：本段只实现 2D / 2D 数组 / 立方体 / 立方体数组 / 3D", target);
    }
    VkFormat fmt = nvn_vk_tex_format((uint32_t)d->d.format);
    if (fmt == VK_FORMAT_UNDEFINED) return fail(err, NVN_GPU_E_UNSUPPORTED, "NVN 格式 0x%x 无 Vulkan 映射", d->d.format);
    int bc_decoded = d->d.format >= 0x41 && d->d.format <= 0x50 &&
                     ((g.dev->bc_decode_mask >> ((uint32_t)d->d.format - 0x41)) & 1u);
    VkFormat sample_fmt = fmt;
    // 各 BC 路线都按 z 片解码到私有 2D 图；拷到 3D 同格式存储需要 Vulkan 1.1 的 maintenance1 语义。
    const int bc_3d = itype == VK_IMAGE_TYPE_3D && s2a_vk_effective_api(g.dev->properties.apiVersion) >= VK_API_VERSION_1_1;
    if (bc_decoded && (!nvn_vk_bcdec_route((uint32_t)d->d.format, NULL, &sample_fmt, &fmt, NULL, NULL) ||
                       (itype != VK_IMAGE_TYPE_2D && !bc_3d) ||
                       (d->usage & (NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE))))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "BC format 0x%x decode: 不支持此用途或维度", d->d.format);
    if (!d->d.width || !d->d.height) return fail(err, NVN_GPU_E_ARG, "尺寸为 0");
    {   // #975：走退回格式的 16 位打包格式写进去的分量位置不对（通道映射只管采样），渲染 / 存储用途具名拒绝
        uint8_t map[4];
        if ((d->usage & (NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE)) && nvn_vk_format_chanmap((uint32_t)d->d.format, map))
            return fail(err, NVN_GPU_E_UNSUPPORTED, "16 位打包格式 0x%x：本设备没有位序对得上的 Vulkan 格式（走退回格式 + 采样通道补偿），不能当渲染目标 / 存储图像", d->d.format);
    }
    uint32_t levels = d->d.levels ? (uint32_t)d->d.levels : 1;
    uint32_t samples = d->d.samples > 1 ? (uint32_t)d->d.samples : 1;
    if (bc_decoded && samples != 1) return fail(err, NVN_GPU_E_UNSUPPORTED, "BC 计算解码不支持多重采样");

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d->usage & NVN_GPU_TEX_SAMPLED) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d->usage & NVN_GPU_TEX_RENDER) {
        usage |= nvn_vk_format_is_depth(fmt) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (d->usage & NVN_GPU_TEX_STORAGE) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (bc_decoded && bc1_astc_storage((uint32_t)d->d.format, d, itype, layers, iflags, &fmt)) sample_fmt = fmt;
    // 可改格式的三种（见 tex_fmt_allowed 上的说明）：any_fmt = 自由 MUTABLE；fmt_list = MUTABLE + 格式列表；都不是 = 不开 MUTABLE
    const int want_view = (d->usage & NVN_GPU_TEX_VIEW_FMT) != 0;
    const int plain = want_view && !bc_decoded && sample_fmt == fmt && !(usage & VK_IMAGE_USAGE_STORAGE_BIT);
    const int depth_fmt = nvn_vk_format_is_depth(fmt);
    const int fmt_list = plain && !depth_fmt && g.dev->image_format_list_enabled;   // 应用 apiVersion 1.1：按启用的扩展判，不按物理设备支持的版本
    const int any_fmt = (want_view && !plain) || (want_view && !depth_fmt && !fmt_list) || sample_fmt != fmt;
    const VkImageCreateFlags mut_flag = (any_fmt || fmt_list) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;

    // 在 vkCreateImage 前按真实格式/usage 查询样本数；不把非法计数送到驱动或校验层。
    int ms_target = target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
    if (ms_target || samples > 1) {
        if (d->d.samples <= 1 || samples > 64 || (samples & (samples - 1)) || levels != 1 || itype != VK_IMAGE_TYPE_2D ||
            (d->d.samples > 1 && (target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)))
            return fail(err, NVN_GPU_E_UNSUPPORTED, "多重采样计数/级数/目标不支持");
        VkImageFormatProperties ip = {0};
        VkImageCreateFlags flags = iflags | mut_flag;
        if (vkGetPhysicalDeviceImageFormatProperties(g.phys, fmt, itype, VK_IMAGE_TILING_OPTIMAL, usage, flags, &ip) != VK_SUCCESS ||
            (uint32_t)d->d.width > ip.maxExtent.width || (uint32_t)d->d.height > ip.maxExtent.height || layers > ip.maxArrayLayers)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "设备不支持该格式/usage 的 %u 重采样贴图", samples);
        if (!(ip.sampleCounts & samples)) {
            uint32_t actual = samples <= 16 ? msaa_pick(samples, ip.sampleCounts) : 0;
            if (!actual) return fail(err, NVN_GPU_E_UNSUPPORTED, "设备不支持该格式/usage 的 %u 重采样贴图", samples);
            samples = actual;
        }
        if (samples > 1 && (usage & VK_IMAGE_USAGE_STORAGE_BIT) && !g.dev->core_features.shaderStorageImageMultisample)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "多重采样存储图像特性未启用");
    }

    // P0-6：方形、≥6 层的二维数组可能被着色器当立方体读（同 iOS tex_fit 对二维数组底图建立方体视图）。Vulkan 的立方体视图要求
    // 建图时带 CUBE_COMPATIBLE（宽 = 高、≥6 层）；先问设备这个格式 / 用途收不收这个标志，不收就不加（立方体视图照旧被拒、核心记账）。
    // 存储用途配上本身不能当存储的格式（核心按 sRGB 别名要来的，见 NVN_GPU_CAP_STORAGE_SRGB_ALIAS）：建图带 EXTENDED_USAGE，
    //   由各视图的格式判能不能存储（view_usage_next）。EXTENDED_USAGE 要求可改格式，核心建图一律带 VIEW_FMT。
    int ext_usage = 0;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && (d->usage & NVN_GPU_TEX_VIEW_FMT)) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g.phys, fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { iflags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; ext_usage = 1; }
    }
    int cube_compat = 0;
    if (target == NVN_TEXTURE_TARGET_2D_ARRAY && layers >= 6 && d->d.width == d->d.height && samples == 1 && !bc_decoded) {
        VkImageFormatProperties ip = {0};
        VkImageCreateFlags flags = iflags | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | mut_flag;
        if (vkGetPhysicalDeviceImageFormatProperties(g.phys, fmt, itype, VK_IMAGE_TILING_OPTIMAL, usage, flags, &ip) == VK_SUCCESS &&
            layers <= ip.maxArrayLayers && (uint32_t)d->d.width <= ip.maxExtent.width && levels <= ip.maxMipLevels) {
            iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; cube_compat = 1;
        }
    }

    struct nvn_gpu_tex_s *t = calloc(1, sizeof *t);
    if (!t) return fail(err, NVN_GPU_E_OOM, "贴图结构分配失败");
    t->format = fmt;
    t->sample_format = sample_fmt;
    t->bc_decoded = bc_decoded;
    t->original_usage = d->usage;
    t->aspect = fmt_aspect(fmt);
    t->original_desc = d->d;
    t->w = d->d.width;
    t->h = d->d.height;
    t->levels = levels;
    t->layers = layers;
    t->depth = depth;
    t->target = target;
    t->array_compatible = array_compat;
    t->cube_compatible = cube_compat;
    t->samples = samples;
    t->nvn_format = (uint32_t)d->d.format;
    t->view_fmt = any_fmt;
    if (fmt_list) {
        t->fmt_list[t->fmt_n++] = fmt;
        VkFormat tw = srgb_twin(fmt);
        if (tw != VK_FORMAT_UNDEFINED) t->fmt_list[t->fmt_n++] = tw;
    }
    t->ext_usage = (uint8_t)ext_usage;
    t->swz = d->swz;
    t->layout = VK_IMAGE_LAYOUT_GENERAL;   // 实际转换在创建后的第一次提交里（待转表）

    VkImageCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = itype;
    ci.format = fmt;
    ci.extent = (VkExtent3D){d->d.width, d->d.height, itype == VK_IMAGE_TYPE_3D ? depth : 1};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.flags = iflags;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.flags |= mut_flag;
    VkImageFormatListCreateInfo fl;
    if (t->fmt_n) {
        memset(&fl, 0, sizeof fl);
        fl.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        fl.viewFormatCount = t->fmt_n; fl.pViewFormats = t->fmt_list;
        fl.pNext = ci.pNext; ci.pNext = &fl;
    }
    LIFE_T0;
    t->usage = ci.usage;
    if (vkCreateImage(g.d, &ci, NULL, &t->image) != VK_SUCCESS) {
        free(t);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateImage 拒绝（格式 0x%x / usage 0x%x / %ux%u）", d->d.format, usage, d->d.width, d->d.height);
    }
    LIFE_ADD(0);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.d, t->image, &req);
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, 0, NULL);
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    t->mem_size = req.size; t->mem_type = type;
    if (type != UINT32_MAX && texmem_on()) t->memory = texmem_take(req.size, type, &t->mem_size);
    if (type == UINT32_MAX || (!t->memory && vkAllocateMemory(g.d, &ai, NULL, &t->memory) != VK_SUCCESS) ||
        vkBindImageMemory(g.d, t->image, t->memory, 0) != VK_SUCCESS) {
        if (t->memory) vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "贴图显存分配失败（%ux%u，%u 级）", d->d.width, d->d.height, levels);
    }
    LIFE_ADD(1);
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = vtype;
    vi.format = sample_fmt;
    VkImageViewUsageCreateInfo vu; vi.pNext = view_usage_next(t, sample_fmt, &vu);
    vi.subresourceRange = (VkImageSubresourceRange){(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect, 0, levels, 0, itype == VK_IMAGE_TYPE_3D ? 1 : layers};   // 深度模板格式：采样只取深度面
    if (vkCreateImageView(g.d, &vi, NULL, &t->view) != VK_SUCCESS) {
        vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "默认贴图视图创建失败");
    }
    LIFE_ADD(2);
    pthread_mutex_lock(&g_mu);
    if (!newimg_add_locked(t)) {
        pthread_mutex_unlock(&g_mu);
        pc_drop_view(t->view);
        vkDestroyImageView(g.d, t->view, NULL);
        vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "待转布局表分配失败");
    }
    t->all_next = g.texs;
    g.texs = t;
    pthread_mutex_unlock(&g_mu);
    *out = t;
    if (d->d.samples > 1 && samples != (uint32_t)d->d.samples)
        msaa_notice_once((uint32_t)d->d.format, (uint32_t)d->d.samples, samples);
    { const uint64_t dt = now_ns() - tc_t0;   // 建贴图超过 3 毫秒单独记一行（走路 / 进新场景时会成批出现；nvk_hitch_log 开时才记）
      static int slowt = -1; if (slowt < 0) slowt = nvn_plat_flag("nvk_hitch_log", 0) != 0;
      if (slowt && dt > 3000000ull) { char b[160]; snprintf(b, sizeof b, "建贴图 %.1f 毫秒（%ux%u）", (double)dt / 1e6, d->d.width, d->d.height); nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", b); } }
    return rc(NVN_GPU_OK);
}

void nvn_gpu_tex_info_get(nvn_gpu_tex th, nvn_gpu_tex_info *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    struct nvn_gpu_tex_s *t = th;
    if (!t) return;
    out->w = t->w;
    out->h = t->h;
    out->depth = t->depth;
    out->levels = t->levels;
    out->layers = t->layers;
    out->samples = t->samples;
    out->nvn_format = t->nvn_format;
    out->fmt_flags = nvn_gpu_format_caps(t->nvn_format);
}

void nvn_gpu_tex_release(nvn_gpu_tex th) {
    struct nvn_gpu_tex_s *t = th;
    if (!t) return;
    pthread_mutex_lock(&g_mu);
    if (!t->released) {
        t->released = 1;
        rel_epoch_bump();
        if (t->refs == 0) destroy_tex_locked(&g, t);
    }
    pthread_mutex_unlock(&g_mu);
}

// NVN 里第 4 通道读作 1 的格式（RGBX / BGRX / RGB5 / BGR5 / RGB_DXT1）：这些格式的存储没有 alpha，
// 分量 a 由硬件供给常数 1。宿主格式是同位宽的 RGBA（Vulkan 没有 RGBX），直接采样会把 X 位当 alpha 读出来
// （0100F2C0115B6000 的光照合成逐层变暗就是这么来的：片元 alpha 读成 0 附近的垃圾值，目标因子 SRC_ALPHA 把已有光照乘小）。
// 与 Metal 侧同一张表（graphics/nvn/gfx/tex/nvn_fmt_mtl.c 的 noalpha：nvn_fmt_to_mtl 把 a 解析成 ONE）。
static int nvn_fmt_alpha_reads_one(uint32_t f) {
    switch (f) {
    case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_RGB_DXT1: case NVN_FORMAT_RGB_DXT1_SRGB: case NVN_FORMAT_RGB5:
    case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
    case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI: case NVN_FORMAT_RGBX16I:
    case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
    case NVN_FORMAT_BGR5: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
        return 1;
    default: return 0;
    }
}

static int exact_color_view(const struct nvn_gpu_tex_s *t, uint32_t nvn_format, VkFormat fmt) {
    nvn_fmt_info base, view;
    uint8_t map[4];
    // Vulkan 非压缩颜色视图按纹素位数分兼容类；NVN 映射表没有独立类的纯 alpha 或多平面格式。
    return t->samples == 1 && (t->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        nvn_fmt_info_of(t->nvn_format, &base) && nvn_fmt_info_of(nvn_format, &view) &&
        !base.depth && !base.stencil && !view.depth && !view.stencil && !base.expand && !view.expand &&
        base.bw == 1 && base.bh == 1 && view.bw == 1 && view.bh == 1 && base.bytes == view.bytes &&
        !nvn_vk_format_chanmap(t->nvn_format, map) && !nvn_vk_format_chanmap(nvn_format, map) &&
        (nvn_gpu_format_caps(nvn_format) & NVN_GPU_FMT_RENDER) && tex_fmt_allowed(t, fmt);
}

static nvn_gpu_rc view_create(nvn_gpu_tex th, const view_t *v, int color_attachment, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    struct nvn_gpu_tex_s *t = th;
    if (!t) return fail(err, NVN_GPU_E_ARG, "贴图为空");
    uint32_t base_level = 0, levels = t->levels, base_layer = 0, layers = t->layers;
    VkFormat fmt = t->sample_format;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    if (v) {
        if (v->set_levels) { base_level = v->base_level; levels = v->nlevels ? v->nlevels : 1; }
        if (v->set_layers) { base_layer = v->min_layer; layers = v->nlayers ? v->nlayers : 1; }
        if (v->set_format && (uint32_t)v->format != t->nvn_format) {
            if (t->bc_decoded) return fail(err, NVN_GPU_E_UNSUPPORTED, "BC 解码贴图的不同格式视图未实现");
            VkFormat of = nvn_vk_tex_format((uint32_t)v->format);
            if (of == VK_FORMAT_UNDEFINED) return fail(err, NVN_GPU_E_ARG, "视图格式 0x%x 无映射", v->format);
            fmt = of;
        }
    }
    // P0-6：立方体兼容的二维数组按立方体（数组）读。视图决策（与旧层共用）只把立方体目标的图当立方体兼容，这里按立方体数组目标问它；
    // 层数要正好 6（立方体）或 6 的倍数（立方体数组），立方体数组视图还要 imageCubeArray。
    uint32_t plan_target = t->target;
    if (v && v->set_target && (v->target == NVN_TEXTURE_TARGET_CUBEMAP || v->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) &&
        t->target == NVN_TEXTURE_TARGET_2D_ARRAY && t->cube_compatible) {
        uint32_t nl = v->set_layers ? (v->nlayers ? v->nlayers : 1) : t->layers;
        if (v->target == NVN_TEXTURE_TARGET_CUBEMAP ? nl != 6 : (nl % 6 != 0))
            return fail(err, NVN_GPU_E_ARG, "二维数组当立方体%s读：层数 %u 不是 %s", v->target == NVN_TEXTURE_TARGET_CUBEMAP ? "" : "数组", nl,
                        v->target == NVN_TEXTURE_TARGET_CUBEMAP ? "6" : "6 的倍数");
        if (v->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && !g.dev->core_features.imageCubeArray)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "二维数组当立方体数组读需要 imageCubeArray 设备特性（未启用）");
        plan_target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    }
    struct nvn_vk_texview_plan plan;
    nvn_vk_texview_plan_make(plan_target, t->depth, t->layers, t->levels, v, 0, t->array_compatible, &plan);
    if (!plan.ok) return fail(err, NVN_GPU_E_ARG, "%s", plan.why);
    base_level = plan.base_level; levels = plan.nlevels;
    base_layer = plan.base_layer; layers = plan.nlayers; type = plan.type;
    if (color_attachment) {
        const uint32_t want = v && v->set_format ? (uint32_t)v->format : t->nvn_format;
        if (type != VK_IMAGE_VIEW_TYPE_2D || levels != 1 || layers != 1 || !exact_color_view(t, want, fmt))
            return fail(err, NVN_GPU_E_UNSUPPORTED, "颜色附件需要兼容且可直接使用的单级单层格式 0x%x → 0x%x", t->nvn_format, want);
        if (t->target == NVN_TEXTURE_TARGET_3D) {
            uint32_t depth = base_level < 32 ? t->depth >> base_level : 0;
            if (base_layer >= (depth ? depth : 1)) return fail(err, NVN_GPU_E_ARG, "颜色附件切片 %u 超出 mip %u 深度", base_layer, base_level);
        }
    }
    int fmt_fell_back = 0, rb_swap = 0;
    if (!tex_fmt_allowed(t, fmt)) {
        VkFormat sub = rb_swap_stand_in(t, fmt);
        if (sub != VK_FORMAT_UNDEFINED) { fmt = sub; rb_swap = 1; }
    }
    if (!tex_fmt_allowed(t, fmt)) {
        if (!t->fmt_n && !(t->usage & (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) && !nvn_vk_format_is_depth(t->format))
            return fail(err, NVN_GPU_E_UNSUPPORTED, "不同格式的视图需要创建时声明 NVN_GPU_TEX_VIEW_FMT");
        // 格式列表 / 深度图（保 UBWC，见 tex_fmt_allowed）：列表外的格式退回本格式，记一次红行
        if (g_tex_view_fallbacks++ == 0 || (g_tex_view_fallbacks & (g_tex_view_fallbacks - 1)) == 0) {
            char msg[200]; snprintf(msg, sizeof msg, "tex_view_fmt_fallback: 图 %d 的视图要格式 %d（不在保压缩的格式列表里），按图本身格式建视图（第 %llu 次）",
                                    (int)t->format, (int)fmt, (unsigned long long)g_tex_view_fallbacks);
            nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", msg);
        }
        fmt = t->sample_format; fmt_fell_back = 1;
    }

    struct nvn_gpu_view_s *o = calloc(1, sizeof *o);
    if (!o) return fail(err, NVN_GPU_E_OOM, "视图结构分配失败");
    o->nvn_format = v && v->set_format && !fmt_fell_back ? (uint32_t)v->format : t->nvn_format;
    o->format = fmt;
    VkImageViewCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = t->image;
    ci.viewType = type;
    ci.format = fmt;
    // 深度模板格式：采样视图只能取一个面（两个面都取的视图不能绑进描述符）。视图设了深度模板模式按它，否则取深度面
    //（对照 nvn_mtl.m @6602 want_st；存储自带「模板视图」的贴图本后端建不出来，见 tex_create）。当附件时另用附件形态（两个面都在）。
    o->aspect = t->aspect;
    if ((t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        o->aspect = v && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    o->type = type;
    o->att_layer = v && v->set_layers ? v->min_layer : 0;
    ci.subresourceRange = (VkImageSubresourceRange){o->aspect, base_level, levels, base_layer, layers};
    // 通道映射：视图自己设了就用视图的，否则用存储自带的（与 nvn_mtl.m:6546 同序，是取代不是叠加）。
    // NVN 的取值 0 零 / 1 一 / 2 R / 3 G / 4 B / 5 A，其它值按该通道恒等。
    // #975：16 位打包格式走退回格式时，NVN 的分量 c 在 Vulkan 格式里是第 fmap[c] 个分量，再叠在上面这一层之下。
    if (!color_attachment) {
        uint8_t fmap[4];
        o->p16_remap = nvn_vk_format_chanmap(o->nvn_format, fmap);
        if (rb_swap) { uint8_t r = fmap[0]; fmap[0] = fmap[2]; fmap[2] = r; }   // RGBA ↔ BGRA 替代视图（rb_swap_stand_in）：NVN 的 R 取存储的 B
        uint8_t sw[4] = {2, 3, 4, 5};
        if (v && v->set_swizzle) memcpy(sw, v->swz, 4);
        else if (t->swz) for (int ch = 0; ch < 4; ch++) sw[ch] = (uint8_t)(t->swz >> (3 * ch) & 7);
        VkComponentSwizzle *comp[4] = {&ci.components.r, &ci.components.g, &ci.components.b, &ci.components.a};
        int a_one = nvn_fmt_alpha_reads_one(o->nvn_format);   // 没有 alpha 的格式：不管映射到哪一档，读到 a 都是 1
        for (int ch = 0; ch < 4; ch++) {
            if (sw[ch] > 5) sw[ch] = (uint8_t)(2 + ch);
            if (a_one && sw[ch] == 5) sw[ch] = 1;
            if (sw[ch] >= 2) sw[ch] = (uint8_t)(2 + fmap[sw[ch] - 2]);
            *comp[ch] = sw[ch] == 0 ? VK_COMPONENT_SWIZZLE_ZERO : sw[ch] == 1 ? VK_COMPONENT_SWIZZLE_ONE : (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + (sw[ch] - 2));
            if (sw[ch] != 2 + ch) o->swizzled = 1;
        }
        if (!o->swizzled) memset(&ci.components, 0, sizeof ci.components);   // 恒等：留 IDENTITY，附件可用
    }
    VkImageViewUsageCreateInfo vu;
    if (color_attachment) {
        vu = (VkImageViewUsageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        ci.pNext = &vu;  // 重解释只用于附件，不能继承视图格式可能不支持的存储等用途。
    } else ci.pNext = view_usage_next(t, ci.format, &vu);
    OC(OC_VIEW);
    if (vkCreateImageView(g.d, &ci, NULL, &o->view) != VK_SUCCESS) {
        free(o);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateImageView 拒绝（格式 / 范围）");
    }
    o->tex = t;
    o->components = ci.components;
    o->base_level = base_level;
    o->levels = levels;
    o->base_layer = base_layer;
    o->layers = layers;
    gmu_lock_rt();
    t->refs++;   // 视图让贴图活到视图 release
    view_link_locked(&g, o);
    pthread_mutex_unlock(&g_mu);
    *out = o;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex th, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) {
    return view_create(th, v, 0, out, err);
}

nvn_gpu_rc nvn_gpu_view_create_color_attachment(nvn_gpu_tex th, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) {
    return view_create(th, v, 1, out, err);
}

nvn_gpu_rc nvn_gpu_view_create_buf(nvn_gpu_buf b, uint64_t off, uint64_t bytes, uint32_t fmt, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    nvn_fmt_info info;
    VkFormat f = nvn_vk_format(fmt);
    if (f == VK_FORMAT_UNDEFINED || !nvn_fmt_info_of(fmt, &info) ||
        info.bw != 1 || info.bh != 1 || !info.bytes)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "缓冲视图格式 0x%x 不支持", fmt);
    VkFormatProperties fp;
    s2a_vk_format_properties(g.phys, f, &fp);
    VkFormatFeatureFlags features = fp.bufferFeatures &
        (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT);
    uint32_t caps = nvn_gpu_format_caps(fmt);
    // 底层 VkBuffer 同时声明两种 texel 用途；视图必须满足两项格式特性。
    if (features != (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "格式 0x%x 不支持缓冲声明的全部 texel 用途", fmt);
    if (!((features & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) && (caps & NVN_GPU_FMT_SAMPLE)) &&
        !((features & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) && (caps & NVN_GPU_FMT_STORAGE)))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "格式 0x%x 不支持纹素缓冲", fmt);
    uint64_t align = g.dev->properties.limits.minTexelBufferOffsetAlignment;
    if (!bytes || bytes % info.bytes || (align && off % align) ||
        bytes / info.bytes > g.dev->properties.limits.maxTexelBufferElements)
        return fail(err, NVN_GPU_E_ARG, "缓冲视图范围 / 对齐非法");
    struct nvn_gpu_view_s *v = calloc(1, sizeof *v);
    if (!v) return fail(err, NVN_GPU_E_OOM, "缓冲视图结构分配失败");
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_buf_s *found = g.bufs;
    while (found && found != b) found = found->all_next;
    if (g.closing || !found || found->released || !buf_range(found, off, bytes)) {
        int closing = g.closing;
        pthread_mutex_unlock(&g_mu);
        free(v);
        return fail(err, closing ? NVN_GPU_E_STOPPED : NVN_GPU_E_ARG, "缓冲无效或视图范围越界");
    }
    VkBufferViewCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    ci.buffer = found->buffer; ci.format = f; ci.offset = off; ci.range = bytes;
    VkResult result = vkCreateBufferView(g.d, &ci, NULL, &v->buffer_view);
    if (result != VK_SUCCESS) {
        pthread_mutex_unlock(&g_mu);
        free(v);
        return fail(err, result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                         ? NVN_GPU_E_OOM : NVN_GPU_E_UNSUPPORTED, "vkCreateBufferView 失败（%d）", result);
    }
    found->refs++;
    v->buf = found; v->buffer_off = off; v->buffer_bytes = bytes; v->nvn_format = fmt; v->buffer_format = f; v->buffer_features = features; v->buffer_caps = caps;
    view_link_locked(&g, v);
    pthread_mutex_unlock(&g_mu);
    *out = v;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_view_release(nvn_gpu_view vh) {
    struct nvn_gpu_view_s *v = vh;
    if (!v) return;
    pthread_mutex_lock(&g_mu);
    if (!v->released) {
        v->released = 1;
        rel_epoch_bump();
        if (v->refs == 0) destroy_view_locked(&g, v);
    }
    pthread_mutex_unlock(&g_mu);
}

// 环绕模式。镜像夹取三种（#976）：MIRROR_CLAMP_TO_EDGE 是镜像一次再夹到边，要 VK_KHR_sampler_mirror_clamp_to_edge；
// MIRROR_CLAMP / MIRROR_CLAMP_TO_BORDER 在 Vulkan 里没有对应模式，按契约不近似，回具名 UNSUPPORTED，由核心决定怎么记、怎么退。
// 返回 NULL = 已写 *out；否则是不支持的原因（ASCII 名字打头，核心与普查按它归类）。
static const char *samp_wrap(uint8_t w, VkSamplerAddressMode *out) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT; return NULL;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return NULL;
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
        if (!g.dev->sampler_mirror_clamp_enabled) return "sampler_mirror_clamp_to_edge：设备没有启用 VK_KHR_sampler_mirror_clamp_to_edge";
        *out = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; return NULL;
    case NVN_WRAP_MODE_MIRROR_CLAMP: return "sampler_wrap_mirror_clamp：Vulkan 没有镜像后按 CLAMP 夹取的模式";
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return "sampler_wrap_mirror_clamp_to_border：Vulkan 没有镜像后夹到边框色的模式";
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return NULL;
    default: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return NULL;
    }
}

// 边框色（#976）。NVN 给的是任意 RGBA（整数 setter 也按数值存成浮点，hle_nvn.c）。正好是 Vulkan 内置三档之一时用内置值，
// 不占自定义边框色配额；否则要 VK_EXT_custom_border_color。返回 0 透明黑、1 不透明黑、2 不透明白，-1 = 不是三档之一。
static int samp_border_preset(const float b[4]) {
    if (b[0] == 0 && b[1] == 0 && b[2] == 0) return b[3] == 0 ? 0 : b[3] == 1 ? 1 : -1;
    return b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1 ? 2 : -1;
}

// 整数视图用的边框色：按数值取整。有符号视图读 int32；无符号视图读同一组 32 位，超过 INT32_MAX 的无符号值按位存。
static int32_t samp_border_int(float f) {
    const double d = (double)f;
    const int64_t v = d <= (double)INT32_MIN ? INT32_MIN : d >= (double)UINT32_MAX ? (int64_t)UINT32_MAX : (int64_t)(d < 0 ? d - 0.5 : d + 0.5);
    const uint32_t bits = (uint32_t)v;
    int32_t out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

// 没有 VK_EXT_custom_border_color 时的就近替代（降级，不是错误）：具名计数 sampler_border_color_nearest 按采样器建成次数计
//   （进程累计，nvn_gpu_stats 读），每局（开会话起）只打首个一行信息级日志：原色 → 浮点 / 整数各取的内置档。
static void samp_border_nearest_note(const float b[4], int pf, int pi) {
    static const char *const name[3] = {"透明黑", "不透明黑", "不透明白"};
    __atomic_fetch_add(&g_border_nearest, 1, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&g_border_nearest_said, 1, __ATOMIC_RELAXED)) return;
    char line[260];
    snprintf(line, sizeof line, "sampler_border_color_nearest：设备没有 VK_EXT_custom_border_color，边框色 (%g, %g, %g, %g) → 浮点%s / 整数%s（就近取内置档，绘制照画；本局首个，之后只计数）",
             b[0], b[1], b[2], b[3], name[pf], name[pi]);
    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
}

static void samp_border_unreserve(const struct nvn_gpu_samp_s *s) {
    if (!s->border_slots) return;
    pthread_mutex_lock(&g_mu);
    g.border_live -= s->border_slots;
    pthread_mutex_unlock(&g_mu);
}

static VkCompareOp samp_compare(uint8_t m) {
    switch (m) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

nvn_gpu_rc nvn_gpu_samp_create(const samp_par *p, nvn_gpu_samp *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!p) return fail(err, NVN_GPU_E_ARG, "samp_par 为空");
    struct nvn_gpu_samp_s *s = calloc(1, sizeof *s);
    if (!s) return fail(err, NVN_GPU_E_OOM, "采样器结构分配失败");
    VkSamplerCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    switch (p->minf) {
    case NVN_MIN_FILTER_NEAREST: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    default: free(s); return fail(err, NVN_GPU_E_ARG, "NVN 最小过滤 %u 无映射", p->minf);
    }
    {
        const uint8_t wrap[3] = {(uint8_t)p->ws, (uint8_t)p->wt, (uint8_t)p->wr};
        VkSamplerAddressMode *mode[3] = {&ci.addressModeU, &ci.addressModeV, &ci.addressModeW};
        for (int i = 0; i < 3; i++) {
            const char *bad = samp_wrap(wrap[i], mode[i]);
            if (bad) { free(s); return fail(err, NVN_GPU_E_UNSUPPORTED, "%s（%c 向环绕 %u）", bad, "STR"[i], wrap[i]); }
        }
    }
    ci.mipLodBias = p->lod_bias;
    ci.minLod = p->lod_min;
    ci.maxLod = p->lod_max;
    if (p->aniso > 1.0f) {
        if (!g.dev->core_features.samplerAnisotropy) {
            free(s);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "请求各向异性 %.2f，但设备未启用 samplerAnisotropy", p->aniso);
        }
        ci.anisotropyEnable = VK_TRUE;
        const float limit = g.dev->properties.limits.maxSamplerAnisotropy;
        ci.maxAnisotropy = p->aniso > limit ? limit : p->aniso;
    } else ci.maxAnisotropy = 1.0f;
    if (p->cmp_mode != NVN_COMPARE_MODE_NONE) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = samp_compare(p->cmp_func);
    }
    // #976：min / max 归约（VK_EXT_sampler_filter_minmax）。只在有线性过滤（放大 / 缩小 / 级间）或各向异性时才挂；全最近点且不开各向异性时足迹单一。
    int mapping_fallback = 0;
    VkSamplerReductionModeCreateInfoEXT rm;
    memset(&rm, 0, sizeof rm);
    rm.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT;
    if (p->reduction != NVN_SAMPLER_REDUCTION_AVERAGE) {
        const char *rn = p->reduction == NVN_SAMPLER_REDUCTION_MIN ? "min" : p->reduction == NVN_SAMPLER_REDUCTION_MAX ? "max" : NULL;
        if (!rn) { free(s); return fail(err, NVN_GPU_E_ARG, "sampler_reduction_mode：NVN 归约模式 %u 无映射", p->reduction); }
        int linear = ci.magFilter == VK_FILTER_LINEAR || ci.minFilter == VK_FILTER_LINEAR || ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR || ci.anisotropyEnable;
        if (linear) {
            if (!g.dev->sampler_minmax_enabled) { free(s); return fail(err, NVN_GPU_E_UNSUPPORTED, "sampler_reduction_%s：设备没有 VK_EXT_sampler_filter_minmax", rn); }
            if (ci.compareEnable) { free(s); return fail(err, NVN_GPU_E_UNSUPPORTED, "sampler_reduction_%s：Vulkan 不允许比较采样同时做 %s 归约", rn, rn); }
            // 无法证明其他分量安全时按原来的平均过滤画，并在建成后记一次原因。
            if (!g.dev->minmax_image_mapping) mapping_fallback = 1;
            else {
                rm.reductionMode = p->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN_EXT : VK_SAMPLER_REDUCTION_MODE_MAX_EXT;
                ci.pNext = &rm;
                s->reduction = (uint8_t)p->reduction;
            }
        }
    }
    // 边框色只在有一向是 CLAMP_TO_BORDER 时用得到；用不到时不建自定义边框色（不要扩展、不占配额），不是三档的取不透明黑（与改前相同）。
    const int preset = samp_border_preset(p->border);
    const int reach = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                      ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                      ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // 设备没有 VK_EXT_custom_border_color（如 Maleoon 920）：不丢绘制，浮点 / 整数采样器各按 RGBA 距离取最近的内置档（见 nvn_vk_border.h），
    //   建成后计 sampler_border_color_nearest、每局首个打一行信息级日志（samp_border_nearest_note）。
    const int approx = preset < 0 && reach && !g.dev->custom_border_enabled;
    const int custom = preset < 0 && reach && !approx;
    const int preset_f = approx ? nvk_border_nearest(p->border, 0) : preset < 0 ? 1 : preset;
    const int preset_i = approx ? nvk_border_nearest(p->border, 1) : preset < 0 ? 1 : preset;
    if (custom && !g.dev->custom_border_without_format) {
        free(s);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "sampler_border_color_custom_format：设备没有 customBorderColorWithoutFormat，建采样器时不知道视图格式");
    }
    static const VkBorderColor fixed[3] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    static const VkBorderColor fixed_int[3] = {VK_BORDER_COLOR_INT_TRANSPARENT_BLACK, VK_BORDER_COLOR_INT_OPAQUE_BLACK, VK_BORDER_COLOR_INT_OPAQUE_WHITE};
    ci.borderColor = custom ? VK_BORDER_COLOR_FLOAT_CUSTOM_EXT : fixed[preset_f];
    VkSamplerCustomBorderColorCreateInfoEXT cb, cbi;   // 浮点 / 整数两个采样器各一份；format 留 UNDEFINED（上面已要求 WithoutFormat）
    memset(&cb, 0, sizeof cb);
    memset(&cbi, 0, sizeof cbi);
    if (custom) {
        pthread_mutex_lock(&g_mu);   // 配额先占后建：两个 VkSampler 都带自定义边框色
        const int room = g.border_live + 2 <= g.dev->custom_border_max;
        if (room) g.border_live += 2;
        pthread_mutex_unlock(&g_mu);
        if (!room) {
            free(s);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "sampler_border_color_custom_quota：自定义边框色采样器已达设备上限 %u", g.dev->custom_border_max);
        }
        s->border_slots = 2;
        cb.sType = cbi.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
        cb.format = cbi.format = VK_FORMAT_UNDEFINED;
        for (int i = 0; i < 4; i++) {
            cb.customBorderColor.float32[i] = p->border[i];
            cbi.customBorderColor.int32[i] = samp_border_int(p->border[i]);
        }
        cb.pNext = ci.pNext;
        ci.pNext = &cb;
    }
    if (s->reduction) {
        s->average_info = ci;
        s->average_info.pNext = NULL;
        s->average_custom = custom;
        if (custom) {
            s->average_border = cb;
            s->average_border.pNext = NULL;
            s->average_info.pNext = &s->average_border;
        }
    }
    if (vkCreateSampler(g.d, &ci, NULL, &s->sampler) != VK_SUCCESS) {
        samp_border_unreserve(s);
        free(s);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateSampler 拒绝（可能缺少 samplerAnisotropy）");
    }
    {   // 整数格式视图用的那一个：过滤全改最近点、关各向异性、边界色换整数的同名值
        VkSamplerCreateInfo ii = ci;
        ii.pNext = NULL;   // 全最近点：归约不起作用，也不要求视图格式有 FILTER_MINMAX
        ii.magFilter = ii.minFilter = VK_FILTER_NEAREST; ii.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ii.anisotropyEnable = VK_FALSE; ii.maxAnisotropy = 1.0f;
        ii.borderColor = custom ? VK_BORDER_COLOR_INT_CUSTOM_EXT : fixed_int[preset_i];
        if (custom) {
            cbi.pNext = ii.pNext == &cb ? cb.pNext : ii.pNext;   // 换掉浮点那份，链上其余结构不动
            ii.pNext = &cbi;
        }
        if (vkCreateSampler(g.d, &ii, NULL, &s->int_sampler) != VK_SUCCESS) {
            vkDestroySampler(g.d, s->sampler, NULL); samp_border_unreserve(s); free(s);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateSampler 拒绝（整数格式用的最近点采样器）");
        }
    }
    pthread_mutex_lock(&g_mu);
    s->all_next = g.samps;
    g.samps = s;
    pthread_mutex_unlock(&g_mu);
    *out = s;
    if (mapping_fallback) nvn_unimpl("gpu", "sampler_reduction_average_no_component_mapping", NULL);
    if (approx) samp_border_nearest_note(p->border, preset_f, preset_i);
    return rc(NVN_GPU_OK);
}

void nvn_gpu_samp_release(nvn_gpu_samp sh) {
    struct nvn_gpu_samp_s *s = sh;
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->released) {
        s->released = 1;
        rel_epoch_bump();
        if (s->refs == 0) destroy_samp_locked(&g, s);
    }
    pthread_mutex_unlock(&g_mu);
}

static void bc_readback(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        struct nvn_gpu_buf_s *b, uint64_t off, uint64_t row_bytes, uint64_t image_bytes);   // 见 bc_upload 前
static void bc_copy_from_buf(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                             struct nvn_gpu_buf_s *b, uint64_t off, uint64_t row_bytes, uint64_t image_bytes);   // 见 bc_upload 后
// 线性内容上传 / 缓冲 ↔ 贴图拷贝。全部在通道外，按 GPU 顺序执行。
// 缓冲 ↔ 贴图拷贝的一个区域只能有一个面（VUID-VkBufferImageCopy-aspectMask-09103，#1581 K）。深度模板贴图的整字拷贝
//   （NVN D24S8 一个 32 位字：低 24 位深度、高 8 位模板）只走深度面：Vulkan 深度面的缓冲布局正是每纹素 4 字节、D24 在低 24 位，
//   写入时高 8 位忽略、读出时高 8 位未定义。模板面另走一个每纹素 1 字节的区域（COPY_FACE_STENCIL，nvn_gpu_tex_upload 拆出来）。
static VkImageAspectFlags copy_face_aspect(const struct nvn_gpu_tex_s *t) {
    return (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect;
}
// face：COPY_FACE_DEFAULT = 按贴图格式（深度模板贴图取深度面，见 copy_face_aspect）；
//   COPY_FACE_DEPTH_F32 = #398 D24 换 D32 存储的贴图，缓冲里已是换算好的 D32 深度面（每纹素 4 字节 float），只拷深度面；
//   COPY_FACE_STENCIL = 只拷模板面，缓冲每纹素 1 字节。
enum { COPY_FACE_DEFAULT = 0, COPY_FACE_DEPTH_F32 = 1, COPY_FACE_STENCIL = 2 };
static void copy_buf_tex_impl(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes,
                              nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r, int face, int source_ready) {
    const int depth_f32 = face == COPY_FACE_DEPTH_F32, stencil = face == COPY_FACE_STENCIL;
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *t = th;
    struct nvn_gpu_buf_s *b = bh;
    if (t && t->bc_decoded) {   // 压缩字节进现解贴图：在段里解码（#697），不直拷
        if (!b) { set_sticky(c, NVN_GPU_E_ARG, "缓冲为空"); return; }
        NVNcopyRegion r3;
        if (t->target == NVN_TEXTURE_TARGET_3D) {   // 与下面直拷同一口径：3D 贴图的 layer 是切片号（#697 Mali：32×32×32 的 BC 贴图按 layer 1 进来被判「层越界」）
            if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
            memset(&r3, 0, sizeof r3);
            if (r) r3 = *r;
            if (r3.depth <= 0) r3.depth = 1;
            r3.zoffset = (int)layer;
            r = &r3; layer = 0;
        }
        bc_copy_from_buf(c, t, level, layer, r, b, off, row_bytes, image_bytes);
        return;
    }
    if (t && stencil && !(t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) { set_sticky(c, NVN_GPU_E_ARG, "模板面拷贝：贴图没有模板面（0x%x）", t->nvn_format); return; }
    if (t && face == COPY_FACE_DEFAULT && nvn_vk_format_substituted(t->nvn_format)) {   // 格式能力已报 NVN_GPU_FMT_SUBST：调用方给的是 D24 字节
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "替换存储格式（D24 换 D32）的缓冲字节不能直接拷（0x%x），走 nvn_gpu_tex_upload", t->nvn_format);
        return;
    }
    uint32_t bpp = depth_f32 ? 4 : stencil ? 1 : t ? fmt_bpp(t->format) : 0;
    // 块信息：压缩格式一行是一排块（blk_w x blk_h 纹素、blk 字节）；没列进 fmt_bpp 的非压缩格式按 NVN 格式表的每纹素字节数
    uint32_t blk_w = 1, blk_h = 1, blk = bpp;
    if (t && face == COPY_FACE_DEFAULT && !bpp) {
        nvn_fmt_info fi;
        if (nvn_fmt_info_of(t->nvn_format, &fi) && !nvn_fmt_expand_target(t->nvn_format, NULL, NULL)) { blk_w = fi.bw; blk_h = fi.bh; blk = fi.bytes; bpp = blk; }
    }
    VkOffset3D o;
    VkExtent3D e;
    NVNcopyRegion rr;
    if (t && t->target == NVN_TEXTURE_TARGET_3D) {
        if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
        memset(&rr, 0, sizeof rr);
        if (r) rr = *r;
        if (rr.depth <= 0) rr.depth = 1;
        rr.zoffset = (int)layer;
        r = &rr;
    }
    if (!t || !b || !bpp || (t->target != NVN_TEXTURE_TARGET_3D && layer >= t->layers) || (off & 3) ||
        !buf_range(b, off, image_bytes) ||
        (blk_w == 1 && blk_h == 1 ? check_copy_params(t, level, r, row_bytes, image_bytes, bpp, &o, &e)
                            : (level >= t->levels || copy_region(t, level, r, &o, &e) || !row_bytes ||
                               row_bytes < (uint64_t)((e.width + blk_w - 1) / blk_w) * blk || image_bytes < row_bytes * ((e.height + blk_h - 1) / blk_h) ||
                               (o.x % (int32_t)blk_w) || (o.y % (int32_t)blk_h)))) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_buf_tex 参数非法（格式 0x%x 级 %u：越界 / 行宽 %llu / 总 %llu / 偏移 %llu 未 4 对齐）", t ? t->nvn_format : 0, level,
                   (unsigned long long)row_bytes, (unsigned long long)image_bytes, (unsigned long long)off);
        return;
    }
    if (!source_ready) full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = (uint32_t)(row_bytes / blk) * blk_w;   // 以纹素计；压缩格式是块数 × 块宽
    bc.bufferImageHeight = t->target == NVN_TEXTURE_TARGET_3D ? ((e.height + blk_h - 1) / blk_h) * blk_h : (uint32_t)(image_bytes / row_bytes) * blk_h;
    bc.imageSubresource = (VkImageSubresourceLayers){depth_f32 ? VK_IMAGE_ASPECT_DEPTH_BIT : stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : copy_face_aspect(t), level,
                                                     t->target == NVN_TEXTURE_TARGET_3D ? 0 : layer, 1};
    bc.imageOffset = o;
    bc.imageExtent = e;
    vkCmdCopyBufferToImage(c->cb, b->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);   // 之后按采样 / 存储使用；本段统一用 GENERAL
    cmd_ref(c, b, REF_BUF);
    cmd_ref(c, t, REF_TEX);
}

void nvn_gpu_copy_buf_tex(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes,
                          nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r) {
    copy_buf_tex_impl(ch, bh, off, row_bytes, image_bytes, th, level, layer, r, COPY_FACE_DEFAULT, 0);
}

#include "backend_vulkan_d24_readback.inc"

void nvn_gpu_copy_tex_buf(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                          nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *t = th;
    struct nvn_gpu_buf_s *b = bh;
    if (t && t->bc_decoded) { bc_readback(c, t, level, layer, r, b, off, row_bytes, image_bytes); return; }   // 按上传时留下的压缩原字节回读
    { const char *no = t ? readback_refusal(t->nvn_format) : NULL;   // 与 NVN_GPU_FMT_READBACK 同一个判断
      if (no) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "%s（0x%x）", no, t->nvn_format); return; } }
    if (t && nvn_vk_format_substituted(t->nvn_format)) {
        d24_readback_copy(c, t, level, layer, r, b, off, row_bytes, image_bytes);
        return;
    }
    uint32_t bpp = t ? fmt_bpp(t->format) : 0;
    // 块信息（同 copy_buf_tex_impl）：压缩格式一行是一排块（blk_w x blk_h 纹素、blk 字节），区域按纹素给、偏移要落在块边上；
    // 没列进 fmt_bpp 的非压缩格式按 NVN 格式表的每纹素字节数。原先这里对压缩格式一律拒绝（核心把块压缩贴图逐字节拷进影子要用它）。
    uint32_t blk_w = 1, blk_h = 1, blk = bpp;
    if (t && !bpp) {
        nvn_fmt_info bi;
        if (nvn_fmt_info_of(t->nvn_format, &bi) && !nvn_fmt_expand_target(t->nvn_format, NULL, NULL)) { blk_w = bi.bw; blk_h = bi.bh; blk = bi.bytes; bpp = blk; }
    }
    VkOffset3D o;
    VkExtent3D e;
    NVNcopyRegion rr;
    if (t && t->target == NVN_TEXTURE_TARGET_3D) {
        if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
        memset(&rr, 0, sizeof rr);
        if (r) rr = *r;
        if (rr.depth <= 0) rr.depth = 1;
        rr.zoffset = (int)layer;
        r = &rr;
    }
    if (!t || !b || !bpp || (t->target != NVN_TEXTURE_TARGET_3D && layer >= t->layers) || (off & 3) ||
        !buf_range(b, off, image_bytes) ||
        (blk_w == 1 && blk_h == 1 ? check_copy_params(t, level, r, row_bytes, image_bytes, bpp, &o, &e)
                            : (level >= t->levels || copy_region(t, level, r, &o, &e) || !row_bytes || (off % blk) ||
                               row_bytes < (uint64_t)((e.width + blk_w - 1) / blk_w) * blk || image_bytes < row_bytes * ((e.height + blk_h - 1) / blk_h) ||
                               (o.x % (int32_t)blk_w) || (o.y % (int32_t)blk_h)))) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_tex_buf 参数非法（格式 0x%x 级 %u：越界 / 行宽 %llu / 总 %llu / 偏移 %llu 没对齐）", t ? t->nvn_format : 0, level,
                   (unsigned long long)row_bytes, (unsigned long long)image_bytes, (unsigned long long)off);
        return;
    }
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = (uint32_t)(row_bytes / blk) * blk_w;   // 以纹素计；压缩格式是块数 × 块宽
    bc.bufferImageHeight = t->target == NVN_TEXTURE_TARGET_3D ? ((e.height + blk_h - 1) / blk_h) * blk_h : (uint32_t)(image_bytes / row_bytes) * blk_h;   // 块压缩要是块高的整数倍（同 copy_buf_tex_impl）；非压缩 blk_h = 1，原样 e.height
    bc.imageSubresource = (VkImageSubresourceLayers){copy_face_aspect(t), level, t->target == NVN_TEXTURE_TARGET_3D ? 0 : layer, 1};   // 深度模板贴图只回读深度面（#1581 K）
    bc.imageOffset = o;
    bc.imageExtent = e;
    vkCmdCopyImageToBuffer(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buffer, 1, &bc);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
    cmd_ref(c, b, REF_BUF);
    cold_note_write(c, b, off, image_bytes, 0, 0);
}

// 压缩图的偏移按块对齐；尺寸若不足一块，只允许落在对应 mip 的边缘。
static int copy_block_region_ok(const struct nvn_gpu_tex_s *t, uint32_t level, const VkOffset3D *o,
                                const VkExtent3D *e, const nvn_fmt_info *fi) {
    uint32_t w = t->w >> level, h = t->h >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    return (uint32_t)o->x % fi->bw == 0 && (uint32_t)o->y % fi->bh == 0 &&
           (e->width % fi->bw == 0 || (uint64_t)(uint32_t)o->x + e->width == w) &&
           (e->height % fi->bh == 0 || (uint64_t)(uint32_t)o->y + e->height == h);
}

void nvn_gpu_copy_tex(nvn_gpu_cmd ch, nvn_gpu_tex sh, uint32_t slevel, uint32_t slayer, const NVNcopyRegion *sr,
                      nvn_gpu_tex dh, uint32_t dlevel, uint32_t dlayer, const NVNcopyRegion *dr) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *s = sh, *d = dh;
    // 两边同一 VkFormat 才拷；D32S8 不在 fmt_bpp 表里（每纹素两个面），同格式深度模板整面拷贝合法，
    // 一并放行（#398：D24S8 换成 D32S8 后贴图间拷贝照旧可用）。
    nvn_fmt_info si = {0}, di = {0};
    const int compressed = s && d && s->format == d->format && !s->bc_decoded && !d->bc_decoded &&
        nvn_vk_format_is_compressed(s->format) && nvn_fmt_info_of(s->nvn_format, &si) &&
        nvn_fmt_info_of(d->nvn_format, &di) && si.bw > 1 && si.bh > 1 &&
        si.bw == di.bw && si.bh == di.bh && si.bytes == di.bytes;
    const int fmt_ok = s && d && s->format == d->format &&
        (fmt_bpp(s->format) || nvn_vk_format_is_depth(s->format) || compressed);
    VkOffset3D so, dof;
    VkExtent3D se, de;
    if (!fmt_ok || slayer >= s->layers || dlayer >= d->layers ||
        slevel >= s->levels || dlevel >= d->levels || slevel >= 32 || dlevel >= 32 ||
        copy_region(s, slevel, sr, &so, &se) || copy_region(d, dlevel, dr, &dof, &de) ||
        se.width != de.width || se.height != de.height || se.depth != de.depth ||
        (compressed && (!copy_block_region_ok(s, slevel, &so, &se, &si) ||
                        !copy_block_region_ok(d, dlevel, &dof, &de, &di)))) {
        // 把是哪一项不合法说清楚（#690：真机上只看到这一句，定不了位）：两边的格式、级 / 层与各自的层数、图的尺寸与级数、两个区域
        const NVNcopyRegion z = {0, 0, 0, 0, 0, 0}; const NVNcopyRegion *a = sr ? sr : &z, *b = dr ? dr : &z;
        set_sticky(c, NVN_GPU_E_ARG, "copy_tex 参数非法：源 格式 %d %ux%ux%u 级 %u/%u 层 %u/%u 区域 %d,%d,%d %dx%dx%d%s；目标 格式 %d %ux%ux%u 级 %u/%u 层 %u/%u 区域 %d,%d,%d %dx%dx%d%s",
                   s ? (int)s->format : -1, s ? s->w : 0, s ? s->h : 0, s ? s->depth : 0, slevel, s ? s->levels : 0, slayer, s ? s->layers : 0, a->xoffset, a->yoffset, a->zoffset, a->width, a->height, a->depth, sr ? "" : "（整级）",
                   d ? (int)d->format : -1, d ? d->w : 0, d ? d->h : 0, d ? d->depth : 0, dlevel, d ? d->levels : 0, dlayer, d ? d->layers : 0, b->xoffset, b->yoffset, b->zoffset, b->width, b->height, b->depth, dr ? "" : "（整级）");
        return;
    }
    full_barrier(c);
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic;
    memset(&ic, 0, sizeof ic);
    ic.srcSubresource = (VkImageSubresourceLayers){s->aspect, slevel, slayer, 1};
    ic.srcOffset = so;
    ic.dstSubresource = (VkImageSubresourceLayers){d->aspect, dlevel, dlayer, 1};
    ic.dstOffset = dof;
    ic.extent = se;
    vkCmdCopyImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, s, REF_TEX);
    cmd_ref(c, d, REF_TEX);
}

void nvn_gpu_clear_view(nvn_gpu_cmd ch, nvn_gpu_view vh, const float rgba[4], const uint32_t u4[4], int is_int) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_view_s *v = vh;
    if (!v) { set_sticky(c, NVN_GPU_E_ARG, "clear_view 视图为空"); return; }
    struct nvn_gpu_tex_s *t = v->tex;
    if (v->nvn_format != t->nvn_format) {
        // vkCmdClearColorImage 按 image 格式解释 clear 值，无法按 view 的覆盖格式生效
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "clear_view 不支持视图覆盖格式");
        return;
    }
    const int ds_img = (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;   // 纯模板 S8 也走深度模板清除（#1581 O）
    if (!ds_img) {
        nvn_fmt_info fi;
        if (nvn_fmt_info_of(v->nvn_format, &fi)) {
            int fmt_int = fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT;
            if (fmt_int != (is_int != 0)) {
                set_sticky(c, NVN_GPU_E_ARG, "clear_view 的 is_int 与贴图格式不符");
                return;
            }
        }
    }
    VkImageSubresourceRange range = {t->aspect, v->base_level, v->levels, v->base_layer, v->layers};
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (ds_img) {
        VkClearDepthStencilValue ds;
        ds.depth = rgba ? rgba[0] : 1.0f;
        ds.stencil = 0;
        vkCmdClearDepthStencilImage(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ds, 1, &range);
    } else {
        VkClearColorValue col;
        memset(&col, 0, sizeof col);
        uint8_t fmap[4];   // #975：退回格式下 NVN 的分量 i 是 Vulkan 的第 fmap[i] 个分量（恒等时 0,1,2,3）
        nvn_vk_format_chanmap(v->nvn_format, fmap);
        for (int i = 0; i < 4; i++) {
            if (is_int) col.uint32[fmap[i]] = u4 ? u4[i] : 0;
            else col.float32[fmap[i]] = rgba ? rgba[i] : 0.0f;
        }
        vkCmdClearColorImage(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
    }
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
}

// 现解贴图（bc_decoded）的压缩原数据影子：显存里存的是解码后的格式，游戏按压缩字节回读（贴图 → 缓冲拷贝）时要的是原字节。
// 这种贴图不能当渲染目标、不能当存储图像，也不接受压缩字节直拷（见 tex_create / copy_buf_tex_impl），内容只来自 bc_upload，
// 所以在上传时留一份原字节就是准确的回读来源，不用重新压缩。影子是一块 GPU 缓冲，写它和读它都录在同一条命令流里：
// 和贴图本身一样按 GPU 顺序生效，命令被放弃（abort）时一起作废。布局：逐级相接；每级内按层（3D 按该级的切片）相接，每层按块行紧排。
static int bc_shadow_layout(const struct nvn_gpu_tex_s *t, uint32_t bb, uint32_t level,
                            uint64_t *level_off, uint64_t *row_pitch, uint64_t *slice_pitch, uint32_t *slices, uint64_t *total) {
    uint64_t at = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t w = t->w >> l, h = t->h >> l, d = t->target == NVN_TEXTURE_TARGET_3D ? t->depth >> l : t->layers;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!d) d = 1;
        uint64_t row = (uint64_t)((w + 3) / 4) * bb, slice = row * ((h + 3) / 4);
        if (l == level) { *level_off = at; *row_pitch = row; *slice_pitch = slice; *slices = d; }
        if (slice && d > (UINT64_MAX - at) / slice) return 0;
        at += slice * d;
    }
    *total = at;
    return level < t->levels;
}

// 现解贴图的贴图 → 缓冲拷贝：从压缩影子取原字节，按调用方的行距写进目标缓冲（在 GPU 流里按序：暂存区 + 拷贝）。
// 没上传过的区域回零（原机上是未定义内容）。3D 贴图按 copy_tex_buf 的约定 layer 是切片、区域深度为 1。
static int bc_shadow_ensure(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint64_t total);
static void bc_readback(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        struct nvn_gpu_buf_s *b, uint64_t off, uint64_t row_bytes, uint64_t image_bytes) {
    VkOffset3D o; VkExtent3D e; int bb = 0; NVNcopyRegion rr;
    int is3d = t->target == NVN_TEXTURE_TARGET_3D;
    if (is3d) { memset(&rr, 0, sizeof rr); if (r) rr = *r; if (rr.depth > 1) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "BC 解码贴图回读一次只读一个切片"); return; }
                rr.depth = 1; rr.zoffset = (int)layer; r = &rr; }
    if (!b || level >= t->levels || (!is3d && layer >= t->layers) || copy_region(t, level, r, &o, &e) || !e.width || !e.height ||
        !nvn_vk_bcdec_route(t->nvn_format, NULL, NULL, NULL, NULL, &bb) || bb <= 0) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：子资源或区域非法"); return;
    }
    uint32_t mw = t->w >> level, mh = t->h >> level; if (!mw) mw = 1; if (!mh) mh = 1;
    if ((o.x & 3) || (o.y & 3) || ((e.width & 3) && (uint32_t)o.x + e.width != mw) || ((e.height & 3) && (uint32_t)o.y + e.height != mh)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：区域未按压缩块对齐"); return;
    }
    uint64_t bw = ((uint64_t)e.width + 3) / 4, bh = ((uint64_t)e.height + 3) / 4, packed_row = bw * (uint32_t)bb;
    if (row_bytes < packed_row || (bh > 1 && row_bytes > (UINT64_MAX - packed_row) / (bh - 1)) ||
        image_bytes < (bh - 1) * row_bytes + packed_row || !buf_range(b, off, (bh - 1) * row_bytes + packed_row)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：目标行距 / 长度非法"); return;
    }
    uint64_t loff = 0, rp = 0, sp = 0, total = 0; uint32_t slices = 0;
    uint32_t slice = is3d ? (uint32_t)o.z : layer;
    if (!bc_shadow_layout(t, (uint32_t)bb, level, &loff, &rp, &sp, &slices, &total) || slice >= slices) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：切片越界"); return;
    }
    if (!bc_shadow_ensure(c, t, total)) return;   // 一次都没上传过：影子建出来是全零（原机上是未定义内容）
    VkBufferCopy *rg = calloc((size_t)bh, sizeof *rg);
    if (!rg) { set_sticky(c, NVN_GPU_E_OOM, "BC 解码贴图回读：拷贝表分配失败"); return; }
    uint64_t src = loff + (uint64_t)slice * sp + (uint64_t)(o.y / 4) * rp + (uint64_t)(o.x / 4) * (uint32_t)bb;
    for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){src + y * rp, off + y * row_bytes, packed_row};
    full_barrier(c);
    vkCmdCopyBuffer(c->cb, t->bc_sbuf, b->buffer, (uint32_t)bh, rg);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    free(rg);
    cmd_ref(c, b, REF_BUF);
    cmd_ref(c, t, REF_TEX);
    cold_note_write(c, b, off, (bh - 1) * row_bytes + packed_row, 0, 0);
}

// 第一次上传时建压缩影子缓冲（大小按 bc_shadow_layout 的总长）。失败置粘滞。
static int bc_shadow_ensure(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint64_t total) {
    if (t->bc_sbuf) return 1;
    VkBufferCreateInfo bi = {0}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = total ? total : 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkMemoryRequirements req;
    if (vkCreateBuffer(g.d, &bi, NULL, &buf) != VK_SUCCESS) { set_sticky(c, NVN_GPU_E_OOM, "BC 上传：压缩影子缓冲建不出（%llu 字节）", (unsigned long long)total); return 0; }
    vkGetBufferMemoryRequirements(g.d, buf, &req);
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, 0, NULL);
    VkMemoryAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(g.d, &ai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(g.d, buf, mem, 0) != VK_SUCCESS) {
        if (mem) vkFreeMemory(g.d, mem, NULL);
        vkDestroyBuffer(g.d, buf, NULL);
        set_sticky(c, NVN_GPU_E_OOM, "BC 上传：压缩影子显存分配失败（%llu 字节）", (unsigned long long)total); return 0;
    }
    t->bc_sbuf = buf; t->bc_smem = mem;
    if (g.bc_time_on) {
        pthread_mutex_lock(&g_mu);
        t->bc_shadow_mem_size = req.size;
        pthread_mutex_unlock(&g_mu);
    }
    full_barrier(c);
    vkCmdFillBuffer(c->cb, buf, 0, VK_WHOLE_SIZE, 0);   // 没上传过的区域回读为零，不留上一块显存的旧内容
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    return 1;
}

// BC 贴图写入区域与源排布的校验（bc_upload 与缓冲 → BC 贴图共用）。返回 1 = 合法，并给出区域、块尺寸、源切片行距。
static int bc_src_check(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        uint64_t row_bytes, uint64_t image_bytes, VkOffset3D *offp, VkExtent3D *extp, int *bbp,
                        uint64_t *bwp, uint64_t *bhp, uint64_t *slice_stridep) {
    VkOffset3D off; VkExtent3D ext; int bb = 0;
    // 原先四种原因合成一句「子资源或区域非法」，真机读数分不出是哪一种（#697 Mali）：逐条报，带上数值
    if (level >= t->levels || layer >= t->layers) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传子资源越界：级 %u / 共 %u，层 %u / 共 %u", level, t->levels, layer, t->layers); return 0;
    }
    if (!nvn_vk_bcdec_route(t->nvn_format, NULL, NULL, NULL, NULL, &bb)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传：格式 %#x 没有现解路线", t->nvn_format); return 0;
    }
    if (copy_region(t, level, r, &off, &ext) || !ext.width || !ext.height || !ext.depth) {
        uint32_t mw = t->w >> level, mh = t->h >> level, md = t->depth >> level;
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传区域超出这一级：级 %u（%ux%ux%u），区域 (%d,%d,%d) %dx%dx%d，目标 %u",
                   level, mw ? mw : 1, mh ? mh : 1, md ? md : 1, r ? r->xoffset : 0, r ? r->yoffset : 0, r ? r->zoffset : 0,
                   r ? r->width : 0, r ? r->height : 0, r ? r->depth : 0, t->target);
        return 0;
    }
    uint32_t mw = t->w >> level, mh = t->h >> level; if (!mw) mw = 1; if (!mh) mh = 1;
    if ((off.x & 3) || (off.y & 3) || ((ext.width & 3) && (uint32_t)off.x + ext.width != mw) ||
        ((ext.height & 3) && (uint32_t)off.y + ext.height != mh)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传区域未按压缩块对齐"); return 0;
    }
    uint64_t bw = ((uint64_t)ext.width + 3) / 4, bh = ((uint64_t)ext.height + 3) / 4;
    uint64_t packed_row = bw * (uint32_t)bb;
    if (row_bytes < packed_row || bh > SIZE_MAX / packed_row ||
        (bh > 1 && row_bytes > (UINT64_MAX - packed_row) / (bh - 1)) || image_bytes < (bh - 1) * row_bytes + packed_row) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传源行距或长度非法"); return 0;
    }
    // image_bytes 是整个体积的可读长度；相邻 z 片相隔 bh 个源块行，不是 image_bytes。
    if (row_bytes > SIZE_MAX / bh) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传源切片行距溢出"); return 0;
    }
    uint64_t slice_stride = row_bytes * bh;
    uint64_t last_slice = (bh - 1) * row_bytes + packed_row;
    if ((uint64_t)(ext.depth - 1) > (SIZE_MAX - last_slice) / slice_stride ||
        image_bytes < (uint64_t)(ext.depth - 1) * slice_stride + last_slice) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传源体积长度非法"); return 0;
    }
    *offp = off; *extp = ext; *bbp = bb; *bwp = bw; *bhp = bh; *slice_stridep = slice_stride;
    return 1;
}

#include "backend_vulkan_bc1_astc.inc"

// 解码用的临时图：解出来的像素先落在这里（带 STORAGE 的可变格式），再按区域拷进目标贴图（bc_upload 与缓冲 → BC 贴图共用）。
static struct bc_temp *bc_temp_image(const struct nvn_gpu_tex_s *t, VkExtent3D ext) {
    struct bc_temp *tmp = calloc(1, sizeof *tmp);
    if (!tmp) return NULL;
    VkImageCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = t->format; ci.extent = (VkExtent3D){ext.width, ext.height, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements mr; VkMemoryAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    if (vkCreateImage(g.d, &ci, NULL, &tmp->image) != VK_SUCCESS) goto fail;
    vkGetImageMemoryRequirements(g.d, tmp->image, &mr);
    ai.allocationSize = mr.size; ai.memoryTypeIndex = s2a_vk_memory_type(g.dev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.d, &ai, NULL, &tmp->memory) != VK_SUCCESS ||
        vkBindImageMemory(g.d, tmp->image, tmp->memory, 0) != VK_SUCCESS) goto fail;
    return tmp;
fail:
    if (tmp->image) vkDestroyImage(g.d, tmp->image, NULL);
    if (tmp->memory) vkFreeMemory(g.d, tmp->memory, NULL);
    free(tmp);
    return NULL;
}

// 解码好的临时图按区域拷进目标贴图（z 片 / 层由调用方给）。
static void bc_temp_to_tex(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, struct bc_temp *tmp, uint32_t level, uint32_t layer,
                           VkOffset3D off, VkExtent3D ext, uint32_t z) {
    VkImageMemoryBarrier b = {0}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = tmp->image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cp = {0}; cp.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}; cp.dstOffset = off; cp.dstOffset.z += (int32_t)z;
    cp.extent = (VkExtent3D){ext.width, ext.height, 1};
    vkCmdCopyImage(c->cb, tmp->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL); cmd_ref(c, t, REF_TEX);
}

// 段内解码的命令（资源由 nvn_vk_bcdec_prepare 建好）：在这里用后端的 vkCmd* 录——两级回放开着时这些名字走包装进录制线程，
// 和段里前后的拷贝保持同一顺序（模块里直接调 vkCmd* 会越过录制线程、跑到前面那几条拷贝之前）。
static void bc_record_decode(struct nvn_gpu_cmd_s *c, const struct nvn_vk_bcdec_rec *rec, VkImage dst,
                             VkBuffer src, VkDeviceSize src_off, uint32_t w, uint32_t h, uint32_t format) {
    VkImageMemoryBarrier b = {0}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = rec->in; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {0};
    cp.bufferOffset = src_off;
    cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = (VkExtent3D){rec->bw, rec->bh, 1};
    vkCmdCopyBufferToImage(c->cb, src, rec->in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageMemoryBarrier db = {0}; db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;   // 临时图整张被覆写：UNDEFINED → GENERAL
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = dst; db.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    db.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &db);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, rec->pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, rec->pl, 0, 1, &rec->ds, 0, NULL);
    int32_t res[2] = { (int32_t)w, (int32_t)h };
    vkCmdPushConstants(c->cb, rec->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, res);
    int time_query = bc_time_begin(c, BC_TIME_RGBA, format, (uint64_t)rec->bw * rec->bh);
    vkCmdDispatch(c->cb, (w + 7) / 8, (h + 7) / 8, 1);
    bc_time_end(c, time_query);
    // 结果交给 bc_temp_to_tex 的屏障（COMPUTE 写 → TRANSFER 读，同时转 TRANSFER_SRC）
}

// 一个切片的段内解码（CPU 字节 → BC 与缓冲 → BC 共用）。紧凑块已在段内暂存 (sb, soff) 里（16 字节对齐），整条按 GPU 顺序录进本段：
//   ① 同一份块拷进压缩影子（回读用，见 bc_readback）；② 解码录进本段（nvn_vk_bcdec_prepare 建资源、bc_record_decode 录命令）落到临时图；
//   ③ 临时图按区域拷进目标。不单独提交、不等 fence；临时图与解码资源挂 c->bc_temps，暂存是段的 arena，都随段释放
//   （放弃的段没提交过，直接回收；执行过的段在完成后回收）。解码绑了自己的计算管线和描述符组：录完让描述符布局缓存失效，下一次派发重绑。
// 失败置粘滞，返回 0。
static int bc_record_slice(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkBuffer sb, uint64_t soff, uint64_t bh, uint64_t packed_row,
                           uint64_t shadow_dst, uint64_t sh_rp, uint32_t level, uint32_t layer, VkOffset3D off, VkExtent3D ext, uint32_t z) {
    char why[192] = {0};
    VkBufferCopy *rg = calloc((size_t)bh, sizeof *rg);
    if (!rg) { set_sticky(c, NVN_GPU_E_OOM, "BC 上传：压缩影子拷贝表分配失败"); return 0; }
    for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){soff + y * packed_row, shadow_dst + y * sh_rp, packed_row};
    full_barrier(c);   // 暂存此前在本段里的写（缓冲 → BC 的源拷贝）对影子拷贝和解码输入可见
    vkCmdCopyBuffer(c->cb, sb, t->bc_sbuf, (uint32_t)bh, rg);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    free(rg);
    if (bc1_astc_image(t)) return bc1_astc_encode(c, t, sb, soff, bh * (packed_row / 8), level, layer, off, ext);
    struct bc_temp *tmp = bc_temp_image(t, ext);
    if (!tmp) { set_sticky(c, NVN_GPU_E_OOM, "BC 上传：临时图分配失败"); return 0; }
    if (nvn_vk_bcdec_prepare(g.bc, g.dev, tmp->image, t->nvn_format, ext.width, ext.height, 0, 0, &tmp->rec, why, sizeof why) != 0) {
        vkDestroyImage(g.d, tmp->image, NULL); vkFreeMemory(g.d, tmp->memory, NULL); free(tmp);
        set_sticky(c, NVN_GPU_E_DEVICE, "BC 解码失败：%s", why); return 0;
    }
    bc_record_decode(c, &tmp->rec, tmp->image, sb, soff, ext.width, ext.height, t->nvn_format);
    c->desc_layout = VK_NULL_HANDLE;   // 计算绑定点被解码占过：下一次派发重绑描述符组
    tmp->next = c->bc_temps; c->bc_temps = tmp;
    bc_temp_to_tex(c, t, tmp, level, layer, off, ext, z);
    return 1;
}

// 解码器（计算管线）首次用到时建，之后只读。g_submit_mu 只护这次建立，不护任何提交或等待。
static int bc_decoder_ready(struct nvn_gpu_cmd_s *c) {
    char why[192] = {0};
    pthread_mutex_lock(&g_submit_mu);
    int ready = g.bc || nvn_vk_bcdec_create(g.dev, &g.bc, why, sizeof why) == 0;
    pthread_mutex_unlock(&g_submit_mu);
    if (!ready) set_sticky(c, NVN_GPU_E_DEVICE, "BC 解码初始化失败：%s", why);
    return ready;
}

// CPU 字节 → BC 现解贴图：每个 z 切片把源行（带行距）拷成段内暂存里的紧凑块，再交 bc_record_slice 录进本段。
// 原先这里在 g_submit_mu 里单独 vkQueueSubmit 解码再等 fence（最长 5 s）：单独提交排在完成线程未退休的段之后，
// 每次等待跟着前面的段一起等（天玑 9000 / Mali 每帧累计中位 641 ms），上层 rp_tex_resolve 拿着 g_rmu 一起等。
static void bc_upload(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer,
                      const NVNcopyRegion *r, const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    VkOffset3D off; VkExtent3D ext; int bb = 0; uint64_t bw = 0, bh = 0, slice_stride = 0;
    if (!bc_src_check(c, t, level, layer, r, row_bytes, image_bytes, &off, &ext, &bb, &bw, &bh, &slice_stride)) return;
    uint64_t packed_row = bw * (uint32_t)bb, bytes = packed_row * bh;
    uint64_t sh_loff = 0, sh_rp = 0, sh_sp = 0, sh_total = 0; uint32_t sh_slices = 0;
    if (!bc_shadow_layout(t, (uint32_t)bb, level, &sh_loff, &sh_rp, &sh_sp, &sh_slices, &sh_total)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传：压缩影子布局溢出"); return;
    }
    const int astc = bc1_astc_image(t);
    if (!bc_shadow_ensure(c, t, sh_total) || (!astc && !bc_decoder_ready(c))) return;
    for (uint32_t z = 0; z < ext.depth; z++) {
        uint32_t s = t->target == NVN_TEXTURE_TARGET_3D ? (uint32_t)off.z + z : layer;
        if (s >= sh_slices) { set_sticky(c, NVN_GPU_E_ARG, "BC 上传：切片越过压缩影子"); return; }
        nvn_gpu_slice ss; nvn_gpu_err se;
        if (!rc_is(nvn_gpu_arena_alloc(c, bytes, astc ? bc1_astc_alignment() : 16, &ss, &se), NVN_GPU_OK)) {
            if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_OOM, "BC 上传暂存分配失败：%s", se.why);
            return;
        }
        for (uint64_t y = 0; y < bh; y++)
            memcpy((uint8_t *)ss.cpu + y * packed_row, (const uint8_t *)src + (uint64_t)z * slice_stride + y * row_bytes, (size_t)packed_row);
        if (astc && !bc1_astc_opaque(ss.cpu, bytes)) {
            set_sticky(c, NVN_GPU_E_ARG, "BC1 ASTC 紧凑快照出现透明索引，需要先换回 RGBA8");
            return;
        }
        uint64_t dst = sh_loff + (uint64_t)s * sh_sp + (uint64_t)(off.y / 4) * sh_rp + (uint64_t)(off.x / 4) * (uint32_t)bb;
        if (!bc_record_slice(c, t, ss.buf->buffer, ss.off, bh, packed_row, dst, sh_rp, level, layer, off, ext, z)) return;
    }
}

// 缓冲 → BC 现解贴图（#697）：游戏把压缩块从 GPU 缓冲拷进 BC 贴图（常见是同一段里刚从另一张 BC 贴图回读出来的字节），
// 录制时 CPU 上没有这些字节：源行（带行距）先在 GPU 上拷成段内暂存里的紧凑块，之后同 CPU 字节那条（bc_record_slice）。
static void bc_copy_from_buf(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                             struct nvn_gpu_buf_s *b, uint64_t boff, uint64_t row_bytes, uint64_t image_bytes) {
    if (bc1_astc_image(t)) {
        set_sticky(c, NVN_GPU_E_ORDER, "GPU 来源的 BC1 上传需要先换回 RGBA8");
        return;
    }
    VkOffset3D off; VkExtent3D ext; int bb = 0; uint64_t bw = 0, bh = 0, slice_stride = 0;
    if (!bc_src_check(c, t, level, layer, r, row_bytes, image_bytes, &off, &ext, &bb, &bw, &bh, &slice_stride)) return;
    if (boff > b->bytes || image_bytes > b->bytes - boff) { set_sticky(c, NVN_GPU_E_ARG, "缓冲 → BC 贴图：源越过缓冲末尾"); return; }
    uint64_t packed_row = bw * (uint32_t)bb, bytes = packed_row * bh;
    uint64_t sh_loff = 0, sh_rp = 0, sh_sp = 0, sh_total = 0; uint32_t sh_slices = 0;
    if (!bc_shadow_layout(t, (uint32_t)bb, level, &sh_loff, &sh_rp, &sh_sp, &sh_slices, &sh_total)) {
        set_sticky(c, NVN_GPU_E_ARG, "缓冲 → BC 贴图：压缩影子布局溢出"); return;
    }
    if (!bc_shadow_ensure(c, t, sh_total) || !bc_decoder_ready(c)) return;
    VkBufferCopy *rg = calloc((size_t)bh, sizeof *rg);
    if (!rg) { set_sticky(c, NVN_GPU_E_OOM, "缓冲 → BC 贴图：拷贝区域表分配失败"); return; }
    cmd_ref(c, b, REF_BUF);
    for (uint32_t z = 0; z < ext.depth && rc_is(c->sticky, NVN_GPU_OK); z++) {
        uint32_t s = t->target == NVN_TEXTURE_TARGET_3D ? (uint32_t)off.z + z : layer;
        if (s >= sh_slices) { set_sticky(c, NVN_GPU_E_ARG, "缓冲 → BC 贴图：切片越过压缩影子"); break; }
        nvn_gpu_slice ss; nvn_gpu_err se;
        if (!rc_is(nvn_gpu_arena_alloc(c, bytes, 16, &ss, &se), NVN_GPU_OK)) {
            if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_OOM, "缓冲 → BC 贴图：暂存分配失败");
            break;
        }
        full_barrier(c);   // 源缓冲此前在本段里的写（如回读）对拷贝可见
        for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){boff + (uint64_t)z * slice_stride + y * row_bytes, ss.off + y * packed_row, packed_row};
        vkCmdCopyBuffer(c->cb, b->buffer, ss.buf->buffer, (uint32_t)bh, rg);
        c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
        uint64_t dst = sh_loff + (uint64_t)s * sh_sp + (uint64_t)(off.y / 4) * sh_rp + (uint64_t)(off.x / 4) * (uint32_t)bb;
        if (!bc_record_slice(c, t, ss.buf->buffer, ss.off, bh, packed_row, dst, sh_rp, level, layer, off, ext, z)) break;
    }
    free(rg);
}

#include "backend_vulkan_bc1_snapshot.inc"

// D24S8 上传的模板面（#1581 K）：从每纹素 32 位字的高 8 位抽出来，按每纹素 1 字节另交一个只有模板面的区域。
//   深度面那一区（整字或 #398 换算后的 D32）由调用方先交；两区都在本段、按 GPU 顺序执行。
static void upload_d24s8_stencil(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                                 const uint8_t *src, uint64_t row_bytes, uint64_t image_bytes, uint32_t ew, uint32_t eh) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;   // 深度面那一区已判非法：不再补交
    if (row_bytes < (uint64_t)ew * 4 || image_bytes < row_bytes * (eh - 1) + (uint64_t)ew * 4) {
        set_sticky(c, NVN_GPU_E_ARG, "tex_upload D24S8 模板面越界（行 %llu、总 %llu、区域 %ux%u）",
                   (unsigned long long)row_bytes, (unsigned long long)image_bytes, ew, eh);
        return;
    }
    nvn_gpu_slice sl;
    nvn_gpu_err e;
    if (!rc_is(nvn_gpu_arena_alloc(ch, (uint64_t)ew * eh, 4, &sl, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "tex_upload 模板面暂存：%s", e.why);
        return;
    }
    uint8_t *dst = (uint8_t *)sl.cpu;
    for (uint32_t y = 0; y < eh; y++) {
        const uint8_t *row = src + (uint64_t)y * row_bytes;
        for (uint32_t x = 0; x < ew; x++) dst[(uint64_t)y * ew + x] = row[(uint64_t)x * 4 + 3];
    }
    copy_buf_tex_impl(ch, sl.buf, sl.off, ew, (uint64_t)ew * eh, th, level, layer, r, COPY_FACE_STENCIL, 0);
}

void nvn_gpu_tex_upload(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!src) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload src 为空"); return; }
    struct nvn_gpu_tex_s *t = th;
    if (!t) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 贴图为空"); return; }
    if (t->bc_decoded) { bc_upload(c, t, level, layer, r, src, row_bytes, image_bytes); return; }
    int ebpp = 0;
    int expand = nvn_fmt_expand_target(t->nvn_format, NULL, &ebpp);
    uint32_t ew = r ? (uint32_t)r->width : (t->w >> level);
    uint32_t eh = r ? (uint32_t)r->height : (t->h >> level);
    if (!ew) ew = 1;
    if (!eh) eh = 1;
    if (expand && (r && (r->width <= 0 || r->height <= 0))) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开区域非法"); return; }
    uint64_t out_row = expand ? (uint64_t)ebpp * ew : 0;
    uint64_t out_bytes = expand ? out_row * eh : 0;
    if (expand) {
        nvn_fmt_info in;
        if (!nvn_fmt_info_of(t->nvn_format, &in)) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 源格式未知"); return; }
        uint64_t need_row = (uint64_t)in.bytes * ew;
        uint64_t need_img = (uint64_t)row_bytes * (eh - 1) + need_row;
        if (row_bytes < need_row || image_bytes < need_img) {
            set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开源越界（行 %llu < %llu 或总 %llu < %llu）",
                       (unsigned long long)row_bytes, (unsigned long long)need_row,
                       (unsigned long long)image_bytes, (unsigned long long)need_img);
            return;
        }
    }
    uint64_t need = expand ? out_bytes : image_bytes;
    nvn_gpu_slice sl;
    nvn_gpu_err e;
    // 暂存偏移要是目标格式一个纹素块字节数的倍数（Vulkan 对缓冲到图像拷贝的要求；压缩格式一块 8 / 16 字节，按 4 对齐会落在块中间）
    uint32_t align = 4;
    { nvn_fmt_info bi; uint32_t bb = expand ? (uint32_t)ebpp : nvn_fmt_info_of(t->nvn_format, &bi) ? bi.bytes : 0;
      if (bb > 4 && !(bb & (bb - 1))) align = bb; }
    if (!rc_is(nvn_gpu_arena_alloc(ch, need, align, &sl, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "tex_upload 暂存：%s", e.why);
        return;
    }
    if (expand) {
        if (nvn_fmt_expand_upload(t->nvn_format, src, (size_t)row_bytes, ew, eh,
                                  sl.cpu, (size_t)out_row) != 0) {
            set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开失败（格式 0x%x）", t->nvn_format);
            return;
        }
        nvn_gpu_copy_buf_tex(ch, sl.buf, sl.off, out_row, out_bytes, th, level, layer, r);
    } else if (nvn_vk_format_substituted(t->nvn_format)) {
        // #398：D24 换成 D32 存储。按原 D24 深度面口径（每纹素 4 字节，低 24 位 UNORM）换成 float 再传深度面。
        if (row_bytes & 3) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload D24 行宽未 4 对齐"); return; }
        memcpy(sl.cpu, src, (size_t)image_bytes);
        nvn_vk_depth24_to_f32(sl.cpu, (size_t)image_bytes / 4);
        copy_buf_tex_impl(ch, sl.buf, sl.off, row_bytes, image_bytes, th, level, layer, r, COPY_FACE_DEPTH_F32, 0);
        if (t->nvn_format == NVN_FORMAT_DEPTH24_STENCIL8) upload_d24s8_stencil(ch, th, level, layer, r, src, row_bytes, image_bytes, ew, eh);   // 换存储后模板面照样要传
    } else {
        memcpy(sl.cpu, src, (size_t)image_bytes);
        nvn_gpu_copy_buf_tex(ch, sl.buf, sl.off, row_bytes, image_bytes, th, level, layer, r);   // 深度模板贴图：这一区只有深度面（copy_face_aspect）
        if (t->nvn_format == NVN_FORMAT_DEPTH24_STENCIL8 && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
            upload_d24s8_stencil(ch, th, level, layer, r, src, row_bytes, image_bytes, ew, eh);
    }
}

#include "backend_vulkan_clear.inc"
#include "backend_vulkan_draw_tex.inc"

void nvn_gpu_clear_rect(nvn_gpu_cmd ch, const nvn_gpu_clear *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "clear_rect 在通道外"); return; }
    es_reset(c);   // 内部画一笔整屏三角形：换管线（只声明了部分动态状态）、改视口裁剪与模板、写 push；下一笔绘制全部重发（#708）
    if (!d || (d->color < 0 && !d->depth && !d->stencil)) { set_sticky(c, NVN_GPU_E_ARG, "clear_rect 什么都不清"); return; }
    if (d->color >= 0 && ((uint32_t)d->color >= c->pass_ncolor || !(c->pass_color_mask >> d->color & 1))) { set_sticky(c, NVN_GPU_E_ARG, "clear_rect 颜色槽 %d 越界或是空位（通道有 %u 个槽）", d->color, c->pass_ncolor); return; }
    if ((d->depth && !(c->pass_ds_aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) || (d->stencil && !(c->pass_ds_aspect & VK_IMAGE_ASPECT_STENCIL_BIT))) {
        set_sticky(c, NVN_GPU_E_ARG, "clear_rect 要清的深度 / 模板面不在通道里"); return;
    }
    if (!d->w || !d->h || d->x < 0 || d->y < 0 || (uint64_t)d->x + d->w > c->pass_w || (uint64_t)d->y + d->h > c->pass_h) {
        set_sticky(c, NVN_GPU_E_ARG, "clear_rect 矩形 %d,%d %ux%u 为空或超出通道 %ux%u", d->x, d->y, d->w, d->h, c->pass_w, c->pass_h); return;
    }
    uint32_t cm = d->color >= 0 ? d->color_mask & 0xfu : 0xfu, sm = d->stencil ? d->stencil_mask : 0xffu;
    if (!cm || !sm) { set_sticky(c, NVN_GPU_E_ARG, "clear_rect 写掩码为 0（颜色 %#x 模板 %#x）", (unsigned)d->color_mask, (unsigned)d->stencil_mask); return; }
    if (c->feedback_read_mask && !feedback_restart(c, 0, 0)) return;
    if (!pass_ensure_area(c, (VkRect2D){{d->x, d->y}, {d->w, d->h}})) return;
    // 一律画一笔整屏三角形（backend_vulkan_clear.inc：不用附件清除命令，理由见那里）。颜色、深度、模板同一笔做
    clr_draw(c, d->color, cm, &d->value, d->depth != 0, d->stencil ? sm : 0, d->stencil_value, d);
    c->desc_layout = VK_NULL_HANDLE; c->desc_dirty = 0xfu;   // 内部清除改绑状态：下一笔全部重建
}

// 存储不跨生命周期复用代号。查询通常在提交线程，原子赋值也允许平台的并行资源准备。
static uint64_t g_access_next;
static uint64_t access_id(uint64_t *slot) {
    uint64_t id = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (!id) {
        uint64_t next = __atomic_add_fetch(&g_access_next, 1, __ATOMIC_RELAXED), empty = 0;
        if (__atomic_compare_exchange_n(slot, &empty, next, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) id = next;
        else id = empty;
    }
    return id;
}
int nvn_gpu_buf_access_span(nvn_gpu_buf b, uint64_t off, uint64_t bytes, nvn_gpu_access_span *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!bytes || !buf_range(b, off, bytes)) return 0;
    out->storage = access_id(&b->access_id); out->off = off; out->bytes = bytes;
    return out->storage != 0;
}
nvn_gpu_tex nvn_gpu_view_texture(nvn_gpu_view v) { return v ? v->tex : NULL; }
int nvn_gpu_view_access_span(nvn_gpu_view v, nvn_gpu_access_span *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!v) return 0;
    if (v->buf) return nvn_gpu_buf_access_span(v->buf, v->buffer_off, v->buffer_bytes, out);
    if (!v->tex || !v->levels || !v->layers) return 0;
    out->storage = access_id(&v->tex->access_id);
    out->level = v->base_level; out->levels = v->levels; out->layer = v->base_layer; out->layers = v->layers;
    // 3D 的 Vulkan array-layer 范围固定 0+1；实际采样覆盖所选 mip 的全部深度切片。
    // 矩形取首级最大深度，较高 mip 的合法切片是其子集；2D 切片视图仍保留真实层范围。
    if (v->type == VK_IMAGE_VIEW_TYPE_3D) {
        uint32_t depth = v->base_level < 32 ? v->tex->depth >> v->base_level : 0;
        out->layer = 0; out->layers = depth ? depth : 1;
    }
    out->aspects = (v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? 1u : 0u) |
                   (v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? 2u : 0u) | (v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? 4u : 0u);
    return out->storage && out->aspects;
}
void nvn_gpu_compute_to_graphics(nvn_gpu_cmd ch) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "计算写的可见性屏障必须在通道外"); return; }
    VkPipelineStageFlags dst = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (g.dev->core_features.geometryShader) dst |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, dst,
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}

void nvn_gpu_barrier(nvn_gpu_cmd ch, uint32_t nvn_bits) {
    (void)nvn_bits;
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    // 保守实现：先把所有已写内存对后续所有阶段可见；按 NVN 位收窄留给后续。
    full_barrier(c);
}

#include "backend_vulkan_cold_guard.inc"
#include "backend_vulkan_depth_blit.inc"
#include "backend_vulkan_easu.inc"

// 带缩放 / 换格式的贴图拷贝。源、目标都取视图的第一级第一层；区域按各自那一级的像素给。
// 颜色采用原生 blit；同格式 D24S8/D32FS8 最近点缩放采用辅助绘制，不依赖深度 BLIT 格式位。
// 其它不支持的格式对报 UNSUPPORTED。sRGB 目标按 sRGB 编码（同颜色附件写入）。
void nvn_gpu_blit(nvn_gpu_cmd ch, nvn_gpu_view sv, const NVNcopyRegion *sr, nvn_gpu_view dv, const NVNcopyRegion *dr, int linear) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "传输操作在通道内"); return; }
    if (!sv || !dv || !sv->tex || !dv->tex || !sr || !dr) { set_sticky(c, NVN_GPU_E_ARG, "blit 参数为空"); return; }
    struct nvn_gpu_tex_s *s = sv->tex, *d = dv->tex;
    if (sv->format != s->format || dv->format != d->format) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：视图换了格式（拷贝按存储的格式做）"); return; }
    if ((sv->p16_remap || dv->p16_remap) && s->nvn_format != d->nvn_format) {   // #975：按分量换算，退回格式的分量位置与 NVN 不同
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：16 位打包格式 0x%x → 0x%x 有一边走退回格式，按分量换算会错位", s->nvn_format, d->nvn_format); return;
    }
    if (s->samples > 1 && d->samples == 1) {
        nvn_fmt_info f;
        if (linear || s->w != d->w || s->h != d->h || s->format != d->format || s->nvn_format != d->nvn_format ||
            s->aspect != VK_IMAGE_ASPECT_COLOR_BIT || d->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
            !nvn_fmt_info_of(s->nvn_format, &f) || f.kind == NVN_FMT_UINT || f.kind == NVN_FMT_SINT || f.depth || f.stencil ||
            s->target == NVN_TEXTURE_TARGET_3D || d->target == NVN_TEXTURE_TARGET_3D) {
            // resolve 直接用 VkImage + 子资源范围画，不用采样视图的通道映射：视图带映射（游戏设的 / 没有 alpha 的格式把 a 钉成 ONE）不影响
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "resolve：只支持同格式非整数颜色二维贴图"); return;
        }
        VkOffset3D so, dof; VkExtent3D se, de;
        if (sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0 ||
            sr->depth != 1 || dr->depth != 1 || sr->zoffset || dr->zoffset || copy_region(s, sv->base_level, sr, &so, &se) || copy_region(d, dv->base_level, dr, &dof, &de) ||
            sv->base_layer >= s->layers || dv->base_layer >= d->layers ||
            se.width != de.width || se.height != de.height || se.depth != 1 || de.depth != 1 || so.z || dof.z) {
            set_sticky(c, NVN_GPU_E_ARG, "resolve：区域越界或尺寸不相同"); return;
        }
        full_barrier(c);
        image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve ir = {0};
        ir.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, sv->base_level, sv->base_layer, 1};
        ir.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, dv->base_level, dv->base_layer, 1};
        ir.srcOffset = so; ir.dstOffset = dof; ir.extent = se;
        vkCmdResolveImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ir);
        image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL); image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
        cmd_ref(c, s, REF_TEX); cmd_ref(c, d, REF_TEX);
        return;
    }
    if (nvn_vk_format_is_depth(s->format) || nvn_vk_format_is_depth(d->format)) { depth_blit(c, sv, sr, dv, dr, linear); return; }
    if (s->samples > 1 || d->samples > 1) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：多重采样没接"); return; }
    { nvn_fmt_info a, b; int ai = nvn_fmt_info_of(s->nvn_format, &a) && (a.kind == NVN_FMT_UINT || a.kind == NVN_FMT_SINT), bi = nvn_fmt_info_of(d->nvn_format, &b) && (b.kind == NVN_FMT_UINT || b.kind == NVN_FMT_SINT);
      if (ai || bi) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：整数格式没接"); return; } }
    VkFormatProperties fs, fd; s2a_vk_format_properties(g.phys, s->format, &fs); s2a_vk_format_properties(g.phys, d->format, &fd);
    if (!(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        (linear && !(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：设备不支持这对格式（%d → %d，线性 %d）", (int)s->format, (int)d->format, linear); return; }
    // copy_region 的非正宽高有「整张」含义，不能直接把反向区域交给它。
    NVNcopyRegion normalized = *sr;
    int flip_x = sr->width < 0, flip_y = sr->height < 0;
    // 不扩展同图自拷贝；不同 NVN 身份可能仍别名到同一个后端图像。
    if ((flip_x || flip_y) && s == d) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：同图镜像没接"); return; }
    if (flip_x || flip_y) {
        int64_t x = sr->xoffset, y = sr->yoffset, w = sr->width, h = sr->height;
        if (flip_x) { x += w; w = -w; }
        if (flip_y) { y += h; h = -h; }
        if (x < 0 || y < 0 || x > INT32_MAX || y > INT32_MAX || w <= 0 || h <= 0 ||
            w > INT32_MAX || h > INT32_MAX || sr->zoffset || sr->depth != 1 || dr->width <= 0 || dr->height <= 0 ||
            s->target == NVN_TEXTURE_TARGET_3D || d->target == NVN_TEXTURE_TARGET_3D) {
            set_sticky(c, NVN_GPU_E_ARG, "blit 反向源区域非法"); return;
        }
        normalized.xoffset = (int)x; normalized.yoffset = (int)y;
        normalized.width = (int)w; normalized.height = (int)h;
    }
    VkOffset3D so, dof; VkExtent3D se, de;
    if (copy_region(s, sv->base_level, &normalized, &so, &se) || copy_region(d, dv->base_level, dr, &dof, &de) || sv->base_layer >= s->layers || dv->base_layer >= d->layers) { set_sticky(c, NVN_GPU_E_ARG, "blit 区域越界"); return; }
    full_barrier(c);
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit ib; memset(&ib, 0, sizeof ib);
    ib.srcSubresource = (VkImageSubresourceLayers){s->aspect, sv->base_level, sv->base_layer, 1};
    ib.dstSubresource = (VkImageSubresourceLayers){d->aspect, dv->base_level, dv->base_layer, 1};
    ib.srcOffsets[0] = so; ib.srcOffsets[1] = (VkOffset3D){so.x + (int32_t)se.width, so.y + (int32_t)se.height, 1};
    if (flip_x) { int32_t x = ib.srcOffsets[0].x; ib.srcOffsets[0].x = ib.srcOffsets[1].x; ib.srcOffsets[1].x = x; }
    if (flip_y) { int32_t y = ib.srcOffsets[0].y; ib.srcOffsets[0].y = ib.srcOffsets[1].y; ib.srcOffsets[1].y = y; }
    ib.dstOffsets[0] = dof; ib.dstOffsets[1] = (VkOffset3D){dof.x + (int32_t)de.width, dof.y + (int32_t)de.height, 1};
    vkCmdBlitImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, s, REF_TEX);
    cmd_ref(c, d, REF_TEX);
}

// 之后的绘制计入 slot，-1 停止。只在声明了 occlusion 的通道里；一个槽一段里只能开一次（Vulkan 的查询重置后只能用一回）。
// 设备有精确计数就要精确值，没有就只保证「有样本通过时非 0」。
void nvn_gpu_occlusion(nvn_gpu_cmd ch, int32_t slot) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass || !c->pass_occl) { set_sticky(c, NVN_GPU_E_ORDER, "遮挡查询只能在声明了 occlusion 的通道里开关"); return; }
    if (!c->qpool) { set_sticky(c, NVN_GPU_E_ARG, "遮挡查询：本段开段时没声明 occlusion_slots"); return; }
    if (slot >= 0 && ((uint32_t)slot >= c->qcount || c->q_used[slot])) { set_sticky(c, NVN_GPU_E_ARG, "遮挡查询槽 %d 越界（本段 %u 槽）或本段已用过", slot, c->qcount); return; }
    if (c->q_active >= 0) { vkCmdEndQuery(c->cb, c->q_active_pool, c->q_active_idx); c->q_active = -1; }
    if (slot < 0) return;
    occl_begin(c, c->qpool, (uint32_t)slot);   // 精确 / 快速计数的选择见 occl_begin
    c->q_active = slot; c->q_active_pool = c->qpool; c->q_active_idx = (uint32_t)slot; c->q_used[slot] = 1; if ((uint32_t)slot + 1 > c->qmax_used) c->qmax_used = (uint32_t)slot + 1;
}

void nvn_gpu_timestamp(nvn_gpu_cmd ch, uint32_t slot, uint32_t type) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->recording) { set_sticky(c, NVN_GPU_E_ORDER, "时间戳：命令段未录制"); return; }
    if (slot >= NVN_GPU_TIMESTAMP_SLOTS || (c->report_used & (UINT64_C(1) << slot)) ||
        (type != NVN_COUNTER_TYPE_TIMESTAMP && type != NVN_COUNTER_TYPE_TIMESTAMP_TOP)) {
        set_sticky(c, NVN_GPU_E_ARG, "时间戳：槽越界、重复或计数器类型无效"); return;
    }
    if (!g.report_calibrate) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "时间戳：缺少 CLOCK_MONOTONIC 校准"); return; }
    if (!c->report_ts) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NVN_GPU_TIMESTAMP_SLOTS };
        VkResult r = vkCreateQueryPool(g.d, &qi, NULL, &c->report_ts);
        if (r != VK_SUCCESS) { set_sticky(c, NVN_GPU_E_DEVICE, "时间戳：查询池创建失败 (%d)", r); return; }
        c->report_begin_ns = now_ns();
    }
    vkCmdWriteTimestamp(c->cb, type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ?
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->report_ts, slot);
    c->report_used |= UINT64_C(1) << slot;
    if (c->report_count <= slot) c->report_count = slot + 1;
}

void nvn_gpu_label(nvn_gpu_cmd c, const char *text) { (void)c; (void)text; }

#include "backend_vulkan_deswizzle.inc"
// 上屏 6 个入口在 backend_vulkan_wsi.inc（文件末尾 include：它要用后面定义的提交与回收函数）

// ---------------------------------------------------------------- 命令

nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许开命令段");
    pthread_mutex_lock(&g_mu);
    int closing = g.closing, lost = g.device_lost, failed = g.failed != 0;
    pthread_mutex_unlock(&g_mu);
    if (closing) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    if (failed) return fail(err, NVN_GPU_E_FAILED, "时间线已进入失败终态（恢复只能关会话重开）");
    if (lost) return fail(err, NVN_GPU_E_DEVICE, "设备已丢失");

    struct nvn_gpu_cmd_s *c = calloc(1, sizeof *c);
    if (!c) return fail(err, NVN_GPU_E_OOM, "cmd 分配失败");
    memset(c->desc_sets, 0, sizeof c->desc_sets); c->desc_layout = VK_NULL_HANDLE; c->desc_dirty = 0xfu;
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = g.dev->queue_family;
    if (!seg_reuse_on() || !seg_take_cmd(&c->pool, &c->cb, &c->init_cb)) {   // 空闲表里没有：新建一个池和它的命令缓冲
    OC(OC_CPOOL);
    if (vkCreateCommandPool(g.d, &pci, NULL, &c->pool) != VK_SUCCESS) {
        free(c);
        return fail(err, NVN_GPU_E_OOM, "vkCreateCommandPool 失败");
    }
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.d, &ai, &c->cb) != VK_SUCCESS) {
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_OOM, "vkAllocateCommandBuffers 失败");
    }
    }
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(c->cb, &bi) != VK_SUCCESS) {
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_DEVICE, "vkBeginCommandBuffer 失败");
    }
    c->recording = 1;
    c->xfer_dirty = 1;   // 前置段 / 上一段的传输写：段末屏障只对主机可见，本段第一次派发前补一道
    c->sticky = rc(NVN_GPU_OK);
    c->q_active = -1;
    c->opt = nvn_plat_flag("nvk_opt_layout", 0) != 0;   // 附件 / 采样用对齐语义的布局（缺省 0 = 全 GENERAL 的旧行为）
    c->feedback_on = nvn_plat_flag("nvk_fb_copy", 1) != 0; // 0 仅供回退诊断，不保证反馈采样正确
    c->feedback_read_on = c->feedback_on && g.dev->load_store_op_none_enabled && nvn_plat_flag("nvk_fb_read", 0) != 0;
    c->pool_fb_on = nvn_plat_flag("nvk_pool_fb", 0) != 0;
    {   // 调试读数（debug 开关 nvk_gputime）：段首尾各写一个 GPU 时间戳，完成时算这一段在 GPU 上从开始到结束用了多久
        static int on = -1; if (on < 0) on = nvn_plat_flag("nvk_gputime", 0);
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 + 2 * TS_PASS_MAX;
        if (on && vkCreateQueryPool(g.d, &qi, NULL, &c->tsq) == VK_SUCCESS) {
            vkCmdResetQueryPool(c->cb, c->tsq, 0, 2 + 2 * TS_PASS_MAX);
            vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->tsq, 0);
        } else c->tsq = VK_NULL_HANDLE;
    }
    if (d && d->occlusion_slots) {   // 查询池：整池重置只能在通道外，放在段的最开头
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = d->occlusion_slots;
        c->q_used = calloc(d->occlusion_slots, 1);
        if (c->q_used && seg_reuse_on()) c->qpool = seg_take_q(d->occlusion_slots);
        if (!c->q_used || (!c->qpool && vkCreateQueryPool(g.d, &qi, NULL, &c->qpool) != VK_SUCCESS)) {
            free(c->q_used); vkDestroyCommandPool(g.d, c->pool, NULL); free(c);
            return fail(err, NVN_GPU_E_OOM, "遮挡查询池（%u 槽）创建失败", d->occlusion_slots);
        }
        c->qcount = d->occlusion_slots;
        vkCmdResetQueryPool(c->cb, c->qpool, 0, c->qcount);
    }
    pthread_mutex_lock(&g_mu);
    if (g.closing) {
        pthread_mutex_unlock(&g_mu);
        if (c->tsq) vkDestroyQueryPool(g.d, c->tsq, NULL);
        if (c->qpool) vkDestroyQueryPool(g.d, c->qpool, NULL);
        free(c->q_used);
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    if (g.land_auto) {
        if (g.land_reported == 2) la_end(&g.land_trial);  // 保留退出后迟到的段也按已报决策取模式
        if (g.land_trial.mode && !g.land_trial.reason) land_auto_check_gap_locked(now_ns());
        c->land_mode = g.land_trial.mode;
        c->land_observe = !g.land_trial.reason;
        if (!g.land_trial.reason) {
            c->land_tracked = 1;
            c->land_epoch = g.land_trial.epoch;
            g.land_live_count++;
        }
    } else c->land_mode = g.land_readback;
    c->live_next = g.live;
    g.live = c;
    pthread_mutex_unlock(&g_mu);
    *out = c;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_cmd_abort(nvn_gpu_cmd ch) {
    if (!ch || !g_open) return;
    struct nvn_gpu_cmd_s *c = ch;
    int owned = 0;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (*pp) {
        *pp = c->live_next;
        c->live_next = NULL;
        owned = 1;   // 摘链与销毁所有权原子完成；并发 abort / close 不会重复释放
    }
    pthread_mutex_unlock(&g_mu);
    if (owned) finish_cmd(c, 0);   // 不执行、不回调、不推进时间线；引用随即解除
}

static void enqueue_segment(struct nvn_gpu_vk *vk, struct vk_segment *s) {
    pthread_mutex_lock(&g_mu);
    if (atomic_load(&vk->submitting) == s) atomic_store(&vk->submitting, NULL);
    if (vk->qtail) vk->qtail->next = s;
    else vk->qhead = s;
    vk->qtail = s;
    if (!s->internal && !submitted_add(vk, s->signal) && (!vk->failed || s->signal < vk->failed)) vk->failed = s->signal;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void publish_signal(uint64_t signal, int failed) {
    pthread_mutex_lock(&g_mu);
    g.last_signal = signal;
    if (failed) {
        if (!g.failed || signal < g.failed) g.failed = signal;
    } else {
        g.stats[STAT_SUBMITS]++;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// 持 g_submit_mu，c 已录完。待转表不空：在 c 的池里另录一个命令缓冲，把表里的图像从 UNDEFINED 转到 GENERAL（见「图像布局」），
// 图像各加一份引用记在 c 上（段完成归还；段没执行由 finish_cmd 放回表）。cbs 填本次提交要带的命令缓冲（转布局的在前），
// 回个数；0 = 分配 / 录制失败。
static uint32_t submit_cbs(struct nvn_gpu_cmd_s *c, VkCommandBuffer cbs[2]) {
    pthread_mutex_lock(&g_mu);
    uint32_t n = g_newimg_n;
    struct nvn_gpu_tex_s **v = NULL;
    if (n) {
        v = malloc((size_t)n * sizeof *v);
        if (!v) { pthread_mutex_unlock(&g_mu); return 0; }
        memcpy(v, g_newimg, (size_t)n * sizeof *v);
        for (uint32_t i = 0; i < n; i++) v[i]->refs++;
        g_newimg_n = 0;
    }
    pthread_mutex_unlock(&g_mu);
    if (!n && !c->report_ts) { cbs[0] = c->cb; return 1; }
    c->newimg = v; c->n_newimg = n;
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (!c->init_cb && vkAllocateCommandBuffers(g.d, &ai, &c->init_cb) != VK_SUCCESS) { c->init_cb = VK_NULL_HANDLE; return 0; }
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(c->init_cb, &bi) != VK_SUCCESS) return 0;
    // 首次报告可能在 render pass 内；reset 必须在独立前置缓冲中，不能打断通道。
    if (c->report_ts) {
        vkCmdResetQueryPool(c->init_cb, c->report_ts, 0, NVN_GPU_TIMESTAMP_SLOTS);
        vkCmdPipelineBarrier(c->init_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    }
    for (uint32_t i = 0; i < n; i++) {
        VkImageMemoryBarrier b;
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = v[i]->image;
        b.subresourceRange = (VkImageSubresourceRange){v[i]->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(c->init_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    if (vkEndCommandBuffer(c->init_cb) != VK_SUCCESS) return 0;
    cbs[0] = c->init_cb; cbs[1] = c->cb;
    return 2;
}

// 提交串行化：signal 校验、vkEndCommandBuffer、vkQueueSubmit、入队必须在同一临界区，
// 否则两个提交线程可能让更大的 signal 先入队，或同时通过 last_signal 检查。
static nvn_gpu_rc commit_impl(nvn_gpu_cmd ch, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    struct nvn_gpu_cmd_s *c = ch;

    // 已成功开段便有退休记录；不能在 commit OOM 时丢 done，也不能同步回调越过中途段。
    struct vk_segment *s = &c->segment;
    s->embedded = 1;
    s->cmd = c;
    s->signal = signal;
    s->done = done;
    s->user = user;
    s->cb = c->cb;

    pthread_mutex_lock(&g_submit_mu);

    pthread_mutex_lock(&g_mu);
    int closing = g.closing, lost = g.device_lost, inject = g.fault_inject, failed_state = g.failed != 0;
    if (inject) g.fault_inject = 0;
    uint64_t last = g.last_signal;
    pthread_mutex_unlock(&g_mu);

    // 契约 v2 的 commit：signal 必须严格大于上一次；本实现同时拒绝 0（F = 0 表示「没有失败」，
    // 无信号段失败后无法表达；见文件头消费审查第 2 条）。
    if (signal == 0 || signal <= last) {
        // 违约被拒：不提交、不推进 last_signal，但按失败处理——发布 F = 上一次合法值 + 1，
        // 再以 FAILED（rc = E_ARG）回调本段一次，signal 用调用方给的原值。
        uint64_t f = add_sat(last, 1);
        pthread_mutex_lock(&g_mu);
        g.failed = g.failed ? (f < g.failed ? f : g.failed) : f;
        pthread_mutex_unlock(&g_mu);
        s->failed = 1;
        s->rc = rc(NVN_GPU_E_ARG);
        snprintf(s->why, sizeof s->why, "signal %llu 未严格大于上一次合法提交 %llu（F=%llu）",
                 (unsigned long long)signal, (unsigned long long)last, (unsigned long long)f);
        nvn_gpu_rc result = fail(err, NVN_GPU_E_ARG, "%s", s->why);
        enqueue_segment(&g, s);
        pthread_mutex_unlock(&g_submit_mu);
        return result;   // worker owns s after enqueue; never read its why here
    }

    if (c->in_pass) set_sticky(c, NVN_GPU_E_ORDER, "commit 时通道还开着");
    nvn_gpu_rc sticky = c->sticky;
    if (!rc_is(sticky, NVN_GPU_OK) || closing || lost || inject || failed_state) {
        nvn_gpu_rc code = !rc_is(sticky, NVN_GPU_OK) ? sticky
                        : rc(closing ? NVN_GPU_E_STOPPED : (lost ? NVN_GPU_E_DEVICE : (failed_state ? NVN_GPU_E_FAILED : NVN_GPU_E_DEVICE)));
        const char *why = !rc_is(sticky, NVN_GPU_OK) ? c->why
                        : (closing ? "会话正在关闭" : (lost ? "设备已丢失" : (failed_state ? "时间线已失败，本段不执行" : "测试注入：本段提交失败")));
        s->failed = 1;
        s->rc = code;
        snprintf(s->why, sizeof s->why, "%s", why);
        fail(err, code.code, "%s", s->why);   // preserve the first cause before worker takes ownership
        publish_signal(signal, 1);
        enqueue_segment(&g, s);
        pthread_mutex_unlock(&g_submit_mu);
        return code;
    }

    // 段末：本段动过的图一律转回 GENERAL（段与段之间的静止布局约定，见「图像布局」）。
    lay_rest_at_commit(c);
    // 段末：让本段的所有 GPU 写对主机可见（fence 只保证执行完，不保证主机可见）。
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);
    if (c->tsq) vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 1);
    // 结束录制并提交。
    VkResult r = vkEndCommandBuffer(c->cb);
    if (r == VK_SUCCESS) {
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        r = vkCreateFence(g.d, &fi, NULL, &fence);
        if (r == VK_SUCCESS) {
            VkSubmitInfo si;
            memset(&si, 0, sizeof si);
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            VkCommandBuffer cbs[2];
            si.commandBufferCount = submit_cbs(c, cbs);
            si.pCommandBuffers = cbs;
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
            if (si.commandBufferCount) {
                atomic_store(&g.submitting, s);
                r = vkQueueSubmit(g.queue, 1, &si, fence);
            }
            if (r == VK_SUCCESS) {
                s->fence = fence;
                s->submitted = 1;
                s->executed = 1;
                publish_signal(signal, 0);
                if (g.cold_enabled) cold_commit_writes(c);
                enqueue_segment(&g, s);   // 此后 cmd 归 worker（finish_cmd 负责摘链）
                pthread_mutex_unlock(&g_submit_mu);
                return rc(NVN_GPU_OK);
            }
            if (r == VK_ERROR_DEVICE_LOST) device_loss_report(&g, s, LOSS_SUBMIT_UNCERTAIN);
            atomic_store(&g.submitting, NULL);
            vkDestroyFence(g.d, fence, NULL);
            err = err ? err : &(nvn_gpu_err){0};
            fail(err, NVN_GPU_E_DEVICE, "vkQueueSubmit 失败");
        } else {
            err = err ? err : &(nvn_gpu_err){0};
            fail(err, NVN_GPU_E_OOM, "vkCreateFence 失败");
        }
    } else {
        err = err ? err : &(nvn_gpu_err){0};
        fail(err, NVN_GPU_E_DEVICE, "vkEndCommandBuffer 失败");
    }

    // 提交前失败：段不执行，按失败终止发布 F，并给本段恰好一次 FAILED 回调。
    s->failed = 1;
    s->rc = err->rc;
    snprintf(s->why, sizeof s->why, "%s", err->why);
    publish_signal(signal, 1);
    enqueue_segment(&g, s);
    pthread_mutex_unlock(&g_submit_mu);
    return err->rc;
}

nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd ch, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    if (!ch) return fail(err, NVN_GPU_E_ARG, "cmd 为空");
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许提交");
    struct nvn_gpu_cmd_s *c = ch;
    pthread_mutex_lock(&g_mu);
    if (g.closing) {   // 交给 close 的第①步统一丢弃，避免和它在同一 cmd 上重复销毁
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (!*pp) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ORDER, "cmd 已失效（abort / close 已接管）");
    }
    *pp = c->live_next;
    c->live_next = NULL;
    g.commit_active++;
    pthread_mutex_unlock(&g_mu);
    nvn_gpu_rc r = commit_impl(ch, signal, done, user, err);
    pthread_mutex_lock(&g_mu);
    g.commit_active--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t nvn_gpu_fence_done(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.completed;
    pthread_mutex_unlock(&g_mu);
    return v;
}

uint64_t nvn_gpu_fence_failed(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.failed;
    pthread_mutex_unlock(&g_mu);
    return v;
}

nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许等栅栏");
    if (v == 0) return fail(err, NVN_GPU_E_ARG, "v = 0 不是提交过的值");
    pthread_mutex_lock(&g_mu);
    if (g.completed >= v) {   // 已完成的旧值不再保留在提交集合里
        pthread_mutex_unlock(&g_mu);
        return rc(NVN_GPU_OK);
    }
    if (g.failed && v >= g.failed) {
        uint64_t f = g.failed;
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_FAILED, "时间线首个失败段 F=%llu", (unsigned long long)f);
    }
    if (!submitted_has(&g, v)) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ARG, "v=%llu 从未提交过", (unsigned long long)v);
    }
    if (timeout_ns == 0) {
        pthread_mutex_unlock(&g_mu);
        return rc(NVN_GPU_E_TIMEOUT);
    }
    struct timespec ts;
    deadline_after(&ts, timeout_ns);
    while (g.completed < v) {
        if (g.failed && v >= g.failed) {
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_FAILED, "时间线首个失败段 F=%llu", (unsigned long long)g.failed);
        }
        if (g.device_lost) {
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_DEVICE, "设备已丢失");
        }
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) != 0) {
            if (g.completed < v) {
                pthread_mutex_unlock(&g_mu);
                return rc(NVN_GPU_E_TIMEOUT);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 读数 / 测试钩子

void nvn_gpu_stats(nvn_gpu_stat_fn fn, void *user, int take) {
    if (!fn || !g_open) return;
    uint64_t v[STAT_N];
    pthread_mutex_lock(&g_mu);
    memcpy(v, g.stats, sizeof v);
    if (take) memset(g.stats, 0, sizeof g.stats);
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < STAT_N; i++) fn(user, stat_names[i], v[i]);
    // 描述符组按布局回收（#1096）：开会话起累计，不随 take 清零
    pthread_mutex_lock(&g_dset_mu);
    const uint64_t ds_new = g_dset_new, ds_reuse = g_dset_reuse, ds_put = g_dset_put, ds_freed = g_dset_freed, ds_taken = g_dset_taken, ds_pools = g_dset_npools, ds_bins = g_dset_nbins, ds_sigmiss = g_dset_sigmiss, ds_capped = g_dset_capped;
    uint64_t ds_idle = 0;
    for (const struct dset_bin *b = g_dset_bins; b; b = b->next) ds_idle += b->nfree;
    pthread_mutex_unlock(&g_dset_mu);
    fn(user, "dset_taken", ds_taken);   // 交给 alloc_sets 的组（复用 + 新分的第一组）
    fn(user, "dset_new", ds_new);       // 向驱动新分的组（每个表项第一次 1 组、之后翻倍，封顶 DSET_BATCH 组）
    fn(user, "dset_reuse", ds_reuse);   // 从空闲串复用的组
    fn(user, "dset_put", ds_put);       // 段完成放回空闲串的组
    fn(user, "dset_freed", ds_freed);   // 放弃的段还给池的组
    fn(user, "dset_busy", __atomic_load_n(&g_dset_busy, __ATOMIC_RELAXED));   // 回放线程取组碰上锁被持着、退回老路径的次数
    fn(user, "dset_pools", ds_pools);   // 增长池个数
    fn(user, "dset_bins", ds_bins);     // 表项数（本会话登记过的布局结构数）
    fn(user, "dset_idle", ds_idle);     // 空闲串里现有的组
    fn(user, "dset_sigmiss", ds_sigmiss);   // 这一笔的写法和表项里定下的不同、退回老路径的次数（稳态应为 0 或很少）
    fn(user, "dset_capped", ds_capped);     // 向驱动要组时已到总量上限、退回老路径的次数（稳态应为 0）
    // 会话级读数（开会话起累计，不随 take 清零）：驱动管线缓存（#822）
    fn(user, "pipe_cache_save_trigger_ns", __atomic_load_n(&g_plcw_get_ns, __ATOMIC_RELAXED));
    fn(user, "pipe_warm_created", __atomic_load_n(&g_warm_created, __ATOMIC_RELAXED));   // 预建出来的管线对象数
    fn(user, "pipe_warm_hits", __atomic_load_n(&g_warm_hits, __ATOMIC_RELAXED));         // 真实 draw 第一次拿到预建对象的次数
    fn(user, "pipe_install_dup", __atomic_load_n(&g_pipe_dup, __ATOMIC_RELAXED));
    fn(user, "occl_cont_used", __atomic_load_n(&g_occl_cont_used, __ATOMIC_RELAXED));   // 遮挡查询续段开出次数（进程累计，不随会话清零）        // 双检抓到「别人已经装过同配方」的次数（新建那份已销毁）
    {   // 在途表读数来自 nvn_pipe_flight（模块内计数）：合并等待 / 表满自己建 / 认领后复检命中 / 已发布 / 失败
        uint64_t pw = 0, pf = 0, pl = 0, pp = 0, pfail = 0;
        nvk_pf_stats(&g_pi, &pw, &pf, &pl, &pp, &pfail);
        fn(user, "pipe_inflight_wait", pw);       // 同配方需求合并到在途创建、等它建完的次数
        fn(user, "pipe_inflight_full", pf);       // 在途表满 128（或初始化失败停用）：这一次不合并、自己建
        fn(user, "pipe_inflight_late_hit", pl);   // 认领后复检缓存命中：省掉一次重复创建
    }
    fn(user, "pipe_cache_loaded_bytes", g_plc_loaded);                              // 开会话时读到并交给驱动的字节数（0 = 没读到）
    fn(user, "pipe_cache_rejected", g_plc_rejected);                                // 旧文件读不出 / 驱动不收、被丢掉的次数
    fn(user, "pipes_created", __atomic_load_n(&g_plc_made, __ATOMIC_RELAXED));      // 经缓存建出的管线条数（图形 + 计算 + 清屏）
    // 局中补写（#868）；进程累计，不随会话清零（后台写可能跨过 close 才收尾）
    fn(user, "pipe_cache_saves", __atomic_load_n(&g_plcw_started, __ATOMIC_RELAXED));         // 交给后台写盘的次数
    fn(user, "pipe_cache_saves_ok", __atomic_load_n(&g_plcw_ok, __ATOMIC_RELAXED));           // 其中写成功的
    fn(user, "pipe_cache_saves_failed", __atomic_load_n(&g_plcw_fail, __ATOMIC_RELAXED));     // 其中写失败的（只记日志；收尾时会再试）
    fn(user, "pipe_cache_saves_same", __atomic_load_n(&g_plcw_same, __ATOMIC_RELAXED));       // 触发了但数据大小与上次保存一致、没写的次数
    // 命令段暂存缓冲的回收表（开会话起累计，不随 take 清零）
    pthread_mutex_lock(&g_seg_mu);
    uint64_t sh = g_seg_stg_hit, sm = g_seg_stg_miss, sk = g_seg_stg_kept, sr = g_seg_stg_rej, sf = g_seg_stg_full;
    pthread_mutex_unlock(&g_seg_mu);
    fn(user, "stg_pool_hit", sh);        // 要暂存块时表里有同大小的、直接再用的次数
    fn(user, "stg_pool_miss", sm);       // 没有同大小的、新建的次数
    fn(user, "stg_pool_kept", sk);       // 放回时留进表里的次数
    fn(user, "stg_pool_rejected", sr);   // 放回时因为不是标准大小（nvk_stg_std）没留、照旧销毁的次数
    fn(user, "stg_pool_full", sf);       // 放回时因为表满没留、照旧销毁的次数
    // 暂存写读数（开会话起累计；stg_chunk 一节）
    fn(user, "stg_write_n", __atomic_load_n(&g_stg_wr[0], __ATOMIC_RELAXED));         // nvn_gpu_buf_write 的笔数
    fn(user, "stg_write_bytes", __atomic_load_n(&g_stg_wr[1], __ATOMIC_RELAXED));     // 其中写入的字节
    fn(user, "stg_write_copies", __atomic_load_n(&g_stg_wr[2], __ATOMIC_RELAXED));    // 录出的暂存到缓冲拷贝命令条数（分块开着时一笔可以是多条）
    fn(user, "stg_write_big", __atomic_load_n(&g_stg_wr[3], __ATOMIC_RELAXED));       // 一笔超过一个标准块的笔数
    fn(user, "stg_new_blocks", __atomic_load_n(&g_stg_wr[4], __ATOMIC_RELAXED));      // 回收表里没取到、向驱动新建的暂存块个数（含所有 arena_alloc 来源）
    fn(user, "stg_new_bytes", __atomic_load_n(&g_stg_wr[5], __ATOMIC_RELAXED));       // 这些新建块的字节
    fn(user, "shader_missing_vertex_stores", __atomic_load_n(&g_vfeat_missing[0], __ATOMIC_RELAXED));   // 顶点前阶段写存储、设备没启用 vertexPipelineStoresAndAtomics 的着色器数
    fn(user, "shader_missing_clip_distance", __atomic_load_n(&g_vfeat_missing[1], __ATOMIC_RELAXED));
    fn(user, "shader_missing_cull_distance", __atomic_load_n(&g_vfeat_missing[2], __ATOMIC_RELAXED));
    fn(user, "spv_unlisted_capability", __atomic_load_n(&g_spv_unlisted_cap, __ATOMIC_RELAXED));   // 现编件声明表外能力、具名计数放行次数（库件同情形按拒收）
    fn(user, "spv_unlisted_extension", __atomic_load_n(&g_spv_unlisted_ext, __ATOMIC_RELAXED));
    fn(user, "spv_device_limit_rejected", __atomic_load_n(&g_spv_devlimit_reject, __ATOMIC_RELAXED));   // 本机上限（特性 / 限值 / apiVersion / 启用扩展）核对拒收次数
    fn(user, "sampler_border_color_nearest", __atomic_load_n(&g_border_nearest, __ATOMIC_RELAXED));   // 没有自定义边框色扩展、边框色就近取内置档建成的采样器数（进程累计）
}

// 用例读：视图全局链与帧缓冲缓存的当前账。0 现存视图数；1 旧路径（nvk_ulist=0）摘链累计走过的节点数（缺省路径恒为 0）；
// 2 帧缓冲缓存里现存的项数；3 自检档（nvk_fbdrop=2）扫到的残留数（缺省档恒为 0）；
// 4 视图链的结构违例数（每个节点：前驱指针指回自己、后继的反向指针指回本节点的 all_next，应恒为 0）；
// 5 让 nvk_ulist / nvk_fbdrop 两个开关的缓存作废（用例改了开关值后立刻生效，返回 0）；6 帧缓冲缓存全表扫的累计次数。
// 动态 UBO 偏移（#1233）的用例读数钩子（只给宿主单测，C208）：0 = 建管线时选了动态 UBO 的管线条数，1 = 建过的带 UBO 的管线条数（进程内累计，用例取前后之差），
// 2 = 设备 maxDescriptorSetUniformBuffersDynamic（开会话时读），3 = 绑定表里「只换偏移」直接放过的项数（进程内累计，用例取前后之差）。
uint64_t nvn_vk_dbg_dyn_stat(int which);
uint64_t nvn_vk_dbg_dyn_stat(int which) {
    if (which == 0) return __atomic_load_n(&g_dyn_pipes[0], __ATOMIC_RELAXED);
    if (which == 1) return __atomic_load_n(&g_dyn_pipes[1], __ATOMIC_RELAXED);
    if (which == 2) return g_max_dyn_ubo;
    if (which == 3) return __atomic_load_n(&g_dyn_fast, __ATOMIC_RELAXED);
    return 0;
}
// 用例把「设备动态 UBO 上限」压低到 v（只降不升；v = 0 还原成设备真值）：宿主设备（lavapipe）的上限比真机大，要核对「刚好在上限」「超上限一个」的取舍只能这样压。
// 只在会话已打开、没有管线正在建的时候调（用例线程同步建管线）。
void nvn_vk_dbg_dyn_limit(uint32_t v);
void nvn_vk_dbg_dyn_limit(uint32_t v) {
    g_max_dyn_ubo = (v && v < g_max_dyn_ubo_dev) ? v : g_max_dyn_ubo_dev;
}
// 用例把动态 UBO 偏移的拆分点压到 bits 位（偏移的低 bits 位走动态偏移、其余高位放描述符；bits = 0 或 ≥ 32 还原成生产的 32 位）。
// 只在没有命令段在录的时候调（用例线程同步录、同步等完成）。
void nvn_vk_dbg_dyn_split(uint32_t bits);
void nvn_vk_dbg_dyn_split(uint32_t bits) {
    g_dyn_lo_mask = (bits && bits < 32) ? ((1ull << bits) - 1) : 0xffffffffull;
}
// 描述符组按布局回收的用例读数钩子（只给宿主单测）：0 新分（向驱动）/ 1 复用 / 2 放回 / 3 放弃的段还给池 / 4 交给 alloc_sets 的总数 /
// 5 空闲串里现有的组（沿登记表遍历数出来的）/ 6 表项数（登记的计数）/ 7 增长池个数 / 8 dead 的表项数（一组也放不进空池的布局）/ 9 回收开着吗（开会话时读的开关）/
// 10 到总量上限、退回老路径的次数 / 11 取组碰锁退回老路径的次数（不要锁）/
// 13 沿登记表遍历数出来的表项数（应恒等于 6：对不上就是链表丢了项）/ 14 写法不同退回老路径的次数 / 15 回收表代际（每次开会话 +1）/ 26 当前的总量上限。
uint64_t nvn_vk_dbg_dset_stat(int which);
uint64_t nvn_vk_dbg_dset_stat(int which) {
    uint64_t r = 0;
    if (which == 9) return (uint64_t)__atomic_load_n(&g_dset_on, __ATOMIC_RELAXED);
    if (which == 11) return __atomic_load_n(&g_dset_busy, __ATOMIC_RELAXED);
    if (which >= 19 && which <= 25) {   // 布局表（持 g_dlay_mu）：19 布局结构数、20 取布局次数、21 其中命中、22 散列桶数、23 最长桶链、24 累计向驱动建过的布局数、25 累计销毁的布局数
        pthread_mutex_lock(&g_dlay_mu);
        switch (which) {
        case 19: r = g_dlay_n; break;
        case 20: r = g_dlay_ask; break;
        case 21: r = g_dlay_hit; break;
        case 22: r = g_dlay_ht_n; break;
        case 23: for (uint32_t i = 0; i < g_dlay_ht_n; i++) { uint64_t c = 0; for (const struct dset_lay *e = g_dlay_ht[i]; e; e = e->hnext) c++; if (c > r) r = c; } break;
        case 24: r = g_dlay_made; break;
        default: r = g_dlay_gone; break;
        }
        pthread_mutex_unlock(&g_dlay_mu);
        return r;
    }
    pthread_mutex_lock(&g_dset_mu);
    switch (which) {
    case 0: r = g_dset_new; break;
    case 1: r = g_dset_reuse; break;
    case 2: r = g_dset_put; break;
    case 3: r = g_dset_freed; break;
    case 4: r = g_dset_taken; break;
    case 5: for (const struct dset_bin *b = g_dset_bins; b; b = b->next) r += b->nfree; break;
    case 6: r = g_dset_nbins; break;
    case 7: r = g_dset_npools; break;
    case 8: for (const struct dset_bin *b = g_dset_bins; b; b = b->next) r += b->dead; break;
    case 10: r = g_dset_capped; break;
    case 13: for (const struct dset_bin *b = g_dset_bins; b; b = b->next) r++; break;
    case 14: r = g_dset_sigmiss; break;
    case 15: r = g_dset_gen; break;
    case 26: r = g_dset_cap; break;
    default: break;
    }
    pthread_mutex_unlock(&g_dset_mu);
    return r;
}
// 用例（只给宿主单测）：直接按结构取一个布局（不经管线），k4 = nb 条 {binding, 类型, 数量, 阶段} 四元组；返回布局句柄的数值，0 = 失败。会话开着才能用；布局归布局表，会话收尾销毁。
uint64_t nvn_vk_dbg_dlay_get(const uint32_t *k4, uint32_t nb);
uint64_t nvn_vk_dbg_dlay_get(const uint32_t *k4, uint32_t nb) {
    VkDescriptorSetLayoutBinding b[32];
    if (nb > 32) return 0;
    for (uint32_t i = 0; i < nb; i++) {
        memset(&b[i], 0, sizeof b[i]);
        b[i].binding = k4[4 * i]; b[i].descriptorType = (VkDescriptorType)k4[4 * i + 1]; b[i].descriptorCount = k4[4 * i + 2]; b[i].stageFlags = (VkShaderStageFlags)k4[4 * i + 3];
    }
    int oom = 0;
    const struct dset_lay *e = dlay_get(b, nb, &oom);
    return e ? (uint64_t)(uintptr_t)e->dsl : 0;
}
// 用例注入（只给宿主单测）：0 不注入；1 一批（> 1 组）向驱动要组时回「池耗尽」，模拟一批放不进空池；2 任何一次向增长池要组都回「池耗尽」，模拟一组也放不进空池。
void nvn_vk_dbg_dset_inject(int mode);
void nvn_vk_dbg_dset_inject(int mode) { __atomic_store_n(&g_dset_test_reject, mode, __ATOMIC_RELAXED); }
// 用例注入（只给宿主单测）：把总量上限改成 n 组（本会话内有效，下次开会话复位成 DSET_MAX_SETS）。
void nvn_vk_dbg_dset_cap(uint32_t n);
void nvn_vk_dbg_dset_cap(uint32_t n) { pthread_mutex_lock(&g_dset_mu); g_dset_cap = n; pthread_mutex_unlock(&g_dset_mu); }
// 用例注入（只给宿主单测）：接下来 n 次登记表项当作内存不够（0 = 取消）。被注入的那几次取组退回老路径、不记结果，下一次取组再登记。
void nvn_vk_dbg_dset_failreg(int n);
void nvn_vk_dbg_dset_failreg(int n) { pthread_mutex_lock(&g_dset_mu); g_dset_test_failreg = n > 0 ? n : 0; pthread_mutex_unlock(&g_dset_mu); }
// 用例（只给宿主单测）：1 = 调用线程持着回收表的锁，回放 / 录制线程取组时 trylock 碰锁、退回老路径；0 = 放开。持着期间不能调用要这把锁的读数（nvn_vk_dbg_dset_stat 的 9 / 11 不要锁）。
void nvn_vk_dbg_dset_hold(int on);
void nvn_vk_dbg_dset_hold(int on) { if (on) pthread_mutex_lock(&g_dset_mu); else pthread_mutex_unlock(&g_dset_mu); }

uint64_t nvn_vk_dbg_view_stat(int which) {
    uint64_t r = 0;
    switch (which) {
    case 0: pthread_mutex_lock(&g_mu); for (struct nvn_gpu_view_s *x = g.views; x; x = x->all_next) r++; pthread_mutex_unlock(&g_mu); break;
    case 1: r = __atomic_load_n(&g_dv_steps_total, __ATOMIC_RELAXED); break;
    case 2: pthread_mutex_lock(&g_pc_mu); for (uint32_t i = 0; i < PC_FB_N; i++) if (g_pc_fb[i].fb) r++; pthread_mutex_unlock(&g_pc_mu); break;
    case 3: r = __atomic_load_n(&g_fbdrop_resid, __ATOMIC_RELAXED); break;
    case 5: g_ulist_cur = g_fbdrop_cur = -1; break;
    case 6: r = __atomic_load_n(&g_pc_scan_n, __ATOMIC_RELAXED); break;
    case 4:
        pthread_mutex_lock(&g_mu);
        for (struct nvn_gpu_view_s *x = g.views; x; x = x->all_next) {
            if (!x->all_pprev || *x->all_pprev != x) r++;
            if (x->all_next && x->all_next->all_pprev != &x->all_next) r++;
        }
        if (g.views && g.views->all_pprev != &g.views) r++;
        pthread_mutex_unlock(&g_mu);
        break;
    default: break;
    }
    return r;
}

// 测试钩子：让某个 signal 的段在 fence 等待时报失败（走真实的失败终止路径）。
void nvn_gpu_vk_test_fail_fence(uint64_t signal) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.test_fail_signal = signal;
    pthread_mutex_unlock(&g_mu);
}

// 测试钩子：让某个 signal 的段先睡 ms，用来测 close 的 TIMEOUT 路径。
void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.test_delay_signal = signal;
    g.test_delay_ms = ms;
    pthread_mutex_unlock(&g_mu);
}

// 测试钩子（同步校验的检测器负控）：on = 0 时派发前不再按传输写补屏障，用来证明用例测得出缺屏障。只给测试用。
void nvn_gpu_vk_test_xfer_barrier(int on) { g_test_no_xfer_bar = !on; }

// 测试钩子（C207）：直接开 / 关诊断计时（nvk_gputime 的效果，g_draw_tm），并把逐笔读数的周期改成 draw_log_ns 纳秒（0 = 还原 5 秒）。只给测试用。
void nvn_gpu_vk_test_diag_timing(int on, uint64_t draw_log_ns) {
    __atomic_store_n(&g_draw_log_ns, draw_log_ns ? draw_log_ns : 5000000000ull, __ATOMIC_RELAXED);
    g_draw_tm = on != 0;
}

// 测试钩子（与 vk_clear 的 s2a_vk_clear_set_submit_fault 同一口径）：下一段 commit 按提交失败处理。
void nvn_gpu_vk_test_set_fault(int mode) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.fault_inject = mode;
    pthread_mutex_unlock(&g_mu);
}

#include "backend_vulkan_wsi.inc"

#ifdef S2A_NVK_BACKEND_HOST_TEST
// ---- 宿主单测入口（只有宿主测试的 cc 命令行定义 S2A_NVK_BACKEND_HOST_TEST；正式构建里这段不存在）----
// 目的：让 android/tests/nvk_pipe_inflight_test.c 用真实的 nvn_gpu_pipe_get / pipe_build / 安装双检
// 跑并发用例，而不需要真设备、worker 线程与上屏链路。测试自己提供 Vk* 桩和一个 struct s2a_vk_device
// （instance / physical / device / queue 填非空假值），并记录驱动侧调用。
// 这里只置管线这条路要用的会话状态：g.dev / g.d、能力表、驱动管线缓存；不起 worker、不碰段 / 缓冲 /
// 贴图 / 上屏。plc_open 走真实路径，测试能核对 VkPipelineCacheCreateInfo.flags（并发依据）。
// 测试结束调 nvn_gpu_vk_test_close_pipe_only 收掉管线、着色器与缓存。
void nvn_gpu_vk_test_open_pipe_only(struct s2a_vk_device *dev) {
    if (g_open || !dev || !dev->device) return;
    memset(&g, 0, sizeof g);
    g.dev = dev;
    g.d = dev->device;
    g.phys = dev->physical;
    g.queue = dev->queue;
    nvn_gpu_open_desc od;
    memset(&od, 0, sizeof od);
    fill_caps(&g, &od);
    plc_open(NULL);   // 缓存目录为空：只建缓存、不读写盘；nvk_pipe_cache=0 时 g_plc = VK_NULL_HANDLE
    g_open = 1;
}

void nvn_gpu_vk_test_close_pipe_only(void) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g_open = 0;   // 之后 nvn_gpu_pipe_get / nvn_gpu_shader_load 入口按「会话关闭」拒收
    struct nvn_gpu_pipe_s *p = g.pipes;
    g.pipes = NULL;
    if (g_pipe_ht) memset(g_pipe_ht, 0, g_pipe_ht_n * sizeof *g_pipe_ht);
    memset(g_cpipe_ht, 0, sizeof g_cpipe_ht);
    struct nvn_gpu_shader_s *sh = g.shaders;
    g.shaders = NULL;
    pthread_mutex_unlock(&g_mu);
    while (p) { struct nvn_gpu_pipe_s *pn = p->all_next; destroy_pipe_objects(p); p = pn; }
    while (sh) {
        struct nvn_gpu_shader_s *sn = sh->all_next;
        if (sh->mod) vkDestroyShaderModule(g.d, sh->mod, NULL);
        free(sh->ents);
        free(sh->tfmt);
        free(sh->write_bounds);
        free(sh);
        sh = sn;
    }
    pthread_mutex_lock(&g_plc_mu);
    VkPipelineCache pc = g_plc;
    g_plc = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_plc_mu);
    if (pc) vkDestroyPipelineCache(g.d, pc, NULL);
}
#endif
#include "backend_vulkan_pool.inc"
