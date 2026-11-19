#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

static double rdtsc_ms(void) {
    return (double)__builtin_ia32_rdtsc() / 2.5e6;
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

static void cfg_h2c8(lz0_config *cfg) {
    lz0_config_default(cfg, LZ0_PARSE_SDP);
    cfg->min_match = 2;
    cfg->max_chain = 8;
    cfg->max_candidates = 16;
    cfg->max_states = 2;
    cfg->jump_levels = 0;
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
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
}

typedef struct {
    uint32_t last_off;
    int cost;
    uint32_t prev_pos;
    uint32_t tok_len;
    uint32_t tok_off;
    uint8_t prev_slot;
    uint8_t can_last;
} fake_st;

static inline void fake_store(fake_st *d, uint32_t last_off, int can_last, int cost,
                              uint32_t prev_pos, uint8_t prev_slot, uint32_t tok_len,
                              uint32_t tok_off) {
    d->last_off = last_off;
    d->can_last = (uint8_t)can_last;
    d->cost = cost;
    d->prev_pos = prev_pos;
    d->prev_slot = prev_slot;
    d->tok_len = tok_len;
    d->tok_off = tok_off;
}

static inline void fake_relax2(fake_st *row, uint8_t *nslot, uint32_t last_off,
                               int can_last, int cost, uint32_t prev_pos, uint8_t prev_slot,
                               uint32_t tok_len, uint32_t tok_off) {
    uint8_t n = *nslot;
    if (n > 0 && row[0].last_off == last_off && row[0].can_last == can_last) {
        if (cost < row[0].cost)
            fake_store(&row[0], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                       tok_off);
        return;
    }
    if (n > 1 && row[1].last_off == last_off && row[1].can_last == can_last) {
        if (cost < row[1].cost)
            fake_store(&row[1], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                       tok_off);
        return;
    }
    if (n < 2) {
        fake_store(&row[n], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
        *nslot = (uint8_t)(n + 1);
        return;
    }
    int w = row[1].cost > row[0].cost ? 1 : 0;
    if (cost < row[w].cost)
        fake_store(&row[w], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
}

static inline int elias_bits(int value) {
    if (value <= 0)
        return 1;
    return ((32 - __builtin_clz((unsigned)value)) << 1) - 1;
}

static inline void emit_one(fake_st *st, uint8_t *ns, uint32_t i, int end, uint32_t off,
                            int cc) {
    fake_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], off, 0, cc, i,
                0, (uint32_t)end, off);
}

static void emit_full(fake_st *st, uint8_t *ns, uint32_t i, int L, uint32_t off, int cost) {
    if (L < 2)
        return;
    int cbase = cost + 10 + elias_bits((((int)off - 1) >> 8) + 1);
    emit_one(st, ns, i, L < 8 ? L : 8, off, cbase + 3);
    if (L < 9)
        return;
    emit_one(st, ns, i, 9, off, cbase + 4);
    if (L < 10)
        return;
    static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                             1031, 2055, 4103, 8199,  16391, 32775, 65543};
    int cl = 6;
    int need = 10;
    for (int p = 0; p < 15 && L >= need; p++) {
        int h = hi[p];
        int end = L < h ? L : h;
        emit_one(st, ns, i, end, off, cbase + cl);
        if (L <= h)
            return;
        cl += 2;
        need = h + 1;
    }
    while (need <= L) {
        int end = lz0_next_plateau_new(need, L);
        emit_one(st, ns, i, end, off, cost + lz0_cost_new((int)off, end));
        need = end + 1;
    }
}

static int plat_only(int L, volatile uint64_t *sink) {
    if (L < 2)
        return 0;
    int n = 1;
    *sink += (uint64_t)(unsigned)(L < 8 ? L : 8);
    if (L < 9)
        return n;
    n++;
    *sink += 9;
    if (L < 10)
        return n;
    static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                             1031, 2055, 4103, 8199,  16391, 32775, 65543};
    int need = 10;
    for (int p = 0; p < 15 && L >= need; p++) {
        int h = hi[p];
        int end = L < h ? L : h;
        *sink += (uint64_t)(unsigned)end;
        n++;
        if (L <= h)
            return n;
        need = h + 1;
    }
    while (need <= L) {
        int end = lz0_next_plateau_new(need, L);
        *sink += (uint64_t)(unsigned)end;
        n++;
        need = end + 1;
    }
    return n;
}

static void emit_relax_only(fake_st *st, uint8_t *ns, uint32_t i, int L, uint32_t off,
                            int cost) {
    if (L < 2)
        return;
    int cbase = cost;
    emit_one(st, ns, i, L < 8 ? L : 8, off, cbase);
    if (L < 9)
        return;
    emit_one(st, ns, i, 9, off, cbase);
    if (L < 10)
        return;
    static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                             1031, 2055, 4103, 8199,  16391, 32775, 65543};
    int need = 10;
    for (int p = 0; p < 15 && L >= need; p++) {
        int h = hi[p];
        int end = L < h ? L : h;
        emit_one(st, ns, i, end, off, cbase);
        if (L <= h)
            return;
        need = h + 1;
    }
    while (need <= L) {
        int end = lz0_next_plateau_new(need, L);
        emit_one(st, ns, i, end, off, cbase);
        need = end + 1;
    }
}

static void emit_last_full(fake_st *st, uint8_t *ns, uint32_t i, int L, uint32_t last_off,
                           int cost) {
    int start = 1;
    while (start <= L) {
        int end = lz0_next_plateau_last(start, L);
        fake_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], last_off, 0,
                    cost + lz0_cost_last(end), i, 0, (uint32_t)end, last_off);
        start = end + 1;
    }
}

typedef struct {
    uint32_t pos;
    uint32_t off;
    uint16_t L;
} emit_rec;

typedef struct {
    uint32_t pos;
    uint32_t last_off;
    uint16_t L;
} last_rec;

static void run_one(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return;
    lz0_config cfg;
    cfg_h2c8(&cfg);

    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census cen;
    memset(&cen, 0, sizeof(cen));
    lz0_sdp_census_out = &cen;
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        printf("=== %s n=%zu e2e FAIL ===\n", kind, n);
        free(in);
        return;
    }
    double cpu_enc = cen.ms_sa_build + cen.ms_insert + cen.ms_find + cen.ms_trans + cen.ms_back;
    double trans = cen.ms_trans;
    uint64_t nplat = cen.n_new_plat_fixed + cen.n_new_plat_hi + cen.n_new_plat_ovf;
    printf("=== %s n=%zu e2e csz=%zu bits=%llu tok new=%u last=%u lit=%u enc=%.1f cpu=%.1f ===\n",
           kind, n, csz, (unsigned long long)st.bits_total, st.n_new, st.n_last, st.n_literals,
           st.total_ms, cpu_enc);
    printf("  e2e  trans=%.1f  lit=%.1f  lastlen=%.1f  last=%.1f  new=%.1f\n",
           trans, cen.ms_trans_lit, cen.ms_trans_lastlen, cen.ms_trans_last, cen.ms_trans_new);
    printf("  pctE lit=%.1f lastlen=%.1f last=%.1f new=%.1f  trans=%.1f\n",
           cpu_enc > 0 ? 100.0 * cen.ms_trans_lit / cpu_enc : 0,
           cpu_enc > 0 ? 100.0 * cen.ms_trans_lastlen / cpu_enc : 0,
           cpu_enc > 0 ? 100.0 * cen.ms_trans_last / cpu_enc : 0,
           cpu_enc > 0 ? 100.0 * cen.ms_trans_new / cpu_enc : 0,
           cpu_enc > 0 ? 100.0 * trans / cpu_enc : 0);
    printf("  pctT lit=%.1f lastlen=%.1f last=%.1f new=%.1f\n",
           trans > 0 ? 100.0 * cen.ms_trans_lit / trans : 0,
           trans > 0 ? 100.0 * cen.ms_trans_lastlen / trans : 0,
           trans > 0 ? 100.0 * cen.ms_trans_last / trans : 0,
           trans > 0 ? 100.0 * cen.ms_trans_new / trans : 0);
    printf("  cnt  n_lit=%llu n_lastlen=%llu n_last_plat=%llu n_emit_new=%llu n_trans_new=%llu "
           "skip=%llu avg_st=%.2f\n",
           (unsigned long long)cen.n_trans_lit, (unsigned long long)cen.n_last_len,
           (unsigned long long)cen.n_last_plat, (unsigned long long)cen.n_emit_new,
           (unsigned long long)cen.n_trans_new, (unsigned long long)cen.n_skip_off,
           cen.n_pos ? (double)cen.sum_states / (double)cen.n_pos : 0);
    printf("  plat fixed=%llu hi=%llu ovf=%llu tot=%llu avg/emit=%.2f\n",
           (unsigned long long)cen.n_new_plat_fixed, (unsigned long long)cen.n_new_plat_hi,
           (unsigned long long)cen.n_new_plat_ovf, (unsigned long long)nplat,
           cen.n_emit_new ? (double)nplat / (double)cen.n_emit_new : 0);
    free(comp);

    lz0_finder f;
    cfg_h2c8(&cfg);
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) != 0) {
        free(in);
        return;
    }

    uint32_t cap = (uint32_t)n * 16u;
    if (cap < 1u << 20)
        cap = 1u << 20;
    emit_rec *recs = (emit_rec *)malloc((size_t)cap * sizeof(emit_rec));
    last_rec *lrecs = (last_rec *)malloc((size_t)n * sizeof(last_rec));
    if (!recs || !lrecs) {
        printf("  isolated ALLOC FAIL\n");
        lz0_finder_free(&f);
        free(recs);
        free(lrecs);
        free(in);
        return;
    }
    uint32_t nr = 0, nl = 0, n_trunc = 0;
    uint64_t n_skip = 0, n_cand = 0;
    lz0_match cands[64];
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        lz0_finder_insert_upto(&f, i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(&f, i, 0, cands, &dummy);
        n_cand += (uint64_t)nc;
        uint32_t last_off = 1;
        if (nc > 0 && cands[0].offset)
            last_off = cands[0].offset;
        int Ll = lz0_find_last_len(&f, i, last_off);
        if (Ll > (int)((uint32_t)n - i))
            Ll = (int)((uint32_t)n - i);
        if (nl < (uint32_t)n) {
            lrecs[nl].pos = i;
            lrecs[nl].last_off = last_off;
            lrecs[nl].L = (uint16_t)(Ll > 65535 ? 65535 : (Ll < 0 ? 0 : Ll));
            nl++;
        }
        for (int k = 0; k < nc; k++) {
            int L = (int)cands[k].length;
            uint32_t off = cands[k].offset;
            if (L < 2 || off == 0) {
                n_skip++;
                continue;
            }
            if (nr + 2 >= cap) {
                uint32_t nc2 = cap * 2u;
                emit_rec *np = (emit_rec *)realloc(recs, (size_t)nc2 * sizeof(emit_rec));
                if (!np) {
                    n_trunc++;
                    break;
                }
                recs = np;
                cap = nc2;
            }
            recs[nr].pos = i;
            recs[nr].off = off;
            recs[nr].L = (uint16_t)(L > 65535 ? 65535 : L);
            nr++;
            recs[nr].pos = i;
            recs[nr].off = off;
            recs[nr].L = recs[nr - 1].L;
            nr++;
        }
    }
    lz0_finder_free(&f);

    fake_st *dst = (fake_st *)calloc((size_t)(n + 1) * 2u, sizeof(fake_st));
    uint8_t *ns = (uint8_t *)calloc((size_t)(n + 1), 1);
    if (!dst || !ns) {
        printf("  isolated DP ALLOC FAIL nr=%u\n", nr);
        free(recs);
        free(lrecs);
        free(dst);
        free(ns);
        free(in);
        return;
    }

    volatile uint64_t sink = 0;
    double t0, t1;

    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < nr; i++)
        sink += (uint64_t)plat_only((int)recs[i].L, &sink);
    t1 = rdtsc_ms();
    double ms_plat = t1 - t0;

    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < nr; i++) {
        uint32_t off = recs[i].off;
        sink += (uint64_t)(unsigned)(10 + elias_bits((((int)off - 1) >> 8) + 1));
    }
    t1 = rdtsc_ms();
    double ms_cbase = t1 - t0;

    memset(dst, 0, (size_t)(n + 1) * 2u * sizeof(fake_st));
    memset(ns, 0, (size_t)(n + 1));
    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < nr; i++)
        emit_relax_only(dst, ns, recs[i].pos, (int)recs[i].L, recs[i].off, 100);
    t1 = rdtsc_ms();
    double ms_relax = t1 - t0;
    sink += ns[1];

    memset(dst, 0, (size_t)(n + 1) * 2u * sizeof(fake_st));
    memset(ns, 0, (size_t)(n + 1));
    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < nr; i++)
        emit_full(dst, ns, recs[i].pos, (int)recs[i].L, recs[i].off, 100);
    t1 = rdtsc_ms();
    double ms_full = t1 - t0;
    sink += dst[0].cost;

    memset(dst, 0, (size_t)(n + 1) * 2u * sizeof(fake_st));
    memset(ns, 0, (size_t)(n + 1));
    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < nl; i++) {
        if (lrecs[i].L >= 1)
            emit_last_full(dst, ns, lrecs[i].pos, (int)lrecs[i].L, lrecs[i].last_off, 100);
    }
    t1 = rdtsc_ms();
    double ms_last_emit = t1 - t0;
    sink += ns[2];

    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < nl; i++) {
        fake_relax2(dst + (size_t)(lrecs[i].pos + 1) * 2u, &ns[lrecs[i].pos + 1],
                    lrecs[i].last_off, 1, 100 + 9, lrecs[i].pos, 0, 1, 0);
    }
    t1 = rdtsc_ms();
    double ms_lit = t1 - t0;
    sink += ns[3];

    double ms_lastlen_walk = 0;

    double ms_body = ms_full - ms_plat;
    if (ms_body < 0)
        ms_body = 0;
    double scale = (nr && cen.n_emit_new) ? (double)cen.n_emit_new / (double)nr : 1.0;
    (void)scale;

    printf("  isolated recs=%u (e2e emit_new=%llu) last_recs=%u cand=%llu skip=%llu trunc=%u sink=%llu\n",
           nr, (unsigned long long)cen.n_emit_new, nl, (unsigned long long)n_cand,
           (unsigned long long)n_skip, n_trunc, (unsigned long long)sink);
    printf("  iso   plat=%.1f  cbase=%.1f  relax=%.1f  full_new=%.1f  body(full-plat)=%.1f\n",
           ms_plat, ms_cbase, ms_relax, ms_full, ms_body);
    printf("  iso   lit=%.1f  last_emit=%.1f  lastlen_walk=%.1f\n", ms_lit, ms_last_emit,
           ms_lastlen_walk);

    double iso_new = ms_full > 0 ? ms_full : 1;
    printf("  iso%%new plat=%.1f cbase=%.1f relax=%.1f body=%.1f\n",
           100.0 * ms_plat / iso_new, 100.0 * ms_cbase / iso_new, 100.0 * ms_relax / iso_new,
           100.0 * ms_body / iso_new);

    double e2e_new = cen.ms_trans_new > 0 ? cen.ms_trans_new : 1;
    double plat_e2e = e2e_new * (ms_full > 0 ? ms_plat / ms_full : 0);
    double body_e2e = e2e_new * (ms_full > 0 ? ms_body / ms_full : 0);
    double relax_e2e = e2e_new * (ms_full > 0 ? ms_relax / ms_full : 0);
    double cbase_e2e = e2e_new * (ms_full > 0 ? ms_cbase / ms_full : 0);
    printf("  attr  plat=%.1f (%.1f%%enc %.1f%%trans %.1f%%new)\n",
           plat_e2e, cpu_enc > 0 ? 100.0 * plat_e2e / cpu_enc : 0,
           trans > 0 ? 100.0 * plat_e2e / trans : 0, 100.0 * plat_e2e / e2e_new);
    printf("  attr  emit-body=%.1f (%.1f%%enc %.1f%%trans %.1f%%new)  [relax~%.1f cbase~%.1f]\n",
           body_e2e, cpu_enc > 0 ? 100.0 * body_e2e / cpu_enc : 0,
           trans > 0 ? 100.0 * body_e2e / trans : 0, 100.0 * body_e2e / e2e_new, relax_e2e,
           cbase_e2e);
    printf("  attr  lit=%.1f (%.1f%%enc) lastlen=%.1f (%.1f%%enc) last=%.1f (%.1f%%enc)\n",
           cen.ms_trans_lit, cpu_enc > 0 ? 100.0 * cen.ms_trans_lit / cpu_enc : 0,
           cen.ms_trans_lastlen, cpu_enc > 0 ? 100.0 * cen.ms_trans_lastlen / cpu_enc : 0,
           cen.ms_trans_last, cpu_enc > 0 ? 100.0 * cen.ms_trans_last / cpu_enc : 0);
    printf("  ns    emit=%.2f trans_new=%.2f plat_iter=%.2f relax_call=%.2f lastlen=%.2f\n",
           cen.n_emit_new ? 1e6 * cen.ms_trans_new / (double)cen.n_emit_new : 0,
           cen.n_trans_new ? 1e6 * cen.ms_trans_new / (double)cen.n_trans_new : 0,
           nr ? 1e6 * ms_plat / (double)nr : 0,
           nplat ? 1e6 * ms_relax / (double)(nplat ? nplat : 1) : 0,
           cen.n_last_len ? 1e6 * cen.ms_trans_lastlen / (double)cen.n_last_len : 0);

    free(dst);
    free(ns);
    free(recs);
    free(lrecs);
    free(in);
}

int main(int argc, char **argv) {
    size_t n = 1024 * 1024;
    const char *kind = "text";
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "1M"))
            n = 1024 * 1024;
        else if (!strcmp(argv[a], "2M"))
            n = 2 * 1024 * 1024;
        else if (!strcmp(argv[a], "5M"))
            n = 5 * 1024 * 1024;
        else if (!strcmp(argv[a], "text") || !strcmp(argv[a], "bin") || !strcmp(argv[a], "rep"))
            kind = argv[a];
    }
    run_one(kind, n);
    return 0;
}
