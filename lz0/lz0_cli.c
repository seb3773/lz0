#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

static unsigned char *read_file(const char *path, size_t *n) {
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
    unsigned char *p = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
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

static int cmd_c(const char *in_path, const char *out_path, int level) {
    size_t n = 0;
    unsigned char *in = read_file(in_path, &n);
    if (!in) {
        fprintf(stderr, "lz0: cannot read %s\n", in_path);
        return 1;
    }
    if (n > 0xffffffffu) {
        fprintf(stderr, "lz0: input larger than 4GiB-1\n");
        free(in);
        return 1;
    }
    unsigned char *raw = NULL;
    size_t rsz = 0;
    lz0_config cfg;
    lz0_stats st;
    lz0_config_level(&cfg, level);
    if (n == 0) {
        unsigned char cont[8] = {'L', 'Z', '0', '1', 0, 0, 0, 0};
        int rc = write_file(out_path, cont, 8);
        free(in);
        return rc != 0;
    }
    if (lz0_compress(in, n, &raw, &rsz, &cfg, &st) != 0) {
        fprintf(stderr, "lz0: compress failed\n");
        free(in);
        return 1;
    }
    unsigned char *cont = (unsigned char *)malloc(8 + rsz);
    if (!cont) {
        free(raw);
        free(in);
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
    int rc = write_file(out_path, cont, 8 + rsz);
    free(cont);
    free(raw);
    free(in);
    return rc != 0;
}

static int cmd_d(const char *in_path, const char *out_path) {
    size_t n = 0;
    unsigned char *cont = read_file(in_path, &n);
    if (!cont) {
        fprintf(stderr, "lz0: cannot read %s\n", in_path);
        return 1;
    }
    if (n < 8 || memcmp(cont, "LZ01", 4) != 0) {
        fprintf(stderr, "lz0: not an LZ01 container\n");
        free(cont);
        return 1;
    }
    uint32_t sz = (uint32_t)cont[4] | ((uint32_t)cont[5] << 8) |
                  ((uint32_t)cont[6] << 16) | ((uint32_t)cont[7] << 24);
    unsigned char *pay = cont + 8;
    size_t psz = n - 8;
    if (sz == 0) {
        int rc = 0;
        if (psz != 0) {
            fprintf(stderr, "lz0: empty size with nonempty payload\n");
            rc = 1;
        } else if (write_file(out_path, NULL, 0) != 0)
            rc = 1;
        free(cont);
        return rc;
    }
    unsigned char *out = (unsigned char *)malloc(sz);
    if (!out) {
        free(cont);
        return 1;
    }
    int d = lz0_decompress_to(pay, (int)psz, out, (int)sz);
    if (d < 0 || (uint32_t)d != sz) {
        fprintf(stderr, "lz0: decompress failed\n");
        free(out);
        free(cont);
        return 1;
    }
    int rc = write_file(out_path, out, (size_t)d);
    free(out);
    free(cont);
    return rc != 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: lz0 c [-1..-9] <in> <out.lz0>\n       lz0 d <in.lz0> <out>\n");
        return 1;
    }
    if (!strcmp(argv[1], "c")) {
        int level = 6;
        int in_idx = 2;
        if (argv[2][0] == '-' && argv[2][1] >= '1' && argv[2][1] <= '9' && argv[2][2] == '\0') {
            level = argv[2][1] - '0';
            in_idx = 3;
        }
        if (argc != in_idx + 2) {
            fprintf(stderr, "usage: lz0 c [-1..-9] <in> <out.lz0>\n");
            return 1;
        }
        return cmd_c(argv[in_idx], argv[in_idx + 1], level);
    }
    if (!strcmp(argv[1], "d")) {
        if (argc != 4) {
            fprintf(stderr, "usage: lz0 d <in.lz0> <out>\n");
            return 1;
        }
        return cmd_d(argv[2], argv[3]);
    }
    fprintf(stderr, "usage: lz0 c [-1..-9] <in> <out.lz0>\n       lz0 d <in.lz0> <out>\n");
    return 1;
}
