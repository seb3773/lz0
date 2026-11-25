#include "lz0.h"
#include "lz0_internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *make_rep(size_t n) {
    unsigned char *p = (unsigned char *)malloc(n);
    if (!p)
        return NULL;
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)((i / 17) & 0xff);
    for (size_t i = n / 4; i + n / 8 < n && i < n; i += n / 7) {
        size_t L = n / 8 < n - i ? n / 8 : n - i;
        memcpy(p + i, p, L);
    }
    return p;
}

static int has_offset(const lz0_match *cands, int nc, uint32_t off) {
    for (int k = 0; k < nc; k++)
        if (cands[k].offset == off)
            return 1;
    return 0;
}

static void best_new_of(const lz0_match *cands, int nc, uint32_t last_off,
                        uint32_t *off, int *len, int *cost, int *sav) {
    *off = 0;
    *len = 0;
    *cost = INT_MAX / 4;
    *sav = -1;
    for (int k = 0; k < nc; k++) {
        if (cands[k].offset == 0 || cands[k].offset == last_off)
            continue;
        int L = (int)cands[k].length;
        if (L < 2)
            continue;
        int start = 2;
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            int c = lz0_cost_new((int)cands[k].offset, end);
            int s = (end << 3) - c;
            if (s > *sav || (s == *sav && end > *len)) {
                *off = cands[k].offset;
                *len = end;
                *cost = c;
                *sav = s;
            }
            start = end + 1;
        }
    }
}

static void best_last_of(int last_len, int *len, int *cost, int *sav) {
    *len = 0;
    *cost = INT_MAX / 4;
    *sav = -1;
    if (last_len < 1)
        return;
    int start = 1;
    while (start <= last_len) {
        int end = lz0_next_plateau_last(start, last_len);
        int c = lz0_cost_last(end);
        int s = (end << 3) - c;
        if (s > *sav || (s == *sav && end > *len)) {
            *len = end;
            *cost = c;
            *sav = s;
        }
        start = end + 1;
    }
}

static void query_fresh(const unsigned char *p, uint32_t n, const lz0_config *cfg,
                        uint32_t pos, uint32_t last_off, const char *tag) {
    lz0_finder f;
    if (lz0_finder_init(&f, p, n, cfg) < 0)
        return;
    lz0_finder_insert_upto(&f, pos);
    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(&f, pos, last_off, cands, &last_len);
    int ll = 0, lc = 0, ls = -1;
    best_last_of(last_len, &ll, &lc, &ls);
    uint32_t noff = 0;
    int nlen = 0, ncost = 0, nsav = -1;
    best_new_of(cands, nc, last_off, &noff, &nlen, &ncost, &nsav);
    int h17 = has_offset(cands, nc, 17);
    printf("%s pos=%u last_off=%u last_raw=%d LAST len=%d cost=%d sav=%d "
           "NEW off=%u len=%d cost=%d sav=%d nc=%d has17=%d\n",
           tag, pos, last_off, last_len, ll, lc, ls, noff, nlen, ncost, nsav, nc, h17);
    int shown = 0;
    for (int k = 0; k < nc && shown < 12; k++) {
        printf("  cand off=%u len=%u cost_new=%d\n", cands[k].offset, cands[k].length,
               lz0_cost_new((int)cands[k].offset, (int)cands[k].length));
        shown++;
    }
    lz0_finder_free(&f);
}

static void dump_chain(const unsigned char *p, uint32_t n, int chain, int log_limit) {
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_GREEDY);
    cfg.max_chain = chain;
    cfg.max_candidates = 16;

    lz0_finder f;
    if (lz0_finder_init(&f, p, n, &cfg) < 0) {
        printf("finder init fail chain=%d\n", chain);
        return;
    }

    lz0_toklist tl;
    lz0_toklist_init(&tl);
    if (lz0_parse_greedy(&f, &tl) < 0) {
        printf("parse fail chain=%d\n", chain);
        lz0_finder_free(&f);
        return;
    }

    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_bitacct acct;
    if (lz0_emit(p, n, &tl, &comp, &csz, &acct) != 0)
        csz = 0;
    free(comp);

    uint32_t n_lit = 0, n_last = 0, n_new = 0;
    uint32_t last = LZ0_INITIAL_OFFSET;
    uint32_t last_off1 = 0, last_other = 0;
    int logged = 0;
    int miss17 = 0, have17 = 0;
    int last_beats_new = 0, new_beats_last = 0, last_chosen_new_better_sav = 0;

    printf("=== greedy chain=%d tokens=%u csize=%zu ratio=%.2f%% ===\n",
           chain, tl.n, csz, n ? 100.0 * (double)csz / (double)n : 0);

    lz0_finder f2;
    lz0_finder_init(&f2, p, n, &cfg);

    for (uint32_t i = 0; i < tl.n; i++) {
        const lz0_token *t = &tl.t[i];
        lz0_finder_insert_upto(&f2, t->pos);
        if (!t->offset) {
            n_lit++;
            continue;
        }
        int is_last = (t->offset == last);
        if (is_last)
            n_last++;
        else
            n_new++;

        if (is_last && last == 1)
            last_off1++;
        else if (is_last)
            last_other++;

        if (is_last) {
            lz0_match cands[64];
            int last_len = 0;
            int nc = lz0_find_matches(&f2, t->pos, last, cands, &last_len);
            int h17 = has_offset(cands, nc, 17);
            if (h17)
                have17++;
            else
                miss17++;
            int ll = 0, lc = 0, ls = -1;
            best_last_of(last_len, &ll, &lc, &ls);
            uint32_t noff = 0;
            int nlen = 0, ncost = 0, nsav = -1;
            best_new_of(cands, nc, last, &noff, &nlen, &ncost, &nsav);
            if (ls >= nsav)
                last_beats_new++;
            else
                new_beats_last++;
            if (nsav > ls)
                last_chosen_new_better_sav++;
            if (logged < log_limit) {
                printf("LAST pos=%u tok_len=%u tok_off=%u last_off=%u raw_last=%d "
                       "LAST len=%d cost=%d sav=%d NEW off=%u len=%d cost=%d sav=%d "
                       "nc=%d has17=%d chosen=LAST\n",
                       t->pos, t->length, t->offset, last, last_len, ll, lc, ls,
                       noff, nlen, ncost, nsav, nc, h17);
                logged++;
            }
        }
        last = t->offset;
    }

    printf("counts lit=%u last=%u new=%u last_off1=%u last_other=%u "
           "last_has17=%d last_miss17=%d last_sav>=new=%d new_sav>last=%d "
           "LAST_chosen_but_NEW_better_sav=%d\n",
           n_lit, n_last, n_new, last_off1, last_other, have17, miss17,
           last_beats_new, new_beats_last, last_chosen_new_better_sav);

    printf("-- snapshots (fresh insert_upto pos) --\n");
    query_fresh(p, n, &cfg, 1, 1, "snap");
    query_fresh(p, n, &cfg, 17, 1, "snap");
    query_fresh(p, n, &cfg, 18, 1, "snap");
    query_fresh(p, n, &cfg, 34, 1, "snap");
    query_fresh(p, n, &cfg, 4352, 1, "snap");
    query_fresh(p, n, &cfg, 4353, 1, "snap");
    query_fresh(p, n, &cfg, n / 4, 1, "snap");
    query_fresh(p, n, &cfg, 8192, 1, "snap");

    lz0_finder_free(&f2);
    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
}

static void size_one(const unsigned char *p, size_t n, const char *name,
                     lz0_parse_t parser, int chain) {
    lz0_config cfg;
    lz0_config_default(&cfg, parser);
    cfg.max_chain = chain;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    if (lz0_compress(p, n, &comp, &csz, &cfg, &st) != 0) {
        printf("%-4s chain=%d FAIL\n", name, chain);
        return;
    }
    printf("%-4s chain=%-3d csize=%5zu ratio=%6.2f%% enc=%6.1fms lit=%u last=%u new=%u\n",
           name, chain, csz, 100.0 * (double)csz / (double)n, st.total_ms,
           st.n_literals, st.n_last, st.n_new);
    free(comp);
}

int main(void) {
    size_t n = 32768;
    unsigned char *p = make_rep(n);
    if (!p)
        return 1;
    dump_chain(p, (uint32_t)n, 32, 8);
    dump_chain(p, (uint32_t)n, 128, 4);
    printf("=== parser sizes n=%zu ===\n", n);
    size_one(p, n, "G", LZ0_PARSE_GREEDY, 32);
    size_one(p, n, "S2", LZ0_PARSE_SDP, 32);
    size_one(p, n, "U", LZ0_PARSE_ULTRA, 32);
    size_one(p, n, "G", LZ0_PARSE_GREEDY, 128);
    size_one(p, n, "S2", LZ0_PARSE_SDP, 128);
    size_one(p, n, "U", LZ0_PARSE_ULTRA, 128);
    free(p);
    return 0;
}
