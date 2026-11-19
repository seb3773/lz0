#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static int tok_cost(const lz0_token *t, uint32_t last_off) {
    if (!t->offset)
        return lz0_cost_literal((int)t->length);
    if (t->offset == last_off)
        return lz0_cost_last((int)t->length);
    return lz0_cost_new((int)t->offset, (int)t->length);
}

static const char *tok_kind(const lz0_token *t, uint32_t last_off) {
    if (!t->offset)
        return "LIT";
    if (t->offset == last_off)
        return "LAST";
    return "NEW";
}

static int parse_one(lz0_finder *f, lz0_parse_t p, lz0_toklist *tl) {
    lz0_toklist_init(tl);
    if (p == LZ0_PARSE_LAZY)
        return lz0_parse_lazy(f, tl);
    if (p == LZ0_PARSE_LITE1)
        return lz0_parse_lite1(f, tl);
    return lz0_parse_dp(f, tl);
}

static void dump_tokens(const char *var, const lz0_toklist *tl, int limit) {
    uint32_t last = LZ0_INITIAL_OFFSET;
    uint32_t nlit = 0, nlast = 0, nnew = 0;
    int shown = 0;
    printf("  %s tokens=%u\n", var, tl->n);
    for (uint32_t i = 0; i < tl->n; i++) {
        const lz0_token *t = &tl->t[i];
        uint32_t before = last;
        int c = tok_cost(t, before);
        const char *k = tok_kind(t, before);
        uint32_t after = t->offset ? t->offset : last;
        if (!t->offset)
            nlit++;
        else if (t->offset == before)
            nlast++;
        else
            nnew++;
        if (shown < limit) {
            printf("    pos=%u last_before=%u %s off=%u len=%u cost=%d last_after=%u\n",
                   t->pos, before, k, t->offset, t->length, c, after);
            shown++;
        }
        last = after;
    }
    printf("    counts lit=%u last=%u new=%u\n", nlit, nlast, nnew);
}

static void dump_div(const char *a_name, const lz0_toklist *a,
                     const char *b_name, const lz0_toklist *b) {
    uint32_t la = LZ0_INITIAL_OFFSET, lb = LZ0_INITIAL_OFFSET;
    uint32_t ia = 0, ib = 0;
    uint32_t pa = 0, pb = 0;
    while (ia < a->n && ib < b->n) {
        if (a->t[ia].pos == b->t[ib].pos &&
            a->t[ia].offset == b->t[ib].offset &&
            a->t[ia].length == b->t[ib].length) {
            if (a->t[ia].offset)
                la = a->t[ia].offset;
            if (b->t[ib].offset)
                lb = b->t[ib].offset;
            pa = a->t[ia].pos + a->t[ia].length;
            pb = b->t[ib].pos + b->t[ib].length;
            ia++;
            ib++;
            continue;
        }
        printf("  first diverge %s vs %s at ia=%u ib=%u\n", a_name, b_name, ia, ib);
        printf("    %s pos=%u last=%u %s off=%u len=%u\n",
               a_name, a->t[ia].pos, la, tok_kind(&a->t[ia], la),
               a->t[ia].offset, a->t[ia].length);
        printf("    %s pos=%u last=%u %s off=%u len=%u\n",
               b_name, b->t[ib].pos, lb, tok_kind(&b->t[ib], lb),
               b->t[ib].offset, b->t[ib].length);
        (void)pa;
        (void)pb;
        return;
    }
    printf("  %s vs %s: no token diverge (na=%u nb=%u)\n", a_name, b_name, a->n, b->n);
}

static void run_kind(const char *kind, const unsigned char *in, uint32_t n) {
    printf("=== %s n=%u ===\n", kind, n);
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_DP);
    cfg.max_chain = 32;
    cfg.max_candidates = 16;

    lz0_finder f2, f1, f3;
    lz0_finder_init(&f2, in, n, &cfg);
    lz0_finder_init(&f1, in, n, &cfg);
    lz0_finder_init(&f3, in, n, &cfg);

    lz0_toklist a2, l1, a3;
    parse_one(&f2, LZ0_PARSE_LAZY, &a2);
    parse_one(&f1, LZ0_PARSE_LITE1, &l1);

    lz0_dp_census cen;
    memset(&cen, 0, sizeof(cen));
    lz0_dp_census_out = &cen;
    parse_one(&f3, LZ0_PARSE_DP, &a3);
    lz0_dp_census_out = NULL;

    dump_tokens("A2", &a2, 10);
    dump_tokens("L1", &l1, 10);
    dump_tokens("A3", &a3, 10);
    dump_div("A2", &a2, "A3", &a3);
    dump_div("L1", &l1, "A3", &a3);
    dump_div("A2", &a2, "L1", &l1);

    printf("  A3 census both_live=%u last_diff=%u close32=%u close64=%u (n=%u)\n",
           cen.both_live, cen.last_diff, cen.last_diff_close32, cen.last_diff_close64, cen.n_pos);
    for (uint32_t s = 0; s < cen.nsamp; s++) {
        int gap = cen.sal_bits[s] - cen.sam_bits[s];
        if (gap < 0)
            gap = -gap;
        printf("    pos=%u al_last=%u al_bits=%d am_last=%u am_bits=%d gap=%d\n",
               cen.spos[s], cen.sal_off[s], cen.sal_bits[s],
               cen.sam_off[s], cen.sam_bits[s], gap);
    }

    lz0_toklist_free(&a2);
    lz0_toklist_free(&l1);
    lz0_toklist_free(&a3);
    lz0_finder_free(&f2);
    lz0_finder_free(&f1);
    lz0_finder_free(&f3);
}

int main(void) {
    size_t n = 32 * 1024;
    const char *kinds[] = {"text", "rep", "bin"};
    for (int k = 0; k < 3; k++) {
        unsigned char *in = make_corpus(kinds[k], n);
        if (!in)
            return 1;
        run_kind(kinds[k], in, (uint32_t)n);
        free(in);
    }
    return 0;
}
