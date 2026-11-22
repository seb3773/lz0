#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <lz4.h>
#include <lz4hc.h>
#include "lz0.h"
#include "lz0dec.h"
#include "zx0_compress_wrapper.h"
#include "zx0_decompress.h"

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

static int run_upkr_cli(const unsigned char *in, size_t in_sz,
                        unsigned char **out, size_t *out_sz,
                        double *enc_ms, double *dec_speed_mbs, int level) {
    char in_path[] = "/tmp/upkr_in_XXXXXX";
    char out_path[] = "/tmp/upkr_out_XXXXXX";
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

    /* In-memory unpack speed benchmark */
    unsigned char *upkr_dest = (unsigned char *)malloc(in_sz + 4096);
    if (!upkr_dest) return -1;

    int iters = (in_sz < 256 * 1024) ? 50 : (in_sz < 1024 * 1024) ? 20 : 5;
    t0 = now_ms();
    for (int i = 0; i < iters; i++) {
        upkr_unpack(upkr_dest, *out);
    }
    double dec_total_ms = (now_ms() - t0) / iters;
    int ok = (memcmp(in, upkr_dest, in_sz) == 0);
    free(upkr_dest);

    if (ok && dec_total_ms > 0) {
        *dec_speed_mbs = ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0);
        return 0;
    }
    return -1;
}

typedef struct {
    const char *name;
    const char *category;
    size_t raw_sz;

    /* LZ0 -6 */
    size_t lz0_6_sz;
    double lz0_6_enc_ms;
    double lz0_6_dec_c_mbs;
    double lz0_6_dec_asm_mbs;
    int lz0_6_ok;

    /* LZ0 -9 Ultra */
    size_t lz0_9_sz;
    double lz0_9_enc_ms;
    double lz0_9_dec_asm_mbs;
    int lz0_9_ok;

    /* UPKR */
    size_t upkr_sz;
    double upkr_enc_ms;
    double upkr_dec_mbs;
    int upkr_level;
    int upkr_ok;

    /* LZ4 */
    size_t lz4_sz;
    double lz4_enc_ms;
    double lz4_dec_mbs;
    int lz4_ok;

    /* LZ4HC */
    size_t lz4hc_sz;
    double lz4hc_enc_ms;
    double lz4hc_dec_mbs;
    int lz4hc_ok;

    /* ZX0 */
    size_t zx0_sz;
    double zx0_enc_ms;
    double zx0_dec_mbs;
    int zx0_ok;
} test_result_t;

static void run_test(const char *path, const char *category, test_result_t *r, int skip_upkr, int skip_l9) {
    memset(r, 0, sizeof(*r));
    r->category = category;

    const char *bn = strrchr(path, '/');
    r->name = bn ? bn + 1 : path;

    size_t in_sz = 0;
    unsigned char *in = read_file(path, &in_sz);
    if (!in || in_sz == 0) {
        fprintf(stderr, "Cannot read %s\n", path);
        return;
    }
    r->raw_sz = in_sz;

    printf("=== Testing %-20s (%8zu B / %6.1f KiB) [%s] ===\n",
           r->name, in_sz, (double)in_sz / 1024.0, category);
    fflush(stdout);

    int iters_asm = (in_sz < 256 * 1024) ? 100 : (in_sz < 1024 * 1024) ? 40 : 15;
    int iters_c   = (in_sz < 256 * 1024) ? 50  : (in_sz < 1024 * 1024) ? 20 : 5;

    /* 1. LZ0 Normal (-6) */
    {
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 6);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        r->lz0_6_enc_ms = now_ms() - t0;
        r->lz0_6_sz = csz;

        unsigned char *out = (unsigned char *)malloc(in_sz + 64);

        /* C decompress */
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters_c; i++) {
            d = lz0_decompress_to(comp, (int)csz, out, (int)in_sz);
        }
        double dec_c_ms = (now_ms() - t0) / iters_c;
        r->lz0_6_ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        r->lz0_6_dec_c_mbs = (dec_c_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_c_ms / 1000.0) : 0;

        /* FAST ASM decompress */
#if defined(__x86_64__) || defined(_M_X64)
        t0 = now_ms();
        int dasm = 0;
        for (int i = 0; i < iters_asm; i++) {
            dasm = lz0dec_fast_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double dec_asm_ms = (now_ms() - t0) / iters_asm;
        if (dasm == (int)in_sz && memcmp(in, out, in_sz) == 0) {
            r->lz0_6_dec_asm_mbs = (dec_asm_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_asm_ms / 1000.0) : 0;
        }
#endif
        free(comp);
        free(out);

        printf("  LZ0 (-6)       : %8zu B (%5.2f%%) | Enc: %6.1f ms | Dec Fast ASM: %6.1f MB/s | C: %5.1f MB/s\n",
               r->lz0_6_sz, (double)r->lz0_6_sz * 100.0 / in_sz,
               r->lz0_6_enc_ms, r->lz0_6_dec_asm_mbs, r->lz0_6_dec_c_mbs);
        fflush(stdout);
    }

    /* 2. LZ0 Ultra (-9) */
    if (!skip_l9) {
        lz0_config cfg;
        lz0_stats stats;
        lz0_config_level(&cfg, 9);

        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = lz0_compress(in, in_sz, &comp, &csz, &cfg, &stats);
        r->lz0_9_enc_ms = now_ms() - t0;
        r->lz0_9_sz = csz;

        unsigned char *out = (unsigned char *)malloc(in_sz + 64);
#if defined(__x86_64__) || defined(_M_X64)
        t0 = now_ms();
        int dasm = 0;
        for (int i = 0; i < iters_asm; i++) {
            dasm = lz0dec_fast_x86_64(comp, (int)csz, out, (int)in_sz);
        }
        double dec_asm_ms = (now_ms() - t0) / iters_asm;
        r->lz0_9_ok = (rc == 0 && dasm == (int)in_sz && memcmp(in, out, in_sz) == 0);
        r->lz0_9_dec_asm_mbs = (dec_asm_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_asm_ms / 1000.0) : 0;
#else
        r->lz0_9_ok = (rc == 0);
#endif
        free(comp);
        free(out);

        printf("  LZ0 (-9 Ultra) : %8zu B (%5.2f%%) | Enc: %6.1f ms | Dec Fast ASM: %6.1f MB/s\n",
               r->lz0_9_sz, (double)r->lz0_9_sz * 100.0 / in_sz,
               r->lz0_9_enc_ms, r->lz0_9_dec_asm_mbs);
        fflush(stdout);
    } else {
        printf("  LZ0 (-9 Ultra) : skipped (> 2M)\n");
    }

    /* 3. UPKR */
    if (!skip_upkr && access("bench/external/bin/upkr", X_OK) == 0) {
        int level = (in_sz <= 20 * 1024) ? 9 : (in_sz <= 256 * 1024) ? 6 : 2;
        r->upkr_level = level;
        unsigned char *comp = NULL;
        size_t csz = 0;
        double enc_ms = 0, dec_mbs = 0;
        int rc = run_upkr_cli(in, in_sz, &comp, &csz, &enc_ms, &dec_mbs, level);
        if (rc == 0) {
            r->upkr_ok = 1;
            r->upkr_sz = csz;
            r->upkr_enc_ms = enc_ms;
            r->upkr_dec_mbs = dec_mbs;
            printf("  UPKR (-%d)      : %8zu B (%5.2f%%) | Enc: %6.1f ms | Dec C unpack: %6.1f MB/s\n",
                   level, r->upkr_sz, (double)r->upkr_sz * 100.0 / in_sz,
                   r->upkr_enc_ms, r->upkr_dec_mbs);
        } else {
            printf("  UPKR (-%d)      : failed\n", level);
        }
        free(comp);
        fflush(stdout);
    } else if (skip_upkr) {
        printf("  UPKR           : skipped (> 2M)\n");
    }

    /* 4. LZ4 */
    {
        int max_dst = LZ4_compressBound((int)in_sz);
        char *comp = (char *)malloc(max_dst);
        char *out = (char *)malloc(in_sz);

        double t0 = now_ms();
        int csz = LZ4_compress_default((const char *)in, comp, (int)in_sz, max_dst);
        r->lz4_enc_ms = now_ms() - t0;
        r->lz4_sz = (size_t)csz;

        int iters_lz4 = (in_sz < 256 * 1024) ? 200 : (in_sz < 1024 * 1024) ? 100 : 30;
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters_lz4; i++) {
            d = LZ4_decompress_safe(comp, out, csz, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters_lz4;
        r->lz4_ok = (csz > 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        r->lz4_dec_mbs = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("  LZ4 (default)  : %8zu B (%5.2f%%) | Enc: %6.1f ms | Dec in-memory: %6.1f MB/s\n",
               r->lz4_sz, (double)r->lz4_sz * 100.0 / in_sz,
               r->lz4_enc_ms, r->lz4_dec_mbs);
        free(comp);
        free(out);
        fflush(stdout);
    }

    /* 4b. LZ4HC (-9) */
    {
        int max_dst = LZ4_compressBound((int)in_sz);
        char *comp = (char *)malloc(max_dst);
        char *out = (char *)malloc(in_sz);

        double t0 = now_ms();
        int csz = LZ4_compress_HC((const char *)in, comp, (int)in_sz, max_dst, 9);
        r->lz4hc_enc_ms = now_ms() - t0;
        r->lz4hc_sz = (size_t)csz;

        int iters_lz4 = (in_sz < 256 * 1024) ? 200 : (in_sz < 1024 * 1024) ? 100 : 30;
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters_lz4; i++) {
            d = LZ4_decompress_safe(comp, out, csz, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters_lz4;
        r->lz4hc_ok = (csz > 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        r->lz4hc_dec_mbs = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("  LZ4HC (-9)     : %8zu B (%5.2f%%) | Enc: %6.1f ms | Dec in-memory: %6.1f MB/s\n",
               r->lz4hc_sz, (double)r->lz4hc_sz * 100.0 / in_sz,
               r->lz4hc_enc_ms, r->lz4hc_dec_mbs);
        free(comp);
        free(out);
        fflush(stdout);
    }

    /* 5. ZX0 (only if <= 64 KB and non-media) */
    if (in_sz <= 64 * 1024 && strstr(category, "Image") == NULL && strstr(category, "Audio") == NULL) {
        unsigned char *comp = NULL;
        size_t csz = 0;
        double t0 = now_ms();
        int rc = zx0_compress_c(in, in_sz, &comp, &csz);
        r->zx0_enc_ms = now_ms() - t0;
        r->zx0_sz = csz;

        unsigned char *out = (unsigned char *)malloc(in_sz + 16);
        t0 = now_ms();
        int d = 0;
        for (int i = 0; i < iters_c; i++) {
            d = zx0_decompress_to(comp, (int)csz, out, (int)in_sz);
        }
        double dec_total_ms = (now_ms() - t0) / iters_c;
        r->zx0_ok = (rc == 0 && d == (int)in_sz && memcmp(in, out, in_sz) == 0);
        r->zx0_dec_mbs = (dec_total_ms > 0) ? ((double)in_sz / (1024.0 * 1024.0)) / (dec_total_ms / 1000.0) : 0;

        printf("  ZX0            : %8zu B (%5.2f%%) | Enc: %6.1f ms | Dec C: %6.1f MB/s\n",
               r->zx0_sz, (double)r->zx0_sz * 100.0 / in_sz,
               r->zx0_enc_ms, r->zx0_dec_mbs);
        free(comp);
        free(out);
        fflush(stdout);
    }

    free(in);
    printf("\n");
}

static void print_markdown_table(const char *title, test_result_t *results, int count) {
    printf("### %s\n\n", title);
    printf("| File / Target | Type / Category | Raw Size | LZ0 Normal (-6) | LZ0 Ultra (-9) | UPKR | LZ4 (default) | LZ4HC (-9) | Decomp. Throughput LZ0 FAST ASM vs LZ4 |\n");
    printf("|---|---|---:|---:|---:|---:|---:|---:|---:|\n");

    for (int i = 0; i < count; i++) {
        test_result_t *r = &results[i];
        if (r->raw_sz == 0) continue;

        double r_lz0_6 = (double)r->lz0_6_sz * 100.0 / (double)r->raw_sz;
        char s_lz0_6[64];
        snprintf(s_lz0_6, sizeof(s_lz0_6), "%zu B (**%.1f%%**)", r->lz0_6_sz, r_lz0_6);

        char s_lz0_9[64];
        if (r->lz0_9_sz > 0) {
            double r_lz0_9 = (double)r->lz0_9_sz * 100.0 / (double)r->raw_sz;
            snprintf(s_lz0_9, sizeof(s_lz0_9), "**%zu B** (**%.1f%%**)", r->lz0_9_sz, r_lz0_9);
        } else {
            snprintf(s_lz0_9, sizeof(s_lz0_9), "-");
        }

        char s_upkr[64];
        if (r->upkr_ok && r->upkr_sz > 0) {
            double r_upkr = (double)r->upkr_sz * 100.0 / (double)r->raw_sz;
            snprintf(s_upkr, sizeof(s_upkr), "%zu B (%.1f%%) [-%d]", r->upkr_sz, r_upkr, r->upkr_level);
        } else {
            snprintf(s_upkr, sizeof(s_upkr), "-");
        }

        char s_lz4[64];
        double r_lz4 = (double)r->lz4_sz * 100.0 / (double)r->raw_sz;
        snprintf(s_lz4, sizeof(s_lz4), "%zu B (%.1f%%)", r->lz4_sz, r_lz4);

        char s_lz4hc[64];
        if (r->lz4hc_ok && r->lz4hc_sz > 0) {
            double r_lz4hc = (double)r->lz4hc_sz * 100.0 / (double)r->raw_sz;
            snprintf(s_lz4hc, sizeof(s_lz4hc), "%zu B (%.1f%%)", r->lz4hc_sz, r_lz4hc);
        } else {
            snprintf(s_lz4hc, sizeof(s_lz4hc), "-");
        }

        char s_speed[64];
        snprintf(s_speed, sizeof(s_speed), "**%.0f MB/s** vs %.0f MB/s",
                 r->lz0_6_dec_asm_mbs, r->lz4_dec_mbs);

        printf("| `%s` | %s | %zu B | %s | %s | %s | %s | %s | %s |\n",
               r->name, r->category, r->raw_sz,
               s_lz0_6, s_lz0_9, s_upkr, s_lz4, s_lz4hc, s_speed);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    typedef struct {
        const char *path;
        const char *category;
        int skip_upkr;
        int skip_l9;
    } target_def;

    static const target_def INDIVIDUAL_FILES[] = {
        { "bench/corpus/fields.c",         "C Source",       0, 0 },
        { "bench/corpus/sum",              "SPARC ELF",    0, 0 },
        { "bench/corpus/lz0_win64.exe",     "Windows PE64", 0, 0 },
        { "bench/corpus/star.png",         "PNG Image",    1, 0 },
        { "bench/corpus/avatar.jpg",       "JPEG Image",   0, 0 },
        { "bench/corpus/break.mp3",        "MP3 CBR Audio",0, 0 },
        { "bench/corpus/texture_rgb24.raw","RGB24 Texture",0, 0 },
        { "bench/corpus/texture_rgba32.raw","RGBA32 Texture",1, 0 },
        { "bench/corpus/audio_pcm16.raw",  "Stereo PCM16 Audio",0, 0 },
        { "bench/corpus/alice29.txt",      "English Text", 0, 0 },
        { "bench/corpus/plrabn12.txt",     "Poetry Text",  1, 0 },
        { "bench/corpus/kennedy.xls",      "Excel Spreadsheet", 1, 0 },
        { "bench/corpus/dickens_1m.txt",   "Large Text",   1, 0 },
        { "bench/corpus/bash_x86_64.bin",   "ELF x86_64",   1, 0 },
        { NULL, NULL, 0, 0 }
    };

    static const target_def BLOBS[] = {
        { "bench/blobs/blob_png_256k.bin", "Blob 4 PNGs",       0, 0 },
        { "bench/blobs/blob_mix_256k.bin", "Mixed Blob (5 files)",  0, 0 },
        { "bench/blobs/blob_png_512k.bin", "Blob 8 PNGs",       0, 0 },
        { "bench/blobs/blob_mix_512k.bin", "Mixed Blob (7 files)",  0, 0 },
        { "bench/blobs/blob_png_2m.bin",   "Blob 8 PNGs 2M",    1, 0 },
        { "bench/blobs/blob_mix_2m.bin",   "Mixed Blob (9 files) 2M", 1, 0 },
        { "bench/blobs/blob_png_5m.bin",   "Blob 15 PNGs 5M",   1, 1 },
        { "bench/blobs/blob_mix_5m.bin",   "Mixed Blob (15 files) 5M", 1, 1 },
        { NULL, NULL, 0, 0 }
    };

    test_result_t file_results[32];
    int file_count = 0;

    test_result_t blob_results[32];
    int blob_count = 0;

    printf("# ====================================================================\n");
    printf("# LZ0 Comprehensive Benchmark Suite: LZ0 vs UPKR vs LZ4 / LZ4HC vs ZX0\n");
    printf("# Benchmarked on: Intel(R) Core(TM) i3-9100 CPU @ 3.60GHz (x86_64 Linux)\n");
    printf("# ====================================================================\n\n");

    /* 1. Individual files */
    printf("## Phase 1: Individual Files (ELF, PE, PNG, XLS, Texts, C Code)\n\n");
    for (int i = 0; INDIVIDUAL_FILES[i].path; i++) {
        if (access(INDIVIDUAL_FILES[i].path, R_OK) == 0) {
            run_test(INDIVIDUAL_FILES[i].path, INDIVIDUAL_FILES[i].category,
                     &file_results[file_count++],
                     INDIVIDUAL_FILES[i].skip_upkr, INDIVIDUAL_FILES[i].skip_l9);
        }
    }

    /* 2. Blobs */
    printf("## Phase 2: Heterogeneous Multi-Resource Blobs (256 KiB to 5 MiB)\n\n");
    for (int i = 0; BLOBS[i].path; i++) {
        if (access(BLOBS[i].path, R_OK) == 0) {
            run_test(BLOBS[i].path, BLOBS[i].category,
                     &blob_results[blob_count++],
                     BLOBS[i].skip_upkr, BLOBS[i].skip_l9);
        }
    }

    printf("# ====================================================================\n");
    printf("# COMPARATIVE SUMMARY TABLES\n");
    printf("# ====================================================================\n\n");

    print_markdown_table("1. Real-World Corpus Comparison: Individual Files",
                         file_results, file_count);

    print_markdown_table("2. Concatenated Multi-Resource Blobs Comparison",
                         blob_results, blob_count);

    return 0;
}
