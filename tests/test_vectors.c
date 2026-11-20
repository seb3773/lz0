#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *p = (unsigned char *)malloc((size_t)sz + 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (sz && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return p;
}

static int check_raw(const char *name, const unsigned char *in, size_t n,
                     const unsigned char *raw, size_t rsz) {
    unsigned char *a = (unsigned char *)malloc(n ? n : 1);
    unsigned char *b = (unsigned char *)malloc(n ? n : 1);
    unsigned char *c = (unsigned char *)malloc(n ? n : 1);
    int bad = 0;
    if (!a || !b || !c)
        bad = 1;
    else {
        int da = lz0_decompress_to(raw, (int)rsz, a, (int)n);
        int db = lz0dec_ref(raw, (int)rsz, b, (int)n);
        int dc = lz0dec_min(raw, (int)rsz, c, (int)n);
        if (da != (int)n || db != (int)n || dc != (int)n)
            bad = 1;
        else if (memcmp(a, in, n) || memcmp(b, in, n) || memcmp(c, in, n))
            bad = 1;
#if defined(__x86_64__) || defined(_M_X64)
        unsigned char *d = (unsigned char *)malloc(n ? n : 1);
        if (!d)
            bad = 1;
        else {
            int dd = lz0dec_x86_64(raw, (int)rsz, d, (int)n);
            if (dd != (int)n || memcmp(d, in, n))
                bad = 1;
            int df = lz0dec_fast_x86_64(raw, (int)rsz, d, (int)n);
            if (df != (int)n || memcmp(d, in, n))
                bad = 1;
            free(d);
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        unsigned char *d = (unsigned char *)malloc(n ? n : 1);
        if (!d)
            bad = 1;
        else {
            int dd = lz0dec_arm64(raw, (int)rsz, d, (int)n);
            if (dd != (int)n || memcmp(d, in, n))
                bad = 1;
            int df = lz0dec_fast_arm64(raw, (int)rsz, d, (int)n);
            if (df != (int)n || memcmp(d, in, n))
                bad = 1;
            free(d);
        }
#elif (defined(__riscv) && __riscv_xlen == 64)
        unsigned char *d = (unsigned char *)malloc(n ? n : 1);
        if (!d)
            bad = 1;
        else {
            int dd = lz0dec_riscv64(raw, (int)rsz, d, (int)n);
            if (dd != (int)n || memcmp(d, in, n))
                bad = 1;
            int df = lz0dec_fast_riscv64(raw, (int)rsz, d, (int)n);
            if (df != (int)n || memcmp(d, in, n))
                bad = 1;
            free(d);
        }
#endif
    }
    printf("%-16s n=%zu raw=%zu %s\n", name, n, rsz, bad ? "FAIL" : "ok");
    free(a);
    free(b);
    free(c);
    return bad;
}

static int run_one(const char *name) {
    char path[256];
    size_t n = 0, rsz = 0, csz = 0;
    snprintf(path, sizeof(path), "tests/vectors/%s.bin", name);
    unsigned char *in = read_all(path, &n);
    snprintf(path, sizeof(path), "tests/vectors/%s.lz0raw", name);
    unsigned char *raw = read_all(path, &rsz);
    snprintf(path, sizeof(path), "tests/vectors/%s.lz0", name);
    unsigned char *cont = read_all(path, &csz);
    int bad = 0;
    if (!strcmp(name, "empty")) {
        if (!in || !raw || !cont || n != 0 || rsz != 0 || csz != 8)
            bad = 1;
        else if (memcmp(cont, "LZ01", 4) || cont[4] || cont[5] || cont[6] || cont[7])
            bad = 1;
        printf("%-16s n=0 raw=0 cont=8 %s\n", name, bad ? "FAIL" : "ok");
        free(in);
        free(raw);
        free(cont);
        return bad;
    }
    if (!in || !raw || !cont || n == 0)
        bad = 1;
    else {
        bad |= check_raw(name, in, n, raw, rsz);
        if (csz != 8 + rsz || memcmp(cont, "LZ01", 4))
            bad = 1;
        else {
            uint32_t sz = (uint32_t)cont[4] | ((uint32_t)cont[5] << 8) |
                          ((uint32_t)cont[6] << 16) | ((uint32_t)cont[7] << 24);
            if (sz != (uint32_t)n || memcmp(cont + 8, raw, rsz))
                bad = 1;
        }
    }
    if (bad && in && raw)
        printf("%-16s container FAIL\n", name);
    free(in);
    free(raw);
    free(cont);
    return bad;
}

int main(void) {
    static const char *names[] = {
        "empty", "onebyte", "literals", "overlap", "mixed",
        "matchlast", "matchnew", "eof_after_lit"
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        fail |= run_one(names[i]);
    return fail ? 1 : 0;
}
