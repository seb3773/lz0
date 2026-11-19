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
    if (n > 0 && row[0].last_off == last_off && row[0].can_last == can_last) {
        if (cost < row[0].cost)
            sdp_store(&row[0], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                      tok_off);
        return;
    }
    if (n > 1 && row[1].last_off == last_off && row[1].can_last == can_last) {
        if (cost < row[1].cost)
            sdp_store(&row[1], last_off, can_last, cost, prev_pos, prev_slot, tok_len,
                      tok_off);
        return;
    }
    if (n < 2) {
        sdp_store(&row[n], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
        *nslot = (uint8_t)(n + 1);
        return;
    }
    int w = row[1].cost > row[0].cost ? 1 : 0;
    if (cost < row[w].cost)
        sdp_store(&row[w], last_off, can_last, cost, prev_pos, prev_slot, tok_len, tok_off);
}

static void restore(sdp_st *st, uint8_t *ns, uint32_t npos) {
    memset(ns, 2, (size_t)npos);
    for (uint32_t i = 0; i < npos; i++) {
        st[i * 2u].last_off = 100u + (i % 97u);
        st[i * 2u].can_last = 0;
        st[i * 2u].cost = 1000;
        st[i * 2u + 1].last_off = 200u + (i % 53u);
        st[i * 2u + 1].can_last = 0;
        st[i * 2u + 1].cost = 800;
    }
}

enum { NPOS = 1 << 22, NCALL = 1 << 23 };

int main(void) {
    printf("sizeof(sdp_st)=%zu\n", sizeof(sdp_st));
    sdp_st *st = (sdp_st *)calloc((size_t)NPOS * 2u, sizeof(sdp_st));
    uint8_t *ns = (uint8_t *)malloc((size_t)NPOS);
    if (!st || !ns)
        return 1;

    static const int ends[] = {8, 9, 11, 15, 23};
    uint32_t *pos = (uint32_t *)malloc((size_t)NCALL * sizeof(uint32_t));
    uint32_t *off = (uint32_t *)malloc((size_t)NCALL * sizeof(uint32_t));
    int *endv = (int *)malloc((size_t)NCALL * sizeof(int));
    int *ccv = (int *)malloc((size_t)NCALL * sizeof(int));
    if (!pos || !off || !endv || !ccv)
        return 1;

    uint32_t rnd = 0xC0FFEEu;
    for (int i = 0; i < NCALL; i++) {
        rnd = rnd * 1664525u + 1013904223u;
        uint32_t p = rnd % (NPOS - 64);
        int e = ends[i % 5];
        uint32_t o = 1u + (rnd % 65536u);
        pos[i] = p;
        endv[i] = e;
        off[i] = o;
        ccv[i] = 5000;
    }

    volatile uint64_t sink = 0;
    double t0, t1;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t d = pos[i] + (uint32_t)endv[i];
        sink += d;
    }
    t1 = rdtsc_ms();
    double ms_idx = t1 - t0;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t d = pos[i] + (uint32_t)endv[i];
        sdp_st *row = st + (size_t)d * 2u;
        uint8_t *nslot = &ns[d];
        sink += (uint64_t)(uintptr_t)row + (uint64_t)(uintptr_t)nslot;
    }
    t1 = rdtsc_ms();
    double ms_addr = t1 - t0;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t p = pos[i];
        int e = endv[i];
        sdp_st *rowbase = st + (size_t)p * 2u;
        uint8_t *nsb = ns + p;
        sdp_st *row = rowbase + (size_t)e * 2u;
        uint8_t *nslot = nsb + e;
        sink += (uint64_t)(uintptr_t)row + (uint64_t)(uintptr_t)nslot;
    }
    t1 = rdtsc_ms();
    double ms_hoist_addr = t1 - t0;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t d = pos[i] + (uint32_t)endv[i];
        sdp_st *row = st + (size_t)d * 2u;
        sink += (uint64_t)(unsigned)row[0].cost + ns[d] + row[0].last_off;
    }
    t1 = rdtsc_ms();
    double ms_touch = t1 - t0;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t d = pos[i] + (uint32_t)endv[i];
        sdp_relax2(st + (size_t)d * 2u, &ns[d], off[i], 0, ccv[i], pos[i], 0,
                   (uint32_t)endv[i], off[i]);
    }
    t1 = rdtsc_ms();
    double ms_full = t1 - t0;
    sink += (uint64_t)(unsigned)st[0].cost;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t p = pos[i];
        int e = endv[i];
        sdp_st *rowbase = st + (size_t)p * 2u;
        uint8_t *nsb = ns + p;
        sdp_relax2(rowbase + (size_t)e * 2u, nsb + e, off[i], 0, ccv[i], p, 0, (uint32_t)e,
                   off[i]);
    }
    t1 = rdtsc_ms();
    double ms_hoist = t1 - t0;
    sink += ns[0];

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        uint32_t d = pos[i] + (uint32_t)endv[i];
        sdp_st *row = st + (size_t)d * 2u;
        uint8_t *nslot = &ns[d];
        sink += (uint64_t)(unsigned)row[0].last_off + row[0].can_last + row[0].cost;
        sink += (uint64_t)(unsigned)row[1].last_off + row[1].can_last + row[1].cost;
        sink += *nslot;
    }
    t1 = rdtsc_ms();
    double ms_bodyload = t1 - t0;

    restore(st, ns, NPOS);
    t0 = rdtsc_ms();
    for (int i = 0; i < NCALL; i++) {
        int e = endv[i];
        uint32_t o = off[i];
        int cc = ccv[i];
        uint32_t p = pos[i];
        sink += (uint64_t)(unsigned)e + o + (unsigned)cc + p;
    }
    t1 = rdtsc_ms();
    double ms_args = t1 - t0;

    printf("destaddr N=%d npos=%d sink=%llu\n", NCALL, NPOS, (unsigned long long)sink);
    printf("  idx_add         %.1f ms  %.2f ns\n", ms_idx, 1e6 * ms_idx / NCALL);
    printf("  addr_form       %.1f ms  %.2f ns\n", ms_addr, 1e6 * ms_addr / NCALL);
    printf("  addr_hoist_i    %.1f ms  %.2f ns\n", ms_hoist_addr, 1e6 * ms_hoist_addr / NCALL);
    printf("  args_setup      %.1f ms  %.2f ns\n", ms_args, 1e6 * ms_args / NCALL);
    printf("  touch_slot0     %.1f ms  %.2f ns\n", ms_touch, 1e6 * ms_touch / NCALL);
    printf("  body_loads      %.1f ms  %.2f ns\n", ms_bodyload, 1e6 * ms_bodyload / NCALL);
    printf("  full(addr+body) %.1f ms  %.2f ns\n", ms_full, 1e6 * ms_full / NCALL);
    printf("  hoist_i+body    %.1f ms  %.2f ns\n", ms_hoist, 1e6 * ms_hoist / NCALL);
    printf("  hoist_vs_full   %+.1f%%  delta=%.2f ns\n",
           ms_full > 0 ? 100.0 * (ms_full - ms_hoist) / ms_full : 0,
           1e6 * (ms_full - ms_hoist) / NCALL);
    return 0;
}
