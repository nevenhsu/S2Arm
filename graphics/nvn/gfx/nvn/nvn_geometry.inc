// General geometry replay: native VS capture -> GS compute -> ordered index
// compaction -> ordinary rendering. Included after the resource/pipeline helpers.
#define GEO_RED(...) do { nvn_be_bad_draw_feat(); MLOG(__VA_ARGS__); } while (0)   // S2Arm: a geometry draw dropped counts for the picture check (mtl_bad_draws)
static NvProg *g_geo_program;
static uint64_t g_geo_indices, g_geo_index_bytes, g_geo_args;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void lc_land(void);

// ---------- S2Arm transform feedback past the vertex stage (10-26, Xenoblade 3: particle geometry shader c01e3ade8e15a842 captures 11 outputs, then DrawTransformFeedback) ----------
// A geometry shader's capture: geo_tf after the compaction, the kernels in nvn_geometry.h. How many vertices it emitted is known on the GPU only, so from the first such capture in a
// Begin the offsets live in g_tf_cnt (bytes per buffer); before it they are the host's tf_off (vertex captures, tf_draw). End / Pause keep a Begin's counts by its control buffer
// (g_tfs: copied on the GPU when they are there); DrawTransformFeedback(mode, address) draws bytes / stride vertices of the buffer whose entry the address names, 8 bytes per buffer
// from the control buffer (taken for the NVN driver's 32-byte control buffer: 4 buffers; the first case is logged with the alternatives, to be checked), stride = vertex stream 0's
// (the hardware's DrawAuto stride), the capture's when that is 0
static id<MTLBuffer> g_tf_cnt, g_tf_snapbuf; static uint64_t g_tf_cnt_gpu;
static id<MTLComputePipelineState> g_tf_ps[4];   // geo_tf_write, geo_tf_count, tf_draw_args, tf_snap
static uint32_t g_tf_last_stride[4];             // bytes per vertex of the latest capture into each buffer
enum { TFS_N = 64 };
static struct { uint64_t ctl; int gpu; uint32_t bytes[4], stride[4]; } g_tfs[TFS_N]; static int g_tfs_next;
static uint64_t g_tfd_args;   // the DrawTransformFeedback being drawn: GPU address of its MTLDrawPrimitivesIndirectArguments (draw)
static int tf_init(void) {
    if (g_tf_ps[3]) return 1;
    static int failed; if (failed) return 0;
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_nvn_geo_tf_msl) options:nil error:&e];
    const char *nm[4] = {"geo_tf_write", "geo_tf_count", "tf_draw_args", "tf_snap"};
    for (int i = 0; lib && i < 4; i++) g_tf_ps[i] = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(nm[i])] error:&e];
    g_tf_cnt = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; g_tf_snapbuf = [g_dev newBufferWithLength:16 * TFS_N options:MTLResourceStorageModeShared];
    if (!g_tf_ps[3] || !g_tf_cnt || !g_tf_snapbuf) { failed = 1; GEO_RED("【红】 transform feedback kernels: %s", e ? e.localizedDescription.UTF8String : "no buffer"); return 0; }
    res_add(g_tf_cnt); res_add(g_tf_snapbuf); g_tf_cnt_gpu = g_tf_cnt.gpuAddress;
    return 1;
}
// After geo_pack_indices (rec = the output records, idx / args = the compacted list and its count): write each feedback buffer the geometry shader captures into
static int geo_tf(NvShader *gs, uint64_t rec, uint64_t idx, uint64_t args, uint64_t capacity) {
    if (!tf_init()) return 0;
    nvn_geo_tf j = {.rec = rec, .idx = idx, .args = args, .cnt = g_tf_cnt_gpu, .use_cnt = (uint32_t)S.tf_gpu, .rec_words = gs->geo_stride,
                    .per_prim = gs->geo_output == 1 ? 1 : gs->geo_output == 6 ? 2 : 3};
    for (int b = 0; b < 4; b++) {
        if (!gs->geo_tf_stride[b]) continue;
        void *mb; uint64_t mo, g;
        if (!S.tf[b].addr || !S.tf[b].size || !mtl_gpu_addr(S.tf[b].addr, S.tf[b].size, &mb, &mo, &g)) {
            GEO_RED("【红】 geometry transform feedback buffer %d of %s is unbound or in no memory pool (%#llx + %llu): not captured", b, gs->key, (unsigned long long)S.tf[b].addr, (unsigned long long)S.tf[b].size); return 0; }
        j.dst[b] = g; j.size[b] = (uint32_t)MIN(S.tf[b].size, UINT32_MAX); j.stride[b] = gs->geo_tf_stride[b]; j.start[b] = (uint32_t)S.tf_off[b]; j.ne[b] = gs->geo_tf_n[b];
        memcpy(j.off[b], gs->geo_tf_off[b], sizeof j.off[b]); memcpy(j.comp[b], gs->geo_tf_comp[b], sizeof j.comp[b]);
        g_tf_last_stride[b] = gs->geo_tf_stride[b];
        pend_add(S.tf[b].addr, S.tf[b].size, 4);   // how far it reaches is known on the GPU only: the whole buffer
    }
    id<MTLBuffer> ab; size_t ao = arena_alloc(sizeof j, &ab); memcpy((uint8_t *)ab.contents + ao, &j, sizeof j); uint64_t jg = ab.gpuAddress + ao;
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, g_tf_ps[0]); enc_c_args(ce, jg, 0);
    enc_c_dispatch(ce, MTLSizeMake((capacity + 63) / 64, 1, 1), MTLSizeMake(64, 1, 1)); enc_c_end(ce);
    ce = enc_c_begin(NULL); enc_c_pso(ce, g_tf_ps[1]); enc_c_args(ce, jg, 0); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(ce);
    S.tf_gpu = 1;
    static int once; if (!once++) MLOG("transform feedback: first geometry capture (%s, buffer 0 %u bytes per vertex, %u entries)", gs->key, gs->geo_tf_stride[0], gs->geo_tf_n[0]);
    return 1;
}
// Replay of End / Pause (before state_op): keep this Begin's counts under its control buffer
static void tf_state_end(const p_tf_state *b) {
    if ((b->what != 1 && b->what != 2) || !S.tf_on) return;
    int k = g_tfs_next; g_tfs_next = (g_tfs_next + 1) % TFS_N;
    g_tfs[k].ctl = b->ctl ? b->ctl : S.tf_ctl; g_tfs[k].gpu = S.tf_gpu;
    for (int i = 0; i < 4; i++) { g_tfs[k].bytes[i] = (uint32_t)S.tf_off[i]; g_tfs[k].stride[i] = g_tf_last_stride[i]; }
    if (!S.tf_gpu || !tf_init()) return;
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    nvn_tf_args j = {.cnt = g_tf_cnt_gpu, .args = g_tf_snapbuf.gpuAddress + 16 * (uint64_t)k};
    id<MTLBuffer> ab; size_t ao = arena_alloc(sizeof j, &ab); memcpy((uint8_t *)ab.contents + ao, &j, sizeof j);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, g_tf_ps[3]); enc_c_args(ce, ab.gpuAddress + ao, 0);
    enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(ce);
}
// DrawTransformFeedback at replay: the indirect arguments (GPU address, 0 = nothing to draw); draw then draws them (g_tfd_args)
static uint64_t tfd_args(const p_draw *d) {
    int k = -1; uint64_t a = d->tfc;
    for (int i = 0; i < TFS_N && k < 0; i++) { int x = (g_tfs_next - 1 - i + TFS_N) % TFS_N; if (g_tfs[x].ctl && a >= g_tfs[x].ctl && a < g_tfs[x].ctl + 32) k = x; }
    if (k < 0 && a == 1) k = (g_tfs_next - 1 + TFS_N) % TFS_N;   // no address given: the latest
    if (k < 0 || !g_tfs[k].ctl) { nvn_be_bad_draw_feat(); RED_ONCE(127, "DrawTransformFeedback: counter address %#llx is in no ended capture's control buffer: not drawn", (unsigned long long)a); return 0; }
    uint32_t b = (uint32_t)((a - g_tfs[k].ctl) / 8) & 3, stride = S.stream[0].stride ? S.stream[0].stride : g_tfs[k].stride[b];
    static int once; if (!once++) MLOG("DrawTransformFeedback: first draw (mode %d, counter %#llx = control buffer %#llx + %llu → buffer %u; vertex stream 0 stride %u, capture stride %u; counts %s)",
        d->prim, (unsigned long long)a, (unsigned long long)g_tfs[k].ctl, (unsigned long long)(a - g_tfs[k].ctl), b, S.stream[0].stride, g_tfs[k].stride[b], g_tfs[k].gpu ? "on the GPU" : "on the CPU");
    if (!stride) { nvn_be_bad_draw_feat(); RED_ONCE(121, "DrawTransformFeedback: no stride (vertex stream 0 and the capture both 0): not drawn"); return 0; }
    id<MTLBuffer> ab; size_t ao = arena_alloc(32, &ab); uint8_t *p = (uint8_t *)ab.contents + ao; uint64_t g = ab.gpuAddress + ao;
    if (!g_tfs[k].gpu) { uint32_t arg[4] = {g_tfs[k].bytes[b] / stride, 1, 0, 0}; memcpy(p, arg, 16); return arg[0] ? g : 0; }
    if (!tf_init()) return 0;
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    nvn_tf_args j = {.cnt = g_tf_snapbuf.gpuAddress + 16 * (uint64_t)k, .args = g, .b = b, .stride = stride}; memcpy(p + 16, &j, sizeof j);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, g_tf_ps[2]); enc_c_args(ce, g + 16, 0);
    enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(ce);
    return g;
}

static id<MTLRenderPipelineState> geo_capture_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = GEO_MARK;
    id<MTLRenderPipelineState> ps = hm_get_id(&g_pso, &k);
    if (ps || hm_get_id(&g_pso_fail, &k)) return ps;
    NSError *e = nil;
    ps = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&k, vs, nil, nil, 0) error:&e];
    if (ps) hm_put_id(&g_pso, &k, ps);
    else { hm_put_id(&g_pso_fail, &k, NSNull.null); MLOG("【红】 geometry vertex capture %s: %s", vs->key, e.localizedDescription.UTF8String); }
    return ps;
}
// ---------- NVN2 task / mesh draws: nvnCommandBufferDrawMeshTasks / DrawMeshTasksIndirect (11-05, The Legend of Zelda: Ocarina of Time 0400486024DEC000's 3D scenes) ----------
// A program's task and mesh shaders are Metal object and mesh functions (the translation's [[object]] / [[mesh]] main0, mesh main0_z for near = 0), kept at stages 3 / 4 (nvn2_cmd.h
// nvn2_stage_host; NvShader.mesh_kind tells them from hull / domain shaders). The draw goes through draw() with a program of its pixel stage alone, as a geometry draw's raster stage
// does, so it takes the render pass, the depth / blend / viewport state and the fragment arguments like any draw; draw() asks mtk_pso for its pipeline and mtk_encode binds and draws.
// The mesh stage shares the vertex stage's argument table (it has no vertex fetch: vertex-table slot 29 is vertex stream 13's, VB_SLOT, rebound through the enc_r_vbuf cache by the
// next vertex draw), a task shader has the object table g_ot; both read the draw's first workgroup at buffer MESH_FIRST_SLOT (interface contract item 3). Indirect: the GPU-written
// {count, first} become MTLDispatchThreadgroupsIndirectArguments {count, 1, 1} with first at word 4 on the GPU (ixc_run), so neither is read on the CPU.
// A pipeline is built in the background (g_mtk_q, mtk_kick); until it is ready the draw is skipped and counted (g_mtk_wait) when skip_safe's rules allow it, otherwise the draw waits
//   for the build within a per-frame budget (mtk_sync_why, mtk_wait); a refused one is reported red once and its draws skipped.
// After the upstream reference (MIT) 1d4b8e82b / #1312 (MIT): mesh_tasks_draw / mtk_pso / mtk_encode, written for this host's two backends, recording and binding state
enum { MESH_FIRST_SLOT = 29 };
_Static_assert(VB_SLOT(13) == MESH_FIRST_SLOT, "slot 29 of the vertex table is vertex stream 13's: a mesh draw writes it through enc_r_vbuf so the next vertex draw rebinds the stream (see above)");
static struct { int on, miss; NvShader *ts, *ms; uint32_t first, count; uint64_t ind, emu; __unsafe_unretained id<MTLFunction> emu_fn; uint32_t emu_vbo, emu_ibo, emu_iblen; } g_mtk;   // emu: #770 the exchange buffer of a draw whose task / mesh stages the host ran (mtk_emu_run), the vertex function that draws its records   // the draw being encoded (on: draw() is drawing it)
typedef struct { pso_key k; const void *ts; uint32_t pad, emu; } mtk_key;   // pso_key with the mesh shader in its vertex place, the task shader (the pipeline's third function), and the user(locn) mask of the padded mesh variant used (0 = none)
static nvn_hmap g_pso_mtk = {.klen = sizeof(mtk_key)}; static os_unfair_lock g_pso_mtk_lock = OS_UNFAIR_LOCK_INIT;
static NSObject *g_pso_mtk_building; static dispatch_queue_t g_mtk_q; static dispatch_group_t g_mtk_group;
// 管线额度（nvn_pso_budget.inc ⑦，D-EH7W）：网格管线也是活着的管线。g_pb_mtk = 表里真管线的条数（不含在建占位、拒建的 NSNull）；
//   放进表一律经 mtk_put_locked、画到经 pb_touch_m（都在 g_pso_mtk_lock 下），超额时与其他表一起按最久没用先放
static _Atomic uint32_t g_pb_mtk;
static void pb_built_m(const mtk_key *k); static void pb_touch_m(const mtk_key *k);
static inline int mtk_real(id v) { return v && v != NSNull.null && v != g_pso_mtk_building; }
static void mtk_put_locked(const mtk_key *k, id v) {
    int was = mtk_real(hm_get_id(&g_pso_mtk, k)), is = mtk_real(v);
    hm_put_id(&g_pso_mtk, k, v);
    if (is && !was) atomic_fetch_add(&g_pb_mtk, 1); else if (was && !is) atomic_fetch_sub(&g_pb_mtk, 1);
    if (is) pb_built_m(k);
}
static uint64_t g_mtk_n, g_mtk_ind_n, g_mtk_wait, g_mtk_skip;   // drawn (of them indirect), skipped while the pipeline was built, skipped as not drawable (red)
// #770 实验开关 S2A_MESH_PACE（缺省 0 = 关，照旧建好即交付）：M1 Max（没有硬件网格着色，驱动走软件网格）换图时 GPU 挂死，都紧跟某条网格管线交付、
//   且发生在多条网格管线排队待交付时；跳过网格绘制不挂；清掉系统缓存全部现编照样挂（作者 11-12 反例，所以不是缓存）。两档各验一个假设：
//   1 = 建好的先攒着，后台一条都不在编了才一起交付（假设：新管线首次上 GPU 时编译服务还在编下一条软件网格管线 → 挂）；
//   2 = 每帧最多交付一条（假设：同一帧里几条新管线首次上 GPU → 挂）。交付和每条管线第一次被画都记帧号与还在编的条数，挂死前最后几行能对上是哪种
static int mtk_pace(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("S2A_MESH_PACE"); v = e && (*e == '1' || *e == '2') && !e[1] ? *e - '0' : 0;
    if (v) MLOG("task / mesh pipelines: pace %d (S2A_MESH_PACE: %s)", v, v == 1 ? "deliver only when no mesh pipeline is still building" : "deliver at most one per frame");
    return v;
}
static _Atomic int g_mtk_inflight;   // mesh pipelines queued or building in the background
static NSMutableArray *g_mtk_ready;   // pace on: built, not delivered yet (each @[NSData key, pipeline or NSNull]); under g_pso_mtk_lock
static uint64_t g_mtk_pub_frame = UINT64_MAX;
static NSMutableSet *g_mtk_drawn;   // pipelines already drawn once (first-draw log)
static NSMapTable *g_mtk_names;   // pipeline → "task … mesh … fragment …" for the #770 logs (not the pipeline's label: -label is also a Metal 4 encoder selector, t_nvn_rec_cover); under g_pso_mtk_lock
static const char *mtk_name(id p) { if (!p || p == NSNull.null) return "(refused)"; os_unfair_lock_lock(&g_pso_mtk_lock); NSString *n = [g_mtk_names objectForKey:p]; os_unfair_lock_unlock(&g_pso_mtk_lock); return n ? n.UTF8String : "?"; }
static void mtk_publish(void) {   // pace on, submit thread, before a mesh draw looks its pipeline up
    int pace = mtk_pace(); if (!pace || !g_mtk_ready.count) return;
    uint64_t f = atomic_load_explicit(&g_cur_frame, memory_order_relaxed); int fly = atomic_load(&g_mtk_inflight);
    if (pace == 1 && fly) return;
    if (pace == 2 && f == g_mtk_pub_frame) return;
    os_unfair_lock_lock(&g_pso_mtk_lock);
    NSUInteger n = pace == 2 ? 1 : g_mtk_ready.count;
    for (NSUInteger i = 0; i < n; i++) { NSArray *r = g_mtk_ready[i]; mtk_key kk; [(NSData *)r[0] getBytes:&kk length:sizeof kk]; mtk_put_locked(&kk, r[1]); }
    NSArray *done = [g_mtk_ready subarrayWithRange:NSMakeRange(0, n)]; [g_mtk_ready removeObjectsInRange:NSMakeRange(0, n)]; NSUInteger left = g_mtk_ready.count;
    os_unfair_lock_unlock(&g_pso_mtk_lock);
    g_mtk_pub_frame = f;
    for (NSArray *r in done) MLOG("task / mesh pipeline delivered (pace %d) at frame %llu: %s; still building %d, built and held %lu", pace, (unsigned long long)f,
                                  mtk_name(r[1]), fly, (unsigned long)left);
}
// ---------- #770 宿主自跑任务 / 网格阶段（S2A_MESH_EMU） ----------
// M1 / M2（没有硬件网格着色，不支持 MTLGPUFamilyApple9）上驱动的软件网格在带任务的网格绘制上挂死（M1 Pro 11-17 二分：只留任务 de3befe4936cac83 / 网格
//   e6759f45b660ec01 那对就挂；网格输出 0 个图元不挂；任务交宿主、网格仍交驱动照挂；苹果论坛 708654 / FB10367407 同类）。这些 GPU 上两段都由宿主跑：
//   任务计算核 main0_tc 把每个任务组的载荷与派生数写进交换缓冲（S2A.Dsh MeshStage EMU_SLOT），排表核 mxe_scan 前缀和出「第几个网格组属于哪个任务组的第几组」，
//   网格计算核 main0_mc 把顶点记录与紧排的索引写进交换缓冲，再用顶点函数 main0_mv 普通索引间接画。都在同一个命令缓冲里、GPU 上排好，CPU 不回读。
//   入口只在宿主现编时定义 S2A_MESH_EMU 才有（dm_shader_base_cached：本机翻译器出的基础源码），其中的循环有圈数上限（MeshStage.GuardLoops）。
//   缺省：GPU 不支持 Apple9 时开（各平台，#1542）；S2A_MESH_EMU=1 / 0 两头强制。做不了的（源码拿不到、参数缓冲对不上）开着时跳过并报红（#1542 安全阀，不再落回驱动），关着（Apple9 起）照旧走驱动。
//   载荷超 16 KB（S2A.Dsh MeshStage.Relay）也由宿主跑：载荷本来就在交换缓冲（设备内存），网格超 32 KB 的阶段内存放交换缓冲头 [23] 起的片区，一个网格组一片（mesh_relay_sw 字）。
//   原先中转对在这里一律不收、开着时整笔跳过：水上场景近海那一对（载荷 27904 B）在 M1 / M2 上整片不画、水面发黑
//   带逐图元输出的网格：翻译器把每个三角形展开成 3 个顶点记录（dm_xvertex_t，带该三角形的逐图元属性），顶点区按 MaxP × 3 个留（g_mxe_vs 第 0 位）
enum { MXE_SLOT = 28, MXE_HEAD = 32, MXE_TASK_CAP = 4096 };
_Static_assert(VB_SLOT(12) == MXE_SLOT, "slot 28 of the vertex table is vertex stream 12's: the host-stage draw writes it through enc_r_vbuf so the next vertex draw rebinds the stream");
static int mtk_emu_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("S2A_MESH_EMU");
    // By GPU family (#1542): on iOS below Apple9 there is no hardware mesh shading; on macOS Apple Silicon (Mac2+) native task/mesh with pass splitting (DM_NVN_MESH_PASS_SPLIT=2)
    //   eliminates the 30s compute prebuild, halves render-target load/store traffic, and runs with zero GPU hangs, so macOS Mac2+/Apple9+ defaults to native while iOS < Apple9 stays emulated.
#if TARGET_OS_IPHONE
    int dflt = ![g_dev supportsFamily:MTLGPUFamilyApple9];
#else
    int dflt = !([g_dev supportsFamily:MTLGPUFamilyApple9] || [g_dev supportsFamily:MTLGPUFamilyMac2]);
#endif
    v = e && *e ? *e == '1' : dflt;
    MLOG("task / mesh draws: the task and mesh stages run %s (S2A_MESH_EMU %s)", v ? "on the host as compute kernels, drawn as an indexed draw of their vertex records (#770)" : "in the driver's object stage", e && *e ? e : "unset");
    return v;
}
// DM_NVN_MESH_PASS_SPLIT: on Apple GPUs before Apple9 (M1 / M2), mixing native task/mesh draws and ordinary draws in one render pass can hang the GPU.
//   2 (default on < Apple9 when running natively): end render pass only where mesh and ordinary draws meet, so consecutive mesh draws share one pass;
//   1: own render pass per mesh draw; 0 (default on Apple9+): share render passes.
static int mtk_split_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_MESH_PASS_SPLIT");
        v = e && *e ? atoi(e) : [g_dev supportsFamily:MTLGPUFamilyApple9] ? 0 : 2;
        if (v < 0 || v > 2) v = 1;
        MLOG("task / mesh native pass split: %s (DM_NVN_MESH_PASS_SPLIT=%d%s)",
             v == 1 ? "own render pass per mesh draw" : v == 2 ? "separate runs from ordinary draws" : "share render passes",
             v, e && *e ? "" : ", by GPU family");
    }
    return v;
}
static void mtk_pass_before_plain(void) {
    if (!mtk_emu_on() && mtk_split_on() == 2 && g_enc && g_mtk_pass_mesh) rp_end();
}
static void mtk_pass_mark_plain(void) {
    if (!mtk_emu_on() && mtk_split_on() == 2 && g_enc) g_mtk_pass_plain = 1;
}
static const char k_mxe_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void mxe_scan(device uint* h [[buffer(0)]], device const uint* ind [[buffer(1)]], uint l [[thread_position_in_threadgroup]], uint nt [[threads_per_threadgroup]],\n"
"                     uint sg [[simdgroup_index_in_threadgroup]], uint nsg [[simdgroups_per_threadgroup]]) {\n"
"  threadgroup uint part[32];\n"
"  uint n = min(h[6] ? h[6] : ind[0], h[3]);\n"
"  uint per = (n + nt - 1) / nt, a = min(l * per, n), b = min(a + per, n), s = 0;\n"
"  for (uint t = a; t < b; t++) s += h[h[0] + t];\n"
"  uint x = simd_prefix_exclusive_sum(s), tot = simd_sum(s);\n"   // 两级 SIMD 前缀和：256 线程，线程 0 只串各 SIMD 组的和（原先 1024 线程、线程 0 串行 1024 圈）
"  if (simd_is_first()) part[sg] = tot;\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  if (l == 0) { uint acc = 0; for (uint i = 0; i < nsg; i++) { uint v = part[i]; part[i] = acc; acc += v; } uint m = min(acc, h[4]); h[8] = m; h[9] = 1; h[10] = 1; h[13] = 0; h[14] = 1; h[15] = 0; h[16] = 0; h[17] = 0; h[18] = acc; h[19] = h[6] ? h[6] : ind[0]; }\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  uint o = part[sg] + x;\n"
"  for (uint t = a; t < b; t++) { uint c = h[h[0] + t]; for (uint j = 0; j < c && o + j < h[4]; j++) h[h[2] + o + j] = (t << 10) | j; o += c; }\n"
"}\n"
// #1542: an indirect task dispatch with the game's workgroup counts as given had no bound (capT only bounds what is written), so one command buffer could run into the GPU
//   watchdog (M1 Pro rebooted). mxe_tclamp copies them clamped to x * y * z <= capT into h[20..22], and the task stage is dispatched from there
"kernel void mxe_tclamp(device uint* h [[buffer(0)]], device const uint* ind [[buffer(1)]]) {\n"
"  uint cap = max(h[3], 1u), x = min(ind[0], cap), y = min(ind[1], max(cap / max(x, 1u), 1u)), z = min(ind[2], max(cap / max(x * y, 1u), 1u));\n"
"  h[20] = x; h[21] = y; h[22] = z;\n"
"}\n"
// A mesh draw without a task stage (#1542: these went to the driver's mesh stage, which hangs on these GPUs): its mesh workgroups, clamped to the exchange's room, and the indexed draw's arguments
"kernel void mxe_monly(device uint* h [[buffer(0)]], device const uint* ind [[buffer(1)]]) {\n"
"  ulong n = (ulong)ind[0] * ind[1] * ind[2];\n"
"  h[8] = (uint)min(n, (ulong)h[4]); h[9] = 1; h[10] = 1; h[13] = 0; h[14] = 1; h[15] = 0; h[16] = 0; h[17] = 0; h[18] = (uint)min(n, (ulong)0xffffffffu); h[19] = 0;\n"
"}\n";
enum { MXE_SCAN, MXE_TCLAMP, MXE_MONLY };
static id<MTLComputePipelineState> mxe_pso(int w) {   // the host stages' own kernels (k_mxe_msl), built together on first use
    static id<MTLComputePipelineState> ps[3]; static int failed; if (ps[0] || failed) return failed ? nil : ps[w];
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_mxe_msl) options:nil error:&e];
    static const char *const names[3] = {"mxe_scan", "mxe_tclamp", "mxe_monly"};
    for (int i = 0; i < 3 && lib; i++) if (!(ps[i] = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(names[i])] error:&e])) break;
    if (!ps[0] || !ps[1] || !ps[2]) { failed = 1; ps[0] = ps[1] = ps[2] = nil; MLOG("【红】 task / mesh host stages: their kernels failed: %s", e.localizedDescription.UTF8String); return nil; }
    return ps[w];
}
static id<MTLComputePipelineState> mxe_scan_pso(void) { return mxe_pso(MXE_SCAN); }
// 宿主自跑阶段的库：同一份基础源码（键 + 阶段）只编一次，网格的 main0_mc / main0_mv 共用；用过的记进 mesh_emu.tsv（pso_user.tsv 旁），下一局开局在后台先编
//   （一款动作冒险游戏：首个 3D 场景 103 次现编合计约 70 s 都落在提交线程上，影片后一帧停 55 s，每局一样；Metal 不替 newLibraryWithSource 留跨局缓存）。
//   g_mxl 值：@[库, 源码]、NSNull（做不了）或在编的 dispatch_group（别的线程等它，不重编）；g_mxp：键/入口 → 后台预建好的计算管线
static NSMutableDictionary *g_mxl, *g_mxp; static os_unfair_lock g_mxl_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableSet *g_mxl_listed; static _Atomic uint64_t g_mxl_wait_ns, g_mxl_hits, g_mxl_built_bg;
static NSString *mxl_list_path(void) { NSString *p = pso_user_path(); return p ? [p.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"mesh_emu.tsv"] : nil; }
// 网格计算核写索引时在 GPU 上剔三角形（DM_NVN_MESH_CULL=1 开，默认关）：只剔一定画不出任何采样点的——三个顶点号有相同的（零面积）、三个角都在同一裁剪面外或都在眼后、
//   目标单采样且视口为整像素时包围盒（两边各放宽 1/256 像素）里没有像素中心。被剔的三角形原位写成退化三角形（三个索引都是它第一个角），本组索引的个数、顺序不变；
//   整组都被剔时不写顶点记录、不占索引。宿主现编时改写 main0_mc / main0_mc_z 的索引循环；网格源码有逐图元输出（dm_prim_t / dm_xvertex_t）、写视口号或层号时不改写。
//   判据在 k_mxc_keep_msl（单测 t_nvn_mesh_emu 用同一段源码在 GPU 上跑，和 CPU 复刻逐条比）；视口与开关经交换缓冲头 [24..28] 传进去
static int mxc_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_NVN_MESH_CULL"); m = e && *e == '1';
    if (m) MLOG("DM_NVN_MESH_CULL=1: mesh host stages turn triangles that cover no sample into degenerate ones"); } return m; }
static const char k_mxc_keep_msl[] =
"static inline bool dm_cull_keep(float4 a, float4 b, float4 c, uint ia, uint ib, uint ic, float4 cv, uint cf) {\n"
"    if (!(cf & 1u)) return true;\n"
"    if (ia == ib || ib == ic || ia == ic) return false;\n"
"    if ((a.w <= 0.0 && b.w <= 0.0 && c.w <= 0.0) || (a.x < -a.w && b.x < -b.w && c.x < -c.w) || (a.x > a.w && b.x > b.w && c.x > c.w)\n"
"        || (a.y < -a.w && b.y < -b.w && c.y < -c.w) || (a.y > a.w && b.y > b.w && c.y > c.w)) return false;\n"
"    if ((cf & 2u) && a.w > 0.0 && b.w > 0.0 && c.w > 0.0) {\n"
"        float2 hs = cv.zw * 0.5, s0 = cv.xy + hs + a.xy / a.w * hs, s1 = cv.xy + hs + b.xy / b.w * hs, s2 = cv.xy + hs + c.xy / c.w * hs;\n"
"        float2 lo = min(s0, min(s1, s2)) - 1.0 / 256.0, hi = max(s0, max(s1, s2)) + 1.0 / 256.0;\n"
"        if (any(ceil(lo - 0.5) > floor(hi - 0.5))) return false;\n"
"    }\n"
"    return true;\n"
"}\n";
static NSString *mxc_patch(NSString *src, const char *key) {
    for (NSString *x in @[@"dm_prim_t", @"dm_xvertex_t", @"[[viewport_array_index]]", @"[[render_target_array_index]]", @"[[primitive_id]]"])
        if ([src containsString:x]) return src;   // 逐图元输出、多视口 / 分层：不剔
    static NSRegularExpression *rx; static dispatch_once_t once;
    dispatch_once(&once, ^{ rx = [NSRegularExpression regularExpressionWithPattern:
        @"    threadgroup uint dm_ibase;\n    if \\((\\w+) == 0u\\) dm_ibase = atomic_fetch_add_explicit\\(\\(device atomic_uint\\*\\)\\(dm_xo \\+ 13\\), np \\* 3u, memory_order_relaxed\\);\n"
         "    threadgroup_barrier\\(mem_flags::mem_threadgroup\\);\n    device uint\\* dm_ib = dm_xo \\+ dm_xo\\[12\\] \\+ dm_ibase; uint dm_v0 = (\\w+) \\* (\\d+)u;\n"
         "    for \\(uint n = \\w+; n < np \\* 3u; n \\+= (\\d+)u\\)\n    \\{\n        uint at = (\\d+)u \\+ n;\n"
         "        dm_ib\\[n\\] = dm_v0 \\+ min\\(\\((\\w+)\\[at >> 2\\] >> \\(\\(at & 3u\\) \\* 8u\\)\\) & 0xffu, (\\d+)u\\);\n    \\}\n" options:0 error:nil]; });
    NSArray<NSTextCheckingResult *> *ms = [rx matchesInString:src options:0 range:NSMakeRange(0, src.length)];
    if (!ms.count) return src;
    NSMutableString *out = [src mutableCopy]; int done = 0;
    for (NSTextCheckingResult *m in ms.reverseObjectEnumerator) {
        NSString *(^g)(NSUInteger) = ^NSString *(NSUInteger i) { return [src substringWithRange:[m rangeAtIndex:i]]; };
        NSString *lid = g(1), *tg = g(2), *maxv = g(3), *thr = g(4), *prim = g(5), *mem = g(6), *maxv1 = g(7);
        NSRange before = NSMakeRange(0, m.range.location);
        NSRange npr = [src rangeOfString:@"    uint np = min(" options:NSBackwardsSearch range:before];
        NSRange br = [src rangeOfString:@"        uint b = " options:NSBackwardsSearch range:before];
        NSRange pr = [src rangeOfString:@"        v.position = " options:NSBackwardsSearch range:before];
        if (npr.location == NSNotFound || br.location == NSNotFound || pr.location == NSNotFound || br.location < npr.location || pr.location < npr.location) continue;
        NSRange bl = [src lineRangeForRange:br], pl = [src lineRangeForRange:pr], nl = [src lineRangeForRange:npr];
        NSString *bexp = [[src substringWithRange:NSMakeRange(br.location + br.length, bl.location + bl.length - br.location - br.length)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString *pexp = [[src substringWithRange:NSMakeRange(pr.location + pr.length, pl.location + pl.length - pr.location - pr.length)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (![bexp hasSuffix:@";"] || ![pexp hasSuffix:@";"]) continue;
        bexp = [bexp substringToIndex:bexp.length - 1]; pexp = [pexp substringToIndex:pexp.length - 1];
        bexp = [[NSRegularExpression regularExpressionWithPattern:@"\\bn\\b" options:0 error:nil] stringByReplacingMatchesInString:bexp options:0 range:NSMakeRange(0, bexp.length) withTemplate:@"dm_c"];
        NSString *npline = [src substringWithRange:nl];
        NSRange mp = [npline rangeOfString:@", " options:NSBackwardsSearch]; NSString *maxp = mp.location == NSNotFound ? @"256" : [[npline substringFromIndex:mp.location + 2] stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"u);\n "]];
        if (maxp.intValue <= 0 || maxp.intValue > 1024) continue;
        // 剔：每个三角形按三个角的位置（同顶点循环的算法）判一次，记进 dm_kf；留下的个数 dm_kn 为 0 就整组返回
        NSString *cull = [NSString stringWithFormat:
            @"    threadgroup uchar dm_kf[%@]; threadgroup atomic_uint dm_kn;\n"
             "    const float4 dm_cv = as_type<float4>(uint4(dm_xo[24], dm_xo[25], dm_xo[26], dm_xo[27])); const uint dm_cf = dm_xo[28];\n"
             "    if (%@ == 0u) atomic_store_explicit(&dm_kn, 0u, memory_order_relaxed);\n"
             "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
             "    for (uint p = %@; p < np; p += %@u)\n    {\n"
             "        float4 dm_q[3]; uint dm_ci[3];\n"
             "        for (uint k = 0u; k < 3u; k++) { uint at = %@u + p * 3u + k; uint dm_c = min((%@[at >> 2] >> ((at & 3u) * 8u)) & 0xffu, %@u); dm_ci[k] = dm_c; uint b = %@; dm_q[k] = %@; }\n"
             "        bool keep = dm_cull_keep(dm_q[0], dm_q[1], dm_q[2], dm_ci[0], dm_ci[1], dm_ci[2], dm_cv, dm_cf);\n"
             "        dm_kf[p] = keep ? 1 : 0;\n"
             "        if (keep) atomic_fetch_add_explicit(&dm_kn, 1u, memory_order_relaxed);\n"
             "    }\n"
             "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
             "    if (np == 0u || atomic_load_explicit(&dm_kn, memory_order_relaxed) == 0u) return;\n",
            maxp, lid, lid, thr, prim, mem, maxv1, bexp, pexp];
        // 写索引：被剔的三角形三个索引都写成它第一个角（退化），其余照原样
        NSString *wr = [NSString stringWithFormat:
            @"    threadgroup uint dm_ibase;\n    if (%@ == 0u) dm_ibase = atomic_fetch_add_explicit((device atomic_uint*)(dm_xo + 13), np * 3u, memory_order_relaxed);\n"
             "    threadgroup_barrier(mem_flags::mem_threadgroup);\n    device uint* dm_ib = dm_xo + dm_xo[12] + dm_ibase; uint dm_v0 = %@ * %@u;\n"
             "    for (uint n = %@; n < np * 3u; n += %@u)\n    {\n        uint at = %@u + (dm_kf[n / 3u] ? n : n / 3u * 3u);\n"
             "        dm_ib[n] = dm_v0 + min((%@[at >> 2] >> ((at & 3u) * 8u)) & 0xffu, %@u);\n    }\n",
            lid, tg, maxv, lid, thr, prim, mem, maxv1];
        [out replaceCharactersInRange:m.range withString:wr];
        [out insertString:cull atIndex:nl.location + nl.length];
        done++;
    }
    if (!done) return src;
    NSRange k = [out rangeOfString:@"kernel void main0_mc("]; if (k.location == NSNotFound) return src;
    [out insertString:@(k_mxc_keep_msl) atIndex:k.location];   // 判据定义放在第一个宿主入口前（同在 S2A_MESH_EMU 段里）
    return out;
}
// 网格阶段内存放设备内存：main0_mc 的阶段内存（一款动作冒险游戏的草地网格 11–13 KB）占满 M1 每核 32 KB 线程组内存，一核只驻 2 个工作组。
//   同中转载荷的做法（MeshStage startEmu）整段改放交换缓冲 [23] 起的片区，一组一片，线程组内存只剩几百字节（同一局 GPU p50 −1.7 ms）。
//   另出一份库（阶段号带 0x100），入口改名 main0_md / main0_md_z，驱动的 [[mesh]] 入口不编；用过的同样记进 mesh_emu.tsv 开局预建。
//   改写不成（没有线程组阶段内存、源码对不上）就照旧用 main0_mc。g_mxd_sw：键 → 一片的字数。DM_NVN_MESH_DEV=1 开（默认关）
static NSMutableDictionary *g_mxd_sw;
static int mxd_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_MESH_DEV"); v = e && *e == '1'; if (v) MLOG("DM_NVN_MESH_DEV=1: mesh host stages keep their stage memory in device memory"); } return v; }
// 设备内存那份每组多占一片阶段内存（dsw 字）：交换缓冲有上限（budget 字节），本笔能放的网格组数因此比线程组那份（每组 p0 字节）少时，本笔改回 main0_mc。
//   原先照样换成 main0_md，多出的组被截掉不画（11-25 帧回放：蕨类网格整丛不画、画面泛白，全关逐像素差均值 30）
static int mxd_fits(uint64_t cap, uint64_t budget, uint64_t p0, uint32_t dsw) {
    return MIN(cap, budget / (p0 + (uint64_t)dsw * 4)) >= MIN(cap, budget / p0);
}
static NSString *mxd_patch(NSString *src, const char *key) {
    static NSRegularExpression *rx, *rt; static dispatch_once_t once;
    dispatch_once(&once, ^{ rx = [NSRegularExpression regularExpressionWithPattern:@"\n    threadgroup spvUnsafeArray<uint, (\\d+)> (\\w+);\n" options:0 error:nil];
        rt = [NSRegularExpression regularExpressionWithPattern:@"\\(dm_xo \\+ dm_xo\\[11\\]\\) \\+ (\\w+) \\* \\d+u;" options:0 error:nil]; });
    NSRange mc = [src rangeOfString:@"kernel void main0_mc("]; if (mc.location == NSNotFound) return nil;
    NSTextCheckingResult *m = [rx firstMatchInString:src options:0 range:NSMakeRange(mc.location, src.length - mc.location)]; if (!m) return nil;   // 中转载荷的网格本来就在设备内存
    NSTextCheckingResult *t = [rt firstMatchInString:src options:0 range:NSMakeRange(mc.location, src.length - mc.location)]; if (!t) return nil;
    int words = [[src substringWithRange:[m rangeAtIndex:1]] intValue], sw = (words + 3) & ~3;
    NSString *mem = [src substringWithRange:[m rangeAtIndex:2]], *tg = [src substringWithRange:[t rangeAtIndex:1]];
    NSMutableString *o = [src mutableCopy];
    [o replaceOccurrencesOfString:[NSString stringWithFormat:@"\n    threadgroup spvUnsafeArray<uint, %d> %@;\n", words, mem]
                       withString:[NSString stringWithFormat:@"\n    device spvUnsafeArray<uint, %d>& %@ = *(device spvUnsafeArray<uint, %d>*)(dm_xo + dm_xo[23] + (ulong)%@ * %du);\n", words, mem, words, tg, sw] options:0 range:NSMakeRange(0, o.length)];
    [o replaceOccurrencesOfString:[NSString stringWithFormat:@"threadgroup spvUnsafeArray<uint, %d>&", words] withString:[NSString stringWithFormat:@"device spvUnsafeArray<uint, %d>&", words] options:0 range:NSMakeRange(0, o.length)];
    [o replaceOccurrencesOfString:@"(threadgroup atomic_uint*)" withString:@"(device atomic_uint*)" options:0 range:NSMakeRange(0, o.length)];
    [o replaceOccurrencesOfString:@"mem_flags::mem_threadgroup" withString:@"mem_flags::mem_device | mem_flags::mem_threadgroup" options:0 range:NSMakeRange(0, o.length)];
    NSRange ms = [o rangeOfString:@"[[mesh]] void main0("], em = [o rangeOfString:@"\n#ifdef S2A_MESH_EMU\n" options:NSBackwardsSearch];
    if (ms.location == NSNotFound || em.location == NSNotFound || em.location < ms.location) return nil;
    [o insertString:@"\n#endif\n" atIndex:em.location]; [o insertString:@"#if 0\n" atIndex:ms.location];
    [o replaceOccurrencesOfString:@"kernel void main0_mc(" withString:@"kernel void main0_md(" options:0 range:NSMakeRange(0, o.length)];
    [o replaceOccurrencesOfString:@"kernel void main0_mc_z(" withString:@"kernel void main0_md_z(" options:0 range:NSMakeRange(0, o.length)];
    os_unfair_lock_lock(&g_mxl_lock); if (!g_mxd_sw) g_mxd_sw = [NSMutableDictionary dictionary]; g_mxd_sw[@(key)] = @(sw); os_unfair_lock_unlock(&g_mxl_lock);
    return o;
}
static NSArray *mxl_build(const char *key, int st, char *why, size_t nw) {
    int dev = st >> 8; st &= 0xff;
    uint8_t md[8] = {0}; for (int i = 0; i < 8; i++) { unsigned b = 0; if (sscanf(key + 2 * i, "%2x", &b) != 1) break; md[i] = (uint8_t)b; }
    char path[1200];
    if (dm_shader_base_cached(md, st, path, sizeof path)) { snprintf(why, nw, "no translation of it on this device"); return nil; }
    NSString *src = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    if (!src) { snprintf(why, nw, "its translation could not be read"); return nil; }
    NSString *plain = src;
    if (st == 5 && mxc_mode()) src = mxc_patch(src, key);
    if (dev) { src = st == 5 ? mxd_patch(src, key) : nil; if (!src) { snprintf(why, nw, "no threadgroup stage memory to move"); return nil; } }
    MTLCompileOptions *co = dm_msl_opts(); co.preprocessorMacros = @{@"S2A_MESH_EMU": @1};
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:co error:&e];
    if (!lib && !dev && src != plain) {   // 剔三角形的改写编不过：照原样编（只是不剔），日志记一笔
        MLOG("【黄】 task / mesh host stages: %s with triangle culling failed to build (%s); built as translated", key, e.localizedDescription.UTF8String);
        src = plain; e = nil; lib = [g_dev newLibraryWithSource:src options:co error:&e]; }
    if (!lib) { snprintf(why, nw, "failed to build: %s", e.localizedDescription.UTF8String); return nil; }
    return @[lib, src];
}
// (键, 阶段) 的库与源码：后台编好的直接拿；在编的等它；都没有就在这里编（首局照旧）。bg：开局预编调的——编不成不记失败，留给绘制时照原路再试
static NSArray *mxl_get(const char *key, int st, char *why, size_t nw, int bg) {
    NSString *k = [NSString stringWithFormat:@"%s/%d", key, st];
    os_unfair_lock_lock(&g_mxl_lock); if (!g_mxl) g_mxl = [NSMutableDictionary dictionary];
    id v = g_mxl[k];
    if (!v) {
        dispatch_group_t g = dispatch_group_create(); dispatch_group_enter(g); g_mxl[k] = g;
        os_unfair_lock_unlock(&g_mxl_lock);
        NSArray *r = mxl_build(key, st, why, nw);
        os_unfair_lock_lock(&g_mxl_lock); if (r || !bg) g_mxl[k] = r ?: (id)NSNull.null; else [g_mxl removeObjectForKey:k]; os_unfair_lock_unlock(&g_mxl_lock);
        dispatch_group_leave(g);
        if (bg && r) atomic_fetch_add(&g_mxl_built_bg, 1);
        return r;
    }
    os_unfair_lock_unlock(&g_mxl_lock);
    if (![v isKindOfClass:NSArray.class] && v != NSNull.null) {   // 在编：等它
        uint64_t t0 = bp_now(); dispatch_group_wait((dispatch_group_t)v, DISPATCH_TIME_FOREVER); atomic_fetch_add(&g_mxl_wait_ns, bp_now() - t0);
        os_unfair_lock_lock(&g_mxl_lock); v = g_mxl[k]; os_unfair_lock_unlock(&g_mxl_lock);
    } else if (!bg) atomic_fetch_add(&g_mxl_hits, 1);
    if (!v && !bg) return mxl_get(key, st, why, nw, 0);   // 等的是一次没编成的预编（已撤掉）：自己编
    if (v == NSNull.null || ![v isKindOfClass:NSArray.class]) { if (!why[0]) snprintf(why, nw, "no translation of it on this device"); return nil; }
    return v;
}
static void mxl_record(const char *key, int st, NSString *entry) {
    NSString *path = mxl_list_path(); if (!path) return;
    NSString *line = [NSString stringWithFormat:@"%s\t%d\t%@", key, st, entry];
    os_unfair_lock_lock(&g_mxl_lock);
    if (!g_mxl_listed) g_mxl_listed = [NSMutableSet set];
    BOOL seen = [g_mxl_listed containsObject:line]; if (!seen) [g_mxl_listed addObject:line];
    os_unfair_lock_unlock(&g_mxl_lock);
    if (seen) return;
    FILE *f = fopen(path.fileSystemRepresentation, "a"); if (!f) return;
    fprintf(f, "%s\n", line.UTF8String); fclose(f);
}
static int mtk_emu_on(void);
// 开局（warm_start）：按上一局记下的清单在后台两路先编库、预建计算管线
static void mtk_emu_prebuild(void) {
    NSString *path = mxl_list_path(); if (!path) return;
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil]; if (!txt.length) return;
    if (!mtk_emu_on()) return;
    NSMutableArray *rows = [NSMutableArray array]; NSMutableSet *libs = [NSMutableSet set];
    for (NSString *l in [txt componentsSeparatedByString:@"\n"]) {
        NSArray *c = [l componentsSeparatedByString:@"\t"]; if (c.count != 3 || [c[0] length] < 16) continue;
        [rows addObject:c]; [libs addObject:[NSString stringWithFormat:@"%@/%@", c[0], c[1]]];
    }
    os_unfair_lock_lock(&g_mxl_lock);   // warm_start 不一定在提交线程上（换目录时重起预热），跟 mxl_record 共用一把锁
    if (!g_mxl_listed) g_mxl_listed = [NSMutableSet set];
    for (NSArray *c in rows) [g_mxl_listed addObject:[c componentsJoinedByString:@"\t"]];
    os_unfair_lock_unlock(&g_mxl_lock);
    if (!rows.count) return;
    uint64_t t0 = bp_now(); NSUInteger nrows = rows.count, nlibs = libs.count;
    MLOG("task / mesh host stages: prebuilding %lu libraries (%lu entries) from %s in the background", (unsigned long)nlibs, (unsigned long)nrows, path.UTF8String);
    dispatch_group_t g = dispatch_group_create(); dispatch_semaphore_t sem = dispatch_semaphore_create(2);
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    __block _Atomic int nlib = 0, npso = 0;
    for (NSArray *c in rows) dispatch_group_async(g, q, ^{ @autoreleasepool {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        if (!dm_gfx_session_stopping()) {
            char why[300] = ""; const char *key = [c[0] UTF8String]; int st = [c[1] intValue]; NSString *entry = c[2];
            NSArray *r = (st & 0x100) && !mxd_on() ? nil : mxl_get(key, st, why, sizeof why, 1);   // main0_md 的行只在开着设备内存时预建
            if (r) { atomic_fetch_add(&nlib, 1);
                if (st == 4 || [entry hasPrefix:@"main0_mc"] || [entry hasPrefix:@"main0_md"]) { id<MTLFunction> f = lib_entry(r[0], entry); NSError *e = nil;
                    id<MTLComputePipelineState> ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
                    if (ps) { os_unfair_lock_lock(&g_mxl_lock); if (!g_mxp) g_mxp = [NSMutableDictionary dictionary]; g_mxp[[NSString stringWithFormat:@"%@/%@", c[0], entry]] = ps; os_unfair_lock_unlock(&g_mxl_lock); atomic_fetch_add(&npso, 1); } } }
        }
        dispatch_semaphore_signal(sem);
    } });
    dispatch_group_notify(g, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MLOG("task / mesh host stages: prebuild done in %.1f s: %lu entries, %d with a library, %d compute pipelines ready (libraries built in the background %llu)",
             (bp_now() - t0) / 1e9, (unsigned long)nrows, (int)nlib, (int)npso, (unsigned long long)atomic_load(&g_mxl_built_bg));
    });
}
static NSMutableDictionary *g_mxe_vs;   // mesh shader key → its vertex record's bytes
static NSMutableDictionary *g_mxe;   // shader key + entry → MTLFunction (task: its compute pipeline) or NSNull (cannot); submit thread
static id mtk_emu_get(NvShader *sh, int st, NSString *entry) {
    NSString *k = [NSString stringWithFormat:@"%s/%@", sh->key, entry];
    if (!g_mxe) g_mxe = [NSMutableDictionary dictionary];
    id got = g_mxe[k]; if (got) return got == NSNull.null ? nil : got;
    char why[300] = ""; id r = nil; uint64_t t0 = bp_now(); NSError *e = nil;
    do {
        NSArray *ls = mxl_get(sh->key, st, why, sizeof why, 0); if (!ls) break;
        id<MTLLibrary> lib = ls[0]; NSString *src = ls[1];
        if (![src containsString:entry]) { snprintf(why, sizeof why, "its translation has no %s (a relayed payload, or the exchange slot is taken)", entry.UTF8String); break; }
        id<MTLFunction> f = lib_entry(lib, entry);
        NSUInteger al = f && sh->rf.nb && ![entry isEqualToString:@"main0_mv"] ? [f newArgumentEncoderWithBufferIndex:sh->rf.abi].encodedLength : sh->arg_len;   // main0_mv takes the vertex records alone
        if (!f || al != sh->arg_len) { snprintf(why, sizeof why, "%s", f ? "it takes another argument buffer than the library's" : "no such entry"); break; }
        if (st == 4 || [entry hasPrefix:@"main0_mc"] || [entry hasPrefix:@"main0_md"]) {
            os_unfair_lock_lock(&g_mxl_lock); r = g_mxp[k]; os_unfair_lock_unlock(&g_mxl_lock);   // 开局后台预建的（mtk_emu_prebuild）
            if (!r) { r = [g_dev newComputePipelineStateWithFunction:f error:&e]; if (!r) snprintf(why, sizeof why, "its compute pipeline failed: %s", e.localizedDescription.UTF8String); } }
        else r = f;
        if (r && st == 5) {   // the vertex record's stride: the float4 members of dm_vertex_t, or of dm_xvertex_t (per-primitive outputs: one record a triangle corner) (S2A.Dsh MeshStage)
            int x = [src containsString:@"struct dm_xvertex_t {"];
            NSRange a = [src rangeOfString:x ? @"struct dm_xvertex_t {" : @"struct dm_vertex_t {"]; NSRange z = a.location == NSNotFound ? a : [src rangeOfString:@"};" options:0 range:NSMakeRange(a.location, src.length - a.location)];
            NSUInteger nf = 0; if (z.location != NSNotFound) nf = [[src substringWithRange:NSMakeRange(a.location, z.location - a.location)] componentsSeparatedByString:@"float4 "].count - 1;
            if (!g_mxe_vs) g_mxe_vs = [NSMutableDictionary dictionary]; g_mxe_vs[@(sh->key)] = @(nf * 16 | (x ? 1 : 0));   // bit 0: expanded (MaxP × 3 records a mesh workgroup)
        }
    } while (0);
    g_mxe[k] = r ?: NSNull.null;
    if (r) { mxl_record(sh->key, st, entry);   // 阶段号带 0x100 的（main0_md）也记，下一局一起预建
        MLOG("task / mesh host stages: %s %s %s ready in %.1f ms (prebuilt libraries used %llu, waited for the background %.1f ms in all)", st == 4 ? "task" : "mesh", sh->key, entry.UTF8String, (bp_now() - t0) / 1e6,
             (unsigned long long)atomic_load(&g_mxl_hits), atomic_load(&g_mxl_wait_ns) / 1e6); }
    else MLOG("【黄】 task / mesh host stages: %s %s %s: %s; its draws stay on the driver's mesh pipeline (#770)", st == 4 ? "task" : "mesh", sh->key, entry.UTF8String, why);
    return r;
}
static int mtk_emu_ready(NvShader *ts, NvShader *ms) {   // the stages' pipelines (first use: built here, on the submit thread); ts nil: a mesh draw without a task stage (#1542)
    if (!ms || !mtk_emu_on() || !mxe_scan_pso() || (ts && !mtk_emu_get(ts, 4, @"main0_tc"))) return 0;
    if (ms->mesh_relay && !ms->mesh_relay_sw) return 0;   // a relayed mesh without its slice size: its stage memory has nowhere to go
    int z = [ms->ventry hasSuffix:@"_z"];
    return mtk_emu_get(ms, 5, z ? @"main0_mc_z" : @"main0_mc") && mtk_emu_get(ms, 5, @"main0_mv");
}
typedef struct { __unsafe_unretained id<MTLComputePipelineState> tp, mp; uint64_t tg, mg, first, xg, ind; NSUInteger ta, ma; int tok, mok; uint32_t capT, tt, mt; } mxe_job;
// Exchange buffer sizes from what this pair needed before (the scan's raw counts read back on completion): the buffer is frame scratch, and sizing every draw for the worst case
//   (8 MB a draw, 155 mesh draws a frame in one busy scene) made the arena take ~1 GB a frame: 5 fps. Too small once = that draw loses its later mesh groups that frame (counted, not drawn),
//   and the next draw of the pair is sized from it
// 网格绘制入几何连段（mesh_run_ok）。DM_NVN_MESH_RUN=1 开（默认关）
static int mesh_run_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_MESH_RUN"); v = e && *e == '1';
        if (v) MLOG("DM_NVN_MESH_RUN=1: consecutive task / mesh draws share a geometry run (host stages in its compute lane, draws in its main pass)"); } return v; }
static NSMutableDictionary *g_mxe_need; static os_unfair_lock g_mxe_need_lock = OS_UNFAIR_LOCK_INIT; static _Atomic uint64_t g_mxe_short;
static void mtk_emu_need(NSString *k, uint32_t *tasks, uint32_t *meshes) {
    os_unfair_lock_lock(&g_mxe_need_lock); NSArray *v = g_mxe_need[k]; os_unfair_lock_unlock(&g_mxe_need_lock);
    *tasks = v ? [v[0] unsignedIntValue] : 0; *meshes = v ? [v[1] unsignedIntValue] : 0;
}
static void mtk_emu_saw(NSString *k, uint32_t tasks, uint32_t meshes) {
    os_unfair_lock_lock(&g_mxe_need_lock); if (!g_mxe_need) g_mxe_need = [NSMutableDictionary dictionary];
    NSArray *v = g_mxe_need[k]; uint32_t t0 = v ? [v[0] unsignedIntValue] : 0, m0 = v ? [v[1] unsignedIntValue] : 0;
    if (tasks > t0 || meshes > m0 || !v) g_mxe_need[k] = @[@(MAX(tasks, t0)), @(MAX(meshes, m0))];
    os_unfair_lock_unlock(&g_mxe_need_lock);
}
// The exchange buffers come from a ring of their own (S2A_MESH_EMU_RING_MB, default 256), not the frame arena: one draw can need 20 MB of vertex records, and arena blocks that
//   size were made anew every frame (footprint 4.4 → 10.8 GB, 5 fps). A region is reused once the submission that used it has completed (g_done); a request the ring cannot hold yet
//   falls back to the arena
static id<MTLBuffer> g_mxr; static uint64_t g_mxr_len, g_mxr_head; static struct { uint64_t lo, hi, fence; } g_mxr_q[512]; static int g_mxr_qh, g_mxr_qn; static uint64_t g_mxr_fallback;
static size_t mxe_alloc(size_t n, __strong id<MTLBuffer> *buf) {
    n = (n + 255) & ~(size_t)255;
    if (!g_mxr) { const char *e = getenv("S2A_MESH_EMU_RING_MB"); g_mxr_len = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
        g_mxr = [g_dev newBufferWithLength:g_mxr_len options:MTLResourceStorageModeShared]; if (g_mxr) res_add(g_mxr); }
    uint64_t done = rc_done();
    while (g_mxr_qn && g_mxr_q[g_mxr_qh].fence <= done) { g_mxr_qh = (g_mxr_qh + 1) % 512; g_mxr_qn--; }
    if (g_mxr && n <= g_mxr_len / 2 && g_mxr_qn < 511) {
        uint64_t lo = g_mxr_head + n > g_mxr_len ? 0 : g_mxr_head, hi = lo + n; int clash = 0;
        for (int i = 0, k = g_mxr_qh; i < g_mxr_qn && !clash; i++, k = (k + 1) % 512) clash = lo < g_mxr_q[k].hi && g_mxr_q[k].lo < hi;
        if (!clash) { int k = (g_mxr_qh + g_mxr_qn++) % 512; g_mxr_q[k].lo = lo; g_mxr_q[k].hi = hi; g_mxr_q[k].fence = g_fence + 1; g_mxr_head = hi; *buf = g_mxr; return (size_t)lo; }
    }
    if (!(g_mxr_fallback++ & 1023)) MLOG("task / mesh host stages: exchange ring full (%llu MB), %llu requests went to the frame arena", (unsigned long long)(g_mxr_len >> 20), (unsigned long long)g_mxr_fallback);
    return arena_alloc(n, buf);
}
static int mtk_emu_cap(NvShader *sh, int st, int vert, uint64_t *gpu, NSUInteger *ai) {   // the argument block this stage binds now, captured (bind_args)
    g_args_cap_ok = 0; g_args_compute = 2; encode_args(nil, sh, st, vert); g_args_compute = 0;
    *gpu = g_args_cap_gpu; *ai = g_args_cap_ai; return g_args_cap_ok;
}
// One draw's host task / mesh stages: its exchange buffer and its bindings now (j); what the draw then needs (e). 0 = not eligible (the draw goes the driver's way)
static int mtk_emu_prep(const p_draw *dw, NvShader *ts, NvShader *ms, uint64_t ind, mxe_job *j) {
    if (!mtk_emu_ready(ts, ms)) return 0;
    int z = [ms->ventry hasSuffix:@"_z"];
    id<MTLComputePipelineState> tp = ts ? mtk_emu_get(ts, 4, @"main0_tc") : nil, mp = mtk_emu_get(ms, 5, z ? @"main0_mc_z" : @"main0_mc");
    id<MTLComputePipelineState> mp0 = mp; uint32_t dsw = 0;   // 阶段内存在设备内存的那份（mxd_patch）：一组一片 dsw 字；mp0 = 线程组那份
    if (!ms->mesh_relay && mxd_on()) { id<MTLComputePipelineState> dp = mtk_emu_get(ms, 5 | 0x100, z ? @"main0_md_z" : @"main0_md");
        os_unfair_lock_lock(&g_mxl_lock); dsw = [g_mxd_sw[@(ms->key)] unsignedIntValue]; os_unfair_lock_unlock(&g_mxl_lock);
        if (dp && dsw) mp = dp; else dsw = 0; }
    id<MTLFunction> vf = mtk_emu_get(ms, 5, @"main0_mv");
    uint32_t vsx = [g_mxe_vs[@(ms->key)] unsignedIntValue], vs = vsx & ~1u, maxp = MAX(ms->mesh_maxp, 1u), maxv = vsx & 1 ? maxp * 3 : MAX(ms->mesh_maxv, 1u);   // expanded: one record a triangle corner
    if (!vs) return 0;
    static uint64_t budget; if (!budget) { const char *ev = getenv("S2A_MESH_EMU_MB"); budget = (uint64_t)(ev && atoi(ev) > 0 ? atoi(ev) : 32) << 20; }   // the most one draw's exchange buffer may take
    NSString *nk = [NSString stringWithFormat:@"%s/%s", ts ? ts->key : "-", ms->key]; uint32_t sawT, sawM; mtk_emu_need(nk, &sawT, &sawM);
    // no task stage (#1542): no task workgroups, payloads or scan; the mesh workgroups are the draw's own (direct: its count; indirect: as many as seen before, mxe_monly clamps)
    uint32_t capT = !ts ? 0 : ind ? MIN(MAX(sawT + sawT / 4 + 16, 64u), MXE_TASK_CAP) : (uint32_t)MIN((uint32_t)dw->count, 65536u), pw = ts ? MAX(((uint32_t)ts->mesh_payload + 3) / 4, 1u) : 0;
    uint32_t capM = ts ? MIN(capT * 32u, MAX(sawM + sawM / 2 + 64, 256u))   // a task workgroup derives at most its thread count of mesh workgroups in these games (ballot + popcount); raise first-draw floor above 64 so MKW/similar task->mesh draws do not truncate on first sight
                       : ind ? MAX(sawM + sawM / 4 + 32, 64u) : (uint32_t)MAX(MIN((uint32_t)dw->count, 65536u), 1u);
    if (dsw && !mxd_fits(capM, budget, (uint64_t)maxv * vs + (uint64_t)maxp * 12, dsw)) { dsw = 0; mp = mp0; }
    uint64_t sw = ms->mesh_relay ? ms->mesh_relay_sw : dsw;   // relayed payload: one stage-memory slice a mesh workgroup (its stage memory is past Metal's 32 KB of threadgroup memory)
    uint64_t per = (uint64_t)maxv * vs + (uint64_t)maxp * 12 + sw * 4; capM = (uint32_t)MAX(1, MIN((uint64_t)capM, budget / per));
    uint64_t w0 = MXE_HEAD + capT + (uint64_t)capT * pw + capM, vbo = (w0 + 3) & ~3ull, ibo = vbo + (uint64_t)capM * maxv * (vs / 4), sl = (ibo + (uint64_t)capM * maxp * 3 + 3) & ~3ull,
             words = sl + (uint64_t)capM * sw;
    id<MTLBuffer> xb; size_t xo = mxe_alloc((size_t)words * 4, &xb); uint32_t *h = (uint32_t *)((uint8_t *)xb.contents + xo); uint64_t xg = xb.gpuAddress + xo;
    memset(h, 0, MXE_HEAD * 4); h[0] = MXE_HEAD; h[1] = MXE_HEAD + capT; h[2] = MXE_HEAD + capT + capT * pw; h[3] = capT; h[4] = capM; h[5] = pw; h[6] = ind ? 0 : (uint32_t)dw->count;
    h[7] = maxp * 3; h[11] = (uint32_t)vbo; h[12] = (uint32_t)ibo; h[23] = sw ? (uint32_t)sl : 0;
    if (mxc_mode()) {   // 视口（存储像素）与开关，见 mxc_patch
        const tex_t *t = R.has_rt && R.rt.n > 0 && R.rt.color[0] ? R.rt.color[0] : R.has_rt ? R.rt.depth : NULL; uint32_t lv = 0;
        if (t == (R.has_rt && R.rt.n > 0 ? R.rt.color[0] : NULL)) lv = t && R.rt.has_cview[0] && R.rt.cview[0].set_levels ? R.rt.cview[0].base_level : 0; else lv = t && R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0;
        double rs = t ? rs_of(tex_mtl(t)) : 1.0; float v[4];
        if (S.has_vp) { v[0] = S.vp[0][0]; v[1] = S.vp[0][1]; v[2] = S.vp[0][2]; v[3] = S.vp[0][3]; } else { v[0] = v[1] = 0; v[2] = t ? (float)MAX(t->d.width >> lv, 1u) : 1; v[3] = t ? (float)MAX(t->d.height >> lv, 1u) : 1; }
        if (rs != 1.0) for (int i = 0; i < 4; i++) v[i] = (float)(v[i] * rs);
        int px = t && t->d.samples <= 1 && g_wd <= 0 && v[2] > 0 && v[3] > 0;
        for (int i = 0; i < 4 && px; i++) px = v[i] == floorf(v[i]);
        memcpy(h + 24, v, 16); h[28] = 1u | (px ? 2u : 0u); }
    if (!ts && !ind) { h[8] = MIN((uint32_t)dw->count, capM); h[9] = h[10] = 1; h[14] = 1; h[18] = (uint32_t)dw->count; }   // a direct mesh draw without a task stage: its groups and draw arguments, as mxe_monly writes them for an indirect one
    uint64_t first = ind ? ind + 16 : 0;
    if (!first) { id<MTLBuffer> b; size_t o = arena_alloc(16, &b); uint32_t f0 = (uint32_t)dw->first; memcpy((uint8_t *)b.contents + o, &f0, 4); first = b.gpuAddress + o; }
    *j = (mxe_job){.tp = tp, .mp = mp, .first = first, .xg = xg, .ind = ind, .capT = capT, .tt = ts ? ts->mesh_threads : 0, .mt = ms->mesh_threads};
    j->tok = ts ? mtk_emu_cap(ts, 3, 0, &j->tg, &j->ta) : 0;
    j->mok = mtk_emu_cap(ms, 4, 1, &j->mg, &j->ma);
    if ((ts && ts->arg_len && !j->tok) || (ms->arg_len && !j->mok)) return 0;   // no argument block captured: a stale one would stay bound
    memset(h + MXE_HEAD, 0, (size_t)capT * 4);   // counts of task workgroups that write none (past the cap) read 0
    { id<MTLBuffer> keep = xb; const uint32_t *hh = h; uint32_t ct = capT, cm = capM;   // learn the sizes once the GPU has run it
      [g_commit_blocks addObject:^{ (void)keep; mtk_emu_saw(nk, hh[19], hh[18]); if (hh[18] > cm || hh[19] > ct) { uint64_t n = atomic_fetch_add(&g_mxe_short, 1) + 1;
          if (!(n & (n - 1))) MLOG("task / mesh host stages: %s needed %u task / %u mesh workgroups, the exchange held %u / %u (%llu such draws so far; the next draw of the pair is sized for it)", nk.UTF8String, hh[19], hh[18], ct, cm, (unsigned long long)n); } }]; }
    g_mtk.emu = xg; g_mtk.emu_fn = vf; g_mtk.emu_vbo = (uint32_t)(vbo * 4); g_mtk.emu_ibo = (uint32_t)(ibo * 4); g_mtk.emu_iblen = (uint32_t)((sl - ibo) * 4);
    return 1;
}
// The prepared draws' stages in one compute pass, three phases with one barrier between them (not one per dispatch): all task stages, all scans, all mesh stages
static void mtk_emu_flush(id ce, const mxe_job *jb, int n) {
    id<MTLComputePipelineState> sp = mxe_scan_pso(); NSUInteger nt = MIN((NSUInteger)256, sp.maxTotalThreadsPerThreadgroup) / 32 * 32; int clamps = 0, tasks = 0;
    for (int i = 0; i < n; i++) { const mxe_job *j = &jb[i]; if (!j->ind) continue;   // indirect counts first, clamped on the GPU (#1542): task groups to capT, mesh-only groups to the exchange's room
        enc_c_pso(ce, mxe_pso(j->tp ? MXE_TCLAMP : MXE_MONLY)); enc_c_args(ce, j->xg, 0); enc_c_args(ce, j->ind, 1); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); clamps = 1; }
    if (clamps) enc_c_dep(ce);
    for (int i = 0; i < n; i++) { const mxe_job *j = &jb[i]; if (!j->tp) continue; tasks = 1;
        enc_c_pso(ce, j->tp); if (j->tok) enc_c_args(ce, j->tg, j->ta); enc_c_args(ce, j->first, MESH_FIRST_SLOT); enc_c_args(ce, j->xg, MXE_SLOT);
        if (j->ind) enc_c_dispatch_indirect(ce, j->xg + 20 * 4, MTLSizeMake(j->tt, 1, 1)); else enc_c_dispatch(ce, MTLSizeMake(j->capT, 1, 1), MTLSizeMake(j->tt, 1, 1)); }
    if (tasks) {
        enc_c_dep(ce); enc_c_pso(ce, sp);
        for (int i = 0; i < n; i++) { const mxe_job *j = &jb[i]; if (!j->tp) continue; enc_c_args(ce, j->xg, 0); enc_c_args(ce, j->ind ? j->ind : j->xg, 1); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(nt, 1, 1)); }
        enc_c_dep(ce);
    }
    for (int i = 0; i < n; i++) { const mxe_job *j = &jb[i];
        enc_c_pso(ce, j->mp); if (j->mok) enc_c_args(ce, j->mg, j->ma); enc_c_args(ce, j->xg, MXE_SLOT); if (!j->tp) enc_c_args(ce, j->first, MESH_FIRST_SLOT);
        enc_c_dispatch_indirect(ce, j->xg + 32, MTLSizeMake(j->mt, 1, 1)); }
}
// Before the draw, outside the render pass: run the draw's task and mesh stages. 0 = not done (the draw goes the driver's way). One compute pass a draw: running the stages of the
//   later draws of a pass in it too (replaying the state ops up to them on a copy of the replay state) made frames wait on cross-queue events, 2-5 fps (11-17 M1 Pro); left out
// 网格绘制入几何连段（mesh_run_on）：宿主阶段放进连段的计算道（道 1），绘制画进道 2 的主遍；连着几笔网格（中间只有状态设置、遮挡计数、发信号）共用一次
//   计算遍与一个不断开的主遍，GPU 次序成了「这几笔的宿主阶段 → 这几笔的绘制」（原先每笔：结束主遍 → 计算遍 → 重开主遍并整张读入附件，一帧约 58 次）。
//   成立的前提同几何连段：宿主阶段只写本笔自己的交换缓冲（任务 / 网格阶段有存储写的不入段）、片元阶段不写内存（geo_fs_writes）、没有待落的内联更新、没有延后 / 前移的遍。
//   段里已有几何核写过东西时先结束段（宿主阶段可能读到）；入段后把段的片元读记成「列举不全」，之后的几何核有写就先结束段
static uint64_t g_mr_draws, g_mr_runs_started;
static int mesh_stage_writes(NvShader *sh) { if (sh) for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].rw) return 1; return 0; }
// 宿主阶段整段挪到这一段的绘制之前跑：读贴图 / 图像 / 纹素缓冲 / 原始内存池表的不入段（可能读到这一段前面的绘制刚画的附件，挪前就读到旧的）。只读常量 / 存储缓冲的才入段
static int mesh_stage_reads_images(NvShader *sh) {
    if (sh) for (int i = 0; i < sh->rf.nb; i++) switch (sh->rf.b[i].kind) {
        case NVN_RB_TEX: case NVN_RB_TEXU: case NVN_RB_TEXD: case NVN_RB_TEXA: case NVN_RB_TEXP: case NVN_RB_IMG: case NVN_RB_IMGU: case NVN_RB_IMGA:
        case NVN_RB_TBUF: case NVN_RB_TBUFU: case NVN_RB_TBLEAD: case NVN_RB_GMEM: case NVN_RB_GMEML: return 1;
        default: break; }
    return 0;
}
static int geo_fs_writes(NvShader *fs);
static int mesh_run_ok(NvShader *ts, NvShader *ms) {
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
    return mesh_run_on() && !g_m3 && rec_on() && g_cb && !S.rast_discard && !g_nubd && !defer_has_active() && !g_hoist_n && !g_fz.active
        && pr && !geo_fs_writes(pr->sh[1]) && !mesh_stage_writes(ts) && !mesh_stage_writes(ms) && !mesh_stage_reads_images(ts) && !mesh_stage_reads_images(ms);
}
static int g_mtk_in_run;   // mesh_tasks_draw: this draw's main-pass draw goes into lane 2
static int mesh_run_enter(int run);   // after the geometry run code (below): ends / keeps the run; 1 = the run stays open for this draw
static void mesh_run_job(const mxe_job *j);
static int mesh_run_lane2(int on);    // 1: enter lane 2 for the draw (returns 1 if in a run); 0: back to lane 0
static int mtk_emu_run(const p_draw *dw, NvShader *ts, NvShader *ms, uint64_t ind) {
    if (!mtk_emu_on() || !mtk_emu_ready(ts, ms)) return 0;
    g_mtk_in_run = 0;
    int run = mesh_run_ok(ts, ms), keep = mesh_run_enter(run);
    if (!keep) {
        if (defer_has_active()) defer_fallback();   // as tfd_args: a deferred merged pass and lazy clears land before a compute pass outside the render pass
        lc_land(); rp_flush(); rp_end(); ubd_land();
    }
    mxe_job j; if (!mtk_emu_prep(dw, ts, ms, ind, &j)) { g_mtk.emu = 0; args_dirty_all(); return 0; }
    if (run) { mesh_run_job(&j); g_mtk_in_run = 1; return 1; }
    id ce = enc_c_begin(NULL); mtk_emu_flush(ce, &j, 1); enc_c_end(ce);
    args_dirty_all();
    static uint64_t n; if (!n++) MLOG("task / mesh host stages: first draw (task %s mesh %s, %s)", ts ? ts->key : "-", ms->key, ind ? "indirect" : "direct");
    static uint64_t nr; if (ms->mesh_relay && !nr++) MLOG("task / mesh host stages: first draw with a relayed payload (task %s mesh %s, %s), stage memory %u words a mesh workgroup in the exchange", ts ? ts->key : "-", ms->key, ind ? "indirect" : "direct", ms->mesh_relay_sw);
    return 1;
}
// ---------- Device stage scratch + direct byte stores + varying packing for native task/mesh draws ----------
enum {
    MTK_SCRATCH_SLOT = 28,          // VB_SLOT(12), unused by native [[mesh]] draws (only S2A_MESH_EMU uses MXE_SLOT=28)
    MTK_SCRATCH_FORCED = 3,
    MTK_SCRATCH_FLAGS = 4,
    MTK_SCRATCH_SLOTS_OFF = 8192,
    MTK_SCRATCH_SLOT_WORDS = 4096,
    MTK_SCRATCH_MIN_WORDS = 1024,   // > 4 KiB stage memory moves to device scratch
    MTK_SCRATCH_SPIN = 65536
};
static os_unfair_lock g_mtk_scratch_lock = OS_UNFAIR_LOCK_INIT;
static id<MTLBuffer> g_mtk_scratch;
static uint64_t g_mtk_scratch_gpu;
static unsigned g_mtk_scratch_slots;
static _Atomic uint64_t g_mtk_scratch_gpu_atomic;

static uint64_t mtk_scratch_gpu(void) {
    uint64_t cached = atomic_load_explicit(&g_mtk_scratch_gpu_atomic, memory_order_acquire);
    if (cached) return cached;
    os_unfair_lock_lock(&g_mtk_scratch_lock);
    if (!g_mtk_scratch) {
        unsigned slots = TARGET_OS_IPHONE ? 1024 : 4096;
        const char *e = getenv("DM_NVN_MESH_SCRATCH_SLOTS");
        if (e && atoi(e) > 0) slots = (unsigned)atoi(e);
        if ((slots & (slots - 1)) || MTK_SCRATCH_FLAGS + slots > MTK_SCRATCH_SLOTS_OFF) {
            os_unfair_lock_unlock(&g_mtk_scratch_lock);
            MDIE("mesh stage scratch: invalid slot count %u", slots);
        }
        size_t bytes = ((size_t)MTK_SCRATCH_SLOTS_OFF + (size_t)slots * MTK_SCRATCH_SLOT_WORDS) * 4;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!buf) {
            os_unfair_lock_unlock(&g_mtk_scratch_lock);
            MDIE("mesh stage scratch: cannot allocate %zu bytes", bytes);
        }
        uint32_t *h = (uint32_t *)buf.contents;
        memset(h, 0, (size_t)MTK_SCRATCH_SLOTS_OFF * 4);
        h[1] = slots - 1;
        h[2] = MTK_SCRATCH_SLOT_WORDS;
        g_mtk_scratch_slots = slots;
        buf.label = @"S2Arm mesh stage scratch";
        res_add(buf);
        res_commit();
        g_mtk_scratch = buf;
        g_mtk_scratch_gpu = buf.gpuAddress;
        atomic_store_explicit(&g_mtk_scratch_gpu_atomic, g_mtk_scratch_gpu, memory_order_release);
        MLOG("task / mesh device stage memory: %u slots of %u KiB (%zu MiB)", slots, MTK_SCRATCH_SLOT_WORDS * 4 / 1024, bytes >> 20);
    }
    uint64_t r = g_mtk_scratch_gpu;
    os_unfair_lock_unlock(&g_mtk_scratch_lock);
    return r;
}
static os_unfair_lock g_mopt_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary *g_mopt_libs, *g_mopt_meta;
static NSMutableSet<NSData *> *g_mopt_upgrading;
static void mtk_scratch_reset(void) {
    os_unfair_lock_lock(&g_mtk_scratch_lock);
    if (g_mtk_scratch) { res_remove(g_mtk_scratch); g_mtk_scratch = nil; }
    g_mtk_scratch_gpu = 0; g_mtk_scratch_slots = 0;
    atomic_store_explicit(&g_mtk_scratch_gpu_atomic, 0, memory_order_release);
    os_unfair_lock_unlock(&g_mtk_scratch_lock);
    os_unfair_lock_lock(&g_mopt_lock);
    [g_mopt_libs removeAllObjects];
    [g_mopt_meta removeAllObjects];
    [g_mopt_upgrading removeAllObjects];
    os_unfair_lock_unlock(&g_mopt_lock);
}
static int mtk_opt_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_MESH_OPT");
        v = e && *e ? atoi(e) != 0 : !TARGET_OS_IPHONE;
        MLOG("task / mesh shader opts: %s (DM_NVN_MESH_OPT=%s)", v ? "on (device stage scratch + direct byte stores + packed varyings)" : "off", e && *e ? e : "default");
    }
    return v;
}
static NSString *mtk_opt_mesh_src(NSString *src, int *out_dev, int *out_bstore) {
    *out_dev = 0; *out_bstore = 0;
    if (!src.length) return nil;
    NSMutableString *cur = [src mutableCopy];
    NSRange main0 = [cur rangeOfString:@"[[mesh]] void main0("];
    if (main0.location != NSNotFound) {
        NSRange emu = [cur rangeOfString:@"\n#ifdef S2A_MESH_EMU\n" options:NSBackwardsSearch];
        if (emu.location != NSNotFound && emu.location > main0.location) {
            [cur deleteCharactersInRange:NSMakeRange(emu.location, cur.length - emu.location)];
        }
    }
    if (main0.location != NSNotFound && ![cur containsString:@"dm_relay"] && ![cur containsString:@"[[buffer(28)]]"]) {
        static NSRegularExpression *rxArr, *rxInit, *rxOp, *rxTgTok;
        static dispatch_once_t onceA;
        dispatch_once(&onceA, ^{
            rxArr = [NSRegularExpression regularExpressionWithPattern:@"    threadgroup spvUnsafeArray<uint, (\\d+)> (\\w+);\n" options:0 error:nil];
            rxInit = [NSRegularExpression regularExpressionWithPattern:@"    for \\(uint i = ([^;]+); i < (\\d+)u; i \\+= (\\d+)u\\) (\\w+)\\[i\\] = 0u;\n" options:0 error:nil];
            rxOp = [NSRegularExpression regularExpressionWithPattern:@"^.*\\boperator\\s*\\[\\].*$" options:NSRegularExpressionAnchorsMatchLines error:nil];
            rxTgTok = [NSRegularExpression regularExpressionWithPattern:@"\\bthreadgroup\\b" options:0 error:nil];
        });
        NSTextCheckingResult *ma = [rxArr firstMatchInString:cur options:0 range:NSMakeRange(main0.location, cur.length - main0.location)];
        if (ma) {
            int words = [[cur substringWithRange:[ma rangeAtIndex:1]] intValue];
            NSString *mem = [cur substringWithRange:[ma rangeAtIndex:2]];
            NSRange searchFrom = NSMakeRange(NSMaxRange(ma.range), cur.length - NSMaxRange(ma.range));
            NSTextCheckingResult *mi = [rxInit firstMatchInString:cur options:0 range:searchFrom];
            if (words > MTK_SCRATCH_MIN_WORDS && words <= MTK_SCRATCH_SLOT_WORDS && mi &&
                [[cur substringWithRange:[mi rangeAtIndex:2]] intValue] == words &&
                [[cur substringWithRange:[mi rangeAtIndex:4]] isEqualToString:mem]) {
                NSString *lid = [cur substringWithRange:[mi rangeAtIndex:1]];
                NSMutableString *prefix = [[cur substringToIndex:main0.location] mutableCopy];
                NSString *arrRef = [NSString stringWithFormat:@"threadgroup spvUnsafeArray<uint, %d>&", words];
                NSString *devRef = [NSString stringWithFormat:@"device spvUnsafeArray<uint, %d>&", words];
                [prefix replaceOccurrencesOfString:arrRef withString:devRef options:0 range:NSMakeRange(0, prefix.length)];
                [prefix replaceOccurrencesOfString:@"(threadgroup atomic_uint*)" withString:@"(device atomic_uint*)" options:0 range:NSMakeRange(0, prefix.length)];
                [prefix replaceOccurrencesOfString:@"mem_flags::mem_threadgroup" withString:@"mem_flags::mem_device | mem_flags::mem_threadgroup" options:0 range:NSMakeRange(0, prefix.length)];
                NSMutableString *probe = [prefix mutableCopy];
                [probe replaceOccurrencesOfString:@"threadgroup_barrier(" withString:@"dm_bar(" options:0 range:NSMakeRange(0, probe.length)];
                [rxOp replaceMatchesInString:probe options:0 range:NSMakeRange(0, probe.length) withTemplate:@""];
                if (![rxTgTok firstMatchInString:probe options:0 range:NSMakeRange(0, probe.length)]) {
                    NSRange insPt = [prefix rangeOfString:@"struct dm_payload_t" options:NSBackwardsSearch];
                    if (insPt.location == NSNotFound) insPt = [prefix rangeOfString:@"struct dm_vertex_t" options:NSBackwardsSearch];
                    if (insPt.location != NSNotFound) {
                        NSString *relHelper = @"static inline __attribute__((always_inline)) void dm_release_slot(device uint* dm_scratch, uint slot, uint lid) { threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup); if (lid == 0u) atomic_store_explicit((device atomic_uint*)(dm_scratch + 4u + slot), 0u, memory_order_relaxed); }\n";
                        [prefix insertString:relHelper atIndex:insPt.location];
                        NSMutableString *tail = [[cur substringFromIndex:main0.location] mutableCopy];
                        [tail replaceOccurrencesOfString:@"mem_flags::mem_threadgroup" withString:@"mem_flags::mem_device | mem_flags::mem_threadgroup" options:0 range:NSMakeRange(0, tail.length)];
                        NSString *declOld = [NSString stringWithFormat:@"    threadgroup spvUnsafeArray<uint, %d> %@;\n", words, mem];
                        NSString *declNew = [NSString stringWithFormat:
                            @"    threadgroup uint dm_slot_tg;\n"
                             "    if (%@ == 0u) { uint s, tries = 0u; for (;;) { s = atomic_fetch_add_explicit((device atomic_uint*)dm_scratch, 1u, memory_order_relaxed) & dm_scratch[1]; if (atomic_exchange_explicit((device atomic_uint*)(dm_scratch + %du + s), 1u, memory_order_relaxed) == 0u) break; if (++tries == %du) { atomic_fetch_add_explicit((device atomic_uint*)(dm_scratch + %du), 1u, memory_order_relaxed); break; } } dm_slot_tg = s; }\n"
                             "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                             "    %@ %@ = *reinterpret_cast<device spvUnsafeArray<uint, %d>*>(dm_scratch + %du + (ulong)dm_slot_tg * dm_scratch[2]);\n",
                            lid, MTK_SCRATCH_FLAGS, MTK_SCRATCH_SPIN, MTK_SCRATCH_FORCED, devRef, mem, words, MTK_SCRATCH_SLOTS_OFF];
                        NSString *relCall = [NSString stringWithFormat:@"    dm_release_slot(dm_scratch, dm_slot_tg, %@);\n}\n", lid];
                        BOOL ok = YES;
                        for (NSString *sigPrefix in @[@"[[mesh]] void main0(", @"[[mesh]] void main0_z("]) {
                            NSRange sr = [tail rangeOfString:sigPrefix];
                            if (sr.location == NSNotFound) { if ([sigPrefix containsString:@"main0("]) { ok = NO; break; } continue; }
                            NSRange sigEnd = [tail rangeOfString:@")\n{\n" options:0 range:NSMakeRange(sr.location, tail.length - sr.location)];
                            if (sigEnd.location == NSNotFound) { ok = NO; break; }
                            [tail insertString:@", device uint* dm_scratch [[buffer(28)]]" atIndex:sigEnd.location];
                            NSRange bodyStart = NSMakeRange(sigEnd.location + @", device uint* dm_scratch [[buffer(28)]])\n{\n".length, tail.length - (sigEnd.location + @", device uint* dm_scratch [[buffer(28)]])\n{\n".length));
                            NSRange bodyClose = [tail rangeOfString:@"\n}\n" options:0 range:bodyStart];
                            if (bodyClose.location == NSNotFound) {
                                if ([tail hasSuffix:@"\n}"]) bodyClose = NSMakeRange(tail.length - 2, 2);
                                else { ok = NO; break; }
                            }
                            static NSRegularExpression *rxRet; static dispatch_once_t onceRet;
                            dispatch_once(&onceRet, ^{ rxRet = [NSRegularExpression regularExpressionWithPattern:@"\\breturn\\b" options:0 error:nil]; });
                            if ([rxRet firstMatchInString:tail options:0 range:NSMakeRange(bodyStart.location, bodyClose.location - bodyStart.location)]) { ok = NO; break; }
                            [tail replaceCharactersInRange:NSMakeRange(bodyClose.location + 1, bodyClose.length - 1) withString:relCall];
                            NSRange declR = [tail rangeOfString:declOld options:0 range:NSMakeRange(bodyStart.location, bodyClose.location - bodyStart.location)];
                            if (declR.location == NSNotFound) { ok = NO; break; }
                            [tail replaceCharactersInRange:declR withString:declNew];
                        }
                        if (ok) { cur = [NSMutableString stringWithFormat:@"%@%@", prefix, tail]; *out_dev = 1; }
                    }
                }
            }
        }
    }
    static NSRegularExpression *rxBstore;
    static dispatch_once_t onceB;
    dispatch_once(&onceB, ^{
        rxBstore = [NSRegularExpression regularExpressionWithPattern:
            @"^void (_\\d+)\\(thread const int& (_\\d+), thread const int& (_\\d+), (threadgroup|device) spvUnsafeArray<uint, (\\d+)>& (_\\d+)\\)\\n\\{\\n(.*?)\\n\\}\\n"
            options:NSRegularExpressionDotMatchesLineSeparators | NSRegularExpressionAnchorsMatchLines error:nil];
    });
    NSArray<NSTextCheckingResult *> *bms = [rxBstore matchesInString:cur options:0 range:NSMakeRange(0, cur.length)];
    for (NSTextCheckingResult *r in bms.reverseObjectEnumerator) {
        NSString *body = [cur substringWithRange:[r rangeAtIndex:7]];
        if (![body containsString:@"insert_bits("] || ![body containsString:@", 8u)"] ||
            ![body containsString:@"atomic_compare_exchange_weak_explicit"] || ![body containsString:@">> uint(2)"] ||
            ![body containsString:@"& 3"] || ![body containsString:@"<< 3"]) continue;
        NSString *rep = [NSString stringWithFormat:
            @"void %@(thread const int& %@, thread const int& %@, %@ spvUnsafeArray<uint, %@>& %@)\n{\n"
             "    reinterpret_cast<%@ uchar*>(&%@[0])[uint(%@)] = uchar(uint(%@));\n}\n",
            [cur substringWithRange:[r rangeAtIndex:1]], [cur substringWithRange:[r rangeAtIndex:2]],
            [cur substringWithRange:[r rangeAtIndex:3]], [cur substringWithRange:[r rangeAtIndex:4]],
            [cur substringWithRange:[r rangeAtIndex:5]], [cur substringWithRange:[r rangeAtIndex:6]],
            [cur substringWithRange:[r rangeAtIndex:4]], [cur substringWithRange:[r rangeAtIndex:6]],
            [cur substringWithRange:[r rangeAtIndex:2]], [cur substringWithRange:[r rangeAtIndex:3]]];
        [cur replaceCharactersInRange:r.range withString:rep];
        (*out_bstore)++;
    }
    return (*out_dev || *out_bstore) ? cur : nil;
}

typedef struct { int loc; NSString *type, *name, *quals; } mpk_member;
typedef struct { int loc, lane; NSString *expr; } mpk_lane;
static BOOL mpk_members(NSString *src, NSString *structName, BOOL allowPosition, NSMutableArray<NSValue *> *out, NSRange *block) {
    NSRegularExpression *decl = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"\\bstruct %@\\s*\\{", structName] options:0 error:nil];
    NSTextCheckingResult *m = [decl firstMatchInString:src options:0 range:NSMakeRange(0, src.length)];
    if (!m) return NO;
    NSRange end = [src rangeOfString:@"};" options:0 range:NSMakeRange(m.range.location, src.length - m.range.location)];
    if (end.location == NSNotFound) return NO;
    NSRange body = NSMakeRange(NSMaxRange(m.range), end.location - NSMaxRange(m.range));
    NSString *bodyStr = [src substringWithRange:body];
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^(\\w+)\\s+(\\w+)\\s*\\[\\[user\\(locn(\\d+)\\)((?:,\\s*\\w+)*)\\]\\]$" options:0 error:nil];
    for (NSString *raw in [bodyStr componentsSeparatedByString:@";"]) {
        NSString *t = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!t.length) continue;
        if (allowPosition && [t containsString:@"[[position"] && [t hasPrefix:@"float4 "]) continue;
        NSTextCheckingResult *r = [re firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
        if (!r) return NO;
        mpk_member *pm = calloc(1, sizeof *pm);
        pm->type = [t substringWithRange:[r rangeAtIndex:1]]; pm->name = [t substringWithRange:[r rangeAtIndex:2]];
        pm->loc = [[t substringWithRange:[r rangeAtIndex:3]] intValue]; pm->quals = [t substringWithRange:[r rangeAtIndex:4]];
        [out addObject:[NSValue valueWithPointer:pm]];
    }
    *block = NSMakeRange(m.range.location, NSMaxRange(end) - m.range.location);
    return YES;
}
static void mpk_members_free(NSMutableArray<NSValue *> *a) { for (NSValue *v in a) free(v.pointerValue); }
static NSArray<NSString *> *mpk_split4(NSString *s) {
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:4];
    int depth = 0; NSUInteger start = 0;
    for (NSUInteger i = 0; i < s.length; i++) {
        unichar c = [s characterAtIndex:i];
        if (c == '(' || c == '[' || c == '<') depth++;
        else if (c == ')' || c == ']' || c == '>') depth--;
        else if (c == ',' && depth == 0) {
            [out addObject:[[s substringWithRange:NSMakeRange(start, i - start)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]];
            start = i + 1;
        }
    }
    [out addObject:[[s substringFromIndex:start] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]];
    return depth == 0 && out.count == 4 ? out : nil;
}
static NSDictionary<NSNumber *, NSArray<NSString *> *> *mpk_exports(NSString *src, NSMutableDictionary<NSNumber *, NSString *> *lineText) {
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^\\s*v\\.a(\\d+)\\s*=\\s*float4\\(([^\\n]*)\\);$" options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    for (NSTextCheckingResult *r in [re matchesInString:src options:0 range:NSMakeRange(0, src.length)]) {
        NSNumber *loc = @([[src substringWithRange:[r rangeAtIndex:1]] intValue]);
        if (d[loc]) continue;
        NSArray<NSString *> *args = mpk_split4([src substringWithRange:[r rangeAtIndex:2]]);
        if (!args) return nil;
        d[loc] = args; lineText[loc] = [src substringWithRange:r.range];
    }
    return d;
}
static BOOL mpk_is_const(NSString *e) { return [e isEqualToString:@"0.0"] || [e isEqualToString:@"1.0"]; }
static int mtk_pack_pair_src(NSString *msrcIn, NSString *fsrcIn, NSString **out_msrc, NSString **out_fsrc, NSString **out_layout, unsigned *out_orig, unsigned *out_new) {
    NSString *msrc = msrcIn, *fsrc = fsrcIn;
    if (!msrc.length || !fsrc.length) return 0;
    {
        NSRange mmain = [msrc rangeOfString:@"[[mesh]] void main0("];
        NSRange emu = mmain.location != NSNotFound ? [msrc rangeOfString:@"\n#ifdef S2A_MESH_EMU\n" options:NSBackwardsSearch] : NSMakeRange(NSNotFound, 0);
        if (emu.location != NSNotFound && emu.location > mmain.location) msrc = [msrc substringToIndex:emu.location];
    }
    {
        static NSRegularExpression *rxMain0U; static dispatch_once_t onceU;
        dispatch_once(&onceU, ^{ rxMain0U = [NSRegularExpression regularExpressionWithPattern:@"\\n(?:\\[\\[[^\\]]+\\]\\]\\s+)?fragment\\s+main0_out_u\\s+main0_u\\(" options:0 error:nil]; });
        NSTextCheckingResult *mu = [rxMain0U firstMatchInString:fsrc options:0 range:NSMakeRange(0, fsrc.length)];
        if (mu) fsrc = [fsrc substringToIndex:mu.range.location];
    }
    if ([fsrc componentsSeparatedByString:@"main0_in"].count != 3) return 0;
    NSString *setV = [msrc containsString:@"dm_out.set_vertex(n, v);"] ? @"dm_out.set_vertex(n, v);" : @"dm_out.set_vertex(n,v);";
    if ([msrc componentsSeparatedByString:setV].count < 2) return 0;
    NSMutableArray<NSValue *> *fin = [NSMutableArray array], *mout = [NSMutableArray array]; NSRange fblock = {0, 0}, mblock = {0, 0};
    BOOL fok = mpk_members(fsrc, @"main0_in", NO, fin, &fblock), mok = mpk_members(msrc, @"dm_vertex_t", YES, mout, &mblock);
    NSMutableDictionary<NSNumber *, NSString *> *exportText = [NSMutableDictionary dictionary];
    NSDictionary<NSNumber *, NSArray<NSString *> *> *exports = mpk_exports(msrc, exportText);
    if (!fok || !mok || !exports || !fin.count) { mpk_members_free(fin); mpk_members_free(mout); return 0; }
    NSMutableSet<NSNumber *> *kept = [NSMutableSet set]; NSMutableArray<NSString *> *groupOrder = [NSMutableArray array];
    { NSMutableArray<NSValue *> *prim = [NSMutableArray array]; NSRange pblock = {0, 0};
      if (mpk_members(msrc, @"dm_prim_t", NO, prim, &pblock)) for (NSValue *v in prim) { mpk_member *pm = v.pointerValue; [kept addObject:@(pm->loc)]; }
      mpk_members_free(prim); }
    NSMutableDictionary<NSString *, NSMutableArray<NSValue *> *> *groups = [NSMutableDictionary dictionary];
    unsigned originalSlots = 0;
    for (NSValue *v in fin) {
        mpk_member *pm = v.pointerValue; NSNumber *loc = @(pm->loc);
        originalSlots++;
        if (![pm->type isEqualToString:@"float4"] || [kept containsObject:loc]) { [kept addObject:loc]; continue; }
        NSArray *e = exports[loc]; if (!e) continue;
        if (!groups[pm->quals]) { groups[pm->quals] = [NSMutableArray array]; [groupOrder addObject:pm->quals]; }
        for (int lane = 0; lane < 4; lane++) if (!mpk_is_const(e[lane])) {
            mpk_lane *pl = calloc(1, sizeof *pl); pl->loc = pm->loc; pl->lane = lane; pl->expr = e[lane];
            [groups[pm->quals] addObject:[NSValue valueWithPointer:pl]];
        }
    }
    NSMutableArray<NSArray<NSValue *> *> *slots = [NSMutableArray array]; NSMutableArray<NSString *> *slotQuals = [NSMutableArray array];
    for (NSString *q in groupOrder) {
        NSArray *lanes = groups[q];
        for (NSUInteger i = 0; i < lanes.count; i += 4) { [slots addObject:[lanes subarrayWithRange:NSMakeRange(i, MIN((NSUInteger)4, lanes.count - i))]]; [slotQuals addObject:q]; }
    }
    unsigned newSlots = (unsigned)(kept.count + slots.count);
    void (^freeAll)(void) = ^{ mpk_members_free(fin); mpk_members_free(mout); for (NSString *q in groups) for (NSValue *v in groups[q]) free(v.pointerValue); };
    if (!slots.count || newSlots >= originalSlots) { freeAll(); return 0; }
    NSMutableArray<NSNumber *> *slotLoc = [NSMutableArray array];
    for (int l = 0, s = 0; (unsigned)s < slots.count && l < 64; l++) if (![kept containsObject:@(l)]) { [slotLoc addObject:@(l)]; s++; }
    if (slotLoc.count != slots.count) { freeAll(); return 0; }
    NSMutableString *layout = [NSMutableString string];
    for (NSUInteger s = 0; s < slots.count; s++) {
        [layout appendFormat:@"%@%@[", slotLoc[s], [slotQuals[s] stringByReplacingOccurrencesOfString:@" " withString:@""]];
        for (NSValue *v in slots[s]) { mpk_lane *pl = v.pointerValue; [layout appendFormat:@"%d.%d,", pl->loc, pl->lane]; }
        [layout appendString:@"]"];
    }
    for (NSValue *v in fin) { mpk_member *pm = v.pointerValue; if (!exports[@(pm->loc)] && ![kept containsObject:@(pm->loc)]) [layout appendFormat:@"z%d", pm->loc]; }
    NSMutableString *mstruct = [NSMutableString stringWithString:@"struct dm_vertex_t {"];
    { NSString *block = [msrc substringWithRange:mblock];
      NSRegularExpression *pos = [NSRegularExpression regularExpressionWithPattern:@"float4\\s+position\\s*\\[\\[[^\\]]*\\]\\]\\s*;" options:0 error:nil];
      NSTextCheckingResult *p = [pos firstMatchInString:block options:0 range:NSMakeRange(0, block.length)];
      if (!p) { freeAll(); return 0; }
      [mstruct appendFormat:@" %@\n", [block substringWithRange:p.range]]; }
    NSMutableString *msource = [NSMutableString stringWithString:msrc];
    for (NSValue *v in mout) { mpk_member *pm = v.pointerValue;
        if ([kept containsObject:@(pm->loc)]) [mstruct appendFormat:@"    %@ %@ [[user(locn%d)]];\n", pm->type, pm->name, pm->loc];
        else if (exportText[@(pm->loc)]) [msource replaceOccurrencesOfString:[exportText[@(pm->loc)] stringByAppendingString:@"\n"] withString:@"" options:0 range:NSMakeRange(0, msource.length)]; }
    NSMutableString *slotWrites = [NSMutableString string];
    for (NSUInteger s = 0; s < slots.count; s++) {
        [mstruct appendFormat:@"    float4 dm_p%lu [[user(locn%@)]];\n", (unsigned long)s, slotLoc[s]];
        NSMutableArray *e = [NSMutableArray array]; for (NSValue *v in slots[s]) { mpk_lane *pl = v.pointerValue; [e addObject:pl->expr]; }
        while (e.count < 4) [e addObject:@"0.0"];
        [slotWrites appendFormat:@"        v.dm_p%lu = float4(%@);\n", (unsigned long)s, [e componentsJoinedByString:@", "]];
    }
    [mstruct appendString:@"};"];
    [msource replaceCharactersInRange:[msource rangeOfString:[msrc substringWithRange:mblock]] withString:mstruct];
    [msource replaceOccurrencesOfString:setV withString:[slotWrites stringByAppendingString:setV] options:0 range:NSMakeRange(0, msource.length)];
    NSRegularExpression *sig = [NSRegularExpression regularExpressionWithPattern:@"fragment\\s+\\w+\\s+main0\\(main0_in\\s+(\\w+)\\s*\\[\\[stage_in\\]\\]" options:0 error:nil];
    NSTextCheckingResult *sm = [sig firstMatchInString:fsrc options:0 range:NSMakeRange(0, fsrc.length)];
    if (!sm) { freeAll(); return 0; }
    NSString *inName = [fsrc substringWithRange:[sm rangeAtIndex:1]];
    NSRange open = [fsrc rangeOfString:@"\n{\n" options:0 range:NSMakeRange(NSMaxRange(sm.range), fsrc.length - NSMaxRange(sm.range))];
    if (open.location == NSNotFound) { freeAll(); return 0; }
    NSMutableString *fstruct = [NSMutableString stringWithString:@"struct main0_in\n{\n"], *locals = [NSMutableString string];
    NSMutableDictionary<NSNumber *, NSMutableArray<NSString *> *> *rebuilt = [NSMutableDictionary dictionary];
    for (NSValue *v in fin) { mpk_member *pm = v.pointerValue; NSArray *e = exports[@(pm->loc)];
        if ([kept containsObject:@(pm->loc)]) continue;
        NSMutableArray *lanes = [NSMutableArray arrayWithCapacity:4];
        for (int lane = 0; lane < 4; lane++) [lanes addObject:e ? (mpk_is_const(e[lane]) ? e[lane] : @"?") : @"0.0"];
        rebuilt[@(pm->loc)] = lanes; }
    static const char *swz = "xyzw";
    for (NSUInteger s = 0; s < slots.count; s++) { NSUInteger i = 0;
        for (NSValue *v in slots[s]) { mpk_lane *pl = v.pointerValue; rebuilt[@(pl->loc)][pl->lane] = [NSString stringWithFormat:@"%@.dm_p%lu.%c", inName, (unsigned long)s, swz[i++]]; } }
    NSMutableString *fbody = [NSMutableString stringWithString:fsrc];
    for (NSValue *v in fin) { mpk_member *pm = v.pointerValue;
        if ([kept containsObject:@(pm->loc)]) { [fstruct appendFormat:@"    %@ %@ [[user(locn%d)%@]];\n", pm->type, pm->name, pm->loc, pm->quals]; continue; }
        NSArray *lanes = rebuilt[@(pm->loc)]; if ([lanes containsObject:@"?"]) { freeAll(); return 0; }
        [locals appendFormat:@"    float4 dm_%@ = float4(%@);\n", pm->name, [lanes componentsJoinedByString:@", "]];
        NSRegularExpression *use = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"\\b%@\\.%@\\b", inName, pm->name] options:0 error:nil];
        [use replaceMatchesInString:fbody options:0 range:NSMakeRange(0, fbody.length) withTemplate:[@"dm_" stringByAppendingString:pm->name]]; }
    for (NSUInteger s = 0; s < slots.count; s++) [fstruct appendFormat:@"    float4 dm_p%lu [[user(locn%@)%@]];\n", (unsigned long)s, slotLoc[s], slotQuals[s]];
    [fstruct appendString:@"};"];
    NSRange fblock2 = [fbody rangeOfString:[fsrc substringWithRange:fblock]];
    if (fblock2.location == NSNotFound) { freeAll(); return 0; }
    [fbody replaceCharactersInRange:fblock2 withString:fstruct];
    NSTextCheckingResult *sm2 = [sig firstMatchInString:fbody options:0 range:NSMakeRange(0, fbody.length)];
    NSRange openB = sm2 ? [fbody rangeOfString:@"\n{\n" options:0 range:NSMakeRange(NSMaxRange(sm2.range), fbody.length - NSMaxRange(sm2.range))] : NSMakeRange(NSNotFound, 0);
    if (openB.location == NSNotFound) { freeAll(); return 0; }
    [fbody replaceCharactersInRange:openB withString:[@"\n{\n" stringByAppendingString:locals]];
    freeAll();
    *out_msrc = msource; *out_fsrc = fbody; *out_layout = layout;
    if (out_orig) *out_orig = originalSlots;
    if (out_new) *out_new = newSlots;
    return 1;
}

static id<MTLLibrary> mtk_opt_compile(NSString *cacheKey, NSString *src) {
    if (!cacheKey.length || !src.length) return nil;
    os_unfair_lock_lock(&g_mopt_lock);
    if (!g_mopt_libs) g_mopt_libs = [NSMutableDictionary dictionary];
    id existing = g_mopt_libs[cacheKey];
    if (!existing) {
        dispatch_group_t grp = dispatch_group_create(); dispatch_group_enter(grp); g_mopt_libs[cacheKey] = grp;
        os_unfair_lock_unlock(&g_mopt_lock);
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:dm_msl_opts() error:&e];
        if (!lib && e) MLOG("【黄】 [mesh-opt] %s compile skipped: %s", cacheKey.UTF8String, e.localizedDescription.UTF8String);
        os_unfair_lock_lock(&g_mopt_lock);
        g_mopt_libs[cacheKey] = lib ?: (id)NSNull.null;
        os_unfair_lock_unlock(&g_mopt_lock);
        dispatch_group_leave(grp);
        return lib;
    }
    os_unfair_lock_unlock(&g_mopt_lock);
    if (![existing conformsToProtocol:@protocol(MTLLibrary)] && existing != NSNull.null) {
        dispatch_group_wait((dispatch_group_t)existing, DISPATCH_TIME_FOREVER);
        os_unfair_lock_lock(&g_mopt_lock);
        existing = g_mopt_libs[cacheKey];
        os_unfair_lock_unlock(&g_mopt_lock);
    }
    return existing == NSNull.null ? nil : existing;
}
static NSString *mtk_base_src(NvShader *sh, int st, BOOL cached_only) {
    const char *out = getenv("DM_MSL_FALLBACK");
    if (!sh || !sh->key[0] || !out || !*out) return nil;
    if (cached_only) {
        char path[1200]; snprintf(path, sizeof path, "%s/%.16s_b.metal", out, sh->key);
        if (access(path, R_OK)) return nil;
        return [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    }
    uint8_t md[8] = {0};
    for (int i = 0; i < 8; i++) { unsigned b = 0; if (sscanf(sh->key + 2 * i, "%2x", &b) != 1) return nil; md[i] = (uint8_t)b; }
    char path[1200];
    if (dm_shader_base_cached(md, st, path, sizeof path)) return nil;
    return [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
}
static NSString *mtk_opt_plan(MTLMeshRenderPipelineDescriptor *m, const pso_key *pk, NvShader *ms, NvShader *fs, uint32_t pad, int merged, int has_fsfn, BOOL cached_only,
                              NSString **out_ms, NSString **out_fs, int *out_dev, int *out_bstore, unsigned *out_orig, unsigned *out_new, BOOL *out_uncached) {
    if (out_ms) *out_ms = nil; if (out_fs) *out_fs = nil; if (out_uncached) *out_uncached = NO;
    if (!mtk_opt_on() || !ms || !pk || pad || merged || has_fsfn) return nil;
    if (m.meshFunction != ms->fn || (fs && m.fragmentFunction != fs->fn)) return nil;
    if (pk->fc || pk->at || pk->msoff) return nil;
    if (fs && (pso_fout_need(pk, fs, merged) || pso_pb_eff(pk, fs, merged, 0))) return nil;
    NSString *msBase = mtk_base_src(ms, 5, cached_only);
    if (!msBase.length) { if (cached_only && out_uncached) *out_uncached = YES; return nil; }
    int dev = 0, bstore = 0;
    NSString *msOpt = mtk_opt_mesh_src(msBase, &dev, &bstore);
    NSString *msIn = msOpt ?: msBase;
    NSString *msPacked = nil, *fsPacked = nil, *layout = nil; unsigned origSlots = 0, newSlots = 0;
    if (fs) {
        NSString *fsBase = mtk_base_src(fs, 1, cached_only);
        if (!fsBase.length) {
            if (cached_only) { if (out_uncached) *out_uncached = YES; return nil; }
        } else {
            mtk_pack_pair_src(msIn, fsBase, &msPacked, &fsPacked, &layout, &origSlots, &newSlots);
        }
    }
    if (!msOpt && !msPacked) return nil;
    if (out_dev) *out_dev = dev; if (out_bstore) *out_bstore = bstore;
    if (out_orig) *out_orig = origSlots; if (out_new) *out_new = newSlots;
    if (msPacked && fsPacked) {
        if (out_ms) *out_ms = msPacked; if (out_fs) *out_fs = fsPacked;
        return [NSString stringWithFormat:@"%s|%s|d%db%dp|%@", ms->key, fs->key, dev, bstore, layout];
    }
    if (!msOpt) return nil;
    if (out_ms) *out_ms = msOpt;
    return [NSString stringWithFormat:@"%s|d%db%d", ms->key, dev, bstore];
}
static id<MTLRenderPipelineState> mtk_build_with_opt(MTLMeshRenderPipelineDescriptor *m, const pso_key *pk, NvShader *ts, NvShader *ms, NvShader *fs, uint32_t pad, int merged, int has_fsfn, int urgent, BOOL *out_need_upgrade, NSError **error) {
    if (out_need_upgrade) *out_need_upgrade = NO;
    NSString *msSrc = nil, *fsSrc = nil; int dev = 0, bstore = 0; unsigned origSlots = 0, newSlots = 0; BOOL uncached = NO;
    NSString *opt_tag = mtk_opt_plan(m, pk, ms, fs, pad, merged, has_fsfn, urgent != 0, &msSrc, &fsSrc, &dev, &bstore, &origSlots, &newSlots, &uncached);
    if (urgent && uncached) {
        if (out_need_upgrade) *out_need_upgrade = YES;
        NSString *baseKey = par_mkey(pk, ts, ms, fs, pad, merged, has_fsfn ? (id)NSNull.null : nil, nil);
        return par_mesh(m, baseKey, error);
    }
    NSString *optKey = opt_tag ? par_mkey(pk, ts, ms, fs, pad, merged, has_fsfn ? (id)NSNull.null : nil, opt_tag) : nil;
    if (urgent && opt_tag && !par_has_key(optKey)) {
        if (out_need_upgrade) *out_need_upgrade = YES;
        NSString *baseKey = par_mkey(pk, ts, ms, fs, pad, merged, has_fsfn ? (id)NSNull.null : nil, nil);
        return par_mesh(m, baseKey, error);
    }
    id<MTLFunction> origMfn = m.meshFunction, origFfn = m.fragmentFunction;
    if (opt_tag && msSrc) {
        if (dev) (void)mtk_scratch_gpu();
        NSString *meshEntry = [ms->fn.name hasSuffix:@"_z"] ? @"main0_z" : @"main0";
        NSString *mpfx = fsSrc ? @"pm|" : @"bm|";
        id<MTLLibrary> mlib = mtk_opt_compile([mpfx stringByAppendingString:opt_tag], msSrc);
        id<MTLLibrary> flib = (mlib && fsSrc) ? mtk_opt_compile([@"pf|" stringByAppendingString:opt_tag], fsSrc) : nil;
        id<MTLFunction> mf = mlib ? lib_entry(mlib, meshEntry) : nil;
        id<MTLFunction> ff = fsSrc ? (flib ? lib_entry(flib, @"main0") : nil) : origFfn;
        if (mf && ff) {
            m.meshFunction = mf;
            if (fsSrc) m.fragmentFunction = ff;
            os_unfair_lock_lock(&g_mopt_lock);
            if (!g_mopt_meta) g_mopt_meta = [NSMutableDictionary dictionary];
            NSString *logKey = fsSrc ? [NSString stringWithFormat:@"%s/%s", ms->key, fs->key] : @(ms->key);
            if (!g_mopt_meta[logKey]) {
                g_mopt_meta[logKey] = @YES;
                if (fsSrc) MLOG("[mesh-opt] mesh %s fs %s: dev=%d bstore=%d slots %u->%u", ms->key, fs->key, dev, bstore, origSlots, newSlots);
                else MLOG("[mesh-opt] mesh %s: dev=%d bstore=%d", ms->key, dev, bstore);
            }
            os_unfair_lock_unlock(&g_mopt_lock);
        } else {
            opt_tag = nil; optKey = nil;
        }
    } else {
        opt_tag = nil; optKey = nil;
    }
    NSString *akey = optKey ?: par_mkey(pk, ts, ms, fs, pad, merged, has_fsfn ? (id)NSNull.null : nil, nil);
    id<MTLRenderPipelineState> p = par_mesh(m, akey, error);
    if (!p && opt_tag) {
        MLOG("【黄】 [mesh-opt] pipeline with opt refused (%s), falling back: %s", ms->key, error && *error ? (*error).localizedDescription.UTF8String : "-");
        m.meshFunction = origMfn; m.fragmentFunction = origFfn;
        if (error) *error = nil;
        akey = par_mkey(pk, ts, ms, fs, pad, merged, has_fsfn ? (id)NSNull.null : nil, nil);
        p = par_mesh(m, akey, error);
    }
    return p;
}

// The pipeline descriptor of a task / mesh draw: the fragment function variant and the attachments as a vertex draw's (pso_desc, no vertex stage); pf: the padded mesh variant (#674) or nil.
//   Shared by the draw (mtk_pso) and the prewarm (mtk_warm), so a pipeline the prewarm built is the one the draw would build
static MTLMeshRenderPipelineDescriptor *mtk_desc(pso_key *k, NvShader *ts, NvShader *ms, NvShader *fs, id<MTLFunction> pf, int merged, id<MTLFunction> fsfn) {
    MTLRenderPipelineDescriptor *d = pso_desc(k, nil, fs, fsfn, merged);
    MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new];
    m.meshFunction = pf ?: ms->fn; m.fragmentFunction = d.fragmentFunction; m.maxTotalThreadsPerMeshThreadgroup = ms->mesh_threads;
    if (ts) { m.objectFunction = ts->fn; m.payloadMemoryLength = ts->mesh_relay ? 16 : ((NSUInteger)ts->mesh_payload + 15) & ~(NSUInteger)15; m.maxTotalThreadsPerObjectThreadgroup = ts->mesh_threads; }   // relayed: object_data carries the task workgroup alone
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageEnabled = d.alphaToCoverageEnabled;
    for (int i = 0; i < 8; i++) m.colorAttachments[i] = d.colorAttachments[i];
    m.depthAttachmentPixelFormat = d.depthAttachmentPixelFormat; m.stencilAttachmentPixelFormat = d.stencilAttachmentPixelFormat;
    return m;
}
// A task / mesh pipeline goes into pso_user.tsv as an M row: M<TAB>pso_skey hex (the mesh shader in the vertex place)<TAB>task shader key ("-": none)<TAB>padded variant mask (hex), so the next start's
//   prewarm (warm_parse / mtk_warm) builds it before the scene, and the next package's seed (pso_seed.py) carries it. A merged pass's pipeline (a specialized fragment) is not recorded,
//   as R rows of one are not (pso_record). Submit thread
static void mtk_record(const mtk_key *mk, NvShader *ts, NvShader *ms, NvShader *fs, int merged) {
    if (merged || mk->k.fsfn) return;
    pso_skey sk; pso_skey_from(&mk->k, ms, fs, 0, NULL, &sk); char hx[sizeof sk * 2 + 1]; pso_skey_hex(&sk, hx);
    pso_user_record("M\t%s\t%s\t%x\n", hx, ts ? ts->key : "-", mk->pad);
}
// The key a draw of these shaders looks its pipeline up with, from an M row (the shader pointers are the shared objects, shader_load)
static void mtk_warm_key(const pso_skey *sk, NvShader *ts, NvShader *ms, NvShader *fs, uint32_t pad, mtk_key *mk) {
    memset(mk, 0, sizeof *mk); mk->k = sk->k; mk->k.vs = (__bridge void *)ms; mk->k.fs = (__bridge void *)fs; mk->k.fsfn = NULL; mk->ts = (__bridge void *)ts; mk->pad = pad;
}
// The prewarm's build of an M row (a background worker): the shared shader objects (warm_shader → shader_load), the key the draw looks up with, the pipeline; nil when it cannot be built
static id mtk_warm(const pso_skey *sk, NSString *mkey, NSString *fkey, NSString *tkey, uint32_t pad, NSData **key) {
    if (!g_mesh_ok) return nil;
    NvShader *ms = warm_shader(mkey, sk->vs_stage), *fs = fkey.length ? warm_shader(fkey, sk->fs_stage) : nil, *ts = tkey.length ? warm_shader(tkey, 3) : nil;
    if (!ms || ms->mesh_kind != 2 || (fkey.length && !fs) || (tkey.length && !ts)) return nil;
    id<MTLFunction> pf = nil;
    if (pad && !(pf = vp_warm(ms, pad))) return nil;   // the padded variant: made now from this device's translation on disk (vp_warm); none there yet: the draw makes it (vp_ask)
    mtk_key mk; mtk_warm_key(sk, ts, ms, fs, pad, &mk);
    if (mk.k.fc && fs && !se_warm(fs, mk.k.fc)) return nil;   // a sampler-emulation variant this device has not made yet (as R rows)
    MTLMeshRenderPipelineDescriptor *m = mtk_desc(&mk.k, ts, ms, fs, pf, 0, nil);
    NSError *e = nil; uint64_t t0 = bp_now();
    id<MTLRenderPipelineState> p = mtk_build_with_opt(m, &mk.k, ts, ms, fs, pad, 0, NO, 0, NULL, &e);
    if (!p) { MLOG("【红】 prewarm: task / mesh pipeline refused (task %s mesh %s fragment %s): %s", ts ? ts->key : "-", ms->key, fs ? fs->key : "-", e.localizedDescription.UTF8String); return nil; }
    NSString *names = [NSString stringWithFormat:@"task %s mesh %s%s fragment %s", ts ? ts->key : "-", ms->key, pf ? " (padded)" : "", fs ? fs->key : "-"];
    os_unfair_lock_lock(&g_pso_mtk_lock); if (!g_mtk_names) g_mtk_names = [NSMapTable strongToStrongObjectsMapTable]; [g_mtk_names setObject:names forKey:p]; os_unfair_lock_unlock(&g_pso_mtk_lock);
    (void)t0; *key = [NSData dataWithBytes:&mk length:sizeof mk]; return p;
}
static uint64_t g_mtk_warm_claimed;
static void mtk_warm_claim(NSData *key, id ps) {   // submit thread (warm_claim): a pipeline the prewarm built goes where the draw looks, unless the draw already has one
    mtk_key mk; if (key.length != sizeof mk) return; [key getBytes:&mk length:sizeof mk];
    os_unfair_lock_lock(&g_pso_mtk_lock); __unsafe_unretained id cur = hm_get_id(&g_pso_mtk, &mk); int put = !cur || cur == g_pso_mtk_building;
    if (put) mtk_put_locked(&mk, ps); os_unfair_lock_unlock(&g_pso_mtk_lock);
    if (put) { uint64_t n = ++g_mtk_warm_claimed; if (!(n & (n - 1))) MLOG("task / mesh pipelines from the prewarm taken by the draws' table: %llu so far", (unsigned long long)n); }
}
// The background build of a mesh pipeline: a concurrent queue, at most mtk_width() at a time (the system compiler service builds them in parallel; one serial queue left a draw that
//   could not wait behind every other pipeline queued before it)
static int mtk_width(void) { static int w; if (!w) { const char *e = getenv("DM_NVN_MESH_PAR"); int n = (int)NSProcessInfo.processInfo.activeProcessorCount; w = e && atoi(e) > 0 ? atoi(e) : n > 4 ? n - 2 : 2; MLOG("task / mesh pipelines: built in the background %d at a time (DM_NVN_MESH_PAR)", w); } return w; }
static dispatch_semaphore_t g_mtk_sem, g_mtk_opt_sem;
static dispatch_queue_t g_mtk_opt_q;
static void mtk_opt_upgrade(id m, pso_key pk_copy, NvShader *ts, NvShader *ms, NvShader *fs, int merged, int has_fsfn, NSString *names, mtk_key kk, id<MTLRenderPipelineState> orig) {
    if (![m isKindOfClass:[MTLMeshRenderPipelineDescriptor class]]) return;
    NSData *kd = [NSData dataWithBytes:&kk length:sizeof kk];
    os_unfair_lock_lock(&g_mopt_lock);
    if (!g_mopt_upgrading) g_mopt_upgrading = [NSMutableSet set];
    BOOL busy = [g_mopt_upgrading containsObject:kd];
    if (!busy) [g_mopt_upgrading addObject:kd];
    os_unfair_lock_unlock(&g_mopt_lock);
    if (busy) return;
    dispatch_group_async(g_mtk_group, g_mtk_opt_q, ^{ @autoreleasepool {
        dispatch_semaphore_wait(g_mtk_opt_sem, DISPATCH_TIME_FOREVER);
        NSError *e = nil;
        id<MTLRenderPipelineState> opt = mtk_build_with_opt((MTLMeshRenderPipelineDescriptor *)m, &pk_copy, ts, ms, fs, kk.pad, merged, has_fsfn, 0, NULL, &e);
        dispatch_semaphore_signal(g_mtk_opt_sem);
        if (opt && opt != orig) {
            os_unfair_lock_lock(&g_pso_mtk_lock);
            if (hm_get_id(&g_pso_mtk, &kk) == orig) {
                mtk_put_locked(&kk, opt);   // 管线额度 ⑦：一律经 mtk_put_locked（真管线换真管线，条数不变）
                if (!g_mtk_names) g_mtk_names = [NSMapTable strongToStrongObjectsMapTable];
                [g_mtk_names setObject:[names stringByAppendingString:@" (opt)"] forKey:opt];
                if (!g_mtk_drawn) g_mtk_drawn = [NSMutableSet set];
                [g_mtk_drawn addObject:opt];
            }
            os_unfair_lock_unlock(&g_pso_mtk_lock);
        }
        os_unfair_lock_lock(&g_mopt_lock);
        [g_mopt_upgrading removeObject:kd];
        os_unfair_lock_unlock(&g_mopt_lock);
    } });
}
static void mtk_kick(id m, const pso_key *pk, NvShader *ts, NvShader *ms, NvShader *fs, int merged, int has_fsfn, NSString *names, const mtk_key *key, int urgent) {   // m: a mesh pipeline descriptor, or (#770 host stages) an ordinary one
    mtk_key kk = *key; pso_key pk_copy = *pk;
    atomic_fetch_add(&g_mtk_inflight, 1);
    if (!g_mtk_q) {
        g_mtk_q = dispatch_queue_create("dm.nvn.mesh", s2a_qos_queue_attr(DISPATCH_QUEUE_CONCURRENT, S2A_QOS_SHADER_VARIANT));
        g_mtk_opt_q = dispatch_queue_create("dm.nvn.mesh.opt", s2a_qos_queue_attr(DISPATCH_QUEUE_CONCURRENT, S2A_QOS_SHADER_PREFETCH));
        g_mtk_group = dispatch_group_create();
        g_mtk_sem = dispatch_semaphore_create(mtk_width());
        g_mtk_opt_sem = dispatch_semaphore_create(2);
    }
    dispatch_group_async(g_mtk_group, g_mtk_q, ^{ @autoreleasepool {
        dispatch_semaphore_wait(g_mtk_sem, DISPATCH_TIME_FOREVER);
        NSError *e = nil; uint64_t t0 = bp_now(); BOOL need_up = NO;
        id<MTLRenderPipelineState> p = [m isKindOfClass:[MTLMeshRenderPipelineDescriptor class]] ? mtk_build_with_opt((MTLMeshRenderPipelineDescriptor *)m, &pk_copy, ts, ms, fs, kk.pad, merged, has_fsfn, urgent, &need_up, &e)
                                                                                                 : [g_dev newRenderPipelineStateWithDescriptor:m error:&e];
        dispatch_semaphore_signal(g_mtk_sem);
        int left = atomic_fetch_sub(&g_mtk_inflight, 1) - 1;
        if (p) MLOG("task / mesh pipeline: %s, %.1f ms in the background (frame %llu, still building %d)", names.UTF8String, (bp_now() - t0) / 1e6,
                    (unsigned long long)atomic_load_explicit(&g_cur_frame, memory_order_relaxed), left);
        else MLOG("【红】 task / mesh pipeline refused (%s): %s; its draws are skipped", names.UTF8String, e.localizedDescription.UTF8String);
        os_unfair_lock_lock(&g_pso_mtk_lock);
        if (p) { if (!g_mtk_names) g_mtk_names = [NSMapTable strongToStrongObjectsMapTable]; [g_mtk_names setObject:names forKey:p]; }
        if (mtk_pace()) { if (!g_mtk_ready) g_mtk_ready = [NSMutableArray array]; [g_mtk_ready addObject:@[[NSData dataWithBytes:&kk length:sizeof kk], p ? (id)p : NSNull.null]]; }   // held: mtk_publish delivers
        else if (hm_get_id(&g_pso_mtk, &kk) == g_pso_mtk_building) mtk_put_locked(&kk, p ?: NSNull.null);   // the prewarm may have handed one over meanwhile: keep that one
        os_unfair_lock_unlock(&g_pso_mtk_lock);
        if (p && need_up && !mtk_pace()) mtk_opt_upgrade(m, pk_copy, ts, ms, fs, merged, has_fsfn, names, kk, p);
    } });
}
// A draw that cannot be skipped waits on the submit thread for its pipeline's background build until it is ready, capped only by the stuck-compiler limit every other draw that cannot
//   be skipped goes by (pso_unsafe_wait_ms, DM_NVN_PSO_UNSAFE_WAIT_MS, default 10 s; 0 = no cap); past that cap it is skipped with a red line. → the pipeline, nil (skipped)
//   D-84J2（11-15，Mac M1 Max）：原先每帧共 250 ms 预算（DM_NVN_MESH_WAIT_MS），超了照跳——跳过的是「某阶段写存储」或「目标跨帧保留内容」的那笔，下游读到没写的数据，
//   3 局 GPU 挂死 / 缺页都出在网格管线首用、紧挨着「没等到、本帧跳过」的那一帧。跳这种绘制在哪块芯片上都不对（老芯片更容易撞上）：不能跳过的就等到编好，
//   只有可丢的（纯颜色输出、本帧新开的目标，mtk_sync_why 返回 NULL）才首用跳过、后台编。DM_NVN_MESH_WAIT_MS 只留作诊断对照：设了就回到每帧预算的旧做法
static uint64_t g_mtk_waits, g_mtk_wait_ns, g_mtk_wait_max, g_mtk_wait_over, g_mtk_wait_frame = UINT64_MAX, g_mtk_wait_frame_ns;
static int g_mtk_budget = -1;   // ms; -1: not read yet (t_nvn_mesh ④d sets it)
static int mtk_budget_ms(void) { if (g_mtk_budget < 0) { const char *e = getenv("DM_NVN_MESH_WAIT_MS"); g_mtk_budget = e && *e ? atoi(e) : 0; } return g_mtk_budget; }   // 0 = no per-frame budget (wait per draw up to pso_unsafe_wait_ms)
static id mtk_wait(const mtk_key *mk, const char *why) {
    uint64_t f = atomic_load_explicit(&g_cur_frame, memory_order_relaxed); if (f != g_mtk_wait_frame) { g_mtk_wait_frame = f; g_mtk_wait_frame_ns = 0; }
    uint64_t budget = (uint64_t)mtk_budget_ms() * 1000000ull, cap = (uint64_t)pso_unsafe_wait_ms() * 1000000ull, t0 = bp_now(); __unsafe_unretained id p = nil; int done = 0;
    while (!mtk_pace()) {
        os_unfair_lock_lock(&g_pso_mtk_lock); p = hm_get_id(&g_pso_mtk, mk); os_unfair_lock_unlock(&g_pso_mtk_lock);
        if (p != g_pso_mtk_building) { done = 1; break; }
        if (budget ? g_mtk_wait_frame_ns + (bp_now() - t0) >= budget : (cap && bp_now() - t0 >= cap)) break;
        usleep(200);
    }
    uint64_t dt = bp_now() - t0; g_mtk_wait_frame_ns += dt; g_mtk_waits++; g_mtk_wait_ns += dt; if (dt > g_mtk_wait_max) g_mtk_wait_max = dt;
    if (!done) g_mtk_wait_over++;
    if (!(g_mtk_waits & (g_mtk_waits - 1)) || !done) MLOG("%stask / mesh draw that cannot be skipped (%s) waited for its background pipeline %.1f ms (%s); %llu waits so far, %.1f ms in all, longest %.1f ms, %llu not ready in time (skipped; %s %d ms)",
        done ? "" : "【红】 ", why, dt / 1e6, done ? "ready" : "not ready: skipped this frame", (unsigned long long)g_mtk_waits, g_mtk_wait_ns / 1e6, g_mtk_wait_max / 1e6, (unsigned long long)g_mtk_wait_over,
        mtk_budget_ms() ? "per-frame budget DM_NVN_MESH_WAIT_MS" : "stuck-compiler cap DM_NVN_PSO_UNSAFE_WAIT_MS", mtk_budget_ms() ? mtk_budget_ms() : pso_unsafe_wait_ms());
    return done && p != NSNull.null ? p : nil;
}
// Why a mesh draw whose pipeline is not built yet may not be skipped (nil: it may, its pipeline builds in the background); the same rules as skip_safe / first_skip_ok
static const char *mtk_sync_why(NvShader *ts, NvShader *ms, NvShader *fs) {
    if (!first_skip_on()) return "first-use skip off (DM_NVN_FIRST_SKIP=0)";
    if (shader_writes(ts) || shader_writes(ms) || shader_writes(fs)) return "a stage writes storage";
    for (int i = 0; i < R.rt.n; i++) if (R.rt.color[i] && !rt_fresh_now(R.rt.color[i])) return "a target not started fresh this frame";
    if (R.rt.depth && !rt_fresh_now(R.rt.depth)) return "a target not started fresh this frame";
    for (int i = 0; i < R.rt.n; i++) if (R.rt.color[i] && !rt_seen_steady(R.rt.color[i])) return "a target not drawn in the last two frames";
    if (R.rt.depth && !rt_seen_steady(R.rt.depth)) return "a target not drawn in the last two frames";
    return NULL;
}
// 一条网格管线第一次画（两条路：已编好直接取 / 丢不起的那笔等后台编好）：记本机记录、打「first drawn」、进本提交的 GPU 出错嫌疑（quar_built）。
//   #1208：网格管线多在后台编、晚几帧才第一次画，隔离原先只收「本提交当场新编的普通管线」，网格管线首画那次提交挂死时嫌疑为空、什么都不隔离，下一局挂在同一处；
//   丢不起的那笔经 mtk_wait 画出时连「first drawn」都不打
static uint64_t g_mtk_quar_skip;
static void mtk_first_drawn(id p, const mtk_key *mk, NvShader *ts, NvShader *ms, NvShader *fs, int merged) {
    [g_mtk_drawn addObject:p]; if (!mk->emu) mtk_record(mk, ts, ms, fs, merged); quar_built(ms, fs);
    MLOG("task / mesh pipeline first drawn at frame %llu: %s; still building %d", (unsigned long long)atomic_load_explicit(&g_cur_frame, memory_order_relaxed),
         mtk_name(p), atomic_load(&g_mtk_inflight));
}
static id<MTLRenderPipelineState> mtk_pso(NvProg *raster) {
    NvShader *ts = g_mtk.ts, *ms = g_mtk.ms, *fs = raster->sh[1]; int merged = g_pso_lmap != NULL;
    mtk_key mk; memset(&mk, 0, sizeof mk); pso_key *k = &mk.k;
    pso_key_fill(k, ms, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, merged);
    memset(k->attr, 0, sizeof k->attr); memset(k->stream, 0, sizeof k->stream);   // no vertex fetch
    if (merged) { memcpy(k->lmap, g_pso_lmap, 8); k->fsfn = (__bridge void *)g_pso_fsfn; }   // as pso_get_vs keys a merged pass's pipeline
    if (g_pso_fc && !merged && !g_pso_fsfn && fs && !pso_fout_need(k, fs, 0)) k->fc = (uint8_t)g_pso_fc;
    g_pso_color_round = pso_color_round_mask(k, fs, merged);
    mk.ts = (__bridge void *)ts; mk.emu = g_mtk.emu ? 1 : 0;   // #770: the host ran the stages, an ordinary pipeline of its vertex function draws them
    // #674: a fragment shader the game pairs at run time with a mesh shader that does not write a user(locn) it reads (Metal refuses the pipeline). Mesh shaders carry no user_out in
    //   their json, so the miss is known from the refusal: then the padded mesh variant for every location the fragment reads is asked for (vp_ask, dm_shader_vpad → dm_mesh_pad_src:
    //   only the locations the mesh does not write get (0,0,0,1)); until it is ready the pair's draws are skipped as before, then the pipeline is built with it (its own cache entry)
    uint32_t want = fs && (fs->rf.has_user & 2) && !mk.emu ? fs->rf.user_in : 0; id<MTLFunction> pf = want ? vp_fn(ms, want) : nil;
    if (pf) mk.pad = want;
    // GPU 挂死隔离（#1208）：先前让 GPU 出错的网格管线（网格 + 片元两键，同普通管线的 quar_name）这局起跳过，不再画
    if (quar_has(ms, fs)) { g_mtk_quar_skip++; static int qn; if (qn++ < 4) MLOG("【红】 GPU error quarantine: task / mesh pipeline (task %s mesh %s fragment %s) skipped (it faulted the GPU before)",
                                                                           ts ? ts->key : "-", ms->key, fs ? fs->key : "-"); return nil; }
    os_unfair_lock_lock(&g_pso_mtk_lock); __unsafe_unretained id ps = hm_get_id(&g_pso_mtk, &mk);
    if (!ps) { if (!g_pso_mtk_building) g_pso_mtk_building = [NSObject new]; mtk_put_locked(&mk, g_pso_mtk_building); }
    else if (mtk_real(ps)) pb_touch_m(&mk);
    os_unfair_lock_unlock(&g_pso_mtk_lock);
    if (ps == NSNull.null && !mk.pad && want) vp_ask(S.prog, ms, want);   // refused unpadded: ask for the padded variant (once; a variant that cannot be made stays failed)
    if (ps && ps != NSNull.null && ps != g_pso_mtk_building) {   // #770: the first draw of each pipeline, with the frame and how many are still building
        if (!g_mtk_drawn) g_mtk_drawn = [NSMutableSet set];
        if (![g_mtk_drawn containsObject:ps]) mtk_first_drawn(ps, &mk, ts, ms, fs, merged);
    }
    if (ps == NSNull.null) return nil;
    if (ps && ps != g_pso_mtk_building) return ps;
    // Not built yet: skipping this draw is only allowed under the rules every other first use goes by (skip_safe, first_skip_ok): no stage writes storage, every target is started
    //   fresh this frame and was drawn in the last two. 11-13 0400486024DEC000 on Duo: the mesh draws of the opening scene were skipped for 10–20 s while 54 mesh pipelines built one
    //   after another in the background, and the scene came out washed white until the last of them was ready. A draw that cannot be skipped waits for its background build, within
    //   a per-frame budget (mtk_wait). Building it here instead held the submit thread 19 s in all (one pipeline 3.1 s) and a cross-queue GPU event wait timed out into a GPU hang
    const char *why = mtk_sync_why(ts, ms, fs);
    if (!ps) {
        id m;
        if (mk.emu) { MTLRenderPipelineDescriptor *d = pso_desc(k, nil, fs, g_pso_fsfn, merged); d.vertexFunction = g_mtk.emu_fn; d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; m = d; }
        else m = mtk_desc(k, ts, ms, fs, pf, merged, g_pso_fsfn);
        NSString *names = [NSString stringWithFormat:@"task %s mesh %s%s fragment %s", ts ? ts->key : "-", ms->key, mk.emu ? " (host stages)" : pf ? " (padded)" : "", fs ? fs->key : "-"];
        mtk_kick(m, k, ts, ms, fs, merged, g_pso_fsfn != nil, names, &mk, why != NULL);
        if (!mk.emu) mtk_record(&mk, ts, ms, fs, merged);   // 请求编译时就记：只在一小段画面里用到、又编得慢的管线（0400486024DEC000 一段事件里那条 2 秒的），等它编好那笔已不再画，「第一次画出」时记就永远进不了种子
    }
    if (!why) return nil;   // building in the background, and this draw may wait for it
    __unsafe_unretained id p = mtk_wait(&mk, why);
    if (p) { if (!g_mtk_drawn) g_mtk_drawn = [NSMutableSet set];
        if (![g_mtk_drawn containsObject:p]) mtk_first_drawn(p, &mk, ts, ms, fs, merged); }
    return p;
}
// Bind task / mesh shader arguments before draw()'s g_enc != en retry check: encode_args can end the render pass
// (e.g. cbuf_pad_alloc GPU copy via blit(), tex_upgrade, or unaligned tbuf copy), leaving g_blit (a compute encoder
// on Metal 4) open. Encoding drawMeshThreadgroups* without retrying sent a render selector to that compute encoder.
static void mtk_bind(id en) {
    NvShader *ts = g_mtk.ts, *ms = g_mtk.ms;
    enc_r_table(en, g_vt, MTLRenderStageMesh);
    if (ts) { enc_r_table(en, g_ot, MTLRenderStageObject); g_args_object = 1; encode_args(en, ts, 3, 0); g_args_object = 0; }
    if (g_enc == en) encode_args(en, ms, 4, 1);
}
static void mtk_encode(id en_) {
    id en = en_; NvShader *ts = g_mtk.ts, *ms = g_mtk.ms;
    if (g_mtk.emu) {   // #770: the host ran the stages (mtk_emu_run): an indexed draw of their vertex records, as many indices as the mesh stage wrote
        enc_r_table(en, g_vt, MTLRenderStageVertex); enc_r_vbuf(en, MXE_SLOT, g_mtk.emu + g_mtk.emu_vbo);
        enc_r_draw_indexed_indirect(en, MTLPrimitiveTypeTriangle, MTLIndexTypeUInt32, g_mtk.emu + g_mtk.emu_ibo, g_mtk.emu_iblen, g_mtk.emu + 13 * 4);
        return;
    }
    if (fault_enc_on()) fault_enc_mark(en, [NSString stringWithFormat:@"task / mesh draw: task %s mesh %s, %s", ts ? ts->key : "-", ms->key, g_mtk.ind ? "indirect" : [NSString stringWithFormat:@"%llu workgroups", (unsigned long long)g_mtk.count].UTF8String]);
    uint64_t first = g_mtk.ind ? g_mtk.ind + 16 : 0;   // indirect: the converted arguments carry it at word 4 (ixc_arrays)
    if (!first) { id<MTLBuffer> b; size_t o = arena_alloc(16, &b); memcpy((uint8_t *)b.contents + o, &g_mtk.first, 4); first = b.gpuAddress + o; }
    if (ts) enc_o_args(first, MESH_FIRST_SLOT);
    enc_r_vbuf(en, MESH_FIRST_SLOT, first);
    uint64_t scratch = atomic_load_explicit(&g_mtk_scratch_gpu_atomic, memory_order_acquire);
    if (scratch) enc_r_vbuf(en, MTK_SCRATCH_SLOT, scratch);
    // Payload relay (S2A.Dsh MeshStage.Relay: a payload past Metal's 16 KB): one buffer, head {mesh slice start (words), task workgroups, slices per task workgroup}, then one payload per
    //   task workgroup, then one mesh stage-memory slice per (task workgroup, mesh workgroup < the task's thread count, which the task clamps its grid to). Bound at the relay slot of the
    //   object table and of the mesh stage (vertex stream 11's slot, through enc_r_vbuf)
    if (ts && ts->mesh_relay) {
        uint64_t n = g_mtk.count, pw = ts->mesh_relay_pw, sw = ms->mesh_relay_sw, head = 8, base = (head + n * pw + 3) & ~3ull, grid = ts->mesh_threads, bytes = (base + n * grid * sw) * 4;
        id<MTLBuffer> rb; size_t ro = arena_alloc((size_t)bytes, &rb); uint32_t *h = (uint32_t *)((uint8_t *)rb.contents + ro); h[0] = (uint32_t)base; h[1] = (uint32_t)n; h[2] = (uint32_t)grid;
        enc_o_args(rb.gpuAddress + ro, ts->mesh_relay); enc_r_vbuf(en, ms->mesh_relay, rb.gpuAddress + ro);
        static int once; if (!once++) MLOG("task / mesh payload relay: first draw (task %s mesh %s), %llu task workgroups, %.1f MB", ts->key, ms->key, (unsigned long long)n, bytes / 1048576.0);
    }
    NSUInteger ot = ts ? ts->mesh_threads : 1, mt = ms->mesh_threads;
    if (g_mtk.ind && fault_enc_on()) { NSUInteger fo; id<MTLBuffer> fbuf; if (m3_buf_at(g_mtk.ind, &fbuf, &fo)) fault_ring_put(fbuf, fo, ts ? ts->key : NULL, ms->key, atomic_load_explicit(&g_cur_frame, memory_order_relaxed)); }
    if (g_mtk.ind) enc_r_mesh_indirect(en, g_mtk.ind, ot, mt);
    else if (!ts && g_mtk.count > 1024) {   // Metal mesh grid is capped at 1024 per dispatch; chunk direct taskless draws cleanly
        uint32_t done = 0;
        while (done < g_mtk.count) {
            uint32_t n = MIN(g_mtk.count - done, 1024u), f0 = g_mtk.first + done;
            id<MTLBuffer> fb; size_t fo = arena_alloc(16, &fb); memcpy((uint8_t *)fb.contents + fo, &f0, 4);
            enc_r_vbuf(en, MESH_FIRST_SLOT, fb.gpuAddress + fo);
            enc_r_mesh(en, n, ot, mt);
            done += n;
        }
    } else enc_r_mesh(en, g_mtk.count, ot, mt);
}
static void mesh_tasks_draw(const p_draw *dw) {
    if (!dw->ind && dw->count <= 0) return;
    if (!g_mesh_ok) { g_mtk_skip++; RED_ONCE(149, "task / mesh draw: this GPU has no object / mesh pipelines (Apple7+ / Mac2): skipped"); return; }
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
    NvShader *ms = pr ? pr->sh[4] : nil, *ts = pr ? pr->sh[3] : nil;
    if (!ms || ms->mesh_kind != 2 || !ms->mesh_task || (ts && ts->mesh_kind != 1)) ts = nil;   // a task shader only for a mesh shader that takes one (a hull shader of an earlier bind is not)
    if (ts && ts->mesh_relay && !mtk_emu_on() && (dw->ind || !ms || !ms->mesh_relay || (uint64_t)dw->count * ts->mesh_threads * ms->mesh_relay_sw * 4 > (512ull << 20))) {   // relayed payload: the buffer is sized on the CPU from the workgroup count
        g_mtk_skip++; RED_ONCE(152, "task / mesh draw with a relayed payload (task %s): %s, skipped", ts->key, dw->ind ? "indirect (workgroup count not known on the CPU)" : !ms || !ms->mesh_relay ? "the mesh shader does not take a relayed payload" : "relay buffer past 512 MiB"); return; }
    if (!ms || ms->mesh_kind != 2 || !ms->fn || (ms->mesh_task && !ts)) {
        g_mtk_skip++; BAD_ERR(); RED_ONCE(147, "task / mesh draw without its shaders (task %s, mesh %s%s): skipped", pr && pr->sh[3] ? pr->sh[3]->key : "-", ms ? ms->key : "-",
                                       ms && ms->mesh_kind == 2 && ms->mesh_task && !ts ? ", which takes a task shader" : ms && ms->mesh_kind != 2 ? ", not a mesh shader" : ""); return; }
    if (S.rast_discard) { g_mtk_skip++; RED_ONCE(150, "task / mesh draw with the rasterizer discarded (mesh %s): its storage side effects are not run, skipped", ms->key); return; }
    if (!img_atomic_prepare(ts, 3) || !img_atomic_prepare(ms, 4)) return;
    if (g_nubd && (g_enc || ubd_lazy()) && ((dw->ind && ubd_hits(dw->ind, dw->ind + 8)) || (ts && ubd_conflict_sh(ts, 3)) || ubd_conflict_sh(ms, 4))) { g_ubd_conflicts++; rp_end(); ubd_land(); }   // as draw() does for its stages
    uint64_t ind = 0;
    if (dw->ind) {
        const ixc_ent *ix = ixc_find(dw, NULL);
        // Not converted yet: the pass ends and inline updates land before the conversion takes its write epoch (ixc_run), else landing them at its compute encoder moved the epoch on and
        //   the conversion was never found again
        if (!ix) { if (g_enc) { g_ixc_splits++; rp_end(); } ubd_land(); ixc_run(dw, NULL); ix = ixc_find(dw, NULL); }
        if (!ix || !ix->args) { g_mtk_skip++; RED_ONCE(148, "indirect task / mesh draw skipped: argument address %#llx is in no memory pool, or the conversion pipeline was not created", (unsigned long long)dw->ind); return; }
        ind = ix->args;
    }
    NvProg *raster = pr->geo_raster;   // the pixel stage alone (geo_draw keeps its reader program here; a mesh program has no geometry stage)
    if (!raster || raster->sh[0] || raster->sh[1] != pr->sh[1]) { raster = [NvProg new]; raster->sh[1] = pr->sh[1]; raster->gp = pr->gp; pr->geo_raster = raster; }
    mtk_publish();   // #770 S2A_MESH_PACE: deliver held pipelines first
    g_mtk.emu = 0; g_mtk.emu_fn = nil;
    // #770: the host runs the stages on GPUs whose driver mesh stage hangs. A draw it cannot run there is skipped, not left to the driver's mesh stage (#1542 safety valve:
    //   a skipped draw beats a GPU hang that reboots the machine); each pair is reported once and counted with the extraction gaps, the cure is the host stages taking it
    if (!mtk_emu_run(dw, ts, ms, ind) && mtk_emu_on()) {
        static NSMutableSet *refused; if (!refused) refused = [NSMutableSet set];
        NSString *pk = [NSString stringWithFormat:@"%s/%s", ts ? ts->key : "-", ms->key];
        g_mtk_skip++; if (![refused containsObject:pk]) { [refused addObject:pk];
            MLOG("【红】 task / mesh draw the host stages cannot run (task %s mesh %s%s): skipped, not left to the driver's mesh stage, which hangs on this GPU (#1542; %lu pairs so far)",
                 ts ? ts->key : "-", ms->key, ts && ts->mesh_relay ? ", relayed payload" : "", (unsigned long)refused.count); }
        return;
    }
    int split = !g_mtk.emu ? mtk_split_on() : 0;
    if (split && g_enc && (split == 1 || (split == 2 && g_mtk_pass_plain))) rp_end();
    g_mtk.on = 1; g_mtk.miss = 0; g_mtk.ts = ts; g_mtk.ms = ms; g_mtk.first = (uint32_t)dw->first; g_mtk.count = (uint32_t)dw->count; g_mtk.ind = ind;
    g_geo_program = raster;
    p_draw triangle = {.prim = 4, .count = 3, .instances = 1};
    int in_run = g_mtk_in_run && mesh_run_lane2(1); g_mtk_in_run = 0;
    draw(&triangle, NULL);
    if (in_run) mesh_run_lane2(0);
    g_geo_program = nil; g_mtk.on = 0; g_mtk.ts = g_mtk.ms = nil; g_mtk.emu = 0; g_mtk.emu_fn = nil; args_dirty_all();
    if (g_mtk.miss) { g_mtk_wait++; return; }
    if (split == 1 && g_enc) rp_end();
    else if (split == 2 && g_enc) g_mtk_pass_mesh = 1;
    swrite_note(pr);   // the task / mesh stages' storage writes (draw() saw the pixel stage alone)
    if (!g_mtk_n++) MLOG("task / mesh: first draw (%s), task %s mesh %s fragment %s, %u workgroups from %u", ind ? "indirect" : "direct", ts ? ts->key : "-", ms->key, pr->sh[1] ? pr->sh[1]->key : "-",
                         g_mtk.count, g_mtk.first);
    if (ind) g_mtk_ind_n++;
}


// ---------- 几何绘制段（S2Arm 11-06 能耗线，金刚：一帧 541 笔几何绘制首尾相接、同一组渲染目标，每笔 抓顶点 / 几何核 / 重开主遍 3 个编码器，主遍断 541 次） ----------
// 段里的几何绘制共用一个抓顶点编码器（录制主流）、一个几何核编码器（道 1）和不断开的主绘制遍（道 2）。段结束时（geo_run_flush）道 1、道 2 接在主流后面，
//   GPU 上的次序成了「全部抓顶点 → 全部几何核 + 索引压缩 → 主遍里全部合成绘制」。与逐笔交错相比只换了这些工作彼此的先后，成立的前提是段里没有别的工作夹在中间、
//   互相不读写对方的输出：
//   ① 段只收「能入段」的几何绘制（geo_run_ok）；执行循环里状态设置之外的任何操作、非几何绘制、结束主遍、开别的编码器、提交 / 发信号 / 等 GPU 都先结束段；
//   ② 片元阶段不写存储缓冲 / 图像（几何核读的输入不会被同段的光栅改）；唯一放进来的片元写是预测生效时的原始池表写（写区间 = 学到的池，geo_runset fw），
//      后面各笔的抓顶点 / 几何核碰到就先结束段；几何核之间每次派发后一道编码器内屏障，按原顺序；
//   ③ 抓顶点读的统一缓冲与顶点：段里没有内联更新（有待落的更新不入段，OP_UPDATE_UB 结束段），各笔的参数在编码当时写进参数表（表每笔快照），
//      换道时作废顶点表缓存、全部参数块重编，所以每笔看到的是它自己那一刻的绑定。
//   只在 Metal 4 + 录制线程（DM_NVN_ENC_THREAD，iOS 缺省开）时成段：分道靠录制流的顺序；其余照原路逐笔
// 几何核之间的屏障（11-06 能耗线 ②，金刚真机：一段 615 笔，每笔「几何核 → 屏障 → 压缩 → 屏障」串行，道 1 每帧 43.4 ms）：
//   几何核只在它读 / 写的区间碰到本段前面几何核写过的区间、或它写的区间碰到前面读过的区间时，才在它前面加一道编码器内屏障；
//   读写区间列举不全的（可写图像、句柄在统一缓冲里的存储缓冲 / 贴图缓冲 / 图像）一律加，之后的也加。压缩只读本笔几何核写的记录、
//   只写本笔自己的暂存，全部挪到段末所有几何核之后，中间一道屏障（geo_run_flush）
typedef struct { uint64_t lo, hi; } geo_rg;
// 原始池表写的预测（nvn_mtl.m gml_take）在本笔几何绘制时的样子，geo_draw 开头取一次，本笔各阶段都按它（geo_fs_writes / geo_stage_ranges）：n = 学到的池区间数，
//   -1 = 还没预测 / 已记满（写在哪都算）/ 多于 GEO_GML_N（按列举不全）。区间是 CPU 地址，与绑定区间同一种（录制入口已把游戏的 GPU 地址换回 CPU，hle_nvn.c GA；
//   学到的区间取自池表的 CPU 起点 g_gmem_cpu）。学到的池只增不减，所以段里后来再取的一份包含前面各笔记写时用的那份（geo_runset_gml）
enum { GEO_GML_N = 256 };
static struct { int n; geo_rg r[GEO_GML_N]; } g_geo_gml = {-1};
static void geo_gml_snap(void) {
    if (!g_gml_on) { g_geo_gml.n = -1; return; }   // 还没有带池表写的着色器跑过：不可能在预测
    pthread_mutex_lock(&g_gml_mu);
    int n = !g_gml_over && g_gml_stable >= GML_STABLE && g_gml_n <= GEO_GML_N ? g_gml_n : -1;   // 同 gml_take 的 predict
    for (int i = 0; i < n; i++) g_geo_gml.r[i] = (geo_rg){g_gml_lo[i], g_gml_hi[i]};
    g_geo_gml.n = n;
    pthread_mutex_unlock(&g_gml_mu);
}
static int geo_gmem_writes(NvShader *sh) { if (sh) for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_GMEM && sh->rf.b[i].rw) return 1; return 0; }
enum { GEO_HZ_N = 64 };
typedef struct { geo_rg w[GEO_HZ_N], r[GEO_HZ_N]; int nw, nr, all; } geo_hz;
static int geo_rg_hit(const geo_rg *a, int na, const geo_rg *b, int nb) {
    for (int i = 0; i < na; i++) for (int j = 0; j < nb; j++) if (a[i].lo < b[j].hi && b[j].lo < a[i].hi) return 1;
    return 0;
}
// 这次几何核（读 rd、写 wr）要不要排在前面已派的几何核之后
static int geo_hz_conflict(const geo_hz *h, const geo_rg *rd, int nrd, const geo_rg *wr, int nwr) {
    return h->all || geo_rg_hit(rd, nrd, h->w, h->nw) || geo_rg_hit(wr, nwr, h->w, h->nw) || geo_rg_hit(wr, nwr, h->r, h->nr);
}
static void geo_hz_add(geo_hz *h, const geo_rg *rd, int nrd, const geo_rg *wr, int nwr) {
    if (h->nw + nwr > GEO_HZ_N || h->nr + nrd > GEO_HZ_N) { h->all = 1; return; }   // 记不下：之后的一律加屏障
    memcpy(h->w + h->nw, wr, sizeof *wr * (size_t)nwr); h->nw += nwr; memcpy(h->r + h->nr, rd, sizeof *rd * (size_t)nrd); h->nr += nrd;
}
// 某一阶段（st：0 顶点、1 片元、2 几何）的着色器经绑定读写的游戏内存区间。返回位：GEO_RK 读区间齐全、GEO_WK 写区间齐全（列举不全的绑定只清它涉及的那一位：
//   只读的贴图缓冲 / 只读句柄不让「写」变不明——11-06 金刚直通几何核 dda4fd846f421f3b 带只读贴图缓冲，原先整个记成列举不全，段里之后每笔都结束段）
enum { GEO_RK = 1, GEO_WK = 2 };
static int geo_stage_ranges(NvShader *gs, int st, geo_rg *rd, int *nrd, geo_rg *wr, int *nwr, int cap) {
    *nrd = *nwr = 0; int k = GEO_RK | GEO_WK, gm = 0;
    if (!gs) return k;
    for (int i = 0; i < gs->rf.nb; i++) { const nvn_rbind *b = &gs->rf.b[i];
        switch (b->kind) {
            case NVN_RB_SBUFP: case NVN_RB_UBOP: case NVN_RB_CBUF: {
                int w = b->kind == NVN_RB_SBUFP && b->rw;
                if (b->reg >= 16) { k &= ~(w ? GEO_WK | GEO_RK : GEO_RK); continue; }
                const p_bind_buf *u = b->kind == NVN_RB_SBUFP ? &S.ssbo[st][b->reg] : &S.ubo[st][b->reg];
                if (!u->addr) continue;
                if ((w ? *nwr : *nrd) >= cap) { k &= ~(w ? GEO_WK : GEO_RK); continue; }
                (w ? wr : rd)[(w ? (*nwr)++ : (*nrd)++)] = (geo_rg){u->addr, u->addr + (u->size ? u->size : 1)};
                break; }
            case NVN_RB_IMG: case NVN_RB_IMGA: if (b->rw) k &= ~GEO_WK; break;   // 图像写：所写贴图的内存这里不列举（读的贴图不在缓冲区间里，不影响读位）
            case NVN_RB_IMGU: case NVN_RB_SBUFU: case NVN_RB_SBUFQ: k &= ~(b->rw ? GEO_WK | GEO_RK : GEO_RK); break;   // 地址在统一缓冲 / 贴图句柄 / 推常量里：不列举
            case NVN_RB_TBUF: case NVN_RB_TBUFU: k &= ~GEO_RK; break;   // 贴图缓冲只读
            case NVN_RB_GMEM:   // 原始池表（地址从内存读来）：可达任一列表里的池，读区间列举不全；写区间在预测生效时 = 学到的池（gml_take 记写的同一份），否则也不全（审计 P1-119 / 120）
                k &= ~GEO_RK;
                if (b->rw && !gm++) { if (g_geo_gml.n < 0 || *nwr + g_geo_gml.n > cap) k &= ~GEO_WK; else { memcpy(wr + *nwr, g_geo_gml.r, sizeof *wr * (size_t)g_geo_gml.n); *nwr += g_geo_gml.n; } }
                break;
            default: break;
        }
    }
    return k;
}
static int geo_gs_ranges(NvShader *gs, geo_rg *rd, int *nrd, geo_rg *wr, int *nwr, int cap) { return geo_stage_ranges(gs, 2, rd, nrd, wr, nwr, cap) == (GEO_RK | GEO_WK); }
// 段内次序的另两种冒险（11-06 审计 P1-120）：段里 GPU 次序是「全部抓顶点 → 全部几何核 + 压缩 → 主遍全部合成绘制」，相对逐笔交错，
//   A 后一笔的抓顶点排到了前面几何核之前：它读的顶点缓冲 / 顶点阶段统一与存储缓冲若是前面几何核写的，读到写之前的字节；
//   B 前一笔的主遍绘制排到了后面几何核之后：后面几何核写的区间若是前面片元阶段读的，读到写之后的字节。
//   段里另记整段累计的几何核写区间 w 与片元读区间 r（geo_hz 的 hz 遇屏障会清，不能当整段记录）；入段前撞上就先结束段（逐笔次序），列举不全按撞上算
enum { GEO_RS_N = 1024 };   // 整段记录：有序、不相交，二分查（11-06 金刚：几何核写的区间分散，64 段 50 笔左右就记满）
//   C 前一笔片元阶段经原始池表的写（只在预测生效时入段，写区间 = 学到的池）排到了后面各笔的抓顶点与几何核之后：后一笔抓顶点读、几何核读 / 写若碰到，
//     读到写之前的字节、或被它盖掉。段里另记片元写区间 fw；gml：段里有按预测记的池表写（1 几何核、2 片元），之后每笔先按当时的预测补进去（geo_runset_gml）。
//   x：有池表写的着色器跑过（g_gml_on）后段里各笔列举到的读写区间，段里有按预测记的写时段末交给 g_gsr：预测漏学的池（gml_read 事后才发现）若落在其中，按同一口径报【红】
typedef struct { geo_rg w[GEO_RS_N], r[GEO_RS_N], fw[GEO_RS_N], x[GEO_RS_N]; int nw, nr, wall, rall, nfw, fwall, gml, nx, xall; geo_rg xb; } geo_runset;
// 有序不相交表 a 与任意区间组 x 有没有相交：每个 x 二分找第一段 hi > x.lo
static int geo_rs_hit(const geo_rg *x, int nx, const geo_rg *a, int na) {
    for (int k = 0; k < nx; k++) { int lo = 0, hi = na; while (lo < hi) { int m = (lo + hi) / 2; if (a[m].hi <= x[k].lo) lo = m + 1; else hi = m; }
        if (lo < na && a[lo].lo < x[k].hi) return 1; }
    return 0;
}
// 返回 0 = 不撞；1..6 = 撞上的原因（geo_hazard_why）
static int geo_run_hazard(const geo_runset *s, const geo_rg *cap, int ncap, int cap_known, const geo_rg *gw, int ngw, int gw_known) {
    if (s->wall) return 1;                                   // A：前面有几何核写区间列举不全
    if (!cap_known && s->nw) return 2;                       // A：本笔抓顶点读区间列举不全
    if (geo_rs_hit(cap, ncap, s->w, s->nw)) return 3;        // A：抓顶点读到前面几何核写过的
    if (s->rall && (ngw || !gw_known)) return 4;             // B：前面有片元读区间列举不全
    if (!gw_known && s->nr) return 5;                        // B：本笔几何核写区间列举不全
    if (geo_rs_hit(gw, ngw, s->r, s->nr)) return 6;          // B：几何核写到前面片元读过的
    return 0;
}
// C（见上）：返回 0 = 不撞；8..10 = 撞上的原因（geo_hazard_why）。gr / gw：本笔几何核读 / 写，cap：抓顶点读（含顶点阶段写）
static int geo_run_hazard_fw(const geo_runset *s, const geo_rg *cap, int ncap, int cap_known, const geo_rg *gr, int ngr, int gr_known, const geo_rg *gw, int ngw, int gw_known) {
    if (s->fwall) return 8;                                  // 前面有片元写区间列举不全
    if (!s->nfw) return 0;
    if (!cap_known || !gr_known || !gw_known) return 9;      // 本笔抓顶点 / 几何核的读写列举不全
    if (geo_rs_hit(cap, ncap, s->fw, s->nfw) || geo_rs_hit(gr, ngr, s->fw, s->nfw) || geo_rs_hit(gw, ngw, s->fw, s->nfw)) return 10;
    return 0;
}
static const char *const geo_hazard_why[] = {"-", "an earlier geometry kernel's writes are not all known", "this capture's reads are not all known", "this capture reads what an earlier geometry kernel wrote",
    "an earlier fragment stage's reads are not all known", "this geometry kernel's writes are not all known", "this geometry kernel writes what an earlier fragment stage read",
    "the pool table's write prediction ended inside the run", "an earlier fragment stage's writes are not all known",
    "this capture's or geometry kernel's accesses are not all known after a fragment stage's pool-table writes", "this capture or geometry kernel touches what an earlier fragment stage wrote through the pool table"};
// 有序区间表里并进一段：重叠或相接的合成一段（段里各笔几何核多写同一批暂存 / 相邻输出，合并后段数很少）；放不下返回 0
static int geo_rg_merge_in(geo_rg *a, int *n, geo_rg x) {
    int i = 0, h = *n; while (i < h) { int m = (i + h) / 2; if (a[m].hi < x.lo) i = m + 1; else h = m; }   // 第一段与 x 重叠或相接、或在 x 之后的
    int j = i; while (j < *n && a[j].lo <= x.hi) { if (a[j].lo < x.lo) x.lo = a[j].lo; if (a[j].hi > x.hi) x.hi = a[j].hi; j++; }
    int nn = *n - (j - i) + 1; if (nn > GEO_RS_N) return 0;
    memmove(a + i + 1, a + j, sizeof *a * (size_t)(*n - j)); a[i] = x; *n = nn; return 1;
}
static void geo_runset_add(geo_runset *s, const geo_rg *gw, int ngw, int gw_known, const geo_rg *fr, int nfr, int fr_known) {
    if (!gw_known) s->wall = 1; else for (int i = 0; i < ngw && !s->wall; i++) if (!geo_rg_merge_in(s->w, &s->nw, gw[i])) s->wall = 1;   // 11-06 金刚：不合并时 33 笔就记满 64 段
    if (!fr_known) s->rall = 1; else for (int i = 0; i < nfr && !s->rall; i++) if (!geo_rg_merge_in(s->r, &s->nr, fr[i])) s->rall = 1;
}
static void geo_runset_add_fw(geo_runset *s, const geo_rg *fw, int nfw, int fw_known) {
    if (!fw_known) s->fwall = 1; else for (int i = 0; i < nfw && !s->fwall; i++) if (!geo_rg_merge_in(s->fw, &s->nfw, fw[i])) s->fwall = 1;
}
// 段里按预测记的池表写，在新的一笔查冒险前按这一刻的预测补全：前面一笔记写时取的预测，可能比它的 gml_take 真正用的少（中间另一份写记录读完、学到了新池）；
//   学到的只增不减，这一刻的一份包含那份。预测已不生效（记满：写在哪都算）返回 7
static int geo_runset_gml(geo_runset *s) {
    if (!s->gml) return 0;
    if (g_geo_gml.n < 0) return 7;
    if (s->gml & 1) geo_runset_add(s, g_geo_gml.r, g_geo_gml.n, 1, NULL, 0, 1);
    if (s->gml & 2) geo_runset_add_fw(s, g_geo_gml.r, g_geo_gml.n, 1);
    return 0;
}
static void geo_runset_note(geo_runset *s, const geo_rg *x, int n) {   // x：记不下时退成一段包住全部的区间（只用于事后查，宽了只会多报）
    for (int i = 0; i < n; i++) {
        if (!s->xb.hi || x[i].lo < s->xb.lo) s->xb.lo = x[i].lo;
        if (x[i].hi > s->xb.hi) s->xb.hi = x[i].hi;
        if (!s->xall && !geo_rg_merge_in(s->x, &s->nx, x[i])) s->xall = 1;
    }
}
// 段末：段里有按预测记的池表写，把 x 交给 g_gsr（同 ubp_get 记的统一缓冲影子，带本次提交的栅栏），gml_read 事后发现漏学的池时一并查
static void geo_runset_gsr(const geo_runset *s) {
    if (!s->gml || !g_gml_on) return;
    const geo_rg *x = s->xall ? &s->xb : s->x; int n = s->xall ? (s->xb.hi > s->xb.lo) : s->nx;
    pthread_mutex_lock(&g_gsr_mu);
    for (int i = 0; i < n; i++) { uint64_t k = g_gsr_n++ % GSR_N; g_gsr[k].fence = g_fence + 1; g_gsr[k].lo = x[i].lo; g_gsr[k].hi = x[i].hi; }
    pthread_mutex_unlock(&g_gsr_mu);
}
static struct { int active, gpc, gpk; id cap, gs, cap_pso; uint64_t runs, draws, kernels, barriers, cut, hazard; geo_hz hz; geo_runset rs; id pack; uint64_t *jobs; size_t njobs, cjobs; mxe_job *mx; int nmx, cmx; uint64_t plain; } g_gr;   // cap_pso: the capture lane's current pipeline (only captures set one there; cleared when the lanes begin)   // gpc / gpk: the lane encoders' GPUPROF labels (gp_lane_begin); jobs: the run's compactions, encoded at its end
// 段为什么结束（诊断，随「geometry run」那行出）：调用方在结束段之前把原因放进 g_gr_why，geo_run_flush 取走并清零；没放的算「结束主遍 / 开别的编码器 / 提交 / 等 GPU」。
//   一帧几百笔几何绘制而每段只装一笔时（每段都断主遍、附件整张读进写出），这一行说清是谁断的
enum { GEO_END_OTHER, GEO_END_OP, GEO_END_DRAW, GEO_END_INELIG, GEO_END_CUT, GEO_END_HAZARD, GEO_END_N };
enum { GEO_INEL_WAIT, GEO_INEL_UBD, GEO_INEL_DEFER, GEO_INEL_HOIST, GEO_INEL_FSW, GEO_INEL_REST, GEO_INEL_N };
static int g_gr_why, g_gr_why_op, g_gr_why_inel;
static uint64_t g_gr_end[GEO_END_N], g_gr_end_op[128], g_gr_inel[GEO_INEL_N];
static int geo_run_active(void) { return g_gr.active; }   // for pass_color_unwritten (nvn_mtl.m, before this include)
// Lane 1: encode the run's queued mesh host stages (mesh_run_job) as one batch, after the run's geometry kernels so far
static void mesh_run_drain(void) {
    if (!g_gr.nmx) return;
    int lane = g_rlane; rec_lane(1);
    if (g_gr.kernels) { enc_c_inner_barrier(g_gr.gs); g_gr.barriers++; } g_gr.hz.all = 1;   // after the run's geometry kernels; the ones after it barrier too
    mtk_emu_flush(g_gr.gs, g_gr.mx, g_gr.nmx); g_gr.nmx = 0;
    rec_lane(lane); args_dirty_all();
}
static int geo_fs_writes(NvShader *fs) {
    if (!fs) return 0;
    for (int i = 0; i < fs->rf.nb; i++) if (fs->rf.b[i].rw && (fs->rf.b[i].kind == NVN_RB_IMG || fs->rf.b[i].kind == NVN_RB_IMGU || fs->rf.b[i].kind == NVN_RB_IMGA || fs->rf.b[i].kind == NVN_RB_SBUFP ||
        (fs->rf.b[i].kind == NVN_RB_GMEM && g_geo_gml.n < 0))) return 1;   // 原始池表写：预测生效时写区间已知（geo_stage_ranges），入段按 C 查；否则同 g_pass_swrite
    return 0;
}
static void geo_run_flush(void) {
    int why = g_gr_why; g_gr_why = 0;   // 先取走：没在段里时也清，免得留给下一次
    if (!g_gr.active) return;
    g_gr_end[why]++;
    if (why == GEO_END_OP) g_gr_end_op[(unsigned)g_gr_why_op < 128 ? g_gr_why_op : 127]++;
    if (why == GEO_END_INELIG) g_gr_inel[g_gr_why_inel]++;
    if (g_rlane) MDIE("geometry run: flushed from lane %d (an encoder, commit or wait inside a lane)", g_rlane);
    g_gr.active = 0;   // first: the calls below must not come back here
    if (g_gr.cap) { gp_lane_end(g_gr.cap, g_gr.gpc, (uint32_t)g_gr.draws); enc_full_barrier_end(g_gr.cap); [g_gr.cap endEncoding]; }
    rec_lane(1);
    mesh_run_drain();
    if (g_gr.njobs) {   // the compactions, after every geometry kernel of the run: each reads only its own kernel's records and writes its own scratch
        enc_c_inner_barrier(g_gr.gs); NSUInteger th = MIN([(id<MTLComputePipelineState>)g_gr.pack maxTotalThreadsPerThreadgroup], 1024) / 32 * 32;
        enc_c_pso(g_gr.gs, g_gr.pack);   // every compaction of the run uses the same pipeline: set it once
        for (size_t i = 0; i < g_gr.njobs; i++) { enc_c_args(g_gr.gs, g_gr.jobs[i], 0); enc_c_dispatch(g_gr.gs, MTLSizeMake(1, 1, 1), MTLSizeMake(th, 1, 1)); }
        g_gr.njobs = 0; }
    gp_lane_end(g_gr.gs, g_gr.gpk, (uint32_t)g_gr.draws); enc_full_barrier_end(g_gr.gs); [g_gr.gs endEncoding]; rec_lane(0);
    geo_runset_gsr(&g_gr.rs); memset(&g_gr.hz, 0, sizeof g_gr.hz); memset(&g_gr.rs, 0, sizeof g_gr.rs);
    rec_lane_splice();
    g_gr.cap = g_gr.gs = g_gr.cap_pso = nil; g_vt_valid = 0; args_dirty_all();
    if (g_gr.runs == 1 || g_gr.runs % 2000 == 0) MLOG("geometry run: %llu runs, %llu geometry draws in them, barriers before %llu of %llu geometry kernels, %llu runs ended to cut the segment for scratch, %llu ended for a read / write across the run's reordering (a run: captures, geometry kernels and main-pass draws each in one encoder)",
        (unsigned long long)g_gr.runs, (unsigned long long)g_gr.draws, (unsigned long long)g_gr.barriers, (unsigned long long)g_gr.kernels, (unsigned long long)g_gr.cut, (unsigned long long)g_gr.hazard);
    if (g_gr.runs == 1 || g_gr.runs % 2000 == 0) {
        int top[3] = {-1, -1, -1};   // 结束段次数最多的三种操作
        for (int o = 0; o < 128; o++) if (g_gr_end_op[o]) for (int k = 0; k < 3; k++) if (top[k] < 0 || g_gr_end_op[o] > g_gr_end_op[top[k]]) { for (int j = 2; j > k; j--) top[j] = top[j - 1]; top[k] = o; break; }
        char ops[96] = ""; size_t n = 0;
        for (int k = 0; k < 3 && top[k] >= 0 && n < sizeof ops; k++) n += (size_t)snprintf(ops + n, sizeof ops - n, "%sop %d%s %llu", k ? ", " : "", top[k], top[k] == OP_UPDATE_UB ? " (inline uniform update)" : "", (unsigned long long)g_gr_end_op[top[k]]);
        MLOG("geometry run ends by cause: a state-changing operation %llu (%s), a non-geometry draw %llu, the next geometry draw not eligible %llu (waits for GPU-written index / vertex data %llu, "
             "inline uniform updates pending %llu, deferred pass %llu, hoisted / merged pass %llu, fragment stage writes memory %llu, other %llu), scratch cut %llu, read / write hazard %llu, "
             "pass end / other encoder / commit / wait %llu", (unsigned long long)g_gr_end[GEO_END_OP], n ? ops : "-", (unsigned long long)g_gr_end[GEO_END_DRAW], (unsigned long long)g_gr_end[GEO_END_INELIG],
             (unsigned long long)g_gr_inel[GEO_INEL_WAIT], (unsigned long long)g_gr_inel[GEO_INEL_UBD], (unsigned long long)g_gr_inel[GEO_INEL_DEFER], (unsigned long long)g_gr_inel[GEO_INEL_HOIST],
             (unsigned long long)g_gr_inel[GEO_INEL_FSW], (unsigned long long)g_gr_inel[GEO_INEL_REST], (unsigned long long)g_gr_end[GEO_END_CUT], (unsigned long long)g_gr_end[GEO_END_HAZARD],
             (unsigned long long)g_gr_end[GEO_END_OTHER]);
    }
}
// The run begins: no encoder is open (geo_draw ended the pass); the two lane encoders begin with a full barrier, not through main_barrier_begin (its state follows the main stream)
// The capture lane (main stream): opened with the run, or (a run a mesh draw began) by its first geometry draw; a run of mesh draws alone opens none
static void geo_run_cap(void) {
    id rd = pass_desc(); [rd setRenderTargetWidth:1]; [rd setRenderTargetHeight:1]; [rd setDefaultRasterSampleCount:1];   // as geo_draw's capture pass
    g_gr.cap = [g_cb renderCommandEncoderWithDescriptor:rd]; g_gr.cap_pso = nil; enc_full_barrier_begin(g_gr.cap);
    enc_r_table(g_gr.cap, g_vt, MTLRenderStageVertex); enc_r_table(g_gr.cap, g_ft, MTLRenderStageFragment); g_gr.gpc = gp_lane_begin(g_gr.cap, 'P', (uint32_t)g_gr.draws);
}
static void geo_run_begin_x(int cap) {   // cap 0：网格绘制开的段，第一笔几何绘制才开抓顶点道（geo_run_cap）
    main_barrier_reset();
    g_gr.cap = nil; if (cap) geo_run_cap();
    rec_lane(1); g_gr.gs = [g_cb computeCommandEncoder]; enc_full_barrier_begin(g_gr.gs); enc_c_table(g_gr.gs, g_ct); g_gr.gpk = gp_lane_begin(g_gr.gs, 'K', (uint32_t)g_gr.draws); rec_lane(0);
    g_gr.active = 1; g_gr.runs++;
}
static void geo_run_begin(void) { geo_run_begin_x(1); }
// Before a geometry draw takes `total` bytes of scratch: once the segment holds ARENA_CUT bytes, hand it to the GPU so its scratch comes back (NvArena, nvn_mtl.m). Outside a run no
//   encoder is open there (geo_draw ended the pass). Inside one the main pass is open and sub_cut cannot cut: the run ends first (its lanes go into the stream), then the pass, then the
//   cut, and the draw begins a new run. 11-06 Donkey Kong (维护者's hand test): a run's draws piled their scratch into one uncut segment past ARENA_CAP, and the next block waited
//   ARENA_WAIT_S for the GPU ("frame scratch: waited 2 s", a 2.1 s freeze)
static void geo_scratch_cut(int run, uint64_t total) {
    if (g_arena_seg + total < ARENA_CUT) return;
    if (run && g_gr.active) { g_gr_why = GEO_END_CUT; geo_run_flush(); rp_end(); g_gr.cut++; }
    sub_cut();
}
// Exec loop: the ops a run lets through (state only; draws decide in draw())
static int geo_run_keeps(uint32_t op) {
    switch (op) {
        case OP_DRAW: case OP_DRAW_IDX: case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_PRIM_RESTART: case OP_ALPHA_REF:
        case OP_JUMP: case OP_CALL: case OP_END: return 1;
        default: return 0;
    }
}
static void geo_lane(int k) { rec_lane(k); g_vt_valid = 0; args_dirty_all(); }
// 网格绘制入几何连段的三个助手（mtk_emu_run / mesh_tasks_draw 用，见 mesh_run_ok）
static int mesh_run_enter(int run) {
    if (run && g_gr.active && (g_gr.rs.nw || g_gr.rs.wall)) { g_gr_why = GEO_END_HAZARD; geo_run_flush(); }   // a geometry kernel of the run wrote memory: the host stages could read it
    if (!run) { if (g_gr.active) { g_gr_why = GEO_END_INELIG; g_gr_why_inel = GEO_INEL_REST; } geo_run_flush(); return 0; }
    return g_gr.active;
}
static void mesh_run_job(const mxe_job *j) {
    if (!g_gr.active) { geo_run_begin_x(0); g_mr_runs_started++; }
    g_gr.rs.rall = 1;   // this draw's fragment reads are not listed: a later geometry kernel that writes ends the run
    // queued: the run's mesh host stages go into lane 1 together (mesh_run_drain), each stage a wave over all of them with one dependency between waves, not 3 per draw.
    //   Each job writes only its own exchange buffer and reads game memory no stage of the run writes (mesh_run_ok), so only the order against the geometry kernels matters
    if (g_gr.nmx == g_gr.cmx) { g_gr.cmx = g_gr.cmx ? 2 * g_gr.cmx : 64; g_gr.mx = realloc(g_gr.mx, (size_t)g_gr.cmx * sizeof *g_gr.mx); if (!g_gr.mx) MDIE("geometry run: out of memory"); }
    g_gr.mx[g_gr.nmx++] = *j;
    g_gr.draws++; g_mr_draws++;
    if (g_mr_draws == 1 || g_mr_draws % 20000 == 0) MLOG("task / mesh host stages in geometry runs: %llu draws in %llu runs started by a mesh draw", (unsigned long long)g_mr_draws, (unsigned long long)g_mr_runs_started);
}
// 网格连段里夹着的普通绘制（不写内存、不需遍外前处理）照原次序画进道 2 的主遍，不结束段（原先一帧约 1.5 万次因此断段）。
//   道 2 里普通绘制与网格绘制的相对次序不变；段里之后有写内存的几何核就结束段（rall）
static int mesh_run_plain(const p_draw *dw, const p_draw_idx *di) {
    if (!mesh_run_on() || !g_gr.active || !g_enc || g_gr.kernels || g_rlane || S.rast_discard || S.tf_on || g_nubd || !S.prog) return 0;
    if ((dw && (dw->mdi_cnt || dw->ind)) || (di && (di->mdi_cnt || di->ind))) return 0;
    __unsafe_unretained NvProg *pr = prog_get(S.prog);
    if (!pr || pr->sh[2] || pr->sh[3] || pr->sh[4] || shader_writes(pr->sh[0]) || geo_fs_writes(pr->sh[1]) || shader_writes(pr->sh[1])) return 0;
    if (ixc_wanted(dw, di) || (di && wide_wanted(di))) return 0;
    g_gr.rs.rall = 1; g_gr.plain++;
    if (g_gr.plain == 1 || g_gr.plain % 20000 == 0) MLOG("geometry run: %llu plain draws kept in mesh runs' main pass", (unsigned long long)g_gr.plain);
    return 1;
}
static int mesh_run_lane2(int on) {
    if (on) { if (!g_gr.active) return 0; geo_lane(2); if (!g_enc) { g_game_barrier = 1; main_barrier_reset(); } return 1; }   // the main pass opened in lane 2 waits for everything before it: the host stages in lane 1
    geo_lane(0); if (!g_gr.active) { static int said; if (!said++) MLOG("【红】 task / mesh draw: the geometry run ended inside its main-pass draw"); }
    return 0;
}
// 几何模拟变体按程序缓存（S2Arm 11-06 能耗线：金刚提交线程 Time Profiler，每笔几何绘制现拼「<键>_bg<n>」「_bgv」再按名字查表，
//   shader_get + 拼字符串占翻译时间约 13.5%）。缓存挂在程序对象上（随它销毁），记着当时的几何阶段 geo_base：几何阶段换了整格作废重查；
//   读记录的顶点变体按深度模式分两格（shader_get 的顶点阶段按 g_depth_zero 取库）；没查到的不缓存，下一笔照原样再查。
//   着色器缓存的条目一经放入不再替换（shader_load_depth），所以缓存住的对象就是 shader_get 会再给的那个
static NvShader *geo_lookup_shader(NSString *k, int st) { return shader_get(k, st); }
static NvShader *(*g_geo_lookup)(NSString *, int) = geo_lookup_shader;   // 单测换成计数版
static void geo_variants(NvProg *pr, NvShader *base, int topology, NvShader **gs, NvShader **reader) {
    *gs = *reader = nil;
    if (topology < 0 || topology >= 5) return;
    if (pr->geo_base != base) { for (int i = 0; i < 5; i++) pr->geo_gs[i] = pr->geo_ps[i] = nil; pr->geo_rd[0] = pr->geo_rd[1] = pr->geo_rc[0] = pr->geo_rc[1] = nil; pr->geo_base = base; }
    int dz = atomic_load(&g_depth_zero) ? 1 : 0;
    if (!pr->geo_gs[topology] || !pr->geo_rd[dz]) {
        NSString *key = [[NSString stringWithUTF8String:base->key] substringToIndex:16];
        if (!pr->geo_gs[topology]) pr->geo_gs[topology] = g_geo_lookup([key stringByAppendingFormat:@"_bg%d", topology], 2);
        if (!pr->geo_rd[dz]) pr->geo_rd[dz] = g_geo_lookup([key stringByAppendingString:@"_bgv"], 0);
    }
    *gs = pr->geo_gs[topology]; *reader = pr->geo_rd[dz];
}
// 直通几何快路（S2Arm 11-06 能耗线）：头标直通、几何本体不写输出也不自己发射的几何着色器（反射 geometry.passthrough，Dsh.cs GeometryFinish），输出图元就是输入图元、
//   顶点值就是顶点阶段抓下的输出。几何核换成只算副作用的 _bgs<拓扑>（不写记录、不写下标），主遍顶点换成读抓顶点缓冲的 _bgc，按 CPU 装好的输入图元下标（map）直接画，
//   不派压缩核。金刚 f5800 回放：几何核道 ÷ 抓顶点道 3.4–3.9 → 1.3–2.3（只删记录写的测量库）。
//   只在：没开变换反馈、单实例、单次调用（实例 / 调用多于 1 时记录与图元号按组排，主遍下标与图元号对不上）、非邻接拓扑（点 0 / 线 1 / 三角 3：邻接的直通取 0 2 4 / 1 2 号顶点，
//   下标与 map 不同）。老库没有标记、或缺 _bgs / _bgc：照旧走记录。DM_NVN_GEO_PT=0 关（回放逐像素对照用）。查表结果按程序缓存，规则同 geo_variants
static int g_geo_pt_off = -1; static uint64_t g_geo_pt_draws;
static void geo_pt_variants(NvProg *pr, NvShader *base, NvShader *gs, int topology, NvShader **side, NvShader **creader) {
    *side = *creader = nil;
    if (g_geo_pt_off < 0) { const char *e = getenv("DM_NVN_GEO_PT"); g_geo_pt_off = e && !strcmp(e, "0"); }
    if (g_geo_pt_off || !gs || !gs->geo_pt || topology < 0 || topology >= 5) return;
    int dz = atomic_load(&g_depth_zero) ? 1 : 0;
    if (!pr->geo_ps[topology] || !pr->geo_rc[dz]) {
        NSString *key = [[NSString stringWithUTF8String:base->key] substringToIndex:16];
        if (!pr->geo_ps[topology]) pr->geo_ps[topology] = g_geo_lookup([key stringByAppendingFormat:@"_bgs%d", topology], 2);
        if (!pr->geo_rc[dz]) pr->geo_rc[dz] = g_geo_lookup([key stringByAppendingString:@"_bgc"], 0);
    }
    *side = pr->geo_ps[topology]; *creader = pr->geo_rc[dz];
}
// 这笔走不走快路：走就把几何核 / 主遍顶点换成 _bgs / _bgc（条件见上）
static int geo_pt_pick(NvProg *pr, NvShader *base, int tf, int instances, int topology, NvShader **gs, NvShader **reader) {
    if (tf || instances != 1 || (*gs)->geo_invocations != 1 || !(topology == 0 || topology == 1 || topology == 3)) return 0;
    NvShader *side, *creader; geo_pt_variants(pr, base, *gs, topology, &side, &creader);
    if (!side || !creader || side->geo_invocations != 1) return 0;
    *gs = side; *reader = creader; return 1;
}
static int geo_vs_writes(NvShader *vs) { for (int i = 0; i < vs->rf.nb; i++) if (vs->rf.b[i].rw) return 1; return 0; }   // the vertex stage writes memory (storage, images, pool tables)
static void geo_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0) {
    NvShader *vs = pr->sh[0], *base = pr->sh[2];
    // Feedback on with a geometry shader that has no capture layout (control section strides all 0, e.g. Xenoblade 3 95bfab6122539993): the GPU captures nothing and still draws,
    // as the vertex path does for a vertex shader without one (draw, not tf_draw)
    int tf = S.tf_on && (base && (base->geo_tf_stride[0] || base->geo_tf_stride[1] || base->geo_tf_stride[2] || base->geo_tf_stride[3]));
    if (!vs || !vs->geo_fn || !base || !base->geo_vertices || pr->sh[3] || pr->sh[4]) {
        GEO_RED("【红】 geometry draw cannot capture this pipeline (vertex %s, geometry %s, tessellation %d, feedback %d)", vs ? vs->key : "-", base ? base->key : "-", !!(pr->sh[3] || pr->sh[4]), S.tf_on); return;
    }
    p_draw dw = dw0 ? *dw0 : (p_draw){0}; p_draw_idx di = di0 ? *di0 : (p_draw_idx){0};
    if (di0 && (di.itype < 0 || di.itype > 2)) { GEO_RED("【红】 geometry index type %d is invalid", di.itype); return; }
    uint64_t ind = dw0 ? dw.ind : di.ind;
    // Indirect records and GPU-written index data need a coherent CPU snapshot for primitive assembly. Index data waits only when the GPU may have written it and not finished:
    // a write queued in this submission (pend), a storage write in it (sbw), or a GPU write of an earlier submission still running (gw); the same test as ubp_get. Waiting on every
    // indexed draw made Xenoblade 3 (95bfab6122539993, hundreds of indexed geometry draws a frame) take 0.4-2 s a frame on the submit thread
    uint64_t ilo = di0 ? di.ibuf : 0, ihi = di0 ? di.ibuf + ((uint64_t)di.count << di.itype) : 0;
    int wait = ind != 0 || (di0 && (pend_hits(ilo, ihi - ilo) || g_sbw_over || sbw_hits(ilo, ihi) || gw_live(ilo, ihi) || ifw_hits(ilo, ihi)));
    for (int i = 0; i < 16; i++) if (S.vbuf[i].size && pend_hits(S.vbuf[i].address, S.vbuf[i].size)) wait = 1;
    for (int i = 0; i < vs->rf.nv; i++) if (vconv_wanted(&vs->rf, i, NULL, 0)) wait = 1;
    geo_gml_snap();   // 本笔各阶段的原始池表写按这一份预测（geo_fs_writes / geo_stage_ranges）
    int run = !g_m3 && rec_on() && g_cb && !tf && !wait && !ind && !S.rast_discard && !g_nubd && !defer_has_active() && !g_hoist_n && !g_fz.active && !geo_fs_writes(pr->sh[1]);
    // 这笔的抓顶点读区间（顶点缓冲 + 顶点阶段统一 / 存储缓冲）、几何核写区间、片元读区间（审计 P1-120，见 geo_runset）；撞上本段已记的就先结束段
    geo_rg h_cap[48], h_gw[32], h_gr[32], h_fr[32], h_fw[32], h_tmp[32]; int h_ncap = 0, h_ngw = 0, h_ngr = 0, h_nfr = 0, h_nfw = 0, h_nt = 0, h_ck = 1, h_gk = 1, h_grk = 1, h_fk = 1, h_fwk = 1;
    if (run) {
        for (int i = 0; i < 16; i++) if (S.vbuf[i].address && S.vbuf[i].size) h_cap[h_ncap++] = (geo_rg){S.vbuf[i].address, S.vbuf[i].address + S.vbuf[i].size};
        h_ck = geo_stage_ranges(vs, 0, h_cap + h_ncap, &h_nt, h_tmp, &h_ngw, 16) == (GEO_RK | GEO_WK); h_ncap += h_nt;   // 顶点阶段写的也当读算，两位都要齐
        if (h_ngw) { memcpy(h_cap + h_ncap, h_tmp, sizeof *h_tmp * (size_t)h_ngw); h_ncap += h_ngw; } h_ngw = 0;   // 顶点阶段写的也当读算（同一笔前后都要对）
        int gk = geo_stage_ranges(base, 2, h_gr, &h_ngr, h_gw, &h_ngw, 32); h_gk = !!(gk & GEO_WK); h_grk = !!(gk & GEO_RK);   // B 看几何核写，C 读写都看
        int fk = geo_stage_ranges(pr->sh[1], 1, h_fr, &h_nfr, h_fw, &h_nfw, 32); h_fk = !!(fk & GEO_RK); h_fwk = !!(fk & GEO_WK);   // B 看片元读；片元的写入段的只有预测下的原始池表写（geo_fs_writes），C 看它
        int why = 0;
        if (g_gr.active && !(why = geo_runset_gml(&g_gr.rs)) && !(why = geo_run_hazard(&g_gr.rs, h_cap, h_ncap, h_ck, h_gw, h_ngw, h_gk))) why = geo_run_hazard_fw(&g_gr.rs, h_cap, h_ncap, h_ck, h_gr, h_ngr, h_grk, h_gw, h_ngw, h_gk);
        if (why) { g_gr_why = GEO_END_HAZARD; geo_run_flush(); if (g_gr.hazard++ < 8) MLOG("geometry run ended before a draw (%llu ends so far): %s (geometry %s)", (unsigned long long)g_gr.hazard, geo_hazard_why[why], base->key); }
    }
    if (!run) {
        g_gr_why = GEO_END_INELIG;
        g_gr_why_inel = wait ? GEO_INEL_WAIT : g_nubd ? GEO_INEL_UBD : defer_has_active() ? GEO_INEL_DEFER : (g_hoist_n || g_fz.active) ? GEO_INEL_HOIST : geo_fs_writes(pr->sh[1]) ? GEO_INEL_FSW : GEO_INEL_REST;
        geo_run_flush();
    }
    if (!(run && g_gr.active)) {
        if (defer_has_active()) defer_fallback();
        lc_land(); rp_flush(); rp_end(); ubd_land();
    }
    if (wait) { mdi_wait(); if (g_mdi_ev.signaledValue < g_mdi_v) return; memset(g_vconv, 0, sizeof g_vconv); }
    if (ind) {
        uint32_t a[5] = {0}; size_t n = dw0 ? 16 : 20;
        const uint8_t *ha = nvn_host_ptr(ind, n); if (!ha) { GEO_RED("【红】 geometry indirect arguments are not readable"); return; }
        memcpy(a, ha, n);
        if (!a[0] || !a[1]) return;
        if (a[0] > INT32_MAX || a[1] > INT32_MAX) { GEO_RED("【红】 geometry indirect draw exceeds the host count range"); return; }
        if (dw0) { dw.count = a[0]; dw.instances = a[1]; dw.first = a[2]; dw.base_instance = a[3]; dw.ind = dw.mdi_cnt = 0; }
        else { di.count = a[0]; di.instances = a[1]; di.ibuf += (uint64_t)a[2] << di.itype; di.base_vertex = (int32_t)a[3]; di.base_instance = a[4]; di.ind = di.mdi_cnt = 0; }
    }
    int prim = dw0 ? dw.prim : di.prim, topology = nvn_geo_topology(prim);
    int count = dw0 ? dw.count : di.count, instances = dw0 ? dw.instances : di.instances;
    uint32_t first_instance = (uint32_t)(dw0 ? dw.base_instance : di.base_instance);
    g_draw_binst = first_instance;
    if (count <= 0 || instances < 0) return;
    if (!instances) instances = 1;
    if (topology < 0 || count > (1 << 22)) { GEO_RED("【红】 geometry primitive %d or count %d exceeds the assembly path", prim, count); return; }
    NvShader *gs, *reader; geo_variants(pr, base, topology, &gs, &reader);
    if (!gs || !reader) { nvn_be_bad_draw_feat(); return; }
    int fast = geo_pt_pick(pr, base, tf, instances, topology, &gs, &reader);
    id<MTLRenderPipelineState> capture = geo_capture_pso(vs); if (!capture) { nvn_be_bad_draw_feat(); return; }
    if (gs->geo_pso_bad) { nvn_be_bad_draw_feat(); return; }
    if (!gs->geo_pso) { NSError *e = nil; gs->geo_pso = [g_dev newComputePipelineStateWithFunction:gs->fn error:&e];
        if (!gs->geo_pso) { gs->geo_pso_bad = 1; GEO_RED("【红】 geometry compute %s: %s（之后这支的几何绘制跳过、不重建）", gs->key, e.localizedDescription.UTF8String); return; } }
    if (gs->geo_pso.maxTotalThreadsPerThreadgroup < 32) { GEO_RED("【红】 geometry kernel cannot run a 32-thread group"); return; }
    static id<MTLComputePipelineState> pack;
    if (!pack) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_nvn_geo_pack_msl) options:nil error:&e];
        if (lib) pack = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_pack_indices"] error:&e];
        if (!pack) { GEO_RED("【红】 geometry index compaction: %s", e.localizedDescription.UTF8String); return; } }

    // Index scratch kept across draws (submit thread only; 11-06 quality line: two malloc / free per geometry draw were a measurable share of the submit thread)
    static uint32_t *order, *unique; static size_t idx_cap;
    if ((size_t)count > idx_cap) {
        size_t cap = MAX((size_t)count, idx_cap * 2); uint32_t *o2 = realloc(order, cap * 4), *u2 = o2 ? realloc(unique, cap * 4) : NULL;
        if (o2) order = o2;
        if (!o2 || !u2) { GEO_RED("【红】 geometry index allocation failed"); return; }
        unique = u2; idx_cap = cap;
    }
    uint32_t lo = UINT32_MAX, hi = 0; size_t nu = 0;
    int restart = di0 && S.restart.on;
    int isz = di0 ? 1 << di.itype : 0;
    uint32_t restart_index = S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX);
    const uint8_t *hib = NULL;   // the index bytes on the host (nvn_host_ptr: a virtual pool's address is no host memory)
    if (di0 && (di.itype < 0 || di.itype > 2 || !(hib = nvn_host_ptr(di.ibuf, (uint64_t)count * isz)))) {
        GEO_RED("【红】 geometry index buffer is not readable"); return;
    }
    // 没有图元重启时（11-16 能耗线：040074A01BF12000 帧回放，一帧 121 笔几何、每笔约 1k 个下标，这段原占 geo_draw 约三成指令）：按下标宽度各走一个紧循环，
    //   只记 order 与上下界（编译器向量化），去重直接读 order（装配之后它不再用），不再逐个抄进 unique；有重启时照旧逐个判
    if (!restart) {
        const uint8_t *q = di0 ? hib : NULL; uint32_t l = UINT32_MAX, h = 0;
        if (!di0) { for (int i = 0; i < count; i++) order[i] = (uint32_t)i; l = 0; h = (uint32_t)count - 1; }
        else if (isz == 2) for (int i = 0; i < count; i++) { uint16_t x; memcpy(&x, q + 2 * (size_t)i, 2); order[i] = x; l = MIN(l, (uint32_t)x); h = MAX(h, (uint32_t)x); }
        else if (isz == 4) for (int i = 0; i < count; i++) { uint32_t x; memcpy(&x, q + 4 * (size_t)i, 4); order[i] = x; l = MIN(l, x); h = MAX(h, x); }
        else for (int i = 0; i < count; i++) { uint32_t x = q[i]; order[i] = x; l = MIN(l, x); h = MAX(h, x); }
        lo = l; hi = h; nu = (size_t)count;
    } else for (int i = 0; i < count; i++) {
        uint32_t v = (uint32_t)i;
        if (di0) {   // typed loads, not a variable-length memcpy per index (a library call each; unaligned-safe through memcpy of a fixed size)
            const uint8_t *q = hib + (size_t)i * isz;
            if (isz == 2) { uint16_t h; memcpy(&h, q, 2); v = h; } else if (isz == 4) memcpy(&v, q, 4); else v = *q;
        }
        order[i] = v;
        if (v == restart_index) continue;
        lo = MIN(lo, v); hi = MAX(hi, v); unique[nu++] = v;
    }
    if (!nu) { return; }
    int64_t first = di0 ? (int64_t)lo + di.base_vertex : dw.first;
    uint64_t span = (uint64_t)hi - lo + 1;
    if (first < 0 || first + span > (uint64_t)UINT32_MAX + 1) { GEO_RED("【红】 geometry vertex range is outside 32-bit vertex IDs"); return; }
    // Normalize only after assembly, so a restart value cannot collide with a
    // normalized vertex number. Indexed captures use a sorted unique index list.
    size_t words = nvn_geo_assemble(prim, order, count, restart, restart_index, NULL, 0);
    int width = nvn_geo_width(topology);
    uint64_t np = words == SIZE_MAX ? 0 : words / width;
    if (!np) { return; }
    uint64_t groups = np * (uint64_t)instances * gs->geo_invocations;
    if (groups > UINT32_MAX || span * (uint64_t)instances > (256ull << 20) / (144 * 4) ||
        (uint64_t)first_instance + instances > (uint64_t)UINT32_MAX + 1) {
        GEO_RED("【红】 geometry draw exceeds the capture or invocation bound"); return;
    }
    uint64_t input_size = span * instances * 144 * 4;
    uint64_t output_size = fast ? 0 : groups * gs->geo_vertices * gs->geo_stride * 4;   // the fast path's kernel writes no records and no indices
    uint64_t raw_size = fast ? 0 : groups * gs->geo_index_stride * 4;
    uint64_t packed_size = fast ? 0 : groups * gs->geo_vertices * (gs->geo_output == 7 ? 3 : gs->geo_output == 6 ? 2 : 1) * 4;
    uint64_t total = input_size + output_size + raw_size + packed_size + words * 4 + nu * 4 + 4096;
    if (groups > UINT32_MAX || total > (256ull << 20) || np > UINT32_MAX) {
        GEO_RED("【红】 geometry draw needs %llu MiB of scratch, above the 256 MiB bound", (unsigned long long)(total >> 20)); return;
    }
    geo_scratch_cut(run, total);
    id<MTLBuffer> scratch; size_t off = arena_alloc((size_t)total, &scratch);
    uint8_t *cpu = (uint8_t *)scratch.contents + off; uint64_t gpu = scratch.gpuAddress + off;
    size_t used = 0;
    #define GEO_ALLOC(N, G, P) do { used = (used + 255) & ~(size_t)255; (G) = gpu + used; (P) = cpu + used; used += (size_t)(N); } while (0)
    uint64_t input, output, raw, packed, map, capture_ix, info, capture_info, args, job;
    void *pinput, *poutput, *praw, *ppacked, *pmap, *pcapture_ix, *pinfo, *pcapture_info, *pargs, *pjob;
    GEO_ALLOC(input_size, input, pinput); GEO_ALLOC(output_size, output, poutput); GEO_ALLOC(raw_size, raw, praw); GEO_ALLOC(packed_size, packed, ppacked);
    GEO_ALLOC(words * 4, map, pmap); GEO_ALLOC(nu * 4, capture_ix, pcapture_ix); GEO_ALLOC(32, info, pinfo); GEO_ALLOC(32, capture_info, pcapture_info);
    GEO_ALLOC(20, args, pargs); GEO_ALLOC(40, job, pjob);
    #undef GEO_ALLOC
    (void)pinput; (void)poutput; (void)praw; (void)ppacked;
    if (used > total) MDIE("geometry scratch size accounting");
    nvn_geo_assemble(prim, order, count, restart, restart_index, pmap, words);
    for (size_t i = 0; i < words; i++) ((uint32_t *)pmap)[i] -= lo;
    static uint64_t *uq_bm; static size_t uq_words;   // submit thread only
    // 抓顶点按区间（11-16 能耗线：040074A01BF12000 帧回放，121 笔几何的去重结果与 [lo, hi] 逐笔相等，去重这段每笔约 1.5 万条指令）：没有重启、顶点阶段不写内存、
    //   跨度不超过下标数时，不排序去重，直接对 [lo + base_vertex, 跨度) 画点（同 tf_draw 的变换反馈抓取）。lo、hi 都是被引用的顶点，中间的顶点地址落在两者之间；
    //   多抓的顶点只写进没人读的记录槽（主遍下标只指被引用的），GPU 多算至多「跨度 − 去重数」≤ 下标数个顶点。非索引绘制本来就按 first / count 抓，不需要去重
    int by_range = !di0 || (!restart && span <= (uint64_t)count && !geo_vs_writes(vs));
    size_t unique_count = by_range ? 0 : nvn_geo_unique_sorted(restart ? unique : order, nu, lo, hi, (uint32_t *)pcapture_ix, &uq_bm, &uq_words);
    uint32_t vi[8] = {(uint32_t)span, (uint32_t)instances, 0, 0, (uint32_t)np, 0, 0, 0}; memcpy(pinfo, vi, 32);
    uint64_t ci[2] = {input, 0}; memcpy(pcapture_info, ci, 16);
    uint32_t cc[4] = {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}; memcpy((uint8_t *)pcapture_info + 16, cc, 16);
    memset(pargs, 0, 20);
    if (fast) { uint32_t a[5] = {(uint32_t)words, 1, 0, 0, 0}; memcpy(pargs, a, 20); }   // what the compaction would write: every input primitive, in order (k_nvn_geo_pack_msl args)
    struct { uint64_t src, dst, args; uint32_t groups, stride, topology, capacity; } pj = {raw, packed, args, (uint32_t)groups, gs->geo_index_stride, gs->geo_output, (uint32_t)(packed_size / 4)};
    memcpy(pjob, &pj, sizeof pj);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4RenderCommandEncoder> re;
    if (run) {
        if (!g_gr.active) geo_run_begin(); else if (!g_gr.cap) geo_run_cap();
        geo_runset_add(&g_gr.rs, h_gw, h_ngw, h_gk, h_fr, h_nfr, h_fk); geo_runset_add_fw(&g_gr.rs, h_fw, h_nfw, h_fwk);
        g_gr.rs.gml |= (h_gk && geo_gmem_writes(base)) | (h_fwk && geo_gmem_writes(pr->sh[1])) << 1;   // 按预测记的池表写（之后每笔 geo_runset_gml 补全）
        if (g_gml_on) { geo_runset_note(&g_gr.rs, h_cap, h_ncap); geo_runset_note(&g_gr.rs, h_gr, h_ngr); geo_runset_note(&g_gr.rs, h_gw, h_ngw); geo_runset_note(&g_gr.rs, h_fr, h_nfr); }
        g_gr.draws++; geo_lane(0); re = g_gr.cap;
    } else {
        MTL4RenderPassDescriptor *rd = pass_desc(); rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
        re = enc_r_begin(rd);
    }
    if (!run || g_gr.cap_pso != capture) { enc_r_pso(re, capture); if (run) g_gr.cap_pso = capture; }   // the capture lane keeps its pipeline across the run's draws
    p_draw vertex_range = {.first = (int32_t)first, .count = (int32_t)span, .instances = instances, .base_instance = (int32_t)first_instance};
    bind_vbufs(re, vs, &vertex_range, NULL);
    vconv_bind(re, vs, 1, &vertex_range, NULL);
    args_dirty_all(); encode_args(re, vs, 0, 1); enc_r_vbuf(re, 13, capture_info);
    if (di0 && !by_range) enc_r_draw_indexed(re, MTLPrimitiveTypePoint, unique_count, MTLIndexTypeUInt32, capture_ix, nu * 4, instances, di.base_vertex, first_instance);
    else if (di0) enc_r_draw_instanced(re, MTLPrimitiveTypePoint, (NSUInteger)first, (NSUInteger)span, instances, first_instance);
    else enc_r_draw_instanced(re, MTLPrimitiveTypePoint, dw.first, dw.count, instances, first_instance);
    if (!run) enc_r_end(re);
    g_geo_bind[0] = input; g_geo_bind[1] = output; g_geo_bind[2] = raw; g_geo_bind[3] = info; g_geo_bind[4] = map;
    g_geo_size[0] = input_size; g_geo_size[1] = output_size; g_geo_size[2] = raw_size; g_geo_size[3] = 32; g_geo_size[4] = words * 4;
    id<MTL4ComputeCommandEncoder> ce;
    if (run && !g_gr.active) MDIE("geometry run ended inside its draw's capture (an encoder or commit from encode_args)");
    if (run) {
        geo_lane(1); ce = g_gr.gs; mesh_run_drain();   // the run's pending mesh host stages first, in order
        geo_rg rd[32], wr[32]; int nrd, nwr, known = geo_gs_ranges(gs, rd, &nrd, wr, &nwr, 32);
        if (!known || geo_hz_conflict(&g_gr.hz, rd, nrd, wr, nwr)) { if (g_gr.kernels) { enc_c_inner_barrier(ce); g_gr.barriers++; } memset(&g_gr.hz, 0, sizeof g_gr.hz); }
        if (!known) g_gr.hz.all = 1; else geo_hz_add(&g_gr.hz, rd, nrd, wr, nwr);
        g_gr.kernels++;
    } else ce = enc_c_begin(NULL);
    args_dirty_all(); encode_args(ce, gs, 2, 0); enc_c_pso(ce, gs->geo_pso);
    enc_c_dispatch(ce, MTLSizeMake((np + 31) / 32, instances, gs->geo_invocations), MTLSizeMake(32, 1, 1));
    // S2Arm 11-05 能耗线（金刚：一帧 790 笔几何绘制，每笔 4 个编码器）：索引压缩读几何核写的记录，同一个计算编码器里隔一道编码器内屏障接着做，
    //   不再另开一个编码器（少一次编码器切换与入口全量屏障）。参数表在每次派发编码时取当时内容，几何核的绑定不受后面改槽 0 影响（原先两个编码器也共用 g_ct）
    if (fast) { if (run) geo_lane(0); else enc_c_end(ce); g_geo_pt_draws++; }
    else if (run) {   // the compaction waits for the run's end (geo_run_flush), after every geometry kernel
        if (g_gr.njobs == g_gr.cjobs) { g_gr.cjobs = g_gr.cjobs ? 2 * g_gr.cjobs : 256; g_gr.jobs = realloc(g_gr.jobs, g_gr.cjobs * sizeof *g_gr.jobs); if (!g_gr.jobs) MDIE("geometry run: out of memory"); }
        g_gr.jobs[g_gr.njobs++] = job; g_gr.pack = pack; geo_lane(0);
    } else {
        enc_c_inner_barrier(ce);
        enc_c_pso(ce, pack); enc_c_args(ce, job, 0);   // one threadgroup, as many threads as the pipeline takes up to 1024 (whole SIMD groups): see k_nvn_geo_pack_msl
        enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(MIN(pack.maxTotalThreadsPerThreadgroup, 1024) / 32 * 32, 1, 1)); enc_c_end(ce);
    }
    if (tf && !geo_tf(gs->geo_tf_stride[0] || gs->geo_tf_stride[1] || gs->geo_tf_stride[2] || gs->geo_tf_stride[3] ? gs : base, output, packed, args, packed_size / 4)) nvn_be_bad_draw_feat();   // the capture (see geo_tf)
    args_dirty_all(); if (!run) { g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0; }   // in a run the main pass's encoder state is unchanged (the lane encoders are others)
    swrite_note(pr); udc_note_draw(pr); g_wr_epoch++; g_mdx_epoch++;
    if (S.rast_discard) return;
    NvProg *raster = pr->geo_raster;
    if (!raster || raster->sh[0] != reader || raster->sh[1] != pr->sh[1]) {
        raster = [NvProg new]; raster->sh[0] = reader; raster->sh[1] = pr->sh[1]; raster->gp = pr->gp; pr->geo_raster = raster;   // gp: the fragment stage's guest program (its sampler-emulation variant, se_want)
    }
    g_geo_program = raster; g_geo_indices = fast ? map : packed; g_geo_index_bytes = fast ? words * 4 : packed_size; g_geo_args = args;   // fast: the input primitives' capture indices (map) are the main pass's
    p_draw synthetic = {.prim = gs->geo_output == 1 ? 0 : gs->geo_output == 6 ? 1 : 4, .count = 3, .instances = 1, .base_instance = (int32_t)first_instance};
    if (run) { geo_lane(2); if (!g_enc) { g_game_barrier = 1; main_barrier_reset(); } }   // the main pass opened in lane 2 waits for everything before it: the compactions in lane 1
    draw(&synthetic, NULL);
    if (run) { geo_lane(0); if (!g_gr.active) MDIE("geometry run ended inside its draw's main-pass draw"); }
    g_geo_program = nil; args_dirty_all();
    static int once; if (!once++) MLOG("geometry compute: vertex %s, geometry %s, %llu input primitives, %d instances", vs->key, gs->key, (unsigned long long)np, instances);
    if (fast && (g_geo_pt_draws == 1 || g_geo_pt_draws % 20000 == 0)) MLOG("geometry passthrough fast path: %llu draws (kernel %s, main pass reads the vertex capture, no records, no compaction)", (unsigned long long)g_geo_pt_draws, gs->key);
}

// ---------- Single-draw vertex capture (diagnostic DM_NVN_VCAP=<in-frame draw index>, frame replay: graphics/nvn/docs/frame_replay.md) ----------
// Before that draw runs, its vertex shader's capture entry point (main0_gs, the one geo_draw uses) runs once over the draw's vertex range with the same vertex buffers, arguments and
// instances, and the replay waits for it (mdi_wait): <S2A_DOCS or cwd>/vcap_f<frame>_d<n>.tsv gets a row per vertex (vertex, instance, clip x y z w, x/w, y/w). The capture entry has no
// edge-push tail (dmsh_build.py vs_push), so these are the positions before the widen push; draw() appends the draw's push parameters (g_wd_push) to the same file.
// Not handled (a red line, the draw itself unchanged): indirect draws, a vertex shader without the capture entry
static char g_vcap_path[1200]; static int g_vcap_hit;
static void vcap_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di, uint64_t frame, long n) {
    NvShader *vs = pr->sh[0];
    if (!vs || !vs->geo_fn || (dw ? dw->ind : di->ind)) { MLOG("【红】 DM_NVN_VCAP draw %ld: %s, not captured", n, !vs ? "no vertex shader" : !vs->geo_fn ? "the vertex shader has no capture entry point" : "an indirect draw"); return; }
    id<MTLRenderPipelineState> capture = geo_capture_pso(vs); if (!capture) return;
    mdi_wait();   // index / vertex data the GPU may still be writing, and a clean point to encode from
    int instances = dw ? dw->instances : di->instances; if (instances <= 0) instances = 1;
    uint32_t first_instance = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    int64_t first; uint64_t span;
    if (dw) { first = dw->first; span = dw->count > 0 ? (uint64_t)dw->count : 0; }
    else {   // the index range the draw uses, read on the CPU (as geo_draw)
        int isz = 1 << di->itype; uint32_t lo = UINT32_MAX, hi = 0;
        const uint8_t *hib = NULL;
        if (di->itype < 0 || di->itype > 2 || di->count <= 0 || !(hib = nvn_host_ptr(di->ibuf, (uint64_t)di->count * isz))) { MLOG("【红】 DM_NVN_VCAP draw %ld: index buffer not readable", n); return; }
        for (int i = 0; i < di->count; i++) { uint32_t v = 0; memcpy(&v, hib + (size_t)i * isz, isz); if (S.restart.on && v == (S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX))) continue; lo = MIN(lo, v); hi = MAX(hi, v); }
        if (lo > hi) return;
        first = (int64_t)lo + di->base_vertex; span = (uint64_t)hi - lo + 1;
    }
    if (!span || first < 0 || span * (uint64_t)instances > (64ull << 20) / (144 * 4)) { MLOG("【红】 DM_NVN_VCAP draw %ld: vertex range %lld+%llu × %d out of bounds", n, (long long)first, (unsigned long long)span, instances); return; }
    id<MTLBuffer> out = [g_dev newBufferWithLength:span * instances * 144 * 4 options:MTLResourceStorageModeShared]; res_add(out);
    id<MTLBuffer> ib; size_t io = arena_alloc(32, &ib); uint8_t *ip = (uint8_t *)ib.contents + io;
    uint64_t ci[2] = {out.gpuAddress, 0}; uint32_t cc[4] = {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}; memcpy(ip, ci, 16); memcpy(ip + 16, cc, 16);
    lc_land(); rp_flush(); rp_end(); ubd_land();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = pass_desc(); rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    p_draw range = {.first = (int32_t)first, .count = (int32_t)span, .instances = instances, .base_instance = (int32_t)first_instance};
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd); enc_r_pso(re, capture); bind_vbufs(re, vs, &range, NULL);
    vconv_bind(re, vs, 1, &range, NULL);
    args_dirty_all(); encode_args(re, vs, 0, 1); enc_r_vbuf(re, 13, ib.gpuAddress + io);
    enc_r_draw_instanced(re, MTLPrimitiveTypePoint, (NSUInteger)first, (NSUInteger)span, (NSUInteger)instances, first_instance);
    enc_r_end(re);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    mdi_wait();
    const char *dir = getenv("S2A_DOCS"); snprintf(g_vcap_path, sizeof g_vcap_path, "%s/vcap_f%llu_d%ld.tsv", dir && *dir ? dir : ".", (unsigned long long)frame, n);
    FILE *f = fopen(g_vcap_path, "w"); if (!f) { MLOG("【红】 DM_NVN_VCAP: cannot write %s", g_vcap_path); return; }
    fprintf(f, "# vs %s fs %s, %s draw, vertices %lld..%lld, %d instances; clip-space position before the widen push\nvertex\tinstance\tx\ty\tz\tw\tx/w\ty/w\n", vs->key, pr->sh[1] ? pr->sh[1]->key : "-", dw ? "plain" : "indexed", (long long)first, (long long)(first + (int64_t)span - 1), instances);
    const float *v = out.contents; float mn = INFINITY, mx = -INFINITY;
    for (int in = 0; in < instances; in++) for (uint64_t k = 0; k < span; k++) { const float *p = v + ((uint64_t)in * span + k) * 144; float t = p[3] != 0 ? p[0] / p[3] : NAN;
        fprintf(f, "%lld\t%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n", (long long)(first + (int64_t)k), in, p[0], p[1], p[2], p[3], t, p[3] != 0 ? p[1] / p[3] : NAN); if (t == t) { mn = MIN(mn, t); mx = MAX(mx, t); } }
    fclose(f); g_vcap_hit = 1;
    MLOG("DM_NVN_VCAP draw %ld (frame %llu): %llu vertices × %d instances of vs %s, clip x/w %.4f .. %.4f → %s", n, (unsigned long long)frame, (unsigned long long)span, instances, vs->key, mn, mx, g_vcap_path);
}

// ---------- S2Arm vertex-stage side effects under rasterizer discard (10-27, Xenoblade 3) ----------
// A vertex shader that writes a storage buffer runs for its writes alone with the rasterizer discarded: Xenoblade 3 skins its characters so, one draw a model between
// SetRasterizerDiscard(1) / (0) with no transform feedback begun, and the character draws read the skinned vertices back. tf_draw dropped every discarded draw without
// feedback as harmless, so the skinned buffers were never written and Noah, the party and the enemies were invisible (10-27 xc3_1027101629_1; the Mac the same).
// Metal runs no vertex function with rasterization off unless it returns void: the shader's geometry capture entry point (main0_gs, dmsh_geometry.py capture) is that function,
// its stores of the vertex outputs going into a scratch buffer nothing reads, in an encoder of its own as geo_draw's capture pass. An indexed draw runs over the index range it
// uses (read on the CPU, as geo_draw does, after the GPU work that may write the indices). → 1 when the draw is handled here (run, or dropped with a red line), 0 when the
// vertex shader writes nothing and the caller skips the draw as before
static int sx_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0) {
    NvShader *vs = pr ? pr->sh[0] : nil;
    if (!vs || !shader_writes(vs)) return 0;
    static int said;
    if (!vs->geo_fn || pr->sh[2] || pr->sh[3] || pr->sh[4]) { if (!said++) GEO_RED("【红】 rasterizer discard: vertex shader %s writes storage but %s: not run", vs->key, vs->geo_fn ? "the pipeline has a geometry / tessellation stage" : "has no capture entry point"); else nvn_be_bad_draw_feat(); return 1; }
    p_draw dw = dw0 ? *dw0 : (p_draw){0}; p_draw_idx di = di0 ? *di0 : (p_draw_idx){0};
    if ((dw0 ? dw.ind : di.ind) || (dw0 ? dw.mdi_cnt : di.mdi_cnt)) { if (!said++) GEO_RED("【红】 rasterizer discard: indirect draw of vertex shader %s that writes storage: not run", vs->key); else nvn_be_bad_draw_feat(); return 1; }
    int count = dw0 ? dw.count : di.count, instances = dw0 ? dw.instances : di.instances;
    uint32_t first_instance = (uint32_t)(dw0 ? dw.base_instance : di.base_instance);
    if (count <= 0 || instances < 0) return 1;
    if (!instances) instances = 1;
    id<MTLRenderPipelineState> capture = geo_capture_pso(vs); if (!capture) { nvn_be_bad_draw_feat(); return 1; }
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    int64_t first = dw.first; uint64_t span = (uint64_t)count; uint32_t *uix = NULL; size_t nu = 0;
    if (di0) {   // the vertices the indices name, each once (sorted, as geo_draw's capture), drawn as 32-bit indices: NVN's 8-bit indices have no Metal type
        int isz = di.itype >= 0 && di.itype <= 2 ? 1 << di.itype : 0;
        const uint8_t *hib = isz ? nvn_host_ptr(di.ibuf, (uint64_t)count * isz) : NULL;
        if (!hib) { if (!said++) GEO_RED("【红】 rasterizer discard: index buffer of vertex shader %s not readable: not run", vs->key); else nvn_be_bad_draw_feat(); return 1; }
        mdi_wait(); if (g_mdi_ev.signaledValue < g_mdi_v) return 1;
        uint32_t restart_index = S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX), lo = UINT32_MAX, hi = 0;
        if (!(uix = malloc((size_t)count * 4))) { GEO_RED("【红】 rasterizer discard: index allocation failed"); return 1; }
        for (int i = 0; i < count; i++) { uint32_t v = 0; memcpy(&v, hib + (size_t)i * isz, isz); if (S.restart.on && v == restart_index) continue; lo = MIN(lo, v); hi = MAX(hi, v); uix[nu++] = v; }
        if (lo > hi) { free(uix); return 1; }
        static uint64_t *uq_bm; static size_t uq_words; nu = nvn_geo_unique_sorted(uix, nu, lo, hi, uix, &uq_bm, &uq_words);   // submit thread only
        first = (int64_t)lo + di.base_vertex; span = (uint64_t)hi - lo + 1;
    }
    uint64_t bytes = span * (uint64_t)instances * 144 * 4;   // main0_gs stores 144 words a vertex (dmsh_geometry.py capture)
    if (first < 0 || first + span > (uint64_t)UINT32_MAX + 1 || bytes > (256ull << 20)) { free(uix); if (!said++) GEO_RED("【红】 rasterizer discard: vertex shader %s over %llu vertices × %d instances exceeds the scratch bound: not run", vs->key, (unsigned long long)span, instances); else nvn_be_bad_draw_feat(); return 1; }
    id<MTLBuffer> scratch; size_t off = arena_alloc((size_t)(bytes + nu * 4 + 512), &scratch);
    uint8_t *cpu = (uint8_t *)scratch.contents + off; uint64_t gpu = scratch.gpuAddress + off, ix = (gpu + 255) & ~255ull, info = (ix + nu * 4 + 255) & ~255ull, dst = info + 256;
    if (nu) { memcpy(cpu + (ix - gpu), uix, nu * 4); } free(uix);
    uint64_t ci[2] = {dst, 0}; uint32_t cc[4] = {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances};
    memcpy(cpu + (info - gpu), ci, 16); memcpy(cpu + (info - gpu) + 16, cc, 16);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = pass_desc(); rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    p_draw vertex_range = {.first = (int32_t)first, .count = (int32_t)span, .instances = instances, .base_instance = (int32_t)first_instance};
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd); enc_r_pso(re, capture); bind_vbufs(re, vs, &vertex_range, NULL);
    g_draw_binst = first_instance;
    vconv_bind(re, vs, 1, &vertex_range, NULL);
    wd_drop(); args_dirty_all(); encode_args(re, vs, 0, 1); enc_r_vbuf(re, 13, info);   // 宽屏不改写只产出数据的绘制（widen_glue.inc wd_drop）
    if (di0) enc_r_draw_indexed(re, MTLPrimitiveTypePoint, nu, MTLIndexTypeUInt32, ix, nu * 4, instances, di.base_vertex, first_instance);
    else enc_r_draw_instanced(re, MTLPrimitiveTypePoint, dw.first, count, instances, first_instance);
    enc_r_end(re);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    swrite_note(pr); udc_note_draw(pr); g_wr_epoch++; g_mdx_epoch++;
    static int once; if (!once++) MLOG("rasterizer discard: vertex shader %s that writes storage run for its writes (%s, %llu vertices, %d instances)", vs->key, di0 ? "indexed" : "array", (unsigned long long)span, instances);
    return 1;
}
