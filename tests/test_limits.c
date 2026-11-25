#define _DEFAULT_SOURCE
#include "lz0.h"
#include "lz0_internal.h"
#include "lz0dec.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && (defined(__LP64__) || defined(_LP64))
#include <sys/mman.h>
#endif

int main(void) {
    int fail = 0;

    /* 1. lz0_compress input bounds: reject 0 and >= 4 GiB without touching input */
    {
        unsigned char dummy = 0x41;
        unsigned char *out = NULL;
        size_t osz = 0;
        lz0_config cfg;
        lz0_config_level(&cfg, 6);
        if (lz0_compress(&dummy, 0, &out, &osz, &cfg, NULL) >= 0) {
            printf("FAIL: compress n=0 succeeded\n");
            free(out);
            fail = 1;
        }
#if SIZE_MAX > 0xffffffffu
        if (lz0_compress(&dummy, (size_t)0x100000000ULL, &out, &osz, &cfg, NULL) >= 0) {
            printf("FAIL: compress n=4GiB succeeded\n");
            free(out);
            fail = 1;
        }
        if (lz0_compress(&dummy, (size_t)0x123456789ULL, &out, &osz, &cfg, NULL) >= 0) {
            printf("FAIL: compress n>4GiB succeeded\n");
            free(out);
            fail = 1;
        }
#endif
    }

    /* 2. Prepare a small valid stream (64 B) for 64-bit decoder bounds checks */
    unsigned char raw_in[64];
    for (size_t i = 0; i < sizeof(raw_in); i++)
        raw_in[i] = (unsigned char)('A' + (i % 13u));
    unsigned char *comp = NULL;
    size_t csz = 0;
    {
        lz0_config cfg;
        lz0_config_level(&cfg, 6);
        if (lz0_compress(raw_in, sizeof(raw_in), &comp, &csz, &cfg, NULL) != 0) {
            printf("FAIL: sample compress failed\n");
            return 1;
        }
    }

    /* 3. Invalid 32-bit and 64-bit decoder arguments */
    {
        unsigned char out[64];
        if (lz0_decompress_to(comp, -1, out, 64) >= 0 ||
            lz0_decompress_to(comp, (int)csz, out, -1) >= 0 ||
            lz0dec_min(comp, -5, out, 64) >= 0 ||
            lz0dec_ref(comp, (int)csz, out, -5) >= 0) {
            printf("FAIL: negative int32 arg accepted\n");
            fail = 1;
        }
        if (lz0_decompress_ex(comp, 0, out, 64) >= 0 ||
            lz0_decompress_ex(comp, csz, out, 0) >= 0 ||
            lz0dec_min_ex(comp, 0, out, 64) >= 0 ||
            lz0dec_ref_ex(comp, csz, out, 0) >= 0) {
            printf("FAIL: zero size_t arg accepted\n");
            fail = 1;
        }
#if SIZE_MAX > 0xffffffffu
        if (lz0_decompress_ex(comp, csz, out, (size_t)INT64_MAX + 1u) >= 0 ||
            lz0dec_min_ex(comp, csz, out, (size_t)INT64_MAX + 1u) >= 0 ||
            lz0dec_ref_ex(comp, csz, out, (size_t)INT64_MAX + 1u) >= 0) {
            printf("FAIL: out_max > INT64_MAX accepted\n");
            fail = 1;
        }
#endif
    }

    /* 4. out_max > INT_MAX succeeds cleanly via 64-bit _ex decoders */
#if SIZE_MAX > 0xffffffffu
    {
        unsigned char out[128];
        size_t big_cap = (size_t)INT_MAX + 4096u;
        int64_t r1 = lz0_decompress_ex(comp, csz, out, big_cap);
        int64_t r2 = lz0dec_min_ex(comp, csz, out, big_cap);
        int64_t r3 = lz0dec_ref_ex(comp, csz, out, big_cap);
        if (r1 != 64 || r2 != 64 || r3 != 64 ||
            memcmp(out, raw_in, 64) != 0) {
            printf("FAIL: _ex big out_max failed (%lld %lld %lld)\n",
                   (long long)r1, (long long)r2, (long long)r3);
            fail = 1;
        }
    }
#endif

    /* 5. in_size > INT_MAX succeeds cleanly on Linux 64-bit via sparse mmap */
#if defined(__linux__) && (defined(__LP64__) || defined(_LP64))
    {
        size_t span = (size_t)INT_MAX + 4096u;
        unsigned char *map = (unsigned char *)mmap(
            NULL, span, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (map != MAP_FAILED) {
            memcpy(map, comp, csz);
            unsigned char out[64];
            int64_t r1 = lz0_decompress_ex(map, span, out, span);
            int64_t r2 = lz0dec_min_ex(map, span, out, span);
            int64_t r3 = lz0dec_ref_ex(map, span, out, span);
            if (r1 != 64 || r2 != 64 || r3 != 64 ||
                memcmp(out, raw_in, 64) != 0) {
                printf("FAIL: _ex >2GiB in_size/out_max failed (%lld %lld %lld)\n",
                       (long long)r1, (long long)r2, (long long)r3);
                fail = 1;
            }
            munmap(map, span);
        }
    }
#endif

    free(comp);
    if (!fail)
        printf("limits ok\n");
    return fail ? 1 : 0;
}

