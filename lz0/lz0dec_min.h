/*
 * lz0dec_min.h - Single-header minimal LZ0 (bitstream v0.1) decompressor.
 *
 * Usage:
 *   In ONE translation unit, before #include "lz0dec_min.h":
 *     #define LZ0DEC_IMPLEMENTATION
 *     #include "lz0dec_min.h"
 *
 *   In any other translation unit:
 *     #include "lz0dec_min.h"
 *
 * Optional:
 *   #define LZ0DEC_STATIC   // mark definitions static (e.g. header-only local use)
 */
#ifndef LZ0DEC_MIN_H
#define LZ0DEC_MIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef LZ0DEC_STATIC
#define LZ0DEC_API static
#else
#define LZ0DEC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

LZ0DEC_API int lz0dec_min(const unsigned char *in, int in_size,
                          unsigned char *out, int out_max);

LZ0DEC_API int64_t lz0dec_min_ex(const unsigned char *in, size_t in_size,
                                 unsigned char *out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* LZ0DEC_MIN_H */

#ifdef LZ0DEC_IMPLEMENTATION
#ifndef LZ0DEC_MIN_IMPLEMENTED
#define LZ0DEC_MIN_IMPLEMENTED

#include <limits.h>

LZ0DEC_API int64_t lz0dec_min_ex(const unsigned char *in, size_t in_size,
                                 unsigned char *out, size_t out_max) {
    if (!in || in_size == 0 || !out || out_max == 0 || out_max > (size_t)INT64_MAX)
        return -1;
    const unsigned char *src = in;
    const unsigned char *end = in + in_size;
    int mask = 0, val = 0;
    size_t last = 1, pos = 0;

    /* bit: refill when mask is 0; MSB-first */
    #define LZ0DEC_FAIL return -1
    for (;;) {
        uint64_t L = 1, v, msb, e, off;
        int b, p, x, t;
        size_t i;
        /* read_elias not inverted: literal length */
        for (;;) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            b = (val & mask) != 0;
            if (b)
                break;
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            p = (val & mask) != 0;
            if (L >= (1ULL << 54))
                LZ0DEC_FAIL;
            L = (L << 1) | (uint64_t)p;
        }
        /* one input bounds check per literal run, not one per byte */
        if (L < 1u || L > (uint64_t)(out_max - pos) || (uint64_t)(end - src) < L)
            LZ0DEC_FAIL;
        out[pos++] = *src++;
        for (i = 1; i < (size_t)L; i++)
            out[pos++] = *src++;
        /* decision bit */
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                LZ0DEC_FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (!b)
            goto lastm;
    newm:
        /* EOF (0) or new offset (1) */
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                LZ0DEC_FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (!b)
            return (int64_t)pos;
        /* inverted elias msb */
        msb = 1;
        for (;;) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            b = (val & mask) != 0;
            if (b)
                break;
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            p = (val & mask) != 0;
            if (msb >= (1ULL << 54))
                LZ0DEC_FAIL;
            msb = (msb << 1) | (uint64_t)(p ^ 1);
        }
        if (src >= end)
            LZ0DEC_FAIL;
        t = *src++;
        off = msb * 256u - (uint64_t)t;
        if (off < 1u || off > (uint64_t)pos)
            LZ0DEC_FAIL;
        last = (size_t)off;
        /* len_new: 3-bit code */
        x = 0;
        for (i = 0; i < 3; i++) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            x = (x << 1) | ((val & mask) != 0);
        }
        if (x < 7)
            v = (uint64_t)(x + 1);
        else {
            e = 1;
            for (;;) {
                mask >>= 1;
                if (!mask) {
                    if (src >= end)
                        LZ0DEC_FAIL;
                    val = *src++;
                    mask = 128;
                }
                b = (val & mask) != 0;
                if (b)
                    break;
                mask >>= 1;
                if (!mask) {
                    if (src >= end)
                        LZ0DEC_FAIL;
                    val = *src++;
                    mask = 128;
                }
                p = (val & mask) != 0;
                if (e >= (1ULL << 54))
                    LZ0DEC_FAIL;
                e = (e << 1) | (uint64_t)p;
            }
            v = 7u + e;
        }
        L = v + 1u;
        if (L < 2u || last < 1u || last > pos || L > (uint64_t)(out_max - pos))
            LZ0DEC_FAIL;
        for (i = 0; i < (size_t)L; i++)
            out[pos + i] = out[pos - last + i];
        pos += (size_t)L;
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                LZ0DEC_FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (b)
            goto newm;
        continue;
    lastm:
        L = 1;
        for (;;) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            b = (val & mask) != 0;
            if (b)
                break;
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    LZ0DEC_FAIL;
                val = *src++;
                mask = 128;
            }
            p = (val & mask) != 0;
            if (L >= (1ULL << 54))
                LZ0DEC_FAIL;
            L = (L << 1) | (uint64_t)p;
        }
        if (L < 1u || last < 1u || last > pos || L > (uint64_t)(out_max - pos))
            LZ0DEC_FAIL;
        for (i = 0; i < (size_t)L; i++)
            out[pos + i] = out[pos - last + i];
        pos += (size_t)L;
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                LZ0DEC_FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (b)
            goto newm;
    }
#undef LZ0DEC_FAIL
}

LZ0DEC_API int lz0dec_min(const unsigned char *in, int in_size,
                          unsigned char *out, int out_max) {
    if (in_size <= 0 || out_max <= 0)
        return -1;
    int64_t r = lz0dec_min_ex(in, (size_t)in_size, out, (size_t)out_max);
    return (r < 0 || r > INT_MAX) ? -1 : (int)r;
}

#endif /* LZ0DEC_MIN_IMPLEMENTED */
#endif /* LZ0DEC_IMPLEMENTATION */

