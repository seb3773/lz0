#ifndef LZ0DEC_H
#define LZ0DEC_H

#include <stddef.h>
#include <stdint.h>

#ifndef LZ0_VERSION
#define LZ0_VERSION "0.6.0-dev"
#endif

#ifdef __cplusplus
extern "C" {
#endif

int lz0dec_ref(const unsigned char *in, int in_size,
               unsigned char *out, int out_max);

int64_t lz0dec_ref_ex(const unsigned char *in, size_t in_size,
                      unsigned char *out, size_t out_max);

int lz0dec_min(const unsigned char *in, int in_size,
               unsigned char *out, int out_max);

int64_t lz0dec_min_ex(const unsigned char *in, size_t in_size,
                      unsigned char *out, size_t out_max);

#if (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && (defined(__x86_64__) || defined(_M_X64))
/*
 * Ultra-Tiny ASM: 143 B (ELF), 160 B (Win64 PE), zero table/heap.
 * WARNING: UNCHECKED trusted-stream API — caller guarantees valid non-empty
 * LZ0 v0.1 input and sufficient output capacity.
 */
int lz0dec_ultra_tiny_x86_64(const unsigned char *in, unsigned char *out);
/* Tiny ASM: 288 B (ELF), 304 B (Win64), zero table, full contract checks */
int lz0dec_tiny_x86_64(const unsigned char *in, int in_size,
                       unsigned char *out, int out_max);
/* Compact ASM: ~1.1 KiB, zero table, pure arithmetic */
int lz0dec_x86_64(const unsigned char *in, int in_size,
                  unsigned char *out, int out_max);
/* Fast ASM: dual-engine FAST/SAFE, branch-free Elias, segment window */
int lz0dec_fast_x86_64(const unsigned char *in, int in_size,
                       unsigned char *out, int out_max);
#endif

#if (defined(__ELF__) || defined(__APPLE__)) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)
/*
 * Ultra-Tiny ASM: 224 B, zero stack/table; UNCHECKED trusted-stream API.
 */
int lz0dec_ultra_tiny_arm64(const unsigned char *in, unsigned char *out);
/* Tiny ASM: 376 B, zero stack/table, full contract checks */
int lz0dec_tiny_arm64(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max);
/* Compact ASM: ~1.3 KiB, zero table, pure arithmetic */
int lz0dec_arm64(const unsigned char *in, int in_size,
                 unsigned char *out, int out_max);
/* Fast ASM: dual-engine FAST/SAFE, branch-free Elias, pure leaf */
int lz0dec_fast_arm64(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max);
#endif

#if defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)
/*
 * Ultra-Tiny ASM: 188 B, zero stack/table; UNCHECKED trusted-stream API.
 */
int lz0dec_ultra_tiny_riscv64(const unsigned char *in, unsigned char *out);
/* Tiny ASM: ~300 B, zero stack/table, full contract checks */
int lz0dec_tiny_riscv64(const unsigned char *in, int in_size,
                        unsigned char *out, int out_max);
/* Compact ASM: ~1.1 KiB, zero table, pure arithmetic, pure leaf */
int lz0dec_riscv64(const unsigned char *in, int in_size,
                   unsigned char *out, int out_max);
/* Fast ASM: dual-engine FAST/SAFE, 64-bit window, pure leaf */
int lz0dec_fast_riscv64(const unsigned char *in, int in_size,
                        unsigned char *out, int out_max);
#endif

#if (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && (defined(__x86_64__) || defined(_M_X64))
#define lz0dec_fast lz0dec_fast_x86_64
#define lz0dec_compact lz0dec_x86_64
#define lz0dec_tiny lz0dec_tiny_x86_64
#define lz0dec_ultra_tiny lz0dec_ultra_tiny_x86_64
#elif (defined(__ELF__) || defined(__APPLE__)) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)
#define lz0dec_fast lz0dec_fast_arm64
#define lz0dec_compact lz0dec_arm64
#define lz0dec_tiny lz0dec_tiny_arm64
#define lz0dec_ultra_tiny lz0dec_ultra_tiny_arm64
#elif defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)
#define lz0dec_fast lz0dec_fast_riscv64
#define lz0dec_compact lz0dec_riscv64
#define lz0dec_tiny lz0dec_tiny_riscv64
#define lz0dec_ultra_tiny lz0dec_ultra_tiny_riscv64
#else
#define lz0dec_fast lz0dec_min
#define lz0dec_compact lz0dec_min
#define lz0dec_tiny lz0dec_min
static inline int lz0dec_ultra_tiny(const unsigned char *in, unsigned char *out) {
    const unsigned char *op0 = out;
    uint32_t off = 1u, bits = 0x80u;
    #define LZ0_UT_NEXTBIT(bvar) do { \
        if ((bits & 0x7fu) == 0u) bits = ((uint32_t)(*in++) << 1) | 1u; \
        else bits <<= 1; \
        (bvar) = (bits >> 8) & 1u; \
    } while (0)
    #define LZ0_UT_ELIAS(outv, inv) do { \
        uint32_t _b, _p; \
        (outv) = 1u; \
        for (;;) { \
            LZ0_UT_NEXTBIT(_b); \
            if (_b) break; \
            LZ0_UT_NEXTBIT(_p); \
            (outv) = ((outv) << 1) | (_p ^ (uint32_t)(inv)); \
        } \
    } while (0)
    for (;;) {
        uint32_t l, b;
        LZ0_UT_ELIAS(l, 0);
        while (l--) *out++ = *in++;
        LZ0_UT_NEXTBIT(b);
        if (b) goto new_off;
        LZ0_UT_ELIAS(l, 0);
    do_match:
        { const unsigned char *s = out - off; while (l--) *out++ = *s++; }
        LZ0_UT_NEXTBIT(b);
        if (!b) continue;
    new_off:
        LZ0_UT_NEXTBIT(b);
        if (!b) break;
        LZ0_UT_ELIAS(off, 1);
        off = (off << 8) - (uint32_t)(*in++);
        l = 1u;
        while (l < 8u) { LZ0_UT_NEXTBIT(b); l = (l << 1) | b; }
        if (l == 15u) { LZ0_UT_ELIAS(l, 0); l += 8u; } else { l -= 6u; }
        goto do_match;
    }
    #undef LZ0_UT_NEXTBIT
    #undef LZ0_UT_ELIAS
    return (int)(out - op0);
}
#endif

#ifdef __cplusplus
}
#endif

#endif
