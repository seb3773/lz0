#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rnd_state;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static unsigned char *make_text(size_t n) {
    unsigned char *p = (unsigned char *)malloc(n);
    rnd_state = 0xC0FFEEu + (uint32_t)n + (uint32_t)'t' * 97u;
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
    return p;
}

static int check_tokens(const unsigned char *in, uint32_t n, const lz0_toklist *tl) {
    uint32_t covered = 0;
    uint32_t last = LZ0_INITIAL_OFFSET;
    int prev_lit = 0;
    for (uint32_t i = 0; i < tl->n; i++) {
        const lz0_token *t = &tl->t[i];
        if (t->pos != covered) {
            printf("gap at token %u pos=%u covered=%u\n", i, t->pos, covered);
            return 1;
        }
        if (t->length == 0) {
            printf("zero length token %u\n", i);
            return 1;
        }
        if (!t->offset) {
            if (prev_lit) {
                printf("consecutive lits at %u\n", i);
                return 1;
            }
            prev_lit = 1;
        } else {
            if (t->offset > t->pos) {
                printf("bad offset token %u off=%u pos=%u\n", i, t->offset, t->pos);
                return 1;
            }
            if (i == 0) {
                printf("starts with match\n");
                return 1;
            }
            prev_lit = 0;
            last = t->offset;
        }
        covered += t->length;
        (void)in;
        (void)last;
    }
    if (covered != n) {
        printf("cover %u != n %u tokens=%u\n", covered, n, tl->n);
        return 1;
    }
    return 0;
}

int main(void) {
    size_t n = 32768;
    unsigned char *in = make_text(n);
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_DP);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) != 0)
        return 1;
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    if (lz0_parse_dp(&f, &tl) != 0) {
        printf("parse fail\n");
        return 1;
    }
    if (check_tokens(in, (uint32_t)n, &tl))
        return 1;

    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_bitacct acct;
    if (lz0_emit(in, (uint32_t)n, &tl, &comp, &csz, &acct) != 0) {
        printf("emit fail\n");
        return 1;
    }
    unsigned char *out = (unsigned char *)malloc(n + 16);
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    printf("tokens=%u csz=%zu decoded=%d\n", tl.n, csz, d);
    int bad = (d != (int)n) || memcmp(out, in, n);
    if (bad) {
        if (d == (int)n) {
            for (size_t k = 0; k < n; k++) {
                if (out[k] != in[k]) {
                    printf("mismatch at %zu in=%02x out=%02x\n", k, in[k], out[k]);
                    uint32_t cov = 0;
                    for (uint32_t t = 0; t < tl.n; t++) {
                        cov += tl.t[t].length;
                        if (cov > k) {
                            printf("token %u pos=%u len=%u off=%u\n", t, tl.t[t].pos,
                                   tl.t[t].length, tl.t[t].offset);
                            break;
                        }
                    }
                    break;
                }
            }
        }
        return 1;
    }
    printf("ok\n");
    free(out);
    free(comp);
    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
    free(in);
    return 0;
}
