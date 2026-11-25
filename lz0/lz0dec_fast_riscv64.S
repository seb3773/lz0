#if defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)

/*
 * int lz0dec_fast_riscv64(const unsigned char *in, int in_size,
 *                         unsigned char *out, int out_max);
 *
 * Dual-engine FAST/SAFE LZ0 decoder for RISC-V 64-bit (RV64GC / optional Zbb).
 * Leaf function (no calls, no frame pointer); saves s0..s4 once at entry.
 *
 * FAST engine (while in_ptr <= in_end - 16):
 *   - Loads a 64-bit MSB-first bit window via WINDOW (Zbb rev8 or SWAR byte-swap).
 *   - Decodes Elias-gamma codes (n <= 8, and n = 9..15 for far offsets/long runs)
 *     via the 256-byte combined control/unzip lookup table .Ltab (zero serial loops).
 *   - Commits consumed bits to in_ptr (COMMIT) before raw literal/LSB reads.
 *   - Dispatches 16-byte unrolled copies behind one out_end - 16 bounds check.
 *
 * SAFE engine (tail / huge codes >= 2^16):
 *   - Sentinel bit reader in t0 (unread bits MSB-aligned + sentinel bit `1`,
 *     s4 == 0x8000000000000000 when empty) with exact bounds-checked copies.
 *
 * Register map:
 *   a0  in_ptr           a1  in_end           a2  out_start        a3  out_end
 *   a4  out_ptr          a5  last_offset      a6  in_end - 16      a7  out_end - 16
 *   t0  FAST leftover (top k bits at 63..64-k) / SAFE [k bits][1 sentinel]
 *   t1  FAST segment bit cursor q (7 - k)
 *   t2  FAST 64-bit bit window (MSB-first)
 *   t3  &.Ltab           t4..t6  scratch
 *   s0  0x00ff00ff00ff00ff  s1  0x0000ffff0000ffff
 *   s2  0xAAAA800000000000  s3  0xAAAAAAAA00000000  s4  0x8000000000000000
 */

/* ======================= SAFE engine macros ======================= */

/* Reads next bit into \reg (0 or 1); jumps to .Lfail on EOF. Clobbers t5. */
.macro SAFE_GETBIT reg
    bne   t0, s4, 7701\@f
    bgeu  a0, a1, .Lfail
    lbu   t0, 0(a0)
    addi  a0, a0, 1
    slli  t0, t0, 56
    srli  t5, s4, 8
    or    t0, t0, t5
7701\@:
    srli  \reg, t0, 63
    slli  t0, t0, 1
.endm

/* Tests next bit: branches to \lbl1 if 1, falls through if 0; refills cleanly. */
.macro SAFE_BR1 lbl1
    bne   t0, s4, 7702\@f
    bgeu  a0, a1, .Lfail
    lbu   t0, 0(a0)
    addi  a0, a0, 1
    slli  t0, t0, 56
    srli  t5, s4, 8
    or    t0, t0, t5
7702\@:
    mv    t5, t0
    slli  t0, t0, 1
    bltz  t5, \lbl1
.endm

/* Tests next bit: branches to \lbl0 if 0, falls through if 1; refills cleanly. */
.macro SAFE_BR0 lbl0
    bne   t0, s4, 7703\@f
    bgeu  a0, a1, .Lfail
    lbu   t0, 0(a0)
    addi  a0, a0, 1
    slli  t0, t0, 56
    srli  t5, s4, 8
    or    t0, t0, t5
7703\@:
    mv    t5, t0
    slli  t0, t0, 1
    bgez  t5, \lbl0
.endm

.macro ELIAS_SLOW reg, inv
    li    \reg, 1
7704\@:
    SAFE_BR1 7705\@f
    srli  t5, \reg, 29
    bnez  t5, .Lfail
    SAFE_GETBIT t6
  .if \inv
    xori  t6, t6, 1
  .endif
    slli  \reg, \reg, 1
    add   \reg, \reg, t6
    j     7704\@b
7705\@:
.endm

/* ======================= FAST engine macros ======================= */

/* t2 <- leftover bits ++ big-endian 64b from a0; preserves t4. */
.macro WINDOW
    xori  t5, t1, 7             /* k = 7 - q (leftover bit count) */
    ld    t2, 0(a0)
#if defined(__riscv_zbb) && (__riscv_zbb > 0)
    rev8  t2, t2
#else
    srli  t6, t2, 8
    and   t6, t6, s0
    and   t2, t2, s0
    slli  t2, t2, 8
    or    t2, t2, t6
    srli  t6, t2, 16
    and   t6, t6, s1
    and   t2, t2, s1
    slli  t2, t2, 16
    or    t2, t2, t6
    srli  t6, t2, 32
    slli  t2, t2, 32
    or    t2, t2, t6
#endif
    srl   t2, t2, t5
    or    t2, t2, t0
.endm

/* Advance a0 += q/8; keep leftover bits in t0, update t1 = q & 7; preserves t4. */
.macro COMMIT
    srli  t5, t1, 3
    add   a0, a0, t5
    andi  t1, t1, 7
    xori  t5, t1, 7
    li    t6, -1
    srl   t6, t6, t5
    not   t6, t6
    and   t0, t2, t6
.endm

/* Called after COMMIT; adds SAFE sentinel bit at 56 + q. Preserves t4. */
.macro TO_SAFE
    addi  t5, t1, 56
    li    t6, 1
    sll   t6, t6, t5
    or    t0, t0, t6
.endm

/* Transition SAFE t0 -> FAST (t0 leftover, t1 q). Clobbers t5, t6. */
.macro TO_FAST
#if defined(__riscv_zbb) && (__riscv_zbb > 0)
    ctz   t5, t0
    addi  t1, t5, -56
    li    t6, 1
    sll   t6, t6, t5
    xor   t0, t0, t6
#else
    neg   t5, t0
    and   t5, t0, t5
    xor   t0, t0, t5
    srli  t5, t5, 56
    li    t1, 0
7706\@:
    andi  t6, t5, 1
    bnez  t6, 7707\@f
    srli  t5, t5, 1
    addi  t1, t1, 1
    j     7706\@b
7707\@:
#endif
.endm

/* Branch-free Elias gamma (n <= 8) from top of t2 -> \reg, updates t2/t1.
   n > 8 -> \fb with t2/t1 untouched. \reg may alias t4. Clobbers t5, t6. */
.macro ELIAS_FAST reg, inv, fb
    and   t5, t2, s2
    beqz  t5, \fb
#if defined(__riscv_zbb) && (__riscv_zbb > 0)
    clz   \reg, t5              /* 2n */
    srli  t5, t2, 56
    srli  t6, t2, 48
    andi  t6, t6, 0xff
    add   t5, t3, t5
    add   t6, t3, t6
    lbu   t5, 0(t5)
    lbu   t6, 0(t6)
    andi  t5, t5, 0x0f
    andi  t6, t6, 0x0f
    slli  t5, t5, 4
    or    t5, t5, t6
  .if \inv
    xori  t5, t5, 0x1ff
  .else
    ori   t5, t5, 0x100
  .endif
    addi  t6, \reg, 1
    sll   t2, t2, t6
    add   t1, t1, t6
    srli  \reg, \reg, 1
    sll   t5, t5, \reg
    srli  \reg, t5, 8
#else
    srli  t5, t2, 56            /* b0 */
    srli  t6, t2, 48
    andi  t6, t6, 0xff          /* b1 */
    add   t5, t3, t5
    add   t6, t3, t6
    lbu   t5, 0(t5)             /* (n0 << 4) | p0 */
    lbu   t6, 0(t6)             /* (n1 << 4) | p1 */
    srli  \reg, t5, 4           /* n0 (0..4) */
    andi  t5, t5, 0x0f
    slli  t5, t5, 4             /* p0 << 4 */
    slli  \reg, \reg, 61
    bgez  \reg, 7708\@f         /* n0 < 4 (>95% fast path) */
    srli  \reg, t6, 4
    addi  \reg, \reg, 4         /* n = 4 + n1 */
    j     7709\@f
7708\@:
    srli  \reg, \reg, 61        /* n = n0 */
7709\@:
    andi  t6, t6, 0x0f
    or    t5, t5, t6            /* d1..d8 */
  .if \inv
    xori  t5, t5, 0x1ff
  .else
    ori   t5, t5, 0x100
  .endif
    slli  t6, \reg, 1
    addi  t6, t6, 1             /* consumed = 2n + 1 */
    sll   t2, t2, t6
    add   t1, t1, t6
    sll   t5, t5, \reg
    srli  \reg, t5, 8
#endif
.endm

/* Windowed 9 <= n <= 15 Elias decoding -> t4. n > 15 -> \fb_safe. Clobbers t5, t6. */
.macro ELIAS9_15 inv, fb_safe
    and   t5, t2, s3
    beqz  t5, \fb_safe
#if defined(__riscv_zbb) && (__riscv_zbb > 0)
    clz   t4, t5                /* 2n in t4 */
    srli  t5, t2, 56
    add   t5, t3, t5
    lbu   t5, 0(t5)
    andi  t5, t5, 0x0f
    slli  t5, t5, 12
    srli  t6, t2, 48
    andi  t6, t6, 0xff
    add   t6, t3, t6
    lbu   t6, 0(t6)
    andi  t6, t6, 0x0f
    slli  t6, t6, 8
    or    t5, t5, t6
    srli  t6, t2, 40
    andi  t6, t6, 0xff
    add   t6, t3, t6
    lbu   t6, 0(t6)
    andi  t6, t6, 0x0f
    slli  t6, t6, 4
    or    t5, t5, t6
    srli  t6, t2, 32
    andi  t6, t6, 0xff
    add   t6, t3, t6
    lbu   t6, 0(t6)
    andi  t6, t6, 0x0f
    or    t5, t5, t6
  .if \inv
    li    t6, 0x1ffff
    xor   t5, t5, t6
  .else
    li    t6, 0x10000
    or    t5, t5, t6
  .endif
    addi  t6, t4, 1
    sll   t2, t2, t6
    add   t1, t1, t6
    srli  t4, t4, 1
    sll   t5, t5, t4
    srli  t4, t5, 16
#else
    srli  t5, t2, 56
    add   t5, t3, t5
    lbu   t5, 0(t5)
    andi  t5, t5, 0x0f
    slli  t4, t5, 12
    srli  t5, t2, 48
    andi  t5, t5, 0xff
    add   t5, t3, t5
    lbu   t5, 0(t5)
    andi  t5, t5, 0x0f
    slli  t5, t5, 8
    or    t4, t4, t5
    srli  t5, t2, 40
    andi  t5, t5, 0xff
    add   t5, t3, t5
    lbu   t5, 0(t5)
    andi  t6, t5, 0x0f
    slli  t6, t6, 4
    or    t4, t4, t6
    srli  t6, t2, 32
    andi  t6, t6, 0xff
    add   t6, t3, t6
    lbu   t6, 0(t6)
    srli  t5, t5, 4
    slli  t5, t5, 61
    bgez  t5, 7710\@f
    srli  t5, t6, 4
    addi  t5, t5, 12
    j     7711\@f
7710\@:
    srli  t5, t5, 61
    addi  t5, t5, 8
7711\@:
    andi  t6, t6, 0x0f
    or    t4, t4, t6
  .if \inv
    li    t6, 0x1ffff
    xor   t4, t4, t6
  .else
    li    t6, 0x10000
    or    t4, t4, t6
  .endif
    slli  t6, t5, 1
    addi  t6, t6, 1
    sll   t2, t2, t6
    add   t1, t1, t6
    sll   t4, t4, t5
    srli  t4, t4, 16
#endif
.endm

/* Match copy: t4 = len, a5 = offset (validated). Sets a4 = out_ptr + len. */
.macro COPY_FAST fb_decide
    add   t5, a4, t4
    bgtu  t5, a7, .Ff_copy
    sub   t6, a4, a5
    li    t5, 16
    bltu  a5, t5, .Lcopy_cold_lt16
    ld    t5, 0(t6)
    ld    t6, 8(t6)
    sd    t5, 0(a4)
    sd    t6, 8(a4)
    li    t5, 16
    bgtu  t4, t5, .Lcopy_cold_gt16
    add   a4, a4, t4
.endm

/* ============================ entry ============================ */

    .text
    .globl lz0dec_fast_riscv64
#if defined(__ELF__)
    .type lz0dec_fast_riscv64, @function
#endif
    .p2align 3
lz0dec_fast_riscv64:
    beqz  a0, .Lfail_nosave
    blez  a1, .Lfail_nosave
    beqz  a2, .Lfail_nosave
    blez  a3, .Lfail_nosave

    addi  sp, sp, -48
    sd    s0, 0(sp)
    sd    s1, 8(sp)
    sd    s2, 16(sp)
    sd    s3, 24(sp)
    sd    s4, 32(sp)

    sext.w a1, a1
    sext.w a3, a3
    add   a1, a0, a1            /* in_end */
    addi  a6, a1, -16           /* in_lim = in_end - 16 */
    add   a3, a2, a3            /* out_end */
    addi  a7, a3, -16           /* out_lim = out_end - 16 */
    mv    a4, a2                /* out_ptr = out_start */
    li    a5, 1                 /* last_offset = 1 */

    li    s0, 0x00ff00ff00ff00ff
    li    s1, 0x0000ffff0000ffff
    li    s2, 0xAAAA800000000000
    li    s3, 0xAAAAAAAA00000000
    li    s4, 1
    slli  s4, s4, 63            /* 0x8000000000000000 */
    lla   t3, .Ltab

    bgtu  a0, a6, .Lstart_safe
    li    t0, 0                 /* leftover = 0 */
    li    t1, 7                 /* q = 7 */
    WINDOW
    j     .F_lit

.Lstart_safe:
    mv    t0, s4
    j     .S_literal

/* ========================= FAST ENGINE ========================= */

    .p2align 3
.F_mn:                          /* t2 at inverted-Elias offset MSB */
    ELIAS_FAST t4, 1, .Ff_new
.F_mn_have_msb:
    COMMIT
    bgeu  a0, a1, .Lfail
    lbu   t5, 0(a0)
    addi  a0, a0, 1
    slli  t4, t4, 8
    sub   a5, t4, t5            /* offset = msb*256 - lsb (>= 1) */
    sub   t5, a4, a2            /* out_pos */
    bltu  t5, a5, .Lfail
    bgtu  a0, a6, .Ff_seg_b
.F_seg_b:
    WINDOW
    srli  t4, t2, 61            /* 3-bit length code */
    slli  t2, t2, 3
    addi  t1, t1, 3
    li    t5, 7
    beq   t4, t5, .F_len_elias
    addi  t4, t4, 2
    COPY_FAST .F_decide
    j     .F_decide

    .p2align 3
.F_decide:                      /* after copy: [decision][enter] */
    bgez  t2, .F_dec_lit
    slli  t5, t2, 1
    bgez  t5, .Leof             /* decision=1, enter=0 -> EOF */
    slli  t2, t2, 2
    addi  t1, t1, 2
    j     .F_mn
.F_dec_lit:
    slli  t2, t2, 1
    addi  t1, t1, 1
.F_lit:
    bgez  t2, .F_lit_gen
    slli  t2, t2, 1             /* Elias == 1 */
    addi  t1, t1, 1
    li    t4, 1
.F_lit_have:
    COMMIT
    add   t5, a0, t4
    bgtu  t5, a6, .Ff_lit_slow
    add   t6, a4, t4
    bgtu  t6, a7, .Ff_lit_slow
    li    t5, 16
    bgtu  t4, t5, .F_lit_long
    ld    t5, 0(a0)
    ld    t6, 8(a0)
    sd    t5, 0(a4)
    sd    t6, 8(a4)
    add   a0, a0, t4
    add   a4, a4, t4

.F_after_lit:                   /* requires a0 <= in_end - 16 */
    WINDOW
    bltz  t2, .F_al_new
    slli  t5, t2, 1             /* MatchLast */
    bgez  t5, .F_ml_gen
    slli  t2, t2, 2             /* decision=0, Elias==1 */
    addi  t1, t1, 2
    li    t4, 1
.F_copy:
    COPY_FAST .F_decide
    j     .F_decide

.F_al_new:
    slli  t5, t2, 1
    bgez  t5, .Leof
    slli  t2, t2, 2
    addi  t1, t1, 2
    j     .F_mn

    .p2align 3
.F_ml_gen:
    slli  t2, t2, 1
    addi  t1, t1, 1
    ELIAS_FAST t4, 0, .Ff_ml
    j     .F_copy

.F_lit_gen:
    ELIAS_FAST t4, 0, .Ff_lit
    j     .F_lit_have

.F_len_elias:
    ELIAS_FAST t4, 0, .Ff_len
    addi  t4, t4, 8
    j     .F_copy

.F_lit_long:                    /* margins guaranteed by .F_lit_have */
    add   t6, a4, t4
1:
    ld    t4, 0(a0)
    ld    t5, 8(a0)
    sd    t4, 0(a4)
    sd    t5, 8(a4)
    addi  a0, a0, 16
    addi  a4, a4, 16
    bltu  a4, t6, 1b
    sub   t5, a4, t6
    sub   a0, a0, t5
    mv    a4, t6
    j     .F_after_lit

/* ---- 9 <= n <= 15 windowed Elias & cold copies (cold subsection) ---- */
    .subsection 1
.Ff_new:
    ELIAS9_15 1, .Ff_new_safe
    j     .F_mn_have_msb

.Ff_new_safe:
    COMMIT
    TO_SAFE
    j     .S_new_body

.Ff_ml:
    ELIAS9_15 0, .Ff_ml_safe
    j     .Ff_long_copy

.Ff_len:
    ELIAS9_15 0, .Ff_len_safe
    addi  t4, t4, 8

.Ff_long_copy:
    /* Copy via SAFE semantics then return cleanly with refreshed WINDOW if possible;
       keeps >= 57 valid bits guaranteed for any subsequent token without inflating
       FAST hot paths. */
    COMMIT
    TO_SAFE
    j     .S_copy

.Ff_len_safe:
    COMMIT
    TO_SAFE
    j     .S_new_len_elias
.Ff_ml_safe:
    COMMIT
    TO_SAFE
    j     .S_match_last
.Ff_lit:
    COMMIT
    TO_SAFE
    j     .S_literal
.Ff_copy:
    COMMIT
    TO_SAFE
    j     .S_copy
.Ff_lit_slow:                   /* already committed */
    TO_SAFE
    j     .S_lit_have_len
.Ff_seg_b:                      /* already committed */
    TO_SAFE
    j     .S_new_len3

/* Shared FAST cold copy helpers */
.Lcopy_cold_gt16:               /* offset >= 16, len > 16 */
    sub   t6, a4, a5            /* reload src (COPY_FAST clobbered t6 with data) */
    add   t5, a4, t4            /* final out_ptr */
    addi  t6, t6, 16
    addi  a4, a4, 16
1:
    ld    t4, 0(t6)
    sd    t4, 0(a4)
    ld    t4, 8(t6)
    sd    t4, 8(a4)
    addi  t6, t6, 16
    addi  a4, a4, 16
    bltu  a4, t5, 1b
    mv    a4, t5
    j     .F_decide

.Lcopy_cold_lt16:               /* 1 <= offset < 16 */
    add   t5, a4, t4            /* final out_ptr */
    li    t4, 8
    bltu  a5, t4, .Lcold_lt8
    /* 8 <= offset < 16: STLF-safe lo/hi pre-load, advance a4 by offset */
    ld    t4, 0(t6)             /* lo = out[-off..-off+7] */
    addi  t6, a4, -8
    ld    t6, 0(t6)             /* hi = out[-8..-1] */
1:
    sd    t4, 0(a4)
    add   a4, a4, a5
    sd    t6, -8(a4)
    bltu  a4, t5, 1b
    mv    a4, t5
    j     .F_decide

.Lcold_lt8:
    li    t4, 1
    beq   a5, t4, .Lcold_p1
    li    t4, 2
    beq   a5, t4, .Lcold_p2
    li    t4, 4
    beq   a5, t4, .Lcold_p4
    li    t4, 3
    beq   a5, t4, .Lcold_p3
    /* offset = 5, 6, 7: mask low p bytes, replicate once, step by a5 */
    ld    t4, 0(t6)
    slli  t6, a5, 3
    neg   t6, t6
    sll   t4, t4, t6
    srl   t4, t4, t6
    neg   t6, t6
    sll   t6, t4, t6
    or    t4, t4, t6
1:
    sd    t4, 0(a4)
    add   a4, a4, a5
    bltu  a4, t5, 1b
    mv    a4, t5
    j     .F_decide

.Lcold_p3:
    ld    t4, 0(t6)
    slli  t4, t4, 40
    srli  t4, t4, 40
    slli  t6, t4, 24
    or    t4, t4, t6
    slli  t6, t4, 48
    or    t4, t4, t6
1:
    sd    t4, 0(a4)
    addi  a4, a4, 6
    bltu  a4, t5, 1b
    mv    a4, t5
    j     .F_decide

.Lcold_p1:
    lbu   t4, 0(t6)
    li    t6, 0x0101010101010101
    mul   t4, t4, t6
    j     .Lcold_16loop

.Lcold_p2:
    lhu   t4, 0(t6)
    slli  t6, t4, 16
    or    t4, t4, t6
    slli  t6, t4, 32
    or    t4, t4, t6
    j     .Lcold_16loop

.Lcold_p4:
    lwu   t4, 0(t6)
    slli  t6, t4, 32
    or    t4, t4, t6

.Lcold_16loop:
1:
    sd    t4, 0(a4)
    sd    t4, 8(a4)
    addi  a4, a4, 16
    bltu  a4, t5, 1b
    mv    a4, t5
    j     .F_decide
    .subsection 0

/* ========================= SAFE ENGINE ========================= */

    .p2align 3
.S_after_lit:
    bleu  a0, a6, .S_to_fast_al
    SAFE_BR1 .S_new_enter
.S_match_last:
    ELIAS_SLOW t4, 0
    j     .S_copy

.S_new_enter:
    SAFE_BR0 .Leof
.S_new_body:
    ELIAS_SLOW t4, 1
    bgeu  a0, a1, .Lfail
    lbu   t5, 0(a0)
    addi  a0, a0, 1
    slli  t4, t4, 8
    sub   a5, t4, t5
    sub   t5, a4, a2
    bltu  t5, a5, .Lfail
    bleu  a0, a6, .S_to_fast_sb
.S_new_len3:
    SAFE_GETBIT t4
    SAFE_GETBIT t6
    slli  t4, t4, 1
    add   t4, t4, t6
    SAFE_GETBIT t6
    slli  t4, t4, 1
    add   t4, t4, t6
    li    t5, 7
    bgeu  t4, t5, .S_new_len_elias
    addi  t4, t4, 2
    j     .S_copy
.S_new_len_elias:
    ELIAS_SLOW t4, 0
    addi  t4, t4, 8

/* Exact match copy: t4 = len (>= 1), a5 = offset (validated). */
.S_copy:
    sub   t5, a3, a4
    bgtu  t4, t5, .Lfail
    add   t5, a4, t4            /* final out_ptr */
    li    t6, 1
    beq   a5, t6, .S_fill1
    li    t6, 8
    bltu  a5, t6, .S_copy_bytes_set
    bltu  t4, t6, .S_copy_bytes_set
    sub   t6, a4, a5            /* src */
    addi  t5, t5, -8            /* limit = final_out - 8 */
1:
    bgeu  a4, t5, 2f
    ld    t4, 0(t6)
    sd    t4, 0(a4)
    addi  t6, t6, 8
    addi  a4, a4, 8
    j     1b
2:
    addi  t5, t5, 8             /* restore final_out */
    sub   t4, t5, a5            /* src_end = final_out - off */
    ld    t4, -8(t4)
    sd    t4, -8(t5)
    mv    a4, t5
    j     .S_decide

.S_fill1:
    li    t6, 8
    bltu  t4, t6, .S_copy_bytes_set
    addi  t6, a4, -1
    lbu   t4, 0(t6)
    li    t6, 0x0101010101010101
    mul   t4, t4, t6
    addi  t5, t5, -8
1:
    bgeu  a4, t5, 2f
    sd    t4, 0(a4)
    addi  a4, a4, 8
    j     1b
2:
    addi  t5, t5, 8
    sd    t4, -8(t5)
    mv    a4, t5
    j     .S_decide

.S_copy_bytes_set:
    sub   t6, a4, a5            /* src */
.S_copy_bytes:
    lbu   t4, 0(t6)
    sb    t4, 0(a4)
    addi  t6, t6, 1
    addi  a4, a4, 1
    bltu  a4, t5, .S_copy_bytes
.S_decide:
    SAFE_BR1 .S_new_enter
.S_literal:
    ELIAS_SLOW t4, 0
/* Exact literal copy: t4 = len (>= 1). */
.S_lit_have_len:
    sub   t5, a3, a4
    bgtu  t4, t5, .Lfail
    sub   t5, a1, a0
    bgtu  t4, t5, .Lfail
    add   t5, a4, t4            /* final out_ptr */
    li    t6, 16
    bltu  t4, t6, 3f
    addi  t5, t5, -16           /* limit = final_out - 16 */
1:
    bgeu  a4, t5, 2f
    ld    t4, 0(a0)
    sd    t4, 0(a4)
    ld    t4, 8(a0)
    sd    t4, 8(a4)
    addi  a0, a0, 16
    addi  a4, a4, 16
    j     1b
2:                              /* overlapping exact 16B tail (1..16 bytes left) */
    addi  t5, t5, 16
    sub   t6, t5, a4
    add   a0, a0, t6
    ld    t4, -16(a0)
    sd    t4, -16(t5)
    ld    t4, -8(a0)
    sd    t4, -8(t5)
    mv    a4, t5
    j     .S_after_lit
3:
    lbu   t4, 0(a0)
    sb    t4, 0(a4)
    addi  a0, a0, 1
    addi  a4, a4, 1
    bltu  a4, t5, 3b
    j     .S_after_lit

    .p2align 3
.S_to_fast_al:
    TO_FAST
    WINDOW
    j     .F_after_lit
.S_to_fast_sb:
    TO_FAST
    j     .F_seg_b

.Leof:
    sub   a0, a4, a2
    ld    s0, 0(sp)
    ld    s1, 8(sp)
    ld    s2, 16(sp)
    ld    s3, 24(sp)
    ld    s4, 32(sp)
    addi  sp, sp, 48
    ret

.Lfail:
    ld    s0, 0(sp)
    ld    s1, 8(sp)
    ld    s2, 16(sp)
    ld    s3, 24(sp)
    ld    s4, 32(sp)
    addi  sp, sp, 48
.Lfail_nosave:
    li    a0, -1
    ret

#if defined(__ELF__)
    .size lz0dec_fast_riscv64, .-lz0dec_fast_riscv64
#endif

    .section .rodata
    .p2align 6
.Ltab:
    /* bits 7..4: control-stop nibble index 0..3 or 4 if (idx & 0xAA) == 0;
       bits 3..0: unzipped payload bits 6,4,2,0 (d1..d4). */
    .byte 0x40,0x41,0x30,0x31,0x42,0x43,0x32,0x33,0x20,0x21,0x20,0x21,0x22,0x23,0x22,0x23
    .byte 0x44,0x45,0x34,0x35,0x46,0x47,0x36,0x37,0x24,0x25,0x24,0x25,0x26,0x27,0x26,0x27
    .byte 0x10,0x11,0x10,0x11,0x12,0x13,0x12,0x13,0x10,0x11,0x10,0x11,0x12,0x13,0x12,0x13
    .byte 0x14,0x15,0x14,0x15,0x16,0x17,0x16,0x17,0x14,0x15,0x14,0x15,0x16,0x17,0x16,0x17
    .byte 0x48,0x49,0x38,0x39,0x4a,0x4b,0x3a,0x3b,0x28,0x29,0x28,0x29,0x2a,0x2b,0x2a,0x2b
    .byte 0x4c,0x4d,0x3c,0x3d,0x4e,0x4f,0x3e,0x3f,0x2c,0x2d,0x2c,0x2d,0x2e,0x2f,0x2e,0x2f
    .byte 0x18,0x19,0x18,0x19,0x1a,0x1b,0x1a,0x1b,0x18,0x19,0x18,0x19,0x1a,0x1b,0x1a,0x1b
    .byte 0x1c,0x1d,0x1c,0x1d,0x1e,0x1f,0x1e,0x1f,0x1c,0x1d,0x1c,0x1d,0x1e,0x1f,0x1e,0x1f
    .byte 0x00,0x01,0x00,0x01,0x02,0x03,0x02,0x03,0x00,0x01,0x00,0x01,0x02,0x03,0x02,0x03
    .byte 0x04,0x05,0x04,0x05,0x06,0x07,0x06,0x07,0x04,0x05,0x04,0x05,0x06,0x07,0x06,0x07
    .byte 0x00,0x01,0x00,0x01,0x02,0x03,0x02,0x03,0x00,0x01,0x00,0x01,0x02,0x03,0x02,0x03
    .byte 0x04,0x05,0x04,0x05,0x06,0x07,0x06,0x07,0x04,0x05,0x04,0x05,0x06,0x07,0x06,0x07
    .byte 0x08,0x09,0x08,0x09,0x0a,0x0b,0x0a,0x0b,0x08,0x09,0x08,0x09,0x0a,0x0b,0x0a,0x0b
    .byte 0x0c,0x0d,0x0c,0x0d,0x0e,0x0f,0x0e,0x0f,0x0c,0x0d,0x0c,0x0d,0x0e,0x0f,0x0e,0x0f
    .byte 0x08,0x09,0x08,0x09,0x0a,0x0b,0x0a,0x0b,0x08,0x09,0x08,0x09,0x0a,0x0b,0x0a,0x0b
    .byte 0x0c,0x0d,0x0c,0x0d,0x0e,0x0f,0x0e,0x0f,0x0c,0x0d,0x0c,0x0d,0x0e,0x0f,0x0e,0x0f

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif

#endif /* __riscv && __riscv_xlen == 64 && !_WIN32 */
