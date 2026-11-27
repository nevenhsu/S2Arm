// NVN 回放后端契约 v2（W1「回放核心共用」D0；设计文档 docs/设计_回放核心共用.md；v2 按 #968 vickwv 10-29 06:55 审查定稿）。
//
// 分层（自上而下）：
//   前端 hle_nvn.c / hle_vk.m / egl.m ──nvn_be_*（nvn_mtl.h / nvn_obj.h，回放核心的上沿，形状不变）──▶
//   回放核心（与图形 API 无关：命令解码、状态追踪、资源身份、着色器变体选择、宽屏挂点、帧节拍；只写一份）──nvn_gpu_*（本文件，下沿）──▶
//   后端（Metal：从 nvn_mtl.m 抽出；Vulkan：替换安卓的 mtl_vk_shim.c + nvn_be_compat.h 宏转接）。
//   显示节拍源不在本文件：归平台层，见 nvn_vsync.h（定稿 #9）。
//
// 本文件只定契约，D0 不接任何实现。编译自洽由 graphics/nvn/tests/t_nvn_backend.c 证明；签名表和纯编译自洽并不证明下面的语义已满足，
// 语义由设计文档 §8 的契约用例清单在实现期回归。
//
// 约定（每个函数的注释按这四项写：语义 / 线程 / 生命周期 / 出错）：
//   词汇：状态与枚举一律用 NVN 自己的记录（nvn_obj.h：blend_st / ds_st / va_st / samp_par / view_t / nvn_texdesc …；格式 = NVNformat 数值；
//         反射来源 = nvn_refl.h 的 NVN_RB_*）。不另造中间枚举：NVN → 原生 API 的翻译只在后端里做一次。
//   句柄：全部不透明，本文件不出现任何 Metal / Vulkan 类型；注释里提到的 MTL* / Vk* 只是给审阅者的对照。
//   线程：
//     [任意]  线程安全，可从任何线程调用；
//     [提交]  回放线程：同一个 nvn_gpu_cmd 只在一个线程上编码（Metal 编码线程 / 安卓翻译 worker 都算这一角色），不同 cmd 互不相干；
//     [上屏]  上屏线程或显示节拍回调线程；
//     [后台]  后台编译线程（管线 / 着色器预热）；
//     [回调]  后端自己的完成回调线程：回调里只许调 [任意] 函数，不许编码、不许提交、不许等栅栏。
//   生命周期（定稿 A3）：句柄由核心 release；release 随时可调，后端负责把真正销毁推迟到该句柄「最后一次使用」结束。使用包括四种：
//     ① 已录制进一个尚未提交的 cmd；② 已提交、尚未完成的 cmd 引用了它；③ 后端自己的后台工作在用（上传暂存、管线编译读着色器、present blit）；
//     ④ 经 GPU 地址间接引用——后端看不到这种引用，由核心对每个这样的缓冲在该 cmd 里调 nvn_gpu_buf_use 声明，声明即保活（Vulkan 可省驻留操作，不能省保活）。
//     ①的 cmd 若被 nvn_gpu_cmd_abort 丢弃，它的引用随之解除。release 之后再录制使用它 = 契约违规（粘滞 NVN_GPU_E_ARG）。
//   出错（报红，不返回假成功；定稿 A7 错误域独立）：
//     · 可失败的函数返回 nvn_gpu_rc（单成员结构体，不能隐式或显式转成 int，见 nvn_gpu_rc 处）；非 OK 时 err->why 写清原因，输出句柄置 NULL；
//       码值在独立区间 [-1099, -1000]，与安卓 native–NativeAOT 会话 ABI（s2a_direct_result：1 … -9）不相交；跨到会话 ABI 的接点必须显式映射（设计文档 §3.3 表）。
//     · 后端不支持的功能返回 NVN_GPU_E_UNSUPPORTED，不画近似结果；
//     · 编码类函数返回 void：违规记成该 cmd 的粘滞错误（首错原文）。带粘滞错误的 cmd 整段不提交（不静默丢一条命令），commit 返回该错误，按失败段处理（见 nvn_gpu_cmd_commit）。
//   取舍由核心定：GPU 故障后跳过这一笔并记住（隔离，nvn_mtl.m quar_*）还是整局停，是核心的策略，后端只如实报。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"    // NVN 状态记录：nvn_texdesc / view_t / samp_par / blend_st / color_st / chmask_st / ms_st / poly_st / ds_st / va_st / vs_st / p_poly_offset / NVNcopyRegion
#include "nvn_refl.h"   // 反射来源 NVN_RB_*（布局项的 src_kind）

#ifdef __cplusplus
extern "C" {
#endif

#define NVN_GPU_CONTRACT_VERSION 26   // 改任何签名或语义 +1；两个后端与核心同批改，t_nvn_backend.c 的签名表同批改
/* 定稿 #1（调用形式）：链接期直接调用 nvn_gpu_*。每个平台构建只链一个后端，Metal 3 / 4 的选择留在 Metal 后端内部。
   理由：零间接、可内联；同进程 A/B 对照没有真实消费者，不为它新增注册实体（vickwv 同意）。 */

// ---------- 句柄（不透明） ----------
typedef struct nvn_gpu_buf_s *nvn_gpu_buf;       // 缓冲（Metal MTLBuffer / Vulkan VkBuffer+内存）
typedef struct nvn_gpu_tex_s *nvn_gpu_tex;       // 贴图存储（MTLTexture / VkImage+内存）
typedef struct nvn_gpu_view_s *nvn_gpu_view;     // 贴图视图（MTLTexture 视图 / VkImageView；缓冲贴图也是视图）
typedef struct nvn_gpu_samp_s *nvn_gpu_samp;     // 采样器
typedef struct nvn_gpu_shader_s *nvn_gpu_shader; // 一个阶段的着色器代码 + 已核对的布局
typedef struct nvn_gpu_pipe_s *nvn_gpu_pipe;     // 图形或计算管线
typedef struct nvn_gpu_cmd_s *nvn_gpu_cmd;       // 一段命令，提交或丢弃即失效
typedef struct nvn_gpu_pool_table_s *nvn_gpu_pool_table; // 不可变的按池 ID 索引资源快照
typedef struct nvn_gpu_surface_s *nvn_gpu_surface; // 上屏面（接收平台层已建好的窗口 / WSI）

// ---------- 出错（定稿 A5 / A7） ----------
enum {
    NVN_GPU_OK = 0,
    NVN_GPU_E_BASE = -1000,          // 独立区间 [-1099, -1000]：与 s2a_direct_result（1 … -9）不相交，单测编译期核对
    NVN_GPU_E_ARG = -1001,           // 参数非法（越界、句柄已释放、格式与用途不符）
    NVN_GPU_E_UNSUPPORTED = -1002,   // 这台设备 / 这个后端没有这项能力（先查 caps，查过还调到这里是核心的错）
    NVN_GPU_E_OOM = -1003,           // 分配失败（显存 / 主存 / 描述符池）
    NVN_GPU_E_DEVICE = -1004,        // 设备丢失；终态，之后所有可失败函数都返回它
    NVN_GPU_E_TIMEOUT = -1005,       // 在给定时限内没有完成证明（不是失败，是「还没证明」）
    NVN_GPU_E_ORDER = -1006,         // 调用顺序违规（通道里做拷贝、cmd 已失效还编码、[回调] 里等栅栏）：核心的错，不可重试
    NVN_GPU_E_PENDING = -1007,       // 异步请求还没出结果（管线后台编译中）；注意会话 ABI 的 Pending 是 +1，不能直接 cast
    NVN_GPU_E_COMPILE = -1008,       // 着色器 / 管线编译或链接失败（why 带编译器原文）
    NVN_GPU_E_LAYOUT = -1009,        // 交来的布局与 blob 自带的反射不一致（set / binding / 类别 / 数量 / 身份）；后端不自行重排
    NVN_GPU_E_OUTDATED = -1010,      // 上屏面过期但可重建（窗口尺寸 / 旋转变了、交换链 out-of-date）：调用方重建面（新 generation）后重试
    NVN_GPU_E_REVOKED = -1011,       // 上屏面已被平台撤销（窗口租约收回、进后台）：不可在原窗口重建，等平台给新窗口
    NVN_GPU_E_FAILED = -1012,        // 时间线已进入失败终态：所等的值落在第一个失败段或之后（见 nvn_gpu_cmd_commit）
    NVN_GPU_E_GPU_FAULT = -1013,     // 段已执行完但 GPU 报了故障（MTLCommandBuffer.error 非设备丢失类）：非终态，回写照做，隔离由核心定
    NVN_GPU_E_STOPPED = -1014,       // 会话正在关闭（nvn_gpu_close 已开始）：新工作一律拒收
    NVN_GPU_E_LAST = -1014,
};
// 返回值类型是单成员结构体而不是 int：`int r = nvn_gpu_open(...)` 与 `(int)nvn_gpu_open(...)` 都编译不过，
// 跨到 int 类错误域（会话 ABI、errno）只能经显式的 .code + 映射表（单测用编译失败探针核对）。
typedef struct { int32_t code; } nvn_gpu_rc;
static inline int nvn_gpu_ok(nvn_gpu_rc r) { return r.code == NVN_GPU_OK; }
typedef struct { nvn_gpu_rc rc; char why[192]; } nvn_gpu_err;   // rc 与返回值相同；why 以 0 结尾。后端只在失败时写

// ---------- 设备与能力（定稿 A2 / A7） ----------
typedef enum { NVN_GPU_API_METAL3 = 3, NVN_GPU_API_METAL4 = 4, NVN_GPU_API_VULKAN = 10 } nvn_gpu_api;
// 能力位 = 已启用（enabled）的能力，不是「设备支持」。核心按位决定走原生还是模拟，后端不做近似。
// 按阶段 / 格式才成立的能力（片元读回、存储、原子）不在这里，见 nvn_gpu_caps 的阶段掩码与 nvn_gpu_format_caps。
enum {
    NVN_GPU_CAP_HOST_IMPORT       = 1u << 0,   // 内存池可零拷贝包游戏内存（newBufferWithBytesNoCopy / VK_EXT_external_memory_host）。安卓目标机未实测，不承诺（定稿 #2）
    NVN_GPU_CAP_GPU_VA            = 1u << 1,   // 缓冲有 64 位 GPU 地址、着色器可按地址访问（gpuAddress / bufferDeviceAddress，Vulkan 侧可选）
    NVN_GPU_CAP_TILE_MEMORY       = 1u << 2,   // 分块渲染：load / store 有真实带宽代价，核心的通道合并 / 存储前瞻值得做
    NVN_GPU_CAP_SAMP_MINMAX       = 1u << 3,   // 采样器 min / max 归约原生（Apple10 + 系统 26 / samplerFilterMinmax）
    NVN_GPU_CAP_SAMP_LOD_BIAS     = 1u << 4,   // 采样器 LOD 偏置原生且实测生效（gal_samp_caps.h 实测 / Vulkan mipLodBias）
    NVN_GPU_CAP_DEPTH_CLIP_CTL    = 1u << 5,   // 裁剪深度可设成 [-1, 1]（Metal 无，靠 main0_z；VK_EXT_depth_clip_control）
    NVN_GPU_CAP_GEOMETRY          = 1u << 6,   // 原生几何着色器（Metal 无，核心 nvn_geometry.inc 模拟）
    NVN_GPU_CAP_TESSELLATION      = 1u << 7,   // 原生曲面细分阶段
    NVN_GPU_CAP_XFB               = 1u << 8,   // 原生变换反馈（Metal 无，核心 tf_draw 模拟）
    NVN_GPU_CAP_INDIRECT_COUNT    = 1u << 9,   // 带计数缓冲的间接多绘（Metal 无，核心 mdx_run 模拟）
    NVN_GPU_CAP_LINKED_PIPE       = 1u << 10,  // 后端有「链接版」首遇管线，且输出与完整版逐像素相同（契约用例 C19 证明过才许置位）。
                                               //   只有 VK_EXT_graphics_pipeline_library 不算：要有与 Metal 链接桩（link_glue.inc）等价的实现
    NVN_GPU_CAP_TEX_BC            = 1u << 11,  // BC1–7 可直接采样
    NVN_GPU_CAP_TEX_ASTC          = 1u << 12,  // ASTC 可直接采样
    NVN_GPU_CAP_GPU_DURATION      = 1u << 13,  // 完成回调给每段 GPU 执行时长（Metal GPUEnd−GPUStart / Vulkan 时间戳差 × timestampPeriod）
    NVN_GPU_CAP_GPU_TIME_HOST     = 1u << 14,  // 完成回调给的 GPU 起止时刻已校准到主机单调时钟（Metal 恒有；Vulkan 要 VK_EXT_calibrated_timestamps）
    NVN_GPU_CAP_SHOWN_TIME        = 1u << 15,  // 能报每帧实际上屏时刻（Metal presentedTime / Vulkan VK_GOOGLE_display_timing 等）；没有就报 UNKNOWN，不拿别的时刻冒充
    NVN_GPU_CAP_DYN_DEPTH_STENCIL = 1u << 16,  // 深度模板是动态状态（Metal DSS 独立对象 / Vulkan extended_dynamic_state 的深度模板部分）
    NVN_GPU_CAP_DYN_RASTER        = 1u << 17,  // 剔除 / 正反面 / 多边形模式是动态状态（Metal 恒有 / Vulkan extended_dynamic_state(3)）
    NVN_GPU_CAP_BLIT_SCALED       = 1u << 18,  // 带缩放 / 格式转换的贴图拷贝原生（Metal 无，核心 copy_t2t_draw 用绘制做；vkCmdBlitImage）
    NVN_GPU_CAP_GPU_DESWIZZLE     = 1u << 19,  // 后端能在 GPU 上解 NVN 块线性排布（Metal 现有 nvn_deswizzle_gpu.h；定稿 #3）
    NVN_GPU_CAP_BLIT_DEPTH_STENCIL = 1u << 20, // 同格式 D24S8/D32FS8、二维单采样、最近点缩放（含全部模板位）
    NVN_GPU_CAP_NO_ATTACH_PASS    = 1u << 21,  // 能开一个附件都不挂的渲染通道（NVN 不挂目标照样绘制：片元只写存储 / 只为遮挡计数）。尺寸由 pass_desc 的 w / h 给；
                                               //   Vulkan 核心 1.0 恒有（零附件子通道 + 带宽高的帧缓冲）；Metal 的 renderTargetWidth / Height 没接，不置位 → 核心照旧跳过并记账
    NVN_GPU_CAP_REPORT_TIMESTAMP  = 1u << 22,  // 逐报告位置的时间戳：已校准 CLOCK_MONOTONIC ns；首版仅 Vulkan，Metal 明确不置位
    NVN_GPU_CAP_STORAGE_SRGB_ALIAS = 1u << 23, // 带图像标志的 sRGB 贴图可按同位宽 UNORM 视图当存储图像写（字节原样，不做 sRGB 编码；建图 EXTENDED_USAGE、
                                               //   其余视图剥掉存储用途）。Vulkan（1.1+）置位；Metal 不置位，核心照旧按「不可写」跳过
    NVN_GPU_CAP_DRAW_TEX          = 1u << 24,  // v20：通道内画贴图矩形（NVN DrawTexture，nvn_gpu_draw_tex，#1452）。Vulkan 置位；
                                               //   Metal 后端不置位（iOS 由 nvn_mtl.m draw_tex 自己画），核心照旧记「未接 op OP_DRAW_TEX」跳过
    NVN_GPU_CAP_BC1_SNAPSHOT      = 1u << 25,  // v22：后端接收整图 CPU 快照，决定 BC1 存储或记录透明度诊断
};
// 绑定资源类别（布局项 cls；描述符上限按它分）
enum { NVN_GPU_RES_UBO = 0, NVN_GPU_RES_SSBO, NVN_GPU_RES_TEX, NVN_GPU_RES_SAMP, NVN_GPU_RES_TEX_SAMP, NVN_GPU_RES_IMAGE,
       NVN_GPU_RES_TBUF, NVN_GPU_RES_IMAGE_TBUF, NVN_GPU_RES_VA, NVN_GPU_RES_N };
typedef struct {
    nvn_gpu_api api;
    uint32_t api_version;          // Metal：系统大版本；Vulkan：VK_MAKE_API_VERSION 数值（基线 1.1）
    uint32_t family;               // Metal：Apple GPU 家族号（MTLGPUFamilyApple<N> 的 N）；Vulkan：0
    uint32_t vendor_id, device_id; // Vulkan：物理设备属性原值；Metal：0 / registryID 低 32 位
    char name[64];
    uint8_t caps_id[16];           // 能力记录身份：生成本结构的那份「已启用能力记录」的摘要。安卓侧与 device_caps.c 输出的 JSON 同源同摘要（用例 C18 核对）
    uint32_t caps;                 // NVN_GPU_CAP_*
    // 以下限值每项都从同一份已启用能力记录取，括号里是来源；不机械复制全部设备属性，只放核心真要用的
    uint32_t max_color_att;        // (maxColorAttachments / Metal 家族表)
    uint32_t max_vertex_streams;   // (maxVertexInputBindings / Metal 31 个缓冲槽里分给顶点流的部分)
    uint32_t max_tex_dim, max_samples, max_viewports;   // (maxImageDimension2D, framebuffer 采样数交集, maxViewports / 家族表)
    uint32_t max_push_bytes;       // (maxPushConstantsSize / Metal setBytes 4096)
    uint32_t min_ubo_align, min_ssbo_align;   // (minUniformBufferOffsetAlignment, minStorageBufferOffsetAlignment / 家族表)
    uint64_t max_ubo_range, max_ssbo_range;   // (maxUniformBufferRange, maxStorageBufferRange / 家族表)
    uint64_t max_buf_bytes;
    uint32_t min_texel_align;      // 缓冲视图（TBUF / IMAGE_TBUF）偏移对齐 (minTexelBufferOffsetAlignment / 0 = 后端不报，核心不做对齐副本)
    uint32_t max_texel_elems;      // 缓冲视图元素上限 (maxTexelBufferElements / 0 = 不报)
    uint32_t max_stage_res[NVN_GPU_RES_N];    // 每阶段每类描述符上限 (maxPerStageDescriptor* / 参数缓冲上限)
    uint32_t max_sets;             // 可同时绑定的描述符集数 (maxBoundDescriptorSets；Metal 0 = 不用 set)
    uint32_t storage_stages;       // 允许存储缓冲 / 存储图像写的 NVN 阶段位（1 << nvn_gpu_stage）(vertexPipelineStoresAndAtomics / fragmentStoresAndAtomics)
    uint32_t atomic_stages;        // 允许图像原子的 NVN 阶段位；具体格式再看 NVN_GPU_FMT_ATOMIC
    uint32_t display_max_hz;       // 有窗口时屏幕最高刷新率，没窗口 0（来自平台层的窗口；nvn_be_display_hz 的来源）
} nvn_gpu_caps;
// 每个 NVN 格式在本设备上的用法位（来源：格式属性 optimalTilingFeatures / Metal 格式能力表）
enum {
    NVN_GPU_FMT_SAMPLE = 1u << 0, NVN_GPU_FMT_FILTER = 1u << 1, NVN_GPU_FMT_RENDER = 1u << 2, NVN_GPU_FMT_BLEND = 1u << 3,
    NVN_GPU_FMT_STORAGE = 1u << 4, NVN_GPU_FMT_DEPTH = 1u << 5, NVN_GPU_FMT_ATOMIC = 1u << 6, NVN_GPU_FMT_VERTEX = 1u << 7,
    NVN_GPU_FMT_FB_FETCH = 1u << 8,   // 作颜色附件时片元着色器可读本像素（Metal [[color(n)]] / Vulkan rasterization_order_attachment_access 等，按格式开）
    NVN_GPU_FMT_SUBST = 1u << 9,      // 有对应但不是同一种存储（BGRA 换 RGBA、D24 换 D32）：读写语义同，逐字节拷贝要核心换算
    NVN_GPU_FMT_READBACK = 1u << 10,  // nvn_gpu_copy_tex_buf 拷出来的就是 NVN 的逐字节内容（ReadTexels 读 GPU 内容的前提）。不含：SUBST 的、深度模板合一的
                                      //   （只拷得出深度面）、D24（深度面高 8 位未定义）。后端 copy_tex_buf 拒收的格式一定没有这一位（两处用同一个判断）
};
typedef struct {
    void *host_device;       // 平台层已建好的设备（定稿 A2：后端不另建设备）。Apple：平台给的 MTLDevice 指针；安卓：Prepare 按 profile 建好的设备上下文（含 enabled 记录）
    const void *caps_record; // 同一份「已启用能力记录」（安卓：device_caps.c 用的那份；Apple：NULL = 后端从 host_device 现查），caps 只从它派生
    size_t caps_record_bytes;
    const char *cache_dir;   // 管线 / 着色器二进制缓存目录；NULL = 不落盘。后端有界维护、收尾时写，不逐帧写盘，不提供公共 checkpoint
    const char *app_label;
} nvn_gpu_open_desc;
// 接管平台层已建好的设备与队列（单队列、单队列族，首版不扩多队列）。先后：平台 Prepare（建设备 / 选 profile）→ nvn_gpu_open → 平台给窗口 → nvn_gpu_surface_attach。
// 语义：进程内同时只有一个打开着的后端；重复调用返回 NVN_GPU_E_ORDER。不拥有 host_device 的寿命（close 不销毁它，交还平台）。
// 线程：[任意]，与其他函数不并发。出错：host_device 缺基线能力（Vulkan 1.1 + 二元信号量 / 栅栏；时间线信号量、设备地址、动态状态、GPL 都是可选能力，缺了只是对应能力位为 0）
//       → NVN_GPU_E_UNSUPPORTED，why 写缺的那一项。
nvn_gpu_rc nvn_gpu_open(const nvn_gpu_open_desc *d, nvn_gpu_err *err);
// 关闭（定稿 A5）。返回 OK 前必须做完全部义务：① 丢弃所有未提交 cmd（同 nvn_gpu_cmd_abort）；② 所有已提交段完成，各段 done 恰好调过一次；
// ③ 所有 present 退休，presented 恰好调过一次；④ 后端后台工作（管线编译 worker、上传）停下并 join；⑤ 返回之后不再有任何回调；⑥ 管线缓存落盘。
// 时限内做不完返回 NVN_GPU_E_TIMEOUT：未完成的义务保留、资源不拆（安卓 s2a_vk_adapter_shutdown 的 -1 语义），调用方不许当正常收局；再调一次 close 接着做。
// 关闭开始后其他可失败函数返回 NVN_GPU_E_STOPPED。线程：[任意]，不许在 [回调] 里调。
nvn_gpu_rc nvn_gpu_close(uint64_t timeout_ns, nvn_gpu_err *err);
void nvn_gpu_caps_get(nvn_gpu_caps *out);   // 打开后不变（display_max_hz 随窗口变）。线程：[任意]。未打开时 *out 全 0
uint32_t nvn_gpu_format_caps(uint32_t nvn_format);   // NVN_GPU_FMT_*。线程：[任意]。不认识的格式返回 0，不猜

// ---------- 缓冲与内存池（定稿 #2：脏页 / 内容身份在核心，后端只给同步原语） ----------
enum { NVN_GPU_MEM_SHARED = 0, NVN_GPU_MEM_DEVICE = 1 };   // SHARED：CPU 可写、一致；DEVICE：只给 GPU
typedef struct { nvn_gpu_buf buf; uint64_t off; uint8_t *cpu; uint64_t va; } nvn_gpu_slice;   // cpu：DEVICE 内存为 NULL；va：无 CAP_GPU_VA 为 0
// 最后一次使用结束时的通知（只调一次，在 [回调]）：核心在这之后才允许宿主解除 / 复用这段 backing。
typedef void (*nvn_gpu_retired_fn)(void *user);
// 把游戏内存池 [host, host+bytes) 原地包成 GPU 缓冲（要 CAP_HOST_IMPORT）。CPU 在 cmd 提交前写入的内容该 cmd 看得到；GPU 写回在该段 done 之前可见。
// 无 CAP_HOST_IMPORT 时返回 NVN_GPU_E_UNSUPPORTED：核心改用 nvn_gpu_buf_create + 按自己的脏页记录（nvn_dirty.c）在提交前 nvn_gpu_buf_write、
// GPU 写过的范围经 copy_buf 到 SHARED 缓冲、done 后拷回。后端不装页保护、不自己猜脏页。
// 线程：[任意]。生命周期：池 Finalize 时核心调 nvn_gpu_buf_release(b, retired, …)；后端持有这段包装与宿主页，直到最后一次使用结束才解除并调 retired。
// 出错：NVN_GPU_E_UNSUPPORTED / NVN_GPU_E_OOM（驱动拒绝，后端可有界重试，nvn_mtl.m 现为约 2 s），why 带区间。
nvn_gpu_rc nvn_gpu_buf_wrap(void *host, uint64_t bytes, nvn_gpu_buf *out, nvn_gpu_err *err);
// 新建缓冲。init 非 NULL 时拷入 bytes 字节（只对 SHARED）。线程：[任意]。出错：NVN_GPU_E_OOM / NVN_GPU_E_ARG（bytes 为 0 或超上限）。
nvn_gpu_rc nvn_gpu_buf_create(uint64_t bytes, int mem, const void *init, nvn_gpu_buf *out, nvn_gpu_err *err);
uint64_t nvn_gpu_buf_va(nvn_gpu_buf b);    // GPU 地址（无 CAP_GPU_VA 为 0）。线程：[任意]
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b);   // CPU 地址（DEVICE 为 NULL）。线程：[任意]
// 释放。retired 可为 NULL；非 NULL 时在最后一次使用（总则 ①–④）结束后恰好调一次。线程：[任意]。NULL 句柄无操作。
void nvn_gpu_buf_release(nvn_gpu_buf b, nvn_gpu_retired_fn retired, void *user);
// 本 cmd 会经 GPU 地址间接用到 b（总则 ④）：后端保活到该 cmd 完成，并保证执行时驻留（Metal 驻留集 / useResource；Vulkan 只保活）。线程：[提交]。
void nvn_gpu_buf_use(nvn_gpu_cmd c, nvn_gpu_buf b);
// 同步原语：在本 cmd 的 GPU 顺序里把 src 的 bytes 字节写进 dst+off（之前的命令读旧值，之后的读新值）；src 调用返回后即可释放。
// 线程：[提交]，通道外。出错：粘滞 NVN_GPU_E_ARG / NVN_GPU_E_ORDER。
void nvn_gpu_buf_write(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off, const void *src, uint64_t bytes);
// 本 cmd 的临时上传区（参数块、内嵌常量、宽屏改写副本、support 缓冲大块）：内容有效到该 cmd 完成。线程：[提交]。出错：NVN_GPU_E_OOM，*out 清零。
nvn_gpu_rc nvn_gpu_arena_alloc(nvn_gpu_cmd c, uint64_t bytes, uint32_t align, nvn_gpu_slice *out, nvn_gpu_err *err);
// 把 src 的 bytes 字节填进本 cmd 的临时上传片 sl（sl->cpu 起，bytes ≤ 申请时的长度）。后端可以把这次拷贝交给别的线程做（Vulkan：两级回放的录制线程，
// 按记录顺序），但保证：① c 提交（commit）或放弃（abort）之前做完；② nvn_gpu_arena_fill_wait(返回值) 返回时做完。src 在做完之前必须保持可读、内容不变
// ——核心改 src 所在的内存之前先等（rp_ub_fill_fence）。返回 0 = 已经在本线程拷完；否则是递增的票号。线程：[提交]。
uint64_t nvn_gpu_arena_fill(nvn_gpu_cmd c, const nvn_gpu_slice *sl, const void *src, uint64_t bytes);
// 等票号 ≤ ticket 的填充全部做完，返回已做完的最大票号；ticket = 0 只查不等。线程：[提交]。
uint64_t nvn_gpu_arena_fill_wait(uint64_t ticket);
// 只让票号 ticket 这一笔做完：还没开始就在本线程拷（之后后端不再拷它），别的线程正在拷就等它拷完。不保证更旧的票号做完
// （要「≤ ticket 全部做完」用 nvn_gpu_arena_fill_wait），nvn_gpu_arena_fill_wait(0) 的返回值仍是「≤ 它的全部做完」的水位，可能小于 ticket。
// 返回 0 = 早已做完（ticket = 0 同此）；1 = 本线程拷的；2 = 别的线程正在拷、等到它拷完。线程：[提交]。
int nvn_gpu_arena_fill_take(uint64_t ticket);

// ---------- 贴图、视图、采样器 ----------
enum {   // 用途创建时给全（Vulkan 的 usage 与可变格式事后补不了）
    NVN_GPU_TEX_SAMPLED = 1u << 0, NVN_GPU_TEX_RENDER = 1u << 1, NVN_GPU_TEX_STORAGE = 1u << 2, NVN_GPU_TEX_ATOMIC = 1u << 3,
    NVN_GPU_TEX_VIEW_FMT = 1u << 4,   // 会建不同格式的视图（PixelFormatView / MUTABLE_FORMAT）
    NVN_GPU_TEX_CPU_UPLOAD = 1u << 5, // 内容来自游戏内存，会经 nvn_gpu_tex_upload* 更新
    NVN_GPU_TEX_TRANSIENT = 1u << 6,  // 只活在一个通道里（memoryless / LAZILY_ALLOCATED）
    NVN_GPU_TEX_BC1_OPAQUE = 1u << 7, // 初始完整压缩快照已证无透明索引，允许宿主选 ASTC 存储。
};
typedef struct {
    nvn_texdesc d;             // NVN 描述原样
    uint32_t usage;            // NVN_GPU_TEX_*
    uint32_t host_w, host_h;   // 宿主实际尺寸（0 = 同 d）：宽屏放宽的目标（nvn_mtl.m tex_widen）
    uint16_t swz;              // 存储自带通道映射（tex_builder_t.swz 打包法，0 = 恒等）
    uint8_t _rsv[2];           // 保留，恒 0（模板模式只在视图记录 view_t 里表达，建贴图描述不带；字节位置留着是为了回放夹具格式不变）
} nvn_gpu_tex_desc;
typedef struct { uint32_t w, h, depth, levels, layers, samples; uint32_t nvn_format; uint32_t fmt_flags; } nvn_gpu_tex_info;
// 建贴图存储。内容未定义。身份（serial、别名、同池同偏移复用）全在核心，后端不缓存贴图。线程：[任意]。
// 出错：NVN_GPU_E_UNSUPPORTED（格式 / 用途组合不支持，why 写格式号与用途位）、NVN_GPU_E_OOM。
nvn_gpu_rc nvn_gpu_tex_create(const nvn_gpu_tex_desc *d, nvn_gpu_tex *out, nvn_gpu_err *err);
void nvn_gpu_tex_info_get(nvn_gpu_tex t, nvn_gpu_tex_info *out);   // 实际尺寸与格式（宽屏挂点读宿主宽高）。线程：[任意]
// 可选：整张 CPU 块线性快照上传时为已证不透明的 BC1 选宿主压缩存储。
// 返回 1 = 已录完整上传，0 = 沿用普通上传，-1 = 失败（命令段报错）。
// 替换存储时 *replacement 为新句柄，由回放更新视图和释放旧句柄。
int nvn_gpu_tex_bc1_snapshot(nvn_gpu_cmd c, nvn_gpu_tex t, const void *bytes, uint64_t size, uint64_t serial, nvn_gpu_tex *replacement);
// 可选：从原始压缩影子重建普通解码存储，让后续写入 / 异格式拷贝沿用原行为。
// 返回 1 = 重建已录，0 = 当前已是普通存储，-1 = 命令段失败。
int nvn_gpu_tex_bc1_unwrap(nvn_gpu_cmd c, nvn_gpu_tex t, nvn_gpu_tex *replacement);

void nvn_gpu_tex_release(nvn_gpu_tex t);
// 视图：v 为 NVN 视图记录原样，NULL = 默认视图。视图让贴图存储活到视图 release。线程：[任意]。出错：格式不兼容或缺 VIEW_FMT → NVN_GPU_E_ARG。
nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex t, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err);
// 可选：建立单级单层颜色附件，按请求的格式写入，分量恒等，仅用于附件。持有贴图引用直到视图释放。线程：[任意]。
// 不兼容、贴图未声明该格式/用途或设备不支持时返回 NVN_GPU_E_UNSUPPORTED，不换用基底或等宽替代格式。
nvn_gpu_rc nvn_gpu_view_create_color_attachment(nvn_gpu_tex t, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err);
// 缓冲视图（TBUF / IMAGE_TBUF 的唯一绑定形态，#784 定稿 11-06）：格式来自着色器句柄解析到的 NVN 贴图对象（target = BUFFER，游戏 nvnTextureBuilderSetFormat /
//   SetSize1D 定的 d.format / d.width），由核心经 texture pool 解析后传进来、按「存储 + 格式」缓存复用；SPIR-V 的 ImageFormat=Unknown 不承载格式，后端不从着色器猜。
//   约束：nvn_format 用作 TBUF 须有 NVN_GPU_FMT_SAMPLE、用作 IMAGE_TBUF 须有 NVN_GPU_FMT_STORAGE（Vulkan：UNIFORM / STORAGE_TEXEL_BUFFER 格式特性）；
//   off 满足设备的纹素缓冲偏移对齐（Vulkan minTexelBufferOffsetAlignment），bytes / 元素字节 ≤ 纹素缓冲元素上限（maxTexelBufferElements）。
//   出错：缺能力 → NVN_GPU_E_UNSUPPORTED；越界 / 不对齐 → NVN_GPU_E_ARG；不许悄悄换格式或截断。线程：[任意]
nvn_gpu_rc nvn_gpu_view_create_buf(nvn_gpu_buf b, uint64_t off, uint64_t bytes, uint32_t nvn_format, nvn_gpu_view *out, nvn_gpu_err *err);
void nvn_gpu_view_release(nvn_gpu_view v);
// v21：取视图实际引用的贴图。借用到视图释放为止；缓冲视图和空句柄返回 NULL。
nvn_gpu_tex nvn_gpu_view_texture(nvn_gpu_view v);
// 采样器：samp_par 原样。归约 / LOD 偏置不原生时照建一个不带它们的采样器——模拟在核心选的 _se 变体里（能力位已如实报），去重缓存在核心。
nvn_gpu_rc nvn_gpu_samp_create(const samp_par *p, nvn_gpu_samp *out, nvn_gpu_err *err);
void nvn_gpu_samp_release(nvn_gpu_samp s);
// 线性内容上传（解排布 / 解压已由核心做完）：在本 cmd 的 GPU 顺序里生效，src 调用返回后即可释放。线程：[提交]，通道外。出错：粘滞 ORDER / ARG。
void nvn_gpu_tex_upload(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        const void *src, uint64_t row_bytes, uint64_t image_bytes);
// 块线性原始字节上传，由后端在 GPU 上解排布（要 CAP_GPU_DESWIZZLE）；L 为 NVN 排布（nvn_tex_layout 的结果）。
// 返回 NVN_GPU_OK：本张贴图已进 GPU（含后端内部 CPU 回退）；非 0：本张贴图没传（布局 / 几何 / 越界校验或暂存分配失败，
//   后端日志带原因，不粘滞命令缓冲），调用方应照原样走 CPU 上传路径；顺序违规仍粘滞（契约 bug）。
/* 定稿 #3：Vulkan 首版可只做 CPU 线性上传（不置 CAP_GPU_DESWIZZLE，核心先解再调 nvn_gpu_tex_upload）；Metal 抽离必须保留现有 GPU 解排布路径
   （nvn_deswizzle_gpu.h），不许借抽离退成 CPU 而改变行为或性能（判据：D1 基准的上传时长读数不变）。 */
nvn_gpu_rc nvn_gpu_tex_upload_blocklinear(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t level, uint32_t layer, const nvn_texlayout *L,
                                          const void *src, uint64_t bytes);

// ---------- 着色器布局（定稿 A1：完整反射过下沿） ----------
typedef enum { NVN_GPU_SH_VS = 0, NVN_GPU_SH_FS = 1, NVN_GPU_SH_GS = 2, NVN_GPU_SH_TCS = 3, NVN_GPU_SH_TES = 4, NVN_GPU_SH_CS = 5 } nvn_gpu_stage;   // = NVN 阶段号
enum { NVN_GPU_ACC_R = 1, NVN_GPU_ACC_W = 2, NVN_GPU_ACC_RW = 3 };
// 一个绑定项。set / binding 是编译器写进 blob 的原值（安卓现行 direct ABI 是四个 set；Metal 不用 set，用 aid = 参数缓冲序号）。
// 消费方（后端）不许改写已编 blob 的 set / binding（例：私自改成每阶段一个 set）；对不上就报 NVN_GPU_E_LAYOUT。
typedef struct {
    uint8_t set;          // 描述符集号（Metal 忽略）
    uint8_t cls;          // NVN_GPU_RES_*
    uint8_t access;       // NVN_GPU_ACC_*
    uint8_t src_kind;     // 来源：nvn_refl.h 的 NVN_RB_*（哪类 NVN 槽：CBUF / TEX / IMG / SAMP / SUPPORT / C1 / DRV0 …）
    uint16_t binding;     // 描述符号（Metal 忽略）
    uint16_t src_reg;     // 来源槽号（NVN 常量缓冲号 / 贴图槽号 …）
    uint32_t src_off;     // v7：来源内的字节偏移；目前只有 NVN_RB_UBOA（UBO 里存 8 字节裸指针的位置）用
    uint16_t count;       // 数组长度（非数组为 1）
    uint16_t aid;         // Metal 参数号（Vulkan 忽略）
    uint32_t stages;      // 可见阶段位（1 << nvn_gpu_stage）
    uint16_t src_reg2;    // 分离采样器来源（NVN_RB_TEX 项）：UINT16_MAX = 无第二来源；0..127 = NVN 采样器槽；
                          //   NVN_GPU_SAMP2_CONST | 池下标 = 着色器里的立即数采样器池下标（类型 3 句柄，不经采样器槽，见下）
} nvn_gpu_layout_ent;
// src_reg2 的「常量采样器」编码（#1594）：高位标记 + 低 12 位采样器池下标（0..NVN_GPU_SAMP2_ID_MASK，句柄高 12 位的范围；0 是真下标）。
// 标记位在采样器槽范围（0..127）之外：不认这种编码的旧消费方会落进「src_reg2 >= 128」的形状检查具名跳过，不会把它当槽号静默绑错。
#define NVN_GPU_SAMP2_CONST   0x8000u
#define NVN_GPU_SAMP2_ID_MASK 0x0FFFu
typedef struct {
    const nvn_gpu_layout_ent *ent; uint32_t nent;
    uint32_t push_off, push_bytes;   // 本阶段 push 常量范围（0 字节 = 不用）；nvn_gpu_push 只许写进这个范围
    uint32_t support_bytes;          // support 缓冲布局长度（字段由反射的 SUPPORT 项描述；超出 push 范围的大块按它放进 arena 再绑）
    uint8_t id[16];                  // 布局身份：编译器产出 blob 时写下的摘要，与 blob 同源（不是短阶段键）
    const char *id_src;              // 身份来源（例 "s2a-direct-program-v1" / "dmsh-refl"），日志用
} nvn_gpu_layout;
typedef enum { NVN_GPU_BLOB_METALLIB = 1, NVN_GPU_BLOB_MSL = 2, NVN_GPU_BLOB_SPIRV = 3 } nvn_gpu_blob;
typedef struct {
    nvn_gpu_stage stage;
    uint8_t key[8];                  // 阶段键：只作日志与缓存名
    nvn_gpu_blob kind;
    const void *blob; size_t bytes;  // 调用返回后后端不再引用
    const char *entry;               // 入口名（main0 / main0_z / entry_layer …，核心按变体选）
    const nvn_gpu_layout *layout;    // 已核对的中间布局；后端装载时与 blob 自带反射逐项核对（set / binding / 类别 / 数量 / 阶段 / 身份）
    uint8_t precompiled;             // 1 = 出包预编库件（LibPack，带现编回退链）：SPIR-V 合规核对对「表外声明」按拒收处理；0 = 现编 / 缓存 / 离线件：表外声明只警告计数
} nvn_gpu_shader_src;
// 装载一个阶段的代码与布局（布局被后端拷走）。线程：[任意]（含 [后台] 预取）。生命周期：到 release；管线持有它用到的着色器。
// 出错：NVN_GPU_E_COMPILE（why 带编译器原文）、NVN_GPU_E_LAYOUT（布局与 blob 不符，why 写第一处不符的项）、NVN_GPU_E_UNSUPPORTED（不收这种 blob）。
nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *out, nvn_gpu_err *err);
// [提交] 可选反射：直接计算派发写该描述符的保守字节末端（相对窗口）；0 表示未知。
uint64_t nvn_gpu_shader_write_end(nvn_gpu_shader sh, unsigned index, unsigned set, unsigned binding, uint32_t groups_x);
void nvn_gpu_shader_release(nvn_gpu_shader s);

// ---------- 管线 ----------
enum { NVN_GPU_SPEC_BOOL = 0, NVN_GPU_SPEC_INT = 1, NVN_GPU_SPEC_UINT = 2, NVN_GPU_SPEC_FLOAT = 3, NVN_GPU_SPEC_MAX = 48 };
typedef struct { uint16_t id; uint8_t type, _rsv; uint32_t bits; } nvn_gpu_spec;   // 特化常量（function constant / VkSpecializationInfo）
typedef struct { nvn_gpu_shader sh; uint32_t nspec; nvn_gpu_spec spec[NVN_GPU_SPEC_MAX]; } nvn_gpu_stage_desc;   // sh = NULL：该阶段不用
// 图形管线描述 = pso_key 去掉 Metal 痕迹（格式改 NVNformat、着色器改句柄）。核心负责规范化（同 pso_key_norm），后端按自己的键去重缓存。
// 管线布局由后端按各阶段布局确定性合并（同 set / binding 的项类别与数量必须一致，否则 NVN_GPU_E_LAYOUT）。
typedef struct {
    nvn_gpu_stage_desc st[5];        // VS FS GS TCS TES
    uint32_t cfmt[8], dfmt, sfmt;    // 附件 NVNformat（0 = 没有）
    uint32_t samples;
    uint8_t lmap[8];                 // 合并通道的颜色槽重映射
    uint32_t prim, patch_size;
    uint8_t prim_restart;            // NVN 条带重启；重启值 = 索引类型的最大值（u8 0xFF / u16 0xFFFF / u32 0xFFFFFFFF）
    uint8_t _rsv_prim[3];
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms;
    va_st attr[16]; vs_st stream[16];
    /* 定稿 #4：静态状态一直带在 desc 里；只在实际启用了对应动态能力（CAP_DYN_DEPTH_STENCIL / CAP_DYN_RASTER）时后端才从自己的键里省略。
       不把变体查找推迟到 draw（预测预编要看得到这些状态）。没有对应能力时 dyn 里的同名字段必须与这里相同，否则粘滞 NVN_GPU_E_ARG。 */
    ds_st ds; poly_st poly;
    uint8_t rast_discard, clip_neg1; // clip_neg1 只在 CAP_DEPTH_CLIP_CTL 时可为 1；否则核心选 main0_z 入口
    /* v6：正面绕序。poly.front 由核心给成「规范化设备坐标（y 朝上）里的正面绕序」，已经算进了队列的光栅翻转位（见 rp_draw.c）；
       后端把它换成自己帧缓冲坐标下的绕序，这取决于它画这笔时翻不翻视口，所以窗口原点也进管线描述：
       origin_ll 必须与绘制时 nvn_gpu_dyn.origin_ll 相同，不同则粘滞 NVN_GPU_E_ARG。 */
    uint8_t origin_ll;
    uint8_t depth_clamp;             // v6：深度夹取（NVN SetDepthClamp）：1 = 超出近远面的片元夹到范围内而不是裁掉。必须与 nvn_gpu_dyn.depth_clamp 相同
} nvn_gpu_pipe_desc;
enum {
    NVN_GPU_PIPE_SYNC = 0,         // 现在就建，阻塞到出结果
    NVN_GPU_PIPE_ASYNC = 1,        // 丢后台编（已缓存则立即返回）；没好返回 NVN_GPU_E_PENDING
    NVN_GPU_PIPE_PROVISIONAL = 2,  // 同 ASYNC，没好时若有链接版（CAP_LINKED_PIPE）先返回它；没有就是 PENDING
};
// 取（或建）图形管线。核心从不因 PENDING 跳绘制：拿不到就改用 SYNC 等。线程：[提交] 与 [后台]。生命周期：后端缓存持有，核心不 release。
// 出错：NVN_GPU_E_COMPILE / NVN_GPU_E_PENDING / NVN_GPU_E_LAYOUT（阶段布局合并冲突）/ NVN_GPU_E_ARG（顶点 / 片元接口不配对，同 pso_pair_ok）。
// 注：VK 后端本身把 ASYNC / PROVISIONAL 按 SYNC 处理（本层不内建后台建管线）。真正的「后台建 + 可跳首用」在其上的
//   安卓 demand 层（android/native/nvn/nvn_vk_pipe_demand.c）：它用 nvn_gpu_pipe_lookup 判命中、用后台线程调 SYNC 建，
//   由回放层 rp_draw 按 iOS skip_safe / 帧预算 / late-stuck 决定跳过或限时等。
nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err);
// 只查缓存、不建：1 = 已有（*out 有效，后端缓存持有），0 = 没有。给异步首用 / 预热等「先看有没有」的调用方。线程：[提交] / [后台]。
int nvn_gpu_pipe_lookup(const nvn_gpu_pipe_desc *d, nvn_gpu_pipe *out);
nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err);   // 计算管线；mode 只认 SYNC / ASYNC

// ---------- 命令：开段、丢弃、通道、状态、绑定、绘制、派发、拷贝 ----------
typedef struct { uint32_t occlusion_slots; const char *label; } nvn_gpu_cmd_desc;
// 开一段命令。一次 NVN 提交可切成多段，段与段按 commit 顺序在 GPU 上执行。线程：[提交]。生命周期：到 commit 或 abort。
// 出错：NVN_GPU_E_DEVICE / NVN_GPU_E_OOM / NVN_GPU_E_STOPPED / NVN_GPU_E_FAILED（时间线已失败终态，见 commit）。
nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err);
// 丢弃一段未提交的命令（Stop / OOM / 核心放弃本次提交）：不执行、不调 done、不推进时间线；它对句柄的引用（总则 ①）随即解除。线程：[提交]。
void nvn_gpu_cmd_abort(nvn_gpu_cmd c);

enum { NVN_GPU_LOAD_LOAD = 0, NVN_GPU_LOAD_CLEAR = 1, NVN_GPU_LOAD_DONTCARE = 2 };
enum { NVN_GPU_STORE_STORE = 0, NVN_GPU_STORE_DONTCARE = 1, NVN_GPU_STORE_RESOLVE = 2, NVN_GPU_STORE_STORE_RESOLVE = 3 };
typedef struct {
    nvn_gpu_view view, resolve;
    uint8_t load, store, _rsv[2];
    union { float f[4]; uint32_t u[4]; int32_t i[4]; } clear;
} nvn_gpu_att;
typedef struct {
    nvn_gpu_att color[8]; uint32_t ncolor;
    nvn_gpu_att depth, stencil;
    float clear_depth; uint32_t clear_stencil;
    uint32_t w, h, layers;
    // 渲染区域（像素，左上原点；w 或 h 为 0 = 整个目标）。附件只在区域内载入 / 保存，区域外保持原内容（Vulkan 的 renderArea 语义）：
    // 核心按这一遍会碰到的视口 / 裁剪矩形给出实际区域，小目标 / 阴影分块这类小遍的载入与保存就不必按整张附件算。
    // 区域内必须盖住本通道所有绘制的像素（核心保证：有绘制超出就结束通道、按并集重开）。
    int32_t area_x, area_y; uint32_t area_w, area_h;
    uint8_t occlusion;    // 本通道会计数遮挡查询
    uint8_t fb_fetch;     // 本通道有片元读本像素颜色（各颜色附件格式须有 NVN_GPU_FMT_FB_FETCH）
    uint8_t _rsv[2];
} nvn_gpu_pass_desc;
// 一个附件都不挂（ncolor 个槽全空、无深度模板）只在 caps 有 NVN_GPU_CAP_NO_ATTACH_PASS 时合法，此时 w / h 就是帧缓冲尺寸（核心按视口给）。
// 开 / 关渲染通道。load / store 全由核心定（TBDR 策略在核心）。通道内只许状态 / 绑定 / 绘制 / 遮挡查询。线程：[提交]。出错：粘滞 ORDER / ARG。
void nvn_gpu_pass_begin(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d);
void nvn_gpu_pass_end(nvn_gpu_cmd c);

typedef struct {
    float vp[16][6]; uint32_t nvp;   // NVN 视口 x, y, w, h, near, far（已叠加 DEPTH_RANGE）
    uint8_t vp_sw[16][4];
    int32_t sc[4]; uint8_t has_sc;
    uint8_t origin_ll;               // 窗口原点在左下（NVN 默认）；后端换成原生约定
    uint8_t depth_clamp, _rsv;
    p_poly_offset po;
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    float blend_color[4];
    ds_st ds; poly_st poly;          // 见定稿 #4
} nvn_gpu_dyn;
void nvn_gpu_set_pipe(nvn_gpu_cmd c, nvn_gpu_pipe p);       // 线程：[提交]，通道内
void nvn_gpu_set_dyn(nvn_gpu_cmd c, const nvn_gpu_dyn *d);   // 线程：[提交]，通道内
void nvn_gpu_set_vbuf(nvn_gpu_cmd c, uint32_t stream, nvn_gpu_buf b, uint64_t off);   // b = NULL 解绑（读 0）

// 绑定一项：ent 是当前管线该阶段着色器布局（nvn_gpu_layout.ent[]）里的下标，cls 必须与该项一致。
/* 定稿 #5：首版沿安卓现行四 set + descriptor pool，不要求 push descriptor。核心管 NVN 状态与「NVN 句柄 → 资源」的解析（描述符池、tex_base / samp_base、
   空槽假贴图），后端管目标描述符 / 参数缓冲的编码与复用（Metal 的参数块复用 args_reuse 随编码一起进 Metal 后端）。 */
// union 按类别取成员（#968 消费审查定稿 ①，10-30；#784 定稿 11-06 改 TBUF / IMAGE_TBUF）：UBO / SSBO → u.b；TEX / IMAGE / TBUF / IMAGE_TBUF → u.view
//   （TBUF / IMAGE_TBUF 的 view 由 nvn_gpu_view_create_buf 建，格式在 view 里，见上；收到 u.b 形态的 TBUF / IMAGE_TBUF 按 NVN_GPU_E_UNSUPPORTED 处理、不猜格式）；
//   核心解析不出缓冲贴图的格式（句柄无效 / 不是 BUFFER 贴图 / 格式没有所需能力位）时给空资源（读 0）并记一次红；
//   IMAGE_TBUF 的着色器声明 ImageFormat=Unknown 时，设备须支持不带格式的存储图像读 / 写（Vulkan shaderStorageImageReadWithoutFormat / WriteWithoutFormat），否则该管线 UNSUPPORTED；SAMP → u.samp；
//   TEX_SAMP（组合采样贴图）→ u.ts，view 与 samp 两个都要、都不得为空（v2 原稿只有分开的 view / samp 两个成员，同在一个 union 里，组合项装不下）；VA → u.va。
//   取错成员或 TEX_SAMP 缺一个：粘滞 NVN_GPU_E_LAYOUT。
typedef struct {
    uint16_t ent; uint8_t cls, _rsv;
    uint32_t elem;           // 数组项的元素号
    union { struct { nvn_gpu_buf buf; uint64_t off, len; } b; nvn_gpu_view view; nvn_gpu_samp samp; struct { nvn_gpu_view view; nvn_gpu_samp samp; } ts; uint64_t va; } u;
} nvn_gpu_binding;
// 按池 ID 的资源快照：两个表分别存采样贴图和采样器；调用方区分真正空项和已登记但资源未构建。
// 空项、类型不符和界外项使用调用方提供的同类型零资源；已登记但不可用的资源必须拒绝，不能换 dummy 报成功。
// 图像类型由 dummy_view 的目标和数值类别确定。所有非空句柄创建时均须存活；创建成功后由快照保活。
// 描述符表尾额外有一个 dummy（下标 count），着色器须将越界 ID 限到 count；容量检查须包含这个表尾。
// 快照不可原地更新，换表先创建新快照；bind 后 release 不影响该命令段，资源至少活到段完成或 abort。
// 默认没有消费者调用此接口，不启用游戏编译能力。设备能力不足、上限不足明确返回 UNSUPPORTED 并写明 why。
// create / release 线程：[任意]；bind 线程：[提交]，失败记该 cmd 的粘滞错误。布局来源须为 NVN_RB_TEXP / NVN_RB_SAMPP，
// 布局 count 等于池 count + 1；当前布局 count 为 16 位，超过其可表达上限要具名拒绝。
enum { NVN_GPU_POOL_IMAGE = 1, NVN_GPU_POOL_SAMPLER = 2 };
enum { NVN_GPU_POOL_EMPTY = 0, NVN_GPU_POOL_READY = 1, NVN_GPU_POOL_UNAVAILABLE = 2 };
typedef struct {
    uint32_t state;
    union { nvn_gpu_view view; nvn_gpu_samp samp; } resource;
} nvn_gpu_pool_item;
typedef struct {
    uint32_t kind, count;
    uint32_t sampler_integer; // sampler 表供整数采样：使用整数兼容的采样器；只认 0 或 1
    const nvn_gpu_pool_item *items;
    union { nvn_gpu_view view; nvn_gpu_samp samp; } dummy;
} nvn_gpu_pool_desc;
nvn_gpu_rc nvn_gpu_pool_create(const nvn_gpu_pool_desc *d, nvn_gpu_pool_table *out, nvn_gpu_err *err);
nvn_gpu_rc nvn_gpu_pool_bind(nvn_gpu_cmd c, nvn_gpu_stage st, uint32_t ent, nvn_gpu_pool_table table, nvn_gpu_err *err);
void nvn_gpu_pool_release(nvn_gpu_pool_table table);
// v21：已建的图像快照是否引用这份贴图（任意子资源）。查实际描述符，包含已替换进去的占位视图。
int nvn_gpu_pool_has_texture(nvn_gpu_pool_table table, nvn_gpu_tex tex);

// 给某阶段一次给全本笔的绑定（替换整张表；没给到的项按布局给空资源）。表项引用的句柄须活到本 cmd 完成（核心保证）。
// 线程：[提交]。出错：粘滞 NVN_GPU_E_LAYOUT（ent 越界 / cls 不符）。
void nvn_gpu_bind(nvn_gpu_cmd c, nvn_gpu_stage st, const nvn_gpu_binding *b, uint32_t n);
// push 常量：写 [off, off+bytes)，必须落在该阶段布局的 push 范围里。
/* 定稿 #6：超出实际 push 范围的大块放进已声明的 support / arena 缓冲（布局里 src_kind = NVN_RB_SUPPORT 的那一项），由 nvn_gpu_bind 绑；
   放在哪里由编译器写下的布局决定，核心与后端都不许临时改上传方式而不改布局。 */
void nvn_gpu_push(nvn_gpu_cmd c, nvn_gpu_stage st, uint32_t off, const void *data, uint32_t bytes);

typedef struct {
    uint32_t prim, count, first, instances, base_instance;
    nvn_gpu_buf ibuf; uint64_t ioff; uint32_t itype; int32_t base_vertex;
    nvn_gpu_buf ind; uint64_t ind_off;
    nvn_gpu_buf cnt; uint64_t cnt_off;   // 要 CAP_INDIRECT_COUNT
    uint32_t max_draws, stride;
} nvn_gpu_draw_args;
void nvn_gpu_draw(nvn_gpu_cmd c, const nvn_gpu_draw_args *a);   // 线程：[提交]，通道内且已 set_pipe。出错：粘滞 ORDER / ARG / UNSUPPORTED
void nvn_gpu_dispatch(nvn_gpu_cmd c, nvn_gpu_pipe cp, uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t ind_off);   // 通道外

// 拷贝与清除（全部在通道外，GPU 顺序）
void nvn_gpu_copy_buf(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t soff, nvn_gpu_buf dst, uint64_t doff, uint64_t bytes);
// 一批缓冲拷贝，结果给图形阶段当常量读（回放的间接绘制 base instance 批量拷）：通道外，GPU 顺序。
//   先一道「此前全部写 → 传输」屏障，各段按（源, 目标）相邻合并成多区域拷贝，最后一道「传输写 → stages 里各阶段的常量读」屏障
//   （stages 第 i 位 = nvn_gpu_stage i，只认 VS / FS / GS）。不发全屏障；目标段之间、目标与源之间互不重叠由调用方保证。
//   线程：[提交]。出错：粘滞 ORDER（通道内）/ ARG（越界、长度不是 4 的倍数）。
typedef struct { nvn_gpu_buf src; uint64_t soff; nvn_gpu_buf dst; uint64_t doff; uint64_t bytes; } nvn_gpu_buf_copy;
void nvn_gpu_copy_bufs_uniform(nvn_gpu_cmd c, const nvn_gpu_buf_copy *v, uint32_t n, uint32_t stages);
// 可选的后端自管回写暂存。只对不适合直接 CPU 回读的源录拷贝，未暂存项返回 offset=UINT64_MAX。
// [提交]，在通道外调用，最后一段里所有源写之后。返回 0 表示未录复制，仍可走原镜像回写；已录制的副作用只通过 done 交付。
typedef struct { nvn_gpu_buf src; uint64_t offset, bytes, readback_offset; } nvn_gpu_readback_span;
int nvn_gpu_readback_enabled(void);
// 本段是否需要候选范围；auto 在 R0 同样观察负载，实际模式在开段时锁存。
int nvn_gpu_readback_cmd_enabled(nvn_gpu_cmd c);
uint64_t nvn_gpu_readback_record(nvn_gpu_cmd c, nvn_gpu_readback_span *spans, uint32_t count);

// 32 位样式填充（NVN ClearBuffer）：bytes 是 4 的倍数，每 4 字节按小端写 value；不是按字节 memset（用例 C01 用 0x11223344 区分）。
void nvn_gpu_fill_buf(nvn_gpu_cmd c, nvn_gpu_buf dst, uint64_t off, uint64_t bytes, uint32_t value);
void nvn_gpu_copy_buf_tex(nvn_gpu_cmd c, nvn_gpu_buf src, uint64_t off, uint64_t row_bytes, uint64_t image_bytes,
                          nvn_gpu_tex dst, uint32_t level, uint32_t layer, const NVNcopyRegion *r);
void nvn_gpu_copy_tex_buf(nvn_gpu_cmd c, nvn_gpu_tex src, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                          nvn_gpu_buf dst, uint64_t off, uint64_t row_bytes, uint64_t image_bytes);
void nvn_gpu_copy_tex(nvn_gpu_cmd c, nvn_gpu_tex src, uint32_t slevel, uint32_t slayer, const NVNcopyRegion *sr,
                      nvn_gpu_tex dst, uint32_t dlevel, uint32_t dlayer, const NVNcopyRegion *dr);
// 颜色 blit 的源区域可用负 width/height 表示反向端点：xoffset/yoffset 是起点，
// 另一端为 offset+extent；后端先校验归一化矩形，再保留端点次序。目标仍为正尺寸。
// 该约定不扩展多采样解析、深度模板复制及三维复制。
void nvn_gpu_blit(nvn_gpu_cmd c, nvn_gpu_view src, const NVNcopyRegion *sr, nvn_gpu_view dst, const NVNcopyRegion *dr, int linear);   // 要 CAP_BLIT_SCALED
void nvn_gpu_clear_view(nvn_gpu_cmd c, nvn_gpu_view v, const float rgba[4], const uint32_t uint4[4], int is_int);
// v4：通道内把当前通道的附件按矩形清成给定值（NVN 带裁剪或带写掩码的 ClearColor / ClearDepthStencil；整屏全掩码的清除由核心折成 load = CLEAR，不走这里）。
// v5：带写掩码——color_mask 是要写的颜色通道（r=1 g=2 b=4 a=8），stencil_mask 是要写的模板位；没点到的通道 / 位保持原值。
// 不看当前绑定的管线与动态状态，调用之后它们的值不保证，核心照「新通道」那样在下一笔绘制前全部重发。
// 矩形与 nvn_gpu_dyn.sc 同一坐标约定，必须整个落在通道范围内且非空；通道的全部层都清。
typedef struct {
    int32_t color;                    // 要清的颜色附件 = pass_desc.color[] 的下标；-1 = 不清颜色
    union { float f[4]; uint32_t u[4]; int32_t i[4]; } value;   // 按附件格式的数值类别取用（同 nvn_gpu_att.clear）
    uint8_t depth, stencil;           // 1 = 清深度 / 清模板（通道必须带对应附件）
    uint8_t color_mask, stencil_mask; // 颜色写掩码（color >= 0 时至少一位；0xF = 四个通道）、模板写掩码（stencil = 1 时至少一位；0xFF = 八位）
    float depth_value; uint32_t stencil_value;
    int32_t x, y; uint32_t w, h;
} nvn_gpu_clear;
// 线程：[提交]，通道内。出错：粘滞 ORDER（通道外）/ ARG（下标越界、通道没有该附件、矩形越界或为空、什么都不清、掩码为 0）。
void nvn_gpu_clear_rect(nvn_gpu_cmd c, const nvn_gpu_clear *d);
// v20：DrawTexture（NVN nvnCommandBufferDrawTexture，#1452）。要 NVN_GPU_CAP_DRAW_TEX。对照 iOS nvn_mtl.m draw_tex 与 Ryujinx HelperShader.DrawTexture：
//   在当前通道里画一个四边形到颜色附件 0（pass_desc.color[0]），只写这一个附件；深度 / 模板测试关、不剔除、单样本着色，
//   视口 = 目的矩形（与 nvn_gpu_dyn.vp 同约定，窗口原点按 origin_ll 换，同普通绘制），裁剪照 sc / has_sc（同 nvn_gpu_dyn.sc）。
//   源按 uv 采样：(u0, v0) 落在裁剪空间 (-1, +1) 那一角、(u1, v1) 落在 (+1, -1) 那一角，中间线性插值；镜像由核心交换端点表达，dst 的 w / h 恒为正。
//   混合与写掩码只取 color(0) 这一份、由调用方显式给（游戏可以不先绑管线就调 DrawTexture，状态不从 set_pipe 取）。
//   同 clear_rect：不看也不保证当前管线、动态状态与绑定，调用之后核心照「新通道」那样在下一笔绘制前全部重发。
typedef struct {
    nvn_gpu_view src; nvn_gpu_samp samp;   // 源：二维颜色视图（浮点 / 归一化读，不收整数、深度模板）；采样器原样用
    float dst[4];                          // 目的矩形 x, y, w, h（像素，w / h > 0）
    float uv[4];                           // 源 u0, v0, u1, v1（按源贴图尺寸归一化）
    int32_t sc[4]; uint8_t has_sc;         // 裁剪（左上原点像素，必须整个落在通道里且非空）；has_sc = 0 = 整个通道
    uint8_t origin_ll;                     // 窗口原点在左下（同 nvn_gpu_dyn.origin_ll）
    uint8_t blend_en;                      // color(0) 开混合（blend 生效）
    uint8_t color_mask;                    // color(0) 写掩码 r=1 g=2 b=4 a=8，至少一位
    blend_st blend;                        // color(0) 的混合；不收高级混合（adv_mode）
    float blend_color[4];
} nvn_gpu_draw_tex_desc;
// 线程：[提交]，通道内。出错：粘滞 ORDER（通道外）/ ARG（句柄为空、矩形为空 / 非有限、裁剪越界或为空、掩码为 0、通道没有颜色附件 0）/
//   UNSUPPORTED（没置 CAP_DRAW_TEX、源不是二维颜色视图或是整数 / 深度模板、颜色附件 0 是整数格式、高级混合、设备没开 dualSrcBlend 却给了双源因子、
//   源正是本通道的附件）。这些核心都在调用前筛掉并具名记账；走到这里报的是核心的错。
void nvn_gpu_draw_tex(nvn_gpu_cmd c, const nvn_gpu_draw_tex_desc *d);
// NVN 屏障（NVNbarrierBits 原样）。
/* 定稿 #7：核心只表达 NVN 的顺序与访问语义（这个位、这次拷贝、这次附件写）；Vulkan 后端自己跟踪每个子资源的 layout / stage / access 并据此发屏障，
   VkImageLayout 不进核心；Metal 4 后端同理自己补编码器间屏障，Metal 3 交给自动冒险追踪。经裸 GPU 地址的写后读只有 NVN 位能表达，核心照游戏原样传。 */
void nvn_gpu_barrier(nvn_gpu_cmd c, uint32_t nvn_barrier_bits);
// 核心记计算写后的读依赖：身份是后端存储的生命周期代号（同存储的不同视图相同，销毁重建不复用）。
// 缓冲按字节；图像按面 / mip / 层的矩形（3D 层表示深度切片）。只查询元数据，不提交、不等待、不改变布局。失败返回 0。
// 面位是契约值（COLOR=1 / DEPTH=2 / STENCIL=4），不携带平台 API 位。
typedef struct {
    uint64_t storage, off, bytes;
    uint32_t level, levels, layer, layers, aspects;   // aspects=0 表示缓冲
} nvn_gpu_access_span;
int nvn_gpu_buf_access_span(nvn_gpu_buf b, uint64_t off, uint64_t bytes, nvn_gpu_access_span *out);
int nvn_gpu_view_access_span(nvn_gpu_view v, nvn_gpu_access_span *out);
// A dynamic prefix comes from the same segment-owned CPU DRV0 slice the
// fragment shader will bind. A null buffer describes a constant prefix.
struct nvn_gpu_cold_prefix {
    nvn_gpu_buf buf;
    nvn_gpu_access_span binding;
    uint32_t fragment_ent, byte, mask;
};
struct nvn_gpu_cold_domain {
    nvn_gpu_buf buf;
    nvn_gpu_access_span binding;
    uint32_t first_word, stride_words, count;
    uint32_t fragment_ent;
    struct nvn_gpu_cold_prefix prefix;
};

// 通道外：此前计算写入对后续间接参数 / 索引 / 顶点 / 图形着色器读取可见；不包含附件载入、计算或传输，不分段、不等待。
// 由核心仅在图形实际读到计算写过的范围时调用；全体计算写对这些读阶段发布，所以核心可一起撤掉待发布表。
void nvn_gpu_compute_to_graphics(nvn_gpu_cmd c);

void nvn_gpu_occlusion(nvn_gpu_cmd c, int32_t slot);   // 之后的绘制计入 slot，-1 停止。通道内且 pass_desc.occlusion = 1
void nvn_gpu_label(nvn_gpu_cmd c, const char *text);   // 调试标签压栈，NULL 出栈

// #653：槽归命令段，按需分配、上限 64，不预扫命令流。仅 CAP_REPORT_TIMESTAMP 时调用。
// 线程：[提交]，通道内外均可；type 仅 NVN TIMESTAMP / TIMESTAMP_TOP（普通=全部命令完成，TOP=管线顶）。
// 每槽最多编码一次；重复 / slot>=64 / 类型非法为粘滞 ARG，无能力为粘滞 UNSUPPORTED。
// 核心不为超额报告调用此入口：该报告沿用 guest_tick 并记账，不能让整段失败。
// abort 不产生结果；查询资源活到 done 返回。无校准、不可用及失败结果不能伪装成主机时间。
enum { NVN_GPU_TIMESTAMP_SLOTS = 64 };
void nvn_gpu_timestamp(nvn_gpu_cmd c, uint32_t slot, uint32_t nvn_counter_type);
typedef struct { uint64_t host_ns; uint8_t valid; } nvn_gpu_timestamp_result; // 0 是合法时间，必须看 valid

// ---------- 提交与完成（定稿 A4 / A6） ----------
enum {
    NVN_GPU_DONE_OK = 0,      // 已执行完
    NVN_GPU_DONE_FAULT = 1,   // 已执行完但 GPU 报了故障（rc = NVN_GPU_E_GPU_FAULT）：非终态，时间线照常推进，核心照做回写并决定隔离（Metal 现行口径）
    NVN_GPU_DONE_FAILED = 2,  // 没有执行或执行结果不可信（提交失败、带粘滞错误、设备丢失、排在失败段之后）：核心不得做任何成功回写
};
enum { NVN_GPU_TIME_NONE = 0, NVN_GPU_TIME_DURATION = 1, NVN_GPU_TIME_HOST = 2 };
typedef struct {
    uint64_t signal;          // 本段的时间线值：严格大于上一段（所以从不为 0）；违约被拒的段回调时是调用方给的原值
    uint32_t status;          // NVN_GPU_DONE_*
    nvn_gpu_rc rc; const char *why;   // why 只在回调期间有效
    uint32_t time_kind;       // NONE：没有时间；DURATION：只有 gpu_dur_ns 可信；HOST：gpu_t0 / gpu_t1 也可信（主机单调时钟 ns，CAP_GPU_TIME_HOST）
    uint64_t gpu_dur_ns, gpu_t0, gpu_t1;   // 不可信的字段为 0，0 不表示「零耗时」，看 time_kind
    const nvn_gpu_timestamp_result *timestamps; uint32_t ntimestamps; // 回调期间只读；FAILED 无有效槽，FAULT 仅消费 valid 槽
    const uint64_t *occlusion; uint32_t nocclusion;   // 各槽样本数（只在回调期间有效；FAILED 时为 NULL）
    const uint8_t *readback; uint64_t readback_bytes;   // CPU 可见，只在本回调有效。已录但数据不可信时整段 FAILED；FAILED 不附数据。
} nvn_gpu_done;
typedef void (*nvn_gpu_done_fn)(void *user, const nvn_gpu_done *d);
// 提交一段。
// 成功路径的发布顺序：GPU 完成 → 后端调 done（核心在这里做完成后才可见的 CPU 副作用：事件值、计数器、遮挡结果写回游戏内存）→ done 返回后，
//   后端以 release 语义把已完成值推进到 signal；fence_done / fence_wait 以 acquire 语义读——等到 signal 的线程一定看得到 done 里的写。
// 失败（定稿 A4：选「终止后续」而不是「记失败区间」）：第一个 FAILED 段的 signal 记为 F。后端先以 release 语义发布 F，再以 FAILED 调该段 done（恰好一次）；
//   之后的每一段（已提交的和以后要提交的）都不执行、各以 FAILED 调一次 done；已完成值永远停在 F 之前。fence_wait(v ≥ F) 返回 NVN_GPU_E_FAILED，
//   不会因为别的值更大而成功。恢复只能关会话重开。理由：NVN 队列严格按序，后面的段读前面段的 GPU 结果，前面一段没执行，后面「成功」也是建在缺失内容上，
//   发布它就是假成功；这也是安卓现在 g_lost_from_fence 的口径。FAULT（已执行、报故障）不触发终止，保持 Metal 现行行为（隔离 quar_*）。
// 线程：[提交]；done 在 [回调]。生命周期：c 在本调用后失效（失败时也失效，等价 abort + FAILED 回调）。
// 出错：返回本段粘滞错误或提交失败原因。
// signal 不严格大于上一次合法提交的值（含 0）（#968 消费审查定稿 ②，10-30）→ 返回 NVN_GPU_E_ARG，本段不提交，按失败处理：后端先以 release 语义发布
//   F = 上一次合法 signal + 1（已有更小的 F 就保持原值），再以 FAILED（rc = NVN_GPU_E_ARG）调本段 done 恰好一次；之后同 A4 终止。
//   理由：被拒的段等于严格按序的队列里缺了一段，后面的段可能读它的结果，与没执行的段同理不能当没发生；F 取「上一次合法值 + 1」，以后任何合法的 signal 都 ≥ F，
//   等它都报 FAILED。只返回 E_ARG、不发布不回调（v2 原稿的写法之一）会让核心等这段的 done 永远等不到、它的回写与资源退休也悬空。
nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd c, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err);
uint64_t nvn_gpu_fence_done(void);     // 已完成到的值（只算已调完 done 的成功 / 故障段）。线程：[任意]，acquire
uint64_t nvn_gpu_fence_failed(void);   // 第一个失败段的 signal F，0 = 没有失败。线程：[任意]，acquire
// 等时间线 ≥ v。timeout_ns = 0 纯查询、不产生副作用。返回 OK / NVN_GPU_E_TIMEOUT / NVN_GPU_E_FAILED（v ≥ F）/ NVN_GPU_E_DEVICE / NVN_GPU_E_ARG（v 从没提交过）。
// 线程：[任意]，[回调] 里调返回 NVN_GPU_E_ORDER。
nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err);

// ---------- 上屏（定稿 A5 / A6 / #8） ----------
// 接平台层给的窗口（Apple：CAMetalLayer 指针；安卓：WSI 租约里的 ANativeWindow 指针）；后端不自己去要窗口，租约归平台层。
// 每次成功 attach 或重建得到一个新 generation（从 1 起，单调递增）。线程：[上屏]。出错：NVN_GPU_E_UNSUPPORTED / NVN_GPU_E_ORDER（已接着一个）/ NVN_GPU_E_REVOKED。
nvn_gpu_rc nvn_gpu_surface_attach(void *native, nvn_gpu_surface *out, nvn_gpu_err *err);
// 过期后在同一窗口上重建（交换链换代）：返回 OK 后 generation +1，旧 generation 的 drawable 全部作废。窗口已撤销返回 NVN_GPU_E_REVOKED。线程：[上屏]。
nvn_gpu_rc nvn_gpu_surface_rebuild(nvn_gpu_surface s, nvn_gpu_err *err);
uint64_t nvn_gpu_surface_gen(nvn_gpu_surface s);   // 当前 generation（0 = 面已撤销）。线程：[任意]
// 撤销：等最后一帧呈现退休，所有已取未呈现的 drawable 收回。OK = 可以归还窗口租约；NVN_GPU_E_TIMEOUT = 退休没证明，调用方必须保留租约。线程：[上屏]。
nvn_gpu_rc nvn_gpu_surface_detach(nvn_gpu_surface s, uint64_t timeout_ns, nvn_gpu_err *err);
typedef struct { nvn_gpu_view view; uint32_t w, h, nvn_format; uint64_t gen; uint64_t token; } nvn_gpu_drawable;
// 取一张交换链图。plat_frame：在显示节拍回调里调用时传平台给的帧对象（nvn_vsync.h 的 tick 参数；Apple 的 drawable 只从它来），否则 NULL。
// 所有权：成功后 drawable 归调用方，必须且只能经 nvn_gpu_present 或 nvn_gpu_drawable_release 交回一次。失败（含 TIMEOUT）时调用方不拥有任何 drawable。
// 线程：[上屏]。出错：NVN_GPU_E_TIMEOUT（没有空闲图，按丢帧算）/ NVN_GPU_E_OUTDATED（要 rebuild）/ NVN_GPU_E_REVOKED。
nvn_gpu_rc nvn_gpu_drawable_acquire(nvn_gpu_surface s, uint64_t timeout_ns, void *plat_frame, nvn_gpu_drawable *out, nvn_gpu_err *err);
// 取了不呈现，原样交回（Vulkan 有 swapchain_maintenance1 时释放，没有时后端留作下次 acquire 的结果）。线程：[上屏]。
void nvn_gpu_drawable_release(nvn_gpu_surface s, const nvn_gpu_drawable *dr);
typedef struct {
    nvn_gpu_view src; int32_t crop[4];
    int32_t dst[4];                      // 核心按 nvn_present_fit.h 算好；矩形外后端清黑
    uint32_t filter;                     // 0 最近点 1 线性 2 后端的抗锯齿缩放
    double at;                           // 期望呈现时刻（主机单调秒；0 = 尽快）
    uint8_t flip_y, _rsv[7];             // 1 = 源是自下而上存的（NVN 窗口原点在左下）：呈现时上下翻过来（v3 新增）
} nvn_gpu_present_desc;
enum { NVN_GPU_SHOWN_AT = 0, NVN_GPU_SHOWN_DROPPED = 1, NVN_GPU_SHOWN_UNKNOWN = 2 };
typedef struct { uint64_t ticket; uint32_t status; double at; } nvn_gpu_shown;   // at 只在 SHOWN_AT 时有意义（主机单调秒）
typedef void (*nvn_gpu_presented_fn)(void *user, const nvn_gpu_shown *s);
// 把 src 画进 drawable 并呈现。后端自己持有内部 blit / present 命令，排在同一 owner 之前提交的段之后，寿命归 close 管（定稿 #8）。
// 所有权：无论返回什么，dr 都已交回（不许再 present / release）。presented 对每个 ticket 恰好调一次（[回调]）：
//   有 CAP_SHOWN_TIME 且拿到实际上屏时刻 → SHOWN_AT；确知没上屏 → DROPPED；上了屏但不知道时刻 → UNKNOWN。
//   不许拿 CPU 端 present 返回时刻或渲染完成栅栏冒充上屏时刻（定稿 A6）。失败时 presented 以 DROPPED 回调。
// 线程：[上屏]。出错：NVN_GPU_E_OUTDATED（dr.gen 不是当前 generation，或交换链刚过期）/ NVN_GPU_E_REVOKED / 提交失败原因。
nvn_gpu_rc nvn_gpu_present(nvn_gpu_surface s, const nvn_gpu_drawable *dr, const nvn_gpu_present_desc *d, uint64_t ticket,
                           nvn_gpu_presented_fn presented, void *user, nvn_gpu_err *err);
/* 定稿 #9：显示节拍源（CAMetalDisplayLink / AChoreographer）不是图形 API，移出 nvn_gpu_*，放进平台接口 nvn_vsync.h；
   interval / hold / 选帧（nvn_mtl.m NvMetalDisplayLinkDelegate 里那段）归核心。 */

// ---------- 后端读数 ----------
// 枚举后端自己的计数，name 为稳定的 ASCII 名；取代三十多个 nvn_be_*_stats 出口与安卓那排返回 0 的桩。take = 1：读后清零的计数清零。线程：[任意]。
typedef void (*nvn_gpu_stat_fn)(void *user, const char *name, uint64_t value);
void nvn_gpu_stats(nvn_gpu_stat_fn fn, void *user, int take);

_Static_assert(NVN_GPU_E_LAST >= NVN_GPU_E_BASE - 99 && NVN_GPU_E_ARG < NVN_GPU_E_BASE, "nvn_gpu 错误码必须留在 [-1099, -1000]");

#ifdef __cplusplus
}
#endif
