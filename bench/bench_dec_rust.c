#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lz0.h"
#include "lz0dec.h"

extern int lz0dec_rust_safe(const unsigned char *in, int in_size,
                            unsigned char *out, int out_max);
extern int lz0dec_rust_fast(const unsigned char *in, int in_size,
                            unsigned char *out, int out_max);

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static unsigned char *read_file(const char *path, size_t *sz_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *p = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *sz_out = (size_t)sz;
    return p;
}

typedef int (*decfn)(const unsigned char *, int, unsigned char *, int);

static double measure(decfn fn, const unsigned char *comp, size_t csz,
                      unsigned char *out, size_t in_sz, int iters) {
    for (int w = 0; w < 5; w++)
        fn(comp, (int)csz, out, (int)in_sz);
    double best = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ns();
        for (int i = 0; i < iters; i++)
            fn(comp, (int)csz, out, (int)in_sz);
        double el = (now_ns() - t0) / iters;
        if (el < best)
            best = el;
    }
    double mib = (double)in_sz / (1024.0 * 1024.0);
    return mib / (best / 1e9);
}

static void bench_one(const char *path) {
    size_t in_sz = 0;
    unsigned char *in = read_file(path, &in_sz);
    if (!in || in_sz == 0)
        return;

    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

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

    int iters = (in_sz < 32 * 1024) ? 2000 :
                (in_sz < 256 * 1024) ? 500 :
                (in_sz < 1024 * 1024) ? 150 : 50;

    unsigned char *out = (unsigned char *)malloc(in_sz + 32);

    double s_c = measure(lz0_decompress_to, comp, csz, out, in_sz, iters);
    double s_min = measure(lz0dec_min, comp, csz, out, in_sz, iters);
    double s_rs_safe = measure(lz0dec_rust_safe, comp, csz, out, in_sz, iters);
    double s_rs_fast = measure(lz0dec_rust_fast, comp, csz, out, in_sz, iters);
#if defined(__x86_64__) || defined(_M_X64)
    double s_asm = measure(lz0dec_x86_64, comp, csz, out, in_sz, iters);
    double s_fast = measure(lz0dec_fast_x86_64, comp, csz, out, in_sz, iters);
#elif defined(__aarch64__) || defined(_M_ARM64)
    double s_asm = measure(lz0dec_arm64, comp, csz, out, in_sz, iters);
    double s_fast = measure(lz0dec_fast_arm64, comp, csz, out, in_sz, iters);
#elif (defined(__riscv) && __riscv_xlen == 64)
    double s_asm = measure(lz0dec_riscv64, comp, csz, out, in_sz, iters);
    double s_fast = measure(lz0dec_fast_riscv64, comp, csz, out, in_sz, iters);
#else
    double s_asm = 0.0, s_fast = 0.0;
#endif

    printf("| `%s` (%.1f K) | %.1f MB/s | %.1f MB/s | %.1f MB/s | **%.1f MB/s** | %.1f MB/s | **%.1f MB/s** |\n",
           basename, (double)in_sz / 1024.0,
           s_c, s_min, s_rs_safe, s_rs_fast, s_asm, s_fast);
    fflush(stdout);

    free(out);
    free(comp);
    free(in);
}

int main(int argc, char **argv) {
    printf("# Decompression Benchmark: C vs Rust Safe (`no_std`) vs Rust Fast vs ASM\n\n");
    printf("| File (Size) | Standard C | Minimal C | Rust Safe (`no_std`) | Rust Fast (`unsafe`) | Compact ASM | FAST ASM |\n");
    printf("|---|---:|---:|---:|---:|---:|---:|\n");

    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            bench_one(argv[i]);
        return 0;
    }

    const char *files[] = {
        "bench/corpus/fields.c",
        "bench/corpus/cp.html",
        "bench/corpus/sum",
        "bench/corpus/lz0_win64.exe",
        "bench/corpus/star.png",
        "bench/corpus/avatar.jpg",
        "bench/corpus/break.mp3",
        "bench/corpus/alice29.txt",
        "bench/corpus/plrabn12.txt",
        "bench/corpus/kennedy.xls",
        "bench/corpus/dickens_1m.txt",
        "bench/corpus/bash_x86_64.bin",
        NULL
    };
    for (int i = 0; files[i]; i++) {
        if (access(files[i], R_OK) == 0)
            bench_one(files[i]);
    }
    return 0;
}

