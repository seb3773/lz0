#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include "lz0dec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void) {
    /* 1. Multi-window classification isolation:
     *    Window 0 (16K): uniform high-entropy data
     *    Window 1 (16K): RGB24 3-byte raster with stride 192
     *    Window 2 (16K): ASCII C source text (must NOT inherit high_entropy or bpp=3) */
    const uint32_t n = 48u * 1024u;
    unsigned char *mix = (unsigned char *)malloc(n);
    if (!mix) {
        fprintf(stderr, "OOM\n");
        return 1;
    }
    uint32_t rng = 0xDEADBEEFu;
    for (uint32_t i = 0; i < 16384u; i++) {
        mix[i] = (unsigned char)(xorshift32(&rng) >> 24);
    }
    for (uint32_t y = 0; y < 85u; y++) {
        for (uint32_t x = 0; x < 64u; x++) {
            uint32_t off = 16384u + (y * 64u + x) * 3u;
            if (off + 2u < 32768u) {
                uint32_t pix = ((x >> 2) * 17u + (y >> 1) * 7u) & 0x7fu;
                mix[off + 0] = (unsigned char)pix;
                mix[off + 1] = (unsigned char)((pix + 41u) & 0x7fu);
                mix[off + 2] = (unsigned char)((pix + 82u) & 0x7fu);
            }
        }
    }
    for (uint32_t i = 32768u; i < n; i++) {
        static const char ctext[] = "for (int i = 0; i < count; i++) out[i] += table[i];\n";
        mix[i] = (unsigned char)ctext[(i - 32768u) % (sizeof(ctext) - 1u)];
    }

    lz0_config cfg;
    lz0_config_level(&cfg, 6);
    lz0_finder f;
    if (lz0_finder_init(&f, mix, n, &cfg) != 0) {
        fprintf(stderr, "FAIL: finder_init\n");
        free(mix);
        return 1;
    }
    if (!f.wins || f.n_wins != 3u) {
        fprintf(stderr, "FAIL: expected 3 windows, got %u\n", f.n_wins);
        lz0_finder_free(&f);
        free(mix);
        return 1;
    }
    if (!f.wins[0].is_high_entropy || f.wins[1].is_high_entropy || f.wins[2].is_high_entropy) {
        fprintf(stderr, "FAIL: high_entropy flags = (%u, %u, %u)\n",
                f.wins[0].is_high_entropy, f.wins[1].is_high_entropy, f.wins[2].is_high_entropy);
        lz0_finder_free(&f);
        free(mix);
        return 1;
    }
    if (f.wins[2].bpp != 0 || f.wins[1].bpp == 0) {
        fprintf(stderr, "FAIL: bpp leakage/detection win1=%u win2=%u\n",
                f.wins[1].bpp, f.wins[2].bpp);
        lz0_finder_free(&f);
        free(mix);
        return 1;
    }

    lz0_match cands[64];
    int last_len = 0;
    lz0_finder_insert_upto(&f, 33000u);
    lz0_find_matches(&f, 33000u, 0, cands, &last_len);
    if (f.cur_win != 2u || f.is_high_entropy != 0 || f.bpp != 0) {
        fprintf(stderr, "FAIL: sync_win at pos=33000 gave win=%u he=%u bpp=%u\n",
                f.cur_win, f.is_high_entropy, f.bpp);
        lz0_finder_free(&f);
        free(mix);
        return 1;
    }
    lz0_finder_free(&f);
    free(mix);

    /* 2. Early incompressible bailout:
     *    a) 32K pure pseudo-random -> early bailout (peak_ram == 0, 1 LiteralRun)
     *    b) Duplicated 16K random (16K + identical 16K) -> MUST NOT bail out (~50% compressed) */
    const uint32_t m = 32u * 1024u;
    unsigned char *rnd = (unsigned char *)malloc(m);
    unsigned char *dup = (unsigned char *)malloc(m);
    if (!rnd || !dup) {
        fprintf(stderr, "OOM\n");
        return 1;
    }
    rng = 0x12345678u;
    for (uint32_t i = 0; i < m; i++)
        rnd[i] = (unsigned char)(xorshift32(&rng) >> 24);
    memcpy(dup, rnd, 16384u);
    memcpy(dup + 16384u, rnd, 16384u);

    unsigned char *out = NULL;
    size_t out_sz = 0;
    lz0_stats st;
    lz0_config_level(&cfg, 9);
    if (lz0_compress(rnd, m, &out, &out_sz, &cfg, &st) != 0) {
        fprintf(stderr, "FAIL: compress random\n");
        return 1;
    }
    if (st.peak_ram != 0 || st.n_literals != 1 || st.n_last != 0 || st.n_new != 0) {
        fprintf(stderr, "FAIL: random did not trigger early bailout (ram=%zu lit=%u new=%u)\n",
                st.peak_ram, st.n_literals, st.n_new);
        return 1;
    }
    unsigned char *dec = (unsigned char *)malloc(m + 64u);
    if (!dec || lz0dec_fast(out, (int)out_sz, dec, (int)m) != (int)m ||
        memcmp(dec, rnd, m) != 0) {
        fprintf(stderr, "FAIL: roundtrip random\n");
        return 1;
    }
    free(out);

    out = NULL;
    out_sz = 0;
    if (lz0_compress(dup, m, &out, &out_sz, &cfg, &st) != 0) {
        fprintf(stderr, "FAIL: compress duplicated random\n");
        return 1;
    }
    if (out_sz >= 18000u) {
        fprintf(stderr, "FAIL: false bailout on duplicated 16K random (out_sz=%zu)\n", out_sz);
        return 1;
    }
    if (lz0dec_fast(out, (int)out_sz, dec, (int)m) != (int)m || memcmp(dec, dup, m) != 0) {
        fprintf(stderr, "FAIL: roundtrip dup random\n");
        return 1;
    }
    free(out);
    free(dec);
    free(rnd);
    free(dup);

    printf("PASS: windowed classification + early incompressible bailout OK\n");
    return 0;
}
