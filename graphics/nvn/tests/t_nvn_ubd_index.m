// 遍内常量更新延后表的查找（11-01 怪猎崛起提交线程）：直接包含生产 nvn_mtl.m，调 ubd_add / ubd_hits / ubd_covers / ubd_read / ubd_shadow / ubd_flush。
// 期望按规则手写，不由被测实现推出：测试里另写一份「逐条扫列表」的参照（改前的语义：新更新整段盖住的旧更新出表、其余按延后顺序留着；
//   命中 = 有一条与查询区间相交；盖满 = 各条区间之并覆盖查询区间；读出 = 各条按延后顺序依次铺上去、每字节留最新写的；
//   影子原地用 = 与区间相交的最新一条整段包住区间且地址 16 字节对齐）。
//   ① 随机 4000 次插入（区间落在 48 页内、长 16–1024 字节、起点 16 字节对齐、互相交叠），每次插入后与参照逐条比表（个数、顺序、区间），
//      再随机 40 个查询区间比命中 / 盖满 / 读出字节 / 影子是否原地；表满 512 条时着陆清空（同 ubd_flush）
//   ② 查找开销：512 条更新铺在 128 页上（每页 4 条），查询区间与它们同页但不相交（绑定和更新同页、页过滤挡不住的情形）。
//      每次 ubd_hits + ubd_covers 的用户态指令数：参照扫法每条至少 3 条指令（取区间、两次比较），512 条 ≥ 1536；有序表二分 log2(512) = 9 步，
//      判据 < 400 条 / 次。
//   ③ 512 个互不相同的页（#895 页过滤表满了死循环的场景）：5 秒内返回、加过的全命中、没加过的不命中；着陆后表空。
//   ④ 真录真画（公开 NVN 接口录命令、提交回放、GPU 读回）：64×1 目标，一遍里画 64 笔，第 i 笔前用 UpdateUniformBuffer 往 3 个 256 字节常量块之一
//      （i % 3）写颜色 i（偶数笔只写块头 16 字节、其余从内存来；奇数笔整块 256 字节），绑到片元 c3、裁剪到第 i 列，片元着色器输出 c3[0]。
//      NVN 按命令顺序生效：第 i 列 = 颜色 i（RGBA8：R = 4i、G = 255 − 4i、B = i、A = 255），64 / 64 列都对。常量块 256 字节、16 KB 各一遍（16 KB 的影子走拷贝线程）。单测 mk 编码线程开 / 关各跑一遍
//   ⑥ 同 ④ 的 16 KB 一遍但每笔只写块头 16 字节（每笔都要部分覆盖影子），提交前让拷贝线程先忙一段 256 MB 的拷贝（这一遍影子的拷贝排在后面）：64 / 64 列仍对 = 提交等到了影子拷完。改前 Metal 3 且编码线程关时主命令缓冲提交不等拷贝线程，
//      GPU 读到还没写的影子（新块全 0）→ 走影子的列错
//   ⑦ 分段提交（arena_cut）后、GPU 做完那一段（arena_seg_done）、暂存块被再用（写成 0xEE）：再取同一段的影子，内容仍 = 内存 + 更新。改前影子缓存按提交认，命中就交回已被再用的旧地址
//   ⑤ 宽屏视图读影子（wd_cb_view，CPU 当场读）：4 KB 绑定部分更新后读，内容 = 内存 + 更新、交给拷贝线程 0 次（改前 2 次：底 + 更新，读的人再等它）；再更新、再读（从上一份影子续拼）同样；
//      16 KB 先绑给 GPU（ubq_gpu：交拷贝线程 2 次）、再更新、再当视图读：内容 = 内存 + 两次更新（续拼要等拷贝线程那两次做完）
//   ⑧ 部分覆盖影子没有新写就原样交回、宽屏副本已是这份影子就不再拷：32 KB 绑定部分更新后视图一次；(a) 不再写、再看：内容同、扣掉计数调用后 < 1000 条指令（再拷 32 KB 要 ≥ 2048 条）；
//      (b) 写落在别处后再取：新做影子 0 份、同一地址；(c) 同 (b) 经视图：0 份、< 1000 条；(d) 这一段有新写：照常续拼 1 份、内容 = 内存 + 两次更新；
//      (e) 副本被延后表那条路（ubd_read）写过之后再按影子看：重拷、内容同影子；(f) 分段提交、暂存块被再用（写 0xEE）后又有别处写：新做一份、内容对（不沿用旧块）
//   ⑨ 宽屏一遍（2346×1080 目标、64 笔整屏三角）：提交里 tex_mtl 每笔 < 4.5 次（颜色目标 0 的尺寸在放宽画法下只取一次；改前每笔 5.06 次）；读回整张存储的指纹打出来，改前编同一单测比
// 反证（11-01 第三刀）：f2e1b214c 编同一单测（tex_mtl 注入同一计数）：⑧(a)(b)(c) ⑨ 红，其余绿、⑨ 读回指纹与改后相同；7f4effbe8 编：⑨ 红。
// 反证：559720a8a 编同一单测：⑥（Metal 3、编码线程关）⑦ 红。NVN_UBP_OLD=1 编 0a2ba6f5f 的实现（影子拷贝立即交拷贝线程）：⑤ 交给拷贝线程次数红。NVN_UBD_OLD=1 编改前的实现（tests 里的 ubd_add 换成改前 mtl_sub_update 里的那几行）：① ③ ④ 照绿（参照即改前语义），② 红（每次几千条指令），⑤ 交给拷贝线程次数红
#ifndef NVN_UBD_SOURCE
#define NVN_UBD_SOURCE "../gfx/nvn/nvn_mtl.m"
#endif
//   ⑫ 顶点 / 片元两阶段绑同一段（塞尔达主场景每笔如此）：一处新写后第一个阶段续拼一份影子、按更新段续拷（返回 3）；第二个阶段命中缓存里同一份影子，
//      也要拿到这份影子的来历（上一份 + 更新段）、按更新段续拷（返回 3，段表 [2048, 2064)），不再整段 32 KB 重拷、交核心整段比（改前返回 1）；新做影子只 1 份
#include NVN_UBD_SOURCE
#include "../gfx/nvn/nvn_protos.h"
#include <signal.h>
#include <unistd.h>

#ifdef NVN_UBD_OLD   // 改前：插入写在 mtl_sub_update 里（整段被盖的出表、追加、记页过滤）
static void ubd_add(uint64_t lo, uint64_t hi, uint64_t gpu, size_t off, id<MTLBuffer> buf) {
    int k = 0; for (int i = 0; i < g_nubd; i++) if (!(lo <= g_ubd[i].lo && g_ubd[i].hi <= hi)) g_ubd[k++] = g_ubd[i];
    g_nubd = k; g_ubd[g_nubd++] = (ubd_ent){lo, hi, gpu, off, buf}; ubd_pf_add(lo, hi);
}
#endif
extern int thread_selfcounts(int type, void *buf, size_t nbytes);
static uint64_t instr(void) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); return c[0]; }
static void on_alarm(int s) { (void)s; static const char m[] = "✗ t_nvn_ubd_index：5 秒没返回\n"; write(1, m, sizeof m - 1); _exit(1); }

typedef struct { uint64_t lo, hi, gpu; size_t off; } ref_ent;   // 参照：延后顺序
static ref_ent ref[UBD_MAX]; static int nref;
static uint8_t *ring; static id<MTLBuffer> ringbuf; static size_t ring_off;
static uint64_t rnd_s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 7; rnd_s ^= rnd_s << 17; return rnd_s; }
static unsigned bad;
#define CHECK(c, ...) do { if (!(c)) { if (bad++ < 12) { printf("✗ "); printf(__VA_ARGS__); putchar('\n'); } } } while (0)

static void add(uint64_t lo, uint64_t hi) {
    size_t off = ring_off; ring_off += (hi - lo + 255) & ~(size_t)255;
    for (uint64_t i = 0; i < hi - lo; i++) ring[off + i] = (uint8_t)(rnd() | 1);   // 每次写入的字节各不相同（非 0）
    int k = 0; for (int i = 0; i < nref; i++) if (!(lo <= ref[i].lo && ref[i].hi <= hi)) ref[k++] = ref[i];
    nref = k; ref[nref++] = (ref_ent){lo, hi, 0x40000000ull + off, off};
    ubd_add(lo, hi, 0x40000000ull + off, off, ringbuf);
}
static void reset(void) { g_nubd = 0; ubd_flush(); nref = 0; ring_off = 0; }
static int ref_hits(uint64_t lo, uint64_t hi) { for (int i = 0; i < nref; i++) if (lo < ref[i].hi && ref[i].lo < hi) return 1; return 0; }
// 参照按定义逐字节判 / 取，只是先和查询区间求交、再在交集里逐字节标记 / 拷贝（11-14：原先每个查询字节扫一遍全部条目、每条条目扫它全部字节，
//   单测四遍共约 30 秒几乎全花在参照上；结果逐字节相同：盖满 = 交集并起来覆盖 [lo, hi) 每个字节，读出 = 按插入次序后写覆盖先写）
static int ref_covers(uint64_t lo, uint64_t hi) {
    uint8_t m[2048] = {0}; if (hi - lo > sizeof m) return 0;
    for (int i = 0; i < nref; i++) { uint64_t a = ref[i].lo > lo ? ref[i].lo : lo, b = ref[i].hi < hi ? ref[i].hi : hi; if (a < b) memset(m + (a - lo), 1, b - a); }
    for (uint64_t x = 0; x < hi - lo; x++) if (!m[x]) return 0;   // 逐字节
    return 1;
}
static void ref_read(uint64_t lo, uint64_t hi, uint8_t *d) {
    for (int i = 0; i < nref; i++) { uint64_t a = ref[i].lo > lo ? ref[i].lo : lo, b = ref[i].hi < hi ? ref[i].hi : hi; if (a < b) memcpy(d + (a - lo), ring + ref[i].off + (a - ref[i].lo), b - a); }
}


// ④ 真录真画
extern void *h_nvnBootstrapLoader(const char *name);
static void *api(const char *name) { void *p = h_nvnBootstrapLoader(name); if (!p) { printf("✗ 取不到 %s\n", name); exit(1); } return p; }
#define API(name) ((NVNT_##name)api(#name))
static NSString *const e2e_src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"vertex float4 vs(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
"struct A { constant float4 *c0 [[id(0)]]; constant float4 *c1 [[id(1)]]; constant float4 *c2 [[id(2)]]; constant float4 *c3 [[id(3)]]; };\n"
"fragment float4 fs(constant A &a [[buffer(0)]]) { return a.c3[0]; }\n";
static uint64_t g_e9_hash, g_e9_tex; static double g_e9_per;   // ⑨：宽屏一遍的读回指纹、提交里 tex_mtl 次数、每笔次数
static void e2e(int bs, int busy, int wide) {
    enum { POOL = 32 << 20, CMD = 1 << 20, RT = 8 << 20, UB = 16 << 20, W = 64 };
    static int inited; static NvShader *f; static NVNbufferAddress ga; static NVNtexture rt, rtw; static prog_t p;
    static NVNdevice dev; static NVNqueue q; static NVNmemoryPool pool; static NVNcommandBuffer cmd; static uint8_t control[16384];
    if (!inited) { inited = 1;   // 设备、池、目标、程序只建一次
    char map[] = "/tmp/t_ubdi_map_XXXXXX", user[] = "/tmp/t_ubdi_user_XXXXXX"; int fd = mkstemp(map); close(fd); fd = mkstemp(user); close(fd);
    setenv("DM_NVN_SHMAP", map, 1); setenv("DM_PSO_USER", user, 1); setenv("DM_PSO_ARCHIVE", "0", 1);
    NVNdeviceBuilder db; API(nvnDeviceBuilderSetDefaults)(&db); if (!API(nvnDeviceInitialize)(&dev, &db)) { printf("✗ ④ 设备\n"); exit(1); }
    NVNqueueBuilder qb; API(nvnQueueBuilderSetDefaults)(&qb); API(nvnQueueBuilderSetDevice)(&qb, &dev); API(nvnQueueInitialize)(&q, &qb); mtl_init(); g_wd = 0;
    uint8_t *mem = NULL; if (posix_memalign((void **)&mem, 65536, POOL)) exit(1); memset(mem, 0, POOL);
    NVNmemoryPoolBuilder pb; API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &dev);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, mem, POOL); API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolInitialize)(&pool, &pb); API(nvnCommandBufferInitialize)(&cmd, &dev); API(nvnCommandBufferAddControlMemory)(&cmd, control, sizeof control);
    NVNtextureBuilder tb; API(nvnTextureBuilderSetDefaults)(&tb); API(nvnTextureBuilderSetDevice)(&tb, &dev); API(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    API(nvnTextureBuilderSetSize2D)(&tb, W, 1); API(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); API(nvnTextureBuilderSetLevels)(&tb, 1);
    API(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_COMPRESSIBLE); API(nvnTextureBuilderSetStorage)(&tb, &pool, RT);
    API(nvnTextureInitialize)(&rt, &tb);
    API(nvnTextureBuilderSetSize2D)(&tb, 2346, 1080); API(nvnTextureBuilderSetStorage)(&tb, &pool, 20 << 20); API(nvnTextureInitialize)(&rtw, &tb);   // ⑨ 宽屏宽的目标（同微基准 SC_RTW=2346）
    NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:e2e_src options:nil error:&err]; if (!lib) { printf("✗ ④ 着色器：%s\n", err.localizedDescription.UTF8String); exit(1); }
    NvProg *pr = [NvProg new]; NvShader *v = [NvShader new]; f = [NvShader new];
    v->stage = 0; v->fn = [lib newFunctionWithName:@"vs"]; strlcpy(v->key, "e2e0000000000000_b", sizeof v->key); v->rf.pos_attr = -1; v->rf.has_user = 1;
    f->stage = 1; f->fn = [lib newFunctionWithName:@"fs"]; strlcpy(f->key, "e2e0000000000001_b", sizeof f->key); f->rf.pos_attr = -1; f->rf.has_user = 2; f->rf.color_known = 1; f->rf.color_mask = 1;
    f->rf.nb = 4; f->rf.b = calloc(4, sizeof *f->rf.b); f->arg_len = 32;
    for (int r = 0; r < 4; r++) { f->rf.b[r].kind = NVN_RB_CBUF; f->rf.b[r].reg = (uint16_t)r; f->rf.b[r].aid = (uint16_t)r; }
    pr->sh[0] = v; pr->sh[1] = f; pr->smask = 3;
    p = (prog_t){.magic = MAGIC('P', 'R', 'O', 'G'), .serial = nvn_new_serial()}; serial_put(p.serial, pr);
    ga = API(nvnMemoryPoolGetBufferAddress)(&pool); }
    for (int r = 0; r < 4; r++) f->cb_ext[r] = (uint32_t)bs;
    API(nvnCommandBufferAddCommandMemory)(&cmd, &pool, 0, CMD); API(nvnCommandBufferBeginRecording)(&cmd);
    if (wide) { g_wd = s2a_widen_init(2346, 1080, S2A_WIDEN_GAME | S2A_WIDEN_FULL); g_wd_gen = s2a_screen_gen(); }
    const NVNtexture *colors[] = {wide ? &rtw : &rt}; const float zero[4] = {0, 0, 0, 0};
    API(nvnCommandBufferSetRenderTargets)(&cmd, 1, colors, NULL, NULL, NULL); API(nvnCommandBufferClearColor)(&cmd, 0, zero, 15);
    API(nvnCommandBufferBindProgram)(&cmd, (const NVNprogram *)&p, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
    for (int r = 0; r < 3; r++) API(nvnCommandBufferBindUniformBuffer)(&cmd, NVN_SHADER_STAGE_FRAGMENT, r, ga + UB + 0x100000, (size_t)bs);
    for (int i = 0; i < W; i++) {
        static float blk[4096]; blk[0] = (4 * i) / 255.0f; blk[1] = (255 - 4 * i) / 255.0f; blk[2] = i / 255.0f; blk[3] = 1.0f;
        for (int k = 4; k < bs / 4; k++) blk[k] = 0.25f;   // 整块写时后面的字节（着色器不读）
        NVNbufferAddress obj = ga + UB + (wide == 2 ? 0 : (NVNbufferAddress)(i % 3) * (NVNbufferAddress)bs);   // wide 2：每笔都写同一块的块头、绑同一处（宽屏胶水按更新段交核心那条路）
        API(nvnCommandBufferUpdateUniformBuffer)(&cmd, obj, (size_t)bs, 0, (i & 1) && !busy ? (size_t)bs : 16, blk);   // ⑥ 只写块头：每笔都走部分覆盖影子
        API(nvnCommandBufferBindUniformBuffer)(&cmd, NVN_SHADER_STAGE_FRAGMENT, 3, obj, (size_t)bs);
        if (!wide) API(nvnCommandBufferSetScissor)(&cmd, i, 0, 1, 1);
        API(nvnCommandBufferDrawArrays)(&cmd, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    }
    NVNcommandHandle h = API(nvnCommandBufferEndRecording)(&cmd);
    static uint8_t *bigs, *bigd; enum { BIG = 256 << 20 };
    if (busy) {   // ⑥ 拷贝线程先忙一大段（256 MB）：这一遍 ≥ 8 KB 影子的拷贝排在它后面，提交不等就会让 GPU 读到还没写的影子
        if (!bigs) { bigs = malloc(BIG); bigd = malloc(BIG); memset(bigs, 1, BIG); memset(bigd, 0, BIG); }
        ubc_push(bigd, bigs, BIG); }
    uint64_t d0 = g_ubd_deferred, m0 = g_ubp_made, w0 = g_ubp_why[0] + g_ubp_why[1] + g_ubp_why[2] + g_ubp_why[3] + g_ubp_why[4]; uint64_t x0 = g_tex_mtl_n, dr0 = g_wd_draws; API(nvnQueueSubmitCommands)(&q, 1, &h); API(nvnQueueFinish)(&q);
    CHECK(!mtl_last_commit_error(), "④ GPU 报错");
    if (wide) {   // ⑨ 读回整张放宽后的存储做指纹（改前 / 改后各编一份比）；提交里 tex_mtl 的次数
        g_e9_tex = g_tex_mtl_n - x0; g_e9_per = (double)g_e9_tex / (double)(g_wd_draws - dr0 ? g_wd_draws - dr0 : 1);
        id<MTLTexture> t = tex_mtl((const tex_t *)&rtw); NSUInteger tw = t.width, th = t.height;
        id<MTLBuffer> out = [g_dev newBufferWithLength:tw * th * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> cq = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [cq commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(tw, th, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:tw * 4 destinationBytesPerImage:tw * th * 4];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint8_t *px = out.contents; uint64_t hsh = 1469598103934665603ull; for (NSUInteger k = 0; k < tw * th * 4; k++) { hsh ^= px[k]; hsh *= 1099511628211ull; }
        g_e9_hash = hsh ^ ((uint64_t)tw << 32) ^ th; g_wd = 0; return; }
    id<MTLTexture> t = tex_mtl((const tex_t *)&rt); id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> cq = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [cq commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, 1, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *px = out.contents; int ok = 0;
    for (int i = 0; i < W; i++) { int good = px[4 * i] == 4 * i && px[4 * i + 1] == 255 - 4 * i && px[4 * i + 2] == i && px[4 * i + 3] == 255; ok += good;
        CHECK(good, "④ 第 %d 列 %u,%u,%u,%u（应 %d,%d,%d,255）", i, px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3], 4 * i, 255 - 4 * i, i); }
    printf("%s %s 真录真画（常量块 %d 字节，Metal %d，编码线程%s%s）：%d / %d 列 = 第 i 笔前写的颜色；这一遍延后的更新 %llu 条、做影子 %llu 份（拒 %llu）\n", ok == W ? "✓" : "✗", busy ? "⑥" : "④", bs, g_m3 ? 3 : 4, rec_on() ? "开" : "关", busy ? "，拷贝线程先忙 256 MB" : "", ok, W, (unsigned long long)(g_ubd_deferred - d0), (unsigned long long)(g_ubp_made - m0), (unsigned long long)(g_ubp_why[0] + g_ubp_why[1] + g_ubp_why[2] + g_ubp_why[3] + g_ubp_why[4] - w0));
}

int main(void) { @autoreleasepool {
    signal(SIGALRM, on_alarm); alarm(60);
    e2e(256, 0, 0); e2e(16384, 0, 0); e2e(16384, 1, 0);
    unsigned bad9 = bad; e2e(256, 0, 1);   // ⑨ 也走整套提交：放在下面手动开提交（nvn_be_sub_begin）之前，判在后面
    // ⑩ 同一块每笔改块头、绑同一处：宽屏胶水按更新段 / 同影子跳过交核心（g_wd_vd 开）与每笔整段交（关）各跑一遍，读回整张存储逐字节同，且开的那遍真走了按更新段
    double per9 = g_e9_per; uint64_t hash9 = g_e9_hash, tex9 = g_e9_tex;   // ⑨ 的读数先存下（⑩ 的两遍会改写它们）
    uint64_t q0 = g_wd_view_ranged; g_wd_vd = 0; e2e(256, 0, 2); uint64_t h10off = g_e9_hash, q1 = g_wd_view_ranged;
    uint64_t vn0 = g_wd_view_n, vs0 = g_wd_view_skip, cd0 = g_wd_cbv_delta, cc0 = g_wd_cbv_copy, cs0 = g_wd_cbv_same, wc0 = g_wd_core;
    g_wd_vd = 1; e2e(256, 0, 2);
    printf("  ⑩ 读数：进核心 %llu 笔、按影子交 %llu 次（同影子跳过 %llu）、副本 整拷 %llu / 只拷更新段 %llu / 同一份 %llu\n", (unsigned long long)(g_wd_core - wc0), (unsigned long long)(g_wd_view_n - vn0),
           (unsigned long long)(g_wd_view_skip - vs0), (unsigned long long)(g_wd_cbv_copy - cc0), (unsigned long long)(g_wd_cbv_delta - cd0), (unsigned long long)(g_wd_cbv_same - cs0)); uint64_t h10on = g_e9_hash, q2 = g_wd_view_ranged; g_wd_vd = -1; g_e9_per = per9; g_e9_hash = hash9; g_e9_tex = tex9;
    unsigned bad4 = bad;
    nvn_be_sub_begin();
    ringbuf = [g_dev newBufferWithLength:64 << 20 options:MTLResourceStorageModeShared]; ring = ringbuf.contents;
    const uint64_t base = 0x80000000ull;
    // ①
    (void)bad4; unsigned queries = 0, inplace = 0, covered = 0, hits = 0;
    for (int it = 0; it < 4000; it++) {
        if (nref >= UBD_MAX - 1) reset();
        uint64_t lo = base + ((rnd() % (48 * 4096)) & ~15ull), n = 16 + ((rnd() % 1009) & ~15ull);
        add(lo, lo + n);
        CHECK(g_nubd == nref, "① 第 %d 次插入后 %d 条，参照 %d 条", it, g_nubd, nref);
        for (int i = 0; i < nref && i < g_nubd; i++) CHECK(g_ubd[i].lo == ref[i].lo && g_ubd[i].hi == ref[i].hi && g_ubd[i].off == ref[i].off, "① 第 %d 次插入后第 %d 条不同", it, i);
        for (int q = 0; q < 40; q++, queries++) {
            uint64_t ql = base - 512 + ((rnd() % (48 * 4096 + 1024)) & ~15ull), qn = 16 + ((rnd() % 600) & ~15ull), qh = ql + qn;
            int h = ref_hits(ql, qh); CHECK(ubd_hits(ql, qh) == h, "① 命中 [%#llx,+%llu) 实现 %d 参照 %d", (unsigned long long)ql, (unsigned long long)qn, !h, h);
            if (!h) continue;
            hits++;
            int c = ref_covers(ql, qh); CHECK(ubd_covers(ql, qh) == c, "① 盖满 [%#llx,+%llu) 参照 %d", (unsigned long long)ql, (unsigned long long)qn, c);
            uint8_t want[1024], got[1024]; memset(want, 0, qn); memset(got, 0, qn); ref_read(ql, qh, want); ubd_read(ql, (size_t)qn, got);
            CHECK(!memcmp(want, got, qn), "① 读出 [%#llx,+%llu) 字节与参照不同", (unsigned long long)ql, (unsigned long long)qn);
            if (!c) continue;
            covered++;
            int nw = -1; for (int i = nref - 1; i >= 0 && nw < 0; i--) if (ql < ref[i].hi && ref[i].lo < qh) nw = i;   // 相交的最新一条
            uint64_t ing = ref[nw].gpu + (ql - ref[nw].lo); int ip = ref[nw].lo <= ql && qh <= ref[nw].hi && !(ing & 15);
            uint64_t g = ubd_shadow(ql, qh); inplace += ip;
            CHECK((g == ing) == ip, "① 影子 [%#llx,+%llu) 原地 实现 %d 参照 %d", (unsigned long long)ql, (unsigned long long)qn, g == ing, ip);
        }
    }
    printf("%s ① 4000 次插入逐条同参照；%u 个查询（命中 %u、盖满 %u、其中影子原地 %u）命中 / 盖满 / 读出 / 原地同参照\n", bad ? "✗" : "✓", queries, hits, covered, inplace);
    // ②
    unsigned bad1 = bad; reset();
    for (int i = 0; i < UBD_MAX; i++) { uint64_t pg = base + (uint64_t)(i / 4) * 4096, lo = pg + (uint64_t)(i % 4) * 512; add(lo, lo + 256); }
    CHECK(g_nubd == UBD_MAX, "② 铺满 %d 条（应 %d）", g_nubd, UBD_MAX);
    uint64_t t0 = instr(); int sink = 0;
    for (int r = 0; r < 64; r++) for (int i = 0; i < 128; i++) { uint64_t lo = base + (uint64_t)i * 4096 + 3072 + 64; sink += ubd_hits(lo, lo + 256) + ubd_covers(lo, lo + 256); }
    double per = (double)(instr() - t0) / (64.0 * 128.0);
    CHECK(sink == 0, "② 同页不相交的查询不应命中（%d 次命中）", sink);
    CHECK(per < 400, "② 512 条下每次 ubd_hits + ubd_covers %.0f 条指令（判据 < 400；逐条扫 ≥ 1536）", per);
    printf("%s ② 512 条更新、同页不相交查询：每次 ubd_hits + ubd_covers %.0f 条指令（判据 < 400）\n", bad > bad1 ? "✗" : "✓", per);
    // ③
    unsigned bad2 = bad; reset(); alarm(5);
    for (int i = 0; i < UBD_MAX; i++) add(base + ((uint64_t)i << 12), base + ((uint64_t)i << 12) + 64);
    int hit = 0; for (int i = 0; i < UBD_MAX; i++) hit += ubd_hits(base + ((uint64_t)i << 12), base + ((uint64_t)i << 12) + 64);
    int miss = 0; for (int i = 0; i < UBD_MAX; i++) miss += ubd_hits(base + ((uint64_t)(i + UBD_MAX) << 12), base + ((uint64_t)(i + UBD_MAX) << 12) + 64);
    CHECK(hit == UBD_MAX && miss == 0, "③ %d 个不同页：命中 %d（应 %d）、没加过的命中 %d（应 0）", UBD_MAX, hit, UBD_MAX, miss);
    reset(); CHECK(!ubd_hits(base, base + 64) && g_nubd == 0, "③ 着陆后表应空");
    alarm(0);
    printf("%s ③ %d 个不同页不卡死、加过的全命中、没加过的不命中、着陆后表空\n", bad > bad2 ? "✗" : "✓", UBD_MAX);
    // ⑤
    unsigned bad5 = bad; reset();
    uint8_t *pm = NULL; if (posix_memalign((void **)&pm, 65536, 1 << 20)) exit(1);
    for (int i = 0; i < 1 << 20; i++) pm[i] = (uint8_t)(i * 7 + 3);
    nvn_be_pool_add(pm, 1 << 20);
    uint64_t B = (uint64_t)(uintptr_t)pm + 0x10000; uint8_t u1[16], u2[16]; memset(u1, 0xA5, 16); memset(u2, 0x5A, 16);
    static uint8_t want5[16384];
    mtl_sub_update(u1, B + 64, 16);
    uint64_t h0 = ubc_head(); int r1 = wd_cb_view(0, 3, B, 4096); uint64_t h1 = ubc_head();
    memcpy(want5, pm + 0x10000, 4096); memset(want5 + 64, 0xA5, 16);
    CHECK(r1 == 1 && !memcmp(g_wd_cbv[0][3], want5, 4096), "⑤ 第一次视图 4096 字节：返回 %d、内容%s同内存 + 更新", r1, r1 == 1 && !memcmp(g_wd_cbv[0][3], want5, 4096) ? "" : "不");
    CHECK(h1 == h0, "⑤ 视图的影子交给拷贝线程 %llu 次（应 0）", (unsigned long long)(h1 - h0));
    mtl_sub_update(u2, B + 1024, 16);
    int r2 = wd_cb_view(0, 3, B, 4096); uint64_t h2 = ubc_head(); memset(want5 + 1024, 0x5A, 16);
    CHECK(r2 == 3 && !memcmp(g_wd_cbv[0][3], want5, 4096), "⑤ 第二次视图（从上一份影子续拼）：返回 %d（应 3 = 只拷更新段）、内容%s同内存 + 两次更新", r2, !memcmp(g_wd_cbv[0][3], want5, 4096) ? "" : "不");
    CHECK(h2 == h1, "⑤ 第二次视图交给拷贝线程 %llu 次（应 0）", (unsigned long long)(h2 - h1));
#if !defined(NVN_UBD_OLD) && !defined(NVN_UBP_OLD)   // 改前没有 ubq_gpu
    {   // 先绑给 GPU（16 KB ≥ 8 KB：交拷贝线程），再在它上面续拼一份给 CPU 读：要等它的拷贝做完
        uint64_t C = B + 0x20000, g; const uint8_t *cc;
        mtl_sub_update(u1, C + 128, 16);
        int ok = ubp_get(C, C + 16384, &g, &cc); ubq_gpu(g_ubp_q); uint64_t h3 = ubc_head();
        CHECK(ok && h3 - h2 == 2, "⑤ 16 KB 影子绑给 GPU：交拷贝线程 %llu 次（应 2：底 + 一次更新）", (unsigned long long)(h3 - h2));
        mtl_sub_update(u2, C + 8192, 16);
        int r3 = wd_cb_view(0, 4, C, 16384);
        memcpy(want5, pm + 0x20000 + 0x10000, 16384); memset(want5 + 128, 0xA5, 16); memset(want5 + 8192, 0x5A, 16);
        CHECK(r3 == 1 && !memcmp(g_wd_cbv[0][4], want5, 16384), "⑤ 在交给拷贝线程的影子上续拼的视图内容%s同内存 + 两次更新", r3 == 1 && !memcmp(g_wd_cbv[0][4], want5, 16384) ? "" : "不");
    }
#endif
    reset();
    printf("%s ⑤ 宽屏视图读影子：4 KB 两次（第二次续拼）内容对、交给拷贝线程 0 次%s\n", bad > bad5 ? "✗" : "✓", 
#if !defined(NVN_UBD_OLD) && !defined(NVN_UBP_OLD)   // 改前没有 ubq_gpu
           "；16 KB 先给 GPU（交 2 次）再续拼给 CPU 内容对"
#else
           ""
#endif
           );
    // ⑦
    {   unsigned bad7 = bad; reset();
        uint64_t B2 = (uint64_t)(uintptr_t)pm + 0x40000, g1, g2; const uint8_t *c1, *c2;
        mtl_sub_update(u1, B2 + 64, 16);
        int ok1 = ubp_get(B2, B2 + 4096, &g1, &c1); ubq_cpu(g_ubp_q);
        memcpy(want5, pm + 0x40000, 4096); memset(want5 + 64, 0xA5, 16);
        CHECK(ok1 && !memcmp(c1, want5, 4096), "⑦ 分段前的影子内容%s对", ok1 && !memcmp(c1, want5, 4096) ? "" : "不");
        uint64_t sf = arena_cut(); arena_seg_done(sf);   // 这一段交给 GPU、GPU 做完：它的暂存块可以再用
        int hit = 0;
        for (int k = 0; k < 64 && !hit; k++) { id<MTLBuffer> b; size_t o = arena_alloc(4u << 20, &b); uint8_t *a = (uint8_t *)b.contents;
            if (c1 >= a && c1 < a + b.length) { memset(a, 0xEE, b.length); hit = 1; } else memset(a + o, 0, 4u << 20); }
        CHECK(hit, "⑦ 夹具：64 次分配内没拿回影子所在的暂存块（夹具没造出「块被再用」）");
        int ok2 = ubp_get(B2, B2 + 4096, &g2, &c2); ubq_cpu(g_ubp_q);
        CHECK(ok2 && !memcmp(c2, want5, 4096), "⑦ 分段后再取同一段影子：内容%s对（%s）", ok2 && !memcmp(c2, want5, 4096) ? "" : "不", c2 == c1 ? "交回了已被再用的旧块地址" : "新做了一份");
        reset();
        printf("%s ⑦ 分段提交后暂存块被再用：同一段影子不再交回旧地址、内容 = 内存 + 更新\n", bad > bad7 ? "✗" : "✓"); }
    // ⑧
    {   unsigned bad8 = bad; reset();
        enum { N8 = 32768 }; static uint8_t want8[N8], old8[N8];
        uint64_t D = (uint64_t)(uintptr_t)pm + 0x60000, E = (uint64_t)(uintptr_t)pm + 0xA0000, g, g2; const uint8_t *c, *c2;
        mtl_sub_update(u1, D + 64, 16);
        memcpy(want8, pm + 0x60000, N8); memset(want8 + 64, 0xA5, 16);
        int r = wd_cb_view(0, 5, D, N8);
        CHECK(r == 1 && !memcmp(g_wd_cbv[0][5], want8, N8), "⑧ 第一次视图 %d 字节：返回 %d、内容%s同内存 + 更新", N8, r, r == 1 && !memcmp(g_wd_cbv[0][5], want8, N8) ? "" : "不");
        // (a) 两次视图之间没有任何写：影子是缓存里同一份，副本里已是它。指令数扣掉一对 instr() 本身（取最小的一次）
        uint64_t ov = ~0ull; for (int k = 0; k < 16; k++) { uint64_t x = instr(), y = instr(); if (y - x < ov) ov = y - x; }
        // 指令数取 8 次里最小的一次（11-10 负载 35–38 时单次读到 1354：线程被抢占 / 缺页的内核指令也记在本线程上；
        //   无写再看是幂等的，8 次都应返回 2；真去整段再拷的话每一次都 ≥ 2048 条，取最小照样拦得住）
        uint64_t i0, ia = ~0ull; int r2 = 2;
        for (int k = 0; k < 8; k++) { i0 = instr(); r = wd_cb_view(0, 5, D, N8); uint64_t x = instr() - i0 - ov; if (x < ia) ia = x; if (r != 2) r2 = r; }
        r = r2;
        CHECK(r == 2 && !memcmp(g_wd_cbv[0][5], want8, N8), "⑧(a) 无写再看：返回 %d（应 2 = 副本已是这份影子、内容与上次交出逐字节同）、内容%s同", r, !memcmp(g_wd_cbv[0][5], want8, N8) ? "" : "不");
        CHECK(ia < 1000, "⑧(a) 无写再看 %llu 条指令（判据 < 1000：副本已是这份影子不再拷；整段 32 KB 再拷一遍 ≥ 2048 条）", (unsigned long long)ia);
        // (b) 写落在别处（E）：这一段自上一份影子后没有新写，上一份就是答案（同一地址、不另做一份）
        int ok1 = ubp_get(D, D + N8, &g, &c); ubq_cpu(g_ubp_q); uint64_t m0 = g_ubp_made;
        mtl_sub_update(u2, E, 16);
        int ok2 = ubp_get(D, D + N8, &g2, &c2); ubq_cpu(g_ubp_q); uint64_t mb = g_ubp_made - m0;
        CHECK(ok1 && ok2 && !memcmp(c2, want8, N8), "⑧(b) 别处写后再取：内容%s同内存 + 更新", ok1 && ok2 && !memcmp(c2, want8, N8) ? "" : "不");
        CHECK(mb == 0 && c2 == c && g2 == g, "⑧(b) 别处写后再取：新做影子 %llu 份（应 0）、%s", (unsigned long long)mb, c2 == c ? "同一份" : "换了一份（整段重拷）");
        // (c) 同 (b) 但经宽屏视图：既不另做影子、也不再拷进副本
        mtl_sub_update(u2, E + 4096, 16);
        m0 = g_ubp_made; uint64_t ic = ~0ull; r2 = 2;   // 同 (a)：取 8 次里最小的指令数，每次都应返回 2、不新做影子
        for (int k = 0; k < 8; k++) { i0 = instr(); r = wd_cb_view(0, 5, D, N8); uint64_t x = instr() - i0 - ov; if (x < ic) ic = x; if (r != 2) r2 = r; }
        r = r2;
        CHECK(r == 2 && !memcmp(g_wd_cbv[0][5], want8, N8) && g_ubp_made == m0 && ic < 1000, "⑧(c) 别处写后视图：返回 %d（应 2）、内容%s同、新做影子 %llu 份（应 0）、%llu 条指令（判据 < 1000）",
              r, !memcmp(g_wd_cbv[0][5], want8, N8) ? "" : "不", (unsigned long long)(g_ubp_made - m0), (unsigned long long)ic);
        // (d) 这一段有新写：照常续拼一份，副本跟着换
        mtl_sub_update(u2, D + 2048, 16); memset(want8 + 2048, 0x5A, 16);
        m0 = g_ubp_made; r = wd_cb_view(0, 5, D, N8);
        CHECK(r == 3 && !memcmp(g_wd_cbv[0][5], want8, N8) && g_ubp_made == m0 + 1, "⑧(d) 这一段新写后视图：返回 %d（应 3 = 续拼自副本里那份、只拷更新段）、内容%s同内存 + 两次更新、新做影子 %llu 份（应 1）",
              r, !memcmp(g_wd_cbv[0][5], want8, N8) ? "" : "不", (unsigned long long)(g_ubp_made - m0));
        // (d') 交核心的更新段 = 这次新写的那一段 [2048, 2064)（手写期望：只有它落在这一段里）
        CHECK(g_wd_dn == 1 && g_wd_dr[0] == 2048 && g_wd_dr[1] == 2064, "⑧(d') 更新段 %d 段、首段 [%u, %u)（应 1 段 [2048, 2064)）", g_wd_dn, g_wd_dr[0], g_wd_dr[1]);
        // (e) 副本被延后表那条路写过（整段被本遍延后的更新盖住 = ubd_read）之后，再按影子看同一份：要重拷，不能当它还是那份影子
        memcpy(old8, want8, N8);
        add(D, D + N8); r = wd_cb_view(0, 5, D, N8);
        static uint8_t dfr[N8]; ref_read(D, D + N8, dfr);
        CHECK(r == 1 && !memcmp(g_wd_cbv[0][5], dfr, N8), "⑧(e) 延后更新盖满时视图 = 延后内容%s", r == 1 && !memcmp(g_wd_cbv[0][5], dfr, N8) ? "" : "不对");
        g_nubd = 0; ubd_flush(); nref = 0;
        r = wd_cb_view(0, 5, D, N8);
        CHECK(r == 1 && !memcmp(g_wd_cbv[0][5], old8, N8), "⑧(e) 延后表清掉后再按影子看：内容%s同影子", r == 1 && !memcmp(g_wd_cbv[0][5], old8, N8) ? "" : "不");
        // (f) 分段提交、暂存块被再用（cdad08ca9 ⑦）后又有别处写：不能沿用旧块里的那份
        int ok3 = ubp_get(D, D + N8, &g, &c); ubq_cpu(g_ubp_q);
        uint64_t sf = arena_cut(); arena_seg_done(sf);
        int hit = 0;
        for (int k = 0; k < 64 && !hit; k++) { id<MTLBuffer> b; size_t o = arena_alloc(4u << 20, &b); uint8_t *a = (uint8_t *)b.contents;
            if (c >= a && c < a + b.length) { memset(a, 0xEE, b.length); hit = 1; } else memset(a + o, 0, 4u << 20); }
        CHECK(hit, "⑧(f) 夹具：64 次分配内没拿回影子所在的暂存块");
        mtl_sub_update(u1, E + 8192, 16);
        int ok4 = ubp_get(D, D + N8, &g2, &c2); ubq_cpu(g_ubp_q);
        CHECK(ok3 && ok4 && !memcmp(c2, old8, N8) && c2 != c, "⑧(f) 暂存块再用 + 别处写后再取：内容%s同内存 + 两次更新（%s）", ok3 && ok4 && !memcmp(c2, old8, N8) ? "" : "不",
              c2 == c ? "交回了已被再用的旧块" : "新做了一份");
        r = wd_cb_view(0, 5, D, N8);
        CHECK(r == 1 && !memcmp(g_wd_cbv[0][5], old8, N8), "⑧(f) 之后视图内容%s同", r == 1 && !memcmp(g_wd_cbv[0][5], old8, N8) ? "" : "不");
        reset();
        printf("%s ⑧ 无新写的影子原样交回、副本已是这份不再拷：无写再看 %llu 条、别处写后视图 %llu 条指令（判据 < 1000）、别处写后新做影子 %llu 份；新写 / 延后表写过副本 / 暂存块再用三种都照常重做、内容对\n",
               bad > bad8 ? "✗" : "✓", (unsigned long long)ia, (unsigned long long)ic, (unsigned long long)mb); }
    // ⑫
    {   unsigned bad12 = bad; reset();
        enum { N12 = 32768 }; static uint8_t want12[N12];
        uint64_t D = (uint64_t)(uintptr_t)pm + 0x60000;
        mtl_sub_update(u1, D + 64, 16);
        memcpy(want12, pm + 0x60000, N12); memset(want12 + 64, 0xA5, 16);
        int r0 = wd_cb_view(0, 6, D, N12), r1 = wd_cb_view(1, 6, D, N12);
        CHECK(r0 == 1 && r1 == 1, "⑫ 两阶段第一次视图：顶点 %d、片元 %d（各自副本第一次装这一段，应都是 1）", r0, r1);
        mtl_sub_update(u2, D + 2048, 16); memset(want12 + 2048, 0x5A, 16);
        uint64_t m0 = g_ubp_made;
        r0 = wd_cb_view(0, 6, D, N12);
        CHECK(r0 == 3 && !memcmp(g_wd_cbv[0][6], want12, N12) && g_wd_dn == 1 && g_wd_dr[0] == 2048 && g_wd_dr[1] == 2064,
              "⑫ 新写后顶点阶段：返回 %d（应 3）、内容%s同、更新段 %d 段 [%u, %u)（应 1 段 [2048, 2064)）", r0, !memcmp(g_wd_cbv[0][6], want12, N12) ? "" : "不", g_wd_dn, g_wd_dr[0], g_wd_dr[1]);
        g_wd_dn = 0; g_wd_dr[0] = g_wd_dr[1] = 0;
        r1 = wd_cb_view(1, 6, D, N12);
        CHECK(r1 == 3 && !memcmp(g_wd_cbv[1][6], want12, N12) && g_wd_dn == 1 && g_wd_dr[0] == 2048 && g_wd_dr[1] == 2064 && g_ubp_made == m0 + 1,
              "⑫ 同一笔片元阶段（缓存里同一份影子）：返回 %d（应 3 = 也按更新段续拼；改前 1 = 整段 32 KB 重拷、交核心整段比）、内容%s同、更新段 %d 段 [%u, %u)、新做影子 %llu 份（应 1：两阶段共用）",
              r1, !memcmp(g_wd_cbv[1][6], want12, N12) ? "" : "不", g_wd_dn, g_wd_dr[0], g_wd_dr[1], (unsigned long long)(g_ubp_made - m0));
        reset();
        printf("%s ⑫ 顶点 / 片元绑同一段：第二个阶段命中缓存的同一份影子也按更新段续拼\n", bad > bad12 ? "✗" : "✓"); }
    // ⑪ 同一槽在同一代延后表里换绑别处（11-02 丝之歌跳台后主画面消失：reg1 在每物体矩阵块与另一块之间换绑）：副本按「阶段 × 槽」各一份，
    //   换了段就不能走「只叠序号 ≥ 上次的更新」的增量，要整段重拼；每次视图内容都要等于这一段自己的延后内容（手写参照 ref_read，不经被测代码）
    {   unsigned b11 = bad; reset(); enum { NA = 64 }; const uint64_t A = 0x10000000ull, B = 0x10002000ull; static uint8_t w[NA];
        add(A, A + NA); add(B, B + NA);
        int r = wd_cb_view(1, 7, A, NA); ref_read(A, A + NA, w);
        CHECK(r == 1 && !memcmp(g_wd_cbv[1][7], w, NA), "⑪ 先看 A：返回 %d（应 1）、内容%s同 A", r, !memcmp(g_wd_cbv[1][7], w, NA) ? "" : "不");
        r = wd_cb_view(1, 7, B, NA); ref_read(B, B + NA, w);
        CHECK(r == 1 && !memcmp(g_wd_cbv[1][7], w, NA), "⑪ 同一槽换看 B：返回 %d（应 1 = 整段重拼；改前回 2、副本还是 A）、内容%s同 B", r, !memcmp(g_wd_cbv[1][7], w, NA) ? "" : "不");
        add(A + 16, A + 32); r = wd_cb_view(1, 7, A, NA); ref_read(A, A + NA, w);
        CHECK(r == 1 && !memcmp(g_wd_cbv[1][7], w, NA), "⑪ A 新写后换回看 A：返回 %d（应 1）、内容%s同 A（改前把新写叠在 B 的副本上）", r, !memcmp(g_wd_cbv[1][7], w, NA) ? "" : "不");
        add(A + 32, A + 48); r = wd_cb_view(1, 7, A, NA); ref_read(A, A + NA, w);
        CHECK(r == 3 && !memcmp(g_wd_cbv[1][7], w, NA) && g_wd_dn == 1 && g_wd_dr[0] == 32 && g_wd_dr[1] == 48, "⑪ 同一段再写再看：返回 %d（应 3 = 增量照走）、内容%s同、更新段 %d 段 [%u, %u)（应 1 段 [32, 48)）",
              r, !memcmp(g_wd_cbv[1][7], w, NA) ? "" : "不", g_wd_dn, g_wd_dr[0], g_wd_dr[1]);
        reset();
        printf("%s ⑪ 同一槽同一代延后表换绑别处：整段重拼、内容逐字节同各段参照，同一段增量照走\n", bad > b11 ? "✗" : "✓"); }
    // ⑨
    {
        CHECK(h10on == h10off && q1 == q0 && q2 - q1 > 0, "⑩ 按更新段交核心：读回指纹 开 %016llx / 关 %016llx（应同）、按更新段交 关 %llu 次（应 0）/ 开 %llu 次（应 > 0）",
              (unsigned long long)h10on, (unsigned long long)h10off, (unsigned long long)(q1 - q0), (unsigned long long)(q2 - q1));
        CHECK(g_e9_per < 4.5, "⑨ 宽屏一遍每笔 tex_mtl %.2f 次（判据 < 4.5：颜色目标 0 的尺寸在放宽画法下取一次；改前每笔多取一次、约 5.06）", g_e9_per);
        printf("%s ⑨ 宽屏一遍（2346×1080 目标、64 笔）：每笔 tex_mtl %.2f 次（共 %llu）、读回整张存储指纹 %016llx（改前同一单测编出应同）\n", bad > bad9 ? "✗" : "✓", g_e9_per,
               (unsigned long long)g_e9_tex, (unsigned long long)g_e9_hash); }
    {   // ⑬ #640：一次提交里同一页排了很多条内联更新时，每次查常量块都把整条链走完（每次绘制一遍，链越积越长，一帧数秒）：超过 PEND_SINCE_MAX 回 -1（不做影子、结束渲染遍），没超照常全收。
        //    期望按语义手写：100 条全在 [P, P+256) 里 → 回 100；20000 条（> 8192）→ 回 -1（修前回 20000），查一次 < 5 ms
        int bad11 = bad; uint64_t P = 0x7f0000100000ull;
        pend_reset(); for (int i = 0; i < 100; i++) pend_add(P + (uint64_t)(i % 16) * 16, 16, 1);
        int n100 = pend_since(P, P + 256, 0, g_ubp_idx);
        CHECK(n100 == 100, "⑬ 同页 100 条：pend_since 回 %d（应 100）", n100);
        pend_reset(); for (int i = 0; i < 20000; i++) pend_add(P + (uint64_t)(i % 16) * 16, 16, 1);
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); int nbig = pend_since(P, P + 256, 0, g_ubp_idx); uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        CHECK(nbig == -1 && dt < 5000000ull, "⑬ 同页 20000 条：pend_since 回 %d（应 -1）、用时 %.3f ms（应 < 5 ms）", nbig, dt / 1e6);
        pend_reset();
        printf("%s ⑬ 同页 100 条回 %d、20000 条回 %d（%.3f ms）\n", bad > bad11 ? "✗" : "✓", n100, nbig, dt / 1e6); }
    {   // ⑭ #41 补充根修与负对照：
        //   (1) 在途提交的内联常量写（ifw_put）不拒绝未命中缓存的部分影子 ubp_get（由 ifw_get 叠回），但 CPU 读仍通过 gw_pending_fence 等它，非 ifw 写（gw_put）仍拒绝 ubp_get；
        //   (2) > 256 KB（SBW_PAGES = 64）的大存储写进 g_sbw_wide / g_gw_wide，20 段 4 MB（合计 20,480 页 > SBW_N）不溢出 g_sbw_over / g_gw_all，
        //       相同大区间的重复登记命中 g_sbw_memo、无关区间 ubp_get 照过，与宽区间重叠的查询在提交前后分别按 sbw / gw 拒绝
        unsigned bad14 = bad; reset();
        memset(g_gw, 0, sizeof g_gw); memset(g_gw_wide, 0, sizeof g_gw_wide); g_gw_all = 0; g_done = 10;
        uint64_t F = (uint64_t)(uintptr_t)pm + 0x20000, g14 = 0; const uint8_t *c14 = NULL;
        uint8_t patch16[16]; for (int i = 0; i < 16; i++) patch16[i] = (uint8_t)(0x80 + i);
        CHECK(ifw_put(F + 16, F + 32, patch16, 15, g_done), "⑭(1) 夹具 ifw_put 应成功");
        uint64_t why2_0 = g_ubp_why[2];
        int ok_ifw = ubp_get(F, F + 256, &g14, &c14); if (ok_ifw) ubq_cpu(g_ubp_q);
        CHECK(ok_ifw == 1 && g_ubp_why[2] == why2_0 && !memcmp(c14 + 16, patch16, 16),
              "⑭(1) 在途 ifw_put 不拒未缓存 ubp_get 且叠回内容对（ok=%d why2 +%llu）", ok_ifw, (unsigned long long)(g_ubp_why[2] - why2_0));
        uint64_t f_cpu = gw_pending_fence(F, F + 256, NULL);
        CHECK(f_cpu == 15, "⑭(1a 负对照) CPU 读仍见在途 ifw 栅栏：%llu（应 15）", (unsigned long long)f_cpu);
        gw_put(F + 0x1000, F + 0x1040, 15, g_done); why2_0 = g_ubp_why[2];
        int ok_nonifw = ubp_get(F + 0x1000, F + 0x1100, &g14, &c14);
        CHECK(ok_nonifw == 0 && g_ubp_why[2] == why2_0 + 1,
              "⑭(1b 负对照) 在途非 ifw 写（gw_put）仍拒 ubp_get（ok=%d why2 +%llu）", ok_nonifw, (unsigned long long)(g_ubp_why[2] - why2_0));

        pend_reset(); memset(g_gw, 0, sizeof g_gw); memset(g_gw_wide, 0, sizeof g_gw_wide); g_gw_all = 0; g_done = 20;
        uint64_t W_hit = (uint64_t)(uintptr_t)pm + 0x80000;   // 512 KB（128 页 > SBW_PAGES=64），落在 pm 内便于做重叠负对照
        sbw_note(W_hit, 512u << 10, SBW_GPU);
        for (int i = 0; i < 20; i++) sbw_note(0x500000000ull + (uint64_t)i * (4u << 20), 4u << 20, SBW_GPU);
        uint32_t sg0 = g_sbw_gen;
        sbw_note(W_hit, 512u << 10, SBW_GPU);
        for (int i = 0; i < 20; i++) sbw_note(0x500000000ull + (uint64_t)i * (4u << 20), 4u << 20, SBW_GPU);
        CHECK(g_sbw_over == 0 && g_nsbw == 0 && g_nsbw_wide == 21 && g_sbw_gen == sg0,
              "⑭(2) 20×4MB+512KB（>SBW_N 页）进 g_sbw_wide 不溢出且重复登记命中 memo（over=%d nsbw=%d wide=%d gen差=%u）",
              g_sbw_over, g_nsbw, g_nsbw_wide, g_sbw_gen - sg0);
        int ok_unrel = ubp_get(F, F + 256, &g14, &c14);
        uint64_t why1_0 = g_ubp_why[1];
        int ok_ov_sbw = ubp_get(W_hit, W_hit + 256, &g14, &c14);
        CHECK(ok_unrel == 1 && ok_ov_sbw == 0 && g_ubp_why[1] == why1_0 + 1,
              "⑭(2 负对照-提交前) 无关 ubp_get 过(%d)，重叠 ubp_get 按 sbw 拒(%d why1 +%llu)", ok_unrel, ok_ov_sbw, (unsigned long long)(g_ubp_why[1] - why1_0));
        gw_commit(25);
        CHECK(g_gw_all <= g_done, "⑭(2) gw_commit 后不触发 g_gw_all（g_gw_all=%llu g_done=%llu）", (unsigned long long)g_gw_all, (unsigned long long)g_done);
        pend_reset();   // 新一次提交、旧 ubp 缓存失效（g_pend_gen 已推进），fence 25 仍在途
        int ok_unrel2 = ubp_get(F, F + 256, &g14, &c14);
        why2_0 = g_ubp_why[2];
        int ok_ov_gw = ubp_get(W_hit, W_hit + 256, &g14, &c14);
        CHECK(ok_unrel2 == 1 && ok_ov_gw == 0 && g_ubp_why[2] == why2_0 + 1,
              "⑭(2 负对照-提交后) 在途 fence 25 下无关 ubp_get 仍过(%d)，重叠 ubp_get 按 gw_wide 拒(%d why2 +%llu)", ok_unrel2, ok_ov_gw, (unsigned long long)(g_ubp_why[2] - why2_0));

        memset(g_gw, 0, sizeof g_gw); memset(g_gw_wide, 0, sizeof g_gw_wide); g_gw_all = 0; g_done = 0; reset();
        printf("%s ⑭ 在途 ifw_put 叠回不拒、>256KB 大写进 wide 表不溢 g_gw_all（正/负对照全过）\n", bad > bad14 ? "✗" : "✓"); }
    printf("%s t_nvn_ubd_index：%s\n", bad ? "✗" : "✓", bad ? "有不符" : "延后表查找同参照、512 条下单次查找低于 400 条指令");
    return bad != 0;
} }
