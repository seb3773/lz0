#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "zx0_compress_wrapper.h"
#include "zx0_decompress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

static int rt_lz0(const unsigned char *in, size_t n, const unsigned char *comp, size_t csz) {
    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out)
        return -1;
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    int ok = (d == (int)n && memcmp(out, in, n) == 0);
    free(out);
    return ok ? 0 : -1;
}

static int rt_zx0(const unsigned char *in, size_t n, const unsigned char *comp, size_t csz) {
    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out)
        return -1;
    int d = zx0_decompress_to(comp, (int)csz, out, (int)n);
    int ok = (d == (int)n && memcmp(out, in, n) == 0);
    free(out);
    return ok ? 0 : -1;
}

static void run_zx0(const char *kind, size_t n, const unsigned char *in) {
    unsigned char *comp = NULL;
    size_t csz = 0;
    double t0 = now_ms();
    if (zx0_compress_c(in, n, &comp, &csz) != 0) {
        printf("%-6s ZX0  FAIL\n", kind);
        return;
    }
    double enc = now_ms() - t0;
    int ok = rt_zx0(in, n, comp, csz);
    printf("%-6s %-4s %10zu %7.2f%% %10.1f %10s\n",
           kind, "ZX0", csz, 100.0 * (double)csz / (double)n, enc, ok == 0 ? "ok" : "FAIL");
    free(comp);
}

static void run_lz0(const char *kind, size_t n, const unsigned char *in,
                    const char *var, lz0_parse_t parser, int score_k) {
    lz0_config cfg;
    lz0_config_default(&cfg, parser);
    cfg.max_chain = 32;
    cfg.max_candidates = 16;
    cfg.score_k = score_k;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    double t0 = now_ms();
    if (lz0_compress(in, n, &comp, &csz, &cfg, &st) != 0) {
        printf("%-6s %-4s FAIL compress\n", kind, var);
        return;
    }
    double enc = now_ms() - t0;
    int ok = rt_lz0(in, n, comp, csz);
    double bits = (double)st.bits_total;
    printf("%-6s %-5s %10zu %7.2f%% %10.1f %10s  parse=%6.1f lit=%u last=%u new=%u bits=%.0f cov=%zu\n",
           kind, var, csz, 100.0 * (double)csz / (double)n, enc, ok == 0 ? "ok" : "FAIL",
           st.parse_ms, st.n_literals, st.n_last, st.n_new, bits, n);
    free(comp);
}

int main(void) {
    size_t n = 32 * 1024;
    const char *kinds[] = {"text", "rep", "bin"};
    printf("%-6s %-4s %10s %8s %10s %10s\n", "kind", "var", "csize", "ratio", "enc_ms", "ok");
    for (int k = 0; k < 3; k++) {
        unsigned char *in = make_corpus(kinds[k], n);
        if (!in)
            return 1;
        run_zx0(kinds[k], n, in);
        run_lz0(kinds[k], n, in, "A2", LZ0_PARSE_LAZY, 8);
        run_lz0(kinds[k], n, in, "L1", LZ0_PARSE_LITE1, 8);
        run_lz0(kinds[k], n, in, "H2k4", LZ0_PARSE_H2, 4);
        run_lz0(kinds[k], n, in, "H2k6", LZ0_PARSE_H2, 6);
        run_lz0(kinds[k], n, in, "H2k8", LZ0_PARSE_H2, 8);
        run_lz0(kinds[k], n, in, "H2k10", LZ0_PARSE_H2, 10);
        run_lz0(kinds[k], n, in, "A3", LZ0_PARSE_DP, 8);
        free(in);
    }
    return 0;
}
