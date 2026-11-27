/*
 * lz0_simd_riscv64.S - Hand-written RISC-V Vector (RVV 1.0) kernels for the
 * LZ0 encoder.
 *
 * RV64 port of lz0_simd_x86_64.S; same contracts, bit-identical results to
 * the C references in lz0_simd.c (see lz0_simd.h).
 *
 * Vector-length agnostic: every loop is strip-mined with VSETVLI, so the code
 * runs unchanged on any VLEN (128, 256, 512, ...). Because VL never exceeds
 * the number of remaining elements, no load touches memory past the end of
 * the input (no tail handling needed, tail/mask agnostic policies).
 *
 * The rest of the library is built for RV64GC; these functions are only
 * called when the kernel reports the V extension (AT_HWCAP bit 'V'), see
 * lz0_simd_init(). ".option arch, +v" enables the vector mnemonics for this
 * file only.
 *
 * Kernels: find_u32, argmax_i32, find_off, kc_worst, count_eq, mru_batch,
 * matchlen, bin40_hits, u_row_prep, sa_scan_side.
 *
 * ABI: LP64 / LP64D. Leaf functions, no stack, only caller-saved registers
 * (a0-a7, t0-t6, v0-v31).
 */

#if defined(__ELF__) && defined(__riscv) && __riscv_xlen == 64 && !defined(_WIN32)
#if (!defined(__clang__) && __GNUC__ >= 12) || (defined(__clang__) && __clang_major__ >= 14)

        .option arch, +v
        .text

#define FUNC(name)              \
        .globl  name;           \
        .hidden name;           \
        .type   name, @function;\
        .p2align 2;             \
name:

#define ENDF(name) .size name, .-name

/* ------------------------------------------------------------------------
 * int lz0_find_u32_rvv(const uint32_t *p = a0, int n = a1, uint32_t key = a2)
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_rvv)
        sext.w  a1, a1
        li      t3, 0                       /* i */
        blez    a1, 2f
1:
        sub     t0, a1, t3                  /* remaining */
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vmseq.vx v0, v8, a2
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        blt     t3, a1, 1b
2:
        li      a0, -1
        ret
3:
        add     a0, t3, t4
        ret
ENDF(lz0_find_u32_rvv)

/* ------------------------------------------------------------------------
 * int lz0_argmax_i32_rvv(const int32_t *p = a0, int n = a1)        n >= 1
 * Pass 1: VREDMAX into v16[0]. Pass 2: tail-call lz0_find_u32_rvv.
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_rvv)
        sext.w  a1, a1
        lw      t5, 0(a0)
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.s.x v16, t5                     /* running max = p[0] */
        li      t3, 0
1:
        sub     t0, a1, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vredmax.vs v16, v8, v16
        add     t3, t3, t1
        blt     t3, a1, 1b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a2, v16
        j       lz0_find_u32_rvv            /* a0, a1 unchanged */
ENDF(lz0_argmax_i32_rvv)

/* ------------------------------------------------------------------------
 * int lz0_find_off_rvv(const lz0_match *m = a0, int n = a1, uint32_t off = a2)
 * Strided load (stride 8 bytes) reads only the offset fields.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_rvv)
        sext.w  a1, a1
        li      t3, 0
        li      t6, 8                       /* sizeof(lz0_match) */
        blez    a1, 2f
1:
        sub     t0, a1, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 3
        add     t2, a0, t2
        vlse32.v v8, (t2), t6
        vmseq.vx v0, v8, a2
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        blt     t3, a1, 1b
2:
        li      a0, -1
        ret
3:
        add     a0, t3, t4
        ret
ENDF(lz0_find_off_rvv)

/* ------------------------------------------------------------------------
 * int lz0_kc_worst_rvv(const int32_t *score = a0, const lz0_match *m = a1,
 *                      int n = a2)                            1 <= n <= 64
 *   pass 1: smin = min(score)                     (VREDMIN)
 *   pass 2: lmin = min(length[i] : score == smin) (masked VREDMINU)
 *   pass 3: first i with score == smin && length == lmin  (VMAND + VFIRST)
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_rvv)
        sext.w  a2, a2
        li      t6, 8
        addi    a3, a1, 4                   /* &m[0].length */
        /* pass 1 */
        lw      t5, 0(a0)
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.s.x v16, t5
        li      t3, 0
1:
        sub     t0, a2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vredmin.vs v16, v8, v16
        add     t3, t3, t1
        blt     t3, a2, 1b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a4, v16                     /* a4 = smin */
        /* pass 2 */
        li      t5, -1
        vmv.s.x v20, t5                     /* running min length = ~0 */
        li      t3, 0
2:
        sub     t0, a2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vmseq.vx v0, v8, a4
        slli    t2, t3, 3
        add     t2, a3, t2
        vlse32.v v12, (t2), t6
        vredminu.vs v20, v12, v20, v0.t
        add     t3, t3, t1
        blt     t3, a2, 2b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a5, v20                     /* a5 = lmin */
        /* pass 3 (a matching element always exists) */
        li      t3, 0
3:
        sub     t0, a2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        slli    t2, t3, 3
        add     t2, a3, t2
        vlse32.v v12, (t2), t6
        vmseq.vx v1, v8, a4
        vmseq.vx v2, v12, a5
        vmand.mm v0, v1, v2
        vfirst.m t4, v0
        bgez    t4, 4f
        add     t3, t3, t1
        blt     t3, a2, 3b
        li      t4, 0                       /* unreachable */
4:
        add     a0, t3, t4
        ret
ENDF(lz0_kc_worst_rvv)

/* ------------------------------------------------------------------------
 * uint32_t lz0_count_eq_rvv(const uint8_t *a = a0, const uint8_t *b = a1,
 *                           uint32_t len = a2, uint32_t step = a3)
 * Elements t = 0, step, ... < len: count = ceil(len / step) samples.
 * Unit-stride loads for step 1, strided loads otherwise; VCPOP on the mask.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_rvv)
        slli    a2, a2, 32
        srli    a2, a2, 32                  /* zero-extend len */
        slli    a3, a3, 32
        srli    a3, a3, 32                  /* zero-extend step */
        li      a4, 0                       /* count */
        beqz    a3, 9f
        add     a5, a2, a3
        addi    a5, a5, -1
        divu    a5, a5, a3                  /* samples */
        li      t0, 1
        bne     a3, t0, 5f
1:                                          /* step 1 */
        beqz    a5, 9f
        vsetvli t1, a5, e8, m8, ta, ma
        vle8.v  v8, (a0)
        vle8.v  v16, (a1)
        vmseq.vv v0, v8, v16
        vcpop.m t2, v0
        add     a4, a4, t2
        add     a0, a0, t1
        add     a1, a1, t1
        sub     a5, a5, t1
        j       1b
5:                                          /* strided */
        beqz    a5, 9f
        vsetvli t1, a5, e8, m8, ta, ma
        vlse8.v v8, (a0), a3
        vlse8.v v16, (a1), a3
        vmseq.vv v0, v8, v16
        vcpop.m t2, v0
        add     a4, a4, t2
        mul     t3, t1, a3
        add     a0, a0, t3
        add     a1, a1, t3
        sub     a5, a5, t1
        j       5b
9:
        mv      a0, a4
        ret
ENDF(lz0_count_eq_rvv)


/* ------------------------------------------------------------------------
 * void lz0_mru_batch_rvv(uint32_t mru[8] = a0, int *n_mru = a1,
 *                        const lz0_match *m = a2, int n = a3)
 * The 8-entry list stays in v8 (e32, LMUL=2, so 8 lanes for any VLEN).
 *   search : VL = n_mru, VMSEQ + VFIRST (dead lanes are outside VL)
 *   update : VSLIDE1UP builds [off, m0..m6], VMERGE under (lane <= idx)
 * ---------------------------------------------------------------------- */
FUNC(lz0_mru_batch_rvv)
        sext.w  a3, a3
        lw      a4, 0(a1)                   /* n_mru */
        vsetivli zero, 8, e32, m2, ta, ma
        vle32.v v8, (a0)
        vid.v   v12                         /* lane indices */
        li      t0, 0                       /* i */
        li      t5, 512
        li      t6, 8
1:
        bge     t0, a3, 9f
        slli    t1, t0, 3
        add     t1, a2, t1
        lwu     t2, 0(t1)                   /* offset */
        lwu     t3, 4(t1)                   /* length */
        addi    t0, t0, 1
        beqz    t2, 1b                      /* offset 0 is never recorded */
        li      t4, 4
        bgeu    t3, t4, 2f
        li      t4, 3
        bltu    t3, t4, 1b
        bgtu    t2, t5, 1b
2:
        li      t4, -1
        beqz    a4, 3f
        vsetvli zero, a4, e32, m2, ta, ma
        vmseq.vx v0, v8, t2
        vfirst.m t4, v0
3:
        bgez    t4, 4f                      /* found: idx = t4 */
        li      t4, 7
        bge     a4, t6, 4f                  /* full: drop lane 7 */
        mv      t4, a4                      /* idx = n_mru */
        addi    a4, a4, 1
4:
        vsetivli zero, 8, e32, m2, ta, ma
        vslide1up.vx v16, v8, t2            /* [off, m0, ..., m6] */
        vmsleu.vx v0, v12, t4               /* lane <= idx */
        vmerge.vvm v8, v8, v16, v0
        j       1b
9:
        vsetivli zero, 8, e32, m2, ta, ma
        vse32.v v8, (a0)
        sw      a4, 0(a1)
        ret
ENDF(lz0_mru_batch_rvv)

/* ------------------------------------------------------------------------
 * int lz0_matchlen_rvv(const uint8_t *a = a0, const uint8_t *b = a1,
 *                      int max = a2)
 * Number of identical leading bytes, in [0, max] (0 if max <= 0).
 * Strip-mined over bytes (e8, LMUL=4: 4 * VLEN / 8 bytes per strip, e.g.
 * 64 bytes at VLEN=128): VMSNE builds the mismatch mask, VFIRST returns the
 * first mismatching element (or -1). VL never exceeds the bytes left, so
 * nothing past a[max] / b[max] is loaded, on any VLEN.
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_rvv)
        sext.w  a2, a2
        li      t3, 0                       /* n */
        blez    a2, 2f
1:
        sub     t0, a2, t3                  /* remaining */
        vsetvli t1, t0, e8, m4, ta, ma
        add     t2, a0, t3
        vle8.v  v8, (t2)
        add     t2, a1, t3
        vle8.v  v16, (t2)
        vmsne.vv v0, v8, v16
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        blt     t3, a2, 1b
2:
        mv      a0, t3
        ret
3:
        add     a0, t3, t4
        ret
ENDF(lz0_matchlen_rvv)

/* ------------------------------------------------------------------------
 * uint64_t lz0_bin40_hits_rvv(const uint8_t *p = a0, uint32_t w0 = a1,
 *                             uint32_t pm = a2)
 * VLEN-agnostic strip-mined strided halfword loads (stride -4 then -8).
 * ---------------------------------------------------------------------- */
FUNC(lz0_bin40_hits_rvv)
        li      t6, 0xffff
        and     a1, a1, t6
        and     a2, a2, t6
        li      t5, 0                       /* result bitmask */

        /* Part 1: b=0..15 at p + 252, stride -4 */
        addi    t2, a0, 252
        li      t3, -4
        li      t0, 16
        li      t4, 0                       /* bit shift */
1:
        vsetvli t1, t0, e16, m4, ta, ma
        vlse16.v v8, (t2), t3
        vand.vx v8, v8, a2
        vmseq.vx v0, v8, a1
        vsetivli zero, 1, e64, m1, ta, ma
        vmv.x.s a3, v0
        li      a4, 1
        sll     a4, a4, t1
        addi    a4, a4, -1
        and     a3, a3, a4
        sll     a3, a3, t4
        or      t5, t5, a3
        slli    a5, t1, 2
        sub     t2, t2, a5
        add     t4, t4, t1
        sub     t0, t0, t1
        bnez    t0, 1b

        /* Part 2: b=16..39 at p + 184, stride -8 */
        addi    t2, a0, 184
        li      t3, -8
        li      t0, 24
2:
        vsetvli t1, t0, e16, m4, ta, ma
        vlse16.v v8, (t2), t3
        vand.vx v8, v8, a2
        vmseq.vx v0, v8, a1
        vsetivli zero, 1, e64, m1, ta, ma
        vmv.x.s a3, v0
        li      a4, 1
        sll     a4, a4, t1
        addi    a4, a4, -1
        and     a3, a3, a4
        sll     a3, a3, t4
        or      t5, t5, a3
        slli    a5, t1, 3
        sub     t2, t2, a5
        add     t4, t4, t1
        sub     t0, t0, t1
        bnez    t0, 2b

        mv      a0, t5
        ret
ENDF(lz0_bin40_hits_rvv)

/* ------------------------------------------------------------------------
 * void lz0_u_row_prep_rvv(lz0_u_prep *p = a0)
 * VLEN-agnostic RVV 1.0 strip-mined implementation for 1 <= nsi <= 64.
 * Vector groups aligned to m4 (v4,v8,v12,v16,v20,v24,v28); v0..v3 reserved
 * for masks and scalar reduction accumulators.
 * ---------------------------------------------------------------------- */
FUNC(lz0_u_row_prep_rvv)
        lw      t2, 28(a0)                  /* nsi (1..64) */
        ld      t0, 8(a0)                   /* rcost */
        lw      t5, 0(t0)                   /* init minc = rcost[0] */
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.s.x v24, t5
        li      t3, 0                       /* s */
1:
        sub     t4, t2, t3
        vsetvli t1, t4, e32, m4, ta, ma
        slli    t6, t3, 2
        add     t6, t0, t6
        vle32.v v8, (t6)
        vredmin.vs v24, v8, v24
        add     t3, t3, t1
        blt     t3, t2, 1b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a3, v24                     /* a3 = minc */

        /* Find lowest s with rcost[s] == minc */
        li      t3, 0
2:
        sub     t4, t2, t3
        vsetvli t1, t4, e32, m4, ta, ma
        slli    t6, t3, 2
        add     t6, t0, t6
        vle32.v v8, (t6)
        vmseq.vx v0, v8, a3
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        j       2b
3:
        add     t3, t3, t4                  /* b_out */
        sw      t3, 44(a0)
        ld      t5, 0(a0)                   /* rkey */
        slli    t6, t3, 2
        add     t6, t5, t6
        lw      a1, 0(t6)
        li      t6, 1
        slli    t6, t6, 31
        addi    t6, t6, -1                  /* 0x7fffffff */
        and     a1, a1, t6                  /* boff */

        /* Setup pass 2 */
        lw      a2, 32(a0)                  /* row_best */
        lw      a3, 36(a0)                  /* margin */
        lw      a4, 40(a0)                  /* exact */
        lwu     a5, 24(a0)                  /* pos */
        li      a6, 0                       /* pass_mask */
        li      a7, 0                       /* lit_cand_mask */
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.s.x v3, t6                      /* min2 = 0x7fffffff */
        li      t4, 0                       /* has_neq = 0 */
        li      t3, 0                       /* s = 0 */
4:
        sub     t0, t2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        ld      t5, 0(a0)                   /* rkey */
        slli    t0, t3, 2
        add     t5, t5, t0
        vle32.v v4, (t5)                    /* key */
        ld      t5, 8(a0)                   /* rcost */
        add     t5, t5, t0
        vle32.v v8, (t5)                    /* cost */
        li      t6, 1
        slli    t6, t6, 31
        addi    t6, t6, -1                  /* 0x7fffffff */
        vand.vx v12, v4, t6                 /* off */

        /* Track min2 across off != boff */
        vmsne.vx v0, v12, a1
        vfirst.m t5, v0
        bltz    t5, 5f
        li      t4, 1
        vredmin.vs v3, v8, v3, v0.t
5:
        /* um_non_exact = margin + 2*(exp + 1) - 1 via exact binary32 conversion */
        vadd.vi v16, v12, -1
        vsrl.vi v16, v16, 8
        vadd.vi v16, v16, 1                 /* msb in [1..2^24] */
        vfcvt.f.xu.v v24, v16
        vsrl.vi v24, v24, 23
        li      t6, -126
        vadd.vx v24, v24, t6                /* 32 - clz */
        vsll.vi v24, v24, 1
        vadd.vi v16, v24, -1                /* eb */
        vadd.vx v16, v16, a3                /* um_non_exact */

        addi    t5, a0, 72
        add     t5, t5, t0                  /* &um[s] */
        beqz    a4, 6f
        lui     t6, 0x10000
        addi    t6, t6, -1                  /* 0x0fffffff */
        vmv.v.x v24, t6
        vse32.v v24, (t5)
        vmseq.vv v1, v4, v4                 /* pass = all 1s */
        j       7f
6:
        vse32.v v16, (t5)
        vadd.vx v24, v16, a2                /* row_best + um */
        vmsge.vv v1, v24, v8                /* c <= row_best + um */
7:
        /* lit_cand = pass & (key < 0) & (off <= pos) */
        vmslt.vx v2, v4, zero
        vmand.mm v2, v2, v1
        vmsleu.vx v0, v12, a5
        vmand.mm v2, v2, v0

        /* h_lit = ((lk ^ (lk >> 16)) * 0x9e3779b1) >> 24 */
        li      t6, 1
        slli    t6, t6, 31
        vor.vx  v20, v4, t6
        vsrl.vi v24, v20, 16
        vxor.vv v20, v20, v24
        lui     t6, 0x9e378
        addi    t6, t6, -0x64f              /* 0x9e3779b1 */
        vmul.vx v20, v20, t6
        vsrl.vi v20, v20, 24
        addi    t5, a0, 328
        add     t5, t5, t0
        vse32.v v20, (t5)

        /* Extract vl-bit masks from v1 and v2 */
        vsetivli zero, 1, e64, m1, ta, ma
        li      t6, -1
        li      t0, 64
        beq     t1, t0, 8f
        li      t6, 1
        sll     t6, t6, t1
        addi    t6, t6, -1
8:
        vmv.x.s t0, v1
        and     t0, t0, t6
        sll     t0, t0, t3
        or      a6, a6, t0
        vmv.x.s t0, v2
        and     t0, t0, t6
        sll     t0, t0, t3
        or      a7, a7, t0

        add     t3, t3, t1
        blt     t3, t2, 4b

        sd      a6, 56(a0)                  /* pass_mask */
        sd      a7, 64(a0)                  /* lit_cand_mask */
        beqz    t4, 10f
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a2, v3                      /* min2 */
        li      t6, 1
        slli    t6, t6, 31
        addi    t6, t6, -1                  /* 0x7fffffff */
        li      t3, 0
9:
        sub     t0, t2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        ld      t5, 0(a0)
        slli    t0, t3, 2
        add     t5, t5, t0
        vle32.v v4, (t5)
        ld      t5, 8(a0)
        add     t5, t5, t0
        vle32.v v8, (t5)
        vand.vx v12, v4, t6
        vmsne.vx v0, v12, a1                /* off != boff */
        vmseq.vx v1, v8, a2                 /* cost == min2 */
        vmand.mm v0, v0, v1
        vfirst.m t4, v0
        bgez    t4, 11f
        add     t3, t3, t1
        j       9b
11:
        add     t3, t3, t4
        sw      t3, 48(a0)
        ret
10:
        li      t0, -1
        sw      t0, 48(a0)
        ret
ENDF(lz0_u_row_prep_rvv)

/* ------------------------------------------------------------------------
 * int lz0_sa_scan_side_rvv(lz0_sa_scan_args *a = a0) -> returns nhits
 * VLEN-agnostic RVV 1.0 implementation using strided loads & vcompress.
 * ---------------------------------------------------------------------- */
FUNC(lz0_sa_scan_side_rvv)
        lwu     a1, 16(a0)                  /* pos */
        lwu     a2, 20(a0)                  /* n */
        lw      a3, 24(a0)                  /* rem count */
        li      t4, 0                       /* nhits */
        blez    a3, 9f
        ld      a4, 0(a0)                   /* cur sa_ptr */
        ld      a5, 8(a0)                   /* in */
        lw      a6, 28(a0)                  /* dir (+1 / -1) */
        slli    a6, a6, 2                   /* byte stride (+4 / -4) */
        lhu     a7, 32(a0)                  /* pref16 */
        lbu     t3, 35(a0)                  /* in_l2 */
1:
        vsetvli t1, a3, e32, m4, ta, ma
        addi    t0, t1, -1
        mul     t0, t0, a6
        add     t0, a4, t0
        lwu     t2, 0(t0)                   /* src_far (zero-ext u32) */
        addi    t5, t2, 1
        bgeu    t5, a2, 5f                  /* src_far + 1 >= n -> tail */
        add     t5, a5, t2
        lbu     t6, 0(t5)
        lbu     t0, 1(t5)
        slli    t0, t0, 8
        or      t6, t6, t0
        bne     t6, a7, 5f

        vlse32.v v8, (a4), a6               /* v8 = src[0..vl-1] */
        bnez    t3, 3f

        /* !in_l2: verify ge3_ok && endpoint byte +2 == b2 */
        lbu     t0, 37(a0)
        beqz    t0, 5f
        addi    t5, t2, 2
        bgeu    t5, a2, 5f
        add     t5, a5, t2
        lbu     t6, 2(t5)
        lbu     t0, 34(a0)
        bne     t6, t0, 5f

        vmsltu.vx v0, v8, a1                /* src < pos */
        vcpop.m t2, v0
        beqz    t2, 4f
        vid.v   v12
        ld      t0, 0(a0)                   /* base_sa */
        sub     t0, a4, t0
        bgez    t0, 2f
        neg     t0, t0
2:
        srli    t0, t0, 2                   /* d_base */
        vadd.vx v12, v12, t0
        li      t6, 0x80
        vor.vx  v12, v12, t6                /* 0x80 | d_idx */
        j       20f

3:
        /* in_l2 */
        lbu     t0, 36(a0)                  /* stop_l2 */
        bnez    t0, 8f
        vmsltu.vx v0, v8, a1                /* src < pos */
        vrsub.vx v16, v8, a1                /* pos - src */
        lwu     t0, 40(a0)                  /* l2_cut */
        vmsltu.vx v1, v16, t0
        vmand.mm v0, v0, v1
        vcpop.m t2, v0
        beqz    t2, 4f
        vid.v   v12
        ld      t0, 0(a0)
        sub     t0, a4, t0
        bgez    t0, 21f
        neg     t0, t0
21:
        srli    t0, t0, 2
        vadd.vx v12, v12, t0

20:
        vcompress.vm v24, v8, v0
        vcompress.vm v28, v12, v0
        ld      t5, 48(a0)                  /* out_src */
        slli    t6, t4, 2
        add     t5, t5, t6
        ld      t6, 56(a0)                  /* out_k */
        add     t6, t6, t4
        add     t4, t4, t2
        vsetvli zero, t2, e32, m4, ta, ma
        vse32.v v24, (t5)
        vsetvli zero, t2, e16, m2, ta, ma
        vnsrl.wi v24, v28, 0
        vsetvli zero, t2, e8, m1, ta, ma
        vnsrl.wi v28, v24, 0
        vse8.v  v28, (t6)

4:
        mul     t0, t1, a6
        add     a4, a4, t0
        sub     a3, a3, t1
        bgtz    a3, 1b
        j       8f

5:
        /* Scalar tail */
        blez    a3, 8f
        ld      t5, 48(a0)                  /* out_src */
        ld      t6, 56(a0)                  /* out_k */
6:
        lwu     t2, 0(a4)                   /* src */
        addi    t0, t2, 1
        bgeu    t0, a2, 8f
        add     t0, a5, t2
        lbu     t1, 0(t0)
        lbu     t0, 1(t0)
        slli    t0, t0, 8
        or      t0, t0, t1
        bne     t0, a7, 8f
        bnez    t3, 62f
        lbu     t0, 37(a0)                  /* ge3_ok */
        beqz    t0, 61f
        addi    t0, t2, 2
        bgeu    t0, a2, 61f
        add     t0, a5, t2
        lbu     t0, 2(t0)
        lbu     t1, 34(a0)                  /* b2 */
        bne     t0, t1, 61f
        bgeu    t2, a1, 63f                 /* src < pos? */
        slli    t0, t4, 2
        add     t0, t5, t0
        sw      t2, 0(t0)
        ld      t1, 0(a0)
        sub     t0, a4, t1
        bgez    t0, 60f
        neg     t0, t0
60:
        srli    t0, t0, 2
        ori     t0, t0, 0x80
        add     t1, t6, t4
        sb      t0, 0(t1)
        addi    t4, t4, 1
        j       63f
61:
        li      t3, 1
62:
        lbu     t0, 36(a0)                  /* stop_l2 */
        bnez    t0, 8f
        bgeu    t2, a1, 63f                 /* src < pos? */
        sub     t0, a1, t2
        lwu     t1, 40(a0)                  /* l2_cut */
        bgeu    t0, t1, 63f
        slli    t0, t4, 2
        add     t0, t5, t0
        sw      t2, 0(t0)
        ld      t1, 0(a0)
        sub     t0, a4, t1
        bgez    t0, 64f
        neg     t0, t0
64:
        srli    t0, t0, 2
        add     t1, t6, t4
        sb      t0, 0(t1)
        addi    t4, t4, 1
63:
        add     a4, a4, a6
        addi    a3, a3, -1
        bgtz    a3, 6b
8:
        sb      t3, 35(a0)
9:
        sw      t4, 44(a0)
        mv      a0, t4
        ret
ENDF(lz0_sa_scan_side_rvv)

#endif /* GCC >= 12 || Clang >= 14 */
#if defined(__ELF__)
        .section .note.GNU-stack, "", @progbits
#endif

#endif /* __ELF__ && __riscv && __riscv_xlen == 64 && !_WIN32 */
