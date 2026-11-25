#ifndef LZ0_H
#define LZ0_H

#include <stddef.h>
#include <stdint.h>

#ifndef LZ0_VERSION
#define LZ0_VERSION "0.6.0-dev"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZ0_PARSE_GREEDY = 1,
    LZ0_PARSE_SDP = 11,
    LZ0_PARSE_ULTRA = 12
} lz0_parse_t;

typedef struct {
    int min_match;
    int max_chain;
    int max_candidates;
    lz0_parse_t parser;
    int score_k;
    int max_states;
    int jump_levels;
    int bt_depth;
    /* Per-instance configuration (populated by lz0_config_level(), isolated = 1).
     * When isolated == 0 (legacy lz0_config_default() or partial aggregate init),
     * lz0_compress / lz0_finder_init snapshot legacy tuning globals into a
     * local config copy; callers using lz0_config_level() or cfg == NULL never
     * read or write encoder globals and are re-entrant and thread-safe. */
    int isolated;
    int level;
    int offset_lsb_bits;
    int len_code;
    int chain_prune;
    int chain_noskip;
    int skip_thresh;
    int skip_stride;
    int hash4_probe;
    int deep_probe;
    int sa_win;
    int sa_lcp;
    int sa_sample;
    int sa_hybrid;
    int sa_thresh;
    int sa_adapt;
    int sa_multiscale;
    int sa_csort_v2;
    int sa_csort_smode;
    int sa_pf_look;
    int sa_count_mode;
    int sa_algo;
    int sdp_fine_matches;
    int len2_max_off;
    int rle_tail;
    int ultra_states;
    size_t ultra_budget;
    int ultra_margin;
    int ultra_fine;
    int ultra_inject;
    int legacy_single_window_scan;
} lz0_config;

typedef struct {
    double finder_ms;
    double parse_ms;
    double emit_ms;
    double total_ms;
    size_t peak_ram;
    uint64_t bits_decision;
    uint64_t bits_elias_len;
    uint64_t bits_offset;
    uint64_t bits_literal;
    uint64_t bits_total;
    uint32_t n_literals;
    uint32_t n_last;
    uint32_t n_new;
} lz0_stats;

extern int lz0_offset_lsb_bits;
extern int lz0_len_code;

void lz0_config_default(lz0_config *cfg, lz0_parse_t parser);
void lz0_config_level(lz0_config *cfg, int level);
void lz0_h2c8_apply(void);

int lz0_compress(const unsigned char *in, size_t in_sz,
                 unsigned char **out, size_t *out_sz,
                 const lz0_config *cfg, lz0_stats *stats);

int lz0_decompress_to(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max);

int64_t lz0_decompress_ex(const unsigned char *in, size_t in_size,
                          unsigned char *out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif
