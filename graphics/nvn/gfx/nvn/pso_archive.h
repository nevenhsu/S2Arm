// S2Arm: the pipeline archive (after MacNativeGames totk_pipeline_archive.h, MIT). A pipeline's GPU code, once compiled on this device, is kept in a Metal binary archive beside
// pso_user.tsv (pipelines/<key>.metalarchive, or batch-*.metalarchive files indexed by batches.tsv, see par_write_batch) and later builds take it from there instead of compiling: the system's own shader cache is cleared by reboots and OS updates, and
// the prepare step (warm_prepare) rebuilt every listed pipeline from scratch after one. Measured there (M4, 2026-10-21): one file per pipeline, because a file holding many costs
// memory about quadratic in their number and Metal searches every archive handed to a pipeline; a one-pipeline file opens in 0.08 ms (about 35 KB) and gives its pipeline in
// 0.04 ms. iPhone 14: prewarm 95-122 s → 5.9 s, pipeline time in long frames 1,452 → 102 ms.
// A build first asks only its own file (MTLPipelineOptionFailOnBinaryArchiveMiss); a miss compiles as before. Writing compiles the pipeline once more, and writes go one at a time
// (g_par_wmu: the build folder a write leaves is told apart from others' by appearing during it): written in place they held the prepare step's 18 workers behind one lock
// (mhrise on the Mac 10-26: 2.0 s → 48.9 s). So the prewarm's misses are only noted (par_write_in_place) and written beside it, one by one by a utility writer (par_note);
// a pipeline first compiled in play is in pso_user.tsv, and the next start's prewarm notes it.
// The key only finds the file: Metal checks the functions and state, so a wrong file is a miss, never a wrong pipeline. A file that misses (another package's libraries under the
// same key) is removed and written again. Valid for one OS version, GPU and translation chain (pipelines/identity.json; otherwise the folder is removed).
// Every archive written leaves a build folder (com.apple.gpuarchiver/gpuarchiver-*, about 380 KB) that Metal never removes: par_write removes the one its own write made, and the
// app's own leftovers are removed at start. DM_PSO_ARCHIVE=0 turns it off. Included by nvn_mtl.m after MLOG, g_dev and pso_user_path.
#pragma once
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "dmsh_chain.h"

static pthread_mutex_t g_par_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet<NSString *> *g_par_have;                              // keys with a file
static NSMutableDictionary<NSString *, NSString *> *g_par_bmap;          // key → batch file name (pipelines/batches.tsv; 11-06, see par_write_batch)
static NSMutableDictionary<NSString *, id<MTLBinaryArchive>> *g_par_open; // opened (kept: Metal keeps their memory anyway)
static NSString *g_par_dir, *g_par_bdir;                                // g_par_bdir: the package's read-only archives (shaders/direct/pipelines, made at pack time)
static NSMutableSet<NSString *> *g_par_bhave;                             // keys with a package file
static int g_par_on;
// #756（iPad Pro M1、iOS 18.3.1：每次起局预热后，后台写管线归档时在 Metal 里 objc_retain 崩，之后每次打开都闪退）：写归档只是「下次起得快」的优化，
//   写不成不影响玩。哪台设备哪个系统版本不再写，只认两条（下面 ①②）：iOS 18.0–18.3 版本先验；崩溃处理器在崩溃线程正写归档时记下的全机标记。
//   原先另有一条「每批写之前在归档目录留 .writing、写完删掉，起局见到它就不再写」：进程死在写里就算——可被划掉、内存超限被杀、挂起时被系统杀
//   （0xdead10cc）也都死在写里、也留下它（11-23 诊断库 2194 份里 338 份报「the last run died while writing it」），这些设备从此每次起局重编全部管线。
//   它分不清真崩和被杀，真崩已由崩溃处理器记全机标记，这条去掉：不再留标记；老版本留下的标记起局时删掉、照常写（par_check_dead_write）
static int g_par_nowrite;
static _Atomic int g_par_closing;   // 进程在收局：不再起新的一批写（s2a_pso_archive_exit）
static pthread_mutex_t g_par_mkmu = PTHREAD_MUTEX_INITIALIZER;   // 起一批写与收局互斥：收局置位之后不再起新的一批
#define PAR_WRITING @".writing"   // 老版本（11-23 之前）写归档时留的标记文件名：只认来删
// 起一批写之前问：已在收局返回 0、不写（#618：收局 _exit 时写线程正写在 Metal 里也无妨，丢一批下次再写）
static int par_may_write(void) {
    pthread_mutex_lock(&g_par_mkmu);
    int go = !atomic_load(&g_par_closing);
    pthread_mutex_unlock(&g_par_mkmu);
    return go;
}
// #756 进阶（11-16）：每款各自的 .writing 标记要每款先崩一次才学会，且用户划掉 / 内存杀也会留下它（玩家诊断 27.x 上 3214 局里 143 局），不能直接全机共享。另加两道：
//   ① 版本先验：iOS 18.0–18.3.x 不写。依据玩家诊断：崩溃线程栈在归档写入器里的 7 局全在 18.1.1 / 18.3.2，18.2 / 18.3.2 有 18 局「上次死在写里」、0 局写成；18.4、18.7.x 共 13 局写成、0 崩
//   ② 原有的「全机学习」（崩溃线程正在写归档时由崩溃处理器记系统版本串进 Application Support/S2Arm/pso_archive_dead.txt，起局读到就整机不写）去掉（11-23 维护者：要根修）：
//      唯一留下崩溃现场的那次（iPhone18,2 iOS 26.0、0.6.7，写进 1852 条后崩）对上符号 = serializeToURL:error: 失败返回后，ARC 立刻 retain 它写回的 NSError
//      时总线错——Metal 在序列化失败路径上交回的 error 指针是失效的（返回那一刻就坏，不是我们自动释放池排空后才读）。根修：写归档的几个调用不取 error（见 par_write_batch），
//      失败只看返回值；全机标记不再记、不再读，老版本留下的标记起局删掉（这台机从此照常写）。全机标记涉及 macOS 15.1 / 15.3、iOS 26.0 / 27.2 / 27.2.1 共 6 个构建，不是某一版独有
static char g_par_os_key[192];
//   ③ macOS 15.0–15.3 不写（11-27，诊断 D-K7DM / D-TWC9 / D-694R）：与 iOS 18.0–18.3 同一代系统，崩在同一处——serializeToURL → airntSerializeToURL →
//      materializeAirNTFromOnlineForSlice 里 objc_retain / objc_msgSend / dispatch_retain 失效对象（Metal 自己的缺陷，不经我们读 error，上面的根修管不到）。
//      诊断库 Mac 按系统版本：15.2 / 15.3 共 3 份（M1 Max / M3 / M4，三款游戏）每局都崩、0 份写成；15.6 写成 8 份、15.7 39 份、0 崩。15.4 / 15.5 没有样本，
//      按 iOS 18.4 已修的同代版本放行；15.1 有 1 份写成，但旧全机标记里记过 15.1 真崩，同代一并不写
static int par_os_gate(int ios, long major, long minor) { return (ios && major == 18 && minor < 4) || (!ios && major == 15 && minor < 4); }
static int par_dead_cleanup(NSString *as_dir) {   // 老版本的全机标记删掉；删了回 1（单测直接调，传临时目录）
    NSString *dead = [[as_dir stringByAppendingPathComponent:@"S2Arm"] stringByAppendingPathComponent:@"pso_archive_dead.txt"];
    if (unlink(dead.fileSystemRepresentation) != 0) return 0;
    MLOG("【黄】 pipeline archive: removed the device-wide \"crashed inside the archive writer\" marker from an older build: the crash was reading the error Metal returns from a failed serialize, which this build no longer reads; writing again (#756)");
    return 1;
}
static void par_global_init(void) {
    NSString *osk = NSProcessInfo.processInfo.operatingSystemVersionString ?: @""; snprintf(g_par_os_key, sizeof g_par_os_key, "%s", osk.UTF8String);
    NSURL *as = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    if (as) par_dead_cleanup(as.path);
}
// ① 命中返回 1（并打一行原因）。单测直接调
static int par_gate_or_learned(int ios, long major, long minor) {
    if (par_os_gate(ios, major, minor)) { MLOG("【黄】 pipeline archive: %s %ld.%ld: Metal's archive writer crashes on iOS 18.0–18.3 / macOS 15.0–15.3 (#756, player diagnoses): reading it as usual, writing nothing", ios ? "iOS" : "macOS", major, minor); return 1; }
    return 0;
}
static void par_check_dead_write(void) {   // 起局（打开归档目录时）调：版本先验或崩溃处理器记过真崩 → 这个系统版本不再写；老版本的 .writing 标记删掉
    { NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
      if (par_gate_or_learned(TARGET_OS_IPHONE, (long)v.majorVersion, (long)v.minorVersion)) { g_par_nowrite = 1; return; } }
    // 老版本留下的 .writing（进程死在写里就留：被划掉 / 内存超限 / 挂起被杀都算，分不清真崩）：没有崩溃处理器的真崩记录佐证（上面全机标记没命中），删掉、照常写。
    //   被它停写过的设备由此自己恢复，不用玩家做什么
    NSString *path = [g_par_dir stringByAppendingPathComponent:PAR_WRITING];
    if (unlink(path.fileSystemRepresentation) == 0)
        MLOG("【黄】 pipeline archive: removed a .writing marker from an older build (left by any death while writing: a swipe, a memory kill, a suspension kill — not proof of a crash in Metal): writing again; a real crash in the writer is recorded by the crash handler (#756)");
}
static const char *g_par_harvest;                                        // S2A_PSO_HARVEST: the folder the harvest writes into (nvn_pso_harvest); writes at once instead of noting
static _Atomic uint64_t g_par_compiled;   // 真现编的管线条数（不从随包 / 本机归档取；归档关着也计）：加载页按它分「优化」与「载入中」（loadprog.c）
static _Atomic uint64_t g_par_hits, g_par_misses, g_par_written, g_par_write_failed, g_par_stale, g_par_ga_removed;
static _Atomic uint64_t g_par_ser_failed;   // 序列化没成的批（Metal 返回失败；它的 error 不读，见 par_write_batch）
static __thread int par_write_in_place;                                  // the prewarm's threads: what they compile is noted for writing (par_flush_bg)
static NSMutableArray *g_par_todo;                                       // @[key, descriptor] noted by the prewarm, written beside it (par_writer)

static NSDictionary *par_identity(id<MTLDevice> dev) {
    return @{@"schema": @1, @"chain": @DMSH_CHAIN, @"gpu": dev.name ?: @"?", @"os": NSProcessInfo.processInfo.operatingSystemVersionString ?: @"?"};
}
static NSString *par_hash(const void *bytes, size_t n) {
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(bytes, (CC_LONG)n, d);
    NSMutableString *s = [NSMutableString stringWithCapacity:40];
    for (int i = 0; i < 20; i++) [s appendFormat:@"%02x", d[i]];   // 160 bits name a file
    return s;
}
static NSString *par_file(NSString *key) { return [g_par_dir stringByAppendingFormat:@"/%@.metalarchive", key]; }
// Metal's archive build folders: com.apple.gpuarchiver under confstr's user cache folder and the Caches folder, each with and without the bundle id (iPhone: Library/Caches/<bundle
// id>/com.apple.gpuarchiver; confstr gives no path there). Folders under the bundle id are this app's own; the others are shared, and par_write removes only folders that appeared
// while it wrote (one write at a time, g_par_wmu)
static pthread_mutex_t g_par_wmu = PTHREAD_MUTEX_INITIALIZER;
static NSArray<NSDictionary *> *par_ga_dirs(void) {   // @{path, own}
    static NSArray *dirs; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableArray *bases = [NSMutableArray array];
        char b[PATH_MAX]; if (confstr(_CS_DARWIN_USER_CACHE_DIR, b, sizeof b)) [bases addObject:@(b)];
        NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
        if (caches) [bases addObject:caches];
        NSString *bundle = NSBundle.mainBundle.bundleIdentifier;
        NSMutableArray *out = [NSMutableArray array]; NSMutableSet *seen = [NSMutableSet set];
        for (NSString *base in bases) for (int own = bundle.length ? 1 : 0; own >= 0; own--) {
            NSString *dir = [(own ? [base stringByAppendingPathComponent:bundle] : base) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
            dir = dir.stringByResolvingSymlinksInPath.stringByStandardizingPath;
            if (![seen containsObject:dir]) { [seen addObject:dir]; [out addObject:@{@"path": dir, @"own": @(own)}]; }
        }
        dirs = out;
    });
    return dirs;
}
// readdir, not NSFileManager: a bare Mac binary shares one folder with every other program (10-26 this Mac: 20,929 entries, 13 GB), and listing it twice per write through
//   NSFileManager made each write about a second (mhrise: 53 of 415 written in a minute)
#include <dirent.h>
static NSSet<NSString *> *par_ga_list(int own_only) {
    NSMutableSet *s = [NSMutableSet set];
    for (NSDictionary *d in par_ga_dirs()) {
        if (own_only && ![d[@"own"] boolValue]) continue;
        DIR *dp = opendir([d[@"path"] fileSystemRepresentation]); if (!dp) continue;
        struct dirent *e; NSString *base = d[@"path"];
        while ((e = readdir(dp))) if (!strncmp(e->d_name, "gpuarchiver-", 12)) [s addObject:[base stringByAppendingFormat:@"/%s", e->d_name]];
        closedir(dp);
    }
    return s;
}
// 跨进程锁：同一用户的裸二进制宿主共用一个 com.apple.gpuarchiver 目录（见上）。同时开几局（普查本机并行）时，A 写存档的「前后两次列目录」之间 B 正在建的
//   gpuarchiver-* 也算成 A 新建的被删掉，B 的序列化找不到自己的临时目录（11-01 普查 93 款里 12 款报 cannot remove、6 款报 cannot write …
//   Failed to generate machO: cannot pack / cannot read …/C//…），存档没写成、下一局重编。写存档整段（列目录、建、序列化、再列、删）在这把锁里串行，
//   前后两次列目录之间多出的只会是本进程的。锁文件放在第一个 gpuarchiver 目录的上一级（同一用户的宿主看到同一路径）；打不开锁照旧只用进程内锁
//   iPhone / iPad / Apple TV 不拿这把文件锁：App 容器里只有本进程一个宿主（没有别的进程会碰同一个 gpuarchiver 目录），进程内的 g_par_wmu 已经串行；
//   而存档写手在后台低优先级跑、一批可写几秒，切到后台时系统发现挂起的 App 还拿着文件锁就直接杀掉（RunningBoard 0xdead10cc，诊断 D-7NAV 11-23 iPhone19,2：
//   符号化后第 156 号线程正停在 par_write_batch → serializeToURL，锁就是这一把），玩家看到的是「切出去回来游戏没了」
#include <sys/file.h>
static int par_ga_lock(void) {
#if TARGET_OS_IPHONE
    return -1;
#else
    static int fd = -2; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (fd == -2) { NSArray *d = par_ga_dirs(); fd = -1;
        if (d.count) { NSString *lp = [[d[0][@"path"] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"s2a-gpuarchiver.lock"]; fd = open(lp.fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC, 0644); }
        if (fd < 0) MLOG("pipeline archive: no cross-process lock for Metal's build folder (open failed): concurrent hosts may remove each other's folders"); }
    pthread_mutex_unlock(&mu);
    if (fd >= 0) while (flock(fd, LOCK_EX) != 0 && errno == EINTR) {}
    return fd;
#endif
}
static void par_ga_unlock(int fd) { if (fd >= 0) flock(fd, LOCK_UN); }
static void par_ga_remove_new(NSSet<NSString *> *before) {
    NSMutableSet<NSString *> *made = [par_ga_list(0) mutableCopy]; [made minusSet:before];
    for (NSString *path in made) {
        NSError *e = nil;
        if ([NSFileManager.defaultManager removeItemAtPath:path error:&e]) atomic_fetch_add(&g_par_ga_removed, 1);
        else { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 4) MLOG("【红】 pipeline archive: cannot remove Metal's build folder %s: %s", path.UTF8String, e.localizedDescription.UTF8String); }
    }
}
// Once: the folder of this OS version, GPU and chain, and the keys it has files for
static void par_open_dir(void) {
    static dispatch_once_t once;
    // 身份（par_identity）要 GPU 名：设备没建好就开，身份里 GPU 写成「?」，和存着的不符 → 整个归档删掉、再写一份「?」，
    //   下一局设备在时又判「变了」再删一次（0.6.10 iOS：快照恢复局在 mtl_init 之前就建着色器、起预热，恢复局与其后的普通局交替清空归档、
    //   每次全部重编，玩家看到「每次都要优化画面」）。设备没建好时这一刻按没有归档走，不算身份、不写、不删；设备建好后的下一次调用才真正打开
    if (!g_dev) {
        static _Atomic int said;
        if (!atomic_exchange(&said, 1)) MLOG("pipeline archive: no Metal device yet, opened once the device exists (its identity needs the GPU name)");
        return;
    }
    dispatch_once(&once, ^{
        const char *ev = getenv("DM_PSO_ARCHIVE"); if (ev && !strcmp(ev, "0")) { MLOG("DM_PSO_ARCHIVE=0: no pipeline archive, every pipeline compiles"); return; }
        // The package's archives (pack time, shader line 10-26: one file per pipeline under the same key, made for the target GPU): looked up first, never removed or rewritten
        g_par_bhave = [NSMutableSet set];
        if (g_shmap_path && !g_par_harvest) {
            g_par_bdir = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pipelines"];
            for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_par_bdir error:nil] ?: @[]) if ([name hasSuffix:@".metalarchive"]) [g_par_bhave addObject:name.stringByDeletingPathExtension];
            if (g_par_bhave.count) MLOG("pipeline archive in the package %s: %lu pipelines", g_par_bdir.UTF8String, (unsigned long)g_par_bhave.count);
        }
        NSString *user = g_par_harvest ? nil : pso_user_path(); if (!user && !g_par_harvest) return;   // no save directory (unit tests): nowhere to keep it
        g_par_dir = g_par_harvest ? @(g_par_harvest) : [user.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pipelines"];
        NSFileManager *fm = NSFileManager.defaultManager;
        dispatch_async(s2a_qos_global(S2A_QOS_IO_BG), ^{   // the app's own build folders left by earlier runs (a crash in a write)
            pthread_mutex_lock(&g_par_wmu); NSUInteger n = 0;
            for (NSString *path in par_ga_list(1)) if ([NSFileManager.defaultManager removeItemAtPath:path error:nil]) n++;
            pthread_mutex_unlock(&g_par_wmu);
            if (n) MLOG("pipeline archive: removed %lu build folders Metal left behind in earlier runs", (unsigned long)n);
        });
        NSString *stamp_path = [g_par_dir stringByAppendingPathComponent:@"identity.json"];
        NSData *sd = [NSData dataWithContentsOfFile:stamp_path];
        NSDictionary *stamp = sd ? [NSJSONSerialization JSONObjectWithData:sd options:0 error:nil] : nil;
        NSDictionary *now = par_identity(g_dev);
        if (![stamp isEqual:now] && [fm fileExistsAtPath:g_par_dir]) {
            if (stamp) {   // which entries differ, old → new: an archive dropped again on the next run with nothing visibly changed needs the values to place it
                NSMutableString *diff = [NSMutableString string]; NSMutableSet *keys = [NSMutableSet setWithArray:now.allKeys]; [keys addObjectsFromArray:[stamp isKindOfClass:NSDictionary.class] ? stamp.allKeys : @[]];
                for (NSString *k in [keys.allObjects sortedArrayUsingSelector:@selector(compare:)]) {
                    id o = [stamp isKindOfClass:NSDictionary.class] ? stamp[k] : nil, n = now[k];
                    if (!(o == n || [o isEqual:n])) [diff appendFormat:@"%s%@ \"%@\" → \"%@\"", diff.length ? "; " : "", k, o ?: @"(none)", n ?: @"(none)"];
                }
                MLOG("pipeline archive: the OS version, GPU or translation chain changed (%s): removed, built again", diff.length ? diff.UTF8String : "stamp unreadable");
            }
            [fm removeItemAtPath:g_par_dir error:nil];
        }
        [fm createDirectoryAtPath:g_par_dir withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSJSONSerialization dataWithJSONObject:now options:NSJSONWritingSortedKeys error:nil] writeToFile:stamp_path atomically:YES];
        g_par_have = [NSMutableSet set]; g_par_open = [NSMutableDictionary dictionary]; uint64_t bytes = 0;
        for (NSString *name in [fm contentsOfDirectoryAtPath:g_par_dir error:nil] ?: @[]) if ([name hasSuffix:@".metalarchive"] && ![name hasPrefix:@"batch-"]) {
            [g_par_have addObject:name.stringByDeletingPathExtension];
            bytes += [[fm attributesOfItemAtPath:[g_par_dir stringByAppendingPathComponent:name] error:nil] fileSize];
        }
        // Batch files (par_write_batch): batches.tsv holds key → file, later lines win (a key written again after a miss); a line whose file is gone is skipped
        g_par_bmap = [NSMutableDictionary dictionary]; NSUInteger nb = 0;
        NSString *idx = [NSString stringWithContentsOfFile:[g_par_dir stringByAppendingPathComponent:@"batches.tsv"] encoding:NSUTF8StringEncoding error:nil];
        NSMutableSet *bfiles = [NSMutableSet set];
        for (NSString *ln in [idx componentsSeparatedByString:@"\n"]) {
            NSArray *c = [ln componentsSeparatedByString:@"\t"]; if (c.count != 2 || ![c[1] hasPrefix:@"batch-"]) continue;
            if (![bfiles containsObject:c[1]]) { if (![fm fileExistsAtPath:[g_par_dir stringByAppendingPathComponent:c[1]]]) continue; [bfiles addObject:c[1]]; }
            g_par_bmap[c[0]] = c[1]; [g_par_have addObject:c[0]];
        }
        for (NSString *f in bfiles) { bytes += [[fm attributesOfItemAtPath:[g_par_dir stringByAppendingPathComponent:f] error:nil] fileSize]; nb++; }
        par_global_init();
        par_check_dead_write();
        g_par_on = 1;
        MLOG("pipeline archive %s: %lu pipelines (%lu in %lu batch files, the rest one file each; %.1f MB), opened when used", g_par_dir.UTF8String, (unsigned long)g_par_have.count,
             (unsigned long)g_par_bmap.count, (unsigned long)nb, bytes / 1048576.0);
    });
}
static id<MTLBinaryArchive> par_bundle_for(NSString *key) {   // the package's file for this key, or nil (kept open like the device's)
    par_open_dir();
    if (!key || ![g_par_bhave containsObject:key]) return nil;
    NSString *k = [@"b/" stringByAppendingString:key];
    pthread_mutex_lock(&g_par_mu);
    id<MTLBinaryArchive> a = g_par_open[k];
    if (!a) {
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new]; d.url = [NSURL fileURLWithPath:[g_par_bdir stringByAppendingFormat:@"/%@.metalarchive", key]];
        a = [g_dev newBinaryArchiveWithDescriptor:d error:nil];
        if (a) g_par_open[k] = a; else [g_par_bhave removeObject:key];
    }
    pthread_mutex_unlock(&g_par_mu);
    return a;
}
static id<MTLBinaryArchive> par_archive_for(NSString *key) {
    par_open_dir();
    if (!g_par_on || !key) return nil;
    pthread_mutex_lock(&g_par_mu);
    id<MTLBinaryArchive> a = g_par_open[key];
    NSString *bf = a ? nil : g_par_bmap[key];
    if (bf) {   // in a batch file: one open archive per file, shared by its keys
        NSString *fk = [@"f/" stringByAppendingString:bf]; a = g_par_open[fk];
        if (!a) { MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new]; d.url = [NSURL fileURLWithPath:[g_par_dir stringByAppendingPathComponent:bf]];
                  a = [g_dev newBinaryArchiveWithDescriptor:d error:nil]; if (a) g_par_open[fk] = a; }
        if (a) g_par_open[key] = a; else { [g_par_bmap removeObjectForKey:key]; [g_par_have removeObject:key]; }
    } else if (!a && [g_par_have containsObject:key]) {
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new]; d.url = [NSURL fileURLWithPath:par_file(key)];
        NSError *e = nil; a = [g_dev newBinaryArchiveWithDescriptor:d error:&e];
        if (a) g_par_open[key] = a;
        else { [g_par_have removeObject:key]; [NSFileManager.defaultManager removeItemAtPath:par_file(key) error:nil]; }
    }
    pthread_mutex_unlock(&g_par_mu);
    return a;
}
static BOOL par_has_key(NSString *key) {
    par_open_dir();
    if (!g_par_on || !key) return NO;
    pthread_mutex_lock(&g_par_mu);
    BOOL have = (g_par_bhave && [g_par_bhave containsObject:key]) || (g_par_have && [g_par_have containsObject:key]);
    pthread_mutex_unlock(&g_par_mu);
    return have;
}
static void par_drop(NSString *key) {   // its file did not give the pipeline (another package's libraries under the same key): written again by the next write
    pthread_mutex_lock(&g_par_mu); [g_par_open removeObjectForKey:key]; [g_par_have removeObject:key];
    BOOL batched = g_par_bmap[key] != nil; [g_par_bmap removeObjectForKey:key]; pthread_mutex_unlock(&g_par_mu);
    if (!batched) [NSFileManager.defaultManager removeItemAtPath:par_file(key) error:nil];   // a batch file holds others' pipelines: kept, the key is written again into a new one
    atomic_fetch_add(&g_par_stale, 1);
}
// One pipeline as its own file (a temporary name, then renamed)
static void par_write(NSString *key, id desc) {
    pthread_mutex_lock(&g_par_mu); BOOL have = [g_par_have containsObject:key]; pthread_mutex_unlock(&g_par_mu);
    if (have) return;
    NSError *e = nil; BOOL ok;
    NSString *path = par_file(key), *temp = [path stringByAppendingString:@".tmp"];
    pthread_mutex_lock(&g_par_wmu); int lk = par_ga_lock();   // 跨进程串行（见 par_ga_lock）
    NSSet<NSString *> *before = par_ga_list(0);
    @autoreleasepool {
        id<MTLBinaryArchive> a = [g_dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
        // 加管线 / 序列化不取 error（见 par_write_batch：Metal 失败路径交回的 error 是失效指针，读它就崩）
        ok = a && ([desc isKindOfClass:MTLRenderPipelineDescriptor.class] ? [a addRenderPipelineFunctionsWithDescriptor:desc error:nil]
                                                                          : [desc isKindOfClass:MTLMeshRenderPipelineDescriptor.class] ? [a addMeshRenderPipelineFunctionsWithDescriptor:desc error:nil]
                                                                          : [a addComputePipelineFunctionsWithDescriptor:desc error:nil]);
        if (ok && ![a serializeToURL:[NSURL fileURLWithPath:temp] error:nil]) { ok = NO; atomic_fetch_add(&g_par_ser_failed, 1); }
        if (ok) ok = rename(temp.fileSystemRepresentation, path.fileSystemRepresentation) == 0;
    }
    par_ga_remove_new(before);
    par_ga_unlock(lk); pthread_mutex_unlock(&g_par_wmu);
    if (!ok) { unlink(temp.fileSystemRepresentation);
               if (atomic_fetch_add(&g_par_write_failed, 1) < 4) MLOG("【红】 pipeline archive: cannot write %s: %s", path.UTF8String, e ? e.localizedDescription.UTF8String : "adding / serializing failed (Metal's error not read) or rename failed"); return; }
    pthread_mutex_lock(&g_par_mu); [g_par_have addObject:key]; pthread_mutex_unlock(&g_par_mu);
    atomic_fetch_add(&g_par_written, 1);
}
// The prewarm's misses go to one writer at utility QoS, started at the first one and running beside the prewarm, off the game's way (the next start takes them from the
//   archive). Not after the prewarm: 11-02 mhrise on an iPhone 17 Pro (thermal state serious) had the first frame wait 64 s for 12,741 of 17,630 lines and the rest 52 s more in
//   play; the writes after it (each compiles its pipeline once more) never finished before the app went to the background, and the runs before it ended before the prewarm did,
//   so every start found 0 in the archive and compiled all 17,630 again. Written as they come, whatever a run compiled is kept, however it ends
// Up to PAR_BATCH noted pipelines into one file (11-06 Mario Kart 8 Deluxe on an iPhone, prewarm list 19,077 lines: one file per pipeline wrote about 95 ms each — the
//   lock, two listings of Metal's build folders, a serialization and a 380 KB build folder per pipeline — so a run wrote 446 of 5,830 in 42.6 s, the rest were lost at exit and every
//   start compiled almost everything again behind the 15 s first-frame cap). Measured on this Mac (fresh pipelines): one per file 9.9–10.9 ms each, 32 per file 2.6–4.1 ms each;
//   opening a 32-pipeline file 1.24 ms (0.04 ms a pipeline, a one-pipeline file 0.22 ms), 64 already 4.3 ms and 200 34 ms (the cost that grows with the count, see the top)
#ifndef PAR_BATCH
#define PAR_BATCH 32   // 单测反证编 -DPAR_BATCH=1
#endif
static void par_write_batch(NSArray<NSArray *> *items) {
    NSMutableArray<NSArray *> *todo = [NSMutableArray array];
    pthread_mutex_lock(&g_par_mu); for (NSArray *it in items) if (![g_par_have containsObject:it[0]]) [todo addObject:it]; pthread_mutex_unlock(&g_par_mu);
    if (!todo.count) return;
    NSString *name = [NSString stringWithFormat:@"batch-%llx-%d.metalarchive", (unsigned long long)clock_gettime_nsec_np(CLOCK_REALTIME), getpid()];
    NSString *path = [g_par_dir stringByAppendingPathComponent:name], *temp = [path stringByAppendingString:@".tmp"];
    NSMutableArray<NSString *> *keys = [NSMutableArray array]; NSError *e = nil; BOOL ok = NO;
    pthread_mutex_lock(&g_par_wmu); int lk = par_ga_lock();
    NSSet<NSString *> *before = par_ga_list(0);
    // #618：已在收局就不写（par_may_write）
    if (!par_may_write()) { par_ga_unlock(lk); pthread_mutex_unlock(&g_par_wmu); return; }
    @autoreleasepool {
        id<MTLBinaryArchive> a = [g_dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
        // 加管线与序列化不取 error（11-23 #756 根修）：唯一留下现场的那次真崩 = serializeToURL:error: 失败返回后 ARC retain 写回的 NSError 时总线错——
        //   Metal 在失败路径交回的 error 指针返回那一刻就已失效，读它（哪怕只是 ARC 的写回 retain）就崩。失败只看返回值、计数（g_par_ser_failed），这一批下次再写
        for (NSArray *it in todo) { id desc = it[1];
            BOOL one = a && ([desc isKindOfClass:MTLRenderPipelineDescriptor.class] ? [a addRenderPipelineFunctionsWithDescriptor:desc error:nil] : [desc isKindOfClass:MTLMeshRenderPipelineDescriptor.class] ? [a addMeshRenderPipelineFunctionsWithDescriptor:desc error:nil] : [a addComputePipelineFunctionsWithDescriptor:desc error:nil]);
            if (one) [keys addObject:it[0]]; else atomic_fetch_add(&g_par_write_failed, 1); }
        if (keys.count && ![a serializeToURL:[NSURL fileURLWithPath:temp] error:nil]) atomic_fetch_add(&g_par_ser_failed, 1);
        else if (keys.count) ok = rename(temp.fileSystemRepresentation, path.fileSystemRepresentation) == 0;
    }
    par_ga_remove_new(before);
    if (ok) {   // the index after the file: a line always names a file that is there
        NSMutableString *lines = [NSMutableString string]; for (NSString *k in keys) [lines appendFormat:@"%@\t%@\n", k, name];
        FILE *f = fopen([g_par_dir stringByAppendingPathComponent:@"batches.tsv"].fileSystemRepresentation, "a");
        ok = f && fputs(lines.UTF8String, f) >= 0; if (f) ok = (fclose(f) == 0) && ok;
    }
    par_ga_unlock(lk); pthread_mutex_unlock(&g_par_wmu);
    if (!ok) { unlink(temp.fileSystemRepresentation);
               if (atomic_fetch_add(&g_par_write_failed, 1) < 4) MLOG("【红】 pipeline archive: cannot write the batch %s (%lu pipelines): %s (serializations failed so far %llu)", name.UTF8String, (unsigned long)keys.count, e ? e.localizedDescription.UTF8String : "serializing failed (Metal's error not read), or rename / index failed", (unsigned long long)atomic_load(&g_par_ser_failed)); return; }
    pthread_mutex_lock(&g_par_mu); for (NSString *k in keys) { g_par_bmap[k] = name; [g_par_have addObject:k]; } pthread_mutex_unlock(&g_par_mu);
    atomic_fetch_add(&g_par_written, keys.count);
}
static int g_par_writer;                                                 // the writer is running (g_par_mu)
static uint64_t g_par_w_t0, g_par_w_n0;
static void par_writer(void) { @autoreleasepool {
    for (;;) @autoreleasepool {
        pthread_mutex_lock(&g_par_mu);
        NSUInteger n = MIN(g_par_todo.count, (NSUInteger)PAR_BATCH); NSArray *batch = n ? [g_par_todo subarrayWithRange:NSMakeRange(0, n)] : nil;
        if (n && !atomic_load(&g_par_closing)) [g_par_todo removeObjectsInRange:NSMakeRange(0, n)]; else { batch = nil; g_par_writer = 0; }
        pthread_mutex_unlock(&g_par_mu);
        if (!batch) break;
        par_write_batch(batch);   // whatever is noted when the writer comes round, up to PAR_BATCH a file
    }
} }
static void par_note(NSString *key, id desc) {
    if (g_par_nowrite || atomic_load(&g_par_closing)) return;   // #756：上次死在写归档里，这个系统版本不再写；#618：在收局
    pthread_mutex_lock(&g_par_mu);
    if (!g_par_todo) g_par_todo = [NSMutableArray array];
    [g_par_todo addObject:@[key, desc]];
    int start = !g_par_writer; g_par_writer = 1;
    if (start && !g_par_w_t0) { g_par_w_t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_par_w_n0 = atomic_load(&g_par_written); }
    pthread_mutex_unlock(&g_par_mu);
    if (start) dispatch_async(s2a_qos_global(S2A_QOS_IO_BG), ^{ par_writer(); });
}
// #618：进程正常收局（runtime/src/lifecycle.c s2a_host_shutdown 一开头经 dlsym 调，两个 _exit(0) 出口之前）：不再起新的一批写。
//   正在写的那一批不等（收局有 1.5 s 兜底，丢一批下次再写），它随 _exit 结束；不留任何标记，下一局照常写
void s2a_pso_archive_exit(void);
void s2a_pso_archive_exit(void) {
    pthread_mutex_lock(&g_par_mkmu);
    atomic_store(&g_par_closing, 1);
    pthread_mutex_unlock(&g_par_mkmu);
}
// After the prewarm: how far the writer is (it goes on until what the prewarm noted is written)
static void par_flush_bg(void) {
    pthread_mutex_lock(&g_par_mu); unsigned long left = (unsigned long)g_par_todo.count; uint64_t t0 = g_par_w_t0, n0 = g_par_w_n0; pthread_mutex_unlock(&g_par_mu);
    if (!t0) return;
    MLOG("pipeline archive: %llu pipelines the prewarm compiled written so far in %.1f s beside it, %lu still to write (Metal build folders removed so far %llu)",
         (unsigned long long)(atomic_load(&g_par_written) - n0), (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9, left, (unsigned long long)atomic_load(&g_par_ga_removed));
}
static _Atomic uint64_t g_par_dedup, g_par_dedup_ns;   // 等了别人在编的同一条：次数、总等待
// 只读计数（单测按事件判、不量墙钟，t_nvn_pso_inflight）：进入「等别人在编的同键」的次数；编完时慢编译定时器还没到的次数。不改行为
static _Atomic uint64_t g_par_wait_enter, g_par_slow_fast;
static void pb_footprint_line(uint64_t built, uint64_t hit, uint64_t compiled);   // nvn_pso_budget.inc：每 256 条一行常驻条数与占用（D-EH7W）
static void par_count(int hit) {
    uint64_t h = hit ? atomic_fetch_add(&g_par_hits, 1) + 1 : atomic_load(&g_par_hits), m = hit ? atomic_load(&g_par_misses) : atomic_fetch_add(&g_par_misses, 1) + 1;
    if (!((h + m) & 255)) pb_footprint_line(h + m, h, m);
    if (!((h + m) & (h + m - 1)) && h + m >= 256) MLOG("pipeline archive: %llu builds, %llu from the archive, %llu compiled; %llu written this run, %llu stale files rewritten; %llu waited for the same key another thread was building (%.1f ms in all)",
                                                      (unsigned long long)(h + m), (unsigned long long)h, (unsigned long long)m, (unsigned long long)atomic_load(&g_par_written), (unsigned long long)atomic_load(&g_par_stale),
                                                      (unsigned long long)atomic_load(&g_par_dedup), atomic_load(&g_par_dedup_ns) / 1e6);
}
// 在途去重（11-05 能耗线）：同一归档键同时只编一次。预热工人（warm_task，utility QoS）、提交线程（pso_get_ 首遇）、并行预编都经 par_render / par_compute；
//   原先各编各的：提交线程急用的那条若正被预热工人编着，它自己再编一遍，两份一起排编译服务（Switch2 线时之笛读数：与提交线程重复建同一条；
//   斯普拉遁3 c1104_r2 第 232 帧单条首用等待 36.7 s）。现在后到的等先到的那份、把自己的 QoS 借给它（同 flight_wait_locked），拿同一个管线对象（Metal 管线对象线程安全）；
//   先到的失败（nil）时后到的自己再编一次，取它自己的错误信息
@interface ParFlight : NSObject { @public dispatch_group_t done; id result; pthread_t builder; _Atomic int gave_up; } @end
@implementation ParFlight @end
static NSMutableDictionary<NSString *, ParFlight *> *g_par_flights;
// 调用方等不及先走了（par_compute_wait 超时）、之后才编完的管线：下一次同键请求直接取走（#884）
static NSMutableDictionary<NSString *, id> *g_par_late;
static os_unfair_lock g_par_fl_lock = OS_UNFAIR_LOCK_INIT;
// 进：有人在编同一键 → 等它编完返回 1、*out = 它的结果；否则登记本线程为编者、*mine = 本次的在途记录，返回 0。
//   wait_ns < 0 = 一直等（原行为）；≥ 0 = 至多等这么久：等不到（或这一键早有人等超时过）返回 1、*out = nil、*gave_up = 1，在途记录留着、编者照常编完（#884）
static int par_fl_enter_wait(NSString *key, id *out, ParFlight **mine, int64_t wait_ns, int *gave_up) {
    *mine = nil; *out = nil; if (gave_up) *gave_up = 0;
    if (!key) return 0;
    os_unfair_lock_lock(&g_par_fl_lock);
    id late = g_par_late[key];
    if (late) { [g_par_late removeObjectForKey:key]; os_unfair_lock_unlock(&g_par_fl_lock); *out = late; return 1; }
    ParFlight *f = g_par_flights[key];
    if (f) {
        if (wait_ns >= 0 && atomic_load(&f->gave_up)) { os_unfair_lock_unlock(&g_par_fl_lock); if (gave_up) *gave_up = 1; return 1; }   // 已有人等超时：不再每次都等满
        pthread_override_t ov = f->builder && !pthread_equal(f->builder, pthread_self()) ? pthread_override_qos_class_start_np(f->builder, qos_class_self(), 0) : NULL;
        os_unfair_lock_unlock(&g_par_fl_lock);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); atomic_fetch_add(&g_par_wait_enter, 1);
        long w = dispatch_group_wait(f->done, wait_ns < 0 ? DISPATCH_TIME_FOREVER : dispatch_time(DISPATCH_TIME_NOW, wait_ns));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (ov) pthread_override_qos_class_end_np(ov);
        atomic_fetch_add(&g_par_dedup, 1); atomic_fetch_add(&g_par_dedup_ns, (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec)));
        if (w) { atomic_store(&f->gave_up, 1); if (gave_up) *gave_up = 1; return 1; }
        *out = f->result; return 1;
    }
    f = [ParFlight new]; f->done = dispatch_group_create(); dispatch_group_enter(f->done); f->builder = pthread_self();
    if (!g_par_flights) g_par_flights = [NSMutableDictionary dictionary];
    g_par_flights[key] = f; *mine = f;
    os_unfair_lock_unlock(&g_par_fl_lock);
    return 0;
}
static int par_fl_enter(NSString *key, id *out, ParFlight **mine) { return par_fl_enter_wait(key, out, mine, -1, NULL); }
static void par_fl_leave(NSString *key, ParFlight *mine, id result) {   // 编者编完：登记结果、摘掉在途、放行等它的；有人等超时先走了的，结果留给下一次同键请求
    if (!mine) return;
    os_unfair_lock_lock(&g_par_fl_lock); mine->result = result; [g_par_flights removeObjectForKey:key];
    if (result && atomic_load(&mine->gave_up)) { if (!g_par_late) g_par_late = [NSMutableDictionary dictionary]; g_par_late[key] = result; }
    os_unfair_lock_unlock(&g_par_fl_lock);
    dispatch_group_leave(mine->done);
}
static id<MTLRenderPipelineState> par_render_(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e);
static id<MTLComputePipelineState> par_compute_(id<MTLFunction> fn, NSString *key, NSError **e);
// A pipeline build that is still running after PAR_SLOW_MS says so, with its archive key, its functions (s<MSL digest>_<entry>: the package maps the digest to the shader key) and the building thread (11-05 Switch 2 Ocarina of Time, test device 2: the submit
//   thread stood inside a compute pipeline build for over 30 s on the first gameplay frame, and nothing in the log named the shader, because a build is logged only when it
//   returns). The note fires from a timer while the build still runs; a build that took longer than PAR_SLOW_MS also logs its time when it returns, and whether it built or failed (11-05: a compile the system killed at its 150 MB limit returned nil after 8.4 s and read as built). Nothing else changes.
enum { PAR_SLOW_MS = 5000 };
static int g_par_slow_ms = PAR_SLOW_MS;   // 单测改短
// #065090：慢编译那一行带上 MSL 摘要（谁的源、行数、哈希、main0 体多长、大计算核拆分 DshOutline 拆没拆，见 msl_note），回落翻译的还把源文件留一份
//   <键>.slow.metal 在兜底目录（诊断包随 .inflight / .fail 一起带走，ios/diag.m）：发版后同类停帧还在，就能当场判拆分是没覆盖到还是拆了也慢。
//   调用方在本线程建管线前设 t_par_msl_note / t_par_msl_src（par_slow_begin 同步拷走），建完清空
static __thread const char *t_par_msl_note, *t_par_msl_src;
enum { PAR_SLOW_KEEP_MAX = 4 };   // 一局至多留几份源
static void par_slow_keep_src(const char *src) {
    static _Atomic int n;
    size_t L = strlen(src); if (L < 7 || strcmp(src + L - 6, ".metal") || atomic_fetch_add(&n, 1) >= PAR_SLOW_KEEP_MAX) return;
    NSString *s = @(src), *d = [[s stringByDeletingPathExtension] stringByAppendingString:@".slow.metal"];
    [NSFileManager.defaultManager removeItemAtPath:d error:nil];
    NSError *e = nil; BOOL ok = [NSFileManager.defaultManager copyItemAtPath:s toPath:d error:&e];
    MLOG("%s MSL source %s kept as %s for the diagnostic package%s%s", ok ? "slow pipeline:" : "【红】 slow pipeline:", s.lastPathComponent.UTF8String, d.lastPathComponent.UTF8String, ok ? "" : ": ", ok ? "" : e.localizedDescription.UTF8String);
}
typedef struct { _Atomic int done; char key[96]; char kind[8]; char thread[32]; char fns[160]; char note[224]; char src[1024]; uint64_t t0; } ParSlow;
static ParSlow *par_slow_begin(NSString *key, const char *kind, NSString *fns) {
    ParSlow *w = calloc(1, sizeof *w); if (!w) return NULL;
    snprintf(w->key, sizeof w->key, "%s", key ? key.UTF8String : "-"); snprintf(w->kind, sizeof w->kind, "%s", kind); snprintf(w->fns, sizeof w->fns, "%s", fns ? fns.UTF8String : "-");
    snprintf(w->note, sizeof w->note, "%s", t_par_msl_note ? t_par_msl_note : "unknown"); snprintf(w->src, sizeof w->src, "%s", t_par_msl_src ? t_par_msl_src : "");
    pthread_getname_np(pthread_self(), w->thread, sizeof w->thread); w->t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    atomic_store(&w->done, 0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)g_par_slow_ms * 1000000), s2a_qos_global(S2A_QOS_IO_BG), ^{
        if (atomic_exchange(&w->done, 2) == 1) { free(w); return; }   // the build returned first: its end freed nothing and left the memory to this block
        MLOG("【红】 %s pipeline %s (functions %s) still compiling after %d ms on thread %s; MSL: %s", w->kind, w->key, w->fns, g_par_slow_ms, w->thread[0] ? w->thread : "?", w->note);
        if (w->src[0]) par_slow_keep_src(w->src);   // 编完先返回就不进这里：只留真慢的
    });
    return w;
}
static void par_slow_end(ParSlow *w, int ok) {
    if (!w) return;
    double ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w->t0) / 1e6;
    if (ms >= g_par_slow_ms) MLOG("%s%s pipeline %s (functions %s) %s after %.0f ms on thread %s", ok ? "" : "【红】 ", w->kind, w->key, w->fns, ok ? "built" : "FAILED", ms, w->thread[0] ? w->thread : "?");
    if (atomic_exchange(&w->done, 1) == 2) free(w);   // the timer already ran: the memory is the end's to free
    else atomic_fetch_add(&g_par_slow_fast, 1);
}
// Build a render pipeline through the archive. key = par_rkey(...), nil = not archived (built as before)
static id<MTLRenderPipelineState> par_render(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e) {
    id got; ParFlight *mine;
    if (par_fl_enter(key, &got, &mine) && got) return got;
    ParSlow *sw = par_slow_begin(key, "render", [NSString stringWithFormat:@"%@ %@", d.vertexFunction.name ?: @"-", d.fragmentFunction.name ?: @"-"]);
    id<MTLRenderPipelineState> ps = par_render_(d, key, e);
    par_slow_end(sw, ps != nil);
    par_fl_leave(key, mine, ps);
    return ps;
}
static id<MTLRenderPipelineState> par_mesh_(MTLMeshRenderPipelineDescriptor *m, NSString *key, NSError **e) {
    id<MTLBinaryArchive> b = par_bundle_for(key);
    if (b) {
        m.binaryArchives = @[b];
        id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        m.binaryArchives = nil;
        if (ps) { par_count(1); return ps; }
    }
    id<MTLBinaryArchive> a = par_archive_for(key);
    if (a) {
        m.binaryArchives = @[a];
        id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        m.binaryArchives = nil;
        if (ps) { par_count(1); return ps; }
        par_drop(key);
    }
    if (key && g_par_on) par_count(0);
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:e];
    if (ps && key && g_par_on) { if (g_par_harvest) par_write(key, m); else par_note(key, m); }
    return ps;
}
// Build a task / mesh pipeline through the archive. key = par_mkey(...), nil = not archived.
static id<MTLRenderPipelineState> par_mesh(MTLMeshRenderPipelineDescriptor *m, NSString *key, NSError **e) {
    id got; ParFlight *mine;
    if (par_fl_enter(key, &got, &mine) && got) return got;
    ParSlow *sw = par_slow_begin(key, "mesh", [NSString stringWithFormat:@"%@ %@ %@", m.objectFunction.name ?: @"-", m.meshFunction.name ?: @"-", m.fragmentFunction.name ?: @"-"]);
    id<MTLRenderPipelineState> ps = par_mesh_(m, key, e);
    par_slow_end(sw, ps != nil);
    par_fl_leave(key, mine, ps);
    return ps;
}
// 重译抢跑（par_compute_wait）：调用方在本线程设 t_par_race（只用一次、取走即清）；块在后台跑、返回替代管线或 nil（做不了）
typedef id<MTLComputePipelineState> (^ParRace)(void);
static __thread __unsafe_unretained ParRace t_par_race;   // 调用方的强引用撑着这次调用
static int64_t g_par_race_wait_ns = 60ll * 1000000000;   // 抢跑在途时的等待上限（单测改短）
static _Atomic uint64_t g_par_race_n, g_par_race_won, g_par_race_lost;   // 起了几次抢跑、抢跑先好、原管线先好
@interface ParWait : NSObject { @public dispatch_semaphore_t sem; int odone, rstate; id<MTLComputePipelineState> ores, rres; NSError *oerr; } @end   // rstate：0 没起 / 1 在跑 / 2 好了 / 3 做不了 / 4 已处理
@implementation ParWait @end
// 建计算管线的实现（单测换成不返回 / 晚返回的桩，#884）
static id<MTLComputePipelineState> (*g_par_compute_impl)(id<MTLFunction>, NSString *, NSError **) = par_compute_;
// 至多等 wait_ns 建一条计算管线（< 0 = 一直等，原行为）。#884：Metal 编译服务对一条计算管线现编一直不返回，提交线程跟着卡死、开场剧情固定定格。
//   有界时在后台队列编、调用方等到上限就返回 nil 且 *gave_up = 1（调用方跳过这一次、不记失败）；后台照常编完，结果登记给下一次同键请求（g_par_late）。
//   同键已经有人等超时过：后到的不再等满、直接 *gave_up = 1
static id<MTLComputePipelineState> par_compute_wait(id<MTLFunction> fn, NSString *key, NSError **e, int64_t wait_ns, int *gave_up) {
    if (gave_up) *gave_up = 0;
    if (!fn) return nil;
    // 在途键：有界时没有归档键也要登记在途（函数对象地址），否则卡住的编译每次派发都再起一份、越堆越多；交给 Metal 的归档键照旧是 key
    NSString *fk = key ?: (wait_ns >= 0 ? [NSString stringWithFormat:@"fn:%p", (__bridge void *)fn] : nil);
    id got; ParFlight *mine; int gu = 0;
    if (par_fl_enter_wait(fk, &got, &mine, wait_ns, &gu)) {
        if (got) return got;
        if (gu) { if (gave_up) *gave_up = 1; return nil; }
    }
    ParSlow *sw = par_slow_begin(key ? key : fn.name, "compute", fn.name);
    if (wait_ns < 0) {
        id<MTLComputePipelineState> ps = g_par_compute_impl(fn, key, e);
        par_slow_end(sw, ps != nil);
        par_fl_leave(fk, mine, ps);
        return ps;
    }
    // 重译抢跑（v0.4.1，#065090）：调用方给了 t_par_race（出包库是旧翻译器出的计算着色器）→ 编满 g_par_slow_ms 还没完就在后台起一份重译管线，谁先好用谁，执行的就是这一次派发
    //   （跳过这一次救不回来：诊断 D-EVQ3 卡住那支 20.5 秒后自己编出，之后 18 秒游戏仍不出帧——它在等被跳过那次派发的结果）。抢跑在途时上限放宽到 g_par_race_wait_ns，
    //   抢跑做不了（返回 nil）上限回到 wait_ns；两边都没好到上限才跳过（#884 的老行为）
    ParWait *w = [ParWait new]; w->sem = dispatch_semaphore_create(0);
    ParFlight *m2 = mine; NSString *k2 = key, *fk2 = fk; id<MTLFunction> f2 = fn;
    dispatch_async(dispatch_get_global_queue(qos_class_self(), 0), ^{
        NSError *ee = nil; id<MTLComputePipelineState> r = g_par_compute_impl(f2, k2, &ee);
        par_slow_end(sw, r != nil);
        w->ores = r; w->oerr = ee;
        par_fl_leave(fk2, m2, r);
        __atomic_store_n(&w->odone, 1, __ATOMIC_RELEASE); dispatch_semaphore_signal(w->sem);
    });
    ParRace race = t_par_race;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); int64_t cap = wait_ns, race_at = race ? (int64_t)g_par_slow_ms * 1000000 : -1;
    for (;;) {
        int64_t el = (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0), until = race_at >= 0 && race_at < cap ? race_at : cap;
        long to = until > el ? dispatch_semaphore_wait(w->sem, dispatch_time(DISPATCH_TIME_NOW, until - el)) : 1;
        if (__atomic_load_n(&w->odone, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&w->rstate, __ATOMIC_ACQUIRE) == 1) atomic_fetch_add(&g_par_race_lost, 1);
            if (e) *e = w->oerr;
            return w->ores;
        }
        int rs = __atomic_load_n(&w->rstate, __ATOMIC_ACQUIRE);
        if (rs == 2) {   // 原管线还在编：在途记录照「等超时过」处理，同键后到的有界请求不再等满（结果仍登记给下一次，g_par_late）
            if (m2) { os_unfair_lock_lock(&g_par_fl_lock); if (!m2->result) atomic_store(&m2->gave_up, 1); os_unfair_lock_unlock(&g_par_fl_lock); }
            atomic_fetch_add(&g_par_race_won, 1); return w->rres;
        }
        if (rs == 3) { __atomic_store_n(&w->rstate, 4, __ATOMIC_RELEASE); cap = wait_ns; continue; }   // 抢跑做不了：回到原上限（已过就立刻跳过）
        if (!to) continue;   // 被信号叫醒但两边都没出结果（抢跑失败的那次已在上面处理）
        el = (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
        if (race_at >= 0 && el >= race_at) {
            race_at = -1; __atomic_store_n(&w->rstate, 1, __ATOMIC_RELEASE); cap = wait_ns > g_par_race_wait_ns ? wait_ns : g_par_race_wait_ns; atomic_fetch_add(&g_par_race_n, 1);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
                id<MTLComputePipelineState> r = race(); w->rres = r;
                __atomic_store_n(&w->rstate, r ? 2 : 3, __ATOMIC_RELEASE); dispatch_semaphore_signal(w->sem);
            } });
            continue;
        }
        if (el < cap) continue;
        if (m2) { os_unfair_lock_lock(&g_par_fl_lock); if (!m2->result) atomic_store(&m2->gave_up, 1); os_unfair_lock_unlock(&g_par_fl_lock); }
        if (gave_up) *gave_up = 1;
        return nil;
    }
}
static id<MTLComputePipelineState> par_compute(id<MTLFunction> fn, NSString *key, NSError **e) { return par_compute_wait(fn, key, e, -1, NULL); }
static id<MTLRenderPipelineState> par_render_(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e) {
    id<MTLBinaryArchive> b = par_bundle_for(key);
    if (b) {   // a miss (another OS / GPU than the package was made for) is no error: the device's archive and the compiler follow
        d.binaryArchives = @[b];
        id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        d.binaryArchives = nil;
        if (ps) { par_count(1); return ps; }
    }
    id<MTLBinaryArchive> a = par_archive_for(key);
    if (a) {
        d.binaryArchives = @[a];
        id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        d.binaryArchives = nil;
        if (ps) { par_count(1); return ps; }
        par_drop(key);
    }
    if (key && g_par_on) par_count(0);
    atomic_fetch_add(&g_par_compiled, 1);
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:e];
    if (ps && key && g_par_on && par_write_in_place) { if (g_par_harvest) par_write(key, d); else par_note(key, d); }
    return ps;
}
static id<MTLComputePipelineState> par_compute_(id<MTLFunction> fn, NSString *key, NSError **e) {
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = fn;
    id<MTLBinaryArchive> b = par_bundle_for(key);
    if (b) {
        d.binaryArchives = @[b];
        id<MTLComputePipelineState> ps = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        d.binaryArchives = nil;
        if (ps) { par_count(1); return ps; }
    }
    id<MTLBinaryArchive> a = par_archive_for(key);
    if (a) {
        d.binaryArchives = @[a];
        id<MTLComputePipelineState> ps = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        d.binaryArchives = nil;
        if (ps) { par_count(1); return ps; }
        par_drop(key);
    }
    if (key && g_par_on) par_count(0);
    atomic_fetch_add(&g_par_compiled, 1);
    id<MTLComputePipelineState> ps = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:e];
    if (ps && key && g_par_on && par_write_in_place) { if (g_par_harvest) par_write(key, d); else par_note(key, d); }
    return ps;
}
