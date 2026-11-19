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

static const char *szlabel(size_t n) {
    if (n == 32 * 1024)
        return "32K";
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
    return "?";
}

static void run_zx0(const char *sz, const char *kind, size_t n, const unsigned char *in) {
    unsigned char *comp = NULL;
    size_t csz = 0;
    double t0 = now_ms();
    if (zx0_compress_c(in, n, &comp, &csz) != 0) {
        printf("%-6s %-6s %-4s FAIL enc\n", sz, kind, "ZX0");
        fflush(stdout);
        return;
    }
    double enc = now_ms() - t0;
    unsigned char *out = (unsigned char *)malloc(n + 16);
    double dec = 0;
    int ok = 0;
    if (out) {
        t0 = now_ms();
        int d = zx0_decompress_to(comp, (int)csz, out, (int)n);
        dec = now_ms() - t0;
        ok = (d == (int)n && memcmp(out, in, n) == 0);
        free(out);
    }
    printf("%-6s %-6s %-6s %10zu %7.2f%% %10.1f %10s %8s %10s %8.2f %12s %10s %12s %6s %6s %6s %6s %s\n",
           sz, kind, "ZX0", csz, 100.0 * (double)csz / (double)n, enc, "-", "-", "-",
           dec, "-", "-", "-", "-", "-", "-", "-", ok ? "ok" : "FAIL");
    free(comp);
    fflush(stdout);
}

static void reset_lz0_diag(void) {
    lz0_chain_prune = 0;
    lz0_chain_noskip = 0;
    lz0_skip_thresh = 0;
    lz0_skip_stride = 0;
    lz0_hash4_probe = 0;
    lz0_deep_probe = 0;
    lz0_sa_win = 0;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 0;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 0;
    lz0_sa_csort_v2 = 1;
    lz0_sa_csort_smode = 1;
    lz0_sa_count_mode = 1;
    lz0_sa_pf_look = 16;
}

static void run_lz0(const char *sz, const char *kind, size_t n, const unsigned char *in,
                    const char *var, int chain, int hybrid) {
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    cfg.min_match = 2;
    cfg.max_chain = chain;
    cfg.max_candidates = 16;
    cfg.max_states = 2;
    cfg.jump_levels = hybrid ? 0 : 16;
    reset_lz0_diag();
    if (hybrid)
        lz0_h2c8_apply();
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census sdp;
    memset(&sdp, 0, sizeof(sdp));
    lz0_sdp_census_out = &sdp;
    if (lz0_compress(in, n, &comp, &csz, &cfg, &st) != 0) {
        lz0_sdp_census_out = NULL;
        reset_lz0_diag();
        printf("%-6s %-6s %-6s FAIL enc\n", sz, kind, var);
        fflush(stdout);
        return;
    }
    lz0_sdp_census_out = NULL;
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
    double avg_best = sdp.n_cand ? (double)sdp.sum_cand_len / (double)sdp.n_cand : 0;
    double p4 = sdp.n_sa_invoke ? 100.0 * (double)sdp.n_sa_stop4 / (double)sdp.n_sa_invoke : 0;
    double p8 = sdp.n_sa_invoke ? 100.0 * (double)sdp.n_sa_stop8 / (double)sdp.n_sa_invoke : 0;
    double p16 = sdp.n_sa_invoke ? 100.0 * (double)sdp.n_sa_stop16 / (double)sdp.n_sa_invoke : 0;
    double sab = sdp.ms_sa_build > 0 ? sdp.ms_sa_build : lz0_sa_build_ms;
    printf("%-6s %-6s %-6s %10zu %7.2f%% %10.1f %10.1f %8.1f %10.0f %8.2f %12llu %10llu %12llu %6.1f %6.1f %6.1f %6.1f %s\n",
           sz, kind, var, csz, 100.0 * (double)csz / (double)n,
           st.total_ms, sdp.ms_find, sab, st.peak_ram / 1024.0, dec,
           (unsigned long long)sdp.n_chain_visit,
           (unsigned long long)sdp.n_chain_check,
           (unsigned long long)sdp.n_sa_invoke,
           p4, p8, p16, avg_best, ok ? "ok" : "FAIL");
    reset_lz0_diag();
    free(comp);
    fflush(stdout);
}

int main(int argc, char **argv) {
    size_t sizes[8];
    int ns = 0;
    int want_text = 1, want_rep = 1, want_bin = 1;
    int do_zx0 = 1;
    int do_lz0 = 1;
    int do_h2 = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "32K"))
            sizes[ns++] = 32 * 1024;
        else if (!strcmp(argv[a], "64K"))
            sizes[ns++] = 64 * 1024;
        else if (!strcmp(argv[a], "256K"))
            sizes[ns++] = 256 * 1024;
        else if (!strcmp(argv[a], "1M"))
            sizes[ns++] = 1024 * 1024;
        else if (!strcmp(argv[a], "2M"))
            sizes[ns++] = 2 * 1024 * 1024;
        else if (!strcmp(argv[a], "5M"))
            sizes[ns++] = 5 * 1024 * 1024;
        else if (!strcmp(argv[a], "text")) {
            want_rep = 0;
            want_bin = 0;
        } else if (!strcmp(argv[a], "rep")) {
            want_text = 0;
            want_bin = 0;
        } else if (!strcmp(argv[a], "bin")) {
            want_text = 0;
            want_rep = 0;
        }         else if (!strcmp(argv[a], "--no-zx0"))
            do_zx0 = 0;
        else if (!strcmp(argv[a], "--no-lz0"))
            do_lz0 = 0;
        else if (!strcmp(argv[a], "--no-h2"))
            do_h2 = 0;
    }
    if (ns == 0) {
        sizes[ns++] = 32 * 1024;
        sizes[ns++] = 64 * 1024;
        sizes[ns++] = 256 * 1024;
        sizes[ns++] = 1024 * 1024;
        sizes[ns++] = 2 * 1024 * 1024;
        sizes[ns++] = 5 * 1024 * 1024;
    }
    const char *kinds[3];
    int nk = 0;
    if (want_text)
        kinds[nk++] = "text";
    if (want_rep)
        kinds[nk++] = "rep";
    if (want_bin)
        kinds[nk++] = "bin";
    printf("%-6s %-6s %-6s %10s %8s %10s %10s %8s %10s %8s %12s %10s %12s %6s %6s %6s %6s %s\n",
           "size", "kind", "var", "csize", "ratio", "enc_ms", "find_ms", "sa_ms", "ram_kb", "dec_ms",
           "visits", "checks", "sa_inv", "p4%", "p8%", "p16%", "avgL", "ok");
    fflush(stdout);
    for (int si = 0; si < ns; si++) {
        size_t n = sizes[si];
        const char *sz = szlabel(n);
        for (int k = 0; k < nk; k++) {
            unsigned char *in = make_corpus(kinds[k], n);
            if (!in)
                return 1;
            if (do_zx0 && n <= 64 * 1024)
                run_zx0(sz, kinds[k], n, in);
            if (do_lz0)
                run_lz0(sz, kinds[k], n, in, "C32", 32, 0);
            if (do_h2)
                run_lz0(sz, kinds[k], n, in, "H2C8", 8, 1);
            free(in);
        }
    }
    return 0;
}
