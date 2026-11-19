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

typedef struct {
    uint32_t pos;
    uint32_t src;
} ml_pair;

typedef struct {
    ml_pair *p;
    uint32_t n;
    uint32_t cap;
} pair_vec;

typedef struct {
    uint64_t n;
    uint64_t sum;
    uint64_t max;
    uint64_t hist[7];
} ml_hist;

static void hist_add(ml_hist *h, int ml) {
    h->n++;
    h->sum += (uint64_t)(unsigned)ml;
    if ((uint64_t)(unsigned)ml > h->max)
        h->max = (uint64_t)(unsigned)ml;
    int b;
    if (ml <= 3)
        b = 0;
    else if (ml <= 7)
        b = 1;
    else if (ml <= 15)
        b = 2;
    else if (ml <= 31)
        b = 3;
    else if (ml <= 63)
        b = 4;
    else if (ml <= 127)
        b = 5;
    else
        b = 6;
    h->hist[b]++;
}

static void vec_push(pair_vec *v, uint32_t pos, uint32_t src) {
    if (v->n >= v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 1u << 20;
        ml_pair *np = (ml_pair *)realloc(v->p, (size_t)nc * sizeof(ml_pair));
        if (!np)
            return;
        v->p = np;
        v->cap = nc;
    }
    v->p[v->n].pos = pos;
    v->p[v->n].src = src;
    v->n++;
}

static uint32_t chain_skip_prev(lz0_finder *f, uint32_t cur, int steps) {
    int t = 0;
    while (t < steps && cur != LZ0_CHAIN_NIL) {
        cur = f->prev[cur];
        t++;
    }
    return cur;
}

static void collect_hash(lz0_finder *f, uint32_t pos, pair_vec *v, ml_hist *h) {
    const unsigned char *in = f->in;
    uint32_t n = f->n;
    int minm = f->min_match;
    int maxl = (int)(n - pos);
    if (maxl < 2 || !f->head)
        return;
    int best_len = minm - 1;
    for (uint32_t off = 1; off <= pos; ) {
        uint32_t src = pos - off;
        if (in[src] == in[pos]) {
            int probe = best_len > 0 ? best_len : (minm - 1);
            if (!(probe > 0 && probe < maxl && in[src + probe] != in[pos + probe])) {
                int ml = lz0_matchlen(in + pos, in + src, maxl);
                hist_add(h, ml);
                vec_push(v, pos, src);
                if (ml >= minm && ml > best_len)
                    best_len = ml;
            }
        }
        if (off > (pos >> 1))
            break;
        off <<= 1;
    }
    uint32_t hsh;
    if (pos + 2 < n)
        hsh = ((uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8) | ((uint32_t)in[pos + 2] << 16)) *
                  2654435761u >>
              (32 - LZ0_HASH_BITS);
    else
        return;
    uint32_t cur = f->head[hsh];
    int checks = 0;
    int gap = 1;
    int near = 4;
    int visited = 0;
    int visit_lim = 1 << 16;
    while (cur != 0xffffffffu && checks < f->max_chain && visited < visit_lim) {
        if (cur >= pos) {
            cur = f->prev[cur];
            visited++;
            continue;
        }
        visited++;
        if (in[cur] == in[pos]) {
            int probe = best_len > 0 ? best_len : (minm - 1);
            if (!(probe > 0 && probe < maxl && in[cur + probe] != in[pos + probe])) {
                int ml = lz0_matchlen(in + pos, in + cur, maxl);
                hist_add(h, ml);
                vec_push(v, pos, cur);
                if (ml >= minm && ml > best_len)
                    best_len = ml;
            }
        }
        checks++;
        int jmpn = (near > 0) ? 1 : gap;
        if (near > 0)
            near--;
        else if (gap < (1 << 20))
            gap <<= 1;
        cur = chain_skip_prev(f, cur, jmpn);
        visited += jmpn;
    }
}

static void collect_sa(lz0_finder *f, uint32_t pos, pair_vec *v, ml_hist *h) {
    if (!f->sa || !f->rank || pos >= f->n)
        return;
    uint32_t n = f->n;
    uint32_t rr = f->rank[pos];
    if (rr == 0xffffffffu || rr >= f->sa_n)
        return;
    int r = (int)rr;
    int ns = (int)f->sa_n;
    int win = f->sa_win;
    int maxl = (int)(n - pos);
    int minm = f->min_match;
    int best = minm - 1;
    const unsigned char *in = f->in;
    for (int d = 1; d <= win; d++) {
        int rkL = r - d;
        int rkR = r + d;
        if (rkL >= 0) {
            uint32_t src = f->sa[rkL];
            if (src < pos) {
                int ml = lz0_matchlen(in + pos, in + src, maxl);
                hist_add(h, ml);
                vec_push(v, pos, src);
                if (ml > best)
                    best = ml;
            }
        }
        if (rkR < ns) {
            uint32_t src = f->sa[rkR];
            if (src < pos) {
                int ml = lz0_matchlen(in + pos, in + src, maxl);
                hist_add(h, ml);
                vec_push(v, pos, src);
                if (ml > best)
                    best = ml;
            }
        }
        if (d == 4 && best >= 16)
            break;
        if (d == 8 && best >= 16)
            break;
    }
}

static double replay(const unsigned char *in, uint32_t n, const pair_vec *v, int sample) {
    uint32_t use = v->n;
    uint32_t stride = 1;
    if (sample > 0 && use > (uint32_t)sample) {
        stride = use / (uint32_t)sample;
        if (stride < 1)
            stride = 1;
        use = (use + stride - 1) / stride;
    }
    volatile uint64_t sink = 0;
    double t0 = rdtsc_ms();
    if (sample > 0 && stride > 1) {
        for (uint32_t i = 0; i < v->n; i += stride) {
            uint32_t pos = v->p[i].pos;
            uint32_t src = v->p[i].src;
            int maxl = (int)(n - pos);
            sink += (uint64_t)(unsigned)lz0_matchlen(in + pos, in + src, maxl);
        }
    } else {
        for (uint32_t i = 0; i < v->n; i++) {
            uint32_t pos = v->p[i].pos;
            uint32_t src = v->p[i].src;
            int maxl = (int)(n - pos);
            sink += (uint64_t)(unsigned)lz0_matchlen(in + pos, in + src, maxl);
        }
    }
    double ms = rdtsc_ms() - t0;
    if (sink == 0xffffffffffffffffull)
        printf("sink\n");
    if (stride > 1)
        ms *= (double)stride;
    (void)use;
    return ms;
}

static void print_hist(const char *tag, const ml_hist *h, double ms) {
    static const char *names[] = {"1-3", "4-7", "8-15", "16-31", "32-63", "64-127", "128+"};
    double avg = h->n ? (double)h->sum / (double)h->n : 0;
    double ns = h->n ? 1e6 * ms / (double)h->n : 0;
    double nsb = h->sum ? 1e6 * ms / (double)h->sum : 0;
    printf("  %s n=%llu bytes=%llu avg=%.1f max=%llu time=%.1f ms  ns/call=%.2f ns/byte=%.3f\n",
           tag, (unsigned long long)h->n, (unsigned long long)h->sum, avg,
           (unsigned long long)h->max, ms, ns, nsb);
    printf("    hist ");
    for (int i = 0; i < 7; i++) {
        if (h->hist[i])
            printf("%s:%llu(%.1f%%) ", names[i], (unsigned long long)h->hist[i],
                   h->n ? 100.0 * (double)h->hist[i] / (double)h->n : 0);
    }
    printf("\n");
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

    pair_vec vh = {0}, vs = {0};
    ml_hist hh, hs;
    memset(&hh, 0, sizeof(hh));
    memset(&hs, 0, sizeof(hs));
    for (uint32_t i = 1; i < (uint32_t)n; i++) {
        lz0_finder_insert_upto(&f, i);
        collect_hash(&f, i, &vh, &hh);
        collect_sa(&f, i, &vs, &hs);
    }

    int sample = 0;
    if (!strcmp(kind, "rep"))
        sample = 20000;

    double ms_h = replay(in, (uint32_t)n, &vh, sample);
    double ms_s = replay(in, (uint32_t)n, &vs, sample);

    printf("=== %s n=%zu isolated lz0_matchlen replay%s ===\n", kind, n,
           sample ? " (sampled, scaled)" : "");
    print_hist("hash", &hh, ms_h);
    print_hist("SA  ", &hs, ms_s);
    double tot = ms_h + ms_s;
    printf("  share hash=%.1f%% SA=%.1f%%  combined=%.1f ms\n",
           tot > 0 ? 100.0 * ms_h / tot : 0, tot > 0 ? 100.0 * ms_s / tot : 0, tot);

    lz0_finder_free(&f);
    free(vh.p);
    free(vs.p);
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
