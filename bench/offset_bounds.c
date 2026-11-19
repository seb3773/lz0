#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int emit_roundtrip(const char *name, const unsigned char *in, uint32_t n,
                          const lz0_toklist *tl) {
    unsigned char *comp = NULL;
    size_t csz = 0;
    lz0_bitacct acct;
    if (lz0_emit(in, n, tl, &comp, &csz, &acct) != 0) {
        printf("%s: emit fail\n", name);
        return 1;
    }
    unsigned char *out = (unsigned char *)malloc((size_t)n + 8);
    if (!out) {
        free(comp);
        return 1;
    }
    int d = lz0_decompress_to(comp, (int)csz, out, (int)n);
    int bad = (d != (int)n) || memcmp(out, in, n);
    printf("%-12s n=%u csz=%zu %s\n", name, n, csz, bad ? "FAIL" : "ok");
    if (bad && d >= 0)
        printf("  decoded=%d\n", d);
    free(out);
    free(comp);
    return bad ? 1 : 0;
}

static int test_offset(uint32_t offset, uint32_t match_len) {
    uint32_t n = offset + match_len;
    unsigned char *in = (unsigned char *)malloc(n);
    if (!in)
        return 1;
    for (uint32_t i = 0; i < offset; i++)
        in[i] = (unsigned char)(i * 131u + 17u);
    memcpy(in + offset, in, match_len);

    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_toklist_push(&tl, 0, offset, 0);
    lz0_toklist_push(&tl, offset, match_len, offset);

    char name[64];
    snprintf(name, sizeof(name), "off=%u", offset);
    int rc = emit_roundtrip(name, in, n, &tl);
    lz0_toklist_free(&tl);
    free(in);
    return rc;
}

static int test_offset1_run(void) {
    uint32_t n = 64;
    unsigned char *in = (unsigned char *)malloc(n);
    memset(in, 0xA5, n);
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_toklist_push(&tl, 0, 1, 0);
    lz0_toklist_push(&tl, 1, n - 1, 1);
    int rc = emit_roundtrip("off=1-run", in, n, &tl);
    lz0_toklist_free(&tl);
    free(in);
    return rc;
}

static int test_eof_after_literals(void) {
    unsigned char in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_toklist_push(&tl, 0, 8, 0);
    int rc = emit_roundtrip("eof-lit", in, 8, &tl);
    lz0_toklist_free(&tl);
    return rc;
}

static int test_eof_after_match(void) {
    unsigned char in[16];
    for (int i = 0; i < 8; i++)
        in[i] = (unsigned char)(10 + i);
    memcpy(in + 8, in, 8);
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_toklist_push(&tl, 0, 8, 0);
    lz0_toklist_push(&tl, 8, 8, 8);
    int rc = emit_roundtrip("eof-match", in, 16, &tl);
    lz0_toklist_free(&tl);
    return rc;
}

int main(void) {
    static const uint32_t offs[] = {
        127, 128, 129, 32639, 32640, 32641, 65535, 131072, 5u * 1024u * 1024u
    };
    int fail = 0;
    fail |= test_offset1_run();
    fail |= test_eof_after_literals();
    fail |= test_eof_after_match();
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        uint32_t match_len = 32;
        if (offs[i] >= 1024u * 1024u)
            match_len = 64;
        fail |= test_offset(offs[i], match_len);
    }
    return fail ? 1 : 0;
}
