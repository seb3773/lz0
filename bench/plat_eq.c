#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdint.h>

int lz0_sa_hybrid;
int lz0_sa_win;
int lz0_sa_sample = 1;
int lz0_sa_lcp;

static int sdp_elias_bits(int value) {
    if (value <= 0)
        return 1;
    return ((32 - __builtin_clz((unsigned)value)) << 1) - 1;
}

static int unroll_ends(int L, int *ends, int *costs, uint32_t off, int src) {
    int n = 0;
    int cbase = src + 10 + sdp_elias_bits((((int)off - 1) >> 8) + 1);
    ends[n] = L < 8 ? L : 8;
    costs[n] = cbase + 3;
    n++;
    if (L < 9)
        return n;
    ends[n] = 9;
    costs[n] = cbase + 4;
    n++;
    if (L < 10)
        return n;
    static const int hi[] = {11, 15, 23, 39, 71, 135, 263, 519, 1031, 2055, 4103, 8199,
                             16391, 32775, 65543};
    int cl = 6;
    int need = 10;
    for (int p = 0; p < 15 && L >= need; p++) {
        int h = hi[p];
        int end = L < h ? L : h;
        ends[n] = end;
        costs[n] = cbase + cl;
        n++;
        if (L <= h)
            return n;
        cl += 2;
        need = h + 1;
    }
    while (need <= L) {
        int end = lz0_next_plateau_new(need, L);
        ends[n] = end;
        costs[n] = src + lz0_cost_new((int)off, end);
        n++;
        need = end + 1;
    }
    return n;
}

static int orig_ends(int L, int *ends, int *costs, uint32_t off, int src) {
    int n = 0;
    int start = 2;
    while (start <= L) {
        int end = lz0_next_plateau_new(start, L);
        ends[n] = end;
        costs[n] = src + lz0_cost_new((int)off, end);
        n++;
        start = end + 1;
    }
    return n;
}

int main(void) {
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
    int fail = 0;
    int e0[64], e1[64], c0[64], c1[64];
    uint32_t offs[] = {1, 2, 8, 16, 255, 256, 257, 1000, 4096, 65535, 65536, 100000};
    int Ls[] = {2, 3, 7, 8, 9, 10, 11, 12, 15, 16, 23, 24, 39, 40, 71, 72, 135, 136,
                200, 263, 310, 519, 1000, 2055, 10000, 65543, 65544, 100000};
    for (unsigned oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++) {
        for (unsigned li = 0; li < sizeof(Ls) / sizeof(Ls[0]); li++) {
            uint32_t off = offs[oi];
            int L = Ls[li];
            int n0 = orig_ends(L, e0, c0, off, 12345);
            int n1 = unroll_ends(L, e1, c1, off, 12345);
            if (n0 != n1) {
                printf("FAIL n L=%d off=%u orig=%d unroll=%d\n", L, off, n0, n1);
                fail++;
                continue;
            }
            for (int i = 0; i < n0; i++) {
                if (e0[i] != e1[i] || c0[i] != c1[i]) {
                    printf("FAIL L=%d off=%u i=%d end %d/%d cost %d/%d\n", L, off, i, e0[i],
                           e1[i], c0[i], c1[i]);
                    fail++;
                    break;
                }
            }
        }
    }
    printf(fail ? "PLAT EQ FAILED %d\n" : "PLAT EQ OK\n", fail);
    return fail ? 1 : 0;
}
