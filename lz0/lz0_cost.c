#include "lz0_internal.h"

int lz0_elias_bits(int value) {
    if (value <= 0)
        return 1;
#ifdef __GNUC__
    return ((32 - __builtin_clz((unsigned)value)) << 1) - 1;
#else
    int bits = 1;
    while (value >>= 1)
        bits += 2;
    return bits;
#endif
}

int lz0_offset_lsb_bits = 8;
int lz0_len_code = 3;

int lz0_len_payload_bits_c(const lz0_config *cfg, int v) {
    int code = cfg ? cfg->len_code : lz0_len_code;
    if (v <= 0)
        return 1;
    if (code == 1) {
        if (v <= 7)
            return v;
        return 7 + lz0_elias_bits(v - 7);
    }
    if (code == 2) {
        if (v <= 7)
            return 3;
        return 3 + lz0_elias_bits(v - 7);
    }
    return lz0_elias_bits(v);
}

int lz0_len_payload_bits(int v) {
    return lz0_len_payload_bits_c(NULL, v);
}

int lz0_len_new_payload_bits_c(const lz0_config *cfg, int v) {
    int code = cfg ? cfg->len_code : lz0_len_code;
    if (code == 3) {
        if (v <= 0)
            return 1;
        if (v <= 7)
            return 3;
        return 3 + lz0_elias_bits(v - 7);
    }
    return lz0_len_payload_bits_c(cfg, v);
}

int lz0_len_new_payload_bits(int v) {
    return lz0_len_new_payload_bits_c(NULL, v);
}

int lz0_cost_literal_c(const lz0_config *cfg, int len) {
    if (len <= 0)
        return 0;
    return 1 + lz0_len_payload_bits_c(cfg, len) + (len << 3);
}

int lz0_cost_literal(int len) {
    return lz0_cost_literal_c(NULL, len);
}

int lz0_cost_last_c(const lz0_config *cfg, int len) {
    if (len <= 0)
        return 0;
    return 1 + lz0_len_payload_bits_c(cfg, len);
}

int lz0_cost_last(int len) {
    return lz0_cost_last_c(NULL, len);
}

int lz0_cost_new_c(const lz0_config *cfg, int offset, int len) {
    if (len < 2 || offset < 1)
        return 1 << 30;
    int sh = cfg ? cfg->offset_lsb_bits : lz0_offset_lsb_bits;
    if (sh < 1)
        sh = 7;
    int offset_msb = (offset - 1) >> sh;
    int lsb_cost = (sh == 7) ? 8 : sh;
    return 1 + 1 + lsb_cost + lz0_elias_bits(offset_msb + 1) +
           lz0_len_new_payload_bits_c(cfg, len - 1);
}

int lz0_cost_new(int offset, int len) {
    return lz0_cost_new_c(NULL, offset, len);
}

static int next_elias_plateau(int value, int max_value) {
    if (value >= max_value)
        return max_value;
    if (value <= 0)
        return max_value < 1 ? max_value : 1;
#ifdef __GNUC__
    unsigned bits = (unsigned)(32 - __builtin_clz((unsigned)value));
    if (bits >= 31)
        return max_value;
    unsigned hi = (1u << bits) - 1u;
    return hi < (unsigned)max_value ? (int)hi : max_value;
#else
    int n = value + 1;
    int cur = lz0_elias_bits(value);
    while (n <= max_value && lz0_elias_bits(n) == cur)
        n++;
    return n - 1;
#endif
}

static int next_len_plateau(int value, int max_value, int code) {
    if (code == 0)
        return next_elias_plateau(value, max_value);
    if (value >= max_value)
        return max_value;
    if (code == 1) {
        if (value < 7)
            return value;
        int p = next_elias_plateau(value - 7, max_value - 7);
        return p + 7;
    }
    if (value < 7)
        return 7 < max_value ? 7 : max_value;
    int p = next_elias_plateau(value - 7, max_value - 7);
    return p + 7;
}

int lz0_next_plateau_last_c(const lz0_config *cfg, int len, int max_len) {
    int lcode = cfg ? cfg->len_code : lz0_len_code;
    int code = (lcode == 3) ? 0 : lcode;
    return next_len_plateau(len, max_len, code);
}

int lz0_next_plateau_last(int len, int max_len) {
    return lz0_next_plateau_last_c(NULL, len, max_len);
}

int lz0_next_plateau_new_c(const lz0_config *cfg, int len, int max_len) {
    if (len < 2)
        len = 2;
    int lcode = cfg ? cfg->len_code : lz0_len_code;
    int code = (lcode == 3) ? 2 : lcode;
    int p = next_len_plateau(len - 1, max_len - 1, code);
    return p + 1;
}

int lz0_next_plateau_new(int len, int max_len) {
    return lz0_next_plateau_new_c(NULL, len, max_len);
}

int lz0_next_plateau_lit_c(const lz0_config *cfg, int len, int max_len) {
    int lcode = cfg ? cfg->len_code : lz0_len_code;
    int code = (lcode == 3) ? 0 : lcode;
    return next_len_plateau(len, max_len, code);
}

int lz0_next_plateau_lit(int len, int max_len) {
    return lz0_next_plateau_lit_c(NULL, len, max_len);
}

static int finder_hash_bits(const lz0_config *cfg, uint32_t n) {
    int sa_win = cfg ? cfg->sa_win : lz0_sa_win;
    if (sa_win < 16)
        return LZ0_HASH_BITS;
    int hb = 11;
    uint32_t lim = n + (n >> 1);
    while (hb < LZ0_HASH_BITS && (1u << (hb + 1)) <= lim)
        hb++;
    return hb;
}

size_t lz0_ram_finder_c(const lz0_config *cfg, uint32_t n, int jump_levels) {
    size_t ram = 0;
    int sa_hybrid = cfg ? cfg->sa_hybrid : lz0_sa_hybrid;
    int sa_win = cfg ? cfg->sa_win : lz0_sa_win;
    int sa_sample = cfg ? cfg->sa_sample : lz0_sa_sample;
    int sa_lcp = cfg ? cfg->sa_lcp : lz0_sa_lcp;
    int hash4_probe = cfg ? cfg->hash4_probe : lz0_hash4_probe;
    int bt_depth = cfg ? cfg->bt_depth : 0;
    int need_hash = (sa_hybrid != 0) || (sa_win <= 0);
    int need_sa = (sa_win > 0);
    if (need_hash) {
        size_t hsz = (size_t)1u << finder_hash_bits(cfg, n);
        int jslots = 0;
        if (jump_levels > 0) {
            int max_s = (n < 262144u) ? 3 : 4;
            jslots = jump_levels < max_s ? jump_levels : max_s;
        }
        ram += (size_t)n * sizeof(uint32_t) * (1u + (uint32_t)jslots) +
               hsz * sizeof(uint32_t);
        if (hash4_probe)
            ram += hsz * sizeof(uint32_t);
    }
    if (need_sa) {
        int samp = sa_sample > 0 ? sa_sample : 1;
        if (samp > 1)
            ram += (size_t)n * sizeof(uint32_t) +
                   ((size_t)n / (size_t)samp) * sizeof(uint32_t);
        else {
            ram += (size_t)n * sizeof(uint32_t) * 2u;
            if (sa_lcp)
                ram += (size_t)n * sizeof(uint32_t);
        }
    }
    if (bt_depth > 0) {
        int bhb = 11;
        int max_bhb = (n < 262144u) ? 16 : 21;
        while (bhb < max_bhb && (1u << bhb) < n)
            bhb++;
        int bh2b = 11;
        while (bh2b < 16 && (1u << bh2b) < n)
            bh2b++;
        ram += (size_t)n * 2u * sizeof(uint32_t) +
               (((size_t)1u << bhb) + ((size_t)1u << bh2b)) * sizeof(uint32_t);
    }
    return ram;
}

size_t lz0_ram_finder(uint32_t n, int jump_levels) {
    return lz0_ram_finder_c(NULL, n, jump_levels);
}

size_t lz0_ram_sdp(uint32_t n, int k) {
    if (k < 1)
        k = 4;
    if (k > 32)
        k = 32;
    if (k == 2) /* K = 2 parser: 12-byte AoS states (key, cost, prev_pos|slot) + live count */
        return (size_t)(n + 1) * 2u * 12u + (size_t)(n + 1);
    if (n <= 0x00ffffffu) {
        /* Sparse slab-backed DP (lz0_parse_ultra_sdp): 16 B/pos index + active rows */
        size_t active_est = (size_t)n < 512u ? (size_t)n : 512u;
        return ((size_t)n + 1) * 16u + active_est * ((size_t)k * 12u + 16u);
    }
    /* generic parser (>16 MiB fallback): 13-byte SoA states + metadata */
    return (size_t)(n + 1) * (size_t)k * 13u + (size_t)(n + 1) * 10u;
}
