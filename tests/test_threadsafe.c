#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include "lz0dec.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(const unsigned char *p, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static unsigned char *make_test_payload(size_t n) {
    unsigned char *buf = (unsigned char *)malloc(n);
    if (!buf)
        return NULL;
    uint32_t rng = 0x13572468u;
    for (size_t i = 0; i < n; i++) {
        size_t region = (i >> 12) & 3u;
        if (region == 0) {
            /* structured repeating C-like records */
            static const char tmpl[] = "static int lz0_test_record_%04u = { off=%u, len=%u };\n";
            buf[i] = (unsigned char)tmpl[(i * 3u + (i >> 7)) % (sizeof(tmpl) - 1u)];
        } else if (region == 1) {
            /* 2D stride + diagonal correlation (16-byte rows) */
            buf[i] = (unsigned char)((i & 0x0fu) * 7u + ((i >> 4) & 0x1fu) * 3u);
        } else if (region == 2) {
            /* mixed low-entropy runs and short literals */
            rng = rng * 1664525u + 1013904223u;
            buf[i] = (i % 19u < 7u) ? (unsigned char)('A' + (i & 3u)) : (unsigned char)(rng >> 26);
        } else {
            /* higher entropy tail/patch */
            rng = rng * 1664525u + 1013904223u;
            buf[i] = (unsigned char)((rng >> 24) & 0x7fu);
        }
    }
    return buf;
}

typedef struct {
    int level;
    const unsigned char *in;
    size_t in_sz;
    const unsigned char *gold;
    size_t gold_sz;
    uint64_t gold_hash;
    int iters;
    int fail;
} worker_arg;

static void *worker_fn(void *arg) {
    worker_arg *w = (worker_arg *)arg;
    unsigned char *dec = (unsigned char *)malloc(w->in_sz + 64u);
    if (!dec) {
        w->fail = 1;
        return NULL;
    }
    for (int it = 0; it < w->iters; it++) {
        lz0_config cfg;
        lz0_config_level(&cfg, w->level);
        unsigned char *out = NULL;
        size_t out_sz = 0;
        lz0_stats st;
        if (lz0_compress(w->in, w->in_sz, &out, &out_sz, &cfg, &st) != 0) {
            w->fail = 2;
            break;
        }
        if (out_sz != w->gold_sz || fnv1a(out, out_sz) != w->gold_hash ||
            memcmp(out, w->gold, out_sz) != 0) {
            free(out);
            w->fail = 3;
            break;
        }
        int dsz = lz0dec_fast(out, (int)out_sz, dec, (int)w->in_sz);
        if (dsz != (int)w->in_sz || memcmp(dec, w->in, w->in_sz) != 0) {
            free(out);
            w->fail = 4;
            break;
        }
        free(out);
    }
    free(dec);
    return NULL;
}

int main(void) {
    const size_t n = 48u * 1024u;
    unsigned char *in = make_test_payload(n);
    if (!in) {
        fprintf(stderr, "OOM\n");
        return 1;
    }

    /* 1. Cross-level config isolation & global interference immunity */
    lz0_config cfg6_ref;
    lz0_config_level(&cfg6_ref, 6);
    unsigned char *ref6 = NULL;
    size_t ref6_sz = 0;
    if (lz0_compress(in, n, &ref6, &ref6_sz, &cfg6_ref, NULL) != 0) {
        fprintf(stderr, "FAIL: ref6 compress\n");
        return 1;
    }

    lz0_config cfg6_before, cfg9_interleave;
    lz0_config_level(&cfg6_before, 6);
    lz0_config_level(&cfg9_interleave, 9);

    /* Deliberately poison legacy globals before compressing with cfg6_before. */
    int sv_sawin = lz0_sa_win;
    int sv_lcode = lz0_len_code;
    int sv_lsb = lz0_offset_lsb_bits;
    int sv_l2 = lz0_len2_max_off;
    int sv_rle = lz0_rle_tail;
    int sv_fine = lz0_sdp_fine_matches;
    int sv_lscan = lz0_legacy_single_window_scan;

    lz0_sa_win = 0;
    lz0_len_code = 1;
    lz0_offset_lsb_bits = 7;
    lz0_len2_max_off = 0;
    lz0_rle_tail = 99;
    lz0_sdp_fine_matches = 1;
    lz0_legacy_single_window_scan = 1;

    unsigned char *out9 = NULL, *out6 = NULL;
    size_t out9_sz = 0, out6_sz = 0;
    if (lz0_compress(in, n, &out9, &out9_sz, &cfg9_interleave, NULL) != 0 ||
        lz0_compress(in, n, &out6, &out6_sz, &cfg6_before, NULL) != 0) {
        fprintf(stderr, "FAIL: interleaved compress\n");
        return 1;
    }
    if (out6_sz != ref6_sz || memcmp(out6, ref6, ref6_sz) != 0) {
        fprintf(stderr, "FAIL: cross-level isolation broken (got %zu vs ref %zu)\n",
                out6_sz, ref6_sz);
        return 1;
    }
    free(out9);
    free(out6);

    /* Test NULL config path under poisoned globals too. */
    unsigned char *null_a = NULL, *null_b = NULL;
    size_t null_a_sz = 0, null_b_sz = 0;
    if (lz0_compress(in, n, &null_a, &null_a_sz, NULL, NULL) != 0) {
        fprintf(stderr, "FAIL: NULL cfg compress (poisoned globals)\n");
        return 1;
    }
    lz0_sa_win = sv_sawin;
    lz0_len_code = sv_lcode;
    lz0_offset_lsb_bits = sv_lsb;
    lz0_len2_max_off = sv_l2;
    lz0_rle_tail = sv_rle;
    lz0_sdp_fine_matches = sv_fine;
    lz0_legacy_single_window_scan = sv_lscan;

    if (lz0_compress(in, n, &null_b, &null_b_sz, NULL, NULL) != 0 ||
        null_a_sz != null_b_sz || memcmp(null_a, null_b, null_a_sz) != 0) {
        fprintf(stderr, "FAIL: NULL cfg isolation broken\n");
        return 1;
    }
    free(null_a);
    free(null_b);
    free(ref6);

    /* 2. Concurrent multi-threaded encoding across levels 1, 3, 6, 8, 9 */
    const int levels[6] = {1, 3, 6, 8, 9, 6};
    unsigned char *gold[6] = {0};
    size_t gold_sz[6] = {0};
    uint64_t gold_hash[6] = {0};

    for (int t = 0; t < 6; t++) {
        lz0_config cfg;
        lz0_config_level(&cfg, levels[t]);
        if (lz0_compress(in, n, &gold[t], &gold_sz[t], &cfg, NULL) != 0) {
            fprintf(stderr, "FAIL: golden compress level %d\n", levels[t]);
            return 1;
        }
        gold_hash[t] = fnv1a(gold[t], gold_sz[t]);
    }

    pthread_t th[6];
    worker_arg args[6];
    for (int t = 0; t < 6; t++) {
        args[t].level = levels[t];
        args[t].in = in;
        args[t].in_sz = n;
        args[t].gold = gold[t];
        args[t].gold_sz = gold_sz[t];
        args[t].gold_hash = gold_hash[t];
        args[t].iters = (levels[t] == 9) ? 2 : 4;
        args[t].fail = 0;
        if (pthread_create(&th[t], NULL, worker_fn, &args[t]) != 0) {
            fprintf(stderr, "FAIL: pthread_create %d\n", t);
            return 1;
        }
    }
    for (int t = 0; t < 6; t++) {
        pthread_join(th[t], NULL);
        if (args[t].fail != 0) {
            fprintf(stderr, "FAIL: worker %d (level %d) code=%d\n", t, levels[t], args[t].fail);
            return 1;
        }
    }

    for (int t = 0; t < 6; t++)
        free(gold[t]);
    free(in);
    printf("PASS: cross-level isolation + 6-thread concurrent determinism OK\n");
    return 0;
}

