#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

int lz0_sa_hybrid;
int lz0_sa_win;
int lz0_sa_sample = 1;
int lz0_sa_lcp;

static double rdtsc_ms(void) {
    return (double)__builtin_ia32_rdtsc() / 2.5e6;
}

typedef struct {
    uint32_t last_off;
    int cost;
    uint32_t prev_pos;
    uint32_t tok_len;
    uint32_t tok_off;
    uint8_t prev_slot;
    uint8_t can_last;
} fake_st;

static inline void fake_store(fake_st *d, uint32_t last_off, int can_last, int cost,
                              uint32_t prev_pos, uint8_t prev_slot, uint32_t tok_len,
                              uint32_t tok_off) {
    d->last_off = last_off;
    d->can_last = (uint8_t)can_last;
    d->cost = cost;
    d->prev_pos = prev_pos;
    d->prev_slot = prev_slot;
    d->tok_len = tok_len;
    d->tok_off = tok_off;
}

static inline void fake_relax2(fake_st *row, uint8_t *nslot, uint32_t last_off,
                               int can_last, int cost, uint32_t prev_pos, uint8_t prev_slot,
                               uint32_t tok_len, uint32_t tok_off) {
    uint8_t n = *nslot;
    if (n > 0 && row[0].last_off == last_off && row[0].can_last == can_last) {
        if (cost < row[0].cost)
            fake_store(&row[0], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
        return;
    }
    if (n > 1 && row[1].last_off == last_off && row[1].can_last == can_last) {
        if (cost < row[1].cost)
            fake_store(&row[1], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
        return;
    }
    if (n < 2) {
        fake_store(&row[n], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
        *nslot = (uint8_t)(n + 1);
        return;
    }
    int w = row[1].cost > row[0].cost ? 1 : 0;
    if (cost < row[w].cost)
        fake_store(&row[w], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
}

static uint32_t rnd_state = 0xC0FFEEu;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

int main(void) {
    const uint32_t npos = 1u << 20;
    const int N = 1 << 22;
    int *Ls = (int *)malloc((size_t)N * sizeof(int));
    uint32_t *offs = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
    uint32_t *is = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
    int *src_cost = (int *)malloc((size_t)N * sizeof(int));
    if (!Ls || !offs || !is || !src_cost)
        return 1;

    static const int lens[] = {4, 6, 8, 9, 11, 15, 19, 23, 31, 39, 47, 55};
    static const int wts[]  = {20, 18, 14, 8, 10, 10, 6, 6, 4, 2, 1, 1};
    int wsum = 0;
    for (int i = 0; i < 12; i++)
        wsum += wts[i];
    for (int i = 0; i < N; i++) {
        int r = (int)(rnd() % (unsigned)wsum);
        int L = lens[0];
        for (int k = 0; k < 12; k++) {
            r -= wts[k];
            if (r < 0) {
                L = lens[k];
                break;
            }
        }
        Ls[i] = L;
        offs[i] = 1u + (rnd() % 65536u);
        is[i] = rnd() % (npos - 256);
        src_cost[i] = (int)(rnd() % 40000u);
    }

    volatile uint64_t sink = 0;
    double t0, t1;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        int start = 2, L = Ls[i], nplat = 0;
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            nplat += end;
            start = end + 1;
        }
        sink += (uint64_t)(unsigned)nplat;
    }
    t1 = rdtsc_ms();
    double ms_plat = t1 - t0;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        int start = 2, L = Ls[i], acc = 0;
        uint32_t off = offs[i];
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            acc += lz0_cost_new((int)off, end);
            start = end + 1;
        }
        sink += (uint64_t)(unsigned)acc;
    }
    t1 = rdtsc_ms();
    double ms_cost = t1 - t0;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        int start = 2, L = Ls[i], acc = 0;
        uint32_t off = offs[i];
        int sh = 8;
        int offset_msb = ((int)off - 1) >> sh;
        int c_off = 1 + 1 + sh + lz0_elias_bits(offset_msb + 1);
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            acc += c_off + lz0_len_new_payload_bits(end - 1);
            start = end + 1;
        }
        sink += (uint64_t)(unsigned)acc;
    }
    t1 = rdtsc_ms();
    double ms_cost_hoist = t1 - t0;

    fake_st *st = (fake_st *)calloc((size_t)npos * 2u, sizeof(fake_st));
    uint8_t *ns = (uint8_t *)calloc((size_t)npos, 1);
    if (!st || !ns)
        return 1;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        int start = 2, L = Ls[i];
        uint32_t off = offs[i];
        uint32_t pos = is[i];
        int cost = src_cost[i];
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            int cc = cost + lz0_cost_new((int)off, end);
            fake_relax2(st + (size_t)(pos + (uint32_t)end) * 2u, &ns[pos + (uint32_t)end],
                        off, 0, cc, pos, 0, (uint32_t)end, off);
            start = end + 1;
        }
    }
    t1 = rdtsc_ms();
    double ms_full = t1 - t0;
    sink += ns[0];

    memset(ns, 0, (size_t)npos);
    memset(st, 0, (size_t)npos * 2u * sizeof(fake_st));
    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        int start = 2, L = Ls[i];
        uint32_t off = offs[i];
        uint32_t pos = is[i];
        int cost = src_cost[i];
        int sh = 8;
        int offset_msb = ((int)off - 1) >> sh;
        int c_off = 1 + 1 + sh + lz0_elias_bits(offset_msb + 1);
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            int cc = cost + c_off + lz0_len_new_payload_bits(end - 1);
            fake_relax2(st + (size_t)(pos + (uint32_t)end) * 2u, &ns[pos + (uint32_t)end],
                        off, 0, cc, pos, 0, (uint32_t)end, off);
            start = end + 1;
        }
    }
    t1 = rdtsc_ms();
    double ms_full_hoist = t1 - t0;
    sink += ns[1];

    memset(ns, 0, (size_t)npos);
    memset(st, 0, (size_t)npos * 2u * sizeof(fake_st));
    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        int start = 2, L = Ls[i];
        uint32_t off = offs[i];
        uint32_t pos = is[i];
        int cost = src_cost[i];
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            fake_relax2(st + (size_t)(pos + (uint32_t)end) * 2u, &ns[pos + (uint32_t)end],
                        off, 0, cost, pos, 0, (uint32_t)end, off);
            start = end + 1;
        }
    }
    t1 = rdtsc_ms();
    double ms_relax_only = t1 - t0;
    sink += st[0].cost;

    printf("newem micro N=%d npos=%u sink=%llu\n", N, npos, (unsigned long long)sink);
    printf("  plateau_only     %.1f ms\n", ms_plat);
    printf("  plat+cost        %.1f ms\n", ms_cost);
    printf("  plat+cost_hoist  %.1f ms\n", ms_cost_hoist);
    printf("  full emit        %.1f ms\n", ms_full);
    printf("  full hoist       %.1f ms\n", ms_full_hoist);
    printf("  plat+relax(noC)  %.1f ms\n", ms_relax_only);
    printf("  implied cost     %.1f  relax+addr %.1f  hoist_save %.1f\n",
           ms_cost - ms_plat, ms_relax_only - ms_plat, ms_full - ms_full_hoist);
    return 0;
}
