// SPDX-License-Identifier: Apache-2.0
// 对齐 MapVirtual 的连续物理段。保留原虚拟 run 供宿主读和不连续映射使用。
// g_bmu 保护。物理池销毁时按 pool 撤记录，虚拟池改映射时按 [lo,hi) 切记录。
struct buf_alias { uint64_t lo, hi, phys, pool; };
static struct buf_alias *aliases;
static size_t alias_count, alias_cap;
static int aliases_live;
static size_t alias_lower_bound(uint64_t addr) {
    size_t lo = 0, hi = alias_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (aliases[mid].hi <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
static uint64_t buf_address(uint64_t addr, uint64_t len) {
    if (!__atomic_load_n(&aliases_live, __ATOMIC_ACQUIRE)) return addr;
    pthread_mutex_lock(&g_bmu);
    size_t i = alias_lower_bound(addr);
    if (i < alias_count && aliases[i].lo <= addr && len <= aliases[i].hi - addr)
        addr = aliases[i].phys + (addr - aliases[i].lo);
    pthread_mutex_unlock(&g_bmu);
    return addr;
}
static void alias_stale(struct shadow *s, struct mirror *m, uint64_t at, uint64_t len) {
    if (!m->synced) return;
    const size_t ps = nvn_dirty_page_size();
    uint64_t p = (at - s->page0) / ps, q = (at + len - 1 - s->page0) / ps;
    if (p >= s->npages) return;
    if (q >= s->npages) q = s->npages - 1;
    if (m->path == MP_LOG) stale_set(m, p, q);
    memset(m->synced + p, 0, (q - p + 1) * sizeof *m->synced);
}
static void alias_dirty(const struct buf_alias *a, uint64_t lo, uint64_t hi) {
    struct shadow *s = nvn_hm_get(&g_shadow, &a->pool);
    if (!s || s->failed) return;
    for (int i = 0; i < 2; i++) alias_stale(s, &s->m[i], a->phys + (lo - a->lo), hi - lo);
}
int rp_buf_alias_set(uint64_t at, uint64_t len, uint64_t phys) {
    const uint8_t *pool = NULL;
    if (!len || len > UINT64_MAX - at || len > UINT64_MAX - phys) return 0;
    if (phys && !rp_pool_find(phys, len, &pool, NULL)) return 0;
    pthread_mutex_lock(&g_bmu);
    const uint64_t end = at + len;
    size_t first = alias_lower_bound(at), last = first;
    struct buf_alias put[3];
    size_t n = 0;
    if (first < alias_count && aliases[first].lo < at) {
        put[n] = aliases[first]; put[n++].hi = at;
    }
    if (phys) put[n++] = (struct buf_alias){at, end, phys, (uintptr_t)pool};
    while (last < alias_count && aliases[last].lo < end) last++;
    if (last > first && aliases[last - 1].hi > end) {
        put[n] = aliases[last - 1]; put[n].phys += end - put[n].lo; put[n++].lo = end;
    }
    size_t count = alias_count - (last - first) + n;
    if (count > alias_cap) {
        size_t cap = count * 2;
        struct buf_alias *p = realloc(aliases, cap * sizeof *p);
        if (!p) { pthread_mutex_unlock(&g_bmu); return 0; }
        aliases = p; alias_cap = cap;
    }
    for (size_t i = first; i < last; i++)
        alias_dirty(&aliases[i], at > aliases[i].lo ? at : aliases[i].lo,
                    end < aliases[i].hi ? end : aliases[i].hi);
    if (last < alias_count)
        memmove(aliases + first + n, aliases + last, (alias_count - last) * sizeof *aliases);
    if (n) memcpy(aliases + first, put, n * sizeof *put);
    alias_count = count;
    size_t i = first ? first - 1 : first;
    while (i + 1 < alias_count && aliases[i].lo <= end) {
        struct buf_alias *a = &aliases[i], *b = a + 1;
        if (a->hi == b->lo && a->pool == b->pool && a->phys + (a->hi - a->lo) == b->phys) {
            a->hi = b->hi;
            memmove(b, b + 1, (--alias_count - i - 1) * sizeof *b);
        } else i++;
    }
    if (phys) {
        struct buf_alias a = {at, end, phys, (uintptr_t)pool};
        alias_dirty(&a, at, end);
    }
    __atomic_store_n(&aliases_live, alias_count != 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_bmu);
    return 1;
}
