// 追不到槽的句柄读整个池（反射 tex_pool / samp_pool → NVN_RB_TEXP / SAMPP，nvn_mtl.m pool_gpu / pool_tab_new / pool_ent_put / pool_seen_scan / pool_pin_retire；Dsh.cs PoolArrays）。
// 11-06 Switch2 线 63：翻译器原先把 Ryujinx 回退数组 <阶段>_<t|s>_cb0_40000000 写死 32 项、下标夹到 31，宿主第 k 项绑第 k 个贴图槽：句柄指的贴图编号 ≥ 32 时读到别的贴图
//   （时之笛 4,454 支，抽查一支是阴影深度比较）。现照上游参考实现（MIT）the reference port：池指针 + 宿主池表 + 已读位图 + 未写渲染目标不建 + 小贴图预建 + 表复用。
// 着色器 = Dsh.cs PoolArrays 改写后的形状（tests/dmsh_pool_arrays_test.sh 核同一形状由改写产出、能编），走生产 dispatch + 真 Metal 设备。
// 贴图：R32F，编号 id 的贴图每个纹素 = id + 0.5（小的 4x4；大的 2048 宽、高按 12 / 14 / 18 / 20 / 22 MB 取，各不相同）。期望按语义手写，与被测实现无关：
//   ① 句柄 ≥ 32 读到正确贴图：贴图号 40、采样器号 40 → 40.5；贴图号 3 → 3.5
//   ② 位图置位：第一次派发读了 40、3、51、60 → 位图里这 4 位为 1；没读的 41、50 为 0
//   ③ 折叠后只建被读的 + 每帧预建上限：首次走池按小到大预建，本帧 32 MB 放得下小贴图 + 12 MB + 14 MB（共约 26 MB），18 MB（50）/ 20 MB（51）不建，
//      读 51 得占位 0.0；下一次提交折叠位图 → 51 建出、读得 51.5，同时 50（上次没读）仍不建、读得 0.0；再下一次 50 也建出、读得 50.5
//   ④ 没被任何遍写过的渲染目标（COMPRESSIBLE，60）读到也不为读而建：两次读都是 0.0、始终没建；同样大小的普通贴图（61）读得 61.5
//   ⑤ 已读贴图每帧上限 128 MB：本帧已记满时，GPU 置了位的 52（22 MB）留在待建表、不建；记账清零后下一次提交建出、读得 52.5
//   ⑥ 表增量维护（11-06 第 42 版：每次提交整表重走 + 新建表缓冲，19.7 帧 / 秒、低区地址空间耗尽）：之后几次提交里改登记 3 次，整表重走 0 次、新建表缓冲 0 张、
//      表地址不变；每次改登记后的那次提交只重查改了的那 1 个编号（登记重查计数 +1）
//   ⑨ 跨提交换贴图：40 改登记成另一张贴图（77.5），下一次提交表里第 40 项就是新贴图的资源号、读得 77.5
//      ⑨b 读过的编号换了存储（序号指向另一张贴图、旧的仍被共享表留着不退役）：下一次提交表里就是新的
//   ⑩ 旧贴图活到引用它的提交做完：表里有 91 的贴图 X，它的内存池销毁、X 交退役。退役栅栏过了、下一次提交开头清延迟队列时 X 仍在常驻集合（表还指着它）；
//      这次提交取表时第 91 项换成占位；这次提交做完、再下一次清队列后 X 才出常驻集合
//   ⑦ 池基址：绑定的贴图池基址 260 时，句柄里的本地号 40 → 第 300 号贴图 → 300.5
//   ⑧ 对照（旧语义）：同一句柄 40 经 32 项槽位数组（TEXD）读 → 夹到槽 31（空）→ 0.0，不是 40.5
// 修前（main 的 nvn_refl.m / nvn_mtl.m）：反射报「B binding kind tex_pool not implemented」，①–⑦ 跑不起来，只有 ⑧ 照旧。
// 8c522975b 的宿主（代号一变整表重走、每次新建表缓冲、不钉常驻）没有这些接口，编不过本单测；它的做法由负对照「整表重走」「不钉常驻」复现，⑥ ⑩ 红。
// 负对照：删 pool_gpu 里的 pool_seen_scan() → ③ 51 不建（红）；pool_unwritten_target 恒 0 → ④ 60 被建（红）；pool_pin_retire 恒 0（不钉常驻）→ ⑩ 红；
//   有改登记就整表重走（丢表重建）→ ⑥ 红；不处理改登记的编号 → ⑥ ⑨ 红；读过的编号不随存储变化重查 → ⑨b 红
#include "../gfx/nvn/nvn_mtl.m"
#include <malloc/malloc.h>
#include <signal.h>
int s2a_direct_fault(void *addr, int is_write, void *uctx);
static void on_sig17(int s, siginfo_t *si, void *uc) { if (s2a_direct_fault(si->si_addr, 1, uc)) return; signal(s, SIG_DFL); }

static unsigned checks, fails;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL setup %s:%d %s\n", __FILE__, __LINE__, #x); exit(2); } } while (0)
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) fails++; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
enum { MEM = 160 << 20, IO_OFF = 0x1000 };
static uint8_t *memory; static pool_t *pool; static size_t mem_next = 0x100000;
static tex_t texs[400];

static tex_t *texture_ex(int id, uint32_t w, uint32_t h, int flags, float val) {   // 每个纹素 = val
    tex_t *t = &texs[id]; memset(t, 0, sizeof *t); t->magic = MAGIC('T', 'E', 'X', 0); t->serial = nvn_new_serial();
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F, .width = w, .height = h, .depth = 1, .levels = 1, .flags = flags};
    nvn_tex_layout(&t->d, &t->L); REQUIRE(t->L.size);
    size_t off = (mem_next + 0xffff) & ~(size_t)0xffff; REQUIRE(off + t->L.size <= MEM); mem_next = off + t->L.size;
    t->pool = pool; t->off = (int64_t)off; t->mem = pool->mem + off;
    float *p = (float *)(memory + off); for (size_t i = 0; i < t->L.size / 4; i++) p[i] = val;   // 每个纹素同值：块线性排布不影响
    nvn_be_tex_init(t); nvn_be_reg_tex(id, t, NULL);
    return t;
}
static tex_t *texture(int id, uint32_t w, uint32_t h, int flags) { return texture_ex(id, w, h, flags, (float)id + 0.5f); }
// 这次提交里贴图池表（基址 0）第 tid 项：直接经 pool_gpu 取（不派发）
static uint64_t table_entry(int tid, int unused, uint64_t *gpu) {
    (void)unused; const pool_tab *tp = NULL; for (int i = 0; i < POOL_TABS; i++) if (g_ptab[i].used && !g_ptab[i].samp && g_ptab[i].base == 0) tp = &g_ptab[i];
    REQUIRE(tp);
    os_unfair_lock_lock(&g_rmu);
    __unsafe_unretained id<MTLBuffer> b = nil; uint64_t a = pool_gpu(0, tp->dtt, tp->dpf, 0, 0, &b); REQUIRE(b);
    uint64_t e = ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[tid];
    os_unfair_lock_unlock(&g_rmu);
    if (gpu) *gpu = a;
    return e;
}
static uint64_t cur_rid(int tid) { os_unfair_lock_lock(&g_rmu); uint64_t r = tex_fast_rid(tid, NULL); os_unfair_lock_unlock(&g_rmu); return r; }
static int resident(id<MTLResource> r) { pthread_mutex_lock(&g_res_mu); int k = [g_res_objects containsObject:r]; pthread_mutex_unlock(&g_res_mu); return k; }
static void empty_sub(void) { nvn_be_sub_begin(); uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
static int made(int id) { return tex_get(texs[id].serial) != nil; }
static int seen_bit(int id) { return g_seen_buf && (((const uint32_t *)g_seen_buf.contents)[id >> 5] >> (id & 31) & 1); }
static NvShader *shader(NSString *src, NSDictionary *refl, char *err, int errlen) {
    NSError *e = nil; MTLCompileOptions *o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion2_4;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:[@"#include <metal_stdlib>\nusing namespace metal;\n" stringByAppendingString:src] options:o error:&e];
    if (!lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); exit(2); }
    NvShader *cs = [NvShader new]; cs->stage = 5; cs->fn = [lib newFunctionWithName:@"main0"]; REQUIRE(cs->fn);
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0]; REQUIRE(cs->ae); cs->arg_len = cs->ae.encodedLength;
    int rc = nvn_refl_from_json(refl, &cs->rf, err, errlen);
    return rc ? nil : cs;
}
static void run_cs(NvShader *cs) {
    static prog_t pg; memset(&pg, 0, sizeof pg); pg.magic = MAGIC('P', 'R', 'O', 'G'); pg.serial = nvn_new_serial();
    NvProg *np = [NvProg new]; np->sh[5] = cs; serial_put(pg.serial, np);
    nvn_be_sub_begin();
    S.cprog = &pg; args_dirty_all();
    p_dispatch d = {.x = 1, .y = 1, .z = 1}; dispatch(&d);
    uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); REQUIRE(!mtl_last_commit_error());
}
// Dsh.cs PoolArrays 改写后的形状：贴图池读置位（dm_pool_mark）、采样器池不置位；句柄从存储缓冲第 0 个 uint4 取，结果写进第 1 个 float4
static NSString *const POOL_SRC =
    @"template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)\n{\n"
     "    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);\n    uint b = 1u << (i & 31u);\n"
     "    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);\n    return i;\n}\n\n"
     "struct spvPool_cp_t2d_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<texture2d<float>, 1> e;\n};\n\n"
     "struct spvPool_cp_s_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<sampler, 1> e;\n};\n\n"
     "struct spvDescriptorSetBuffer0\n{\n    device uint4* cp_s0 [[id(0)]];\n    constant spvPool_cp_t2d_cb0_40000000* cp_t2d_cb0_40000000 [[id(1)]];\n"
     "    constant spvPool_cp_s_cb0_40000000* cp_s_cb0_40000000 [[id(2)]];\n};\n\n"
     "kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gid [[thread_position_in_grid]])\n{\n"
     "    if (gid.x != 0) return;\n    uint4 h = spvDescriptorSet0.cp_s0[0]; float4 r;\n"
     "    for (int k = 0; k < 4; k++) {\n        uint t = min(h[k] & 1048575u, 1048575u), s = min(h[k] >> 20u, 4095u);\n"
     "        r[k] = (&spvDescriptorSet0.cp_t2d_cb0_40000000->e[0])[dm_pool_mark(spvDescriptorSet0.cp_t2d_cb0_40000000, min(uint(t), spvDescriptorSet0.cp_t2d_cb0_40000000->n))]"
     ".sample((&spvDescriptorSet0.cp_s_cb0_40000000->e[0])[min(uint(s), spvDescriptorSet0.cp_s_cb0_40000000->n)], float2(0.5), level(0.0)).x;\n    }\n"
     "    spvDescriptorSet0.cp_s0[1] = as_type<uint4>(r);\n}\n";
// 旧语义：32 项槽位数组（TEXD / SAMPD），下标夹到 31
static NSString *const SLOT_SRC =
    @"struct spvDescriptorSetBuffer0\n{\n    device uint4* cp_s0 [[id(0)]];\n    array<texture2d<float>, 32> cp_t2d_cb0_40000000 [[id(1)]];\n    array<sampler, 32> cp_s_cb0_40000000 [[id(33)]];\n};\n\n"
     "kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gid [[thread_position_in_grid]])\n{\n"
     "    if (gid.x != 0) return;\n    uint h = spvDescriptorSet0.cp_s0[0].x; uint t = min(h & 1048575u, 31u), s = min(h >> 20u, 31u);\n"
     "    float v = spvDescriptorSet0.cp_t2d_cb0_40000000[t].sample(spvDescriptorSet0.cp_s_cb0_40000000[s], float2(0.5), level(0.0)).x;\n"
     "    spvDescriptorSet0.cp_s0[1] = as_type<uint4>(float4(v));\n}\n";
// 图像池（Dsh.cs PoolArrays 改写可写图像回退数组的形状）：句柄从存储缓冲第 0 个 uint4 的 x 取、要写的值在 y，写满 4x4
static NSString *const IMG_SRC =
    @"template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)\n{\n"
     "    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);\n    uint b = 1u << (i & 31u);\n"
     "    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);\n    return i;\n}\n\n"
     "struct spvPool_cp_i2d_cb0_40000000\n{\n    uint n;\n    ulong seen;\n    array<texture2d<float, access::write>, 1> e;\n};\n\n"
     "struct spvDescriptorSetBuffer0\n{\n    device uint4* cp_s0 [[id(0)]];\n    constant spvPool_cp_i2d_cb0_40000000* cp_i2d_cb0_40000000 [[id(1)]];\n};\n\n"
     "kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gid [[thread_position_in_grid]])\n{\n"
     "    if (gid.x != 0) return;\n    uint4 h = spvDescriptorSet0.cp_s0[0]; uint t = min(h.x & 1048575u, 1048575u);\n"
     "    for (uint y = 0; y < 4; y++) for (uint x = 0; x < 4; x++)\n"
     "        (&spvDescriptorSet0.cp_i2d_cb0_40000000->e[0])[dm_pool_mark(spvDescriptorSet0.cp_i2d_cb0_40000000, min(uint(t), spvDescriptorSet0.cp_i2d_cb0_40000000->n))]"
     ".write(float4(as_type<float>(h.y)), uint2(x, y));\n}\n";
static float out4[4];
static void read4(NvShader *cs, uint32_t h0, uint32_t h1, uint32_t h2, uint32_t h3) {
    uint32_t *io = (uint32_t *)(memory + IO_OFF); io[0] = h0; io[1] = h1; io[2] = h2; io[3] = h3; for (int i = 4; i < 8; i++) io[i] = 0xffffffffu;
    S.ssbo[5][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)io, .size = 32};
    run_cs(cs);
    memcpy(out4, io + 4, 16);
}
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0);
    mtl_init();
    REQUIRE(!posix_memalign((void **)&memory, 65536, MEM)); memset(memory, 0, MEM);
    pool = calloc(1, sizeof *pool); REQUIRE(pool);
    *pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = memory, .size = MEM};
    nvn_be_pool_add(memory, MEM);
    const int MB = 1 << 20; const uint32_t W = 2048;   // 大贴图 2048 宽 R32F：一行 8 KB，高 = 字节 / 8 KB
    texture(3, 4, 4, 0); texture(40, 4, 4, 0); texture(41, 4, 4, 0); texture(61, 4, 4, 0); texture(300, 4, 4, 0);
    texture(60, 4, 4, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    texture(70, W, 12 * MB / (W * 4), 0); texture(71, W, 14 * MB / (W * 4), 0); texture(50, W, 18 * MB / (W * 4), 0); texture(51, W, 20 * MB / (W * 4), 0);
    REQUIRE(texs[70].L.size < texs[71].L.size && texs[71].L.size < texs[50].L.size && texs[50].L.size < texs[51].L.size);
    samp_par sp; memset(&sp, 0, sizeof sp); sp.lod_max = 1000; nvn_be_reg_samp(40, &sp); nvn_be_reg_samp(1, &sp);
    S.tex_base = 0; S.samp_base = 0;
    char err[512] = "";
    NSDictionary *refl = @{ @"route": @"B", @"threadgroup_size": @[@1, @1, @1], @"bindings": @[
        @{ @"kind": @"sbuf", @"reg": @0, @"arg_id": @0, @"rw": @YES, @"msl_name": @"cp_s0", @"msl_type": @"device uint4*" },
        @{ @"kind": @"tex_pool", @"elem": @"texture2d<float>", @"arg_id": @1, @"msl_name": @"cp_t2d_cb0_40000000", @"msl_type": @"constant spvPool_cp_t2d_cb0_40000000*" },
        @{ @"kind": @"samp_pool", @"elem": @"sampler", @"arg_id": @2, @"msl_name": @"cp_s_cb0_40000000", @"msl_type": @"constant spvPool_cp_s_cb0_40000000*" } ] };
    NvShader *cs = shader(POOL_SRC, refl, err, sizeof err);
    VERIFY(cs && cs->rf.nb == 3 && cs->rf.b[1].kind == NVN_RB_TEXP && cs->rf.b[2].kind == NVN_RB_SAMPP && cs->rf.pool && cs->rf.bindless,
           "反射：tex_pool → TEXP、samp_pool → SAMPP、记成池 / 无绑定（%s）", cs ? "转换成功" : err);
    if (cs) {
        // 第一次提交：读 40（采样器 40）、3、51、60
        read4(cs, 40u | 40u << 20, 3u | 1u << 20, 51u | 1u << 20, 60u | 1u << 20);
        VERIFY(near(out4[0], 40.5f) && near(out4[1], 3.5f), "① 句柄 40（采样器 40）→ %.2f（期望 40.50）、句柄 3 → %.2f（期望 3.50）", out4[0], out4[1]);
        VERIFY(seen_bit(40) && seen_bit(3) && seen_bit(51) && seen_bit(60) && !seen_bit(41) && !seen_bit(50),
               "② 位图：40 %d、3 %d、51 %d、60 %d（期望全 1）；41 %d、50 %d（期望 0）", seen_bit(40), seen_bit(3), seen_bit(51), seen_bit(60), seen_bit(41), seen_bit(50));
        VERIFY(made(70) && made(71) && !made(50) && !made(51) && near(out4[2], 0.0f),
               "③ 首次走池预建到本帧 32 MB：12 MB %d、14 MB %d（期望 1）；18 MB %d、20 MB %d（期望 0）；读 51 → %.2f（期望占位 0.00）", made(70), made(71), made(50), made(51), out4[2]);
        // 第二次提交：折叠位图 → 51 建；50 上次没读，仍不建
        read4(cs, 51u | 1u << 20, 41u | 1u << 20, 50u | 1u << 20, 60u | 1u << 20);
        VERIFY(made(51) && near(out4[0], 51.5f) && !made(50) && near(out4[2], 0.0f) && near(out4[1], 41.5f),
               "③ 折叠后只建被读的：51 已建 %d、读得 %.2f（期望 1、51.50）；50 已建 %d、读得 %.2f（期望 0、0.00）；41 → %.2f（期望 41.50）", made(51), out4[0], made(50), out4[2], out4[1]);
        read4(cs, 50u | 1u << 20, 60u | 1u << 20, 61u | 1u << 20, 3u | 1u << 20);
        VERIFY(made(50) && near(out4[0], 50.5f), "③ 再下一次：50 已建 %d、读得 %.2f（期望 1、50.50）", made(50), out4[0]);
        VERIFY(!made(60) && near(out4[1], 0.0f) && near(out4[2], 61.5f), "④ 没写过的渲染目标 60：已建 %d、读得 %.2f（期望 0、0.00）；普通贴图 61 → %.2f（期望 61.50）", made(60), out4[1], out4[2]);
        // ⑤ 本帧已读贴图记满 128 MB：GPU 置了 52 的位（这里直接置），下一次提交不建
        texture(52, W, 22 * MB / (W * 4), 0);
        ((uint32_t *)g_seen_buf.contents)[52 >> 5] |= 1u << (52 & 31);
        g_walk_req = POOL_REQ_BYTES;
        read4(cs, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        int todo52 = g_seen_todo[52 >> 5] >> (52 & 31) & 1;
        VERIFY(!made(52) && todo52, "⑤ 记满 128 MB：52 已建 %d（期望 0）、仍在待建表 %d（期望 1）", made(52), todo52);
        g_walk_req = 0;
        read4(cs, 52u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        VERIFY(made(52) && near(out4[0], 52.5f), "⑤ 记账清零后：52 已建 %d、读得 %.2f（期望 1、52.50）", made(52), out4[0]);
        // ⑥ 增量维护：之后的提交里改登记，不整表重走、不新建表缓冲、表地址不变，只重查改了的编号
        uint64_t w0 = g_pool_walks, m0 = g_pool_made, gpu0 = 0; nvn_be_sub_begin(); table_entry(3, 0, &gpu0); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        unsigned one = 1; uint64_t gpu1 = gpu0;
        for (int k = 0; k < 3; k++) {
            texture_ex(41, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 41.5f);
            uint64_t d0 = g_pool_upd_dirty; nvn_be_sub_begin(); table_entry(3, 0, &gpu1); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
            if (g_pool_upd_dirty - d0 != 1 || gpu1 != gpu0) one = 0;
        }
        VERIFY(g_pool_walks - w0 == 0 && g_pool_made - m0 == 0 && one, "⑥ 改登记 3 次：整表重走 %llu 次、新建表缓冲 %llu 张（期望 0、0）；每次只重查 1 个编号且表地址不变 %u（期望 1）",
               (unsigned long long)(g_pool_walks - w0), (unsigned long long)(g_pool_made - m0), one);
        // ⑦ 池基址 260：本地号 40 → 第 300 号
        S.tex_base = 260;
        read4(cs, 40u | 1u << 20, 40u | 1u << 20, 40u | 1u << 20, 40u | 1u << 20);
        VERIFY(near(out4[0], 300.5f), "⑦ 贴图池基址 260、句柄 40 → %.2f（期望 300.50）", out4[0]);
        S.tex_base = 0;
        // ⑨ 跨提交换贴图（新登记的带 IMAGE：不归「首用取存储」，单测里每次解析都重传、资源号每次都变，比不出新旧）
        read4(cs, 40u | 40u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        atomic_fetch_add(&g_cur_frame, 1);   // 新的一帧：本帧预建额度重新算（单测不出帧，帧号不会自己走）
        texture_ex(40, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 77.5f);
        nvn_be_sub_begin(); uint64_t e40 = table_entry(40, 0, NULL), now40 = cur_rid(40); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        read4(cs, 40u | 40u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        VERIFY(e40 == now40 && near(out4[0], 77.5f), "⑨ 改登记后的下一次提交：第 40 项 = 新贴图 %d（期望 1）；读 40 → %.2f（期望 77.50）", e40 == now40, out4[0]);
        // ⑨b 被读过的编号换了存储（序号指向另一张贴图，旧的仍被共享表留着、不退役）：下一次提交表里就是新的
        atomic_fetch_add(&g_cur_frame, 1);
        texture_ex(90, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 90.5f);
        read4(cs, 90u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        REQUIRE(near(out4[0], 90.5f));
        id<MTLTexture> X9 = tex_get(texs[90].serial); REQUIRE([X9 conformsToProtocol:@protocol(MTLTexture)]);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:X9.pixelFormat width:X9.width height:X9.height mipmapped:NO]; td.usage = X9.usage; td.storageMode = X9.storageMode;
        id<MTLTexture> Y = [g_dev newTextureWithDescriptor:td]; REQUIRE(Y); res_add(Y);
        empty_sub(); nvn_be_sub_begin(); table_entry(90, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }   // 90 的读位已折进 g_seen_done（不再是待建）
        serial_put(texs[90].serial, Y);
        nvn_be_sub_begin(); uint64_t e90 = table_entry(90, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        VERIFY(e90 == Y.gpuResourceID._impl, "⑨b 读过的 90 换了存储：下一次提交第 90 项 = 新贴图 %d（期望 1）", e90 == Y.gpuResourceID._impl);
        // ⑩ 旧贴图活到引用它的提交做完：91 在另一个内存池里，表里有它的贴图 X；池销毁（hle_nvn.c dpool_finalize 的做法：清共享表 → 贴图交退役 → 池失效）
        atomic_fetch_add(&g_cur_frame, 1);
        pool_t *pool2 = calloc(1, sizeof *pool2); REQUIRE(pool2);
        *pool2 = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .flags = pool->flags, .mem = memory, .size = MEM};   // 另一个池对象（同一段宿主内存）
        pool_t *keep = pool; pool = pool2; texture_ex(91, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 91.5f); pool = keep;
        read4(cs, 91u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        REQUIRE(near(out4[0], 91.5f));
        id<MTLTexture> X = tex_get(texs[91].serial); REQUIRE([X conformsToProtocol:@protocol(MTLTexture)]);
        nvn_be_sub_begin(); REQUIRE(table_entry(91, 0, NULL) == X.gpuResourceID._impl); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        nvn_be_alias_drop_pool(pool2); pool2->magic = 0;   // X 退役挂在下一次提交的栅栏上
        serial_del(texs[91].serial);
        empty_sub();
        nvn_be_sub_begin();   // 开头清延迟队列：X 的退役栅栏已过
        int res1 = resident(X);
        uint64_t e91 = table_entry(91, 0, NULL); int res2 = resident(X);
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        empty_sub();   // 开头清队列：取表那次提交已做完
        int res3 = resident(X);
        uint64_t dmy = g_dummy_tex_rid[MTLTextureType2D][0];
        VERIFY(res1 && e91 == dmy && res2 && !res3, "⑩ 池销毁后的旧贴图 X：退役栅栏过后仍常驻 %d（期望 1，表还指着它）；取表时第 91 项换成占位 %d（期望 1）、X 仍常驻 %d（期望 1）；取表那次提交做完后出常驻集合 %d（期望 1）",
               res1, e91 == dmy, res2, !res3);
    }
    // ⑩b 退役请求晚到（11-14 ⑩ 偶发红的根因，确定性复现）：完成回调在自己的线程上清延迟队列，先把对象取出队列、之后才 res_remove；
    //   提交线程在这中间把表项换走、钉子拔掉，res_remove 就看不到钉子，把本次提交还指着的贴图移出常驻集合。这里直接按「表项先换走、res_remove 后到」的次序做
    {   atomic_fetch_add(&g_cur_frame, 1);
        pool_t *pool3 = calloc(1, sizeof *pool3); REQUIRE(pool3);
        *pool3 = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .flags = pool->flags, .mem = memory, .size = MEM};
        pool_t *keep = pool; pool = pool3; texture_ex(92, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 92.5f); pool = keep;
        read4(cs, 92u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        REQUIRE(near(out4[0], 92.5f));
        id<MTLTexture> Z = tex_get(texs[92].serial); REQUIRE([Z conformsToProtocol:@protocol(MTLTexture)]);
        nvn_be_sub_begin(); REQUIRE(table_entry(92, 0, NULL) == Z.gpuResourceID._impl); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        nvn_be_sub_begin();
        uint64_t before = table_entry(92, 0, NULL);   // 本次提交指着 Z（之前编码的绘制可能读它）
        nvn_be_alias_drop_pool(pool3); pool3->magic = 0; serial_del(texs[92].serial);
        uint64_t e92 = table_entry(92, 0, NULL);   // 同一次提交里表项换成占位：最后一根钉子拔掉
        res_remove(Z);   // 晚到的退役请求（回调线程 defer_drain 取出后才调）
        int r1 = resident(Z);
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        empty_sub(); empty_sub();   // 开头清队列：本次提交已做完
        int r2 = resident(Z);
        uint64_t dmy = g_dummy_tex_rid[MTLTextureType2D][0];
        VERIFY(before == Z.gpuResourceID._impl && e92 == dmy && r1 && !r2, "⑩b 表项换走后才到的退役请求：本次提交指过 %d、换成占位 %d（期望 1 / 1）；res_remove 后仍常驻 %d（期望 1，本次提交还没做完）；提交做完后出常驻集合 %d（期望 1）",
               before == Z.gpuResourceID._impl, e92 == dmy, r1, !r2);
    }
    // ⑩c 两次退役请求、一次在拔钉前一次晚到（池销毁、serial_del 各排一个；⑩ 并发跑时剩下的那种偶发）。仿 ⑩b：完成回调在自己的线程上清延迟队列，先把对象取出队列、之后才 res_remove；
    //   提交线程在这中间把表项换走、钉子拔掉，res_remove 就看不到钉子，把本次提交还指着的贴图移出常驻集合。这里直接按「表项先换走、res_remove 后到」的次序做
    {   atomic_fetch_add(&g_cur_frame, 1);
        pool_t *pool4 = calloc(1, sizeof *pool4); REQUIRE(pool4);
        *pool4 = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .flags = pool->flags, .mem = memory, .size = MEM};
        pool_t *keep = pool; pool = pool4; texture_ex(93, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 93.5f); pool = keep;
        read4(cs, 93u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        REQUIRE(near(out4[0], 93.5f));
        id<MTLTexture> W = tex_get(texs[93].serial); REQUIRE([W conformsToProtocol:@protocol(MTLTexture)]);
        nvn_be_sub_begin(); REQUIRE(table_entry(93, 0, NULL) == W.gpuResourceID._impl); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        nvn_be_sub_begin();
        uint64_t before = table_entry(93, 0, NULL);   // 本次提交指着 W（之前编码的绘制可能读它）
        nvn_be_alias_drop_pool(pool4); pool4->magic = 0; serial_del(texs[93].serial);
        res_remove(W);   // 第一个退役请求在拔钉前到：钉着，只记下
        uint64_t e93 = table_entry(93, 0, NULL);   // 同一次提交里表项换成占位：最后一根钉子拔掉
        res_remove(W);   // 晚到的第二个退役请求（回调线程 defer_drain 取出后才调）
        int r1 = resident(W);
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        empty_sub(); empty_sub();   // 开头清队列：本次提交已做完
        int r2 = resident(W);
        uint64_t dmy = g_dummy_tex_rid[MTLTextureType2D][0];
        VERIFY(before == W.gpuResourceID._impl && e93 == dmy && r1 && !r2, "⑩c 表项换走后才到的退役请求：本次提交指过 %d、换成占位 %d（期望 1 / 1）；res_remove 后仍常驻 %d（期望 1，本次提交还没做完）；提交做完后出常驻集合 %d（期望 1）",
               before == W.gpuResourceID._impl, e93 == dmy, r1, !r2);
    }
    // ⑩d 拔钉后两个退役请求都晚到（池销毁、serial_del 各一个，回调线程先后处理）：第一个延后时不能把记下的栅栏用掉，否则第二个直接移出。仿 ⑩b：完成回调在自己的线程上清延迟队列，先把对象取出队列、之后才 res_remove；
    //   提交线程在这中间把表项换走、钉子拔掉，res_remove 就看不到钉子，把本次提交还指着的贴图移出常驻集合。这里直接按「表项先换走、res_remove 后到」的次序做
    {   atomic_fetch_add(&g_cur_frame, 1);
        pool_t *pool5 = calloc(1, sizeof *pool5); REQUIRE(pool5);
        *pool5 = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .flags = pool->flags, .mem = memory, .size = MEM};
        pool_t *keep = pool; pool = pool5; texture_ex(94, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 94.5f); pool = keep;
        read4(cs, 94u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        REQUIRE(near(out4[0], 94.5f));
        id<MTLTexture> V = tex_get(texs[94].serial); REQUIRE([V conformsToProtocol:@protocol(MTLTexture)]);
        nvn_be_sub_begin(); REQUIRE(table_entry(94, 0, NULL) == V.gpuResourceID._impl); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        nvn_be_sub_begin();
        uint64_t before = table_entry(94, 0, NULL);   // 本次提交指着 V（之前编码的绘制可能读它）
        nvn_be_alias_drop_pool(pool5); pool5->magic = 0; serial_del(texs[94].serial);
        uint64_t e94 = table_entry(94, 0, NULL);   // 同一次提交里表项换成占位：最后一根钉子拔掉
        res_remove(V); res_remove(V);   // 两个晚到的退役请求
        int r1 = resident(V);
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        empty_sub(); empty_sub();   // 开头清队列：本次提交已做完
        int r2 = resident(V);
        uint64_t dmy = g_dummy_tex_rid[MTLTextureType2D][0];
        VERIFY(before == V.gpuResourceID._impl && e94 == dmy && r1 && !r2, "⑩d 表项换走后才到的退役请求：本次提交指过 %d、换成占位 %d（期望 1 / 1）；res_remove 后仍常驻 %d（期望 1，本次提交还没做完）；提交做完后出常驻集合 %d（期望 1）",
               before == V.gpuResourceID._impl, e94 == dmy, r1, !r2);
    }
    // ⑪ 表长大后对象数组和登记的容量一样长：越过缓冲（按 64 KB 取整）的长大换新缓冲，之后在新缓冲容量内再长大不再 realloc，
    //    所以对象数组必须一次给够 t->cap 项（修前只给 n + n/4 + 1 项，第二次长大后 pool_ent_put 读越界，第47版 objc_retain 0x1f7 崩）
    { int ti = -1; for (int k = 0; k < POOL_TABS; k++) if (g_ptab[k].used && !g_ptab[k].samp) { ti = k; break; }
      pool_tab *t = ti >= 0 ? &g_ptab[ti] : NULL; size_t have = 0, need = 0; uint32_t c0 = t ? t->cap : 0;
      if (t) { pool_tab_put_n(t, t->cap); pool_tab_put_n(t, t->cap - 1); have = malloc_size(t->obj); need = (size_t)t->cap * sizeof(void *); }
      VERIFY(t && t->cap > c0 && have >= need, "⑪ 池表长大越过缓冲后：容量 %u → %u，对象数组 %zu 字节（期望 ≥ 容量 × 8 = %zu）", c0, t ? t->cap : 0, have, need); }
    // ⑧ 对照：旧的 32 项槽位数组
    NSDictionary *refl_slot = @{ @"route": @"B", @"threadgroup_size": @[@1, @1, @1], @"bindings": @[
        @{ @"kind": @"sbuf", @"reg": @0, @"arg_id": @0, @"rw": @YES, @"msl_name": @"cp_s0", @"msl_type": @"device uint4*" },
        @{ @"kind": @"tex", @"arg_id": @1, @"msl_name": @"cp_t2d_cb0_40000000", @"msl_type": @"array<texture2d<float>, 32>" },
        @{ @"kind": @"?", @"arg_id": @33, @"msl_name": @"cp_s_cb0_40000000", @"msl_type": @"array<sampler, 32>" } ] };
    err[0] = 0; NvShader *old = shader(SLOT_SRC, refl_slot, err, sizeof err);
    if (old) { read4(old, 40u | 40u << 20, 0, 0, 0); }
    VERIFY(old && old->rf.dyn && near(out4[0], 0.0f), "⑧ 对照：同一句柄 40 经 32 项槽位数组 → %.2f（旧语义夹到槽 31、读占位 0.00，不是 40.50；%s）", old ? out4[0] : -1.0f, old ? "TEXD" : err);
    // ⑫ #1117 驻留集按类拆（nvn_res_by_kind，显存账行用的同一个函数）：期望手写——往驻留集里加一张 1024×1024 RGBA8 私有纹理（4 MiB）和一个
    //   2 MiB 私有缓冲，前后两次读数里纹理至少涨 4 MB、私有缓冲至少涨 2 MB。反证：分类写反（私有 / 共享对调）→ 私有缓冲不涨
    {
        int64_t tex0 = -1, buf0 = -1, tex1 = -1, buf1 = -1;
        for (int round = 0; round < 2; round++) {
            if (round == 1) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1024 height:1024 mipmapped:NO];
                d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
                id<MTLTexture> T = [g_dev newTextureWithDescriptor:d]; id<MTLBuffer> B = [g_dev newBufferWithLength:2u << 20 options:MTLResourceStorageModePrivate];
                REQUIRE(T && B); res_add(T); res_add(B);
            }
            uint64_t k[8]; nvn_res_by_kind(k); long long tb = (long long)(k[1] >> 20), pb = (long long)(k[3] >> 20);
            if (round == 0) { tex0 = tb; buf0 = pb; } else { tex1 = tb; buf1 = pb; }
        }
        VERIFY(tex1 - tex0 >= 4 && buf1 - buf0 >= 2, "⑫ 驻留集按类：加 4 MiB 私有纹理后纹理 %lld → %lld MB（期望至少 +4）、加 2 MiB 私有缓冲后私有缓冲 %lld → %lld MB（期望至少 +2）",
               (long long)tex0, (long long)tex1, (long long)buf0, (long long)buf1);
    }
    // ⑬ 图像池（可写元素，反射 rw：nvn_refl.m tex_pool、nvn_mtl.m pool_fit / pool_mark_writes）。句柄从存储缓冲来的图像写，以前走 32 槽数组、宿主只能改 c2 里的句柄，
    //    写进槽里碰巧那张。期望按语义手写：
    //    a 反射记成可写；b 句柄 121（带 IMAGE，可写用途）写 9.0 → 再按读池读 121 得 9.00、读 120 仍 120.50（没写错张），池记为 GPU 写过
    //    c 句柄 122（COMPRESSIBLE、不带 IMAGE：渲染目标类，建时没有可写用途，也没有遍画过它）写 7.0：图像池照样建出它（不按「没写过的渲染目标不建」挡），
    //      没有可写用途 → 本次写进哑图、读 122 不是 7.00；下一次提交前升级成可写，再写 7.0 → 读得 7.00
    //    d 句柄 200（没登记）写：写进可写哑图，提交不报错
    //    负对照：pool_mark_writes 不标（池不记 GPU 写过）→ b 红；图像池也按「没写过的渲染目标不建」挡 → c 一直写哑图、红
    if (cs) {
        texture_ex(120, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 120.5f); texture_ex(121, 4, 4, NVN_TEXTURE_FLAGS_IMAGE, 121.5f); texture(122, 4, 4, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        NSDictionary *refl_img = @{ @"route": @"B", @"threadgroup_size": @[@1, @1, @1], @"bindings": @[
            @{ @"kind": @"sbuf", @"reg": @0, @"arg_id": @0, @"rw": @YES, @"msl_name": @"cp_s0", @"msl_type": @"device uint4*" },
            @{ @"kind": @"tex_pool", @"elem": @"texture2d<float, access::write>", @"arg_id": @1, @"msl_name": @"cp_i2d_cb0_40000000", @"msl_type": @"constant spvPool_cp_i2d_cb0_40000000*" } ] };
        err[0] = 0; NvShader *ws = shader(IMG_SRC, refl_img, err, sizeof err);
        VERIFY(ws && ws->rf.b[1].kind == NVN_RB_TEXP && ws->rf.b[1].rw, "⑬a 反射：可写元素的 tex_pool → TEXP 且可写（%s）", ws ? "转换成功" : err);
        if (ws) {
            float v9 = 9.0f, v7 = 7.0f; uint32_t u9, u7; memcpy(&u9, &v9, 4); memcpy(&u7, &v7, 4);
            read4(ws, 121u, u9, 0, 0);
            read4(cs, 121u | 1u << 20, 120u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
            VERIFY(near(out4[0], 9.0f) && near(out4[1], 120.5f) && pool_gpu_written(pool), "⑬b 句柄 121 写 9.0 → 读 121 得 %.2f（期望 9.00）、读 120 得 %.2f（期望 120.50）；池记 GPU 写过 %d（期望 1）",
                   out4[0], out4[1], pool_gpu_written(pool));
            read4(ws, 122u, u7, 0, 0);
            read4(cs, 122u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
            float first = out4[0];
            read4(ws, 122u, u7, 0, 0);
            read4(cs, 122u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
            VERIFY(!near(first, 7.0f) && near(out4[0], 7.0f) && made(122), "⑬c 没有可写用途、没画过的渲染目标 122：第一次写后读得 %.2f（期望不是 7.00，写进哑图）；升级后再写读得 %.2f（期望 7.00）；已建 %d（期望 1）", first, out4[0], made(122));
            read4(ws, 200u, u7, 0, 0);
            VERIFY(1, "⑬d 句柄 200（没登记）写进可写哑图，提交没报错");
        }
    }
    // ⑭ 采样器按描述计数（老 GPU 缺口 #14：每个采样器号一个采样器对象，参数缓冲能引用的是不同描述的个数，Apple7 / 8 上限 996）。期望手写：
    //    新登记 2000–2099 共 100 个号，参数只有 3 种（各号 lod_bias = 号 % 3），逐个取对象 → 活着的对象 +100、不同描述 +3；
    //    再把 2000（2000 % 3 = 2，bias 2.25）改登记成第 4 种（bias 9.25）并取对象 → 对象数不变、不同描述 +1；
    //    再把 % 3 == 2 的 34 个号（2000、2003 … 2099）全登记成 9.25、逐个取对象 → bias 2.25 那种一个不剩：对象数仍 100、不同描述比上一步 −1
    {
        os_unfair_lock_lock(&g_rmu); uint64_t s0 = g_samp_states, d0 = g_samp_distinct; os_unfair_lock_unlock(&g_rmu);
        samp_par q; memset(&q, 0, sizeof q); q.lod_max = 1000;
        for (int i = 2000; i < 2100; i++) { q.lod_bias = (float)(i % 3) + 0.25f; nvn_be_reg_samp(i, &q); }
        os_unfair_lock_lock(&g_rmu); for (int i = 2000; i < 2100; i++) REQUIRE(samp_by_id(i)); uint64_t s1 = g_samp_states, d1 = g_samp_distinct; os_unfair_lock_unlock(&g_rmu);
        q.lod_bias = 9.25f; nvn_be_reg_samp(2000, &q);
        os_unfair_lock_lock(&g_rmu); REQUIRE(samp_by_id(2000)); uint64_t s2 = g_samp_states, d2 = g_samp_distinct; os_unfair_lock_unlock(&g_rmu);
        int nb0 = 0;
        for (int i = 2000; i < 2100; i++) if (i % 3 == 2) { nvn_be_reg_samp(i, &q); nb0++; }   // bias 2.25 那种（2000 已改走，剩 2003、2006 …）全改成 9.25
        os_unfair_lock_lock(&g_rmu); for (int i = 2000; i < 2100; i++) REQUIRE(samp_by_id(i)); uint64_t s3 = g_samp_states, d3 = g_samp_distinct; os_unfair_lock_unlock(&g_rmu);
        VERIFY(s1 - s0 == 100 && d1 - d0 == 3 && s2 == s1 && d2 - d1 == 1 && s3 == s1 && d3 == d2 - 1 && nb0 == 34,
               "⑭ 采样器按描述计数：100 个号 3 种参数 → 对象 +%llu、描述 +%llu（期望 100、3）；改一个号成第 4 种 → 对象差 %lld、描述 +%llu（期望 0、1）；"
               "bias 2.25 那种的 %d 个号全改走 → 对象差 %lld、描述 %lld（期望 34 个、0、−1）",
               (unsigned long long)(s1 - s0), (unsigned long long)(d1 - d0), (long long)(s2 - s1), (unsigned long long)(d2 - d1), nb0, (long long)(s3 - s1), (long long)d3 - (long long)d2);
    }
    // ⑮ 内存吃紧时放掉闲置的活对象贴图（#1386，nvn_mtl.m tex_idle_reclaim）。期望按语义手写：
    //    150 闲置、存储没被 GPU 写过、不在池表里 → 被放（序号表里没了）；151 闲置但存储上有 GPU 写记录 → 留；152 刚用过 → 留；153 只经池表读过（登记在池表里，GPU 随时会读）→ 留；
    //    放了 1 张；再经池读 150 → 从游戏内存重建，读得 150.50。余量充足（不设假余量）时什么都不放。
    //    负对照：去掉 GPU 写判断 → 151 被放、红；去掉池表判断 → 池表那张被放、红；去掉闲置判断 → 152 被放、红
    if (cs) {
        texture(150, 4, 4, 0); texture(151, 4, 4, 0); texture(152, 4, 4, 0);
        os_unfair_lock_lock(&g_rmu); REQUIRE(tex_by_id(150)); REQUIRE(tex_by_id(151)); REQUIRE(tex_by_id(152)); os_unfair_lock_unlock(&g_rmu);
        gpw_mark(&texs[151], 0, 0, 1, 0, 0, 4, 4);
        texture(153, 4, 4, 0); int pin = 153;   // 只经池表读过、存储没被 GPU 写过：池表登记是唯一挡住它的条件
        read4(cs, 153u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20); read4(cs, 153u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        { __unsafe_unretained id o = tex_get(texs[pin].serial); id<MTLTexture> x = [o isKindOfClass:[NvPkg class]] ? ((NvPkg *)o)->tex : (id<MTLTexture>)o; REQUIRE(x && !x.buffer && tex_pinned((__bridge void *)x));
          uint64_t *rr = NULL, bb = 0; REQUIRE(gpw_stale(texs[pin].mem, texs[pin].L.size, &rr, &bb) == 0); free(rr); }
        empty_sub(); empty_sub(); empty_sub();
        g_tex_idle_subs = 2;
        os_unfair_lock_lock(&g_rmu);
        for (int i = 0; i < g_ntreg; i++) if (g_treg[i].ok) g_treg[i].used = g_fence + 1;   // everything else counts as just used
        g_treg[150].used = g_treg[151].used = 1; for (int h = (int)(uintptr_t)nvn_hm_get(&g_treg_ser, &texs[pin].serial); h; h = g_treg[h - 1].ser_next) g_treg[h - 1].used = 1;
        os_unfair_lock_unlock(&g_rmu);
        uint64_t n0 = g_reclaim_n; g_reclaim_next = 0; empty_sub();
        int quiet = (int)(g_reclaim_n - n0);   // 余量未知（Mac、没设假余量）：不放
        os_unfair_lock_lock(&g_rmu); g_treg[150].used = g_treg[151].used = 1; for (int h = (int)(uintptr_t)nvn_hm_get(&g_treg_ser, &texs[pin].serial); h; h = g_treg[h - 1].ser_next) g_treg[h - 1].used = 1; os_unfair_lock_unlock(&g_rmu);   // 上一次提交里池表重走会把登记着的贴图记成刚用过
        atomic_store(&g_mem_avail_fake, (int64_t)100 << 20); g_reclaim_next = 0; empty_sub();
        int gone150 = !made(150), kept = made(151) && made(152) && made(pin); uint64_t dn = g_reclaim_n - n0;
        read4(cs, 150u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        read4(cs, 150u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        VERIFY(quiet == 0 && gone150 && kept && dn == 1 && near(out4[0], 150.5f) && made(150),
               "⑮ 闲置活对象贴图回收：余量未知时放 %d 张（期望 0）；余量 100 MB 时 150 放了 %d、151 / 152 / 池表 %d 都留 %d、共放 %llu 张（期望 1、1、1）；重建后读 150 得 %.2f（期望 150.50）",
               quiet, gone150, pin, kept, (unsigned long long)dn, out4[0]);
        atomic_store(&g_mem_avail_fake, -1); g_tex_idle_subs = TEX_IDLE_SUBS;
    }
    // ⑯ #674：池表同提交滞留哑图 + 在途 GPU 缓冲原地覆写竞态（画面隔帧亮暗闪烁）
    if (cs) {
        for (int k = 0; k < POOL_TABS; k++) if (g_ptab[k].used && g_ptab[k].rw) pool_tab_drop(k);   // 隔离前面 ⑬ 的可写图像池表
        texture(160, 4, 4, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        texture(161, 4, 4, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        read4(cs, 160u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);   // 标记 160 为着色器已读
        // ⑯a 同提交内：先登记未写目标 → 池表先落哑图；随后同提交建图（模拟渲染遍写入，抬 g_ser_gen）→ 再次 pool_gpu 必须立刻换回真贴图
        nvn_be_sub_begin();
        nvn_be_reg_tex(160, &texs[161], NULL);
        uint64_t e_before = table_entry(160, 0, NULL);
        REQUIRE(tex_mtl(&texs[161]));
        uint64_t e_after = table_entry(160, 0, NULL);
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        uint64_t dum = g_ptab[0].dummy;
        VERIFY(e_before == dum && e_after != dum, "⑯a 同提交内目标写入后池表立刻更新：写前=%#llx(哑=%#llx) 写后=%#llx", (unsigned long long)e_before, (unsigned long long)dum, (unsigned long long)e_after);
        // ⑯b 在途 GPU 读池表期间改登记：旧表缓冲里的项不能被原地改成哑图，应写时拷贝出新缓冲
        texture(162, 4, 4, 0); texture(163, 4, 4, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        REQUIRE(tex_mtl(&texs[162]));
        nvn_be_sub_begin();
        os_unfair_lock_lock(&g_rmu);
        __unsafe_unretained id<MTLBuffer> b_old = nil, b_new = nil;
        pool_gpu(0, (int)MTLTextureType2D, NVN_DPF_FLOAT, 0, 0, &b_old);
        id<MTLBuffer> b_old_keep = b_old;
        uint64_t rid_old = ((uint64_t *)((uint8_t *)b_old_keep.contents + DMSH_POOL_HEAD))[162];
        os_unfair_lock_unlock(&g_rmu);
        // 不等提交完成（GPU 仍在途），改登记 162 成未写目标 163 并再次 pool_gpu
        nvn_be_reg_tex(162, &texs[163], NULL);
        os_unfair_lock_lock(&g_rmu);
        pool_gpu(0, (int)MTLTextureType2D, NVN_DPF_FLOAT, 0, 0, &b_new);
        uint64_t rid_old_after = ((uint64_t *)((uint8_t *)b_old_keep.contents + DMSH_POOL_HEAD))[162];
        uint64_t rid_new_after = ((uint64_t *)((uint8_t *)b_new.contents + DMSH_POOL_HEAD))[162];
        os_unfair_lock_unlock(&g_rmu);
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        VERIFY(rid_old != dum && b_new != b_old_keep && rid_old_after == rid_old && rid_new_after == dum,
               "⑯b 在途池表写时拷贝：旧表保留真图(%d) 新表换缓冲(%d) 新表为哑图(%d)",
               rid_old_after == rid_old, b_new != b_old_keep, rid_new_after == dum);
        // ⑰ pool_seen_scan 快速重查：仅 guest 脏代号变化（dg != g_seen_dirty, sg == g_seen_ser）时跳过非 CPU 写入贴图（view_pkg == 0）；全局串口代号前进时照常全量重查
        os_unfair_lock_lock(&g_rmu);
        pool_seen_scan();   // 同步 g_seen_sub / g_seen_dirty / g_seen_ser
        uint64_t dg_now = nvn_dirty_now();
        int gpu_seen = 0, pkg_seen = 0, total_seen = 0;
        // 找一个已读贴图，强制设一个为 view_pkg=0（GPU 独占），统计当前所有已读 ID 中会被 dg-only 重查的数量
        int gpu_id = -1;
        for (int i = 1; i < g_ntreg; i++) {
            if (!g_treg[i].ok || !pool_seen(i) || (g_seen_todo[(unsigned)i >> 5] >> (i & 31) & 1)) continue;
            if (gpu_id < 0 && g_treg[i].view && g_treg[i].view_ser == g_ser_gen) { gpu_id = i; g_treg[i].view_pkg = 0; }
            total_seen++;
            if (g_treg[i].view && g_treg[i].view_pkg) { g_treg[i].view_dirty = dg_now - 1; pkg_seen++; }
            else gpu_seen++;
        }
        uint64_t u0 = g_pool_upd_seen;
        g_pkg_epoch++; g_seen_dirty--;   // 模拟仅 guest 脏代号前进（新 epoch 下同提交内 !new_sub && recheck_dg）
        pool_seen_scan();
        uint64_t du_dg = g_pool_upd_seen - u0;
        uint64_t u1 = g_pool_upd_seen;
        ser_gen_bump_all();   // 模拟全局串口代号前进（sg_hit 对全量已读生效）
        pool_seen_scan();
        uint64_t du_sg = g_pool_upd_seen - u1;
        uint32_t nw = g_seen_n;
        os_unfair_lock_unlock(&g_rmu);
        VERIFY(gpu_seen > 0 && nw > 0 && (int)du_dg == pkg_seen && (int)du_sg == total_seen,
               "⑰ pool_seen_scan 紧凑重查：seen_n=%u，dg-only 重查 %llu（期望仅 view_pkg=%d，跳过 gpu_only=%d），sg 重查 %llu（期望全量=%d）",
               nw, (unsigned long long)du_dg, pkg_seen, gpu_seen, (unsigned long long)du_sg, total_seen);
        // ⑱ pool_room：按 GPU 真占（不含零拷贝包的游戏内存）对工作集一半、内存余量、单帧 32 MB 放行；新登记贴图首查即入池表不落哑图
        //   期望手算：工作集 28 GB → 上限 14 GB；真占 5 GB 余 9 GB，单帧未花 → 32 MB，已花 28 MB → 4 MB，花满 → 0；真占 15 GB → 0；
        //   余量门槛 = max(余量下限 1 GB, 2 × 回收门槛 1.5 GB) = 3 GB（回收关着 = 1 GB）；余量 2.5 GB（够旧门槛、在回收区间里）→ 0，= 3 GB → 0，3 GB + 256 MB → 32 MB
        {
            const uint64_t ws = 28ull << 30, gpu_ok = 5ull << 30, gpu_hi = 15ull << 30, margin = 1ull << 30, rlo = 1536ull << 20;
            const uint64_t floor = pool_avail_floor(margin, rlo), floor_off = pool_avail_floor(margin, 0);
            uint64_t r_live   = pool_room_calc(gpu_ok, ws, 0, 0, 0, margin);
            uint64_t r_spent  = pool_room_calc(gpu_ok, ws, POOL_FRAME_BYTES - (4u << 20), 0, 0, margin);
            uint64_t r_full   = pool_room_calc(gpu_ok, ws, POOL_FRAME_BYTES, 0, 0, margin);
            uint64_t r_gpu_hi = pool_room_calc(gpu_hi, ws, 0, 0, 0, margin);
            uint64_t r_ios_mid = pool_room_calc(gpu_ok, ws, 0, 1, 2560ull << 20, floor);
            uint64_t r_ios_lo = pool_room_calc(gpu_ok, ws, 0, 1, 3ull << 30, floor);
            uint64_t r_ios_ok = pool_room_calc(gpu_ok, ws, 0, 1, (3ull << 30) + (256u << 20), floor);
            // 设备总量里压进超过工作集二十分之一的零拷贝包（同游戏内存：未碰的匿名映射，不占实存），真占不变：新贴图照样首查即建（原先按设备总量判，这里落哑图）
            size_t pg = (size_t)getpagesize(), big = ((size_t)(g_dev.recommendedMaxWorkingSetSize / 20) + (64u << 20) + pg - 1) & ~(pg - 1);
            void *gm = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); REQUIRE(gm != MAP_FAILED);
            id<MTLBuffer> wrap = nocopy_buf(gm, big, nil); REQUIRE(wrap);
            VERIFY((uint64_t)g_dev.currentAllocatedSize >= g_dev.recommendedMaxWorkingSetSize / 20 && s2a_gpu_alloc_bytes() < g_dev.recommendedMaxWorkingSetSize / 2,
                   "⑱ 前提：设备总量 %llu MB ≥ 工作集二十分之一 %llu MB，真占 %llu MB", (unsigned long long)(g_dev.currentAllocatedSize >> 20),
                   (unsigned long long)(g_dev.recommendedMaxWorkingSetSize / 20 >> 20), (unsigned long long)(s2a_gpu_alloc_bytes() >> 20));
            texture(164, 4, 4, 0);
            uint64_t e164 = table_entry(164, 0, NULL);
            wrap = nil;   // 映射不拆：缓冲由自动释放池收，拆早了 Metal 还引用着
            VERIFY(r_live == POOL_FRAME_BYTES && r_spent == (4u << 20) && r_full == 0 &&
                   r_gpu_hi == 0 && floor == (3ull << 30) && floor_off == margin && r_ios_mid == 0 && r_ios_lo == 0 && r_ios_ok == POOL_FRAME_BYTES && e164 != dum,
                   "⑱ pool_room：live=%lluMB spent=%lluMB full=%llu gpu_hi=%llu floor=%lluMB floor_off=%lluMB ios_mid=%llu ios_lo=%llu ios_ok=%lluMB e164=%#llx(dum=%#llx)",
                   (unsigned long long)(r_live >> 20), (unsigned long long)(r_spent >> 20), (unsigned long long)r_full,
                   (unsigned long long)r_gpu_hi, (unsigned long long)(floor >> 20), (unsigned long long)(floor_off >> 20), (unsigned long long)r_ios_mid, (unsigned long long)r_ios_lo,
                   (unsigned long long)(r_ios_ok >> 20), (unsigned long long)e164, (unsigned long long)dum);
        }
    }
    // ⑱ pool_seen_scan 按串口分桶 + 永久空视图不空转：
    //   - GPU 专用已读贴图（!view_pkg）与视图持续为 NULL 的已读 ID 在脏页代数 / 无关串口桶变化时不触发 pool_seen_scan 重查；
    //   - 负对照 A：自身串口发生变更（serial_put 抬高它自己的 g_sgen_bucket）→ 触发重查（g_pool_upd_seen +1）；
    //   - 负对照 B：CPU 写过的（view_pkg）已读贴图在宿主脏页代数前进且 epoch 翻新后 → 触发重查（g_pool_upd_seen +1）
    if (cs) {
        { struct sigaction sa = {0}; sa.sa_sigaction = on_sig17; sa.sa_flags = SA_SIGINFO; sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); }
        nvn_dirty_add_range(memory, MEM);
        // 170：GPU 专用贴图（IMAGE 用途不走打包、无 layout），选一个与已有 seen ID 不撞桶的串口
        uint64_t s170 = nvn_new_serial();
        for (;;) { int clash = 0; for (uint32_t j = 0; j < g_seen_n; j++) { uint32_t id_ = g_seen_list[j]; if ((int)id_ > 0 && (int)id_ < g_ntreg && g_treg[id_].ok && (g_treg[id_].t.serial & (SGEN_BUCKETS - 1)) == (s170 & (SGEN_BUCKETS - 1))) { clash = 1; break; } } if (!clash) break; s170 = nvn_new_serial(); }
        tex_t *t170 = &texs[170]; memset(t170, 0, sizeof *t170); t170->magic = MAGIC('T', 'E', 'X', 0); t170->serial = s170;
        t170->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F, .width = 4, .height = 4, .depth = 1, .levels = 1, .flags = NVN_TEXTURE_FLAGS_IMAGE};
        nvn_tex_layout(&t170->d, &t170->L); size_t off170 = (mem_next + 0xffff) & ~(size_t)0xffff; REQUIRE(off170 + t170->L.size <= MEM); mem_next = off170 + t170->L.size;
        t170->pool = pool; t170->off = (int64_t)off170; t170->mem = pool->mem + off170;
        nvn_be_tex_init(t170); nvn_be_reg_tex(170, t170, NULL);
        read4(cs, 170u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        // 172：永久无法建出视图的已读 ID（BC 格式但视图指定非 BC 格式，tex_by_id_ptr 必回 NULL）
        tex_t *t172 = &texs[172]; memset(t172, 0, sizeof *t172); t172->magic = MAGIC('T', 'E', 'X', 0); t172->serial = nvn_new_serial();
        t172->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA_DXT1, .width = 4, .height = 4, .depth = 1, .levels = 1};
        nvn_tex_layout(&t172->d, &t172->L); size_t off172 = (mem_next + 0xffff) & ~(size_t)0xffff; REQUIRE(off172 + t172->L.size <= MEM); mem_next = off172 + t172->L.size;
        t172->pool = pool; t172->off = (int64_t)off172; t172->mem = pool->mem + off172;
        view_t v172; memset(&v172, 0, sizeof v172); v172.set_format = 1; v172.format = NVN_FORMAT_RGBA8;
        nvn_be_tex_init(t172); nvn_be_reg_tex(172, t172, &v172);
        read4(cs, 172u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);
        // 171：CPU 写过（view_pkg = 1）的已读贴图
        texture(171, 4, 4, 0); nvn_be_tex_cpu_written(&texs[171]);
        read4(cs, 171u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20, 3u | 1u << 20);

        // 先走两轮把 todo / g_ser_gen 稳定下来，确认 170(!view_pkg) / 171(view_pkg) / 172(!view)
        empty_sub(); nvn_be_sub_begin(); (void)table_entry(170, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        REQUIRE(g_treg[170].view && !g_treg[170].view_pkg && g_treg[171].view && g_treg[171].view_pkg && !g_treg[172].view);
        size_t pgoff = (mem_next + 0x3fff) & ~(size_t)0x3fff; REQUIRE(pgoff + 0x4000 <= MEM); mem_next = pgoff + 0x4000;
        nvn_dirty_protect(memory + pgoff, 0x4000); memory[pgoff] ^= 1;
        nvn_be_sub_begin(); g_pkg_epoch++; (void)table_entry(170, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }

        // 先测：只抬一个与所有已有 seen ID 不同桶的串口（170、172 都不能被重查）
        uint64_t other_ser = nvn_new_serial();
        for (;;) { int clash = 0; for (uint32_t j = 0; j < g_seen_n; j++) { uint32_t id_ = g_seen_list[j]; if ((int)id_ > 0 && (int)id_ < g_ntreg && g_treg[id_].ok && (g_treg[id_].t.serial & (SGEN_BUCKETS - 1)) == (other_ser & (SGEN_BUCKETS - 1))) { clash = 1; break; } } if (!clash) break; other_ser = nvn_new_serial(); }
        serial_put(other_ser, [NSObject new]);
        uint64_t upd0 = g_pool_upd_seen;
        nvn_be_sub_begin(); (void)table_entry(170, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        uint64_t du_other = g_pool_upd_seen - upd0;

        // 同 epoch 下再触一次真写保护故障（先在该提交内让所有 view_pkg ID 同步 view_ep == g_pkg_epoch，再同一提交/epoch 内再脏一次）：170(!view_pkg) / 172(!view) / 同epoch已查的171 都不得重查
        nvn_dirty_protect(memory + pgoff, 0x4000); memory[pgoff] ^= 4;
        nvn_be_sub_begin(); g_pkg_epoch++; (void)table_entry(170, 0, NULL);   // recheck_dg=1，让所有 view_pkg 的 view_ep 同步到当前 g_pkg_epoch
        nvn_dirty_protect(memory + pgoff, 0x4000); memory[pgoff] ^= 8;
        upd0 = g_pool_upd_seen;
        (void)table_entry(170, 0, NULL);
        uint64_t du_dirty_same_ep = g_pool_upd_seen - upd0;
        { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }

        // 负对照 A：170 自身串口变更 → +1
        id<MTLTexture> x170 = tex_get(t170->serial); REQUIRE([x170 conformsToProtocol:@protocol(MTLTexture)]);
        serial_put(t170->serial, x170);
        upd0 = g_pool_upd_seen;
        nvn_be_sub_begin(); (void)table_entry(170, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        uint64_t du_own = g_pool_upd_seen - upd0;

        // 负对照 B：脏页代数前进 + g_pkg_epoch 翻新 → 触发 view_pkg(171) 重查
        nvn_dirty_protect(memory + pgoff, 0x4000); memory[pgoff] ^= 2;
        uint64_t dirty_upd0 = g_pool_upd_seen;
        nvn_be_sub_begin(); g_pkg_epoch++; (void)table_entry(170, 0, NULL); { uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); }
        uint64_t du_dirty = g_pool_upd_seen - dirty_upd0;

        VERIFY(du_other == 0 && du_dirty_same_ep == 0 && du_own == 1 && du_dirty >= 1,
               "⑱ pool_seen_scan 串口分桶与永久 NULL 跳过：无关串口桶更新=%llu(应0) 同epoch脏页更新=%llu(应0) 自身串口桶更新=%llu(应1) 脏页+epoch触发pkg更新=%llu(应>=1)",
               (unsigned long long)du_other, (unsigned long long)du_dirty_same_ep, (unsigned long long)du_own, (unsigned long long)du_dirty);
        serial_del(other_ser);
    }
    printf("%s t_nvn_pool_tex：%u 项，%u 项失败（追不到槽的句柄按编号读池：池表 + 已读位图 + 未写渲染目标不建 + 小贴图预建 + 表复用）\n", fails ? "✗" : "✓", checks, fails);
    return fails != 0;
} }
