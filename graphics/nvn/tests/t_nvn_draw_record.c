// 真实公开 NVN API 录制，同生产 draw_idx_rec / bind_vbuf_rec / bind_program_rec 解码；不提交 GPU 绘制。
// 记录在录制自己的宿主块里（句柄 = 第一条记录），游戏给的命令内存逐字节不动；GetCommandMemoryUsed / Free 按设备字节（hle_nvn.c dev_bytes：
// 绘制 32（DrawMeshTasks 取驱动真值 12）、带索引绘制按入口取驱动真值 DrawElements 24 / BaseVertex 48、72 / Instanced 44、48（间接仍 48）、渲染目标 96、绑程序 64、END 0，其余按记录大小）。混合格式交错检查字段与下一条记录边界；72B 命令内存验证设备口径正好用满不回调。
#include "nvn_protos.h"
#include "nvn_obj.h"
#include "nvn2_cmd.h"   // OP_MULTI_DRAW / p_multi_draw（large_mdi）
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

extern void *h_nvnBootstrapLoader(const char *name);
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "✗ t_nvn_draw_record %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)
static void *load(const char *name) {
    void *p = h_nvnBootstrapLoader(name); CHECK(p); return p;
}
#define API(name) ((NVNT_##name)load(#name))

static NVNdevice device;
static NVNmemoryPool pool;
static uint8_t *memory;

static void start(NVNcommandBuffer *cmd, size_t size) {
    memset(memory, 0xa5, 65536);
    memset(cmd, 0, sizeof *cmd);
    CHECK(API(nvnCommandBufferInitialize)(cmd, &device));
    API(nvnCommandBufferAddCommandMemory)(cmd, &pool, 0, size);
    API(nvnCommandBufferBeginRecording)(cmd);
}
static void untouched(size_t first, size_t last) {
    for (size_t i = first; i < last; i++) CHECK(memory[i] == 0xa5);
}
static uint8_t *records(NVNcommandHandle h) { CHECK(h); CHECK((uint8_t *)(uintptr_t)h < memory || (uint8_t *)(uintptr_t)h >= memory + 65536); return (uint8_t *)(uintptr_t)h; }
static void same_draw(const p_draw_idx *a, const p_draw_idx *b) {
    CHECK(a->prim == b->prim); CHECK(a->itype == b->itype);
    CHECK(a->count == b->count); CHECK(a->base_vertex == b->base_vertex);
    CHECK(a->base_instance == b->base_instance); CHECK(a->instances == b->instances);
    CHECK(a->ibuf == b->ibuf); CHECK(a->ind == b->ind);
    CHECK(a->mdi_cnt == b->mdi_cnt); CHECK(a->mdi_i == b->mdi_i); CHECK(a->pad == 0);
}

static void mixed(void) {
    NVNcommandBuffer cmd; start(&cmd, 512);
    // 地址是命令的输入，不由 CPU 解引用；超过 32 位以捕获窄化错误。
    API(nvnCommandBufferDrawElements)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 17, 0x123456780ull);
    API(nvnCommandBufferDrawElementsBaseVertex)(&cmd, NVN_DRAW_PRIMITIVE_LINES, NVN_INDEX_TYPE_UNSIGNED_INT, 9, 0x234567890ull, -7);
    API(nvnCommandBufferDrawElementsInstanced)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, NVN_INDEX_TYPE_UNSIGNED_BYTE, 23, 0x3456789a0ull, 11, 4, 3);
    // 单实例但 base instance 非零，不能丢失该维度。
    API(nvnCommandBufferDrawElementsInstanced)(&cmd, NVN_DRAW_PRIMITIVE_POINTS, NVN_INDEX_TYPE_UNSIGNED_SHORT, 5, 0x456789ab0ull, -2, 8, 1);
    API(nvnCommandBufferDrawElementsIndirect)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 0x56789abc0ull, 0x6789abcd0ull);
    API(nvnCommandBufferMultiDrawElementsIndirectCount)(&cmd, NVN_DRAW_PRIMITIVE_LINES, NVN_INDEX_TYPE_UNSIGNED_SHORT,
        0x789abcde0ull, 0x89abcdef0ull, 0x9abcdef00ull, 2, 24);
    // 长记录后再接短记录，防止 decoder 误读下一条记录的内容。
    API(nvnCommandBufferDrawElements)(&cmd, NVN_DRAW_PRIMITIVE_POINTS, NVN_INDEX_TYPE_UNSIGNED_BYTE, 2, 0xabcdef010ull);
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    const p_draw_idx expected[] = {
        {.prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .itype=NVN_INDEX_TYPE_UNSIGNED_SHORT, .count=17, .instances=1, .ibuf=0x123456780ull},
        {.prim=NVN_DRAW_PRIMITIVE_LINES, .itype=NVN_INDEX_TYPE_UNSIGNED_INT, .count=9, .base_vertex=-7, .instances=1, .ibuf=0x234567890ull},
        {.prim=NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, .itype=NVN_INDEX_TYPE_UNSIGNED_BYTE, .count=23, .base_vertex=11, .base_instance=4, .instances=3, .ibuf=0x3456789a0ull},
        {.prim=NVN_DRAW_PRIMITIVE_POINTS, .itype=NVN_INDEX_TYPE_UNSIGNED_SHORT, .count=5, .base_vertex=-2, .base_instance=8, .instances=1, .ibuf=0x456789ab0ull},
        {.prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .itype=NVN_INDEX_TYPE_UNSIGNED_INT, .instances=1, .ibuf=0x56789abc0ull, .ind=0x6789abcd0ull},
        {.prim=NVN_DRAW_PRIMITIVE_LINES, .itype=NVN_INDEX_TYPE_UNSIGNED_SHORT, .instances=1, .ibuf=0x789abcde0ull, .ind=0x89abcdef0ull, .mdi_cnt=0x9abcdef00ull},
        {.prim=NVN_DRAW_PRIMITIVE_LINES, .itype=NVN_INDEX_TYPE_UNSIGNED_SHORT, .instances=1, .ibuf=0x789abcde0ull, .ind=0x89abcdf08ull, .mdi_cnt=0x9abcdef00ull, .mdi_i=1},
        {.prim=NVN_DRAW_PRIMITIVE_POINTS, .itype=NVN_INDEX_TYPE_UNSIGNED_BYTE, .count=2, .instances=1, .ibuf=0xabcdef010ull},
    };
    const unsigned sizes[] = {32, 32, 40, 40, 64, 64, 64, 32};
    size_t offset = 0;
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        op_hdr h; memcpy(&h, rec + offset, sizeof h);
        CHECK(h.op == OP_DRAW_IDX); CHECK(h.size == sizes[i]);
        p_draw_idx decoded; memset(&decoded, 0xcc, sizeof decoded);
        same_draw(draw_idx_rec(rec + offset + 8, h.size, &decoded), &expected[i]);
        offset += h.size;
    }
    op_hdr end; memcpy(&end, rec + offset, sizeof end);
    CHECK(end.op == OP_END); CHECK(end.size == 8); CHECK(offset + 8 == 376);
    // 设备字节（驱动真值，#1904 旷野之息 NVN1 驱动实测与 NVN2 反汇编一致）：DrawElements 24、BaseVertex(-7) 72、Instanced(11, 4) 48、Instanced(-2, 8) 48、
    //   间接 48、多绘两条各 48（间接不在入口表，仍按估算）、DrawElements 24、END 0
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 24 + 72 + 48 + 48 + 48 + 2 * 48 + 24);
    CHECK(API(nvnCommandBufferGetCommandMemoryFree)(&cmd) == 512 - (24 + 72 + 48 + 48 + 48 + 2 * 48 + 24));
    untouched(0, 65536);
    API(nvnCommandBufferFinalize)(&cmd);
}

static void tight(void) {
    NVNcommandBuffer cmd; start(&cmd, 88);
    // 设备字节：深度范围 16（记录大小）+ BaseVertex(-13) 72 + END 0 = 88，正好用满；没设回调也不借窗口（借了 Used 不会正好是 88、Free 不会是 0）
    API(nvnCommandBufferSetDepthRange)(&cmd, 0.25f, 0.75f);
    API(nvnCommandBufferDrawElementsBaseVertex)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 31, 0x123450000ull, -13);
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    const op_hdr *prefix = (const op_hdr *)rec;
    CHECK(prefix->op == OP_DEPTH_RANGE); CHECK(prefix->size == 16);
    const op_hdr *h = (const op_hdr *)(rec + 16);
    CHECK(h->op == OP_DRAW_IDX); CHECK(h->size == 32);
    p_draw_idx decoded;
    same_draw(draw_idx_rec(rec + 24, h->size, &decoded), &(p_draw_idx){
        .prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .itype=NVN_INDEX_TYPE_UNSIGNED_INT,
        .count=31, .base_vertex=-13, .instances=1, .ibuf=0x123450000ull});
    const op_hdr *end = (const op_hdr *)(rec + 48);
    CHECK(end->op == OP_END); CHECK(end->size == 8);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 88);
    CHECK(API(nvnCommandBufferGetCommandMemoryFree)(&cmd) == 0);
    untouched(0, 65536);
    API(nvnCommandBufferFinalize)(&cmd);
}

static void vertex_buffers(void) {
    NVNcommandBuffer cmd; start(&cmd, 512);
    // 独立输入覆盖零长度、32位上界/超界，地址始终超过32位。
    const NVNbufferRange ranges[][2] = {
        {{0x123456780ull, 0x180}},
        {{0x234567890ull, 0}},
        {{0x3456789a0ull, UINT32_MAX}},
        {{0x456789ab0ull, (uint64_t)UINT32_MAX + 1}},
        {{0x56789abc0ull, 0x88}, {0x6789abcd0ull, 0x123456789ull}},
    };
    const int first[] = {3, 0, 15, 7, 4};
    const int count[] = {1, 1, 1, 1, 2};
    const unsigned sizes[] = {24, 24, 24, 32, 48};
    for (unsigned i = 0; i < 5; i++) {
        if (i < 4)
            API(nvnCommandBufferBindVertexBuffer)(&cmd, first[i], ranges[i][0].address, ranges[i][0].size);
        else
            API(nvnCommandBufferBindVertexBuffers)(&cmd, first[i], count[i], ranges[i]);
        API(nvnCommandBufferDrawElementsBaseVertex)(&cmd, NVN_DRAW_PRIMITIVE_LINES,
            NVN_INDEX_TYPE_UNSIGNED_SHORT, 11 + i, 0x789abcdef0ull + i * 64, -(int)i);
    }
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    size_t offset = 0;
    for (unsigned i = 0; i < 5; i++) {
        op_hdr h; memcpy(&h, rec + offset, sizeof h);
        CHECK(h.op == OP_BIND_VBUF); CHECK(h.size == sizes[i]);
        p_bind_vbuf tmp; memset(&tmp, 0xcc, sizeof tmp);
        const p_bind_vbuf *decoded = bind_vbuf_rec(rec + offset + 8, h.size, &tmp);
        CHECK(decoded->first == first[i]); CHECK(decoded->n == count[i]);
        for (int j = 0; j < count[i]; j++) {
            CHECK(decoded->r[j].address == ranges[i][j].address);
            CHECK(decoded->r[j].size == ranges[i][j].size);
        }
        offset += h.size;
        memcpy(&h, rec + offset, sizeof h);
        CHECK(h.op == OP_DRAW_IDX); CHECK(h.size == 32);
        p_draw_idx draw;
        same_draw(draw_idx_rec(rec + offset + 8, h.size, &draw), &(p_draw_idx){
            .prim=NVN_DRAW_PRIMITIVE_LINES, .itype=NVN_INDEX_TYPE_UNSIGNED_SHORT,
            .count=11 + i, .base_vertex=-(int)i, .instances=1, .ibuf=0x789abcdef0ull + i * 64});
        offset += h.size;
    }
    op_hdr end; memcpy(&end, rec + offset, sizeof end);
    CHECK(end.op == OP_END); CHECK(end.size == 8); CHECK(offset + 8 == 320);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 24 + 24 + 24 + 32 + 48 + 48 + 4 * 72);   // 绑顶点缓冲按记录大小；BaseVertex 基顶点 0 是 48、-1..-4 各 72；END 0
    CHECK(API(nvnCommandBufferGetCommandMemoryFree)(&cmd) == 512 - (152 + 48 + 288));
    untouched(0, 65536);
    API(nvnCommandBufferFinalize)(&cmd);
}
// 游戏的内存回调按「至少 N 字节」正好补 N（无主之地3 虚幻 4：每次 40 字节、真池真偏移）：宿主照用，不借窗口（11-01 前不足 64 字节当没给、改借，借的段永不回收）。
// N 是设备字节：每条 DrawElements 24、END 0；每次正好补一条，20 条绘制共 20 次回调、用掉 20 × 24 字节（END 不占字节、不回调）
static size_t g_ring = 0x8000; static unsigned g_cbs; static size_t g_need[32];
static void exact_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent e, size_t min, void *d) {
    (void)d; CHECK(e == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY); CHECK(g_ring + min <= 65536); CHECK(g_cbs < 32);
    g_need[g_cbs++] = min;
    API(nvnCommandBufferAddCommandMemory)(c, &pool, (ptrdiff_t)g_ring, min); g_ring += min;
}
static void exact_grant(void) {
    NVNcommandBuffer cmd; memset(&cmd, 0, sizeof cmd); CHECK(API(nvnCommandBufferInitialize)(&cmd, &device));
    API(nvnCommandBufferSetMemoryCallback)(&cmd, exact_cb);
    API(nvnCommandBufferBeginRecording)(&cmd);
    for (int i = 0; i < 20; i++) API(nvnCommandBufferDrawElements)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3 + i, 0x100000000ull + 64 * (uint64_t)i);
    CHECK(API(nvnCommandBufferEndRecording)(&cmd) != 0);
    CHECK(g_cbs == 20);
    for (unsigned i = 0; i < 20; i++) CHECK(g_need[i] == 24);
    CHECK(g_ring == 0x8000 + 20 * 24);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 24);   // 从最后一次补的内存算起：最后一条绘制（借窗口不清零已用，借过就不会是 24）
    CHECK(API(nvnCommandBufferGetCommandMemoryFree)(&cmd) == 0);
    API(nvnCommandBufferFinalize)(&cmd);
}
// GetCommandMemorySize = 游戏最近一次 AddCommandMemory 给的段长，录制中不变（大乱斗 01006A800016E000 按它给命令缓冲排序，main+0x3850850 introsort；
//   旧实现 Used + Free 现算，溢出到宿主段 / 换段中间态 / 并发读时变，比较不稳定，扫描越过数组头读 0x50）。期望按设备字节手算（hle_nvn.c g_dev_bytes）：
//   DrawElements 24B（驱动真值）、END 0B；设备游标不写跳转（跳转只在宿主块里）
static void memory_size(void) {
    PFNNVNCOMMANDBUFFERGETCOMMANDMEMORYSIZEPROC size_of = API(nvnCommandBufferGetCommandMemorySize);
    PFNNVNCOMMANDBUFFERGETCOMMANDMEMORYUSEDPROC used_of = API(nvnCommandBufferGetCommandMemoryUsed);
    PFNNVNCOMMANDBUFFERGETCOMMANDMEMORYFREEPROC free_of = API(nvnCommandBufferGetCommandMemoryFree);
    NVNcommandBuffer cmd; memset(&cmd, 0, sizeof cmd); CHECK(API(nvnCommandBufferInitialize)(&cmd, &device));
    CHECK(size_of(&cmd) == 0); CHECK(free_of(&cmd) == 0);                      // 还没给过命令内存
    API(nvnCommandBufferAddCommandMemory)(&cmd, &pool, 0x2000, 256);
    CHECK(size_of(&cmd) == 256); CHECK(free_of(&cmd) == 256);
    API(nvnCommandBufferBeginRecording)(&cmd);
    // 256B 段：每条 24 设备字节，前 10 条（240B）在段内，第 11 条（264B）装不下；无回调 → 借宿主窗口
    for (int i = 1; i <= 11; i++) {
        API(nvnCommandBufferDrawElements)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3, 0x100000000ull);
        CHECK(size_of(&cmd) == 256);
        if (i <= 10) { CHECK(used_of(&cmd) == 24u * (unsigned)i); CHECK(free_of(&cmd) == 256 - 24u * (unsigned)i); }
    }
    CHECK(used_of(&cmd) == 256);                                             // 借窗口：游戏给的段算用尽，已用 = 大小、不超过大小（审计 #1534 P1-99；旧实现会报 11 × 24 = 264 > 256）
    CHECK(free_of(&cmd) == 0);                                               // 已超出游戏给的段：Free = 大小 − 已用 = 0，不报宿主窗口剩余
    // 录制中游戏再给 128B：Size 换成 128，Used 从新段起算
    API(nvnCommandBufferAddCommandMemory)(&cmd, &pool, 0x3000, 128);
    CHECK(size_of(&cmd) == 128); CHECK(used_of(&cmd) == 0); CHECK(free_of(&cmd) == 128);
    API(nvnCommandBufferDrawElements)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3, 0x100000000ull);
    CHECK(size_of(&cmd) == 128); CHECK(used_of(&cmd) == 24); CHECK(free_of(&cmd) == 104);
    CHECK(API(nvnCommandBufferEndRecording)(&cmd) != 0);
    CHECK(size_of(&cmd) == 128); CHECK(used_of(&cmd) == 24); CHECK(free_of(&cmd) == 104);   // END 不占字节
    // 游戏回调拿不出内存、给空池（宿主借窗口续录）：Size 是游戏给的 0，不是宿主窗口长
    API(nvnCommandBufferAddCommandMemory)(&cmd, NULL, 0, 0);
    CHECK(size_of(&cmd) == 0); CHECK(free_of(&cmd) == 0);
    API(nvnCommandBufferFinalize)(&cmd);
}
// 另一线程并发读 Size：游戏每次回调都补 1 KB，读到的只能是 1024（录制线程写记录、换段跳转、清零 used 的中间态都不该漏出来）
#define RING_SEG 1024u
static size_t g_szring; static volatile int g_szdone; static NVNcommandBuffer g_szcmd; static unsigned long g_szreads, g_szbad; static size_t g_szbadval;
static void ring_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent e, size_t min, void *d) {
    (void)e; (void)d; CHECK(min <= RING_SEG);
    if (g_szring + RING_SEG > 65536) g_szring = 0x4000;
    API(nvnCommandBufferAddCommandMemory)(c, &pool, (ptrdiff_t)g_szring, RING_SEG); g_szring += RING_SEG;
}
static void *size_reader(void *arg) {
    PFNNVNCOMMANDBUFFERGETCOMMANDMEMORYSIZEPROC size_of = (PFNNVNCOMMANDBUFFERGETCOMMANDMEMORYSIZEPROC)arg;
    while (!__atomic_load_n(&g_szdone, __ATOMIC_ACQUIRE)) {
        size_t v = size_of(&g_szcmd); g_szreads++;
        if (v != RING_SEG) { g_szbad++; g_szbadval = v; }
    }
    return NULL;
}
static void memory_size_concurrent(void) {
    memset(&g_szcmd, 0, sizeof g_szcmd); CHECK(API(nvnCommandBufferInitialize)(&g_szcmd, &device));
    API(nvnCommandBufferSetMemoryCallback)(&g_szcmd, ring_cb);
    g_szring = 0x4000 + RING_SEG; API(nvnCommandBufferAddCommandMemory)(&g_szcmd, &pool, 0x4000, RING_SEG);
    PFNNVNCOMMANDBUFFERDRAWELEMENTSPROC draw = API(nvnCommandBufferDrawElements);
    PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC begin = API(nvnCommandBufferBeginRecording);
    PFNNVNCOMMANDBUFFERENDRECORDINGPROC endrec = API(nvnCommandBufferEndRecording);
    pthread_t t; CHECK(pthread_create(&t, NULL, size_reader, (void *)API(nvnCommandBufferGetCommandMemorySize)) == 0);
    for (int r = 0; r < 20000; r++) {
        begin(&g_szcmd);
        for (int i = 0; i < 20; i++) draw(&g_szcmd, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3, 0x100000000ull);
        CHECK(endrec(&g_szcmd) != 0);
    }
    __atomic_store_n(&g_szdone, 1, __ATOMIC_RELEASE); pthread_join(t, NULL);
    fprintf(stderr, "  并发读 Size：%lu 次，异常 %lu 次（末个异常值 %zu）\n", g_szreads, g_szbad, g_szbadval);
    CHECK(g_szreads > 0); CHECK(g_szbad == 0);
    API(nvnCommandBufferFinalize)(&g_szcmd);
}
static void viewports(void) {
    NVNcommandBuffer cmd; start(&cmd, 512);
    // 视口按实际个数录：头 8B + first/n 8B + 每个 24B（6 个 float 一格）；n=1 共 40B、n=2 共 64B（定长时每条 408B）
    API(nvnCommandBufferSetViewport)(&cmd, 10, 20, 30, 40);
    const float v2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    API(nvnCommandBufferSetViewports)(&cmd, 2, 2, v2);
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    op_hdr h; memcpy(&h, rec, sizeof h); CHECK(h.op == OP_VIEWPORTS); CHECK(h.size == 40);
    const p_viewports *a = (const p_viewports *)(rec + 8);
    CHECK(a->first == 0); CHECK(a->n == 1); CHECK(a->v[0][0] == 10.f); CHECK(a->v[0][3] == 40.f);
    memcpy(&h, rec + 40, sizeof h); CHECK(h.op == OP_VIEWPORTS); CHECK(h.size == 64);
    const p_viewports *b = (const p_viewports *)(rec + 48);
    CHECK(b->first == 2); CHECK(b->n == 2); CHECK(b->v[0][0] == 1.f); CHECK(b->v[1][0] == 5.f); CHECK(b->v[1][3] == 8.f);
    memcpy(&h, rec + 104, sizeof h); CHECK(h.op == OP_END); CHECK(h.size == 8);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 104);   // 视口按记录大小：40 + 64 + END 0
    API(nvnCommandBufferFinalize)(&cmd);
}

// SET_RT 按实际内容录（nvn_obj.h p_set_rt_c）：头 8B + 紧凑头 8B + 非空纹理 k 个指针 8B + 有视图 v 个 view_t + k 份 tex_t 快照
// 定长旧格式每条 8 + 240 + sizeof(tex_t) × (n + 1)：n=1 504B、n=2 632B，与深度 / 视图无关
static size_t set_rt_expect(unsigned k, unsigned v) { return 8 + 8 + 8 * (size_t)k + sizeof(view_t) * v + sizeof(tex_t) * k; }
static void texobj(tex_t *t, uint64_t serial) { memset(t, 0, sizeof *t); t->magic = MAGIC('T', 'E', 'X', 0); t->serial = serial; t->d.width = 64; }
static void viewobj(view_t *v, int level, int layer) { memset(v, 0, sizeof *v); v->set_levels = v->set_layers = 1; v->base_level = (uint8_t)level; v->nlevels = 1; v->min_layer = (uint16_t)layer; v->nlayers = 1; }
static int in_rec(const void *p, const uint8_t *rec, size_t size) { return (const uint8_t *)p >= rec && (const uint8_t *)p + sizeof(tex_t) <= rec + size; }
static void render_targets(void) {
    NVNcommandBuffer cmd; start(&cmd, 2048);
    tex_t c0, c1, d; texobj(&c0, 101); texobj(&c1, 102); texobj(&d, 103);
    view_t v0, v1, dv; viewobj(&v0, 1, 2); viewobj(&v1, 3, 4); viewobj(&dv, 5, 6);
    const NVNtexture *one[1] = {(const NVNtexture *)&c0}, *two[2] = {(const NVNtexture *)&c0, (const NVNtexture *)&c1};
    const NVNtextureView *views[2] = {(const NVNtextureView *)&v0, (const NVNtextureView *)&v1};
    API(nvnCommandBufferSetRenderTargets)(&cmd, 1, one, NULL, NULL, NULL);                                        // n=1 无深度：k=1 v=0
    API(nvnCommandBufferSetRenderTargets)(&cmd, 1, one, NULL, (const NVNtexture *)&d, NULL);                      // n=1 带深度：k=2 v=0
    API(nvnCommandBufferSetRenderTargets)(&cmd, 2, two, views, (const NVNtexture *)&d, (const NVNtextureView *)&dv);   // n=2 带深度与视图：k=3 v=3
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    const size_t want[3] = {set_rt_expect(1, 0), set_rt_expect(2, 0), set_rt_expect(3, 3)};
    CHECK(want[0] == 152 && want[1] == 288 && want[2] == 472);   // sizeof(tex_t) = 128、sizeof(view_t) = 16 时的值，算式与格式说明对得上
    size_t off = 0; p_set_rt t[3];
    for (int i = 0; i < 3; i++) {
        op_hdr h; memcpy(&h, rec + off, sizeof h); CHECK(h.op == OP_SET_RT); CHECK(h.size == want[i]);
        CHECK(set_rt_rec(rec + off + 8, h.size, &t[i]) == &t[i]);
        const p_set_rt *r = &t[i];
        for (int s = 0; s < 8; s++) { if (s >= r->n) CHECK(!r->color[s]); if (s >= r->n || i < 2) CHECK(!r->has_cview[s]); }
        for (int s = 0; s < r->n; s++) { CHECK(in_rec(r->color[s], rec + off, h.size)); CHECK(r->color[s]->serial == (s ? 102u : 101u)); }
        if (r->depth) { CHECK(in_rec(r->depth, rec + off, h.size)); CHECK(r->depth->serial == 103); }
        off += h.size;
    }
    CHECK(t[0].n == 1 && !t[0].depth && !t[0].has_dview);
    CHECK(t[1].n == 1 && t[1].depth && !t[1].has_dview);
    CHECK(t[2].n == 2 && t[2].depth && t[2].color[0] != t[2].color[1]);
    CHECK(t[2].has_cview[0] && t[2].cview[0].base_level == 1 && t[2].cview[0].min_layer == 2);
    CHECK(t[2].has_cview[1] && t[2].cview[1].base_level == 3 && t[2].cview[1].min_layer == 4);
    CHECK(t[2].has_dview && t[2].dview.base_level == 5 && t[2].dview.min_layer == 6);
    op_hdr end; memcpy(&end, rec + off, sizeof end); CHECK(end.op == OP_END); CHECK(end.size == 8);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 3 * 96);   // 渲染目标的快照只在宿主记录里：设备按 96 计
    untouched(0, 65536);
    API(nvnCommandBufferFinalize)(&cmd);
}

// BindProgram 只录程序身份（11-01，勇者斗恶龙 XI S 命令内存 OOM）：整条 = 头 + prog 指针 + 序号 + 阶段位 + 补齐；内容按序号从宿主表还原。
// 键的期望独立算：SHA-256(control 前 0x880 字节 ‖ code 前 ncode 字节) 的前 8 字节（control +0x20 自描述长度为 0 时取 0x880）。
enum { CODE_OFF = 0x7000, CODE_N = 0x40, MANY = 300 };
static uint8_t ctl_buf[MANY + 4][0x880];
static void key_of(const uint8_t *ctl, const uint8_t *code, uint32_t n, uint8_t out[8]) {
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, ctl, 0x880); CC_SHA256_Update(&c, code, n); CC_SHA256_Final(md, &c); memcpy(out, md, 8);
}
static NVNshaderData shader(int i, uint32_t code_off) {
    uint8_t *ctl = ctl_buf[i]; memset(ctl, 0, 0x880);
    uint32_t n = CODE_N; memcpy(ctl + 0x704, &n, 4); ctl[0x100] = (uint8_t)i; ctl[0x101] = (uint8_t)(i >> 8);   // 各段内容不同，键不同
    return (NVNshaderData){API(nvnMemoryPoolGetBufferAddress)(&pool) + code_off, ctl};
}
// NVN2 构建器建程序（金刚 11-04）：SetDefaults / SetDevice / SetShaders 后 InitializeFromBuilder，与 ProgramInitialize + SetShaders 建出同一份程序：
//   2 个阶段、两阶段键 = 独立算的 SHA-256 前 8 字节；阶段数越界回失败
static void program_builder(void) {
    typedef void (*f_def)(void *); typedef void (*f_dev)(void *, NVNdevice *); typedef void (*f_sh)(void *, int, const NVNshaderData *);
    typedef NVNboolean (*f_init)(NVNprogram *, const void *);
    for (int i = 0; i < 2 * CODE_N; i++) memory[CODE_OFF + i] = (uint8_t)(i * 7 + 1);
    _Alignas(16) uint8_t b[0x80]; memset(b, 0xcc, sizeof b);   // NVN2 程序构建器：驱动 SetDefaults 写到 +0x63（11-04 起按驱动布局存）
    ((f_def)load("nvnProgramBuilderSetDefaults"))(b); ((f_dev)load("nvnProgramBuilderSetDevice"))(b, &device);
    NVNshaderData sd[2] = {shader(0, CODE_OFF), shader(1, CODE_OFF + CODE_N)};
    ((f_sh)load("nvnProgramBuilderSetShaders"))(b, 2, sd);
    static NVNprogram p; CHECK(((f_init)load("nvnProgramInitializeFromBuilder"))(&p, b));
    uint8_t want[2][8]; key_of(ctl_buf[0], memory + CODE_OFF, CODE_N, want[0]); key_of(ctl_buf[1], memory + CODE_OFF + CODE_N, CODE_N, want[1]);
    NVNcommandBuffer cmd; start(&cmd, 512);   // 快照在绑定时登记（同游戏：建完程序再绑）
    API(nvnCommandBufferBindProgram)(&cmd, &p, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT); API(nvnCommandBufferEndRecording)(&cmd);
    uint8_t tk[6][8], tok = 0, tst[6]; const prog_t *snap = nvn_prog_snap_get(((const prog_t *)&p)->serial, tk, &tok, tst);
    CHECK(snap); CHECK(snap->nstage == 2); CHECK(tok == 3); CHECK(!memcmp(tk[0], want[0], 8)); CHECK(!memcmp(tk[1], want[1], 8));
    static NVNprogram bad; ((f_sh)load("nvnProgramBuilderSetShaders"))(b, 7, sd); CHECK(!((f_init)load("nvnProgramInitializeFromBuilder"))(&bad, b));
}
static void program_bind(void) {
    NVNcommandBuffer cmd; start(&cmd, 512);
    for (int i = 0; i < 2 * CODE_N; i++) memory[CODE_OFF + i] = (uint8_t)(i * 7 + 1);
    static NVNprogram prog; CHECK(API(nvnProgramInitialize)(&prog, &device));
    NVNshaderData sd[2] = {shader(0, CODE_OFF), shader(1, CODE_OFF + CODE_N)};
    CHECK(API(nvnProgramSetShaders)(&prog, 2, sd));
    uint8_t want[2][8]; key_of(ctl_buf[0], memory + CODE_OFF, CODE_N, want[0]); key_of(ctl_buf[1], memory + CODE_OFF + CODE_N, CODE_N, want[1]);
    const uint64_t serial = ((const prog_t *)&prog)->serial;
    API(nvnCommandBufferBindProgram)(&cmd, &prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    const unsigned want_size = 8 /* op_hdr */ + 8 /* prog */ + 8 /* serial */ + 4 /* stages */ + 4 /* pad */;
    op_hdr h; memcpy(&h, rec, sizeof h);
    CHECK(h.op == OP_BIND_PROGRAM); CHECK(h.size == want_size);
    op_hdr end; memcpy(&end, rec + h.size, sizeof end); CHECK(end.op == OP_END);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 64);   // 绑程序设备按 64 计、END 0
    // 解码与直接查表一致，且与独立期望一致
    uint8_t tk[6][8], tok = 0xee, tst[6];
    const prog_t *snap = nvn_prog_snap_get(serial, tk, &tok, tst); CHECK(snap);
    const uint8_t *code0 = memory + CODE_OFF;
    #define SAME_AS_BIND(b) do { \
        CHECK((b)->prog == (const prog_t *)&prog); CHECK((b)->stages == (NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT)); \
        CHECK((b)->snap.serial == serial); CHECK((b)->snap.nstage == 2); \
        CHECK((b)->snap.sd[0].data == (uint64_t)(uintptr_t)code0); CHECK((b)->snap.sd[1].data == (uint64_t)(uintptr_t)(code0 + CODE_N)); \
        CHECK((b)->snap.sd[0].control == NULL && (b)->snap.sd[1].control == NULL); \
        CHECK((b)->kok == 3); CHECK(!memcmp((b)->keys[0], want[0], 8)); CHECK(!memcmp((b)->keys[1], want[1], 8)); } while (0)
    p_bind_program t; memset(&t, 0xcc, sizeof t);
    const p_bind_program *b = bind_program_rec(rec + 8, h.size, &t);
    SAME_AS_BIND(b);
    CHECK(tok == b->kok); CHECK(!memcmp(tk, b->keys, sizeof tk)); CHECK(!memcmp(tst, b->stage_of, sizeof tst));
    CHECK(!memcmp(&b->snap, snap, sizeof *snap));
    // 绑定后游戏重新 SetShaders 同一对象（换一阶段、换内容）、再造远超 64 个新序号并都绑定，最后 Finalize：最初那条记录仍还原出绑定当时的程序
    NVNshaderData sd2 = shader(2, CODE_OFF + CODE_N);
    CHECK(API(nvnProgramSetShaders)(&prog, 1, &sd2)); CHECK(((const prog_t *)&prog)->serial != serial);
    NVNcommandBuffer many; memset(&many, 0, sizeof many); CHECK(API(nvnCommandBufferInitialize)(&many, &device));
    API(nvnCommandBufferAddCommandMemory)(&many, &pool, 0x1000, 0x5000); API(nvnCommandBufferBeginRecording)(&many);
    static NVNprogram others[MANY];
    for (int i = 0; i < MANY; i++) {
        CHECK(API(nvnProgramInitialize)(&others[i], &device)); NVNshaderData s1 = shader(3 + i, CODE_OFF);
        CHECK(API(nvnProgramSetShaders)(&others[i], 1, &s1));
        API(nvnCommandBufferBindProgram)(&many, &others[i], NVN_SHADER_STAGE_VERTEX_BIT);
    }
    CHECK(API(nvnCommandBufferEndRecording)(&many) != 0);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&many) == MANY * 64);
    API(nvnProgramFinalize)(&prog);
    for (int i = 0; i < MANY; i++) API(nvnProgramFinalize)(&others[i]);
    memset(&t, 0xcc, sizeof t);
    b = bind_program_rec(rec + 8, h.size, &t);
    SAME_AS_BIND(b);
    CHECK(nvn_prog_snap_get(serial, NULL, NULL, NULL) == snap);   // 表扩容不搬条目：回放层持有的快照指针仍有效
    // 长度不对的载荷不还原（什么也不绑）
    memset(&t, 0xcc, sizeof t); b = bind_program_rec(rec + 8, h.size + 8, &t);
    CHECK(b->prog == NULL && b->stages == 0 && b->kok == 0);
    #undef SAME_AS_BIND
    API(nvnCommandBufferFinalize)(&many); API(nvnCommandBufferFinalize)(&cmd);
}
// 间接计数多绘超过 1,024 条（补齐上游参考实现（MIT）c7ba23df）：整段录成一条 OP_MULTI_DRAW，字段原样（地址不是游戏窗口里的，GA 原样返回）、后面紧跟 END；
//   正好 1,024 条的照旧逐条录（第一条是 OP_DRAW、mdi_i 0）。原先 1,025 条以上截成前 1,024 条逐条录，第 1,024 条以后的绘制全丢（MIO 最多 4,594 / 8,192 条）
static void large_mdi(void) {
    NVNcommandBuffer cmd; start(&cmd, 512);
    API(nvnCommandBufferMultiDrawArraysIndirectCount)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x123456780ull, 0x234567890ull, 1025, 16);
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    op_hdr h; memcpy(&h, rec, sizeof h);
    CHECK(h.op == OP_MULTI_DRAW); CHECK(h.size == 8 + sizeof(p_multi_draw));
    p_multi_draw m; memcpy(&m, rec + 8, sizeof m);
    CHECK(m.ind == 0x123456780ull); CHECK(m.cnt == 0x234567890ull); CHECK(m.ibuf == 0); CHECK(m.stride == 16); CHECK(m.max == 1025);
    CHECK(m.prim == NVN_DRAW_PRIMITIVE_TRIANGLES); CHECK(m.itype == -1);
    memcpy(&h, rec + h.size, sizeof h); CHECK(h.op == OP_END);
    API(nvnCommandBufferFinalize)(&cmd);
    start(&cmd, 512);   // 带索引、负步长、INT_MAX 条
    API(nvnCommandBufferMultiDrawElementsIndirectCount)(&cmd, NVN_DRAW_PRIMITIVE_LINES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 0x3456789a0ull, 0x456789ab0ull, 0x56789abc0ull, 0x7fffffff, -20);
    rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    memcpy(&h, rec, sizeof h); CHECK(h.op == OP_MULTI_DRAW);
    memcpy(&m, rec + 8, sizeof m);
    CHECK(m.ind == 0x456789ab0ull); CHECK(m.cnt == 0x56789abc0ull); CHECK(m.ibuf == 0x3456789a0ull); CHECK(m.stride == -20); CHECK(m.max == 0x7fffffff);
    CHECK(m.prim == NVN_DRAW_PRIMITIVE_LINES); CHECK(m.itype == NVN_INDEX_TYPE_UNSIGNED_SHORT);
    memcpy(&h, rec + h.size, sizeof h); CHECK(h.op == OP_END);
    API(nvnCommandBufferFinalize)(&cmd);
    start(&cmd, 512);   // 正好 1,024 条：照旧逐条
    API(nvnCommandBufferMultiDrawArraysIndirectCount)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x123456780ull, 0x234567890ull, 1024, 16);
    rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    memcpy(&h, rec, sizeof h); CHECK(h.op == OP_DRAW);
    p_draw dt; const p_draw *dp = draw_rec(rec + 8, h.size, &dt); p_draw d = *dp; CHECK(d.ind == 0x123456780ull); CHECK(d.mdi_cnt == 0x234567890ull); CHECK(d.mdi_i == 0);
    API(nvnCommandBufferFinalize)(&cmd);
}
// DrawMeshTasks 的设备字节按驱动真值 12（NVN2 驱动只写一条方法头加 2 个数据字），不按表里 OP_DRAW 的 32；覆盖只在这一条，之后的 DrawArrays 仍查表 32
static void mesh_tasks(void) {
    void (*mesh)(NVNcommandBuffer *, int, int) = (void (*)(NVNcommandBuffer *, int, int))load("nvnCommandBufferDrawMeshTasks");
    NVNcommandBuffer cmd; start(&cmd, 512);
    mesh(&cmd, 3, 7);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 12);
    API(nvnCommandBufferDrawArrays)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 12 + 32);
    mesh(&cmd, 0, 1);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 12 + 32 + 12);
    CHECK(API(nvnCommandBufferGetCommandMemoryFree)(&cmd) == 512 - (12 + 32 + 12));
    uint8_t *rec = records(API(nvnCommandBufferEndRecording)(&cmd));
    op_hdr h; memcpy(&h, rec, sizeof h); CHECK(h.op == OP_DRAW);
    p_draw dt; const p_draw *d = draw_rec(rec + 8, h.size, &dt);
    CHECK(d->prim == DM_PRIM_MESH_TASKS); CHECK(d->first == 3); CHECK(d->count == 7); CHECK(d->instances == 1);
    CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == 12 + 32 + 12);   // END 0
    untouched(0, 512);
    API(nvnCommandBufferFinalize)(&cmd);
}
// 顶点属性 / 顶点流状态的设备字节：NVN2 按驱动真值（属性 8 + 4 × n，流 16 + 4 × n + 4 × m，n / m 各补成偶数，m = 最后一个除数非 0 的流号 + 1）；
//   NVN1 仍按记录大小（属性 8 + 8 + 4 × n、流 8 + 8 + 8 × n，按 8 对齐）。平台标志由原生层读 NPDM 设（svc_mem.c s2a_npdm_ounce）：合成 NPDM 只填 META、
//   ACID 偏移（+0x78）与 ACID 魔数（新一代平台在 ACID + 0x300，上一代在 + 0x200），同 vm_test.c case_npdm_ounce
int s2a_npdm_ounce(const char *npdm);
int s2a_platform_ounce(void);
static void set_ounce(int on) {
    char path[1024]; const char *td = getenv("TMPDIR"); snprintf(path, sizeof path, "%s/t_nvn_draw_record_npdm_XXXXXX", td && *td ? td : "/tmp");
    int fd = mkstemp(path); CHECK(fd >= 0);
    uint8_t buf[0x500] = {0}; uint32_t acid = 0x100; memcpy(buf, "META", 4); memcpy(buf + 0x78, &acid, 4); memcpy(buf + acid + (on ? 0x300 : 0x200), "ACID", 4);
    CHECK(write(fd, buf, sizeof buf) == (ssize_t)sizeof buf); close(fd);
    CHECK(s2a_npdm_ounce(path) == on); CHECK(s2a_platform_ounce() == on); unlink(path);
}
static void vertex_state(void) {
    NVNvertexAttribState a[5]; NVNvertexStreamState s[3];
    for (int i = 0; i < 5; i++) API(nvnVertexAttribStateSetDefaults)(&a[i]);
    const struct { int ounce, na, ns, div_at; size_t attr, stream; } cases[] = {
        {1, 4, 2, -1, 24, 24},   // 四个属性 8 + 16；两个流 16 + 8，除数全 0
        {1, 5, 3, -1, 32, 32},   // 五个属性补成六个；三个流补成四个
        {1, 4, 3, 1, 24, 40},    // 流 1 有除数：m = 2
        {1, 4, 3, 2, 24, 48},    // 流 2 有除数：m = 3，补成 4
        {0, 4, 2, -1, 32, 32},   // NVN1：记录大小
        {0, 5, 3, 2, 40, 40},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        set_ounce(cases[k].ounce);
        for (int i = 0; i < 3; i++) {
            API(nvnVertexStreamStateSetDefaults)(&s[i]); API(nvnVertexStreamStateSetStride)(&s[i], 16 + 4 * i);
            API(nvnVertexStreamStateSetDivisor)(&s[i], i == cases[k].div_at ? 2 : 0);
        }
        NVNcommandBuffer cmd; start(&cmd, 512);
        API(nvnCommandBufferBindVertexAttribState)(&cmd, cases[k].na, a);
        CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == cases[k].attr);
        API(nvnCommandBufferBindVertexStreamState)(&cmd, cases[k].ns, s);
        CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == cases[k].attr + cases[k].stream);
        API(nvnCommandBufferBindVertexBuffer)(&cmd, 0, 0x123456780ull, 0x100);   // 单个顶点缓冲 24，与驱动相同，两种平台都不变
        CHECK(API(nvnCommandBufferGetCommandMemoryUsed)(&cmd) == cases[k].attr + cases[k].stream + 24);
        records(API(nvnCommandBufferEndRecording)(&cmd));
        untouched(0, 512);
        API(nvnCommandBufferFinalize)(&cmd);
    }
    set_ounce(0);
}
int main(void) {
    // 空着色器映射表：SetShaders 算出的键都查不到、不现译（本测只核记录与快照表，不建管线）
    char map[1024]; const char *td = getenv("TMPDIR"); snprintf(map, sizeof map, "%s/t_nvn_draw_record_XXXXXX", td && *td ? td : "/tmp");
    int fd = mkstemp(map); CHECK(fd >= 0); close(fd); setenv("DM_NVN_SHMAP", map, 1);
    NVNdeviceBuilder db = {{0}};
    API(nvnDeviceBuilderSetDefaults)(&db); CHECK(API(nvnDeviceInitialize)(&device, &db));
    CHECK(posix_memalign((void **)&memory, 65536, 65536) == 0);
    NVNmemoryPoolBuilder pb = {{0}};
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, memory, 65536);
    CHECK(API(nvnMemoryPoolInitialize)(&pool, &pb));
    mixed(); large_mdi(); tight(); vertex_buffers(); viewports(); render_targets(); exact_grant(); program_bind(); program_builder(); memory_size(); memory_size_concurrent(); mesh_tasks(); vertex_state();
    API(nvnMemoryPoolFinalize)(&pool); API(nvnDeviceFinalize)(&device); free(memory); unlink(map);
    printf("✓ t_nvn_draw_record：%u 条断言，记录在宿主块、游戏命令内存不动、已用按设备字节，混合draw/VB解码与72B正好用满、视口与渲染目标按实际内容录；回调按设备字节正好补所需不借窗口；BindProgram 32B 身份记录按序号还原、重设 / 终结 / 300 个新序号后仍取绑定当时快照；命令内存 Size 录制 / 换段 / 借窗口 / 并发读中恒为最近一次 AddCommandMemory 的段长\n", checks);
    return 0;
}
