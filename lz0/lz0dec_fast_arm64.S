#if defined(__ELF__) && defined(__aarch64__) && !defined(_WIN32)

/*
 * int lz0dec_fast_arm64(const unsigned char *in, int in_size,
 *                  unsigned char *out, int out_max);
 *
 * LZ0 decoder for AArch64 (AAPCS64). Pure leaf: no stack frame, no calls,
 * only caller-saved registers x0..x17 and v0.
 *
 * Two engines share one register state:
 *
 *  FAST engine (while in_ptr <= in_end - 16):
 *    A "bit segment" is the run of control bits between two raw-byte reads.
 *    Each segment loads ONE 64-bit MSB-first window (ldr+rev+lsr+orr),
 *    decodes all fields with shifts / TBZ, and commits the bit count back
 *    to in_ptr (single ADD with LSR #3 operand) before the next raw read.
 *    Elias-gamma codes with n <= 8 payload bits decode branch-free:
 *    CLZ on (window & control-mask) gives 2n, two byte-table lookups give
 *    the payload. Offset MSB codes with 9 <= n <= 15 (offsets up to 8 MiB)
 *    also stay in the window (four lookups, .Ff_new), so far matches never
 *    fall back to the SAFE engine. Copies are 16-byte q-register wildcopies behind ONE
 *    "end <= limit-16" compare per token.
 *
 *  SAFE engine (last 16 input bytes, last 16 output bytes, length codes
 *  n > 8, offset codes n > 15):
 *    Sentinel bit reader in w8 (bits left-aligned at bit 31, followed by a
 *    1 marker): ADDS w8,w8,w8 gives the bit in C and flags exhaustion in Z.
 *    Exact, overrun-free copies.
 *
 * Every check performed by lz0dec_ref is preserved; results are bit-exact.
 *
 * Registers:
 *   x0 in_ptr     x1 in_end      x6 in_end - 16
 *   x3 out_ptr    x4 out_end     x7 out_end - 16     x2 out_start
 *   x5 last_offset
 *   x8  fast: leftover bits of current byte, left-aligned (rest zero)
 *       safe: w8 = [bits][1][0..] left-aligned at bit 31
 *   x9  fast: bit counter q (segment start = 7 - leftover bits)
 *   x10 fast: bit window
 *   x11 0xAAAA800000000000 (Elias control-bit mask)
 *   x12 table base (.Lunzip, then .Ls_tab at +256)
 *   x13..x17, v0 : scratch
 */

/* ======================= SAFE engine macros ======================= */

.macro GETBIT                   /* one bit -> C flag */
    adds w8, w8, w8
    b.ne 7771f
    cmp x0, x1
    b.hs .Lfail
    ldrb w13, [x0], #1
    lsl w13, w13, #24
    orr w8, w13, #0x800000
    adds w8, w8, w8
7771:
.endm

.macro ELIAS_SLOW reg, inv      /* interleaved Elias gamma -> \reg */
    mov \reg, #1
7772:
    GETBIT
    b.cs 7773f
    lsr x13, \reg, #29
    cbnz x13, .Lfail
    GETBIT
  .if \inv
    cset w13, cc
    add \reg, x13, \reg, lsl #1
  .else
    adc \reg, \reg, \reg
  .endif
    b 7772b
7773:
.endm

/* ======================= FAST engine macros ======================= */

.macro WINDOW                   /* x10 <- leftover bits ++ bytes at x0 */
    eor w13, w9, #7             /* k = leftover bit count */
    ldr x10, [x0]
    rev x10, x10
    lsr x10, x10, x13
    orr x10, x10, x8
.endm

.macro COMMIT                   /* x0 += q/8 ; x8 = top k' bits ; x9 = q&7 */
    add x0, x0, x9, lsr #3
    and x9, x9, #7
    eor w13, w9, #7             /* k' */
    mov x17, #-1
    lsr x17, x17, x13
    bic x8, x10, x17
.endm

.macro TO_SAFE                  /* after COMMIT */
    lsr x8, x8, #32
    add w13, w9, #24
    mov w17, #1
    lsl w17, w17, w13
    orr w8, w8, w17
.endm

.macro TO_FAST
    rbit w13, w8
    clz w13, w13                /* sentinel position s = 24 + j */
    sub x9, x13, #24
    mov w17, #1
    lsl w17, w17, w13
    bic w8, w8, w17
    lsl x8, x8, #32
.endm

/* Branch-free Elias gamma (n <= 8) from top of x10 -> \reg, consumed.
   n > 8 -> \fb with x10/x9 untouched. Clobbers x13 x15 x17. */
.macro ELIAS_FAST reg, inv, fb
    ands x13, x10, x11
    b.eq \fb
    clz x13, x13                /* 2n */
    ubfx x17, x10, #56, #8      /* c0 d1 c1 d2 c2 d3 c3 d4 */
    ubfx x15, x10, #48, #8      /* c4 d5 c5 d6 c6 d7 c7 d8 */
    ldrb w17, [x12, x17]
    ldrb w15, [x12, x15]
    orr w15, w15, w17, lsl #4   /* d1..d8 */
  .if \inv
    eor w15, w15, #0x1ff        /* invert payload, leading 1 */
  .else
    orr w15, w15, #0x100        /* leading 1 */
  .endif
    add x17, x13, #1            /* consumed = 2n + 1 */
    lsl x10, x10, x17
    add x9, x9, x17
    lsr x13, x13, #1            /* n */
    lsl w15, w15, w13
    lsr \reg, x15, #8           /* 1 d1 .. dn */
.endm

/* Match copy: x14 = len, x5 = offset (validated). Done -> fall through or
   jump to \cont. Within 16 bytes of out_end -> \fb, nothing written. */
.macro COPY_FAST fb, cont
    add x13, x3, x14
    cmp x13, x7
    b.hi \fb
    sub x15, x3, x5
    cmp x5, #16
    b.lo 8881f
    ldr q0, [x15]
    str q0, [x3]
    cmp x14, #16
    b.hi 8882f
    mov x3, x13
  .subsection 1
8882:                           /* offset >= 16, len > 16 */
    add x15, x15, #16
    add x17, x3, #16
1:
    ldr q0, [x15], #16
    str q0, [x17], #16
    cmp x17, x13
    b.lo 1b
    mov x3, x13
    b \cont
8881:
    cmp x5, #8
    b.lo 2f
    /* offset 8..15: lo = out[-off..-off+7] and hi = out[-8..-1] together
       hold the whole period (off <= 16) and both loads end before out.
       Each step stores lo at q and hi at q+off-8, then q += off. */
    ldr x16, [x15]              /* lo */
    sub x17, x3, #8
    ldr x17, [x17]              /* hi */
    sub x14, x5, #8             /* off - 8 */
1:
    str x16, [x3]
    str x17, [x3, x14]
    add x3, x3, x5
    cmp x3, x13
    b.lo 1b
    mov x3, x13
    b \cont
2:
    cmp x5, #1
    b.ne 3f
    ld1r {v0.16b}, [x15]        /* offset 1: byte broadcast */
1:
    str q0, [x3], #16
    cmp x3, x13
    b.lo 1b
    mov x3, x13
    b \cont
3:  /* offset p = 2..7: build y = p-periodic doubleword in a register.
       Mask low p bytes with shl/lsr by 64-8p, then duplicate via shifts.
       Store y every s = p*floor(8/p) bytes: no byte loop, one load. */
    lsl x14, x5, #3             /* 8p */
    mov x16, #64
    sub x16, x16, x14           /* 64 - 8p */
    ldr x17, [x15]
    lsl x17, x17, x16
    lsr x17, x17, x16
    lsl x15, x17, x14
    orr x17, x17, x15
    lsl x15, x15, x14
    orr x17, x17, x15
    lsl x15, x15, x14
    orr x17, x17, x15
    add x16, x12, #256          /* .Ls_tab at .Lunzip + 256 */
    ldrb w16, [x16, x5]
1:
    str x17, [x3]
    add x3, x3, x16
    cmp x3, x13
    b.lo 1b
    mov x3, x13
    b \cont
  .subsection 0
.endm

/* ============================ entry ============================ */

    .text
    .globl lz0dec_fast_arm64
#if defined(__ELF__)
    .type lz0dec_fast_arm64, %function
#endif
    .p2align 5
lz0dec_fast_arm64:
    cbz x0, .Lfail
    cmp w1, #0
    b.le .Lfail
    cbz x2, .Lfail
    cmp w3, #0
    b.le .Lfail

    add x1, x0, w1, sxtw        /* in_end  */
    sub x6, x1, #16
    add x4, x2, w3, sxtw        /* out_end */
    sub x7, x4, #16
    mov x3, x2
    mov x5, #1                  /* last_offset = 1 */
    movz x11, #0xAAAA, lsl #48
    movk x11, #0x8000, lsl #32
    adrp x12, .Lunzip
    add x12, x12, :lo12:.Lunzip
    mov w8, #0x80000000         /* empty bit buffer */

    cmp x0, x6                  /* first token is always a LiteralRun */
    b.hi .S_literal
    TO_FAST
    WINDOW
    b .F_lit

/* ========================= FAST ENGINE ========================= */

    .p2align 5
.F_mn:                          /* x10 at the inverted-Elias offset MSB */
    ELIAS_FAST x16, 1, .Ff_new
.F_mn_have_msb:
    COMMIT
    cmp x0, x1                  /* provably in bounds; kept as defence */
    b.hs .Lfail
    ldrb w13, [x0], #1
    lsl x16, x16, #8
    sub x5, x16, x13            /* offset = msb*256 - lsb (>= 1) */
    add x13, x2, x5
    cmp x13, x3                 /* offset <= out_pos */
    b.hi .Lfail
    cmp x0, x6
    b.hi .Ff_seg_b
.F_seg_b:
    WINDOW
    lsr x14, x10, #61           /* 3-bit length code */
    cmp x14, #7
    b.eq .F_len_elias
    add x14, x14, #2
    COPY_FAST .Ff_copy3, .F_mn_decide
.F_mn_decide:                   /* bits: [len3][decision][enter] */
    tbz x10, #60, .F_mn_lit
    tbz x10, #59, .Leof
    lsl x10, x10, #5
    add x9, x9, #5
    b .F_mn
.F_mn_lit:
    lsl x10, x10, #4
    add x9, x9, #4
    b .F_lit

    .p2align 4
.F_decide:                      /* after a copy: [decision][enter] */
    tbz x10, #63, .F_dec_lit
    tbz x10, #62, .Leof
    lsl x10, x10, #2
    add x9, x9, #2
    b .F_mn
.F_dec_lit:
    lsl x10, x10, #1
    add x9, x9, #1
.F_lit:
    tbz x10, #63, .F_lit_gen
    lsl x10, x10, #1            /* Elias == 1 */
    add x9, x9, #1
    mov x16, #1
.F_lit_have:
    COMMIT
    add x13, x0, x16
    cmp x13, x6
    b.hi .Ff_lit_slow
    add x14, x3, x16
    cmp x14, x7
    b.hi .Ff_lit_slow
    ldr q0, [x0]
    str q0, [x3]
    cmp x16, #16
    b.hi .F_lit_long
    mov x0, x13
    mov x3, x14

.F_after_lit:                   /* requires x0 <= in_end - 16 */
    WINDOW
    tbnz x10, #63, .F_al_new
    /* MatchLast; offset needs no check: last_offset <= out_pos always */
    tbz x10, #62, .F_ml_gen
    lsl x10, x10, #2            /* decision 0 + Elias == 1 */
    add x9, x9, #2
    mov x14, #1
.F_copy:
    COPY_FAST .Ff_copy, .F_decide
    b .F_decide

.F_al_new:
    tbz x10, #62, .Leof
    lsl x10, x10, #2
    add x9, x9, #2
    b .F_mn

    .p2align 4
.F_ml_gen:
    lsl x10, x10, #1
    add x9, x9, #1
    ELIAS_FAST x14, 0, .Ff_ml
    b .F_copy

.F_lit_gen:
    ELIAS_FAST x16, 0, .Ff_lit
    b .F_lit_have

.F_len_elias:
    lsl x10, x10, #3
    add x9, x9, #3
    ELIAS_FAST x14, 0, .Ff_len
    add x14, x14, #8
    b .F_copy

.F_lit_long:                    /* first 16 done; margins guaranteed */
    mov x17, #16
1:
    ldr q0, [x0, x17]
    str q0, [x3, x17]
    add x17, x17, #16
    cmp x17, x16
    b.lo 1b
    mov x0, x13
    mov x3, x14
    b .F_after_lit

/* ---- offset MSB with 9 <= n <= 15 payload bits: still windowed ----
   Far offsets (> 128 KiB) are common on multi-MiB inputs. Decoding them in
   the window (4 table lookups) avoids a FAST->SAFE->FAST round trip and a
   bit-serial loop. The window always holds >= 42 unread bits here (at most
   22 bits are consumed before an offset code), and bits shifted in below
   the valid region are zero, so a stop bit found by CLZ is always genuine.
   x14 (length) is dead here and used as scratch. */
.Ff_new:
    movz x13, #0xAAAA, lsl #48  /* control bits c0..c15 */
    movk x13, #0xAAAA, lsl #32
    ands x13, x10, x13
    b.eq .Ff_new_safe           /* n > 15 */
    clz x13, x13                /* 2n */
    ubfx x17, x10, #56, #8
    ubfx x15, x10, #48, #8
    ubfx x14, x10, #40, #8
    ubfx x16, x10, #32, #8
    ldrb w17, [x12, x17]        /* d1..d4   */
    ldrb w15, [x12, x15]        /* d5..d8   */
    ldrb w14, [x12, x14]        /* d9..d12  */
    ldrb w16, [x12, x16]        /* d13..d16 */
    orr w16, w16, w14, lsl #4
    orr w16, w16, w15, lsl #8
    orr w16, w16, w17, lsl #12
    eor w16, w16, #0x1ffff      /* invert payload, leading 1 */
    add x17, x13, #1            /* consumed = 2n + 1 */
    lsl x10, x10, x17
    add x9, x9, x17
    lsr x13, x13, #1            /* n */
    lsl w16, w16, w13
    lsr x16, x16, #16           /* 1 d1 .. dn */
    b .F_mn_have_msb

/* ---- fast -> safe hand-offs: commit window, continue bit by bit ---- */
.Ff_new_safe:
    COMMIT
    TO_SAFE
    b .S_new_body
.Ff_len:
    COMMIT
    TO_SAFE
    b .S_new_len_elias
.Ff_ml:
    COMMIT
    TO_SAFE
    b .S_match_last
.Ff_lit:
    COMMIT
    TO_SAFE
    b .S_literal
.Ff_copy3:
    lsl x10, x10, #3            /* consume the 3-bit length code */
    add x9, x9, #3
.Ff_copy:
    COMMIT
    TO_SAFE
    b .S_copy
.Ff_lit_slow:                   /* already committed */
    TO_SAFE
    mov x14, x16
    b .S_lit_have_len
.Ff_seg_b:                      /* already committed */
    TO_SAFE
    b .S_new_len3

/* ========================= SAFE ENGINE ========================= */

    .p2align 4
.S_after_lit:
    cmp x0, x6
    b.ls .S_to_fast_al
    GETBIT
    b.cs .S_new_enter
.S_match_last:
    ELIAS_SLOW x14, 0
    b .S_copy

.S_new_enter:
    GETBIT
    b.cc .Leof
.S_new_body:
    ELIAS_SLOW x16, 1
    cmp x0, x1
    b.hs .Lfail
    ldrb w13, [x0], #1
    lsl x16, x16, #8
    sub x5, x16, x13
    add x13, x2, x5
    cmp x13, x3
    b.hi .Lfail
    cmp x0, x6
    b.ls .S_to_fast_sb
.S_new_len3:
    mov w14, #0
    GETBIT
    adc w14, w14, w14
    GETBIT
    adc w14, w14, w14
    GETBIT
    adc w14, w14, w14
    cmp w14, #7
    b.hs .S_new_len_elias
    add x14, x14, #2
    b .S_copy
.S_new_len_elias:
    ELIAS_SLOW x14, 0
    add x14, x14, #8

/* exact match copy: x14 = len (>= 1), x5 = offset (validated) */
.S_copy:
    sub x13, x4, x3
    cmp x14, x13
    b.hi .Lfail
    sub x15, x3, x5             /* src */
    add x13, x3, x14            /* end */
    cmp x5, #1
    b.eq .S_fill1
    cmp x5, #8
    b.lo .S_copy_bytes
    cmp x14, #8
    b.lo .S_copy_bytes
    sub x16, x13, #8
1:
    cmp x3, x16
    b.hs 2f
    ldr x17, [x15], #8
    str x17, [x3], #8
    b 1b
2:                              /* overlapping exact 8-byte tail */
    sub x17, x13, x5
    ldur x17, [x17, #-8]
    stur x17, [x13, #-8]
    mov x3, x13
    b .S_decide
.S_fill1:
    cmp x14, #8
    b.lo .S_copy_bytes
    ldrb w17, [x15]
    mov x16, #0x0101010101010101
    mul x17, x17, x16
    sub x16, x13, #8
1:
    cmp x3, x16
    b.hs 2f
    str x17, [x3], #8
    b 1b
2:
    stur x17, [x13, #-8]
    mov x3, x13
    b .S_decide
.S_copy_bytes:
    ldrb w17, [x15], #1
    strb w17, [x3], #1
    cmp x3, x13
    b.lo .S_copy_bytes
.S_decide:
    GETBIT
    b.cs .S_new_enter
.S_literal:
    ELIAS_SLOW x14, 0
/* exact literal copy: x14 = len (>= 1) */
.S_lit_have_len:
    sub x13, x4, x3
    cmp x14, x13
    b.hi .Lfail
    sub x13, x1, x0
    cmp x14, x13
    b.hi .Lfail
    add x13, x3, x14
    cmp x14, #16
    b.lo 3f
    sub x16, x13, #16
1:
    cmp x3, x16
    b.hs 2f
    ldr q0, [x0], #16
    str q0, [x3], #16
    b 1b
2:                              /* overlapping exact 16-byte tail */
    sub x17, x13, x3            /* 1..16 bytes left */
    add x0, x0, x17
    ldur q0, [x0, #-16]
    stur q0, [x13, #-16]
    mov x3, x13
    b .S_after_lit
3:
    ldrb w17, [x0], #1
    strb w17, [x3], #1
    cmp x3, x13
    b.lo 3b
    b .S_after_lit

.S_to_fast_al:
    TO_FAST
    b .F_after_lit
.S_to_fast_sb:
    TO_FAST
    b .F_seg_b

.Leof:
    sub x0, x3, x2
    ret
.Lfail:
    mov x0, #-1
    ret
#if defined(__ELF__)
    .size lz0dec_fast_arm64, .-lz0dec_fast_arm64
#endif

    .section .rodata
    .p2align 6
.Lunzip:                        /* bits 6,4,2,0 of index -> 4-bit value */
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
.Ls_tab:                        /* .Lunzip + 256: s = p*floor(8/p) */
    .byte 0, 0, 8, 6, 8, 5, 6, 7

#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif

#endif /* __aarch64__ && !_WIN32 */

