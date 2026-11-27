// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真池表、脏页跟踪、镜像解析、提交顺序和 Vulkan 后端。
// 夹具只安排三笔绘制的位置和着色器；回放核心决定前置段和拆遍。
#define main backend_fixture_main
#define nvn_plat_flag backend_fixture_flag
#include "t_backend_vk.c"
#undef nvn_plat_flag
#undef main
__typeof__(backend_fixture_flag) nvn_plat_flag;
#include "rp_exec.c"
#include "nvn_dirty.h"
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>

static nvn_gpu_err test_err;
static int hoist, pass_open, passes;
static void (*prepare_write)(void *, size_t);

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "mirror fixture line %d: %s (%d %s)\n", __LINE__, #expr, test_err.rc.code, test_err.why); \
    exit(2); \
} } while (0)

int nvn_plat_flag(const char *key, int dflt) {
    if (!strcmp(key, "nvk_mirror_disjoint")) return hoist;
    return backend_fixture_flag(key, dflt);
}

static int (*dirty_fault)(void *, int, void *);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { dirty_fault = fn; REQUIRE(fn); }
// 只允许完成回调写入本臂指定的物理字时认领一次缺页。宿主可能把受保护
// 的写报告为 MAPERR 或 ACCERR，交给真实脏页钩子判定它是否能释放这页。
static _Atomic(uintptr_t) landing_word;
static void alias_page_fault(int sig, siginfo_t *info, void *context) {
    uintptr_t address = (uintptr_t)info->si_addr;
    if (info->si_code > 0 && address && dirty_fault &&
        atomic_compare_exchange_strong(&landing_word, &address, 0) &&
        dirty_fault(info->si_addr, 1, context)) return;
    _Exit(128 + sig);
}
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { prepare_write = fn; }

// 本夹具不录计数器报告、调试转储或贴图快照。若意外进入其失败/回落路径，直接报错。
void rp_debug_dump_poll(uint64_t *fence) { (void)fence; }
void rp_res_xfer_aborted(uint64_t sub) { (void)sub; REQUIRE(0); }
void rp_tex_async_step(nvn_gpu_cmd c) { (void)c; }
uint64_t rp_gpu_wr_gen;
void rp_draw_stats(uint64_t *draws, uint64_t *skips) {
    if (draws) *draws = 0;
    if (skips) *skips = 0;
}
int s2a_guest_tick_from_host_ns(uint64_t ns, uint64_t *out) { (void)ns; (void)out; REQUIRE(0); return 0; }
uint64_t s2a_guest_tick(void) { REQUIRE(0); return 0; }
int s2a_nvn_render_cond(void) { REQUIRE(0); return 0; }

// 只替代游戏的通道规划。切遍要真实结束 VkRenderPass，续遍用 LOAD 保住第一笔像素。
int rp_pass_is_open(void) { return pass_open; }
void rp_pass_end(nvn_gpu_cmd c) {
    if (pass_open) { nvn_gpu_pass_end(c); pass_open = 0; }
}
void rp_pass_flush(nvn_gpu_cmd c) { rp_pass_end(c); }

enum { WIDTH = 4, HEIGHT = 4 };
static void write_vertices(uint8_t *dst, int green) {
    const float vertices[][8] = {
        {-1, -1, 0, 1, !green, green, 0, 1},
        { 3, -1, 0, 1, !green, green, 0, 1},
        {-1,  3, 0, 1, !green, green, 0, 1},
    };
    prepare_write(dst, sizeof vertices);
    memcpy(dst, vertices, sizeof vertices);
}

static void draw_column(nvn_gpu_view view, nvn_gpu_pipe pipe, const void *guest, unsigned col) {
    nvn_gpu_buf mirror = NULL;
    uint64_t off = 0;
    REQUIRE(rp_buf_resolve(g_cmd, (uint64_t)(uintptr_t)guest, 3 * 8 * sizeof(float), &mirror, &off));
    if (!pass_open) {
        nvn_gpu_pass_desc pass = {0};
        pass.ncolor = 1;
        pass.color[0].view = view;
        pass.color[0].load = passes ? NVN_GPU_LOAD_LOAD : NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[3] = 1;
        pass.w = WIDTH; pass.h = HEIGHT; pass.layers = 1;
        nvn_gpu_pass_begin(g_cmd, &pass);
        pass_open = 1;
        passes++;
    }
    nvn_gpu_dyn dyn = {0};
    dyn.origin_ll = 1; dyn.nvp = 1; dyn.has_sc = 1;
    dyn.vp[0][2] = WIDTH; dyn.vp[0][3] = HEIGHT; dyn.vp[0][5] = 1;
    dyn.sc[0] = col; dyn.sc[2] = 1; dyn.sc[3] = HEIGHT;
    nvn_gpu_set_pipe(g_cmd, pipe);
    nvn_gpu_set_dyn(g_cmd, &dyn);
    nvn_gpu_set_vbuf(g_cmd, 0, mirror, off);
    const nvn_gpu_draw_args draw = {.prim = 4, .count = 3, .instances = 1};
    nvn_gpu_draw(g_cmd, &draw);
}

static void commit_mirror(void) {
    uint64_t fence = nvn_be_sub_commit();
    REQUIRE(OK(nvn_gpu_fence_wait(fence, 5000000000ull, &test_err)));
    REQUIRE(rp_fence_done(fence) && !nvn_gpu_fence_failed());
}

static void copy_guest_word(const uint32_t *word, nvn_gpu_buf out, uint64_t offset) {
    nvn_gpu_buf src = NULL;
    uint64_t src_off = 0;
    REQUIRE(rp_buf_resolve(g_cmd, (uintptr_t)word, sizeof *word, &src, &src_off));
    nvn_gpu_copy_buf(g_cmd, src, src_off, out, offset, sizeof *word);
}

static void check_alias_direction(const char *name, uint32_t *writer, uint32_t *reader,
                                  const uint32_t *guard, uint32_t *physical, int gpu, uint32_t marker,
                                  nvn_gpu_buf readback) {
    const size_t word = sizeof *writer;
    // 提前把两端的旧值读到 GPU；首次分配时从 CPU 上传不能充当别名同步。
    nvn_be_sub_begin();
    copy_guest_word(writer, readback, 0);
    copy_guest_word(reader, readback, word);
    commit_mirror();
    const uint8_t *bytes = nvn_gpu_buf_cpu(readback);
    uint32_t seen[3];
    REQUIRE(bytes);
    memcpy(seen, bytes, 2 * word);
    REQUIRE(seen[0] == 0 && seen[1] == 0);
    nvn_be_sub_begin();
    if (gpu) {
        nvn_gpu_buf dst = NULL;
        uint64_t off = 0;
        REQUIRE(rp_buf_resolve(g_cmd, (uintptr_t)writer, word, &dst, &off));
        REQUIRE(rp_post_gpu_write((uintptr_t)writer, word));
        CHECK(rp_buf_gpu_pending((uintptr_t)reader, word) && !rp_buf_gpu_pending((uintptr_t)guard, word),
              "%s: pending write range did not follow the mapped physical word", name);
        nvn_gpu_fill_buf(g_cmd, dst, off, word, marker);
    } else {
        prepare_write(writer, word);
        *writer = marker;
        REQUIRE(*reader == marker);  // 先证两 CPU VA 确实指向同一文件页。
    }
    copy_guest_word(reader, readback, 0);
    copy_guest_word(writer, readback, word);  // 同地址正控
    copy_guest_word(guard, readback, 2 * word);  // 另一物理页、同页内偏移
    if (gpu) {
        REQUIRE(*physical == 0);  // GPU 写尚未提交，别名读回不能从客体内存取得 marker。
        atomic_store(&landing_word, (uintptr_t)physical);
    }
    commit_mirror();
    atomic_store(&landing_word, 0);
    memcpy(seen, bytes, sizeof seen);
    printf("VPOOL_ALIAS arm=%s expected=%08x cross=%08x same=%08x guard=%08x landed=%08x\n",
           name, marker, seen[0], seen[1], seen[2], *reader);
    CHECK(seen[0] == marker, "%s: GPU read stale bytes through alias", name);
    CHECK(seen[1] == marker && seen[2] == 0x71717171 && *reader == marker,
          "%s: same-address read, disjoint guard or CPU landing failed", name);
}

static void test_aligned_aliases(void) {
    const size_t page = nvn_dirty_page_size();
    FILE *file = tmpfile();
    REQUIRE(file && !ftruncate(fileno(file), 2 * page));
    uint32_t *phys = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), 0);
    uint32_t *va = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), page);
    uint32_t *vb = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), page);
    REQUIRE(phys != MAP_FAILED && va != MAP_FAILED && vb != MAP_FAILED);
    struct sigaction action = {.sa_sigaction = alias_page_fault, .sa_flags = SA_SIGINFO}, old_action;
    REQUIRE(atomic_is_lock_free(&landing_word));
    sigemptyset(&action.sa_mask);
    REQUIRE(!sigaction(SIGSEGV, &action, &old_action));
    uint32_t *mapped = phys + page / sizeof *phys;
    memset(phys, 0x71, page);
    mapped[0] = 0x12345678;
    REQUIRE(va[0] == mapped[0] && vb[0] == mapped[0]);
    // 对齐 MapVirtual 的回放入口：物理池登记脏页，两个虚拟 run 各有独立 VA。
    REQUIRE(nvn_dirty_log_configure(1));
    nvn_be_pool_add(phys, 2 * page);
    nvn_dirty_add_range(phys, 2 * page);
    REQUIRE(rp_buf_alias_set((uintptr_t)va, page, (uintptr_t)mapped));
    nvn_be_pool_add(va, page);
    REQUIRE(rp_buf_alias_set((uintptr_t)vb, page, (uintptr_t)mapped));
    nvn_be_pool_add(vb, page);
    REQUIRE(prepare_write && nvn_dirty_tracked(phys, 2 * page));
    nvn_gpu_buf out = make_buf(3 * sizeof *phys, NVN_GPU_MEM_SHARED, NULL);
    REQUIRE(out);
    check_alias_direction("gpu-virtual-physical", va + 1, mapped + 1, phys + 1, mapped + 1, 1, 0x23456781, out);
    check_alias_direction("gpu-physical-virtual", mapped + 2, vb + 2, phys + 2, mapped + 2, 1, 0x34567812, out);
    check_alias_direction("gpu-virtual-virtual", va + 3, vb + 3, phys + 3, mapped + 3, 1, 0x45678123, out);
    check_alias_direction("cpu-virtual-physical", vb + 4, mapped + 4, phys + 4, mapped + 4, 0, 0x56781234, out);
    check_alias_direction("cpu-physical-virtual", mapped + 5, va + 5, phys + 5, mapped + 5, 0, 0x67812345, out);
    // 将一个别名改映到物理第一页，另一个仍映到第二页。批量解析和单段解析都读实字节。
    REQUIRE(mmap(vb, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fileno(file), 0) == vb);
    REQUIRE(rp_buf_alias_set((uintptr_t)vb, page, (uintptr_t)phys));
    uint64_t addr[] = {(uintptr_t)(vb + 2), (uintptr_t)(va + 2)};
    const uint64_t len[] = {4, 4};
    nvn_gpu_buf bufs[2];
    uint64_t offsets[2];
    unsigned char ok[2];
    nvn_be_sub_begin();
    REQUIRE(rp_buf_resolve_multi(2, addr, len, bufs, offsets, ok) && ok[0] && ok[1]);
    for (int i = 0; i < 2; i++) nvn_gpu_copy_buf(g_cmd, bufs[i], offsets[i], out, 4 * i, 4);
    commit_mirror();
    uint32_t remapped[2];
    memcpy(remapped, nvn_gpu_buf_cpu(out), sizeof remapped);
    printf("VPOOL_ALIAS remap expected=71717171,34567812 actual=%08x,%08x\n", remapped[0], remapped[1]);
    CHECK(remapped[0] == 0x71717171 && remapped[1] == 0x34567812, "alias remap changed the other alias");
    CHECK(!nvn_unimpl_kinds(), "alias: replay reported unsupported calls");
    nvn_gpu_buf_release(out, NULL, NULL);
    REQUIRE(rp_buf_alias_set((uintptr_t)vb, page, 0));
    REQUIRE(rp_buf_alias_set((uintptr_t)va, page, 0));
    nvn_be_pool_del(vb);
    nvn_be_pool_del(va);
    nvn_be_pool_del(phys);
    nvn_dirty_remove_range(phys);
    REQUIRE(!munmap(vb, page) && !munmap(va, page) && !munmap(phys, 2 * page));
    REQUIRE(!fclose(file));
    REQUIRE(!sigaction(SIGSEGV, &old_action, NULL));
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    const char *arm = argv[2];
    const int overlap = !strcmp(arm, "overlap");
    const int pending = !strcmp(arm, "pending");
    const int off = !strcmp(arm, "off");
    const int alias = !strcmp(arm, "alias");
    REQUIRE(alias || overlap || pending || off || !strcmp(arm, "gap"));
    hoist = !off;
    char why[256] = {0};
    REQUIRE(!s2a_vk_device_create(&g_dev, why, sizeof why));
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "replay-mirror-hoist";
    REQUIRE(open_backend());

    if (alias) {
        test_aligned_aliases();
        CHECK(OK(nvn_gpu_close(5000000000ull, &test_err)), "backend close failed: %s", test_err.why);
        CHECK(!s2a_vk_device_destroy(&g_dev, 1), "device destroy failed");
        return g_fail ? 1 : 0;
    }

    const size_t page = nvn_dirty_page_size(), bytes = 8 * page;
    uint8_t *guest = NULL;
    REQUIRE(!posix_memalign((void **)&guest, page, bytes));
    memset(guest, 0, bytes);
    REQUIRE(nvn_dirty_log_configure(1));
    nvn_dirty_add_range(guest, bytes);
    nvn_be_pool_add(guest, bytes);
    REQUIRE(prepare_write && nvn_dirty_tracked(guest, bytes));
    uint8_t *const target = guest + 3 * page;
    write_vertices(guest, 0);
    write_vertices(guest + 6 * page, 0);
    write_vertices(target, 0);
    write_vertices(target + 128, 0);

    nvn_gpu_buf mirror = NULL;
    nvn_be_sub_begin();
    REQUIRE(rp_buf_resolve(g_cmd, (uint64_t)(uintptr_t)guest, bytes, &mirror, NULL));
    commit_mirror();

    nvn_gpu_pipe pipe = color_pipe(argv[1], 0);
    nvn_gpu_tex tex = make_tex(WIDTH, HEIGHT, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view view = make_view(tex);
    const size_t pixel_bytes = WIDTH * HEIGHT * 4;
    nvn_gpu_buf readback = make_buf(pixel_bytes, NVN_GPU_MEM_SHARED, NULL);
    REQUIRE(pipe && tex && view && readback);
    nvn_be_sub_begin();
    if (pending) {
        const uint64_t gpu_off = 5 * page;
        const uint32_t marker = 0xdecafbad;
        // 完成回调在宿主写回；把目标页开放给它，回写数据仍必须来自 GPU。
        prepare_write(guest + gpu_off, sizeof marker);
        REQUIRE(rp_post_gpu_write((uint64_t)(uintptr_t)(guest + gpu_off), sizeof marker));
        nvn_gpu_buf_write(g_cmd, mirror, gpu_off, &marker, sizeof marker);
        REQUIRE(rp_buf_gpu_pending((uint64_t)(uintptr_t)(guest + gpu_off), sizeof marker));
    }
    // [第 0 页，第 6 页] 的老桶包络罩住第 3 页。overlap 臂再让第一笔真实读取第 3 页。
    draw_column(view, pipe, overlap ? target : guest, 0);
    draw_column(view, pipe, guest + 6 * page, 1);
    if (overlap) write_vertices(target, 1);
    write_vertices(target + 128, 1);
    draw_column(view, pipe, target + 128, 2);
    const int pre = g_pre != NULL;
    rp_pass_end(g_cmd);
    nvn_gpu_copy_tex_buf(g_cmd, tex, 0, 0, NULL, readback, 0, WIDTH * 4, pixel_bytes);
    commit_mirror();

    static const uint8_t row[WIDTH][4] = {{255, 0, 0, 255}, {255, 0, 0, 255},
                                           {0, 255, 0, 255}, {0, 0, 0, 255}};
    const uint8_t *pixels = nvn_gpu_buf_cpu(readback);
    REQUIRE(pixels);
    unsigned bad = 0;
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) bad += memcmp(pixels + 4 * (y * WIDTH + x), row[x], 4) != 0;
    uint32_t landed = 0;
    memcpy(&landed, guest + 5 * page, sizeof landed);
    printf("MIRROR_HOIST arm=%s pre=%d passes=%d draw1=%02x%02x%02x%02x bad_pixels=%u landed=%08x\n",
           arm, pre, passes, pixels[0], pixels[1], pixels[2], pixels[3], bad, landed);
    const int want_pre = !off && !overlap && !pending;
    CHECK(pre == want_pre && passes == 2 - want_pre, "%s: wrong pre-submit placement / pass count", arm);
    CHECK(!bad, "%s: rendered bytes changed", arm);
    CHECK(landed == (pending ? 0xdecafbadu : 0), "%s: GPU writeback changed", arm);
    CHECK(!nvn_unimpl_kinds(), "%s: replay reported unsupported calls", arm);

    nvn_gpu_buf_release(readback, NULL, NULL);
    nvn_gpu_view_release(view);
    nvn_gpu_tex_release(tex);
    nvn_be_pool_del(guest);
    nvn_dirty_remove_range(guest);
    free(guest);
    CHECK(OK(nvn_gpu_close(5000000000ull, &test_err)), "backend close failed: %s", test_err.why);
    CHECK(!s2a_vk_device_destroy(&g_dev, 1), "device destroy failed");
    return g_fail ? 1 : 0;
}
