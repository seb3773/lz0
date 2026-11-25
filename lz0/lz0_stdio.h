#ifndef LZ0_STDIO_H
#define LZ0_STDIO_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* Shared CLI stream I/O supporting "-" for stdin/stdout and 64-bit file sizes. */
static inline int lz0_cli_set_binary(FILE *fp) {
#if defined(_WIN32)
    return _setmode(_fileno(fp), _O_BINARY);
#else
    (void)fp;
    return 0;
#endif
}

static inline unsigned char *lz0_cli_read(const char *path, size_t *sz_out) {
    int is_stdin = (strcmp(path, "-") == 0);
    FILE *f = is_stdin ? stdin : fopen(path, "rb");
    if (!f)
        return NULL;

    if (is_stdin) {
        lz0_cli_set_binary(stdin);
        size_t cap = 65536u;
        size_t n = 0;
        unsigned char *buf = (unsigned char *)malloc(cap);
        if (!buf)
            return NULL;
        for (;;) {
            if (n == cap) {
                if (cap > SIZE_MAX / 2u) {
                    free(buf);
                    return NULL;
                }
                cap *= 2u;
                unsigned char *nb = (unsigned char *)realloc(buf, cap);
                if (!nb) {
                    free(buf);
                    return NULL;
                }
                buf = nb;
            }
            size_t want = cap - n;
            size_t got = fread(buf + n, 1u, want, f);
            n += got;
            if (got < want) {
                if (ferror(f)) {
                    free(buf);
                    return NULL;
                }
                break;
            }
        }
        *sz_out = n;
        return buf;
    }

#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    int64_t sz64 = _ftelli64(f);
    if (sz64 < 0 || (uint64_t)sz64 > (uint64_t)SIZE_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    off_t sz64 = ftello(f);
    if (sz64 < 0 || (uint64_t)sz64 > (uint64_t)SIZE_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
#endif
    size_t sz = (size_t)sz64;
    unsigned char *p = (unsigned char *)malloc(sz > 0 ? sz : 1u);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(p, 1u, sz, f) != sz) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *sz_out = sz;
    return p;
}

static inline int lz0_cli_write(const char *path, const unsigned char *p, size_t n) {
    int is_stdout = (strcmp(path, "-") == 0);
    FILE *f = is_stdout ? stdout : fopen(path, "wb");
    if (!f)
        return -1;
    if (is_stdout)
        lz0_cli_set_binary(stdout);
    int rc = 0;
    if (n > 0 && fwrite(p, 1u, n, f) != n)
        rc = -1;
    if (is_stdout) {
        if (fflush(f) != 0)
            rc = -1;
    } else {
        if (fclose(f) != 0)
            rc = -1;
    }
    return rc;
}

#endif
