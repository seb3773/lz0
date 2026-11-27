#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "lz0.h"
#include "lz0dec.h"

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
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

static void bench_one(const char *path) {
    size_t in_sz = 0;
    unsigned char *in = read_file(path, &in_sz);
    if (!in || in_sz == 0) return;

    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    // Compress with LZ0 standard (-6)
    lz0_config cfg;
    lz0_stats stats;
    lz0_config_level(&cfg, 6);
    unsigned char *comp = NULL;
    size_t csz = 0;
    if (lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats) != 0) {
        fprintf(stderr, "Compress error on %s\n", path);
        free(in);
        return;
    }

    // Number of iterations based on size
    int iters;
#if defined(__aarch64__) || defined(_M_ARM64) || (defined(__riscv) && __riscv_xlen == 64)
    // Adjusted iteration counts for QEMU user-mode emulation
    if (in_sz < 32 * 1024) iters = 200;
    else if (in_sz < 256 * 1024) iters = 50;
    else if (in_sz < 1024 * 1024) iters = 15;
    else iters = 8;
#else
    if (in_sz < 32 * 1024) iters = 2000;
    else if (in_sz < 256 * 1024) iters = 500;
    else if (in_sz < 1024 * 1024) iters = 150;
    else iters = 50;
#endif

    unsigned char *out = (unsigned char *)malloc(in_sz + 32);

    // Warm-up cache
    for (int i = 0; i < 5; i++) {
        lz0_decompress_to(comp, (int)csz, out, (int)in_sz);
#if defined(__x86_64__) || defined(_M_X64)
        lz0dec_ultra_tiny_x86_64(comp, out);
        lz0dec_tiny_x86_64(comp, (int)csz, out, (int)in_sz);
        lz0dec_x86_64(comp, (int)csz, out, (int)in_sz);
#elif defined(__aarch64__) || defined(_M_ARM64)
        lz0dec_ultra_tiny_arm64(comp, out);
        lz0dec_tiny_arm64(comp, (int)csz, out, (int)in_sz);
        lz0dec_arm64(comp, (int)csz, out, (int)in_sz);
#elif (defined(__riscv) && __riscv_xlen == 64)
        lz0dec_ultra_tiny_riscv64(comp, out);
        lz0dec_tiny_riscv64(comp, (int)csz, out, (int)in_sz);
        lz0dec_riscv64(comp, (int)csz, out, (int)in_sz);
#endif
    }

    // Benchmark C: lz0_decompress_to
    double best_c_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0_decompress_to(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_c_ns) best_c_ns = elapsed;
    }

    // Benchmark C min: lz0dec_min
    double best_min_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_min(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_min_ns) best_min_ns = elapsed;
    }

#if defined(__x86_64__) || defined(_M_X64)
    // Benchmark ASM Ultra-Tiny: lz0dec_ultra_tiny_x86_64
    double best_utiny_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_ultra_tiny_x86_64(comp, out);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_utiny_ns) best_utiny_ns = elapsed;
    }
    // Benchmark ASM Tiny: lz0dec_tiny_x86_64
    double best_tiny_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_tiny_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_tiny_ns) best_tiny_ns = elapsed;
    }
    // Benchmark ASM Compact: lz0dec_x86_64
    double best_asm_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_asm_ns) best_asm_ns = elapsed;
    }
    // Benchmark ASM Fast: lz0dec_fast_x86_64
    double best_fast_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_fast_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_fast_ns) best_fast_ns = elapsed;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    // Benchmark ASM Ultra-Tiny: lz0dec_ultra_tiny_arm64
    double best_utiny_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_ultra_tiny_arm64(comp, out);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_utiny_ns) best_utiny_ns = elapsed;
    }
    // Benchmark ASM Tiny: lz0dec_tiny_arm64
    double best_tiny_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_tiny_arm64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_tiny_ns) best_tiny_ns = elapsed;
    }
    // Benchmark ASM Compact: lz0dec_arm64
    double best_asm_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_arm64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_asm_ns) best_asm_ns = elapsed;
    }
    // Benchmark ASM Fast: lz0dec_fast_arm64
    double best_fast_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_fast_arm64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_fast_ns) best_fast_ns = elapsed;
    }
#elif (defined(__riscv) && __riscv_xlen == 64)
    // Benchmark ASM Ultra-Tiny: lz0dec_ultra_tiny_riscv64
    double best_utiny_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_ultra_tiny_riscv64(comp, out);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_utiny_ns) best_utiny_ns = elapsed;
    }
    // Benchmark ASM Tiny: lz0dec_tiny_riscv64
    double best_tiny_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_tiny_riscv64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_tiny_ns) best_tiny_ns = elapsed;
    }
    // Benchmark ASM Compact: lz0dec_riscv64
    double best_asm_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_riscv64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_asm_ns) best_asm_ns = elapsed;
    }
    // Benchmark ASM Fast: lz0dec_fast_riscv64
    double best_fast_ns = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            lz0dec_fast_riscv64(comp, (int)csz, out, (int)in_sz);
        }
        double elapsed = (now_ns() - t0) / iters;
        if (elapsed < best_fast_ns) best_fast_ns = elapsed;
    }
#else
    double best_asm_ns = 0;
    double best_fast_ns = 0;
#endif

    double mib = (double)in_sz / (1024.0 * 1024.0);
    double speed_c = mib / (best_c_ns / 1e9);
    double speed_min = mib / (best_min_ns / 1e9);
    double speed_asm = (best_asm_ns > 0) ? mib / (best_asm_ns / 1e9) : 0;
    double speed_fast = (best_fast_ns > 0) ? mib / (best_fast_ns / 1e9) : 0;
    double speedup_vs_c = (best_c_ns > 0 && best_fast_ns > 0) ? ((speed_fast / speed_c) - 1.0) * 100.0 : 0;

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64) || (defined(__riscv) && __riscv_xlen == 64)
    double speed_utiny = (best_utiny_ns > 0) ? mib / (best_utiny_ns / 1e9) : 0;
    double speed_tiny = (best_tiny_ns > 0) ? mib / (best_tiny_ns / 1e9) : 0;
    printf("| `%s` (%.1f K) | %.1f MB/s | %.1f MB/s | %.1f MB/s | %.1f MB/s | %.1f MB/s | **%.1f MB/s** | **%+5.1f%%** |\n",
           basename, (double)in_sz / 1024.0,
           speed_c,
           speed_min,
           speed_utiny,
           speed_tiny,
           speed_asm,
           speed_fast,
           speedup_vs_c);
#else
    printf("| `%s` (%.1f K) | %.1f MB/s | %.1f MB/s | %.1f MB/s | **%.1f MB/s** | **%+5.1f%%** |\n",
           basename, (double)in_sz / 1024.0,
           speed_c,
           speed_min,
           speed_asm,
           speed_fast,
           speedup_vs_c);
#endif
    fflush(stdout);

    free(out);
    free(comp);
    free(in);
}

int main(int argc, char **argv) {
#if defined(__aarch64__) || defined(_M_ARM64)
    printf("# ARM64 (AArch64) Decompression Benchmark: Standard C vs Minimal C vs Ultra-Tiny ASM vs Tiny ASM vs Compact ASM vs FAST ASM\n\n");
    printf("| File (Size) | Standard C | Minimal C (`lz0dec_min`) | Ultra-Tiny ASM (`lz0dec_ultra_tiny_arm64`) | Tiny ASM (`lz0dec_tiny_arm64`) | Compact ASM (`lz0dec_arm64`) | FAST ASM (`lz0dec_fast_arm64`) | Speedup FAST vs C |\n");
#elif (defined(__riscv) && __riscv_xlen == 64)
    printf("# RISC-V 64 Decompression Benchmark: Standard C vs Minimal C vs Ultra-Tiny ASM vs Tiny ASM vs Compact ASM vs FAST ASM\n\n");
    printf("| File (Size) | Standard C | Minimal C (`lz0dec_min`) | Ultra-Tiny ASM (`lz0dec_ultra_tiny_riscv64`) | Tiny ASM (`lz0dec_tiny_riscv64`) | Compact ASM (`lz0dec_riscv64`) | FAST ASM (`lz0dec_fast_riscv64`) | Speedup FAST vs C |\n");
#else
    printf("# x86-64 Decompression Benchmark: Standard C vs Minimal C vs Ultra-Tiny ASM vs Tiny ASM vs Compact ASM vs FAST ASM\n\n");
    printf("| File (Size) | Standard C | Minimal C (`lz0dec_min`) | Ultra-Tiny ASM (`lz0dec_ultra_tiny_x86_64`) | Tiny ASM (`lz0dec_tiny_x86_64`) | Compact ASM (`lz0dec_x86_64`) | FAST ASM (`lz0dec_fast_x86_64`) | Speedup FAST vs C |\n");
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64) || (defined(__riscv) && __riscv_xlen == 64)
    printf("|---|---:|---:|---:|---:|---:|---:|---:|\n");
#else
    printf("|---|---:|---:|---:|---:|---:|\n");
#endif

    if (argc > 1) {
        for (int i = 1; i < argc; i++) bench_one(argv[i]);
        return 0;
    }

    const char *files[] = {
        "bench/corpus/fields.c",
        "bench/corpus/cp.html",
        "bench/corpus/alice29.txt",
        "bench/corpus/plrabn12.txt",
        "bench/corpus/kennedy.xls",
        "bench/corpus/dickens_1m.txt",
        "bench/corpus/bash_x86_64.bin",
        NULL
    };

    for (int i = 0; files[i]; i++) {
        if (access(files[i], R_OK) == 0) {
            bench_one(files[i]);
        }
    }
    return 0;
}

