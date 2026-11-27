// NVN2 任务 / 网格绘制（11-05，时之笛 0400486024DEC000 进 3D 场景用 DrawMeshTasks / DrawMeshTasksIndirect）：直接包含生产 nvn_mtl.m，经 draw()（OP_DRAW 图元 DM_PRIM_MESH_TASKS）
//   走产品的网格绘制路径画到离屏目标、读回像素。着色器是 mk 用 xcrun metal 编好的真库（t_nvn_mesh.metal，形状照「网格着色接口约定」：入口 main0 / main0_z、起始工作组号在 buffer(29)），
//   反射 json 由本测按约定字段写进库目录，经产品的 shader_get → shader_build 载入。片元着色器写白。
// 期望按约定与夹具几何手算，不由被测实现推出（目标 8×4 RGBA8，先清黑；网格工作组画第 c 列整列，列掩码 bit c = 该列 4 个像素全白）：
//   ① 载入：任务 1111… 是 object（线程 32、载荷 4 → 管线按 16 对齐）；网格 2222… 是 mesh 且带任务、3333… 不带；开 near = 0 后网格载入的入口是 main0_z
//   ② 网格上限：max_vertices 300（> Metal 256）的网格着色器载不进来（nil），报红 1 行点名该键
//   ③ 只绑任务 / 网格 / 片元（BindProgram 位 0x40|0x80|0x02 录成 0x1a，hle_nvn.c）的程序不要求顶点阶段：bind_stages 回这个程序本身（改前回 NULL = 「没有程序」，整笔丢）
//   ④ 无任务 DrawMeshTasks(first 2, count 3)：首笔管线在后台建、本笔跳过并计数（等待数 +1、不报红）；建好后再画 → 列 2、3、4 白 = 0x1c
//   ⑤ 有任务 DrawMeshTasks(first 1, count 2)：任务 t = 1、2 各派 2 个网格组，列 2t、2t+1 → 列 2..5 = 0x3c
//   ⑥ 间接：参数位置 CPU 先写 {7, 0}（录制 / 回放若在 CPU 上读会画列 0..6），同一提交里 GPU 拷贝写成 {3, 4} 后再画 → 列 4、5、6 = 0x70；交完后该处 CPU 读到 {3, 4}
//   ⑦ 间接零工作量：GPU 写 {0, 1} → 一列都不画（0x00）、不报红、照样记一笔间接绘制
//   ⑧ 程序没有网格着色器：跳过、报红 1 行（不当普通绘制画）
//   ⑨ 着色器映射表的阶段名（翻译侧 shader_map.tsv 第 3 列写 task / mesh）经 prog_get 的 stage_of 落到宿主阶段 3 / 4（nvn2_stage_host：NVN 阶段 6 / 7 放曲面两阶段的位置），
//      shader_get 按该阶段载入的是 object / mesh 着色器（11-06 时之笛日志每局 20–30 行「shader … (task|mesh) failed to load」：stage_of 不认这两个名字回 -1，库里明明有）
//   ⑫ #1208 GPU 挂死隔离收网格管线：第一次画出进本提交嫌疑；隔离后同一对跳过、报红、写进隔离表
//   ⑩ GPU 没有对象 / 网格管线（g_mesh_ok = 0，MTLGPUFamilyMetal3 以下）：跳过并计数、报红 1 行、不画（不当普通绘制画）
// 校验层（mk 另跑 MTL_DEBUG_LAYER=1 两套后端）：全部项照过、不中止。负对照（11-11 手工）：改前 pso_desc 给网格管线设 vertexFunction = 网格函数，校验层两套后端都 abort rc=134（setVertexFunction 断言）
// 负对照（11-05 手工做，见坑记录）：把 mtk_encode 写进 29 号槽的起始号换成 0 → ④ 0x07、⑤ 0x0f、⑥ 0x07，三项红
#include "../gfx/nvn/nvn_mtl.m"

static unsigned failures, checks;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL setup %s:%d: %s\n", __FILE__, __LINE__, #c); exit(2); } } while (0)
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) failures++; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

static NSString *capture(void (^f)(void)) {   // 这一段打到 stderr 的全部行
    char path[] = "/tmp/t_nvn_mesh_XXXXXX"; int fd = mkstemp(path); REQUIRE(fd >= 0);
    fflush(stderr); int saved = dup(2); REQUIRE(saved >= 0); dup2(fd, 2); close(fd);
    f();
    fflush(stderr); dup2(saved, 2); close(saved);
    NSString *out = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil] ?: @"";
    unlink(path); return out;
}
static int red_lines(NSString *text, NSString *needle) {
    int n = 0; for (NSString *l in [text componentsSeparatedByString:@"\n"]) n += [l containsString:@"【红】"] && (!needle || [l containsString:needle]); return n;
}
static void put_json(NSString *dir, NSString *key, NSDictionary *extra) {
    NSMutableDictionary *j = [@{@"route": @"B", @"bindings": @[], @"chain": @DMSH_CHAIN, @"entry": @"main0"} mutableCopy]; [j addEntriesFromDictionary:extra];
    NSData *d = [NSJSONSerialization dataWithJSONObject:j options:0 error:nil]; REQUIRE(d);
    NSString *path = [NSString stringWithFormat:@"%@/%@.json", dir, key]; REQUIRE([d writeToFile:path atomically:YES]);
}
static tex_t g_rt;
static void rt_make(void) {
    memset(&g_rt, 0, sizeof g_rt); g_rt.magic = MAGIC('T', 'E', 'X', 0); g_rt.serial = nvn_new_serial();
    g_rt.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 8, .height = 4, .depth = 1, .levels = 1};
    nvn_tex_layout(&g_rt.d, &g_rt.L); nvn_be_tex_init(&g_rt); REQUIRE(tex_mtl(&g_rt));
}
static uint64_t g_frame = 1;
static void frame_begin(void) {   // 新的一帧：目标设成 g_rt 并清黑（清除在渲染遍开时落下）
    nvn_be_set_frame(++g_frame); nvn_be_sub_begin(); state_reset();
    const struct { op_hdr set; p_set_rt_c rt; const tex_t *color0; uint8_t snap[sizeof(tex_t)]; op_hdr clear; p_clear_color color; op_hdr end; } c = {
        .set = {OP_SET_RT, sizeof(op_hdr) + sizeof(p_set_rt_c) + 8 + sizeof(tex_t)}, .rt = {.n = 1, .cmask = 1}, .color0 = &g_rt,
        .clear = {OP_CLEAR_COLOR, sizeof(op_hdr) + sizeof(p_clear_color)}, .color = {.index = 0, .mask = 15, .rgba = {0, 0, 0, 1}}, .end = {OP_END, sizeof(op_hdr)} };
    nvn_be_exec((const uint8_t *)&c);
}
// 交掉这一帧、读回目标：列掩码（bit c = 第 c 列 4 个像素全白）；有半白的列或杂色回 -1
static int frame_columns(void) {
    rp_flush(); uint64_t fence = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(fence, 10000000000ull)); REQUIRE(!mtl_last_commit_error());
    id<MTLBuffer> rb = [g_dev newBufferWithLength:8 * 4 * 4 options:MTLResourceStorageModeShared]; REQUIRE(rb);
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:tex_mtl(&g_rt) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 4, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:128];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; REQUIRE(cb.status == MTLCommandBufferStatusCompleted);
    const uint8_t *px = rb.contents; int mask = 0;
    for (int c = 0; c < 8; c++) { int white = 0;
        for (int r = 0; r < 4; r++) { const uint8_t *p = px + 32 * r + 4 * c; if (p[0] == 255 && p[1] == 255 && p[2] == 255) white++; else if (p[0] || p[1] || p[2]) return -1; }
        if (white == 4) mask |= 1 << c; else if (white) return -1; }
    return mask;
}
// 一笔网格绘制；管线还在后台建（本笔跳过、等待数 +1）就等建好再画一次。*waited = 本笔是否先跳过过
static void mesh_draw(const p_draw *dw, int *waited) {
    uint64_t w0 = g_mtk_wait; draw(dw, NULL);
    *waited = g_mtk_wait != w0;
    if (*waited) { dispatch_group_wait(g_mtk_group, DISPATCH_TIME_FOREVER); draw(dw, NULL); }
}

int main(void) {
    @autoreleasepool {
        const char *libdir = getenv("T_MESH_LIB"); REQUIRE(libdir);   // mk 编好的 <键>_b.metallib 所在目录；json 由本测写
        setenv("DM_PRESENT_LINK", "0", 1);
        // 本测自己的目录：pso_user.tsv 旁边还有 GPU 挂死隔离表 gpu_quarantine.tsv（⑫ 写它；放共用 /tmp 会被下一局 / 别的单测读到）
        char pdir[] = "/tmp/t_nvn_mesh_XXXXXX"; REQUIRE(mkdtemp(pdir)); char pso_user[sizeof pdir + 16]; snprintf(pso_user, sizeof pso_user, "%s/pso_user.tsv", pdir);
        { int pfd = open(pso_user, O_CREAT | O_WRONLY, 0644); REQUIRE(pfd >= 0); close(pfd); } setenv("DM_PSO_USER", pso_user, 1);   // ④c：本测记下的网格管线行
        mtl_init(); g_msl_dir = @(libdir);
        printf("后端 Metal %d，录制 %s\n", g_m3 ? 3 : 4, rec_on() ? "开" : "关");
        NSString *dir = @(libdir);
        put_json(dir, @"1111111111111111_b", @{@"stage": @"ts", @"msl_entry_kind": @"object", @"mesh_threads": @32, @"mesh_payload": @4});
        put_json(dir, @"2222222222222222_b", @{@"stage": @"ms", @"msl_entry_kind": @"mesh", @"entry_z": @"main0_z", @"mesh_threads": @32, @"mesh_payload": @4, @"mesh_task": @1, @"max_vertices": @4, @"max_primitives": @2});
        put_json(dir, @"3333333333333333_b", @{@"stage": @"ms", @"msl_entry_kind": @"mesh", @"entry_z": @"main0_z", @"mesh_threads": @32, @"max_vertices": @4, @"max_primitives": @2});
        put_json(dir, @"4444444444444444_b", @{@"stage": @"ms", @"msl_entry_kind": @"mesh", @"entry_z": @"main0_z", @"mesh_threads": @32, @"max_vertices": @300, @"max_primitives": @2});
        nvn_be_set_depth_zero(1);

        // ①
        NvShader *ts = shader_get(@"1111111111111111_b", 3), *mt = shader_get(@"2222222222222222_b", 4), *md = shader_get(@"3333333333333333_b", 4);
        VERIFY(ts && ts->mesh_kind == 1 && ts->mesh_threads == 32 && ts->mesh_payload == 4, "① 任务着色器载入：object、线程 32、载荷 4（实得 %d / %u / %u）", ts ? ts->mesh_kind : -1, ts ? ts->mesh_threads : 0, ts ? ts->mesh_payload : 0);
        VERIFY(mt && mt->mesh_kind == 2 && mt->mesh_task && md && md->mesh_kind == 2 && !md->mesh_task, "① 网格着色器载入：2222 带任务、3333 不带");
        VERIFY(mt && md && [mt->ventry isEqual:@"main0_z"] && [md->ventry isEqual:@"main0_z"], "① near = 0 下网格入口是 main0_z（实得 %s / %s）", mt ? mt->ventry.UTF8String : "-", md ? md->ventry.UTF8String : "-");
        REQUIRE(ts && mt && md);
        // ②
        __block NvShader *big = nil; NSString *o = capture(^{ big = shader_get(@"4444444444444444_b", 4); });
        VERIFY(!big && red_lines(o, @"4444444444444444_b") == 1, "② max_vertices 300 的网格着色器载不进、报红 1 行（实得 %s，红 %d 行；stderr：%s）", big ? "载入了" : "nil", red_lines(o, @"4444444444444444_b"), o.UTF8String);

        // ⑪ #418 Switch 1 曲面细分着色器在阶段 3 / 4：json 带 tess、msl_entry_kind 是 vertex / kernel。期望（按约定手写）：不按 task / mesh 契约判——载入成功、不是网格种类、
        //   不打入口类型 / not usable 红行。反证：修前同一份 json 打入口类型红行，再因没有工作组线程数判「not usable」返回 nil
        { NSString *src = [NSString stringWithFormat:@"%@/1111111111111111_b.metallib", dir], *dst = [NSString stringWithFormat:@"%@/5555555555555555_b.metallib", dir];
          [NSFileManager.defaultManager removeItemAtPath:dst error:nil]; REQUIRE([NSFileManager.defaultManager copyItemAtPath:src toPath:dst error:nil]);   // 库内容无关：只验载入时的契约判定
          put_json(dir, @"5555555555555555_b", @{@"stage": @"hs", @"msl_entry_kind": @"vertex", @"tess": @{@"mode": @0}});
          __block NvShader *hs = nil; NSString *o11 = capture(^{ hs = shader_get(@"5555555555555555_b", 3); });
          VERIFY(hs && hs->mesh_kind == 0 && red_lines(o11, @"5555555555555555_b") == 0, "⑪ 曲面细分着色器（阶段 3，带 tess、入口类型 vertex）：载入、非网格种类、0 行红（实得 %s，种类 %d，红 %d 行；stderr：%s）",
                 hs ? "载入了" : "nil", hs ? hs->mesh_kind : -1, red_lines(o11, @"5555555555555555_b"), o11.UTF8String); }

        NvShader *fs = [NvShader new]; fs->stage = 1; strlcpy(fs->key, "ffffffffffffffff_b", sizeof fs->key); fs->ventry = @"main0";
        { NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(1); }\n" options:nil error:&e]; REQUIRE(l); fs->fn = [l newFunctionWithName:@"main0"]; REQUIRE(fs->fn); }
        NvProg *direct = [NvProg new]; direct->sh[1] = fs; direct->sh[4] = md;
        NvProg *task = [NvProg new]; task->sh[1] = fs; task->sh[3] = ts; task->sh[4] = mt;
        NvProg *none = [NvProg new]; none->sh[1] = fs;
        static prog_t gp[3]; NvProg *progs[3] = {direct, task, none};
        for (int i = 0; i < 3; i++) { memset(&gp[i], 0, sizeof gp[i]); gp[i].magic = MAGIC('P', 'R', 'O', 'G'); gp[i].serial = nvn_new_serial(); serial_put(gp[i].serial, progs[i]); }
        rt_make();

        // ③
        frame_begin();
        const prog_t *bound = bind_stages(&gp[1], 0x1a);
        VERIFY(bound == &gp[1], "③ 任务 + 网格 + 片元（位 0x1a）、没有顶点阶段的程序：bind_stages 回它本身（实得 %p，期望 %p）", (const void *)bound, (const void *)&gp[1]);

        // ④
        S.prog = &gp[0]; uint64_t n0 = g_mtk_n; __block int waited = 0;
        p_draw d4 = {.prim = DM_PRIM_MESH_TASKS, .first = 2, .count = 3, .instances = 1};
        o = capture(^{ mesh_draw(&d4, &waited); });
        int cols = frame_columns();
        // 11-13 0400486024DEC000 开局发白：网格管线没编好时一律跳过，不按 skip_safe 的规则。期望（按规则手写）：这是目标第一次被画（近两帧没画过），这笔不能跳过 →
        //   在每帧预算（缺省 250 ms）内等后台那条编好、本笔照画，日志写明原因与「ready」；反证：修前同一处先跳过（waited = 1）、不打这一行
        VERIFY(!waited && [o containsString:@"waited for its background pipeline"] && [o containsString:@"(ready)"] && [o containsString:@"a target not drawn in the last two frames"] && red_lines(o, nil) == 0,
               "④ 首笔管线、目标近两帧没画过：不跳过，等后台编好照画、写明原因、不报红（跳过过 %d；stderr：%s）", waited, o.UTF8String);
        VERIFY(cols == 0x1c && g_mtk_n == n0 + 1, "④ 无任务 first 2 count 3：列 2、3、4 白（列掩码 %#x，期望 0x1c；画出 +%llu）", cols, (unsigned long long)(g_mtk_n - n0));

        // ⑤
        frame_begin(); S.prog = &gp[1];
        p_draw d5 = {.prim = DM_PRIM_MESH_TASKS, .first = 1, .count = 2, .instances = 1};
        o = capture(^{ int w; mesh_draw(&d5, &w); });
        cols = frame_columns();
        VERIFY(cols == 0x3c && red_lines(o, nil) == 0, "⑤ 有任务 first 1 count 2：任务 1、2 各派 2 组 → 列 2..5 白（列掩码 %#x，期望 0x3c；stderr：%s）", cols, o.UTF8String);

        // ⑥ ⑦：参数在内存池里，GPU 在同一提交里写
        static uint32_t pool[4096] __attribute__((aligned(16384)));
        nvn_be_pool_add(pool, sizeof pool);
        pool[0] = 3; pool[1] = 4; pool[4] = 0; pool[5] = 1;   // 种子（GPU 拷贝的源）
        pool[64] = 7; pool[65] = 0; pool[68] = 7; pool[69] = 0;   // 参数位置：CPU 看到的旧值
        frame_begin(); S.prog = &gp[0]; uint64_t i0 = g_mtk_ind_n;
        mtl_sub_copy((uint64_t)(uintptr_t)&pool[0], (uint64_t)(uintptr_t)&pool[64], 8);
        p_draw d6 = {.prim = DM_PRIM_MESH_TASKS, .instances = 1, .ind = (uint64_t)(uintptr_t)&pool[64]};
        o = capture(^{ int w; mesh_draw(&d6, &w); });
        cols = frame_columns();
        VERIFY(cols == 0x70 && g_mtk_ind_n == i0 + 1 && red_lines(o, nil) == 0, "⑥ 间接：GPU 写 {3, 4}（CPU 先见 {7, 0}）→ 列 4、5、6 白（列掩码 %#x，期望 0x70；间接 +%llu；stderr：%s）",
               cols, (unsigned long long)(g_mtk_ind_n - i0), o.UTF8String);
        VERIFY(pool[64] == 3 && pool[65] == 4, "⑥ 参数确实是 GPU 写的：交完后 CPU 读到 {%u, %u}，期望 {3, 4}", pool[64], pool[65]);

        frame_begin(); S.prog = &gp[0]; i0 = g_mtk_ind_n;
        mtl_sub_copy((uint64_t)(uintptr_t)&pool[4], (uint64_t)(uintptr_t)&pool[68], 8);
        p_draw d7 = {.prim = DM_PRIM_MESH_TASKS, .instances = 1, .ind = (uint64_t)(uintptr_t)&pool[68]};
        o = capture(^{ int w; mesh_draw(&d7, &w); });
        cols = frame_columns();
        VERIFY(cols == 0 && g_mtk_ind_n == i0 + 1 && red_lines(o, nil) == 0, "⑦ 间接零工作量：GPU 写 {0, 1} → 一列不画（列掩码 %#x，期望 0）、照记一笔、不报红（stderr：%s）", cols, o.UTF8String);

        // ④b 目标已连续几帧画过（④–⑦）、这帧开头清过：丢一笔最多影响一帧，新管线照旧后台编、本笔先跳过。期望：跳过过、不打「built on the submit thread」、
        //   等编好再画出列 2、3、4。换一个片元着色器（新键）让管线是第一次遇到
        { NvShader *fs2 = [NvShader new]; fs2->stage = 1; strlcpy(fs2->key, "eeeeeeeeeeeeeeee_b", sizeof fs2->key); fs2->ventry = @"main0";
          NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(1, 1, 1, 0.5); }\n" options:nil error:&e]; REQUIRE(l); fs2->fn = [l newFunctionWithName:@"main0"]; REQUIRE(fs2->fn);
          NvProg *direct2 = [NvProg new]; direct2->sh[1] = fs2; direct2->sh[4] = md;
          static prog_t gp4b; memset(&gp4b, 0, sizeof gp4b); gp4b.magic = MAGIC('P', 'R', 'O', 'G'); gp4b.serial = nvn_new_serial(); serial_put(gp4b.serial, direct2);
          frame_begin(); S.prog = &gp4b; __block int w4b = 0;
          o = capture(^{ mesh_draw(&d4, &w4b); });
          cols = frame_columns();
          VERIFY(w4b && ![o containsString:@"built on the submit thread"] && red_lines(o, nil) == 0 && cols == 0x1c,
                 "④b 目标稳定且这帧清过：新网格管线后台编、本笔先跳过，编好后画出列 2、3、4（跳过过 %d，列掩码 %#x；stderr：%s）", w4b, cols, o.UTF8String); }

        // ④c 网格管线进本机记录（pso_user.tsv 的 M 行）→ 下一局预热按这行拼回绘制查表用的键、把管线交给表 → 首笔网格绘制直接画（11-13 开局发白根修②）。
        //   期望（按格式手写）：④ 当场编、⑤ 带任务、④b 后台编的三条各记一行 M（⑥ ⑦ 与 ④ 同一条），无任务的第 2 列是「-」、⑤ 那行是任务键 1111…，第 3 列补齐掩码 0；用 ④ 那行（片元 ffff…）解析出的键与
        //   绘制时的键逐字节相同（表里查得到 ④ 的管线）；把表里这条删掉（新开局）后经交接函数放回，同一笔绘制不跳过、不当场编、画出列 2、3、4
        { NSString *txt = [NSString stringWithContentsOfFile:@(pso_user) encoding:NSUTF8StringEncoding error:nil]; NSMutableArray<NSString *> *mrows = [NSMutableArray array];
          for (NSString *l in [txt componentsSeparatedByString:@"\n"]) if ([l hasPrefix:@"M\t"]) [mrows addObject:l];
          NSString *row4 = nil, *row5 = nil;
          for (NSString *l in mrows) { NSArray *c = [l componentsSeparatedByString:@"\t"]; pso_skey sk; if (c.count != 4 || pso_skey_parse([c[1] UTF8String], &sk) || strcmp(sk.fs, "ffffffffffffffff_b")) continue;
              if (!strcmp(sk.vs, "3333333333333333_b")) row4 = l; else if (!strcmp(sk.vs, "2222222222222222_b")) row5 = l; }
          VERIFY(mrows.count == 3 && row4 && [[row4 componentsSeparatedByString:@"\t"][2] isEqual:@"-"] && [[row4 componentsSeparatedByString:@"\t"][3] isEqual:@"0"] &&
                 row5 && [[row5 componentsSeparatedByString:@"\t"][2] isEqual:@"1111111111111111_b"],
                 "④c ④ / ⑤ / ④b 的网格管线各记一行 M（实得 %lu 行），无任务记「-」、⑤ 记任务键、补齐掩码 0（④ 那行第 2、3 列：%s）", (unsigned long)mrows.count,
                 row4 ? [[row4 componentsSeparatedByString:@"\t"] subarrayWithRange:NSMakeRange(2, 2)].description.UTF8String : "无");
          REQUIRE(row4);
          if (!g_shmap) g_shmap = [NSMutableDictionary dictionary];   // 本测没有映射表：把夹具的键登记成本款的（warm_stale 才不当别的游戏版本丢掉）
          for (NSString *k in @[@"1111111111111111", @"2222222222222222", @"3333333333333333", @"ffffffffffffffff", @"eeeeeeeeeeeeeeee"]) g_shmap[k] = @"x";
          long lines = 0; NSArray<WarmTask *> *tasks = warm_parse(@[@(pso_user)], &lines); WarmTask *t4 = nil;
          for (WarmTask *t in tasks) if (t->kind == 4 && [t->fkey isEqual:@"ffffffffffffffff_b"] && [t->vkey isEqual:@"3333333333333333_b"]) t4 = t;
          VERIFY(t4 && [t4->vkey isEqual:@"3333333333333333_b"] && !t4->tkey.length && t4->pad == 0, "④c 预热解析 M 行：网格键 3333…、片元键 ffff…、无任务、掩码 0（实得 %s / %s / %s / %#x）",
                 t4 ? t4->vkey.UTF8String : "-", t4 ? t4->fkey.UTF8String : "-", t4 ? t4->tkey.UTF8String : "-", t4 ? t4->pad : 0);
          REQUIRE(t4);
          // ④f D-75KF：同一对（④ 那行的 pso_skey、无任务）既有补齐行（掩码 20）又有不补齐行（掩码 0）→ 预热丢掉不补齐那行（它被拒过）；⑤ 那行（有任务键）没有补齐行、照留。
          //   期望（按规则手写）：kind 4 里 3333…/ffff… 无任务的只剩掩码 0x20 一条；⑤ 那条在
          { NSString *rest = [[row4 componentsSeparatedByString:@"\t"][1] copy], *pp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"t_nvn_mesh_pad_rows.tsv"];
            NSString *txt = [NSString stringWithFormat:@"%@\nM\t%@\t-\t20\n%@\n", row4, rest, row5];
            [txt writeToFile:pp atomically:YES encoding:NSUTF8StringEncoding error:nil];
            long ln = 0; NSArray<WarmTask *> *tp = warm_parse(@[pp], &ln); int unpadded = 0, padded = 0, with_task = 0;
            for (WarmTask *t in tp) if (t->kind == 4 && [t->fkey isEqual:@"ffffffffffffffff_b"]) {
                if ([t->vkey isEqual:@"3333333333333333_b"] && !t->tkey.length) { if (t->pad) padded += t->pad == 0x20; else unpadded++; }
                else if (t->tkey.length) with_task++; }
            VERIFY(unpadded == 0 && padded == 1 && with_task == 1 && ln == 2, "④f 同对有补齐行时预热丢掉不补齐那行（实得 不补齐 %d / 补齐 %d / 带任务 %d / 行数 %ld，期望 0 / 1 / 1 / 2）", unpadded, padded, with_task, ln);
            [NSFileManager.defaultManager removeItemAtPath:pp error:nil]; }
          pso_skey sk; [t4->rs[0] getBytes:&sk length:sizeof sk]; mtk_key mk; mtk_warm_key(&sk, NULL, md, fs, 0, &mk);
          os_unfair_lock_lock(&g_pso_mtk_lock); id had = hm_get_id(&g_pso_mtk, &mk); os_unfair_lock_unlock(&g_pso_mtk_lock);
          VERIFY(had && had != NSNull.null && had != g_pso_mtk_building, "④c 按 M 行拼回的键在网格管线表里查得到 ④ 编好的那条（%p）", (__bridge void *)had);
          REQUIRE(had);
          os_unfair_lock_lock(&g_pso_mtk_lock); hm_del_id(&g_pso_mtk, &mk); os_unfair_lock_unlock(&g_pso_mtk_lock);
          uint64_t wc0 = g_mtk_warm_claimed; mtk_warm_claim([NSData dataWithBytes:&mk length:sizeof mk], had);
          VERIFY(g_mtk_warm_claimed == wc0 + 1, "④c 交接：表里没有这条时，预热交来的管线放进表（交接 +%llu）", (unsigned long long)(g_mtk_warm_claimed - wc0));
          frame_begin(); S.prog = &gp[0]; uint64_t sn0 = g_mtk_waits; __block int w4c = 0;
          o = capture(^{ mesh_draw(&d4, &w4c); });
          cols = frame_columns();
          VERIFY(!w4c && g_mtk_waits == sn0 && cols == 0x1c && red_lines(o, nil) == 0,
                 "④c 预热交来的管线：同一笔不跳过、不用等、画出列 2、3、4（跳过过 %d，等待 +%llu，列掩码 %#x；stderr：%s）", w4c, (unsigned long long)(g_mtk_waits - sn0), cols, o.UTF8String); }
        unlink(pso_user);

        // ④d（D-84J2 起）丢不起的那笔等到管线编好，只受「编译服务卡死」封顶（DM_NVN_PSO_UNSAFE_WAIT_MS，这里设 300 ms）：编译被挂住时等满封顶才跳过、打红行并写明是封顶。
        //   新目标（近两帧没画过）+ 新片元（管线没编过）。④e：编译在等待中途放开 → 等到编好就画出，不计跳过、没有红行
        { tex_t rt2; memset(&rt2, 0, sizeof rt2); rt2.magic = MAGIC('T', 'E', 'X', 0); rt2.serial = nvn_new_serial(); rt2.d = g_rt.d; nvn_tex_layout(&rt2.d, &rt2.L); nvn_be_tex_init(&rt2); REQUIRE(tex_mtl(&rt2));
          NvShader *fs3 = [NvShader new]; fs3->stage = 1; strlcpy(fs3->key, "dddddddddddddddd_b", sizeof fs3->key); fs3->ventry = @"main0";
          NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(1, 1, 1, 0.25); }\n" options:nil error:&e]; REQUIRE(l); fs3->fn = [l newFunctionWithName:@"main0"]; REQUIRE(fs3->fn);
          NvProg *direct3 = [NvProg new]; direct3->sh[1] = fs3; direct3->sh[4] = md;
          static prog_t gp4d; memset(&gp4d, 0, sizeof gp4d); gp4d.magic = MAGIC('P', 'R', 'O', 'G'); gp4d.serial = nvn_new_serial(); serial_put(gp4d.serial, direct3);
          nvn_be_set_frame(++g_frame); nvn_be_sub_begin(); state_reset();
          const struct { op_hdr set; p_set_rt_c rt; const tex_t *color0; uint8_t snap[sizeof(tex_t)]; op_hdr end; } c = {
              .set = {OP_SET_RT, sizeof(op_hdr) + sizeof(p_set_rt_c) + 8 + sizeof(tex_t)}, .rt = {.n = 1, .cmask = 1}, .color0 = &rt2, .end = {OP_END, sizeof(op_hdr)} };
          nvn_be_exec((const uint8_t *)&c);
          // 后台编译先挂住（占满后台并发名额 g_mtk_sem）：这笔管线在检查时一定还没编好，期望不靠编译快慢。
          //   原先不挂：门禁机编得快（0.1 ms 编好），检查时已能画、没走「跳过」，超预算 +0 / 跳过 +0（main c3708ffdb 已知红）
          REQUIRE(g_mtk_sem); int held = mtk_width(); for (int i = 0; i < held; i++) dispatch_semaphore_wait(g_mtk_sem, DISPATCH_TIME_FOREVER);
          int b0 = g_mtk_budget, u0 = g_pso_unsafe_ms; g_mtk_budget = 0; g_pso_unsafe_ms = 300; S.prog = &gp4d; uint64_t ov0 = g_mtk_wait_over, w0 = g_mtk_wait, mx0 = g_mtk_wait_max;
          uint64_t t4d = bp_now(); o = capture(^{ draw(&d4, NULL); }); double ms4d = (bp_now() - t4d) / 1e6;
          g_mtk_budget = b0; g_pso_unsafe_ms = u0; for (int i = 0; i < held; i++) dispatch_semaphore_signal(g_mtk_sem);
          dispatch_group_wait(g_mtk_group, DISPATCH_TIME_FOREVER);
          rp_flush(); uint64_t fence = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(fence, 10000000000ull));
          (void)mx0;
          VERIFY(g_mtk_wait_over == ov0 + 1 && g_mtk_wait == w0 + 1 && ms4d >= 300 && [o containsString:@"stuck-compiler cap DM_NVN_PSO_UNSAFE_WAIT_MS 300 ms"] && [o containsString:@"not ready: skipped this frame"] && red_lines(o, nil) == 1,
                 "④d 编译挂住：丢不起的那笔等满封顶 300 ms 才跳过、计数、打一条红行写明封顶（等了 %.0f ms，超时 +%llu，跳过 +%llu；stderr：%s）", ms4d, (unsigned long long)(g_mtk_wait_over - ov0), (unsigned long long)(g_mtk_wait - w0), o.UTF8String);
          // 这笔从没画出来过，它的管线在请求编译时就进了本机记录（M 行，片元键 dddd…）。反证：只在第一次画出时记，这里就没有
          int rec4d = 0; pso_user_seen_load();
          for (NSString *l in g_pso_user_seen) { if (![l hasPrefix:@"M\t"]) continue; pso_skey sk; NSArray *c = [l componentsSeparatedByString:@"\t"];
              if (c.count >= 2 && !pso_skey_parse([c[1] UTF8String], &sk) && !strcmp(sk.fs, "dddddddddddddddd_b")) rec4d++; }
          VERIFY(rec4d == 1, "④d 没画出来过的网格管线在请求编译时就记了一行 M（实得 %d 行）", rec4d); }
        // ④e 同样丢不起的一笔（又一张新目标 + 又一个新片元），编译挂住约 100 ms 后放开：这笔等到管线编好就画出——不跳过、不计超时、没有红行，等待 ≥ 挂住的时长
        { tex_t rt3; memset(&rt3, 0, sizeof rt3); rt3.magic = MAGIC('T', 'E', 'X', 0); rt3.serial = nvn_new_serial(); rt3.d = g_rt.d; nvn_tex_layout(&rt3.d, &rt3.L); nvn_be_tex_init(&rt3); REQUIRE(tex_mtl(&rt3));
          NvShader *fs4 = [NvShader new]; fs4->stage = 1; strlcpy(fs4->key, "eeeeeeeeeeeeeeee_b", sizeof fs4->key); fs4->ventry = @"main0";
          NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(1, 1, 1, 0.5); }\n" options:nil error:&e]; REQUIRE(l); fs4->fn = [l newFunctionWithName:@"main0"]; REQUIRE(fs4->fn);
          NvProg *direct4 = [NvProg new]; direct4->sh[1] = fs4; direct4->sh[4] = md;
          static prog_t gp4e; memset(&gp4e, 0, sizeof gp4e); gp4e.magic = MAGIC('P', 'R', 'O', 'G'); gp4e.serial = nvn_new_serial(); serial_put(gp4e.serial, direct4);
          nvn_be_set_frame(++g_frame); nvn_be_sub_begin(); state_reset();
          const struct { op_hdr set; p_set_rt_c rt; const tex_t *color0; uint8_t snap[sizeof(tex_t)]; op_hdr end; } c = {
              .set = {OP_SET_RT, sizeof(op_hdr) + sizeof(p_set_rt_c) + 8 + sizeof(tex_t)}, .rt = {.n = 1, .cmask = 1}, .color0 = &rt3, .end = {OP_END, sizeof(op_hdr)} };
          nvn_be_exec((const uint8_t *)&c);
          int held = mtk_width(); for (int i = 0; i < held; i++) dispatch_semaphore_wait(g_mtk_sem, DISPATCH_TIME_FOREVER);
          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ for (int i = 0; i < held; i++) dispatch_semaphore_signal(g_mtk_sem); });
          int b0 = g_mtk_budget, u0 = g_pso_unsafe_ms; g_mtk_budget = 0; g_pso_unsafe_ms = 10000; S.prog = &gp4e; uint64_t ov0 = g_mtk_wait_over, w0 = g_mtk_wait, n0 = g_mtk_n;
          uint64_t t4e = bp_now(); NSString *o4 = capture(^{ draw(&d4, NULL); }); double ms4e = (bp_now() - t4e) / 1e6;
          g_mtk_budget = b0; g_pso_unsafe_ms = u0;
          dispatch_group_wait(g_mtk_group, DISPATCH_TIME_FOREVER);
          rp_flush(); uint64_t fence = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(fence, 10000000000ull));
          VERIFY(g_mtk_wait_over == ov0 && g_mtk_wait == w0 && g_mtk_n == n0 + 1 && ms4e >= 100 && ms4e < 10000 && red_lines(o4, nil) == 0,
                 "④e 编译 100 ms 后放开：丢不起的那笔等到编好就画出（等了 %.0f ms，画出 +%llu，跳过 +%llu，超时 +%llu；stderr：%s）", ms4e, (unsigned long long)(g_mtk_n - n0), (unsigned long long)(g_mtk_wait - w0), (unsigned long long)(g_mtk_wait_over - ov0), o4.UTF8String); }

        // ⑧
        frame_begin(); S.prog = &gp[2]; uint64_t s0 = g_mtk_skip;
        p_draw d8 = {.prim = DM_PRIM_MESH_TASKS, .first = 0, .count = 4, .instances = 1};
        o = capture(^{ draw(&d8, NULL); draw(&d8, NULL); });
        cols = frame_columns();
        VERIFY(cols == 0 && g_mtk_skip == s0 + 2 && red_lines(o, nil) == 1, "⑧ 程序没有网格着色器：两笔都跳过（+%llu）、报红 1 行、不画（列掩码 %#x；stderr：%s）", (unsigned long long)(g_mtk_skip - s0), cols, o.UTF8String);
        nvn_be_pool_del(pool);

        // ⑨
        int st_t = stage_of(@"task"), st_m = stage_of(@"mesh");
        NvShader *mt9 = st_t >= 0 ? shader_get(@"1111111111111111_b", st_t) : nil, *mm9 = st_m >= 0 ? shader_get(@"3333333333333333_b", st_m) : nil;
        VERIFY(st_t == 3 && st_m == 4 && mt9 && mt9->mesh_kind == 1 && mm9 && mm9->mesh_kind == 2,
               "⑨ 映射表阶段名 task / mesh → 宿主阶段 3 / 4 并载入 object / mesh（实得 %d / %d，种类 %d / %d）", st_t, st_m, mt9 ? mt9->mesh_kind : -1, mm9 ? mm9->mesh_kind : -1);

        // ⑩
        frame_begin(); S.prog = &gp[0]; s0 = g_mtk_skip; n0 = g_mtk_n; int ok_saved = g_mesh_ok; g_mesh_ok = 0;
        p_draw d10 = {.prim = DM_PRIM_MESH_TASKS, .first = 0, .count = 4, .instances = 1};
        o = capture(^{ draw(&d10, NULL); draw(&d10, NULL); });
        g_mesh_ok = ok_saved;
        cols = frame_columns();
        VERIFY(cols == 0 && g_mtk_skip == s0 + 2 && g_mtk_n == n0 && red_lines(o, @"no object / mesh pipelines") == 1,
               "⑩ GPU 无网格管线：两笔都跳过（+%llu）、报红 1 行、不画（列掩码 %#x；stderr：%s）", (unsigned long long)(g_mtk_skip - s0), cols, o.UTF8String);

        // ⑫ #1208 GPU 挂死隔离收网格管线：一条网格管线（网格 3333… + 新片元 cccc…）第一次画出 → 进本提交的嫌疑（quar_take 里有「3333333333333333\tcccccccccccccccc」，
        //   格式同普通管线的 quar_name：两键各前 16 字）；这批嫌疑报错隔离后，同一对的网格绘制跳过、不画、报红 1 行点名隔离，并写进隔离表（以后的局也跳过）。
        //   反证：改前 mtk_pso 不调 quar_built / quar_has —— 嫌疑里没有它（GPU 在这次提交挂死时什么都不隔离，下一局挂在同一处），隔离后照画列 0..3
        { NvShader *fs5 = [NvShader new]; fs5->stage = 1; strlcpy(fs5->key, "cccccccccccccccc_b", sizeof fs5->key); fs5->ventry = @"main0";
          NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(1); }\n" options:nil error:&e]; REQUIRE(l); fs5->fn = [l newFunctionWithName:@"main0"]; REQUIRE(fs5->fn);
          NvProg *direct5 = [NvProg new]; direct5->sh[1] = fs5; direct5->sh[4] = md;
          static prog_t gp12; memset(&gp12, 0, sizeof gp12); gp12.magic = MAGIC('P', 'R', 'O', 'G'); gp12.serial = nvn_new_serial(); serial_put(gp12.serial, direct5);
          NSString *pair = @"3333333333333333\tcccccccccccccccc";
          frame_begin(); (void)quar_take(); S.prog = &gp12;
          p_draw d12 = {.prim = DM_PRIM_MESH_TASKS, .first = 0, .count = 4, .instances = 1};
          __block int w12 = 0; o = capture(^{ mesh_draw(&d12, &w12); });
          NSArray<NSString *> *sus = quar_take();   // 提交前取：frame_columns 的提交会自己取走
          cols = frame_columns();
          VERIFY(cols == 0x0f && [sus containsObject:pair] && [o containsString:@"first drawn"],
                 "⑫ 网格管线第一次画出：画出列 0..3、进本提交的 GPU 出错嫌疑、打 first drawn（列掩码 %#x，嫌疑 %s；stderr：%s）", cols, [sus componentsJoinedByString:@" | "].UTF8String, o.UTF8String);
          quar_fault(sus, [NSError errorWithDomain:@"t" code:1 userInfo:nil]);
          frame_begin(); uint64_t q0 = g_mtk_quar_skip; S.prog = &gp12;
          o = capture(^{ draw(&d12, NULL); draw(&d12, NULL); });
          cols = frame_columns();
          NSString *qf = [NSString stringWithContentsOfFile:[@(pdir) stringByAppendingPathComponent:@"gpu_quarantine.tsv"] encoding:NSUTF8StringEncoding error:nil] ?: @"";
          VERIFY(quar_has(md, fs5) && cols == 0 && g_mtk_quar_skip == q0 + 2 && red_lines(o, @"quarantine") == 2 && [qf containsString:pair],
                 "⑫ 隔离后同一对网格绘制两笔都跳过（+%llu）、不画（列掩码 %#x）、每笔报红 1 行（全局前 4 笔）、写进隔离表（%s；stderr：%s）",
                 (unsigned long long)(g_mtk_quar_skip - q0), cols, [qf containsString:pair] ? "有" : "无", o.UTF8String);
          unlink([[@(pdir) stringByAppendingPathComponent:@"gpu_quarantine.tsv"] fileSystemRepresentation]); rmdir(pdir); }
        // ⑬ #1542 宿主自跑网格的间接组数上界（M1 / M2 上任务 / 网格由宿主计算核跑，本机 M5 走驱动，这里直接跑那两个夹紧核，单线程、安全）：
        //   任务：交换头 h[3] = capT 64，游戏间接组数 {100000, 100000, 1}（垃圾计数）→ 派发用的 h[20..22] 乘积 ≤ 64 且 x = 64（改前直接按 10^10 组派任务核 = 单个命令缓冲无界）；
        //   正常 {3, 1, 1} → {3, 1, 1} 不改；无任务的网格：h[4] = 容量 64，间接 {1000, 1000, 1} → 网格组 h[8] = 64、h[9] = h[10] = 1、索引数 h[13] = 0、实例数 h[14] = 1、
        //   原始组数 h[18] = 1000000（下一笔按它放大）、任务数 h[19] = 0
        { id<MTLComputePipelineState> tc = mxe_pso(MXE_TCLAMP), mo = mxe_pso(MXE_MONLY);
          id<MTLBuffer> hb = [g_dev newBufferWithLength:4 * MXE_HEAD options:MTLResourceStorageModeShared], ib = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
          uint32_t *h = (uint32_t *)hb.contents, *iv = (uint32_t *)ib.contents; id<MTLCommandQueue> q = [g_dev newCommandQueue];
          void (^run1)(id<MTLComputePipelineState>) = ^(id<MTLComputePipelineState> ps) {
              id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
              [e setComputePipelineState:ps]; [e setBuffer:hb offset:0 atIndex:0]; [e setBuffer:ib offset:0 atIndex:1];
              [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; };
          VERIFY(tc && mo, "⑬ 宿主自跑网格的两个夹紧核编得出");
          if (tc && mo) {
              memset(h, 0, 4 * MXE_HEAD); h[3] = 64; iv[0] = 100000; iv[1] = 100000; iv[2] = 1; run1(tc);
              uint64_t prod = (uint64_t)h[20] * h[21] * h[22];
              VERIFY(h[20] == 64 && prod <= 64, "⑬ 任务间接组数 {100000, 100000, 1}、capT 64 → 派发 {%u, %u, %u}（乘积 %llu ≤ 64、x = 64）", h[20], h[21], h[22], (unsigned long long)prod);
              memset(h, 0, 4 * MXE_HEAD); h[3] = 64; iv[0] = 3; iv[1] = 1; iv[2] = 1; run1(tc);
              VERIFY(h[20] == 3 && h[21] == 1 && h[22] == 1, "⑬ 任务间接组数 {3, 1, 1} 不改（实得 {%u, %u, %u}）", h[20], h[21], h[22]);
              memset(h, 0, 4 * MXE_HEAD); h[4] = 64; h[13] = 99; iv[0] = 1000; iv[1] = 1000; iv[2] = 1; run1(mo);
              VERIFY(h[8] == 64 && h[9] == 1 && h[10] == 1 && h[13] == 0 && h[14] == 1 && h[18] == 1000000 && h[19] == 0,
                     "⑬ 无任务网格间接 {1000, 1000, 1}、容量 64 → 组 {%u, %u, %u}、索引数 %u、实例 %u、原始 %u、任务 %u", h[8], h[9], h[10], h[13], h[14], h[18], h[19]);
          } }
        // ⑭ DM_NVN_MESH_PASS_SPLIT=2（M1 / M2 等无原生剔除时按「网格」vs「普通」切换断点）+ 无任务原生网格 count > 1024 分块派发
        {
            NvShader *vs14 = [NvShader new]; vs14->stage = 0; strlcpy(vs14->key, "1414141414141414_b", sizeof vs14->key); vs14->ventry = @"main0"; vs14->rf.pos_attr = -1; vs14->load_depth_zero = 1;
            { NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "vertex float4 main0(uint vid [[vertex_id]]) { return float4(2.0 + float(vid), 2.0, 0.0, 1.0); }\n" options:nil error:&e];
              REQUIRE(l); vs14->fn = [l newFunctionWithName:@"main0"]; REQUIRE(vs14->fn); }
            NvProg *plain14 = [NvProg new]; plain14->sh[0] = vs14; plain14->sh[1] = fs;
            static prog_t gp_plain14; memset(&gp_plain14, 0, sizeof gp_plain14); gp_plain14.magic = MAGIC('P', 'R', 'O', 'G'); gp_plain14.serial = nvn_new_serial(); serial_put(gp_plain14.serial, plain14);

            int want_split = [g_dev supportsFamily:MTLGPUFamilyApple9] ? 0 : 2;
            const char *se = getenv("DM_NVN_MESH_PASS_SPLIT");
            if (!se || !*se) VERIFY(mtk_split_on() == want_split, "⑭ 缺省 DM_NVN_MESH_PASS_SPLIT=%d（期望 %d）", mtk_split_on(), want_split);

            p_draw d14a = {.prim = DM_PRIM_MESH_TASKS, .first = 0, .count = 1, .instances = 1};
            p_draw d14b = {.prim = DM_PRIM_MESH_TASKS, .first = 2, .count = 1, .instances = 1};
            p_draw d14p = {.prim = 4, .count = 3, .instances = 1};
            int w14 = 0;
            if (!mtk_emu_on() && mtk_split_on() == 2) {
                frame_begin(); uint64_t rp0 = g_stat_pass; S.prog = &gp[0];
                mesh_draw(&d14a, &w14); mesh_draw(&d14b, &w14);
                cols = frame_columns();
                VERIFY(cols == 0x05 && g_stat_pass == rp0 + 1,
                       "⑭ 连续两笔直跑网格绘制共用 1 遍：渲染遍 +%llu（期望 1），列掩码 %#x（期望 0x05）", (unsigned long long)(g_stat_pass - rp0), cols);

                frame_begin(); rp0 = g_stat_pass; S.prog = &gp[0]; mesh_draw(&d14a, &w14);
                S.prog = &gp_plain14; draw(&d14p, NULL);
                S.prog = &gp[0]; mesh_draw(&d14b, &w14);
                cols = frame_columns();
                VERIFY(cols == 0x05 && g_stat_pass == rp0 + 3,
                       "⑭ 网格 / 普通 / 网格交错在边界切遍：渲染遍 +%llu（期望 3），列掩码 %#x（期望 0x05）", (unsigned long long)(g_stat_pass - rp0), cols);

                frame_begin(); rp0 = g_stat_pass; S.prog = &gp[0]; mesh_draw(&d14a, &w14);
                R.has_sc = 1; R.sc = (p_scissor){7, 0, 1, 4};
                clear_color_tri(&(p_clear_color){.index = 0, .mask = 15, .rgba = {0, 0, 0, 1}});
                R.has_sc = 0;
                S.prog = &gp[0]; mesh_draw(&d14b, &w14);
                cols = frame_columns();
                VERIFY(cols == 0x05 && g_stat_pass == rp0 + 3,
                       "⑭ 网格后遍内三角清除（clear_color_tri）后接网格在边界切遍：渲染遍 +%llu（期望 3），列掩码 %#x（期望 0x05）", (unsigned long long)(g_stat_pass - rp0), cols);
            }
            // 无任务直跑网格 count > 1024（1028 拆成 1024 + 4 两段，起始号分别落在 buffer(29)）：列 0..7 全白，Zero GPU/Metal 校验错误
            frame_begin(); S.prog = &gp[0];
            p_draw d14big = {.prim = DM_PRIM_MESH_TASKS, .first = 0, .count = 1028, .instances = 1};
            mesh_draw(&d14big, &w14);
            cols = frame_columns();
            VERIFY(cols == 0xff, "⑭ 无任务直跑网格 count=1028（>1024 分块派发）：列掩码 %#x（期望 0xff）", cols);

            frame_begin(); S.prog = &gp[0];
            p_draw d14hi = {.prim = DM_PRIM_MESH_TASKS, .first = 1024, .count = 4, .instances = 1};
            mesh_draw(&d14hi, &w14);
            cols = frame_columns();
            VERIFY(cols == 0, "⑭ 无任务直跑网格 first=1024 count=4：提交无报错、列掩码 %#x（期望 0）", cols);
        }

        // ⑮ 任务 / 网格管线二进制归档（par_mkey + par_mesh → MTLBinaryArchive 落盘并命中；mtk_pso 重用同一份）
        {
            pso_key pk15; memset(&pk15, 0, sizeof pk15);
            pso_key_fill(&pk15, md, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
            memset(pk15.attr, 0, sizeof pk15.attr); memset(pk15.stream, 0, sizeof pk15.stream);
            NSString *k15 = par_mkey(&pk15, nil, md, fs, 0, 0, nil, nil);
            VERIFY(k15 && [k15 hasPrefix:@"m"], "⑮ par_mkey 产出带前缀的网格管线归档键（%s）", k15 ? k15.UTF8String : "-");

            MTLMeshRenderPipelineDescriptor *d15a = mtk_desc(&pk15, nil, md, fs, nil, 0, nil);
            NSError *e15 = nil;
            uint64_t h0 = atomic_load(&g_par_hits), m0 = atomic_load(&g_par_misses), sf0 = atomic_load(&g_par_ser_failed), wf0 = atomic_load(&g_par_write_failed);
            int had = g_par_have && [g_par_have containsObject:k15];
            id<MTLRenderPipelineState> ps15a = par_mesh(d15a, k15, &e15);
            VERIFY(ps15a != nil && (had || atomic_load(&g_par_misses) == m0 + 1), "⑮ par_mesh 首次建管线成功（archive had=%d, misses +%llu）", had, (unsigned long long)(atomic_load(&g_par_misses) - m0));
            par_write(k15, mtk_desc(&pk15, nil, md, fs, nil, 0, nil));
            VERIFY(atomic_load(&g_par_ser_failed) == sf0 && atomic_load(&g_par_write_failed) == wf0 && [g_par_have containsObject:k15],
                   "⑮ addMeshRenderPipelineFunctionsWithDescriptor + serializeToURL 写盘成功（ser_fail=%llu write_fail=%llu）",
                   (unsigned long long)(atomic_load(&g_par_ser_failed) - sf0), (unsigned long long)(atomic_load(&g_par_write_failed) - wf0));

            MTLMeshRenderPipelineDescriptor *d15b = mtk_desc(&pk15, nil, md, fs, nil, 0, nil);
            uint64_t h1 = atomic_load(&g_par_hits);
            id<MTLRenderPipelineState> ps15b = par_mesh(d15b, k15, &e15);
            VERIFY(ps15b != nil && atomic_load(&g_par_hits) == h1 + 1, "⑮ par_mesh 二次经 MTLBinaryArchive 直接命中（hits +%llu, before_first=%llu）",
                   (unsigned long long)(atomic_load(&g_par_hits) - h1), (unsigned long long)h0);

            if (ps15b) {
                mtk_key mk15; memset(&mk15, 0, sizeof mk15); mk15.k = pk15; mk15.ts = NULL;
                os_unfair_lock_lock(&g_pso_mtk_lock);
                hm_put_id(&g_pso_mtk, &mk15, ps15b);
                os_unfair_lock_unlock(&g_pso_mtk_lock);
                frame_begin(); S.prog = &gp[0]; int w15 = 0;
                p_draw dw15 = {.prim = DM_PRIM_MESH_TASKS, .first = 4, .count = 2, .instances = 1};
                mesh_draw(&dw15, &w15);
                int cols15 = frame_columns();
                VERIFY(cols15 == 0x30, "⑮ 归档命中的网格管线绘制列 4..5（列掩码 %#x，期望 0x30）", cols15);
            }
        }

        // ⑯ 原生网格优化（大阶段内存迁设备暂存 + CAS 字节写直改 + 跨阶段变分打包 + 带 opt_tag 归档命中 + GPU 渲染等价）
        if (!mtk_emu_on()) {
            NSString *mSrc16 =
                @"#include <metal_stdlib>\n"
                 "using namespace metal;\n"
                 "template<typename T, uint N> struct spvUnsafeArray { T elements[N ? N : 1];\n"
                 "    thread T& operator[](size_t pos) thread { return elements[pos]; }\n"
                 "    const thread T& operator[](size_t pos) const thread { return elements[pos]; }\n"
                 "    device T& operator[](size_t pos) device { return elements[pos]; }\n"
                 "    const device T& operator[](size_t pos) const device { return elements[pos]; }\n"
                 "    threadgroup T& operator[](size_t pos) threadgroup { return elements[pos]; }\n"
                 "    const threadgroup T& operator[](size_t pos) const threadgroup { return elements[pos]; }\n"
                 "};\n"
                 "static inline __attribute__((always_inline)) uint dm_insert_bits(uint base, uint ins, uint off, uint bits) {\n"
                 "    return insert_bits(base, ins, off, bits);\n"
                 "}\n"
                 "void _16(thread const int& _1, thread const int& _2, threadgroup spvUnsafeArray<uint, 2048>& _3)\n"
                 "{\n"
                 "    uint _4 = uint(_1) >> uint(2);\n"
                 "    uint _5 = (uint(_1) & 3u) << 3;\n"
                 "    uint _6 = _3[_4];\n"
                 "    uint _7;\n"
                 "    for (;;) {\n"
                 "        uint _8 = dm_insert_bits(_6, uint(_2), _5, 8u);\n"
                 "        bool _9 = atomic_compare_exchange_weak_explicit((threadgroup atomic_uint*)&_3[_4], &_6, _8, memory_order_relaxed, memory_order_relaxed);\n"
                 "        if (_9) break;\n"
                 "    }\n"
                 "}\n"
                 "struct dm_vertex_t {\n"
                 "    float4 position [[position]];\n"
                 "    float4 a0 [[user(locn0)]];\n"
                 "    float4 a1 [[user(locn1)]];\n"
                 "    float4 a2 [[user(locn2)]];\n"
                 "    float4 a3 [[user(locn3)]];\n"
                 "    float4 a4 [[user(locn4)]];\n"
                 "    float4 a5 [[user(locn5)]];\n"
                 "};\n"
                 "using dm_mesh_t = metal::mesh<dm_vertex_t, void, 4, 2, metal::topology::triangle>;\n"
                 "[[mesh]] void main0(uint dm_lid [[thread_index_in_threadgroup]], constant uint& dm_first_wg [[buffer(29)]], uint dm_gid [[threadgroup_position_in_grid]], dm_mesh_t dm_out)\n"
                 "{\n"
                 "    threadgroup spvUnsafeArray<uint, 2048> s_mem;\n"
                 "    for (uint i = dm_lid; i < 2048u; i += 32u) s_mem[i] = 0u;\n"
                 "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                 "    if (dm_lid == 0u) {\n"
                 "        int o0 = 0, v0 = 0, o1 = 1, v1 = 1, o2 = 2, v2 = 2, o3 = 3, v3 = 2, o4 = 4, v4 = 1, o5 = 5, v5 = 3;\n"
                 "        _16(o0, v0, s_mem); _16(o1, v1, s_mem); _16(o2, v2, s_mem);\n"
                 "        _16(o3, v3, s_mem); _16(o4, v4, s_mem); _16(o5, v5, s_mem);\n"
                 "    }\n"
                 "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                 "    if (dm_lid == 0u) dm_out.set_primitive_count(2u);\n"
                 "    if (dm_lid < 4u) {\n"
                 "        float4 pos = dm_lid == 0u ? float4(-1.0, -1.0, 0.0, 1.0) : dm_lid == 1u ? float4(-0.5, -1.0, 0.0, 1.0) : dm_lid == 2u ? float4(-1.0, 1.0, 0.0, 1.0) : float4(-0.5, 1.0, 0.0, 1.0);\n"
                 "        dm_vertex_t v = {};\n"
                 "        v.position = pos;\n"
                 "        v.a0 = float4(0.25, 0.0, 0.5, 1.0);\n"
                 "        v.a1 = float4(0.0, 0.25, 1.0, 0.0);\n"
                 "        v.a2 = float4(0.25, 0.0, 0.0, 1.0);\n"
                 "        v.a3 = float4(0.0, 0.0, 0.0, 0.0);\n"
                 "        v.a5 = float4(0.0, 0.5, 0.0, 1.0);\n"
                 "        uint n = dm_lid;\n"
                 "        dm_out.set_vertex(n, v);\n"
                 "    }\n"
                 "    if (dm_lid < 6u) {\n"
                 "        uint w = s_mem[dm_lid >> 2];\n"
                 "        uint idx = (w >> ((dm_lid & 3u) * 8u)) & 0xffu;\n"
                 "        dm_out.set_index(dm_lid, idx);\n"
                 "    }\n"
                 "}\n"
                 "\n#ifdef S2A_MESH_EMU\n"
                 "kernel void main0_mc(uint dm_lid [[thread_index_in_threadgroup]], device uint* dm_xo [[buffer(28)]])\n"
                 "{\n"
                 "    if (dm_lid == 0u) dm_xo[0] = 0u;\n"
                 "}\n"
                 "#endif\n";
            NSString *fSrc16 =
                @"#include <metal_stdlib>\n"
                 "using namespace metal;\n"
                 "struct main0_out_u { uint4 c0 [[color(0)]]; };\n"
                 "struct main0_in\n"
                 "{\n"
                 "    float4 a0 [[user(locn0)]];\n"
                 "    float4 a1 [[user(locn1)]];\n"
                 "    float4 a2 [[user(locn2), flat]];\n"
                 "    float4 a3 [[user(locn3)]];\n"
                 "    float4 a4 [[user(locn4)]];\n"
                 "    float4 a5 [[user(locn5)]];\n"
                 "};\n"
                 "fragment float4 main0(main0_in in [[stage_in]])\n"
                 "{\n"
                 "    float s = in.a0.x + in.a0.y + in.a0.z + in.a0.w + in.a1.x + in.a1.y + in.a1.z + in.a1.w +\n"
                 "              in.a2.x + in.a2.w + in.a3.x + in.a3.w + in.a4.y + in.a4.z + in.a5.y + in.a5.w;\n"
                 "    return fabs(s - 5.75) < 0.01 ? float4(1.0) : float4(1.0, 0.0, 0.0, 1.0);\n"
                 "}\n"
                 "[[early_fragment_tests]] fragment main0_out_u main0_u(main0_in in [[stage_in]])\n"
                 "{\n"
                 "    return main0_out_u{uint4(1)};\n"
                 "}\n";

            int dev16 = 0, bstore16 = 0;
            NSString *mOpt16 = mtk_opt_mesh_src(mSrc16, &dev16, &bstore16);
            VERIFY(dev16 == 1 && bstore16 == 1 && mOpt16 &&
                   [mOpt16 containsString:@"device uint* dm_scratch [[buffer(28)]]"] &&
                   [mOpt16 containsString:@"reinterpret_cast<device uchar*>"],
                   "⑯ mtk_opt_mesh_src 迁移阶段内存至设备暂存并改写字节写（dev=%d bstore=%d）", dev16, bstore16);
            REQUIRE(mOpt16);

            NSString *mPack16 = nil, *fPack16 = nil, *layout16 = nil;
            unsigned orig16 = 0, new16 = 0;
            int pak16 = mtk_pack_pair_src(mOpt16, fSrc16, &mPack16, &fPack16, &layout16, &orig16, &new16);
            VERIFY(pak16 && orig16 == 6 && new16 <= 4 && mPack16 && fPack16,
                   "⑯ mtk_pack_pair_src 跨阶段变分打包压缩槽位（ok=%d %u->%u layout=%s）", pak16, orig16, new16, layout16 ? layout16.UTF8String : "-");
            REQUIRE(pak16);

            NSError *e16 = nil;
            id<MTLLibrary> mLibOrig = [g_dev newLibraryWithSource:mSrc16 options:dm_msl_opts() error:&e16]; REQUIRE(mLibOrig);
            id<MTLLibrary> fLibOrig = [g_dev newLibraryWithSource:fSrc16 options:dm_msl_opts() error:&e16]; REQUIRE(fLibOrig);
            id<MTLLibrary> mLibOpt  = [g_dev newLibraryWithSource:mPack16 options:dm_msl_opts() error:&e16]; REQUIRE(mLibOpt);
            id<MTLLibrary> fLibOpt  = [g_dev newLibraryWithSource:fPack16 options:dm_msl_opts() error:&e16]; REQUIRE(fLibOpt);

            NvShader *m16 = [NvShader new]; m16->stage = 4; m16->mesh_kind = 2; m16->mesh_threads = 32; m16->mesh_maxv = 4; m16->mesh_maxp = 2; m16->load_depth_zero = 1;
            strlcpy(m16->key, "aaaaaaaaaaaaaaaa_b", sizeof m16->key); m16->ventry = @"main0"; m16->fn = lib_entry(mLibOrig, @"main0"); REQUIRE(m16->fn);
            NvShader *f16 = [NvShader new]; f16->stage = 1; strlcpy(f16->key, "bbbbbbbbbbbbbbbb_b", sizeof f16->key); f16->ventry = @"main0";
            f16->fn = lib_entry(fLibOrig, @"main0"); REQUIRE(f16->fn);

            NvProg *prog16 = [NvProg new]; prog16->sh[1] = f16; prog16->sh[4] = m16;
            static prog_t gp16; memset(&gp16, 0, sizeof gp16); gp16.magic = MAGIC('P', 'R', 'O', 'G'); gp16.serial = nvn_new_serial(); serial_put(gp16.serial, prog16);

            pso_key pk16; memset(&pk16, 0, sizeof pk16);
            pso_key_fill(&pk16, m16, f16, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
            memset(pk16.attr, 0, sizeof pk16.attr); memset(pk16.stream, 0, sizeof pk16.stream);

            MTLMeshRenderPipelineDescriptor *d16orig = mtk_desc(&pk16, nil, m16, f16, nil, 0, nil);
            id<MTLRenderPipelineState> ps16orig = [g_dev newRenderPipelineStateWithMeshDescriptor:d16orig options:MTLPipelineOptionNone reflection:nil error:&e16];
            REQUIRE(ps16orig);

            MTLMeshRenderPipelineDescriptor *d16opt = mtk_desc(&pk16, nil, m16, f16, nil, 0, nil);
            d16opt.meshFunction = lib_entry(mLibOpt, @"main0"); REQUIRE(d16opt.meshFunction);
            d16opt.fragmentFunction = lib_entry(fLibOpt, @"main0"); REQUIRE(d16opt.fragmentFunction);
            (void)mtk_scratch_gpu();

            NSString *optTag16 = [NSString stringWithFormat:@"%s|%s|d%db%dp|%@", m16->key, f16->key, dev16, bstore16, layout16];
            NSString *k16orig = par_mkey(&pk16, nil, m16, f16, 0, 0, nil, nil);
            NSString *k16opt  = par_mkey(&pk16, nil, m16, f16, 0, 0, nil, optTag16);
            VERIFY(k16opt && ![k16opt isEqualToString:k16orig], "⑯ par_mkey 带 opt_tag 产出独立归档键");

            id<MTLRenderPipelineState> ps16optA = par_mesh(d16opt, k16opt, &e16);
            REQUIRE(ps16optA);
            par_write(k16opt, d16opt);
            uint64_t h16 = atomic_load(&g_par_hits);
            id<MTLRenderPipelineState> ps16optB = par_mesh(d16opt, k16opt, &e16);
            VERIFY(ps16optB != nil && atomic_load(&g_par_hits) == h16 + 1, "⑯ par_mesh + par_write 优化管线二次命中归档（hits +%llu）", (unsigned long long)(atomic_load(&g_par_hits) - h16));
            REQUIRE(ps16optB);

            mtk_key mk16; memset(&mk16, 0, sizeof mk16); mk16.k = pk16;
            p_draw dw16 = {.prim = DM_PRIM_MESH_TASKS, .first = 0, .count = 1, .instances = 1};
            os_unfair_lock_lock(&g_pso_mtk_lock); hm_put_id(&g_pso_mtk, &mk16, ps16orig); os_unfair_lock_unlock(&g_pso_mtk_lock);
            frame_begin(); S.prog = &gp16; int w16 = 0; mesh_draw(&dw16, &w16);
            int colsOrig = frame_columns();

            os_unfair_lock_lock(&g_pso_mtk_lock); hm_put_id(&g_pso_mtk, &mk16, ps16optB); os_unfair_lock_unlock(&g_pso_mtk_lock);
            frame_begin(); S.prog = &gp16; mesh_draw(&dw16, &w16);
            int colsOpt = frame_columns();
            VERIFY(colsOrig == 0x03 && colsOpt == colsOrig, "⑯ 优化+打包后网格管线真实 GPU 渲染与原版等价（orig=%#x opt=%#x，期望 0x03）", colsOrig, colsOpt);

            const uint32_t *shdr = g_mtk_scratch ? (const uint32_t *)g_mtk_scratch.contents : NULL;
            uint32_t forced16 = shdr ? shdr[MTK_SCRATCH_FORCED] : 999u;
            uint32_t busySlots16 = 0;
            if (shdr) for (uint32_t s = 0; s < g_mtk_scratch_slots; s++) if (shdr[MTK_SCRATCH_FLAGS + s]) busySlots16++;
            VERIFY(shdr && forced16 == 0 && busySlots16 == 0, "⑯ 设备暂存槽全数正常归还且无强制占用（forced=%u busy=%u）", forced16, busySlots16);
        }
        // ⑰ 间接任务/网格绘制开着渲染遍，且绑定阶段因短 UBO GPU 写触发 cbuf_pad_alloc(gpu_src=1) -> blit() -> rp_end()：
        //   必须在 g_enc != en 重试检查前绑定任务/网格参数（否则往已被关掉的渲染编码器发 RO_R_MESH_IND 崩在 dm-encode），
        //   且 g_cb_pad 对已由 GPU 拷入暂存的条目跳过 !gw_live 拒缓存（否则重试遍再次 blit()->rp_end() 命中 RED_ONCE(136)）
        if (!mtk_emu_on()) {
            NSString *mSrc17 =
                @"#include <metal_stdlib>\n"
                 "using namespace metal;\n"
                 "struct Ubo17 { float4 v[16]; };\n"
                 "struct Args17 { constant Ubo17 *c0 [[id(0)]]; };\n"
                 "struct V17 { float4 p [[position]]; };\n"
                 "using M17 = mesh<V17, void, 4, 2, topology::triangle>;\n"
                 "[[mesh]] void main0(M17 dm_out, uint dm_lid [[thread_index_in_threadgroup]], uint dm_gid [[threadgroup_position_in_grid]], constant uint *dm_first [[buffer(29)]], constant Args17 &args [[buffer(0)]]) {\n"
                 "    float ok0 = (*args.c0).v[0].x;\n"
                 "    float oob = (*args.c0).v[4].x;\n"
                 "    bool good = (fabs(ok0 - 1.0f) < 0.01f) && (fabs(oob) < 0.01f);\n"
                 "    if (dm_lid == 0u) dm_out.set_primitive_count(good ? 2u : 0u);\n"
                 "    if (good && dm_lid < 4u) {\n"
                 "        uint col = dm_first[0] + dm_gid;\n"
                 "        V17 v;\n"
                 "        float x = float(col + (dm_lid & 1u)) / 4.0f - 1.0f;\n"
                 "        v.p = float4(x, (dm_lid & 2u) ? 1.0f : -1.0f, 0.0f, 1.0f);\n"
                 "        dm_out.set_vertex(dm_lid, v);\n"
                 "    }\n"
                 "    const uchar ix[6] = {0, 1, 2, 2, 1, 3};\n"
                 "    if (good && dm_lid < 6u) dm_out.set_index(dm_lid, ix[dm_lid]);\n"
                 "}\n";
            NSError *e17 = nil;
            id<MTLLibrary> mLib17 = [g_dev newLibraryWithSource:mSrc17 options:dm_msl_opts() error:&e17]; REQUIRE(mLib17);
            NvShader *m17 = [NvShader new]; m17->stage = 4; m17->mesh_kind = 2; m17->mesh_threads = 32; m17->mesh_maxv = 4; m17->mesh_maxp = 2; m17->load_depth_zero = 1;
            strlcpy(m17->key, "1717171717171717_b", sizeof m17->key); m17->ventry = @"main0";
            m17->fn = lib_entry(mLib17, @"main0"); REQUIRE(m17->fn);
            m17->rf.nb = 1; m17->rf.b = calloc(1, sizeof *m17->rf.b); REQUIRE(m17->rf.b);
            m17->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};
            m17->ae = [m17->fn newArgumentEncoderWithBufferIndex:0]; REQUIRE(m17->ae);
            m17->arg_len = m17->ae.encodedLength;
            m17->cb_ext[0] = 128;

            NvProg *prog17 = [NvProg new]; prog17->sh[1] = fs; prog17->sh[4] = m17;
            static prog_t gp17; memset(&gp17, 0, sizeof gp17); gp17.magic = MAGIC('P', 'R', 'O', 'G'); gp17.serial = nvn_new_serial(); serial_put(gp17.serial, prog17);

            static uint32_t pool17[4096] __attribute__((aligned(16384)));
            nvn_be_pool_add(pool17, sizeof pool17);
            pool17[0] = 2; pool17[1] = 1;     // indirect mesh args: count=2, first=1 -> columns 1, 2 (0x06)
            pool17[2] = 1; pool17[3] = 5;     // indirect mesh args: count=1, first=5 -> column 5 (0x20)
            memset(&pool17[64], 0, 512);
            pool17[64 + 16] = 0x3f800000u;    // +64B poison = 1.0f; padded short UBO must read 0.0f here

            // Case 17a: same-submission GPU fill + open render pass before indirect mesh draw
            frame_begin(); S.prog = &gp17;
            mtl_sub_fill((uint64_t)(uintptr_t)&pool17[64], 16, 0x3f800000u);
            S.ubo[4][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)&pool17[64], .size = 16};
            p_draw d17a = {.prim = DM_PRIM_MESH_TASKS, .instances = 1, .ind = (uint64_t)(uintptr_t)&pool17[0]};
            o = capture(^{
                int w17 = 0;
                // Pre-warm pipeline outside pass, then re-open pass so draw reaches mtk_bind with g_enc != nil
                mesh_draw(&d17a, &w17);
            });
            cols = frame_columns();

            // Repeat cleanly with pipeline warm AND render pass explicitly open when draw starts
            frame_begin(); S.prog = &gp17;
            mtl_sub_fill((uint64_t)(uintptr_t)&pool17[64], 16, 0x3f800000u);
            rp_begin(); REQUIRE(g_enc);
            S.ubo[4][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)&pool17[64], .size = 16};
            o = capture(^{ int w17 = 0; mesh_draw(&d17a, &w17); });
            int cols17a = frame_columns();
            VERIFY(cols == 0x06 && cols17a == 0x06 && red_lines(o, nil) == 0 && !mtl_last_commit_error(),
                   "⑰a 间接网格绘制在已开遍中因 GPU 填短 UBO 重开遍：不崩于编码线程且列 1、2 白（%#x / %#x；stderr：%s）",
                   cols, cols17a, o.UTF8String);

            // Case 17b: in-flight GPU write (gw_live != 0) from previous unwaited submission + open render pass -> second retry hits e->gpu_src cache cleanly
            nvn_be_set_frame(++g_frame); nvn_be_sub_begin(); state_reset();
            pool17[64 + 16] = 0x3f800000u;
            mtl_sub_fill((uint64_t)(uintptr_t)&pool17[64], 16, 0x3f800000u);
            (void)nvn_be_sub_commit();   // purposely do not wait so gw_live is active during next submission

            frame_begin(); S.prog = &gp17;
            S.ubo[4][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)&pool17[64], .size = 16};
            p_draw d17b = {.prim = DM_PRIM_MESH_TASKS, .instances = 1, .ind = (uint64_t)(uintptr_t)&pool17[2]};
            uint64_t made0 = g_cb_pad_made;
            rp_begin(); REQUIRE(g_enc);
            o = capture(^{ int w17 = 0; mesh_draw(&d17b, &w17); });
            int cols17b = frame_columns();
            VERIFY(cols17b == 0x20 && g_cb_pad_made == made0 + 1 && red_lines(o, nil) == 0 && !mtl_last_commit_error(),
                   "⑰b 在途 gw_live 触发 GPU 补零重试命中 g_cb_pad：无红行、只拷 1 次（+%llu）、列 5 白（%#x；stderr：%s）",
                   (unsigned long long)(g_cb_pad_made - made0), cols17b, o.UTF8String);

            nvn_be_pool_del(pool17);
        }
        // ⑱ mtk_opt_mesh_src 含早退 return 时拒绝 dev=1 改写，避免提前 return 漏还 dm_scratch 槽
        {
            NSString *retSrc18 =
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "struct dm_vertex_t { float4 position [[position]]; };\n"
                 "using M18 = mesh<dm_vertex_t, void, 4, 2, topology::triangle>;\n"
                 "[[mesh]] void main0(M18 dm_out, uint dm_lid [[thread_index_in_threadgroup]])\n"
                 "{\n"
                 "    threadgroup spvUnsafeArray<uint, 2048> _99;\n"
                 "    for (uint i = dm_lid; i < 2048u; i += 32u) _99[i] = 0u;\n"
                 "    if (dm_lid > 100u) return;\n"
                 "    dm_out.set_primitive_count(0u);\n"
                 "}\n";
            int dev18 = 0, bstore18 = 0;
            NSString *opt18 = mtk_opt_mesh_src(retSrc18, &dev18, &bstore18);
            VERIFY(!dev18 && !opt18, "⑱ 含早期 return 的网格着色器拒绝 dev=1 暂存改写（dev=%d opt=%d）", dev18, opt18 != nil);
        }
        s2a_pso_archive_exit();
        pthread_mutex_lock(&g_par_wmu);
        pthread_mutex_unlock(&g_par_wmu);
        [NSFileManager.defaultManager removeItemAtPath:@(pdir) error:nil];
    }
    if (failures) { printf("✗ t_nvn_mesh：%u 项里 %u 项不符\n", checks, failures); return 1; }   // 原生门禁按「✓ t_ / ✗ t_」逐组判
    printf("✓ t_nvn_mesh：%u 项全过（任务 / 网格绘制：约定字段载入、上限报红、无顶点阶段的程序、直接 / 有任务 / GPU 写参数的间接 / 零工作量画出的列与手算一致、缺网格着色器报红、映射表阶段名 task / mesh 载入、无网格管线的 GPU 跳过报红）\n", checks); return 0;
}
