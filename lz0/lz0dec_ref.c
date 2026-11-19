#include "lz0dec.h"

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

static int br_elias(br_t *br, int inverted) {
    int value = 1;
    for (;;) {
        if (br_bit(br))
            return br->fail ? -1 : value;
        int p = br_bit(br);
        if (br->fail || value >= (1 << 29)) {
            br->fail = 1;
            return -1;
        }
        value = (value << 1) | (p ^ inverted);
    }
}

static int br_len_new(br_t *br) {
    int x = (br_bit(br) << 2) | (br_bit(br) << 1) | br_bit(br);
    if (br->fail)
        return -1;
    if (x < 7)
        return x + 1;
    int e = br_elias(br, 0);
    if (e < 0)
        return -1;
    return 7 + e;
}

static int copy_match(unsigned char *out, int *out_pos, int out_max,
                      int offset, int length) {
    int src = *out_pos - offset;
    if (offset < 1 || src < 0)
        return -1;
    if (length < 1 || *out_pos > out_max - length)
        return -1;
    for (int i = 0; i < length; i++)
        out[*out_pos + i] = out[src + i];
    *out_pos += length;
    return 0;
}

static int new_loop(br_t *br, unsigned char *out, int *out_pos, int out_max,
                    int *last_offset) {
    for (;;) {
        if (!br_bit(br))
            return br->fail ? -1 : 0;
        int msb = br_elias(br, 1);
        int t = br_byte(br);
        if (msb < 1 || t < 0)
            return -1;
        long long off = (long long)msb * 256 - t;
        if (off < 1 || off > *out_pos)
            return -1;
        *last_offset = (int)off;
        int v = br_len_new(br);
        if (v < 1)
            return -1;
        if (copy_match(out, out_pos, out_max, *last_offset, v + 1) < 0)
            return -1;
        if (!br_bit(br))
            return br->fail ? -1 : 1;
    }
}

int lz0dec_ref(const unsigned char *in, int in_size,
               unsigned char *out, int out_max) {
    if (!in || in_size <= 0 || !out || out_max <= 0)
        return -1;

    br_t br;
    br.src = in;
    br.end = in + in_size;
    br.mask = 0;
    br.value = 0;
    br.fail = 0;

    int last_offset = LZ0DEC_INITIAL_OFFSET;
    int out_pos = 0;

    for (;;) {
        int length = br_elias(&br, 0);
        if (length < 1)
            return -1;
        if (out_pos > out_max - length)
            return -1;
        for (int i = 0; i < length; i++) {
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
                return out_pos;
        } else {
            length = br_elias(&br, 0);
            if (length < 1)
                return -1;
            if (copy_match(out, &out_pos, out_max, last_offset, length) < 0)
                return -1;
            if (br_bit(&br)) {
                int r = new_loop(&br, out, &out_pos, out_max, &last_offset);
                if (r < 0)
                    return -1;
                if (r == 0)
                    return out_pos;
            } else if (br.fail) {
                return -1;
            }
        }
        if (br.fail)
            return -1;
    }
}
