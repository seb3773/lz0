#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz0.h"

static int check(const char *name, const unsigned char *in, size_t n, lz0_parse_t p) {
    lz0_config cfg;
    lz0_config_default(&cfg, p);
    cfg.max_chain = 16;
    cfg.max_candidates = 8;
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    if (lz0_compress(in, n, &comp, &csz, &cfg, &st) != 0) {
        printf("%s: compress fail\n", name);
        return 1;
    }
    unsigned char *out = (unsigned char *)malloc(n + 8);
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    int bad = (d != (int)n) || memcmp(out, in, n);
    printf("%s n=%zu csz=%zu ratio=%.2f%% enc=%.2fms %s\n",
           name, n, csz, 100.0 * csz / n, st.total_ms, bad ? "FAIL" : "ok");
    free(comp);
    free(out);
    return bad;
}

static int check_level(const char *name, const unsigned char *in, size_t n, int level) {
    lz0_config cfg;
    lz0_config_level(&cfg, level);
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_stats st;
    if (lz0_compress(in, n, &comp, &csz, &cfg, &st) != 0) {
        printf("%s: compress fail\n", name);
        return 1;
    }
    unsigned char *out = (unsigned char *)malloc(n + 8);
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    int bad = (d != (int)n) || memcmp(out, in, n);
    printf("%s n=%zu csz=%zu ratio=%.2f%% enc=%.2fms %s\n",
           name, n, csz, 100.0 * csz / n, st.total_ms, bad ? "FAIL" : "ok");
    free(comp);
    free(out);
    return bad;
}

int main(void) {
    int fail = 0;
    const char *s = "the quick brown fox jumps over the lazy dog. the quick brown fox.";
    fail |= check("G-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_GREEDY);
    fail |= check("SDP-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_SDP);
    fail |= check("ULT-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_ULTRA);
    fail |= check_level("L1-str", (const unsigned char *)s, strlen(s), 1);
    fail |= check_level("L2-str", (const unsigned char *)s, strlen(s), 2);
    fail |= check_level("L6-str", (const unsigned char *)s, strlen(s), 6);
    fail |= check_level("L8-str", (const unsigned char *)s, strlen(s), 8);
    fail |= check_level("L9-str", (const unsigned char *)s, strlen(s), 9);

    unsigned char zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    fail |= check_level("L1-z", zeros, sizeof(zeros), 1);
    fail |= check_level("L6-z", zeros, sizeof(zeros), 6);
    fail |= check_level("L9-z", zeros, sizeof(zeros), 9);

    unsigned char seq[4096];
    for (int i = 0; i < 4096; i++)
        seq[i] = (unsigned char)i;
    fail |= check_level("L1-seq", seq, sizeof(seq), 1);
    fail |= check_level("L6-seq", seq, sizeof(seq), 6);
    fail |= check_level("L9-seq", seq, sizeof(seq), 9);

    unsigned char mixed[8192];
    for (int i = 0; i < 8192; i++)
        mixed[i] = (unsigned char)((i * 7) ^ (i >> 3));
    memcpy(mixed + 2000, mixed, 1500);
    memcpy(mixed + 5000, mixed + 100, 800);
    fail |= check("G-mix", mixed, sizeof(mixed), LZ0_PARSE_GREEDY);
    fail |= check("SDP-mix", mixed, sizeof(mixed), LZ0_PARSE_SDP);
    fail |= check("ULT-mix", mixed, sizeof(mixed), LZ0_PARSE_ULTRA);
    fail |= check_level("L1-mix", mixed, sizeof(mixed), 1);
    fail |= check_level("L2-mix", mixed, sizeof(mixed), 2);
    fail |= check_level("L6-mix", mixed, sizeof(mixed), 6);
    fail |= check_level("L8-mix", mixed, sizeof(mixed), 8);
    fail |= check_level("L9-mix", mixed, sizeof(mixed), 9);

    return fail ? 1 : 0;
}
