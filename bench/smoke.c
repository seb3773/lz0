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

int main(void) {
    int fail = 0;
    const char *s = "the quick brown fox jumps over the lazy dog. the quick brown fox.";
    fail |= check("A1-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_GREEDY);
    fail |= check("A1b-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_GREEDY_COST);
    fail |= check("A2-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_LAZY);
    fail |= check("A2b-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_LAZY2);
    fail |= check("A3-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_DP);
    fail |= check("L1-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_LITE1);
    fail |= check("L2-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_LITE2);
    fail |= check("L1F-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_LITE1F);
    fail |= check("L1FN-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_LITE1FN);
    fail |= check("H2-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_H2);
    fail |= check("SDP-str", (const unsigned char *)s, strlen(s), LZ0_PARSE_SDP);

    unsigned char zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    fail |= check("A1-z", zeros, sizeof(zeros), LZ0_PARSE_GREEDY);
    fail |= check("A1b-z", zeros, sizeof(zeros), LZ0_PARSE_GREEDY_COST);
    fail |= check("A2-z", zeros, sizeof(zeros), LZ0_PARSE_LAZY);
    fail |= check("A2b-z", zeros, sizeof(zeros), LZ0_PARSE_LAZY2);
    fail |= check("A3-z", zeros, sizeof(zeros), LZ0_PARSE_DP);

    unsigned char seq[4096];
    for (int i = 0; i < 4096; i++)
        seq[i] = (unsigned char)i;
    fail |= check("A1-seq", seq, sizeof(seq), LZ0_PARSE_GREEDY);
    fail |= check("A1b-seq", seq, sizeof(seq), LZ0_PARSE_GREEDY_COST);
    fail |= check("A2-seq", seq, sizeof(seq), LZ0_PARSE_LAZY);
    fail |= check("A2b-seq", seq, sizeof(seq), LZ0_PARSE_LAZY2);
    fail |= check("A3-seq", seq, sizeof(seq), LZ0_PARSE_DP);

    unsigned char mixed[8192];
    for (int i = 0; i < 8192; i++)
        mixed[i] = (unsigned char)((i * 7) ^ (i >> 3));
    memcpy(mixed + 2000, mixed, 1500);
    memcpy(mixed + 5000, mixed + 100, 800);
    fail |= check("A1-mix", mixed, sizeof(mixed), LZ0_PARSE_GREEDY);
    fail |= check("A1b-mix", mixed, sizeof(mixed), LZ0_PARSE_GREEDY_COST);
    fail |= check("A2-mix", mixed, sizeof(mixed), LZ0_PARSE_LAZY);
    fail |= check("A2b-mix", mixed, sizeof(mixed), LZ0_PARSE_LAZY2);
    fail |= check("A3-mix", mixed, sizeof(mixed), LZ0_PARSE_DP);
    fail |= check("L1-mix", mixed, sizeof(mixed), LZ0_PARSE_LITE1);
    fail |= check("L2-mix", mixed, sizeof(mixed), LZ0_PARSE_LITE2);
    fail |= check("L1F-mix", mixed, sizeof(mixed), LZ0_PARSE_LITE1F);
    fail |= check("L1FN-mix", mixed, sizeof(mixed), LZ0_PARSE_LITE1FN);
    fail |= check("H2-mix", mixed, sizeof(mixed), LZ0_PARSE_H2);
    fail |= check("SDP-mix", mixed, sizeof(mixed), LZ0_PARSE_SDP);

    return fail ? 1 : 0;
}
