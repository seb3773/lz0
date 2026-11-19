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

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int g_pf = 0;

static const char *szlabel(size_t n) {
    if (n == 256 * 1024)
        return "256K";
    if (n == 1024 * 1024)
        return "1M";
    if (n == 2 * 1024 * 1024)
        return "2M";
    if (n == 5 * 1024 * 1024)
        return "5M";
    return "?";
}

static int run_one(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    uint32_t *sa0 = NULL, *rk0 = NULL, *sa1 = NULL, *rk1 = NULL;

    double t0 = now_ms();
    if (lz0_sa_debug_build_algo(in, n, 0, &sa0, &rk0) != 0) {
        printf("prefix-doubling build failed\n");
        return 1;
    }
    double t1 = now_ms();
    if (lz0_sa_debug_build_algo(in, n, 1, &sa1, &rk1) != 0) {
        printf("SA-IS build failed\n");
        return 1;
    }
    double t2 = now_ms();

    int sa_eq = memcmp(sa0, sa1, n * sizeof(uint32_t)) == 0;
    int rk_eq = memcmp(rk0, rk1, n * sizeof(uint32_t)) == 0;
    double b = t1 - t0, v = t2 - t1;
    printf("%-4s %-8s n=%zu pd=%.1fms sais=%.1fms speedup=%.2fx SA=%s rank=%s\n",
           szlabel(n), kind, n, b, v, v > 0 ? b / v : 0.0,
           sa_eq ? "eq" : "DIFF", rk_eq ? "eq" : "DIFF");

    free(sa0);
    free(rk0);
    free(sa1);
    free(rk1);
    free(in);
    return (sa_eq && rk_eq) ? 0 : 1;
}

int main(int argc, char **argv) {
    const size_t sizes[] = {256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 5 * 1024 * 1024};
    int fail = 0;
    int ai = 1;
    if (ai < argc && !strcmp(argv[ai], "pf")) {
        g_pf = 1;
        ai++;
    }
    if (argc - ai >= 2) {
        size_t n = (size_t)strtoull(argv[ai + 1], NULL, 10);
        return run_one(argv[ai], n);
    }
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fail |= run_one("text", sizes[i]);
        fail |= run_one("bin", sizes[i]);
    }
    return fail;
}
