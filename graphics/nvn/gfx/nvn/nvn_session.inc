// Included after the backend implementation. Only the session coordinator calls
// these after joining every guest thread. Device/compiler caches stay reusable;
// guest-address tables and all resources wrapping guest memory do not.
static struct timespec session_deadline(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 10; return t;
}
int nvn_be_request_stop(void) {
    os_unfair_lock_lock(&g_shpre_mu);
    atomic_store(&g_shpre_stopping, 1);
    os_unfair_lock_unlock(&g_shpre_mu);
    atomic_store(&g_present_discard, 1);
    atomic_store(&g_link_stop, 1);
    pthread_mutex_lock(&g_pq_mu);
    g_pq_read = g_pq_in; // queued frames have not been submitted and can be dropped
    pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_link_mu); pthread_cond_broadcast(&g_link_cv); pthread_mutex_unlock(&g_link_mu);
    return 0;
}
int nvn_be_present_shutdown(void) {
    nvn_be_request_stop();
    struct timespec dl = session_deadline();
    pthread_mutex_lock(&g_pq_mu);
    while (g_link_started || g_classic_busy || g_pq_inflight) {
        link_stop_locked();
        if (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&g_pq_mu); MLOG("【红】重启：上屏线程/完成回调未排空"); return -ETIMEDOUT;
        }
    }
    g_pq_done = g_pq_in;
    pthread_mutex_unlock(&g_pq_mu);
    link_join();
    g_prefetched_drawable = nil;
    if (@available(macOS 26.0, iOS 26.0, *)) {
        if (g_present_layer_resident && !g_m3) [g_q4 removeResidencySet:g_present_layer_resident.residencySet];
    }
    g_present_layer_resident = nil;
    atomic_store_explicit(&g_present_out_w, 0, memory_order_relaxed);
    atomic_store_explicit(&g_present_out_h, 0, memory_order_relaxed);
    return 0;
}
static int session_wait_backend(void) {
    struct timespec dl = session_deadline();
    // Both lane sets (the prefetch and the lookahead's loads): stopping is set and enqueues happen under g_shpre_mu, so no load can be queued after these markers
    dispatch_group_t prefetch_done = dispatch_group_create();
    os_unfair_lock_lock(&g_shpre_mu);
    shq_mark(&g_shpre, prefetch_done); shq_mark(&g_shurg, prefetch_done);
    for (int i = 0; i < PAIRPRE_LANES; i++) if (g_pairpre_q[i]) dispatch_group_async(prefetch_done, g_pairpre_q[i], ^{});   // 登记时先取拉取基础管线的四条道（nvn_be_pair_prefetch）：停时等预热的循环也看 g_shpre_stopping
    os_unfair_lock_unlock(&g_shpre_mu);
    if (dispatch_group_wait(prefetch_done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) return -ETIMEDOUT;
    // A guest submit returns only after publishing its encode records. No
    // producer remains here; neither queue can gain new work after this point.
    pthread_mutex_lock(&g_rb_mu);
    while (g_rb_head || g_rb_busy) {
        if (pthread_cond_timedwait(&g_rb_idle, &g_rb_mu, &dl) == ETIMEDOUT) { pthread_mutex_unlock(&g_rb_mu); return -ETIMEDOUT; }
    }
    pthread_mutex_unlock(&g_rb_mu);
    if (atomic_load(&g_ubc_done) != atomic_load(&g_ubc_head) || g_cb || g_up_cb) return -EBUSY;
    if (g_nubd) { MLOG("【红】重启：仍有 %d 项常量更新未落地，保留旧会话", g_nubd); return -EBUSY; }
    pthread_mutex_lock(&g_mu);
    while (g_done < g_fence) {
        if (pthread_cond_timedwait(&g_cv, &g_mu, &dl) == ETIMEDOUT) { pthread_mutex_unlock(&g_mu); return -ETIMEDOUT; }
    }
    pthread_mutex_unlock(&g_mu);
    uint64_t end = now_ns() + 10000000000ull;
    while (atomic_load(&g_up_pending) || atomic_load(&g_warm_running)) {
        if (now_ns() >= end) return -ETIMEDOUT;
        usleep(1000);
    }
    pthread_mutex_lock(&g_pp_mu);
    for (;;) {
        int busy = 0; for (int i = 0; i < PSO_PEND_MAX; i++) busy |= g_pp[i].used && !g_pp[i].done;
        if (!busy) break;
        if (pthread_cond_timedwait(&g_pp_cv, &g_pp_mu, &dl) == ETIMEDOUT) { pthread_mutex_unlock(&g_pp_mu); return -ETIMEDOUT; }
    }
    pthread_mutex_unlock(&g_pp_mu);
    if (g_feedback_q) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(g_feedback_q, ^{ dispatch_semaphore_signal(done); });
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) return -ETIMEDOUT;
    }
    return 0;
}
static void session_resource_release(void *v) {
    id o = (__bridge id)v;
    if ([o conformsToProtocol:@protocol(MTLResource)]) res_remove(o);
    CFRelease(v);
}
int nvn_be_drain_session(void) { return session_wait_backend(); }
// 销毁旧会话时经手的 Metal 对象（查表取出来的贴图、缓冲）有自动释放的引用，调用线程是长活的会话线程、没有自己的池：包一层，旧局的贴图当场放
static int nvn_be_shutdown_session_body(void);
int nvn_be_shutdown_session(void) { @autoreleasepool { return nvn_be_shutdown_session_body(); } }
static int nvn_be_shutdown_session_body(void) {
    int e = session_wait_backend(); if (e) { MLOG("【红】重启：Metal 工作仍持有旧会话 (%d)", e); return e; }
    // Keep completed immutable shader loads in the device cache. This also
    // consumes the old prefetch handoff before its pipeline maps are cleared.
    warm_claim();
    // 停在跨队列等待上的队列记录（事件地址在旧局游戏内存里、续点指着旧局命令流）随旧局走：留着的话新局 xq_ready 解引用旧事件地址、看门狗到点回放旧命令流
    while (g_nxq) xq_drop(&g_xq[0]);
    g_xq_cur = NULL; g_xq_unwind = 0; g_xq_nuc = g_xq_ucover = 0;
    pool_session_reset();   // before the owners below: its pins would keep what they remove resident
    // No callbacks can touch these objects now. Drop descriptors first, then
    // texture/program owners, then no-copy memory-pool buffers.
    for (int i = 0; i < g_ntreg; i++) {
        treg_t *t = &g_treg[i];
        if (t->view) CFRelease(t->view);
        if (t->alt) { if (t->alt_copy) res_remove((__bridge id)t->alt); CFRelease(t->alt); }   // alt_copy: a 3D copy of its own (tex3d_prepare, #674), in the residency set
        if (t->ar) CFRelease(t->ar); if (t->im) CFRelease(t->im);
    }
    free(g_treg); g_treg = NULL; g_ntreg = g_tpool_n = 0;
    // 「贴图序号 → 登记号」随登记表一起清：留着的话指向已释放的旧表（平时新一局序号一路往上不会撞；按快照恢复会把序号拨回快照时的值，撞上旧表项、链表越界写崩）
    nvn_hm_clear(&g_treg_ser, NULL);
    // 同理按贴图序号 / 上一局游戏内存建的：未压缩贴图 → 池、即时存档的贴图地址表与待写回表（新一局的池、贴图对象都是新的）
    pthread_mutex_lock(&g_cpu_tex_mu); nvn_hm_clear(&g_cpu_unc, NULL); pthread_mutex_unlock(&g_cpu_tex_mu);
    nvn_hm_clear(&g_snap_texaddr, NULL); nvn_hm_clear(&g_snap_ghost_pend, session_resource_release);   // 这些贴图读进来时登记进了常驻集（snap_al_load），只放引用的话常驻集还攥着，每回一次快照留一批
    for (int i = 0; i < g_nsreg; i++) if (g_sview[i]) CFRelease(g_sview[i]);
    free(g_sreg); free(g_sreg_ok); free(g_sview); free(g_srid);
    g_sreg = NULL; g_sreg_ok = NULL; g_sview = NULL; g_srid = NULL; g_nsreg = 0;
    for (size_t i = 0; i < sizeof g_tser / sizeof *g_tser; i++) atomic_store(&g_tser[i], 0);
    nvn_hm_clear(&g_vc, vc_set_free);
    nvn_hm_clear(&g_by_serial, session_resource_release);
    nvn_hm_clear(&g_alias, session_resource_release);
    nvn_hm_clear(&g_ghost, session_resource_release);   // 同 g_snap_ghost_pend：只剩这张表持有、进过常驻集的贴图（恢复放回的、已销毁序号的）也要出集
    nvn_hm_clear(&g_alias_seen, NULL); nvn_hm_clear(&g_tex_live, NULL);
    // 与上面几张同键的两张也清：共享存储「最后一次 GPU 写在几号栅栏」（号倒回后旧的大号让活着的渲染目标被判成「之后被覆盖」）、幽灵贴图的淘汰环（旧序号会把新局同序号的幽灵淘汰掉）
    nvn_hm_clear(&g_alias_wrote, NULL); memset(g_ghost_ring, 0, sizeof g_ghost_ring); g_ghost_next = 0;
    pthread_mutex_lock(&g_gpw_mu);
    nvn_hm_clear(&g_gpw, free); nvn_hm_clear(&g_gpw_ser, free);
    nvn_hm_clear(&g_gpw_own, free);   // 贴图序号 → GPU 写过的存储范围，同上两张（序号按快照拨回后会撞上旧项，CPU 写入不再重传）
    g_gpw_skips = 0; g_gpw_gen = 1; atomic_store(&g_gpw_nblk, 0);
    pthread_mutex_unlock(&g_gpw_mu);
    // 「GPU 写过的池」按池对象地址记：新局同地址的池一上来就算写过，未压缩贴图不再按 CPU 内容重传。抬代数让每线程缓存失效
    pthread_mutex_lock(&g_pool_gpuw_mu); nvn_hm_clear(&g_pool_gpuw, NULL); atomic_fetch_add(&g_pool_gpuw_gen, 1); atomic_store(&g_pool_gpuw_new, 0); pthread_mutex_unlock(&g_pool_gpuw_mu);
    free(g_gone); g_gone = NULL; g_gone_n = g_gone_cap = 0;
    if (g_alias_set) CFSetRemoveAllValues(g_alias_set);
    nvn_hm_clear(&g_cpu_tex, NULL);
    // Atomic texture upgrades use the same serial/alias owners above. Their
    // weak usage registry and queued descriptor ids belong to this session.
    pthread_mutex_lock(&g_atomic_mu); g_atomic_images = nil; pthread_mutex_unlock(&g_atomic_mu);
    g_imfit_npend = g_imwr_npend = 0;
    memset(g_red_once, 0, sizeof g_red_once);
    g_noload_wrong = 0;   // the lookahead-wrong red lines: the first 8 of each session (sd_finish)
    for (id<MTLBuffer> b in g_keep) res_remove(b);
    [g_keep removeAllObjects]; g_dead_pools = nil;
    free(g_pools); g_pools = NULL; g_npool = g_cap = g_pool_res_n = 0;
    // 大池记录同池表一起清（11-21 D-Q349 等 8 份诊断，0.4.5 起的「-[__NSArrayM insertObject:atIndex:]: object cannot be nil」）：块表 chunk / full 存的是池表下标，
    //   池表清空、新一局重新登记后，上一局的块表还指着那些下标。新一局查到同一段地址（大池地址每局多半相同）就拿旧下标去读新表：越界或撞上别的条目，
    //   块加长时把那条的缓冲（空）塞进 old 数组，抛 ObjC 异常闪退。每份都是进程内重开之后的那一局。旧块缓冲在 old 里（强引用），清掉随会话放
    for (int k = 0; k < g_nbig; k++) { for (id<MTLBuffer> ob in g_big[k].old) res_remove(ob); free(g_big[k].chunk); g_big[k].chunk = NULL; g_big[k].old = nil; }
    g_nbig = 0;
    // 原始池表的访问位（gslot：位 → 池表下标）同理：位图里旧局置的位留着，新局首次 gmem_scan 按旧下标取 g_pools[i]（没有边界判断）
    g_gslot_hi = g_gslot_nfree = 0; if (g_gbits) memset(g_gbits.contents, 0, GSLOT_MAX / 8);
    // 其余按旧局地址 / 栅栏号记的小表：地址兜底池的键（新局首次提交的 adhoc_drop 会把同键的新池注销）、条件渲染的已提交记录（号倒回后旧的大号一直算「没做完」、读 0）
    //   与写过报告的地址表、见过的事件地址
    g_adhoc_nk = 0; g_rc_nsubmitted = 0; free(g_rc_occ); g_rc_occ = NULL; g_rc_ocap = g_rc_on = 0; g_neva = 0;
    free(g_pidx); g_pidx = NULL; g_pidx_cap = g_pidx_ok = 0; g_plast = -1;
    for (int s = 0; s < GMEM_RECYCLE; s++) {
        if (g_gmem_rec[s].buf) { res_remove(g_gmem_rec[s].buf); g_gmem_rec[s].buf = nil; }
        g_gmem_rec[s].fence = 0; atomic_store(&g_gmem_rec[s].busy, 0);
    }
    if (g_gmem_buf && g_gmem_cur_slot < 0) res_remove(g_gmem_buf);
    g_gmem_buf = nil; g_gmem_cpu = nil; g_gmem_gen = g_gmem_built_rgen = 0; g_gmem_cur_slot = -1; g_gmem_built_on = -1;
    g_last_end_relaxed = 0; g_game_barrier = 0; main_barrier_reset();
    atomic_fetch_add(&g_pool_gen, 1); atomic_fetch_add(&g_gmem_pgen, 1); atomic_fetch_add(&g_reg_gen, 1); atomic_fetch_add(&g_ser_gen, 1);
    // Keys for these caches include program/guest buffer identity. Reuse only
    // the compiled shader cache (its keys are content hashes).
    nvn_hm_clear(&g_pso, hm_rel); nvn_hm_clear(&g_cps, hm_rel);
    if (g_mtk_group) dispatch_group_wait(g_mtk_group, DISPATCH_TIME_FOREVER);   // task / mesh pipelines (nvn_geometry.inc mtk_pso): the background builds write the map
    mtk_scratch_reset();
    os_unfair_lock_lock(&g_pso_mtk_lock); nvn_hm_clear(&g_pso_mtk, hm_rel); os_unfair_lock_unlock(&g_pso_mtk_lock);
    nvn_hm_clear(&g_pso_fail, hm_rel); nvn_hm_clear(&g_pso_retry, hm_rel);
    ir_clear(); nvn_hm_clear(&g_rt_seen, NULL); nvn_hm_clear(&g_rt_seen_mem, NULL); nvn_hm_clear(&g_rt_fresh, NULL);
    g_rt_history_frame = 0; g_rt_history_valid = 0;
    for (int i = 0; i < PSO_PEND_MAX; i++) { g_pp[i].ps = nil; g_pp[i].used = g_pp[i].done = g_pp[i].late = 0; }
    g_geo_program = nil; g_pso_fsfn = nil; g_pso_lmap = NULL;
    g_prog_last = nil; g_prog_last_ptr = NULL; g_prog_last_serial = g_prog_last_gen = 0;
    memset(&g_bind_cache, 0, sizeof g_bind_cache);
    g_pp_prog = g_pp_cprog = NULL; g_pp_rt = NULL;
    defer_clear_all(); pend_reset(); state_reset(); memset(&R, 0, sizeof R);
    for (int i = 0; i < g_nfzs; i++) { free(g_fzs[i].S_); g_fzs[i].S_ = NULL; }
    g_nfzs = g_fzs_bad = g_fz_checking = 0; memset(g_fzs, 0, sizeof g_fzs); memset(&g_fz, 0, sizeof g_fz);
    memset(&g_pass, 0, sizeof g_pass); g_rt_range_active = g_rt_range_pending = 0;
    g_exec_next = g_exec_cur = NULL; g_rest = NULL; g_nrest = g_ret_n = g_hoist_n = 0;
    memset(g_ret, 0, sizeof g_ret); memset(g_hoist_skip, 0, sizeof g_hoist_skip);
    memset(g_vconv, 0, sizeof g_vconv); g_vconv_next = 0;
    memset(g_ubpc, 0, sizeof g_ubpc); g_ubpc_i = 0;
    memset(g_ubc, 0, sizeof g_ubc); // counters stay monotonic for the idle host worker
    memset(g_gw, 0, sizeof g_gw); g_gw_all = 0;
    g_ixc_n = g_wide_n = g_mdx_n = g_nlc = 0; g_mdx_stop = NULL;
    memset(&g_mdxc, 0, sizeof g_mdxc);
    free(g_sdx); free(g_sdx_tmp); free(g_sdx_op); free(g_rtx_seq); free(g_drx_seq);
    g_sdx = g_sdx_tmp = NULL; g_sdx_op = NULL; g_rtx_seq = g_drx_seq = NULL;
    g_sdx_n = g_sdx_cap = g_sdx_nop = g_sdx_opcap = g_rtx_n = g_rtx_cap = g_drx_n = g_drx_cap = 0; g_sdx_ok = 0;
    vis_clear(); g_vis_on = g_vis_counting = 0;
    free(g_post); g_post = NULL; g_npost = g_postcap = 0;
    for (int i = 0; i < 8; i++) { g_bcrt_sh[i] = nil; g_bcrt_buf[i] = nil; g_ms_att[i] = nil; g_ts_att[i] = nil; }
    for (int i = 0; i < 4; i++) { g_fb_src[i] = nil; g_fb_use[i] = nil; g_fb_rid[i] = 0; }
    g_nbcrt = g_ms_na = g_ts_na = g_fb_n = 0;
    g_vt_valid = 0; memset(g_vt_addr, 0, sizeof g_vt_addr); memset(g_args_dirty, 1, sizeof g_args_dirty);
    g_args_reg_seen = g_args_dirty_seen = 0; g_c0_dim_buf = nil;
    for (int i = 0; i < 3; i++) { g_m3_sent_enc[i] = nil; g_m3_sent_tab[i] = nil; }
    if (g_dev) {
        g_vt = g_ft = g_ct = g_up_ct = g_present_args = nil;
        NSError *error = nil;
        g_vt = rec_table(tab_new(31, 1, 0, &error)); g_ft = rec_table(tab_new(31, 1, 0, &error)); g_ct = rec_table(tab_new(31, 1, 0, &error));
        g_up_ct = tab_new(31, 1, 0, &error); g_present_args = tab_new(1, 1, 1, &error);
        if (!g_vt || !g_ft || !g_ct || !g_up_ct || !g_present_args) return -ENOMEM;
    }
    // Staging and submission arenas contain copies, but must not retain old
    // no-copy texture owners through the upload batch or encoded commands.
    g_up_texs = nil; g_up_keep = nil; g_up_lost = nil; g_up_old = nil;
    g_nup_blits = g_nup_decs = 0;
    free(g_up_blits); g_up_blits = NULL; g_up_blitcap = 0;
    free(g_up_decs); g_up_decs = NULL; g_up_deccap = 0;
    for (NvArena *a in g_arenas) res_remove(a->buf);
    for (NvArena *a in g_sub_arenas) res_remove(a->buf);
    g_cur_arena = nil; g_sub_arenas = nil; g_arenas = nil;
    size_abi_reset(); g_wd_rs_gpu = 0; g_wd_rs_data = NULL;
    res_remove(g_ubs_buf); g_ubs_buf = nil; g_ubs_off = g_ubs_size = 0;
    pthread_mutex_lock(&g_rb_mu);
    while (g_rb_free) { rec_blk *b = g_rb_free; g_rb_free = b->next; free(b); }
    if (g_rb_cur) { free(g_rb_cur); g_rb_cur = NULL; }
    g_rx_cb = g_rx_enc = nil;
    pthread_mutex_unlock(&g_rb_mu);
    // Native widening holds copied uniform buffers and per-shader core ids.
    for (int s = 0; s < 6; s++) for (int b = 0; b < 16; b++) { free(g_wd_cbv[s][b]); g_wd_cbv[s][b] = NULL; g_wd_cbq[s][b] = 0; }
    memset(g_wd_last, 0, sizeof g_wd_last); memset(g_wd_b, 0, sizeof g_wd_b); memset(g_wd_gpu, 0, sizeof g_wd_gpu);
    memset(g_wd_bound, 0, sizeof g_wd_bound); memset(g_wd_had, 0, sizeof g_wd_had); memset(g_wd_viewm, 0, sizeof g_wd_viewm);
    g_wd = -1; g_wd_sh = g_wd_sh_push = g_wd_nt = g_wd_npr = g_wd_npr_arm = 0;
    memset(g_wd_t, 0, sizeof g_wd_t); memset(g_wd_s3d, 0, sizeof g_wd_s3d); g_wd_s3d_n = 0;   // 表本身也清（上一行只清了条数）：按贴图序号查，同序号同尺寸直接返回旧局的值、不向新的宽屏核心会话登记
    g_wd_c0 = 0; g_wd_d0 = ~0ull; g_wd_fskey = "-";
    // All prefetch producers are drained. Compiled code stays resident, but
    // widening ids and the first-use recording bit belong to the retired run.
    os_unfair_lock_lock(&g_shader_lock);
    NSDictionary<NSString *, NvShader *> *shaders = [g_shcache copy];
    os_unfair_lock_unlock(&g_shader_lock);
    NSMutableSet<NSString *> *cached_prefetch = [NSMutableSet set];
    int depth_zero = atomic_load(&g_depth_zero);
    os_unfair_lock_lock(&g_rmu);
    for (NvShader *sh in shaders.allValues) {
        sh->wsh = 0; sh->s_rec = 0;
        if (sh->size_abi) { sh->ab_buf = nil; sh->ab_gpu = sh->ab_size_sub = 0; }
        NSString *key = @(sh->key);
        if (key.length != 18 || ![key hasSuffix:@"_b"]) continue;
        NSString *hash = [key substringToIndex:16], *stage = (NSString *)(id)g_shmap[hash];
        int st = stage ? stage_of(stage) : -1;
        // Match exactly what shader_prefetch will load, not a layer or a
        // vertex object captured under the other depth convention.
        if (st >= 0 && st != 2 && shaders[shader_identity(key, st, 0, depth_zero)] == sh)
            [cached_prefetch addObject:hash];
    }
    os_unfair_lock_unlock(&g_rmu);
    os_unfair_lock_lock(&g_shpre_mu);
    // Previously skipped cold keys may now be available. Already cached
    // libraries need no second background load just because a run restarted.
    g_shpre_seen = cached_prefetch;
    g_shpre_n = g_shpre_ns = 0;
    atomic_store(&g_cold_skips, 0);
    os_unfair_lock_unlock(&g_shpre_mu);
    // 深度约定计数（「depth mode changed after N vertex shader constructions」）是逐局的：上面按旧局 depth_zero 算完预取集合后清回 NVN 缺省（0），
    //   新局 nvnDeviceInitialize 设 0 不算翻转、游戏 SetDepthMode(1) 翻转时计数为 0 不报红（同首局）；着色器缓存键自带深度模式，旧局建的不会被错用
    atomic_store(&g_vs_loaded, 0); atomic_store(&g_depth_zero, 0);
    memset(g_pq, 0, sizeof g_pq); g_ext_ev = nil; g_ext_pend3 = 0;
    g_link_applied = 0; g_link_hold_until = g_link_vperiod = g_link_vhalf = 0;
    g_link_pending = 0; g_link_pending_since = g_link_pending_period = 0;
    g_low = (link_backoff){0};   // 降档退避（nvn_link_backoff.h）逐局：旧局顶到 64 秒的 hold、停在低频档的 since 都不带进新局（满屏补丁线复核 11-04）
    g_link20_last = 0; g_link20_n = 0; g_link20_sample = g_link20_reported = 0;
    memset(g_link20_ns, 0, sizeof g_link20_ns); atomic_store(&g_link20_unavailable, 0);
    g_link_tick_ns = g_link_tick_n = g_link_dropped = g_link_held = g_link_started_tick = 0;   // 拍号与「上一帧开工拍号」一起从 0 起，否则新局每帧等满超时锁 30 帧
    g_xv_npresent = 0; g_cur_frame = 0; g_present_n = 0; g_stat_pass = g_stat_draw = 0;
    g_nshotq = 0; g_td_list = nil; g_td_frames = nil;
    free(g_td_owned_path); g_td_owned_path = NULL;
    g_td_frame = -2; g_td_path = NULL; g_td_w = g_td_h = 0; g_td_done = 0; g_lp = nil;
    atomic_store(&g_sub_thread_set, 0);
    // Ordinary deferred releases target fence+1. There will be no next
    // submission in this session, and all prior callbacks have completed.
    for (NvDeferredRelease *d in g_deferred) {
        if (!d->keep_resident && [d->object conformsToProtocol:@protocol(MTLResource)]) res_remove(d->object);
        d->object = nil;
    }
    [g_deferred removeAllObjects]; res_commit();
    return 0;
}
void nvn_be_begin_session(void) {
    os_unfair_lock_lock(&g_shpre_mu);
    atomic_store(&g_shpre_stopping, 0);
    os_unfair_lock_unlock(&g_shpre_mu);
    atomic_store(&g_link_stop, 0); atomic_store(&g_present_discard, 0);
    g_dm_present_link = present_link_from_env();   // 同 mtl_init：补帧开着时不起显示链（120 档补丁除外），每局开头不再只按 DM_PRESENT_LINK 缺省 1
}
