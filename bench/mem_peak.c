/*
 * mem_peak.c - Encoder peak heap allocation bench.
 *
 * Usage: lz0_mem_peak <levels-csv> file...
 * For every (file, level): compressed size, FNV-1a 64-bit hash of the stream
 * (same hash as lz0_enc_bench) and the encoder's peak live heap, i.e. the
 * high-water mark of the bytes held by malloc/calloc/realloc between entry
 * into and return from lz0_compress() (input buffer excluded, output stream
 * included). The allocator is interposed in this binary (glibc __libc_*
 * entry points), so the figure is exact and independent of page touching;
 * peak RSS growth (VmHWM) is printed alongside for reference.
 */
#define _GNU_SOURCE
#include "lz0.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__GLIBC__)
#include <malloc.h>
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static size_t mp_live, mp_peak;

static void mp_add(void *p) {
    if (p) {
        mp_live += malloc_usable_size(p);
        if (mp_live > mp_peak)
            mp_peak = mp_live;
    }
}
void *malloc(size_t n) {
    void *p = __libc_malloc(n);
    mp_add(p);
    return p;
}
void *calloc(size_t a, size_t b) {
    void *p = __libc_calloc(a, b);
    mp_add(p);
    return p;
}
void free(void *p) {
    if (p)
        mp_live -= malloc_usable_size(p);
    __libc_free(p);
}
void *realloc(void *old, size_t n) {
    size_t was = old ? malloc_usable_size(old) : 0;
    void *p = __libc_realloc(old, n);
    if (p || n == 0)
        mp_live -= was;
    mp_add(p);
    return p;
}
#define MP_EXACT 1
#else
static size_t mp_live, mp_peak;
#define MP_EXACT 0
#endif

static long status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, kl)) {
            v = atol(line + kl);
            break;
        }
    }
    fclose(f);
    return v;
}

static uint64_t fnv(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <levels-csv> file...\n", argv[0]);
        return 2;
    }
    int levels[16], nl = 0;
    for (char *s = strtok(argv[1], ","); s && nl < 16; s = strtok(NULL, ","))
        levels[nl++] = atoi(s);
    if (!MP_EXACT)
        printf("(no glibc: exact heap tracking unavailable, peak heap column is 0)\n");
    printf("%-16s %3s %9s %9s %-16s %11s %8s %10s\n", "file", "lvl", "in", "out", "fnv1a",
           "peak_heap_KB", "B/byte", "dHWM_KB");
    for (int a = 2; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        if (!fp) {
            perror(argv[a]);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        unsigned char *in = malloc(n > 0 ? (size_t)n : 1);
        if (!in || fread(in, 1, (size_t)n, fp) != (size_t)n)
            return 1;
        fclose(fp);
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        for (int li = 0; li < nl; li++) {
            lz0_config cfg;
            lz0_config_level(&cfg, levels[li]);
            unsigned char *out = NULL;
            size_t os = 0;
            long hwm0 = status_kb("VmHWM:");
            size_t live0 = mp_live;
            mp_peak = mp_live;
            if (lz0_compress(in, (size_t)n, &out, &os, &cfg, NULL) < 0) {
                printf("FAIL %s L%d\n", base, levels[li]);
                return 1;
            }
            size_t peak = mp_peak - live0;
            long hwm1 = status_kb("VmHWM:");
            printf("%-16s L%-2d %9ld %9zu %016llx %11.1f %8.2f %10ld\n", base, levels[li], n, os,
                   (unsigned long long)fnv(out, os), (double)peak / 1024.0,
                   n > 0 ? (double)peak / (double)n : 0.0, hwm1 - hwm0);
            free(out);
        }
        free(in);
    }
    return 0;
}
