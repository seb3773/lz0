/*
 * lz0dec_tiny_riscv64.S - Tiny freestanding LZ0 v0.1 decompressor for RISC-V 64-bit (RV64GC)
 *
 * Size-optimized, contract-compliant LZ0 v0.1 assembly decompressor:
 * - 100% pure leaf function: 0 bytes stack allocation (ra saved in t6 / t5).
 * - Zero heap allocations, zero lookup tables (300 B RV64GC .text).
 * - Full bounds validation matching lz0_decompress_to (guard-page safe).
 */

#if defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)

/*
 * Inline fast-path bit reader: shifts sentinel a5 left in 1 RVC instruction
 * and calls .Lrefill only once per input byte (87.5% inline hit rate).
 */
.macro RBIT
    slli a5, a5, 1
    bne a5, a6, 99f
    jal .Lrefill
99:
.endm

.text
.globl lz0dec_tiny_riscv64
#if defined(__ELF__)
.type lz0dec_tiny_riscv64, @function
#endif

lz0dec_tiny_riscv64:
    mv t6, ra                       /* save C caller return address (0 stack) */
    beqz a0, .Lfail
    blez a1, .Lfail
    beqz a2, .Lfail
    blez a3, .Lfail

    add a1, a0, a1                  /* a1 = in_end */
    mv t4, a2                       /* t4 = out_start */
    add a3, a2, a3                  /* a3 = out_end */
    mv a4, a2                       /* a4 = out_ptr */
    li a7, 1                        /* a7 = last_offset = 1 */
    li a6, 1
    slli a6, a6, 63                 /* a6 = 1 << 63 (empty-reader marker) */
    srli a5, a6, 1                  /* a5 = empty sentinel reader */

.Ldo_literal:
    jal .Lelias0                    /* a2 = L (>= 1) */
    sub t0, a1, a0
    bltu t0, a2, .Lfail
    sub t0, a3, a4
    bltu t0, a2, .Lfail
.Llit_lp:
    lbu t0, 0(a0)
    sb t0, 0(a4)
    addi a0, a0, 1
    addi a4, a4, 1
    addi a2, a2, -1
    bnez a2, .Llit_lp

.Lafter_lit:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    RBIT
    bltz a5, .Lenter_new_loop

.Ldo_match_last:
    jal .Lelias0                    /* a2 = L (>= 1) */

.Lcopy_match:
    sub t0, a4, t4
    bltu t0, a7, .Lfail
    sub t0, a3, a4
    bltu t0, a2, .Lfail
    sub t3, a4, a7                  /* t3 = src = out_ptr - last_offset */
.Lmatch_lp:
    lbu t0, 0(t3)
    sb t0, 0(a4)
    addi t3, t3, 1
    addi a4, a4, 1
    addi a2, a2, -1
    bnez a2, .Lmatch_lp

.Lafter_match:
    /* Decision bit after Match: 0 -> LiteralRun, 1 -> enter new_loop */
    RBIT
    bgez a5, .Ldo_literal

.Lenter_new_loop:
    /* 0 -> EOF, 1 -> MatchNew */
    RBIT
    bgez a5, .Leof

    jal .Lelias1                    /* a2 = msb (>= 1) */
    bgeu a0, a1, .Lfail
    lbu t0, 0(a0)
    addi a0, a0, 1
    slli a7, a2, 8
    sub a7, a7, t0                  /* a7 = msb * 256 - t (>= 1) */

    li a2, 1
.Llen3_lp:
    RBIT
    srli t0, a5, 63
    slli a2, a2, 1
    add a2, a2, t0
    li t0, 8
    bltu a2, t0, .Llen3_lp
    addi a2, a2, -15                /* RVC: a2 == 0 iff x == 7 */
    beqz a2, .Llen3_long
    addi a2, a2, 9                  /* L = (8 + x) - 6 = x + 2 (2..8) */
    j .Lcopy_match

.Llen3_long:
    jal .Lelias0
    addi a2, a2, 8                  /* L = elias + 8 (>= 9) */
    j .Lcopy_match

.Leof:
    sub a0, a4, t4
    jr t6

.Lfail:
    li a0, -1
    jr t6

/* Read Elias gamma into a2; t2 = 0 (normal) or 1 (inverted). */
.Lelias1:
    li t2, 1
    j .Lelias
.Lelias0:
    li t2, 0
.Lelias:
    mv t5, ra
    li a2, 1
.Lel_lp:
    RBIT
    bltz a5, .Lel_ret
    srli t0, a2, 29
    bnez t0, .Lfail
    RBIT
    srli t0, a5, 63
    xor t0, t0, t2
    slli a2, a2, 1
    add a2, a2, t0
    j .Lel_lp
.Lel_ret:
    jr t5

/* Refill sentinel bitreader from [a0++] into bit 63 of a5. */
.Lrefill:
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    addi a5, a5, 1                  /* RVC: set sentinel bit 0 (bit 0 was 0 after slli) */
    slli a5, a5, 55
    ret

#if defined(__ELF__)
.size lz0dec_tiny_riscv64, .-lz0dec_tiny_riscv64
.section .note.GNU-stack,"",@progbits
#endif

#endif /* __ELF__ && __riscv && __riscv_xlen == 64 && !_WIN32 */

