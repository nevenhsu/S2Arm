// 计算派发的屏障按读写范围决定（nvn_mtl.m disp_ranges / disp_hits / disp_note）。不开 GPU：只测「范围算得对不对、该不该放屏障」。
// 期望全部手算：存储缓冲槽读 + 写、统一缓冲槽只读；读碰到此前的写、写碰到此前的写或读 → 要屏障；互不相交 → 不要。
// 两遍（runtime/tests/mk/nvn_dispatch_range.mk）：DM_NVN_DISPATCH_RANGE=range 测派发之间按范围（上一版）；缺省测按游戏屏障（马车世界缺口 ⑥）：
//   派发之间只在游戏发过屏障时排，碰到拷贝的范围照排；派发的读写另记，之后的拷贝碰到要排。反证：去掉 disp_hits 里看 g_dsp_game_bar 那行 → 丙红
#ifndef NVN_PRODUCER_SOURCE
#define NVN_PRODUCER_SOURCE "../gfx/nvn/nvn_mtl.m"
#endif
#include NVN_PRODUCER_SOURCE
static int fails;
static void expect(const char *what, long want, long got) { printf("%s 期望 %ld → 真结果 %ld\n", what, want, got); if (want != got) fails++; }
static NvShader *prog(int n, const nvn_rbind *b) { NvShader *s = [NvShader new]; s->rf.nb = n; s->rf.b = calloc((size_t)n, sizeof *b); memcpy(s->rf.b, b, (size_t)n * sizeof *b); return s; }
static void reset(void) { g_bw_n = g_br_n = g_dw_n = g_dr_n = 0; g_blit_ops = 0; g_dsp_game_bar = 0; memset(&S, 0, sizeof S); }
static int game_mode(void) {   // 缺省：按游戏屏障
    disp_rng r;
    nvn_rbind b1[3] = {{.kind = NVN_RB_SBUFP, .reg = 0}, {.kind = NVN_RB_CBUF, .reg = 1}, {.kind = NVN_RB_SUPPORT}};
    NvShader *A = prog(3, b1);
    expect("模式：按游戏屏障", 2, disp_mode());
    reset(); S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1000, 0x100}; S.ubo[5][1] = (p_bind_buf){5, 1, 0x9000, 0x40};
    disp_ranges(A, 0, &r); disp_note(&r);
    expect("甲记下：派发写表 1 条", 1, g_dw_n); expect("派发读表 2 条", 2, g_dr_n); expect("拷贝写表不动", 0, g_bw_n); expect("拷贝读表不动", 0, g_br_n); expect("操作数 1", 1, g_blit_ops);
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1080, 0x10};
    disp_ranges(A, 0, &r); expect("乙读甲写过的段、游戏没发屏障：不要屏障", 0, disp_hits(&r));
    g_dsp_game_bar = 1; uint64_t gb0 = g_disp_gbar;
    expect("丙同一次派发、游戏发过屏障：要屏障", 1, disp_hits(&r)); expect("游戏屏障计数 +1", 1, (long)(g_disp_gbar - gb0)); g_dsp_game_bar = 0;
    expect("丁拷贝读甲写过的段：要屏障", 1, blit_conflict(0x1080, 0x1090, 0x7000, 0x7010));
    expect("丁拷贝写甲读过的统一缓冲：要屏障", 1, blit_conflict(0, 0, 0x9010, 0x9020));
    expect("丁拷贝与甲不相交：不要屏障", 0, blit_conflict(0x20000, 0x20100, 0x30000, 0x30100));
    g_bw_lo[0] = 0x5000; g_bw_hi[0] = 0x5100; g_bw_n = 1; g_br_lo[0] = 0x6000; g_br_hi[0] = 0x6100; g_br_n = 1;   // 一次拷贝：读 0x6000 写 0x5000
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x40000, 0x10}; S.ubo[5][1] = (p_bind_buf){5, 1, 0x5040, 0x10};
    disp_ranges(A, 0, &r); expect("戊派发读拷贝写过的段：要屏障", 1, disp_hits(&r));
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x6080, 0x10}; S.ubo[5][1] = (p_bind_buf){5, 1, 0xA000, 0x10};
    disp_ranges(A, 0, &r); expect("戊派发写拷贝读过的段：要屏障", 1, disp_hits(&r));
    g_bw_n = g_br_n = 0;
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1000, 0x100}; S.ubo[5][1] = (p_bind_buf){5, 1, 0x9000, 0x40};
    disp_ranges(A, 0, &r); disp_note(&r); expect("己同一段再记：派发写表仍 1 条", 1, g_dw_n); expect("派发读表仍 2 条", 2, g_dr_n);
    g_dw_n = DW_MAX; expect("庚派发写表已满：要屏障", 1, disp_hits(&r));
    // 辛：未知读写范围的派发（disp_need_barrier(NULL)）在按游戏屏障模式下不放屏障、写 g_dw 通配；拷贝碰它照放、游戏发过屏障或有在先拷贝才放
    reset();
    g_dw_lo[0] = 0; g_dw_hi[0] = ~0ull; g_dw_n = 1; g_blit_ops = 1;
    expect("辛未知范围连发且无游戏屏障/无拷贝：不要屏障", 0, disp_need_barrier(NULL));
    expect("辛有界派发紧随未知范围且无游戏屏障/无拷贝：不要屏障", 0, disp_need_barrier(&r));
    disp_note(&r);
    expect("辛通配已覆盖时不重记 g_dw：仍 1 条", 1, g_dw_n);
    expect("辛拷贝碰未知范围派发：要屏障", 1, blit_conflict(0x1000, 0x1100, 0x2000, 0x2100));
    g_bw_lo[0] = 0x5000; g_bw_hi[0] = 0x5100; g_bw_n = 1;
    expect("辛有在先拷贝时未知范围派发：要屏障", 1, disp_need_barrier(NULL));
    g_bw_n = 0; g_dsp_game_bar = 1; uint64_t gb1 = g_disp_gbar;
    expect("辛游戏发过屏障时未知范围派发：要屏障", 1, disp_need_barrier(NULL));
    expect("辛未知范围命中游戏屏障计数 +1", 1, (long)(g_disp_gbar - gb1));
    if (fails) printf("✗ t_nvn_dispatch_range（按游戏屏障）：%d 项不符\n", fails); else printf("✓ t_nvn_dispatch_range（按游戏屏障）：派发之间看游戏屏障、与拷贝之间看范围、未知范围、去重、表满\n");
    return fails != 0;
}
int main(void) { @autoreleasepool {
    { const char *m = getenv("DM_NVN_DISPATCH_RANGE"); if (!m || strcmp(m, "range")) return game_mode(); }
    disp_rng r;
    // 甲：存储槽 0（0x1000 起 0x100 字节）+ 统一槽 1（0x9000 起 0x40）
    nvn_rbind b1[3] = {{.kind = NVN_RB_SBUFP, .reg = 0}, {.kind = NVN_RB_CBUF, .reg = 1}, {.kind = NVN_RB_SUPPORT}};
    NvShader *A = prog(3, b1);
    reset(); S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1000, 0x100}; S.ubo[5][1] = (p_bind_buf){5, 1, 0x9000, 0x40};
    expect("甲：范围算得出", 1, disp_ranges(A, 0, &r));
    expect("甲：写范围个数", 1, r.nw); expect("甲：读范围个数（存储槽 + 统一槽）", 2, r.nr);
    expect("甲：写范围起", 0x1000, (long)r.wlo[0]); expect("甲：写范围止", 0x1100, (long)r.whi[0]);
    expect("甲：统一槽读范围止", 0x9040, (long)r.rhi[1]);
    expect("前面没有操作：不要屏障", 0, g_blit_ops && disp_hits(&r)); disp_note(&r);
    expect("记下之后：写范围 1 条", 1, g_bw_n); expect("读范围 2 条", 2, g_br_n); expect("操作数 1", 1, g_blit_ops);
    // 乙：另一个发射器，存储槽 0 换到 0x2000（不相交）、同一个统一槽（只读对只读）
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x2000, 0x100};
    expect("乙：范围算得出", 1, disp_ranges(A, 0, &r)); expect("乙与甲互不相交：不要屏障", 0, disp_hits(&r)); disp_note(&r);
    // 丙：读甲写过的那段（存储槽 0 = 0x1080 起 0x10，落在甲的写范围里）
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1080, 0x10};
    expect("丙：范围算得出", 1, disp_ranges(A, 0, &r)); expect("丙碰到甲的写：要屏障", 1, disp_hits(&r));
    // 丁：写别人读过的统一缓冲（存储槽 0 = 0x9020 起 0x10，落在统一槽的读范围里）
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x9020, 0x10};
    expect("丁：范围算得出", 1, disp_ranges(A, 0, &r)); expect("丁写到此前读过的范围：要屏障", 1, disp_hits(&r));
    // 首尾相接不算相交：0x1100 起
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1100, 0x10}; S.ubo[5][1] = (p_bind_buf){5, 1, 0xA000, 0x40};
    expect("首尾相接：范围算得出", 1, disp_ranges(A, 0, &r)); expect("首尾相接不相交：不要屏障", 0, disp_hits(&r));
    // 算不出的情形：一律回 0（照旧每次放屏障）
    expect("间接派发：算不出", 0, disp_ranges(A, 1, &r));
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1000, 0};
    expect("存储槽绑了但大小为 0：算不出", 0, disp_ranges(A, 0, &r));
    S.ssbo[5][0] = (p_bind_buf){5, 0, 0, 0};
    expect("存储槽没绑：算得出、没有写范围", 1, disp_ranges(A, 0, &r) && r.nw == 0);
    nvn_rbind bi[2] = {{.kind = NVN_RB_SBUFP, .reg = 0}, {.kind = NVN_RB_IMG, .reg = 0}};
    expect("带图像绑定：算不出", 0, disp_ranges(prog(2, bi), 0, &r));
    nvn_rbind bg[1] = {{.kind = NVN_RB_GMEM}};
    expect("带全局内存绑定：算不出", 0, disp_ranges(prog(1, bg), 0, &r));
    nvn_rbind bu[1] = {{.kind = NVN_RB_SBUFU}};
    expect("带追不到槽的存储缓冲：算不出", 0, disp_ranges(prog(1, bu), 0, &r));
    nvn_rbind bt[1] = {{.kind = NVN_RB_TBUF}};
    expect("带贴图缓冲：算不出", 0, disp_ranges(prog(1, bt), 0, &r));
    expect("没有程序：算不出", 0, disp_ranges(nil, 0, &r));
    nvn_rbind bh[4] = {{.kind = NVN_RB_LB}, {.kind = NVN_RB_DRV0}, {.kind = NVN_RB_DRV2}, {.kind = NVN_RB_TEX}};
    expect("只带宿主自己的表和只读贴图：算得出、没有范围", 1, disp_ranges(prog(4, bh), 0, &r) && r.nw == 0 && r.nr == 0);
    // 表快满：放不下就要屏障
    reset(); S.ssbo[5][0] = (p_bind_buf){5, 0, 0x1000, 0x100}; S.ubo[5][1] = (p_bind_buf){5, 1, 0x9000, 0x40}; disp_ranges(A, 0, &r);
    g_bw_n = BW_MAX; g_blit_ops = 1;
    expect("写范围表已满：要屏障", 1, disp_hits(&r));
    // 关掉开关：一律算不出
    setenv("DM_NVN_DISPATCH_RANGE", "0", 1);
    // 开关只读一次环境变量，这里不再重测（另一进程测）
    if (fails) printf("✗ t_nvn_dispatch_range：%d 项不符\n", fails); else printf("✓ t_nvn_dispatch_range：派发读写范围、相交判断、算不出的各种情形\n");
    return fails != 0; } }
