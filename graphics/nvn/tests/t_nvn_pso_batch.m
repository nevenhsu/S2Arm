// 管线归档批量写回（pso_archive.h par_write_batch，11-06 马车8 卡预热：一条一个文件写回 ~95 ms/条，一局写不完、退出即丢、每次启动从零编）。
//   直接包含生产 nvn_mtl.m，在真 Metal 设备上编 70 条不同的计算管线（先编一遍 = 预热），一次放进待写队列再同步跑写回。期望（按 PAR_BATCH=32 手算）：
//   ① 写出 3 个批文件（32 + 32 + 6）、batches.tsv 70 行、每行指向在的文件；② 70 个键都能经 par_archive_for 取到归档，只准命中归档地建管线（FailOnBinaryArchiveMiss）70/70 成；
//   ③ 写成计数 +70、写失败 0。反证（构建规则另编 -DPAR_BATCH=1）：批文件 70 个，① 不符
#include "../gfx/nvn/nvn_mtl.m"
static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL setup %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) failures++; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
int main(void) { @autoreleasepool {
    g_dev = MTLCreateSystemDefaultDevice(); REQUIRE(g_dev);
    char tmpl[] = "/tmp/t_pso_batch.XXXXXX"; REQUIRE(mkdtemp(tmpl));
    g_par_dir = @(tmpl); g_par_have = [NSMutableSet set]; g_par_open = [NSMutableDictionary dictionary]; g_par_bmap = [NSMutableDictionary dictionary]; g_par_on = 1;
    enum { N = 70 }; NSMutableArray *descs = [NSMutableArray array], *keys = [NSMutableArray array];
    for (int i = 0; i < N; i++) {
        NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void k(device float *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] = b[i] * %d.0 + %d.0; }", i + 2, i];
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&e]; REQUIRE(lib);
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = [lib newFunctionWithName:@"k"];
        REQUIRE([g_dev newComputePipelineStateWithDescriptor:d options:0 reflection:nil error:&e]);   // 预热先编一遍
        [descs addObject:d]; [keys addObject:[NSString stringWithFormat:@"t-batch-%02d", i]];
    }
    uint64_t w0 = atomic_load(&g_par_written), f0 = atomic_load(&g_par_write_failed);
    pthread_mutex_lock(&g_par_mu); g_par_todo = [NSMutableArray array]; for (int i = 0; i < N; i++) [g_par_todo addObject:@[keys[i], descs[i]]]; g_par_writer = 1; pthread_mutex_unlock(&g_par_mu);
    par_writer();
    NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_par_dir error:nil];
    NSUInteger files = [names filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'batch-' AND SELF ENDSWITH '.metalarchive'"]].count;
    NSString *idx = [NSString stringWithContentsOfFile:[g_par_dir stringByAppendingPathComponent:@"batches.tsv"] encoding:NSUTF8StringEncoding error:nil];
    NSUInteger lines = 0, dangling = 0;
    for (NSString *ln in [idx componentsSeparatedByString:@"\n"]) { NSArray *c = [ln componentsSeparatedByString:@"\t"]; if (c.count != 2) continue; lines++;
        if (![NSFileManager.defaultManager fileExistsAtPath:[g_par_dir stringByAppendingPathComponent:c[1]]]) dangling++; }
    VERIFY(files == 3 && lines == N && dangling == 0, "① 批文件 %lu 个（期望 3）、索引 %lu 行（期望 70）、指向不存在文件 %lu 行（期望 0）", (unsigned long)files, (unsigned long)lines, (unsigned long)dangling);
    int got = 0;
    for (int i = 0; i < N; i++) { id<MTLBinaryArchive> a = par_archive_for(keys[i]); if (!a) continue;
        MTLComputePipelineDescriptor *d = [descs[i] copy]; d.binaryArchives = @[a]; NSError *e = nil;
        if ([g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e]) got++; }
    VERIFY(got == N, "② 只准命中归档地建管线 %d/%d", got, N);
    VERIFY(atomic_load(&g_par_written) - w0 == N && atomic_load(&g_par_write_failed) == f0, "③ 写成 +%llu（期望 70）、写失败 +%llu（期望 0）",
           (unsigned long long)(atomic_load(&g_par_written) - w0), (unsigned long long)(atomic_load(&g_par_write_failed) - f0));
    // 11-23：写归档不再留 .writing；「不再写」只认版本先验与崩溃处理器的真崩记录（⑬ ⑭）。老版本留下的标记（被划掉 / 内存超限 / 挂起被杀都会留）起局删掉、照常写
    NSString *wflag = [g_par_dir stringByAppendingPathComponent:PAR_WRITING];
    char t3[] = "/tmp/t_pso_dead3.XXXXXX"; REQUIRE(mkdtemp(t3));   // 老版本的全机标记放临时目录（par_dead_cleanup 按目录删，不碰本机真实的 Application Support）
    VERIFY(![NSFileManager.defaultManager fileExistsAtPath:wflag], "④ 写完 70 条：归档目录里没有 .writing 标记（新版不留）");
    // ⑤ 反证（原判据）：老版本写到一半被杀留下的标记（带版本行，原先据此从此不写）→ 起局删掉、照常写
    REQUIRE(([@"s2a-pso-writing 2 pid=1 batch-x\n" writeToFile:wflag atomically:NO encoding:NSUTF8StringEncoding error:nil]));
    par_check_dead_write();
    VERIFY(g_par_nowrite == 0 && ![NSFileManager.defaultManager fileExistsAtPath:wflag], "⑤ 老版本被杀留下的 .writing（没有真崩记录）：标记删掉、照常写（nowrite=%d，应 0）", g_par_nowrite);
    REQUIRE([@"batch-x" writeToFile:wflag atomically:NO encoding:NSUTF8StringEncoding error:nil]);   // 更老的无版本行标记
    par_check_dead_write();
    VERIFY(g_par_nowrite == 0 && ![NSFileManager.defaultManager fileExistsAtPath:wflag], "⑦ 更老的无版本行标记：同样删掉、照常写（nowrite=%d）", g_par_nowrite);
    // ⑥ #756 根修（11-23）：全机「写归档时真崩」标记不再停写——老版本留下的标记起局删掉、照常写；序列化失败不读 Metal 交回的 error（那个指针是坏的），只计数、这一批不写
    { char sd[1100]; snprintf(sd, sizeof sd, "%s/S2Arm", t3); mkdir(sd, 0755); snprintf(sd, sizeof sd, "%s/S2Arm/pso_archive_dead.txt", t3);
      REQUIRE(([@"Version 26.0 (Build 23A330)\n" writeToFile:@(sd) atomically:NO encoding:NSUTF8StringEncoding error:nil]));
      int del = par_dead_cleanup(@(t3)); par_check_dead_write();
      VERIFY(del == 1 && access(sd, F_OK) != 0 && g_par_nowrite == 0, "⑥a 老版本的全机真崩标记：起局删掉（删了 %d、还在 %d）、照常写（nowrite=%d，应 0）", del, access(sd, F_OK) == 0, g_par_nowrite); }
    { NSString *saved = g_par_dir; g_par_dir = [NSString stringWithFormat:@"%s/no-such-dir/pipelines", t3];   // 写不进的目录：序列化必失败
      uint64_t sf0 = atomic_load(&g_par_ser_failed), w2 = atomic_load(&g_par_written);
      par_write_batch(@[@[@"t-ser-fail", descs[3]]]);
      pthread_mutex_lock(&g_par_mu); BOOL had = [g_par_have containsObject:@"t-ser-fail"]; pthread_mutex_unlock(&g_par_mu);
      g_par_dir = saved;
      VERIFY(atomic_load(&g_par_ser_failed) == sf0 + 1 && atomic_load(&g_par_written) == w2 && !had,
             "⑥b 序列化失败（目录写不进）：不崩、没写成计数 +%llu（应 1）、写成 +%llu（应 0）、键没记成已写（%d，应 0）",
             (unsigned long long)(atomic_load(&g_par_ser_failed) - sf0), (unsigned long long)(atomic_load(&g_par_written) - w2), had); }
    g_par_nowrite = 1;   // 版本先验命中时（⑬）：par_note 不进待写队列
    pthread_mutex_lock(&g_par_mu); NSUInteger before = g_par_todo.count; pthread_mutex_unlock(&g_par_mu);
    par_note(@"t-after-dead-write", descs[0]);
    pthread_mutex_lock(&g_par_mu); NSUInteger after = g_par_todo.count; pthread_mutex_unlock(&g_par_mu);
    VERIFY(after == before, "⑥c 不写时 par_note 不进待写队列（%lu → %lu）", (unsigned long)before, (unsigned long)after);
    g_par_nowrite = 0; atomic_store(&g_par_closing, 0);
    // ⑧ 一批写到一半正常收局（#618）：不留标记，下一局照常写
    s2a_pso_archive_exit();
    VERIFY(![NSFileManager.defaultManager fileExistsAtPath:wflag], "⑧ 写到一半正常收局：没有标记");
    par_check_dead_write();
    VERIFY(g_par_nowrite == 0, "⑨ 下一局起局：照常写（nowrite=%d）", g_par_nowrite);
    // 收局之后写线程再起一批（与收局赛跑输了的那一侧）：不写、不进队列
    uint64_t w1 = atomic_load(&g_par_written);
    par_write_batch(@[@[@"t-after-exit", descs[1]]]);
    VERIFY(![NSFileManager.defaultManager fileExistsAtPath:wflag] && atomic_load(&g_par_written) == w1, "⑩ 收局后起的一批：不留标记、不写（写成 +%llu）", (unsigned long long)(atomic_load(&g_par_written) - w1));
    pthread_mutex_lock(&g_par_mu); before = g_par_todo.count; pthread_mutex_unlock(&g_par_mu);
    par_note(@"t-after-exit-note", descs[2]);
    pthread_mutex_lock(&g_par_mu); after = g_par_todo.count; pthread_mutex_unlock(&g_par_mu);
    VERIFY(after == before, "⑪ 收局后 par_note 不进待写队列（%lu → %lu）", (unsigned long)before, (unsigned long)after);
    { char cmd[256]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", t3); system(cmd); }
    [NSFileManager.defaultManager removeItemAtPath:g_par_dir error:nil];
    // ⑬ #756 版本先验（期望按玩家诊断定的区间手写）：iOS 18.0–18.3 不写；18.4、17.7、26.0 与非 iOS 的 18.2 照写
    VERIFY(par_os_gate(1, 18, 0) && par_os_gate(1, 18, 3) && !par_os_gate(1, 18, 4) && !par_os_gate(1, 17, 7) && !par_os_gate(1, 26, 0) && !par_os_gate(0, 18, 2),
           "⑬ 版本先验：iOS 18.0 / 18.3 不写，18.4 / 17.7 / 26.0 / 非 iOS 18.2 照写");
    // ⑬b macOS 同代（D-K7DM 15.2、D-TWC9 15.3.1、D-694R 15.3 每局崩在 airntSerializeToURL）：15.0 / 15.2 / 15.3 不写；15.4、15.6、14.7、26.0、27.3 照写；iOS 15.2 照写
    VERIFY(par_os_gate(0, 15, 0) && par_os_gate(0, 15, 2) && par_os_gate(0, 15, 3) && !par_os_gate(0, 15, 4) && !par_os_gate(0, 15, 6) && !par_os_gate(0, 14, 7)
           && !par_os_gate(0, 26, 0) && !par_os_gate(0, 27, 3) && !par_os_gate(1, 15, 2),
           "⑬b 版本先验：macOS 15.0 / 15.2 / 15.3 不写，15.4 / 15.6 / 14.7 / 26.0 / 27.3 与 iOS 15.2 照写");
    // ⑭ 不再有「全机学习」：只剩版本先验（⑬）决定不写；非 18.0–18.3 的系统一律照写
    VERIFY(!par_gate_or_learned(1, 26, 0) && !par_gate_or_learned(1, 27, 2) && !par_gate_or_learned(0, 15, 7) && par_gate_or_learned(1, 18, 2) && par_gate_or_learned(0, 15, 3),
           "⑭ 只有版本先验：iOS 26.0 / 27.2、macOS 15.7 照写，iOS 18.2、macOS 15.3 不写");
    if (failures) { printf("✗ t_nvn_pso_batch：%u 项里 %u 项不符\n", checks, failures); return 1; }
    printf("✓ t_nvn_pso_batch：%u 项全过（70 条管线写成 3 个批文件，按键全部取回；老版本的 .writing 与全机真崩标记都删掉、照常写；序列化失败不读 error、只计数；正常收局不留标记）\n", checks); return 0;
} }
