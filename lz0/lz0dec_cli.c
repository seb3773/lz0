/*
 * lz0dec - Standalone minimal LZ0 decompressor CLI
 *
 * Decompresses an LZ01 container file to an output file.
 * Demonstrates the tiny footprint of the LZ0 decompression routine.
 */

#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *p = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!p) { fclose(f); return NULL; }
    if (sz && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p); fclose(f); return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return p;
}

static int write_file(const char *path, const unsigned char *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(p, 1, n, f) != n) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "LZ0 Standalone Decompressor v0.1.0\n");
        fprintf(stderr, "Usage: lz0dec <input.lz0> <output>\n");
        return 1;
    }

    size_t in_sz = 0;
    unsigned char *cont = read_file(argv[1], &in_sz);
    if (!cont) {
        fprintf(stderr, "lz0dec: error: cannot read '%s'\n", argv[1]);
        return 1;
    }

    if (in_sz < 8 || memcmp(cont, "LZ01", 4) != 0) {
        fprintf(stderr, "lz0dec: error: '%s' is not a valid LZ01 container\n", argv[1]);
        free(cont);
        return 1;
    }

    uint32_t uncomp_sz = (uint32_t)cont[4] |
                         ((uint32_t)cont[5] << 8) |
                         ((uint32_t)cont[6] << 16) |
                         ((uint32_t)cont[7] << 24);

    unsigned char *payload = cont + 8;
    size_t payload_sz = in_sz - 8;

    if (uncomp_sz == 0) {
        if (payload_sz != 0) {
            fprintf(stderr, "lz0dec: error: corrupt empty file payload\n");
            free(cont);
            return 1;
        }
        write_file(argv[2], NULL, 0);
        free(cont);
        return 0;
    }

    unsigned char *out = (unsigned char *)malloc(uncomp_sz);
    if (!out) {
        fprintf(stderr, "lz0dec: error: memory allocation failure\n");
        free(cont);
        return 1;
    }

    int decomp_len = lz0dec_fast(payload, (int)payload_sz, out, (int)uncomp_sz);
    if (decomp_len < 0 || (uint32_t)decomp_len != uncomp_sz) {
        fprintf(stderr, "lz0dec: error: decompression failed (corrupt or truncated stream)\n");
        free(out);
        free(cont);
        return 1;
    }

    if (write_file(argv[2], out, (size_t)decomp_len) != 0) {
        fprintf(stderr, "lz0dec: error: failed to write output file '%s'\n", argv[2]);
        free(out);
        free(cont);
        return 1;
    }

    free(out);
    free(cont);
    return 0;
}

