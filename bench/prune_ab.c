#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

static int cmp_match(const void *a, const void *b) {
    const lz0_match *x = (const lz0_match *)a;
    const lz0_match *y = (const lz0_match *)b;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    if (x->length != y->length)
        return x->length < y->length ? -1 : 1;
    return 0;
}

static int has_exact(const lz0_match *s, int n, lz0_match m) {
    for (int i = 0; i < n; i++)
        if (s[i].offset == m.offset && s[i].length == m.length)
            return 1;
    return 0;
}

static void set_compare(const unsigned char *in, size_t n, int mode) {
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    cfg.min_match = 2;
    cfg.max_chain = 32;
    cfg.max_candidates = 16;
    cfg.max_states = 2;
    cfg.jump_levels = 16;
    lz0_finder fa, fb;
    lz0_chain_prune = 0;
    if (lz0_finder_init(&fa, in, (uint32_t)n, &cfg) < 0) {
        printf("init fail\n");
        return;
    }
    lz0_chain_prune = mode;
    if (lz0_finder_init(&fb, in, (uint32_t)n, &cfg) < 0) {
        lz0_finder_free(&fa);
        printf("init fail\n");
        return;
    }
    uint64_t npos = 0, mism = 0;
    uint64_t base_n = 0, hit_n = 0;
    uint64_t base2 = 0, hit2 = 0, base3 = 0, hit3 = 0, base4 = 0, hit4 = 0;
    uint64_t only_base = 0, only_pr = 0;
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        lz0_match ca[64], cb[64];
        int la = 0, lb = 0;
        lz0_chain_prune = 0;
        lz0_finder_insert_upto(&fa, i);
        int na = lz0_find_matches(&fa, i, 0, ca, &la);
        lz0_chain_prune = mode;
        lz0_finder_insert_upto(&fb, i);
        int nb = lz0_find_matches(&fb, i, 0, cb, &lb);
        npos++;
        qsort(ca, (size_t)na, sizeof(lz0_match), cmp_match);
        qsort(cb, (size_t)nb, sizeof(lz0_match), cmp_match);
        int same = (na == nb);
        if (same) {
            for (int k = 0; k < na; k++)
                if (ca[k].offset != cb[k].offset || ca[k].length != cb[k].length) {
                    same = 0;
                    break;
                }
        }
        if (!same)
            mism++;
        for (int k = 0; k < na; k++) {
            base_n++;
            uint32_t L = ca[k].length;
            int hit = has_exact(cb, nb, ca[k]);
            if (L == 2) {
                base2++;
                if (hit)
                    hit2++;
            } else if (L == 3) {
                base3++;
                if (hit)
                    hit3++;
            } else {
                base4++;
                if (hit)
                    hit4++;
            }
            if (hit)
                hit_n++;
            else
                only_base++;
        }
        for (int k = 0; k < nb; k++)
            if (!has_exact(ca, na, cb[k]))
                only_pr++;
    }
    lz0_chain_prune = 0;
    lz0_finder_free(&fa);
    lz0_finder_free(&fb);
    printf("  set  pos=%llu mismatch=%llu recall=%.4f r2=%.4f r3=%.4f r4=%.4f only_base=%llu only_pr=%llu\n",
           (unsigned long long)npos, (unsigned long long)mism,
           base_n ? (double)hit_n / (double)base_n : 1.0,
           base2 ? (double)hit2 / (double)base2 : 1.0,
           base3 ? (double)hit3 / (double)base3 : 1.0,
           base4 ? (double)hit4 / (double)base4 : 1.0,
           (unsigned long long)only_base, (unsigned long long)only_pr);
    fflush(stdout);
}

static void compress_one(const char *kind, size_t n, const unsigned char *in, int mode) {
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    cfg.min_match = 2;
    cfg.max_chain = 32;
    cfg.max_candidates = 16;
    cfg.max_states = 2;
    cfg.jump_levels = 16;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census cen;
    memset(&cen, 0, sizeof(cen));
    lz0_sdp_census_out = &cen;
    lz0_chain_prune = mode;
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    lz0_chain_prune = 0;
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        printf("  P=%d FAIL\n", mode);
        return;
    }
    printf("  P=%d csz=%zu ratio=%.2f%% parse=%.1f find=%.1f visit=%llu check=%llu matchlen=%llu cand=%llu\n",
           mode, csz, 100.0 * (double)csz / (double)n, st.parse_ms, cen.ms_find,
           (unsigned long long)cen.n_chain_visit, (unsigned long long)cen.n_chain_check,
           (unsigned long long)cen.n_matchlen, (unsigned long long)cen.n_cand);
    free(comp);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int do_32 = 1, do_256 = 1, do_1m = 0;
    int modes[3];
    int nm = 0;
    int want_text = 1, want_rep = 1, want_bin = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "1M"))
            do_1m = 1;
        else if (!strcmp(argv[a], "--no-32"))
            do_32 = 0;
        else if (!strcmp(argv[a], "--no-256"))
            do_256 = 0;
        else if (!strcmp(argv[a], "P=1"))
            modes[nm++] = 1;
        else if (!strcmp(argv[a], "P=2"))
            modes[nm++] = 2;
        else if (!strcmp(argv[a], "P=3"))
            modes[nm++] = 3;
        else if (!strcmp(argv[a], "P=4"))
            modes[nm++] = 4;
        else if (!strcmp(argv[a], "text")) {
            want_rep = 0;
            want_bin = 0;
        } else if (!strcmp(argv[a], "rep")) {
            want_text = 0;
            want_bin = 0;
        } else if (!strcmp(argv[a], "bin")) {
            want_text = 0;
            want_rep = 0;
        }
    }
    if (nm == 0) {
        modes[0] = 1;
        modes[1] = 2;
        nm = 2;
    }
    const char *kinds[3];
    int nk = 0;
    if (want_text)
        kinds[nk++] = "text";
    if (want_rep)
        kinds[nk++] = "rep";
    if (want_bin)
        kinds[nk++] = "bin";
    size_t szs[4];
    int ns = 0;
    if (do_32)
        szs[ns++] = 32 * 1024;
    if (do_256)
        szs[ns++] = 256 * 1024;
    if (do_1m)
        szs[ns++] = 1024 * 1024;
    for (int si = 0; si < ns; si++) {
        for (int k = 0; k < nk; k++) {
            unsigned char *in = make_corpus(kinds[k], szs[si]);
            if (!in)
                return 1;
            printf("=== n=%zu %s ===\n", szs[si], kinds[k]);
            compress_one(kinds[k], szs[si], in, 0);
            for (int mi = 0; mi < nm; mi++) {
                compress_one(kinds[k], szs[si], in, modes[mi]);
                if (szs[si] <= 256 * 1024)
                    set_compare(in, szs[si], modes[mi]);
            }
            free(in);
        }
    }
    return 0;
}
