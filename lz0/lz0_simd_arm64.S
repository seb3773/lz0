/*
 * lz0_simd_arm64.S - Hand-written AdvSIMD (NEON) kernels for the LZ0 encoder.
 *
 * AArch64 port of lz0_simd_x86_64.S; same contracts, bit-identical results
 * to the C references in lz0_simd.c (see lz0_simd.h).
 *
 * ABI: AAPCS64. Leaf functions, no stack frame. Only caller-saved registers
 * are used (x0-x17, v0-v7, v16-v31); v8-v15 are never touched. No memory is
 * read outside [p, p + n): tails use overlapping in-bounds loads or scalar
 * code. AdvSIMD is architecturally mandatory on AArch64, so no run-time
 * check is required.
 *
 * Kernels: find_u32, argmax_i32, find_off, kc_worst (incl. n == 32 fast path),
 * count_eq, mru_batch, matchlen, bin40_hits, u_row_prep, sa_scan_side.
 *
 * First-set-lane extraction: comparison results (all-ones / zero lanes) are
 * narrowed to one byte or halfword per lane, moved to a GPR, then
 * RBIT + CLZ gives the bit position of the lowest matching lane.
 */

#if (defined(__ELF__) || defined(__APPLE__)) && defined(__aarch64__) && !defined(_WIN32)

        .text

#if defined(__APPLE__)
.macro FUNC name
        .globl  _\name
        .private_extern _\name
        .p2align 4
_\name:
.Lbody_\name:
.endm
.macro ENDF name
.endm
#else
.macro FUNC name
        .globl  \name
        .hidden \name
        .type   \name, %function
        .p2align 4
\name:
.Lbody_\name:
.endm
.macro ENDF name
        .size   \name, .-\name
.endm
#endif

/* ------------------------------------------------------------------------
 * int lz0_find_u32_neon(const uint32_t *p = x0, int n = w1, uint32_t key = w2)
 * ---------------------------------------------------------------------- */
FUNC lz0_find_u32_neon
        sxtw    x1, w1
        cmp     x1, #4
        b.lt    .Lfu_small
        dup     v0.4s, w2
        mov     x3, #0                      /* i */
        sub     x4, x1, #8                  /* last start of an 8-block */
.Lfu_loop8:
        cmp     x3, x4
        b.gt    .Lfu_4
        add     x5, x0, x3, lsl #2
        ldp     q1, q2, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v2.4s, v2.4s, v0.4s
        uzp1    v3.8h, v1.8h, v2.8h         /* 8 lanes -> 8 halfwords */
        xtn     v3.8b, v3.8h                /* -> 8 bytes */
        fmov    x6, d3
        cbnz    x6, .Lfu_hit8
        add     x3, x3, #8
        b       .Lfu_loop8
.Lfu_4:                                     /* 0..7 left */
        sub     x4, x1, #4                  /* n - 4 (>= 0) */
        cmp     x3, x4
        b.gt    .Lfu_tail
        add     x5, x0, x3, lsl #2
        ldr     q1, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v3.4h, v1.4s
        fmov    x6, d3
        cbnz    x6, .Lfu_hit4
        add     x3, x3, #4
.Lfu_tail:                                  /* 0..3 left: overlap at n - 4 */
        cmp     x3, x1
        b.ge    .Lfu_none
        mov     x3, x4
        add     x5, x0, x3, lsl #2
        ldr     q1, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v3.4h, v1.4s
        fmov    x6, d3
        cbnz    x6, .Lfu_hit4
.Lfu_none:
        mov     w0, #-1
        ret
.Lfu_hit8:                                  /* byte lanes */
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #3
        ret
.Lfu_hit4:                                  /* halfword lanes */
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #4
        ret
.Lfu_small:                                 /* n < 4: scalar */
        mov     x3, #0
.Lfu_sloop:
        cmp     x3, x1
        b.ge    .Lfu_none
        ldr     w5, [x0, x3, lsl #2]
        cmp     w5, w2
        b.eq    .Lfu_sret
        add     x3, x3, #1
        b       .Lfu_sloop
.Lfu_sret:
        mov     w0, w3
        ret
ENDF lz0_find_u32_neon

/* ------------------------------------------------------------------------
 * int lz0_argmax_i32_neon(const int32_t *p = x0, int n = w1)      n >= 1
 * ---------------------------------------------------------------------- */
FUNC lz0_argmax_i32_neon
        sxtw    x1, w1
        cmp     x1, #4
        b.lt    .Lam_small
        ldr     q0, [x0]
        mov     v1.16b, v0.16b
        mov     x3, #4
        sub     x4, x1, #8
.Lam_loop8:
        cmp     x3, x4
        b.gt    .Lam_4
        add     x5, x0, x3, lsl #2
        ldp     q2, q3, [x5]
        smax    v0.4s, v0.4s, v2.4s
        smax    v1.4s, v1.4s, v3.4s
        add     x3, x3, #8
        b       .Lam_loop8
.Lam_4:
        sub     x4, x1, #4
        cmp     x3, x4
        b.gt    .Lam_tail
        add     x5, x0, x3, lsl #2
        ldr     q2, [x5]
        smax    v0.4s, v0.4s, v2.4s
.Lam_tail:
        add     x5, x0, x4, lsl #2          /* last 4, may overlap */
        ldr     q2, [x5]
        smax    v1.4s, v1.4s, v2.4s
        smax    v0.4s, v0.4s, v1.4s
        smaxv   s0, v0.4s
        fmov    w2, s0
        b       .Lbody_lz0_find_u32_neon    /* x0, w1 unchanged; finds it */
.Lam_small:                                 /* n in 1..3 */
        ldr     w2, [x0]
        mov     w6, #0
        mov     x3, #1
.Lam_sloop:
        cmp     x3, x1
        b.ge    .Lam_sret
        ldr     w5, [x0, x3, lsl #2]
        cmp     w5, w2
        b.le    .Lam_snext                  /* signed, strict max */
        mov     w2, w5
        mov     w6, w3
.Lam_snext:
        add     x3, x3, #1
        b       .Lam_sloop
.Lam_sret:
        mov     w0, w6
        ret
ENDF lz0_argmax_i32_neon

/* ------------------------------------------------------------------------
 * int lz0_find_off_neon(const lz0_match *m = x0, int n = w1, uint32_t off = w2)
 * LD2 de-interleaves {offset, length} pairs: v1 = 4 offsets, v2 = 4 lengths.
 * ---------------------------------------------------------------------- */
FUNC lz0_find_off_neon
        sxtw    x1, w1
        cmp     x1, #4
        b.lt    .Lfo_small
        dup     v0.4s, w2
        mov     x3, #0
        sub     x4, x1, #8
.Lfo_loop8:
        cmp     x3, x4
        b.gt    .Lfo_4
        add     x5, x0, x3, lsl #3
        ld2     {v1.4s, v2.4s}, [x5], #32
        ld2     {v3.4s, v4.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v3.4s, v3.4s, v0.4s
        uzp1    v5.8h, v1.8h, v3.8h
        xtn     v5.8b, v5.8h
        fmov    x6, d5
        cbnz    x6, .Lfo_hit8
        add     x3, x3, #8
        b       .Lfo_loop8
.Lfo_4:
        sub     x4, x1, #4
        cmp     x3, x4
        b.gt    .Lfo_tail
        add     x5, x0, x3, lsl #3
        ld2     {v1.4s, v2.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v5.4h, v1.4s
        fmov    x6, d5
        cbnz    x6, .Lfo_hit4
        add     x3, x3, #4
.Lfo_tail:
        cmp     x3, x1
        b.ge    .Lfo_none
        mov     x3, x4
        add     x5, x0, x3, lsl #3
        ld2     {v1.4s, v2.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v5.4h, v1.4s
        fmov    x6, d5
        cbnz    x6, .Lfo_hit4
.Lfo_none:
        mov     w0, #-1
        ret
.Lfo_hit8:
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #3
        ret
.Lfo_hit4:
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #4
        ret
.Lfo_small:
        mov     x3, #0
.Lfo_sloop:
        cmp     x3, x1
        b.ge    .Lfo_none
        add     x5, x0, x3, lsl #3
        ldr     w5, [x5]
        cmp     w5, w2
        b.eq    .Lfo_sret
        add     x3, x3, #1
        b       .Lfo_sloop
.Lfo_sret:
        mov     w0, w3
        ret
ENDF lz0_find_off_neon

/* ------------------------------------------------------------------------
 * int lz0_kc_worst_neon(const int32_t *score = x0, const lz0_match *m = x1,
 *                       int n = w2)                           1 <= n <= 64
 * Fully vectorised lexicographic minimum:
 *   pass 1: smin  = min(score)
 *   pass 2: lmin  = min(length[i] : score[i] == smin)   (others -> 0xffffffff)
 *   pass 3: first i with score[i] == smin && length[i] == lmin
 * Overlapping tails are harmless for min (idempotent) and for pass 3 (the
 * re-checked lanes are known not to match).
 * ---------------------------------------------------------------------- */
FUNC lz0_kc_worst_neon
        sxtw    x2, w2
        cmp     x2, #32
        b.eq    .Lkw_n32
        cmp     x2, #4
        b.lt    .Lkw_small
        sub     x4, x2, #4                  /* x4 = n - 4 */
        /* pass 1 */
        ldr     q0, [x0]
        mov     x3, #4
.Lkw_p1:
        cmp     x3, x4
        b.gt    .Lkw_p1t
        add     x9, x0, x3, lsl #2
        ldr     q1, [x9]
        smin    v0.4s, v0.4s, v1.4s
        add     x3, x3, #4
        b       .Lkw_p1
.Lkw_p1t:
        add     x9, x0, x4, lsl #2
        ldr     q1, [x9]
        smin    v0.4s, v0.4s, v1.4s
        sminv   s0, v0.4s
        dup     v0.4s, v0.s[0]              /* v0 = smin x4 */
        /* pass 2 */
        movi    v7.2d, #0xffffffffffffffff  /* running min length */
        mov     x3, #0
.Lkw_p2:
        cmp     x3, x4
        b.gt    .Lkw_p2t
        add     x9, x0, x3, lsl #2
        ldr     q1, [x9]
        add     x5, x1, x3, lsl #3
        ld2     {v2.4s, v3.4s}, [x5]        /* v3 = lengths */
        cmeq    v1.4s, v1.4s, v0.4s
        orn     v3.16b, v3.16b, v1.16b      /* non-min lanes -> all ones */
        umin    v7.4s, v7.4s, v3.4s
        add     x3, x3, #4
        b       .Lkw_p2
.Lkw_p2t:
        cmp     x3, x2
        b.ge    .Lkw_p2r
        add     x9, x0, x4, lsl #2
        ldr     q1, [x9]
        add     x5, x1, x4, lsl #3
        ld2     {v2.4s, v3.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        orn     v3.16b, v3.16b, v1.16b
        umin    v7.4s, v7.4s, v3.4s
.Lkw_p2r:
        uminv   s7, v7.4s
        dup     v7.4s, v7.s[0]              /* v7 = lmin x4 */
        /* pass 3 */
        mov     x3, #0
.Lkw_p3:
        cmp     x3, x4
        b.gt    .Lkw_p3t
.Lkw_p3b:
        add     x9, x0, x3, lsl #2
        ldr     q1, [x9]
        add     x5, x1, x3, lsl #3
        ld2     {v2.4s, v3.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v3.4s, v3.4s, v7.4s
        and     v1.16b, v1.16b, v3.16b
        xtn     v1.4h, v1.4s
        fmov    x6, d1
        cbnz    x6, .Lkw_hit
        add     x3, x3, #4
        b       .Lkw_p3
.Lkw_p3t:                                   /* a match must exist in the tail */
        mov     x3, x4
        b       .Lkw_p3b
.Lkw_hit:
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #4
        ret
.Lkw_small:                                 /* n in 1..3: scalar */
        ldr     w8, [x0]                    /* best score */
        ldr     w9, [x1, #4]                /* best length */
        mov     w6, #0
        mov     x3, #1
.Lkw_sloop:
        cmp     x3, x2
        b.ge    .Lkw_sret
        ldr     w10, [x0, x3, lsl #2]
        add     x5, x1, x3, lsl #3
        ldr     w11, [x5, #4]
        cmp     w10, w8
        b.lt    .Lkw_take                   /* signed score < best */
        b.ne    .Lkw_snext
        cmp     w11, w9
        b.hs    .Lkw_snext                  /* unsigned length >= best */
.Lkw_take:
        mov     w8, w10
        mov     w9, w11
        mov     w6, w3
.Lkw_snext:
        add     x3, x3, #1
        b       .Lkw_sloop
.Lkw_sret:
        mov     w0, w6
        ret
.Lkw_n32:
        ldp     q1, q2, [x0]
        ldp     q3, q4, [x0, #32]
        ldp     q5, q6, [x0, #64]
        ldp     q7, q16, [x0, #96]
        smin    v0.4s, v1.4s, v2.4s
        smin    v17.4s, v3.4s, v4.4s
        smin    v18.4s, v5.4s, v6.4s
        smin    v19.4s, v7.4s, v16.4s
        smin    v0.4s, v0.4s, v17.4s
        smin    v18.4s, v18.4s, v19.4s
        smin    v0.4s, v0.4s, v18.4s
        sminv   s0, v0.4s
        dup     v0.4s, v0.s[0]              /* smin */
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v2.4s, v2.4s, v0.4s
        cmeq    v3.4s, v3.4s, v0.4s
        cmeq    v4.4s, v4.4s, v0.4s
        cmeq    v5.4s, v5.4s, v0.4s
        cmeq    v6.4s, v6.4s, v0.4s
        cmeq    v7.4s, v7.4s, v0.4s
        cmeq    v16.4s, v16.4s, v0.4s
        uzp1    v1.8h, v1.8h, v2.8h
        uzp1    v3.8h, v3.8h, v4.8h
        uzp1    v1.16b, v1.16b, v3.16b
        uzp1    v5.8h, v5.8h, v6.8h
        uzp1    v7.8h, v7.8h, v16.8h
        uzp1    v5.16b, v5.16b, v7.16b
#if defined(__APPLE__)
        adrp    x8, .Lkw_bitw@PAGE
        add     x8, x8, .Lkw_bitw@PAGEOFF
#else
        adrp    x8, .Lkw_bitw
        add     x8, x8, :lo12:.Lkw_bitw
#endif
        ldr     q17, [x8]                   /* {1,2,4,8,16,32,64,128} x 2 */
        and     v1.16b, v1.16b, v17.16b
        and     v5.16b, v5.16b, v17.16b
        addp    v1.16b, v1.16b, v5.16b
        addp    v1.16b, v1.16b, v1.16b
        addp    v1.16b, v1.16b, v1.16b
        fmov    w10, s1
        rbit    w11, w10
        clz     w0, w11                     /* idx0 (zero-extends into x0) */
        sub     w11, w10, #1
        and     w10, w10, w11
        cbz     w10, .Lkw_done32
        ldr     x9, [x1, x0, lsl #3]
        lsr     x9, x9, #32                 /* best_len = m[idx0].length */
.Lkw32_tie:
        rbit    w11, w10
        clz     w12, w11
        sub     w11, w10, #1
        and     w10, w10, w11
        ldr     x13, [x1, x12, lsl #3]
        lsr     x13, x13, #32
        cmp     w13, w9
        csel    w9, w13, w9, lo
        csel    w0, w12, w0, lo
        cbnz    w10, .Lkw32_tie
.Lkw_done32:
        ret
ENDF lz0_kc_worst_neon

/* ------------------------------------------------------------------------
 * uint32_t lz0_count_eq_neon(const uint8_t *a = x0, const uint8_t *b = x1,
 *                            uint32_t len = w2, uint32_t step = w3)
 * Per 16 bytes: CMEQ, AND (lane-select 1/0 bytes), UADALP into 8 x u16.
 * The u16 accumulator is folded into 4 x u32 every 8192 blocks (max lane
 * value 2 * 8192 = 16384), so any len < 2^32 is safe. Blocks start at
 * multiples of 16, hence of the step; the remainder is scalar.
 * ---------------------------------------------------------------------- */
FUNC lz0_count_eq_neon
        mov     w2, w2                      /* zero-extend len */
        mov     w3, w3                      /* zero-extend step */
        cmp     w3, #2
        b.eq    .Lce_m2
        cmp     w3, #4
        b.eq    .Lce_m4
        movi    v4.16b, #1                  /* step 1 */
        b       .Lce_go
.Lce_m2:
        movi    v4.8h, #1                   /* bytes 01 00 ... */
        b       .Lce_go
.Lce_m4:
        movi    v4.4s, #1                   /* bytes 01 00 00 00 ... */
.Lce_go:
        movi    v6.2d, #0                   /* u32 x4 total */
        mov     x4, #0                      /* t */
.Lce_chunk:
        add     x5, x4, #16
        cmp     x5, x2
        b.hi    .Lce_fold                   /* fewer than 16 bytes left */
        movi    v5.2d, #0                   /* u16 x8 partial */
        mov     x7, #8192                   /* blocks per chunk */
.Lce_loop:
        ldr     q0, [x0, x4]
        ldr     q1, [x1, x4]
        cmeq    v0.16b, v0.16b, v1.16b
        and     v0.16b, v0.16b, v4.16b
        uadalp  v5.8h, v0.16b
        add     x4, x4, #16
        subs    x7, x7, #1
        b.eq    .Lce_flush
        add     x5, x4, #16
        cmp     x5, x2
        b.ls    .Lce_loop
.Lce_flush:
        uadalp  v6.4s, v5.8h
        b       .Lce_chunk
.Lce_fold:
        addv    s6, v6.4s
        fmov    w8, s6                      /* count */
.Lce_tail:                                  /* t is a multiple of 16 */
        cmp     x4, x2
        b.hs    .Lce_ret
        ldrb    w5, [x0, x4]
        ldrb    w6, [x1, x4]
        cmp     w5, w6
        cinc    w8, w8, eq
        add     x4, x4, x3
        b       .Lce_tail
.Lce_ret:
        mov     w0, w8
        ret
ENDF lz0_count_eq_neon


/* ------------------------------------------------------------------------
 * void lz0_mru_batch_neon(uint32_t mru[8] = x0, int *n_mru = x1,
 *                         const lz0_match *m = x2, int n = w3)
 * Same algorithm as lz0_mru_batch_sse2: the list stays in v0/v1, EXT builds
 * the shifted list, CMHI against lane indices builds the blend masks.
 * ---------------------------------------------------------------------- */
FUNC lz0_mru_batch_neon
        ldp     q0, q1, [x0]
        ldr     w4, [x1]                    /* n_mru */
#if defined(__APPLE__)
        adrp    x5, .Lmru_lanes@PAGE
        add     x5, x5, .Lmru_lanes@PAGEOFF
#else
        adrp    x5, .Lmru_lanes
        add     x5, x5, :lo12:.Lmru_lanes
#endif
        ldp     q6, q7, [x5]                /* {0..3}, {4..7} */
        sxtw    x3, w3
        mov     x6, #0                      /* i */
.Lmb_loop:
        cmp     x6, x3
        b.ge    .Lmb_done
        add     x7, x2, x6, lsl #3
        ldp     w8, w9, [x7]                /* offset, length */
        add     x6, x6, #1
        cbz     w8, .Lmb_loop               /* offset 0 is never recorded */
        cmp     w9, #4
        b.hs    .Lmb_take
        cmp     w9, #3
        b.lo    .Lmb_loop
        cmp     w8, #512
        b.hi    .Lmb_loop
.Lmb_take:
        dup     v2.4s, w8                   /* off x4 */
        dup     v3.4s, w4                   /* n_mru x4 */
        cmhi    v4.4s, v3.4s, v6.4s         /* live lanes 0-3 */
        cmhi    v5.4s, v3.4s, v7.4s         /* live lanes 4-7 */
        cmeq    v16.4s, v0.4s, v2.4s
        cmeq    v17.4s, v1.4s, v2.4s
        and     v16.16b, v16.16b, v4.16b
        and     v17.16b, v17.16b, v5.16b
        uzp1    v16.8h, v16.8h, v17.8h
        xtn     v16.8b, v16.8h
        fmov    x10, d16
        cbz     x10, .Lmb_insert
        rbit    x10, x10
        clz     x10, x10
        lsr     w10, w10, #3                /* idx = found lane */
        b       .Lmb_shift
.Lmb_insert:
        mov     w10, #7
        cmp     w4, #8
        b.ge    .Lmb_shift
        mov     w10, w4                     /* idx = n_mru */
        add     w4, w4, #1
.Lmb_shift:
        dup     v3.4s, w10
        cmhs    v4.4s, v3.4s, v6.4s         /* M0: idx >= lane */
        cmhs    v5.4s, v3.4s, v7.4s         /* M1 */
        ext     v16.16b, v2.16b, v0.16b, #12 /* [off, m0, m1, m2] */
        ext     v17.16b, v0.16b, v1.16b, #12 /* [m3, m4, m5, m6] */
        bit     v0.16b, v16.16b, v4.16b
        bit     v1.16b, v17.16b, v5.16b
        b       .Lmb_loop
.Lmb_done:
        stp     q0, q1, [x0]
        str     w4, [x1]
        ret
ENDF lz0_mru_batch_neon

#if defined(__APPLE__)
        .section __TEXT,__const
#else
        .section .rodata
#endif
        .p2align 4
.Lmru_lanes:
        .word   0, 1, 2, 3, 4, 5, 6, 7
        .text

/* ------------------------------------------------------------------------
 * int lz0_matchlen_neon(const uint8_t *a = x0, const uint8_t *b = x1,
 *                       int max = w2)
 * Number of identical leading bytes, in [0, max] (0 if max <= 0).
 * 32 bytes per iteration: EOR of the two inputs (non-zero byte = mismatch),
 * ORR of both halves, UMAXP reduction to 64 bits, one CBNZ. On a hit the
 * XOR qwords are scanned in order; RBIT + CLZ of the first non-zero qword
 * gives the bit index, /8 the byte. Then one 16-byte block, then an
 * overlapping in-bounds block ending at max. max < 16: two 8-byte words
 * (the second overlapping) or a byte loop. Never reads a[max] / b[max].
 * ---------------------------------------------------------------------- */
FUNC lz0_matchlen_neon
        sxtw    x2, w2
        mov     x3, #0                      /* n */
        cmp     x2, #16
        b.lt    .Lml_lt16
        sub     x4, x2, #32                 /* last start of a 32-byte block */
.Lml_loop32:
        cmp     x3, x4
        b.gt    .Lml_16
        add     x5, x0, x3
        add     x6, x1, x3
        ldp     q0, q1, [x5]
        ldp     q2, q3, [x6]
        eor     v0.16b, v0.16b, v2.16b
        eor     v1.16b, v1.16b, v3.16b
        orr     v4.16b, v0.16b, v1.16b
        umaxp   v4.4s, v4.4s, v4.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit32
        add     x3, x3, #32
        b       .Lml_loop32
.Lml_hit32:
        umaxp   v4.4s, v0.4s, v0.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit16              /* mismatch in the first half */
        add     x3, x3, #16
        mov     v0.16b, v1.16b
.Lml_hit16:                                 /* v0 = a ^ b at n, non-zero */
        fmov    x7, d0
        cbnz    x7, .Lml_hitq
        mov     x7, v0.d[1]
        add     x3, x3, #8
.Lml_hitq:                                  /* x7 = a ^ b at n, non-zero */
        rbit    x7, x7
        clz     x7, x7
        add     x0, x3, x7, lsr #3
        ret
.Lml_16:                                    /* 0..31 left, max >= 16 */
        sub     x4, x2, #16                 /* max - 16 (>= 0) */
        cmp     x3, x4
        b.gt    .Lml_tail
        ldr     q0, [x0, x3]
        ldr     q2, [x1, x3]
        eor     v0.16b, v0.16b, v2.16b
        umaxp   v4.4s, v0.4s, v0.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit16
        add     x3, x3, #16
.Lml_tail:                                  /* 0..15 left: overlap at max - 16 */
        cmp     x3, x2
        b.ge    .Lml_full
        mov     x3, x4
        ldr     q0, [x0, x3]
        ldr     q2, [x1, x3]
        eor     v0.16b, v0.16b, v2.16b
        umaxp   v4.4s, v0.4s, v0.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit16
.Lml_full:
        mov     w0, w2
        ret
.Lml_lt16:                                  /* max < 16 */
        cmp     x2, #8
        b.lt    .Lml_bytes
        ldr     x5, [x0]
        ldr     x6, [x1]
        eor     x7, x5, x6
        cbnz    x7, .Lml_hitq
        sub     x3, x2, #8                  /* overlapping word at max - 8 */
        ldr     x5, [x0, x3]
        ldr     x6, [x1, x3]
        eor     x7, x5, x6
        cbnz    x7, .Lml_hitq
        mov     w0, w2
        ret
.Lml_bytes:                                 /* max < 8 (possibly <= 0) */
        cmp     x3, x2
        b.ge    .Lml_bret
        ldrb    w5, [x0, x3]
        ldrb    w6, [x1, x3]
        cmp     w5, w6
        b.ne    .Lml_bret
        add     x3, x3, #1
        b       .Lml_bytes
.Lbret_zero:
.Lml_bret:
        mov     w0, w3
        ret
ENDF lz0_matchlen_neon

/* ------------------------------------------------------------------------
 * uint64_t lz0_bin40_hits_neon(const uint8_t *p = x0, uint32_t w0 = w1,
 *                              uint32_t pm = w2)
 * Bitmask b in [0, 40) of positions matching (ld16(p+256-off) & pm) == w0.
 * Uses only caller-saved registers x0..x7 and v0..v7, v16..v20.
 * ---------------------------------------------------------------------- */
FUNC lz0_bin40_hits_neon
        uxth    w1, w1
        uxth    w2, w2
        dup     v0.8h, w1                   /* vw0 */
        dup     v1.8h, w2                   /* vpm */
#if defined(__APPLE__)
        adrp    x5, .Lb40_weights@PAGE
        add     x5, x5, .Lb40_weights@PAGEOFF
#else
        adrp    x5, .Lb40_weights
        add     x5, x5, :lo12:.Lb40_weights
#endif
        ldr     q2, [x5]                    /* {128..1, 128..1} */

        /* G0: b=7..0 from p+224..255 */
        ldp     q16, q17, [x0, #224]
        uzp1    v6.8h, v16.8h, v17.8h
        and     v6.16b, v6.16b, v1.16b
        cmeq    v6.8h, v6.8h, v0.8h

        /* G1: b=15..8 from p+192..223 */
        ldp     q16, q17, [x0, #192]
        uzp1    v7.8h, v16.8h, v17.8h
        and     v7.16b, v7.16b, v1.16b
        cmeq    v7.8h, v7.8h, v0.8h

        /* G2: b=23..16 from p+128..191 */
        ldp     q16, q17, [x0, #160]
        ldp     q18, q19, [x0, #128]
        uzp1    v16.4s, v16.4s, v17.4s
        uzp1    v18.4s, v18.4s, v19.4s
        uzp1    v16.8h, v18.8h, v16.8h
        and     v16.16b, v16.16b, v1.16b
        cmeq    v16.8h, v16.8h, v0.8h

        /* G3: b=31..24 from p+64..127 */
        ldp     q17, q18, [x0, #96]
        ldp     q19, q20, [x0, #64]
        uzp1    v17.4s, v17.4s, v18.4s
        uzp1    v19.4s, v19.4s, v20.4s
        uzp1    v17.8h, v19.8h, v17.8h
        and     v17.16b, v17.16b, v1.16b
        cmeq    v17.8h, v17.8h, v0.8h

        /* G4: b=39..32 from p+0..63 */
        ldp     q18, q19, [x0, #32]
        ldp     q20, q3, [x0, #0]
        uzp1    v18.4s, v18.4s, v19.4s
        uzp1    v20.4s, v20.4s, v3.4s
        uzp1    v18.8h, v20.8h, v18.8h
        and     v18.16b, v18.16b, v1.16b
        cmeq    v18.8h, v18.8h, v0.8h

        /* Narrow 5 groups -> bitmask bytes 0..4 via weights + addp */
        movi    v5.16b, #0
        uzp1    v3.16b, v6.16b, v7.16b
        uzp1    v4.16b, v16.16b, v17.16b
        uzp1    v5.16b, v18.16b, v5.16b
        and     v3.16b, v3.16b, v2.16b
        and     v4.16b, v4.16b, v2.16b
        and     v5.16b, v5.16b, v2.16b
        addp    v3.16b, v3.16b, v4.16b
        addp    v5.16b, v5.16b, v5.16b
        addp    v3.16b, v3.16b, v5.16b
        addp    v3.16b, v3.16b, v3.16b
        fmov    x0, d3
        ubfx    x0, x0, #0, #40
        ret
ENDF lz0_bin40_hits_neon

/* ------------------------------------------------------------------------
 * void lz0_u_row_prep_neon(lz0_u_prep *p = x0)
 * Guard-page clean for 1 <= nsi <= 64, ELF & Apple Silicon ABI safe.
 * Touches only x0..x17 and v0..v7, v16..v31.
 * ---------------------------------------------------------------------- */
FUNC lz0_u_row_prep_neon
        ldr     x1, [x0, #0]                /* rkey */
        ldr     x2, [x0, #8]                /* rcost */
        ldrsw   x3, [x0, #28]               /* nsi (1..64) */
        cmp     x3, #4
        b.lt    .Lurp_small

        /* 1. Find global min cost across 0..nsi-1 (overlapping in-bounds tail) */
        sub     x4, x3, #4                  /* nsi - 4 >= 0 */
        ldr     q0, [x2]
        mov     x5, #4
.Lurp_p1_loop:
        cmp     x5, x4
        b.gt    .Lurp_p1_tail
        add     x6, x2, x5, lsl #2
        ldr     q1, [x6]
        smin    v0.4s, v0.4s, v1.4s
        add     x5, x5, #4
        b       .Lurp_p1_loop
.Lurp_p1_tail:
        add     x6, x2, x4, lsl #2
        ldr     q1, [x6]
        smin    v0.4s, v0.4s, v1.4s
        sminv   s0, v0.4s
        dup     v0.4s, v0.s[0]

        /* 2. Find lowest index b_out with rcost[b_out] == minc */
        mov     x5, #0
.Lurp_findb:
        cmp     x5, x4
        csel    x5, x4, x5, gt
        add     x6, x2, x5, lsl #2
        ldr     q1, [x6]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v1.4h, v1.4s
        fmov    x7, d1
        cbnz    x7, .Lurp_foundb
        add     x5, x5, #4
        b       .Lurp_findb
.Lurp_foundb:
        rbit    x7, x7
        clz     x7, x7
        add     w6, w5, w7, lsr #4
        str     w6, [x0, #44]               /* b_out */
        ldr     w7, [x1, x6, lsl #2]
        and     w7, w7, #0x7fffffff
        dup     v0.4s, w7                   /* v0 = boff */

        /* 3. Load vector constants and setup main 4-wide loop */
#if defined(__APPLE__)
        adrp    x8, .Lu_neon_consts@PAGE
        add     x8, x8, .Lu_neon_consts@PAGEOFF
#else
        adrp    x8, .Lu_neon_consts
        add     x8, x8, :lo12:.Lu_neon_consts
#endif
        ldp     q3, q4, [x8]                /* q3 = 0x7fffffff, q4 = 0x9e3779b1 */
        ldp     q21, q18, [x8, #32]         /* q21 = 0x0fffffff, q18 = bitw8 */
        movi    v16.4s, #1
        movi    v17.4s, #63
        movi    v19.4s, #0x80, lsl #24      /* 0x80000000 */
        ldr     w8, [x0, #24]
        dup     v5.4s, w8                   /* pos */
        ldr     w8, [x0, #32]
        dup     v6.4s, w8                   /* row_best */
        ldr     w8, [x0, #36]
        dup     v7.4s, w8                   /* margin */
        ldr     w8, [x0, #40]
        cmp     w8, #0
        csetm   w8, ne
        dup     v20.4s, w8                  /* v20 = all-ones if exact else 0 */

        mov     v1.16b, v3.16b              /* v_min2 = 0x7fffffff */
        movi    v2.16b, #0                  /* v_any_neq = 0 */
        mov     x9, #0                      /* pass_mask */
        mov     x10, #0                     /* lit_cand_mask */
        add     x11, x0, #72                /* &um[0] */
        add     x12, x0, #328               /* &h_lit[0] */
        mov     x5, #0                      /* s */
.Lurp_main:
        cmp     x5, x4
        b.gt    .Lurp_mtail
.Lurp_mbody:
        add     x6, x1, x5, lsl #2
        ldr     q22, [x6]                   /* key */
        add     x6, x2, x5, lsl #2
        ldr     q23, [x6]                   /* cost */
        and     v24.16b, v22.16b, v3.16b    /* off = key & 0x7fffffff */

        /* um = margin + (63 - 2*clz(((off - 1) >> 8) + 1)) */
        sub     v25.4s, v24.4s, v16.4s
        ushr    v25.4s, v25.4s, #8
        add     v25.4s, v25.4s, v16.4s
        clz     v25.4s, v25.4s
        shl     v25.4s, v25.4s, #1
        sub     v25.4s, v17.4s, v25.4s
        add     v25.4s, v25.4s, v7.4s
        mov     v26.16b, v20.16b
        bsl     v26.16b, v21.16b, v25.16b
        add     x6, x11, x5, lsl #2
        str     q26, [x6]

        /* h_lit = ((lk ^ (lk >> 16)) * 0x9e3779b1) >> 24 */
        orr     v27.16b, v22.16b, v19.16b
        ushr    v28.4s, v27.4s, #16
        eor     v27.16b, v27.16b, v28.16b
        mul     v27.4s, v27.4s, v4.4s
        ushr    v27.4s, v27.4s, #24
        add     x6, x12, x5, lsl #2
        str     q27, [x6]

        /* pass & lit_cand 4-bit masks */
        add     v25.4s, v6.4s, v25.4s       /* row_best + um_non_exact */
        cmge    v25.4s, v25.4s, v23.4s      /* c <= row_best + um */
        orr     v25.16b, v25.16b, v20.16b
        cmlt    v28.4s, v22.4s, #0          /* key & 0x80000000 */
        cmhs    v29.4s, v5.4s, v24.4s       /* off <= pos (unsigned) */
        and     v28.16b, v28.16b, v25.16b
        and     v28.16b, v28.16b, v29.16b
        uzp1    v25.8h, v25.8h, v28.8h
        xtn     v25.8b, v25.8h
        and     v25.8b, v25.8b, v18.8b
        addp    v25.8b, v25.8b, v25.8b
        addp    v25.8b, v25.8b, v25.8b
        fmov    w6, s25
        ubfx    x7, x6, #0, #4
        lsl     x7, x7, x5
        orr     x9, x9, x7
        ubfx    x7, x6, #8, #4
        lsl     x7, x7, x5
        orr     x10, x10, x7

        /* track min2 for off != boff */
        cmeq    v29.4s, v24.4s, v0.4s
        mvn     v30.16b, v29.16b
        orr     v2.16b, v2.16b, v30.16b
        bsl     v29.16b, v3.16b, v23.16b
        smin    v1.4s, v1.4s, v29.4s

        cmp     x5, x4
        b.eq    .Lurp_done
        add     x5, x5, #4
        b       .Lurp_main
        .ltorg
.Lurp_mtail:
        cmp     x5, x3
        b.ge    .Lurp_done
        mov     x5, x4
        b       .Lurp_mbody

.Lurp_done:
        str     x9, [x0, #56]               /* pass_mask */
        str     x10, [x0, #64]              /* lit_cand_mask */
        umaxv   s2, v2.4s
        fmov    w6, s2
        cbz     w6, .Lurp_b2_none
        sminv   s1, v1.4s
        dup     v1.4s, v1.s[0]              /* min2 */
        mov     x5, #0
.Lurp_fb2:
        cmp     x5, x4
        csel    x5, x4, x5, gt
        add     x6, x1, x5, lsl #2
        ldr     q22, [x6]
        add     x6, x2, x5, lsl #2
        ldr     q23, [x6]
        and     v22.16b, v22.16b, v3.16b
        cmeq    v22.4s, v22.4s, v0.4s       /* off == boff */
        cmeq    v23.4s, v23.4s, v1.4s       /* cost == min2 */
        bic     v23.16b, v23.16b, v22.16b   /* cost == min2 && off != boff */
        xtn     v23.4h, v23.4s
        fmov    x7, d23
        cbnz    x7, .Lurp_found_b2
        add     x5, x5, #4
        b       .Lurp_fb2
.Lurp_found_b2:
        rbit    x7, x7
        clz     x7, x7
        add     w6, w5, w7, lsr #4
        str     w6, [x0, #48]
        ret
.Lurp_b2_none:
        mov     w6, #-1
        str     w6, [x0, #48]
        ret

.Lurp_small:                                /* 1 <= nsi <= 3: scalar */
        ldr     w4, [x2]                    /* minc */
        mov     w5, #0                      /* b */
        mov     x6, #1
.Lurp_sb:
        cmp     x6, x3
        b.ge    .Lurp_sb_done
        ldr     w7, [x2, x6, lsl #2]
        cmp     w7, w4
        csel    w4, w7, w4, lt
        csel    w5, w6, w5, lt
        add     x6, x6, #1
        b       .Lurp_sb
.Lurp_sb_done:
        str     w5, [x0, #44]               /* b_out */
        ldr     w7, [x1, x5, lsl #2]
        and     w7, w7, #0x7fffffff         /* boff */
        ldr     w8, [x0, #32]               /* row_best */
        ldr     w9, [x0, #36]               /* margin */
        ldr     w10, [x0, #40]              /* exact */
        ldr     w11, [x0, #24]              /* pos */
        mov     w12, #0x7fffffff            /* min2 */
        mov     w13, #-1                    /* b2 */
        mov     x14, #0                     /* pass */
        mov     x15, #0                     /* lit */
        mov     x6, #0                      /* s */
.Lurp_sloop:
        ldr     w4, [x1, x6, lsl #2]        /* key */
        ldr     w5, [x2, x6, lsl #2]        /* c */
        and     w16, w4, #0x7fffffff        /* off */
        cmp     w16, w7
        b.eq    1f
        cmp     w13, #0
        b.lt    2f
        cmp     w5, w12
        b.ge    1f
2:
        mov     w12, w5
        mov     w13, w6
1:
        cbz     w10, 3f
        mov     w17, #0x0fffffff
        mov     x5, #1
        b       4f
3:
        sub     w17, w16, #1
        lsr     w17, w17, #8
        add     w17, w17, #1
        clz     w17, w17
        mov     w4, #63
        sub     w17, w4, w17, lsl #1
        add     w17, w17, w9                /* um */
        add     w4, w8, w17
        cmp     w5, w4
        cset    x5, le
        ldr     w4, [x1, x6, lsl #2]        /* reload key */
4:
        add     x1, x0, #72
        str     w17, [x1, x6, lsl #2]
        ldr     x1, [x0, #0]                /* reload rkey */
        lsl     x5, x5, x6
        orr     x14, x14, x5
        tbz     w4, #31, 5f
        cmp     w16, w11
        b.hi    5f
        orr     x15, x15, x5
5:
        orr     w4, w4, #0x80000000
        eor     w4, w4, w4, lsr #16
        mov     w17, #0x79b1
        movk    w17, #0x9e37, lsl #16
        mul     w4, w4, w17
        lsr     w4, w4, #24
        add     x17, x0, #328
        str     w4, [x17, x6, lsl #2]
        add     x6, x6, #1
        cmp     x6, x3
        b.lt    .Lurp_sloop
        str     w13, [x0, #48]
        str     x14, [x0, #56]
        str     x15, [x0, #64]
        ret
ENDF lz0_u_row_prep_neon

/* ------------------------------------------------------------------------
 * int lz0_sa_scan_side_neon(lz0_sa_scan_args *a = x0) -> returns nhits
 * Guard-page clean for count <= 128, ELF & Apple Silicon ABI safe.
 * Uses only caller-saved registers x0..x17 and v0..v7, v16..v23.
 * ---------------------------------------------------------------------- */
FUNC lz0_sa_scan_side_neon
        ldr     x1, [x0, #0]                /* sa_ptr */
        ldr     x2, [x0, #8]                /* in */
        ldr     w3, [x0, #16]               /* pos */
        ldr     w4, [x0, #20]               /* n */
        ldr     w5, [x0, #24]               /* count */
        mov     w13, #0                     /* nhits */
        cmp     w5, #0
        b.le    .Lsa_ret_zero

        mov     x6, x1                      /* base_sa_ptr */
        ldr     w7, [x0, #40]               /* l2_cut */
        dup     v4.4s, w3                   /* v_pos */
        dup     v5.4s, w7                   /* v_l2_cut */
        ldr     x11, [x0, #48]              /* out_src */
        ldr     x12, [x0, #56]              /* out_k */
        ldrh    w14, [x0, #32]              /* pref16 */
        ldrb    w15, [x0, #35]              /* in_l2 */
        ldrb    w16, [x0, #37]              /* ge3_ok */
        ldrb    w17, [x0, #34]              /* b2 */
        ldr     w7, [x0, #28]               /* dir */
#if defined(__APPLE__)
        adrp    x8, .Lsa_neon_wR@PAGE
        add     x8, x8, .Lsa_neon_wR@PAGEOFF
#else
        adrp    x8, .Lsa_neon_wR
        add     x8, x8, :lo12:.Lsa_neon_wR
#endif
        cmp     w7, #0
        b.lt    .Lsa_L_init

        ldr     q6, [x8]                    /* {1, 2, 4, 8} */
.Lsa_R_4:
        cmp     w5, #4
        b.lt    .Lsa_R_tail
        ldr     w7, [x1, #12]               /* src_far = sa[3] */
        uxtw    x8, w7
        add     x8, x8, #1
        cmp     x8, x4
        b.hs    .Lsa_R_tail
        ldrh    w8, [x2, x7]
        cmp     w8, w14
        b.ne    .Lsa_R_tail

        ldr     q0, [x1]
        cmhi    v1.4s, v4.4s, v0.4s         /* src < pos */
        cbnz    w15, .Lsa_R_l2

        cbz     w16, .Lsa_R_tail
        uxtw    x8, w7
        add     x8, x8, #2
        cmp     x8, x4
        b.hs    .Lsa_R_tail
        add     x8, x2, x7
        ldrb    w8, [x8, #2]
        cmp     w8, w17
        b.ne    .Lsa_R_tail

        and     v1.16b, v1.16b, v6.16b
        addv    s1, v1.4s
        fmov    w9, s1
        cbz     w9, .Lsa_R_next4
        sub     x8, x1, x6
        lsr     x8, x8, #2                  /* base_d */
.Lsa_R_e3:
        rbit    w10, w9
        clz     w10, w10
        sub     w7, w9, #1
        and     w9, w9, w7
        ldr     w7, [x1, x10, lsl #2]
        str     w7, [x11, x13, lsl #2]
        add     w7, w8, w10
        orr     w7, w7, #0x80
        strb    w7, [x12, x13]
        add     w13, w13, #1
        cbnz    w9, .Lsa_R_e3
.Lsa_R_next4:
        add     x1, x1, #16
        sub     w5, w5, #4
        b       .Lsa_R_4

.Lsa_R_l2:
        ldrb    w8, [x0, #36]               /* stop_l2 */
        cbnz    w8, .Lsa_ret
        sub     v2.4s, v4.4s, v0.4s
        cmhi    v2.4s, v5.4s, v2.4s         /* pos - src < l2_cut */
        and     v1.16b, v1.16b, v2.16b
        and     v1.16b, v1.16b, v6.16b
        addv    s1, v1.4s
        fmov    w9, s1
        cbz     w9, .Lsa_R_next4
        sub     x8, x1, x6
        lsr     x8, x8, #2
.Lsa_R_e2:
        rbit    w10, w9
        clz     w10, w10
        sub     w7, w9, #1
        and     w9, w9, w7
        ldr     w7, [x1, x10, lsl #2]
        str     w7, [x11, x13, lsl #2]
        add     w7, w8, w10
        strb    w7, [x12, x13]
        add     w13, w13, #1
        cbnz    w9, .Lsa_R_e2
        b       .Lsa_R_next4

.Lsa_R_tail:
        cmp     w5, #0
        b.le    .Lsa_ret
.Lsa_R_tloop:
        ldr     w7, [x1]
        uxtw    x8, w7
        add     x8, x8, #1
        cmp     x8, x4
        b.hs    .Lsa_ret
        ldrh    w8, [x2, x7]
        cmp     w8, w14
        b.ne    .Lsa_ret
        cbnz    w15, .Lsa_R_t_isl2
        cbz     w16, .Lsa_R_t_gol2
        uxtw    x8, w7
        add     x8, x8, #2
        cmp     x8, x4
        b.hs    .Lsa_R_t_gol2
        add     x8, x2, x7
        ldrb    w8, [x8, #2]
        cmp     w8, w17
        b.ne    .Lsa_R_t_gol2
        cmp     w7, w3
        b.hs    .Lsa_R_tnext
        str     w7, [x11, x13, lsl #2]
        sub     x8, x1, x6
        lsr     x8, x8, #2
        orr     w8, w8, #0x80
        strb    w8, [x12, x13]
        add     w13, w13, #1
        b       .Lsa_R_tnext
.Lsa_R_t_gol2:
        mov     w15, #1
.Lsa_R_t_isl2:
        ldrb    w8, [x0, #36]
        cbnz    w8, .Lsa_ret
        cmp     w7, w3
        b.hs    .Lsa_R_tnext
        sub     w8, w3, w7
        ldr     w9, [x0, #40]               /* l2_cut */
        cmp     w8, w9
        b.hs    .Lsa_R_tnext
        str     w7, [x11, x13, lsl #2]
        sub     x8, x1, x6
        lsr     x8, x8, #2
        strb    w8, [x12, x13]
        add     w13, w13, #1
.Lsa_R_tnext:
        add     x1, x1, #4
        sub     w5, w5, #1
        cbnz    w5, .Lsa_R_tloop
        b       .Lsa_ret

.Lsa_L_init:
        ldr     q6, [x8, #16]               /* {8, 4, 2, 1} */
.Lsa_L_4:
        cmp     w5, #4
        b.lt    .Lsa_L_tail
        ldr     w7, [x1, #-12]              /* src_far = sa[-3] */
        uxtw    x8, w7
        add     x8, x8, #1
        cmp     x8, x4
        b.hs    .Lsa_L_tail
        ldrh    w8, [x2, x7]
        cmp     w8, w14
        b.ne    .Lsa_L_tail

        ldur    q0, [x1, #-12]
        cmhi    v1.4s, v4.4s, v0.4s         /* src < pos */
        cbnz    w15, .Lsa_L_l2

        cbz     w16, .Lsa_L_tail
        uxtw    x8, w7
        add     x8, x8, #2
        cmp     x8, x4
        b.hs    .Lsa_L_tail
        add     x8, x2, x7
        ldrb    w8, [x8, #2]
        cmp     w8, w17
        b.ne    .Lsa_L_tail

        and     v1.16b, v1.16b, v6.16b
        addv    s1, v1.4s
        fmov    w9, s1
        cbz     w9, .Lsa_L_next4
        sub     x8, x6, x1
        lsr     x8, x8, #2                  /* base_d */
.Lsa_L_e3:
        rbit    w10, w9
        clz     w10, w10
        sub     w7, w9, #1
        and     w9, w9, w7
        sub     x7, x1, x10, lsl #2
        ldr     w7, [x7]
        str     w7, [x11, x13, lsl #2]
        add     w7, w8, w10
        orr     w7, w7, #0x80
        strb    w7, [x12, x13]
        add     w13, w13, #1
        cbnz    w9, .Lsa_L_e3
.Lsa_L_next4:
        sub     x1, x1, #16
        sub     w5, w5, #4
        b       .Lsa_L_4

.Lsa_L_l2:
        ldrb    w8, [x0, #36]
        cbnz    w8, .Lsa_ret
        sub     v2.4s, v4.4s, v0.4s
        cmhi    v2.4s, v5.4s, v2.4s
        and     v1.16b, v1.16b, v2.16b
        and     v1.16b, v1.16b, v6.16b
        addv    s1, v1.4s
        fmov    w9, s1
        cbz     w9, .Lsa_L_next4
        sub     x8, x6, x1
        lsr     x8, x8, #2
.Lsa_L_e2:
        rbit    w10, w9
        clz     w10, w10
        sub     w7, w9, #1
        and     w9, w9, w7
        sub     x7, x1, x10, lsl #2
        ldr     w7, [x7]
        str     w7, [x11, x13, lsl #2]
        add     w7, w8, w10
        strb    w7, [x12, x13]
        add     w13, w13, #1
        cbnz    w9, .Lsa_L_e2
        b       .Lsa_L_next4

.Lsa_L_tail:
        cmp     w5, #0
        b.le    .Lsa_ret
.Lsa_L_tloop:
        ldr     w7, [x1]
        uxtw    x8, w7
        add     x8, x8, #1
        cmp     x8, x4
        b.hs    .Lsa_ret
        ldrh    w8, [x2, x7]
        cmp     w8, w14
        b.ne    .Lsa_ret
        cbnz    w15, .Lsa_L_t_isl2
        cbz     w16, .Lsa_L_t_gol2
        uxtw    x8, w7
        add     x8, x8, #2
        cmp     x8, x4
        b.hs    .Lsa_L_t_gol2
        add     x8, x2, x7
        ldrb    w8, [x8, #2]
        cmp     w8, w17
        b.ne    .Lsa_L_t_gol2
        cmp     w7, w3
        b.hs    .Lsa_L_tnext
        str     w7, [x11, x13, lsl #2]
        sub     x8, x6, x1
        lsr     x8, x8, #2
        orr     w8, w8, #0x80
        strb    w8, [x12, x13]
        add     w13, w13, #1
        b       .Lsa_L_tnext
.Lsa_L_t_gol2:
        mov     w15, #1
.Lsa_L_t_isl2:
        ldrb    w8, [x0, #36]
        cbnz    w8, .Lsa_ret
        cmp     w7, w3
        b.hs    .Lsa_L_tnext
        sub     w8, w3, w7
        ldr     w9, [x0, #40]
        cmp     w8, w9
        b.hs    .Lsa_L_tnext
        str     w7, [x11, x13, lsl #2]
        sub     x8, x6, x1
        lsr     x8, x8, #2
        strb    w8, [x12, x13]
        add     w13, w13, #1
.Lsa_L_tnext:
        sub     x1, x1, #4
        sub     w5, w5, #1
        cbnz    w5, .Lsa_L_tloop
.Lsa_ret:
        strb    w15, [x0, #35]              /* a->in_l2 */
        str     w13, [x0, #44]              /* a->nhits */
        mov     w0, w13
        ret
.Lsa_ret_zero:
        str     w13, [x0, #44]
        mov     w0, #0
        ret
ENDF lz0_sa_scan_side_neon

#if defined(__APPLE__)
        .section __TEXT,__const
#else
        .section .rodata
#endif
        .p2align 4
.Lu_neon_consts:
        .word   0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
        .word   0x9e3779b1, 0x9e3779b1, 0x9e3779b1, 0x9e3779b1
        .word   0x0fffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff
        .byte   1, 2, 4, 8, 1, 2, 4, 8, 1, 2, 4, 8, 1, 2, 4, 8
.Lsa_neon_wR:
        .word   1, 2, 4, 8
.Lsa_neon_wL:
        .word   8, 4, 2, 1
.Lkw_bitw:
        .byte   1, 2, 4, 8, 16, 32, 64, 128
        .byte   1, 2, 4, 8, 16, 32, 64, 128
.Lb40_weights:
        .byte   128, 64, 32, 16, 8, 4, 2, 1
        .byte   128, 64, 32, 16, 8, 4, 2, 1
        .text

#if defined(__ELF__)
        .section .note.GNU-stack, "", %progbits
#endif

#endif /* (__ELF__ || __APPLE__) && __aarch64__ && !_WIN32 */
