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

static uint64_t hist_pct(const uint64_t *h, uint64_t total, double pct) {
    if (!total)
        return 0;
    uint64_t need = (uint64_t)((double)total * pct);
    if (need == 0)
        need = 1;
    uint64_t acc = 0;
    for (int b = 0; b < 32; b++) {
        acc += h[b];
        if (acc >= need)
            return 1ull << b;
    }
    return 1ull << 31;
}

static void cfg_h2c8(lz0_config *cfg) {
    lz0_config_default(cfg, LZ0_PARSE_SDP);
    lz0_h2c8_apply();
}

static void reset_diag(void) {
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

static void run_one(const char *kind, size_t n, const unsigned char *in) {
    lz0_config cfg;
    cfg_h2c8(&cfg);
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    lz0_sdp_census cen;
    memset(&cen, 0, sizeof(cen));
    lz0_sdp_census_out = &cen;
    long rss0 = rss_kb();
    int rc = lz0_compress(in, n, &comp, &csz, &cfg, &st);
    long rss1 = rss_kb();
    lz0_sdp_census_out = NULL;
    if (rc != 0) {
        printf("=== %s %s H2C8 FAIL ===\n", szlabel(n), kind);
        reset_diag();
        return;
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
    uint32_t nn = (uint32_t)n;
    size_t ram_hash = (size_t)nn * sizeof(uint32_t) +
                      (size_t)LZ0_HASH_SIZE * sizeof(uint32_t);
    size_t ram_sa = (size_t)nn * sizeof(uint32_t) * 2u;
    size_t ram_sdp = lz0_ram_sdp(nn, 2);
    double sab = cen.ms_sa_build > 0 ? cen.ms_sa_build : lz0_sa_build_ms;
    double find_hash = cen.ms_find - cen.ms_sa_probe;
    if (find_hash < 0)
        find_hash = 0;
    uint64_t ntok = (uint64_t)st.n_literals + (uint64_t)st.n_last + (uint64_t)st.n_new;
    double p4 = cen.n_sa_invoke ? 100.0 * (double)cen.n_sa_stop4 / (double)cen.n_sa_invoke : 0;
    double p8 = cen.n_sa_invoke ? 100.0 * (double)cen.n_sa_stop8 / (double)cen.n_sa_invoke : 0;
    double p16 = cen.n_sa_invoke ? 100.0 * (double)cen.n_sa_stop16 / (double)cen.n_sa_invoke : 0;
    double avg_ml = cen.n_matchlen ? (double)cen.sum_ml / (double)cen.n_matchlen : 0;
    double avg_cl = cen.n_cand ? (double)cen.sum_cand_len / (double)cen.n_cand : 0;
    double avg_st = cen.n_pos ? (double)cen.sum_states / (double)cen.n_pos : 0;
    uint64_t p50 = hist_pct(cen.hist_ml, cen.n_matchlen, 0.50);
    uint64_t p90 = hist_pct(cen.hist_ml, cen.n_matchlen, 0.90);
    uint64_t p99 = hist_pct(cen.hist_ml, cen.n_matchlen, 0.99);

    printf("=== %s %s H2C8 csz=%zu ratio=%.2f%% enc=%.1f parse=%.1f emit=%.1f dec=%.2f %s ===\n",
           szlabel(n), kind, csz, 100.0 * (double)csz / (double)n,
           st.total_ms, st.parse_ms, st.emit_ms, dec, ok ? "ok" : "FAIL");
    printf("  SA    build=%.1f probe=%.1f inv=%llu cmp=%llu mlB=%llu stop4=%.1f%% stop8=%.1f%% stop16=%.1f%% ram=%zuKB\n",
           sab, cen.ms_sa_probe,
           (unsigned long long)cen.n_sa_invoke, (unsigned long long)cen.n_sa_cmp,
           (unsigned long long)cen.n_sa_ml_bytes, p4, p8, p16, ram_sa / 1024);
    printf("  SAbd  init=%.1f k2=%.1f csort=%.1f copy=%.1f rerank=%.1f final=%.1f passes=%u csort_n=%llu (prefix-doubling + counting-sort)\n",
           cen.ms_sa_init, cen.ms_sa_k2, cen.ms_sa_csort, cen.ms_sa_copy,
           cen.ms_sa_rerank, cen.ms_sa_final, cen.n_sa_passes,
           (unsigned long long)cen.n_sa_csort_n);
    printf("  csort clear=%.1f count=%.1f prefix=%.1f scatter=%.1f\n",
           cen.ms_cs_clear, cen.ms_cs_count, cen.ms_cs_prefix, cen.ms_cs_scatter);
    printf("  find  total=%.1f hash=%.1f insert=%.1f visit=%llu check=%llu pow2=%llu nfind=%llu\n",
           cen.ms_find, find_hash, cen.ms_insert,
           (unsigned long long)cen.n_chain_visit, (unsigned long long)cen.n_chain_check,
           (unsigned long long)cen.n_pow2, (unsigned long long)cen.n_find);
    {
        uint64_t vis = cen.n_chain_visit ? cen.n_chain_visit : 1;
        uint64_t nsa = cen.n_sa_invoke ? cen.n_sa_invoke : 1;
        uint64_t nml = cen.n_sa_ml ? cen.n_sa_ml : 1;
        printf("  hashx pow2=%.1f chain=%.1f hash=%llu head=%llu prev=%llu vis=%llu ml=%llu ok=%llu rej0=%llu rejp=%llu ns/vis=%.2f\n",
               cen.ms_hash_pow2, cen.ms_hash_chain,
               (unsigned long long)cen.n_hash_comp, (unsigned long long)cen.n_head,
               (unsigned long long)cen.n_prev, (unsigned long long)cen.n_chain_visit,
               (unsigned long long)cen.n_chain_ml, (unsigned long long)cen.n_chain_ok,
               (unsigned long long)cen.n_chain_rej_b0, (unsigned long long)cen.n_chain_rej_probe,
               1e6 * cen.ms_hash_chain / (double)vis);
        printf("  hashx pow2ml=%llu pow2ok=%llu cand_preSA=%llu\n",
               (unsigned long long)cen.n_pow2_ml, (unsigned long long)cen.n_pow2_ok,
               (unsigned long long)cen.n_cand_hash);
        printf("  sax   saprobe=%.1f src_skip=%llu dup=%llu newkept=%llu newdrop=%llu ns/inv=%.1f ns/ml=%.2f\n",
               cen.ms_sa_probe,
               (unsigned long long)cen.n_sa_src_skip, (unsigned long long)cen.n_sa_dup,
               (unsigned long long)cen.n_sa_new_kept, (unsigned long long)cen.n_sa_new_drop,
               1e6 * cen.ms_sa_probe / (double)nsa, 1e6 * cen.ms_sa_probe / (double)nml);
    }
    printf("  ml    calls=%llu bytes=%llu avg=%.1f p50~%llu p90~%llu p99~%llu max=%llu last_len=%llu\n",
           (unsigned long long)cen.n_matchlen, (unsigned long long)cen.sum_ml,
           avg_ml, (unsigned long long)p50, (unsigned long long)p90, (unsigned long long)p99,
           (unsigned long long)cen.max_ml, (unsigned long long)cen.n_last_len);
    printf("  hist  ");
    for (int b = 0; b < 24; b++) {
        if (cen.hist_ml[b])
            printf("%d:%llu ", b, (unsigned long long)cen.hist_ml[b]);
    }
    printf("\n");
    printf("  cand  kept=%llu full=%llu avgL=%.1f maxL=%llu\n",
           (unsigned long long)cen.n_cand, (unsigned long long)cen.n_pos_full,
           avg_cl, (unsigned long long)cen.max_cand_len);
    printf("  parse trans=%.1f back=%.1f avg_st=%.2f max_st=%u lit=%llu last=%llu new=%llu skip=%llu emit_new=%llu tok=%llu\n",
           cen.ms_trans, cen.ms_back, avg_st, cen.max_states,
           (unsigned long long)cen.n_trans_lit, (unsigned long long)cen.n_trans_last,
           (unsigned long long)cen.n_trans_new, (unsigned long long)cen.n_skip_off,
           (unsigned long long)cen.n_emit_new, (unsigned long long)ntok);
    printf("  trans lit=%.1f lastlen=%.1f last_emit=%.1f new_emit=%.1f\n",
           cen.ms_trans_lit, cen.ms_trans_lastlen, cen.ms_trans_last, cen.ms_trans_new);
    {
        uint64_t nplat = cen.n_new_plat_fixed + cen.n_new_plat_hi + cen.n_new_plat_ovf;
        double avg_plat = cen.n_emit_new ? (double)nplat / (double)cen.n_emit_new : 0;
        printf("  newx  plat_fixed=%llu plat_hi=%llu plat_ovf=%llu plat_tot=%llu avg_plat/emit=%.2f last_plat=%llu\n",
               (unsigned long long)cen.n_new_plat_fixed, (unsigned long long)cen.n_new_plat_hi,
               (unsigned long long)cen.n_new_plat_ovf, (unsigned long long)nplat, avg_plat,
               (unsigned long long)cen.n_last_plat);
    }
    double avg_sa_ml = cen.n_sa_ml ? (double)cen.sum_sa_ml / (double)cen.n_sa_ml : 0;
    printf("  SAprb ml=%llu ok=%llu avg=%.1f max=%llu bytes=%llu\n",
           (unsigned long long)cen.n_sa_ml, (unsigned long long)cen.n_sa_ml_ok,
           avg_sa_ml, (unsigned long long)cen.max_sa_ml,
           (unsigned long long)cen.n_sa_ml_bytes);
    double cpu_enc = cen.ms_sa_build + cen.ms_insert + cen.ms_find + cen.ms_trans + cen.ms_back;
    if (cpu_enc > 0) {
        printf("  cpu%% build=%.1f insert=%.1f hash=%.1f saprobe=%.1f trans=%.1f (lit=%.1f lastlen=%.1f last=%.1f new=%.1f) back=%.1f  (cpu_sum=%.1f)\n",
               100.0 * cen.ms_sa_build / cpu_enc,
               100.0 * cen.ms_insert / cpu_enc,
               100.0 * find_hash / cpu_enc,
               100.0 * cen.ms_sa_probe / cpu_enc,
               100.0 * cen.ms_trans / cpu_enc,
               100.0 * cen.ms_trans_lit / cpu_enc,
               100.0 * cen.ms_trans_lastlen / cpu_enc,
               100.0 * cen.ms_trans_last / cpu_enc,
               100.0 * cen.ms_trans_new / cpu_enc,
               100.0 * cen.ms_back / cpu_enc, cpu_enc);
    }
    printf("  tok   lit=%u last=%u new=%u bits=%llu\n",
           st.n_literals, st.n_last, st.n_new, (unsigned long long)st.bits_total);
    size_t ram_prev = (size_t)nn * sizeof(uint32_t);
    size_t ram_jmp = 0;
    size_t ram_head = (size_t)LZ0_HASH_SIZE * sizeof(uint32_t);
    size_t ram_sa_only = (size_t)nn * sizeof(uint32_t);
    size_t ram_rank = (size_t)nn * sizeof(uint32_t);
    size_t ram_sdp_st = (size_t)(nn + 1) * 2u * 12u;
    size_t ram_sdp_ns = (size_t)(nn + 1);
    printf("  ram   peak=%.0fKB hash=%zuKB(prev=%zu jmp=%zu head=%zu) sa=%zuKB(sa=%zu rank=%zu) sdp=%zuKB(st=%zu ns=%zu) rss0=%ld rss1=%ld\n",
           st.peak_ram / 1024.0, ram_hash / 1024, ram_prev / 1024, ram_jmp / 1024, ram_head / 1024,
           ram_sa / 1024, ram_sa_only / 1024, ram_rank / 1024,
           ram_sdp / 1024, ram_sdp_st / 1024, ram_sdp_ns / 1024, rss0, rss1);
    printf("  dec   %.2f ms  %.1f MB/s\n",
           dec, dec > 0 ? ((double)n / 1e6) / (dec / 1000.0) : 0);
    reset_diag();
    free(comp);
    fflush(stdout);
}

int main(int argc, char **argv) {
    size_t n = 256 * 1024;
    int kind_only = -1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "256K"))
            n = 256 * 1024;
        else if (!strcmp(argv[a], "1M"))
            n = 1024 * 1024;
        else if (!strcmp(argv[a], "2M"))
            n = 2 * 1024 * 1024;
        else if (!strcmp(argv[a], "5M"))
            n = 5 * 1024 * 1024;
        else if (!strcmp(argv[a], "text"))
            kind_only = 0;
        else if (!strcmp(argv[a], "rep"))
            kind_only = 1;
        else if (!strcmp(argv[a], "bin"))
            kind_only = 2;
        else if (!strcmp(argv[a], "v2"))
            lz0_sa_csort_v2 = 1;
        else if (!strcmp(argv[a], "pf"))
            lz0_sa_csort_smode = 1;
        else if (!strcmp(argv[a], "base")) {
            lz0_sa_csort_v2 = 0;
            lz0_sa_csort_smode = 0;
            lz0_sa_count_mode = 0;
        } else if (!strncmp(argv[a], "look=", 5))
            lz0_sa_pf_look = atoi(argv[a] + 5);
        else if (!strncmp(argv[a], "cm=", 3))
            lz0_sa_count_mode = atoi(argv[a] + 3);
    }
    const char *kinds[] = {"text", "rep", "bin"};
    for (int k = 0; k < 3; k++) {
        if (kind_only >= 0 && k != kind_only)
            continue;
        unsigned char *in = make_corpus(kinds[k], n);
        if (!in)
            return 1;
        run_one(kinds[k], n, in);
        free(in);
    }
    return 0;
}
