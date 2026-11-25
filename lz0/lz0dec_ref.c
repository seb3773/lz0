#include "lz0dec.h"
#include <limits.h>

enum { LZ0DEC_INITIAL_OFFSET = 1 };

typedef struct {
    const unsigned char *src;
    const unsigned char *end;
    int mask;
    int value;
    int fail;
} br_t;

static int br_byte(br_t *br) {
    if (br->fail || br->src >= br->end) {
        br->fail = 1;
        return -1;
    }
    return *br->src++;
}

static int br_bit(br_t *br) {
    br->mask >>= 1;
    if (!br->mask) {
        int b = br_byte(br);
        if (b < 0)
            return 0;
        br->mask = 128;
        br->value = b;
    }
    return (br->value & br->mask) ? 1 : 0;
}

static int br_elias(br_t *br, int inverted, uint64_t *val_out) {
    uint64_t value = 1;
    for (;;) {
        if (br_bit(br)) {
            if (br->fail)
                return -1;
            *val_out = value;
            return 0;
        }
        int p = br_bit(br);
        if (br->fail || value >= (1ULL << 54)) {
            br->fail = 1;
            return -1;
        }
        value = (value << 1) | (uint64_t)(p ^ inverted);
    }
}

static int br_len_new(br_t *br, uint64_t *val_out) {
    int x = (br_bit(br) << 2) | (br_bit(br) << 1) | br_bit(br);
    if (br->fail)
        return -1;
    if (x < 7) {
        *val_out = (uint64_t)(x + 1);
        return 0;
    }
    uint64_t e = 0;
    if (br_elias(br, 0, &e) < 0)
        return -1;
    *val_out = 7u + e;
    return 0;
}

static int copy_match(unsigned char *out, size_t *out_pos, size_t out_max,
                      size_t offset, uint64_t length) {
    if (offset < 1u || offset > *out_pos || length < 1u ||
        length > (uint64_t)(out_max - *out_pos))
        return -1;
    size_t src = *out_pos - offset;
    size_t len = (size_t)length;
    for (size_t i = 0; i < len; i++)
        out[*out_pos + i] = out[src + i];
    *out_pos += len;
    return 0;
}

static int new_loop(br_t *br, unsigned char *out, size_t *out_pos, size_t out_max,
                    size_t *last_offset) {
    for (;;) {
        if (!br_bit(br))
            return br->fail ? -1 : 0;
        uint64_t msb = 0;
        if (br_elias(br, 1, &msb) < 0 || msb < 1u)
            return -1;
        int t = br_byte(br);
        if (t < 0)
            return -1;
        uint64_t off = msb * 256u - (uint64_t)t;
        if (off < 1u || off > (uint64_t)*out_pos)
            return -1;
        *last_offset = (size_t)off;
        uint64_t v = 0;
        if (br_len_new(br, &v) < 0 || v < 1u)
            return -1;
        if (copy_match(out, out_pos, out_max, *last_offset, v + 1u) < 0)
            return -1;
        if (!br_bit(br))
            return br->fail ? -1 : 1;
    }
}

int64_t lz0dec_ref_ex(const unsigned char *in, size_t in_size,
                      unsigned char *out, size_t out_max) {
    if (!in || in_size == 0 || !out || out_max == 0 || out_max > (size_t)INT64_MAX)
        return -1;

    br_t br;
    br.src = in;
    br.end = in + in_size;
    br.mask = 0;
    br.value = 0;
    br.fail = 0;

    size_t last_offset = LZ0DEC_INITIAL_OFFSET;
    size_t out_pos = 0;

    for (;;) {
        uint64_t length = 0;
        if (br_elias(&br, 0, &length) < 0 || length < 1u)
            return -1;
        if (length > (uint64_t)(out_max - out_pos))
            return -1;
        for (size_t i = 0; i < (size_t)length; i++) {
            int c = br_byte(&br);
            if (c < 0)
                return -1;
            out[out_pos++] = (unsigned char)c;
        }
        if (br_bit(&br)) {
            int r = new_loop(&br, out, &out_pos, out_max, &last_offset);
            if (r < 0)
                return -1;
            if (r == 0)
                return (int64_t)out_pos;
        } else {
            if (br_elias(&br, 0, &length) < 0 || length < 1u)
                return -1;
            if (copy_match(out, &out_pos, out_max, last_offset, length) < 0)
                return -1;
            if (br_bit(&br)) {
                int r = new_loop(&br, out, &out_pos, out_max, &last_offset);
                if (r < 0)
                    return -1;
                if (r == 0)
                    return (int64_t)out_pos;
            } else if (br.fail) {
                return -1;
            }
        }
        if (br.fail)
            return -1;
    }
}

int lz0dec_ref(const unsigned char *in, int in_size,
               unsigned char *out, int out_max) {
    if (in_size <= 0 || out_max <= 0)
        return -1;
    int64_t r = lz0dec_ref_ex(in, (size_t)in_size, out, (size_t)out_max);
    return (r < 0 || r > INT_MAX) ? -1 : (int)r;
}
