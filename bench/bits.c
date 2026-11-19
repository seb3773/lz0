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

static void dump_hist(const char *lab, const uint64_t *h) {
    printf("  %s", lab);
    for (int i = 0; i < 8; i++)
        printf(" [%d]=%llu", i, (unsigned long long)h[i]);
    printf("\n");
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void run_one(const char *kind, size_t n, const unsigned char *in, int prune, int lsb, int lenc, int k) {
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    cfg.min_match = 2;
    cfg.max_chain = 32;
    cfg.max_candidates = 16;
    cfg.max_states = k;
    cfg.jump_levels = 16;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_stream_census sc;
    lz0_sdp_census sdp;
    memset(&sc, 0, sizeof(sc));
    memset(&sdp, 0, sizeof(sdp));
    lz0_stream_census_out = &sc;
    lz0_sdp_census_out = &sdp;
    lz0_chain_prune = prune;
    lz0_offset_lsb_bits = lsb;
    lz0_len_code = lenc;
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    lz0_stream_census_out = NULL;
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        lz0_chain_prune = 0;
        lz0_offset_lsb_bits = 8;
        lz0_len_code = 3;
        printf("%zu %s P=%d LSB=%d LEN=%d K=%d FAIL\n", n, kind, prune, lsb, lenc, k);
        return;
    }
    unsigned char *dec = (unsigned char *)malloc(n);
    double t0 = now_ms();
    int d = lz0_decompress_to(comp, (int)csz, dec, (int)n);
    double dec_ms = now_ms() - t0;
    int rt = (d == (int)n && dec && memcmp(dec, in, n) == 0);
    free(dec);
    lz0_chain_prune = 0;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
    uint64_t bits_lit = sc.bits_lit_flag + sc.bits_lit_len + sc.bits_lit_pay;
    uint64_t bits_last = sc.bits_last_flag + sc.bits_last_len;
    uint64_t bits_new = sc.bits_new_flag + sc.bits_new_off_elias + sc.bits_new_off_lsb + sc.bits_new_len;
    uint64_t bits_off = sc.bits_new_off_elias + sc.bits_new_off_lsb;
    uint64_t bits_len = sc.bits_lit_len + sc.bits_last_len + sc.bits_new_len;
    uint64_t bits_flag = sc.bits_lit_flag + sc.bits_last_flag + sc.bits_new_flag + sc.bits_eof;
    uint64_t bits_tot = bits_lit + bits_last + bits_new + sc.bits_eof;
    double pct = bits_tot ? 100.0 / (double)bits_tot : 0;
    printf("=== n=%zu %s P=%d LSB=%d LEN=%d K=%d csz=%zu ratio=%.2f%% bits=%llu enc=%.1f parse=%.1f ram=%.0fKB dec=%.1f %s ===\n",
           n, kind, prune, lsb, lenc, k, csz, 100.0 * (double)csz / (double)n,
           (unsigned long long)bits_tot, st.total_ms, st.parse_ms, st.peak_ram / 1024.0,
           dec_ms, rt ? "ok" : "FAIL");
    printf("  sdp   avg_st=%.2f max_st=%u evict=%llu n_pos=%u parse=%.1f find=%.1f\n",
           sdp.n_pos ? (double)sdp.sum_states / (double)sdp.n_pos : 0,
           sdp.max_states, (unsigned long long)sdp.n_evicted, sdp.n_pos,
           sdp.ms_trans, sdp.ms_find);
    printf("  tok   lit=%llu last=%llu new=%llu  bytes lit=%llu last=%llu new=%llu\n",
           (unsigned long long)sc.n_lit, (unsigned long long)sc.n_last,
           (unsigned long long)sc.n_new, (unsigned long long)sc.bytes_lit,
           (unsigned long long)sc.bytes_last, (unsigned long long)sc.bytes_new);
    printf("  bits  lit=%.1f%% last=%.1f%% new=%.1f%% eof=%.1f%%\n",
           (double)bits_lit * pct, (double)bits_last * pct,
           (double)bits_new * pct, (double)sc.bits_eof * pct);
    printf("  split flag=%.1f%% len=%.1f%% off=%.1f%% pay=%.1f%%\n",
           (double)bits_flag * pct, (double)bits_len * pct,
           (double)bits_off * pct, (double)sc.bits_lit_pay * pct);
    printf("  new   flag=%llu elias=%llu lsb=%llu len=%llu avg_off=%.0f\n",
           (unsigned long long)sc.bits_new_flag,
           (unsigned long long)sc.bits_new_off_elias,
           (unsigned long long)sc.bits_new_off_lsb,
           (unsigned long long)sc.bits_new_len,
           sc.n_new ? (double)sc.sum_off / (double)sc.n_new : 0);
    dump_hist("len2k", sc.hist_len);
    dump_hist("off2k", sc.hist_off);
    printf("  exact");
    for (int i = 1; i <= 16; i++)
        printf(" L%d=%llu/%llub", i, (unsigned long long)sc.nlen[i],
               (unsigned long long)sc.blen[i]);
    printf(" >32=%llu/%llub\n", (unsigned long long)sc.nlen[32],
           (unsigned long long)sc.blen[32]);
    printf("  newL");
    for (int i = 1; i <= 16; i++)
        printf(" %d=%llu", i, (unsigned long long)sc.nlen_new[i]);
    printf(" >32=%llu\n", (unsigned long long)sc.nlen_new[32]);
    printf("  lastL");
    for (int i = 1; i <= 16; i++)
        printf(" %d=%llu", i, (unsigned long long)sc.nlen_last[i]);
    printf(" >32=%llu\n", (unsigned long long)sc.nlen_last[32]);
    printf("  litL");
    for (int i = 1; i <= 16; i++)
        printf(" %d=%llu", i, (unsigned long long)sc.nlen_lit[i]);
    printf(" >32=%llu\n", (unsigned long long)sc.nlen_lit[32]);
    free(comp);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int do_32 = 1, do_256 = 1, do_1m = 0, do_2m = 0, do_5m = 0;
    int prunes[4];
    int nprune = 0;
    int lsbs[4];
    int nlsb = 0;
    int lencs[4];
    int nlenc = 0;
    int ks[8];
    int nks = 0;
    int want_text = 1, want_rep = 1, want_bin = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "1M"))
            do_1m = 1;
        else if (!strcmp(argv[a], "2M"))
            do_2m = 1;
        else if (!strcmp(argv[a], "5M"))
            do_5m = 1;
        else if (!strcmp(argv[a], "--no-32"))
            do_32 = 0;
        else if (!strcmp(argv[a], "--no-256"))
            do_256 = 0;
        else if (!strncmp(argv[a], "P=", 2))
            prunes[nprune++] = atoi(argv[a] + 2);
        else if (!strncmp(argv[a], "LSB=", 4))
            lsbs[nlsb++] = atoi(argv[a] + 4);
        else if (!strncmp(argv[a], "LEN=", 4))
            lencs[nlenc++] = atoi(argv[a] + 4);
        else if (!strncmp(argv[a], "K=", 2))
            ks[nks++] = atoi(argv[a] + 2);
        else if (!strcmp(argv[a], "text")) {
            want_rep = 0;
            want_bin = 0;
        } else if (!strcmp(argv[a], "rep")) {
            want_text = 0;
            want_bin = 0;
        } else if (!strcmp(argv[a], "bin")) {
            want_text = 0;
            want_rep = 0;
        }
    }
    const char *kinds[3];
    int nk = 0;
    if (want_text)
        kinds[nk++] = "text";
    if (want_rep)
        kinds[nk++] = "rep";
    if (want_bin)
        kinds[nk++] = "bin";
    size_t szs[6];
    int ns = 0;
    if (do_32)
        szs[ns++] = 32 * 1024;
    if (do_256)
        szs[ns++] = 256 * 1024;
    if (do_1m)
        szs[ns++] = 1024 * 1024;
    if (do_2m)
        szs[ns++] = 2 * 1024 * 1024;
    if (do_5m)
        szs[ns++] = 5 * 1024 * 1024;
    if (nprune == 0) {
        prunes[0] = 0;
        nprune = 1;
    }
    if (nlsb == 0) {
        lsbs[0] = 8;
        nlsb = 1;
    }
    if (nlenc == 0) {
        lencs[0] = 3;
        nlenc = 1;
    }
    if (nks == 0) {
        ks[0] = 2;
        nks = 1;
    }
    for (int si = 0; si < ns; si++) {
        for (int k = 0; k < nk; k++) {
            unsigned char *in = make_corpus(kinds[k], szs[si]);
            if (!in)
                return 1;
            for (int pi = 0; pi < nprune; pi++)
                for (int li = 0; li < nlsb; li++)
                    for (int ei = 0; ei < nlenc; ei++)
                        for (int ki = 0; ki < nks; ki++)
                            run_one(kinds[k], szs[si], in, prunes[pi], lsbs[li], lencs[ei], ks[ki]);
            free(in);
        }
    }
    return 0;
}
