#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0dec.h"
#include "lz0_stdio.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "LZ0 Compressor CLI v%s\n\n", LZ0_VERSION);
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s c [-1..-9] <in|-> <out.lz0|->   Compress input (default level 6)\n", prog);
    fprintf(stderr, "  %s d <in.lz0|-> <out|->           Decompress LZ01 stream\n", prog);
    fprintf(stderr, "  %s -h | --help                    Show help\n", prog);
    fprintf(stderr, "  %s -v | --version                 Show version\n", prog);
}

static int cmd_c(const char *in_path, const char *out_path, int level) {
    size_t n = 0;
    unsigned char *in = lz0_cli_read(in_path, &n);
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
        int rc = lz0_cli_write(out_path, cont, 8);
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
    int rc = lz0_cli_write(out_path, cont, 8 + rsz);
    free(cont);
    free(raw);
    free(in);
    return rc != 0;
}

static int cmd_d(const char *in_path, const char *out_path) {
    size_t n = 0;
    unsigned char *cont = lz0_cli_read(in_path, &n);
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
        } else if (lz0_cli_write(out_path, NULL, 0) != 0)
            rc = 1;
        free(cont);
        return rc;
    }
    unsigned char *out = (unsigned char *)malloc(sz);
    if (!out) {
        free(cont);
        return 1;
    }
    int64_t d;
    if (psz <= (size_t)INT_MAX && sz <= (uint32_t)INT_MAX)
        d = lz0dec_fast(pay, (int)psz, out, (int)sz);
    else
        d = lz0_decompress_ex(pay, psz, out, (size_t)sz);
    if (d < 0 || (uint64_t)d != (uint64_t)sz) {
        fprintf(stderr, "lz0: decompress failed\n");
        free(out);
        free(cont);
        return 1;
    }
    int rc = lz0_cli_write(out_path, out, (size_t)d);
    free(out);
    free(cont);
    return rc != 0;
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
            printf("lz0 %s\n", LZ0_VERSION);
            return 0;
        }
    }
    if (argc < 4) {
        print_usage(argv[0]);
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
