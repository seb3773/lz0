#include "lz0_internal.h"
#include "lz0_simd.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

int lz0_sdp_fine_matches = 0;

static double sdp_now_ms(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    return (double)__builtin_ia32_rdtsc() / 2.5e6;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

int lz0_tokens_valid(const lz0_toklist *tl, uint32_t n) {
    uint32_t covered = 0;
    uint32_t last = LZ0_INITIAL_OFFSET;
    int can_last = 0;
    int after_lit = 0;
    if (tl->n == 0 || tl->t[0].offset != 0)
        return 0;
    for (uint32_t i = 0; i < tl->n; i++) {
        const lz0_token *t = &tl->t[i];
        if (t->pos != covered || t->length == 0)
            return 0;
        if (!t->offset) {
            if (after_lit)
                return 0;
            after_lit = 1;
            can_last = 1;
        } else {
            if (t->offset > t->pos)
                return 0;
            if (t->offset == last) {
                if (!can_last)
                    return 0;
            }
            last = t->offset;
            after_lit = 0;
            can_last = 0;
        }
        covered += t->length;
    }
    return covered == n;
}

void lz0_toklist_init(lz0_toklist *tl) {
    tl->t = NULL;
    tl->n = 0;
    tl->cap = 0;
}

void lz0_toklist_free(lz0_toklist *tl) {
    free(tl->t);
    tl->t = NULL;
    tl->n = 0;
    tl->cap = 0;
}

/*
 * Appends m tokens at once and returns the first of them (uninitialised, the
 * caller fills all m), or NULL on overflow / allocation failure. The list
 * grows to exactly n + m entries when it has to grow, so a parser that knows
 * its token count (backtracking parsers count the chain first) neither keeps
 * a temporary reversed copy nor over-allocates by up to 2x.
 */
lz0_token *lz0_toklist_extend(lz0_toklist *tl, uint32_t m) {
    if (m > UINT32_MAX - tl->n)
        return NULL;
    uint32_t need = tl->n + m;
    if (need > tl->cap) {
        lz0_token *nt = (lz0_token *)realloc(tl->t, (size_t)need * sizeof(lz0_token));
        if (!nt)
            return NULL;
        tl->t = nt;
        tl->cap = need;
    }
    lz0_token *base = tl->t + tl->n;
    tl->n = need;
    return base;
}

int lz0_toklist_push(lz0_toklist *tl, uint32_t pos, uint32_t length, uint32_t offset) {
    if (tl->n >= tl->cap) {
        uint32_t nc = tl->cap ? tl->cap * 2 : 256;
        lz0_token *nt = (lz0_token *)realloc(tl->t, (size_t)nc * sizeof(lz0_token));
        if (!nt)
            return -1;
        tl->t = nt;
        tl->cap = nc;
    }
    tl->t[tl->n].pos = pos;
    tl->t[tl->n].length = length;
    tl->t[tl->n].offset = offset;
    tl->n++;
    return 0;
}

typedef struct {
    int kind;
    int len;
    uint32_t offset;
    int cost;
    int savings;
} dec_t;

static void best_match_at(lz0_finder *f, uint32_t i, uint32_t last_off, int allow_last,
                          dec_t *best) {
    best->kind = -1;
    best->len = 0;
    best->offset = 0;
    best->cost = INT_MAX / 4;
    best->savings = -1;

    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(f, i, last_off, cands, &last_len);
    int max_left = (int)(f->n - i);

    if (allow_last && last_len >= 1) {
        int L = last_len;
        if (L > max_left)
            L = max_left;
        int start = 1;
        while (start <= L) {
            int end = lz0_next_plateau_last_c(&f->opts, start, L);
            int c = lz0_cost_last_c(&f->opts, end);
            int sav = (end << 3) - c;
            long long lhs = (long long)c * best->len;
            long long rhs = (long long)best->cost * end;
            if (best->kind < 0 || lhs < rhs || (lhs == rhs && end > best->len)) {
                best->kind = 1;
                best->len = end;
                best->offset = last_off;
                best->cost = c;
                best->savings = sav;
            }
            start = end + 1;
        }
    }

    for (int k = 0; k < nc; k++) {
        int L = (int)cands[k].length;
        uint32_t off = cands[k].offset;
        if (L < 2 || off == 0)
            continue;
        if (off == last_off)
            continue;
        int start = 2;
        if (start == 2 && !lz0_is_len2_allowed(f, off))
            start = 3;
        while (start <= L) {
            int end = lz0_next_plateau_new_c(&f->opts, start, L);
            int c = lz0_cost_new_c(&f->opts, (int)off, end);
            int sav = (end << 3) - c;
            long long lhs = (long long)c * best->len;
            long long rhs = (long long)best->cost * end;
            if (best->kind < 0 || lhs < rhs || (lhs == rhs && end > best->len)) {
                best->kind = 2;
                best->len = end;
                best->offset = off;
                best->cost = c;
                best->savings = sav;
            }
            start = end + 1;
        }
    }
}

int lz0_parse_greedy(lz0_finder *f, lz0_toklist *tl) {
    uint32_t i = 0;
    uint32_t last_off = LZ0_INITIAL_OFFSET;
    uint32_t n = f->n;

    while (i < n) {
        uint32_t lit_start = i;
        dec_t chosen;
        chosen.kind = -1;
        i++;
        while (i < n) {
            lz0_finder_insert_upto(f, i);
            dec_t d;
            best_match_at(f, i, last_off, 1, &d);
            if (d.kind < 0 || d.savings <= 0) {
                i++;
                continue;
            }
            chosen = d;
            break;
        }
        if (lz0_toklist_push(tl, lit_start, i - lit_start, 0) < 0)
            return -1;
        if (i >= n)
            break;
        if (chosen.kind < 0)
            continue;
        if (lz0_toklist_push(tl, i, (uint32_t)chosen.len, chosen.offset) < 0)
            return -1;
        if (chosen.kind == 2)
            last_off = chosen.offset;
        i += (uint32_t)chosen.len;
        while (i < n) {
            lz0_finder_insert_upto(f, i);
            dec_t d;
            best_match_at(f, i, last_off, 0, &d);
            if (d.kind != 2 || d.savings <= 0)
                break;
            if (lz0_toklist_push(tl, i, (uint32_t)d.len, d.offset) < 0)
                return -1;
            last_off = d.offset;
            i += (uint32_t)d.len;
        }
    }
    return 0;
}

lz0_sdp_census *lz0_sdp_census_out = NULL;
lz0_stream_census *lz0_stream_census_out = NULL;

/*
 * Packed state key shared by the K = 2 and the generic SDP parsers:
 *     key = last_off | SDP_LIT_FLAG   (flag set iff the state ends a literal run,
 *                                      i.e. a repeat match may follow: can_last)
 */
#define SDP_LIT_FLAG 0x80000000u

/*
 * K = 2 state: 12 bytes (formerly a 24-byte record carrying last_off,
 * can_last, tok_len and tok_off). The token that reached a state is implied:
 *     tok_len = pos - prev_pos
 *     tok_off = (key & SDP_LIT_FLAG) ? 0 : key
 * (literal runs carry offset 0, both match kinds set last_off = their offset),
 * and with two slots per row the predecessor slot is a single bit packed on
 * top of prev_pos (positions < 2^31, enforced by the parser). Lookup (key
 * equality), insertion order, eviction (slot 1 iff strictly costlier) and the
 * strict "cost <" replacement rule are unchanged, so every decision and the
 * bitstream are identical to the former layout.
 */
#define SDP_PP_SLOT 0x80000000u

typedef struct {
    uint32_t key;  /* last_off | SDP_LIT_FLAG after a literal run */
    int32_t cost;
    uint32_t pp;   /* prev_pos | prev_slot << 31 */
} sdp_st;

static inline uint32_t sdp_pp_pos(uint32_t pp) { return pp & ~SDP_PP_SLOT; }
static inline uint8_t sdp_pp_slot(uint32_t pp) { return (uint8_t)(pp >> 31); }

static inline void sdp_store(sdp_st *d, uint32_t key, int cost, uint32_t prev_pos,
                             uint8_t prev_slot) {
    d->key = key;
    d->cost = cost;
    d->pp = prev_pos | ((uint32_t)prev_slot << 31);
}

/* Decisions are identical to the former form: in a full row (n == 2) a
 * transition with cost >= max(row[0].cost, row[1].cost) can neither improve a
 * matching slot (strict "<") nor evict the worst one, so it returns at once;
 * below that bound, a non-matching key always evicts the worst slot w. */
static inline void sdp_relax2_impl(sdp_st *row, uint8_t *nslot, uint32_t key, int cost,
                                   uint32_t prev_pos, uint8_t prev_slot,
                                   uint32_t lit_pos, const int lit_tie) {
    uint8_t n = *nslot;
    if (n == 2) {
        int c0 = row[0].cost, c1 = row[1].cost;
        int max_c = c0 > c1 ? c0 : c1;
        if (cost > max_c || (!lit_tie && cost == max_c))
            return;
        if (row[0].key == key) {
            if (cost < c0 ||
                (lit_tie && cost == c0 &&
                 lz0_lit_headroom(lit_pos - prev_pos) >
                     lz0_lit_headroom(lit_pos - sdp_pp_pos(row[0].pp))))
                sdp_store(&row[0], key, cost, prev_pos, prev_slot);
            return;
        }
        if (row[1].key == key) {
            if (cost < c1 ||
                (lit_tie && cost == c1 &&
                 lz0_lit_headroom(lit_pos - prev_pos) >
                     lz0_lit_headroom(lit_pos - sdp_pp_pos(row[1].pp))))
                sdp_store(&row[1], key, cost, prev_pos, prev_slot);
            return;
        }
        if (lit_tie && cost >= max_c)
            return;
        sdp_store(&row[c1 > c0 ? 1 : 0], key, cost, prev_pos, prev_slot);
        return;
    }
    if (n > 0 && row[0].key == key) {
        if (cost < row[0].cost ||
            (lit_tie && cost == row[0].cost &&
             lz0_lit_headroom(lit_pos - prev_pos) >
                 lz0_lit_headroom(lit_pos - sdp_pp_pos(row[0].pp))))
            sdp_store(&row[0], key, cost, prev_pos, prev_slot);
        return;
    }
    if (n > 1 && row[1].key == key) {
        if (cost < row[1].cost ||
            (lit_tie && cost == row[1].cost &&
             lz0_lit_headroom(lit_pos - prev_pos) >
                 lz0_lit_headroom(lit_pos - sdp_pp_pos(row[1].pp))))
            sdp_store(&row[1], key, cost, prev_pos, prev_slot);
        return;
    }
    sdp_store(&row[n], key, cost, prev_pos, prev_slot);
    *nslot = (uint8_t)(n + 1);
}

static inline void sdp_relax2(sdp_st *row, uint8_t *nslot, uint32_t key, int cost,
                              uint32_t prev_pos, uint8_t prev_slot) {
    sdp_relax2_impl(row, nslot, key, cost, prev_pos, prev_slot, 0, 0);
}

static inline void sdp_relax2_lit(sdp_st *row, uint8_t *nslot, uint32_t key, int cost,
                                  uint32_t prev_pos, uint8_t prev_slot, uint32_t lit_pos) {
    sdp_relax2_impl(row, nslot, key, cost, prev_pos, prev_slot, lit_pos, 1);
}

static inline int sdp_use_fine(const lz0_finder *f) {
    /* Selective fine matches (`== 2`): skip strided inputs (`num_strides > 0`),
     * where sub-plateau extensions crowd 2-slot rows with redundant short offsets. */
    return f->opts.sdp_fine_matches == 1 ||
           (f->opts.sdp_fine_matches == 2 && f->num_strides == 0);
}

static void sdp_emit_last2(const lz0_finder *f, sdp_st *st, uint8_t *ns, uint32_t i, int L,
                           uint32_t last_off, int cost, uint8_t slot) {
    lz0_sdp_census *cen = (lz0_sdp_census *)f->census;
    int fine = sdp_use_fine(f);
    if (f->opts.len_code == 3) {
        if (fine) {
            static const uint8_t cost_last_1_8[] = {0, 2, 4, 4, 6, 6, 6, 6, 8};
            int max_fine = L < 8 ? L : 8;
            for (int l = 1; l <= max_fine; l++) {
                sdp_relax2(st + (size_t)(i + (uint32_t)l) * 2u, &ns[i + (uint32_t)l],
                           last_off, cost + (int)cost_last_1_8[l], i, slot);
                if (cen) {
                    cen->n_trans_last++;
                    cen->n_last_plat++;
                }
            }
            if (L <= 8)
                return;
            uint32_t hi = 15u;
            int add = 8;
            for (;;) {
                int end = hi < (uint32_t)L ? (int)hi : L;
                sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end],
                           last_off, cost + add, i, slot);
                if (cen) {
                    cen->n_trans_last++;
                    cen->n_last_plat++;
                }
                if (end == L)
                    return;
                hi = (hi << 1) | 1u;
                add += 2;
            }
        }
        /* last-match cost = 1 + elias(end); Elias plateaus end at 2^k - 1,
         * where the cost is 1 + (2k - 1) = 2k */
        uint32_t hi = 1u;
        int add = 2;
        for (;;) {
            int end = hi < (uint32_t)L ? (int)hi : L;
            sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end],
                       last_off, cost + add, i, slot);
            if (cen) {
                cen->n_trans_last++;
                cen->n_last_plat++;
            }
            if (end == L)
                return;
            hi = (hi << 1) | 1u;
            add += 2;
        }
    }
    int start = 1;
    if (fine) {
        int max_fine = L < 8 ? L : 8;
        for (int l = 1; l <= max_fine; l++) {
            sdp_relax2(st + (size_t)(i + (uint32_t)l) * 2u, &ns[i + (uint32_t)l], last_off,
                       cost + lz0_cost_last_c(&f->opts, l), i, slot);
            if (cen) {
                cen->n_trans_last++;
                cen->n_last_plat++;
            }
        }
        start = 9;
    }
    while (start <= L) {
        int end = lz0_next_plateau_last_c(&f->opts, start, L);
        sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], last_off,
                   cost + lz0_cost_last_c(&f->opts, end), i, slot);
        if (cen) {
            cen->n_trans_last++;
            cen->n_last_plat++;
        }
        start = end + 1;
    }
}

static inline int sdp_elias_bits(int value) {
    if (value <= 0)
        return 1;
    return ((32 - __builtin_clz((unsigned)value)) << 1) - 1;
}

static inline void sdp_emit_one_new2(sdp_st *st, uint8_t *ns, uint32_t i, int end,
                                     uint32_t off, int cc, uint8_t slot,
                                     lz0_sdp_census *cen) {
    sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], off, cc, i,
               slot);
    if (cen)
        cen->n_trans_new++;
}

static void sdp_emit_new2(const lz0_finder *f, sdp_st *st, uint8_t *ns, uint32_t i, int L, uint32_t off,
                          int cost, uint8_t slot) {
    lz0_sdp_census *cen = (lz0_sdp_census *)f->census;
    int len2 = lz0_is_len2_allowed(f, off);
    if (L < 2 || (L == 2 && !len2))
        return;
    if (f->opts.len_code == 3 && f->opts.offset_lsb_bits == 8) {
        int cbase = cost + 10 + sdp_elias_bits((((int)off - 1) >> 8) + 1);
        if (sdp_use_fine(f)) {
            int max_fine = L < 8 ? L : 8;
            for (int l = len2 ? 2 : 3; l <= max_fine; l++) {
                sdp_emit_one_new2(st, ns, i, l, off, cbase + 3, slot, cen);
                if (cen)
                    cen->n_new_plat_fixed++;
            }
        } else {
            sdp_emit_one_new2(st, ns, i, L < 8 ? L : 8, off, cbase + 3, slot, cen);
            if (cen)
                cen->n_new_plat_fixed++;
        }
        if (L < 9)
            return;
        sdp_emit_one_new2(st, ns, i, 9, off, cbase + 4, slot, cen);
        if (cen)
            cen->n_new_plat_fixed++;
        if (L < 10)
            return;
        static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                                 1031, 2055, 4103, 8199,  16391, 32775, 65543};
        int cl = 6;
        int need = 10;
        for (int p = 0; p < 15 && L >= need; p++) {
            int h = hi[p];
            int end = L < h ? L : h;
            sdp_emit_one_new2(st, ns, i, end, off, cbase + cl, slot, cen);
            if (cen)
                cen->n_new_plat_hi++;
            if (L <= h)
                return;
            cl += 2;
            need = h + 1;
        }
        while (need <= L) {
            int end = lz0_next_plateau_new_c(&f->opts, need, L);
            sdp_emit_one_new2(st, ns, i, end, off, cost + lz0_cost_new_c(&f->opts, (int)off, end), slot,
                             cen);
            if (cen)
                cen->n_new_plat_ovf++;
            need = end + 1;
        }
        return;
    }
    int start = 2;
    if (start == 2 && !lz0_is_len2_allowed(f, off))
        start = 3;
    while (start <= L) {
        int end = lz0_next_plateau_new_c(&f->opts, start, L);
        sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], off,
                   cost + lz0_cost_new_c(&f->opts, (int)off, end), i, slot);
        if (cen)
            cen->n_trans_new++;
        start = end + 1;
    }
}

/* Backtracking for the K = 2 rows (the generic parser uses sdp_rows_backtrack).
 * Two walks of the predecessor chain: the first validates it and counts the
 * tokens, the second writes them back to front straight into the token list,
 * so no reversed copy of up to n tokens is ever allocated. */
static int sdp_backtrack(const sdp_st *st, const uint8_t *ns, uint32_t n, lz0_toklist *tl) {
    if (ns[n] == 0)
        return -1;
    int best = 0;
    const sdp_st *endrow = st + (size_t)n * 2u;
    for (int s = 1; s < ns[n]; s++) {
        if (endrow[s].cost < endrow[best].cost)
            best = s;
    }
    uint32_t m = 0;
    uint32_t p = n;
    uint32_t slot = (uint32_t)best;
    while (p > 0) {
        uint32_t prev = sdp_pp_pos(st[(size_t)p * 2u + slot].pp);
        if (prev >= p)
            return -1;
        slot = sdp_pp_slot(st[(size_t)p * 2u + slot].pp);
        p = prev;
        m++;
    }
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out)
        return -1;
    p = n;
    slot = (uint32_t)best;
    for (uint32_t k = m; k-- > 0;) {
        const sdp_st *cur = &st[(size_t)p * 2u + slot];
        uint32_t prev = sdp_pp_pos(cur->pp);
        out[k].pos = prev;
        out[k].length = p - prev;
        out[k].offset = (cur->key & SDP_LIT_FLAG) ? 0 : cur->key;
        slot = sdp_pp_slot(cur->pp);
        p = prev;
    }
    return 0;
}

static int lz0_parse_sdp_k2(lz0_finder *f, lz0_toklist *tl) {
    uint32_t n = f->n;
    /* prev_pos shares its word with the slot bit, offsets with the literal flag */
    if (n >= SDP_PP_SLOT)
        return -1;
    lz0_sdp_census *cen = (lz0_sdp_census *)f->census;
    sdp_st *st = (sdp_st *)malloc((size_t)(n + 1) * 2u * sizeof(sdp_st));
    uint8_t *ns = (uint8_t *)calloc((size_t)(n + 1), sizeof(uint8_t));
    if (!st || !ns) {
        free(st);
        free(ns);
        return -1;
    }
    sdp_store(&st[0], LZ0_INITIAL_OFFSET, 0, 0, 0);
    /* rows are only read below their live count; clearing the rest of the
     * first two rows keeps GCC's -Wmaybe-uninitialized analysis quiet */
    memset(&st[1], 0, (n >= 1 ? 3u : 1u) * sizeof(sdp_st));
    ns[0] = 1;
    double ms_insert = 0, ms_find = 0, ms_trans = 0;
    double ms_lit = 0, ms_lastlen = 0, ms_last = 0, ms_new = 0;
    int timed = cen != NULL;

    /* 10 bits (flag + elias(1) + 8) in v0.4.0; hoisted for other codes */
    const int lit1 = f->opts.len_code == 3 ? 10 : lz0_cost_literal_c(&f->opts, 1);
    lz0_match cands[64];
    for (uint32_t i = 0; i < n; i++) {
        int nsi = ns[i];
        if (nsi == 0)
            continue;
        double t0 = timed ? sdp_now_ms() : 0;
        lz0_finder_insert_upto(f, i);
        double t1 = timed ? sdp_now_ms() : 0;
        int max_left = (int)(n - i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(f, i, 0, cands, &dummy);
        double t2 = timed ? sdp_now_ms() : 0;
        sdp_st *row = st + (size_t)i * 2u;
        for (int s = 0; s < nsi; s++) {
            const sdp_st *cur = &row[s];
            uint32_t key = cur->key;
            uint32_t last_off = key & ~SDP_LIT_FLAG;
            int can_last = (key & SDP_LIT_FLAG) != 0;
            int cost = cur->cost;
            uint8_t slot = (uint8_t)s;
            double tl0 = timed ? sdp_now_ms() : 0;
            if (!can_last && max_left >= 1) {
                sdp_relax2_lit(st + (size_t)(i + 1) * 2u, &ns[i + 1], key | SDP_LIT_FLAG,
                               cost + lit1, i, slot, i + 1);
                if (cen)
                    cen->n_trans_lit++;
            }
            /* a literal-run state (tok_off == 0) extends its run from the
             * origin: tok_len = i - prev_pos (always >= 1 here) */
            uint32_t opos = sdp_pp_pos(cur->pp);
            if (can_last && max_left >= 1 && opos < i) {
                uint8_t oslot = sdp_pp_slot(cur->pp);
                const sdp_st *origin = &st[(size_t)opos * 2u + oslot];
                sdp_relax2_lit(st + (size_t)(i + 1) * 2u, &ns[i + 1], key,
                               origin->cost + lz0_cost_literal_c(&f->opts, (int)(i - opos + 1)),
                               opos, oslot, i + 1);
                if (cen)
                    cen->n_trans_lit++;
            }
            double tl1 = timed ? sdp_now_ms() : 0;
            if (can_last) {
                int L = lz0_find_last_len(f, i, last_off);
                if (L > max_left)
                    L = max_left;
                double tl2 = timed ? sdp_now_ms() : 0;
                if (L >= 1)
                    sdp_emit_last2(f, st, ns, i, L, last_off, cost, slot);
                if (timed) {
                    ms_lastlen += tl2 - tl1;
                    ms_last += sdp_now_ms() - tl2;
                }
            }
            if (timed)
                ms_lit += tl1 - tl0;
        }
        if (i > 0) {
            double tn0 = timed ? sdp_now_ms() : 0;
            int b = 0;
            if (nsi > 1 && row[1].cost < row[0].cost)
                b = 1;
            /* nsi <= 2: the second source is the other slot iff its offset differs */
            int b2 = -1;
            if (nsi > 1 && ((row[0].key ^ row[1].key) & ~SDP_LIT_FLAG) != 0)
                b2 = 1 - b;
            for (int k = 0; k < nc; k++) {
                int L = (int)cands[k].length;
                uint32_t off = cands[k].offset;
                if (L < 2 || off == 0)
                    continue;
                if (L == 2 && !lz0_is_len2_allowed(f, off))
                    continue;
                int src = b;
                if (off == (row[b].key & ~SDP_LIT_FLAG)) {
                    if (b2 < 0)
                        continue;
                    src = b2;
                }
                if (cen)
                    cen->n_emit_new++;
                sdp_emit_new2(f, st, ns, i, L, off, row[src].cost, (uint8_t)src);
            }
            if (timed)
                ms_new += sdp_now_ms() - tn0;
        }
        if (timed) {
            ms_insert += t1 - t0;
            ms_find += t2 - t1;
            ms_trans += sdp_now_ms() - t2;
        }
        if (cen) {
            cen->sum_states += (uint64_t)nsi;
            if ((uint32_t)nsi > cen->max_states)
                cen->max_states = (uint32_t)nsi;
            cen->sum_distinct_last +=
                (nsi > 1 && ((row[0].key ^ row[1].key) & ~SDP_LIT_FLAG) != 0) ? 2u : 1u;
        }
    }
    double t_back0 = timed ? sdp_now_ms() : 0;
    if (cen) {
        cen->n_pos = n;
        cen->ram = lz0_ram_sdp(n, 2);
        cen->ms_insert = ms_insert;
        cen->ms_find = ms_find;
        cen->ms_trans = ms_trans;
        cen->ms_trans_lit = ms_lit;
        cen->ms_trans_lastlen = ms_lastlen;
        cen->ms_trans_last = ms_last;
        cen->ms_trans_new = ms_new;
        if (ns[n] > 0) {
            cen->sum_states += (uint64_t)ns[n];
            if ((uint32_t)ns[n] > cen->max_states)
                cen->max_states = (uint32_t)ns[n];
        }
    }
    int rc = sdp_backtrack(st, ns, n, tl);
    free(st);
    free(ns);
    if (cen && timed)
        cen->ms_back = sdp_now_ms() - t_back0;
    return rc;
}

/*
 * Generic SDP parser (K > 2): rows of up to K states per position, stored as
 * structure-of-arrays (same layout idea as the -9 parser in lz0_ultra.c).
 *
 * A state is identified by a packed 32-bit key
 *     key = last_off | SDP_LIT_FLAG   (flag set iff the state ends a literal run,
 *                                      i.e. a repeat match may follow)
 * Keys are unique within a row, so a row lookup is a single u32 vector search.
 *
 * The token that reached a state is not stored, it is implied by the key:
 *     tok_len = pos - prev_pos
 *     tok_off = (key & SDP_LIT_FLAG) ? 0 : key
 * (literal runs carry offset 0, both match kinds set last_off = their offset).
 * A state therefore takes 13 bytes (key, cost, prev_pos, prev_slot) instead of
 * the 24-byte AoS record. Each row also keeps a 64-bit key filter (O(1)
 * rejection of absent keys before the vector search) and its cached worst slot.
 *
 * Every decision (lookup, insert position, evicted slot = lowest index of the
 * highest cost, best/second-best source) is identical to the former AoS
 * implementation, so the token list and the bitstream are unchanged.
 */
typedef struct {
    int K;
    uint32_t *key;      /* [pos*K + s] last_off | SDP_LIT_FLAG after a literal run */
    int32_t *cost;      /* [pos*K + s] */
    uint32_t *prev_pos; /* [pos*K + s] position of the predecessor state */
    uint8_t *prev_slot; /* [pos*K + s] slot of the predecessor state */
    uint8_t *ns;        /* live slots per position (<= K) */
    uint8_t *worst;     /* cached worst slot of a full row, 0xff = unknown */
    uint64_t *bloom;    /* per-row 64-bit key filter (bits are never cleared) */
} sdp_rows;

static inline uint64_t sdp_key_bit(uint32_t key) {
    return 1ull << ((key * 0x9E3779B1u) >> 26);
}

static void sdp_rows_free(sdp_rows *d) {
    free(d->key);
    free(d->cost);
    free(d->prev_pos);
    free(d->prev_slot);
    free(d->ns);
    free(d->worst);
    free(d->bloom);
}

static inline void sdp_relax_impl(sdp_rows *d, uint32_t pos, uint32_t key, int cost,
                                  uint32_t prev_pos, uint8_t prev_slot, lz0_sdp_census *cen,
                                  const int lit_tie) {
    const int K = d->K;
    size_t base = (size_t)pos * (size_t)K;
    int n = d->ns[pos];
    if (cen)
        cen->n_relax++;
    int w = -1;
    if (n == K) {
        w = d->worst[pos];
        if (w == 0xff) {
            w = lz0_simd_argmax_i32(d->cost + base, K);
            d->worst[pos] = (uint8_t)w;
        }
        int wc = d->cost[base + (size_t)w];
        if (cost > wc || (!lit_tie && cost == wc))
            return;
    }
    /* keys are unique within a row; the filter rejects most absent keys
     * without searching (a false positive only costs the vector search) */
    uint64_t bit = sdp_key_bit(key);
    int s = (d->bloom[pos] & bit) ? lz0_simd_find_u32(d->key + base, n, key) : -1;
    if (s >= 0) {
        int cur = d->cost[base + (size_t)s];
        if (cost < cur ||
            (lit_tie && cost == cur &&
             lz0_lit_headroom(pos - prev_pos) >
                 lz0_lit_headroom(pos - d->prev_pos[base + (size_t)s]))) {
            d->cost[base + (size_t)s] = cost;
            d->prev_pos[base + (size_t)s] = prev_pos;
            d->prev_slot[base + (size_t)s] = prev_slot;
            /* a lower cost can only move the argmax away from this slot */
            if (d->worst[pos] == (uint8_t)s)
                d->worst[pos] = 0xff;
        }
        return;
    }
    if (w >= 0 && lit_tie && cost >= d->cost[base + (size_t)w])
        return;
    if (n < K) {
        d->key[base + (size_t)n] = key;
        d->cost[base + (size_t)n] = cost;
        d->prev_pos[base + (size_t)n] = prev_pos;
        d->prev_slot[base + (size_t)n] = prev_slot;
        d->ns[pos] = (uint8_t)(n + 1);
        d->worst[pos] = 0xff;
        d->bloom[pos] |= bit;
        return;
    }
    /* full row: evict the slot holding the highest cost (cost < worst_cost is guaranteed) */
    if (cen)
        cen->n_evicted++;
    d->key[base + (size_t)w] = key;
    d->cost[base + (size_t)w] = cost;
    d->prev_pos[base + (size_t)w] = prev_pos;
    d->prev_slot[base + (size_t)w] = prev_slot;
    d->worst[pos] = 0xff;
    d->bloom[pos] |= bit;
}

static inline void sdp_relax(sdp_rows *d, uint32_t pos, uint32_t key, int cost,
                             uint32_t prev_pos, uint8_t prev_slot, lz0_sdp_census *cen) {
    sdp_relax_impl(d, pos, key, cost, prev_pos, prev_slot, cen, 0);
}

static inline void sdp_relax_lit(sdp_rows *d, uint32_t pos, uint32_t key, int cost,
                                 uint32_t prev_pos, uint8_t prev_slot, lz0_sdp_census *cen) {
    sdp_relax_impl(d, pos, key, cost, prev_pos, prev_slot, cen, 1);
}

static void sdp_emit_last(const lz0_finder *f, sdp_rows *d, uint32_t i, int L, uint32_t last_off,
                          int cost, uint8_t slot, lz0_sdp_census *cen) {
    int fine = sdp_use_fine(f);
    if (f->opts.len_code == 3) {
        if (fine) {
            static const uint8_t cost_last_1_8[] = {0, 2, 4, 4, 6, 6, 6, 6, 8};
            int max_fine = L < 8 ? L : 8;
            for (int l = 1; l <= max_fine; l++)
                sdp_relax(d, i + (uint32_t)l, last_off, cost + (int)cost_last_1_8[l], i, slot, cen);
            if (L <= 8)
                return;
            uint32_t hi = 15u;
            int add = 8;
            for (;;) {
                int end = hi < (uint32_t)L ? (int)hi : L;
                sdp_relax(d, i + (uint32_t)end, last_off, cost + add, i, slot, cen);
                if (end == L)
                    return;
                hi = (hi << 1) | 1u;
                add += 2;
            }
        }
        uint32_t hi = 1u;
        int add = 2;
        for (;;) {
            int end = hi < (uint32_t)L ? (int)hi : L;
            sdp_relax(d, i + (uint32_t)end, last_off, cost + add, i, slot, cen);
            if (end == L)
                return;
            hi = (hi << 1) | 1u;
            add += 2;
        }
    }
    int start = 1;
    if (fine) {
        int max_fine = L < 8 ? L : 8;
        for (int l = 1; l <= max_fine; l++)
            sdp_relax(d, i + (uint32_t)l, last_off, cost + lz0_cost_last_c(&f->opts, l), i, slot, cen);
        start = 9;
    }
    while (start <= L) {
        int end = lz0_next_plateau_last_c(&f->opts, start, L);
        sdp_relax(d, i + (uint32_t)end, last_off, cost + lz0_cost_last_c(&f->opts, end), i, slot, cen);
        start = end + 1;
    }
}

static void sdp_emit_new(const lz0_finder *f, sdp_rows *d, uint32_t i, int L, uint32_t off,
                         int cost, uint8_t slot, lz0_sdp_census *cen) {
    int len2 = lz0_is_len2_allowed(f, off);
    if (L < 2 || (L == 2 && !len2))
        return;
    if (f->opts.len_code == 3 && f->opts.offset_lsb_bits == 8) {
        int cbase = cost + 10 + sdp_elias_bits((((int)off - 1) >> 8) + 1);
        if (sdp_use_fine(f)) {
            int max_fine = L < 8 ? L : 8;
            for (int l = len2 ? 2 : 3; l <= max_fine; l++)
                sdp_relax(d, i + (uint32_t)l, off, cbase + 3, i, slot, cen);
        } else {
            sdp_relax(d, i + (uint32_t)(L < 8 ? L : 8), off, cbase + 3, i, slot, cen);
        }
        if (L < 9)
            return;
        sdp_relax(d, i + 9u, off, cbase + 4, i, slot, cen);
        if (L < 10)
            return;
        static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                                 1031, 2055, 4103, 8199,  16391, 32775, 65543};
        int cl = 6;
        int need = 10;
        for (int p = 0; p < 15 && L >= need; p++) {
            int h = hi[p];
            int end = L < h ? L : h;
            sdp_relax(d, i + (uint32_t)end, off, cbase + cl, i, slot, cen);
            if (L <= h)
                return;
            cl += 2;
            need = h + 1;
        }
        while (need <= L) {
            int end = lz0_next_plateau_new_c(&f->opts, need, L);
            sdp_relax(d, i + (uint32_t)end, off, cost + lz0_cost_new_c(&f->opts, (int)off, end), i, slot,
                      cen);
            need = end + 1;
        }
        return;
    }
    int start;
    if (sdp_use_fine(f)) {
        int max_fine = L < 8 ? L : 8;
        for (int l = len2 ? 2 : 3; l <= max_fine; l++)
            sdp_relax(d, i + (uint32_t)l, off, cost + lz0_cost_new_c(&f->opts, (int)off, l), i, slot, cen);
        start = 9;
    } else {
        start = len2 ? 2 : 3;
    }
    while (start <= L) {
        int end = lz0_next_plateau_new_c(&f->opts, start, L);
        sdp_relax(d, i + (uint32_t)end, off, cost + lz0_cost_new_c(&f->opts, (int)off, end), i, slot, cen);
        start = end + 1;
    }
}

static int sdp_rows_backtrack(const sdp_rows *d, uint32_t n, lz0_toklist *tl) {
    const size_t K = (size_t)d->K;
    if (d->ns[n] == 0)
        return -1;
    const int32_t *endcost = d->cost + (size_t)n * K;
    int best = 0;
    for (int s = 1; s < d->ns[n]; s++) {
        if (endcost[s] < endcost[best])
            best = s;
    }
    /* pass 1: validate the chain and count its tokens (no reversed copy) */
    uint32_t m = 0;
    uint32_t p = n;
    size_t slot = (size_t)best;
    while (p > 0) {
        size_t c = (size_t)p * K + slot;
        uint32_t prev = d->prev_pos[c];
        if (prev >= p)
            return -1;
        slot = d->prev_slot[c];
        p = prev;
        m++;
    }
    /* pass 2: same walk, tokens written back to front in place */
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out)
        return -1;
    p = n;
    slot = (size_t)best;
    for (uint32_t k = m; k-- > 0;) {
        size_t c = (size_t)p * K + slot;
        uint32_t prev = d->prev_pos[c];
        uint32_t key = d->key[c];
        out[k].pos = prev;
        out[k].length = p - prev;
        out[k].offset = (key & SDP_LIT_FLAG) ? 0 : key;
        slot = d->prev_slot[c];
        p = prev;
    }
    return 0;
}

int lz0_parse_sdp(lz0_finder *f, lz0_toklist *tl) {
    uint32_t n = f->n;
    int K = f->max_states > 0 ? f->max_states : 2;
    if (K > 32)
        K = 32;
    /* Adaptive K according to file size in Ultra: K=32 for <= 512KB, K=16 for <= 2MB, K=8 for > 2MB */
    if (K > 16 && n > 512 * 1024)
        K = 16;
    if (K > 8 && n > 2048 * 1024)
        K = 8;
    if (K == 2)
        return lz0_parse_sdp_k2(f, tl);
    if (!f->census && n <= 0x00ffffffu)
        return lz0_parse_ultra_sdp(f, tl);
    /* offsets must leave room for the literal flag in the packed key */
    if (n >= SDP_LIT_FLAG)
        return -1;
    lz0_sdp_census *cen = (lz0_sdp_census *)f->census;
    sdp_rows d;
    memset(&d, 0, sizeof(d));
    d.K = K;
    size_t cells = ((size_t)n + 1) * (size_t)K;
    d.key = (uint32_t *)malloc(cells * sizeof(uint32_t));
    d.cost = (int32_t *)malloc(cells * sizeof(int32_t));
    d.prev_pos = (uint32_t *)malloc(cells * sizeof(uint32_t));
    d.prev_slot = (uint8_t *)malloc(cells);
    d.ns = (uint8_t *)calloc((size_t)n + 1, 1);
    d.worst = (uint8_t *)malloc((size_t)n + 1);
    d.bloom = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    if (!d.key || !d.cost || !d.prev_pos || !d.prev_slot || !d.ns || !d.worst || !d.bloom) {
        sdp_rows_free(&d);
        return -1;
    }
    memset(d.worst, 0xff, (size_t)n + 1);
    d.bloom[0] = sdp_key_bit(LZ0_INITIAL_OFFSET);
    d.key[0] = LZ0_INITIAL_OFFSET;
    d.cost[0] = 0;
    d.prev_pos[0] = 0;
    d.prev_slot[0] = 0;
    d.ns[0] = 1;

    lz0_match cands[64];
    const int lit1 = (f->opts.len_code == 3) ? 10 : lz0_cost_literal_c(&f->opts, 1);
    for (uint32_t i = 0; i < n; i++) {
        int nsi = d.ns[i];
        if (nsi == 0)
            continue;
        lz0_finder_insert_upto(f, i);
        int max_left = (int)(n - i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(f, i, 0, cands, &dummy);
        size_t base = (size_t)i * (size_t)K;
        const uint32_t *rkey = d.key + base;
        const int32_t *rcost = d.cost + base;
        for (int s = 0; s < nsi; s++) {
            uint32_t key = rkey[s];
            uint32_t last_off = key & ~SDP_LIT_FLAG;
            int cost = rcost[s];
            uint8_t slot = (uint8_t)s;
            if (!(key & SDP_LIT_FLAG)) {
                /* start a literal run */
                sdp_relax_lit(&d, i + 1, key | SDP_LIT_FLAG, cost + lit1, i, slot,
                              cen);
            } else {
                /* extend the literal run from its origin (states at i > 0 always
                 * have prev_pos < i, so the run length is >= 1) */
                uint32_t opos = d.prev_pos[base + (size_t)s];
                uint8_t oslot = d.prev_slot[base + (size_t)s];
                int ocost = d.cost[(size_t)opos * (size_t)K + oslot];
                sdp_relax_lit(&d, i + 1, key, ocost + lz0_cost_literal_c(&f->opts, (int)(i + 1 - opos)),
                              opos, oslot, cen);
                int L = lz0_find_last_len(f, i, last_off);
                if (L > max_left)
                    L = max_left;
                if (L >= 1)
                    sdp_emit_last(f, &d, i, L, last_off, cost, slot, cen);
            }
        }
        if (i > 0 && nc > 0) {
            int b = 0;
            for (int s2 = 1; s2 < nsi; s2++) {
                if (rcost[s2] < rcost[b])
                    b = s2;
            }
            uint32_t boff = rkey[b] & ~SDP_LIT_FLAG;
            int b2 = -1;
            for (int s2 = 0; s2 < nsi; s2++) {
                if ((rkey[s2] & ~SDP_LIT_FLAG) != boff && (b2 < 0 || rcost[s2] < rcost[b2]))
                    b2 = s2;
            }
            for (int k = 0; k < nc; k++) {
                int L = (int)cands[k].length;
                uint32_t off = cands[k].offset;
                if (L < 2 || off == 0)
                    continue;
                if (L == 2 && !lz0_is_len2_allowed(f, off))
                    continue;
                int src = b;
                if (off == boff) {
                    if (b2 < 0)
                        continue;
                    src = b2;
                }
                sdp_emit_new(f, &d, i, L, off, rcost[src], (uint8_t)src, cen);
            }
        }
        if (cen) {
            cen->sum_states += (uint64_t)nsi;
            if ((uint32_t)nsi > cen->max_states)
                cen->max_states = (uint32_t)nsi;
            uint32_t a = rkey[0] & ~SDP_LIT_FLAG;
            int nd = 1;
            for (int s = 1; s < nsi; s++) {
                if ((rkey[s] & ~SDP_LIT_FLAG) != a) {
                    nd = 2;
                    break;
                }
            }
            cen->sum_distinct_last += (uint64_t)nd;
        }
    }
    if (cen) {
        cen->n_pos = n;
        cen->ram = lz0_ram_sdp(n, K);
        if (d.ns[n] > 0) {
            cen->sum_states += (uint64_t)d.ns[n];
            if ((uint32_t)d.ns[n] > cen->max_states)
                cen->max_states = (uint32_t)d.ns[n];
        }
    }
    int rc = sdp_rows_backtrack(&d, n, tl);
    sdp_rows_free(&d);
    return rc;
}
