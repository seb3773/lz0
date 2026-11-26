#include "lz0_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t index;
    size_t bit_index;
    int bit_pos; /* bits still free in active control byte: 8 down to 0 */
    uint8_t cur_byte;
    int backtrack;
    int lsb7_bt;
    lz0_bitacct *acct;
} wr_t;

/* Packed Elias gamma LUT for v = 1..255 (bits in MSB..LSB of low `nbits` bits). */
const lz0_elias_lut_t lz0_elias_lut[256] = {
    {0,0,0}, {0x1,0x1,1}, {0x1,0x3,3}, {0x3,0x1,3}, {0x1,0xb,5}, {0x3,0x9,5}, {0x9,0x3,5}, {0xb,0x1,5},
    {0x1,0x2b,7}, {0x3,0x29,7}, {0x9,0x23,7}, {0xb,0x21,7}, {0x21,0xb,7}, {0x23,0x9,7}, {0x29,0x3,7}, {0x2b,0x1,7},
    {0x1,0xab,9}, {0x3,0xa9,9}, {0x9,0xa3,9}, {0xb,0xa1,9}, {0x21,0x8b,9}, {0x23,0x89,9}, {0x29,0x83,9}, {0x2b,0x81,9},
    {0x81,0x2b,9}, {0x83,0x29,9}, {0x89,0x23,9}, {0x8b,0x21,9}, {0xa1,0xb,9}, {0xa3,0x9,9}, {0xa9,0x3,9}, {0xab,0x1,9},
    {0x1,0x2ab,11}, {0x3,0x2a9,11}, {0x9,0x2a3,11}, {0xb,0x2a1,11}, {0x21,0x28b,11}, {0x23,0x289,11}, {0x29,0x283,11}, {0x2b,0x281,11},
    {0x81,0x22b,11}, {0x83,0x229,11}, {0x89,0x223,11}, {0x8b,0x221,11}, {0xa1,0x20b,11}, {0xa3,0x209,11}, {0xa9,0x203,11}, {0xab,0x201,11},
    {0x201,0xab,11}, {0x203,0xa9,11}, {0x209,0xa3,11}, {0x20b,0xa1,11}, {0x221,0x8b,11}, {0x223,0x89,11}, {0x229,0x83,11}, {0x22b,0x81,11},
    {0x281,0x2b,11}, {0x283,0x29,11}, {0x289,0x23,11}, {0x28b,0x21,11}, {0x2a1,0xb,11}, {0x2a3,0x9,11}, {0x2a9,0x3,11}, {0x2ab,0x1,11},
    {0x1,0xaab,13}, {0x3,0xaa9,13}, {0x9,0xaa3,13}, {0xb,0xaa1,13}, {0x21,0xa8b,13}, {0x23,0xa89,13}, {0x29,0xa83,13}, {0x2b,0xa81,13},
    {0x81,0xa2b,13}, {0x83,0xa29,13}, {0x89,0xa23,13}, {0x8b,0xa21,13}, {0xa1,0xa0b,13}, {0xa3,0xa09,13}, {0xa9,0xa03,13}, {0xab,0xa01,13},
    {0x201,0x8ab,13}, {0x203,0x8a9,13}, {0x209,0x8a3,13}, {0x20b,0x8a1,13}, {0x221,0x88b,13}, {0x223,0x889,13}, {0x229,0x883,13}, {0x22b,0x881,13},
    {0x281,0x82b,13}, {0x283,0x829,13}, {0x289,0x823,13}, {0x28b,0x821,13}, {0x2a1,0x80b,13}, {0x2a3,0x809,13}, {0x2a9,0x803,13}, {0x2ab,0x801,13},
    {0x801,0x2ab,13}, {0x803,0x2a9,13}, {0x809,0x2a3,13}, {0x80b,0x2a1,13}, {0x821,0x28b,13}, {0x823,0x289,13}, {0x829,0x283,13}, {0x82b,0x281,13},
    {0x881,0x22b,13}, {0x883,0x229,13}, {0x889,0x223,13}, {0x88b,0x221,13}, {0x8a1,0x20b,13}, {0x8a3,0x209,13}, {0x8a9,0x203,13}, {0x8ab,0x201,13},
    {0xa01,0xab,13}, {0xa03,0xa9,13}, {0xa09,0xa3,13}, {0xa0b,0xa1,13}, {0xa21,0x8b,13}, {0xa23,0x89,13}, {0xa29,0x83,13}, {0xa2b,0x81,13},
    {0xa81,0x2b,13}, {0xa83,0x29,13}, {0xa89,0x23,13}, {0xa8b,0x21,13}, {0xaa1,0xb,13}, {0xaa3,0x9,13}, {0xaa9,0x3,13}, {0xaab,0x1,13},
    {0x1,0x2aab,15}, {0x3,0x2aa9,15}, {0x9,0x2aa3,15}, {0xb,0x2aa1,15}, {0x21,0x2a8b,15}, {0x23,0x2a89,15}, {0x29,0x2a83,15}, {0x2b,0x2a81,15},
    {0x81,0x2a2b,15}, {0x83,0x2a29,15}, {0x89,0x2a23,15}, {0x8b,0x2a21,15}, {0xa1,0x2a0b,15}, {0xa3,0x2a09,15}, {0xa9,0x2a03,15}, {0xab,0x2a01,15},
    {0x201,0x28ab,15}, {0x203,0x28a9,15}, {0x209,0x28a3,15}, {0x20b,0x28a1,15}, {0x221,0x288b,15}, {0x223,0x2889,15}, {0x229,0x2883,15}, {0x22b,0x2881,15},
    {0x281,0x282b,15}, {0x283,0x2829,15}, {0x289,0x2823,15}, {0x28b,0x2821,15}, {0x2a1,0x280b,15}, {0x2a3,0x2809,15}, {0x2a9,0x2803,15}, {0x2ab,0x2801,15},
    {0x801,0x22ab,15}, {0x803,0x22a9,15}, {0x809,0x22a3,15}, {0x80b,0x22a1,15}, {0x821,0x228b,15}, {0x823,0x2289,15}, {0x829,0x2283,15}, {0x82b,0x2281,15},
    {0x881,0x222b,15}, {0x883,0x2229,15}, {0x889,0x2223,15}, {0x88b,0x2221,15}, {0x8a1,0x220b,15}, {0x8a3,0x2209,15}, {0x8a9,0x2203,15}, {0x8ab,0x2201,15},
    {0xa01,0x20ab,15}, {0xa03,0x20a9,15}, {0xa09,0x20a3,15}, {0xa0b,0x20a1,15}, {0xa21,0x208b,15}, {0xa23,0x2089,15}, {0xa29,0x2083,15}, {0xa2b,0x2081,15},
    {0xa81,0x202b,15}, {0xa83,0x2029,15}, {0xa89,0x2023,15}, {0xa8b,0x2021,15}, {0xaa1,0x200b,15}, {0xaa3,0x2009,15}, {0xaa9,0x2003,15}, {0xaab,0x2001,15},
    {0x2001,0xaab,15}, {0x2003,0xaa9,15}, {0x2009,0xaa3,15}, {0x200b,0xaa1,15}, {0x2021,0xa8b,15}, {0x2023,0xa89,15}, {0x2029,0xa83,15}, {0x202b,0xa81,15},
    {0x2081,0xa2b,15}, {0x2083,0xa29,15}, {0x2089,0xa23,15}, {0x208b,0xa21,15}, {0x20a1,0xa0b,15}, {0x20a3,0xa09,15}, {0x20a9,0xa03,15}, {0x20ab,0xa01,15},
    {0x2201,0x8ab,15}, {0x2203,0x8a9,15}, {0x2209,0x8a3,15}, {0x220b,0x8a1,15}, {0x2221,0x88b,15}, {0x2223,0x889,15}, {0x2229,0x883,15}, {0x222b,0x881,15},
    {0x2281,0x82b,15}, {0x2283,0x829,15}, {0x2289,0x823,15}, {0x228b,0x821,15}, {0x22a1,0x80b,15}, {0x22a3,0x809,15}, {0x22a9,0x803,15}, {0x22ab,0x801,15},
    {0x2801,0x2ab,15}, {0x2803,0x2a9,15}, {0x2809,0x2a3,15}, {0x280b,0x2a1,15}, {0x2821,0x28b,15}, {0x2823,0x289,15}, {0x2829,0x283,15}, {0x282b,0x281,15},
    {0x2881,0x22b,15}, {0x2883,0x229,15}, {0x2889,0x223,15}, {0x288b,0x221,15}, {0x28a1,0x20b,15}, {0x28a3,0x209,15}, {0x28a9,0x203,15}, {0x28ab,0x201,15},
    {0x2a01,0xab,15}, {0x2a03,0xa9,15}, {0x2a09,0xa3,15}, {0x2a0b,0xa1,15}, {0x2a21,0x8b,15}, {0x2a23,0x89,15}, {0x2a29,0x83,15}, {0x2a2b,0x81,15},
    {0x2a81,0x2b,15}, {0x2a83,0x29,15}, {0x2a89,0x23,15}, {0x2a8b,0x21,15}, {0x2aa1,0xb,15}, {0x2aa3,0x9,15}, {0x2aa9,0x3,15}, {0x2aab,0x1,15}
};

static inline void wr_byte(wr_t *w, unsigned char value) {
    w->data[w->index++] = value;
}

static inline void wr_bits(wr_t *w, uint32_t code, int nbits, int cat) {
    if (w->acct) {
        if (cat == 0)
            w->acct->bits_decision += (uint64_t)nbits;
        else if (cat == 1)
            w->acct->bits_elias_len += (uint64_t)nbits;
        else if (cat == 2)
            w->acct->bits_offset += (uint64_t)nbits;
    }
    if (w->backtrack) {
        uint32_t top_bit = (code >> (nbits - 1)) & 1u;
        if (w->lsb7_bt && top_bit && w->index > 0)
            w->data[w->index - 1] |= 1u;
        w->backtrack = 0;
        w->lsb7_bt = 0;
        nbits--;
        if (nbits == 0)
            return;
        code &= (1u << nbits) - 1u;
    }
    while (nbits > 0) {
        if (w->bit_pos == 0) {
            w->bit_index = w->index;
            w->data[w->index++] = 0;
            w->cur_byte = 0;
            w->bit_pos = 8;
        }
        int take = nbits < w->bit_pos ? nbits : w->bit_pos;
        int shift = nbits - take;
        uint32_t chunk = (code >> shift) & ((1u << take) - 1u);
        w->bit_pos -= take;
        w->cur_byte |= (uint8_t)(chunk << w->bit_pos);
        w->data[w->bit_index] = w->cur_byte;
        nbits -= take;
    }
}

static inline void wr_bit(wr_t *w, int value, int cat) {
    wr_bits(w, (uint32_t)(value != 0), 1, cat);
}

static inline void wr_elias(wr_t *w, uint32_t value, int invert, int cat) {
    if (value < 256u) {
        const lz0_elias_lut_t *e = &lz0_elias_lut[value];
        wr_bits(w, invert ? e->inv_code : e->code, e->nbits, cat);
        return;
    }
    int hi = 31 - __builtin_clz(value);
    for (int b = hi - 1; b >= 0; b--) {
        uint32_t bit = ((value >> b) & 1u) ^ (uint32_t)(invert != 0);
        wr_bits(w, bit, 2, cat);
    }
    wr_bits(w, 1u, 1, cat);
}

static void wr_len_coded(wr_t *w, uint32_t v, int cat, int code) {
    if (code == 1) {
        if (v <= 7u) {
            uint32_t c = (((1u << (v - 1u)) - 1u) << 1);
            wr_bits(w, c, (int)v, cat);
            return;
        }
        wr_bits(w, 0x7fu, 7, cat);
        wr_elias(w, v - 7u, 0, cat);
        return;
    }
    if (code == 2) {
        if (v <= 7u) {
            wr_bits(w, v - 1u, 3, cat);
            return;
        }
        wr_bits(w, 7u, 3, cat);
        wr_elias(w, v - 7u, 0, cat);
        return;
    }
    wr_elias(w, v, 0, cat);
}

static void wr_len(wr_t *w, uint32_t v, int cat, int lcode) {
    int code = (lcode == 3) ? 0 : lcode;
    wr_len_coded(w, v, cat, code);
}

static void wr_len_new(wr_t *w, uint32_t v, int cat, int lcode) {
    int code = (lcode == 3) ? 2 : lcode;
    wr_len_coded(w, v, cat, code);
}

int lz0_emit_cfg(const unsigned char *in, uint32_t n, const lz0_toklist *tl,
                 unsigned char **out, size_t *out_sz, lz0_bitacct *acct,
                 const lz0_config *cfg) {
    if (!tl || tl->n == 0)
        return -1;
    size_t cap = (size_t)n + 64;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf)
        return -1;
    wr_t w;
    memset(&w, 0, sizeof(w));
    w.data = buf;
    w.backtrack = 1;
    w.acct = acct;
    if (acct)
        memset(acct, 0, sizeof(*acct));

    uint32_t last_off = LZ0_INITIAL_OFFSET;
    int can_last = 0;
    int iso = (cfg && cfg->isolated);
    int lcode = cfg ? cfg->len_code : lz0_len_code;
    int lsb_bits = cfg ? cfg->offset_lsb_bits : lz0_offset_lsb_bits;
    lz0_stream_census *sc = iso ? NULL : lz0_stream_census_out;

    for (uint32_t k = 0; k < tl->n; k++) {
        const lz0_token *t = &tl->t[k];
        if (w.index + 32u + (size_t)t->length >= cap) {
            cap = cap * 2u + (size_t)t->length + 64u;
            unsigned char *nb = (unsigned char *)realloc(w.data, cap);
            if (!nb) {
                free(w.data);
                return -1;
            }
            w.data = nb;
        }
        if (!t->offset) {
            wr_bit(&w, 0, 0);
            wr_len(&w, t->length, 1, lcode);
            memcpy(w.data + w.index, in + t->pos, t->length);
            w.index += t->length;
            if (acct)
                acct->bits_literal += (uint64_t)t->length << 3;
            if (sc) {
                sc->n_lit++;
                sc->bytes_lit += t->length;
                sc->bits_lit_flag++;
                sc->bits_lit_len += (uint64_t)lz0_len_payload_bits_c(cfg, (int)t->length);
                sc->bits_lit_pay += (uint64_t)t->length << 3;
                int lb = 0;
                uint32_t L = t->length;
                while (L > 1 && lb < 7) {
                    L >>= 1;
                    lb++;
                }
                sc->hist_len[lb]++;
                int ix = t->length > 32 ? 32 : (int)t->length;
                sc->nlen[ix]++;
                sc->nlen_lit[ix]++;
                sc->blen[ix] += (uint64_t)lz0_len_payload_bits_c(cfg, (int)t->length);
            }
            can_last = 1;
        } else if (t->offset == last_off && can_last) {
            wr_bit(&w, 0, 0);
            wr_len(&w, t->length, 1, lcode);
            if (sc) {
                sc->n_last++;
                sc->bytes_last += t->length;
                sc->bits_last_flag++;
                sc->bits_last_len += (uint64_t)lz0_len_payload_bits_c(cfg, (int)t->length);
                int lb = 0;
                uint32_t L = t->length;
                while (L > 1 && lb < 7) {
                    L >>= 1;
                    lb++;
                }
                sc->hist_len[lb]++;
                int ix = t->length > 32 ? 32 : (int)t->length;
                sc->nlen[ix]++;
                sc->nlen_last[ix]++;
                sc->blen[ix] += (uint64_t)lz0_len_payload_bits_c(cfg, (int)t->length);
            }
            can_last = 0;
        } else {
            wr_bit(&w, 1, 0);
            wr_bit(&w, 1, 2);
            {
                int sh = lsb_bits;
                if (sh < 1)
                    sh = 7;
                uint32_t mod = 1u << sh;
                uint32_t msb = ((t->offset - 1u) / mod) + 1u;
                uint32_t rem = (t->offset - 1u) % mod;
                wr_elias(&w, msb, 1, 2);
                if (sh == 7) {
                    wr_byte(&w, (unsigned char)((127u - rem) << 1));
                    if (acct)
                        acct->bits_offset += 8;
                    w.backtrack = 1;
                    w.lsb7_bt = 1;
                } else if (sh == 8) {
                    wr_byte(&w, (unsigned char)(255u - rem));
                    if (acct)
                        acct->bits_offset += 8;
                    w.backtrack = 0;
                    w.lsb7_bt = 0;
                } else {
                    uint32_t inv = (mod - 1u) - rem;
                    for (int b = sh - 1; b >= 0; b--)
                        wr_bit(&w, (int)((inv >> b) & 1u), 2);
                    w.backtrack = 0;
                    w.lsb7_bt = 0;
                }
                wr_len_new(&w, t->length - 1u, 1, lcode);
                if (sc) {
                    sc->n_new++;
                    sc->bytes_new += t->length;
                    sc->bits_new_flag += 2;
                    sc->bits_new_off_elias += (uint64_t)lz0_elias_bits((int)msb);
                    sc->bits_new_off_lsb += (sh == 7) ? 8u : (uint64_t)sh;
                    sc->bits_new_len += (uint64_t)lz0_len_new_payload_bits_c(cfg, (int)t->length - 1);
                    sc->sum_off += t->offset;
                    int ob = 0;
                    uint32_t o = t->offset;
                    while (o > 1 && ob < 7) {
                        o >>= 1;
                        ob++;
                    }
                    sc->hist_off[ob]++;
                    int lb = 0;
                    uint32_t L = t->length;
                    while (L > 1 && lb < 7) {
                        L >>= 1;
                        lb++;
                    }
                    sc->hist_len[lb]++;
                    int ix = t->length > 32 ? 32 : (int)t->length;
                    sc->nlen[ix]++;
                    sc->nlen_new[ix]++;
                    sc->blen[ix] += (uint64_t)lz0_len_new_payload_bits_c(cfg, (int)t->length - 1);
                }
            }
            last_off = t->offset;
            can_last = 0;
        }
    }

    if (w.index + 32u >= cap) {
        cap += 64u;
        unsigned char *nb = (unsigned char *)realloc(w.data, cap);
        if (!nb) {
            free(w.data);
            return -1;
        }
        w.data = nb;
    }
    wr_bit(&w, 1, 0);
    wr_bit(&w, 0, 2);
    if (sc)
        sc->bits_eof += 2;

    *out = w.data;
    *out_sz = w.index;
    return 0;
}

int lz0_emit(const unsigned char *in, uint32_t n, const lz0_toklist *tl,
             unsigned char **out, size_t *out_sz, lz0_bitacct *acct) {
    return lz0_emit_cfg(in, n, tl, out, out_sz, acct, NULL);
}
