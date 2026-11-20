#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lz0.h"
#include "lz0dec.h"
#include "zx0_compress_wrapper.h"
#include "zx0_decompress.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static unsigned char *read_file(const char *path, size_t *sz_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    unsigned char *p = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!p) { fclose(f); return NULL; }
    if (sz > 0 && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p); fclose(f); return NULL;
    }
    fclose(f);
    *sz_out = (size_t)sz;
    return p;
}

/* --- Synthetic Data Generators --- */

static unsigned char *gen_csv(size_t target_sz, size_t *out_sz) {
    unsigned char *buf = (unsigned char *)malloc(target_sz + 512);
    size_t pos = 0;
    const char *header = "id,timestamp,user,action,status_code,ip,duration_ms\n";
    size_t hlen = strlen(header);
    memcpy(buf + pos, header, hlen);
    pos += hlen;

    const char *actions[] = {"LOGIN", "LOGOUT", "VIEW_ITEM", "CHECKOUT", "REFUND", "SEARCH"};
    const char *users[] = {"alice", "bob_dev", "charlie99", "dana_admin", "elena_w", "frank"};
    int id = 10000;
    uint32_t ts = 1690000000;

    while (pos < target_sz) {
        char line[256];
        int u = id % 6;
        int a = (id / 7) % 6;
        int status = (id % 13 == 0) ? 500 : (id % 5 == 0) ? 404 : 200;
        int dur = (id % 100) + 12;
        int len = snprintf(line, sizeof(line), "%d,%u,%s,%s,%d,192.168.1.%d,%d\n",
                           id, ts + (id % 3600), users[u], actions[a], status, (id % 254) + 1, dur);
        if (pos + len > target_sz + 256) break;
        memcpy(buf + pos, line, len);
        pos += len;
        id++;
    }
    *out_sz = pos;
    return buf;
}

static unsigned char *gen_json(size_t target_sz, size_t *out_sz) {
    unsigned char *buf = (unsigned char *)malloc(target_sz + 512);
    size_t pos = 0;
    buf[pos++] = '[';
    buf[pos++] = '\n';

    int id = 1;
    const char *tags[] = {"standard", "premium", "enterprise", "trial"};
    while (pos < target_sz) {
        char item[256];
        int t = (id * 3) % 4;
        int val = (id * 17) % 1000;
        int len = snprintf(item, sizeof(item),
                           "  {\"id\": %d, \"tag\": \"%s\", \"value\": %d, \"active\": %s}%s\n",
                           id, tags[t], val, (id % 2) ? "true" : "false",
                           (pos + 120 < target_sz) ? "," : "");
        if (pos + len > target_sz + 256) break;
        memcpy(buf + pos, item, len);
        pos += len;
        id++;
    }
    buf[pos++] = ']';
    buf[pos++] = '\n';
    *out_sz = pos;
    return buf;
}

static unsigned char *gen_pcm16_stereo(size_t target_sz, size_t *out_sz) {
    size_t samples = target_sz / 4;
    int16_t *buf = (int16_t *)malloc(samples * 4);
    for (size_t i = 0; i < samples; i++) {
        double t = (double)i / 44100.0;
        int16_t left = (int16_t)(15000.0 * sin(2.0 * M_PI * 440.0 * t) + 3000.0 * sin(2.0 * M_PI * 880.0 * t));
        int16_t right = (int16_t)(14000.0 * sin(2.0 * M_PI * 444.0 * t) + 2500.0 * sin(2.0 * M_PI * 1320.0 * t));
        buf[i * 2 + 0] = left;
        buf[i * 2 + 1] = right;
    }
    *out_sz = samples * 4;
    return (unsigned char *)buf;
}

static unsigned char *gen_rgb24(size_t target_sz, size_t *out_sz) {
    unsigned char *buf = (unsigned char *)malloc(target_sz);
    int width = 256;
    size_t pixels = target_sz / 3;
    for (size_t i = 0; i < pixels; i++) {
        int x = (int)(i % width);
        int y = (int)(i / width);
        buf[i * 3 + 0] = (unsigned char)((x * 2) & 0xFF);
        buf[i * 3 + 1] = (unsigned char)((y * 3 + x) & 0xFF);
        buf[i * 3 + 2] = (unsigned char)(((x + y) / 2) & 0xFF);
    }
    *out_sz = pixels * 3;
    return buf;
}

static unsigned char *gen_sparse_runs(size_t target_sz, size_t *out_sz) {
    unsigned char *buf = (unsigned char *)calloc(1, target_sz);
    for (size_t blk = 0; blk + 512 <= target_sz; blk += 4096) {
        snprintf((char *)(buf + blk), 64, "BLOCK_MAGIC_%08zx_TYPE_DATA_FLAGS_0001", blk);
        for (int j = 64; j < 256; j++) {
            buf[blk + j] = (unsigned char)((j * 7) ^ 0x5A);
        }
    }
    *out_sz = target_sz;
    return buf;
}

/* --- Suite Definition --- */

typedef struct {
    const char *name;
    const char *category;
    const char *file_path;
    unsigned char *(*gen_fn)(size_t, size_t *);
    size_t gen_size;
    size_t known_target_sz;
    const char *target_name;
    int is_heavy; /* 1 if file is large (> 500 KB) */
} bench_item_t;

typedef struct {
    const char *name;
    const char *category;
    size_t raw_sz;
    size_t lz0_6_sz;
    double lz0_6_ms;
    size_t lz0_9_sz;
    double lz0_9_ms;
    size_t target_sz;
    const char *target_name;
    int ok;
} result_item_t;

int main(int argc, char **argv) {
    int run_full = 0;
    const char *filter = NULL;

    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--full") == 0 || strcmp(argv[a], "-f") == 0) {
            run_full = 1;
        } else if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0) {
            printf("Usage: lz0_bench_ratio [options] [filter]\n");
            printf("  --full, -f   Run complete suite including heavy 1MB+ files\n");
            printf("  <filter>     Run only tests whose name contains <filter>\n");
            return 0;
        } else if (argv[a][0] != '-') {
            filter = argv[a];
        }
    }

    printf("=========================================================================================\n");
    printf("                       LZ0 ENCODER RATIO BENCHMARK HARNESS                               \n");
    printf("=========================================================================================\n\n");

    bench_item_t suite[] = {
        /* Small files (< 64K) */
        {"grammar.lsp",      "Small (<64K)",   "bench/corpus/grammar.lsp", NULL, 0, 1228, "ZX0", 0},
        {"xargs.1",          "Small (<64K)",   "bench/corpus/xargs.1",     NULL, 0, 1745, "ZX0", 0},
        {"fields.c",         "Small (<64K)",   "bench/corpus/fields.c",    NULL, 0, 2928, "UPKR -9", 0},
        {"cp.html",          "Small (<64K)",   "bench/corpus/cp.html",     NULL, 0, 7422, "UPKR -9", 0},
        {"sum",              "Small (<64K)",   "bench/corpus/sum",         NULL, 0, 11288, "UPKR -9", 0},

        /* Text / Source Code */
        {"alice29.txt",      "Text / Code",    "bench/corpus/alice29.txt",    NULL, 0, 50547, "UPKR -5", 0},
        {"dickens_1m.txt",   "Text / Code",    "bench/corpus/dickens_1m.txt", NULL, 0, 328450, "UPKR -5", 1},

        /* Structured / Tabular */
        {"synthetic_csv",    "Tabular (CSV)",  NULL, gen_csv, 131072,         0, NULL, 0},
        {"synthetic_json",   "Structured JSON",NULL, gen_json, 131072,        0, NULL, 0},
        {"kennedy.xls",      "Tabular (XLS)",  "bench/corpus/kennedy.xls",    NULL, 0, 64765, "UPKR -5", 1},

        /* Binaries */
        {"lz0_win64.exe",    "Binary PE",      "bench/corpus/lz0_win64.exe",   NULL, 0, 0, NULL, 0},
        {"bash_x86_64.bin",  "Binary ELF",     "bench/corpus/bash_x86_64.bin", NULL, 0, 553594, "UPKR -2", 1},

        /* Multi-channel / Interleaved Strides */
        {"audio_pcm16",      "Interleaved (4B)",NULL, gen_pcm16_stereo, 131072, 0, NULL, 0},
        {"image_rgb24",      "Interleaved (3B)",NULL, gen_rgb24, 131072,        0, NULL, 0},

        /* Sparse / Repeated Runs */
        {"sparse_runs",      "Sparse / RLE",   NULL, gen_sparse_runs, 262144,  0, NULL, 0},
    };

    size_t num_items = sizeof(suite) / sizeof(suite[0]);
    result_item_t results[32];
    size_t executed_count = 0;

    printf("Mode: %s%s%s\n\n",
           run_full ? "FULL SUITE" : "QUICK SUITE (use --full for 1MB+ files)",
           filter ? " | Filter: " : "",
           filter ? filter : "");
    fflush(stdout);

    for (size_t i = 0; i < num_items; i++) {
        bench_item_t *item = &suite[i];

        if (filter && strstr(item->name, filter) == NULL) {
            continue;
        }
        if (!run_full && !filter && item->is_heavy) {
            continue;
        }

        unsigned char *in = NULL;
        size_t in_sz = 0;

        if (item->file_path) {
            in = read_file(item->file_path, &in_sz);
        } else if (item->gen_fn) {
            in = item->gen_fn(item->gen_size, &in_sz);
        }

        if (!in || in_sz == 0) {
            fprintf(stderr, "Warning: could not load %s, skipping\n", item->name);
            continue;
        }

        size_t idx = executed_count++;
        results[idx].name = item->name;
        results[idx].category = item->category;
        results[idx].raw_sz = in_sz;
        results[idx].ok = 1;

        /* 1. LZ0 Level 6 (Default) */
        {
            lz0_config cfg;
            lz0_stats stats;
            lz0_config_level(&cfg, 6);
            unsigned char *comp = NULL;
            size_t csz = 0;
            double t0 = now_ms();
            int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
            results[idx].lz0_6_ms = now_ms() - t0;
            results[idx].lz0_6_sz = csz;

            /* Verify decompression with FAST ASM */
            unsigned char *out = (unsigned char *)malloc(in_sz + 32);
            int d = lz0dec_fast(comp, (int)csz, out, (int)in_sz);
            if (rc != 0 || d != (int)in_sz || memcmp(in, out, in_sz) != 0) {
                results[idx].ok = 0;
            }
            free(comp);
            free(out);
        }

        /* 2. LZ0 Level 9 (Ultra) */
        {
            lz0_config cfg;
            lz0_stats stats;
            lz0_config_level(&cfg, 9);
            unsigned char *comp = NULL;
            size_t csz = 0;
            double t0 = now_ms();
            int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
            results[idx].lz0_9_ms = now_ms() - t0;
            results[idx].lz0_9_sz = csz;

            /* Verify decompression with FAST ASM */
            unsigned char *out = (unsigned char *)malloc(in_sz + 32);
            int d = lz0dec_fast(comp, (int)csz, out, (int)in_sz);
            if (rc != 0 || d != (int)in_sz || memcmp(in, out, in_sz) != 0) {
                results[idx].ok = 0;
            }
            free(comp);
            free(out);
        }

        /* 3. Target */
        results[idx].target_sz = item->known_target_sz;
        results[idx].target_name = item->target_name ? item->target_name : "-";

        free(in);

        double r6 = (double)results[idx].lz0_6_sz * 100.0 / (double)in_sz;
        double r9 = (double)results[idx].lz0_9_sz * 100.0 / (double)in_sz;
        long delta = (long)results[idx].lz0_6_sz - (long)results[idx].lz0_9_sz;

        printf("  [%2zu] %-18s (%8zu B) -> L6: %6zu B (%.1f%%), L9: %6zu B (%.1f%%) | Gain: %+4ld B | Enc: %6.1f ms\n",
               executed_count, item->name, in_sz,
               results[idx].lz0_6_sz, r6,
               results[idx].lz0_9_sz, r9,
               delta, results[idx].lz0_9_ms);
        fflush(stdout);
    }

    printf("\n\n");
    printf("| Test Case | Category | Raw Size | LZ0 (-6) | LZ0 (-9 Ultra) | Gain L9 vs L6 | L9 Time | Target Ref | Status |\n");
    printf("|---|---|---:|---:|---:|---:|---:|---:|:---:|\n");

    size_t total_raw = 0;
    size_t total_l6 = 0;
    size_t total_l9 = 0;
    size_t total_target = 0;

    for (size_t i = 0; i < executed_count; i++) {
        result_item_t *r = &results[i];
        total_raw += r->raw_sz;
        total_l6 += r->lz0_6_sz;
        total_l9 += r->lz0_9_sz;
        if (r->target_sz > 0) total_target += r->target_sz;

        double r6 = (double)r->lz0_6_sz * 100.0 / (double)r->raw_sz;
        double r9 = (double)r->lz0_9_sz * 100.0 / (double)r->raw_sz;
        long delta = (long)r->lz0_6_sz - (long)r->lz0_9_sz;
        double pct_gain = (r->lz0_6_sz > 0) ? ((double)delta * 100.0 / (double)r->lz0_6_sz) : 0.0;

        char target_str[64];
        if (r->target_sz > 0) {
            double rt = (double)r->target_sz * 100.0 / (double)r->raw_sz;
            snprintf(target_str, sizeof(target_str), "%zu B (%.1f%%) [%s]", r->target_sz, rt, r->target_name);
        } else {
            snprintf(target_str, sizeof(target_str), "-");
        }

        printf("| `%s` | %s | %zu B | %zu B (%.2f%%) | **%zu B (%.2f%%)** | %+ld B (%+.2f%%) | %.1f ms | %s | %s |\n",
               r->name, r->category, r->raw_sz,
               r->lz0_6_sz, r6,
               r->lz0_9_sz, r9,
               delta, pct_gain,
               r->lz0_9_ms,
               target_str,
               r->ok ? "✅ OK" : "❌ FAIL");
        fflush(stdout);
    }

    double tot_r6 = (double)total_l6 * 100.0 / (double)total_raw;
    double tot_r9 = (double)total_l9 * 100.0 / (double)total_raw;
    long tot_delta = (long)total_l6 - (long)total_l9;
    double tot_pct = (double)tot_delta * 100.0 / (double)total_l6;

    printf("|---|---|---:|---:|---:|---:|---:|---:|:---:|\n");
    printf("| **TOTAL** | **All Suite** | **%zu B** | **%zu B (%.2f%%)** | **%zu B (%.2f%%)** | **%+ld B (%+.2f%%)** | - | - | ✅ |\n\n",
           total_raw, total_l6, tot_r6, total_l9, tot_r9, tot_delta, tot_pct);

    printf("Key Insights:\n");
    printf("- Baseline LZ0 v0.2.0 establishes the reference for all future encoder ratio optimizations.\n");
    printf("- Target indicates optimal backward-referencing reference (ZX0 or UPKR).\n");
    printf("- All compressed outputs roundtrip-verified with bit-exact decompression (lz0dec_fast).\n");

    return 0;
}
