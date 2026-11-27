// 上屏拷贝只在画进画布那次套补丁画幅（#1169：自适应满屏 + SMAA 中 / 高左右留黑、画面挤窄）。含生产 nvn_mtl.m 源码（同 t_fi_link.m 写法）+ 真 Metal 设备。
//   补丁画幅从 runtime/src/cpatch.c 来：装一份画幅 2.0 的定画幅变体（同 t_cpatch.c ⑧ 写法），换到画幅 2.0 → s2a_cpatch_present_aspect() = 2.0。
//   期望按语义手写（源图 1920×1080 全白）：
//   ① 拷进 16:9 的中间图（抗锯齿 / 放大器的输入，screen = 0）：原样拷满，顶上第 10 行是白的。
//   ② 对照——中间图也套画幅（旧实现每次都套，screen = 1）：画幅 2.0 塞进 1920×1080 → 视口 1920×960、上下各 60 行黑，第 10 行是黑的；最后拉满时黑边留下 = 挤窄
//   ③ 画进 2160×1080 的画布（画幅 2.0，screen = 1）：铺满，最左一列是白的
#ifndef PRESENT_ASPECT_SOURCE
#define PRESENT_ASPECT_SOURCE "../gfx/nvn/nvn_mtl.m"
#endif
#include PRESENT_ASPECT_SOURCE
#include <sys/mman.h>

static unsigned checks, failures;
#define CHECK(c, ...) do { checks++; int ok=!!(c); failures+=!ok; printf("%s ",ok?"PASS":"FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while(0)
void s2a_cpatch_load(module_t *m, const char *dir); void s2a_cpatch_aspect(double A);

static id<MTLTexture> tex(NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
    return [g_dev newTextureWithDescriptor:d];
}
static uint32_t px(id<MTLTexture> t, NSUInteger x, NSUInteger y) { uint32_t v = 0; [t getBytes:&v bytesPerRow:4 fromRegion:MTLRegionMake2D(x, y, 1, 1) mipmapLevel:0]; return v; }
static id<MTLTexture> run(id<MTLTexture> src, NSUInteger w, NSUInteger h, int screen) {
    id<MTLTexture> dst = tex(w, h); id<MTLCommandBuffer> cb = [g_q commandBuffer];
    present_blit(cb, src, dst, NULL, NO, screen); [cb commit]; [cb waitUntilCompleted]; return dst;
}

int main(void) { @autoreleasepool {
    setenv("DM_PSO_MANIFEST","off",1); setenv("DM_PSO_ARCHIVE","0",1); unsetenv("S2A_CPATCH_ON");
    char dir[] = "/tmp/t_present_aspect.XXXXXX"; if (!mkdtemp(dir)) return 2;
    char p[256]; snprintf(p, sizeof p, "%s/cpatch.tsv", dir); FILE *f = fopen(p, "w");
    fprintf(f, "编号\t模块\t位\t类别\t段名\t许可\t来源\t数据写入\t缺省开\t目标帧率\t画幅\n");
    fprintf(f, "widescreen:V\tmain\t12\twidescreen\t2:1\t\tv.pchtxt\t3200:00:20\t0\t\t2.0000\n"); fclose(f);
    module_t m; memset(&m, 0, sizeof m); snprintf(m.name, sizeof m.name, "%s", "main");
    m.size = 0x8000; m.base = mmap(NULL, m.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); m.key_page_off = 0x4000; m.key_page_size = 0x4000;
    s2a_cpatch_load(&m, dir); s2a_cpatch_aspect(2.0); unlink(p); rmdir(dir);
    if (fabs(s2a_cpatch_present_aspect() - 2.0) > 1e-6) { printf("SETUP FAIL 补丁画幅 %.4f（期望 2.0）\n", s2a_cpatch_present_aspect()); return 2; }
    mtl_init();
    id<MTLTexture> src = tex(1920, 1080); { size_t n = 1920 * 1080; uint32_t *w = malloc(n * 4); for (size_t i = 0; i < n; i++) w[i] = 0xFFFFFFFFu;
        [src replaceRegion:MTLRegionMake2D(0, 0, 1920, 1080) mipmapLevel:0 withBytes:w bytesPerRow:1920 * 4]; free(w); }
    uint32_t a = px(run(src, 1920, 1080, 0), 960, 10), b = px(run(src, 1920, 1080, 1), 960, 10), c = px(run(src, 2160, 1080, 1), 0, 540);
    CHECK((a & 0xFFFFFF) == 0xFFFFFF, "① 中间图不套补丁画幅：第 10 行 %08x（期望白）", a);
    CHECK((b & 0xFFFFFF) == 0, "② 对照：中间图套画幅 2.0（旧实现）→ 第 10 行 %08x（期望黑：上下各 60 行黑边）", b);
    CHECK((c & 0xFFFFFF) == 0xFFFFFF, "③ 画进 2:1 画布：最左一列 %08x（期望白，铺满）", c);
    {
        int ow = -1, oh = -1;
        nvn_be_present_out_size(&ow, &oh);
        CHECK(ow == 0 && oh == 0, "④a 上屏前 nvn_be_present_out_size 为 0x0（实得 %dx%d）", ow, oh);
        id<MTLTexture> p2k = tex(2560, 1440);
        uint64_t ser = mtl_ext_tex((__bridge void *)p2k);
        tex_t pt = { .magic = MAGIC('T', 'E', 'X', 0), .serial = ser };
        present_now(&pt, 0, 1, 1, (const int[4]){0, 0, 1920, 1080}, nil, 0);
        nvn_be_present_out_size(&ow, &oh);
        CHECK(ow == 1920 && oh == 1080, "④b 1920x1080 裁切上屏后 nvn_be_present_out_size = 1920x1080（实得 %dx%d）", ow, oh);
        present_now(&pt, 0, 2, 2, (const int[4]){0, 0, 2048, 1152}, nil, 0);
        nvn_be_present_out_size(&ow, &oh);
        CHECK(ow == 2048 && oh == 1152, "④c 2048x1152 裁切上屏后 nvn_be_present_out_size = 2048x1152（实得 %dx%d）", ow, oh);
        present_now(&pt, 0, 3, 3, NULL, nil, 0);
        nvn_be_present_out_size(&ow, &oh);
        CHECK(ow == 2560 && oh == 1440, "④d 无裁切上屏后 nvn_be_present_out_size = 2560x1440（实得 %dx%d）", ow, oh);
        nvn_be_present_shutdown();
        nvn_be_present_out_size(&ow, &oh);
        CHECK(ow == 0 && oh == 0, "④e nvn_be_present_shutdown 后归零为 0x0（实得 %dx%d）", ow, oh);
    }
    printf("%s t_present_aspect：%u 项，%u 项不符\n", failures ? "✗" : "✓", checks, failures);
    return failures != 0;
} }
