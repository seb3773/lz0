/*
 * test_simd.c - Differential test of the encoder SIMD kernels.
 *
 * Every available tier is checked against the portable C reference on
 * random inputs of every length 0..300 (heavy ties, duplicates, extreme
 * values). Inputs are placed right before a PROT_NONE guard page so any
 * read past the last element faults immediately.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "lz0_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

static unsigned char *guard_base;
static size_t guard_len;

/* Returns a pointer such that [p, p + bytes) ends exactly at a guard page. */
static void *guarded(size_t bytes) {
    long pg = sysconf(_SC_PAGESIZE);
    if (!guard_base) {
        guard_len = (size_t)pg * 8;
        guard_base = mmap(NULL, guard_len + (size_t)pg, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (guard_base == MAP_FAILED || mprotect(guard_base + guard_len, (size_t)pg, PROT_NONE)) {
            perror("mmap/mprotect");
            exit(2);
        }
    }
    return guard_base + guard_len - bytes;
}

static uint32_t pick(uint32_t range) {
    switch (rnd() % 4) {
    case 0: return rnd();
    case 1: return rnd() % 4;
    case 2: return 0x80000000u + rnd() % 3;
    default: return rnd() % (range ? range : 1);
    }
}

static int fails = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL [%s] ", lz0_simd_name());                 \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
            if (++fails > 20)                                               \
                exit(1);                                                    \
        }                                                                   \
    } while (0)

static void run_tier(int iters) {
    for (int it = 0; it < iters; it++) {
        int n = (int)(rnd() % 301);
        uint32_t range = 1 + rnd() % 64;

        uint32_t *k = guarded((size_t)n * 4);
        for (int i = 0; i < n; i++)
            k[i] = pick(range);
        uint32_t key = (n && rnd() % 2) ? k[rnd() % n] : pick(range);
        CHECK(lz0_simd_find_u32(k, n, key) == lz0_find_u32_c(k, n, key), "find_u32 n=%d", n);
        if (n >= 1) {
            int32_t *c = (int32_t *)k;
            CHECK(lz0_simd_argmax_i32(c, n) == lz0_argmax_i32_c(c, n), "argmax n=%d", n);
        }

        int nm = n > 64 ? n % 65 : n;
        lz0_match *m = guarded((size_t)nm * sizeof(lz0_match));
        for (int i = 0; i < nm; i++) {
            m[i].offset = pick(range);
            m[i].length = pick(range);
        }
        uint32_t off = (nm && rnd() % 2) ? m[rnd() % nm].length : pick(range);
        if (rnd() % 2 && nm)
            off = m[rnd() % nm].offset;
        CHECK(lz0_simd_find_off(m, nm, off) == lz0_find_off_c(m, nm, off), "find_off n=%d", nm);
        if (nm >= 1) {
            /* score array placed in a separate, also guarded, buffer */
            int32_t sc_local[64];
            for (int i = 0; i < nm; i++)
                sc_local[i] = (int32_t)(rnd() % 2 ? (int32_t)(rnd() % 5) - 2 : (int32_t)rnd());
            lz0_match mcopy[64];
            memcpy(mcopy, m, (size_t)nm * sizeof(lz0_match));
            int32_t *sc = guarded((size_t)nm * 4);
            memcpy(sc, sc_local, (size_t)nm * 4);
            CHECK(lz0_simd_kc_worst(sc, mcopy, nm) == lz0_kc_worst_c(sc, mcopy, nm),
                  "kc_worst n=%d", nm);
        }

        {
            /* MRU batch: random list state (stale lanes beyond n_mru too) */
            uint32_t mru_a[8], mru_b[8];
            int nm_a = (int)(rnd() % 9), nm_b;
            for (int i = 0; i < 8; i++)
                mru_a[i] = rnd() % 3 ? 1 + rnd() % 600 : rnd();
            memcpy(mru_b, mru_a, sizeof(mru_a));
            nm_b = nm_a;
            int nb = (int)(rnd() % 65);
            lz0_match *bm = guarded((size_t)nb * sizeof(lz0_match));
            for (int i = 0; i < nb; i++) {
                bm[i].offset = rnd() % 2 ? mru_a[rnd() % 8] : rnd() % 1024;
                bm[i].length = rnd() % 8;
            }
            lz0_mru_batch_c(mru_a, &nm_a, bm, nb);
            lz0_simd_mru_batch(mru_b, &nm_b, bm, nb);
            CHECK(nm_a == nm_b && !memcmp(mru_a, mru_b, sizeof(mru_a)), "mru_batch n=%d", nb);
        }

        uint32_t len = rnd() % 2 ? rnd() % 300 : rnd() % 9000;
        static unsigned char abuf[9100];
        unsigned char *b = guarded(len);
        int alpha = 1 + (int)(rnd() % 4);
        for (uint32_t i = 0; i < len; i++) {
            abuf[i] = (unsigned char)(rnd() % (unsigned)alpha);
            b[i] = (unsigned char)(rnd() % (unsigned)alpha);
        }
        static const uint32_t steps[] = {1, 2, 4, 3};
        for (int s = 0; s < 4; s++) {
            uint32_t st = steps[s];
            CHECK(lz0_simd_count_eq(abuf, b, len, st) == lz0_count_eq_c(abuf, b, len, st),
                  "count_eq len=%u step=%u", len, st);
        }
    }
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 200000;
    int max = lz0_simd_init();
    for (int lvl = max; lvl >= 0; lvl--) {
        lz0_simd_force = lvl;
        lz0_simd_init();
        run_tier(iters);
        printf("tier %-5s: %d random cases x 6 kernels OK\n", lz0_simd_name(), iters);
    }
    if (fails) {
        printf("test_simd: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_simd: all tiers match the C reference.\n");
    return 0;
}
