/*
 * lz0dec_ultra_tiny_riscv64.S - Extreme size-optimized trusted-stream LZ0 v0.1
 *                               decompressor for RISC-V 64-bit (RV64GC)
 *
 * int lz0dec_ultra_tiny_riscv64(const unsigned char *in, unsigned char *out);
 *
 * - 100% pure leaf function: 0 bytes stack allocation (ra saved in t6 / t5).
 * - Zero heap allocations, zero lookup tables (188 B RV64GC .text).
 * - Unchecked trusted-input contract: caller guarantees a valid non-empty
 *   LZ0 v0.1 stream and sufficient output capacity.
 */

#if defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)

.text
.globl lz0dec_ultra_tiny_riscv64
#if defined(__ELF__)
.type lz0dec_ultra_tiny_riscv64, @function
#endif

lz0dec_ultra_tiny_riscv64:
    mv t6, ra                       /* save C caller return address (0 stack) */
    mv a4, a1                       /* a4 = out_ptr (a1 stays out_start) */
    li a7, 1                        /* a7 = last_offset = 1 */
    li a6, 1
    slli a6, a6, 63                 /* a6 = 1 << 63 (empty-reader marker) */
    srli a5, a6, 1                  /* a5 = empty sentinel reader */

.Ldo_literal:
    jal .Lelias0                    /* a2 = L (>= 1) */
.Llit_lp:
    lbu a3, 0(a0)
    sb a3, 0(a4)
    addi a0, a0, 1
    addi a4, a4, 1
    addi a2, a2, -1
    bnez a2, .Llit_lp

.Lafter_lit:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    jal .Lbit
    bnez a3, .Lenter_new_loop

.Ldo_match_last:
    jal .Lelias0                    /* a2 = L (>= 1) */

.Lcopy_match:
    sub t3, a4, a7                  /* t3 = src = out_ptr - last_offset */
.Lmatch_lp:
    lbu a3, 0(t3)
    sb a3, 0(a4)
    addi t3, t3, 1
    addi a4, a4, 1
    addi a2, a2, -1
    bnez a2, .Lmatch_lp

.Lafter_match:
    /* Decision bit after Match: 0 -> LiteralRun, 1 -> enter new_loop */
    jal .Lbit
    beqz a3, .Ldo_literal

.Lenter_new_loop:
    /* 0 -> EOF, 1 -> MatchNew */
    jal .Lbit
    beqz a3, .Leof

    jal .Lelias1                    /* a2 = msb (>= 1) */
    lbu a3, 0(a0)
    addi a0, a0, 1
    slli a7, a2, 8
    sub a7, a7, a3                  /* a7 = msb * 256 - t (>= 1) */

    li a2, 1
.Llen3_lp:
    jal .Lbit
    slli a2, a2, 1
    add a2, a2, a3
    li a3, 8
    bltu a2, a3, .Llen3_lp
    addi a2, a2, -15                /* RVC: a2 == 0 iff x == 7 */
    beqz a2, .Llen3_long
    addi a2, a2, 9                  /* L = x + 2 (2..8) */
    j .Lcopy_match

.Llen3_long:
    jal .Lelias0
    addi a2, a2, 8                  /* L = elias + 8 (>= 9) */
    j .Lcopy_match

.Leof:
    sub a0, a4, a1
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
    jal .Lbit
    bnez a3, .Lel_ret
    jal .Lbit
    xor a3, a3, t2
    slli a2, a2, 1
    add a2, a2, a3
    j .Lel_lp
.Lel_ret:
    jr t5

/* Shift next bit into a3 (0/1) via sentinel a5; refill from [a0++] when exhausted. */
.Lbit:
    slli a5, a5, 1
    bne a5, a6, .Lbit_out
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    addi a5, a5, 1
    slli a5, a5, 55
.Lbit_out:
    srli a3, a5, 63
    ret

#if defined(__ELF__)
.size lz0dec_ultra_tiny_riscv64, .-lz0dec_ultra_tiny_riscv64
.section .note.GNU-stack,"",@progbits
#endif

#endif /* __ELF__ && __riscv && __riscv_xlen == 64 && !_WIN32 */
