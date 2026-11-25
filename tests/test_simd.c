/*
 * test_simd.c - Differential test of the encoder SIMD kernels.
 *
 * Every available tier is checked against the portable C reference on
 * random inputs of every length 0..300 (heavy ties, duplicates, extreme
 * values). Inputs are placed right before a PROT_NONE guard page so any
 * read past the last element faults immediately.
 *
 * matchlen: both operands end on their own guard page; exhaustive sweep of
 * every max in [-2, 160] x every mismatch position, then random cases up to
 * 32 KiB (full matches, mismatches near either end, several mismatches,
 * aliased operands as in an overlapping copy). The vector tier, the inline
 * wrapper and the C reference are each checked against a byte loop.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "lz0_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

/* Two independent guarded regions (slot 0 and 1), 8 pages each, every one
 * followed by a PROT_NONE page. */
static unsigned char *guard_base[2];
static size_t guard_len;

/* Returns a pointer such that [p, p + bytes) ends exactly at the guard page
 * of the given slot. */
static void *guarded_slot(int slot, size_t bytes) {
    if (!guard_base[slot]) {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        size_t pg = (size_t)si.dwPageSize;
        guard_len = pg * 8;
        unsigned char *g = (unsigned char *)VirtualAlloc(
            NULL, guard_len + pg, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old = 0;
        if (!g || !VirtualProtect(g + guard_len, pg, PAGE_NOACCESS, &old)) {
            fprintf(stderr, "VirtualAlloc/VirtualProtect failed\n");
            exit(2);
        }
#else
        size_t pg = (size_t)sysconf(_SC_PAGESIZE);
        guard_len = pg * 8;
        unsigned char *g = mmap(NULL, guard_len + pg, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g == MAP_FAILED || mprotect(g + guard_len, pg, PROT_NONE)) {
            perror("mmap/mprotect");
            exit(2);
        }
#endif
        guard_base[slot] = g;
    }
    return guard_base[slot] + guard_len - bytes;
}

static void *guarded(size_t bytes) { return guarded_slot(0, bytes); }

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

/* Byte-at-a-time reference, independent of lz0_matchlen_c. */
static int matchlen_ref(const unsigned char *a, const unsigned char *b, int max) {
    int n = 0;
    while (n < max && a[n] == b[n])
        n++;
    return n;
}

static void check_matchlen(const unsigned char *a, const unsigned char *b, int max,
                           const char *what) {
    int want = matchlen_ref(a, b, max);
    int c = lz0_matchlen_c(a, b, max);
    int tier = LZ0_SIMD_CALL(lz0_matchlen, a, b, max);
    int wrap = lz0_simd_matchlen(a, b, max);
    CHECK(c == want && tier == want && wrap == want,
          "matchlen %s max=%d want=%d c=%d tier=%d wrapper=%d", what, max, want, c, tier,
          wrap);
}

/* a and b both end exactly at a guard page (b may also alias a). */
static void matchlen_case(int max, int mism, int extra, int alias) {
    size_t sz = max > 0 ? (size_t)max : 0;
    unsigned char *a = guarded_slot(0, sz);
    unsigned char *b = guarded_slot(1, sz);
    for (size_t i = 0; i < sz; i++)
        a[i] = (unsigned char)rnd();
    memcpy(b, a, sz);
    if (mism >= 0 && mism < max)
        b[mism] ^= (unsigned char)(1u + rnd() % 255u); /* any bit, incl. bit 7 */
    for (int e = 0; e < extra && max > 0; e++)        /* later mismatches too */
        b[rnd() % sz] ^= (unsigned char)(1u << (rnd() % 8));
    check_matchlen(a, b, max, "two-buf");
    check_matchlen(b, a, max, "swapped");
    if (alias && max > 1) {
        /* overlapping operands, as for an offset-d copy: a + d vs a */
        int d = 1 + (int)(rnd() % (unsigned)(max - 1 < 40 ? max - 1 : 40));
        int per = 1 + (int)(rnd() % 4);
        for (int i = 0; i < max; i++)
            a[i] = (unsigned char)(i % per);
        if (rnd() % 2)
            a[rnd() % sz] ^= 0x80;
        check_matchlen(a + d, a, max - d, "alias");
    }
}

static void run_matchlen(int iters) {
    /* exhaustive: every max in [-2, 160], every mismatch position */
    for (int max = -2; max <= 160; max++)
        for (int p = 0; p <= (max > 0 ? max : 0); p++)
            matchlen_case(max, p, 0, 0);
    /* random: long matches (up to 8 pages), multiple mismatches, aliasing */
    for (int it = 0; it < iters; it++) {
        int max;
        switch (rnd() % 4) {
        case 0: max = (int)(rnd() % 64); break;
        case 1: max = (int)(rnd() % 600); break;
        default: max = (int)(rnd() % 32769); break;
        }
        int mism;
        switch (rnd() % 4) {
        case 0: mism = max; break;                            /* full match */
        case 1: mism = max - 1 - (int)(rnd() % 40); break;    /* near the end */
        case 2: mism = (int)(rnd() % 40); break;              /* near the start */
        default: mism = max > 0 ? (int)(rnd() % (unsigned)max) : 0; break;
        }
        matchlen_case(max, mism, (int)(rnd() % 3), (int)(rnd() % 2));
    }
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 200000;
    int max = lz0_simd_init();
    for (int lvl = max; lvl >= 0; lvl--) {
        lz0_simd_force = lvl;
        lz0_simd_init();
        run_tier(iters);
        run_matchlen(iters / 10 + 1);
        printf("tier %-5s: %d random cases x 7 kernels OK\n", lz0_simd_name(), iters);
    }
    if (fails) {
        printf("test_simd: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_simd: all tiers match the C reference.\n");
    return 0;
}
