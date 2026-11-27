// 片元深度偏移（nvn_mtl.m fragz_bias_units）：读 gl_FragCoord.z 又取样深度贴图、只测不写的 LESS / LEQUAL 绘制，setDepthBias 往通过方向偏 1 单位，
//   补 Apple GPU 上片元 [[position]].z 比固定功能写进的深度远 1 ULP 的差（零容差自剥离比较 z > 已存深度 把前表面砍掉）。含生产 nvn_mtl.m 源码。期望按语义手写：
//   ① 选择：只有「读 z、取样深度贴图、有深度附件、测深度、不写深度、LESS / LEQUAL」全满足才给 -1；GREATER / GEQUAL（反向 Z）、EQUAL、ALWAYS、写深度、不读 z、
//      不取样深度贴图、没有深度附件都是 0
//   ② 真 Metal（单位换算，上限按「浮点深度的一个单位 = 2^(e-23)，e = 三角形最大深度的指数」）：深度跨度大的三角形（左 z 1e-4，右顶点在屏幕右缘），先按固定功能
//      写一遍深度，再用 ① 给的单位画同一个三角形读 [[position]].z。右顶点 z 0.999 与正好 1.0（指数段翻一倍）各一遍：
//      a. 每个像素往近处移的量 ≥ 1 ULP（补得上 1 ULP 的差：原本 z = 已存 + 1 ULP 的前表面不再被砍）
//      b. 最远端（这一行最右、z 最大）：已存深度比片元近 3 ULP 时偏移后的 z 仍 > 已存——比前表面远 3 ULP 的东西照样判为在后面（维护者要的「远处像素 3 ULP 仍不过」）
//      c. 每个像素移的绝对量 ≤ 一个单位（0.999 → 2^-24，1.0 → 2^-23）。M1 实测不是全三角形同一个量：z 0.5 以下的像素移 2^-25（0.999 那遍），
//         但都不超过这个上限；近端（z 0.002）因此是一百多 ULP，在一般 Z 下那是贴近相机的地方
//   负对照：fragz_bias_units 让 GREATER / GEQUAL 也给 → ① 红；默认改成 2 单位 → 1.0 那遍 ② b 红（最远端移 4 ULP）
#ifndef FRAGZ_BIAS_SOURCE
#define FRAGZ_BIAS_SOURCE "../gfx/nvn/nvn_mtl.m"
#endif
#include FRAGZ_BIAS_SOURCE
#include <math.h>
#include "s2a_t_result.h"

static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "SETUP FAIL %d: %s\n", __LINE__, #c); exit(2); } } while (0)
#define CHECK(c, ...) do { checks++; int ok=!!(c); failures+=!ok; printf("%s ",ok?"PASS":"FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while(0)

static const char *k_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V vs(uint i [[vertex_id]], constant float2 &z [[buffer(0)]]) { float2 P[3] = {float2(-1,-3), float2(-1,3), float2(1,0)}; V o; o.p = float4(P[i], i == 2 ? z.y : z.x, 1); return o; }\n"
    "fragment float fz(V in [[stage_in]]) { return in.p.z; }\n";

static float ulp_up(float x) { return nextafterf(x, INFINITY) - x; }

int main(void) { @autoreleasepool {
    unsetenv("DM_NVN_FRAGZ_BIAS");
    // ① 选择（参数：读 z, 测深度, 写深度, 比较函数, 有深度附件, 取样深度贴图）
    const int L = NVN_DEPTH_FUNC_LEQUAL, LS = NVN_DEPTH_FUNC_LESS, G = NVN_DEPTH_FUNC_GREATER, GE = NVN_DEPTH_FUNC_GEQUAL, EQ = NVN_DEPTH_FUNC_EQUAL, AL = NVN_DEPTH_FUNC_ALWAYS;
    CHECK(fragz_bias_units(1, 1, 0, L, 1, 1, "t") == -1.0f, "① LEQUAL、读 z、取样深度贴图、只测不写 → -1 单位");
    CHECK(fragz_bias_units(1, 1, 0, LS, 1, 1, "t") == -1.0f, "① LESS 同上 → -1 单位");
    CHECK(fragz_bias_units(1, 1, 0, G, 1, 1, "t") == 0, "① GREATER（反向 Z）→ 0");
    CHECK(fragz_bias_units(1, 1, 0, GE, 1, 1, "t") == 0, "① GEQUAL（反向 Z）→ 0");
    CHECK(fragz_bias_units(1, 1, 0, EQ, 1, 1, "t") == 0 && fragz_bias_units(1, 1, 0, AL, 1, 1, "t") == 0, "① EQUAL / ALWAYS → 0");
    CHECK(fragz_bias_units(1, 1, 1, L, 1, 1, "t") == 0, "① 写深度 → 0（偏移会改写进缓冲的内容）");
    CHECK(fragz_bias_units(0, 1, 0, L, 1, 1, "t") == 0, "① 不读 gl_FragCoord.z（只读 .xy 的 frag_coord 不算）→ 0");
    CHECK(fragz_bias_units(1, 1, 0, L, 1, 0, "t") == 0, "① 没取样深度贴图 → 0");
    CHECK(fragz_bias_units(1, 0, 0, L, 1, 1, "t") == 0 && fragz_bias_units(1, 1, 0, L, 0, 1, "t") == 0, "① 不测深度 / 没有深度附件 → 0");
    CHECK(shader_fragz_from_json(@{@"frag_coord": @YES}, 1) == 1, "①b 缺 frag_z 时按 frag_coord 回退 → 1");
    CHECK(shader_fragz_from_json(@{@"frag_coord": @YES, @"frag_z": @NO}, 1) == 0, "①b 显式 frag_z=0 时保持 0");
    CHECK(shader_fragz_from_json(@{@"frag_coord": @NO, @"frag_z": @YES}, 0) == 1, "①b 显式 frag_z=1 时返回 1");

    // ② 真 Metal：256×1，三角形左缘 z 1e-4，右顶点在屏幕右缘（那一行最右的像素 z 接近右顶点，和三角形最大深度同一个指数段），两遍都把 [[position]].z 写进 R32F
    {
        float units = fragz_bias_units(1, 1, 0, L, 1, 1, "t");
        id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
        if (!d) { CHECK(0, "② 没有 Metal 设备"); goto done; }
        id<MTLLibrary> lib = [d newLibraryWithSource:@(k_src) options:nil error:&e];
        if (!lib) { CHECK(0, "② 编不出测试着色器：%s", e.localizedDescription.UTF8String); goto done; }
        const int W = 256;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:1 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:W height:1 mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget; cd.storageMode = MTLStorageModeShared;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fz"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float; pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&e];
        MTLDepthStencilDescriptor *dw = [MTLDepthStencilDescriptor new]; dw.depthCompareFunction = MTLCompareFunctionAlways; dw.depthWriteEnabled = YES;
        MTLDepthStencilDescriptor *dt = [MTLDepthStencilDescriptor new]; dt.depthCompareFunction = MTLCompareFunctionLessEqual; dt.depthWriteEnabled = NO;
        id<MTLDepthStencilState> sw = [d newDepthStencilStateWithDescriptor:dw], st = [d newDepthStencilStateWithDescriptor:dt];
        id<MTLCommandQueue> q = [d newCommandQueue];
        const float tops[2] = {0.999f, 1.0f};
        for (int k = 0; k < 2; k++) {
            float zz[2] = {1e-4f, tops[k]}, unit = ldexpf(1.0f, ilogbf(tops[k]) - 23);   // 期望的一个单位：按最大深度的指数
            id<MTLTexture> dep = [d newTextureWithDescriptor:td], col[2] = {[d newTextureWithDescriptor:cd], [d newTextureWithDescriptor:cd]};
            id<MTLCommandBuffer> cb = [q commandBuffer];
            for (int pass = 0; pass < 2; pass++) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = col[pass]; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.depthAttachment.texture = dep; rp.depthAttachment.loadAction = pass ? MTLLoadActionLoad : MTLLoadActionClear; rp.depthAttachment.clearDepth = 1;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
                [en setRenderPipelineState:ps]; [en setDepthStencilState:pass ? st : sw];
                if (pass) [en setDepthBias:units slopeScale:0 clamp:0];
                [en setVertexBytes:zz length:sizeof zz atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
            }
            [cb commit]; [cb waitUntilCompleted];
            static float a[256], b[256];
            [col[0] getBytes:a bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0];
            [col[1] getBytes:b bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, 1) mipmapLevel:0];
            int all_ge1 = 1, drawn = 0, far = -1; float max_sh = 0, near_ulps = 0;
            for (int i = 0; i < W; i++) {
                if (a[i] <= 0) continue;
                drawn++; float sh = a[i] - b[i], u = ulp_up(a[i]);   // 往近处移的量（正 = 移近）
                if (sh < u) all_ge1 = 0;
                if (sh > max_sh) max_sh = sh;
                if (i == 0) near_ulps = sh / u;
                if (far < 0 || a[i] > a[far]) far = i;
            }
            CHECK(drawn == W, "② 右顶点 z %.3g：三角形盖满 %d 个像素（画了 %d）", tops[k], W, drawn);
            CHECK(all_ge1, "② 右顶点 z %.3g：a. 每个像素都至少往近处移 1 ULP", tops[k]);
            if (far >= 0) {
                float z = a[far], u = ulp_up(z), stored = z - 3 * u;   // 已存深度比片元近 3 ULP（片元在它后面 3 ULP）
                CHECK(b[far] > stored, "② 右顶点 z %.3g：b. 最远端 z=%.7g 移 %.2f ULP，偏移后 %.9g 仍 > 近 3 ULP 的已存 %.9g（照样判为在后面）",
                      tops[k], z, (z - b[far]) / u, b[far], stored);
            }
            CHECK(max_sh <= -units * unit * 1.0001f, "② 右顶点 z %.3g：c. 移的绝对量最大 %.3g ≤ 一个单位 %.3g（近端 z=%.4g 处约 %.0f ULP）",
                  tops[k], max_sh, -units * unit, a[0], near_ulps);
        }
    }
    {   // ③ fs_samples_depth 覆盖 NVN_RB_TEXU（绑定在统一缓冲里的无绑定深度贴图句柄）
        char map[] = "/tmp/t_fragz_map_XXXXXX"; int fd = mkstemp(map); REQUIRE(fd >= 0); close(fd);
        char user[] = "/tmp/t_fragz_usr_XXXXXX"; fd = mkstemp(user); REQUIRE(fd >= 0); close(fd);
        setenv("DM_NVN_SHMAP", map, 1); setenv("DM_PSO_USER", user, 1); setenv("DM_PSO_ARCHIVE", "0", 1);
        mtl_init();
        static uint32_t ubo_mem[4096] __attribute__((aligned(16384)));
        memset(ubo_mem, 0, sizeof ubo_mem);
        nvn_be_pool_add(ubo_mem, sizeof ubo_mem);
        tex_t td3, tc3;
        memset(&td3, 0, sizeof td3); td3.magic = MAGIC('T', 'E', 'X', 0); td3.serial = nvn_new_serial();
        td3.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_DEPTH32F, .width = 16, .height = 16, .depth = 1, .levels = 1};
        nvn_be_reg_tex(20, &td3, NULL);
        memset(&tc3, 0, sizeof tc3); tc3.magic = MAGIC('T', 'E', 'X', 0); tc3.serial = nvn_new_serial();
        tc3.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = 1, .levels = 1};
        nvn_be_reg_tex(21, &tc3, NULL);
        ubo_mem[0] = 21;   // scalar color
        ubo_mem[2] = 21;   // array[0] color
        ubo_mem[4] = 20;   // array[1] depth
        S.ubo[1][0] = (p_bind_buf){.addr = (uint64_t)(uintptr_t)ubo_mem, .size = 64};
        S.tex_base = S.samp_base = 0; g_handle_read_failed = 0;
        NvShader *fs3 = [NvShader new];
        fs3->rf.b = calloc(1, sizeof *fs3->rf.b); REQUIRE(fs3->rf.b); fs3->rf.nb = 1;
        fs3->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXU, .reg = 0, .off = 0};
        CHECK(fs_samples_depth(fs3) == 0, "③ NVN_RB_TEXU 标量指颜色贴图 → fs_samples_depth = 0");
        ubo_mem[0] = 20;
        CHECK(fs_samples_depth(fs3) == 1, "③ NVN_RB_TEXU 标量指深度贴图 → fs_samples_depth = 1");
        fs3->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXU, .reg = 0, .off = 0, .n = 3};
        CHECK(fs_samples_depth(fs3) == 1, "③ NVN_RB_TEXU 数组第 2 项指深度贴图 → fs_samples_depth = 1");
        ubo_mem[0] = 21; ubo_mem[4] = 21;
        CHECK(fs_samples_depth(fs3) == 0, "③ NVN_RB_TEXU 数组全为颜色贴图 → fs_samples_depth = 0");
        nvn_be_pool_del(ubo_mem);
        unlink(map); unlink(user);
    }
done:
    printf("t_nvn_fragz_bias: %u checks\n", checks);
    return S2A_T_RESULT("t_nvn_fragz_bias", failures);
} }
