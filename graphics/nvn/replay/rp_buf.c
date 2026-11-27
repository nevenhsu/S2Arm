// 内存池在后端的镜像缓冲，与缓冲类命令。
// iOS 上内存池就是零拷贝的 MTLBuffer（nvn_mtl.m pool_add @556），GPU 直接读游戏内存。Vulkan 没有通用的等价物，这里的做法是：
//   · 游戏内存是唯一真值；每个被当缓冲用到的池在后端有一份等大的镜像，按页同步；
//   · 同步点 = 每次使用点（绘制 / 派发 / 拷贝取这段地址时）：自上次同步后被写过的页，在当前段的 GPU 顺序里重传；
//   · 所以按 GPU 顺序生效的 CPU 数据写（OP_UPDATE_UB / OP_COPY_B2B / OP_CLEAR_BUF）只需要改游戏内存：
//     排在它前面的使用点已经带着旧内容同步过，排在它后面的使用点会看到页脏了再同步新内容。
// GPU 往池里写（贴图拷回缓冲、计算着色器写存储缓冲、变换反馈）要反方向回写游戏内存，随那几条命令一起接。
// 平台有零拷贝后备（安卓 AHB）时镜像与同步都可以省掉，那是平台接口「内存池后备」一组，接它不改这里的对外形状。
#include "rp_exec.h"
#include "rp_res.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_hmap.h"
#include "nvn_fmt_info.h"
#include "nvn_dirty.h"
#include "nvn_mtl.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// off/len 是镜像持有的完整区间；copy_len 是从 off 起需要回写到客体的字节数。只在同次提交、同一镜像内合并。
struct gpu_write { uint64_t off, len, id; uint8_t mi, landing; uint64_t sub, copy_len; };
// 一份镜像。synced[i]：第 i 页上次同步取的脏页代号，0 = 从没同步；sub[i]：那次同步所在的提交序号；
// checked[i]：第 i 页上次逐页核对时的全局写代号（代号没动 = 那之后游戏没写过任何被跟踪的页，这一页不用再核）。
// use_sub / use_fence：最近一次用到这份镜像的提交（序号、栅栏值）；prev_fence：再往前一次用到它的提交的栅栏值——
// 它做完了，显卡就不再读这份镜像里的旧内容，这次提交第一次同步的页可以直接写。
// path：这份镜像走哪条核对路（建镜像时定，之后不变）：MP_LOG = 脏页追加日志（缺省，开关 nvk_buf_dirtylog=0 可关）；MP_PLAIN = 逐页（日志不可用时的退回路径）。
// 脏页追加日志（#1501 A②）：stale 每页一位 =「这份镜像里这页是旧的」，stale_sum 每位汇总 stale 的一个字（一位管 64 页，一个字管 4096 页）；
// log_cur / log_id：这份镜像在池的追加日志里读到哪（nvn_dirty_log_*）。两份镜像各有一套，互不影响。
enum { MP_PLAIN = 0, MP_LOG = 2, READ_BUCKETS = 32, EXACT_RANGES = 64 };
struct read_range { uint64_t sub, lo, hi; };   // 页区间 [lo, hi]；同桶碰撞取包络，宁可多走有序传输
struct byte_range { uint64_t lo, hi; };        // 镜像内半开区间 [lo, hi)
struct mirror_exact { uint64_t sub; unsigned n; struct byte_range ranges[EXACT_RANGES]; };
struct mirror { nvn_gpu_buf buf; uint64_t *synced, *sub, *checked; uint64_t seen_map; uint8_t seen_ok, path; uint64_t use_sub, use_fence, prev_fence;
                uint64_t *stale, *stale_sum; uint64_t log_cur, log_id, alias_sub;
                struct read_range reads[READ_BUCKETS]; struct mirror_exact *exact; };
// 每个池最多两份镜像。平时只有第 0 份；游戏的 Finish 被放行、不等上一帧做完时（rp_exec.c nvn_be_finish_publish），每帧换一份用：
// 上一帧的活显卡还在做，读的是另一份。两份各记各的页代号，换回来时自己把这两帧里变过的页补上。
// landing：正在锁外把 GPU 写回的内容拷进游戏内存（拷贝是 13 MB/帧量级，压在全局锁里会把提交线程挡住 11–21 毫秒/帧）。
// 池销毁时须等拷贝完成再返回：调用方可能在返回后立即解除游戏内存的映射。
// landing/nlr：有多段「显卡写回」正在锁外拷进游戏内存。拷贝期间**只**禁止对这些区间做「直接写镜像」——
// 以前按整份池禁止，走路时剔除数据每帧都在落地，于是几乎每次解析都退回「录成 GPU 拷贝 + 全屏障」，
// 真机表现为走路一顿一顿（池镜像同步 4.2 MB/帧、其中直接写只剩 0.2 MB，见坑记录续 33）。
struct land_rng { uint64_t off, len; };
struct shadow { const uint8_t *mem; uint64_t size; struct mirror m[2]; uint64_t npages; uintptr_t page0; struct gpu_write *gw; int ngw, cgw; int landing;
                int log_on;
                int untracked;      // 整个池不在写跟踪里（nvn_dirty_span 取不到）：没有页代号可比，页新旧靠「本次提交里同步过」加「回放自己登记的写」判，见 page_fresh
                const void *span;   // log_on 时：整个池在写跟踪里的范围句柄（日志按它读）
                int failed;   // 第一份镜像建不出来：记住，之后这个池直接按未接返回，不再每笔重建（池释放时随表项删掉，重建池会重试）
                struct land_rng lr[16]; int nlr; };
static int g_mirror_cur;   // 现在用第几份；只在提交锁里改（Finish 与提交互斥）
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_land_cv = PTHREAD_COND_INITIALIZER;
static nvn_hmap g_shadow = {.klen = 8};   // 池起点 → struct shadow *
static uint64_t g_sync_bytes, g_sync_runs;
static nvn_gpu_buf g_null_storage;
static int g_gw_live;   // 所有池的未落地登记数（含正在落地的项）
static int g_untracked_shadows;   // 不受写跟踪的池里有镜像的个数（原子读写）：为 0（受跟踪的游戏全程如此）时 rp_write_note 不多走一步
static int g_exact_on, g_exact_unknown;   // g_bmu 下，整次提交锁定，拆段不清零
static uint64_t g_exact_mapping, g_exact_pool;
static int g_gw_merge_on = -1; static unsigned g_gw_merge_k;   // 开关 nvk_land_merge 的缓存与读取计数（见 rp_buf_gpu_write_note2）
static int g_land_noresync = -1;   // g_bmu 内按会话锁定
static int land_noresync_on(void) {
    if (g_land_noresync < 0) {
        g_land_noresync = nvn_plat_flag("nvk_land_noresync", 1) != 0;
        nvn_plat_log(NVN_PLAT_LOG_INFO, "buf", g_land_noresync ?
                     "在途整页免同步（debug.s2a.nvk_land_noresync）=1" :
                     "在途整页免同步（debug.s2a.nvk_land_noresync）=0");
    }
    return g_land_noresync;
}
#ifdef S2A_GPU_LAND_TEST
int rp_test_gw_live(void) { return __atomic_load_n(&g_gw_live, __ATOMIC_ACQUIRE); }
void rp_test_gw_merge_reload(void) { __atomic_store_n(&g_gw_merge_on, -1, __ATOMIC_RELAXED); }   // 单测换开关值后让下一次登记立刻重读
int rp_test_gw_get(uint64_t pool_key, int i, uint64_t *off, uint64_t *len) {   // 这个池登记表第 i 条的区间；返回表里的条数（池没有镜像 = -1）
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_get(&g_shadow, &pool_key); int n = s ? s->ngw : -1;
    if (s && i >= 0 && i < s->ngw) { if (off) *off = s->gw[i].off; if (len) *len = s->gw[i].len; }
    pthread_mutex_unlock(&g_bmu);
    return n;
}
#endif

int rp_pool_find(uint64_t addr, uint64_t len, const uint8_t **mem, uint64_t *size);   // rp_entry.c
// 只链接镜像的后端夹具可能没有回放通道或池表。缺少任一证明时保留原同步顺序。
// 弱链接[replay]：rp_pass_is_open
int rp_pass_is_open(void) __attribute__((weak));
// 弱链接[replay]：rp_pool_mirror_epoch
uint64_t rp_pool_mirror_epoch(const uint8_t *mem, uint64_t size) __attribute__((weak));

// ---------- 回放窗口：别的线程销毁池时，回放手里的镜像缓冲不能当场放 ----------
// 一次提交的回放（rp_exec.c nvn_be_sub_begin → nvn_be_sub_commit）里，回放线程从 rp_buf_resolve 取到镜像缓冲句柄，到后端 cmd_ref 登记它之间没有任何锁护着；
// 游戏的别的线程这时销毁池（MapVirtual 合并 / 拆分 run 时对被动到的 run 先 pool_del 再 pool_add，或 MemoryPoolFinalize），镜像缓冲当场 release，
// 回放线程随后录制它 = 后端「release 之后仍在使用句柄」契约违规：整段提交失败，时间线进失败终态，恢复只能重开会话。
// 窗口开着、销毁来自别的线程：池表项与影子照常撤（之后的解析看不到这个池），镜像缓冲进待放列表，窗口收尾（rp_buf_hold_end）时一并 release；
// 命令已经 cmd_ref 的缓冲由后端引用计数保活到段完成。窗口外、或销毁来自开窗口的那个线程（同线程不会在回放中途销毁池）：原样当场放。
// 窗口只记「开了没有」不计层数：漏了配对的 end 时，下一次完整的 begin / end 自愈。
static int g_hold; static pthread_t g_hold_tid;   // 开着的回放窗口及开它的线程（持 g_bmu 读写）
static nvn_gpu_buf *g_limbo; static int g_nlimbo, g_climbo; static uint64_t g_limbo_total;   // 待放的镜像缓冲、累计延后放掉的个数（日志限流用）
static int limbo_push(nvn_gpu_buf b) {   // 调用方持 g_bmu；0 = 记不下（内存不足），调用方当场放
    if (g_nlimbo == g_climbo) {
        const int cap = g_climbo ? g_climbo * 2 : 8; nvn_gpu_buf *p = realloc(g_limbo, (size_t)cap * sizeof *p);
        if (!p) return 0;
        g_limbo = p; g_climbo = cap;
    }
    g_limbo[g_nlimbo++] = b; return 1;
}
static void mirror_free(struct mirror *m, int defer) {
    if (m->buf && !(defer && limbo_push(m->buf))) nvn_gpu_buf_release(m->buf, NULL, NULL);
    free(m->synced); free(m->sub); free(m->checked); free(m->stale); free(m->stale_sum); free(m->exact); memset(m, 0, sizeof *m);
}
static void shadow_free(struct shadow *s, int defer) { if (s->ngw) __atomic_sub_fetch(&g_gw_live, s->ngw, __ATOMIC_RELEASE); if (s->untracked) __atomic_sub_fetch(&g_untracked_shadows, 1, __ATOMIC_RELEASE);
                                                       mirror_free(&s->m[0], defer); mirror_free(&s->m[1], defer); free(s->gw); free(s); }
// 镜像映射代号：新建镜像、换镜像都会让「同一段地址解析出的 (缓冲, 偏移)」变掉。回放核心的解析结果缓存按它作废。

// ---------- 脏页追加日志（nvk_buf_dirtylog，#1501 A②）----------
// 解析一段时不按范围逐页比代号，只做两件事：把池日志里本镜像游标之后的事件灌进本镜像的「旧页」位图（O(新事件数)），
// 再在位图里取 [p, q] 内的置位页同步（O(范围 / 4096 + 旧页数)）。间接带索引绘制「索引地址到池尾」二十多万页的范围也是这个价。
// 一页只在「加保护之后、此刻只读」时才清位：加保护被并发的解保护挡住的页（nvn_dirty_page_armed 为 0）留着，下次再同步。
// 日志不可用（环绕、跟踪不可靠、换底、与别的池重叠）时按逐页代号重建一次位图（先起游标、后读代号），重建不了就整份镜像退回逐页路径。
static void stale_set(struct mirror *m, uint64_t p, uint64_t q) {   // [p, q]（含）置旧
    for (uint64_t w = p >> 6; w <= q >> 6; w++) {
        const uint64_t lo = w == p >> 6 ? (p & 63) : 0, hi = w == q >> 6 ? (q & 63) : 63;
        m->stale[w] |= (hi - lo == 63 ? ~0ull : ((1ull << (hi - lo + 1)) - 1) << lo);
        m->stale_sum[w >> 6] |= 1ull << (w & 63);
    }
}
static void stale_clear(struct mirror *m, uint64_t k) {
    const uint64_t w = k >> 6;
    m->stale[w] &= ~(1ull << (k & 63));
    if (!m->stale[w]) m->stale_sum[w >> 6] &= ~(1ull << (w & 63));
}
struct log_fill { struct shadow *s; struct mirror *m; uint64_t n; };
static void log_event(void *user, uintptr_t a, uintptr_t b) {   // 事件 [a, b)：宿主页对齐地址，裁到这个池
    struct log_fill *f = user; struct shadow *s = f->s; const size_t ps = nvn_dirty_page_size();
    const uintptr_t lo = s->page0, hi = s->page0 + (uintptr_t)(s->npages * ps);
    f->n++;
    if (b <= lo || a >= hi) return;
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    stale_set(f->m, (a - lo) / ps, (b - 1 - lo) / ps);
}
// 重建：先起游标（之后的写都会进日志），再按逐页代号标旧（这一刻之前的写），两者合起来不漏。整池 O(页数)，只在日志不可用时付一次。
static int log_rebuild(struct shadow *s, struct mirror *m) {
    if (!nvn_dirty_log_open(s->span, &m->log_id, &m->log_cur)) return 0;
    const size_t ps = nvn_dirty_page_size();
    enum { CH = 4096 };
    uint64_t gens[CH];
    for (uint64_t p = 0; p < s->npages; p += CH) {
        const uint64_t n = s->npages - p < CH ? s->npages - p : CH;
        uintptr_t a = s->page0 + (uintptr_t)(p * ps), e = s->page0 + (uintptr_t)((p + n) * ps);
        if (a < (uintptr_t)s->mem) a = (uintptr_t)s->mem;
        if (e > (uintptr_t)s->mem + s->size) e = (uintptr_t)s->mem + s->size;
        if (nvn_dirty_page_gens(s->span, (const void *)a, e - a, gens, CH) != (long)n) return 0;
        for (uint64_t k = 0; k < n; k++) if (!m->synced[p + k] || gens[k] > m->synced[p + k]) stale_set(m, p + k, p + k);
    }
    rp_perf_add(RP_PERF_BUF_LOG_REBUILD, 1);
    return 1;
}
// 新镜像：一页都没同步过，整份置旧；游标从现在起。
static int log_arm(struct shadow *s, struct mirror *m) {
    const uint64_t words = (s->npages + 63) / 64, sums = (words + 63) / 64;
    m->stale = calloc(words ? (size_t)words : 1, sizeof *m->stale);
    m->stale_sum = calloc(sums ? (size_t)sums : 1, sizeof *m->stale_sum);
    if (!m->stale || !m->stale_sum || !nvn_dirty_log_open(s->span, &m->log_id, &m->log_cur)) {
        free(m->stale); free(m->stale_sum); m->stale = m->stale_sum = NULL; return 0;
    }
    if (s->npages) stale_set(m, 0, s->npages - 1);
    return 1;
}
// 日志不可用且重建不了：这份镜像改走逐页路径（synced 一直按同步时刻记着，逐页路径接得上；checked 没维护，关掉它的快路一次）
static void log_drop(struct mirror *m) { free(m->stale); free(m->stale_sum); m->stale = m->stale_sum = NULL; m->path = MP_PLAIN; m->seen_ok = 0; }
// 弱链接[replay]：nvn_vk_cold_mirror_track
// 弱链接[replay]：nvn_vk_cold_mirror_host_write
void nvn_vk_cold_mirror_track(nvn_gpu_buf) __attribute__((weak));
void nvn_vk_cold_mirror_host_write(nvn_gpu_buf, uint64_t, uint64_t) __attribute__((weak));
static int mirror_make(struct shadow *s, int i) {
    struct mirror *m = &s->m[i];
    nvn_gpu_buf b = NULL; nvn_gpu_err err = {0};
    if (!nvn_gpu_ok(nvn_gpu_buf_create(s->size, NVN_GPU_MEM_SHARED, NULL, &b, &err))) {
        // 完整原因：池大小、申请字节、单次分配上限、SSBO 范围上限、错误码、后端原文。按池大小去重（同一池每笔都会走到这里）。
        nvn_gpu_caps cp; nvn_gpu_caps_get(&cp);
        static uint64_t said_size[8]; static int nsaid;
        int dup = 0; for (int k = 0; k < nsaid; k++) if (said_size[k] == s->size) dup = 1;
        if (!dup && nsaid < 8) { said_size[nsaid++] = s->size;
            char t[200]; snprintf(t, sizeof t, "池镜像（第 %d 份）建不出来：池 %p+%llu 字节，申请 %llu，分配上限 %llu，SSBO 范围上限 %llu，错误 %d：%s",
                                  i, (const void *)s->mem, (unsigned long long)s->size, (unsigned long long)s->size,
                                  (unsigned long long)cp.max_buf_bytes, (unsigned long long)cp.max_ssbo_range, (int)err.rc.code, err.why);
            nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", t); }
        return 0; }
    m->buf = b;
    if (nvn_vk_cold_mirror_track) nvn_vk_cold_mirror_track(b);
    rp_perf_add(RP_PERF_MIRROR_N, 1);
    m->synced = calloc(s->npages ? s->npages : 1, sizeof *m->synced);
    m->sub = calloc(s->npages ? s->npages : 1, sizeof *m->sub);
    m->checked = calloc(s->npages ? s->npages : 1, sizeof *m->checked);
    if (!m->synced || !m->sub || !m->checked) { mirror_free(m, 0); return 0; }   // 页表没分出来：不留半份镜像（之后每次使用都会读它）
    if (s->log_on && log_arm(s, m)) m->path = MP_LOG;   // 起不了日志就沿用逐页路径
    return 1;
}
static struct shadow *shadow_get(const uint8_t *mem, uint64_t size) {
    uint64_t k = (uint64_t)(uintptr_t)mem;
    struct shadow *s = nvn_hm_get(&g_shadow, &k);
    if (s) {
        if (s->failed) { nvn_unimpl("gpu", "buf_create_pool_mirror", mem); return NULL; }
        return s;
    }
    size_t ps = nvn_dirty_page_size();
    s = calloc(1, sizeof *s);
    if (!s) { nvn_unimpl("gpu", "pool_mirror_alloc_failed", mem); return NULL; }
    s->mem = mem; s->size = size;
    s->page0 = (uintptr_t)mem & ~(uintptr_t)(ps - 1);
    s->npages = ((uintptr_t)mem + size - s->page0 + ps - 1) / ps;
    s->untracked = nvn_dirty_span(mem, (size_t)size) == NULL;   // 池登记时就定了（写跟踪在登记池的同一刻起区间），之后不变
    // 脏页追加日志：开关开着、写跟踪给这个池起了日志（nvk_session 在登记任何池之前配置）才用；整池的范围句柄在这里取一次
    if (nvn_plat_flag("nvk_buf_dirtylog", 1) && (s->span = nvn_dirty_span(mem, (size_t)size))) { uint64_t id, cur; s->log_on = nvn_dirty_log_open(s->span, &id, &cur); }
    if (!mirror_make(s, 0)) { s->failed = 1; s->untracked = 0; nvn_hm_put(&g_shadow, &k, s); nvn_unimpl("gpu", "buf_create_pool_mirror", mem); return NULL; }
    if (s->untracked) __atomic_add_fetch(&g_untracked_shadows, 1, __ATOMIC_RELEASE);
    nvn_hm_put(&g_shadow, &k, s);
    return s;
}
// [off, off + len) 这一段该用哪一份镜像：显卡写过、还没回写落地的段只在写它的那一份里有；其余用当前这一份。
// 顺带记下「这次提交用到了它」。第二份建不出来就退回第一份（显卡还在用它时按序拷，结果照样对）。
static struct mirror *mirror_pick(struct shadow *s, uint64_t off, uint64_t len, int *mi) {
    int i = g_mirror_cur;
    for (int k = 0; k < s->ngw; k++) if (s->gw[k].off < off + len && off < s->gw[k].off + s->gw[k].len) { i = s->gw[k].mi; break; }
    if (!s->m[i].buf && !mirror_make(s, i)) i = 0;
    struct mirror *m = &s->m[i]; const uint64_t sub = rp_sub_serial();
    if (m->use_sub != sub) { m->prev_fence = m->use_fence; m->use_sub = sub; m->use_fence = rp_sub_fence(); }
    if (mi) *mi = i;
    return m;
}

void rp_buf_frame_flip(void) { pthread_mutex_lock(&g_bmu); g_mirror_cur ^= 1; pthread_mutex_unlock(&g_bmu); }

// 第 i 页在镜像里还是新的吗（新的 = 这次使用不用再传）。
//   · 受写跟踪的池：同步之后页代号没有超过同步时取的那个值，就是没人写过；
//   · 不受写跟踪的池（整池 nvn_dirty_span 取不到：安卓的虚拟池映射段属于这一类）没有代号可比。以前每次使用都整段重传，
//     而这类池的用法是一笔绘制取三四段、一帧取上千次：同一页一帧被重传几百次，每次再因「同一次提交里的第二次同步」把当前通道切开。
//     现在：本次提交里同步过的页算新的（命令按 NVN 的规矩只读提交那一刻之前写好的内容，提交之后游戏对同一处的改写不归这次提交），
//     回放自己在这次提交中途改了这一页（三条按序写命令、显卡写回落地）由 pages_invalidate_locked 作废。
//   · 受写跟踪的池遇到跟踪不可靠 / 一段页数太多（got != n）：仍然每次都传，行为不变。
static inline int page_fresh(const struct shadow *s, const struct mirror *m, uint64_t i, int tracked, const uint64_t *gens, uint64_t p, uint64_t sub0) {
    if (!m->synced[i]) return 0;
    if (tracked) return gens[i - p] <= m->synced[i];
    return s->untracked && m->sub[i] == sub0;
}
// 回放自己改了游戏内存池内偏移 [off, off + len)：两份镜像里（skip_mi 那份除外）这几页作废，下次使用必须重新同步。
// 只清 synced，不动 sub：sub 还记着「这页本次提交里用过」，重新同步时 sync_run 据此录成 GPU 上有序的拷贝，不会直接写进显卡可能正在读的镜像。
static void pages_invalidate_locked(struct shadow *s, uint64_t off, uint64_t len, int skip_mi) {
    if (!len || off >= s->size) return;
    if (len > s->size - off) len = s->size - off;
    const size_t ps = nvn_dirty_page_size();
    const uint64_t a = (uint64_t)(uintptr_t)s->mem + off, p = (a - s->page0) / ps, q = (a + len - 1 - s->page0) / ps;
    for (int i = 0; i < 2; i++) {
        struct mirror *m = &s->m[i];
        if (i == skip_mi || !m->synced) continue;
        for (uint64_t k = p; k <= q && k < s->npages; k++) m->synced[k] = 0;
    }
}
#include "rp_buf_alias.inc"

// 回放在 CPU 上改了游戏内存 [addr, addr + n)（rp_write_note 调）：落在不受写跟踪的池里就作废镜像里的对应页。
// 受跟踪的池靠写跟踪发现这些写，不需要；没有任何不受跟踪的池时（原子读）一步都不走。
static void buf_invalidate_written(uint64_t addr, uint64_t n) {
    if (!n || (!__atomic_load_n(&g_untracked_shadows, __ATOMIC_ACQUIRE) && !__atomic_load_n(&aliases_live, __ATOMIC_ACQUIRE))) return;
    const uint8_t *mem; uint64_t size;
    if (!rp_pool_find(addr, n, &mem, &size)) return;
    uint64_t k = (uint64_t)(uintptr_t)mem;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_get(&g_shadow, &k);
    if (s && s->untracked) pages_invalidate_locked(s, addr - k, n, -1);
    if (s && alias_count) {
        for (size_t i = 0; i < alias_count; i++) {
            const struct buf_alias *a = &aliases[i];
            if (a->pool == k && a->phys < addr + n && addr < a->phys + (a->hi - a->lo)) {
                for (int mi = 0; mi < 2; mi++) alias_stale(s, &s->m[mi], addr, n);
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_bmu);
}

// 当前镜像的在途登记可以首尾相接；另一份镜像里的字节不能证明本页完整。
static int gw_covers(const struct shadow *s, int mi, uint64_t a, uint64_t b) {
    while (a < b) {
        uint64_t end = a;
        for (int i = 0; i < s->ngw; i++) {
            const struct gpu_write *w = &s->gw[i];
            if (w->mi == mi && w->off <= a && w->off + w->len > end) end = w->off + w->len;
        }
        if (end == a) return 0;
        a = end;
    }
    return 1;
}
static int gw_covers_page(const struct shadow *s, int mi, uint64_t k, size_t ps, uint64_t *bytes) {
    const uintptr_t base = (uintptr_t)s->mem;
    const uintptr_t page = s->page0 + (uintptr_t)(k * ps);
    const uintptr_t a = page > base ? page : base;
    const uintptr_t end = page + ps < base + s->size ? page + ps : base + s->size;
    *bytes = end - a;
    return gw_covers(s, mi, a - base, end - base);
}
// 读写在同一份镜像里共用一张表。只并重叠或接邻的区间，保留空隙；溢出则整份镜像退回原顺序。
static inline void mirror_touch(struct mirror *m, uint64_t lo, uint64_t hi) {
    if (!g_exact_on || g_exact_unknown) return;
    const uint64_t sub = rp_sub_serial();
    if (!m->exact) {
        m->exact = calloc(1, sizeof *m->exact);
        if (!m->exact) { g_exact_unknown = 1; return; }
    }
    struct mirror_exact *x = m->exact;
    if (x->sub != sub) { x->sub = sub; x->n = 0; }
    if (x->n > EXACT_RANGES) return;
    for (unsigned i = 0; i < x->n;) {
        const struct byte_range r = x->ranges[i];
        if (r.lo <= lo && hi <= r.hi) return;
        if (r.hi < lo || hi < r.lo) { i++; continue; }
        if (r.lo < lo) lo = r.lo;
        if (r.hi > hi) hi = r.hi;
        x->ranges[i] = x->ranges[--x->n];
    }
    if (x->n == EXACT_RANGES) { x->n++; return; }
    x->ranges[x->n++] = (struct byte_range){lo, hi};
}

static void mirror_unknown_address(void) {
    pthread_mutex_lock(&g_bmu);
    g_exact_unknown = 1;
    pthread_mutex_unlock(&g_bmu);
}

// 把游戏内存的 [a, b)（池内偏移）传进镜像，绕开 GPU 写过、还没回写落地的那些段：
// 那些字节镜像里的才是新的，游戏内存里是旧的（同步按页，一页里别的字节脏了不能连它一起盖回去）。
static void sync_write(nvn_gpu_cmd xc, uint8_t *direct, struct shadow *s, struct mirror *m, uint64_t a, uint64_t b) {   // direct 非空：直接写进镜像的 CPU 映射（见 rp_buf_resolve）
    while (a < b) {
        uint64_t stop = b, skip_to = 0;   // 找起点落在 [a, b) 里最靠前的一段；a 本身在某段里就先跳过去
        int inside = 0;
        for (int i = 0; i < s->ngw; i++) { uint64_t g0 = s->gw[i].off, g1 = g0 + s->gw[i].len;
            if (g0 <= a && a < g1) { inside = 1; if (g1 > skip_to) skip_to = g1; }
            else if (g0 > a && g0 < stop) stop = g0; }
        if (inside) { a = skip_to; continue; }
        const uint64_t begin = a;
        if (direct) memcpy(direct + a, s->mem + a, (size_t)(stop - a));
        else nvn_gpu_buf_write(xc, m->buf, a, s->mem + a, stop - a);
        mirror_touch(m, a, stop);
        a = stop;
        if (direct && nvn_vk_cold_mirror_host_write) nvn_vk_cold_mirror_host_write(m->buf, begin, stop - begin);
    }
}

// GPU 往池里写（计算着色器写存储缓冲）。提交线程登记这一段，返回登记号；段完成时 rp_buf_gpu_write_land 把结果拷回游戏内存、撤掉登记。
// pool_key：这段所在池的起点（落地时凭它找镜像：池已经销毁就什么都不做）。0 = 地址不在登记过的池里。
// 同段合并（开关 nvk_land_merge，缺省开）：本次提交里已经登记过、同一份镜像、还没开始落地、区间与这一段相交或首尾相接的那一条，
// 直接扩成并集，不另起一条（merged 非空时才合并；返回已有那条的登记号，*merged 置 1，调用方不要再把它记进本次提交的落地表）。
// 为什么：落地是段完成时按登记逐条把镜像拷回游戏内存，一条登记就是一次整段 memcpy。同一块存储缓冲被多个派发反复绑定写、每次都登记整段绑定范围时，
// 重叠的几条拷的是同一份镜像里同一段字节（段完成时镜像里已是最终内容），拷一次与拷多次结果相同。
// 只并「相交 / 首尾相接」的：并集里没有任何没被写过的字节。中间有空隙的不桥接——空隙里的镜像字节可能比游戏内存里的旧，落地会把新内容盖掉。
uint64_t rp_buf_gpu_write_note_prefix(uint64_t addr, uint64_t len, uint64_t copy_len, uint64_t *pool_key, int *merged) {
    static uint64_t next_id;
    const uint8_t *mem; uint64_t size;
    if (merged) *merged = 0;
    if (!copy_len || copy_len > len) return 0;
    addr = buf_address(addr, len);
    if (!rp_pool_find(addr, len, &mem, &size)) { mirror_unknown_address(); return 0; }
    if (g_gw_merge_on < 0 || !(++g_gw_merge_k & 255)) g_gw_merge_on = nvn_plat_flag("nvk_land_merge", 1) != 0;   // 开关：每 256 次登记重读一次
    const int merge_on = g_gw_merge_on;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = shadow_get(mem, size); uint64_t id = 0;
    if (s) {
        const uint64_t off = addr - (uint64_t)(uintptr_t)mem, sub = rp_sub_serial();
        rp_perf_add(RP_PERF_GW_REG_N, 1); rp_perf_add(RP_PERF_GW_REG_KB, len / 1024);
        if (merged && merge_on) {
            int mi = g_mirror_cur;   // 与 mirror_pick 同一规则（不带它记账的副作用）：与任一未落地登记相交就跟着那一条的镜像，否则用当前这一份
            for (int k = 0; k < s->ngw; k++) if (s->gw[k].off < off + len && off < s->gw[k].off + s->gw[k].len) { mi = s->gw[k].mi; break; }
            // 同一份镜像、同一次提交、没开始落地的登记之间两两互不相接（每次登记都经过这里合并），所以与这一段相接的记录只可能直接与这一段相接，单遍扫描即可。
            // 那一份镜像还没建出来 = 不可能有记在它上面的登记，也就没有可并的：走下面的老路（mirror_pick 会建它）。
            int hit = -1; uint64_t lo = off, hi = off + len, copy_hi = off + copy_len, old_sum = 0;   // 并集 [lo, hi)；old_sum = 被并掉的旧记录原长度之和
            for (int k = 0; s->m[mi].buf && k < s->ngw; k++) {
                struct gpu_write *g = &s->gw[k];
                if (k == hit || g->sub != sub || g->mi != mi || g->landing || g->off > hi || lo > g->off + g->len) continue;   // 相交或首尾相接才并
                if (g->off < lo) lo = g->off;
                if (g->off + g->len > hi) hi = g->off + g->len;
                if (g->off + g->copy_len > copy_hi) copy_hi = g->off + g->copy_len;
                old_sum += g->len;
                if (hit < 0) { hit = k; id = g->id; }
                else {   // 又一条与并集相接：吸进第一条，自己撤掉（它的登记号还在本次提交的落地表里，迟到的落地按「找不到」忽略）
                    rp_perf_add(RP_PERF_GW_MERGED_N, 1);
                    s->gw[k] = s->gw[--s->ngw]; __atomic_sub_fetch(&g_gw_live, 1, __ATOMIC_RELEASE);
                    if (hit == s->ngw) hit = k;   // 被换走的是最后一条、恰好是 hit：它现在在 k
                    k--;                          // 换过来的那条还没看
                }
            }
            if (hit >= 0) {
                struct gpu_write *g = &s->gw[hit];
                g->off = lo; g->len = hi - lo; g->copy_len = copy_hi - lo;
                mirror_touch(&s->m[mi], off, off + len);
                const uint64_t grow = g->len > old_sum ? g->len - old_sum : 0;   // 这一段真正新增的字节；其余是和已登记的重叠（本来要多拷一遍的）
                rp_perf_add(RP_PERF_GW_MERGED_N, 1);
                rp_perf_add(RP_PERF_GW_MERGED_KB, (len > grow ? len - grow : 0) / 1024);
                *pool_key = (uint64_t)(uintptr_t)mem; *merged = 1;
                pthread_mutex_unlock(&g_bmu);
                return id;
            }
        }
        if (s->ngw == s->cgw) { int nc = s->cgw ? s->cgw * 2 : 8; struct gpu_write *n = realloc(s->gw, (size_t)nc * sizeof *n); if (n) { s->gw = n; s->cgw = nc; } }
        if (s->ngw < s->cgw) { int mi = 0; mirror_pick(s, off, len, &mi);   // 和取这段缓冲时（rp_buf_resolve）挑的是同一份
            id = ++next_id; s->gw[s->ngw++] = (struct gpu_write){.off = off, .len = len, .copy_len = copy_len, .id = id, .mi = (uint8_t)mi, .landing = 0, .sub = sub}; *pool_key = (uint64_t)(uintptr_t)mem;
            mirror_touch(&s->m[mi], off, off + len);
            __atomic_add_fetch(&g_gw_live, 1, __ATOMIC_RELEASE); }
    }
    if (!id) g_exact_unknown = 1;
    pthread_mutex_unlock(&g_bmu);
    return id;
}
uint64_t rp_buf_gpu_write_note2(uint64_t addr, uint64_t len, uint64_t *key, int *merged) {
    return rp_buf_gpu_write_note_prefix(addr, len, len, key, merged);
}
uint64_t rp_buf_gpu_write_note(uint64_t addr, uint64_t len, uint64_t *pool_key) { return rp_buf_gpu_write_note2(addr, len, pool_key, NULL); }   // 不合并的老入口（单测与只登记一条的调用方）
int rp_buf_gpu_write_pending(uint64_t addr, uint64_t len) {
    addr = buf_address(addr, len);
    const uint8_t *mem; uint64_t size;
    if (!len || !rp_pool_find(addr, len, &mem, &size)) return -1;
    const uint64_t off = addr - (uint64_t)(uintptr_t)mem, key = (uint64_t)(uintptr_t)mem;
    int hit = 0;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_get(&g_shadow, &key);
    if (s) {
        for (int i = 0; i < s->ngw && !hit; i++) if (s->gw[i].off < off + len && off < s->gw[i].off + s->gw[i].len) hit = 1;
        if (s->landing > s->nlr) hit = 1;   // 正在拷回的段多于记下的区间：说不清，按有算
        for (int i = 0; i < s->nlr && !hit; i++) if (s->lr[i].off < off + len && off < s->lr[i].off + s->lr[i].len) hit = 1;
    }
    pthread_mutex_unlock(&g_bmu);
    return hit;
}
// 在途可以用多个登记的并集盖住一页，撤掉其中任一段就要重新检查。池首尾的半个宿主页也可能在途免同步。
static void land_restale(struct shadow *s, int mi, uint64_t off, uint64_t len) {
    struct mirror *m = &s->m[mi];
    if (!land_noresync_on() || m->path != MP_LOG || !len) return;
    const size_t ps = nvn_dirty_page_size();
    const uint64_t lo = (uintptr_t)(s->mem + off) - s->page0;
    stale_set(m, lo / ps, (lo + len - 1) / ps);
}
// refs 按池和登记号排序。同一池只走一趟账本；已合并撤销的登记留空。
void rp_buf_gpu_write_snapshot(const struct rp_gpu_write_ref *refs, uint32_t count, nvn_gpu_readback_span *spans) {
    for (uint32_t first = 0; first < count;) {
        const uint64_t key = refs[first].pool_key;
        uint32_t end = first + 1;
        while (end < count && refs[end].pool_key == key) end++;
        pthread_mutex_lock(&g_bmu);
        const struct shadow *s = nvn_hm_get(&g_shadow, &key);
        for (int i = 0; s && i < s->ngw; i++) {
            const struct gpu_write *w = &s->gw[i];
            if (w->sub != rp_sub_serial() || w->landing) continue;
            uint32_t lo = first, hi = end;
            while (lo < hi) {
                const uint32_t mid = lo + (hi - lo) / 2;
                if (refs[mid].id < w->id) lo = mid + 1;
                else hi = mid;
            }
            if (lo < end && refs[lo].id == w->id)
                spans[refs[lo].index] = (nvn_gpu_readback_span){s->m[w->mi].buf, w->off, w->copy_len, UINT64_MAX};
        }
        pthread_mutex_unlock(&g_bmu);
        first = end;
    }
}
void rp_buf_gpu_write_land(uint64_t pool_key, uint64_t id, int ok) {
    rp_buf_gpu_write_land_from(pool_key, id, ok, NULL);
}
void rp_buf_gpu_write_land_from(uint64_t pool_key, uint64_t id, int ok, const uint8_t *staged) {   // 线程：[完成回调]
    // 登记保留到锁外拷贝结束：同步和镜像选择都必须避开尚未完整落地的区间。
    // 锁内只标记正在拷，13 MB 的拷贝放到锁外做：这一段原来持锁拷贝，
    // 与提交线程的缓冲解析抢同一把锁，重叠档实测「锁等待」11.1–21.2 毫秒/帧（见坑记录续 25）。
    const uint8_t *src = NULL, *mem = NULL; uint64_t off = 0, len = 0, copy_len = 0; struct shadow *s = NULL; int found = 0, wmi = 0;
    pthread_mutex_lock(&g_bmu);
    s = nvn_hm_get(&g_shadow, &pool_key);
    if (s) for (int i = 0; i < s->ngw; i++) if (s->gw[i].id == id && !s->gw[i].landing) {
        mem = s->mem; off = s->gw[i].off; len = s->gw[i].len; wmi = s->gw[i].mi; copy_len = s->gw[i].copy_len;
        const uint8_t *mirror = ok && !staged ? nvn_gpu_buf_cpu(s->m[wmi].buf) : NULL;
        if (ok) src = staged ? staged : (mirror ? mirror + off : NULL);
        rp_perf_add(RP_PERF_LAND_N, 1); rp_perf_add(RP_PERF_LAND_KB, copy_len / 1024);
        s->gw[i].landing = 1; found = 1;
        s->landing++;
        if (s->nlr < 16) s->lr[s->nlr++] = (struct land_rng){off, len};   // 锁外拷贝期间要避开的区间
        break; }
    pthread_mutex_unlock(&g_bmu);
    if (!found) return;
#ifdef S2A_GPU_LAND_TEST
    extern void rp_test_land_copy(void *, const void *, size_t);
    if (src) rp_test_land_copy((uint8_t *)(uintptr_t)(mem + off), src, (size_t)copy_len);
#else
    if (src) memcpy((uint8_t *)(uintptr_t)(mem + off), src, (size_t)copy_len);   // 游戏内存是真值：GPU 的结果回到它那里
#endif
    pthread_mutex_lock(&g_bmu);
    for (int i = 0; i < s->ngw; i++) if (s->gw[i].id == id) { s->gw[i] = s->gw[--s->ngw]; break; }
    for (int i = s->nlr - 1; i >= 0; i--) if (s->lr[i].off == off && s->lr[i].len == len) { s->lr[i] = s->lr[--s->nlr]; break; }
    // 登记撤掉之后同一段改用当前那份镜像：不受写跟踪的池里，那份镜像此前同步这些页时按登记跳过了这段字节，落地的内容它没有——作废这几页，下次使用重新同步。
    // 写它的那一份镜像里本来就是新的，不动（受跟踪的池里落地的 memcpy 自己会让页变脏）。
    if (ok && s->untracked) pages_invalidate_locked(s, off, len, wmi);
    land_restale(s, wmi, off, len);
    s->landing--;
    __atomic_sub_fetch(&g_gw_live, 1, __ATOMIC_RELEASE);   // 锁内撤登记与计数保持一致
    if (!s->landing) pthread_cond_broadcast(&g_land_cv);   // 池销毁 / 会话收尾可安全解除目标映射
    pthread_mutex_unlock(&g_bmu);
}

// 每次成功解析只写一个桶，不逐页扫描；碰撞合并包络不会漏掉已读范围。
// 只在脏页要同步时扫固定数量的桶；同一提交即使拆段也不能直接覆盖先前绘制会读的镜像。
static void mirror_note_read(struct mirror *m, uint64_t lo, uint64_t hi) {
    uint64_t sub = rp_sub_serial();
    uint64_t h = lo ^ (hi * UINT64_C(0x9e3779b97f4a7c15));
    struct read_range *r = &m->reads[(h ^ (h >> 32)) & (READ_BUCKETS - 1)];
    if (r->sub != sub) { *r = (struct read_range){sub, lo, hi}; return; }
    if (lo < r->lo) r->lo = lo;
    if (hi > r->hi) r->hi = hi;
}
static int mirror_read_hit(struct mirror *m, uint64_t lo, uint64_t hi) {
    uint64_t sub = rp_sub_serial();
    for (int i = 0; i < READ_BUCKETS; i++) {
        const struct read_range *r = &m->reads[i];
        if (r->sub == sub && r->lo <= hi && lo <= r->hi) return 1;
    }
    return 0;
}

static int mirror_sync_class(const struct shadow *s, const struct mirror *m, uint64_t lo, uint64_t hi) {
    const struct mirror_exact *x = m->exact;
    if (g_exact_unknown || !x || x->sub != rp_sub_serial() || x->n > EXACT_RANGES ||
        s->ngw || s->landing || s->untracked || nvn_dirty_tracking_unreliable() ||
        g_exact_mapping != nvn_dirty_mapping_generation() ||
        !g_exact_pool || g_exact_pool != rp_pool_mirror_epoch(s->mem, s->size))
        return RP_PERF_MIRROR_FALLBACK;
    for (unsigned i = 0; i < x->n; i++) {
        const struct byte_range r = x->ranges[i];
        if (r.lo < hi && lo < r.hi) return RP_PERF_MIRROR_OVERLAP;
    }
    return RP_PERF_MIRROR_HOIST;
}

// 锁内版：调用方已持有 g_bmu，mem/size 是调用方查好的池。一笔绘制常要解析 3~4 段（顶点流 + 存储缓冲），
// 逐段各抢一次全局锁时「找池取镜像」在重叠档读到 13.7–16.6 毫秒/帧（5627 次、每次约 2.4 微秒）——
// 大头是这把锁与完成线程（落地 GPU 写）互抢，所以另给批量入口，一笔绘制只抢一次。
// 把第 i..j 页（池内页号，含）这一串从游戏内存同步进镜像：先保护、取代号、再读；记下同步代号与所在提交（两条核对路共用）。
static inline void sync_cpu_run(struct shadow *s, struct mirror *m, const uint8_t *mem, uint64_t size, uint64_t i, uint64_t j) {
        const size_t ps = nvn_dirty_page_size();
        uintptr_t a = s->page0 + (uintptr_t)(i * ps), b = s->page0 + (uintptr_t)((j + 1) * ps);   // 这一串脏页，裁到池的范围
        if (a < (uintptr_t)mem) a = (uintptr_t)mem;
        if (b > (uintptr_t)mem + size) b = (uintptr_t)mem + size;
        nvn_dirty_protect((const void *)a, b - a); uint64_t st = nvn_dirty_now(); if (!st) st = 1;   // 先保护、取代号、再读
        uint64_t last = 0; for (uint64_t k = i; k <= j; k++) if (m->sub[k] > last) last = m->sub[k];
        if (mirror_read_hit(m, i, j)) last = rp_sub_serial();   // 干净读取也属于本次提交，不得在 GPU 读之前直写镜像
        // 本次提交里第一次同步这一串：镜像是 CPU 也能写的共享缓冲，直接写进去，不录成 GPU 上的拷贝（理由与真机读数见坑记录）。
        // 正在拷回的段数多于登记数（表满 16 段没记上）→ 说不清哪段在拷，按原口径整池退回录拷贝
        int landing_hit = s->landing > s->nlr;   // 这一段里有没有正在锁外拷回的区间与 [a,b) 相交：有就只能退回录拷贝
        if (!landing_hit && s->nlr) { const uint64_t ra = a - (uintptr_t)mem, rb = b - (uintptr_t)mem;
            for (int k = 0; k < s->nlr; k++) if (s->lr[k].off < rb && ra < s->lr[k].off + s->lr[k].len) { landing_hit = 1; break; } }
        uint8_t *d = last != rp_sub_serial() && rp_fence_done(m->prev_fence) && !landing_hit ? nvn_gpu_buf_cpu(m->buf) : NULL;
        if (d) { sync_write(NULL, d, s, m, a - (uintptr_t)mem, b - (uintptr_t)mem); last = rp_sub_serial(); rp_perf_add(RP_PERF_BUF_SYNC_DIRECT_KB, (b - a) / 1024); }
        else {
            const uint64_t lo = a - (uintptr_t)mem, hi = b - (uintptr_t)mem;
            // 证明针对实际写入的整组裁剪页，不能只看当前绑定请求的字节。
            const int classify = g_exact_on && last == rp_sub_serial() && rp_pass_is_open();
            int result = classify ? mirror_sync_class(s, m, lo, hi) : RP_PERF_MIRROR_FALLBACK;
            if (result == RP_PERF_MIRROR_HOIST) last = 0;   // 仍走 GPU 传输，只移到前置段
            nvn_gpu_cmd xc = rp_xfer_cmd(&last);
            if (xc) sync_write(xc, NULL, s, m, lo, hi);
            if (classify) {
                if (result == RP_PERF_MIRROR_HOIST && !rp_pass_is_open()) {
                    rp_perf_add(RP_PERF_P_CUT_XFER, 1);   // 前置段分配失败，主段仍然收了通道
                    result = RP_PERF_MIRROR_FALLBACK;
                }
                rp_perf_add(result, 1);
            }
        }
        for (uint64_t k = i; k <= j; k++) { m->synced[k] = st; m->sub[k] = last; }
        g_sync_bytes += b - a; g_sync_runs++; rp_perf_add(RP_PERF_BUF_SYNC_KB, (b - a) / 1024);
}
static inline void sync_run(struct shadow *s, struct mirror *m, const uint8_t *mem, uint64_t size, uint64_t i, uint64_t j) {
    if (!s->ngw || !land_noresync_on()) {
        sync_cpu_run(s, m, mem, size, i, j);
        return;
    }
    const size_t ps = nvn_dirty_page_size();
    const int mi = (int)(m - s->m);
    uint64_t skipped = 0;
    for (uint64_t k = i; k <= j;) {
        uint64_t bytes;
        if (gw_covers_page(s, mi, k, ps, &bytes)) {
            // 没有 CPU 字节要搬，避免布防、取传输段和切通道。撤登记后恢复同步。
            if (m->path == MP_LOG) stale_clear(m, k);
            else m->synced[k] = 0;   // 逐页快路不能把此页当成已同步
            skipped += bytes;
            k++;
            continue;
        }
        const uint64_t first = k++;
        while (k <= j && !gw_covers_page(s, mi, k, ps, &bytes)) k++;
        sync_cpu_run(s, m, mem, size, first, k - 1);
    }
    if (skipped) rp_perf_add(RP_PERF_BUF_SYNC_GW_KB, skipped / 1024);
}
static int resolve_part_locked(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size, struct shadow *s, struct mirror *m, int count_resolve);
// 脏页追加日志这条路的解析（见 log_arm 一节）：不逐页比代号，只灌新事件、在旧页位图里求交。
static int resolve_log(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size, struct shadow *s, struct mirror *m) {
    rp_perf_add(RP_PERF_RES_N, 1);
    struct log_fill f = {s, m, 0};
    if (!nvn_dirty_log_read(s->span, m->log_id, &m->log_cur, log_event, &f)) {
        rp_perf_add(RP_PERF_BUF_LOG_EV, f.n);
        if (!log_rebuild(s, m)) { log_drop(m); return resolve_part_locked(addr, len, mem, size, s, m, 0); }
    } else rp_perf_add(RP_PERF_BUF_LOG_EV, f.n);
    const size_t ps = nvn_dirty_page_size();
    const uint64_t p = ((uintptr_t)addr - s->page0) / ps, q = ((uintptr_t)addr + len - 1 - s->page0) / ps;
    uint64_t words = 0, pages = 0;
    for (uint64_t w = p >> 6; w <= q >> 6;) {   // 每次跳到下一个非零字：汇总字为 0 的整 4096 页一步跨过
        const uint64_t sw = w >> 6, sbits = m->stale_sum[sw] >> (w & 63);
        words++;
        if (!sbits) { w = (sw + 1) << 6; continue; }
        w += (uint64_t)__builtin_ctzll(sbits);
        if (w > q >> 6) break;
        uint64_t bits = m->stale[w];
        if (w == p >> 6) bits &= ~0ull << (p & 63);
        if (w == q >> 6 && (q & 63) != 63) bits &= (1ull << ((q & 63) + 1)) - 1;
        while (bits) {   // 字内连续的旧页成一串同步
            const uint64_t i = (w << 6) + (uint64_t)__builtin_ctzll(bits);
            uint64_t j = i; while (j + 1 <= q && j + 1 < ((w + 1) << 6) && (bits >> ((j + 1) & 63) & 1)) j++;
            sync_run(s, m, mem, size, i, j);
            pages += j - i + 1;
            for (uint64_t k = i; k <= j; k++) {   // 此刻只读的页才清：之后的写必然进日志；被并发解保护挡住的页留着，下次再同步
                bits &= ~(1ull << (k & 63));
                if ((m->stale[w] >> (k & 63) & 1) &&
                    nvn_dirty_page_armed(s->span, (const void *)(s->page0 + (uintptr_t)(k * ps)))) stale_clear(m, k);
            }
        }
        w++;
    }
    rp_perf_add(RP_PERF_BUF_LOG_WORDS, words);
    if (pages) { rp_perf_add(RP_PERF_BUF_SLOW_N, 1); rp_perf_add(RP_PERF_BUF_PAGES_N, pages); } else rp_perf_add(RP_PERF_BUF_FAST_N, 1);
    return 1;
}
static int g_res_tm = -1; static unsigned g_res_tmk;
static uint64_t res_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static int resolve_part_locked(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size,
                               struct shadow *s, struct mirror *m, int count_resolve) {
    if (g_res_tm < 0 || !(++g_res_tmk & 255)) g_res_tm = nvn_plat_flag("nvk_res_tm", 0) != 0;
    const uint64_t q0 = g_res_tm ? res_now() : 0;
    if (count_resolve) rp_perf_add(RP_PERF_RES_N, 1);
    if (!s) s = shadow_get(mem, size);
    if (!s) return 0;
    if (!m) m = mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, NULL);
    size_t ps = nvn_dirty_page_size();
    uint64_t p = ((uintptr_t)addr - s->page0) / ps, q = ((uintptr_t)addr + len - 1 - s->page0) / ps, n = q - p + 1;
    uint64_t now0 = nvn_dirty_now();
    uint64_t map0 = nvn_dirty_mapping_generation();   // 跟踪的区间有增减 / 换底：代际变，快路径作废
    if (m->seen_ok && m->seen_map == map0 && !nvn_dirty_tracking_unreliable()) {
        // 快路径：要用的每一页都在「全局写代号 = 现在这个值」时核对过（核对过 = 当时是干净的或当场同步了），之后没有任何写。
        // 必须按页记：只记「镜像上次核对时的代号」不够——上次核对的是别的页，这几页可能在那之前就脏了（11-11 真机：镜头一动几何全碎）
        int all = 1; for (uint64_t i = p; i <= q; i++) if (!m->synced[i] || m->checked[i] != now0) { all = 0; break; }
        if (all) { rp_perf_add(RP_PERF_BUF_FAST_N, 1); if (q0) rp_perf_add(RP_PERF_RES_FIND_NS, res_now() - q0); return 1; }
    }
    uint64_t stack[256], *gens = n <= 256 ? stack : malloc((size_t)n * sizeof *gens);
    long got = gens ? nvn_dirty_page_gens(nvn_dirty_span((const void *)(uintptr_t)addr, (size_t)len), (const void *)(uintptr_t)addr, (size_t)len, gens, (size_t)n) : -1;
    int tracked = got == (long)n;   // 没被写跟踪的池：见 page_fresh
    if (tracked) { rp_perf_add(RP_PERF_BUF_SLOW_N, 1); rp_perf_add(RP_PERF_BUF_PAGES_N, n); } else rp_perf_add(RP_PERF_BUF_NOSPAN_N, 1);
    const uint64_t sub0 = rp_sub_serial(); uint64_t synced_pages = 0;
    for (uint64_t i = p; i <= q;) {
        if (page_fresh(s, m, i, tracked, gens, p, sub0)) { i++; continue; }
        uint64_t j = i; while (j + 1 <= q && !page_fresh(s, m, j + 1, tracked, gens, p, sub0)) j++;
        sync_run(s, m, mem, size, i, j);
        synced_pages += j - i + 1;
        i = j + 1;
    }
    if (!tracked && s->untracked && !synced_pages) rp_perf_add(RP_PERF_BUF_NOSPAN_FRESH_N, 1);   // 不受写跟踪、这一段本次提交里都同步过：免传
    if (gens != stack) free(gens);
    m->seen_ok = (uint8_t)tracked; m->seen_map = map0;
    for (uint64_t i = p; i <= q; i++) m->checked[i] = now0;   // now0 取在读各页代号之前：这之后才写的页，代号已经不等于 now0
    if (q0) rp_perf_add(RP_PERF_RES_LA_NS, res_now() - q0);
    return 1;
}
static int resolve_inner(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size, struct shadow *s, struct mirror *m) {
    if (m->path == MP_LOG) return resolve_log(addr, len, mem, size, s, m);
    return resolve_part_locked(addr, len, mem, size, s, m, 1);   // 日志不可用（不受写跟踪 / 重叠池 / 跟踪不可靠 / 开关关）
}
static int resolve_locked(uint64_t addr, uint64_t len, const uint8_t *mem, uint64_t size) {
    struct shadow *s = shadow_get(mem, size);
    if (!s) { g_exact_unknown = 1; return 0; }
    // 整段先选一次镜像：未落地的 GPU 写可能只覆盖其中一个摘要块。
    struct mirror *m = mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, NULL);
    // 虚拟地址上的 CPU 写不经过物理地址的页保护。提交内首次访问这份镜像时
    // 将对应物理页置旧，按实际绑定范围按需同步，其余页沿用物理池的脏页日志。
    if (alias_count && m->alias_sub != rp_sub_serial()) {
        for (size_t i = 0; i < alias_count; i++) {
            const struct buf_alias *a = &aliases[i];
            if (a->pool == (uintptr_t)mem) alias_stale(s, m, a->phys, a->hi - a->lo);
        }
        m->alias_sub = rp_sub_serial();
    }
    int ok = resolve_inner(addr, len, mem, size, s, m);
    if (ok) {
        const size_t ps = nvn_dirty_page_size();
        uint64_t lo = ((uintptr_t)addr - s->page0) / ps, hi = ((uintptr_t)addr + len - 1 - s->page0) / ps;
        mirror_note_read(m, lo, hi);
        // 按整个绑定范围记读，着色器实际用了其中哪些字节不用猜。
        const uint64_t off = addr - (uintptr_t)mem;
        mirror_touch(m, off, off + len);
    } else g_exact_unknown = 1;
    return ok;
}
// 宿主用例的替身钩子（弱符号，生产里没有实现）：返回非 0 时解析一律失败，
// 用来构造「准备阶段给不出镜像」这条只能靠故障注入覆盖的路径。
int rp_test_buf_resolve_block(void) __attribute__((weak));

int rp_buf_resolve(nvn_gpu_cmd c, uint64_t addr, uint64_t len, nvn_gpu_buf *buf, uint64_t *off) {
    if (rp_test_buf_resolve_block && rp_test_buf_resolve_block()) { mirror_unknown_address(); return 0; }
    addr = buf_address(addr, len);
    (void)c;
    const uint8_t *mem; uint64_t size;
    if (!len || !rp_pool_find(addr, len, &mem, &size)) {
        if (len) mirror_unknown_address();
        nvn_unimpl("buf", "address_outside_pools", (const void *)(uintptr_t)addr);
        return 0;
    }
    pthread_mutex_lock(&g_bmu);
    int r = resolve_locked(addr, len, mem, size);
    if (r) {
        struct shadow *s = shadow_get(mem, size);
        if (s) {
            struct mirror *m = mirror_pick(s, addr - (uint64_t)(uintptr_t)mem, len, NULL);
            if (buf) *buf = m->buf;
            if (off) *off = addr - (uint64_t)(uintptr_t)mem;
        } else r = 0;
    }
    pthread_mutex_unlock(&g_bmu);
    return r;
}
// 批量：一次抢锁解析多段（顶点流 / 存储缓冲），成功写 1、失败写 0。
static void resolve_multi_chunk(int n, const uint64_t *guest, const uint64_t *len, nvn_gpu_buf *buf, uint64_t *off, unsigned char *ok) {
    uint64_t addr[16];
    for (int i = 0; i < n; i++) addr[i] = buf_address(guest[i], len[i]);
    const uint8_t *mem[16]; uint64_t size[16];
    for (int i = 0; i < n; i++) { mem[i] = NULL; size[i] = 0; ok[i] = 0; if (len[i]) rp_pool_find(addr[i], len[i], &mem[i], &size[i]); }
    const uint64_t w0 = (g_res_tm && n) ? res_now() : 0;
    pthread_mutex_lock(&g_bmu);
    if (w0) rp_perf_add(RP_PERF_RES_LW_NS, res_now() - w0);
    for (int i = 0; i < n; i++) {
        if (!mem[i]) { if (len[i]) g_exact_unknown = 1; continue; }
        struct shadow *s = shadow_get(mem[i], size[i]);
        if (!s) { g_exact_unknown = 1; continue; }
        if (!resolve_locked(addr[i], len[i], mem[i], size[i])) continue;
        struct mirror *m = mirror_pick(s, addr[i] - (uint64_t)(uintptr_t)mem[i], len[i], NULL);
        buf[i] = m->buf; off[i] = addr[i] - (uint64_t)(uintptr_t)mem[i]; ok[i] = 1;
    }
    pthread_mutex_unlock(&g_bmu);
}
int rp_buf_resolve_multi(int n, const uint64_t *addr, const uint64_t *len, nvn_gpu_buf *buf, uint64_t *off, unsigned char *ok) {
    // Storage bindings can exceed the 16 vertex streams. Resolve every item;
    // the fixed scratch arrays bound each lock acquisition, not the request.
    while (n > 0) {
        int count = n < 16 ? n : 16;
        resolve_multi_chunk(count, addr, len, buf, off, ok);
        n -= count; addr += count; len += count; buf += count; off += count; ok += count;
    }
    return 1;
}

void rp_buf_pool_gone(const void *mem) {
    uint64_t k = (uint64_t)(uintptr_t)mem;
    char said[192]; said[0] = 0;
    pthread_mutex_lock(&g_bmu);
    size_t kept = 0;
    for (size_t i = 0; i < alias_count; i++)
        if (aliases[i].pool != k) aliases[kept++] = aliases[i];
    alias_count = kept;
    __atomic_store_n(&aliases_live, kept != 0, __ATOMIC_RELEASE);
    struct shadow *s = nvn_hm_del(&g_shadow, &k);
#ifdef S2A_GPU_LAND_TEST
    extern void rp_test_pool_gone(void);
    rp_test_pool_gone();
#endif
    if (s) {
        while (s->landing) pthread_cond_wait(&g_land_cv, &g_bmu);   // 等待不持锁，允许落地完成；返回后调用方才可解除映射
        // 等待之后再判窗口：等的时候窗口可能已经收了。镜像缓冲是后端自己的存储（不是游戏内存的别名），晚放不碍调用方解除映射
        const int defer = g_hold && !pthread_equal(g_hold_tid, pthread_self());
        const int nb = (s->m[0].buf != NULL) + (s->m[1].buf != NULL);
        shadow_free(s, defer);
        if (defer && nb) {   // 只记前几次和 2 的幂次的累计数：真机上这条每次都意味着一次本来会撞契约违规的销毁
            const uint64_t n = (g_limbo_total += (uint64_t)nb);
            if (n <= 8 || !(n & (n - 1))) snprintf(said, sizeof said, "池销毁撞上回放窗口：池 %p 的 %d 份镜像缓冲延到提交收尾再放（累计 %llu 份）", mem, nb, (unsigned long long)n);
        }
    }
    pthread_mutex_unlock(&g_bmu);
    if (said[0]) nvn_plat_log(NVN_PLAT_LOG_INFO, "buf", said);
}
void rp_buf_hold_begin(void) {
    const int exact = rp_pass_is_open && rp_pool_mirror_epoch && nvn_plat_flag("nvk_mirror_disjoint", 0) != 0;
    pthread_mutex_lock(&g_bmu);
    g_hold = 1; g_hold_tid = pthread_self();
    g_exact_on = exact; g_exact_unknown = 0;
    if (exact) {
        g_exact_pool = rp_pool_mirror_epoch(NULL, 0);
        g_exact_mapping = nvn_dirty_mapping_generation();
    }
    pthread_mutex_unlock(&g_bmu);
}
void rp_buf_hold_end(void) {
    nvn_gpu_buf *take = NULL; int n = 0;
    pthread_mutex_lock(&g_bmu);
    g_hold = 0; g_exact_on = 0;
    if (g_nlimbo) { take = g_limbo; n = g_nlimbo; g_limbo = NULL; g_nlimbo = g_climbo = 0; }
    pthread_mutex_unlock(&g_bmu);
    for (int i = 0; i < n; i++) nvn_gpu_buf_release(take[i], NULL, NULL);   // 后端按引用计数退役：已被命令登记的缓冲留到段完成
    free(take);
}
nvn_gpu_buf rp_buf_for_null_storage(void) {
    pthread_mutex_lock(&g_bmu);
    if (!g_null_storage) {
        // 对齐 Ryujinx DescriptorSetUpdater：不支持 nullDescriptor 时，未使用的缓冲绑定用 64 KiB 物理对象。
        // 只初始化一次；着色器写入后不承诺内容仍为零，也不把写入落到任何 guest 地址。
        void *zero = calloc(1, 0x10000);
        nvn_gpu_err err = {0};
        if (!zero || !nvn_gpu_ok(nvn_gpu_buf_create(0x10000, NVN_GPU_MEM_SHARED, zero, &g_null_storage, &err))) {
            g_null_storage = NULL;
            if (nvn_unimpl("gpu", "null_storage_create", NULL) == 1 && err.why[0]) nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", err.why);
        }
        free(zero);
    }
    nvn_gpu_buf result = g_null_storage;
    pthread_mutex_unlock(&g_bmu); return result;
}

void rp_buf_reset(void) {
    pthread_mutex_lock(&g_bmu);
    // 收尾也不能释放正在被锁外拷贝引用的镜像；等待期间表可能变化，醒来重新扫描。
    for (;;) {
        int landing = 0;
        for (uint32_t i = 0; i < g_shadow.cap; i++) { struct shadow *s = g_shadow.vals[i]; if (s && s->landing) { landing = 1; break; } }
        if (!landing) break;
        pthread_cond_wait(&g_land_cv, &g_bmu);
    }
    for (uint32_t i = 0; i < g_shadow.cap; i++) { struct shadow *s = g_shadow.vals[i]; if (s) shadow_free(s, 0); }
    for (int i = 0; i < g_nlimbo; i++) nvn_gpu_buf_release(g_limbo[i], NULL, NULL);   // 收尾：没配上 end 的窗口里延后的缓冲也放掉
    free(g_limbo); g_limbo = NULL; g_nlimbo = g_climbo = 0; g_hold = 0; g_exact_on = 0;
    if (g_null_storage) nvn_gpu_buf_release(g_null_storage, NULL, NULL);
    g_null_storage = NULL;
    free(aliases); aliases = NULL; alias_count = alias_cap = 0;
    __atomic_store_n(&aliases_live, 0, __ATOMIC_RELEASE);
    free(g_shadow.keys); free(g_shadow.vals); nvn_hm_init(&g_shadow, 8); g_mirror_cur = 0;
    g_land_noresync = -1;
    pthread_mutex_unlock(&g_bmu);
}
void rp_buf_stats(uint64_t *bytes, uint64_t *runs) { pthread_mutex_lock(&g_bmu); if (bytes) *bytes = g_sync_bytes; if (runs) *runs = g_sync_runs; pthread_mutex_unlock(&g_bmu); }
int rp_buf_mirrors(void) { pthread_mutex_lock(&g_bmu); int n = (int)g_shadow.n; pthread_mutex_unlock(&g_bmu); return n; }

// ---------- 缓冲类命令：只改游戏内存（见文件头）----------
static int in_pool(uint64_t a, uint64_t n) { return n && rp_pool_find(a, n, NULL, NULL); }
// 回放时在 CPU 上改游戏缓冲内容的三个命令：改一次代号加一（rp_draw 的段内常量缓冲切片按它失效）
uint64_t rp_cpu_write_gen;
// 写入区间日志（开关 nvk_slc_gen 用）：只记这三个命令改写过的 [a, b)，给「暂存片还能不能用」逐区间判定。
// 用全局写代号判会让所有区间一起作废；按区间判的话，每帧固定的那几块常量缓冲（内容没变）就能一直用同一片暂存。
enum { WL_N = 128 };
static struct wl_ent { uint64_t seq, a, b; } g_wl[WL_N];
static uint64_t g_wl_seq, g_wl_put;
uint64_t rp_write_mark(void) { return g_wl_seq; }
void rp_write_note(uint64_t a, uint64_t n) {
    a = buf_address(a, n);
    g_wl[g_wl_put].seq = ++g_wl_seq; g_wl[g_wl_put].a = a; g_wl[g_wl_put].b = a + n; g_wl_put = (g_wl_put + 1) % WL_N;
    buf_invalidate_written(a, n);   // 不受写跟踪的池靠这条知道「回放自己改了哪几页」（受跟踪的池由写跟踪看到）
}
// mark 之后有没有人写过和 [a, a+n) 重叠的区间。只顺着记号扫「mark 之后的那几条」——
// 一帧要问 8000 次左右，扫满 128 条就是一百万次比较（实测这样做的缓存反而更慢）。
// 日志绕一圈（mark 太旧、记录已被覆盖）时保守当作写过。
int rp_write_touched(uint64_t mark, uint64_t a, uint64_t n) {
    const uint64_t seq = __atomic_load_n(&g_wl_seq, __ATOMIC_RELAXED);
    if (mark == seq) return 0;
    if (seq - mark > WL_N) return 1;
    a = buf_address(a, n);
    const uint64_t b = a + n;
    for (uint64_t s = mark + 1; s <= seq; s++) {   // 第 s 次写入放在下标 (s-1)%WL_N（rp_write_note 的写法）
        const struct wl_ent *w = &g_wl[(s - 1) % WL_N];
        if (w->seq == s && w->a < b && a < w->b) return 1;
    }
    return 0;
}
// [addr, addr+len) 与还没落地的 GPU 写相交吗（登记着的、正在锁外拷回的；表满说不清的按相交算）。线程：[提交]
int rp_buf_gpu_pending(uint64_t addr, uint64_t len) {
    if (!len || !__atomic_load_n(&g_gw_live, __ATOMIC_ACQUIRE)) return 0;
    addr = buf_address(addr, len);
    const uint8_t *mem; uint64_t size;
    if (!rp_pool_find(addr, len, &mem, &size)) return 0;
    uint64_t k = (uint64_t)(uintptr_t)mem, a = addr - k, b = a + len; int hit = 0;
    pthread_mutex_lock(&g_bmu);
    struct shadow *s = nvn_hm_get(&g_shadow, &k);
    if (s) {
        for (int i = 0; i < s->ngw && !hit; i++) hit = s->gw[i].off < b && a < s->gw[i].off + s->gw[i].len;
        if (!hit) hit = s->landing > s->nlr;
        for (int i = 0; i < s->nlr && !hit; i++) hit = s->lr[i].off < b && a < s->lr[i].off + s->lr[i].len;
    }
    pthread_mutex_unlock(&g_bmu);
    return hit;
}

// 三条 CPU 改游戏内存的命令遇到还没落地的 GPU 写（#1022、#697）：CPU 现在读写游戏内存，读到的是 GPU 写之前的旧内容；写进去的又会被稍后的落地盖掉。
// 这时改在 GPU 上按提交顺序做（照 iOS mtl_sub_copy / mtl_sub_fill）：在镜像里拷 / 填 / 写，目标登记成本次提交的 GPU 写——镜像同步跳过它、段完成时落回游戏内存；
// 常量缓冲绑到这段时直接绑镜像（rp_draw.c）。不相交就返回 0，调用方照旧在 CPU 上做（绝大多数情况，零额外开销）。
static int gpu_dst_done(uint64_t d, uint64_t n, const char *what, const void *rec) {
    if (!rp_post_gpu_write(d, n)) nvn_unimpl("buf", what, rec);   // 登记不上：结果留在镜像里、落不回游戏内存（记账，不静默）
    rp_cpu_write_gen++; rp_write_note(d, n); rp_gpu_wr_gen++;   // 目标内容变了（游戏内存要到落地才变）：段内切片表、整组沿用、base instance 批量表按它作废
    rp_perf_add(RP_PERF_BUF_GPU_ORDER_N, 1);
    return 1;
}
int rp_op_update_ub_gpu(nvn_gpu_cmd c, const void *a, const void *rec) {
    uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
    if (!c || !n || !in_pool(d, n) || !rp_buf_gpu_pending(d, n)) return 0;
    rp_pass_flush(c);
    nvn_gpu_buf db; uint64_t off;
    if (!rp_buf_resolve(c, d, n, &db, &off)) return 0;
    nvn_gpu_buf_write(c, db, off, (const uint8_t *)a + 16, n);
    return gpu_dst_done(d, n, "update_gpu_write_note_failed", rec);
}
int rp_op_copy_b2b_gpu(nvn_gpu_cmd c, const void *a, const void *rec) {
    uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
    if (!c || !n || !in_pool(s, n) || !in_pool(d, n) || (!rp_buf_gpu_pending(s, n) && !rp_buf_gpu_pending(d, n))) return 0;
    s = buf_address(s, n); d = buf_address(d, n);
    if (s < d + n && d < s + n) { nvn_unimpl("buf", "copy_overlap_gpu_pending", rec); return 0; }   // 源目标重叠（GPU 拷贝不许）：照旧 CPU 做，记账
    rp_pass_flush(c);
    nvn_gpu_buf sb, db; uint64_t so, doff;
    if (!rp_buf_resolve(c, s, n, &sb, &so) || !rp_buf_resolve(c, d, n, &db, &doff)) return 0;
    nvn_gpu_copy_buf(c, sb, so, db, doff, n);
    return gpu_dst_done(d, n, "copy_gpu_write_note_failed", rec);
}
int rp_op_clear_buf_gpu(nvn_gpu_cmd c, const p_clear_buf *cb, const void *rec) {
    if (!c || !cb->size || !in_pool(cb->addr, cb->size) || !rp_buf_gpu_pending(cb->addr, cb->size)) return 0;
    if ((cb->addr | cb->size) & 3) { nvn_unimpl("buf", "clear_unaligned_gpu_pending", rec); return 0; }   // GPU 填充要 4 字节对齐：照旧 CPU 做，记账
    rp_pass_flush(c);
    nvn_gpu_buf db; uint64_t off;
    if (!rp_buf_resolve(c, cb->addr, cb->size, &db, &off)) return 0;
    nvn_gpu_fill_buf(c, db, off, cb->size, cb->value);
    return gpu_dst_done(cb->addr, cb->size, "clear_gpu_write_note_failed", rec);
}
void rp_op_update_ub(const void *a, const void *rec) {
    uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
    if (!n) return;
    if (!in_pool(d, n)) { nvn_unimpl("buf", "update_outside_pools", rec); return; }
    rp_ub_fill_fence(d, n); memcpy((void *)(uintptr_t)d, (const uint8_t *)a + 16, (size_t)n); rp_cpu_write_gen++; rp_write_note(d, n);
}
void rp_op_copy_b2b(const void *a, const void *rec) {
    uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
    if (!n) return;
    if (!in_pool(s, n) || !in_pool(d, n)) { nvn_unimpl("buf", "copy_outside_pools", rec); return; }
    rp_ub_fill_fence(d, n); memmove((void *)(uintptr_t)d, (const void *)(uintptr_t)s, (size_t)n); rp_cpu_write_gen++; rp_write_note(d, n);
}
void rp_op_clear_buf(const p_clear_buf *cb, const void *rec) {   // 32 位样式填充（不是按字节）
    if (!cb->size) return;
    if (!in_pool(cb->addr, cb->size)) { nvn_unimpl("buf", "clear_outside_pools", rec); return; }
    rp_ub_fill_fence(cb->addr, cb->size); uint8_t *p = (uint8_t *)(uintptr_t)cb->addr; uint64_t n = cb->size, i = 0; rp_cpu_write_gen++; rp_write_note(cb->addr, cb->size);
    for (; i + 4 <= n; i += 4) memcpy(p + i, &cb->value, 4);
    if (i < n) memcpy(p + i, &cb->value, (size_t)(n - i));
}

// 缓冲到贴图：未落地的 GPU 写在池镜像中，纯 CPU 源仍走上传入口（含格式转换）。
void rp_op_copy_b2t(nvn_gpu_cmd c, const p_copy_b2t *o, const void *rec) {
    const tex_t *t = o->tex;
    if (!t || !c) return;
    nvn_fmt_info fi;
    if (!nvn_fmt_info_of(t->d.format, &fi) || o->r.width <= 0 || o->r.height <= 0 || o->r.depth < 0) {
        nvn_unimpl("tex", "b2t_bad_format_or_region", rec); return;
    }
    uint32_t level = o->has_view && o->view.set_levels ? o->view.base_level : 0;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t layer0 = !is3d && o->has_view && o->view.set_layers ? o->view.min_layer : 0;
    uint32_t nz = o->r.depth ? (uint32_t)o->r.depth : 1;
    if (level >= (t->d.levels ? t->d.levels : 1u) || level >= 32) {
        nvn_unimpl("tex", "b2t_bounds", rec); return;
    }
    uint32_t w = t->d.width >> level, h = t->d.height >> level;
    if (!w) w = 1;
    if (!h || rp_fold_1d_target(t->d.target) != t->d.target) h = 1;
    uint32_t layers = is3d ? t->d.depth >> level : rp_tex_layer_count(t);
    if (is3d && !layers) layers = 1;
    uint64_t end_x = (uint64_t)(uint32_t)o->r.xoffset + (uint32_t)o->r.width;
    uint64_t end_y = (uint64_t)(uint32_t)o->r.yoffset + (uint32_t)o->r.height;
    if (o->r.xoffset < 0 || o->r.yoffset < 0 || o->r.zoffset < 0 ||
        end_x > w || end_y > h || (uint64_t)layer0 + (uint32_t)o->r.zoffset + nz > layers ||
        o->r.xoffset % fi.bw || o->r.yoffset % fi.bh ||
        (o->r.width % fi.bw && end_x != w) || (o->r.height % fi.bh && end_y != h)) {
        nvn_unimpl("tex", "b2t_bounds", rec); return;
    }
    uint64_t bytes = (((uint64_t)o->r.width + fi.bw - 1) / fi.bw) * fi.bytes;
    uint64_t rows = ((uint64_t)o->r.height + fi.bh - 1) / fi.bh;
    uint64_t row = o->row_stride ? o->row_stride : bytes;
    if (row < bytes || (!fi.expand && row % fi.bytes) ||
        row / fi.bytes > UINT32_MAX / fi.bw || row > UINT64_MAX / rows) {
        nvn_unimpl("tex", "b2t_bad_stride", rec); return;
    }
    uint64_t img = o->image_stride ? o->image_stride : row * rows;
    if (img < row * rows || img / row > UINT32_MAX / fi.bh || img > (UINT64_MAX - o->src) / nz) {
        nvn_unimpl("tex", "b2t_bad_stride", rec); return;
    }
    uint64_t total = img * nz;
    if (!in_pool(o->src, total)) { nvn_unimpl("buf", "b2t_source_outside_pools", rec); return; }
    nvn_gpu_buf source = NULL;
    uint64_t off = 0;
    if (rp_buf_gpu_pending(o->src, total)) {
        // 上传入口要在 CPU 上展开 / 转深度或拆深度模板面，不能拿旧客体字节做转换。
        if (fi.expand || (fi.depth && fi.stencil) || (rp_fmt_caps(t->d.format) & NVN_GPU_FMT_SUBST)) {
            nvn_unimpl("tex", "b2t_gpu_source_requires_conversion", rec); return;
        }
        if (!rp_buf_resolve(c, o->src, total, &source, &off)) {
            nvn_unimpl("buf", "b2t_pending_source_unavailable", rec); return;
        }
        if ((off & 3) || off % fi.bytes || (nz > 1 && ((img & 3) || img % fi.bytes))) {
            nvn_unimpl("tex", "b2t_gpu_source_alignment", rec); return;
        }
    }
    nvn_gpu_tex g = rp_tex_resolve_plain(t, c);
    if (!g) return;
    if (t->d.target == NVN_TEXTURE_TARGET_3D && nvn_plat_flag("nvk_drawlog", 0)) {   // 调试开关下：三维贴图的每次上传打一行
        char b[200]; snprintf(b, sizeof b, "三维贴图上传 #%llu 格式 %#x 尺寸 %dx%dx%d 级 %u：区域 %d,%d,%d %dx%dx%d 行距 %llu 片距 %llu（记录里 行 %llu 片 %llu）块 %dx%d 每块 %d 字节",
                             (unsigned long long)t->serial, (unsigned)t->d.format, t->d.width, t->d.height, t->d.depth, level, o->r.xoffset, o->r.yoffset, o->r.zoffset, o->r.width, o->r.height, o->r.depth,
                             (unsigned long long)row, (unsigned long long)img, (unsigned long long)o->row_stride, (unsigned long long)o->image_stride, fi.bw, fi.bh, fi.bytes);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", b);
    }
    for (uint32_t z = 0; z < nz; z++) {
        NVNcopyRegion r = o->r; r.depth = 1;
        // 第几层 / 第几片都经 layer 交给后端（三维贴图的 layer 就是深度方向的片号；视图的起始层只对分层贴图有意义）
        uint32_t layer = (uint32_t)o->r.zoffset + z + (t->d.target == NVN_TEXTURE_TARGET_3D ? 0 : layer0);
        r.zoffset = 0;
        if (source) nvn_gpu_copy_buf_tex(c, source, off + z * img, row, img, g, level, layer, &r);
        else nvn_gpu_tex_upload(c, g, level, layer, &r, (const void *)(uintptr_t)(o->src + z * img), row, img);
    }
    rp_tex_mark_gpu_written(t);   // 这张贴图的内容从此不再从它自己的存储重传
}

// 贴图 → 贴图的拷贝（nvn_mtl.m copy_t2t_ @14049）。源 / 目标各按自己视图的级与起始层；同尺寸、同格式、不镜像的逐层直接拷（三维的一次拷若干片）；
// 降采样记录（DM_COPY_RESOLVE）在两边采样数、格式、尺寸都相同时就是整张拷贝。缩放、换格式、镜像、多重采样解析：iOS 是画出来 / 解析出来的，这里还没接，分开记账。
void rp_op_copy_t2t(nvn_gpu_cmd c, const p_copy_t2t *o, const void *rec) {
    const tex_t *s = o->src, *d = o->dst; if (!s || !d || !c) return;
    uint32_t sl = o->has_sv && o->sv.set_levels ? o->sv.base_level : 0, dl = o->has_dv && o->dv.set_levels ? o->dv.base_level : 0;
    uint32_t sz0 = o->has_sv && o->sv.set_layers ? o->sv.min_layer : 0, dz0 = o->has_dv && o->dv.set_layers ? o->dv.min_layer : 0;
    int sfmt = o->has_sv && o->sv.set_format ? o->sv.format : s->d.format, dfmt = o->has_dv && o->dv.set_format ? o->dv.format : d->d.format;
    int s3 = s->d.target == NVN_TEXTURE_TARGET_3D, d3 = d->d.target == NVN_TEXTURE_TARGET_3D;
    NVNcopyRegion sr = o->sr, dr = o->dr;
    if (nvn_plat_flag("nvk_drawlog", 0)) { static int said; if (said < 8) { said++; char b[260];
        snprintf(b, sizeof b, "贴图拷贝 #%llu(%#x %dx%d 采样 %d 级 %u 层 %u) %d,%d %dx%dx%d → #%llu(%#x %dx%d 采样 %d 级 %u 层 %u) %d,%d %dx%dx%d 标志 %#x", (unsigned long long)s->serial, (unsigned)sfmt, s->d.width, s->d.height, s->d.samples, sl, sz0,
                 sr.xoffset, sr.yoffset, sr.width, sr.height, sr.depth, (unsigned long long)d->serial, (unsigned)dfmt, d->d.width, d->d.height, d->d.samples, dl, dz0, dr.xoffset, dr.yoffset, dr.width, dr.height, dr.depth, (unsigned)o->flags);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "copy", b); } }
    if (o->flags & DM_COPY_RESOLVE) {
        int ss = s->d.samples > 1 ? s->d.samples : 1, ds = d->d.samples > 1 ? d->d.samples : 1;
        if (ss > 1 && ds == 1 && sfmt == dfmt && sfmt == s->d.format && dfmt == d->d.format &&
            s->d.width == d->d.width && s->d.height == d->d.height && !s3 && !d3 && sl == 0 && dl == 0) {
            nvn_fmt_info f;
            if (!nvn_fmt_info_of((uint32_t)sfmt, &f) || f.kind == NVN_FMT_UINT || f.kind == NVN_FMT_SINT || f.depth || f.stencil) {
                nvn_unimpl("tex", "t2t_multisample_resolve", rec); return;
            }
            view_t vs = {0}, vd = {0};
            vs.set_levels = vd.set_levels = 1; vs.nlevels = vd.nlevels = 1;
            vs.set_layers = vd.set_layers = 1; vs.min_layer = (int)sz0; vd.min_layer = (int)dz0; vs.nlayers = vd.nlayers = 1;
            rp_view_pin_begin();   // 连取两个视图：取第二个时不能把第一个轮转放掉
            nvn_gpu_view gv = rp_view_for_plain(s, &vs, c), hv = rp_view_for_plain(d, &vd, c);
            if (!gv || !hv) { rp_view_pin_end(); return; }
            NVNcopyRegion r = {0, 0, 0, s->d.width, s->d.height, 1};
            nvn_gpu_blit(c, gv, &r, hv, &r, 0);
            rp_view_pin_end();
            rp_tex_mark_gpu_written(d);
            return;
        }
        if (ss != ds || sfmt != dfmt || s->d.width != d->d.width || s->d.height != d->d.height) { nvn_unimpl("tex", ss > 1 && ds == 1 ? "t2t_multisample_resolve" : "t2t_downsample_case", rec); return; }
        uint32_t w = (uint32_t)s->d.width >> sl, h = (uint32_t)s->d.height >> sl; if (!w) w = 1; if (!h) h = 1;
        sr = (NVNcopyRegion){0, 0, 0, (int)w, (int)h, 1}; dr = sr;
    } else {
        int nz = sr.depth > 0 ? sr.depth : 1, dnz = dr.depth > 0 ? dr.depth : 1;
        int mirror = o->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT);
        if ((o->flags & NVN_COPY_FLAGS_MIRROR_Z_BIT) || (mirror && s->serial == d->serial)) { nvn_unimpl("tex", "t2t_mirror", rec); return; }
        if (mirror || sr.width != dr.width || sr.height != dr.height || sfmt != dfmt) {
            // 缩放 / 换格式：二维、单层、单采样。颜色走原生 blit；同格式 D24S8/D32FS8 最近点缩放由后端能力位声明。
            nvn_fmt_info fa, fb; int ia = !nvn_fmt_info_of((uint32_t)sfmt, &fa) || fa.kind == NVN_FMT_UINT || fa.kind == NVN_FMT_SINT || fa.depth || fa.stencil;
            int ib = !nvn_fmt_info_of((uint32_t)dfmt, &fb) || fb.kind == NVN_FMT_UINT || fb.kind == NVN_FMT_SINT || fb.depth || fb.stencil;
            int depth_pair = sfmt == dfmt && sfmt == s->d.format && dfmt == d->d.format &&
                (sfmt == NVN_FORMAT_DEPTH24_STENCIL8 || sfmt == NVN_FORMAT_DEPTH32F_STENCIL8) && !mirror && !(o->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT);
            if (s3 || d3 || nz != 1 || dnz != 1 || s->d.samples > 1 || d->d.samples > 1 || ((ia || ib) && !depth_pair)) { nvn_unimpl("tex", mirror ? "t2t_mirror" : sfmt != dfmt ? "t2t_format_change" : "t2t_scaled", rec); return; }
            nvn_gpu_caps caps; nvn_gpu_caps_get(&caps);
            if (depth_pair ? !(caps.caps & NVN_GPU_CAP_BLIT_DEPTH_STENCIL) : !(caps.caps & NVN_GPU_CAP_BLIT_SCALED)) {
                nvn_unimpl("tex", mirror ? "t2t_mirror" : depth_pair ? "t2t_scaled" : "t2t_scaled_no_native_blit", rec); return;
            }
            // NVN 区域仍为正尺寸；先防止反向端点的有符号加法溢出。
            if (mirror && (sr.width <= 0 || sr.height <= 0 || dr.width <= 0 || dr.height <= 0 ||
                sr.xoffset < 0 || sr.yoffset < 0 || (int64_t)sr.xoffset + sr.width > INT32_MAX ||
                (int64_t)sr.yoffset + sr.height > INT32_MAX)) { nvn_unimpl("tex", "t2t_mirror", rec); return; }
            // 视图只取级与层：拷贝按存储的格式做
            view_t vs, vd; memset(&vs, 0, sizeof vs); memset(&vd, 0, sizeof vd);
            vs.set_levels = 1; vs.base_level = (int)sl; vs.nlevels = 1; vs.set_layers = 1; vs.min_layer = (int)(sz0 + (uint32_t)sr.zoffset); vs.nlayers = 1;
            vd.set_levels = 1; vd.base_level = (int)dl; vd.nlevels = 1; vd.set_layers = 1; vd.min_layer = (int)(dz0 + (uint32_t)dr.zoffset); vd.nlayers = 1;
            // 一次取源 / 目标两个视图再 blit：同一段取用窗口，第二个不许把第一个轮转淘汰（#699）。
            // 两笔连续取用本身最多推进一次轮转，但要和 pass 内多附件守同一条不变量。
            rp_view_pin_begin();
            nvn_gpu_view gv = rp_view_for_plain(s, &vs, c), hv = rp_view_for_plain(d, &vd, c);
            if (!gv || !hv) { rp_view_pin_end(); return; }
            NVNcopyRegion a = sr, b = dr; a.zoffset = b.zoffset = 0; a.depth = b.depth = 1;
            // Ryujinx ExtentsToOffset3D 同样保留两端次序：镜像沿源矩形反向行走。
            if (mirror & NVN_COPY_FLAGS_MIRROR_X_BIT) { a.xoffset += a.width; a.width = -a.width; }
            if (mirror & NVN_COPY_FLAGS_MIRROR_Y_BIT) { a.yoffset += a.height; a.height = -a.height; }
            nvn_gpu_blit(c, gv, &a, hv, &b, (o->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) != 0);
            rp_view_pin_end();
            rp_tex_mark_gpu_written(d);
            return;
        }
        if (nz != dnz) { nvn_unimpl("tex", "t2t_scaled", rec); return; }
        if (s3 != d3) { nvn_unimpl("tex", "t2t_3d_mismatch", rec); return; }
    }
    if (sr.width <= 0 || sr.height <= 0) return;
    nvn_gpu_tex gs = rp_tex_resolve_plain(s, c), gd = rp_tex_resolve_plain(d, c);
    if (!gs || !gd) return;
    int nz = sr.depth > 0 ? sr.depth : 1;
    if (s3) nvn_gpu_copy_tex(c, gs, sl, 0, &sr, gd, dl, 0, &dr);   // 三维：片在区域的深度方向上，一次拷完
    else for (int z = 0; z < nz; z++) {
        NVNcopyRegion a = sr, b = dr; a.zoffset = b.zoffset = 0; a.depth = b.depth = 1;
        nvn_gpu_copy_tex(c, gs, sl, sz0 + (uint32_t)sr.zoffset + (uint32_t)z, &a, gd, dl, dz0 + (uint32_t)dr.zoffset + (uint32_t)z, &b);
    }
    rp_tex_mark_gpu_written(d);   // 目标的内容从此以 GPU 为准
}
