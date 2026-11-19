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
    lz0_sa_win = 16;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 1;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 1;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
}

static int record_stop(lz0_finder *f, uint32_t pos, int minm) {
    if (!f->sa || !f->rank || pos >= f->n)
        return 0;
    uint32_t n = f->n;
    uint32_t rr = f->rank[pos];
    if (rr == 0xffffffffu || rr >= f->sa_n)
        return 0;
    int r = (int)rr;
    int ns = (int)f->sa_n;
    int win = f->sa_win;
    int maxl = (int)(n - pos);
    int best = minm - 1;
    int stop = win;
    const unsigned char *in = f->in;
    for (int d = 1; d <= win; d++) {
        int rkL = r - d;
        int rkR = r + d;
        if (rkL >= 0) {
            uint32_t src = f->sa[rkL];
            if (src < pos) {
                int ml = lz0_matchlen(in + pos, in + src, maxl);
                if (ml > best)
                    best = ml;
            }
        }
        if (rkR < ns) {
            uint32_t src = f->sa[rkR];
            if (src < pos) {
                int ml = lz0_matchlen(in + pos, in + src, maxl);
                if (ml > best)
                    best = ml;
            }
        }
        if (d == 4 && best >= 16) {
            stop = 4;
            break;
        }
        if (d == 8 && best >= 16) {
            stop = 8;
            break;
        }
    }
    return stop;
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
    uint8_t *stops = (uint8_t *)calloc(n, 1);
    uint32_t *ranks = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!stops || !ranks) {
        lz0_finder_free(&f);
        free(in);
        return;
    }

    int minm = cfg.min_match;
    double t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++)
        stops[i] = (uint8_t)record_stop(&f, i, minm);
    double ms_record = rdtsc_ms() - t0;

    volatile uint64_t sink = 0;
    t0 = rdtsc_ms();
    for (uint32_t i = 0; i < (uint32_t)n; i++)
        ranks[i] = f.rank[i];
    double ms_rank = rdtsc_ms() - t0;
    sink += ranks[1];

    uint64_t n_sal = 0, n_skip = 0, n_ml = 0, n_oob = 0;
    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        int stop = stops[i];
        uint32_t rr = ranks[i];
        if (rr >= f.sa_n)
            continue;
        int r = (int)rr;
        int ns = (int)f.sa_n;
        for (int d = 1; d <= stop; d++) {
            int rkL = r - d;
            int rkR = r + d;
            if (rkL >= 0) {
                uint32_t src = f.sa[rkL];
                n_sal++;
                if (src >= i)
                    n_skip++;
                else
                    n_ml++;
            } else
                n_oob++;
            if (rkR < ns) {
                uint32_t src = f.sa[rkR];
                n_sal++;
                if (src >= i)
                    n_skip++;
                else
                    n_ml++;
            } else
                n_oob++;
        }
    }
    double ms_sa = rdtsc_ms() - t0;
    sink += n_sal;

    uint64_t n_ml2 = 0, sum_ml = 0, n_ok = 0;
    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        int stop = stops[i];
        uint32_t rr = ranks[i];
        if (rr >= f.sa_n)
            continue;
        int r = (int)rr;
        int ns = (int)f.sa_n;
        int maxl = (int)((uint32_t)n - i);
        for (int d = 1; d <= stop; d++) {
            int rkL = r - d;
            int rkR = r + d;
            if (rkL >= 0) {
                uint32_t src = f.sa[rkL];
                if (src < i) {
                    int ml = lz0_matchlen(in + i, in + src, maxl);
                    n_ml2++;
                    sum_ml += (uint64_t)(unsigned)ml;
                    if (ml >= minm)
                        n_ok++;
                }
            }
            if (rkR < ns) {
                uint32_t src = f.sa[rkR];
                if (src < i) {
                    int ml = lz0_matchlen(in + i, in + src, maxl);
                    n_ml2++;
                    sum_ml += (uint64_t)(unsigned)ml;
                    if (ml >= minm)
                        n_ok++;
                }
            }
        }
    }
    double ms_ml = rdtsc_ms() - t0;
    sink += sum_ml;

    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        int stop = stops[i];
        uint32_t rr = ranks[i];
        if (rr >= f.sa_n)
            continue;
        int r = (int)rr;
        int ns = (int)f.sa_n;
        for (int d = 1; d <= stop; d++) {
            int rkL = r - d;
            int rkR = r + d;
            if (rkL >= 0) {
                uint32_t src = f.sa[rkL];
                if (src >= i)
                    sink += src;
            }
            if (rkR < ns) {
                uint32_t src = f.sa[rkR];
                if (src >= i)
                    sink += src;
            }
        }
    }
    double ms_skip_only = rdtsc_ms() - t0;

    t0 = rdtsc_ms();
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        int stop = stops[i];
        uint32_t rr = ranks[i];
        if (rr >= f.sa_n)
            continue;
        int r = (int)rr;
        int ns = (int)f.sa_n;
        for (int d = 1; d <= stop; d++) {
            int rkL = r - d;
            int rkR = r + d;
            if (rkL >= 0)
                sink += f.sa[rkL];
            if (rkR < ns)
                sink += f.sa[rkR];
        }
    }
    double ms_saload = rdtsc_ms() - t0;

    uint64_t n_inv = (uint64_t)n - 1;
    uint64_t n_stop4 = 0, n_stop8 = 0, n_stop16 = 0;
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        if (stops[i] == 4)
            n_stop4++;
        else if (stops[i] == 8)
            n_stop8++;
        else
            n_stop16++;
    }

    printf("=== %s n=%zu saprobe-split sink=%llu ===\n", kind, n, (unsigned long long)sink);
    printf("  record(full probe)=%.1f  rank_scan=%.1f  sa_walk(src>=?)=%.1f  matchlen=%.1f\n",
           ms_record, ms_rank, ms_sa, ms_ml);
    printf("  sa_load_only=%.1f  sa_skip_branch=%.1f  implied_src_check=%.1f\n",
           ms_saload, ms_skip_only, ms_sa - ms_saload);
    printf("  ops  inv=%llu sa[]=%llu skip=%llu(%.1f%%) ml=%llu oob=%llu ok=%llu avgml=%.1f\n",
           (unsigned long long)n_inv, (unsigned long long)n_sal, (unsigned long long)n_skip,
           n_sal ? 100.0 * (double)n_skip / (double)n_sal : 0,
           (unsigned long long)n_ml2, (unsigned long long)n_oob, (unsigned long long)n_ok,
           n_ml2 ? (double)sum_ml / (double)n_ml2 : 0);
    printf("  ns   rank=%.2f sa[]=%.2f skip_br=%.2f ml=%.2f  stop4=%.1f%% stop8=%.1f%% stop16=%.1f%%\n",
           1e6 * ms_rank / (double)n_inv,
           n_sal ? 1e6 * ms_saload / (double)n_sal : 0,
           n_sal ? 1e6 * ms_skip_only / (double)n_sal : 0,
           n_ml2 ? 1e6 * ms_ml / (double)n_ml2 : 0,
           100.0 * (double)n_stop4 / (double)n_inv,
           100.0 * (double)n_stop8 / (double)n_inv,
           100.0 * (double)n_stop16 / (double)n_inv);
    printf("  share of (rank+sa_walk+ml): rank=%.1f%% sa_walk=%.1f%% matchlen=%.1f%%\n",
           100.0 * ms_rank / (ms_rank + ms_sa + ms_ml),
           100.0 * ms_sa / (ms_rank + ms_sa + ms_ml),
           100.0 * ms_ml / (ms_rank + ms_sa + ms_ml));

    lz0_finder_free(&f);
    free(stops);
    free(ranks);
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
