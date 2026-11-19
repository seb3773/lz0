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

static int cmp_match(const void *a, const void *b) {
    const lz0_match *x = (const lz0_match *)a;
    const lz0_match *y = (const lz0_match *)b;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    if (x->length != y->length)
        return x->length < y->length ? -1 : 1;
    return 0;
}

static int same_set(lz0_match *a, int na, lz0_match *b, int nb) {
    if (na != nb)
        return 0;
    qsort(a, (size_t)na, sizeof(lz0_match), cmp_match);
    qsort(b, (size_t)nb, sizeof(lz0_match), cmp_match);
    for (int i = 0; i < na; i++)
        if (a[i].offset != b[i].offset || a[i].length != b[i].length)
            return 0;
    return 1;
}

static int run_kind(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    cfg.min_match = 2;
    cfg.max_chain = 32;
    cfg.max_candidates = 16;
    cfg.max_states = 2;
    lz0_finder fa, fb;
    if (lz0_finder_init(&fa, in, (uint32_t)n, &cfg) < 0 ||
        lz0_finder_init(&fb, in, (uint32_t)n, &cfg) < 0) {
        free(in);
        return 1;
    }
    int mism = 0;
    uint64_t npos = 0, ncand = 0;
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        lz0_finder_insert_upto(&fa, i);
        lz0_finder_insert_upto(&fb, i);
        lz0_match ca[64], cb[64];
        int la = 0, lb = 0;
        lz0_force_chain_walk = 1;
        int na = lz0_find_matches(&fa, i, 0, ca, &la);
        lz0_force_chain_walk = 0;
        int nb = lz0_find_matches(&fb, i, 0, cb, &lb);
        npos++;
        ncand += (uint64_t)na;
        if (la != lb || !same_set(ca, na, cb, nb)) {
            if (mism < 8)
                printf("%s pos=%u walk n=%d last=%d skip n=%d last=%d\n",
                       kind, i, na, la, nb, lb);
            mism++;
        }
    }
    printf("%s n=%zu pos=%llu cand=%llu mismatch=%d %s\n",
           kind, n, (unsigned long long)npos, (unsigned long long)ncand, mism,
           mism ? "FAIL" : "ok");
    lz0_finder_free(&fa);
    lz0_finder_free(&fb);
    free(in);
    return mism ? 1 : 0;
}

int main(int argc, char **argv) {
    size_t n = 32 * 1024;
    if (argc >= 2 && !strcmp(argv[1], "256K"))
        n = 256 * 1024;
    const char *kinds[] = {"text", "rep", "bin"};
    int fail = 0;
    for (int k = 0; k < 3; k++)
        fail |= run_kind(kinds[k], n);
    return fail;
}
