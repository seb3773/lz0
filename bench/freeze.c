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

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "VmRSS:", 6)) {
            v = atol(line + 6);
            break;
        }
    }
    fclose(f);
    return v;
}

static int run_one(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    lz0_h2c8_apply();
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    long rss0 = rss_kb();
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    long rss1 = rss_kb();
    if (rc != 0) {
        printf("%-5s %-4s FAIL enc\n", szlabel(n), kind);
        free(in);
        return 1;
    }
    unsigned char *out = (unsigned char *)malloc(n + 16);
    double dec = 0;
    int ok = 0;
    if (out) {
        double t0 = now_ms();
        int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
        dec = now_ms() - t0;
        ok = (d == (int)n && memcmp(out, in, n) == 0);
        free(out);
    }
    printf("%-5s %-4s %10zu %7.2f%% %10.1f %8.2f %8.1f %8ld %s\n",
           szlabel(n), kind, csz, 100.0 * (double)csz / (double)n,
           st.total_ms, dec, lz0_sa_build_ms, rss1 > rss0 ? rss1 - rss0 : rss1,
           ok ? "ok" : "FAIL");
    free(comp);
    free(in);
    return ok ? 0 : 1;
}

int main(void) {
    const size_t sizes[] = {256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 5 * 1024 * 1024};
    int fail = 0;
    printf("%-5s %-4s %10s %8s %10s %8s %8s %8s %s\n",
           "size", "kind", "csize", "ratio", "enc_ms", "dec_ms", "sa_ms", "rss_kb", "ok");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fail |= run_one("text", sizes[i]);
        fail |= run_one("bin", sizes[i]);
    }
    return fail;
}
