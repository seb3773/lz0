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
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
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

static const unsigned char *sa_cmp_in;
static uint32_t sa_cmp_n;
static int sa_cmp_suf(const void *a, const void *b) {
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;
    uint32_t la = sa_cmp_n - ia;
    uint32_t lb = sa_cmp_n - ib;
    uint32_t ml = la < lb ? la : lb;
    int c = memcmp(sa_cmp_in + ia, sa_cmp_in + ib, ml);
    if (c != 0)
        return c;
    return (la < lb) ? -1 : (la > lb ? 1 : 0);
}

static void run_tier(int iters) {
    unsigned char sa_in_cache[160];
    uint32_t sa_cache[160];
    uint32_t rank_cache[160];
    uint32_t sa_in_n = 0;
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

        static const uint16_t b40_off[40] = {
            4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  48,  52,  56,
            60,  64,  72,  80,  88,  96,  104, 112, 120, 128, 136, 144, 152, 160,
            168, 176, 184, 192, 200, 208, 216, 224, 232, 240, 248, 256
        };
        unsigned char *b40 = guarded(256);
        for (int i = 0; i < 256; i++)
            b40[i] = (unsigned char)rnd();
        uint32_t pm = (rnd() % 4 == 0) ? 0x00ffu : 0xffffu;
        uint32_t w0 = rnd() & pm;
        if (rnd() % 3 == 0) {
            for (int h = 0; h < 6; h++) {
                int slot = (int)(rnd() % 40);
                int off = 256 - b40_off[slot];
                b40[off] = (unsigned char)(w0 & 0xffu);
                if (pm == 0xffffu || (rnd() & 1))
                    b40[off + 1] = (unsigned char)((w0 >> 8) & 0xffu);
                else
                    b40[off + 1] = (unsigned char)rnd();
            }
        }
        uint64_t want_b40 = 0;
        for (int i = 0; i < 40; i++) {
            uint16_t v;
            memcpy(&v, b40 + 256 - b40_off[i], 2);
            want_b40 |= (uint64_t)((v & pm) == w0) << i;
        }
        uint64_t got_b40 = lz0_simd_bin40_hits(b40, w0, pm);
        uint64_t ref_b40 = lz0_bin40_hits_c(b40, w0, pm);
        CHECK(got_b40 == want_b40 && ref_b40 == want_b40,
              "bin40 got=0x%llx ref=0x%llx want=0x%llx pm=0x%x",
              (unsigned long long)got_b40, (unsigned long long)ref_b40,
              (unsigned long long)want_b40, pm);

        {
            /* u_row_prep: both rkey and rcost placed on independent guard pages */
            int nsi = 1 + (int)(rnd() % 64);
            if (it < 64)
                nsi = it + 1;
            uint32_t *uk = guarded_slot(0, (size_t)nsi * 4);
            int32_t *uc = guarded_slot(1, (size_t)nsi * 4);
            static const uint32_t uoffs[] = {1, 2, 16, 255, 256, 257, 512, 513, 65536};
            int32_t base_c = (int32_t)(rnd() % 5000);
            int32_t minc = 0x7fffffff;
            for (int i = 0; i < nsi; i++) {
                uint32_t off = (rnd() % 3 == 0) ? uoffs[rnd() % 9] : (1u + (rnd() % 0x100000u));
                if (rnd() % 3 == 0 && i > 0)
                    off = uk[rnd() % i] & 0x7fffffffu;
                uk[i] = off | ((rnd() & 1u) ? 0x80000000u : 0u);
                int32_t c = (rnd() % 4 == 0) ? base_c : (base_c + (int32_t)(rnd() % 40));
                uc[i] = c;
                if (c < minc)
                    minc = c;
            }
            lz0_u_prep ref, got;
            memset(&ref, 0, sizeof(ref));
            ref.rkey = uk;
            ref.rcost = uc;
            ref.in = NULL;
            ref.pos = (rnd() % 4 == 0) ? (rnd() % 20) : (100u + rnd() % 0x10000u);
            ref.nsi = nsi;
            ref.row_best = minc - (int32_t)(rnd() % 3);
            ref.margin = (int32_t)(rnd() % 24);
            ref.exact = (rnd() % 5 == 0) ? 1 : 0;
            got = ref;
            lz0_u_row_prep_c(&ref);
            lz0_simd_u_row_prep(&got);
            CHECK(got.b_out == ref.b_out && got.b2_out == ref.b2_out &&
                      got.pass_mask == ref.pass_mask &&
                      got.lit_cand_mask == ref.lit_cand_mask &&
                      !memcmp(got.um, ref.um, (size_t)nsi * 4) &&
                      !memcmp(got.h_lit, ref.h_lit, (size_t)nsi * 4),
                  "u_row_prep nsi=%d exact=%d b=(%d,%d) b2=(%d,%d)", nsi, ref.exact, got.b_out,
                  ref.b_out, got.b2_out, ref.b2_out);
        }

        {
            /* sa_scan_side: sorted SA slice & input buffer placed on guard pages */
            if (it % 16 == 0) {
                sa_in_n = 16u + (rnd() % 145u);
                int salph = 1 + (int)(rnd() % 4);
                for (uint32_t i = 0; i < sa_in_n; i++)
                    sa_in_cache[i] = (unsigned char)(rnd() % (unsigned)salph);
                for (uint32_t i = 0; i < sa_in_n; i++)
                    sa_cache[i] = i;
                sa_cmp_in = sa_in_cache;
                sa_cmp_n = sa_in_n;
                qsort(sa_cache, sa_in_n, sizeof(uint32_t), sa_cmp_suf);
                for (uint32_t i = 0; i < sa_in_n; i++)
                    rank_cache[sa_cache[i]] = i;
            }
            uint32_t pos = rnd() % (sa_in_n > 2 ? sa_in_n - 1 : 1);
            if (pos + 2 <= sa_in_n) {
                uint32_t rr = rank_cache[pos];
                int dir = (rnd() & 1u) ? 1 : -1;
                int avail = dir > 0 ? (int)(sa_in_n - 1u - rr) : (int)rr;
                int cnt = avail > 0 ? (1 + (int)(rnd() % (unsigned)(avail < 128 ? avail : 128))) : 0;
                uint32_t *sa_g = guarded_slot(0, (size_t)cnt * 4);
                unsigned char *in_g = guarded_slot(1, sa_in_n);
                memcpy(in_g, sa_in_cache, sa_in_n);
                if (dir > 0 && cnt > 0) {
                    memcpy(sa_g, sa_cache + rr + 1, (size_t)cnt * 4);
                } else if (dir < 0 && cnt > 0) {
                    memcpy(sa_g, sa_cache + rr - (uint32_t)cnt, (size_t)cnt * 4);
                }
                const uint32_t *sa_ptr = (dir > 0) ? sa_g : (cnt > 0 ? sa_g + cnt - 1 : sa_g);
                uint16_t pref16;
                memcpy(&pref16, in_g + pos, 2);
                uint8_t ge3_ok = (uint8_t)(pos + 2u < sa_in_n && (rnd() % 8 != 0));
                uint8_t b2 = ge3_ok ? in_g[pos + 2] : 0;
                static const uint32_t cuts[] = {0u, 8u, 32u, 257u, 0xffffffffu};
                uint32_t src_c[128], src_s[128];
                uint8_t k_c[128], k_s[128];
                lz0_sa_scan_args a_c = {sa_ptr, in_g, pos, sa_in_n, cnt, dir, pref16, b2,
                                        (uint8_t)(rnd() & 1u), (uint8_t)(rnd() % 4 == 0),
                                        ge3_ok, cuts[rnd() % 5], 0, src_c, k_c};
                lz0_sa_scan_args a_s = a_c;
                a_s.out_src = src_s;
                a_s.out_k = k_s;
                int nc = lz0_sa_scan_side_c(&a_c);
                int ns = lz0_simd_sa_scan_side(&a_s);
                CHECK(nc == ns && a_c.in_l2 == a_s.in_l2 &&
                          (nc == 0 || (!memcmp(src_c, src_s, (size_t)nc * 4) &&
                                       !memcmp(k_c, k_s, (size_t)nc))),
                      "sa_scan_side dir=%d cnt=%d n=(%d,%d) l2=(%u,%u)", dir, cnt, nc, ns,
                      a_c.in_l2, a_s.in_l2);
            }
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
        printf("tier %-5s: %d random cases x 9 kernels OK\n", lz0_simd_name(), iters);
    }
    if (fails) {
        printf("test_simd: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_simd: all tiers match the C reference.\n");
    return 0;
}
