#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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
} sdp_st;

static inline void sdp_store(sdp_st *d, uint32_t last_off, int can_last, int cost,
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

static inline void sdp_relax2(sdp_st *row, uint8_t *nslot, uint32_t last_off,
                              int can_last, int cost, uint32_t prev_pos, uint8_t prev_slot,
                              uint32_t tok_len, uint32_t tok_off) {
    uint8_t n = *nslot;
    if (n == 2) {
        if (row[0].last_off == last_off && row[0].can_last == can_last) {
            if (cost < row[0].cost)
                sdp_store(&row[0], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                          tok_off);
            return;
        }
        if (row[1].last_off == last_off && row[1].can_last == can_last) {
            if (cost < row[1].cost)
                sdp_store(&row[1], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                          tok_off);
            return;
        }
        int w = row[1].cost > row[0].cost ? 1 : 0;
        if (cost < row[w].cost)
            sdp_store(&row[w], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                      tok_off);
        return;
    }
    if (n > 0 && row[0].last_off == last_off && row[0].can_last == can_last) {
        if (cost < row[0].cost)
            sdp_store(&row[0], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                      tok_off);
        return;
    }
    sdp_store(&row[n], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
    *nslot = (uint8_t)(n + 1);
}

static inline void sdp_relax2_lose_fast(sdp_st *row, uint32_t last_off, int can_last,
                                        int cost) {
    if (row[0].last_off == last_off && row[0].can_last == can_last) {
        if (cost < row[0].cost)
            row[0].cost = cost;
        return;
    }
    if (row[1].last_off == last_off && row[1].can_last == can_last) {
        if (cost < row[1].cost)
            row[1].cost = cost;
        return;
    }
    int w = row[1].cost > row[0].cost ? 1 : 0;
    if (cost < row[w].cost)
        row[w].cost = cost;
}

enum { NPOS = 1 << 18, N = 1 << 23 };

int main(void) {
    sdp_st *st = (sdp_st *)calloc((size_t)NPOS * 2u, sizeof(sdp_st));
    uint8_t *ns = (uint8_t *)malloc((size_t)NPOS);
    if (!st || !ns)
        return 1;
    memset(ns, 2, (size_t)NPOS);
    for (uint32_t i = 0; i < NPOS; i++) {
        st[i * 2u].last_off = 100u + (i % 97u);
        st[i * 2u].can_last = 0;
        st[i * 2u].cost = 1000;
        st[i * 2u + 1].last_off = 200u + (i % 53u);
        st[i * 2u + 1].can_last = 0;
        st[i * 2u + 1].cost = 800;
    }

    volatile uint64_t sink = 0;
    double t0, t1;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_relax2(st + p * 2u, &ns[p], 999u, 0, 5000, 0, 0, 8, 999u);
    }
    t1 = rdtsc_ms();
    double ms_lose = t1 - t0;
    sink += (uint64_t)(unsigned)st[0].cost;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_relax2(st + p * 2u, &ns[p], 100u + (p % 97u), 0, 5000, 0, 0, 8, 100u);
    }
    t1 = rdtsc_ms();
    double ms_same0 = t1 - t0;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_relax2(st + p * 2u, &ns[p], 200u + (p % 53u), 0, 5000, 0, 0, 8, 200u);
    }
    t1 = rdtsc_ms();
    double ms_same1 = t1 - t0;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_relax2(st + p * 2u, &ns[p], 100u + (p % 97u), 0, 1, 0, 0, 8, 100u);
    }
    t1 = rdtsc_ms();
    double ms_better = t1 - t0;
    sink += (uint64_t)(unsigned)st[0].cost;

    for (uint32_t i = 0; i < NPOS; i++) {
        st[i * 2u].cost = 1000;
        st[i * 2u + 1].cost = 800;
    }
    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_relax2(st + p * 2u, &ns[p], 999u, 0, 1, 0, 0, 8, 999u);
    }
    t1 = rdtsc_ms();
    double ms_win = t1 - t0;
    sink += (uint64_t)(unsigned)st[0].cost;

    memset(ns, 0, (size_t)NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        ns[p] = 0;
        sdp_relax2(st + p * 2u, &ns[p], 7u, 0, 10, 0, 0, 8, 7u);
    }
    t1 = rdtsc_ms();
    double ms_fill = t1 - t0;
    sink += ns[0];

    memset(ns, 2, (size_t)NPOS);
    for (uint32_t i = 0; i < NPOS; i++) {
        st[i * 2u].last_off = 100u + (i % 97u);
        st[i * 2u].cost = 1000;
        st[i * 2u + 1].last_off = 200u + (i % 53u);
        st[i * 2u + 1].cost = 800;
    }
    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_relax2_lose_fast(st + p * 2u, 999u, 0, 5000);
    }
    t1 = rdtsc_ms();
    double ms_lose_fast = t1 - t0;
    sink += (uint64_t)(unsigned)st[1].cost;

    t0 = rdtsc_ms();
    for (int i = 0; i < N; i++) {
        uint32_t p = (uint32_t)i % NPOS;
        sdp_st *row = st + p * 2u;
        sink += (uint64_t)(unsigned)row[0].cost + (uint64_t)(unsigned)row[1].cost;
        sink += row[0].last_off + row[1].last_off;
    }
    t1 = rdtsc_ms();
    double ms_load = t1 - t0;

    printf("relax2 micro N=%d npos=%d sink=%llu\n", N, NPOS, (unsigned long long)sink);
    printf("  lose        %.1f ms\n", ms_lose);
    printf("  same slot0  %.1f ms\n", ms_same0);
    printf("  same slot1  %.1f ms\n", ms_same1);
    printf("  better      %.1f ms\n", ms_better);
    printf("  evict_win   %.1f ms\n", ms_win);
    printf("  fill        %.1f ms\n", ms_fill);
    printf("  lose_fast   %.1f ms  (cost-only, no store of pred/tok)\n", ms_lose_fast);
    printf("  load4       %.1f ms  (two last_off + two cost)\n", ms_load);
    printf("  ns/call lose=%.2f same0=%.2f same1=%.2f better=%.2f win=%.2f fill=%.2f fast=%.2f load=%.2f\n",
           1e6 * ms_lose / N, 1e6 * ms_same0 / N, 1e6 * ms_same1 / N, 1e6 * ms_better / N,
           1e6 * ms_win / N, 1e6 * ms_fill / N, 1e6 * ms_lose_fast / N, 1e6 * ms_load / N);
    return 0;
}
