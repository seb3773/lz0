#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

static double rdtsc_ms(void) {
    return (double)__builtin_ia32_rdtsc() / 2.5e6;
}

static uint32_t rnd_state = 0x12345678u;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static unsigned char *make_corpus(const char *kind, size_t n) {
    unsigned char *p = (unsigned char *)malloc(n);
    if (!p)
        return NULL;
    rnd_state = 0xC0FFEEu + (uint32_t)n + (uint32_t)kind[0] * 97u;
    if (!strcmp(kind, "text")) {
        const char *w[] = {
            "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ",
            "and ", "then ", "compress ", "this ", "pattern ", "again ", "with ",
            "some ", "variation ", "ZX0 ", "window ", "offset ", "literal ", "match "
        };
        size_t i = 0;
        while (i < n) {
            const char *s = w[rnd() % 22];
            size_t L = strlen(s);
            if (i + L > n)
                L = n - i;
            memcpy(p + i, s, L);
            i += L;
        }
    } else if (!strcmp(kind, "rep")) {
        for (size_t i = 0; i < n; i++)
            p[i] = (unsigned char)((i / 17) & 0xff);
        for (size_t i = n / 4; i + n / 8 < n && i < n; i += n / 7)
            memcpy(p + i, p, n / 8 < n - i ? n / 8 : n - i);
    } else {
        for (size_t i = 0; i < n; i++) {
            if ((i & 15) == 0)
                p[i] = 0xC3;
            else if ((i & 7) == 1)
                p[i] = (unsigned char)(i >> 8);
            else
                p[i] = (unsigned char)(rnd() & (i & 1 ? 0x0f : 0xff));
        }
        size_t blk = n > 256 ? 256 : n / 2;
        for (size_t i = blk; i + blk < n; i += blk + (rnd() % 64))
            memcpy(p + i, p + (rnd() % (i ? i : 1)), blk);
    }
    return p;
}

static void cfg_h2c8(lz0_config *cfg) {
    lz0_config_default(cfg, LZ0_PARSE_SDP);
    cfg->min_match = 2;
    cfg->max_chain = 8;
    cfg->max_candidates = 16;
    cfg->max_states = 2;
    cfg->jump_levels = 0;
    lz0_chain_prune = 0;
    lz0_chain_noskip = 0;
    lz0_skip_thresh = 0;
    lz0_skip_stride = 0;
    lz0_hash4_probe = 0;
    lz0_deep_probe = 0;
    lz0_sa_win = 16;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 1;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 1;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
}

static uint32_t skip_prev(lz0_finder *f, uint32_t cur, int steps) {
    int t = 0;
    while (t < steps && cur != LZ0_CHAIN_NIL) {
        cur = f->prev[cur];
        t++;
    }
    return cur;
}

typedef struct {
    uint64_t n_pow2;
    uint64_t n_pow2_b0;
    uint64_t n_pow2_probe_pass;
    uint64_t n_head;
    uint64_t n_vis;
    uint64_t n_prev;
    uint64_t n_b0;
    uint64_t n_probe_pass;
    uint64_t n_keep;
} hash_ops;

static void walk_pow2(lz0_finder *f, uint32_t pos, hash_ops *op, int do_ml, volatile uint64_t *sink) {
    const unsigned char *in = f->in;
    uint32_t n = f->n;
    int minm = f->min_match;
    int maxl = (int)(n - pos);
    if (maxl < 2)
        return;
    int best_len = minm - 1;
    for (uint32_t off = 1; off <= pos; ) {
        op->n_pow2++;
        uint32_t src = pos - off;
        if (in[src] == in[pos]) {
            op->n_pow2_b0++;
            int probe = best_len > 0 ? best_len : (minm - 1);
            if (!(probe > 0 && probe < maxl && in[src + probe] != in[pos + probe])) {
                op->n_pow2_probe_pass++;
                if (do_ml) {
                    int ml = lz0_matchlen(in + pos, in + src, maxl);
                    *sink += (uint64_t)(unsigned)ml;
                    if (ml >= minm && ml > best_len)
                        best_len = ml;
                }
            }
        }
        if (off > (pos >> 1))
            break;
        off <<= 1;
    }
}

static void walk_chain(lz0_finder *f, uint32_t pos, hash_ops *op, int do_ml, int do_keep,
                       lz0_match *out, volatile uint64_t *sink) {
    const unsigned char *in = f->in;
    uint32_t n = f->n;
    int minm = f->min_match;
    int maxl = (int)(n - pos);
    if (maxl < 2 || !f->head)
        return;
    int nout = 0;
    int best_len = minm - 1;
    uint32_t hsh;
    if (pos + 2 < n)
        hsh = ((uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8) | ((uint32_t)in[pos + 2] << 16)) *
                  2654435761u >>
              (32 - LZ0_HASH_BITS);
    else
        return;
    uint32_t cur = f->head[hsh];
    op->n_head++;
    int checks = 0;
    int gap = 1;
    int near = 4;
    int visited = 0;
    int visit_lim = 1 << 16;
    while (cur != 0xffffffffu && checks < f->max_chain && visited < visit_lim) {
        if (cur >= pos) {
            cur = f->prev[cur];
            visited++;
            op->n_vis++;
            op->n_prev++;
            continue;
        }
        visited++;
        op->n_vis++;
        if (in[cur] == in[pos]) {
            op->n_b0++;
            int probe = best_len > 0 ? best_len : (minm - 1);
            if (!(probe > 0 && probe < maxl && in[cur + probe] != in[pos + probe])) {
                op->n_probe_pass++;
                int ml = 0;
                if (do_ml) {
                    ml = lz0_matchlen(in + pos, in + cur, maxl);
                    *sink += (uint64_t)(unsigned)ml;
                    if (ml >= minm && ml > best_len)
                        best_len = ml;
                }
                if (do_keep && do_ml && ml >= minm) {
                    lz0_match m;
                    m.offset = pos - cur;
                    m.length = (uint32_t)ml;
                    int had = 0;
                    for (int k = 0; k < nout; k++) {
                        if (out[k].offset == m.offset) {
                            had = 1;
                            if (m.length > out[k].length)
                                out[k].length = m.length;
                            break;
                        }
                    }
                    if (!had && nout < 16)
                        out[nout++] = m;
                    op->n_keep++;
                }
            }
        }
        checks++;
        int jmpn = (near > 0) ? 1 : gap;
        if (near > 0)
            near--;
        else if (gap < (1 << 20))
            gap <<= 1;
        cur = skip_prev(f, cur, jmpn);
        visited += jmpn;
        op->n_prev += (uint64_t)jmpn;
        op->n_vis += (uint64_t)jmpn;
    }
}

static void run_one(const char *kind, size_t n) {
    unsigned char *in = make_corpus(kind, n);
    if (!in)
        return;
    lz0_config cfg;
    cfg_h2c8(&cfg);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) != 0) {
        free(in);
        return;
    }

    volatile uint64_t sink = 0;
    hash_ops z;
    memset(&z, 0, sizeof(z));
    lz0_match cands[16];

    lz0_finder_free(&f);
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) != 0) {
        free(in);
        return;
    }
    double t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++)
        lz0_finder_insert_upto(&f, i);
    double ms_ins = rdtsc_ms() - t0;
    sink += f.last_insert;

    lz0_finder_free(&f);
    lz0_finder_init(&f, in, (uint32_t)n, &cfg);
    hash_ops op_p;
    memset(&op_p, 0, sizeof(op_p));
    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        lz0_finder_insert_upto(&f, i);
        walk_pow2(&f, i, &op_p, 0, &sink);
    }
    double ms_ins_pow = rdtsc_ms() - t0;

    lz0_finder_free(&f);
    lz0_finder_init(&f, in, (uint32_t)n, &cfg);
    hash_ops op_c;
    memset(&op_c, 0, sizeof(op_c));
    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        lz0_finder_insert_upto(&f, i);
        walk_chain(&f, i, &op_c, 0, 0, cands, &sink);
    }
    double ms_ins_ch = rdtsc_ms() - t0;

    lz0_finder_free(&f);
    lz0_finder_init(&f, in, (uint32_t)n, &cfg);
    hash_ops op_m;
    memset(&op_m, 0, sizeof(op_m));
    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        lz0_finder_insert_upto(&f, i);
        walk_pow2(&f, i, &op_m, 1, &sink);
        walk_chain(&f, i, &op_m, 1, 1, cands, &sink);
    }
    double ms_full = rdtsc_ms() - t0;

    double ms_pow = ms_ins_pow - ms_ins;
    double ms_ch = ms_ins_ch - ms_ins;
    double ms_mlkeep = ms_full - ms_ins_pow - (ms_ins_ch - ms_ins);
    if (ms_pow < 0)
        ms_pow = 0;
    if (ms_ch < 0)
        ms_ch = 0;

    printf("=== %s n=%zu isolated hash split sink=%llu ===\n", kind, n,
           (unsigned long long)sink);
    printf("  insert=%.1f  pow2_filt=%.1f  chain_walk=%.1f  ml+keep=%.1f  full=%.1f ms\n",
           ms_ins, ms_pow, ms_ch, ms_mlkeep, ms_full);
    printf("  pow2 n=%llu b0=%llu probe_pass=%llu  ns/pow2=%.2f\n",
           (unsigned long long)op_p.n_pow2, (unsigned long long)op_p.n_pow2_b0,
           (unsigned long long)op_p.n_pow2_probe_pass,
           op_p.n_pow2 ? 1e6 * ms_pow / (double)op_p.n_pow2 : 0);
    printf("  chain head=%llu vis=%llu prev=%llu b0=%llu probe_pass=%llu keep=%llu\n",
           (unsigned long long)op_c.n_head, (unsigned long long)op_c.n_vis,
           (unsigned long long)op_c.n_prev, (unsigned long long)op_c.n_b0,
           (unsigned long long)op_c.n_probe_pass, (unsigned long long)op_m.n_keep);
    printf("  ns/vis=%.2f ns/prev=%.2f ns/head=%.2f\n",
           op_c.n_vis ? 1e6 * ms_ch / (double)op_c.n_vis : 0,
           op_c.n_prev ? 1e6 * ms_ch / (double)op_c.n_prev : 0,
           op_c.n_head ? 1e6 * ms_ch / (double)op_c.n_head : 0);
    double tot = ms_ins + ms_pow + ms_ch + (ms_mlkeep > 0 ? ms_mlkeep : 0);
    if (tot < 1)
        tot = ms_full;
    printf("  share insert=%.1f%% pow2=%.1f%% chain=%.1f%% ml+keep=%.1f%%  of isolated=%.1f ms\n",
           100.0 * ms_ins / tot, 100.0 * ms_pow / tot, 100.0 * ms_ch / tot,
           100.0 * (ms_mlkeep > 0 ? ms_mlkeep : 0) / tot, tot);

    lz0_finder_free(&f);
    free(in);
}

int main(int argc, char **argv) {
    size_t n = 1024 * 1024;
    const char *kind = "text";
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "1M"))
            n = 1024 * 1024;
        else if (!strcmp(argv[a], "2M"))
            n = 2 * 1024 * 1024;
        else if (!strcmp(argv[a], "5M"))
            n = 5 * 1024 * 1024;
        else if (!strcmp(argv[a], "text") || !strcmp(argv[a], "bin") || !strcmp(argv[a], "rep"))
            kind = argv[a];
    }
    run_one(kind, n);
    return 0;
}
