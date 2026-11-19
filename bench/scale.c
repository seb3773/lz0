#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
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

static int rt_lz0(const unsigned char *in, size_t n, const unsigned char *comp, size_t csz,
                  double *dec_ms) {
    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out)
        return -1;
    double t0 = now_ms();
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    *dec_ms = now_ms() - t0;
    int ok = (d == (int)n && memcmp(out, in, n) == 0);
    free(out);
    return ok ? 0 : -1;
}

static int rt_zx0(const unsigned char *in, size_t n, const unsigned char *comp, size_t csz,
                  double *dec_ms) {
    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out)
        return -1;
    double t0 = now_ms();
    int d = zx0_decompress_to(comp, (int)csz, out, (int)n);
    *dec_ms = now_ms() - t0;
    int ok = (d == (int)n && memcmp(out, in, n) == 0);
    free(out);
    return ok ? 0 : -1;
}

static void run_zx0(const char *sz, const char *kind, size_t n, const unsigned char *in) {
    unsigned char *comp = NULL;
    size_t csz = 0;
    double t0 = now_ms();
    if (zx0_compress_c(in, n, &comp, &csz) != 0) {
        printf("%-6s %-6s %-4s FAIL\n", sz, kind, "ZX0");
        return;
    }
    double enc = now_ms() - t0;
    double dec = 0;
    int ok = rt_zx0(in, n, comp, csz, &dec);
    printf("%-6s %-6s %-4s %10zu %7.2f%% %10.1f %8.3f %8s %8s %10s %7s %7s %10s\n",
           sz, kind, "ZX0", csz, 100.0 * (double)csz / (double)n, enc, dec,
           "-", "-", "-", "-", "-", ok == 0 ? "ok" : "FAIL");
    free(comp);
    fflush(stdout);
}

static void run_lz0(const char *sz, const char *kind, size_t n, const unsigned char *in,
                    const char *var, lz0_parse_t parser) {
    lz0_config cfg;
    lz0_config_default(&cfg, parser);
    cfg.min_match = 2;
    cfg.max_chain = 32;
    cfg.max_candidates = 16;
    cfg.max_states = 2;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census cen;
    memset(&cen, 0, sizeof(cen));
    if (parser == LZ0_PARSE_SDP)
        lz0_sdp_census_out = &cen;
    double t0 = now_ms();
    if (lz0_compress(in, n, &comp, &csz, &cfg, &st) != 0) {
        printf("%-6s %-6s %-4s FAIL compress\n", sz, kind, var);
        lz0_sdp_census_out = NULL;
        return;
    }
    double enc = now_ms() - t0;
    lz0_sdp_census_out = NULL;
    double dec = 0;
    int ok = rt_lz0(in, n, comp, csz, &dec);
    char ast[16] = "-", als[16] = "-";
    if (parser == LZ0_PARSE_SDP && cen.n_pos) {
        snprintf(ast, sizeof(ast), "%.2f", (double)cen.sum_states / (double)cen.n_pos);
        snprintf(als, sizeof(als), "%.2f", (double)cen.sum_distinct_last / (double)cen.n_pos);
    }
    printf("%-6s %-6s %-4s %10zu %7.2f%% %10.1f %8.3f %8.1f %8.1f %10.0f %7s %7s %10s\n",
           sz, kind, var, csz, 100.0 * (double)csz / (double)n, enc, dec,
           st.parse_ms, st.emit_ms, st.peak_ram / 1024.0, ast, als,
           ok == 0 ? "ok" : "FAIL");
    free(comp);
    fflush(stdout);
}

static const char *szlabel(size_t n) {
    if (n == 64 * 1024)
        return "64K";
    if (n == 256 * 1024)
        return "256K";
    if (n == 1024 * 1024)
        return "1M";
    if (n == 2 * 1024 * 1024)
        return "2M";
    if (n == 5 * 1024 * 1024)
        return "5M";
    if (n == 32 * 1024)
        return "32K";
    return "?";
}

int main(int argc, char **argv) {
    size_t sizes[] = {64 * 1024, 256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 5 * 1024 * 1024};
    int ns = 5;
    int do_zx0_max = 64 * 1024;
    int no_zx0 = 0;
    int kind_only = -1;
    int s2_only = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--no-zx0"))
            no_zx0 = 1;
        else if (!strcmp(argv[a], "--s2-only"))
            s2_only = 1;
        else if (!strcmp(argv[a], "64K")) {
            sizes[0] = 64 * 1024;
            ns = 1;
        } else if (!strcmp(argv[a], "256K")) {
            sizes[0] = 256 * 1024;
            ns = 1;
        } else if (!strcmp(argv[a], "1M")) {
            sizes[0] = 1024 * 1024;
            ns = 1;
        } else if (!strcmp(argv[a], "2M")) {
            sizes[0] = 2 * 1024 * 1024;
            ns = 1;
        } else if (!strcmp(argv[a], "5M")) {
            sizes[0] = 5 * 1024 * 1024;
            ns = 1;
        } else if (!strcmp(argv[a], "text"))
            kind_only = 0;
        else if (!strcmp(argv[a], "rep"))
            kind_only = 1;
        else if (!strcmp(argv[a], "bin"))
            kind_only = 2;
    }
    if (no_zx0)
        do_zx0_max = 0;
    const char *kinds[] = {"text", "rep", "bin"};
    printf("%-6s %-6s %-4s %10s %8s %10s %8s %8s %8s %10s %7s %7s %10s\n",
           "size", "kind", "var", "csize", "ratio", "enc_ms", "dec_ms",
           "parse", "emit", "ram_kb", "avg_st", "avg_lo", "ok");
    fflush(stdout);
    for (int s = 0; s < ns; s++) {
        size_t n = sizes[s];
        const char *sz = szlabel(n);
        int do_zx0 = (n <= (size_t)do_zx0_max);
        for (int k = 0; k < 3; k++) {
            if (kind_only >= 0 && k != kind_only)
                continue;
            unsigned char *in = make_corpus(kinds[k], n);
            if (!in)
                return 1;
            if (!s2_only) {
                if (do_zx0)
                    run_zx0(sz, kinds[k], n, in);
                else
                    printf("%-6s %-6s %-4s %10s %8s %10s %8s %8s %8s %10s %7s %7s %10s\n",
                           sz, kinds[k], "ZX0", "-", "-", "skip", "-", "-", "-", "-", "-", "-",
                           "skip");
                run_lz0(sz, kinds[k], n, in, "A2", LZ0_PARSE_LAZY);
            }
            run_lz0(sz, kinds[k], n, in, "S2", LZ0_PARSE_SDP);
            free(in);
        }
    }
    return 0;
}
