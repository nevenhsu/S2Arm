// Switch 2 着色器里的张量核矩阵乘（Ampere HMMA，操作码 0x23c）：游戏自带的超分神经网络（11-05 时之笛 Switch 2 版开局 11 个计算程序，10 个带 HMMA 16–768 条）。
// 翻译器把 HMMA 拆成逐线程的标量乘加：69 KB 的程序翻成 1.7 MB、3.8 万行 MSL，Apple 编译器编数分钟（M5 Max 上编 G18P 机器码 131–199 秒），
//   提交线程同步等，开局停在第 94 帧；即便编好，按 Eden 系（Eglantine #589：software expansion 驱动编数分钟、远慢于帧预算）也跑不进帧时间。
// 顶替（同 Eglantine PR #614 的 stand-in）：这组程序不派发。只写缓冲区的那几层（网络入口 / 中间层）直接跳过；写图的那一层（网络出口）不跑网络，
//   把它自己读的低分辨率画面用 MetalFX 空间放大写满它要写的输出图。选图规则：输出 = 它绑定的第一张可写图；输入 = 它读的二维贴图里宽高比与输出差 < 2%、
//   输出 / 输入在 1–4 倍之间、面积最大的一张（Eden 第一版按常量缓冲区第一对同格式图选，选到 4x4 / 120x67 的特征图）；挑不出就报红、这一遍不写。
//   画质：没有网络的细节重建，比原版超分软；忠实执行（HMMA → simdgroup_matrix）另做（维护者 11-05 定：先顶替、再做忠实版对照）。
// 识别：prog_get 首次解析计算着色器时用 nvn2_hmma_count（nvn2.h）扫游戏原始代码区，只看 Switch 2 程序（Maxwell 没有 HMMA）
// 输出域（11-05 时之笛游玩画面整屏发白的根因）：网络出口层写出的不一定是它读的画面本身。游玩时 5 支出口层（fa3033d05c0a41d6 等）读 RGB9E5 低分辨率画面，
//   每个分量先加一个本地偏置、再 HMUL2 R, R, |R| 取带符号平方，加网络残差后写 RG11B10 输出——输出在输入的平方域；照原样放大写进去等于每个分量开了平方根
//   （0.04 → 0.2），暗森林整屏发白。标题画面 11 支网络 0 条这种平方，读写同域。规则：出口层的 HMUL2 R, R, |R| 条数（nvn2_hsq_count，prog_get 记进 tensor_sq）> 0
//   → 放大前先把输入逐分量平方进一张 RGBA16F 临时图（tensor_sq_pass），再交 MetalFX；偏置与残差是网络算的，顶替没有，按 0
// 输出域之二（11-14 一款体育游戏整个画面像盖了一层半透明白底）：10 支出口层（725bcf5c3f4bb3f3 等）读 RGB9E5 低分辨率画面、写 RG11B10，出口处把结果从 sRGB 解码成线性
//   （HSETP2 比 0.04045、HMUL2 × 1/12.92、HFMA2 + 0.055/1.055、MUFU LG2 × 2.4 EX2，逐条核过）——网络在 sRGB 域算，读的画面是 sRGB 编码的值；
//   照原样放大写进去等于线性图里放了 sRGB 值（0.2 → 应为 0.033），暗部整体抬亮、发灰发白。规则：出口层的 HSETP2 R, 0.04045 条数（nvn2_srgb_count，prog_get 记进 tensor_srgb）> 0、
//   且不是平方域 → 放大前先把输入逐分量 sRGB 解码进同一种 RGBA16F 临时图（tensor_sq_pass 的第二个核），再交 MetalFX；分屏输入块不在左上角时先裁块再解码
// 有效区域（11-05 第十九版：平方修好后仍只有左上 1/4 屏是画面、其余白）：这一族出口层（带平方的 15 支，机器码同构，逐支核过）只读一张贴图、写一张图，
//   参数在推常量槽（c[24 + k]，NVN_RB_PUSH）里按 float4 排：c[1].xy（int）= 输出起点，c[6].xy（int）= 输出宽高（各线程按「剩余宽度 = c[6].x − 组号 × c[4].z」夹边），
//   c[6].zw（float）= 每个输出像素在输入上走的归一化坐标步长（采样坐标 = (输出 x + 0.5) × c[6].z）。动态分辨率下游戏只把画面画在输入图左上一块：
//   输入有效宽 = c[6].x × c[6].z × 输入图宽。整张输入图交 MetalFX 时有效画面只落在输出左上、其余是输入图里没画的区域。
//   规则：带平方的出口层、绑定恰好 1 张贴图 + 1 张图 + 1 个推常量槽 → 按本次派发的推常量给 MetalFX 设输入内容宽高；输出区域不是整张图时先放大进同尺寸临时图，
//   再拷到输出图的起点。参数不成立（宽高 ≤ 0、步长非正 / 非有限、越出图）报红一次、照整张图做
static uint64_t g_tensor_skip, g_tensor_up, g_tensor_red, g_tensor_off;
#include "tensor_widen.h"   // 满屏放宽下挑不出输入几时退回 16:9（#726）
int s2a_screen_w(void); int s2a_screen_h(void);   // runtime/src/widen.c（满屏门之后的屏幕宽 / 高；原生 16:9 = 1920 × 1080）
int s2a_widen_force_native(int on);   // runtime/src/widen.c：运行中退回原生 16:9（图形单测由 tests/host_stubs.h 给桩）
static s2a_tensor_widen_t g_tensor_widen;   // 只在 g_rmu 外、派发线程里改（tensor_standin 由录制 / 编码线程串行调）
// 诊断开关 DM_NVN_TENSOR_STANDIN=0（11-11 #376 / #464：M1 主机模式 Metal 4 进场景后 GPU 挂死，顶替用的 Metal 4 版 MetalFX 空间放大是头号嫌疑）：网络照旧不派发，
//   出口层也不做顶替（不平方、不调 MetalFX、不拷），输出图这一遍不写——画面不对，只为判挂死是不是顶替引起的。缺省（不设 / 其他值）照旧顶替，零变化
static int g_tensor_standin_on = -1;
static int tensor_standin_decide(const char *e) { return !(e && *e == '0'); }   // =0：关；不设或其他值：开（同 DM_NVN_GMEM）
static int tensor_standin_enabled(void) {
    if (g_tensor_standin_on < 0) { const char *e = getenv("DM_NVN_TENSOR_STANDIN");
        g_tensor_standin_on = tensor_standin_decide(e);
        MLOG("张量核网络顶替：%s（DM_NVN_TENSOR_STANDIN %s）", g_tensor_standin_on ? "开，出口层用 MetalFX 空间放大写输出图" : "关（诊断）：出口层不顶替，输出图不写", e && *e ? e : "未设"); }
    return g_tensor_standin_on;
}
static NvDlss *g_tensor_fx[4];
static id<MTLTexture> g_tensor_sq_tex[4]; static unsigned g_tensor_sq_n;
static uint64_t g_tensor_sq_runs, g_tensor_rect_runs;
static id<MTLTexture> g_tensor_out_tex[4]; static unsigned g_tensor_out_n;
// 放大器的定尺寸输入图（tensor_fix_in）：每种（格式、输出尺寸）一张，尺寸 = 输出尺寸；记着上次的有效区域，变了才把区域外清零
static struct { __strong id<MTLTexture> t; NSUInteger cw, ch; } g_tensor_fix[4]; static unsigned g_tensor_fix_n;
static uint64_t g_tensor_fix_runs, g_tensor_fix_clears;

// 出口层绑定恰好 1 张贴图 + 1 张图 + 1 个推常量槽时返回那个槽号，否则 -1（tensor_rect / tensor_rect_io 两族共用）
static int tensor_push_slot(NvShader *cs) {
    int slot = -1, ntex = 0, nimg = 0;
    for (int i = 0; i < cs->rf.nb; i++) { const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind == NVN_RB_PUSH) slot = slot < 0 ? b->reg : 99; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) ntex++; else if (b->kind == NVN_RB_IMG) nimg++; }
    return ntex == 1 && nimg == 1 && slot >= 0 && slot < NVN2_PUSH_SLOTS ? slot : -1;
}
// 本次派发的有效区域（文件头「有效区域」）：成功返回 1，r = {输入内容宽, 高, 输出起点 x, y, 输出宽, 高}（都按宿主存储尺寸）；这一族以外返回 0（照整张图）
static int tensor_rect(NvShader *cs, const tex_t *tc, id<MTLTexture> c, const tex_t *to, id<MTLTexture> o, NSUInteger r[6]) {
    int slot = tensor_push_slot(cs);
    if (!cs->tensor_sq || slot < 0) return 0;
    const uint8_t *p = S.push[1][slot]; int32_t x0, y0, w, h; float kx, ky;
    memcpy(&x0, p + 16, 4); memcpy(&y0, p + 20, 4); memcpy(&w, p + 96, 4); memcpy(&h, p + 100, 4); memcpy(&kx, p + 104, 4); memcpy(&ky, p + 108, 4);
    double sx = (double)o.width / MAX(to->d.width, 1u), sy = (double)o.height / MAX(to->d.height, 1u);   // 游戏尺寸 → 宿主存储尺寸（满屏放宽 / 内部分辨率，同 dlss_eval）
    long X0 = lround(x0 * sx), Y0 = lround(y0 * sy), W = lround(w * sx), H = lround(h * sy);
    double iw = (double)w * kx * c.width, ih = (double)h * ky * c.height;
    (void)tc;
    if (x0 < 0 || y0 < 0 || W <= 0 || H <= 0 || X0 + W > (long)o.width || Y0 + H > (long)o.height || !isfinite(kx) || !isfinite(ky) || kx <= 0 || ky <= 0
        || iw < 0.5 || ih < 0.5 || iw > c.width + 0.5 || ih > c.height + 0.5) {
        if (!(cs->tensor_log & 4)) { cs->tensor_log |= 4; MLOG("【红】 张量核网络顶替：%s 推常量槽 %d 的有效区域不成立（输出起点 %d,%d 宽高 %d x %d，步长 %g / %g，输入图 %lux%lu、输出图 %lux%lu），照整张图放大",
            cs->key, slot, x0, y0, w, h, kx, ky, (unsigned long)c.width, (unsigned long)c.height, (unsigned long)o.width, (unsigned long)o.height); }
        return 0;
    }
    r[0] = (NSUInteger)MAX(lround(iw), 1L); r[1] = (NSUInteger)MAX(lround(ih), 1L); r[2] = (NSUInteger)X0; r[3] = (NSUInteger)Y0; r[4] = (NSUInteger)W; r[5] = (NSUInteger)H;
    if (r[0] > c.width) r[0] = c.width; if (r[1] > c.height) r[1] = c.height;
    return 1;
}
// 分屏（不带平方的另一族出口层：一张贴图 + 一张图 + 一个推常量槽）：推常量 c[0].xyzw（int）= 输入区域 起点 x,y + 宽高减一，c[1].xyzw（int）= 输出区域 起点 x,y + 宽高减一，
//   c[2].xy = 1 / 输入区域宽高。单人画面两块都是整张图；双人左右分屏时输出图 2048x1152 分两次派发、每次写半边（c[1] = 0,0,1023,1151 / 1024,0,1023,1151），
//   读的是 342x384 的半屏画面——整张输出图 16:9 和它宽高比对不上，选图规则挑不出、这一遍不写，3D 画面全黑（只剩界面）。
//   规则：不带平方、绑定恰好 1 张二维贴图 + 1 张图 + 1 个推常量槽，c[0] / c[1] 两块区域都在图内、宽高比差 < 2%、放大 1–4 倍 → 返回 1，
//   r = {输入内容宽, 高, 输出起点 x, y, 输出宽, 高, 输入起点 x, y}（宿主存储尺寸）；其他情形返回 0（照旧整张图）。
//   输入起点不在左上角（上下分屏两块共用一张输入图时）：MetalFX 的输入内容只能从左上角算，先把这块拷进同尺寸临时图再放大
static int tensor_rect_io(NvShader *cs, const tex_t *tc, id<MTLTexture> c, const tex_t *to, id<MTLTexture> o, NSUInteger r[8]) {
    int slot = tensor_push_slot(cs);
    if (cs->tensor_sq || slot < 0 || !c || !o) return 0;
    const uint8_t *p = S.push[1][slot]; int32_t in[4], out[4];
    memcpy(in, p, 16); memcpy(out, p + 16, 16);
    double ix = (double)c.width / MAX(tc->d.width, 1u), iy = (double)c.height / MAX(tc->d.height, 1u);   // 游戏尺寸 → 宿主存储尺寸（同 tensor_rect）
    double ox = (double)o.width / MAX(to->d.width, 1u), oy = (double)o.height / MAX(to->d.height, 1u);
    long IX = lround(in[0] * ix), IY = lround(in[1] * iy), iw = lround((in[2] + 1.0) * ix), ih = lround((in[3] + 1.0) * iy);
    long X0 = lround(out[0] * ox), Y0 = lround(out[1] * oy), W = lround((out[2] + 1.0) * ox), H = lround((out[3] + 1.0) * oy);
    if (in[0] < 0 || in[1] < 0 || in[2] < 0 || in[3] < 0 || out[0] < 0 || out[1] < 0 || out[2] < 0 || out[3] < 0
        || iw <= 0 || ih <= 0 || IX + iw > (long)c.width || IY + ih > (long)c.height || W <= 0 || H <= 0 || X0 + W > (long)o.width || Y0 + H > (long)o.height) return 0;
    double s = (double)W / iw, ai = (double)iw / ih, ao = (double)W / H;
    if (s < 1.0 - 1e-6 || s > 4.0 + 1e-6 || fabs(ai - ao) / ao > 0.02) return 0;
    r[0] = (NSUInteger)iw; r[1] = (NSUInteger)ih; r[2] = (NSUInteger)X0; r[3] = (NSUInteger)Y0; r[4] = (NSUInteger)W; r[5] = (NSUInteger)H; r[6] = (NSUInteger)IX; r[7] = (NSUInteger)IY;
    return 1;
}
// 输出区域不是整张输出图时 MetalFX 先写的临时图：输出图的格式、区域宽高，留最近 4 种（同 tensor_sq_tex）
static id<MTLTexture> tensor_tmp_tex(__strong id<MTLTexture> *cache, unsigned *n, MTLPixelFormat pf, NSUInteger w, NSUInteger h, MTLTextureUsage u) {   // u: what MetalFX writes it with
    for (int i = 0; i < 4; i++) { id<MTLTexture> t = cache[i]; if (t && t.pixelFormat == pf && t.width == w && t.height == h && (t.usage & u) == u) return t; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.usage = u | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    if (!t) { RED_ONCE(163, "张量核网络顶替：建 %lux%lu 的临时图失败，输出图这一遍不写", (unsigned long)w, (unsigned long)h); return nil; }
    int slot = (int)((*n)++ % 4);
    if (cache[slot]) defer_obj_release(cache[slot]);
    cache[slot] = t; res_add(t);
    return t;
}
static id<MTLTexture> tensor_out_tex(MTLPixelFormat pf, NSUInteger w, NSUInteger h, MTLTextureUsage u) { return tensor_tmp_tex(g_tensor_out_tex, &g_tensor_out_n, pf, w, h, u); }
// 分屏输入块的临时图（tensor_rect_io）：自己一组，不和输出临时图共用（同格式同尺寸时会拿到同一张）
static id<MTLTexture> g_tensor_in_tex[4]; static unsigned g_tensor_in_n;
static id<MTLTexture> tensor_in_tex(MTLPixelFormat pf, NSUInteger w, NSUInteger h, MTLTextureUsage u) { return tensor_tmp_tex(g_tensor_in_tex, &g_tensor_in_n, pf, w, h, u); }

// 平方用的临时图：输入的宽高、RGBA16F（可写、MetalFX 可读）；动态分辨率下宽高会变，留最近 4 种，换掉的那张等用过它的提交完成再放（defer_obj_release 连驻留一起退）
static id<MTLTexture> tensor_sq_tex(NSUInteger w, NSUInteger h) {
    for (int i = 0; i < 4; i++) { id<MTLTexture> t = g_tensor_sq_tex[i]; if (t && t.width == w && t.height == h) return t; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    if (!t) { RED_ONCE(163, "张量核网络顶替：建 %lux%lu 的平方临时图失败，输出图这一遍不写", (unsigned long)w, (unsigned long)h); return nil; }
    int slot = (int)(g_tensor_sq_n++ % 4);
    if (g_tensor_sq_tex[slot]) defer_obj_release(g_tensor_sq_tex[slot]);
    g_tensor_sq_tex[slot] = t; res_add(t);
    return t;
}
// 逐分量带符号平方：d.rgb = s.rgb · |s.rgb|，d.a = s.a（出口层只对颜色分量做平方）
// 逐分量 sRGB 解码（tensor_srgb）：x ≤ 0.04045 → x / 12.92，否则 ((x + 0.055) / 1.055)^2.4，同出口层那段（门限比较在前，负值走线性段）；d.a = s.a
static const char k_tensor_sq_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void tensor_sq(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]], uint2 p [[thread_position_in_grid]]) {\n"
"  if (p.x >= s.get_width() || p.y >= s.get_height() || p.x >= d.get_width() || p.y >= d.get_height()) return;\n"
"  float4 v = s.read(p); d.write(float4(v.rgb * abs(v.rgb), v.a), p); }\n"
"kernel void tensor_srgb(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]], uint2 p [[thread_position_in_grid]]) {\n"
"  if (p.x >= s.get_width() || p.y >= s.get_height() || p.x >= d.get_width() || p.y >= d.get_height()) return;\n"
"  float4 v = s.read(p); float3 c = select(pow((max(v.rgb, 0.04045f) + 0.055f) / 1.055f, 2.4f), v.rgb / 12.92f, v.rgb <= 0.04045f); d.write(float4(c, v.a), p); }\n";
// S2Arm #674: k_tensor_sq_msl is compiled from source as well; built at the first stand-in it held the submit thread 74-118 ms in the first 3D scene when the system's compile
//   cache missed. On Switch 2 with the stand-in on, mtl_init starts the build in the background (tensor_sq_prewarm); the first pass waits for it (tensor_sq_ready), as ixc_prewarm / ixc_ready
static id<MTLComputePipelineState> g_tsq_ps, g_tsrgb_ps; static NSError *g_tsq_err;
static dispatch_group_t g_tsq_pre;   // the background build, NULL = not started
static void tensor_sq_build(void) {
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_tensor_sq_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"tensor_sq"] : nil;
    id<MTLFunction> g = lib ? [lib newFunctionWithName:@"tensor_srgb"] : nil;
    g_tsq_ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
    g_tsrgb_ps = g_tsq_ps && g ? [g_dev newComputePipelineStateWithFunction:g error:&e] : nil; g_tsq_err = e;
}
static void tensor_sq_prewarm(void) { if (!nvn2_on() || !tensor_standin_enabled() || g_tsq_pre) return; g_tsq_pre = dispatch_group_create(); dispatch_group_async(g_tsq_pre, s2a_qos_global(S2A_QOS_SHADER_PREFETCH), ^{ @autoreleasepool { tensor_sq_build(); } }); }
static id<MTLComputePipelineState> tensor_sq_ready(void) { if (g_tsq_pre) dispatch_group_wait(g_tsq_pre, DISPATCH_TIME_FOREVER); else tensor_sq_build(); return g_tsq_ps; }   // once: tensor_sq_pass keeps the table (g_tsrgb_ps is built with it)
static uint64_t g_tensor_srgb_runs;
// dom：1 = 带符号平方（tensor_sq）、2 = sRGB 解码（tensor_srgb）；两个核同一份源码、同一张参数表
static int tensor_sq_pass(id<MTLTexture> s, id<MTLTexture> d, int dom) {
    static id<MTLComputePipelineState> ps[2]; static id at; static int failed;
    if (failed) return 0;
    if (!at) { NSError *e = nil;
        ps[0] = tensor_sq_ready(); ps[1] = g_tsrgb_ps; if (!ps[0] || !ps[1]) e = g_tsq_err;
        at = ps[0] && ps[1] ? rec_table(tab_new(1, 2, 0, &e)) : nil;
        if (!at) { failed = 1; MLOG("【红】 张量核网络顶替：平方 / sRGB 解码计算核建不出（%s），要换输出域的出口层输出图不写", e.localizedDescription.UTF8String); return 0; } }
    id ce = enc_c_begin(NULL); enc_c_table(ce, at); tab_tex(at, s, 0); tab_tex(at, d, 1); [ce setComputePipelineState:ps[dom == 2]];
    NSUInteger gw = MIN(s.width, d.width), gh = MIN(s.height, d.height);
    enc_c_dispatch(ce, MTLSizeMake((gw + 15) / 16, (gh + 15) / 16, 1), MTLSizeMake(16, 16, 1));
    enc_c_end(ce);
    args_dirty_all();   // the compute table was this pass's: the game's next dispatch binds its own again
    if (dom == 2) g_tensor_srgb_runs++; else g_tensor_sq_runs++;
    return 1;
}

// 输入图尺寸不等于放大器输入容量（= 输出尺寸）时：有效区域 cw x ch 从 src 左上角拷进定尺寸输入图左上角；有效区域变了就把区域外（右条、下条）清零，
//   免得 MetalFX 在边缘采到上一种尺寸留下的像素（能耗线 11-15）。拷贝在调用方开的拷贝编码器里，调用方关编码器即全屏障。失败回 nil（这一遍不写）
static int tensor_fix_get(MTLPixelFormat pf, NSUInteger W, NSUInteger H, NSUInteger cw, NSUInteger ch, MTLTextureUsage u, int *need_clear) {
    if (cw > W || ch > H || !cw || !ch) { RED_ONCE(195, "张量核网络顶替：有效区域 %lux%lu 超出放大器输入容量 %lux%lu，这一遍不写", (unsigned long)cw, (unsigned long)ch, (unsigned long)W, (unsigned long)H); return -1; }
    int k = -1;
    for (int i = 0; i < 4; i++) { id<MTLTexture> t = g_tensor_fix[i].t; if (t && t.pixelFormat == pf && t.width == W && t.height == H && (t.usage & u) == u) { k = i; break; } }
    if (k < 0) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:W height:H mipmapped:NO];
        d.usage = u | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
        if (!t) { RED_ONCE(196, "张量核网络顶替：建 %lux%lu 的定尺寸输入图失败，这一遍不写", (unsigned long)W, (unsigned long)H); return -1; }
        k = (int)(g_tensor_fix_n++ % 4);
        if (g_tensor_fix[k].t) defer_obj_release(g_tensor_fix[k].t);
        g_tensor_fix[k].t = t; g_tensor_fix[k].cw = g_tensor_fix[k].ch = 0; res_add(t);
    }
    if (need_clear) *need_clear = g_tensor_fix[k].cw != cw || g_tensor_fix[k].ch != ch;
    return k;
}
static void tensor_fix_clear_border(id e, int k, NSUInteger W, NSUInteger H, NSUInteger cw, NSUInteger ch) {
    id<MTLTexture> fx = g_tensor_fix[k].t;
    if (g_tensor_fix[k].cw != cw || g_tensor_fix[k].ch != ch) {   // 区域外清零：右条 [cw, W) x [0, ch)，下条 [0, W) x [ch, H)
        size_t bpp = mtl_pf_bytes(fx.pixelFormat), row = (size_t)W * bpp;
        id<MTLBuffer> zb = bpp ? zero_buf(row * H) : nil;
        if (!zb) { RED_ONCE(197, "张量核网络顶替：定尺寸输入图格式 %lu 清不了零，区域外可能带旧像素", (unsigned long)fx.pixelFormat); }
        else {
            if (cw < W) enc_b_copy_buffer_texture(e, zb, 0, row, row * H, MTLSizeMake(W - cw, ch, 1), fx, 0, 0, MTLOriginMake(cw, 0, 0));
            if (ch < H) enc_b_copy_buffer_texture(e, zb, 0, row, row * H, MTLSizeMake(W, H - ch, 1), fx, 0, 0, MTLOriginMake(0, ch, 0));
            g_tensor_fix_clears++;
        }
        g_tensor_fix[k].cw = cw; g_tensor_fix[k].ch = ch;
    }
}
static id<MTLTexture> tensor_fix_in(id<MTLTexture> src, NSUInteger W, NSUInteger H, NSUInteger cw, NSUInteger ch, MTLTextureUsage u) {
    int need_clear = 0;
    int k = tensor_fix_get(src.pixelFormat, W, H, cw, ch, u, &need_clear);
    if (k < 0) return nil;
    id<MTLTexture> fx = g_tensor_fix[k].t;
    id e = blit();
    enc_b_copy_texture(e, src, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(cw, ch, 1), fx, 0, 0, MTLOriginMake(0, 0, 0));
    if (need_clear) tensor_fix_clear_border(e, k, W, H, cw, ch);
    enc_close_blit();
    g_tensor_fix_runs++;
    return fx;
}
// 放大器按（输入格式、输出格式、输出尺寸）一个，输入容量固定等于输出尺寸（网络放大 1–4 倍，输入不会大过输出）；输入图尺寸不等时先拷进定尺寸输入图（tensor_fix_in），
//   有效区域交 inputContentWidth / Height。原先按输入图尺寸精确缓存 4 个：游戏动态降分辨率时输入尺寸一直变（854x480、763x429、683x384…），放大器跟着反复新建、挤掉旧的；
//   第三类 GPU 挂死 4/4 份样本（M1 Pro / M2，时之笛）都紧跟在这之后（D-698W / D-KV2T / D-P2RA / D-EA46）
static NvDlss *tensor_scaler_for(MTLPixelFormat cf, NSUInteger cw, NSUInteger ch, id<MTLTexture> o) {
    for (int i = 0; i < 4; i++) { NvDlss *f = g_tensor_fx[i]; if (f && f->cf == cf && f->of == o.pixelFormat && f->ow == o.width && f->oh == o.height) return f; }
    Class scl = dlss_class("MTLFXSpatialScalerDescriptor");
    if (!scl) { RED_ONCE(160, "张量核网络顶替：这台设备没有 MetalFX 空间放大，输出图这一遍不写"); return nil; }
    MTLFXSpatialScalerDescriptor *sd = [scl new]; BOOL m4 = !g_m3; id sc = nil;
    sd.colorTextureFormat = cf; sd.outputTextureFormat = o.pixelFormat; sd.inputWidth = o.width; sd.inputHeight = o.height; sd.outputWidth = o.width; sd.outputHeight = o.height;
    BOOL ldr = cf == MTLPixelFormatRGBA8Unorm || cf == MTLPixelFormatBGRA8Unorm || cf == MTLPixelFormatRGBA8Unorm_sRGB || cf == MTLPixelFormatBGRA8Unorm_sRGB || cf == MTLPixelFormatRGB10A2Unorm;
    sd.colorProcessingMode = ldr ? MTLFXSpatialScalerColorProcessingModePerceptual : MTLFXSpatialScalerColorProcessingModeHDR;
    if (m4) { if (@available(macOS 26.0, iOS 26.0, *)) { id comp = present_fx_compiler(); if (comp) sc = [sd newSpatialScalerWithDevice:g_dev compiler:comp]; } }
    else sc = [sd newSpatialScalerWithDevice:g_dev];
    if (!sc) { RED_ONCE(161, "张量核网络顶替：建 MetalFX 空间放大器失败（%lux%lu 格式 %lu → %lux%lu 格式 %lu），输出图这一遍不写", (unsigned long)cw, (unsigned long)ch,
                        (unsigned long)cf, (unsigned long)o.width, (unsigned long)o.height, (unsigned long)o.pixelFormat); return nil; }
    id<MTLFence> fence = m4 ? [g_dev newFence] : g_fence3; [(id<MTLFXSpatialScalerBase>)sc setFence:fence];
    int slot = (int)(g_tensor_up % 4); for (int i = 0; i < 4; i++) if (!g_tensor_fx[i]) { slot = i; break; }
    if (g_tensor_fx[slot]) defer_obj_unref(g_tensor_fx[slot]->scaler);
    NvDlss *f = [NvDlss new]; f->scaler = sc; f->spatial = YES; f->m4 = m4;
    f->cf = cf; f->of = o.pixelFormat; f->iw = o.width; f->ih = o.height; f->ow = o.width; f->oh = o.height; g_tensor_fx[slot] = f;
    MLOG("张量核网络顶替：MetalFX 空间放大（%s）格式 %lu → %lux%lu 格式 %lu，输入容量 %lux%lu（这一遍输入 %lux%lu，有效区域交 inputContentWidth / Height）", m4 ? "Metal 4" : "Metal 3",
         (unsigned long)cf, (unsigned long)o.width, (unsigned long)o.height, (unsigned long)o.pixelFormat, (unsigned long)o.width, (unsigned long)o.height, (unsigned long)cw, (unsigned long)ch);
    return f;
}
static NvDlss *tensor_scaler(id<MTLTexture> c, id<MTLTexture> o) { return tensor_scaler_for(c.pixelFormat, c.width, c.height, o); }

// 写目标用同一份存储的恒等通道视图（同 Ryujinx：存储图像绑恒等通道视图）：按贴图编号取到的是采样用视图，登记带通道重排（R11G11B10F 的 a = 1 等）时
//   是重排视图，Metal 给它的用途只有 ShaderRead；拿它去 dlss_usage 就缺渲染目标用途，tex_upgrade 新建一张、整张拷贝、换进序号表，下次又在新底图上建重排视图，
//   每帧再升级一次（11-05 时之笛第二十版 ser=4149 升级 4354 次，单测 t_nvn_tensor_upgrade_once）。恒等视图的用途随底图，层级 / 层范围不变
static id<MTLTexture> tensor_write_view(id<MTLTexture> v) {
    MTLTextureSwizzleChannels s = v.swizzle; id<MTLTexture> p = v.parentTexture;
    if (!p || (s.red == MTLTextureSwizzleRed && s.green == MTLTextureSwizzleGreen && s.blue == MTLTextureSwizzleBlue && s.alpha == MTLTextureSwizzleAlpha)) return v;
    NSUInteger ns = v.textureType == MTLTextureType3D ? 1 : v.arrayLength * (v.textureType == MTLTextureTypeCube || v.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    id<MTLTexture> w = mtl_view(p, v.pixelFormat, v.textureType, NSMakeRange(v.parentRelativeLevel, v.mipmapLevelCount), NSMakeRange(v.parentRelativeSlice, ns));
    return w ? w : v;
}
// 派发一个带 HMMA 的计算程序时调（dispatch）：不派发它；它写图就把读到的低分辨率画面放大进去
static void tensor_standin(NvShader *cs) {
    if (!tensor_standin_enabled()) { g_tensor_off++; return; }   // 诊断关（见 g_tensor_standin_on）
    os_unfair_lock_lock(&g_rmu);
    int otid = -1;
    for (int i = 0; i < cs->rf.nb && otid < 0; i++) { const nvn_rbind *b = &cs->rf.b[i]; if (b->kind == NVN_RB_IMG && b->reg < 32) { int tid = (int)(S.img[5][b->reg] & 0xfffff); if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) otid = tid; } }
    if (otid < 0) { os_unfair_lock_unlock(&g_rmu); g_tensor_skip++; return; }   // 只写缓冲区的一层（网络入口 / 中间层）
    // 两张图的贴图对象在锁里拷成值再用：g_treg 是登记表本身，别的线程登记新编号时在 g_rmu 里 realloc 扩表、旧表释放，出锁后再经 &g_treg[i].t 读就悬空
    //   （11-05 时之笛 Switch 2 版测试设备2：tex_upgrade 读 t->serial 段错误，单测 t_nvn_tensor_treg）。拷贝同录制时的快照（tex_snap），后面只读它的字段
    tex_t to_v = g_treg[otid].t, tc_v; const tex_t *to = &to_v; void *ov = NULL; (void)tex_fast_rid(otid, &ov); id<MTLTexture> o = (__bridge id<MTLTexture>)ov;
    const tex_t *tc = NULL; id<MTLTexture> c = nil; double ar = o ? (double)o.width / MAX(o.height, 1u) : 0;
    tex_t t1_v; id<MTLTexture> t1 = nil; int n2d = 0;   // 读的二维贴图（不论宽高比）：只有一张时给分屏规则（tensor_rect_io）用
    for (int i = 0; o && i < cs->rf.nb; i++) {
        const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TEXU) continue;
        int tid = b->kind == NVN_RB_TEXU ? ubo_tex_id(b->n ? ubo_handle(5, b->reg, b->off) : ubo_packed(5, b)) : (int)(tex_slot_h(S.tex[5], S.img[5], b->reg) & 0xfffff);
        if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || tid == otid) continue;
        void *tv = NULL; (void)tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
        if (!t || t.textureType != MTLTextureType2D || !t.width || !t.height) continue;
        if (n2d++ == 0) { t1 = t; t1_v = g_treg[tid].t; }
        double s = (double)o.width / t.width, a = (double)t.width / t.height;
        if (s < 1.0 - 1e-6 || s > 4.0 + 1e-6 || fabs(a - ar) / ar > 0.02) continue;
        if (!c || t.width * t.height > c.width * c.height) { c = t; tc_v = g_treg[tid].t; tc = &tc_v; }
    }
    os_unfair_lock_unlock(&g_rmu);
    if (o) o = tensor_write_view(o);   // 写目标：不带通道重排（tensor_write_view）
    // 分屏规则（tensor_rect_io）：推常量给出的输入 / 输出区域成立、而且输出只占一块（或按整张输出图挑不出输入）才用；单人整屏照旧，零变化
    NSUInteger rio[8]; int io = 0;
    if (o && n2d == 1 && tensor_rect_io(cs, &t1_v, t1, to, o, rio) && (!c || rio[2] || rio[3] || rio[4] != o.width || rio[5] != o.height)) { io = 1; c = t1; tc_v = t1_v; tc = &tc_v; }
    if (!o || !c) {
        g_tensor_red++;
        if (!(cs->tensor_log & 1)) { cs->tensor_log |= 1; MLOG("【红】 张量核网络顶替：%s 写图 %lux%lu，读的贴图里没有宽高比一致、放大 1–4 倍的低分辨率画面，这一遍不写", cs->key, o ? (unsigned long)o.width : 0ul, o ? (unsigned long)o.height : 0ul); }
        // 满屏放宽下挑不出（#726：放宽的目标与没放宽的图宽高比对不上）：不同的帧里累计到 S2A_TENSOR_WIDEN_FRAMES 就本局退回 16:9（tensor_widen.h），不让输出图一直空着
        if (o && s2a_tensor_widen_revert_due(&g_tensor_widen, atomic_load(&g_cur_frame), s2a_screen_w() > 1920 || s2a_screen_h() > 1080)) {
            int fr = s2a_widen_force_native(1);
            MLOG("【红】 张量核网络顶替：满屏放宽（屏幕 %dx%d）下 %d 个不同的帧都挑不出对得上的输入（%s 写图 %lux%lu），本局退回原生 16:9（%s）", s2a_screen_w(), s2a_screen_h(), S2A_TENSOR_WIDEN_FRAMES, cs->key,
                 (unsigned long)o.width, (unsigned long)o.height, fr > 0 ? "已热切，下一个渲染遍起生效" : fr < 0 ? "游戏自己出宽图，退不了" : "已经是 16:9 或还没定屏，没变化");
        }
        return;
    }
    int dom = cs->tensor_sq ? 1 : cs->tensor_srgb ? 2 : 0;   // 出口层的输出域（文件头）：0 同域、1 平方、2 sRGB 解码
    if (!(cs->tensor_log & 2)) { cs->tensor_log |= 2; MLOG("张量核网络顶替：%s 不派发，读 %lux%lu（格式 %lu）→ 写 %lux%lu（格式 %lu）用 MetalFX 空间放大%s", cs->key, (unsigned long)c.width, (unsigned long)c.height, (unsigned long)c.pixelFormat, (unsigned long)o.width, (unsigned long)o.height, (unsigned long)o.pixelFormat,
        dom == 1 ? "；它的输出是读的画面逐分量带符号平方加残差（HMUL2 R, R, |R|），放大前先把输入平方" : dom == 2 ? "；它的输出是读的画面加残差后从 sRGB 解码成线性（HSETP2 R, 0.04045），放大前先把输入 sRGB 解码" : ""); }
    int icrop = io && (rio[6] || rio[7]);   // 分屏输入块不在左上角：先拷进临时图（tensor_rect_io）
    id<MTLTexture> crop = icrop ? tensor_in_tex(c.pixelFormat, rio[0], rio[1], MTLTextureUsageShaderRead) : nil;
    if (icrop && !crop) { g_tensor_red++; return; }
    id<MTLTexture> src = crop ? crop : c;   // 换域那一遍读的：输入本身或它的裁块
    NSUInteger rc[6] = {c.width, c.height, 0, 0, o.width, o.height};   // 输入内容宽高、输出区域（文件头「有效区域」）
    int rect = io ? (memcpy(rc, rio, sizeof rc), 1) : tensor_rect(cs, tc, c, to, o, rc);
    if (icrop && !(cs->tensor_log & 16)) { cs->tensor_log |= 16; MLOG("张量核网络顶替：%s 输入块从 (%lu,%lu) 起，先拷进 %lux%lu 临时图再放大", cs->key, (unsigned long)rio[6], (unsigned long)rio[7], (unsigned long)rio[0], (unsigned long)rio[1]); }
    if (rect && !(cs->tensor_log & 8)) { cs->tensor_log |= 8; MLOG("张量核网络顶替：%s 按本次推常量取有效区域：输入 %lux%lu 里的 %lux%lu → 输出 %lux%lu 的 (%lu,%lu) 起 %lux%lu", cs->key,
        (unsigned long)c.width, (unsigned long)c.height, (unsigned long)rc[0], (unsigned long)rc[1], (unsigned long)o.width, (unsigned long)o.height, (unsigned long)rc[2], (unsigned long)rc[3], (unsigned long)rc[4], (unsigned long)rc[5]); }
    int part = rc[2] || rc[3] || rc[4] != o.width || rc[5] != o.height;   // 输出只写一块：MetalFX 先写同尺寸临时图
    id<MTLTexture> ot = o;
    if (part && !(ot = tensor_out_tex(o.pixelFormat, rc[4], rc[5], MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget))) { g_tensor_red++; return; }
    int fuse_fix = dom && (src.width != ot.width || src.height != ot.height) && rc[0] == src.width && rc[1] == src.height;
    id<MTLTexture> in = nil; NvDlss *f = nil; id<MTLFXSpatialScalerBase> ss = nil;
    int fk = -1, need_clear = 0;
    if (fuse_fix) {
        f = tensor_scaler_for(MTLPixelFormatRGBA16Float, src.width, src.height, ot);
        if (!f) { g_tensor_red++; return; }
        ss = f->scaler;
        if (part && (ot.usage & ss.outputTextureUsage) != ss.outputTextureUsage) { g_tensor_red++; RED_ONCE(165, "张量核网络顶替：MetalFX 要的输出用途 %#lx 临时图没有，输出图这一遍不写", (unsigned long)ss.outputTextureUsage); return; }
        c = dlss_usage(tc, c, MTLTextureUsageShaderRead);
        if (!crop) src = c;
        if (part) o = dlss_usage(to, o, MTLTextureUsageShaderRead);
        else ot = o = dlss_usage(to, o, ss.outputTextureUsage);
        fk = tensor_fix_get(MTLPixelFormatRGBA16Float, f->iw, f->ih, rc[0], rc[1], ss.colorTextureUsage | MTLTextureUsageShaderWrite, &need_clear);
        if (fk < 0) { g_tensor_red++; return; }
        in = g_tensor_fix[fk].t;
        if ((in.usage & ss.colorTextureUsage) != ss.colorTextureUsage) { g_tensor_red++; RED_ONCE(164, "张量核网络顶替：MetalFX 要的输入用途 %#lx 平方 / 裁块临时图没有，输出图这一遍不写", (unsigned long)ss.colorTextureUsage); return; }
    } else {
        in = dom ? tensor_sq_tex(src.width, src.height) : src;   // what MetalFX reads: the input itself, its crop, or its signed square / sRGB decode (see the file header)
        if (!in) { g_tensor_red++; return; }
        f = tensor_scaler(in, ot);
        if (!f) { g_tensor_red++; return; }
        ss = f->scaler;
        if (part && (ot.usage & ss.outputTextureUsage) != ss.outputTextureUsage) { g_tensor_red++; RED_ONCE(165, "张量核网络顶替：MetalFX 要的输出用途 %#lx 临时图没有，输出图这一遍不写", (unsigned long)ss.outputTextureUsage); return; }
        if (in == c) in = c = dlss_usage(tc, c, ss.colorTextureUsage);
        else {
            c = dlss_usage(tc, c, MTLTextureUsageShaderRead);
            if (!crop) src = c;
            if ((in.usage & ss.colorTextureUsage) != ss.colorTextureUsage) { g_tensor_red++; RED_ONCE(164, "张量核网络顶替：MetalFX 要的输入用途 %#lx 平方 / 裁块临时图没有，输出图这一遍不写", (unsigned long)ss.colorTextureUsage); return; }
        }
        if (part) o = dlss_usage(to, o, MTLTextureUsageShaderRead);   // 拷贝目标：拷贝不要额外用途，只保证它已解析
        else ot = o = dlss_usage(to, o, ss.outputTextureUsage);
    }
    zskip_del(to->serial);
    pkg_gpu_wrote(to, 0, 0, 1, 0, 0, (uint32_t)to->d.width, (uint32_t)to->d.height);
    if (g_wd > 0) { pthread_mutex_lock(&g_wd_mu); s2a_widen_gpu_wrote((uint32_t)to->serial, (uint32_t)tc->serial); pthread_mutex_unlock(&g_wd_mu); }
    rp_flush();
    int had_barrier = 0;
    if (crop) {   // 裁块：把输入块拷进临时图左上角，拷贝编码器关了即全屏障，换域那一遍 / MetalFX 读到拷完的临时图
        enc_b_copy_texture(blit(), c, 0, 0, MTLOriginMake(rio[6], rio[7], 0), MTLSizeMake(rio[0], rio[1], 1), crop, 0, 0, MTLOriginMake(0, 0, 0));
        enc_close_blit(); had_barrier = 1;
    }
    if (fuse_fix) {
        if (need_clear) { tensor_fix_clear_border(blit(), fk, f->iw, f->ih, rc[0], rc[1]); enc_close_blit(); }
        if (!tensor_sq_pass(src, in, dom)) { g_tensor_red++; return; }
        g_tensor_fix_runs++; had_barrier = 1;
    } else {
        if (dom) {   // 平方 / sRGB 解码这一遍：enc_c_begin/end 自带前后队列全屏障，MetalFX 直接读到写完的临时图
            if (!tensor_sq_pass(src, in, dom)) { g_tensor_red++; return; }
            had_barrier = 1;
        }
        if (in.width != f->iw || in.height != f->ih) {   // 输入图不等于放大器输入容量：有效区域拷进定尺寸输入图（tensor_fix_in）
            if (!(in = tensor_fix_in(in, f->iw, f->ih, rc[0], rc[1], ss.colorTextureUsage))) { g_tensor_red++; return; }
            had_barrier = 1;
        }
    }
    if (!had_barrier) { (void)blit(); enc_close_blit(); }   // 前一侧全屏障（同 dlss_eval）
    uint64_t a[15] = {0}; a[11] = rc[0]; a[12] = rc[1]; a[13] = 1;   // 输入内容宽高：有效区域（不成立 / 这一族以外 = 整张输入图）
    if (rec_on()) { rec_t *r = rec_put(RO_FX, 5); memcpy(r->a, a, sizeof a); r->a[0] = rec_obj(f->scaler); r->a[1] = rec_obj(in); r->a[2] = rec_obj(nil); r->a[3] = rec_obj(nil); r->a[4] = rec_obj(ot); }
    else { a[0] = (uint64_t)(__bridge void *)f->scaler; a[1] = (uint64_t)(__bridge void *)in; a[4] = (uint64_t)(__bridge void *)ot; dlss_fx_run(g_cb, a); }
    if (part) {   // 拷贝编码器开时等此前全部工作（含 MetalFX），把临时图拷到输出区域起点
        enc_b_copy_texture(blit(), ot, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(rc[4], rc[5], 1), o, 0, 0, MTLOriginMake(rc[2], rc[3], 0));
        enc_close_blit();
    }
    if (rect) g_tensor_rect_runs++;
    g_last_end_relaxed = 0; main_barrier_reset();
    g_tensor_up++;
}
