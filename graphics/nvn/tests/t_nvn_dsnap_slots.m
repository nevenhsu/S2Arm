// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 同遍深度采样的快照存储（nvn_mtl.m dsnap_storage）按形状复用，不在几张深度之间来回重建。
// 背景（11-13 0400486024DEC000 Mac 读档进场景）：每帧先后采样 768x432、1536x864 两张深度（Depth32Float_Stencil8），快照存储只留一份，
//   每帧来回整张新建、旧的推迟释放，每 300 帧新建 900 张、436 MB，常驻集跟着进出 900 次。
// 期望（按「同形状的存储可原地复用」这条现有语义手写，不由被测实现推出）：
//   ① 两种形状交替取 50 轮：每种形状每次拿到的都是第一次那张（不新建）
//   ② 四种形状交替取 20 轮：同上
//   ③ 六种形状（两两同宽、高不同）轮流取：每次拿到的存储格式、宽高、级数都和请求一致，且在常驻集里（份数不够时换掉旧的，不能拿错形状）
//   ④ 带级偏移（tl = 1）：按那一级的宽高建，和直接请求那一级尺寸的是同一张
//   ⑤ 4 倍多重采样（D-ATHR：Mac 上一款台球游戏同遍采样自己的 4 倍多重采样深度，原先原样边读边写）：存储是 2D 多重采样、4 个采样、1 级，与同尺寸单采样的不是同一张
//   ⑥ 能不能整张拷（dsnap_copyable）：2D 单采样、2D 多重采样（采样数 > 1）能；2D 却多采样、多重采样却 1 采样、别的纹理的视图、2D 数组不能
//   ⑦ 真拷一次：4 倍多重采样深度清成 0.375 → 拷进 ⑤ 的快照 → 快照按第 0 个采样读回：每个像素都是 0.375
#include "../gfx/nvn/nvn_mtl.m"

static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "SETUP FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(2); } } while (0)
#define CHECK(c, ...) do { checks++; int good_ = !!(c); failures += !good_; printf("%s ", good_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
static const MTLPixelFormat DS = MTLPixelFormatDepth32Float_Stencil8;
static int resident(id<MTLTexture> t) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:t]; pthread_mutex_unlock(&g_res_mu); return r; }
static int rounds_same(const NSUInteger (*sz)[2], int n, int rounds) {   // n 种形状交替取 rounds 轮，换过存储的次数
    __strong id<MTLTexture> first[8]; int changed = 0;
    for (int i = 0; i < n; i++) first[i] = dsnap_storage(DS, sz[i][0], sz[i][1], 0, 1, 1);
    for (int r = 0; r < rounds; r++) for (int i = 0; i < n; i++) changed += dsnap_storage(DS, sz[i][0], sz[i][1], 0, 1, 1) != first[i];
    return changed;
}
int main(void) {
    @autoreleasepool {
        char map[] = "/tmp/t_dsnap_map_XXXXXX"; int fd = mkstemp(map); REQUIRE(fd >= 0); close(fd);
        char user[] = "/tmp/t_dsnap_user_XXXXXX"; fd = mkstemp(user); REQUIRE(fd >= 0); close(fd);
        setenv("DM_NVN_SHMAP", map, 1); setenv("DM_PSO_USER", user, 1); setenv("DM_PSO_ARCHIVE", "0", 1);
        mtl_init(); REQUIRE(g_dev);
        static const NSUInteger two[2][2] = {{768, 432}, {1536, 864}};
        int c = rounds_same(two, 2, 50);
        CHECK(c == 0, "① 768x432 / 1536x864 交替取 50 轮：存储不换（换了 %d 次，期望 0）", c);
        static const NSUInteger four[4][2] = {{683, 384}, {341, 192}, {170, 96}, {1536, 864}};
        c = rounds_same(four, 4, 20);
        CHECK(c == 0, "② 四种形状交替取 20 轮：存储不换（换了 %d 次，期望 0）", c);
        static const NSUInteger six[6][2] = {{64, 32}, {64, 16}, {32, 64}, {32, 8}, {48, 48}, {48, 24}};
        int bad = 0, cold = 0;
        for (int r = 0; r < 5; r++) for (int i = 0; i < 6; i++) { id<MTLTexture> t = dsnap_storage(DS, six[i][0], six[i][1], 0, 1, 1);
            bad += !t || t.pixelFormat != DS || t.width != six[i][0] || t.height != six[i][1] || t.mipmapLevelCount != 1; cold += t && !resident(t); }
        CHECK(!bad && !cold, "③ 六种形状轮流取：形状不对 %d 次、不在常驻集 %d 次（期望都 0）", bad, cold);
        id<MTLTexture> a = dsnap_storage(DS, 200, 100, 1, 1, 1), b = dsnap_storage(DS, 100, 50, 0, 1, 1);
        CHECK(a && a.width == 100 && a.height == 50 && a == b, "④ 级偏移 1：按 100x50 建（得 %lux%lu），与直接请求 100x50 同一张（%s）",
            (unsigned long)a.width, (unsigned long)a.height, a == b ? "是" : "否");
        id<MTLTexture> m4 = dsnap_storage(DS, 256, 128, 0, 1, 4), s1 = dsnap_storage(DS, 256, 128, 0, 1, 1);
        CHECK(m4 && m4.textureType == MTLTextureType2DMultisample && m4.sampleCount == 4 && m4.mipmapLevelCount == 1 && m4 != s1 && dsnap_storage(DS, 256, 128, 0, 1, 4) == m4,
            "⑤ 4 倍多重采样快照存储：类型 %lu 采样 %lu 级 %lu，与单采样不同张、再取同一张", (unsigned long)m4.textureType, (unsigned long)m4.sampleCount, (unsigned long)m4.mipmapLevelCount);
        CHECK(dsnap_copyable(MTLTextureType2D, 1, 0) && dsnap_copyable(MTLTextureType2DMultisample, 4, 0) && !dsnap_copyable(MTLTextureType2D, 4, 0)
            && !dsnap_copyable(MTLTextureType2DMultisample, 1, 0) && !dsnap_copyable(MTLTextureType2DMultisample, 4, 1) && !dsnap_copyable(MTLTextureType2DArray, 1, 0), "⑥ 能不能整张拷的判定");
        {   // ⑦
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:DS width:256 height:128 mipmapped:NO];
            d.textureType = MTLTextureType2DMultisample; d.sampleCount = 4; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
            id<MTLTexture> src = [g_dev newTextureWithDescriptor:d]; REQUIRE(src);
            id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.depthAttachment.texture = src; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 0.375; rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = src; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(256, 128, 1) toTexture:m4 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            id<MTLTexture> v = fb_narrow_view(m4, src, MTLTextureType2DMultisample, 0, 1, 0, 1);   // 着色器读的视图（同源格式）
            id<MTLBuffer> out = [g_dev newBufferWithLength:256 * 128 * 4 options:MTLResourceStorageModeShared];
            [cb commit]; [cb waitUntilCompleted];
            int copied = cb.status == MTLCommandBufferStatusCompleted;
            // 快照按采样 0 读回：计算核 depth2d_ms<float>.read(pos, 0)
            NSError *err = nil;
            id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void k(depth2d_ms<float> t [[texture(0)]], device float *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { o[p.y * 256 + p.x] = t.read(p, 0); }" options:nil error:&err];
            id<MTLComputePipelineState> ps = lib ? [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err] : nil; REQUIRE(ps);
            id<MTLCommandBuffer> c2 = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c2 computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setTexture:v atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(256, 128, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)]; [ce endEncoding]; [c2 commit]; [c2 waitUntilCompleted];
            const float *f = out.contents; int wrong = 0; for (int i = 0; i < 256 * 128; i++) wrong += f[i] != 0.375f;
            CHECK(copied && v && v.textureType == MTLTextureType2DMultisample && c2.status == MTLCommandBufferStatusCompleted && !wrong,
                "⑦ 多重采样深度清成 0.375 拷进快照、按采样 0 读回：拷贝完成 %d、视图类型 %lu、不等于 0.375 的像素 %d（期望 0）、首像素 %.4f", copied, (unsigned long)v.textureType, wrong, f[0]);
        }
        {   // ⑧ 颜色自采样快照（fb_snap_shape）同槽位多种形状交替取 50 轮不重建，且不同槽位不共享存储
            id<MTLTexture> f0 = fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 1440, 810, 0, 1, 1, 0);
            id<MTLTexture> f1 = fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 720, 405, 0, 1, 1, 0);
            id<MTLTexture> f2 = fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 1440, 810, 0, 1, 1, 1);
            int chg = 0;
            for (int r = 0; r < 50; r++) {
                chg += fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 1440, 810, 0, 1, 1, 0) != f0;
                chg += fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 720, 405, 0, 1, 1, 0) != f1;
            }
            CHECK(f0 && f1 && f2 && f0 != f1 && f0 != f2 && chg == 0,
                "⑧ fb_snap_shape 同槽位交替 1440x810 / 720x405 50 轮不重建（换了 %d 次，期望 0），不同槽位隔离", chg);
        }
        {   // ⑨ 快照池合计上限（PR #2230 复审）：4 槽 × 4 种 1080p RGBA16F（每张约 16.6 MB，16 张约 265 MB），每次取当一笔新绘制。
            //   期望（手算）：合计保留 ≤ FB_SNAP_CAP（96 MB，最多容 5 张）→ 因上限放掉 ≥ 11 张；刚取的那张留着（同参数再取是同一张）；改前合计约 265 MB、放掉 0
            uint64_t ev0 = g_fbsnap_cap_evict; id<MTLTexture> last = nil;
            for (int sl = 0; sl < 4; sl++) for (int w = 0; w < 4; w++) { g_draw_seq++; last = fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 1920, 1080 - (NSUInteger)w, 0, 1, 1, sl); }
            id<MTLTexture> again = fb_snap_shape(MTLTextureType2D, MTLPixelFormatRGBA16Float, 1920, 1077, 0, 1, 1, 3);
            uint64_t ev = g_fbsnap_cap_evict - ev0;
            CHECK(last && again == last && g_fbsnap_bytes <= FB_SNAP_CAP && ev >= 11,
                "⑨ 快照池合计 %.1f MB（上限 %u MB）、因上限放掉 %llu 张（期望 ≥ 11）、刚取的那张还在 %d", g_fbsnap_bytes / 1048576.0, FB_SNAP_CAP >> 20, (unsigned long long)ev, again == last);
        }
        {   // ⑩ dsnap_stale_dispatch 按计算着色器写的图像精确失效 g_dsnap
            static uint8_t dpool[1 << 20] __attribute__((aligned(65536)));
            nvn_be_pool_add(dpool, sizeof dpool);
            tex_t td10, tc10, to10;
            memset(&td10, 0, sizeof td10); td10.magic = MAGIC('T', 'E', 'X', 0); td10.serial = nvn_new_serial(); td10.mem = dpool; td10.L.size = 0x10000;
            nvn_be_reg_tex(30, &td10, NULL);
            memset(&tc10, 0, sizeof tc10); tc10.magic = MAGIC('T', 'E', 'X', 0); tc10.serial = nvn_new_serial(); tc10.mem = dpool + 0x40000; tc10.L.size = 0x10000;
            nvn_be_reg_tex(31, &tc10, NULL);
            memset(&to10, 0, sizeof to10); to10.magic = MAGIC('T', 'E', 'X', 0); to10.serial = nvn_new_serial(); to10.mem = dpool + 0x8000; to10.L.size = 0x4000;
            nvn_be_reg_tex(32, &to10, NULL);
            g_dsnap.valid = 1; g_dsnap.ser = td10.serial; g_dsnap.mem = td10.mem; g_dsnap.size = td10.L.size; g_dsnap.base = nil;

            NvShader *cs_buf = [NvShader new];
            cs_buf->rf.b = calloc(1, sizeof *cs_buf->rf.b); REQUIRE(cs_buf->rf.b); cs_buf->rf.nb = 1;
            cs_buf->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .rw = 1};
            dsnap_stale_dispatch(cs_buf);
            CHECK(g_dsnap.valid == 1, "⑩ 只写缓冲的计算核不失效深度快照");

            NvShader *cs_img = [NvShader new];
            cs_img->rf.b = calloc(1, sizeof *cs_img->rf.b); REQUIRE(cs_img->rf.b); cs_img->rf.nb = 1;
            cs_img->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .reg = 0, .rw = 1};
            S.img[5][0] = 31; g_handle_read_failed = 0;
            dsnap_stale_dispatch(cs_img);
            CHECK(g_dsnap.valid == 1, "⑩ 写无关图像的计算核不失效深度快照");

            S.img[5][0] = 32;
            dsnap_stale_dispatch(cs_img);
            CHECK(g_dsnap.valid == 0, "⑩ 写重叠地址范围图像的计算核失效深度快照");

            g_dsnap.valid = 1; S.img[5][0] = 30;
            dsnap_stale_dispatch(cs_img);
            CHECK(g_dsnap.valid == 0, "⑩ 写同序列号图像的计算核失效深度快照");

            g_dsnap.valid = 1; dsnap_stale_dispatch(NULL);
            CHECK(g_dsnap.valid == 0, "⑩ cs==NULL 保守失效深度快照");

            g_dsnap.valid = 1; cs_buf->tensor = 1; dsnap_stale_dispatch(cs_buf);
            CHECK(g_dsnap.valid == 0, "⑩ tensor==1 保守失效深度快照");

            g_dsnap.valid = 1; cs_buf->tensor = 0; cs_buf->builtin = "test"; dsnap_stale_dispatch(cs_buf);
            CHECK(g_dsnap.valid == 0, "⑩ builtin!=NULL 保守失效深度快照");

            nvn_be_pool_del(dpool);
        }
        unlink(map); unlink(user);
    }
    printf("%s t_nvn_dsnap_slots: %u checks %u failures\n", failures ? "✗" : "✓", checks, failures);
    return failures ? 1 : 0;
}
