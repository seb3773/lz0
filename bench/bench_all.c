#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <lz4.h>
#include "lz0.h"
#include "lz0dec.h"
#include "zx0_compress_wrapper.h"
#include "zx0_decompress.h"

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

static int run_upkr_cli(const unsigned char *in, size_t in_sz,
                        unsigned char **out, size_t *out_sz,
                        double *enc_ms, double *dec_ms, int level) {
    char in_path[] = "/tmp/upkr_in_XXXXXX";
    char out_path[] = "/tmp/upkr_out_XXXXXX";
    char dec_path[] = "/tmp/upkr_dec_XXXXXX";
    int fd_in = mkstemp(in_path);
    int fd_out = mkstemp(out_path);
    int fd_dec = mkstemp(dec_path);
    if (fd_in < 0 || fd_out < 0 || fd_dec < 0) return -1;
    write(fd_in, in, in_sz);
    close(fd_in); close(fd_out); close(fd_dec);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "bench/external/bin/upkr -%d %s %s >/dev/null 2>&1",
             level, in_path, out_path);
    double t0 = now_ms();
    int rc = system(cmd);
    *enc_ms = now_ms() - t0;
    if (rc != 0) {
        unlink(in_path); unlink(out_path); unlink(dec_path);
        return -1;
    }

    *out = read_file(out_path, out_sz);

    snprintf(cmd, sizeof(cmd), "bench/external/bin/upkr -d %s %s >/dev/null 2>&1",
             out_path, dec_path);
    t0 = now_ms();
    rc = system(cmd);
    *dec_ms = now_ms() - t0;

    size_t restored_sz = 0;
    unsigned char *restored = read_file(dec_path, &restored_sz);
    int ok = (rc == 0 && restored && restored_sz == in_sz &&
              memcmp(in, restored, in_sz) == 0);

    free(restored);
    unlink(in_path); unlink(out_path); unlink(dec_path);
    return ok ? 0 : -1;
}

typedef struct {
    const char *algo;
    size_t comp_size;
    double ratio;
    double enc_ms;
    double dec_speed_mbs;
    int ok;
} bench_result_t;

static void benchmark_file(const char *path, int force_zx0) {
    size_t in_sz = 0;
    unsigned char *in = read_file(path, &in_sz);
    if (!in || in_sz == 0) {
        fprintf(stderr, "Skipping %s (cannot read or empty)\n", path);
        free(in);
        return;
    }

    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    printf("\n### `%s` (%zu bytes / %.2f KiB)\n\n", basename, in_sz, (double)in_sz / 1024.0);
    printf("| Compresseur | Taille comp. | Ratio | Enc. (ms) | Déc. (Mo/s) | Intégrité |\n");
    printf("|---|---:|---:|---:|---:|:---:|\n");

    // 1. LZ0 (H2C8 - standard level 6)
    {
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 6);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        double enc_ms = now_ms() - t0;

        int iters = (in_sz < 256 * 1024) ? 50 : (in_sz < 1024 * 1024) ? 20 : 5;
        unsigned char *out = (unsigned char *)malloc(in_sz + 16);
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters; i++) {
            d = lz0_decompress_to(comp, (int)csz, out, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters;
        int ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double speed = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("| **LZ0 (-6 C)** | %zu B | **%.2f%%** | %.1f ms | %.1f Mo/s | %s |\n",
               csz, (double)csz * 100.0 / in_sz, enc_ms, speed, ok ? "✅ OK" : "❌ FAIL");

#if defined(__x86_64__) || defined(_M_X64)
        t0 = now_ms();
        int dasm = 0;
        for (int i = 0; i < iters; i++) {
            dasm = lz0dec_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double dec_asm_ms = (now_ms() - t0) / iters;
        int ok_asm = (rc == 0 && dasm == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double speed_asm = (dec_asm_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_asm_ms / 1000.0) : 0;
        printf("| **LZ0 (-6 ASM x86_64)** | %zu B | **%.2f%%** | - | **%.1f Mo/s** | %s |\n",
               csz, (double)csz * 100.0 / in_sz, speed_asm, ok_asm ? "✅ OK" : "❌ FAIL");
#endif

        free(comp);
        free(out);
    }

    // 1b. LZ0 (Ultra level 9)
    {
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 9);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        double enc_ms = now_ms() - t0;

        int iters = (in_sz < 256 * 1024) ? 50 : (in_sz < 1024 * 1024) ? 20 : 5;
        unsigned char *out = (unsigned char *)malloc(in_sz + 16);
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters; i++) {
            d = lz0_decompress_to(comp, (int)csz, out, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters;
        int ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double speed = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("| **LZ0 (-9 C)** | %zu B | **%.2f%%** | %.1f ms | %.1f Mo/s | %s |\n",
               csz, (double)csz * 100.0 / in_sz, enc_ms, speed, ok ? "✅ OK" : "❌ FAIL");

#if defined(__x86_64__) || defined(_M_X64)
        t0 = now_ms();
        int dasm = 0;
        for (int i = 0; i < iters; i++) {
            dasm = lz0dec_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double dec_asm_ms = (now_ms() - t0) / iters;
        int ok_asm = (rc == 0 && dasm == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double speed_asm = (dec_asm_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_asm_ms / 1000.0) : 0;
        printf("| **LZ0 (-9 ASM x86_64)** | %zu B | **%.2f%%** | - | **%.1f Mo/s** | %s |\n",
               csz, (double)csz * 100.0 / in_sz, speed_asm, ok_asm ? "✅ OK" : "❌ FAIL");
#endif

        free(comp);
        free(out);
    }

    // 2. ZX0
    if (in_sz <= 64 * 1024 || force_zx0) {
        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = zx0_compress_c(in, in_sz, &comp, &csz);
        double enc_ms = now_ms() - t0;

        int iters = (in_sz < 64 * 1024) ? 20 : 5;
        unsigned char *out = (unsigned char *)malloc(in_sz + 16);
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters; i++) {
            d = zx0_decompress_to(comp, (int)csz, out, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters;
        int ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double speed = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("| ZX0 | %zu B | %.2f%% | %.1f ms | %.1f Mo/s | %s |\n",
               csz, (double)csz * 100.0 / in_sz, enc_ms, speed, ok ? "✅ OK" : "❌ FAIL");

        free(comp);
        free(out);
    } else {
        printf("| ZX0 | *Ignoré* | - | *Trop lent (>64K)* | - | - |\n");
    }

    // 3. UPKR
    if (access("bench/external/bin/upkr", X_OK) == 0) {
        int level = (in_sz <= 128 * 1024) ? 9 : (in_sz <= 1024 * 1024) ? 5 : 2;
        unsigned char *comp = NULL;
        size_t csz = 0;
        double enc_ms = 0, dec_ms = 0;
        int rc = run_upkr_cli(in, in_sz, &comp, &csz, &enc_ms, &dec_ms, level);
        if (rc == 0) {
            double speed = (dec_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_ms / 1000.0) : 0;
            printf("| UPKR (-%d) | %zu B | %.2f%% | %.1f ms | %.1f Mo/s | %s |\n",
                   level, csz, (double)csz * 100.0 / in_sz, enc_ms, speed, "✅ OK");
        } else {
            printf("| UPKR (-%d) | - | - | - | - | ❌ FAIL |\n", level);
        }
        free(comp);
    }

    // 4. LZ4
    {
        int max_dst = LZ4_compressBound((int)in_sz);
        char *comp = (char *)malloc(max_dst);
        char *out = (char *)malloc(in_sz);

        double t0 = now_ms();
        int csz = LZ4_compress_default((const char *)in, comp, (int)in_sz, max_dst);
        double enc_ms = now_ms() - t0;

        int iters = (in_sz < 256 * 1024) ? 100 : (in_sz < 1024 * 1024) ? 50 : 20;
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters; i++) {
            d = LZ4_decompress_safe(comp, out, csz, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters;
        int ok = (csz > 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        double speed = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("| LZ4 (default) | %d B | %.2f%% | %.1f ms | %.1f Mo/s | %s |\n",
               csz, (double)csz * 100.0 / in_sz, enc_ms, speed, ok ? "✅ OK" : "❌ FAIL");

        free(comp);
        free(out);
    }

    free(in);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            benchmark_file(argv[i], 0);
        }
        return 0;
    }

    // Default: run on selection of corpus files
    const char *default_files[] = {
        "bench/corpus/fields.c",
        "bench/corpus/alice29.txt",
        "bench/corpus/plrabn12.txt",
        "bench/corpus/kennedy.xls",
        "bench/corpus/dickens_1m.txt",
        "bench/corpus/bash_x86_64.bin",
        NULL
    };

    printf("# Benchmark Comparatif Multi-Compresseurs\n");
    printf("**Comparaison :** LZ0 (H2C8) vs ZX0 vs UPKR vs LZ4\n");

    for (int i = 0; default_files[i]; i++) {
        if (access(default_files[i], R_OK) == 0) {
            benchmark_file(default_files[i], 0);
        }
    }
    return 0;
}

