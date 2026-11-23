/*
 * find_bench.c - Match-finder (lz0_find_matches) speed + exactness bench.
 *
 * Usage: lz0_find_bench <reps> <levels-csv> file...
 * For every (file, level) the finder is driven alone, the way the parsers
 * drive it (insert_upto(pos) then find_matches(pos) at every position), and
 * the full candidate list of every position is folded into an FNV-1a hash.
 * Two builds produce identical candidate lists iff all hashes match. Time is
 * best-of-<reps> wall clock of the find loop; on x86-64 the TSC delta
 * (reference cycles) of the same run is printed too.
 */
#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
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
static unsigned long long tsc(void) {
#if defined(__x86_64__) && defined(__GNUC__)
    return (unsigned long long)__builtin_ia32_rdtsc();
#else
    return 0;
#endif
}
static uint64_t fnv_u32(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 1099511628211ull;
    }
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
    for (char *s = strtok(argv[2], ","); s && nl < 16; s = strtok(NULL, ","))
        levels[nl++] = atoi(s);
    double tot_ms[16] = {0};
    for (int a = 3; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        if (!fp) { perror(argv[a]); return 1; }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        unsigned char *in = malloc((size_t)n);
        if (!in || fread(in, 1, (size_t)n, fp) != (size_t)n) return 1;
        fclose(fp);
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        for (int li = 0; li < nl; li++) {
            double best = 1e30;
            unsigned long long best_cyc = 0;
            uint64_t h = 0, ncand = 0;
            for (int r = 0; r < reps; r++) {
                lz0_config cfg;
                lz0_config_level(&cfg, levels[li]);
                if (getenv("LZ0_SIMD"))
                    lz0_simd_force = atoi(getenv("LZ0_SIMD"));
                lz0_finder f;
                if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) { printf("INIT FAIL\n"); return 1; }
                lz0_match cands[128];
                uint64_t hh = 1469598103934665603ull, nc = 0;
                double t0 = now_ms();
                unsigned long long c0 = tsc();
                for (uint32_t pos = 0; pos < (uint32_t)n; pos++) {
                    int last_len = 0;
                    lz0_finder_insert_upto(&f, pos);
                    int k = lz0_find_matches(&f, pos, pos > 1 ? 1u : 0u, cands, &last_len);
                    hh = fnv_u32(hh, (uint32_t)k);
                    hh = fnv_u32(hh, (uint32_t)last_len);
                    for (int i = 0; i < k; i++) {
                        hh = fnv_u32(hh, cands[i].offset);
                        hh = fnv_u32(hh, cands[i].length);
                    }
                    nc += (uint64_t)k;
                }
                unsigned long long c1 = tsc();
                double t = now_ms() - t0;
                lz0_finder_free(&f);
                if (t < best) { best = t; best_cyc = c1 - c0; }
                if (r && hh != h) { printf("NONDETERMINISTIC %s\n", base); return 1; }
                h = hh;
                ncand = nc;
            }
            tot_ms[li] += best;
            printf("%-14s L%d %8ld cand=%9llu %016llx %9.2f ms %12llu tsc %7.2f tsc/B\n", base,
                   levels[li], n, (unsigned long long)ncand, (unsigned long long)h, best, best_cyc,
                   (double)best_cyc / (double)n);
            fflush(stdout);
        }
        free(in);
    }
    for (int li = 0; li < nl; li++)
        printf("TOTAL L%d %10.2f ms\n", levels[li], tot_ms[li]);
    printf("SIMD tier: %s\n", lz0_simd_name());
    return 0;
}
