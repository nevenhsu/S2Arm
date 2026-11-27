// 按阶段绑定沿用的旧程序被游戏释放后再换绑另一阶段（暗黑3 10-26 冷局 keys_hash 崩溃）：直接包含生产 nvn_mtl.m 调 bind_stages。
// 期望值不跑被测实现得来：顶点阶段取绑定时那份程序的着色器（夹具里登记的 VS），片元取新程序的（FS）；旧程序对象被改成崩溃现场的野值后不能再被读。
// 反证：子进程照旧实现那样对失效对象调 prog_get，必须死于信号（证明这个夹具真能复现崩溃，判据不会绿着失效）。
#include "../gfx/nvn/nvn_mtl.m"
#include <sys/wait.h>
#include <spawn.h>
#include <mach-o/dyld.h>
extern char **environ;

static unsigned failures, checks;
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) failures++; \
    printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

static NvShader *named(const char *key, int stage) { NvShader *s = [NvShader new]; s->stage = stage; strlcpy(s->key, key, sizeof s->key); return s; }
static void put(prog_t *p, uint64_t ser, NvShader *vs, NvShader *fs) {
    memset(p, 0, sizeof *p); p->magic = MAGIC('P', 'R', 'O', 'G'); p->nstage = 2; p->serial = ser;
    NvProg *o = [NvProg new]; o->sh[0] = vs; o->sh[1] = fs; serial_put(ser, o);
}
// 单阶段程序（太鼓 10-30：每个程序只含一个阶段）：smask = 这个程序自己的阶段位（prog_get 按映射表登记），unres = 有键没查到阶段
static void put1(prog_t *p, uint64_t ser, NvShader *sh, int st, uint8_t smask, uint8_t unres) {
    memset(p, 0, sizeof *p); p->magic = MAGIC('P', 'R', 'O', 'G'); p->nstage = 1; p->serial = ser;
    NvProg *o = [NvProg new]; o->sh[st] = sh; o->smask = smask; o->unres = unres; serial_put(ser, o);
}
static const char *k0(const prog_t *p) { __unsafe_unretained NvProg *c = p ? serial_get(p->serial) : nil; return c && c->sh[0] ? c->sh[0]->key : "-"; }
static const char *k1(const prog_t *p) { __unsafe_unretained NvProg *c = p ? serial_get(p->serial) : nil; return c && c->sh[1] ? c->sh[1]->key : "-"; }
static uint8_t junk[0x880];   // 崩溃现场：对象被复用，控制段指针指向一块可读的别的数据（+0x704 读出像样的代码长度），代码区指针是 0x20200ffffffff（x1 / x5）
static void wreck(prog_t *p) {
    memset(junk, 0x5a, sizeof junk); uint32_t ncode = 0x40; memcpy(junk + 0x704, &ncode, 4);
    p->magic = 0x2020; p->serial = 0x00020200ffffffffull; p->nstage = 1; p->sd[0].control = junk; p->sd[0].data = 0x20200ffffffffull;
}

// 反证的子进程：本程序带 --stale-child 重新启动（posix_spawn = fork + 立刻 exec），在全新进程里搭同一个失效对象、走旧路径 prog_get。
//   原先在本进程里 fork() 后直接调 prog_get：多线程进程 fork 后子进程只该调异步信号安全的函数（POSIX），Metal / 日志初始化后本进程已有别的线程，
//   fork 那一刻谁拿着锁（序号表锁 g_smu、malloc、ObjC 运行时、日志），子进程就永远等那把锁、不占 CPU——11-27 门禁一次「300 秒无进展」挂死就是这个形态
static int stale_child(void) {
    @autoreleasepool {
        static prog_t a; NvShader *vs_a = named("vs_a", 0), *fs_a = named("fs_a", 1);
        put(&a, 9003, vs_a, fs_a); serial_del(9003); wreck(&a);   // 同 ③：a 已从序号表删掉、对象被复用成崩溃现场的野值
        __unsafe_unretained NvProg *o = prog_get(&a);
        _exit(o && o->sh[0] == vs_a ? 1 : 0);
    }
}
static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_hold_cv = PTHREAD_COND_INITIALIZER; static int g_hold_state;
static void *hold_smu(void *arg) {   // 另一条线程拿住序号表锁，直到主线程说放
    (void)arg; pthread_mutex_lock(&g_smu);
    pthread_mutex_lock(&g_hold_mu); g_hold_state = 1; pthread_cond_broadcast(&g_hold_cv);
    while (g_hold_state != 2) pthread_cond_wait(&g_hold_cv, &g_hold_mu);
    pthread_mutex_unlock(&g_hold_mu); pthread_mutex_unlock(&g_smu); return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--stale-child")) return stale_child();
    @autoreleasepool {
        char map[] = "/tmp/t_bindstale_XXXXXX"; int fd = mkstemp(map); if (fd < 0) return 2; close(fd); setenv("DM_NVN_SHMAP", map, 1);   // 空映射表：替身程序没有阶段，不查表
        static prog_t a, b;
        NvShader *vs_a = named("vs_a", 0), *fs_a = named("fs_a", 1), *fs_b = named("fs_b", 1);
        put(&a, 9001, vs_a, fs_a); put(&b, 9002, nil, fs_b);

        // ① 顶点 + 片元一起绑 a，再只换片元成 b：程序都还活着，走组合程序
        state_reset();
        const prog_t *p1 = bind_stages(&a, 0x3);
        VERIFY(p1 == &a, "同一程序绑两阶段直接用它");
        const prog_t *p2 = bind_stages(&b, 0x2);
        __unsafe_unretained NvProg *c2 = p2 ? serial_get(p2->serial) : nil;
        VERIFY(c2 && c2->sh[0] == vs_a && c2->sh[1] == fs_b, "活着时组合：顶点 vs_a、片元 fs_b → %s / %s", c2 && c2->sh[0] ? c2->sh[0]->key : "-", c2 && c2->sh[1] ? c2->sh[1]->key : "-");

        // ② 同样先绑 a 两阶段，然后游戏释放 / 复用 a，再只换片元：顶点仍要 a 绑定时的着色器，且不读 a 的野值
        state_reset();
        bind_stages(&a, 0x3);
        wreck(&a);
        const prog_t *p3 = bind_stages(&b, 0x2);
        __unsafe_unretained NvProg *c3 = p3 ? serial_get(p3->serial) : nil;
        VERIFY(c3 && c3->sh[0] == vs_a && c3->sh[1] == fs_b, "a 失效后组合：顶点按绑定时序号取回 vs_a、片元 fs_b → %s / %s", c3 && c3->sh[0] ? c3->sh[0]->key : "-", c3 && c3->sh[1] ? c3->sh[1]->key : "-");
        VERIFY(S.bst[0] != &a, "顶点阶段不再指着失效的 a");

        // ③ a 被释放（序号表里也删了）后再换绑：顶点阶段为空，不崩
        state_reset();
        put(&a, 9003, vs_a, fs_a);
        bind_stages(&a, 0x3);
        serial_del(9003); wreck(&a);
        const prog_t *p4 = bind_stages(&b, 0x2);
        __unsafe_unretained NvProg *c4 = p4 ? serial_get(p4->serial) : nil;
        VERIFY(c4 && !c4->sh[0] && c4->sh[1] == fs_b, "a 已释放：顶点为空、片元 fs_b，不读野指针");

        // ④ 太鼓：顶点程序、片元程序各只含一个阶段，都按全部图形阶段（0x1f）先后绑：片元程序只替换片元，顶点留着
        static prog_t v, f, fu, v0, f0;
        NvShader *vs_v = named("vs_v", 0), *fs_f = named("fs_f", 1);
        put1(&v, 9101, vs_v, 0, 0x1, 0); put1(&f, 9102, fs_f, 1, 0x2, 0);
        state_reset();
        bind_stages(&v, 0x1f);
        const prog_t *p5 = bind_stages(&f, 0x1f);
        VERIFY(!strcmp(k0(p5), "vs_v") && !strcmp(k1(p5), "fs_f"), "④ 单阶段程序都按 0x1f 绑：顶点 vs_v、片元 fs_f → %s / %s", k0(p5), k1(p5));
        //   反证：同样的绑定，程序不登记自己的阶段（= 旧规则，阶段位里每个阶段都换）→ 顶点被片元程序冲掉
        put1(&v0, 9103, vs_v, 0, 0, 0); put1(&f0, 9104, fs_f, 1, 0, 0);
        state_reset();
        bind_stages(&v0, 0x1f);
        const prog_t *p6 = bind_stages(&f0, 0x1f);
        VERIFY(!strcmp(k0(p6), "-") && !strcmp(k1(p6), "fs_f"), "④ 反证：按旧规则顶点被冲掉 → %s / %s", k0(p6), k1(p6));
        // ⑤ 空程序照旧解绑阶段位里的阶段（银河战士生存恐惧深度预处理的用法）
        state_reset();
        bind_stages(&v, 0x1f); bind_stages(&f, 0x1f);
        bind_stages(NULL, 0x2);
        VERIFY(S.bst[1] == NULL && S.bst[0] == &v, "⑤ 绑空程序到片元：片元解绑、顶点仍是 v");
        // ⑥ 程序里有键没查到阶段（不在映射表）：退回旧规则，缺的那段照样在绘制时报出来，不悄悄沿用旧阶段
        put1(&fu, 9105, fs_f, 1, 0x2, 1);
        state_reset();
        bind_stages(&v, 0x1f);
        bind_stages(&fu, 0x1f);
        VERIFY(S.bst[0] == &fu, "⑥ 有未解析的键：0x1f 全换成这个程序（顶点阶段指向它）");

        // ⑦ 异度3（10-31）：先绑带几何阶段的程序（顶点 + 几何 + 片元），再把只有顶点 + 片元的程序按 0x1f 绑：几何阶段解绑，不残留上一个程序的几何着色器
        static prog_t g, u;
        NvShader *vs_g = named("vs_g", 0), *gs_g = named("gs_g", 2), *fs_g = named("fs_g", 1), *vs_u = named("vs_u", 0), *fs_u = named("fs_u", 1);
        memset(&g, 0, sizeof g); g.magic = MAGIC('P', 'R', 'O', 'G'); g.nstage = 3; g.serial = 9201;
        { NvProg *o = [NvProg new]; o->sh[0] = vs_g; o->sh[1] = fs_g; o->sh[2] = gs_g; o->smask = 0x7; serial_put(9201, o); }
        memset(&u, 0, sizeof u); u.magic = MAGIC('P', 'R', 'O', 'G'); u.nstage = 2; u.serial = 9202;
        { NvProg *o = [NvProg new]; o->sh[0] = vs_u; o->sh[1] = fs_u; o->smask = 0x3; serial_put(9202, o); }
        state_reset();
        bind_stages(&g, 0x1f);
        const prog_t *p7 = bind_stages(&u, 0x1f);
        __unsafe_unretained NvProg *c7 = p7 ? serial_get(p7->serial) : nil;
        VERIFY(c7 && c7->sh[0] == vs_u && c7->sh[1] == fs_u && !c7->sh[2], "⑦ 0x1f 绑只有顶点 + 片元的程序：几何阶段解绑 → 顶点 %s、片元 %s、几何 %s", k0(p7), k1(p7), c7 && c7->sh[2] ? c7->sh[2]->key : "-");
        //   太鼓规则同时成立：只有片元的程序按 0x1f 绑，顶点留着、几何解绑
        state_reset();
        bind_stages(&g, 0x1f);
        const prog_t *p8 = bind_stages(&f, 0x1f);
        __unsafe_unretained NvProg *c8 = p8 ? serial_get(p8->serial) : nil;
        VERIFY(c8 && c8->sh[0] == vs_g && c8->sh[1] == fs_f && !c8->sh[2], "⑦ 只有片元的程序按 0x1f 绑：顶点留 vs_g、片元 fs_f、几何解绑 → %s / %s / %s", k0(p8), k1(p8), c8 && c8->sh[2] ? c8->sh[2]->key : "-");

        // 反证：旧实现在组合时对 S.bst 里的失效对象调 prog_get，取不回绑定时的顶点着色器 vs_a（所以组合必须按绑定时序号取，见 ②）。
        //   原先断言「子进程里必崩」：失效对象的代码区指针是 0x20200ffffffff，旧 prog_get 取键读野地址就崩。11-17 aad3c7407 起取键前先查游戏映射、
        //   读不了就不取键按缺阶段报，不再崩——崩不崩已不是旧路径错不错的判据。现在判结果：子进程里拿到 vs_a 退出 1（反证不成立），
        //   拿不到退出 0；照旧崩了也算旧路径取不回（子进程里做，崩了不连累本进程）
        //   子进程用 posix_spawn 起本程序（见 stale_child），起的那一刻另一条线程正拿着序号表锁 g_smu：全新进程不继承这把锁，照常走完。
        fflush(stdout);
        pthread_t th; pthread_create(&th, NULL, hold_smu, NULL);
        pthread_mutex_lock(&g_hold_mu); while (g_hold_state != 1) pthread_cond_wait(&g_hold_cv, &g_hold_mu); pthread_mutex_unlock(&g_hold_mu);
        //   形态核对（期望手写：fork 复制的是「锁被占着」的内存，子进程里试锁必然拿不到）：同样的时刻 fork，子进程只做 trylock 就退——
        //   EBUSY 说明旧写法的子进程会在 prog_get → serial_get 里永远等这把锁
        pid_t fk = fork();
        if (fk == 0) _exit(pthread_mutex_trylock(&g_smu) == EBUSY ? 3 : 4);
        int fs = 0; waitpid(fk, &fs, 0);
        VERIFY(WIFEXITED(fs) && WEXITSTATUS(fs) == 3, "形态：别的线程拿着序号表锁时 fork，子进程里这把锁是占着的（旧写法会在 prog_get 里永远等它）→ %s",
               WIFEXITED(fs) ? (WEXITSTATUS(fs) == 3 ? "占着" : "空着") : "子进程异常");
        char self[PATH_MAX]; uint32_t sl = sizeof self; pid_t pid = -1; int ws = 0;
        int sr = _NSGetExecutablePath(self, &sl) == 0 ? posix_spawn(&pid, self, NULL, NULL, (char *[]){self, "--stale-child", NULL}, environ) : -1;
        if (sr == 0) waitpid(pid, &ws, 0);
        pthread_mutex_lock(&g_hold_mu); g_hold_state = 2; pthread_cond_broadcast(&g_hold_cv); pthread_mutex_unlock(&g_hold_mu); pthread_join(th, NULL);
        VERIFY(sr == 0 && (WIFSIGNALED(ws) || (WIFEXITED(ws) && WEXITSTATUS(ws) == 0)), "反证：对失效对象 prog_get（旧实现的路径）取不回绑定时的 vs_a（posix_spawn 起的子进程，序号表锁被别的线程拿着也不受影响）→ %s",
               sr ? "起不了子进程" : WIFSIGNALED(ws) ? strsignal(WTERMSIG(ws)) : WEXITSTATUS(ws) == 0 ? "没取回（顶点阶段缺）" : "竟取回了 vs_a");
    }
    unlink(getenv("DM_NVN_SHMAP"));
    if (failures) printf("✗ t_nvn_bindstale：%u 项里 %u 项不过\n", checks, failures);   // 原生门禁按「✓ t_ / ✗ t_」逐组判（runtime/gate.sh）
    else printf("✓ t_nvn_bindstale：%u 项全过（沿用阶段的程序失效后按绑定时序号取着色器；旧路径读失效对象取不回绑定时的着色器）\n", checks);
    return failures ? 1 : 0;
}
