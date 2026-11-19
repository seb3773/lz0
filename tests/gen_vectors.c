#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

static int write_file(const char *path, const unsigned char *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (n && fwrite(p, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int check_all(const unsigned char *in, size_t n, const unsigned char *raw, size_t rsz) {
    unsigned char *a = (unsigned char *)malloc(n ? n : 1);
    unsigned char *b = (unsigned char *)malloc(n ? n : 1);
    unsigned char *c = (unsigned char *)malloc(n ? n : 1);
    if (!a || !b || !c) {
        free(a);
        free(b);
        free(c);
        return -1;
    }
    int da = lz0_decompress_to(raw, (int)rsz, a, (int)n);
    int db = lz0dec_ref(raw, (int)rsz, b, (int)n);
    int dc = lz0dec_min(raw, (int)rsz, c, (int)n);
    int bad = 0;
    if (n == 0)
        bad = 1;
    else if (da != (int)n || db != (int)n || dc != (int)n)
        bad = 1;
    else if (memcmp(a, in, n) || memcmp(b, in, n) || memcmp(c, in, n))
        bad = 1;
    free(a);
    free(b);
    free(c);
    return bad;
}

static int emit_one(const char *dir, const char *name, const unsigned char *in, size_t n) {
    char path[512];
    unsigned char *raw = NULL;
    size_t rsz = 0;
    lz0_config cfg;
    lz0_stats st;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    lz0_h2c8_apply();
    if (n == 0) {
        snprintf(path, sizeof(path), "%s/%s.bin", dir, name);
        if (write_file(path, NULL, 0) != 0)
            return 1;
        snprintf(path, sizeof(path), "%s/%s.lz0raw", dir, name);
        if (write_file(path, NULL, 0) != 0)
            return 1;
        unsigned char cont[8] = {'L', 'Z', '0', '1', 0, 0, 0, 0};
        snprintf(path, sizeof(path), "%s/%s.lz0", dir, name);
        if (write_file(path, cont, 8) != 0)
            return 1;
        printf("%-16s n=0 raw=0 container=8 (empty)\n", name);
        return 0;
    }
    if (lz0_compress(in, n, &raw, &rsz, &cfg, &st) != 0) {
        printf("%s: compress fail\n", name);
        return 1;
    }
    if (check_all(in, n, raw, rsz) != 0) {
        printf("%s: decoder mismatch\n", name);
        free(raw);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/%s.bin", dir, name);
    if (write_file(path, in, n) != 0)
        return 1;
    snprintf(path, sizeof(path), "%s/%s.lz0raw", dir, name);
    if (write_file(path, raw, rsz) != 0)
        return 1;
    unsigned char *cont = (unsigned char *)malloc(8 + rsz);
    if (!cont) {
        free(raw);
        return 1;
    }
    cont[0] = 'L';
    cont[1] = 'Z';
    cont[2] = '0';
    cont[3] = '1';
    uint32_t sz = (uint32_t)n;
    cont[4] = (unsigned char)(sz & 255);
    cont[5] = (unsigned char)((sz >> 8) & 255);
    cont[6] = (unsigned char)((sz >> 16) & 255);
    cont[7] = (unsigned char)((sz >> 24) & 255);
    memcpy(cont + 8, raw, rsz);
    snprintf(path, sizeof(path), "%s/%s.lz0", dir, name);
    if (write_file(path, cont, 8 + rsz) != 0) {
        free(cont);
        free(raw);
        return 1;
    }
    printf("%-16s n=%zu raw=%zu\n", name, n, rsz);
    free(cont);
    free(raw);
    return 0;
}

int main(void) {
    const char *dir = "tests/vectors";
    if (mkdir("tests", 0755) != 0 && errno != EEXIST)
        return 1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return 1;

    int fail = 0;
    fail |= emit_one(dir, "empty", NULL, 0);

    {
        unsigned char in[] = {'x'};
        fail |= emit_one(dir, "onebyte", in, 1);
    }
    {
        unsigned char in[] = "hello world";
        fail |= emit_one(dir, "literals", in, 11);
    }
    {
        unsigned char in[] = {'a', 'a', 'a', 'a'};
        fail |= emit_one(dir, "overlap", in, 4);
    }
    {
        unsigned char in[32];
        memcpy(in, "abcdef", 6);
        memcpy(in + 6, "abcdef", 6);
        memcpy(in + 12, "zzzzzzzzzzzzzz", 14);
        memcpy(in + 26, "abcdef", 6);
        fail |= emit_one(dir, "mixed", in, 32);
    }
    {
        unsigned char in[64];
        memset(in, 'q', 64);
        fail |= emit_one(dir, "matchlast", in, 64);
    }
    {
        unsigned char in[48];
        memcpy(in, "pattern-XY", 10);
        memcpy(in + 10, "........", 8);
        memcpy(in + 18, "pattern-XY", 10);
        memcpy(in + 28, "........", 8);
        memcpy(in + 36, "pattern-XY", 10);
        memcpy(in + 46, "!!", 2);
        fail |= emit_one(dir, "matchnew", in, 48);
    }
    {
        unsigned char in[] = {'a', 'b'};
        fail |= emit_one(dir, "eof_after_lit", in, 2);
    }
    return fail ? 1 : 0;
}
