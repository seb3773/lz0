/*
 * Ultra-mode ratio bench: compress each file, verify with the reference
 * decoder, print compressed size and time.
 * usage: ub <mode> files...
 *   mode: l1..l9  -> lz0_config_level(level) + lz0_compress
 *         opt[:L] -> exhaustive optimal parser with offset limit L (0 = none)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lz0_internal.h"
#include "lz0dec.h"

extern int lz0_ultra_inject;
int lz0_parse_opt(lz0_finder *f, lz0_toklist *tl, int offset_limit);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static unsigned char *read_file(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    rewind(f);
    unsigned char *b = malloc(s + 1);
    if (fread(b, 1, s, f) != (size_t)s) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = s;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ub <mode> files...\n");
        return 2;
    }
    const char *mode = argv[1];
    int bad = 0;
    for (int a = 2; a < argc; a++) {
        size_t n;
        unsigned char *in = read_file(argv[a], &n);
        if (!in || !n) {
            fprintf(stderr, "cannot read %s\n", argv[a]);
            bad = 1;
            continue;
        }
        unsigned char *out = NULL;
        size_t osz = 0;
        double t0 = now_ms();
        int rc;
        if (mode[0] == 'l' || mode[0] == 'u') {
            lz0_config cfg;
            lz0_config_default(&cfg, LZ0_PARSE_SDP);
            lz0_config_level(&cfg, mode[0] == 'u' ? 9 : atoi(mode + 1));
            if (mode[0] == 'u')
                cfg.parser = LZ0_PARSE_ULTRA;
            /* experiment overrides */
            const char *e;
            if ((e = getenv("UB_K"))) cfg.max_states = lz0_ultra_states = atoi(e);
            if ((e = getenv("UB_MARGIN"))) lz0_ultra_margin = atoi(e);
            if ((e = getenv("UB_FINE"))) lz0_ultra_fine = atoi(e);
            if ((e = getenv("UB_INJ"))) lz0_ultra_inject = atoi(e);
            if ((e = getenv("UB_CANDS"))) cfg.max_candidates = atoi(e);
            if ((e = getenv("UB_CHAIN"))) cfg.max_chain = atoi(e);
            if ((e = getenv("UB_BT"))) cfg.bt_depth = atoi(e);
            if ((e = getenv("UB_SAWIN"))) lz0_sa_win = atoi(e);
            if ((e = getenv("UB_MSCALE"))) lz0_sa_multiscale = atoi(e);
            if ((e = getenv("UB_LEN2"))) lz0_len2_max_off = atoi(e);
            if ((e = getenv("UB_DEEP"))) lz0_deep_probe = atoi(e);
            if ((e = getenv("UB_RLE"))) lz0_rle_tail = atoi(e);
            rc = lz0_compress(in, n, &out, &osz, &cfg, NULL);
        } else {
            int lim = 0;
            const char *c = strchr(mode, ':');
            if (c)
                lim = atoi(c + 1);
            lz0_finder f;
            memset(&f, 0, sizeof(f));
            f.in = in;
            f.n = (uint32_t)n;
            lz0_toklist tl;
            lz0_toklist_init(&tl);
            lz0_offset_lsb_bits = 8;
            lz0_len_code = 3;
            rc = lz0_parse_opt(&f, &tl, lim);
            if (rc == 0 && !lz0_tokens_valid(&tl, (uint32_t)n))
                rc = -2;
            if (rc == 0)
                rc = lz0_emit(in, (uint32_t)n, &tl, &out, &osz, NULL);
            lz0_toklist_free(&tl);
        }
        double t1 = now_ms();
        int ok = 0;
        if (rc == 0) {
            unsigned char *dec = malloc(n + 16);
            int d = lz0dec_ref(out, (int)osz, dec, (int)n);
            ok = (d == (int)n && memcmp(dec, in, n) == 0);
            free(dec);
        }
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        uint32_t h = 2166136261u;
        for (size_t q = 0; q < osz; q++)
            h = (h ^ out[q]) * 16777619u;
        printf("%-24s %10zu %10zu %9.1f %s %08x\n", base, n, osz, t1 - t0, ok ? "OK" : "FAIL", h);
        if (!ok)
            bad = 1;
        fflush(stdout);
        free(out);
        free(in);
    }
    return bad;
}
