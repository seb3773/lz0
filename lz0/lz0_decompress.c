#include "lz0.h"
#include "lz0_internal.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define INITIAL_OFFSET 1

typedef struct {
    const unsigned char *src;
    const unsigned char *src_end;
    int bit_mask;
    int bit_value;
    int backtrack;
    int last_byte;
    int fail;       /* set when a control-byte refill ran past src_end */
} BitReader;

static inline int rd_byte(BitReader *br) {
    if (br->src >= br->src_end)
        return -1;
    br->last_byte = *br->src++;
    return br->last_byte;
}

static inline int rd_bit(BitReader *br) {
    if (br->backtrack) {
        br->backtrack = 0;
        return br->last_byte & 1;
    }
    br->bit_mask >>= 1;
    if (!br->bit_mask) {
        br->bit_mask = 128;
        br->bit_value = rd_byte(br);
        if (br->bit_value < 0) {
            br->fail = 1;
            return 0;
        }
    }
    return (br->bit_value & br->bit_mask) ? 1 : 0;
}

static inline int rd_elias_gamma(BitReader *br, int inverted, uint64_t *val_out) {
    uint64_t value = 1;
    while (!rd_bit(br)) {
        int b = rd_bit(br);
        if (value >= (1ULL << 54))
            return -1;
        value = (value << 1) | (uint64_t)(b ^ inverted);
    }
    if (br->fail)
        return -1;
    *val_out = value;
    return 0;
}

static inline int rd_len_coded(BitReader *br, int code, uint64_t *len_out) {
    if (code == 1) {
        uint64_t n = 1;
        while (rd_bit(br)) {
            n++;
            if (n > 7u) {
                uint64_t e = 0;
                if (rd_elias_gamma(br, 0, &e) < 0)
                    return -1;
                *len_out = 7u + e;
                return 0;
            }
        }
        if (br->fail)
            return -1;
        *len_out = n;
        return 0;
    }
    if (code == 2) {
        int x = (rd_bit(br) << 2) | (rd_bit(br) << 1) | rd_bit(br);
        if (br->fail)
            return -1;
        if (x < 7) {
            *len_out = (uint64_t)(x + 1);
            return 0;
        }
        uint64_t e = 0;
        if (rd_elias_gamma(br, 0, &e) < 0)
            return -1;
        *len_out = 7u + e;
        return 0;
    }
    return rd_elias_gamma(br, 0, len_out);
}

static inline int rd_len(BitReader *br, int len_code, uint64_t *len_out) {
    int code = (len_code == 3) ? 0 : len_code;
    return rd_len_coded(br, code, len_out);
}

static inline int rd_len_new(BitReader *br, int len_code, uint64_t *len_out) {
    int code = (len_code == 3) ? 2 : len_code;
    return rd_len_coded(br, code, len_out);
}

static inline int rd_new_offset(BitReader *br, size_t *offset_out, int lsb_bits) {
    if (!rd_bit(br))
        return br->fail ? -1 : 0;   /* a truncated stream is not an EOF */
    uint64_t msb = 0;
    if (rd_elias_gamma(br, 1, &msb) < 0 || msb < 1u)
        return -1;
    int sh = lsb_bits;
    if (sh < 1)
        sh = 7;
    if (sh == 7) {
        int t = rd_byte(br);
        if (t < 0)
            return -1;
        uint64_t off = (msb << 7) - (uint64_t)(t >> 1);
        if (off < 1u || off > (uint64_t)SIZE_MAX)
            return -1;
        *offset_out = (size_t)off;
        br->backtrack = 1;
        return 1;
    }
    if (sh == 8) {
        int t = rd_byte(br);
        if (t < 0)
            return -1;
        uint64_t off = (msb << 8) - (uint64_t)t;
        if (off < 1u || off > (uint64_t)SIZE_MAX)
            return -1;
        *offset_out = (size_t)off;
        br->backtrack = 0;
        return 1;
    }
    uint64_t inv = 0;
    for (int i = 0; i < sh; i++)
        inv = (inv << 1) | (uint64_t)rd_bit(br);
    if (br->fail)
        return -1;
    uint64_t off = (msb << sh) - inv;
    if (off < 1u || off > (uint64_t)SIZE_MAX)
        return -1;
    *offset_out = (size_t)off;
    br->backtrack = 0;
    return 1;
}

static inline int copy_match(unsigned char *out, size_t out_pos, size_t out_max,
                             size_t offset, uint64_t length) {
    if (offset < 1u || offset > out_pos || length < 1u ||
        length > (uint64_t)(out_max - out_pos))
        return -1;
    size_t src_pos = out_pos - offset;
    size_t len = (size_t)length;
    for (size_t i = 0; i < len; i++)
        out[out_pos + i] = out[src_pos + i];
    return 0;
}

/* Decoder core. len_code / lsb_bits are the private encoder knobs; the
   public entry point instantiates this body with the public encoding
   (LEN=3, LSB=8) as compile-time constants, so every knob test folds away
   and no global is re-read inside the loop (stores through `out` may alias
   any global, which forced a reload per token before). */
static inline __attribute__((always_inline))
int64_t decompress_core(const unsigned char *in, size_t in_size,
                        unsigned char *out, size_t out_max,
                        int len_code, int lsb_bits) {
    BitReader br;
    br.src = in;
    br.src_end = in + in_size;
    br.bit_mask = 0;
    br.bit_value = 0;
    br.backtrack = 0;
    br.last_byte = 0;
    br.fail = 0;

    size_t last_offset = INITIAL_OFFSET;
    size_t out_pos = 0;

    for (;;) {
        uint64_t length = 0;
        if (rd_len(&br, len_code, &length) < 0 || length < 1u ||
            length > (uint64_t)(out_max - out_pos))
            return -1;
        /* one bounds check per run instead of one per byte */
        if ((uint64_t)(br.src_end - br.src) < length)
            return -1;
        /* length >= 1: the first byte is copied outside the loop, so the
           dominant single-byte runs skip the loop set-up entirely */
        size_t len = (size_t)length;
        out[out_pos] = br.src[0];
        for (size_t i = 1; i < len; i++)
            out[out_pos + i] = br.src[i];
        br.src += len;
        br.last_byte = br.src[-1];
        out_pos += len;
        if (rd_bit(&br)) {
            for (;;) {
                int nr = rd_new_offset(&br, &last_offset, lsb_bits);
                if (nr == 0)
                    return (int64_t)out_pos;
                if (nr < 0)
                    return -1;
                if (rd_len_new(&br, len_code, &length) < 0)
                    return -1;
                length += 1u;
                if (copy_match(out, out_pos, out_max, last_offset, length) < 0)
                    return -1;
                out_pos += (size_t)length;
                if (!rd_bit(&br))
                    break;
            }
        } else {
            if (rd_len(&br, len_code, &length) < 0)
                return -1;
            if (copy_match(out, out_pos, out_max, last_offset, length) < 0)
                return -1;
            out_pos += (size_t)length;
            if (!rd_bit(&br))
                continue;
            for (;;) {
                int nr = rd_new_offset(&br, &last_offset, lsb_bits);
                if (nr == 0)
                    return (int64_t)out_pos;
                if (nr < 0)
                    return -1;
                uint64_t len2 = 0;
                if (rd_len_new(&br, len_code, &len2) < 0)
                    return -1;
                len2 += 1u;
                if (copy_match(out, out_pos, out_max, last_offset, len2) < 0)
                    return -1;
                out_pos += (size_t)len2;
                if (!rd_bit(&br))
                    break;
            }
        }
    }
}

int64_t lz0_decompress_ex(const unsigned char *in, size_t in_size,
                          unsigned char *out, size_t out_max) {
    if (!in || in_size == 0 || !out || out_max == 0 || out_max > (size_t)INT64_MAX)
        return -1;
    int len_code = lz0_len_code, lsb_bits = lz0_offset_lsb_bits;
#if !defined(__riscv)
    /* Constant-folded instance for the public encoding. Not used on RISC-V:
       with GCC 15 -O3 the extra instance is unrolled differently there and
       executes more instructions on match-heavy data (ptt5, kennedy.xls). */
    if (len_code == 3 && lsb_bits == 8)
        return decompress_core(in, in_size, out, out_max, 3, 8);
#endif
    return decompress_core(in, in_size, out, out_max, len_code, lsb_bits);
}

int lz0_decompress_to(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max) {
    if (in_size <= 0 || out_max <= 0)
        return -1;
    int64_t r = lz0_decompress_ex(in, (size_t)in_size, out, (size_t)out_max);
    return (r < 0 || r > INT_MAX) ? -1 : (int)r;
}
