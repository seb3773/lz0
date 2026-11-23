/*
 * enc_bench.c - Encoder speed and bit-exactness bench.
 *
 * Usage: lz0_enc_bench <reps> <levels-csv> file...
 * Prints one line per (file, level): input size, compressed size, FNV-1a
 * 64-bit hash of the compressed stream, best-of-<reps> encode time, MB/s.
 * Every stream is decoded by the reference, FAST and compact decoders and
 * compared with the input. Two runs (e.g. before/after an optimisation, or
 * two SIMD tiers) are bit-identical iff all hashes match.
 *
 * Environment (bench only, never read by the library):
 *   LZ0_SIMD=<n>   cap the encoder SIMD tier (0 = C, 1 = SSE2/NEON/RVV, 2 = AVX2)
 *   LZ0_JL=<n>     override the hash-chain skip-list depth (cfg.jump_levels)
 */
#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0dec.h"
#include "lz0_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static uint64_t fnv(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <reps> <levels-csv> file...\n", argv[0]);
        return 2;
    }
    int reps = atoi(argv[1]);
    if (reps < 1)
        reps = 1;
    int levels[16], nl = 0;
    for (char *s = strtok(argv[2], ","); s; s = strtok(NULL, ",")) levels[nl++] = atoi(s);
    double tot_ms[16] = {0};
    for (int a = 3; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        if (!fp) { perror(argv[a]); return 1; }
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        unsigned char *in = malloc(n), *dec = malloc(n + 64);
        if (fread(in, 1, n, fp) != (size_t)n) return 1;
        fclose(fp);
        const char *base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        for (int li = 0; li < nl; li++) {
            double best = 1e30; size_t osz = 0; uint64_t h = 0;
            for (int r = 0; r < reps; r++) {
                lz0_config cfg; lz0_config_level(&cfg, levels[li]);
                if (getenv("LZ0_JL"))
                    cfg.jump_levels = atoi(getenv("LZ0_JL"));
                if (getenv("LZ0_SIMD"))
                    lz0_simd_force = atoi(getenv("LZ0_SIMD"));
                unsigned char *out = NULL; size_t os = 0;
                double t0 = now_ms();
                if (lz0_compress(in, n, &out, &os, &cfg, NULL) < 0) { printf("FAIL %s\n", base); return 1; }
                double t = now_ms() - t0;
                if (t < best) best = t;
                uint64_t hh = fnv(out, os);
                if (r && hh != h) { printf("NONDETERMINISTIC %s\n", base); return 1; }
                h = hh; osz = os;
                if (r == 0) {
                    int d1 = lz0dec_ref(out, (int)os, dec, (int)n);
                    int d2 = lz0dec_fast(out, (int)os, dec, (int)n);
                    int d3 = lz0dec_compact(out, (int)os, dec, (int)n);
                    if (d1 != n || d2 != n || d3 != n || memcmp(dec, in, n)) { printf("DECODE MISMATCH %s L%d\n", base, levels[li]); return 1; }
                }
                free(out);
            }
            tot_ms[li] += best;
            printf("%-16s L%d %9ld %9zu %016llx %10.2f ms %8.2f MB/s\n", base, levels[li], n, osz,
                   (unsigned long long)h, best, n / 1048576.0 / (best / 1e3));
            fflush(stdout);
        }
        free(in); free(dec);
    }
    for (int li = 0; li < nl; li++) printf("TOTAL L%d %10.2f ms\n", levels[li], tot_ms[li]);
    printf("SIMD tier: %s\n", lz0_simd_name());
    return 0;
}
