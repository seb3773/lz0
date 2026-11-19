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

static void cfg_h2c8(lz0_config *cfg) {
    lz0_config_default(cfg, LZ0_PARSE_SDP);
    lz0_h2c8_apply();
}

static int run_one(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    lz0_config cfg;
    lz0_stats st0, st1;
    unsigned char *c0 = NULL, *c1 = NULL;
    size_t s0 = 0, s1 = 0;
    cfg_h2c8(&cfg);
    lz0_sa_algo = 0;
    if (lz0_compress(in, n, &c0, &s0, &cfg, &st0) != 0) {
        printf("FAIL compress prefix-doubling\n");
        return 1;
    }
    cfg_h2c8(&cfg);
    lz0_sa_algo = 1;
    if (lz0_compress(in, n, &c1, &s1, &cfg, &st1) != 0) {
        printf("FAIL compress SA-IS\n");
        return 1;
    }
    lz0_sa_algo = 1;
    int same = (s0 == s1) && memcmp(c0, c1, s0) == 0;
    int tok = (st0.n_literals == st1.n_literals && st0.n_last == st1.n_last &&
               st0.n_new == st1.n_new && st0.bits_total == st1.bits_total);
    unsigned char *out = malloc(n + 16);
    int rt = 0;
    if (out) {
        int d = lz0_decompress_to(c1, (int)s1, out, (int)n);
        rt = (d == (int)n && memcmp(out, in, n) == 0);
        free(out);
    }
    printf("%-4s n=%zu pd=%zu sais=%zu stream=%s tok=%s bits=%llu/%llu rt=%s\n",
           kind, n, s0, s1, same ? "IDENTICAL" : "DIFF", tok ? "eq" : "DIFF",
           (unsigned long long)st0.bits_total, (unsigned long long)st1.bits_total,
           rt ? "ok" : "BAD");
    free(c0);
    free(c1);
    free(in);
    return (same && tok && rt) ? 0 : 1;
}

int main(int argc, char **argv) {
    const size_t sizes[] = {256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 5 * 1024 * 1024};
    int fail = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fail |= run_one("text", sizes[i]);
        fail |= run_one("bin", sizes[i]);
    }
    printf(fail ? "STREAM VALIDATION FAILED\n" : "STREAM VALIDATION PASSED\n");
    return fail;
}
