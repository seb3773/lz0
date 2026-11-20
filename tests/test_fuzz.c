#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rnd_state = 0xC0FFEEu;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state;
}

static int fuzz_roundtrip(int niter, int maxn) {
    int fail = 0;
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_SDP);
    lz0_h2c8_apply();
    for (int i = 0; i < niter; i++) {
        int n = 1 + (int)(rnd() % (uint32_t)maxn);
        unsigned char *in = (unsigned char *)malloc((size_t)n);
        if (!in)
            return 1;
        int mode = (int)(rnd() % 3);
        if (mode == 0) {
            memset(in, (unsigned char)rnd(), (size_t)n);
        } else if (mode == 1) {
            for (int k = 0; k < n; k++)
                in[k] = (unsigned char)rnd();
        } else {
            for (int k = 0; k < n; k++)
                in[k] = (unsigned char)(k < 8 ? rnd() : in[k - 1 - (rnd() % 8)]);
        }
        unsigned char *raw = NULL;
        size_t rsz = 0;
        lz0_stats st;
        if (lz0_compress(in, (size_t)n, &raw, &rsz, &cfg, &st) != 0) {
            printf("rt %d n=%d compress FAIL\n", i, n);
            free(in);
            fail = 1;
            continue;
        }
        unsigned char *a = (unsigned char *)malloc((size_t)n);
        unsigned char *b = (unsigned char *)malloc((size_t)n);
        unsigned char *c = (unsigned char *)malloc((size_t)n);
        int da = lz0_decompress_to(raw, (int)rsz, a, n);
        int db = lz0dec_ref(raw, (int)rsz, b, n);
        int dc = lz0dec_min(raw, (int)rsz, c, n);
#if defined(__x86_64__) || defined(_M_X64)
        unsigned char *d = (unsigned char *)malloc((size_t)n);
        int dd = lz0dec_x86_64(raw, (int)rsz, d, n);
        unsigned char *df_buf = (unsigned char *)malloc((size_t)n);
        int df = lz0dec_fast_x86_64(raw, (int)rsz, df_buf, n);
        if (da != n || db != n || dc != n || dd != n || df != n || memcmp(a, in, (size_t)n) ||
            memcmp(b, in, (size_t)n) || memcmp(c, in, (size_t)n) || memcmp(d, in, (size_t)n) ||
            memcmp(df_buf, in, (size_t)n)) {
            printf("rt %d n=%d decode FAIL lib=%d ref=%d min=%d asm=%d fast=%d\n", i, n, da, db, dc, dd, df);
            fail = 1;
        }
        free(df_buf);
        free(d);
#elif defined(__aarch64__) || defined(_M_ARM64)
        unsigned char *d = (unsigned char *)malloc((size_t)n);
        int dd = lz0dec_arm64(raw, (int)rsz, d, n);
        unsigned char *df_buf = (unsigned char *)malloc((size_t)n);
        int df = lz0dec_fast_arm64(raw, (int)rsz, df_buf, n);
        if (da != n || db != n || dc != n || dd != n || df != n || memcmp(a, in, (size_t)n) ||
            memcmp(b, in, (size_t)n) || memcmp(c, in, (size_t)n) || memcmp(d, in, (size_t)n) ||
            memcmp(df_buf, in, (size_t)n)) {
            printf("rt %d n=%d decode FAIL lib=%d ref=%d min=%d arm64=%d fast=%d\n", i, n, da, db, dc, dd, df);
            fail = 1;
        }
        free(df_buf);
        free(d);
#elif (defined(__riscv) && __riscv_xlen == 64)
        unsigned char *d = (unsigned char *)malloc((size_t)n);
        int dd = lz0dec_riscv64(raw, (int)rsz, d, n);
        unsigned char *df_buf = (unsigned char *)malloc((size_t)n);
        int df = lz0dec_fast_riscv64(raw, (int)rsz, df_buf, n);
        if (da != n || db != n || dc != n || dd != n || df != n || memcmp(a, in, (size_t)n) ||
            memcmp(b, in, (size_t)n) || memcmp(c, in, (size_t)n) || memcmp(d, in, (size_t)n) ||
            memcmp(df_buf, in, (size_t)n)) {
            printf("rt %d n=%d decode FAIL lib=%d ref=%d min=%d riscv64=%d fast=%d\n", i, n, da, db, dc, dd, df);
            fail = 1;
        }
        free(df_buf);
        free(d);
#else
        if (da != n || db != n || dc != n || memcmp(a, in, (size_t)n) ||
            memcmp(b, in, (size_t)n) || memcmp(c, in, (size_t)n)) {
            printf("rt %d n=%d decode FAIL lib=%d ref=%d min=%d\n", i, n, da, db, dc);
            fail = 1;
        }
#endif
        free(a);
        free(b);
        free(c);
        free(raw);
        free(in);
    }
    return fail;
}

static int fuzz_garbage(int niter) {
    int fail = 0;
    unsigned char out[4096];
    for (int i = 0; i < niter; i++) {
        int n = 1 + (int)(rnd() % 64);
        unsigned char *raw = (unsigned char *)malloc((size_t)n);
        if (!raw)
            return 1;
        for (int k = 0; k < n; k++)
            raw[k] = (unsigned char)rnd();
        int da = lz0_decompress_to(raw, n, out, 4096);
        int db = lz0dec_ref(raw, n, out, 4096);
        int dc = lz0dec_min(raw, n, out, 4096);
#if defined(__x86_64__) || defined(_M_X64)
        int dd = lz0dec_x86_64(raw, n, out, 4096);
        int df = lz0dec_fast_x86_64(raw, n, out, 4096);
        if ((da > 4096) || (db > 4096) || (dc > 4096) || (dd > 4096) || (df > 4096)) {
            printf("garbage overflow i=%d\n", i);
            fail = 1;
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        int dd = lz0dec_arm64(raw, n, out, 4096);
        int df = lz0dec_fast_arm64(raw, n, out, 4096);
        if ((da > 4096) || (db > 4096) || (dc > 4096) || (dd > 4096) || (df > 4096)) {
            printf("garbage overflow i=%d\n", i);
            fail = 1;
        }
#elif (defined(__riscv) && __riscv_xlen == 64)
        int dd = lz0dec_riscv64(raw, n, out, 4096);
        int df = lz0dec_fast_riscv64(raw, n, out, 4096);
        if ((da > 4096) || (db > 4096) || (dc > 4096) || (dd > 4096) || (df > 4096)) {
            printf("garbage overflow i=%d\n", i);
            fail = 1;
        }
#else
        if ((da > 4096) || (db > 4096) || (dc > 4096)) {
            printf("garbage overflow i=%d\n", i);
            fail = 1;
        }
#endif
        if (da > 0 && db > 0 && da != db) {
            /* both succeeded with different lengths: allowed only if both <= 4096 */
        }
        free(raw);
    }
    return fail;
}

int main(void) {
    int fail = 0;
    fail |= fuzz_roundtrip(80, 512);
    fail |= fuzz_garbage(200);
    printf("fuzz %s\n", fail ? "FAIL" : "ok");
    return fail ? 1 : 0;
}
