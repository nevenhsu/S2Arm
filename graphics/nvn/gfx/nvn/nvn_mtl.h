// Backend interface of the NVN null device (nvn_be_*: what the front ends hle_nvn.c / hle_vk.m / egl.m call; #199 WP17 D0), implemented here for Metal: memory pool = zero-copy MTLBuffer, submit = MTLCommandBuffer, sync = MTLSharedEvent.
// Ordering convention (to avoid repeating the GO_MAP_SHARE corrupted-picture mistake of the x86 route): every visible effect of "GPU completion" (sync object ready, event values, counters)
// happens only after the Metal command buffer has actually completed; CPU-side waits look only at the completed value of the MTLSharedEvent.
#pragma once
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void nvn_be_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip);
void nvn_be_set_depth_zero(int on);          // NVN depth mode: 1 = near is 0 (D3D / Metal), 0 = near is -w (GL, NVN's default); picks the vertex shaders' entry point (see shader_load in nvn_mtl.m)
void nvn_be_set_origin_lower_left(int on);
void nvn_be_set_present_lower_left(int on);   // the presented window's origin at WindowInitialize (hle_nvn.c win_t.present_ll): 1 = its image is stored bottom-up, flipped at present   // NVN window origin: 1 = lower left (GL, NVN's default), 0 = upper left (D3D / Metal); see nvn_mtl.m
void bf_want_note(const char *key);

// Memory pool: [mem, mem+size) is expanded outward to page boundaries to build a zero-copy buffer; look up the buffer and offset by GPU address (= CPU address)
void nvn_be_pool_add(const void *mem, uint64_t size);
void nvn_be_pool_cover(const void *mem, uint64_t size, uint64_t need);   // 池上伸出池尾的缓冲对象：池在 Metal 侧的覆盖扩到 [mem, mem + need)（D-6VG6）
void nvn_be_pool_del(const void *mem);
// 安卓直连：宿主已建立可读写别名；phys 为 0 时只撤掉该虚拟区间。
int rp_buf_alias_set(uint64_t at, uint64_t len, uint64_t phys);
// 虚拟池（hle_nvn.c，苹果宿主）：[at, at+len) 是虚拟池地址（不是宿主内存）。alias：登记成物理内存 phys 所在那条池的别名（同一个 Metal 缓冲、同一批字节，不另占内存）；
//   phys 不整段落在一条已登记的池里 → 0。add_at：一段宿主内存 host（页对齐，hlen 字节）登记在 at，缓冲释放时 munmap 它（失败时当场 munmap）；key 是注销用的键（nvn_be_pool_del）。都按 key 注销
int nvn_be_pool_alias(uint64_t at, uint64_t len, uint64_t phys);
int nvn_be_pool_add_at(uint64_t at, uint64_t len, void *host, uint64_t hlen, const void *key);
void nvn_be_pool_addressed(const void *mem, uint64_t size);   // the game holds a GPU address in this pool range, so raw global memory may reach it (nvn_mtl.m gmem_map)
// memory pool Finalize: evict the textures built on this pool (pool object address) from the storage-sharing table
void nvn_be_alias_drop_pool(const void *pool);
int nvn_be_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // 1 on success; buf is a (__bridge) id<MTLBuffer>

typedef struct {
    uint32_t pso, cps, shaders, dss;
    uint32_t view_nonnull, view_slots, live_tex, sview_nonnull;
    uint32_t pools;
    uint64_t pool_adds;
    uint32_t c1_bufs; uint64_t c1_alloc, c1_data;   // Shader c1 constant segment: buffer count (4 MiB blocks + dedicated 64 KiB), allocated bytes, actual bytes
} mtl_res_count;
void mtl_res_counts(uint32_t tick, const char *why);   // Print one line of resource counts (the host calls it per tick / event, see dm_res_report_register in dm.h)
void mtl_res_snapshot(mtl_res_count *out);

// One submit: begin -> several operations -> commit (returns the fence value at which this submit completes)
void nvn_be_sub_begin(void);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);               // Buffer copy in GPU ordering
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // Write the event value after completion (0 write / 1 add)
// GPU 等事件（nvnCommandBufferWaitEvent）：与 SignalEvent 同一条 OP_SIGNAL_EVENT 记录 {事件地址, 方式, 值}，方式带 NVN_EV_WAIT 位 = 等到事件满足条件
//   （方式 0 等于 / 1 回绕大于等于，NVNeventWaitMode）再往下执行；不另开命令码（nvn_obj.h 是出包输入，改它全库直连库重编）
#define NVN_EV_WAIT 0x80000000u
void nvn_be_event_written(volatile uint32_t *val);   // CPU 写过事件值（nvnEventSignal）：记下这个事件由 CPU 写、复查在等的 GPU 等待，满足的放行
void nvn_be_event_gpu_recorded(volatile uint32_t *val);   // 游戏在某个命令缓冲里录了写这个事件的 GPU 侧 SignalEvent（录制时，不等提交）：记下它由 GPU 侧写，回放到等待点时按跨队列信号处理（nvn_mtl.m ev_wait ⑤）
void mtl_sub_counter(uint64_t addr, uint32_t type);                       // Write the counter record after completion
uint64_t nvn_be_sub_commit(void);
// 一次 NVN 队列提交（hle_nvn.c nvnQueueSubmitCommands）：按跨队列事件顺序回放（nvn_mtl.m nvn_xqueue.inc）——等别的队列还没回放的信号时这条队列余下的命令停住，
//   信号回放后再接着回放；其余按 sub_begin → sub_index → exec → sub_commit。每段回放前调 nvn_hle_queue_enter(q)、sub_begin 后调 nvn_hle_piece_begun()（hle 实现）
void nvn_be_submit(void *q, const uint64_t *h, int n);
void nvn_be_xq_flush(void *q, const char *why);                // CPU 要这条队列的工作（QueueFinish / 上屏）：停住的部分立刻放行回放
int nvn_be_xq_fence(void *q, void *sync);                       // QueueFenceSync：q 有停住的工作 → 回 1，这个栅栏欠着，那部分回放提交时再发布
int nvn_be_xq_owed(const void *sync, void **q);                 // 这个同步对象的栅栏是否欠着（q = 欠它的队列）
int nvn_be_xq_owed_wait(const void *sync, uint64_t ns);         // 等欠着的栅栏发布，最多 ns 纳秒；1 = 已发布
void nvn_be_xq_wait_queue(void *q, void *owner);                // QueueWaitSync(q, 欠在 owner 上的栅栏)：q 之后的提交排在 owner 停住的工作后面

uint64_t nvn_be_last_fence(void);                       // Fence value of the most recent submit (0 = never submitted)
int nvn_be_wait(uint64_t fence, uint64_t timeout_ns);   // Returns 1 when the GPU has completed up to fence, 0 on timeout
void nvn_be_finish(void);                               // Wait for all submitted commands to complete
uint64_t nvn_be_finish_publish(void);                   // nvnQueueFinish under the submit lock: flush this thread's upload batch, return the fence to wait for
void nvn_be_finish_wait(uint64_t fence);                // ... then, without the lock: wait for that fence and the pending uploads
void *nvn_be_fence_event(void);                                       // 本层提交栅栏的共享事件（MTLSharedEvent，值 = 已完成栅栏号；GL 层跨队列等待用）
void nvn_be_queue_wait_event(void *ev, uint64_t v);                    // 此后提交的 NVN 工作在 GPU 上等共享事件 ev 到 v
int nvn_be_event_wait(void *ev, uint64_t v, uint64_t timeout_ns);      // CPU 等共享事件 ev 到 v，1 = 已到
int nvn_be_wait_error(uint64_t *seen, uint64_t timeout_ns);            // 等出现比 *seen 新的 GPU 执行错误，1 = 有（*seen 更新）
void nvn_be_drain(void);                                // Wait for all commands on the queue (including presents) to complete: submit an empty command buffer and wait for it
void nvn_be_xv_window_tex(uint64_t serial);   // Register a window (swapchain) texture: the cross-submit validity ledger never collects it
// External producers (the Vulkan layer, dm/gfx/vk): the shared Metal device; a serial for one of their textures; present it once their event reaches ev_val (ticket for nvn_be_present_wait)
void *mtl_device(void);
void *mtl_nocopy_buf(void *p, uint64_t n);   // 零拷贝 MTLBuffer（+1 引用，调用方 __bridge_transfer 接）：与本层同一记账入口 nocopy_buf，浮层「GPU 已分配」不重复计入（#1827）；nil = 建不出
uint64_t mtl_ext_tex(void *tex);
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]);   // crop: x, y, w, h of the texture to show (NULL = all of it)
void mtl_fit_pillar(int kind);   // 满屏判据免判种类（同 g_fit_pillar：1 = 核心说这帧是 16:9 原比例页；2 = 上屏贴图两侧是放宽内容；5 = 原比例回退）：GL / Vulkan 前端上屏前按宽屏核心结论设
void nvn_be_present_wait(uint64_t ticket);   // as in nvn_obj.h: wait until that present has been handed to the GPU
void nvn_be_link_sample(uint64_t *ticks, uint64_t *time_ns);   // 只读实际计数拍 / 最近拍的单调时刻，用两次采样量显示链接频率
