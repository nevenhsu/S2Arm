// 管线归档身份要设备（pso_archive.h par_open_dir）：0.6.10 iOS 快照恢复局在 mtl_init 之前就建着色器、起预热，开归档时 g_dev 还是空，
//   身份里 GPU 名写成「?」→ 和存着的不符整档删掉、再写「?」，下一局设备在时又判「变了」再删，恢复局与其后普通局交替清空、每次全部重编。
//   直接包含生产 nvn_mtl.m，归档目录放临时目录（g_par_harvest 指过去），里面先放「这台设备的身份 + 一个归档文件 k1」。期望（手写，不跑被测逻辑推）：
//   ① 设备没建好时开归档：k1 文件还在、identity.json 字节不变、归档不算打开（g_par_on 0）
//   ② 同一进程设备建好后再开：k1 还在、身份不变、归档打开且认得 k1
//   ③ 下一次起局（另起子进程，一开始就有设备）：k1 还在、认得 k1——连续两次起局归档被复用
#include "../gfx/nvn/nvn_mtl.m"
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL setup %s:%d: %s\n", __FILE__, __LINE__, #c); exit(2); } } while (0)
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) failures++; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

static NSString *read_text(NSString *dir, NSString *name) { return [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil]; }
static BOOL has(NSString *dir, NSString *name) { return [NSFileManager.defaultManager fileExistsAtPath:[dir stringByAppendingPathComponent:name]]; }

int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); REQUIRE(dev);
    if (argc == 3 && !strcmp(argv[1], "--next-run")) {   // ③ 子进程：下一次起局，一开始就有设备
        g_dev = dev; g_par_harvest = argv[2]; par_open_dir();
        NSString *dir = @(argv[2]);
        return has(dir, @"k1.metalarchive") && g_par_on && [g_par_have containsObject:@"k1"] ? 0 : 1;
    }
    char tmpl[] = "/tmp/t_pso_identity.XXXXXX"; REQUIRE(mkdtemp(tmpl));
    NSString *dir = @(tmpl);
    NSString *want = [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:par_identity(dev) options:NSJSONWritingSortedKeys error:nil] encoding:NSUTF8StringEncoding];
    REQUIRE([want writeToFile:[dir stringByAppendingPathComponent:@"identity.json"] atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    REQUIRE([[NSData dataWithBytes:"k1" length:2] writeToFile:[dir stringByAppendingPathComponent:@"k1.metalarchive"] atomically:YES]);
    g_par_harvest = tmpl;

    g_dev = nil; par_open_dir();
    VERIFY(has(dir, @"k1.metalarchive") && [read_text(dir, @"identity.json") isEqualToString:want] && !g_par_on,
           "① 设备没建好时开归档：期望 k1 在、身份不变、未打开 → 真结果 k1 %s、身份 %s、打开 %d",
           has(dir, @"k1.metalarchive") ? "在" : "被删", [read_text(dir, @"identity.json") isEqualToString:want] ? "不变" : (read_text(dir, @"identity.json") ?: @"(没有)").UTF8String, g_par_on);

    g_dev = dev; par_open_dir();
    VERIFY(has(dir, @"k1.metalarchive") && [read_text(dir, @"identity.json") isEqualToString:want] && g_par_on && [g_par_have containsObject:@"k1"],
           "② 设备建好后再开：期望 k1 在、身份不变、打开且认得 k1 → 真结果 k1 %s、身份 %s、打开 %d、认得 %d",
           has(dir, @"k1.metalarchive") ? "在" : "被删", [read_text(dir, @"identity.json") isEqualToString:want] ? "不变" : "变了", g_par_on, (int)[g_par_have containsObject:@"k1"]);

    pid_t pid; char *cargv[] = {argv[0], "--next-run", tmpl, NULL}; int st = -1;
    REQUIRE(posix_spawn(&pid, argv[0], NULL, NULL, cargv, environ) == 0); REQUIRE(waitpid(pid, &st, 0) == pid);
    VERIFY(WIFEXITED(st) && WEXITSTATUS(st) == 0, "③ 下一次起局（子进程，有设备）：期望 归档被复用（k1 在且认得）→ 真结果 子进程退出 %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);

    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    printf("%s t_nvn_pso_identity：%u 项，%u 项不符\n", failures ? "✗" : "✓", checks, failures);
    return failures ? 1 : 0;
} }
