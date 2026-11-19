#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include "zx0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define MAX_OFF_ZX0 32640
#define MAX_CAND 16

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static uint32_t rnd_state = 0x12345678u;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static unsigned char *make_corpus(const char *kind, size_t n) {
    unsigned char *p = (unsigned char *)malloc(n);
    if (!p)
        return NULL;
    rnd_state = 0xC0FFEEu + (uint32_t)n + (uint32_t)kind[0] * 97u;
    if (!strcmp(kind, "text")) {
        const char *w[] = {
            "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ",
            "and ", "then ", "compress ", "this ", "pattern ", "again ", "with ",
            "some ", "variation ", "ZX0 ", "window ", "offset ", "literal ", "match "
        };
        size_t i = 0;
        while (i < n) {
            const char *s = w[rnd() % 22];
            size_t L = strlen(s);
            if (i + L > n)
                L = n - i;
            memcpy(p + i, s, L);
            i += L;
        }
    } else if (!strcmp(kind, "rep")) {
        for (size_t i = 0; i < n; i++)
            p[i] = (unsigned char)((i / 17) & 0xff);
        for (size_t i = n / 4; i + n / 8 < n && i < n; i += n / 7)
            memcpy(p + i, p, n / 8 < n - i ? n / 8 : n - i);
    } else {
        for (size_t i = 0; i < n; i++) {
            if ((i & 15) == 0)
                p[i] = 0xC3;
            else if ((i & 7) == 1)
                p[i] = (unsigned char)(i >> 8);
            else
                p[i] = (unsigned char)(rnd() & (i & 1 ? 0x0f : 0xff));
        }
        size_t blk = n > 256 ? 256 : n / 2;
        for (size_t i = blk; i + blk < n; i += blk + (rnd() % 64))
            memcpy(p + i, p + (rnd() % (i ? i : 1)), blk);
    }
    return p;
}

typedef struct {
    uint32_t pos;
    uint32_t length;
    uint32_t offset;
    uint32_t last;
} gtok;

static int walk_zx0(BLOCK *optimal, gtok *out, int cap) {
    BLOCK *prev = NULL;
    BLOCK *next;
    while (optimal) {
        next = optimal->chain;
        optimal->chain = prev;
        prev = optimal;
        optimal = next;
    }
    int n = 0;
    uint32_t last = LZ0_INITIAL_OFFSET;
    for (optimal = prev->chain; optimal; prev = optimal, optimal = optimal->chain) {
        int length = optimal->index - prev->index;
        int pos = prev->index + 1;
        if (length <= 0 || n >= cap)
            return -1;
        out[n].pos = (uint32_t)pos;
        out[n].length = (uint32_t)length;
        out[n].offset = (uint32_t)optimal->offset;
        out[n].last = last;
        if (optimal->offset)
            last = (uint32_t)optimal->offset;
        n++;
    }
    return n;
}

static void lz0_cfg(lz0_config *cfg, int chain, int noskip, int thresh, int stride, int h4, int deep) {
    lz0_config_default(cfg, LZ0_PARSE_SDP);
    cfg->min_match = 2;
    cfg->max_chain = chain;
    cfg->max_candidates = 16;
    cfg->max_states = 2;
    cfg->jump_levels = 16;
    lz0_chain_prune = 0;
    lz0_chain_noskip = noskip;
    lz0_skip_thresh = thresh;
    lz0_skip_stride = stride;
    lz0_hash4_probe = h4;
    lz0_deep_probe = deep;
    lz0_sa_win = 0;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 0;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 0;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
}

static uint32_t gap_hash3(const unsigned char *p) {
    return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) * 2654435761u >> (32 - LZ0_HASH_BITS);
}

static int hash3_chain_depth(lz0_finder *f, uint32_t pos, uint32_t zoff) {
    if (zoff == 0 || pos < zoff || pos + 2 >= f->n || !f->head || !f->prev)
        return 0;
    uint32_t target = pos - zoff;
    uint32_t h = gap_hash3(f->in + pos);
    uint32_t cur = f->head[h];
    int depth = 0;
    int lim = 1 << 20;
    while (cur != LZ0_CHAIN_NIL && depth < lim) {
        depth++;
        if (cur == target)
            return depth;
        cur = f->prev[cur];
    }
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t median_u32(uint32_t *v, int n) {
    if (n <= 0)
        return 0;
    qsort(v, (size_t)n, sizeof(uint32_t), cmp_u32);
    if (n & 1)
        return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2;
}

static uint32_t pct_u32(uint32_t *v, int n, int p) {
    if (n <= 0)
        return 0;
    qsort(v, (size_t)n, sizeof(uint32_t), cmp_u32);
    if (n == 1)
        return v[0];
    int i = (int)(((long long)p * (n - 1) + 50) / 100);
    if (i < 0)
        i = 0;
    if (i >= n)
        i = n - 1;
    return v[i];
}

static int depth_bucket(int d) {
    if (d <= 0)
        return 12;
    if (d <= 8)
        return 0;
    if (d <= 16)
        return 1;
    if (d <= 24)
        return 2;
    if (d <= 32)
        return 3;
    if (d <= 48)
        return 4;
    if (d <= 64)
        return 5;
    if (d <= 96)
        return 6;
    if (d <= 128)
        return 7;
    if (d <= 192)
        return 8;
    if (d <= 256)
        return 9;
    if (d <= 512)
        return 10;
    return 11;
}

static const char *const DEPTH_LAB[13] = {
    "1-8", "9-16", "17-24", "25-32", "33-48", "49-64",
    "65-96", "97-128", "129-192", "193-256", "257-512", ">512", "nf"
};

#define ORACLE_LIM 512

static void dump_depth_stats(const char *title, uint32_t *d, int n) {
    uint64_t sum = 0, mx = 0;
    uint64_t buck[13];
    memset(buck, 0, sizeof(buck));
    for (int i = 0; i < n; i++) {
        sum += d[i];
        if (d[i] > mx)
            mx = d[i];
        buck[depth_bucket((int)d[i])]++;
    }
    printf("  %s n=%d avg=%.1f p25=%u med=%u p75=%u max=%llu\n",
           title, n, n ? (double)sum / (double)n : 0,
           pct_u32(d, n, 25), median_u32(d, n), pct_u32(d, n, 75),
           (unsigned long long)mx);
    printf("    buckets");
    for (int b = 0; b < 12; b++)
        printf(" %s=%llu", DEPTH_LAB[b], (unsigned long long)buck[b]);
    printf("\n");
}

static void run_deep_oracle(const char *kind, size_t n, const unsigned char *in,
                            const gtok *zx, int nz) {
    lz0_config cfg;
    lz0_cfg(&cfg, 32, 0, 0, 0, 0, 0);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  oracle finder FAIL\n");
        return;
    }
    static const int TH[5] = {16, 20, 24, 28, 32};
    uint32_t *first_d[5];
    uint32_t *best_d[5];
    int nf[5], nb[5];
    for (int t = 0; t < 5; t++) {
        first_d[t] = (uint32_t *)malloc(n * sizeof(uint32_t));
        best_d[t] = (uint32_t *)malloc(n * sizeof(uint32_t));
        nf[t] = 0;
        nb[t] = 0;
        if (!first_d[t] || !best_d[t]) {
            printf("  oracle alloc FAIL\n");
            lz0_finder_free(&f);
            return;
        }
    }
    uint32_t *zx_exact = NULL;
    uint32_t *zx_first = NULL;
    uint32_t *zx_best = NULL;
    uint32_t *zx_exact16 = NULL;
    uint32_t *zx_first16 = NULL;
    uint32_t *zx_best16 = NULL;
    int n_zx_exact = 0, n_zx_first = 0, n_zx_best = 0;
    int n_zx_exact16 = 0, n_zx_first16 = 0, n_zx_best16 = 0;
    int n_zx_a1631 = 0;
    if (nz > 0) {
        zx_exact = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        zx_first = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        zx_best = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        zx_exact16 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        zx_first16 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        zx_best16 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    }
    uint64_t n_pos_short = 0, n_pos_all = 0;
    uint64_t n_vis = 0, n_ml = 0;
    double t0 = now_ms();
    lz0_match cands[MAX_CAND];
    for (uint32_t pos = 0; pos + 2 < (uint32_t)n; pos++) {
        lz0_finder_insert_upto(&f, pos);
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, LZ0_INITIAL_OFFSET, cands, &last_len);
        int best_len = 0;
        for (int c = 0; c < nc; c++) {
            if ((int)cands[c].length > best_len)
                best_len = (int)cands[c].length;
        }
        n_pos_all++;
        if (best_len >= 16)
            continue;
        n_pos_short++;
        uint32_t cur = f.head[gap_hash3(in + pos)];
        int depth = 0;
        int first_at[5];
        for (int t = 0; t < 5; t++)
            first_at[t] = 0;
        int walk_best = best_len;
        int walk_best_d = 0;
        int maxl = (int)((uint32_t)n - pos);
        while (cur != LZ0_CHAIN_NIL && depth < ORACLE_LIM) {
            depth++;
            n_vis++;
            if (cur >= pos) {
                cur = f.prev[cur];
                continue;
            }
            if (in[cur] != in[pos]) {
                cur = f.prev[cur];
                continue;
            }
            int ml = lz0_matchlen(in + pos, in + cur, maxl);
            n_ml++;
            if (ml > walk_best) {
                walk_best = ml;
                walk_best_d = depth;
            }
            for (int t = 0; t < 5; t++) {
                if (!first_at[t] && ml >= TH[t])
                    first_at[t] = depth;
            }
            cur = f.prev[cur];
        }
        for (int t = 0; t < 5; t++) {
            if (first_at[t])
                first_d[t][nf[t]++] = (uint32_t)first_at[t];
            if (walk_best >= TH[t] && walk_best_d > 0)
                best_d[t][nb[t]++] = (uint32_t)walk_best_d;
        }
    }
    double oracle_ms = now_ms() - t0;
    printf("=== n=%zu %s DEEP ORACLE lim=%d ===\n", n, kind, ORACLE_LIM);
    printf("  pos all=%llu short(best<16)=%llu (%.1f%%) vis=%llu ml=%llu oracle=%.1f ms\n",
           (unsigned long long)n_pos_all, (unsigned long long)n_pos_short,
           n_pos_all ? 100.0 * (double)n_pos_short / (double)n_pos_all : 0,
           (unsigned long long)n_vis, (unsigned long long)n_ml, oracle_ms);
    for (int t = 0; t < 5; t++) {
        printf("  recover >=%d  positions=%d / %llu (%.1f%% of short)\n",
               TH[t], nf[t], (unsigned long long)n_pos_short,
               n_pos_short ? 100.0 * (double)nf[t] / (double)n_pos_short : 0);
        char title[64];
        snprintf(title, sizeof(title), "first_ge_%d", TH[t]);
        dump_depth_stats(title, first_d[t], nf[t]);
        snprintf(title, sizeof(title), "best_ge_%d", TH[t]);
        dump_depth_stats(title, best_d[t], nb[t]);
    }

    if (zx && nz > 0 && zx_exact && zx_first && zx_best) {
        lz0_finder_free(&f);
        if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
            printf("  oracle zx finder FAIL\n");
            goto oracle_done;
        }
        uint64_t zx_short = 0, zx_a = 0, zx_hit = 0, zx_a_long = 0;
        t0 = now_ms();
        for (int i = 0; i < nz; i++) {
            if (!zx[i].offset || zx[i].offset == zx[i].last)
                continue;
            uint32_t pos = zx[i].pos;
            uint32_t zoff = zx[i].offset;
            uint32_t zlen = zx[i].length;
            if (pos + 2 >= (uint32_t)n || pos < zoff)
                continue;
            lz0_finder_insert_upto(&f, pos);
            int last_len = 0;
            int nc = lz0_find_matches(&f, pos, zx[i].last, cands, &last_len);
            int best_len = 0;
            int full_hit = 0;
            for (int c = 0; c < nc; c++) {
                if ((int)cands[c].length > best_len)
                    best_len = (int)cands[c].length;
                if (cands[c].offset == zoff && cands[c].length >= zlen)
                    full_hit = 1;
            }
            if (full_hit) {
                zx_hit++;
                continue;
            }
            zx_a++;
            if (best_len >= 16)
                zx_a_long++;
            else
                zx_short++;
            uint32_t target = pos - zoff;
            uint32_t cur = f.head[gap_hash3(in + pos)];
            int depth = 0;
            int exact = 0, first_ge = 0, wbest = 0, wbest_d = 0;
            int maxl = (int)((uint32_t)n - pos);
            while (cur != LZ0_CHAIN_NIL && depth < ORACLE_LIM) {
                depth++;
                if (cur >= pos) {
                    cur = f.prev[cur];
                    continue;
                }
                if (in[cur] != in[pos]) {
                    cur = f.prev[cur];
                    continue;
                }
                int ml = lz0_matchlen(in + pos, in + cur, maxl);
                if (cur == target && !exact)
                    exact = depth;
                if (!first_ge && ml >= (int)zlen)
                    first_ge = depth;
                if (ml > wbest) {
                    wbest = ml;
                    wbest_d = depth;
                }
                cur = f.prev[cur];
            }
            int focus = (zlen >= 16 && zlen <= 31);
            if (focus)
                n_zx_a1631++;
            if (exact) {
                zx_exact[n_zx_exact++] = (uint32_t)exact;
                if (focus)
                    zx_exact16[n_zx_exact16++] = (uint32_t)exact;
            }
            if (first_ge) {
                zx_first[n_zx_first++] = (uint32_t)first_ge;
                if (focus)
                    zx_first16[n_zx_first16++] = (uint32_t)first_ge;
            }
            if (wbest_d) {
                zx_best[n_zx_best++] = (uint32_t)wbest_d;
                if (focus)
                    zx_best16[n_zx_best16++] = (uint32_t)wbest_d;
            }
        }
        printf("  zxNew A=%llu prod_hit=%llu A_best>=16=%llu A_and_short=%llu A_16-31=%d zx_walk=%.1f ms\n",
               (unsigned long long)zx_a, (unsigned long long)zx_hit,
               (unsigned long long)zx_a_long, (unsigned long long)zx_short,
               n_zx_a1631, now_ms() - t0);
        dump_depth_stats("zx A exact-off", zx_exact, n_zx_exact);
        dump_depth_stats("zx A first ml>=zlen", zx_first, n_zx_first);
        dump_depth_stats("zx A best-in-512", zx_best, n_zx_best);
        dump_depth_stats("zx A16-31 exact-off", zx_exact16, n_zx_exact16);
        dump_depth_stats("zx A16-31 first ml>=zlen", zx_first16, n_zx_first16);
        dump_depth_stats("zx A16-31 best-in-512", zx_best16, n_zx_best16);
    }

oracle_done:
    for (int t = 0; t < 5; t++) {
        free(first_d[t]);
        free(best_d[t]);
    }
    free(zx_exact);
    free(zx_first);
    free(zx_best);
    free(zx_exact16);
    free(zx_first16);
    free(zx_best16);
    lz0_finder_free(&f);
    fflush(stdout);
}

#define REP_LIM 512
#define REP_MAXC 512

typedef struct {
    uint32_t cur;
    int depth;
    int ml;
    unsigned char b3;
    uint32_t p4;
    uint64_t p8;
} rep_cand;

static int set_has_u8(const unsigned char *s, int n, unsigned char v) {
    for (int i = 0; i < n; i++)
        if (s[i] == v)
            return 1;
    return 0;
}

static int set_has_u32(const uint32_t *s, int n, uint32_t v) {
    for (int i = 0; i < n; i++)
        if (s[i] == v)
            return 1;
    return 0;
}

static int set_has_u64(const uint64_t *s, int n, uint64_t v) {
    for (int i = 0; i < n; i++)
        if (s[i] == v)
            return 1;
    return 0;
}

static int pick_prefix(const rep_cand *c, int nc, int kind, int k, int *idx) {
    int n = 0;
    unsigned char b3s[16];
    uint32_t p4s[16];
    uint64_t p8s[16];
    for (int i = 0; i < nc && n < k; i++) {
        int keep = 0;
        if (kind == 3) {
            if (!set_has_u8(b3s, n, c[i].b3)) {
                b3s[n] = c[i].b3;
                keep = 1;
            }
        } else if (kind == 4) {
            if (!set_has_u32(p4s, n, c[i].p4)) {
                p4s[n] = c[i].p4;
                keep = 1;
            }
        } else {
            if (!set_has_u64(p8s, n, c[i].p8)) {
                p8s[n] = c[i].p8;
                keep = 1;
            }
        }
        if (keep)
            idx[n++] = i;
    }
    return n;
}

static void dump_u32_hist(const char *title, uint32_t *v, int n) {
    if (n <= 0) {
        printf("  %s n=0\n", title);
        return;
    }
    uint64_t sum = 0, mx = 0;
    for (int i = 0; i < n; i++) {
        sum += v[i];
        if (v[i] > mx)
            mx = v[i];
    }
    printf("  %s n=%d avg=%.2f p25=%u med=%u p75=%u max=%llu\n",
           title, n, (double)sum / (double)n,
           pct_u32(v, n, 25), median_u32(v, n), pct_u32(v, n, 75),
           (unsigned long long)mx);
}

static void run_rep_oracle(const char *kind, size_t n, const unsigned char *in,
                           const gtok *zx, int nz) {
    lz0_config cfg;
    lz0_cfg(&cfg, 32, 0, 0, 0, 0, 0);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  rep finder FAIL\n");
        return;
    }
    enum { SCH_A, SCH_B, SCH_C, SCH_D, SCH_E, SCH_F, SCH_G, SCH_H, NSCH };
    static const char *slab[NSCH] = {
        "A newest", "B new+old", "C b3x4", "D b3x8",
        "E p4x4", "F p4x8", "G p8x4", "H p8x8"
    };
    uint64_t hit16[NSCH], hitA[NSCH], nrep[NSCH], sum_best[NSCH], sum_best16[NSCH];
    memset(hit16, 0, sizeof(hit16));
    memset(hitA, 0, sizeof(hitA));
    memset(nrep, 0, sizeof(nrep));
    memset(sum_best, 0, sizeof(sum_best));
    memset(sum_best16, 0, sizeof(sum_best16));

    uint32_t *db3 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *dp4 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *dp8 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *rank_b3 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *rank_p4 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *rank_p8 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *same_b3 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    if (!db3 || !dp4 || !dp8 || !rank_b3 || !rank_p4 || !rank_p8 || !same_b3) {
        printf("  rep alloc FAIL\n");
        lz0_finder_free(&f);
        return;
    }
    int nd = 0, n_first_b3 = 0, n_first_p4 = 0, n_first_p8 = 0;
    int n_allA = 0, n_a1631 = 0, n_a1631_in = 0;
    uint64_t n_vis = 0;
    lz0_match pcands[MAX_CAND];
    rep_cand chain[REP_MAXC];
    double t0 = now_ms();
    for (int i = 0; i < nz; i++) {
        if (!zx[i].offset || zx[i].offset == zx[i].last)
            continue;
        uint32_t pos = zx[i].pos;
        uint32_t zoff = zx[i].offset;
        uint32_t zlen = zx[i].length;
        if (pos + 2 >= (uint32_t)n || pos < zoff)
            continue;
        lz0_finder_insert_upto(&f, pos);
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, zx[i].last, pcands, &last_len);
        int full_hit = 0;
        for (int c = 0; c < nc; c++) {
            if (pcands[c].offset == zoff && pcands[c].length >= zlen)
                full_hit = 1;
        }
        if (full_hit)
            continue;
        n_allA++;
        int focus = (zlen >= 16 && zlen <= 31);
        if (focus)
            n_a1631++;
        uint32_t target = pos - zoff;
        uint32_t cur = f.head[gap_hash3(in + pos)];
        int nch = 0, depth = 0, exact_i = -1;
        int maxl = (int)((uint32_t)n - pos);
        while (cur != LZ0_CHAIN_NIL && depth < REP_LIM && nch < REP_MAXC) {
            depth++;
            n_vis++;
            if (cur >= pos) {
                cur = f.prev[cur];
                continue;
            }
            int clen = (int)((uint32_t)n - cur);
            int ml = 0;
            if (in[cur] == in[pos])
                ml = lz0_matchlen(in + pos, in + cur, maxl < clen ? maxl : clen);
            rep_cand rc;
            rc.cur = cur;
            rc.depth = depth;
            rc.ml = ml;
            rc.b3 = (cur + 3 < (uint32_t)n) ? in[cur + 3] : 0;
            rc.p4 = 0;
            rc.p8 = 0;
            int n4 = clen < 4 ? clen : 4;
            int n8 = clen < 8 ? clen : 8;
            memcpy(&rc.p4, in + cur, (size_t)n4);
            memcpy(&rc.p8, in + cur, (size_t)n8);
            if (cur == target)
                exact_i = nch;
            chain[nch++] = rc;
            cur = f.prev[cur];
        }
        if (focus && exact_i >= 0)
            n_a1631_in++;
        if (nch <= 0)
            continue;

        int n_b3 = 0, n_p4 = 0, n_p8 = 0;
        unsigned char ub3[256];
        memset(ub3, 0, sizeof(ub3));
        uint32_t up4[256];
        uint64_t up8[256];
        int rb3 = 0, rp4 = 0, rp8 = 0, nb_same = 0;
        unsigned char zb3 = 0;
        uint32_t zp4 = 0;
        uint64_t zp8 = 0;
        if (exact_i >= 0) {
            zb3 = chain[exact_i].b3;
            zp4 = chain[exact_i].p4;
            zp8 = chain[exact_i].p8;
        }
        for (int c = 0; c < nch; c++) {
            if (!ub3[chain[c].b3]) {
                ub3[chain[c].b3] = 1;
                n_b3++;
                if (exact_i >= 0 && !rb3 && chain[c].b3 == zb3)
                    rb3 = n_b3;
            }
            if (!set_has_u32(up4, n_p4, chain[c].p4) && n_p4 < 256) {
                up4[n_p4++] = chain[c].p4;
                if (exact_i >= 0 && !rp4 && chain[c].p4 == zp4)
                    rp4 = n_p4;
            }
            if (!set_has_u64(up8, n_p8, chain[c].p8) && n_p8 < 256) {
                up8[n_p8++] = chain[c].p8;
                if (exact_i >= 0 && !rp8 && chain[c].p8 == zp8)
                    rp8 = n_p8;
            }
            if (exact_i >= 0 && chain[c].b3 == zb3)
                nb_same++;
        }
        if (focus && exact_i >= 0) {
            db3[nd] = (uint32_t)n_b3;
            dp4[nd] = (uint32_t)n_p4;
            dp8[nd] = (uint32_t)n_p8;
            rank_b3[nd] = (uint32_t)rb3;
            rank_p4[nd] = (uint32_t)rp4;
            rank_p8[nd] = (uint32_t)rp8;
            same_b3[nd] = (uint32_t)nb_same;
            nd++;
            if (rb3 == 1)
                n_first_b3++;
            if (rp4 == 1)
                n_first_p4++;
            if (rp8 == 1)
                n_first_p8++;
        }

        int idx[16];
        int nsel[NSCH];
        nsel[SCH_A] = nch > 0 ? 1 : 0;
        if (nsel[SCH_A])
            idx[0] = 0;
        nsel[SCH_B] = 0;
        if (nch > 0) {
            idx[0] = 0;
            nsel[SCH_B] = 1;
            if (nch > 1) {
                idx[1] = nch - 1;
                nsel[SCH_B] = 2;
            }
        }
        int tmp[16];
        int maps[NSCH][16];
        if (nsel[SCH_A])
            maps[SCH_A][0] = 0;
        if (nsel[SCH_B]) {
            maps[SCH_B][0] = 0;
            if (nsel[SCH_B] == 2)
                maps[SCH_B][1] = nch - 1;
        }
        nsel[SCH_C] = pick_prefix(chain, nch, 3, 4, tmp);
        memcpy(maps[SCH_C], tmp, (size_t)nsel[SCH_C] * sizeof(int));
        nsel[SCH_D] = pick_prefix(chain, nch, 3, 8, tmp);
        memcpy(maps[SCH_D], tmp, (size_t)nsel[SCH_D] * sizeof(int));
        nsel[SCH_E] = pick_prefix(chain, nch, 4, 4, tmp);
        memcpy(maps[SCH_E], tmp, (size_t)nsel[SCH_E] * sizeof(int));
        nsel[SCH_F] = pick_prefix(chain, nch, 4, 8, tmp);
        memcpy(maps[SCH_F], tmp, (size_t)nsel[SCH_F] * sizeof(int));
        nsel[SCH_G] = pick_prefix(chain, nch, 8, 4, tmp);
        memcpy(maps[SCH_G], tmp, (size_t)nsel[SCH_G] * sizeof(int));
        nsel[SCH_H] = pick_prefix(chain, nch, 8, 8, tmp);
        memcpy(maps[SCH_H], tmp, (size_t)nsel[SCH_H] * sizeof(int));

        for (int s = 0; s < NSCH; s++) {
            int best = 0, got = 0;
            nrep[s] += (uint64_t)nsel[s];
            for (int r = 0; r < nsel[s]; r++) {
                int j = maps[s][r];
                if (chain[j].ml > best)
                    best = chain[j].ml;
                if (exact_i >= 0 && j == exact_i && chain[j].ml >= (int)zlen)
                    got = 1;
            }
            sum_best[s] += (uint64_t)best;
            if (got)
                hitA[s]++;
            if (focus && exact_i >= 0) {
                sum_best16[s] += (uint64_t)best;
                if (got)
                    hit16[s]++;
            }
        }
    }
    double ms = now_ms() - t0;
    printf("=== n=%zu %s REP ORACLE lim=%d ===\n", n, kind, REP_LIM);
    printf("  A=%d A16-31=%d in512=%d vis=%llu ms=%.1f\n",
           n_allA, n_a1631, n_a1631_in, (unsigned long long)n_vis, ms);
    dump_u32_hist("distinct b3", db3, nd);
    dump_u32_hist("distinct p4", dp4, nd);
    dump_u32_hist("distinct p8", dp8, nd);
    dump_u32_hist("rank ZX0 b3", rank_b3, nd);
    dump_u32_hist("rank ZX0 p4", rank_p4, nd);
    dump_u32_hist("rank ZX0 p8", rank_p8, nd);
    dump_u32_hist("same-b3 count", same_b3, nd);
    printf("  ZX0 is newest of its b3=%d (%.1f%%)  p4=%d (%.1f%%)  p8=%d (%.1f%%)  of %d\n",
           n_first_b3, nd ? 100.0 * (double)n_first_b3 / (double)nd : 0,
           n_first_p4, nd ? 100.0 * (double)n_first_p4 / (double)nd : 0,
           n_first_p8, nd ? 100.0 * (double)n_first_p8 / (double)nd : 0, nd);
    printf("  scheme                         hit16   %%16  hitA    %%A  avg_nrep  avg_best  avg_best16\n");
    for (int s = 0; s < NSCH; s++) {
        printf("  %-28s %5llu %5.1f%% %5llu %5.1f%%   %6.2f    %6.1f      %6.1f\n",
               slab[s],
               (unsigned long long)hit16[s],
               n_a1631_in ? 100.0 * (double)hit16[s] / (double)n_a1631_in : 0,
               (unsigned long long)hitA[s],
               n_allA ? 100.0 * (double)hitA[s] / (double)n_allA : 0,
               n_allA ? (double)nrep[s] / (double)n_allA : 0,
               n_allA ? (double)sum_best[s] / (double)n_allA : 0,
               n_a1631_in ? (double)sum_best16[s] / (double)n_a1631_in : 0);
    }
    free(db3);
    free(dp4);
    free(dp8);
    free(rank_b3);
    free(rank_p4);
    free(rank_p8);
    free(same_b3);
    lz0_finder_free(&f);
    fflush(stdout);
}

#define SIG_LIM 512
#define SIG_MAXC 512
#define NSIG 8

static const int SIG_N[NSIG] = {3, 4, 5, 6, 8, 10, 12, 16};

static int prefix_eq(const unsigned char *a, const unsigned char *b, int n, int maxa, int maxb) {
    if (n > maxa || n > maxb)
        return 0;
    return memcmp(a, b, (size_t)n) == 0;
}

static void dump_sig_group(const char *title, uint32_t *csz, uint32_t *rank, int n, int n_found, int n_base) {
    uint64_t le[6] = {0};
    static const int lims[6] = {1, 2, 4, 8, 16, 32};
    uint64_t newest = 0;
    uint64_t sumc = 0, sumr = 0, mxc = 0, mxr = 0;
    for (int i = 0; i < n; i++) {
        sumc += csz[i];
        sumr += rank[i];
        if (csz[i] > mxc)
            mxc = csz[i];
        if (rank[i] > mxr)
            mxr = rank[i];
        if (rank[i] == 1)
            newest++;
        for (int k = 0; k < 6; k++) {
            if (csz[i] <= (uint32_t)lims[k])
                le[k]++;
        }
    }
    printf("  %s n=%d found=%d/%d (%.1f%%)\n", title, n, n_found, n_base,
           n_base ? 100.0 * (double)n_found / (double)n_base : 0);
    if (n <= 0)
        return;
    printf("    class  avg=%.1f p25=%u med=%u p75=%u max=%llu\n",
           (double)sumc / (double)n, pct_u32(csz, n, 25), median_u32(csz, n),
           pct_u32(csz, n, 75), (unsigned long long)mxc);
    printf("    rank   avg=%.1f p25=%u med=%u p75=%u max=%llu  newest=%llu (%.1f%%)\n",
           (double)sumr / (double)n, pct_u32(rank, n, 25), median_u32(rank, n),
           pct_u32(rank, n, 75), (unsigned long long)mxr,
           (unsigned long long)newest, 100.0 * (double)newest / (double)n);
    printf("    class<= 1=%.1f%% 2=%.1f%% 4=%.1f%% 8=%.1f%% 16=%.1f%% 32=%.1f%%\n",
           100.0 * (double)le[0] / (double)n, 100.0 * (double)le[1] / (double)n,
           100.0 * (double)le[2] / (double)n, 100.0 * (double)le[3] / (double)n,
           100.0 * (double)le[4] / (double)n, 100.0 * (double)le[5] / (double)n);
}

static void run_sig_oracle(const char *kind, size_t n, const unsigned char *in,
                           const gtok *zx, int nz) {
    lz0_config cfg;
    lz0_cfg(&cfg, 32, 0, 0, 0, 0, 0);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  sig finder FAIL\n");
        return;
    }
    uint32_t *cszA[NSIG], *rankA[NSIG], *csz16[NSIG], *rank16[NSIG];
    int nA[NSIG], n16[NSIG];
    int n_foundA[NSIG], n_found16[NSIG];
    for (int s = 0; s < NSIG; s++) {
        cszA[s] = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        rankA[s] = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        csz16[s] = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        rank16[s] = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
        nA[s] = n16[s] = 0;
        n_foundA[s] = n_found16[s] = 0;
        if (!cszA[s] || !rankA[s] || !csz16[s] || !rank16[s]) {
            printf("  sig alloc FAIL\n");
            lz0_finder_free(&f);
            return;
        }
    }
    int n_allA = 0, n_a1631 = 0, n_a1631_in = 0, n_allA_in = 0;
    uint64_t n_vis = 0;
    lz0_match pcands[MAX_CAND];
    uint32_t chain[SIG_MAXC];
    double t0 = now_ms();
    for (int i = 0; i < nz; i++) {
        if (!zx[i].offset || zx[i].offset == zx[i].last)
            continue;
        uint32_t pos = zx[i].pos;
        uint32_t zoff = zx[i].offset;
        uint32_t zlen = zx[i].length;
        if (pos + 2 >= (uint32_t)n || pos < zoff)
            continue;
        lz0_finder_insert_upto(&f, pos);
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, zx[i].last, pcands, &last_len);
        int full_hit = 0;
        for (int c = 0; c < nc; c++) {
            if (pcands[c].offset == zoff && pcands[c].length >= zlen)
                full_hit = 1;
        }
        if (full_hit)
            continue;
        n_allA++;
        int focus = (zlen >= 16 && zlen <= 31);
        if (focus)
            n_a1631++;
        uint32_t target = pos - zoff;
        uint32_t cur = f.head[gap_hash3(in + pos)];
        int nch = 0, depth = 0, exact_i = -1;
        int maxp = (int)((uint32_t)n - pos);
        while (cur != LZ0_CHAIN_NIL && depth < SIG_LIM && nch < SIG_MAXC) {
            depth++;
            n_vis++;
            if (cur < pos) {
                if (cur == target)
                    exact_i = nch;
                chain[nch++] = cur;
            }
            cur = f.prev[cur];
        }
        if (exact_i >= 0) {
            n_allA_in++;
            if (focus)
                n_a1631_in++;
        }
        for (int s = 0; s < NSIG; s++) {
            int N = SIG_N[s];
            int cls = 0, rnk = 0, got = 0;
            for (int c = 0; c < nch; c++) {
                uint32_t src = chain[c];
                int maxs = (int)((uint32_t)n - src);
                if (!prefix_eq(in + pos, in + src, N, maxp, maxs))
                    continue;
                cls++;
                if (src == target && !rnk) {
                    rnk = cls;
                    got = 1;
                }
            }
            if (got) {
                n_foundA[s]++;
                cszA[s][nA[s]] = (uint32_t)cls;
                rankA[s][nA[s]] = (uint32_t)rnk;
                nA[s]++;
                if (focus) {
                    n_found16[s]++;
                    csz16[s][n16[s]] = (uint32_t)cls;
                    rank16[s][n16[s]] = (uint32_t)rnk;
                    n16[s]++;
                }
            }
        }
    }
    double ms = now_ms() - t0;
    printf("=== n=%zu %s SIG ORACLE lim=%d ===\n", n, kind, SIG_LIM);
    printf("  A=%d in512=%d  A16-31=%d in512=%d vis=%llu ms=%.1f\n",
           n_allA, n_allA_in, n_a1631, n_a1631_in, (unsigned long long)n_vis, ms);
    for (int s = 0; s < NSIG; s++) {
        char t16[64], tA[64];
        snprintf(t16, sizeof(t16), "N=%d A16-31", SIG_N[s]);
        snprintf(tA, sizeof(tA), "N=%d all-A", SIG_N[s]);
        dump_sig_group(t16, csz16[s], rank16[s], n16[s], n_found16[s], n_a1631);
        dump_sig_group(tA, cszA[s], rankA[s], nA[s], n_foundA[s], n_allA);
    }
    for (int s = 0; s < NSIG; s++) {
        free(cszA[s]);
        free(rankA[s]);
        free(csz16[s]);
        free(rank16[s]);
    }
    lz0_finder_free(&f);
    fflush(stdout);
}

#define SA_WIN 64
#define NWIN 7
static const int SA_RAD[NWIN] = {1, 2, 4, 8, 16, 32, 64};
static const int SA_TH[5] = {16, 20, 24, 28, 32};

static void counting_sort_sa(int *sa, int *out, const int *key, int n, int maxk) {
    int *cnt = (int *)calloc((size_t)maxk + 1, sizeof(int));
    if (!cnt)
        return;
    for (int i = 0; i < n; i++)
        cnt[key[sa[i]]]++;
    for (int i = 1; i <= maxk; i++)
        cnt[i] += cnt[i - 1];
    for (int i = n - 1; i >= 0; i--) {
        int k = key[sa[i]];
        out[--cnt[k]] = sa[i];
    }
    free(cnt);
}

static int build_suffix_array(const unsigned char *s, int n, int *sa, int *rank) {
    int *tmp = (int *)malloc((size_t)n * sizeof(int));
    int *nr = (int *)malloc((size_t)n * sizeof(int));
    int *k2 = (int *)malloc((size_t)n * sizeof(int));
    if (!tmp || !nr || !k2) {
        free(tmp);
        free(nr);
        free(k2);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        sa[i] = i;
        rank[i] = (int)s[i] + 1;
    }
    int maxk = 256;
    for (int k = 1; k < n; k <<= 1) {
        for (int i = 0; i < n; i++)
            k2[i] = (i + k < n) ? rank[i + k] + 1 : 0;
        counting_sort_sa(sa, tmp, k2, n, maxk + 1);
        memcpy(sa, tmp, (size_t)n * sizeof(int));
        counting_sort_sa(sa, tmp, rank, n, maxk);
        memcpy(sa, tmp, (size_t)n * sizeof(int));
        nr[sa[0]] = 1;
        maxk = 1;
        for (int i = 1; i < n; i++) {
            int a = sa[i - 1], b = sa[i];
            int a2 = (a + k < n) ? rank[a + k] : -1;
            int b2 = (b + k < n) ? rank[b + k] : -1;
            if (rank[a] != rank[b] || a2 != b2)
                maxk++;
            nr[b] = maxk;
        }
        memcpy(rank, nr, (size_t)n * sizeof(int));
        if (maxk == n)
            break;
    }
    for (int i = 0; i < n; i++)
        rank[i]--;
    free(tmp);
    free(nr);
    free(k2);
    return 0;
}

static void run_sa_oracle(const char *kind, size_t n, const unsigned char *in,
                          const gtok *zx, int nz) {
    int nn = (int)n;
    int *sa = (int *)malloc((size_t)nn * sizeof(int));
    int *rank = (int *)malloc((size_t)nn * sizeof(int));
    if (!sa || !rank) {
        printf("  sa alloc FAIL\n");
        free(sa);
        free(rank);
        return;
    }
    double t0 = now_ms();
    if (build_suffix_array(in, nn, sa, rank) != 0) {
        printf("  sa build FAIL\n");
        free(sa);
        free(rank);
        return;
    }
    double sa_ms = now_ms() - t0;
    printf("=== n=%zu %s SA ORACLE win=%d sa_build=%.1f ms ===\n", n, kind, SA_WIN, sa_ms);
    fflush(stdout);

    lz0_config cfg;
    lz0_cfg(&cfg, 32, 0, 0, 0, 0, 0);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  sa finder FAIL\n");
        free(sa);
        free(rank);
        return;
    }

    uint64_t rec16[5][NWIN], recA[5][NWIN];
    memset(rec16, 0, sizeof(rec16));
    memset(recA, 0, sizeof(recA));
    uint64_t rec_exact16[NWIN], rec_exactA[NWIN];
    memset(rec_exact16, 0, sizeof(rec_exact16));
    memset(rec_exactA, 0, sizeof(rec_exactA));
    uint64_t n_cmp = 0, n_ml_bytes = 0;
    uint64_t sum_best16 = 0, sum_bestA = 0;
    uint64_t sum_best16w[NWIN], sum_bestAw[NWIN];
    memset(sum_best16w, 0, sizeof(sum_best16w));
    memset(sum_bestAw, 0, sizeof(sum_bestAw));
    uint32_t *d_exact16 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *d_best16 = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *d_exactA = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    uint32_t *d_bestA = (uint32_t *)malloc((size_t)nz * sizeof(uint32_t));
    int ne16 = 0, nb16 = 0, neA = 0, nbA = 0;
    int n_allA = 0, n_a1631 = 0, n_a1631_in = 0, n_allA_in = 0;
    int n_exact_inwin16 = 0, n_exact_inwinA = 0;
    lz0_match pcands[MAX_CAND];
    t0 = now_ms();
    for (int i = 0; i < nz; i++) {
        if (!zx[i].offset || zx[i].offset == zx[i].last)
            continue;
        uint32_t pos = zx[i].pos;
        uint32_t zoff = zx[i].offset;
        uint32_t zlen = zx[i].length;
        if (pos + 2 >= (uint32_t)n || pos < zoff)
            continue;
        lz0_finder_insert_upto(&f, pos);
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, zx[i].last, pcands, &last_len);
        int full_hit = 0;
        for (int c = 0; c < nc; c++) {
            if (pcands[c].offset == zoff && pcands[c].length >= zlen)
                full_hit = 1;
        }
        if (full_hit)
            continue;
        n_allA++;
        int focus = (zlen >= 16 && zlen <= 31);
        if (focus)
            n_a1631++;
        uint32_t target = pos - zoff;
        int rpos = rank[pos];
        int rtgt = rank[target];
        int rdist = rpos > rtgt ? rpos - rtgt : rtgt - rpos;
        int in512 = 0;
        {
            uint32_t cur = f.head[gap_hash3(in + pos)];
            int depth = 0;
            while (cur != LZ0_CHAIN_NIL && depth < 512) {
                depth++;
                if (cur == target) {
                    in512 = 1;
                    break;
                }
                cur = f.prev[cur];
            }
        }
        if (in512) {
            n_allA_in++;
            if (focus)
                n_a1631_in++;
        }
        int maxl = (int)((uint32_t)n - pos);
        int best_ml = 0, best_rd = 0;
        int best_w[NWIN];
        memset(best_w, 0, sizeof(best_w));
        int got_th[5][NWIN];
        int got_ex[NWIN];
        memset(got_th, 0, sizeof(got_th));
        memset(got_ex, 0, sizeof(got_ex));
        for (int d = 1; d <= SA_WIN; d++) {
            int sides[2] = {rpos - d, rpos + d};
            for (int s = 0; s < 2; s++) {
                int rr = sides[s];
                if (rr < 0 || rr >= nn)
                    continue;
                int src = sa[rr];
                n_cmp++;
                if (src < 0 || (uint32_t)src >= pos)
                    continue;
                int ml = lz0_matchlen(in + pos, in + src, maxl);
                n_ml_bytes += (uint64_t)ml;
                if (ml > best_ml) {
                    best_ml = ml;
                    best_rd = d;
                }
                int is_ex = ((uint32_t)src == target && ml >= (int)zlen);
                for (int w = 0; w < NWIN; w++) {
                    if (d <= SA_RAD[w]) {
                        if (ml > best_w[w])
                            best_w[w] = ml;
                        if (is_ex)
                            got_ex[w] = 1;
                        for (int t = 0; t < 5; t++) {
                            if (ml >= SA_TH[t])
                                got_th[t][w] = 1;
                        }
                    }
                }
            }
        }
        sum_bestA += (uint64_t)best_ml;
        for (int w = 0; w < NWIN; w++)
            sum_bestAw[w] += (uint64_t)best_w[w];
        if (best_rd > 0)
            d_bestA[nbA++] = (uint32_t)best_rd;
        d_exactA[neA++] = (uint32_t)rdist;
        if (rdist > 0 && rdist <= SA_WIN)
            n_exact_inwinA++;
        for (int w = 0; w < NWIN; w++) {
            if (got_ex[w])
                rec_exactA[w]++;
            for (int t = 0; t < 5; t++) {
                if (got_th[t][w])
                    recA[t][w]++;
            }
        }
        if (focus && in512) {
            sum_best16 += (uint64_t)best_ml;
            for (int w = 0; w < NWIN; w++)
                sum_best16w[w] += (uint64_t)best_w[w];
            if (best_rd > 0)
                d_best16[nb16++] = (uint32_t)best_rd;
            d_exact16[ne16++] = (uint32_t)rdist;
            if (rdist > 0 && rdist <= SA_WIN)
                n_exact_inwin16++;
            for (int w = 0; w < NWIN; w++) {
                if (got_ex[w])
                    rec_exact16[w]++;
                for (int t = 0; t < 5; t++) {
                    if (got_th[t][w])
                        rec16[t][w]++;
                }
            }
        }
    }
    double walk_ms = now_ms() - t0;
    int den16 = n_a1631_in > 0 ? n_a1631_in : 1;
    int denA = n_allA > 0 ? n_allA : 1;
    printf("  A=%d in512=%d  A16-31=%d in512=%d  walk=%.1f ms cmp=%llu ml_bytes=%llu\n",
           n_allA, n_allA_in, n_a1631, n_a1631_in, walk_ms,
           (unsigned long long)n_cmp, (unsigned long long)n_ml_bytes);
    printf("  recover fraction of A16-31-in512=%d  (any cand with ml>=T)\n", n_a1631_in);
    printf("    win   >=16    >=20    >=24    >=28    >=32   exactZX0\n");
    for (int w = 0; w < NWIN; w++) {
        printf("    ±%-3d %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%%\n",
               SA_RAD[w],
               100.0 * (double)rec16[0][w] / (double)den16,
               100.0 * (double)rec16[1][w] / (double)den16,
               100.0 * (double)rec16[2][w] / (double)den16,
               100.0 * (double)rec16[3][w] / (double)den16,
               100.0 * (double)rec16[4][w] / (double)den16,
               100.0 * (double)rec_exact16[w] / (double)den16);
    }
    printf("  recover fraction of all A=%d\n", n_allA);
    printf("    win   >=16    >=20    >=24    >=28    >=32   exactZX0\n");
    for (int w = 0; w < NWIN; w++) {
        printf("    ±%-3d %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%%\n",
               SA_RAD[w],
               100.0 * (double)recA[0][w] / (double)denA,
               100.0 * (double)recA[1][w] / (double)denA,
               100.0 * (double)recA[2][w] / (double)denA,
               100.0 * (double)recA[3][w] / (double)denA,
               100.0 * (double)recA[4][w] / (double)denA,
               100.0 * (double)rec_exactA[w] / (double)denA);
    }
    printf("  exact ZX0 rank-distance  A16-31 in512 n=%d  in±64=%d\n", ne16, n_exact_inwin16);
    dump_u32_hist("    dist", d_exact16, ne16);
    printf("  exact ZX0 rank-distance  all A n=%d  in±64=%d\n", neA, n_exact_inwinA);
    dump_u32_hist("    dist", d_exactA, neA);
    printf("  best-ml-in-±64 rank-distance  A16-31\n");
    dump_u32_hist("    dist", d_best16, nb16);
    printf("  best-ml-in-±64 rank-distance  all A\n");
    dump_u32_hist("    dist", d_bestA, nbA);
    printf("  avg best_len ±64  A16-31=%.1f  allA=%.1f\n",
           n_a1631_in ? (double)sum_best16 / (double)n_a1631_in : 0,
           n_allA ? (double)sum_bestA / (double)n_allA : 0);
    printf("  scheme  probes  exact16  %%16  exactA  %%A   avg_best16  avg_bestA\n");
    {
        static const char *lab[NWIN] = {"B ±1", "C ±2", "D ±4", "E ±8", "F ±16", "G ±32", "H ±64"};
        for (int w = 0; w < NWIN; w++) {
            printf("  %-7s %4d  %6llu %5.1f%% %6llu %5.1f%%     %6.1f      %6.1f\n",
                   lab[w], 2 * SA_RAD[w],
                   (unsigned long long)rec_exact16[w],
                   100.0 * (double)rec_exact16[w] / (double)den16,
                   (unsigned long long)rec_exactA[w],
                   100.0 * (double)rec_exactA[w] / (double)denA,
                   n_a1631_in ? (double)sum_best16w[w] / (double)n_a1631_in : 0,
                   n_allA ? (double)sum_bestAw[w] / (double)n_allA : 0);
        }
    }
    printf("  A newest neighbor = B ±1 (rank-1 and rank+1)\n");
    free(d_exact16);
    free(d_best16);
    free(d_exactA);
    free(d_bestA);
    lz0_finder_free(&f);
    free(sa);
    free(rank);
    fflush(stdout);
}

static void run_one_sa_proto(const char *kind, size_t n, const unsigned char *in,
                             const gtok *zx, int nz, int win, int lcp, int sample) {
    lz0_config cfg;
    lz0_cfg(&cfg, 32, 0, 0, 0, 0, 0);
    lz0_sa_win = win;
    lz0_sa_lcp = lcp;
    lz0_sa_sample = sample;
    cfg.jump_levels = 0;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census sdp;
    memset(&sdp, 0, sizeof(sdp));
    lz0_sdp_census_out = &sdp;
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        printf("=== n=%zu %s SA win=%d lcp=%d samp=%d FAIL ===\n", n, kind, win, lcp, sample);
        lz0_sa_win = 0;
        lz0_sa_lcp = 0;
        lz0_sa_sample = 1;
        lz0_sa_hybrid = 0;
        lz0_sa_thresh = 0;
        lz0_sa_adapt = 0;
        return;
    }
    unsigned char *dec = (unsigned char *)malloc(n);
    int rt = 0;
    if (dec) {
        int d = lz0_decompress_to(comp, (int)csz, dec, (int)n);
        rt = (d == (int)n && memcmp(dec, in, n) == 0);
        free(dec);
    }
    printf("=== n=%zu %s SA win=%d lcp=%d samp=%d csz=%zu ratio=%.2f%% enc=%.1f find=%.1f sabuild=%.1f ram=%.0fKB cmp=%llu mlB=%llu check=%llu %s ===\n",
           n, kind, win, lcp, sample, csz, 100.0 * (double)csz / (double)n,
           st.total_ms, sdp.ms_find, sdp.ms_sa_build > 0 ? sdp.ms_sa_build : lz0_sa_build_ms,
           st.peak_ram / 1024.0,
           (unsigned long long)sdp.n_sa_cmp,
           (unsigned long long)sdp.n_sa_ml_bytes,
           (unsigned long long)sdp.n_chain_check, rt ? "ok" : "FAIL");
    free(comp);

    lz0_finder f;
    lz0_sa_win = win;
    lz0_sa_lcp = lcp;
    lz0_sa_sample = sample;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  sa proto finder FAIL\n");
        lz0_sa_win = 0;
        lz0_sa_lcp = 0;
        lz0_sa_sample = 1;
        lz0_sa_hybrid = 0;
        lz0_sa_thresh = 0;
        lz0_sa_adapt = 0;
        return;
    }
    uint64_t n_zx_new = 0, A = 0, n_full = 0;
    uint64_t rec_th[5], rec_ex16 = 0, rec_exA = 0, n_a1631 = 0;
    uint64_t sum_best = 0, sum_best16 = 0;
    memset(rec_th, 0, sizeof(rec_th));
    static const int TH[5] = {16, 20, 24, 28, 32};
    lz0_match cands[MAX_CAND];
    for (int i = 0; i < nz; i++) {
        if (!zx[i].offset || zx[i].offset == zx[i].last)
            continue;
        n_zx_new++;
        uint32_t pos = zx[i].pos;
        uint32_t zoff = zx[i].offset;
        uint32_t zlen = zx[i].length;
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, zx[i].last, cands, &last_len);
        int best = 0, full = 0;
        for (int c = 0; c < nc; c++) {
            if ((int)cands[c].length > best)
                best = (int)cands[c].length;
            if (cands[c].offset == zoff && cands[c].length >= zlen)
                full = 1;
        }
        sum_best += (uint64_t)best;
        if (full)
            n_full++;
        else
            A++;
        int focus = (zlen >= 16 && zlen <= 31);
        if (focus) {
            n_a1631++;
            sum_best16 += (uint64_t)best;
            if (full)
                rec_ex16++;
        }
        if (full)
            rec_exA++;
        for (int t = 0; t < 5; t++) {
            if (best >= TH[t])
                rec_th[t]++;
        }
    }
    printf("  class A=%llu (%.1f%%) full=%llu (%.1f%%) zxNew=%llu A16-31=%llu exact16=%llu (%.1f%%)\n",
           (unsigned long long)A, n_zx_new ? 100.0 * (double)A / (double)n_zx_new : 0,
           (unsigned long long)n_full, n_zx_new ? 100.0 * (double)n_full / (double)n_zx_new : 0,
           (unsigned long long)n_zx_new, (unsigned long long)n_a1631,
           (unsigned long long)rec_ex16, n_a1631 ? 100.0 * (double)rec_ex16 / (double)n_a1631 : 0);
    printf("  recover zxNew  >=16=%.1f%% >=20=%.1f%% >=24=%.1f%% >=28=%.1f%% >=32=%.1f%% exactA=%.1f%% avg_best=%.1f avg_best16=%.1f\n",
           n_zx_new ? 100.0 * (double)rec_th[0] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[1] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[2] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[3] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[4] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_exA / (double)n_zx_new : 0,
           n_zx_new ? (double)sum_best / (double)n_zx_new : 0,
           n_a1631 ? (double)sum_best16 / (double)n_a1631 : 0);
    lz0_finder_free(&f);
    lz0_sa_win = 0;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 0;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 0;
    fflush(stdout);
}

static void run_sa_proto(const char *kind, size_t n, const unsigned char *in,
                         const gtok *zx, int nz) {
    static const int wins[4] = {4, 8, 16, 32};
    printf("=== n=%zu %s SA PROTO A (full SA, no LCP) ===\n", n, kind);
    for (int i = 0; i < 4; i++)
        run_one_sa_proto(kind, n, in, zx, nz, wins[i], 0, 1);
    printf("=== n=%zu %s SA PROTO B (full SA + LCP, win=16) ===\n", n, kind);
    run_one_sa_proto(kind, n, in, zx, nz, 16, 1, 1);
    printf("=== n=%zu %s SA PROTO C (sampled SA, win=16, no LCP) ===\n", n, kind);
    static const int samps[4] = {2, 4, 8, 16};
    for (int i = 0; i < 4; i++)
        run_one_sa_proto(kind, n, in, zx, nz, 16, 0, samps[i]);
}

static void run_one_hybrid(const char *kind, size_t n, const unsigned char *in,
                           const gtok *zx, int nz, const char *lab,
                           int chain, int win, int thresh, int adapt, int jumps) {
    lz0_config cfg;
    lz0_cfg(&cfg, chain, 0, 0, 0, 0, 0);
    cfg.jump_levels = jumps;
    lz0_sa_win = win;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 1;
    lz0_sa_thresh = thresh;
    lz0_sa_adapt = adapt;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census sdp;
    memset(&sdp, 0, sizeof(sdp));
    lz0_sdp_census_out = &sdp;
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        printf("=== n=%zu %s HYBRID %s FAIL ===\n", n, kind, lab);
        lz0_sa_win = 0;
        lz0_sa_hybrid = 0;
        lz0_sa_thresh = 0;
        lz0_sa_adapt = 0;
        return;
    }
    unsigned char *dec = (unsigned char *)malloc(n);
    int rt = 0;
    if (dec) {
        int d = lz0_decompress_to(comp, (int)csz, dec, (int)n);
        rt = (d == (int)n && memcmp(dec, in, n) == 0);
        free(dec);
    }
    double avg_probes = sdp.n_sa_invoke ? (double)sdp.n_sa_cmp / (double)sdp.n_sa_invoke : 0;
    printf("=== n=%zu %s HYBRID %s csz=%zu ratio=%.2f%% enc=%.1f find=%.1f sabuild=%.1f ram=%.0fKB visit=%llu sa_inv=%llu sa_cmp=%llu avgP=%.2f stop4=%llu stop8=%llu stop16=%llu mlB=%llu check=%llu %s ===\n",
           n, kind, lab, csz, 100.0 * (double)csz / (double)n,
           st.total_ms, sdp.ms_find, sdp.ms_sa_build > 0 ? sdp.ms_sa_build : lz0_sa_build_ms,
           st.peak_ram / 1024.0,
           (unsigned long long)sdp.n_chain_visit,
           (unsigned long long)sdp.n_sa_invoke,
           (unsigned long long)sdp.n_sa_cmp,
           avg_probes,
           (unsigned long long)sdp.n_sa_stop4,
           (unsigned long long)sdp.n_sa_stop8,
           (unsigned long long)sdp.n_sa_stop16,
           (unsigned long long)sdp.n_sa_ml_bytes,
           (unsigned long long)sdp.n_chain_check, rt ? "ok" : "FAIL");
    free(comp);

    lz0_finder f;
    lz0_sa_win = win;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 1;
    lz0_sa_thresh = thresh;
    lz0_sa_adapt = adapt;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  hybrid finder FAIL\n");
        lz0_sa_win = 0;
        lz0_sa_hybrid = 0;
        lz0_sa_thresh = 0;
        lz0_sa_adapt = 0;
        return;
    }
    uint64_t n_zx_new = 0, A = 0, n_full = 0;
    uint64_t rec_th[5], rec_ex16 = 0, rec_exA = 0, n_a1631 = 0;
    uint64_t sum_best = 0, sum_best16 = 0;
    memset(rec_th, 0, sizeof(rec_th));
    static const int TH[5] = {16, 20, 24, 28, 32};
    lz0_match cands[MAX_CAND];
    for (int i = 0; i < nz; i++) {
        if (!zx[i].offset || zx[i].offset == zx[i].last)
            continue;
        n_zx_new++;
        uint32_t pos = zx[i].pos;
        uint32_t zoff = zx[i].offset;
        uint32_t zlen = zx[i].length;
        lz0_finder_insert_upto(&f, pos);
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, zx[i].last, cands, &last_len);
        int best = 0, full = 0;
        for (int c = 0; c < nc; c++) {
            if ((int)cands[c].length > best)
                best = (int)cands[c].length;
            if (cands[c].offset == zoff && cands[c].length >= zlen)
                full = 1;
        }
        sum_best += (uint64_t)best;
        if (full)
            n_full++;
        else
            A++;
        int focus = (zlen >= 16 && zlen <= 31);
        if (focus) {
            n_a1631++;
            sum_best16 += (uint64_t)best;
            if (full)
                rec_ex16++;
        }
        if (full)
            rec_exA++;
        for (int t = 0; t < 5; t++) {
            if (best >= TH[t])
                rec_th[t]++;
        }
    }
    printf("  class A=%llu (%.1f%%) full=%llu (%.1f%%) zxNew=%llu A16-31=%llu exact16=%llu (%.1f%%)\n",
           (unsigned long long)A, n_zx_new ? 100.0 * (double)A / (double)n_zx_new : 0,
           (unsigned long long)n_full, n_zx_new ? 100.0 * (double)n_full / (double)n_zx_new : 0,
           (unsigned long long)n_zx_new, (unsigned long long)n_a1631,
           (unsigned long long)rec_ex16, n_a1631 ? 100.0 * (double)rec_ex16 / (double)n_a1631 : 0);
    printf("  recover zxNew  >=16=%.1f%% >=20=%.1f%% >=24=%.1f%% >=28=%.1f%% >=32=%.1f%% exactA=%.1f%% avg_best=%.1f avg_best16=%.1f\n",
           n_zx_new ? 100.0 * (double)rec_th[0] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[1] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[2] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[3] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_th[4] / (double)n_zx_new : 0,
           n_zx_new ? 100.0 * (double)rec_exA / (double)n_zx_new : 0,
           n_zx_new ? (double)sum_best / (double)n_zx_new : 0,
           n_a1631 ? (double)sum_best16 / (double)n_a1631 : 0);
    lz0_finder_free(&f);
    lz0_sa_win = 0;
    lz0_sa_hybrid = 0;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 0;
    fflush(stdout);
}

static void run_hybrid(const char *kind, size_t n, const unsigned char *in,
                       const gtok *zx, int nz) {
    printf("=== n=%zu %s HYBRID H1 (hash3 then SA +/-16 iff best_len<T) ===\n", n, kind);
    run_one_hybrid(kind, n, in, zx, nz, "H1 T=8 win=16", 32, 16, 8, 0, 16);
    run_one_hybrid(kind, n, in, zx, nz, "H1 T=12 win=16", 32, 16, 12, 0, 16);
    run_one_hybrid(kind, n, in, zx, nz, "H1 T=16 win=16", 32, 16, 16, 0, 16);
    printf("=== n=%zu %s HYBRID H2 (hash3 then adaptive SA +/-4->8->16) ===\n", n, kind);
    run_one_hybrid(kind, n, in, zx, nz, "H2 adapt win=16", 32, 16, 0, 1, 16);
    printf("=== n=%zu %s HYBRID H3 (H1+H2 T=8/12/16) ===\n", n, kind);
    run_one_hybrid(kind, n, in, zx, nz, "H3 T=8 adapt", 32, 16, 8, 1, 16);
    run_one_hybrid(kind, n, in, zx, nz, "H3 T=12 adapt", 32, 16, 12, 1, 16);
    run_one_hybrid(kind, n, in, zx, nz, "H3 T=16 adapt", 32, 16, 16, 1, 16);
}

static void run_h2_chain_sweep(const char *kind, size_t n, const unsigned char *in,
                               const gtok *zx, int nz) {
    static const int chains[6] = {4, 8, 12, 16, 24, 32};
    printf("=== n=%zu %s H2 chain sweep (adapt SA +/-4->8->16, hash3 2^k skip) ===\n", n, kind);
    for (int i = 0; i < 6; i++) {
        char lab[48];
        snprintf(lab, sizeof(lab), "H2 C=%d", chains[i]);
        run_one_hybrid(kind, n, in, zx, nz, lab, chains[i], 16, 0, 1, 16);
    }
}

static void run_h2_jump_sweep(const char *kind, size_t n, const unsigned char *in,
                              const gtok *zx, int nz) {
    static const int jumps[5] = {0, 4, 8, 12, 16};
    printf("=== n=%zu %s H2 C=8 jump_levels sweep ===\n", n, kind);
    for (int i = 0; i < 5; i++) {
        char lab[48];
        snprintf(lab, sizeof(lab), "H2C8 J=%d", jumps[i]);
        run_one_hybrid(kind, n, in, zx, nz, lab, 8, 16, 0, 1, jumps[i]);
    }
}

static const gtok *tok_at(const gtok *t, int nt, uint32_t pos) {
    int lo = 0, hi = nt;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (t[m].pos + t[m].length <= pos)
            lo = m + 1;
        else if (t[m].pos > pos)
            hi = m;
        else
            return &t[m];
    }
    return NULL;
}

static void measure_chain(const char *kind, size_t n, const unsigned char *in,
                          const gtok *zx, int nz, int chain, int noskip, int thresh, int stride, int h4, int deep) {
    lz0_config cfg;
    lz0_cfg(&cfg, chain, noskip, thresh, stride, h4, deep);
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census sdp;
    memset(&sdp, 0, sizeof(sdp));
    lz0_sdp_census_out = &sdp;
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        printf("=== n=%zu %s chain=%d noskip=%d T=%d S=%d D=%d DP=%d FAIL ===\n", n, kind, chain, noskip, thresh, stride, h4, deep);
        lz0_skip_thresh = 0;
        lz0_chain_noskip = 0;
        lz0_skip_stride = 0;
        lz0_hash4_probe = 0;
        lz0_deep_probe = 0;
        return;
    }
    unsigned char *dec = (unsigned char *)malloc(n);
    int rt = 0;
    if (dec) {
        int d = lz0_decompress_to(comp, (int)csz, dec, (int)n);
        rt = (d == (int)n && memcmp(dec, in, n) == 0);
        free(dec);
    }
    printf("=== n=%zu %s chain=%d noskip=%d T=%d S=%d D=%d DP=%d csz=%zu ratio=%.2f%% enc=%.1f find=%.1f parse=%.1f ram=%.0fKB visit=%llu deepv=%llu check=%llu h4=%llu %s ===\n",
           n, kind, chain, noskip, thresh, stride, h4, deep, csz, 100.0 * (double)csz / (double)n,
           st.total_ms, sdp.ms_find, st.parse_ms, st.peak_ram / 1024.0,
           (unsigned long long)sdp.n_chain_visit,
           (unsigned long long)sdp.n_deep_visit,
           (unsigned long long)sdp.n_chain_check,
           (unsigned long long)sdp.n_hash4_probe, rt ? "ok" : "FAIL");
    free(comp);

    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  finder FAIL\n");
        return;
    }
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    if (lz0_parse_sdp(&f, &tl) != 0) {
        printf("  lz0 parse FAIL\n");
        lz0_toklist_free(&tl);
        lz0_finder_free(&f);
        return;
    }
    gtok *lz = (gtok *)malloc((size_t)(tl.n + 1) * sizeof(gtok));
    uint32_t last = LZ0_INITIAL_OFFSET;
    uint64_t n_lz_lit = 0, n_lz_last = 0, n_lz_new = 0;
    for (uint32_t i = 0; i < tl.n; i++) {
        lz[i].pos = tl.t[i].pos;
        lz[i].length = tl.t[i].length;
        lz[i].offset = tl.t[i].offset;
        lz[i].last = last;
        if (!tl.t[i].offset)
            n_lz_lit++;
        else if (tl.t[i].offset == last)
            n_lz_last++;
        else
            n_lz_new++;
        if (tl.t[i].offset)
            last = tl.t[i].offset;
    }
    lz0_finder_free(&f);
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("  finder2 FAIL\n");
        lz0_toklist_free(&tl);
        free(lz);
        return;
    }

    uint64_t n_zx_new = 0;
    uint64_t A = 0, B = 0, C = 0, D = 0, same = 0, extend = 0;
    uint64_t A_len[8];
    memset(A_len, 0, sizeof(A_len));
    uint64_t n_off_in_cand = 0, n_full_in_cand = 0;
    uint64_t sum_zx_off = 0, sum_zx_len = 0, sum_lz_best = 0;
    uint64_t dep_all[6], dep_l8[6], dep_l16[6], dep_l32[6];
    memset(dep_all, 0, sizeof(dep_all));
    memset(dep_l8, 0, sizeof(dep_l8));
    memset(dep_l16, 0, sizeof(dep_l16));
    memset(dep_l32, 0, sizeof(dep_l32));
    uint64_t n_l8 = 0, n_l16 = 0, n_l32 = 0;
    uint64_t sum_d_all = 0, sum_d_l16 = 0, max_d_all = 0, max_d_l16 = 0;
    uint32_t *d_all = (uint32_t *)malloc((size_t)(nz + 1) * sizeof(uint32_t));
    uint32_t *d_l16 = (uint32_t *)malloc((size_t)(nz + 1) * sizeof(uint32_t));
    int nd_all = 0, nd_l16 = 0;

    for (int i = 0; i < nz; i++) {
        if (!zx[i].offset || zx[i].offset == zx[i].last)
            continue;
        n_zx_new++;
        uint32_t pos = zx[i].pos;
        uint32_t zoff = zx[i].offset;
        uint32_t zlen = zx[i].length;
        sum_zx_off += zoff;
        sum_zx_len += zlen;
        lz0_finder_insert_upto(&f, pos);
        lz0_match cands[MAX_CAND];
        int last_len = 0;
        int nc = lz0_find_matches(&f, pos, zx[i].last, cands, &last_len);
        int best_len = 0;
        int off_hit = 0;
        int full_hit = 0;
        for (int c = 0; c < nc; c++) {
            if ((int)cands[c].length > best_len)
                best_len = (int)cands[c].length;
            if (cands[c].offset == zoff) {
                off_hit = 1;
                if (cands[c].length >= zlen)
                    full_hit = 1;
            }
        }
        sum_lz_best += (uint64_t)best_len;
        if (off_hit)
            n_off_in_cand++;
        if (full_hit)
            n_full_in_cand++;
        if (!full_hit) {
            A++;
            int b = 0;
            unsigned u = zlen;
            while (u > 1 && b < 7) {
                u >>= 1;
                b++;
            }
            A_len[b]++;
            int d = hash3_chain_depth(&f, pos, zoff);
            int db;
            if (d <= 0)
                db = 5;
            else if (d <= 32)
                db = 0;
            else if (d <= 64)
                db = 1;
            else if (d <= 128)
                db = 2;
            else if (d <= 256)
                db = 3;
            else
                db = 4;
            dep_all[db]++;
            if (zlen >= 8 && zlen <= 15) {
                dep_l8[db]++;
                n_l8++;
            } else if (zlen >= 16 && zlen <= 31) {
                dep_l16[db]++;
                n_l16++;
            } else if (zlen >= 32) {
                dep_l32[db]++;
                n_l32++;
            }
            if (d > 0) {
                if (d_all)
                    d_all[nd_all++] = (uint32_t)d;
                sum_d_all += (uint64_t)d;
                if ((uint64_t)d > max_d_all)
                    max_d_all = (uint64_t)d;
                if (zlen >= 16 && zlen <= 31) {
                    if (d_l16)
                        d_l16[nd_l16++] = (uint32_t)d;
                    sum_d_l16 += (uint64_t)d;
                    if ((uint64_t)d > max_d_l16)
                        max_d_l16 = (uint64_t)d;
                }
            }
        }
        const gtok *lt = tok_at(lz, (int)tl.n, pos);
        if (!lt || lt->pos != pos) {
            D++;
            continue;
        }
        if (lt->offset == zoff && lt->length == zlen) {
            same++;
            continue;
        }
        if (lt->offset == zoff && lt->length >= zlen) {
            extend++;
            continue;
        }
        int cz = lz0_cost_new((int)zoff, (int)zlen);
        int clz;
        if (!lt->offset)
            clz = lz0_cost_literal((int)lt->length);
        else if (lt->offset == lt->last)
            clz = lz0_cost_last((int)lt->length);
        else
            clz = lz0_cost_new((int)lt->offset, (int)lt->length);
        if (full_hit && cz > clz)
            B++;
        else if (full_hit)
            C++;
    }

    printf("  lz tok  lit=%llu last=%llu new=%llu\n",
           (unsigned long long)n_lz_lit, (unsigned long long)n_lz_last,
           (unsigned long long)n_lz_new);
    printf("  class   A=%llu (%.1f%%) B=%llu C=%llu same=%llu extend=%llu D=%llu zxNew=%llu\n",
           (unsigned long long)A,
           n_zx_new ? 100.0 * (double)A / (double)n_zx_new : 0,
           (unsigned long long)B, (unsigned long long)C,
           (unsigned long long)same, (unsigned long long)extend,
           (unsigned long long)D, (unsigned long long)n_zx_new);
    printf("  finder  full=%llu (%.1f%%) avg_zx_off=%.0f avg_zx_len=%.1f avg_lz_best=%.1f\n",
           (unsigned long long)n_full_in_cand,
           n_zx_new ? 100.0 * (double)n_full_in_cand / (double)n_zx_new : 0,
           n_zx_new ? (double)sum_zx_off / (double)n_zx_new : 0,
           n_zx_new ? (double)sum_zx_len / (double)n_zx_new : 0,
           n_zx_new ? (double)sum_lz_best / (double)n_zx_new : 0);
    printf("  A lens  1=%llu 2=%llu 3-4=%llu 5-8=%llu 9-16=%llu 17-32=%llu 33-64=%llu >64=%llu\n",
           (unsigned long long)A_len[0], (unsigned long long)A_len[1],
           (unsigned long long)A_len[2], (unsigned long long)A_len[3],
           (unsigned long long)A_len[4], (unsigned long long)A_len[5],
           (unsigned long long)A_len[6], (unsigned long long)A_len[7]);
    {
        static const char *dlab[6] = {"1-32", "33-64", "65-128", "129-256", ">256", "nf"};
        printf("  A depth all=%llu 8-15=%llu 16-31=%llu >=32=%llu\n",
               (unsigned long long)A, (unsigned long long)n_l8,
               (unsigned long long)n_l16, (unsigned long long)n_l32);
        for (int i = 0; i < 6; i++) {
            printf("  depth %-7s all=%llu (%.1f%%)  8-15=%llu (%.1f%%)  16-31=%llu (%.1f%%)  >=32=%llu (%.1f%%)\n",
                   dlab[i],
                   (unsigned long long)dep_all[i], A ? 100.0 * (double)dep_all[i] / (double)A : 0,
                   (unsigned long long)dep_l8[i], n_l8 ? 100.0 * (double)dep_l8[i] / (double)n_l8 : 0,
                   (unsigned long long)dep_l16[i], n_l16 ? 100.0 * (double)dep_l16[i] / (double)n_l16 : 0,
                   (unsigned long long)dep_l32[i], n_l32 ? 100.0 * (double)dep_l32[i] / (double)n_l32 : 0);
        }
        printf("  depth found all n=%d avg=%.1f med=%u max=%llu\n",
               nd_all, nd_all ? (double)sum_d_all / (double)nd_all : 0,
               median_u32(d_all, nd_all), (unsigned long long)max_d_all);
        printf("  depth found 16-31 n=%d avg=%.1f med=%u max=%llu\n",
               nd_l16, nd_l16 ? (double)sum_d_l16 / (double)nd_l16 : 0,
               median_u32(d_l16, nd_l16), (unsigned long long)max_d_l16);
    }
    free(d_all);
    free(d_l16);
    lz0_finder_free(&f);
    lz0_toklist_free(&tl);
    free(lz);
    lz0_skip_thresh = 0;
    lz0_chain_noskip = 0;
    lz0_skip_stride = 0;
    lz0_hash4_probe = 0;
    lz0_deep_probe = 0;
    fflush(stdout);
}

int main(int argc, char **argv) {
    size_t n = 256 * 1024;
    const char *kind = "text";
    int chains[16];
    int noskips[16];
    int threshs[16];
    int strides[16];
    int h4s[16];
    int deeps[16];
    int nc = 0;
    int do_zx0 = 1;
    int do_oracle = 0;
    int do_sa = 0;
    int do_hybrid = 0;
    int do_h2_chain = 0;
    int do_h2_jump = 1;
    int cur_ns = 0;
    int cur_t = 0;
    int cur_c = 32;
    int cur_s = 0;
    int cur_d = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "32K"))
            n = 32 * 1024;
        else if (!strcmp(argv[a], "64K"))
            n = 64 * 1024;
        else if (!strcmp(argv[a], "256K"))
            n = 256 * 1024;
        else if (!strcmp(argv[a], "1M"))
            n = 1024 * 1024;
        else if (!strcmp(argv[a], "text") || !strcmp(argv[a], "rep") || !strcmp(argv[a], "bin"))
            kind = argv[a];
        else if (!strncmp(argv[a], "NS=", 3))
            cur_ns = atoi(argv[a] + 3);
        else if (!strncmp(argv[a], "C=", 2))
            cur_c = atoi(argv[a] + 2);
        else if (!strncmp(argv[a], "T=", 2))
            cur_t = atoi(argv[a] + 2);
        else if (!strncmp(argv[a], "S=", 2))
            cur_s = atoi(argv[a] + 2);
        else if (!strncmp(argv[a], "D=", 2))
            cur_d = atoi(argv[a] + 2);
        else if (!strncmp(argv[a], "DP=", 3)) {
            deeps[nc] = atoi(argv[a] + 3);
            h4s[nc] = cur_d;
            strides[nc] = cur_s;
            threshs[nc] = cur_t;
            chains[nc] = cur_c;
            noskips[nc] = cur_ns;
            nc++;
        } else if (!strcmp(argv[a], "--no-zx0"))
            do_zx0 = 0;
        else if (!strcmp(argv[a], "--oracle"))
            do_oracle = 1;
        else if (!strcmp(argv[a], "--no-oracle"))
            do_oracle = 0;
        else if (!strcmp(argv[a], "--sa"))
            do_sa = 1;
        else if (!strcmp(argv[a], "--no-hybrid"))
            do_hybrid = 0;
        else if (!strcmp(argv[a], "--hybrid"))
            do_hybrid = 1;
        else if (!strcmp(argv[a], "--no-h2-chain"))
            do_h2_chain = 0;
        else if (!strcmp(argv[a], "--h2-chain"))
            do_h2_chain = 1;
        else if (!strcmp(argv[a], "--no-h2-jump"))
            do_h2_jump = 0;
        else if (!strcmp(argv[a], "--h2-jump"))
            do_h2_jump = 1;
    }
    if (nc == 0) {
        chains[nc] = 32;
        noskips[nc] = 0;
        threshs[nc] = 0;
        strides[nc] = 0;
        h4s[nc] = 0;
        deeps[nc++] = 0;
        do_oracle = 0;
    }
    lz0_chain_prune = 0;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    gtok *zx = NULL;
    int nz = 0;
    if (do_zx0) {
        printf("=== n=%zu %s ZX0 parse ===\n", n, kind);
        fflush(stdout);
        double t0 = now_ms();
        unsigned char *copy = (unsigned char *)malloc(n);
        if (!copy)
            return 1;
        memcpy(copy, in, n);
        BLOCK *opt = zx0_optimize(copy, (int)n, 0, MAX_OFF_ZX0);
        printf("  zx0_optimize %.1f ms\n", now_ms() - t0);
        fflush(stdout);
        zx = (gtok *)malloc((n + 8) * sizeof(gtok));
        nz = walk_zx0(opt, zx, (int)n + 8);
        free(copy);
        if (nz < 0) {
            printf("  zx0 walk FAIL\n");
            free(zx);
            free(in);
            return 1;
        }
        uint64_t nlit = 0, nlast = 0, nnew = 0;
        for (int i = 0; i < nz; i++) {
            if (!zx[i].offset)
                nlit++;
            else if (zx[i].offset == zx[i].last)
                nlast++;
            else
                nnew++;
        }
        printf("  zx tok  lit=%llu last=%llu new=%llu\n",
               (unsigned long long)nlit, (unsigned long long)nlast,
               (unsigned long long)nnew);
        fflush(stdout);
    }
    if (nc > 0 && (chains[0] != 32 || noskips[0] != 0 || threshs[0] != 0 ||
                   strides[0] != 0 || h4s[0] != 0 || deeps[0] != 0 || nc > 1)) {
        for (int i = 0; i < nc; i++)
            measure_chain(kind, n, in, zx, nz, chains[i], noskips[i], threshs[i], strides[i], h4s[i], deeps[i]);
    }
    if (do_oracle)
        run_deep_oracle(kind, n, in, zx, nz);
    if (do_sa)
        run_sa_proto(kind, n, in, zx, nz);
    if (do_hybrid)
        run_hybrid(kind, n, in, zx, nz);
    if (do_h2_chain)
        run_h2_chain_sweep(kind, n, in, zx, nz);
    if (do_h2_jump)
        run_h2_jump_sweep(kind, n, in, zx, nz);
    free(zx);
    free(in);
    return 0;
}
