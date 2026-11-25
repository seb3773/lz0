#define _POSIX_C_SOURCE 200809L
#include "lz0_internal.h"
#include "lz0_simd.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Skip-list depth for levels 7..9 (4 bytes per level per input byte, built
 * on demand by the finder only when long hash-chain walks happen). */
#ifndef LZ0_SKIP_LEVELS_HIGH
#define LZ0_SKIP_LEVELS_HIGH 12
#endif

static void lz0_config_init_defaults(lz0_config *cfg, lz0_parse_t parser) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->min_match = 2;
    cfg->max_chain = 8;
    cfg->max_candidates = 16;
    cfg->parser = parser;
    cfg->score_k = 8;
    cfg->max_states = 2;
    cfg->jump_levels = 0;
    cfg->bt_depth = 0;
    cfg->isolated = 0;
    cfg->level = 0;
    cfg->offset_lsb_bits = 8;
    cfg->len_code = 3;
    cfg->sa_win = 16;
    cfg->sa_sample = 1;
    cfg->sa_hybrid = 1;
    cfg->sa_adapt = 1;
    cfg->sa_csort_v2 = 1;
    cfg->sa_csort_smode = 1;
    cfg->sa_pf_look = 16;
    cfg->sa_count_mode = 1;
    cfg->sa_algo = 1;
    cfg->len2_max_off = 32;
    cfg->ultra_budget = (size_t)768 << 20;
    cfg->ultra_margin = 32;
    cfg->ultra_fine = 8;
}

void lz0_config_default(lz0_config *cfg, lz0_parse_t parser) {
    lz0_config_init_defaults(cfg, parser);
}

void lz0_config_level(lz0_config *cfg, int level) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;

    lz0_config_init_defaults(cfg, LZ0_PARSE_SDP);
    cfg->isolated = 1;
    cfg->level = level;

    if (level == 1) {
        // Fast mode (hash-only Greedy, no SA-IS, minimal memory)
        cfg->min_match = 2;
        cfg->max_chain = 2;
        cfg->max_candidates = 4;
        cfg->parser = LZ0_PARSE_GREEDY;
        cfg->score_k = 8;
        cfg->max_states = 2;
        cfg->jump_levels = 0;
        cfg->bt_depth = 0;
        cfg->sa_win = 0;
        cfg->sa_adapt = 0;
        cfg->sdp_fine_matches = 0;
        cfg->deep_probe = 0;
        cfg->hash4_probe = 0;
    } else if (level == 2) {
        // Fast SDP (hash-only, zero SA-IS allocation)
        cfg->min_match = 2;
        cfg->max_chain = 4;
        cfg->max_candidates = 8;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 2;
        cfg->jump_levels = 0;
        cfg->bt_depth = 0;
        cfg->sa_win = 0;
        cfg->sa_adapt = 0;
        cfg->sdp_fine_matches = 0;
        cfg->deep_probe = 0;
        cfg->hash4_probe = 0;
    } else if (level == 3) {
        // Balanced fast SA + SDP K=2
        cfg->min_match = 2;
        cfg->max_chain = 4;
        cfg->max_candidates = 8;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 2;
        cfg->jump_levels = 0;
        cfg->bt_depth = 0;
        cfg->sa_win = 4;
        cfg->sa_adapt = 1;
        cfg->sdp_fine_matches = 0;
        cfg->deep_probe = 0;
        cfg->hash4_probe = 0;
    } else if (level <= 6) {
        // Standard H2C8 (Default)
        cfg->min_match = 2;
        cfg->max_chain = 8;
        cfg->max_candidates = 16;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 2;
        cfg->jump_levels = 0;
        cfg->bt_depth = 0;
        cfg->sa_win = 16;
        cfg->sa_adapt = 1;
        cfg->sdp_fine_matches = 2;
        cfg->deep_probe = 0;
        cfg->hash4_probe = 0;
    } else if (level <= 8) {
        // High compression
        cfg->min_match = 2;
        cfg->max_chain = 32;
        cfg->max_candidates = 32;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 8;
        /* hash-chain skip list: exact, turns long chain walks into jumps */
        cfg->jump_levels = LZ0_SKIP_LEVELS_HIGH;
        cfg->bt_depth = 0;
        cfg->sa_win = 64;
        cfg->sa_adapt = 1;
        cfg->sdp_fine_matches = 1;
        cfg->deep_probe = 2;
        cfg->hash4_probe = 1;
    } else {
        // Level 9: Ultra maximal compression (dedicated ultra parser)
        cfg->min_match = 2;
        cfg->max_chain = 64;
        cfg->max_candidates = 48;
        cfg->parser = LZ0_PARSE_ULTRA;
        cfg->score_k = 8;
        cfg->max_states = 32;
        cfg->jump_levels = LZ0_SKIP_LEVELS_HIGH;
        cfg->bt_depth = 32;
        cfg->sa_win = 128;
        cfg->sa_multiscale = 1;
        cfg->sa_adapt = 0;
        cfg->sdp_fine_matches = 1;
        cfg->deep_probe = 4;
        cfg->hash4_probe = 1;
        cfg->len2_max_off = 0;
        cfg->rle_tail = 64;
        cfg->ultra_states = 0;
        cfg->ultra_margin = 32;
        cfg->ultra_fine = 8;
        cfg->ultra_inject = 1;
    }
}

void lz0_h2c8_apply(void) {
    lz0_chain_prune = 0;
    lz0_chain_noskip = 0;
    lz0_skip_thresh = 0;
    lz0_skip_stride = 0;
    lz0_hash4_probe = 0;
    lz0_deep_probe = 0;
    lz0_sa_win = 16;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 1;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 1;
    lz0_sa_multiscale = 0;
    lz0_sa_csort_v2 = 1;
    lz0_sa_csort_smode = 1;
    lz0_sa_pf_look = 16;
    lz0_sa_count_mode = 1;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
    lz0_sa_algo = 1;
    lz0_sdp_census_out = NULL;
    lz0_len2_max_off = 32;
    lz0_rle_tail = 0;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

static int emit_single_literal(const unsigned char *in, uint32_t n, const lz0_config *cfg,
                               unsigned char **out, size_t *out_sz, lz0_bitacct *acct) {
    lz0_toklist single_tl;
    lz0_toklist_init(&single_tl);
    if (lz0_toklist_push(&single_tl, 0, n, 0) < 0) {
        lz0_toklist_free(&single_tl);
        return -1;
    }
    int rc = lz0_emit_cfg(in, n, &single_tl, out, out_sz, acct, cfg);
    lz0_toklist_free(&single_tl);
    return rc;
}

static void fill_compress_stats(lz0_stats *stats, const lz0_bitacct *acct, double t0,
                                double t1, double t2, double t3) {
    stats->finder_ms = t1 - t0;
    stats->parse_ms = t2 - t1;
    stats->emit_ms = t3 - t2;
    stats->total_ms = t3 - t0;
    stats->bits_decision = acct->bits_decision;
    stats->bits_elias_len = acct->bits_elias_len;
    stats->bits_offset = acct->bits_offset;
    stats->bits_literal = acct->bits_literal;
    stats->bits_total = acct->bits_decision + acct->bits_elias_len +
                        acct->bits_offset + acct->bits_literal;
}

typedef struct {
    unsigned char *data;
    size_t index;
    size_t cap;
    size_t bit_index;
    unsigned int bit_mask;
    int backtrack;
    uint32_t n_lit;
    uint32_t n_last;
    uint32_t n_new;
    lz0_bitacct acct;
} lz0_dwr_t;

static inline int l1_clz32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(v);
#else
    int n = 0;
    while ((v & 0x80000000u) == 0u) {
        v <<= 1;
        n++;
    }
    return n;
#endif
}

static int l1_ensure(lz0_dwr_t *w, size_t need) {
    if (w->index + need <= w->cap)
        return 0;
    size_t ncap = w->cap * 2u + need + 64u;
    unsigned char *p = (unsigned char *)realloc(w->data, ncap);
    if (!p)
        return -1;
    w->data = p;
    w->cap = ncap;
    return 0;
}

static inline void l1_byte(lz0_dwr_t *w, unsigned char v) {
    w->data[w->index++] = v;
}

static inline void l1_bit(lz0_dwr_t *w, int v, int cat) {
    if (cat == 0)
        w->acct.bits_decision++;
    else if (cat == 1)
        w->acct.bits_elias_len++;
    else if (cat == 2)
        w->acct.bits_offset++;
    if (w->backtrack) {
        if (v && w->index > 0)
            w->data[w->index - 1u] |= 1u;
        w->backtrack = 0;
        return;
    }
    if (!w->bit_mask) {
        w->bit_mask = 128u;
        w->bit_index = w->index;
        l1_byte(w, 0);
    }
    if (v)
        w->data[w->bit_index] |= (unsigned char)w->bit_mask;
    w->bit_mask >>= 1;
}

static inline void l1_elias(lz0_dwr_t *w, uint32_t value, int invert, int cat) {
    int hi = 31 - l1_clz32(value);
    for (int b = hi - 1; b >= 0; b--) {
        int bit = (int)((value >> b) & 1u);
        l1_bit(w, 0, cat);
        l1_bit(w, invert ? !bit : bit, cat);
    }
    l1_bit(w, 1, cat);
}

static inline void l1_emit_lit(lz0_dwr_t *w, const unsigned char *p, uint32_t L) {
    l1_bit(w, 0, 0);
    l1_elias(w, L, 0, 1);
    memcpy(w->data + w->index, p, L);
    w->index += L;
    w->acct.bits_literal += (uint64_t)L << 3;
    w->n_lit++;
}

static inline void l1_emit_last(lz0_dwr_t *w, uint32_t L) {
    l1_bit(w, 0, 0);
    l1_elias(w, L, 0, 1);
    w->n_last++;
}

static inline void l1_emit_new(lz0_dwr_t *w, uint32_t off, uint32_t L) {
    l1_bit(w, 1, 0);
    l1_bit(w, 1, 2);
    uint32_t msb = ((off - 1u) >> 8) + 1u;
    uint32_t rem = (off - 1u) & 255u;
    l1_elias(w, msb, 1, 2);
    l1_byte(w, (unsigned char)(255u - rem));
    w->acct.bits_offset += 8;
    w->backtrack = 0;
    uint32_t v = L - 1u;
    if (v <= 7u) {
        uint32_t x = v - 1u;
        l1_bit(w, (int)((x >> 2) & 1u), 1);
        l1_bit(w, (int)((x >> 1) & 1u), 1);
        l1_bit(w, (int)(x & 1u), 1);
    } else {
        l1_bit(w, 1, 1);
        l1_bit(w, 1, 1);
        l1_bit(w, 1, 1);
        l1_elias(w, v - 7u, 0, 1);
    }
    w->n_new++;
}

static inline int l1_ebits(uint32_t v) {
    return ((32 - l1_clz32(v)) << 1) - 1;
}

static inline int l1_cost_last(uint32_t L) {
    return 1 + l1_ebits(L);
}

static inline int l1_cost_new(uint32_t off, uint32_t L) {
    uint32_t msb = ((off - 1u) >> 8) + 1u;
    return 10 + l1_ebits(msb) + (L <= 8u ? 3 : (3 + l1_ebits(L - 7u)));
}

static inline uint32_t l1_h3(const unsigned char *p, int shift) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> shift;
}

static int l1_win_high_entropy(const unsigned char *in, uint32_t len) {
    uint32_t counts[256] = {0};
    uint32_t step = len > 16384u ? (len >> 14) : 1u;
    uint32_t sample = 0;
    for (uint32_t i = 0; i < len; i += step) {
        counts[in[i]]++;
        sample++;
    }
    int distinct = 0;
    uint32_t max_c = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i]) {
            distinct++;
            if (counts[i] > max_c)
                max_c = counts[i];
        }
    }
    return (distinct >= 250 && (uint64_t)max_c * 1000u < (uint64_t)sample * 18u);
}

static inline int l1_better(int bk, int bl, int bc, int L, int c) {
    if (bk < 0)
        return 1;
    long long lhs = (long long)c * bl;
    long long rhs = (long long)bc * L;
    return lhs < rhs || (lhs == rhs && L > bl);
}

static void l1_find_best(const unsigned char *in, uint32_t n, uint32_t i,
                         const uint32_t *h2, int shift, const uint32_t mru[4], int nmru,
                         uint32_t last_off, int allow_last, uint32_t len2_lim,
                         int *kind_out, uint32_t *len_out, uint32_t *off_out) {
    int max_left = (int)(n - i);
    int bk = -1, bl = 0, bc = 0x3fffffff, bs = -1;
    uint32_t bo = 0;
    uint32_t w0 = lz0_ld16(in + i);

    if (allow_last && last_off >= 1u && last_off <= i && in[i] == in[i - last_off]) {
        int L = lz0_simd_matchlen(in + i, in + i - last_off, max_left);
        int c = l1_cost_last((uint32_t)L);
        int sav = (L << 3) - c;
        if (sav > 0) {
            bk = 1;
            bl = L;
            bo = last_off;
            bc = c;
            bs = sav;
        }
    }
    if (i + 2u < n) {
        uint32_t hv = l1_h3(in + i, shift);
        for (int slot = 0; slot < 2; slot++) {
            uint32_t p = h2[(size_t)hv * 2u + (uint32_t)slot];
            if (p >= i)
                continue;
            uint32_t off = i - p;
            if (off == last_off)
                continue;
            if (lz0_ld16(in + p) == w0) {
                int L = 2 + lz0_simd_matchlen(in + i + 2, in + p + 2, max_left - 2);
                if (L == 2 && off > len2_lim)
                    continue;
                int c = l1_cost_new(off, (uint32_t)L);
                int sav = (L << 3) - c;
                if (sav > 0 && l1_better(bk, bl, bc, L, c)) {
                    bk = 2;
                    bl = L;
                    bo = off;
                    bc = c;
                    bs = sav;
                }
            }
        }
    }
    for (int k = 0; k < nmru; k++) {
        uint32_t off = mru[k];
        if (off == 0u || off > i || off == last_off)
            continue;
        uint32_t p = i - off;
        if (lz0_ld16(in + p) == w0) {
            int L = 2 + lz0_simd_matchlen(in + i + 2, in + p + 2, max_left - 2);
            if (L < 3)
                continue;
            int c = l1_cost_new(off, (uint32_t)L);
            int sav = (L << 3) - c;
            if (sav > 0 && l1_better(bk, bl, bc, L, c)) {
                bk = 2;
                bl = L;
                bo = off;
                bc = c;
                bs = sav;
            }
        }
    }
    if (bs <= 0)
        bk = -1;
    *kind_out = bk;
    *len_out = (uint32_t)bl;
    *off_out = bo;
}

static inline uint32_t l1_sync_win(const unsigned char *in, uint32_t n, uint32_t i,
                                   uint32_t *cur_win, uint32_t cur_lim) {
    uint32_t widx = i >> LZ0_WIN_SHIFT;
    if (widx != *cur_win) {
        *cur_win = widx;
        uint32_t off = widx << LZ0_WIN_SHIFT;
        uint32_t wlen = n - off;
        if (wlen > LZ0_WIN_SIZE)
            wlen = LZ0_WIN_SIZE;
        return (wlen >= 1024u && l1_win_high_entropy(in + off, wlen)) ? 32u : 256u;
    }
    return cur_lim;
}

static inline void l1_mru_push(uint32_t mru[4], int *nmru, uint32_t off) {
    if (mru[0] != off) {
        for (int k = 3; k > 0; k--)
            mru[k] = mru[k - 1];
        mru[0] = off;
        if (*nmru < 4)
            (*nmru)++;
    }
}

/* Streaming low-RAM hash-greedy encoder for -1: replaces prev[n] + toklist
 * with a bounded 2-slot hash bucket array and direct bit emission (~1.0..3.95 B/B
 * peak heap instead of ~8..149 B/B). */
static int compress_fast_greedy(const unsigned char *in, uint32_t n,
                                unsigned char **out, size_t *out_sz,
                                const lz0_config *cfg, lz0_stats *stats, double t0) {
    lz0_simd_init();
    int hb = 10;
    while (hb < 17 && ((size_t)1u << (hb + 1)) * 8u <= (size_t)n * 3u)
        hb++;
    uint32_t H = 1u << hb;
    int shift = 32 - hb;
    size_t hbytes = (size_t)H * 2u * sizeof(uint32_t);
    uint32_t *h2 = (uint32_t *)malloc(hbytes);
    if (!h2)
        return -1;
    memset(h2, 0xff, hbytes);

    lz0_dwr_t w;
    memset(&w, 0, sizeof(w));
    w.cap = (size_t)n + 128u;
    w.data = (unsigned char *)malloc(w.cap);
    if (!w.data) {
        free(h2);
        return -1;
    }
    w.backtrack = 1;
    stats->peak_ram = hbytes + w.cap;

    double t1 = now_ms();
    uint32_t i = 0, last_off = LZ0_INITIAL_OFFSET, ins = 0;
    uint32_t cur_win = UINT32_MAX, len2_lim = 256u;
    uint32_t mru[4] = {0, 0, 0, 0};
    int nmru = 0;

    while (i < n) {
        uint32_t lit_start = i++;
        uint32_t c_len = 0, c_off = 0;
        int c_kind = -1;
        while (i < n) {
            len2_lim = l1_sync_win(in, n, i, &cur_win, len2_lim);
            for (; ins < i && ins + 2u < n; ins++) {
                uint32_t hv = l1_h3(in + ins, shift);
                h2[(size_t)hv * 2u + 1u] = h2[(size_t)hv * 2u];
                h2[(size_t)hv * 2u] = ins;
            }
            if (n - i < 2u) {
                i++;
                continue;
            }
            l1_find_best(in, n, i, h2, shift, mru, nmru, last_off, 1, len2_lim,
                         &c_kind, &c_len, &c_off);
            if (c_kind >= 0)
                break;
            i++;
        }
        if (l1_ensure(&w, (size_t)(i - lit_start) + 64u) < 0) {
            free(h2);
            free(w.data);
            return -1;
        }
        l1_emit_lit(&w, in + lit_start, i - lit_start);
        if (i >= n || c_kind < 0)
            continue;
        if (l1_ensure(&w, 64u) < 0) {
            free(h2);
            free(w.data);
            return -1;
        }
        if (c_kind == 1) {
            l1_emit_last(&w, c_len);
        } else {
            l1_emit_new(&w, c_off, c_len);
            last_off = c_off;
            l1_mru_push(mru, &nmru, c_off);
        }
        i += c_len;
        while (i < n && c_kind == 2) {
            len2_lim = l1_sync_win(in, n, i, &cur_win, len2_lim);
            for (; ins < i && ins + 2u < n; ins++) {
                uint32_t hv = l1_h3(in + ins, shift);
                h2[(size_t)hv * 2u + 1u] = h2[(size_t)hv * 2u];
                h2[(size_t)hv * 2u] = ins;
            }
            if (n - i < 2u)
                break;
            int nk = -1;
            uint32_t nl = 0, no = 0;
            l1_find_best(in, n, i, h2, shift, mru, nmru, last_off, 0, len2_lim,
                         &nk, &nl, &no);
            if (nk != 2)
                break;
            if (l1_ensure(&w, 64u) < 0) {
                free(h2);
                free(w.data);
                return -1;
            }
            l1_emit_new(&w, no, nl);
            last_off = no;
            l1_mru_push(mru, &nmru, no);
            i += nl;
        }
    }
    if (l1_ensure(&w, 16u) < 0) {
        free(h2);
        free(w.data);
        return -1;
    }
    l1_bit(&w, 1, 0);
    l1_bit(&w, 0, 2);
    free(h2);

    *out = w.data;
    *out_sz = w.index;
    stats->n_literals = w.n_lit;
    stats->n_last = w.n_last;
    stats->n_new = w.n_new;

    /* Zero-inflation safety net (same contract as the general path). */
    int kb = 0;
    size_t tmp = n;
    while (tmp >>= 1)
        kb++;
    size_t single_sz = (size_t)n + (size_t)(2 * kb + 10) / 8u;
    if (*out_sz > single_sz) {
        unsigned char *alt_out = NULL;
        size_t alt_sz = 0;
        lz0_bitacct alt_acct;
        if (emit_single_literal(in, n, cfg, &alt_out, &alt_sz, &alt_acct) == 0 &&
            alt_sz < *out_sz) {
            free(*out);
            *out = alt_out;
            *out_sz = alt_sz;
            w.acct = alt_acct;
            stats->n_literals = 1;
            stats->n_last = 0;
            stats->n_new = 0;
        } else {
            free(alt_out);
        }
    }

    double t3 = now_ms();
    fill_compress_stats(stats, &w.acct, t0, t1, t3, t3);
    return 0;
}

int lz0_compress(const unsigned char *in, size_t in_sz,
                 unsigned char **out, size_t *out_sz,
                 const lz0_config *cfg, lz0_stats *stats) {
    if (!in || in_sz == 0 || in_sz > 0xffffffffu || !out || !out_sz)
        return -1;
    lz0_config local;
    if (!cfg) {
        lz0_config_level(&local, 6);
        cfg = &local;
    } else {
        local = *cfg;
        cfg = &local;
    }
    if (local.max_states >= 16 && in_sz <= 65536) {
        local.max_states = 32;
    }
    lz0_stats dummy;
    if (!stats)
        stats = &dummy;
    memset(stats, 0, sizeof(*stats));

    double t0 = now_ms();
    if (in_sz >= 4096u && lz0_is_early_incompressible(in, (uint32_t)in_sz)) {
        double t1 = now_ms();
        lz0_bitacct acct;
        int rc = emit_single_literal(in, (uint32_t)in_sz, cfg, out, out_sz, &acct);
        double t3 = now_ms();
        if (rc == 0) {
            stats->n_literals = 1;
            fill_compress_stats(stats, &acct, t0, t1, t1, t3);
        }
        return rc;
    }

    if (cfg->parser == LZ0_PARSE_GREEDY && cfg->sa_win <= 0 &&
        cfg->bt_depth <= 0 && cfg->jump_levels <= 0 &&
        cfg->max_chain <= 2 && cfg->offset_lsb_bits == 8 &&
        cfg->len_code == 3 && !cfg->hash4_probe &&
        cfg->chain_prune == 0 &&
        (cfg->isolated || (!lz0_sdp_census_out && !lz0_stream_census_out))) {
        return compress_fast_greedy(in, (uint32_t)in_sz, out, out_sz, cfg, stats, t0);
    }

    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)in_sz, cfg) < 0)
        return -1;
    /* finder RAM without the skip list: it is only built on demand (see
     * lz0_find.c) and added below if it was */
    stats->peak_ram = lz0_ram_finder_c(&f.opts, (uint32_t)in_sz, 0);
    if (f.opts.parser == LZ0_PARSE_SDP) {
        int k = f.opts.max_states > 0 ? f.opts.max_states : 4;
        stats->peak_ram += lz0_ram_sdp((uint32_t)in_sz, k);
    }
    if (f.opts.parser == LZ0_PARSE_ULTRA)
        stats->peak_ram += lz0_ram_ultra_c(&f.opts, (uint32_t)in_sz);

    lz0_toklist tl;
    lz0_toklist_init(&tl);

    double t1 = now_ms();
    int rc;
    if (cfg->parser == LZ0_PARSE_GREEDY)
        rc = lz0_parse_greedy(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_SDP)
        rc = lz0_parse_sdp(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_ULTRA)
        rc = lz0_parse_ultra(&f, &tl);
    else
        rc = -1;
    double t2 = now_ms();
    if (f.jmp)
        stats->peak_ram += (size_t)in_sz * (size_t)f.jump_levels * sizeof(uint32_t);
    if (rc < 0 || !lz0_tokens_valid(&tl, (uint32_t)in_sz)) {
        lz0_toklist_free(&tl);
        lz0_finder_free(&f);
        return -1;
    }

    lz0_bitacct acct;
    rc = lz0_emit_cfg(in, (uint32_t)in_sz, &tl, out, out_sz, &acct, &f.opts);
    double t3 = now_ms();

    /* Piste D: Zero-inflation safety net.
     * If the parsed stream inflated beyond the cost of a single literal run,
     * replace it with an exact single LiteralRun token covering the entire input.
     * A single literal run is guaranteed to cost only in_sz + ceil((2*k + 3)/8) bytes
     * (at most in_sz + 5..8 bytes), eliminating any parser-induced inflation. */
    if (rc == 0 && *out && in_sz > 0) {
        int k = 0;
        size_t tmp = in_sz;
        while (tmp >>= 1)
            k++;
        size_t single_sz = in_sz + (size_t)(2 * k + 10) / 8;
        if (*out_sz > single_sz) {
            unsigned char *alt_out = NULL;
            size_t alt_sz = 0;
            lz0_bitacct alt_acct;
            if (emit_single_literal(in, (uint32_t)in_sz, &f.opts, &alt_out, &alt_sz, &alt_acct) == 0 &&
                alt_sz < *out_sz) {
                free(*out);
                *out = alt_out;
                *out_sz = alt_sz;
                acct = alt_acct;
                tl.n = 1;
                tl.t[0].pos = 0;
                tl.t[0].length = (uint32_t)in_sz;
                tl.t[0].offset = 0;
            } else {
                free(alt_out);
            }
        }
    }

    uint32_t lo = LZ0_INITIAL_OFFSET;
    for (uint32_t i = 0; i < tl.n; i++) {
        if (!tl.t[i].offset)
            stats->n_literals++;
        else {
            int is_last = (tl.t[i].offset == lo);
            lo = tl.t[i].offset;
            if (is_last)
                stats->n_last++;
            else
                stats->n_new++;
        }
    }

    fill_compress_stats(stats, &acct, t0, t1, t2, t3);

    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
    return rc;
}
