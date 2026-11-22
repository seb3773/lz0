/*
 * Exhaustive optimal parser for LZ0 (ZX0-style offset scan).
 *
 * For every position and every offset up to a limit, it tracks the best
 * chain ending with a match at that offset and the best chain ending with a
 * literal run that follows a match at that offset. This is exact for the
 * LZ0 token grammar within the offset limit, at O(n * limit) cost.
 */
#include "lz0_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int bits;
    int index;
    uint32_t offset;
    int chain;
    int ref;
} opt_blk;

typedef struct {
    opt_blk *b;
    int cap;
    int free_head;
} opt_pool;

static int pool_alloc(opt_pool *p, int bits, int index, uint32_t offset, int chain) {
    int id;
    if (p->free_head >= 0) {
        id = p->free_head;
        p->free_head = p->b[id].chain;
    } else {
        /* grow */
        int ncap = p->cap ? p->cap * 2 : 1 << 16;
        opt_blk *nb = (opt_blk *)realloc(p->b, (size_t)ncap * sizeof(opt_blk));
        if (!nb)
            return -1;
        for (int i = ncap - 1; i >= p->cap; i--) {
            nb[i].chain = (i == ncap - 1) ? -1 : i + 1;
        }
        id = p->cap;
        p->free_head = (p->cap + 1 < ncap) ? p->cap + 1 : -1;
        p->b = nb;
        p->cap = ncap;
    }
    opt_blk *k = &p->b[id];
    k->bits = bits;
    k->index = index;
    k->offset = offset;
    k->chain = chain;
    k->ref = 0;
    if (chain >= 0)
        p->b[chain].ref++;
    return id;
}

static void pool_release(opt_pool *p, int id) {
    while (id >= 0) {
        opt_blk *k = &p->b[id];
        if (--k->ref > 0)
            return;
        int next = k->chain;
        k->chain = p->free_head;
        p->free_head = id;
        id = next;
    }
}

static inline void pool_assign(opt_pool *p, int *slot, int id) {
    if (id >= 0)
        p->b[id].ref++;
    if (*slot >= 0)
        pool_release(p, *slot);
    *slot = id;
}

static inline int ebits(int v) {
    return ((32 - __builtin_clz((unsigned)v)) << 1) - 1;
}

/* Bits of the MatchNew length field for length L >= 2. */
static inline int lennew_bits(int L) {
    int v = L - 1;
    return v <= 7 ? 3 : 3 + ebits(v - 7);
}

/* Per-offset state. Match and literal blocks are kept as lazy descriptors
 * and only materialized in the pool when another block needs to point at
 * them, which removes almost all allocations from the inner loop. */
typedef struct {
    int lm_bits;   /* best chain ending with a match at this offset */
    int lm_index;  /* end index of that match, or INT_MIN if none */
    int lm_id;     /* materialized block, or -1 */
    int lm_pred;   /* NEW: predecessor block id; REP: -1 (uses ll) */
    int ll_bits;   /* chain: last match at this offset + literal run */
    int ll_index;  /* end index of the literal run, or INT_MIN if none */
    int ll_id;     /* materialized literal block, or -1 */
    int ll_pred;   /* materialized match block preceding the literals */
    int ml;        /* current match run length at this offset */
} opt_od;

#define OPT_NONE (-2147483647 - 1)

static int od_mat_ll(opt_pool *p, opt_od *d) {
    if (d->ll_id < 0) {
        int id = pool_alloc(p, d->ll_bits, d->ll_index, 0, d->ll_pred);
        if (id < 0)
            return -1;
        p->b[id].ref++;
        d->ll_id = id;
    }
    return d->ll_id;
}

static int od_mat_lm(opt_pool *p, opt_od *d, uint32_t offset) {
    if (d->lm_id < 0) {
        int pred = d->lm_pred;
        if (pred < 0) {
            pred = od_mat_ll(p, d);
            if (pred < 0)
                return -1;
        }
        int id = pool_alloc(p, d->lm_bits, d->lm_index, offset, pred);
        if (id < 0)
            return -1;
        p->b[id].ref++;
        d->lm_id = id;
    }
    return d->lm_id;
}

int lz0_parse_opt(lz0_finder *f, lz0_toklist *tl, int offset_limit) {
    const unsigned char *in = f->in;
    int n = (int)f->n;
    if (n <= 0)
        return -1;
    if (offset_limit <= 0 || offset_limit > n)
        offset_limit = n;
    opt_pool pool = {NULL, 0, -1};
    opt_od *od = (opt_od *)malloc((size_t)(offset_limit + 1) * sizeof(opt_od));
    int *optimal = (int *)malloc((size_t)n * sizeof(int));
    int *opt_bits = (int *)malloc((size_t)n * sizeof(int));
    int *best_length = (int *)malloc((size_t)(n + 2) * sizeof(int));
    int *best_bits = (int *)malloc((size_t)(n + 2) * sizeof(int));
    if (!od || !optimal || !opt_bits || !best_length || !best_bits)
        goto fail;
    for (int o = 0; o <= offset_limit; o++) {
        od[o].lm_index = OPT_NONE;
        od[o].lm_id = -1;
        od[o].lm_pred = -1;
        od[o].ll_index = OPT_NONE;
        od[o].ll_id = -1;
        od[o].ll_pred = -1;
        od[o].ml = 0;
    }
    {
        int fake = pool_alloc(&pool, 0, -1, LZ0_INITIAL_OFFSET, -1);
        if (fake < 0)
            goto fail;
        pool.b[fake].ref++;
        opt_od *d = &od[LZ0_INITIAL_OFFSET];
        d->lm_bits = 0;
        d->lm_index = -1;
        d->lm_id = fake;
    }

    for (int index = 0; index < n; index++) {
        unsigned char cur = in[index];
        int can_match = index != 0;
        int bl_size = 1;
        int max_offset = index > offset_limit ? offset_limit : (index < 1 ? 1 : index);
        int cand_bits = 0x7fffffff;
        int cand_off = 0;
        int cand_lit = 0;
        const unsigned char *src = in + index;
        for (int offset = 1; offset <= max_offset; offset++) {
            opt_od *d = &od[offset];
            if (can_match && cur == src[-offset]) {
                int has = 0;
                if (d->ll_index != OPT_NONE) {
                    int bits = d->ll_bits + 1 + ebits(index - d->ll_index);
                    if (d->lm_id >= 0) {
                        pool_release(&pool, d->lm_id);
                        d->lm_id = -1;
                    }
                    d->lm_bits = bits;
                    d->lm_index = index;
                    d->lm_pred = -1;
                    has = 1;
                }
                if (++d->ml > 1) {
                    int ml = d->ml;
                    if (bl_size < ml) {
                        if (bl_size < 2) {
                            best_length[2] = 2;
                            best_bits[2] = opt_bits[index - 2] + lennew_bits(2);
                            bl_size = 2;
                        }
                        while (bl_size < ml) {
                            bl_size++;
                            int b2 = opt_bits[index - bl_size] + lennew_bits(bl_size);
                            if (b2 <= best_bits[bl_size - 1]) {
                                best_length[bl_size] = bl_size;
                                best_bits[bl_size] = b2;
                            } else {
                                best_length[bl_size] = best_length[bl_size - 1];
                                best_bits[bl_size] = best_bits[bl_size - 1];
                            }
                        }
                    }
                    int bits = best_bits[ml] + 10 + ebits(((offset - 1) >> 8) + 1);
                    if (!has || bits < d->lm_bits) {
                        if (d->lm_id >= 0) {
                            pool_release(&pool, d->lm_id);
                            d->lm_id = -1;
                        }
                        d->lm_bits = bits;
                        d->lm_index = index;
                        d->lm_pred = optimal[index - best_length[ml]];
                        has = 1;
                    }
                }
                if (has && d->lm_bits < cand_bits) {
                    cand_bits = d->lm_bits;
                    cand_off = offset;
                    cand_lit = 0;
                }
            } else {
                d->ml = 0;
                if (d->lm_index != OPT_NONE) {
                    if (d->lm_id < 0 || d->ll_pred != d->lm_id) {
                        /* the last match changed since the previous literal */
                        int id = od_mat_lm(&pool, d, (uint32_t)offset);
                        if (id < 0)
                            goto fail;
                        pool.b[id].ref++;
                        if (d->ll_pred >= 0)
                            pool_release(&pool, d->ll_pred);
                        d->ll_pred = id;
                    }
                    if (d->ll_id >= 0) {
                        pool_release(&pool, d->ll_id);
                        d->ll_id = -1;
                    }
                    int length = index - d->lm_index;
                    d->ll_bits = d->lm_bits + 1 + ebits(length) + (length << 3);
                    d->ll_index = index;
                    if (d->ll_bits < cand_bits) {
                        cand_bits = d->ll_bits;
                        cand_off = offset;
                        cand_lit = 1;
                    }
                }
            }
        }
        if (cand_off == 0)
            goto fail;
        {
            opt_od *d = &od[cand_off];
            int id = cand_lit ? od_mat_ll(&pool, d) : od_mat_lm(&pool, d, (uint32_t)cand_off);
            if (id < 0)
                goto fail;
            pool.b[id].ref++;
            optimal[index] = id;
            opt_bits[index] = cand_bits;
        }
    }

    {
        int cnt = 0;
        for (int id = optimal[n - 1]; id >= 0 && pool.b[id].index >= 0; id = pool.b[id].chain)
            cnt++;
        lz0_token *rev = (lz0_token *)malloc((size_t)(cnt + 1) * sizeof(lz0_token));
        if (!rev)
            goto fail;
        int m = 0;
        for (int id = optimal[n - 1]; id >= 0 && pool.b[id].index >= 0; id = pool.b[id].chain) {
            opt_blk *k = &pool.b[id];
            int prev_index = pool.b[k->chain].index;
            rev[m].pos = (uint32_t)(prev_index + 1);
            rev[m].length = (uint32_t)(k->index - prev_index);
            rev[m].offset = k->offset;
            m++;
        }
        int rc = 0;
        for (int k = m; k-- > 0;) {
            if (lz0_toklist_push(tl, rev[k].pos, rev[k].length, rev[k].offset) < 0) {
                rc = -1;
                break;
            }
        }
        free(rev);
        free(pool.b);
        free(od);
        free(optimal);
        free(opt_bits);
        free(best_length);
        free(best_bits);
        return rc;
    }
fail:
    free(pool.b);
    free(od);
    free(optimal);
    free(opt_bits);
    free(best_length);
    free(best_bits);
    return -1;
}
