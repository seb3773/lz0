#include "lz0.h"
#include "lz0_internal.h"
#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int decode_all(const char *name, const unsigned char *in, uint32_t n,
                      const unsigned char *comp, size_t csz) {
    unsigned char *a = (unsigned char *)malloc((size_t)n + 8);
    unsigned char *b = (unsigned char *)malloc((size_t)n + 8);
    unsigned char *c = (unsigned char *)malloc((size_t)n + 8);
    int bad = 0;
    if (!a || !b || !c)
        bad = 1;
    else {
        int da = lz0_decompress_to(comp, (int)csz, a, (int)n);
        int db = lz0dec_ref(comp, (int)csz, b, (int)n);
        int dc = lz0dec_min(comp, (int)csz, c, (int)n);
        if (da != (int)n || db != (int)n || dc != (int)n)
            bad = 1;
        else if (memcmp(a, in, n) || memcmp(b, in, n) || memcmp(c, in, n))
            bad = 1;
#if defined(__x86_64__) || defined(_M_X64)
        unsigned char *d = (unsigned char *)malloc((size_t)n + 8);
        if (!d)
            bad = 1;
        else {
            int dd = lz0dec_x86_64(comp, (int)csz, d, (int)n);
            if (dd != (int)n || memcmp(d, in, n))
                bad = 1;
            int df = lz0dec_fast_x86_64(comp, (int)csz, d, (int)n);
            if (df != (int)n || memcmp(d, in, n))
                bad = 1;
            free(d);
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        unsigned char *d = (unsigned char *)malloc((size_t)n + 8);
        if (!d)
            bad = 1;
        else {
            int dd = lz0dec_arm64(comp, (int)csz, d, (int)n);
            if (dd != (int)n || memcmp(d, in, n))
                bad = 1;
            int df = lz0dec_fast_arm64(comp, (int)csz, d, (int)n);
            if (df != (int)n || memcmp(d, in, n))
                bad = 1;
            free(d);
        }
#elif (defined(__riscv) && __riscv_xlen == 64)
        unsigned char *d = (unsigned char *)malloc((size_t)n + 8);
        if (!d)
            bad = 1;
        else {
            int dd = lz0dec_riscv64(comp, (int)csz, d, (int)n);
            if (dd != (int)n || memcmp(d, in, n))
                bad = 1;
            int df = lz0dec_fast_riscv64(comp, (int)csz, d, (int)n);
            if (df != (int)n || memcmp(d, in, n))
                bad = 1;
            free(d);
        }
#endif
    }
    printf("%-16s n=%u csz=%zu %s\n", name, n, csz, bad ? "FAIL" : "ok");
    free(a);
    free(b);
    free(c);
    return bad;
}

static int emit_rt(const char *name, const unsigned char *in, uint32_t n,
                   const lz0_toklist *tl) {
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_bitacct acct;
    if (lz0_emit(in, n, tl, &comp, &csz, &acct) != 0) {
        printf("%s: emit fail\n", name);
        return 1;
    }
    int bad = decode_all(name, in, n, comp, csz);
    free(comp);
    return bad;
}

static int test_offset(uint32_t offset, uint32_t match_len) {
    uint32_t n = offset + match_len;
    unsigned char *in = (unsigned char *)malloc(n);
    if (!in)
        return 1;
    for (uint32_t i = 0; i < offset; i++)
        in[i] = (unsigned char)(i * 131u + 17u);
    for (uint32_t i = 0; i < match_len; i++)
        in[offset + i] = in[i];
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_toklist_push(&tl, 0, offset, 0);
    lz0_toklist_push(&tl, offset, match_len, offset);
    char name[64];
    snprintf(name, sizeof(name), "off=%u", offset);
    int rc = emit_rt(name, in, n, &tl);
    lz0_toklist_free(&tl);
    free(in);
    return rc;
}

static int test_len(uint32_t L) {
    uint32_t n = 1 + L;
    unsigned char *in = (unsigned char *)malloc(n);
    if (!in)
        return 1;
    in[0] = 0xA5;
    memset(in + 1, 0xA5, L);
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_toklist_push(&tl, 0, 1, 0);
    lz0_toklist_push(&tl, 1, L, 1);
    char name[64];
    snprintf(name, sizeof(name), "len=%u", L);
    int rc = emit_rt(name, in, n, &tl);
    lz0_toklist_free(&tl);
    free(in);
    return rc;
}

int main(void) {
    int fail = 0;
    const uint32_t offs[] = {1, 127, 128, 129, 32639, 32640, 32641, 65535, 131072};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
        fail |= test_offset(offs[i], 32);
    fail |= test_offset(5u * 1024u * 1024u, 64);
    const uint32_t lens[] = {1, 2, 3, 7, 8, 9, 10, 11, 15, 16};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
        fail |= test_len(lens[i]);
    return fail ? 1 : 0;
}
