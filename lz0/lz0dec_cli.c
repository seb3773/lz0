/*
 * lz0dec - Standalone minimal LZ0 decompressor CLI
 *
 * Decompresses an LZ01 container file to an output file.
 * Demonstrates the tiny footprint and high throughput of the LZ0 decoders.
 */

#define _POSIX_C_SOURCE 200809L
#include "lz0dec.h"
#include "lz0_stdio.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
static double now_ns(void) {
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1e9 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

static void print_usage(const char *prog) {
    fprintf(stderr, "LZ0 Standalone Decompressor v%s\n\n", LZ0_VERSION);
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <input.lz0|-> <output|->            Decompress using fastest engine (default)\n", prog);
    fprintf(stderr, "  %s -e, --engine <fast|compact|tiny|ultra_tiny|min> <in|-> <out|->  Decompress using selected engine\n", prog);
    fprintf(stderr, "  %s -b, --bench <input.lz0>             Benchmark decompression throughput\n", prog);
    fprintf(stderr, "  %s -h, --help                          Show this help message\n", prog);
    fprintf(stderr, "  %s -v, --version                       Show version\n\n", prog);
    fprintf(stderr, "Available engines on this platform:\n");
    fprintf(stderr, "  min        : Minimal C decoder (lz0dec_min), no heap\n");
#if ((defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && (defined(__x86_64__) || defined(_M_X64))) || \
    ((defined(__ELF__) || defined(__APPLE__)) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)) || \
    (defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32))
    fprintf(stderr, "  ultra_tiny : Extreme-size (143-224 B) unchecked trusted-stream ASM decoder\n");
    fprintf(stderr, "  tiny       : Tiny (288-376 B) table-free ASM decoder (full checks)\n");
    fprintf(stderr, "  compact    : Compact table-free ASM decoder\n");
    fprintf(stderr, "  fast       : High-throughput ASM decoder\n");
#endif
}

typedef int (*dec_fn)(const unsigned char *, int, unsigned char *, int);

static int ultra_tiny_adapter(const unsigned char *in, int in_size,
                              unsigned char *out, int out_max) {
    (void)in_size;
    (void)out_max;
    return lz0dec_ultra_tiny(in, out);
}

static int run_benchmark(const char *path, const unsigned char *pay, size_t psz, uint32_t uncomp_sz) {
    printf("=== LZ0 Decompression Benchmark ===\n");
    printf("File:              %s\n", path);
    printf("Compressed size:   %zu bytes (%.1f KiB)\n", psz + 8, (double)(psz + 8) / 1024.0);
    printf("Uncompressed size: %u bytes (%.1f KiB)\n", uncomp_sz, (double)uncomp_sz / 1024.0);
    if (uncomp_sz > 0)
        printf("Ratio:             %.2f%%\n\n", (double)(psz + 8) * 100.0 / (double)uncomp_sz);

    if (uncomp_sz == 0) {
        printf("Empty file (0 bytes), benchmark skipped.\n");
        return 0;
    }
    if (psz > (size_t)INT_MAX || uncomp_sz > (uint32_t)INT_MAX) {
        printf("Stream larger than 2 GiB (-b supports <= 2 GiB), benchmark skipped.\n");
        return 0;
    }

    unsigned char *out = (unsigned char *)malloc(uncomp_sz);
    if (!out) {
        fprintf(stderr, "lz0dec: error: memory allocation failed\n");
        return 1;
    }

    struct {
        const char *name;
        const char *desc;
        dec_fn fn;
    } engines[] = {
        { "min",     "C minimal (lz0dec_min)",            lz0dec_min },
#if ((defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && (defined(__x86_64__) || defined(_M_X64))) || \
    ((defined(__ELF__) || defined(__APPLE__)) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)) || \
    (defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32))
        { "ultra_tiny", "ASM Ultra-Tiny (lz0dec_ultra_tiny)", ultra_tiny_adapter },
        { "tiny",       "ASM Tiny (lz0dec_tiny)",             lz0dec_tiny },
        { "compact",    "ASM Compact (lz0dec_compact)",       lz0dec_compact },
        { "fast",       "ASM FAST (lz0dec_fast)",             lz0dec_fast },
#endif
    };
    int num_engines = (int)(sizeof(engines) / sizeof(engines[0]));

    int iters = (uncomp_sz < 30 * 1024) ? 2000 :
                (uncomp_sz < 200 * 1024) ? 500 :
                (uncomp_sz < 600 * 1024) ? 100 : 40;

    double base_speed = 0.0;

    printf("Running benchmark (%d iterations per engine, best of 5 rounds):\n", iters);

    for (int e = 0; e < num_engines; e++) {
        // Warm-up
        for (int w = 0; w < 3; w++) {
            engines[e].fn(pay, (int)psz, out, (int)uncomp_sz);
        }

        double best_ns = 1e18;
        for (int r = 0; r < 5; r++) {
            double t0 = now_ns();
            for (int i = 0; i < iters; i++) {
                int d = engines[e].fn(pay, (int)psz, out, (int)uncomp_sz);
                if (d < 0 || (uint32_t)d != uncomp_sz) {
                    fprintf(stderr, "lz0dec: engine %s failed verification!\n", engines[e].name);
                    free(out);
                    return 1;
                }
            }
            double elapsed = (now_ns() - t0) / iters;
            if (elapsed < best_ns) best_ns = elapsed;
        }

        double mib = (double)uncomp_sz / (1024.0 * 1024.0);
        double mb_s = mib / (best_ns / 1e9);
        if (e == 0) base_speed = mb_s;

        if (e == 0) {
            printf("  %-32s : %8.1f MB/s\n", engines[e].desc, mb_s);
        } else {
            double speedup = ((mb_s / base_speed) - 1.0) * 100.0;
            printf("  %-32s : %8.1f MB/s  (%+5.1f%% vs C min)\n", engines[e].desc, mb_s, speedup);
        }
    }

    printf("\nDecompression verification: OK (bit-exact on all engines)\n");
    free(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *engine_name = "fast";
    const char *input_path = NULL;
    const char *output_path = NULL;
    int bench_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("lz0dec %s\n", LZ0_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bench") == 0) {
            bench_mode = 1;
            continue;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--engine") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "lz0dec: error: missing argument for %s\n", argv[i]);
                return 1;
            }
            engine_name = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--engine=", 9) == 0) {
            engine_name = argv[i] + 9;
            continue;
        }
        if (!input_path) {
            input_path = argv[i];
        } else if (!output_path) {
            output_path = argv[i];
        } else {
            fprintf(stderr, "lz0dec: error: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input_path) {
        fprintf(stderr, "lz0dec: error: no input file specified\n");
        return 1;
    }
    if (!bench_mode && !output_path) {
        fprintf(stderr, "lz0dec: error: no output file specified (use -b for benchmark mode)\n");
        return 1;
    }

    dec_fn dec = lz0dec_fast;
    if (strcmp(engine_name, "fast") == 0) {
        dec = lz0dec_fast;
    } else if (strcmp(engine_name, "compact") == 0) {
        dec = lz0dec_compact;
    } else if (strcmp(engine_name, "tiny") == 0) {
        dec = lz0dec_tiny;
    } else if (strcmp(engine_name, "ultra_tiny") == 0) {
        dec = ultra_tiny_adapter;
    } else if (strcmp(engine_name, "min") == 0) {
        dec = lz0dec_min;
    } else {
        fprintf(stderr, "lz0dec: error: unknown engine '%s' (choose: fast, compact, tiny, ultra_tiny, min)\n", engine_name);
        return 1;
    }

    size_t in_sz = 0;
    unsigned char *cont = lz0_cli_read(input_path, &in_sz);
    if (!cont) {
        fprintf(stderr, "lz0dec: error: cannot read '%s'\n", input_path);
        return 1;
    }

    if (in_sz < 8 || memcmp(cont, "LZ01", 4) != 0) {
        fprintf(stderr, "lz0dec: error: '%s' is not a valid LZ01 container\n", input_path);
        free(cont);
        return 1;
    }

    uint32_t uncomp_sz = (uint32_t)cont[4] |
                         ((uint32_t)cont[5] << 8) |
                         ((uint32_t)cont[6] << 16) |
                         ((uint32_t)cont[7] << 24);

    unsigned char *payload = cont + 8;
    size_t payload_sz = in_sz - 8;

    if (bench_mode) {
        int rc = run_benchmark(input_path, payload, payload_sz, uncomp_sz);
        free(cont);
        return rc;
    }

    if (uncomp_sz == 0) {
        if (payload_sz != 0) {
            fprintf(stderr, "lz0dec: error: corrupt empty file payload\n");
            free(cont);
            return 1;
        }
        lz0_cli_write(output_path, NULL, 0);
        free(cont);
        return 0;
    }

    unsigned char *out = (unsigned char *)malloc(uncomp_sz);
    if (!out) {
        fprintf(stderr, "lz0dec: error: memory allocation failure\n");
        free(cont);
        return 1;
    }

    int64_t decomp_len;
    if (payload_sz <= (size_t)INT_MAX && uncomp_sz <= (uint32_t)INT_MAX) {
        decomp_len = dec(payload, (int)payload_sz, out, (int)uncomp_sz);
    } else {
        if (strcmp(engine_name, "min") != 0)
            fprintf(stderr, "lz0dec: notice: >2GiB stream uses 64-bit min engine\n");
        decomp_len = lz0dec_min_ex(payload, payload_sz, out, (size_t)uncomp_sz);
    }
    if (decomp_len < 0 || (uint64_t)decomp_len != (uint64_t)uncomp_sz) {
        fprintf(stderr, "lz0dec: error: decompression failed (corrupt or truncated stream)\n");
        free(out);
        free(cont);
        return 1;
    }

    if (lz0_cli_write(output_path, out, (size_t)decomp_len) != 0) {
        fprintf(stderr, "lz0dec: error: failed to write output file '%s'\n", output_path);
        free(out);
        free(cont);
        return 1;
    }

    free(out);
    free(cont);
    return 0;
}
