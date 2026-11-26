/*
 * Ultra (-9) sparse DP parser.
 *
 * Same search model as the generic SDP parser (states keyed by
 * (last_offset, after_literal) at every position, new matches taken from
 * the finder candidates) with:
 *   - direct-pointer tiered active-row slab pool (8 / 32 / 128|256 slots,
 *     power-of-two capacity shifts, future rows only, freed once expanded),
 *   - 16-byte header + O(1) open-addressed htab[256] on tier 2 (K <= 128)
 *     for ~1.5-probe key lookup on both vector and scalar targets,
 *   - 8x16 block-max eviction tracking (24 scalar ops vs 128 on full rows)
 *     paired with cached worst-cost early reject before touching row buffers,
 *   - flat reference-counted DAG backtrack arena with lazy literal anchors,
 *   - streaming literal-run cost and early best[pos] / cutoff filtering.
 * Only the parser working memory and runtime change; the compressed stream
 * stays bit-identical to the dense formulation.
 */
#include "lz0_internal.h"
#include "lz0_simd.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int lz0_ultra_states = 0;
size_t lz0_ultra_budget = (size_t)768 << 20;
int lz0_ultra_margin = 32;
int lz0_ultra_fine = 8;
int lz0_ultra_inject = 0;

#define U_LIT_FLAG   0x80000000u
#define U_POS_MASK   0x00ffffffu
#define U_BT_NIL     0xffffffffu
#define U_SLAB_SHIFT 5
#define U_SLAB_N     (1u << U_SLAB_SHIFT)

#if defined(__GNUC__) || defined(__clang__)
#define U_ALWAYS_INLINE inline __attribute__((always_inline))
#define U_NOINLINE      __attribute__((noinline))
#else
#define U_ALWAYS_INLINE inline
#define U_NOINLINE
#endif

typedef struct {
    uint64_t bloom; /* 64-bit key filter (tier 0, and manual K > 128 fallback) */
    uint8_t ns;     /* live slots in row */
    uint8_t shift;  /* log2(cap): 3 (cap 8), 5 (cap 32), 7 (128), 8 (256) */
    uint8_t worst;  /* cached worst slot when row is full, 0xff = unknown */
    uint8_t tier;   /* pool tier 0..2 */
    uint32_t _pad;
} u_row_hdr; /* 16 bytes, 8-byte aligned */

/* Tier 2 trailer appended after rbt[cap] (kept 8-byte aligned). */
typedef struct {
    uint8_t htab[256];   /* 0xff = empty, 0xfe = tombstone, 0..127 = slot index */
    uint16_t bw_dirty;   /* bit b => 16-slot block b's local max needs recomputing */
    uint8_t bw_inited;   /* 1 once bw_slot[] has been initialized at ns == K */
    uint8_t tomb;        /* tombstone count (rebuild when >= 48) */
    uint8_t bw_slot[16]; /* argmax slot per 16-entry block (up to K <= 255) */
    uint8_t _pad[4];
} u_tier2_extra; /* 280 bytes, multiple of 8 */

typedef struct {
    unsigned char **slabs;
    uint32_t n_slabs;
    uint32_t cap_slabs;
    u_row_hdr *free_head;
    size_t stride;
    int cap;
    int shift;
} u_row_tier;

typedef struct {
    uint32_t parent; /* parent node id, or next_free when on free list */
    uint32_t refcnt; /* live refs from active rows, child nodes, or step pins */
    uint32_t pos;    /* stream byte position */
    uint32_t key;    /* match offset, or off | U_LIT_FLAG for literal anchor */
} u_bt_node;

typedef struct {
    int32_t best;  /* lowest cost seen at pos */
    int32_t worst; /* cached worst cost when row[pos] is full & worst known, else INT_MAX/2 */
} u_pos_lim;

typedef struct {
    uint32_t n;
    int K;
    int use_ht;
    int oom;
    size_t alloc_bytes;
    u_pos_lim *lim;   /* [n + 1] (best, worst_cutoff) packed 8B/byte */
    u_row_hdr **rows; /* [n + 1] direct pointer to active row or NULL */
    u_row_tier tiers[3];
    u_bt_node *bt;
    uint32_t bt_next;
    uint32_t bt_cap;
    uint32_t bt_free;
} u_dp;

typedef struct {
    uint32_t src_pos;
    uint32_t src_key;
    uint32_t orig_bt;
    uint32_t lit_opos;
    uint32_t *anchor_slot; /* non-NULL => materialize anchor lazily on accept */
} u_src_ref;

static inline int u_elias(int v) {
    return ((32 - __builtin_clz((unsigned)v)) << 1) - 1;
}

static inline int u_off_eb(uint32_t off) {
    return u_elias((int)((off - 1) >> 8) + 1);
}

static inline int u_margin(uint32_t off, int margin) {
    return margin + u_off_eb(off);
}

static inline int u_cost_lit(int len) {
    return 1 + u_elias(len) + (len << 3);
}

static inline int u_cost_last(int len) {
    return 1 + u_elias(len);
}

static inline int u_len_new(int len) {
    int v = len - 1;
    return v <= 7 ? 3 : 3 + u_elias(v - 7);
}

static U_ALWAYS_INLINE uint32_t u_hbit(uint32_t key) {
    return (key * 0x9E3779B1u) >> 26;
}

static U_ALWAYS_INLINE uint32_t u_h256(uint32_t key) {
    return ((key ^ (key >> 16)) * 0x9E3779B1u) >> 24;
}

static U_ALWAYS_INLINE uint32_t *u_row_key(u_row_hdr *r) {
    return (uint32_t *)(r + 1);
}

static U_ALWAYS_INLINE int32_t *u_row_cost(u_row_hdr *r) {
    return (int32_t *)((uint32_t *)(r + 1) + (1u << r->shift));
}

static U_ALWAYS_INLINE uint32_t *u_row_bt(u_row_hdr *r) {
    return (uint32_t *)(r + 1) + (2u << r->shift);
}

static U_ALWAYS_INLINE uint64_t *u_row_bloom4(u_row_hdr *r) {
    return (uint64_t *)((unsigned char *)(r + 1) + (12u << r->shift));
}

static U_ALWAYS_INLINE u_tier2_extra *u_row_t2(u_row_hdr *r) {
    return (u_tier2_extra *)((unsigned char *)(r + 1) + (12u << r->shift));
}

static U_ALWAYS_INLINE uint32_t bt_pos(const u_dp *d, uint32_t id) {
    return d->bt[id].pos;
}

static U_ALWAYS_INLINE void bt_inc(u_dp *d, uint32_t id) {
    if (id != U_BT_NIL)
        d->bt[id].refcnt++;
}

static U_NOINLINE void bt_dec_slow(u_dp *d, uint32_t id) {
    /* Iterative free cascade keeps stack O(1) across arbitrarily long chains */
    for (;;) {
        u_bt_node *bn = &d->bt[id];
        uint32_t par = bn->parent;
        bn->parent = d->bt_free;
        d->bt_free = id;
        if (par == U_BT_NIL)
            break;
        if (--d->bt[par].refcnt != 0)
            break;
        id = par;
    }
}

static U_ALWAYS_INLINE void bt_dec(u_dp *d, uint32_t id) {
    if (id != U_BT_NIL && --d->bt[id].refcnt == 0)
        bt_dec_slow(d, id);
}

static U_NOINLINE uint32_t bt_grow(u_dp *d) {
    uint32_t nc = d->bt_cap ? d->bt_cap * 2u : 2048u;
    u_bt_node *nb = (u_bt_node *)realloc(d->bt, (size_t)nc * sizeof(u_bt_node));
    if (!nb) {
        d->oom = 1;
        return U_BT_NIL;
    }
    d->alloc_bytes += (size_t)(nc - d->bt_cap) * sizeof(u_bt_node);
    d->bt = nb;
    d->bt_cap = nc;
    return d->bt_next++;
}

static inline uint32_t bt_alloc(u_dp *d, uint32_t pos, uint32_t key, uint32_t parent) {
    uint32_t id;
    if (d->bt_free != U_BT_NIL) {
        id = d->bt_free;
        d->bt_free = d->bt[id].parent;
    } else if (d->bt_next < d->bt_cap) {
        id = d->bt_next++;
    } else {
        id = bt_grow(d);
        if (id == U_BT_NIL)
            return U_BT_NIL;
    }
    u_bt_node *bn = &d->bt[id];
    bn->parent = parent;
    bn->refcnt = 0;
    bn->pos = pos;
    bn->key = key;
    bt_inc(d, parent);
    return id;
}

static inline void u_row_init(u_row_hdr *r, int tier, int shift, int use_ht) {
    r->bloom = 0;
    r->ns = 0;
    r->shift = (uint8_t)shift;
    r->worst = 0xff;
    r->tier = (uint8_t)tier;
    if (tier == 1 || (tier == 2 && !use_ht)) {
        uint64_t *b4 = u_row_bloom4(r);
        b4[0] = b4[1] = b4[2] = b4[3] = 0;
    } else if (tier == 2) {
        u_tier2_extra *e = u_row_t2(r);
        memset(e->htab, 0xff, sizeof(e->htab));
        e->bw_dirty = 0;
        e->bw_inited = 0;
        e->tomb = 0;
    }
}

static U_NOINLINE u_row_hdr *u_row_alloc_slab(u_dp *d, int tier) {
    u_row_tier *tr = &d->tiers[tier];
    if (tr->n_slabs == tr->cap_slabs) {
        uint32_t nc = tr->cap_slabs ? tr->cap_slabs * 2u : 16u;
        unsigned char **nsl =
            (unsigned char **)realloc(tr->slabs, (size_t)nc * sizeof(unsigned char *));
        if (!nsl) {
            d->oom = 1;
            return NULL;
        }
        d->alloc_bytes += (size_t)(nc - tr->cap_slabs) * sizeof(unsigned char *);
        tr->slabs = nsl;
        tr->cap_slabs = nc;
    }
    size_t slab_bytes = (size_t)U_SLAB_N * tr->stride;
    unsigned char *slab = (unsigned char *)malloc(slab_bytes);
    if (!slab) {
        d->oom = 1;
        return NULL;
    }
    d->alloc_bytes += slab_bytes;
    tr->slabs[tr->n_slabs++] = slab;
    for (uint32_t k = 1; k + 1u < U_SLAB_N; k++) {
        u_row_hdr *cur = (u_row_hdr *)(slab + (size_t)k * tr->stride);
        u_row_hdr *nxt = (u_row_hdr *)(slab + (size_t)(k + 1u) * tr->stride);
        memcpy(u_row_key(cur), &nxt, sizeof(nxt));
    }
    u_row_hdr *last = (u_row_hdr *)(slab + (size_t)(U_SLAB_N - 1u) * tr->stride);
    u_row_hdr *null_p = NULL;
    memcpy(u_row_key(last), &null_p, sizeof(null_p));
    tr->free_head = (u_row_hdr *)(slab + tr->stride);
    u_row_hdr *r = (u_row_hdr *)slab;
    u_row_init(r, tier, tr->shift, d->use_ht);
    return r;
}

static inline u_row_hdr *u_row_alloc(u_dp *d, int tier) {
    u_row_tier *tr = &d->tiers[tier];
    u_row_hdr *r = tr->free_head;
    if (!r)
        return u_row_alloc_slab(d, tier);
    u_row_hdr *nxt;
    memcpy(&nxt, u_row_key(r), sizeof(nxt));
    tr->free_head = nxt;
    u_row_init(r, tier, tr->shift, d->use_ht);
    return r;
}

static inline void u_row_free(u_dp *d, uint32_t pos) {
    u_row_hdr *r = d->rows[pos];
    if (!r)
        return;
    u_row_tier *tr = &d->tiers[r->tier];
    memcpy(u_row_key(r), &tr->free_head, sizeof(tr->free_head));
    tr->free_head = r;
    d->rows[pos] = NULL;
}

static U_NOINLINE u_row_hdr *u_row_promote(u_dp *d, uint32_t pos, u_row_hdr *old_r) {
    int old_t = old_r->tier;
    int old_ns = old_r->ns;
    u_row_hdr *nr = u_row_alloc(d, old_t + 1);
    if (!nr)
        return NULL;
    nr->bloom = old_r->bloom;
    nr->ns = (uint8_t)old_ns;
    nr->worst = old_r->worst;
    uint32_t *nk = u_row_key(nr);
    memcpy(nk, u_row_key(old_r), (size_t)old_ns * sizeof(uint32_t));
    memcpy(u_row_cost(nr), u_row_cost(old_r), (size_t)old_ns * sizeof(int32_t));
    memcpy(u_row_bt(nr), u_row_bt(old_r), (size_t)old_ns * sizeof(uint32_t));
    if (nr->tier == 1 || (nr->tier == 2 && !d->use_ht)) {
        uint64_t *b4 = u_row_bloom4(nr);
        for (int s = 0; s < old_ns; s++) {
            uint32_t h = u_h256(nk[s]);
            b4[h >> 6] |= 1ull << (h & 63u);
        }
    } else if (nr->tier == 2) {
        u_tier2_extra *e = u_row_t2(nr);
        for (int s = 0; s < old_ns; s++) {
            uint32_t p = u_h256(nk[s]) & 255u;
            while (e->htab[p] != 0xff)
                p = (p + 1u) & 255u;
            e->htab[p] = (uint8_t)s;
        }
    }
    u_row_free(d, pos);
    d->rows[pos] = nr;
    return nr;
}

static U_NOINLINE void u_ht_rebuild(u_row_hdr *r, const uint32_t *rkey) {
    u_tier2_extra *e = u_row_t2(r);
    memset(e->htab, 0xff, sizeof(e->htab));
    int n = r->ns;
    for (int s = 0; s < n; s++) {
        uint32_t p = u_h256(rkey[s]) & 255u;
        while (e->htab[p] != 0xff)
            p = (p + 1u) & 255u;
        e->htab[p] = (uint8_t)s;
    }
    e->tomb = 0;
}

static U_NOINLINE uint32_t u_alloc_anchor(u_dp *d, const u_src_ref *sref) {
    uint32_t anc = bt_alloc(d, sref->src_pos, sref->src_key, sref->orig_bt);
    if (anc == U_BT_NIL)
        return U_BT_NIL;
    bt_inc(d, anc); /* pin across step i so evictions cannot drop refcnt to 0 mid-step */
    *sref->anchor_slot = anc;
    return anc;
}

static U_ALWAYS_INLINE uint32_t u_resolve_bt(u_dp *d, const u_src_ref *sref) {
    if (!sref->anchor_slot)
        return sref->orig_bt;
    uint32_t anc = *sref->anchor_slot;
    if (anc != U_BT_NIL)
        return anc;
    return u_alloc_anchor(d, sref);
}

static inline int u_block_argmax(const int32_t *rcost, int lo, int hi) {
    int m = lo;
    int32_t best = rcost[lo];
    for (int s = lo + 1; s < hi; s++) {
        int32_t c = rcost[s];
        if (c > best) {
            best = c;
            m = s;
        }
    }
    return m;
}

static U_NOINLINE void u_find_worst(u_dp *d, uint32_t pos, u_row_hdr *r) {
    const int32_t *rcost = u_row_cost(r);
    int K = d->K;
    if (r->tier != 2 || !d->use_ht) {
        int w = lz0_simd_argmax_i32(rcost, K);
        r->worst = (uint8_t)w;
        d->lim[pos].worst = rcost[w];
        return;
    }
    u_tier2_extra *m = u_row_t2(r);
    int nb = (K + 15) >> 4;
    if (!m->bw_inited) {
        for (int b = 0; b < nb; b++) {
            int lo = b << 4;
            int hi = lo + 16 < K ? lo + 16 : K;
            m->bw_slot[b] = (uint8_t)u_block_argmax(rcost, lo, hi);
        }
        m->bw_inited = 1;
        m->bw_dirty = 0;
    } else {
        uint16_t dirty = m->bw_dirty;
        while (dirty) {
            int b = __builtin_ctz((unsigned)dirty);
            dirty &= (uint16_t)(dirty - 1u);
            int lo = b << 4;
            int hi = lo + 16 < K ? lo + 16 : K;
            m->bw_slot[b] = (uint8_t)u_block_argmax(rcost, lo, hi);
        }
        m->bw_dirty = 0;
    }
    int w = m->bw_slot[0];
    int32_t wc = rcost[w];
    for (int b = 1; b < nb; b++) {
        int s = m->bw_slot[b];
        if (rcost[s] > wc) {
            wc = rcost[s];
            w = s;
        }
    }
    r->worst = (uint8_t)w;
    d->lim[pos].worst = wc;
}

static U_ALWAYS_INLINE void u_t2_mark_dirty(u_dp *d, uint32_t pos, u_row_hdr *r, int s) {
    if (r->tier == 2 && d->use_ht) {
        u_tier2_extra *m = u_row_t2(r);
        if (m->bw_inited && m->bw_slot[s >> 4] == (uint8_t)s)
            m->bw_dirty |= (uint16_t)(1u << (s >> 4));
    }
    if (r->worst == (uint8_t)s) {
        r->worst = 0xff;
        d->lim[pos].worst = INT_MAX / 2;
    }
}

static U_NOINLINE void u_relax_first_row(u_dp *d, uint32_t pos, uint32_t key, int cost,
                                         const u_src_ref *sref) {
    u_row_hdr *r = u_row_alloc(d, 0);
    if (!r)
        return;
    d->rows[pos] = r;
    uint32_t new_bt = u_resolve_bt(d, sref);
    if (d->oom)
        return;
    bt_inc(d, new_bt);
    u_row_key(r)[0] = key;
    u_row_cost(r)[0] = cost;
    u_row_bt(r)[0] = new_bt;
    r->ns = 1;
    r->bloom = 1ull << u_hbit(key);
}

static U_NOINLINE void u_relax_append(u_dp *d, uint32_t pos, u_row_hdr *r, int n, uint32_t key,
                                      int cost, const u_src_ref *sref) {
    if (n == (1 << r->shift)) {
        r = u_row_promote(d, pos, r);
        if (!r)
            return;
    }
    uint32_t new_bt = u_resolve_bt(d, sref);
    if (d->oom)
        return;
    bt_inc(d, new_bt);
    u_row_key(r)[n] = key;
    u_row_cost(r)[n] = cost;
    u_row_bt(r)[n] = new_bt;
    r->ns = (uint8_t)(n + 1);
    if (r->tier == 2 && d->use_ht) {
        u_tier2_extra *e = u_row_t2(r);
        uint32_t p = u_h256(key) & 255u;
        while (e->htab[p] != 0xff)
            p = (p + 1u) & 255u;
        e->htab[p] = (uint8_t)n;
    } else if (r->tier == 1 || r->tier == 2) {
        uint32_t h = u_h256(key);
        u_row_bloom4(r)[h >> 6] |= 1ull << (h & 63u);
    } else {
        r->bloom |= 1ull << u_hbit(key);
    }
}

static U_ALWAYS_INLINE int u_find_small(const u_row_hdr *r, const uint32_t *rkey, int n,
                                        uint32_t key) {
    if (r->tier == 0) {
        if (!(r->bloom & (1ull << u_hbit(key))))
            return -1;
        for (int j = 0; j < n; j++)
            if (rkey[j] == key)
                return j;
        return -1;
    }
    uint32_t h = u_h256(key);
    if (!(((const uint64_t *)((const unsigned char *)(r + 1) + (12u << r->shift)))[h >> 6] &
          (1ull << (h & 63u))))
        return -1;
    return lz0_simd_find_u32(rkey, n, key);
}

static U_ALWAYS_INLINE void u_relax_um(u_dp *d, uint32_t pos, uint32_t key, int cost,
                                       const u_src_ref *sref, int um, const int lit_tie) {
    u_pos_lim *pl = &d->lim[pos];
    if (um < INT_MAX / 8 && cost > pl->best + um)
        return;
    if (!lit_tie && cost >= pl->worst)
        return;
    if (cost < pl->best)
        pl->best = cost;
    u_row_hdr *r = d->rows[pos];
    if (__builtin_expect(!r, 0)) {
        u_relax_first_row(d, pos, key, cost, sref);
        return;
    }
    int n = r->ns;
    int32_t *rcost = u_row_cost(r);
    int w = -1;
    if (n == d->K) {
        if (__builtin_expect(r->worst == 0xff, 0))
            u_find_worst(d, pos, r);
        w = r->worst;
        if (cost > rcost[w] || (!lit_tie && cost == rcost[w]))
            return;
    }
    uint32_t *rkey = u_row_key(r);
    uint32_t *rbt = u_row_bt(r);
    int s = -1;
    uint32_t ht_ins = 0;
    const int is_ht = (r->tier == 2 && d->use_ht);
    if (__builtin_expect(is_ht, 1)) {
        const uint8_t *ht = u_row_t2(r)->htab;
        uint32_t p = u_h256(key) & 255u;
        uint32_t first_tomb = 0xffffffffu;
        for (;;) {
            uint8_t sv = ht[p];
            if (sv == 0xff) {
                ht_ins = (first_tomb != 0xffffffffu) ? first_tomb : p;
                break;
            }
            if (sv == 0xfe) {
                if (first_tomb == 0xffffffffu)
                    first_tomb = p;
            } else if (rkey[sv] == key) {
                s = (int)sv;
                break;
            }
            p = (p + 1u) & 255u;
        }
    } else {
        s = u_find_small(r, rkey, n, key);
    }
    if (s >= 0) {
        int cur = rcost[s];
        if (cost < cur ||
            (lit_tie && cost == cur &&
             lz0_lit_headroom(pos - sref->lit_opos) >
                 lz0_lit_headroom(pos - bt_pos(d, rbt[s])))) {
            uint32_t new_bt = u_resolve_bt(d, sref);
            if (d->oom)
                return;
            bt_inc(d, new_bt);
            uint32_t old_bt = rbt[s];
            rcost[s] = cost;
            rbt[s] = new_bt;
            bt_dec(d, old_bt);
            u_t2_mark_dirty(d, pos, r, s);
        }
        return;
    }
    if (w >= 0 && lit_tie && cost >= rcost[w])
        return;
    if (w < 0) {
        if (__builtin_expect(n < (1 << r->shift), 1)) {
            uint32_t new_bt = u_resolve_bt(d, sref);
            if (d->oom)
                return;
            bt_inc(d, new_bt);
            rkey[n] = key;
            rcost[n] = cost;
            rbt[n] = new_bt;
            r->ns = (uint8_t)(n + 1);
            if (is_ht)
                u_row_t2(r)->htab[ht_ins] = (uint8_t)n;
            else if (r->tier == 1 || r->tier == 2) {
                uint32_t h = u_h256(key);
                u_row_bloom4(r)[h >> 6] |= 1ull << (h & 63u);
            } else {
                r->bloom |= 1ull << u_hbit(key);
            }
        } else {
            u_relax_append(d, pos, r, n, key, cost, sref);
        }
        return;
    }
    /* full row, key absent, cost < worst already established: evict w */
    uint32_t new_bt = u_resolve_bt(d, sref);
    if (d->oom)
        return;
    bt_inc(d, new_bt);
    if (is_ht) {
        u_tier2_extra *e = u_row_t2(r);
        uint32_t old_k = rkey[w];
        uint32_t dp = u_h256(old_k) & 255u;
        while (e->htab[dp] != (uint8_t)w)
            dp = (dp + 1u) & 255u;
        e->htab[ht_ins] = (uint8_t)w;
        e->htab[dp] = 0xfe;
        uint32_t old_bt = rbt[w];
        rkey[w] = key;
        rcost[w] = cost;
        rbt[w] = new_bt;
        bt_dec(d, old_bt);
        if (++e->tomb >= 48)
            u_ht_rebuild(r, rkey);
        u_t2_mark_dirty(d, pos, r, w);
    } else {
        uint32_t old_bt = rbt[w];
        rkey[w] = key;
        rcost[w] = cost;
        rbt[w] = new_bt;
        bt_dec(d, old_bt);
        if (r->tier == 1 || r->tier == 2) {
            uint32_t h = u_h256(key);
            u_row_bloom4(r)[h >> 6] |= 1ull << (h & 63u);
        } else {
            r->bloom |= 1ull << u_hbit(key);
        }
        u_t2_mark_dirty(d, pos, r, w);
    }
}

static U_ALWAYS_INLINE void u_relax_fast(u_dp *d, uint32_t pos, uint32_t key, int cost,
                                         const u_src_ref *sref, int um) {
    u_pos_lim pl = d->lim[pos];
    if ((um >= INT_MAX / 8 || cost <= pl.best + um) && cost < pl.worst)
        u_relax_um(d, pos, key, cost, sref, um, 0);
}

static void u_emit_last(const lz0_finder *f, u_dp *d, uint32_t i, int L, uint32_t off,
                        int cost, const u_src_ref *sref, int um, int fine_lim) {
    if (f->opts.len_code == 3) {
        if (fine_lim == 8) {
            static const uint8_t cost_last_1_8[] = {0, 2, 4, 4, 6, 6, 6, 6, 8};
            int fine = L < 8 ? L : 8;
            for (int l = 1; l <= fine; l++)
                u_relax_fast(d, i + (uint32_t)l, off, cost + (int)cost_last_1_8[l], sref, um);
            if (L < 9)
                return;
            for (int k = 4; k < 31; k++) {
                int h = (int)((1u << k) - 1u);
                int end = L < h ? L : h;
                u_relax_fast(d, i + (uint32_t)end, off, cost + 2 * k, sref, um);
                if (L <= h)
                    return;
            }
            return;
        }
        if (fine_lim == 0) {
            for (int k = 1; k < 31; k++) {
                int h = (int)((1u << k) - 1u);
                int end = L < h ? L : h;
                u_relax_fast(d, i + (uint32_t)end, off, cost + 2 * k, sref, um);
                if (L <= h)
                    return;
            }
            return;
        }
    }
    int fine = L < fine_lim ? L : fine_lim;
    for (int l = 1; l <= fine; l++)
        u_relax_fast(d, i + (uint32_t)l, off, cost + lz0_cost_last_c(&f->opts, l), sref, um);
    int start = fine + 1;
    while (start <= L) {
        int end = lz0_next_plateau_last_c(&f->opts, start, L);
        u_relax_fast(d, i + (uint32_t)end, off, cost + lz0_cost_last_c(&f->opts, end), sref, um);
        start = end + 1;
    }
}

static void u_emit_new(const lz0_finder *f, u_dp *d, uint32_t i, int L, uint32_t off,
                       int cost, const u_src_ref *sref, int allow_len2, int exact, int fine_lim) {
    int off_eb = u_off_eb(off);
    int base = cost + 10 + off_eb;
    int um = exact ? (INT_MAX / 8) : (f->opts.ultra_margin + off_eb);
    if (f->opts.len_code == 3 && f->opts.offset_lsb_bits == 8 && (fine_lim == 8 || exact)) {
        if (fine_lim >= 8) {
            int fine = L < 8 ? L : 8;
            int l = allow_len2 ? 2 : 3;
            int c3 = base + 3;
            for (; l <= fine; l++)
                u_relax_fast(d, i + (uint32_t)l, off, c3, sref, um);
        } else {
            u_relax_fast(d, i + (uint32_t)(L < 8 ? L : 8), off, base + 3, sref, um);
        }
        if (L < 9)
            return;
        u_relax_fast(d, i + 9u, off, base + 4, sref, um);
        if (L < 10)
            return;
        static const int hi[] = {11,   15,   23,   39,    71,    135,   263,  519,
                                 1031, 2055, 4103, 8199, 16391, 32775, 65543};
        int cl = 6;
        int need = 10;
        for (int p = 0; p < 15; p++) {
            int h = hi[p];
            int end = L < h ? L : h;
            u_relax_fast(d, i + (uint32_t)end, off, base + cl, sref, um);
            if (L <= h)
                return;
            cl += 2;
            need = h + 1;
        }
        while (need <= L) {
            int end = lz0_next_plateau_new_c(&f->opts, need, L);
            u_relax_fast(d, i + (uint32_t)end, off, base + u_len_new(end), sref, um);
            need = end + 1;
        }
        return;
    }
    int fine = L < fine_lim ? L : fine_lim;
    for (int l = 2; l <= fine; l++) {
        if (l == 2 && !allow_len2)
            continue;
        u_relax_fast(d, i + (uint32_t)l, off, base + u_len_new(l), sref, um);
    }
    int start = fine + 1;
    if (start < (allow_len2 ? 2 : 3))
        start = allow_len2 ? 2 : 3;
    while (start <= L) {
        int end = lz0_next_plateau_new_c(&f->opts, start, L);
        u_relax_fast(d, i + (uint32_t)end, off,
                     cost + lz0_cost_new_c(&f->opts, (int)off, end), sref, um);
        start = end + 1;
    }
}

int lz0_ultra_states_for_c(const lz0_config *cfg, uint32_t n) {
    int K = cfg ? cfg->ultra_states : lz0_ultra_states;
    size_t budget = cfg ? cfg->ultra_budget : lz0_ultra_budget;
    if (K <= 0) {
        /* auto: as many states as the memory budget allows (12 bytes each) */
        size_t k = budget / (((size_t)n + 1) * 12u);
        K = k > 128 ? 128 : (int)k;
        if (K < 12)
            K = 12;
    }
    if (K < 2)
        K = 2;
    if (K > 255)
        K = 255;
    return K;
}

int lz0_ultra_states_for(uint32_t n) {
    return lz0_ultra_states_for_c(NULL, n);
}

size_t lz0_ram_ultra_c(const lz0_config *cfg, uint32_t n) {
    size_t K = (size_t)lz0_ultra_states_for_c(cfg, n);
    /* Index arrays (best/worst + rows[] = 16 B/byte) plus sparse active rows */
    size_t active_est = (size_t)n < 512u ? (size_t)n : 512u;
    return ((size_t)n + 1) * 16u + active_est * (K * 12u + 16u);
}

size_t lz0_ram_ultra(uint32_t n) {
    return lz0_ram_ultra_c(NULL, n);
}

static void u_free(u_dp *d) {
    free(d->lim);
    free(d->rows);
    for (int t = 0; t < 3; t++) {
        for (uint32_t s = 0; s < d->tiers[t].n_slabs; s++)
            free(d->tiers[t].slabs[s]);
        free(d->tiers[t].slabs);
    }
    free(d->bt);
}

static int u_dp_run(lz0_finder *f, lz0_toklist *tl, int K, int exact) {
    uint32_t n = f->n;
    if (n == 0)
        return -1;
    if (n > U_POS_MASK)
        return -1;
    u_dp d;
    memset(&d, 0, sizeof(d));
    d.n = n;
    d.K = K;
    d.use_ht = (K > 32 && K <= 128);
    d.bt_free = U_BT_NIL;

    int t_shifts[3] = {3, 5, K <= 64 ? 6 : K <= 128 ? 7 : 8};
    for (int t = 0; t < 3; t++) {
        d.tiers[t].shift = t_shifts[t];
        d.tiers[t].cap = 1 << t_shifts[t];
        d.tiers[t].free_head = NULL;
        size_t base = sizeof(u_row_hdr) + ((size_t)12u << t_shifts[t]);
        size_t extra = (t == 1 || (t == 2 && !d.use_ht)) ? 32u : (t == 2) ? sizeof(u_tier2_extra) : 0u;
        d.tiers[t].stride = (base + extra + 7u) & ~(size_t)7u;
    }

    size_t idx_bytes =
        ((size_t)n + 1) * sizeof(u_pos_lim) + ((size_t)n + 1) * sizeof(u_row_hdr *);
    d.lim = (u_pos_lim *)malloc(((size_t)n + 1) * sizeof(u_pos_lim));
    d.rows = (u_row_hdr **)calloc((size_t)n + 1, sizeof(u_row_hdr *));
    if (!d.lim || !d.rows) {
        u_free(&d);
        return -1;
    }
    d.alloc_bytes = idx_bytes;
    for (uint32_t i = 0; i <= n; i++) {
        d.lim[i].best = INT_MAX / 2;
        d.lim[i].worst = INT_MAX / 2;
    }

    uint32_t root = bt_alloc(&d, 0, LZ0_INITIAL_OFFSET, U_BT_NIL);
    u_row_hdr *r0 = u_row_alloc(&d, 0);
    if (d.oom || root == U_BT_NIL || !r0) {
        u_free(&d);
        return -1;
    }
    d.rows[0] = r0;
    bt_inc(&d, root);
    u_row_key(r0)[0] = LZ0_INITIAL_OFFSET;
    u_row_cost(r0)[0] = 0;
    u_row_bt(r0)[0] = root;
    r0->ns = 1;
    d.lim[0].best = 0;

    lz0_match cands[64];
    const int is_len3 = (f->opts.len_code == 3);
    const int lit1 = is_len3 ? 10 : lz0_cost_literal_c(&f->opts, 1);
    const unsigned char *in = f->in;
    const int margin = f->opts.ultra_margin;
    const int always_len2 = !exact && (f->opts.len2_max_off <= 0);
    const int has_census = (f->census != NULL);
    const int full_tier = (K > 32) ? 2 : (K > 8) ? 1 : 0;
    const int use_inj = !exact && f->opts.ultra_inject;
    const int fine_lim = exact ? ((f->opts.sdp_fine_matches == 1 ||
                                   (f->opts.sdp_fine_matches == 2 && f->num_strides == 0))
                                      ? 8
                                      : 0)
                               : f->opts.ultra_fine;
    uint32_t anchor[256];

    for (uint32_t i = 0; i < n; i++) {
        u_row_hdr *ri = d.rows[i];
        if (!ri)
            continue;
        int nsi = ri->ns;
        if (nsi == 0) {
            u_row_free(&d, i);
            continue;
        }
        lz0_finder_insert_upto(f, i);
        int max_left = (int)(n - i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(f, i, 0, cands, &dummy);

        /* Pre-size row i+1 directly when row i already exceeded tier 1. */
        if (nsi > 32 && !d.rows[i + 1]) {
            d.rows[i + 1] = u_row_alloc(&d, full_tier);
            if (d.oom) {
                u_free(&d);
                return -1;
            }
        }

        uint32_t *rkey_i = u_row_key(ri);
        int32_t *rcost_i = u_row_cost(ri);
        uint32_t *rbt_i = u_row_bt(ri);
        int32_t row_best = d.lim[i].best;
        int b = -1, b2 = -1;
        for (int s = 0; s < nsi; s++)
            anchor[s] = U_BT_NIL;

        for (int s = 0; s < nsi; s++) {
            uint32_t key = rkey_i[s];
            uint32_t off = key & ~U_LIT_FLAG;
            int cost = rcost_i[s];
            int um = exact ? (INT_MAX / 8) : u_margin(off, margin);
            if (!exact && cost > row_best + um)
                continue;
            if (b < 0 || cost < rcost_i[b])
                b = s;
            uint32_t orig_bt = rbt_i[s];
            if (!(key & U_LIT_FLAG)) {
                int lcost = cost + lit1;
                if (exact || lcost <= d.lim[i + 1].best + um) {
                    u_src_ref sref = {i, key, orig_bt, i, &anchor[s]};
                    u_relax_um(&d, i + 1, off | U_LIT_FLAG, lcost, &sref, um, 1);
                }
            } else {
                uint32_t opos = bt_pos(&d, orig_bt);
                int prev_lit = is_len3 ? u_cost_lit((int)(i - opos))
                                       : lz0_cost_literal_c(&f->opts, (int)(i - opos));
                int next_lit = is_len3 ? u_cost_lit((int)(i + 1 - opos))
                                       : lz0_cost_literal_c(&f->opts, (int)(i + 1 - opos));
                int lcost = (cost - prev_lit) + next_lit;
                if (exact || lcost <= d.lim[i + 1].best + um) {
                    u_src_ref lit_ref = {i, key, orig_bt, opos, NULL};
                    u_relax_um(&d, i + 1, key, lcost, &lit_ref, um, 1);
                }
                if (off >= 1 && off <= i && in[i] == in[i - off]) {
                    int L;
                    if (!has_census) {
                        if (i >= f->rle_start && i < f->rle_end && off <= i - f->rle_start)
                            L = (int)(f->rle_end - i);
                        else
                            L = 1 + lz0_simd_matchlen(in + i + 1, in + i - off + 1,
                                                      max_left - 1);
                    } else {
                        L = lz0_find_last_len(f, i, off);
                    }
                    if (L > max_left)
                        L = max_left;
                    if (L >= 1) {
                        u_src_ref m_ref = {i, key, orig_bt, i, &anchor[s]};
                        u_emit_last(f, &d, i, L, off, cost, &m_ref, um, fine_lim);
                    }
                }
            }
            if (d.oom)
                break;
        }
        if (d.oom) {
            u_free(&d);
            return -1;
        }
        if (use_inj && i > 0 && b >= 0 && max_left >= 2 && nc < 64) {
            uint32_t cur16 = lz0_ld16(in + i);
            uint32_t inj_off[8];
            int32_t inj_cost[8];
            int ninj = 0;
            for (int s = 0; s < nsi; s++) {
                uint32_t off = rkey_i[s] & ~U_LIT_FLAG;
                if (off == 0 || off > i || lz0_ld16(in + i - off) != cur16)
                    continue;
                int dup = 0;
                for (int k = 0; k < nc; k++) {
                    if (cands[k].offset == off) {
                        dup = 1;
                        break;
                    }
                }
                if (dup)
                    continue;
                int32_t sc = rcost_i[s];
                int seen = 0;
                for (int j = 0; j < ninj; j++) {
                    if (inj_off[j] == off) {
                        if (sc < inj_cost[j])
                            inj_cost[j] = sc;
                        seen = 1;
                        break;
                    }
                }
                if (seen)
                    continue;
                if (ninj < 8) {
                    inj_off[ninj] = off;
                    inj_cost[ninj] = sc;
                    ninj++;
                } else {
                    int w = 0;
                    for (int j = 1; j < 8; j++) {
                        if (inj_cost[j] > inj_cost[w])
                            w = j;
                    }
                    if (sc < inj_cost[w]) {
                        inj_off[w] = off;
                        inj_cost[w] = sc;
                    }
                }
            }
            for (int j = 0; j < ninj && nc < 64; j++) {
                uint32_t off = inj_off[j];
                int ml;
                if (i >= f->rle_start && i < f->rle_end && off <= i - f->rle_start) {
                    ml = (int)(f->rle_end - i);
                    if (ml > max_left)
                        ml = max_left;
                } else {
                    ml = 2 + lz0_simd_matchlen(in + i + 2, in + i - off + 2, max_left - 2);
                }
                if (ml >= 3 || always_len2 || lz0_is_len2_allowed(f, off)) {
                    cands[nc].offset = off;
                    cands[nc].length = (uint32_t)ml;
                    nc++;
                }
            }
        }
        if (i > 0 && b >= 0 && nc > 0) {
            uint32_t boff = rkey_i[b] & ~U_LIT_FLAG;
            for (int s = 0; s < nsi; s++) {
                if ((rkey_i[s] & ~U_LIT_FLAG) != boff &&
                    (b2 < 0 || rcost_i[s] < rcost_i[b2]))
                    b2 = s;
            }
            for (int k = 0; k < nc; k++) {
                int L = (int)cands[k].length;
                uint32_t off = cands[k].offset;
                if (L < 2 || off == 0)
                    continue;
                int l2ok = always_len2 || lz0_is_len2_allowed(f, off);
                if (L == 2 && !l2ok)
                    continue;
                int src = b;
                if (off == boff) {
                    if (b2 < 0)
                        continue;
                    src = b2;
                }
                u_src_ref nref = {i, rkey_i[src], rbt_i[src], i, &anchor[src]};
                u_emit_new(f, &d, i, L, off, rcost_i[src], &nref, l2ok, exact, fine_lim);
                if (d.oom)
                    break;
            }
        }
        if (d.oom) {
            u_free(&d);
            return -1;
        }
        for (int s = 0; s < nsi; s++) {
            if (anchor[s] != U_BT_NIL)
                bt_dec(&d, anchor[s]);
            bt_dec(&d, rbt_i[s]);
        }
        u_row_free(&d, i);
    }

    if (d.oom || !d.rows[n]) {
        u_free(&d);
        return -1;
    }
    u_row_hdr *rn = d.rows[n];
    int nsn = rn->ns;
    if (nsn == 0) {
        u_free(&d);
        return -1;
    }
    int32_t *rcost_n = u_row_cost(rn);
    uint32_t *rkey_n = u_row_key(rn);
    uint32_t *rbt_n = u_row_bt(rn);
    int best = 0;
    for (int s = 1; s < nsn; s++) {
        if (rcost_n[s] < rcost_n[best])
            best = s;
    }

    /* pass 1: validate the DAG chain back to position 0 and count tokens */
    uint32_t m = 0;
    uint32_t p = n;
    uint32_t id = rbt_n[best];
    while (p > 0) {
        if (id == U_BT_NIL) {
            u_free(&d);
            return -1;
        }
        const u_bt_node *bn = &d.bt[id];
        uint32_t prev = bn->pos;
        if (prev >= p) {
            u_free(&d);
            return -1;
        }
        p = prev;
        id = bn->parent;
        m++;
    }

    /* pass 2: same walk, writing tokens back to front in place */
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out) {
        u_free(&d);
        return -1;
    }
    p = n;
    uint32_t cur_key = rkey_n[best];
    id = rbt_n[best];
    for (uint32_t k = m; k-- > 0;) {
        const u_bt_node *bn = &d.bt[id];
        uint32_t prev = bn->pos;
        out[k].pos = prev;
        out[k].length = p - prev;
        out[k].offset = (cur_key & U_LIT_FLAG) ? 0 : cur_key;
        p = prev;
        cur_key = bn->key;
        id = bn->parent;
    }

    f->ultra_peak_ram = d.alloc_bytes;
    u_free(&d);
    return 0;
}

int lz0_parse_ultra(lz0_finder *f, lz0_toklist *tl) {
    if (f->n > U_POS_MASK)
        return lz0_parse_sdp(f, tl);
    return u_dp_run(f, tl, lz0_ultra_states_for_c(&f->opts, f->n), 0);
}

int lz0_parse_ultra_sdp(lz0_finder *f, lz0_toklist *tl) {
    int K = f->max_states > 0 ? f->max_states : 2;
    if (K > 32)
        K = 32;
    if (K > 16 && f->n > 512 * 1024)
        K = 16;
    if (K > 8 && f->n > 2048 * 1024)
        K = 8;
    return u_dp_run(f, tl, K, 1);
}
