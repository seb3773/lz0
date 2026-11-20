#include "lz0.h"
#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_fail(const char *name, const unsigned char *raw, int rsz, int out_max) {
    unsigned char buf[64];
    int cap = out_max < 64 ? out_max : 64;
    if (cap < 1)
        cap = 1;
    int a = lz0_decompress_to(raw, rsz, buf, cap);
    int b = lz0dec_ref(raw, rsz, buf, cap);
    int c = lz0dec_min(raw, rsz, buf, cap);
#if defined(__x86_64__) || defined(_M_X64)
    int d = lz0dec_x86_64(raw, rsz, buf, cap);
    int df = lz0dec_fast_x86_64(raw, rsz, buf, cap);
    int ok = (a < 0 && b < 0 && c < 0 && d < 0 && df < 0);
    printf("%-20s lib=%d ref=%d min=%d asm=%d fast=%d %s\n", name, a, b, c, d, df, ok ? "ok" : "FAIL");
#elif defined(__aarch64__) || defined(_M_ARM64)
    int d = lz0dec_arm64(raw, rsz, buf, cap);
    int df = lz0dec_fast_arm64(raw, rsz, buf, cap);
    int ok = (a < 0 && b < 0 && c < 0 && d < 0 && df < 0);
    printf("%-20s lib=%d ref=%d min=%d arm64=%d fast=%d %s\n", name, a, b, c, d, df, ok ? "ok" : "FAIL");
#elif (defined(__riscv) && __riscv_xlen == 64)
    int d = lz0dec_riscv64(raw, rsz, buf, cap);
    int df = lz0dec_fast_riscv64(raw, rsz, buf, cap);
    int ok = (a < 0 && b < 0 && c < 0 && d < 0 && df < 0);
    printf("%-20s lib=%d ref=%d min=%d riscv64=%d fast=%d %s\n", name, a, b, c, d, df, ok ? "ok" : "FAIL");
#else
    int ok = (a < 0 && b < 0 && c < 0);
    printf("%-20s lib=%d ref=%d min=%d %s\n", name, a, b, c, ok ? "ok" : "FAIL");
#endif
    return ok ? 0 : 1;
}

int main(void) {
    int fail = 0;
    unsigned char tiny[1] = {0};
    fail |= expect_fail("empty_in", tiny, 0, 8);
    fail |= expect_fail("trunc_one", tiny, 1, 8);

    unsigned char trunc_elias[] = {0x00};
    fail |= expect_fail("trunc_elias", trunc_elias, 1, 16);

    unsigned char no_eof[] = {0x80, 'x'};
    fail |= expect_fail("no_eof", no_eof, 2, 16);

    unsigned char bad_off[8];
    memset(bad_off, 0, sizeof(bad_off));
    fail |= expect_fail("zeros", bad_off, 8, 16);

    {
        unsigned char in[] = {'a', 'a', 'a', 'a'};
        unsigned char *raw = NULL;
        size_t rsz = 0;
        lz0_config cfg;
        lz0_stats st;
        lz0_config_default(&cfg, LZ0_PARSE_SDP);
        lz0_h2c8_apply();
        if (lz0_compress(in, 4, &raw, &rsz, &cfg, &st) != 0) {
            printf("overlap compress fail\n");
            return 1;
        }
        fail |= expect_fail("trunc_payload", raw, rsz > 1 ? (int)rsz - 1 : 1, 4);
        fail |= expect_fail("too_small_out", raw, (int)rsz, 1);
        unsigned char *out = (unsigned char *)malloc(4);
        int d = lz0dec_ref(raw, (int)rsz, out, 4);
        if (d != 4 || memcmp(out, in, 4)) {
            printf("sanity overlap FAIL\n");
            fail = 1;
        } else {
            printf("%-20s ok\n", "sanity_overlap");
        }
        free(out);
        free(raw);
    }
    return fail ? 1 : 0;
}
