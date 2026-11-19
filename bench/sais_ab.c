#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

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

static const char *szlabel(size_t n) {
    if (n == 256)
        return "256B";
    if (n == 1024)
        return "1K";
    if (n == 4096)
        return "4K";
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

static void cfg_h2c8(lz0_config *cfg) {
    lz0_config_default(cfg, LZ0_PARSE_SDP);
    lz0_h2c8_apply();
}

static int sa_eq_report(const char *kind, size_t n, const uint32_t *sa0, const uint32_t *rk0,
                        const uint32_t *sa1, const uint32_t *rk1, double t0, double t1) {
    int sa_eq = memcmp(sa0, sa1, n * sizeof(uint32_t)) == 0;
    int rk_eq = memcmp(rk0, rk1, n * sizeof(uint32_t)) == 0;
    int first = -1;
    if (!sa_eq) {
        for (size_t i = 0; i < n; i++) {
            if (sa0[i] != sa1[i]) {
                first = (int)i;
                break;
            }
        }
    }
    printf("%-5s %-4s n=%zu pd=%.1fms sais=%.1fms speedup=%.2fx SA=%s rank=%s",
           szlabel(n), kind, n, t0, t1, t1 > 0 ? t0 / t1 : 0.0,
           sa_eq ? "eq" : "DIFF", rk_eq ? "eq" : "DIFF");
    if (first >= 0)
        printf(" first=%d pd=%u sais=%u", first, sa0[first], sa1[first]);
    printf("\n");
    return (sa_eq && rk_eq) ? 0 : 1;
}

static int run_sa(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    uint32_t *sa0 = NULL, *rk0 = NULL, *sa1 = NULL, *rk1 = NULL;
    double t0 = now_ms();
    if (lz0_sa_debug_build_algo(in, n, 0, &sa0, &rk0) != 0) {
        printf("pd build failed %s n=%zu\n", kind, n);
        free(in);
        return 1;
    }
    double t1 = now_ms();
    if (lz0_sa_debug_build_algo(in, n, 1, &sa1, &rk1) != 0) {
        printf("sais build failed %s n=%zu\n", kind, n);
        free(sa0);
        free(rk0);
        free(in);
        return 1;
    }
    double t2 = now_ms();
    int rc = sa_eq_report(kind, n, sa0, rk0, sa1, rk1, t1 - t0, t2 - t1);
    free(sa0);
    free(rk0);
    free(sa1);
    free(rk1);
    free(in);
    return rc;
}

static int run_e2e(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return 1;
    lz0_config cfg;
    unsigned char *c0 = NULL, *c1 = NULL;
    size_t s0 = 0, s1 = 0;
    lz0_stats st0, st1;
    lz0_sdp_census cen0, cen1;
    memset(&cen0, 0, sizeof(cen0));
    memset(&cen1, 0, sizeof(cen1));

    cfg_h2c8(&cfg);
    lz0_sa_algo = 0;
    lz0_sdp_census_out = &cen0;
    long rss0 = rss_kb();
    double w0 = now_ms();
    if (lz0_compress(in, n, &c0, &s0, &cfg, &st0) != 0) {
        printf("FAIL compress pd %s n=%zu\n", kind, n);
        free(in);
        return 1;
    }
    double w1 = now_ms();
    long rss1 = rss_kb();
    lz0_sdp_census_out = NULL;

    cfg_h2c8(&cfg);
    lz0_sa_algo = 1;
    lz0_sdp_census_out = &cen1;
    long rss2 = rss_kb();
    double w2 = now_ms();
    if (lz0_compress(in, n, &c1, &s1, &cfg, &st1) != 0) {
        printf("FAIL compress sais %s n=%zu\n", kind, n);
        free(c0);
        free(in);
        return 1;
    }
    double w3 = now_ms();
    long rss3 = rss_kb();
    lz0_sdp_census_out = NULL;
    lz0_sa_algo = 0;

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
    double pd_cpu = cen0.ms_sa_build + cen0.ms_insert + cen0.ms_find + cen0.ms_trans + cen0.ms_back;
    double sais_cpu = cen1.ms_sa_build + cen1.ms_insert + cen1.ms_find + cen1.ms_trans + cen1.ms_back;
    double e2e_gain = pd_cpu > 0 ? 100.0 * (pd_cpu - sais_cpu) / pd_cpu : 0;
    printf("%-5s %-4s csz=%zu/%zu stream=%s tok=%s bits=%llu/%llu rt=%s\n",
           szlabel(n), kind, s0, s1, same ? "IDENTICAL" : "DIFF", tok ? "eq" : "DIFF",
           (unsigned long long)st0.bits_total, (unsigned long long)st1.bits_total,
           rt ? "ok" : "BAD");
    printf("      pd   wall=%.0f cpu=%.0f sab=%.0f rss=%ldKB\n",
           w1 - w0, pd_cpu, cen0.ms_sa_build, rss1 - rss0);
    printf("      sais wall=%.0f cpu=%.0f sab=%.0f rss=%ldKB e2e=%.1f%% sa_speedup=%.2fx\n",
           w3 - w2, sais_cpu, cen1.ms_sa_build, rss3 - rss2, e2e_gain,
           cen1.ms_sa_build > 0 ? cen0.ms_sa_build / cen1.ms_sa_build : 0.0);
    free(c0);
    free(c1);
    free(in);
    return (same && tok && rt) ? 0 : 1;
}

int main(int argc, char **argv) {
    int fail = 0;
    int mode = 0;
    int ai = 1;
    if (ai < argc && !strcmp(argv[ai], "sa")) {
        mode = 0;
        ai++;
    } else if (ai < argc && !strcmp(argv[ai], "e2e")) {
        mode = 1;
        ai++;
    }
    if (argc - ai >= 2) {
        size_t n = (size_t)strtoull(argv[ai + 1], NULL, 10);
        if (mode == 1)
            return run_e2e(argv[ai], n);
        return run_sa(argv[ai], n);
    }
    if (mode == 0) {
        const size_t tiny[] = {256, 1024, 4096};
        for (size_t i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
            fail |= run_sa("text", tiny[i]);
            fail |= run_sa("bin", tiny[i]);
        }
        if (fail)
            return fail;
        const size_t sizes[] = {256 * 1024, 1024 * 1024};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            fail |= run_sa("text", sizes[i]);
            fail |= run_sa("bin", sizes[i]);
        }
        return fail;
    }
    const size_t sizes[] = {1024 * 1024, 2 * 1024 * 1024, 5 * 1024 * 1024};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fail |= run_e2e("text", sizes[i]);
        if (sizes[i] == 5 * 1024 * 1024)
            fail |= run_e2e("bin", sizes[i]);
    }
    return fail;
}
