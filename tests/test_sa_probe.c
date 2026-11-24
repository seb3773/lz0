/* Mission 5A: exact SA candidates, census and guard-page regressions.
 * Compile WITHOUT a separate lz0_find.c (included for static kernel access).
 * The reference kernels below are frozen from 6d5c0f4, not the new path.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../lz0/lz0_find.c"

static int sa_ref_side(lz0_finder *f, uint32_t pos, uint32_t src, int known,
                         int minm, int maxl, int *best_len, lz0_match *out, int *nout,
                         lz0_sdp_census *cen) {
    const unsigned char *in = f->in;
    int ml;
    if (known >= maxl)
        ml = maxl;
    else if (known > 0)
        ml = known + lz0_simd_matchlen(in + pos + known, in + src + known, maxl - known);
    else
        ml = lz0_simd_matchlen(in + pos, in + src, maxl);
    if (cen) {
        census_ml(cen, ml);
        cen->n_chain_check++;
        cen->n_sa_ml++;
        cen->sum_sa_ml += (uint64_t)ml;
        if ((uint64_t)ml > cen->max_sa_ml)
            cen->max_sa_ml = (uint64_t)ml;
        cen->n_sa_ml_bytes += (uint64_t)(ml > known ? ml - known : 0);
        if (ml >= minm)
            cen->n_sa_ml_ok++;
    }
    if (ml >= minm) {
        lz0_match m;
        m.offset = pos - src;
        m.length = (uint32_t)ml;
        if (ml > *best_len)
            *best_len = ml;
        if (cen) {
            int had = already_has_offset(out, *nout, m.offset);
            int kept = keep_candidate(f, out, nout, f->max_candidates, m);
            if (had)
                cen->n_sa_dup++;
            else if (kept)
                cen->n_sa_new_kept++;
            else
                cen->n_sa_new_drop++;
        } else {
            keep_candidate(f, out, nout, f->max_candidates, m);
        }
    }
    return ml;
}

static LZ0_ALWAYS_INLINE void sa_ref_one(lz0_finder *f, uint32_t pos, int rk, int lcp_ix,
                                           int *acc, int use_lcp, int minm, int maxl,
                                           int *best_len, lz0_match *out, int *nout,
                                           lz0_sdp_census *cen, const int with_cen) {
    uint32_t src = f->sa[rk];
    if (with_cen)
        cen->n_sa_cmp++;
    if (use_lcp) {
        int v = (int)f->lcp[lcp_ix];
        if (v < *acc)
            *acc = v;
    }
    if (src < pos) {
        int known = 0;
        if (use_lcp) {
            known = *acc;
            if (known < 0)
                known = 0;
            if (known > maxl)
                known = maxl;
        }
        sa_ref_side(f, pos, src, known, minm, maxl, best_len, out, nout,
                      with_cen ? cen : NULL);
    } else if (with_cen) {
        cen->n_sa_src_skip++;
    }
}

/* with_cen and with_pf are compile-time constants at every call site: the
 * census-free path carries no census branches, and inputs below
 * LZ0_SA_PF_MIN_INPUT run a loop with no prefetch code at all. */
static LZ0_ALWAYS_INLINE void sa_ref_core(lz0_finder *f, uint32_t pos, int minm, int maxl,
                                            int *best_len, lz0_match *out, int *nout,
                                            lz0_sdp_census *cen, const int with_cen,
                                            const int with_pf) {
    (void)with_pf; /* unused when LZ0_SA_PF_K == 0 */
    uint32_t n = f->n;
    int win = f->sa_win;
    uint32_t qpos = pos;
    if (f->sa_sample > 1) {
        uint32_t rem = pos % (uint32_t)f->sa_sample;
        if (rem)
            qpos = pos - rem;
    }
    uint32_t rr = (qpos < n) ? f->rank[qpos] : 0xffffffffu;
    if (rr == 0xffffffffu || rr >= f->sa_n)
        return;
    int r = (int)rr;
    int ns = (int)f->sa_n;
    int accL = INT_MAX;
    int accR = INT_MAX;
    int use_lcp = (lz0_sa_lcp && f->lcp && f->sa_sample <= 1);
    int adapt = lz0_sa_adapt;
    int ms = lz0_sa_multiscale;
    int stop_at = 0;
    if (with_cen)
        cen->n_sa_invoke++;
    int max_d = win;
    if (ms == 1 && max_d < 512)
        max_d = 512;
    if (ms == 2 && max_d < 1024)
        max_d = 1024;

    int d = 1;
#if LZ0_SA_PF_K > 0
    const uint32_t *sa = f->sa;
    const unsigned char *in = f->in;
    const uint32_t hot = pos < n ? pos : 0; /* every prefetch stays in [0, n) */
    /* last distance of the unit-stride part of the schedule */
    int unit_d = ms == 0 ? max_d : ms == 1 ? 128 : 48;
    if (unit_d > max_d)
        unit_d = max_d;
    /* with adapt, the probe may stop right after d = 4 or d = 8: run ahead
     * only up to the next checkpoint until that stop is ruled out */
    int pf_lim = (adapt && unit_d > 4) ? 4 : unit_d;
    /* prologue: distances 2 .. K (d = 1 is used immediately) */
    if (with_pf)
        for (int k = 2; k <= LZ0_SA_PF_K && k <= pf_lim; k++)
            sa_pf_payload(sa, in, hot, r, ns, k);
#endif

    while (d <= max_d) {
#if LZ0_SA_PF_K > 0
        if (with_pf && d + LZ0_SA_PF_K <= pf_lim)
            sa_pf_payload(sa, in, hot, r, ns, d + LZ0_SA_PF_K);
#endif
        if (r - d >= 0)
            sa_ref_one(f, pos, r - d, r - d + 1, &accL, use_lcp, minm, maxl, best_len, out,
                         nout, cen, with_cen);
        if (r + d < ns)
            sa_ref_one(f, pos, r + d, r + d, &accR, use_lcp, minm, maxl, best_len, out, nout,
                         cen, with_cen);
        if (adapt && (d == 4 || d == 8)) {
            if (*best_len >= 16) {
                stop_at = d;
                break;
            }
#if LZ0_SA_PF_K > 0
            if (with_pf) {
                /* no early stop: widen the run-ahead to the next checkpoint
                 * and catch up on the distances skipped meanwhile */
                int lim = (d == 4 && unit_d > 8) ? 8 : unit_d;
                for (int k = pf_lim + 1; k <= d + LZ0_SA_PF_K && k <= lim; k++)
                    sa_pf_payload(sa, in, hot, r, ns, k);
                pf_lim = lim;
            }
#endif
        }
        d = sa_next_d(d, ms);
    }
    if (with_cen) {
        if (stop_at == 4)
            cen->n_sa_stop4++;
        else if (stop_at == 8)
            cen->n_sa_stop8++;
        else
            cen->n_sa_stop16++;
    }
}


static void reset_candidates(lz0_finder *f) {
    memset(f->kc_off, 0, sizeof(f->kc_off));
    memset(f->kc_score, 0, sizeof(f->kc_score));
    f->kc_worst = -1;
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    assert(page >= 256);
    unsigned char *map = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(map != MAP_FAILED);
    assert(mprotect(map + page, (size_t)page, PROT_NONE) == 0);
    unsigned char *in = map + page - 256;
    memset(in, 'q', 256);
    memcpy(in, "abcX....abcY....abcZ", 20);
    in[254] = 'a'; in[255] = 'b';
    lz0_finder f = {0};
    f.in = in; f.n = 256; f.max_candidates = 64; f.min_match = 2;
    lz0_len2_max_off = 0; /* This fixture admits every nonzero length-2 offset. */
    lz0_match out[64] = {{0}};
    int count = 0, best = 16;

    /* Rule: a prefix mismatch can never add a length >= 2 candidate. */
    assert(sa_probe_side(&f, 16, 3, 0, 2, 8, &best, out, &count, NULL) == 0);
    assert(count == 0 && best == 16);
    /* Rule: inability to beat best_len does NOT justify dropping a source. */
    assert(sa_probe_side(&f, 16, 0, 0, 2, 8, &best, out, &count, NULL) == 3);
    assert(count == 1 && out[0].offset == 16 && out[0].length == 3 && best == 16);

    /* Rule: left then right, including shorter matches and duplicate updates. */
    uint32_t ranks[2] = {0, 8};
    f.sa = ranks;
    reset_candidates(&f); count = 0;
    sa_probe_pair(&f, 16, 0, 1, lz0_ld16(in + 16), 2, 8, &best, out, &count);
    assert(count == 2 && out[0].offset == 16 && out[0].length == 3);
    assert(out[1].offset == 8 && out[1].length == 3 && best == 16);

    /* Rule: a future suffix at n-1 must not get a 16-bit load (guard page). */
    ranks[0] = 255; ranks[1] = 0;
    reset_candidates(&f); count = 0; best = 1;
    sa_probe_pair(&f, 254, 0, 1, lz0_ld16(in + 254), 2, 2, &best, out, &count);
    assert(count == 1 && out[0].offset == 254 && out[0].length == 2 && best == 2);

    /* Rule: census still measures one-byte matches rejected by the prefix. */
    lz0_sdp_census cen = {0};
    in[8] = 'a'; in[9] = 'd';
    reset_candidates(&f); count = 0; best = 1;
    assert(sa_probe_side(&f, 16, 8, 0, 2, 8, &best, out, &count, &cen) == 1);
    assert(count == 0 && cen.n_sa_ml == 1 && cen.sum_sa_ml == 1);
    assert(cen.n_sa_ml_bytes == 1 && cen.n_sa_ml_ok == 0 && cen.n_chain_check == 1);

    unsigned long checks = 0;
    for (int pattern = 0; pattern < 4; pattern++) {
        uint32_t seed = 0x12345678u;
        for (int i = 0; i < 256; i++) {
            seed = seed * 1664525u + 1013904223u;
            in[i] = pattern == 0 ? (unsigned char)(seed >> 24) :
                    pattern == 1 ? (unsigned char)('a' + (i % 7)) :
                    pattern == 2 ? (unsigned char)('a' + ((seed >> 24) & 3u)) : 'x';
        }
        for (int level = 1; level <= 9; level++)
        for (int lcp = 0; lcp <= 1; lcp++)
        for (int sample = 1; sample <= 3; sample += 2)
        for (int adapt = 0; adapt <= 1; adapt++)
        for (int ms = 0; ms <= 2; ms++) {
            lz0_config cfg;
            lz0_config_level(&cfg, level);
            lz0_sa_lcp = lcp; lz0_sa_sample = sample;
            lz0_sa_adapt = adapt; lz0_sa_multiscale = ms;
            lz0_finder opt, ref;
            assert(lz0_finder_init(&opt, in, 256, &cfg) == 0);
            assert(lz0_finder_init(&ref, in, 256, &cfg) == 0);
            for (uint32_t pos = 0; pos < 255; pos++)
            for (int census = 0; census <= 1; census++) {
                lz0_match a[64] = {{0}}, b[64] = {{0}};
                int na = 0, nb = 0, ba = (pos & 1) ? 16 : 1, bb = ba;
                lz0_sdp_census ca = {0}, cb = {0};
                reset_candidates(&opt); reset_candidates(&ref);
                sa_probe_core(&opt, pos, 2, (int)(256 - pos), &ba, a, &na,
                              census ? &ca : NULL, census, 0);
                sa_ref_core(&ref, pos, 2, (int)(256 - pos), &bb, b, &nb,
                            census ? &cb : NULL, census, 0);
                assert(na == nb && ba == bb);
                for (int k = 0; k < na; k++) {
                    assert(a[k].offset == b[k].offset && a[k].length == b[k].length);
                }
                assert(memcmp(&ca, &cb, sizeof(ca)) == 0);
                checks++;
            }
            lz0_finder_free(&opt); lz0_finder_free(&ref);
        }
    }
    assert(munmap(map, (size_t)page * 2) == 0);
    printf("SA probe regressions: 6 targeted rules + %lu exact differential cases OK\n", checks);
    return 0;
}
