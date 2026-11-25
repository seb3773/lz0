#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "zx0_compress_wrapper.h"
#include "zx0_decompress.h"
#include "zx0_internal.h"
#include "lz0.h"

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
    } else if (!strcmp(kind, "bin")) {
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
    } else {
        for (size_t i = 0; i < n; i++)
            p[i] = (unsigned char)(rnd() >> 16);
    }
    return p;
}

static int roundtrip_lz0(const unsigned char *in, size_t n,
                         const unsigned char *comp, size_t csz) {
    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out)
        return -1;
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    int ok = (d == (int)n && memcmp(out, in, n) == 0);
    free(out);
    return ok ? 0 : -1;
}

static int roundtrip_zx0(const unsigned char *in, size_t n,
                         const unsigned char *comp, size_t csz) {
    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out)
        return -1;
    int d = zx0_decompress_to(comp, (int)csz, out, (int)n);
    int ok = (d == (int)n && memcmp(out, in, n) == 0);
    free(out);
    return ok ? 0 : -1;
}

static void print_header(void) {
    printf("%-6s %-6s %-4s %10s %8s %10s %10s %8s %8s %8s %8s %10s\n",
           "size", "kind", "var", "csize", "ratio", "enc_ms", "dec_ms",
           "find_ms", "parse_ms", "emit_ms", "ram_kb", "ok");
}

static void run_one(const char *kind, size_t n, const unsigned char *in,
                    const char *var, int do_zx0, lz0_parse_t parser,
                    int min_match, int max_chain, int max_cand) {
    unsigned char *comp = NULL;
    size_t csz = 0;
    double enc_ms = 0, dec_ms = 0, find_ms = 0, parse_ms = 0, emit_ms = 0;
    size_t ram = 0;
    int ok = -1;

    if (!strcmp(var, "ZX0")) {
        if (!do_zx0) {
            printf("%-6zu %-6s %-4s %10s %8s %10s %10s %8s %8s %8s %8s %10s\n",
                   n, kind, var, "-", "-", "skip", "-", "-", "-", "-", "-", "skip");
            return;
        }
        double t0 = now_ms();
        if (zx0_compress_c(in, n, &comp, &csz) != 0) {
            printf("%-6zu %-6s %-4s FAIL compress\n", n, kind, var);
            return;
        }
        enc_ms = now_ms() - t0;
        double t1 = now_ms();
        ok = roundtrip_zx0(in, n, comp, csz);
        dec_ms = now_ms() - t1;
        ram = 0;
        free(comp);
    } else {
        lz0_config cfg;
        lz0_config_default(&cfg, parser);
        cfg.min_match = min_match;
        cfg.max_chain = max_chain;
        cfg.max_candidates = max_cand;
        lz0_stats st;
        double t0 = now_ms();
        if (lz0_compress(in, n, &comp, &csz, &cfg, &st) != 0) {
            printf("%-6zu %-6s %-4s FAIL compress\n", n, kind, var);
            return;
        }
        enc_ms = now_ms() - t0;
        find_ms = st.finder_ms;
        parse_ms = st.parse_ms;
        emit_ms = st.emit_ms;
        ram = st.peak_ram;
        double t1 = now_ms();
        ok = roundtrip_lz0(in, n, comp, csz);
        dec_ms = now_ms() - t1;
        free(comp);
    }

    double ratio = (csz > 0) ? (100.0 * (double)csz / (double)n) : 0;
    printf("%-6zu %-6s %-4s %10zu %7.2f%% %10.1f %10.3f %8.1f %8.1f %8.1f %8.0f %10s\n",
           n, kind, var, csz, ratio, enc_ms, dec_ms, find_ms, parse_ms, emit_ms,
           ram / 1024.0, ok == 0 ? "ok" : "FAIL");
    fflush(stdout);
}

int main(int argc, char **argv) {
    int quick = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--quick"))
            quick = 1;

    size_t sizes_q[] = {32 * 1024, 64 * 1024, 256 * 1024};
    size_t sizes_f[] = {32 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024, 2 * 1024 * 1024, 5 * 1024 * 1024};
    size_t *sizes = quick ? sizes_q : sizes_f;
    int ns = quick ? 3 : 6;
    const char *kinds[] = {"text", "rep", "bin", "rand"};
    int nk = quick ? 2 : 4;

    print_header();
    for (int s = 0; s < ns; s++) {
        size_t n = sizes[s];
            int do_zx0 = (n <= 32 * 1024);
        for (int k = 0; k < nk; k++) {
            unsigned char *in = make_corpus(kinds[k], n);
            if (!in) {
                fprintf(stderr, "oom\n");
                return 1;
            }
            run_one(kinds[k], n, in, "ZX0", do_zx0, 0, 2, 32, 16);
            run_one(kinds[k], n, in, "G", 0, LZ0_PARSE_GREEDY, 2, 4, 8);
            run_one(kinds[k], n, in, "S2", 0, LZ0_PARSE_SDP, 2, 8, 16);
            run_one(kinds[k], n, in, "S8", 0, LZ0_PARSE_SDP, 2, 32, 32);
            if (n <= 64 * 1024)
                run_one(kinds[k], n, in, "U", 0, LZ0_PARSE_ULTRA, 2, 64, 48);
            else
                printf("%-6zu %-6s %-4s %10s %8s %10s %10s %8s %8s %8s %8s %10s\n",
                       n, kinds[k], "U", "-", "-", "skip", "-", "-", "-", "-", "-", "skip");
            free(in);
        }
    }
    return 0;
}
