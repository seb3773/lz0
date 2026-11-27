/*
 * lz0_simd.h - SIMD kernels used by the LZ0 encoder (finder and parsers).
 *
 * Every kernel has a portable C reference (lz0_*_c) and, where available,
 * hand-written assembly variants. All variants return exactly the same
 * value for the same input (same index on ties, same counts), so the
 * selected tier never changes the compressed output.
 *
 * Tier selection (lz0_simd_level), resolved once by lz0_simd_init():
 *   0 = portable C
 *   x86-64 (SysV): 1 = SSE2 (baseline), 2 = AVX2
 *   AArch64      : 1 = AdvSIMD/NEON (baseline)
 *   RISC-V 64    : 1 = RVV 1.0 (only when the kernel reports the V extension)
 * lz0_simd_force (-1 = auto) caps the level, e.g. for A/B benchmarks.
 *
 * Kernels: find_u32, argmax_i32, find_off, kc_worst, count_eq, mru_batch,
 * matchlen (longest common prefix of two byte strings, used by every match
 * length computation of the finder).
 */
#ifndef LZ0_SIMD_H
#define LZ0_SIMD_H

#include "lz0_internal.h"
#include <stdint.h>
#include <string.h>

#if !defined(LZ0_NO_SIMD) && (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && (defined(__x86_64__) || defined(_M_X64))
#define LZ0_SIMD_X86_64 1
#elif !defined(LZ0_NO_SIMD) && (defined(__ELF__) || defined(__APPLE__)) && defined(__aarch64__) && !defined(_WIN32)
#define LZ0_SIMD_ARM64 1
#elif !defined(LZ0_NO_SIMD) && defined(__ELF__) && defined(__riscv) && __riscv_xlen == 64 && !defined(_WIN32)
/* RVV 1.0 and '.option arch, +v' require binutils >= 2.38 (GCC >= 12 or Clang >= 14) */
#if (!defined(__clang__) && __GNUC__ >= 12) || (defined(__clang__) && __clang_major__ >= 14)
#define LZ0_SIMD_RISCV64 1
#endif
#endif

extern int lz0_simd_level;
extern int lz0_simd_force;

/* Detects the best tier for this CPU (honouring lz0_simd_force). Cheap and
 * idempotent; called from lz0_finder_init(). Returns the selected level. */
int lz0_simd_init(void);
/* Human readable name of the active tier ("c", "sse2", "avx2", "neon", "rvv"). */
const char *lz0_simd_name(void);

/* Portable references (also the fallback tier). */
int lz0_find_u32_c(const uint32_t *p, int n, uint32_t key);
int lz0_argmax_i32_c(const int32_t *p, int n);
int lz0_find_off_c(const lz0_match *m, int n, uint32_t off);
int lz0_kc_worst_c(const int32_t *score, const lz0_match *m, int n);
uint32_t lz0_count_eq_c(const unsigned char *a, const unsigned char *b, uint32_t len,
                        uint32_t step);
void lz0_mru_batch_c(uint32_t *mru, int *n_mru, const lz0_match *m, int n);
int lz0_matchlen_c(const unsigned char *a, const unsigned char *b, int max);
uint64_t lz0_bin40_hits_c(const unsigned char *p, uint32_t w0, uint32_t pm);

#define LZ0_SIMD_DECL(sfx)                                                          \
    int lz0_find_u32_##sfx(const uint32_t *p, int n, uint32_t key);                 \
    int lz0_argmax_i32_##sfx(const int32_t *p, int n);                              \
    int lz0_find_off_##sfx(const lz0_match *m, int n, uint32_t off);                \
    int lz0_kc_worst_##sfx(const int32_t *score, const lz0_match *m, int n);        \
    uint32_t lz0_count_eq_##sfx(const unsigned char *a, const unsigned char *b,     \
                                uint32_t len, uint32_t step);                       \
    int lz0_matchlen_##sfx(const unsigned char *a, const unsigned char *b, int max);\
    uint64_t lz0_bin40_hits_##sfx(const unsigned char *p, uint32_t w0, uint32_t pm);

#if defined(LZ0_SIMD_X86_64)
LZ0_SIMD_DECL(sse2)
LZ0_SIMD_DECL(avx2)
void lz0_mru_batch_sse2(uint32_t *mru, int *n_mru, const lz0_match *m, int n);
#define lz0_mru_batch_avx2 lz0_mru_batch_sse2
#define LZ0_SIMD_CALL(fn, ...)                                                      \
    (lz0_simd_level >= 2 ? fn##_avx2(__VA_ARGS__)                                   \
     : lz0_simd_level == 1 ? fn##_sse2(__VA_ARGS__) : fn##_c(__VA_ARGS__))
#elif defined(LZ0_SIMD_ARM64)
LZ0_SIMD_DECL(neon)
void lz0_mru_batch_neon(uint32_t *mru, int *n_mru, const lz0_match *m, int n);
#define LZ0_SIMD_CALL(fn, ...)                                                      \
    (lz0_simd_level >= 1 ? fn##_neon(__VA_ARGS__) : fn##_c(__VA_ARGS__))
#elif defined(LZ0_SIMD_RISCV64)
LZ0_SIMD_DECL(rvv)
void lz0_mru_batch_rvv(uint32_t *mru, int *n_mru, const lz0_match *m, int n);
#define LZ0_SIMD_CALL(fn, ...)                                                      \
    (lz0_simd_level >= 1 ? fn##_rvv(__VA_ARGS__) : fn##_c(__VA_ARGS__))
#else
#define LZ0_SIMD_CALL(fn, ...) fn##_c(__VA_ARGS__)
#endif

/* 64-bit little-endian word compare (first differing byte = lowest set bit
 * of a ^ b / 8). Big-endian or non-GNU builds use the byte loop instead. */
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LZ0_MATCHLEN_LE64 1
#endif

/* Lowest i in [0, n) with p[i] == key, or -1. */
static inline int lz0_simd_find_u32(const uint32_t *p, int n, uint32_t key) {
    return LZ0_SIMD_CALL(lz0_find_u32, p, n, key);
}

/* Lowest i in [0, n) with p[i] == max(p). Requires n >= 1. */
static inline int lz0_simd_argmax_i32(const int32_t *p, int n) {
    return LZ0_SIMD_CALL(lz0_argmax_i32, p, n);
}

/* Lowest i in [0, n) with m[i].offset == off, or -1. */
static inline int lz0_simd_find_off(const lz0_match *m, int n, uint32_t off) {
    return LZ0_SIMD_CALL(lz0_find_off, m, n, off);
}

/* Lowest i in [0, n) minimising (score[i], m[i].length) lexicographically.
 * Requires 1 <= n <= 64. */
static inline int lz0_simd_kc_worst(const int32_t *score, const lz0_match *m, int n) {
    return LZ0_SIMD_CALL(lz0_kc_worst, score, m, n);
}

/* Number of t in {0, step, 2*step, ...}, t < len, with a[t] == b[t].
 * Vector tiers support step 1, 2 and 4; other steps use the C reference. */
static inline uint32_t lz0_simd_count_eq(const unsigned char *a, const unsigned char *b,
                                         uint32_t len, uint32_t step) {
    if (step != 1 && step != 2 && step != 4)
        return lz0_count_eq_c(a, b, len, step);
    return LZ0_SIMD_CALL(lz0_count_eq, a, b, len, step);
}

/* Move-to-front of every non-zero m[i].offset with length >= 4, or
 * length >= 3 and offset <= 512, into the 8-entry MRU list mru[0..*n_mru). */
static inline void lz0_simd_mru_batch(uint32_t *mru, int *n_mru, const lz0_match *m, int n) {
    if (n <= 0)
        return;
    LZ0_SIMD_CALL(lz0_mru_batch, mru, n_mru, m, n);
}

/* Number of identical leading bytes of a and b, in [0, max] (0 if max <= 0).
 * Reads only a[0..max) and b[0..max).
 *
 * Most finder candidates differ within the first 8 bytes, so a single
 * 64-bit compare is done inline; only candidates that survive it pay for
 * the call into the vector tier (32 / 16 bytes per step). */
static inline int lz0_simd_matchlen(const unsigned char *a, const unsigned char *b, int max) {
#if defined(LZ0_MATCHLEN_LE64)
    if (__builtin_expect(max >= 8, 1)) {
        uint64_t xa, xb;
        memcpy(&xa, a, 8);
        memcpy(&xb, b, 8);
        if (xa != xb)
            return (int)(__builtin_ctzll(xa ^ xb) >> 3);
        if (max == 8)
            return 8;
        return 8 + LZ0_SIMD_CALL(lz0_matchlen, a + 8, b + 8, max - 8);
    }
#endif
    return lz0_matchlen_c(a, b, max);
}

/* Bitmask b in [0, 40) of lz0_bin_offsets[b] matches at p = in + pos - 256.
 * Reads strictly within [p, p + 256). */
static inline uint64_t lz0_simd_bin40_hits(const unsigned char *p, uint32_t w0, uint32_t pm) {
    return LZ0_SIMD_CALL(lz0_bin40_hits, p, w0, pm);
}

/* Batch row preparation for ultra DP (1 <= nsi <= 64): finds b_out, b2_out,
 * pass_mask, lit_cand_mask, um[0..nsi), and h_lit[0..nsi). */
typedef struct {
    const uint32_t *rkey;
    const int32_t *rcost;
    const unsigned char *in;
    uint32_t pos;
    int32_t nsi;
    int32_t row_best;
    int32_t margin;
    int32_t exact;
    int32_t b_out;
    int32_t b2_out;
    int32_t _pad;
    uint64_t pass_mask;
    uint64_t lit_cand_mask;
    int32_t um[64];
    uint32_t h_lit[64];
} lz0_u_prep;

void lz0_u_row_prep_c(lz0_u_prep *p);
#if defined(LZ0_SIMD_X86_64)
void lz0_u_row_prep_avx2(lz0_u_prep *p);
#endif
static inline void lz0_simd_u_row_prep(lz0_u_prep *p) {
#if defined(LZ0_SIMD_X86_64)
    if (lz0_simd_level >= 2) {
        lz0_u_row_prep_avx2(p);
        return;
    }
#endif
    lz0_u_row_prep_c(p);
}

/* Batch suffix-array side scan (count <= 128). Writes hits with src < pos in
 * order of distance from origin; out_k[h] encodes (len>=3 ? 0x80 : 0) | d_idx. */
typedef struct {
    const uint32_t *sa_ptr;
    const unsigned char *in;
    uint32_t pos;
    uint32_t n;
    int32_t count;
    int32_t dir;
    uint16_t pref16;
    uint8_t b2;
    uint8_t in_l2;
    uint8_t stop_l2;
    uint8_t ge3_ok;
    uint32_t l2_cut;
    int32_t nhits;
    uint32_t *out_src;
    uint8_t *out_k;
} lz0_sa_scan_args;

int lz0_sa_scan_side_c(lz0_sa_scan_args *a);
#if defined(LZ0_SIMD_X86_64)
int lz0_sa_scan_side_avx2(lz0_sa_scan_args *a);
#endif
static inline int lz0_simd_sa_scan_side(lz0_sa_scan_args *a) {
#if defined(LZ0_SIMD_X86_64)
    if (lz0_simd_level >= 2)
        return lz0_sa_scan_side_avx2(a);
#endif
    return lz0_sa_scan_side_c(a);
}

#endif
