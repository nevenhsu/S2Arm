// 原始全局内存池表（反射种类 gmem_map / gmem_log → NVN_RB_GMEM / GMEML，nvn_mtl.m gmem_map / gmem_scan / gml_take，hle_nvn.c pool_mark_addressed）：
//   着色器从内存里读出一个地址再访问，翻译器追不到具体存储缓冲时按「宿主池表」查池（GlobalToStorage.TryGenerateRawStorageOp，上游参考实现（MIT））。
//   11-06 前宿主没有这张表：反射把哨兵槽 0xff 当统一缓冲槽 252、宿主读不到 → 绑哑缓冲 → 这类读全是 0、写全丢（时之笛 2,206 支着色器）。
//   11-06 晚 按需常驻：表里每个池都常驻时 iPhone 常驻超内核上限（时之笛命令缓冲被拒 5392 次、35.8 → 23.8 帧），iOS 只好关表；改成只有被原始访问碰到的池才常驻。
// 期望（按 GlobalToStorage 的表格式与 NVN / Metal 常驻语义手写，不由被测实现推出）：
//   表 = 8192 项 × 32 字节 {起点, 终点（不含）, 差值 = 池的 Metal GPU 地址 − 游戏手里的地址, 提示字, 状态字}，按起点升序，第 0 项空（差值字 = 访问位图地址），空项起点 ~0；
//   游戏手里的是 hle_nvn.c 给每个池分的 GPU 地址窗口（gva），表项按窗口区间；只收游戏拿到过地址的池（GetBufferAddress / BufferInitialize / GetTextureAddress）；
//   每个整块落在某项（冷项按真区间）里的 2 MiB 块 c，第 (c mod 8192) 项的提示字 = c << 13 | 项号。写记录 = 每项一位的位图；完成时宿主把写过的池按 CPU 区间记下。
//   按需常驻约定：GPU 只能碰常驻集合里的池。没常驻的池 = 冷项 {起点, 起点, 真终点}（查表当「不在任何池里」：读得哨兵、写丢），状态字 = 访问位号 + 1，碰到时置访问位；
//   下一次提交编码前宿主读位 → 放进常驻集合并提交 → 重建表让这一项可用。只因原始访问常驻的池保留访问位，GAGE_BASE（300）帧没碰到就移出（项立刻变冷、
//   常驻集合等那次提交跑完才放），每被移出一次滞后翻倍；绑定给过 GPU 地址的池（pool_res）永久常驻、状态字 0。
//   ① 反射：gmem_map（rw）→ GMEM 可写、gmem_log → GMEML
//   ② 池 A（取地址，又被当存储缓冲绑定 → 常驻）、B（只建缓冲）拿过地址、C 没有：表 2 项；A 可用、状态 0；B 冷项 {gB, gB, gB + 4 MiB}、状态 = 位号 + 1；C 不在；
//      A 的整 2 MiB 块提示字 = 块号 << 13 | A 的项号；B 没被碰过 → 不在常驻集合（未访问的池不进常驻）
//   ③ 第一次访问：A 的字读到；B 的读得哨兵、写被丢（B 存储不变、写记录不置位）、B 的访问位置上（首次访问安全：GPU 没碰不常驻的 B）
//   ④ 下一次提交：B 进常驻集合、项可用；B 的字读到、写落地、写记录学到 B 的 CPU 区间
//   ⑤ 给 C 取地址 → 表 3 项、C 冷；碰一次后的下一次提交读到 C 的字
//   ⑩ 每次提交后：表里每个可用项的池都在常驻集合里
//   ⑧ 移出带滞后：B 最后被看到后第 300 帧还常驻；第 361 帧移出：项立刻变冷、常驻集合仍留着（那次提交可能用了旧表），下一次提交（前一次已完成）才放
//   ⑨ 再碰 B → 重新常驻；被移出过一次滞后翻倍：之后第 361 帧仍常驻（基数 300 会移出）、第 639 帧移出
//   ⑪ 绑定给过地址的池（pool_res）：表重建成状态 0 的可用项，长期没碰也不移出
//   ⑦ 开不开：不设 / 设 1 / 设空 → 开（iOS 与 Mac 一样），设 0 → 关；关时表 0 项、不为表加常驻
//   ⑥ 存储缓冲字节数表里表 = 8192 × 32 = 262144、写记录 = 8192 / 8 = 1024
// 修前（main 的整池常驻）：② 「B 没被碰过不在常驻集合」红、③ B 读到值（没跳过）、⑦ iOS 缺省关。负对照见 docs/坑记录.md 本条
#include "../gfx/nvn/nvn_mtl.m"
#include "nvn_protos.h"

extern void *h_nvnBootstrapLoader(const char *name);
static unsigned checks, fails;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL setup %s:%d %s\n", __FILE__, __LINE__, #x); exit(2); } } while (0)
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) fails++; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
static void *load(const char *name) { void *e = h_nvnBootstrapLoader(name); REQUIRE(e); return e; }
#define API(name) ((NVNT_##name)load(#name))
enum { PSIZE = 4 << 20, SKEW = 0x5000, IO_OFF = 0x300000, N = 8 };
typedef struct { NVNmemoryPool pool; uint8_t *mem; } pool_box;
static NVNdevice device;
static void make_pool(pool_box *b, uint32_t tag) {
    void *raw; REQUIRE(posix_memalign(&raw, 65536, PSIZE + 65536) == 0);
    b->mem = (uint8_t *)raw + SKEW;   // 4 KB 对齐、非 64 KB 对齐：CPU 地址与窗口地址的低位不同
    for (int i = 0; i < PSIZE / 4; i++) ((uint32_t *)b->mem)[i] = tag | (uint32_t)i;
    NVNmemoryPoolBuilder pb; API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb, b->mem, PSIZE);
    API(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    REQUIRE(API(nvnMemoryPoolInitialize)(&b->pool, &pb));
}
static uint64_t gva_of(const uint8_t *cpu) {   // 窗口地址按 GPU 地址表读（不经取地址入口：C 要保持「没给过地址」）
    static uint64_t t[3 * 1024]; int n = nvn_gva_snapshot(t, 1024);
    for (int i = 0; i < n; i++) if (t[3 * i + 1] == (uint64_t)(uintptr_t)cpu) return t[3 * i];
    return 0;
}
// 着色器（照 GlobalToStorage 的查表约定手写）：第 i 线程按 io[i] = {地址, 操作（0 读 / 1 写）, 写入值} 找起点 ≤ 地址的最后一项 k；地址 < 终点：可用（状态字非 0 先置访问位），
//   读 res[i] = *(地址 + 差值)、写 *(地址 + 差值) = 值、置写记录位、res[i] = 项号；终点 = 起点且地址 < 真终点（差值字）：冷项，置访问位、res[i] = 0xdeadbeef；其余查不到 0xdeadbeef
static NSString *const SRC = @"struct E { ulong lo, hi, delta; uint hint, state; };\n"
    "struct IO { ulong addr; uint op, val; };\n"
    "struct Args { device const E *map [[id(0)]]; device atomic_uint *log [[id(1)]]; device IO *io [[id(2)]]; constant uint *sz [[id(3)]]; };\n"
    "static void note(device const E *m, uint st) { if (!st) return; device atomic_uint *b = reinterpret_cast<device atomic_uint *>(m[0].delta) + ((st - 1) >> 5); uint bit = 1u << ((st - 1) & 31);\n"
    "    if (!(atomic_load_explicit(b, memory_order_relaxed) & bit)) atomic_fetch_or_explicit(b, bit, memory_order_relaxed); }\n"
    "kernel void k(constant Args &a [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
    "    if (i >= 8) return;\n"
    "    ulong x = a.io[i].addr; int k = 0;\n"
    "    for (int j = 1; j < 8192 && a.map[j].lo != ~0ul; j++) if (a.map[j].lo <= x) k = j;\n"
    "    device uint *res = (device uint *)(a.io + 8) + i;\n"
    "    if (i == 0) { res[8] = a.sz[0]; res[9] = a.sz[1]; }\n"
    "    if (!(x < a.map[k].hi)) { if (a.map[k].hi == a.map[k].lo && x < a.map[k].delta) note(a.map, a.map[k].state); *res = 0xdeadbeefu; return; }\n"
    "    note(a.map, a.map[k].state);\n"
    "    device uint *p = reinterpret_cast<device uint *>(x + a.map[k].delta);\n"
    "    if (a.io[i].op) { *p = a.io[i].val; atomic_fetch_or_explicit(&a.log[k >> 5], 1u << (k & 31), memory_order_relaxed); *res = uint(k); } else *res = *p;\n"
    "}\n";
static NvShader *shader(int *rc, char *err, int errlen) {
    NSError *e = nil; MTLCompileOptions *o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion3_1;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:[@"#include <metal_stdlib>\nusing namespace metal;\n" stringByAppendingString:SRC] options:o error:&e];
    if (!lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); REQUIRE(lib); }
    NvShader *cs = [NvShader new]; cs->stage = 5; cs->fn = [lib newFunctionWithName:@"k"]; REQUIRE(cs->fn);
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0]; REQUIRE(cs->ae); cs->arg_len = cs->ae.encodedLength;
    *rc = nvn_refl_from_json(@{ @"route": @"B", @"threadgroup_size": @[@8, @1, @1], @"bindings": @[
        @{ @"kind": @"gmem_map", @"arg_id": @0, @"rw": @YES, @"msl_name": @"map", @"msl_type": @"device const E*" },
        @{ @"kind": @"gmem_log", @"arg_id": @1, @"msl_name": @"log", @"msl_type": @"device atomic_uint*" },
        @{ @"kind": @"sbuf", @"reg": @0, @"arg_id": @2, @"rw": @YES, @"msl_name": @"io", @"msl_type": @"device IO*" },
        @{ @"kind": @"sbsize", @"arg_id": @3, @"msl_name": @"sz", @"msl_type": @"constant uint*" } ] }, &cs->rf, err, errlen);
    return cs;
}
typedef struct { uint64_t addr; uint32_t op, val; } io_t;
static void run(NvShader *cs, uint8_t *io_cpu) {
    static prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P', 'R', 'O', 'G'); pg.serial = nvn_new_serial();
    NvProg *np = [NvProg new]; np->sh[5] = cs; serial_put(pg.serial, np);
    nvn_be_sub_begin();
    S.cprog = &pg; args_dirty_all();
    S.ssbo[5][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)io_cpu, .size = N * sizeof(io_t) + (N + 2) * 4};
    p_dispatch d = {.x = 1, .y = 1, .z = 1}; dispatch(&d);
    // GPU 完成等待上限 30 秒（原 10 秒）；完成回调推进 g_done 等到为止，兜底 30 秒（原 1 秒：负载高时回调晚跑，后面读到旧状态假红）
    uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 30000000000ull)); REQUIRE(!mtl_last_commit_error());
    for (int t = 0; t < 30000; t++) { pthread_mutex_lock(&g_mu); uint64_t d = g_done; pthread_mutex_unlock(&g_mu); if (d >= f) break; usleep(1000); }   // 完成回调推进 g_done
}
static pool_ent *pool_of(const uint8_t *mem) { pthread_mutex_lock(&g_mu); int i = pool_find((uint64_t)(uintptr_t)mem, 1); pthread_mutex_unlock(&g_mu); REQUIRE(i >= 0); return &g_pools[i]; }
static int in_res(id<MTLBuffer> b) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:b]; pthread_mutex_unlock(&g_res_mu); return r; }
static int nent(void) { const uint64_t *t = (const uint64_t *)g_gmem_buf.contents; int n = 0; while (n + 1 < 8192 && t[(n + 1) * 4] != ~0ull) n++; return n; }
static int ent_of(uint64_t g) { const uint64_t *t = (const uint64_t *)g_gmem_buf.contents; for (int k = 1; k <= nent(); k++) if (t[k * 4] == g) return k; return 0; }
static uint32_t tw_state(uint64_t g) { int k = ent_of(g); return k ? ((const uint32_t *)g_gmem_buf.contents)[k * 8 + 7] : ~0u; }
static int cold(uint64_t g) { int k = ent_of(g); const uint64_t *t = (const uint64_t *)g_gmem_buf.contents; return k && t[k * 4 + 1] == t[k * 4]; }
// ⑩ 表里每个可用项（终点 > 起点）的池都在常驻集合里：返回违反的项数
static int usable_not_resident(void) {
    const uint64_t *t = (const uint64_t *)g_gmem_buf.contents; int bad = 0;
    uint64_t snap[3 * 64]; int ng = nvn_gva_snapshot(snap, 64);
    for (int k = 1; k <= nent(); k++) { if (t[k * 4 + 1] == t[k * 4]) continue;
        uint64_t cpu = 0; for (int s = 0; s < ng; s++) if (t[k * 4] >= snap[3 * s] && t[k * 4] < snap[3 * s] + snap[3 * s + 2]) cpu = snap[3 * s + 1] + (t[k * 4] - snap[3 * s]);
        if (!cpu || !in_res(pool_of((const uint8_t *)(uintptr_t)cpu)->buf)) bad++; }
    return bad;
}

int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0);
    NVNdeviceBuilder db; API(nvnDeviceBuilderSetDefaults)(&db); REQUIRE(API(nvnDeviceInitialize)(&device, &db));
    mtl_init();
    static pool_box A, B, C;
    make_pool(&A, 0xa0000000u); make_pool(&B, 0xb0000000u); make_pool(&C, 0xc0000000u);
    uint64_t gA = API(nvnMemoryPoolGetBufferAddress)(&A.pool);   // A：池取地址
    NVNbufferBuilder bb; API(nvnBufferBuilderSetDefaults)(&bb); API(nvnBufferBuilderSetDevice)(&bb, &device);
    API(nvnBufferBuilderSetStorage)(&bb, &B.pool, 0x1000, 0x1000); NVNbuffer buf; REQUIRE(API(nvnBufferInitialize)(&buf, &bb));   // B：只建缓冲
    uint64_t gB = gva_of(B.mem), gC = gva_of(C.mem);
    REQUIRE(gA && gB && gC && gA % 65536 == 0 && gB % 65536 == 0);
    char err[512] = ""; int rc = 0;
    NvShader *cs = shader(&rc, err, sizeof err);
    VERIFY(rc == 0, "① 反射转换成功（rc %d%s%s）", rc, err[0] ? "：" : "", err);
    if (rc) goto done;
    VERIFY(cs->rf.nb == 4 && cs->rf.b[0].kind == NVN_RB_GMEM && cs->rf.b[0].rw && cs->rf.b[1].kind == NVN_RB_GMEML, "① 种类：%u（期望 GMEM %u）可写 %u、%u（期望 GMEML %u）",
           cs->rf.b[0].kind, (unsigned)NVN_RB_GMEM, cs->rf.b[0].rw, cs->rf.b[1].kind, (unsigned)NVN_RB_GMEML);
    io_t *io = (io_t *)(A.mem + IO_OFF); uint32_t *res = (uint32_t *)(io + N);
    const uint32_t B10 = 0xb0000000u | 0x10;
    #define IO1 do { io[0] = (io_t){gA + 0x10, 0, 0}; io[1] = (io_t){gA + PSIZE - 4, 0, 0}; io[2] = (io_t){gB + 0x2000, 0, 0}; io[3] = (io_t){gC + 0x10, 0, 0}; \
        io[4] = (io_t){0x1234, 0, 0}; io[5] = (io_t){gB + 0x40, 1, 0x5a5a1234u}; io[6] = (io_t){gA + PSIZE, 0, 0}; io[7] = (io_t){gB + 0x44, 0, 0}; } while (0)
    #define IO_A do { for (int q = 0; q < N; q++) io[q] = (io_t){gA + 0x10, 0, 0}; } while (0)
    { void *bb; uint64_t oo; REQUIRE(nvn_be_addr((uint64_t)(uintptr_t)A.mem, 1, &bb, &oo)); }   // A 先被当缓冲绑定过（游戏里常见：存储缓冲所在的池）→ pool_res：常驻且永不移出
    IO1; run(cs, (uint8_t *)io);
    pool_ent *pA = pool_of(A.mem), *pB = pool_of(B.mem);
    {   // ② 表的内容、③ 第一次访问
        const uint64_t *t = (const uint64_t *)g_gmem_buf.contents; const uint32_t *tw = (const uint32_t *)g_gmem_buf.contents;
        int kA = gA < gB ? 1 : 2, kB = 3 - kA, n = nent();
        VERIFY(n == 2, "② 表里 %d 项（期望 2：A、B；C 没给过地址）", n);
        VERIFY(t[kA * 4] == gA && t[kA * 4 + 1] == gA + PSIZE && tw[kA * 8 + 7] == 0, "② 第 %d 项 [%#llx, %#llx) 状态 %u（期望 A 窗口 [%#llx, %#llx)、绑定给过地址 → 状态 0）",
               kA, (unsigned long long)t[kA * 4], (unsigned long long)t[kA * 4 + 1], tw[kA * 8 + 7], (unsigned long long)gA, (unsigned long long)(gA + PSIZE));
        VERIFY(t[kB * 4] == gB && t[kB * 4 + 1] == gB && t[kB * 4 + 2] == gB + PSIZE && tw[kB * 8 + 7] != 0 && tw[kB * 8 + 7] == (uint32_t)pB->gslot,
               "② 第 %d 项 {%#llx, %#llx, %#llx} 状态 %u（期望 B 冷项 {%#llx, 同起点, 真终点 %#llx}、状态 = B 的访问位号 + 1 = %d）", kB, (unsigned long long)t[kB * 4], (unsigned long long)t[kB * 4 + 1],
               (unsigned long long)t[kB * 4 + 2], tw[kB * 8 + 7], (unsigned long long)gB, (unsigned long long)(gB + PSIZE), pB->gslot);
        VERIFY(t[2] == g_gbits.gpuAddress, "② 第 0 项差值字 %#llx（期望访问位图地址 %#llx）", (unsigned long long)t[2], (unsigned long long)g_gbits.gpuAddress);
        uint64_t c0 = (gA + (1ull << 21) - 1) >> 21, c1 = (gA + PSIZE) >> 21; int hok = c1 > c0;
        for (uint64_t c = c0; c < c1; c++) hok &= tw[(c % 8192) * 8 + 6] == (uint32_t)(c << 13 | (unsigned)kA);
        uint64_t d0 = (gB + (1ull << 21) - 1) >> 21, d1 = (gB + PSIZE) >> 21; int hb = d1 > d0;
        for (uint64_t c = d0; c < d1; c++) hb &= tw[(c % 8192) * 8 + 6] == (uint32_t)(c << 13 | (unsigned)kB);
        VERIFY(hok && hb, "② 提示字：A 的整 2 MiB 块 → 第 %d 项 %s、B（冷项按真区间）→ 第 %d 项 %s", kA, hok ? "对" : "错", kB, hb ? "对" : "错");
        VERIFY(!pB->res && !in_res(pB->buf) && pA->res && in_res(pA->buf), "② 未访问的池不进常驻：B 常驻 %d / 在常驻集合 %d（期望 0 / 0）；A（绑定过）%d / %d（期望 1 / 1）", pB->res, in_res(pB->buf), pA->res, in_res(pA->buf));
        VERIFY(res[0] == (0xa0000000u | 4) && res[1] == (0xa0000000u | (PSIZE / 4 - 1)), "③ 读 A+0x10 = %#x（期望 %#x）、A 末字 = %#x（期望 %#x）", res[0], 0xa0000000u | 4, res[1], 0xa0000000u | (PSIZE / 4 - 1));
        VERIFY(res[2] == 0xdeadbeefu && res[7] == 0xdeadbeefu && res[5] == 0xdeadbeefu && ((uint32_t *)B.mem)[0x10] == B10,
               "③ 首次访问安全：B 不常驻时读 B+0x2000 → %#x、B+0x44 → %#x、写 B+0x40 → %#x（期望都跳过 0xdeadbeef），B 存储字 = %#x（期望不变 %#x）", res[2], res[7], res[5], ((uint32_t *)B.mem)[0x10], B10);
        VERIFY(res[3] == 0xdeadbeefu && res[4] == 0xdeadbeefu && res[6] == 0xdeadbeefu, "③ C 的窗口地址 → %#x、0x1234 → %#x、A 尾后一字 → %#x（期望都查不到 0xdeadbeef）", res[3], res[4], res[6]);
        int bit = pB->gslot - 1; uint32_t bw = ((const uint32_t *)g_gbits.contents)[bit >> 5];
        VERIFY(bit >= 0 && (bw >> (bit & 31) & 1), "③ B 的访问位（位 %d）置上：%u（期望 1）", bit, bit >= 0 ? bw >> (bit & 31) & 1 : 0);
        pthread_mutex_lock(&g_gml_mu); int ln = g_gml_n; pthread_mutex_unlock(&g_gml_mu);
        VERIFY(ln == 0, "③ 写被丢 → 写记录没学到池：%d（期望 0）", ln);
        VERIFY(res[8] == 8192 * 32 && res[9] == 1024, "⑥ 字节数表：池表 %u（期望 262144）、写记录 %u（期望 1024）", res[8], res[9]);
        VERIFY(usable_not_resident() == 0, "⑩ 第 1 次提交后可用项的池不在常驻集合的项数 %d（期望 0）", usable_not_resident());
    }
    IO1; run(cs, (uint8_t *)io);
    {   // ④ 下一次提交：B 常驻、可用
        int kB = ent_of(gB);
        VERIFY(pB->res && in_res(pB->buf) && pB->gm == 1 && !cold(gB), "④ 访问后进常驻：B 常驻 %d / 在常驻集合 %d / 因原始访问常驻 %d / 项冷 %d（期望 1 / 1 / 1 / 0）", pB->res, in_res(pB->buf), pB->gm, cold(gB));
        VERIFY(res[2] == (0xb0000000u | 0x800) && res[7] == (0xb0000000u | 0x11) && ((uint32_t *)B.mem)[0x10] == 0x5a5a1234u && res[5] == (uint32_t)kB,
               "④ 读 B+0x2000 = %#x（期望 %#x）、B+0x44 = %#x（期望 %#x）、写 B+0x40 落地 → %#x（期望 0x5a5a1234）落在第 %u 项（期望 %d）", res[2], 0xb0000000u | 0x800, res[7], 0xb0000000u | 0x11, ((uint32_t *)B.mem)[0x10], res[5], kB);
        pthread_mutex_lock(&g_gml_mu); int ln = g_gml_n; uint64_t llo = ln ? g_gml_lo[0] : 0, lhi = ln ? g_gml_hi[0] : 0; pthread_mutex_unlock(&g_gml_mu);
        VERIFY(ln == 1 && llo == (uint64_t)(uintptr_t)B.mem && lhi == (uint64_t)(uintptr_t)B.mem + PSIZE, "④ 写记录学到 %d 个池（期望 1）[%#llx, %#llx)（期望 B 的 CPU 区间 [%p, +%#x)）",
               ln, (unsigned long long)llo, (unsigned long long)lhi, (void *)B.mem, PSIZE);
        VERIFY(usable_not_resident() == 0, "⑩ 第 2 次提交后可用项的池不在常驻集合的项数 %d（期望 0）", usable_not_resident());
    }
    uint64_t gC2 = API(nvnMemoryPoolGetBufferAddress)(&C.pool);
    IO1; run(cs, (uint8_t *)io); int c_cold = nent() == 3 && cold(gC) && res[3] == 0xdeadbeefu;
    IO1; run(cs, (uint8_t *)io);
    VERIFY(gC2 == gC && c_cold && !cold(gC) && res[3] == (0xc0000000u | 4) && usable_not_resident() == 0, "⑤ 给 C 取地址后表 3 项、C 先冷（%d）、碰过后下一次提交读到 %#x（期望 %#x），可用项都常驻（违反 %d）",
           c_cold, res[3], 0xc0000000u | 4, usable_not_resident());
    pool_ent *pC = pool_of(C.mem); int c_gm = pC->gm == 1 && tw_state(gC) != 0;
    { void *bb; uint64_t oo; REQUIRE(nvn_be_addr((uint64_t)(uintptr_t)C.mem, 1, &bb, &oo)); }   // ⑪ C 之后又被绑定给了地址 → 表重建、C 的项状态 0、永不移出
    IO1; run(cs, (uint8_t *)io);
    VERIFY(c_gm && pC->reg && tw_state(gC) == 0 && !cold(gC), "⑪ C 先因原始访问常驻（状态非 0）%d，绑定后表重建：状态 %u（期望 0）、项冷 %d（期望 0）", c_gm, tw_state(gC), cold(gC));
    {   // ⑧ 移出带滞后（帧号 = 上屏计数 g_present_n；B 最后被看到在第 0 帧）
        IO_A; run(cs, (uint8_t *)io);   // 读掉上一次提交置的位（第 0 帧看到）
        g_present_n = 300; run(cs, (uint8_t *)io);
        int stay = pB->gm == 1 && !cold(gB) && in_res(pB->buf);
        g_present_n = 361; run(cs, (uint8_t *)io);
        int out_cold = cold(gB) && pB->gm == 0, still = in_res(pB->buf) && pB->res;
        run(cs, (uint8_t *)io);
        int gone = !in_res(pB->buf) && !pB->res;
        VERIFY(stay && out_cold && still && gone, "⑧ 第 300 帧仍常驻 %d（期望 1）；第 361 帧移出：项变冷 %d、常驻集合暂留 %d（期望 1 / 1）；下一次提交放掉 %d（期望 1）", stay, out_cold, still, gone);
        VERIFY(pC->res && in_res(pC->buf) && !cold(gC), "⑪ 同样没碰的 C（绑定过）第 361 帧后仍常驻 %d / 在常驻集合 %d、项冷 %d（期望 1 / 1 / 0）", pC->res, in_res(pC->buf), cold(gC));
        VERIFY(usable_not_resident() == 0, "⑩ 移出后可用项的池不在常驻集合的项数 %d（期望 0）", usable_not_resident());
    }
    {   // ⑨ 再碰 B → 重新常驻；滞后翻倍（被移出过 1 次：600 帧）
        IO1; io[5] = (io_t){gB + 0x40, 0, 0}; run(cs, (uint8_t *)io); int first = res[2] == 0xdeadbeefu; run(cs, (uint8_t *)io);
        int back = pB->gm == 1 && in_res(pB->buf) && res[2] == (0xb0000000u | 0x800);
        IO_A; run(cs, (uint8_t *)io);   // 第 361 帧看到
        g_present_n = 722; run(cs, (uint8_t *)io); int stay = pB->gm == 1 && !cold(gB);
        g_present_n = 1000; run(cs, (uint8_t *)io); int out = pB->gm == 0 && cold(gB);
        VERIFY(first && back && stay && out, "⑨ 移出后再碰：首次跳过 %d、下一次读到 %d（期望 1 / 1）；滞后翻倍：第 722 帧（看到后 361 帧）仍常驻 %d、第 1000 帧（639 帧）移出 %d（期望 1 / 1）", first, back, stay, out);
    }
    VERIFY(gmem_decide(NULL) == 1 && gmem_decide("1") == 1 && gmem_decide("") == 1 && gmem_decide("0") == 0,
           "⑦ 开不开（iOS 与 Mac 同一判法）：不设 %d（期望 1）、设 1 %d（期望 1）、设空 %d（期望 1）、设 0 %d（期望 0）", gmem_decide(NULL), gmem_decide("1"), gmem_decide(""), gmem_decide("0"));
    {   // ⑫ 过滤代数（g_gmem_pgen）与环复用（GMEM_RECYCLE / busy 保护）：
        //   ① 未取过地址范围的池建池 + 无变化 pool_age() 不改 g_gmem_pgen、gmem_map 返回同一缓冲；
        //   ② 槽 busy > 0 时跳过复用，空闲且 fence <= g_done 后回到原槽、不新建缓冲
        id<MTLBuffer> b0 = gmem_map(); int s0 = g_gmem_cur_slot; uint64_t pg0 = atomic_load(&g_gmem_pgen);
        pool_box D; make_pool(&D, 0xd0000000u);
        uint64_t pg1 = atomic_load(&g_gmem_pgen);
        id<MTLBuffer> b_same1 = gmem_map();
        pthread_mutex_lock(&g_mu); g_present_n += GAGE_CHECK + 1; uint64_t pgen0 = atomic_load(&g_pool_gen); pool_age(); uint64_t pgen1 = atomic_load(&g_pool_gen); pthread_mutex_unlock(&g_mu);
        uint64_t pg2 = atomic_load(&g_gmem_pgen);
        id<MTLBuffer> b_same2 = gmem_map();
        VERIFY(pg1 == pg0 && b_same1 == b0 && pgen1 > pgen0 && pg2 == pg0 && b_same2 == b0,
               "⑫-1 未取址池与空 pool_age 不触发 gmem_map 重建（pgen %llu→%llu, pool_gen %llu→%llu, 同缓冲 %d/%d）",
               (unsigned long long)pg0, (unsigned long long)pg2, (unsigned long long)pgen0, (unsigned long long)pgen1, b_same1 == b0, b_same2 == b0);
        REQUIRE(s0 >= 0);
        atomic_fetch_add(&g_gmem_rec[s0].busy, 1);
        atomic_fetch_add(&g_gmem_pgen, 1);
        id<MTLBuffer> b1 = gmem_map(); int s1 = g_gmem_cur_slot;
        atomic_fetch_add(&g_gmem_pgen, 1);
        id<MTLBuffer> b2 = gmem_map();
        atomic_fetch_sub(&g_gmem_rec[s0].busy, 1);
        g_gmem_rec[s0].fence = g_done;
        atomic_fetch_add(&g_gmem_pgen, 1);
        id<MTLBuffer> b3 = gmem_map(); int s3 = g_gmem_cur_slot;
        VERIFY(b1 != b0 && s1 != s0 && b2 != b0 && b2 != b1 && b3 == b0 && s3 == s0,
               "⑫-2 环复用尊重 busy/fence 并在空闲后原槽复用（s0=%d, s1=%d, s3=%d, b3==b0:%d）", s0, s1, s3, b3 == b0);
    }
    {   // ⑦ 开关关（DM_NVN_GMEM=0）：表重建成 0 项、不为表加常驻；按 A 的窗口地址读查不到（0xdeadbeef），与没有池表时一样
        g_gmem_on = 0; atomic_fetch_add(&g_pool_gen, 1); run(cs, (uint8_t *)io); int res0 = g_pool_res_n;   // 先跑一次：放掉第 1000 帧移出、等着的 B
        io[0] = (io_t){gA + 0x10, 0, 0}; atomic_fetch_add(&g_pool_gen, 1); run(cs, (uint8_t *)io);
        int n = nent();
        VERIFY(n == 0 && res[0] == 0xdeadbeefu && g_pool_res_n == res0, "⑦ 开关关：表 %d 项（期望 0），读 A+0x10 → %#x（期望 0xdeadbeef），常驻池数 %d → %d（期望不变）", n, res[0], res0, g_pool_res_n); }
done:
    printf("%s t_nvn_gmem：%u 项，%u 项失败（原始全局内存池表：只收给过地址的池、按 GPU 地址窗口建表、提示字、写记录位图、碰到的池才常驻）\n", fails ? "✗" : "✓", checks, fails);
    return fails != 0;
} }
