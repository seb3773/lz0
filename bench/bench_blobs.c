#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lz0.h"
#include "lz0dec.h"
#include "lz0_internal.h"

/* Freestanding UPKR C unpacker function */
void* upkr_unpack(void* destination, void* compressed_data);

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

typedef struct {
    const char *name;
    const char *path;
    int num_files;
    const char *desc;
} blob_target;

static const blob_target BLOBS[] = {
    { "blob_png_256k", "bench/blobs/blob_png_256k.bin", 4,  "4 PNGs" },
    { "blob_mix_256k", "bench/blobs/blob_mix_256k.bin", 5,  "ELF + PE + XLS + PNG + Text" },
    { "blob_png_512k", "bench/blobs/blob_png_512k.bin", 8,  "8 PNGs" },
    { "blob_mix_512k", "bench/blobs/blob_mix_512k.bin", 7,  "ELF + PE + PNGs + XLS + C + Text" },
    { "blob_png_2m",   "bench/blobs/blob_png_2m.bin",   8,  "8 distinct PNGs" },
    { "blob_mix_2m",   "bench/blobs/blob_mix_2m.bin",   9,  "ELF + PE + PNGs + XLS + Text" },
    { "blob_png_5m",   "bench/blobs/blob_png_5m.bin",   15, "15 distinct PNGs" },
    { "blob_mix_5m",   "bench/blobs/blob_mix_5m.bin",   15, "ELF + PE + SPARC + PNGs + XLS + Text + Fax" },
};

static int run_upkr_cli(const unsigned char *in, size_t in_sz,
                        unsigned char **out, size_t *out_sz,
                        double *enc_ms, double *dec_speed_mbs, int level) {
    char in_path[] = "/tmp/upkr_bench_in_XXXXXX";
    char out_path[] = "/tmp/upkr_bench_out_XXXXXX";
    int fd_in = mkstemp(in_path);
    int fd_out = mkstemp(out_path);
    if (fd_in < 0 || fd_out < 0) return -1;
    ssize_t w = write(fd_in, in, in_sz);
    (void)w;
    close(fd_in); close(fd_out);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "bench/external/bin/upkr -%d %s %s >/dev/null 2>&1",
             level, in_path, out_path);
    double t0 = now_ms();
    int rc = system(cmd);
    *enc_ms = now_ms() - t0;
    if (rc != 0) {
        unlink(in_path); unlink(out_path);
        return -1;
    }

    *out = read_file(out_path, out_sz);
    unlink(in_path); unlink(out_path);

    if (!*out || *out_sz == 0) return -1;

    /* Benchmark freestanding C unpacker */
    unsigned char *dest = (unsigned char *)malloc(in_sz + 64);
    if (!dest) return -1;

    int iters = (in_sz < 512 * 1024) ? 5 : 2;
    t0 = now_ms();
    void *end_ptr = NULL;
    for (int i = 0; i < iters; i++) {
        end_ptr = upkr_unpack(dest, *out);
    }
    double dec_ms = (now_ms() - t0) / iters;
    size_t restored_sz = (size_t)((unsigned char *)end_ptr - dest);

    int ok = (restored_sz == in_sz && memcmp(in, dest, in_sz) == 0);
    *dec_speed_mbs = (dec_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_ms / 1000.0) : 0.0;
    free(dest);

    return ok ? 0 : -1;
}

static void benchmark_blob(const blob_target *tgt, int test_upkr, int upkr_level) {
    size_t in_sz = 0;
    unsigned char *in = read_file(tgt->path, &in_sz);
    if (!in || in_sz == 0) {
        fprintf(stderr, "Error reading %s\n", tgt->path);
        free(in);
        return;
    }

    printf("\n### `%s` (%zu bytes / %.2f KiB, %d files - %s)\n\n",
           tgt->name, in_sz, (double)in_sz / 1024.0, tgt->num_files, tgt->desc);
    printf("| Compressor / Configuration | Packed Size | Ratio | Gain vs Raw | Enc. (ms) | Decomp. (MB/s) | Decompression |\n");
    printf("|---|---:|---:|---:|---:|---:|:---:|\n");

    /* 1. LZ0 Normal (-6) */
    {
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 6);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        double enc_ms = now_ms() - t0;

        int iters = (in_sz <= 512 * 1024) ? 50 : (in_sz <= 2 * 1024 * 1024) ? 10 : 3;
        unsigned char *out = (unsigned char *)malloc(in_sz + 64);
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters; i++) {
            d = lz0dec_fast(comp, (int)csz, out, (int)in_sz);
        }
        double dec_ms = (now_ms() - t0) / iters;
        int ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double dec_mbs = (dec_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_ms / 1000.0) : 0.0;

        printf("| **LZ0 Normal (-6)** | %zu B | **%.2f%%** | %+.2f%% | %.1f ms | **%.1f MB/s** | %s |\n",
               csz, (double)csz * 100.0 / in_sz, ((double)csz - (double)in_sz) * 100.0 / in_sz,
               enc_ms, dec_mbs, ok ? "✅ Bit-exact" : "❌ FAIL");
        free(comp);
        free(out);
    }

    /* 2. LZ0 Ultra (-9) [Multi-Window Stride Scan - Current] */
    size_t csz_ultra_current = 0;
    {
        lz0_legacy_single_window_scan = 0;
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 9);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        double enc_ms = now_ms() - t0;
        csz_ultra_current = csz;

        int iters = (in_sz <= 512 * 1024) ? 50 : (in_sz <= 2 * 1024 * 1024) ? 10 : 3;
        unsigned char *out = (unsigned char *)malloc(in_sz + 64);
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters; i++) {
            d = lz0dec_fast(comp, (int)csz, out, (int)in_sz);
        }
        double dec_ms = (now_ms() - t0) / iters;
        int ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double dec_mbs = (dec_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_ms / 1000.0) : 0.0;

        printf("| **LZ0 Ultra (-9) [Multi-Scan]** | %zu B | **%.2f%%** | %+.2f%% | %.1f ms | **%.1f MB/s** | %s |\n",
               csz, (double)csz * 100.0 / in_sz, ((double)csz - (double)in_sz) * 100.0 / in_sz,
               enc_ms, dec_mbs, ok ? "✅ Bit-exact" : "❌ FAIL");
        free(comp);
        free(out);
    }

    /* 3. LZ0 Ultra (-9) [Legacy Single-Window Stride Scan - Before] */
    {
        lz0_legacy_single_window_scan = 1;
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 9);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        double enc_ms = now_ms() - t0;
        (void)rc; (void)enc_ms;

        long delta = (long)csz_ultra_current - (long)csz;
        printf("| *LZ0 Ultra (-9) [Legacy 1-Win]* | %zu B | %.2f%% | %s%ld B vs Multi | - | - | (comparison) |\n",
               csz, (double)csz * 100.0 / in_sz, delta <= 0 ? "" : "+", delta);
        free(comp);
        lz0_legacy_single_window_scan = 0;
    }

    /* 4. UPKR */
    if (test_upkr) {
        unsigned char *comp = NULL;
        size_t csz = 0;
        double enc_ms = 0.0, dec_mbs = 0.0;
        int rc = run_upkr_cli(in, in_sz, &comp, &csz, &enc_ms, &dec_mbs, upkr_level);
        if (rc == 0) {
            printf("| **UPKR (-%d)** | %zu B | **%.2f%%** | %+.2f%% | %.1f ms | %.1f MB/s | ✅ Bit-exact |\n",
               upkr_level, csz, (double)csz * 100.0 / in_sz,
               ((double)csz - (double)in_sz) * 100.0 / in_sz, enc_ms, dec_mbs);
        } else {
            printf("| UPKR (-%d) | Failed / Timeout | - | - | - | - | ❌ FAIL |\n", upkr_level);
        }
        free(comp);
    } else {
        printf("| UPKR | (Excluded for size > 2 MiB: time > 10 min) | - | - | - | - | - |\n");
    }

    free(in);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int target_idx = -1;
    int upkr_level = 2;
    int test_upkr = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-upkr") == 0) {
            test_upkr = 0;
        } else if (strncmp(argv[i], "-l=", 3) == 0) {
            upkr_level = atoi(argv[i] + 3);
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            target_idx = atoi(argv[i] + 9);
        }
    }

    printf("# Comparative Benchmark: Heterogeneous Multi-Resource Blobs\n");
    printf("Corpus: 8 Blobs (Pure PNGs and Mixed binary/image/text, from 256 KiB to 5 MiB)\n\n");

    int n_targets = (int)(sizeof(BLOBS) / sizeof(BLOBS[0]));
    for (int i = 0; i < n_targets; i++) {
        if (target_idx >= 0 && target_idx != i)
            continue;

        int allow_upkr = test_upkr;
        int lvl = upkr_level;

        /* For 2MB: use upkr -1 (or -0). For 5MB: skip upkr unless requested */
        if (i >= 4) { // 2MB and 5MB
            if (i >= 6) { // 5MB
                allow_upkr = 0; // Skip 5MB UPKR to avoid 30min hang
            } else {
                lvl = 1; // 2MB UPKR -1
            }
        }

        benchmark_blob(&BLOBS[i], allow_upkr, lvl);
    }

    return 0;
}
