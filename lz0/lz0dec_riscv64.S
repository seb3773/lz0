#
# lz0dec_riscv64.s - Compact freestanding LZ0 decompressor for RISC-V 64-bit (RV64GC)
# 
# C prototype:
# int lz0dec_riscv64(const unsigned char *in, int in_size,
# unsigned char *out, int out_max);
# 
# Architecture & ABI:
# - RISC-V 64-bit (RV64I / RV64GC, standard lp64/lp64d ABI)
# - 100% pure leaf function: ZERO stack allocations (sp untouched)
# - ZERO subroutine calls: completely inlined state machine
# - All state held in caller-saved registers (a0-a7, t0-t6)
# - Fast-path for Elias gamma == 1 (resolved in 3 instructions)
# - 64-bit wildcopy transfers (ld/sd) with boundary safety check
# - 64-bit broadcast stores (mul by 0x0101010101010101) for offset == 1
# 
# Register mapping:
# a0 : in_ptr (input read pointer)
# a1 : in_end (in + in_size)
# a2 : out_start (base output pointer)
# a3 : out_end (out + out_max)
# a4 : out_ptr (output write pointer)
# a5 : sentinel bit reader: current bit at bit 63, unread bits below,
#      then a 1 marker; a5 == a6 after SLLI means the byte is used up
# a6 : constant 1 << 63
# a7 : last_offset (initialized to 1)
# t0..t6 : temporaries (scratch, length, copy pointers)

#if defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)

.text
.globl lz0dec_riscv64
#if defined(__ELF__)
.type lz0dec_riscv64, @function
#endif
.p2align 3

lz0dec_riscv64:
# Input validation
    beqz a0, .Lfail
    blez a1, .Lfail
    beqz a2, .Lfail
    blez a3, .Lfail

# Setup pointers in registers
    add a1, a0, a1          #  a1 = in_end 
    add a3, a2, a3          #  a3 = out_end 
    mv a4, a2               #  a4 = out_ptr 
# a2 = out_start

    li a7, 1                #  a7 = last_offset = 1 
    li a6, 1
    slli a6, a6, 63         # a6 = 1 << 63 (empty-reader marker)
    srli a5, a6, 1          # a5 = empty sentinel reader (first SLLI -> marker)

# First token is ALWAYS a LiteralRun
    j .Ldo_literal

.Lmain_loop_after_literal:
# Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop
    slli a5, a5, 1
    bne a5, a6, 1f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
1:
    bltz a5, .Lenter_new_loop

.Ldo_match_last:
# Fast-path Elias == 1
    slli a5, a5, 1
    bne a5, a6, 1f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
1:
    bgez a5, .Lml_full_elias
    li t2, 1                #  t2 = length = 1 
    j .Lmatch_last_have_len

.Lml_full_elias:
    li t2, 1
.Lml_elias_loop:
    srli t0, t2, 29
    bnez t0, .Lfail

# Payload bit
    slli a5, a5, 1
    bne a5, a6, 2f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
2:
    srli t0, a5, 63
    slli t2, t2, 1
    add t2, t2, t0

# Control bit
    slli a5, a5, 1
    bne a5, a6, 3f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
3:
    bgez a5, .Lml_elias_loop

.Lmatch_last_have_len:
# Validate last_offset >= 1
    blez a7, .Lfail

# Check src = out_ptr - last_offset >= out_start
    sub t3, a4, a7          #  t3 = src 
    bltu t3, a2, .Lfail

# Check out_ptr + L <= out_end
    sub t0, a3, a4
    bltu t0, t2, .Lfail

    j .Ldo_match_copy

.Ldo_literal:
# Fast-path Elias == 1
    slli a5, a5, 1
    bne a5, a6, 1f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
1:
    bgez a5, .Llit_full_elias
    # single-byte literal (most frequent run length): direct copy
    bgeu a4, a3, .Lfail
    bgeu a0, a1, .Lfail
    lbu t0, 0(a0)
    sb t0, 0(a4)
    addi a0, a0, 1
    addi a4, a4, 1
    j .Lmain_loop_after_literal

.Llit_full_elias:
    li t2, 1
.Llit_elias_loop:
    srli t0, t2, 29
    bnez t0, .Lfail

# Payload bit
    slli a5, a5, 1
    bne a5, a6, 2f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
2:
    srli t0, a5, 63
    slli t2, t2, 1
    add t2, t2, t0

# Control bit
    slli a5, a5, 1
    bne a5, a6, 3f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
3:
    bgez a5, .Llit_elias_loop

.Llit_have_len:
# Check out_ptr + L <= out_end
    sub t0, a3, a4
    bltu t0, t2, .Lfail

# Check in_ptr + L <= in_end
    sub t0, a1, a0
    bltu t0, t2, .Lfail

    li t0, 8
    bgtu t2, t0, .Llit_large

# Short literal (1..8 bytes)
    addi t0, a4, 8
    bgtu t0, a3, .Llit_byte_loop
    addi t0, a0, 8
    bgtu t0, a1, .Llit_byte_loop

    ld t0, 0(a0)
    sd t0, 0(a4)
    add a0, a0, t2
    add a4, a4, t2
    j .Lmain_loop_after_literal

.Llit_large:
    li t0, 8
.Llit_large_loop:
    bltu t2, t0, .Llit_large_tail
    ld t1, 0(a0)
    sd t1, 0(a4)
    addi a0, a0, 8
    addi a4, a4, 8
    addi t2, t2, -8
    j .Llit_large_loop

.Llit_large_tail:
    beqz t2, .Lmain_loop_after_literal
    addi t0, a4, 8
    bgtu t0, a3, .Llit_byte_loop
    addi t0, a0, 8
    bgtu t0, a1, .Llit_byte_loop

    addi t1, t2, -8
    add t4, a0, t1
    add t5, a4, t1
    ld t0, 0(t4)
    sd t0, 0(t5)
    add a0, a0, t2
    add a4, a4, t2
    j .Lmain_loop_after_literal

.Llit_byte_loop:
    beqz t2, .Lmain_loop_after_literal
    lbu t0, 0(a0)
    sb t0, 0(a4)
    addi a0, a0, 1
    addi a4, a4, 1
    addi t2, t2, -1
    j .Llit_byte_loop

.Lenter_new_loop:
# Read 1 bit: 0 -> EOF, 1 -> MatchNew
    slli a5, a5, 1
    bne a5, a6, 1f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
1:
    bgez a5, .Leof_reached

# Bit 1: MatchNew -> Inverted Elias for offset MSB
    li t1, 1                #  t1 = msb 
.Lmsb_elias_loop:
    slli a5, a5, 1
    bne a5, a6, 2f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
2:
    bltz a5, .Lmsb_elias_done


    slli a5, a5, 1
    bne a5, a6, 3f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
3:
    not t0, a5               # inverted bit
    srli t0, t0, 63
    slli t1, t1, 1
    add t1, t1, t0
    srli t0, t1, 30          # limit check folded into the back-edge (v >= 2^30 after shift <=> v >= 2^29 before)
    beqz t0, .Lmsb_elias_loop
    j .Lfail

.Lmsb_elias_done:
    blez t1, .Lfail

# Read raw LSB byte
    bgeu a0, a1, .Lfail
    lbu t4, 0(a0)
    addi a0, a0, 1

# offset = msb * 256 - t
    slli a7, t1, 8
    sub a7, a7, t4          #  a7 = last_offset 

# Validate offset: 1 <= offset <= (out_ptr - out_start)
    blez a7, .Lfail
    sub t0, a4, a2
    bltu t0, a7, .Lfail

# Read 3 bits for MatchNew length
# Bit 1
    slli a5, a5, 1
    bne a5, a6, 4f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
4:
    srli t2, a5, 63

# Bit 2
    slli a5, a5, 1
    bne a5, a6, 5f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
5:
    srli t0, a5, 63
    slli t2, t2, 1
    add t2, t2, t0

# Bit 3
    slli a5, a5, 1
    bne a5, a6, 6f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
6:
    srli t0, a5, 63
    slli t2, t2, 1
    add t2, t2, t0

    li t0, 7
    bgeu t2, t0, .Lnew_len_elias
    addi t2, t2, 2
    j .Lnew_len_ready

.Lnew_len_elias:
# Read uninverted Elias gamma
    li t2, 1
.Lnl_elias_loop:
    slli a5, a5, 1
    bne a5, a6, 7f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
7:
    bltz a5, .Lnl_elias_done


    slli a5, a5, 1
    bne a5, a6, 8f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
8:
    srli t0, a5, 63
    slli t2, t2, 1
    add t2, t2, t0
    srli t0, t2, 30          # limit check folded into the back-edge (v >= 2^30 after shift <=> v >= 2^29 before)
    beqz t0, .Lnl_elias_loop
    j .Lfail

.Lnl_elias_done:
    addi t2, t2, 8

.Lnew_len_ready:
    sub t0, a3, a4
    bltu t0, t2, .Lfail

    sub t3, a4, a7          #  t3 = src 

.Ldo_match_copy:
# Copy t2 bytes from t3 to a4
    blez t2, .Lafter_match

    li t0, 8
    bltu a7, t0, .Lmatch_overlap_short

# offset >= 8: Fast copy
    bgtu t2, t0, .Lmatch_large

# Short match (1..8 bytes) with offset >= 8
    addi t0, a4, 8
    bgtu t0, a3, .Lmatch_byte_loop
    ld t0, 0(t3)
    sd t0, 0(a4)
    add a4, a4, t2
    j .Lafter_match

.Lmatch_large:
    li t0, 8
.Lmatch_large_loop:
    bltu t2, t0, .Lmatch_large_tail
    ld t1, 0(t3)
    sd t1, 0(a4)
    addi t3, t3, 8
    addi a4, a4, 8
    addi t2, t2, -8
    j .Lmatch_large_loop

.Lmatch_large_tail:
    beqz t2, .Lafter_match
    addi t0, a4, 8
    bgtu t0, a3, .Lmatch_byte_loop
    addi t1, t2, -8
    add t4, t3, t1
    add t5, a4, t1
    ld t0, 0(t4)
    sd t0, 0(t5)
    add a4, a4, t2
    j .Lafter_match

.Lmatch_byte_loop:
    beqz t2, .Lafter_match
    lbu t0, 0(t3)
    sb t0, 0(a4)
    addi t3, t3, 1
    addi a4, a4, 1
    addi t2, t2, -1
    j .Lmatch_byte_loop

.Lmatch_overlap_short:
    li t0, 1
    bne a7, t0, .Lmatch_short_bytes

# Offset == 1: run of identical byte
    lbu t0, 0(t3)
# Broadcast byte across 64-bit register: t0 * 0x0101010101010101
    li t4, 0x0101010101010101
    mul t0, t0, t4

    li t5, 8
    bgtu t2, t5, .Lfill1_large

    addi t5, a4, 8
    bgtu t5, a3, .Lfill1_bytes
    sd t0, 0(a4)
    add a4, a4, t2
    j .Lafter_match

.Lfill1_large:
    li t5, 32
.Lfill1_32:
    bltu t2, t5, .Lfill1_8
    sd t0, 0(a4)
    sd t0, 8(a4)
    sd t0, 16(a4)
    sd t0, 24(a4)
    addi a4, a4, 32
    addi t2, t2, -32
    j .Lfill1_32

.Lfill1_8:
    li t5, 8
.Lfill1_8_loop:
    bltu t2, t5, .Lfill1_tail
    sd t0, 0(a4)
    addi a4, a4, 8
    addi t2, t2, -8
    j .Lfill1_8_loop

.Lfill1_tail:
    beqz t2, .Lafter_match
    addi t5, a4, 8
    bgtu t5, a3, .Lfill1_bytes
    addi t1, t2, -8
    add t6, a4, t1
    sd t0, 0(t6)
    add a4, a4, t2
    j .Lafter_match

.Lfill1_bytes:
    beqz t2, .Lafter_match
    sb t0, 0(a4)
    addi a4, a4, 1
    addi t2, t2, -1
    j .Lfill1_bytes

.Lmatch_short_bytes:
    lbu t0, 0(t3)
    sb t0, 0(a4)
    addi t3, t3, 1
    addi a4, a4, 1
    addi t2, t2, -1
    bnez t2, .Lmatch_short_bytes

.Lafter_match:
# Decision bit after Match: 0 -> LiteralRun, 1 -> new_loop
    slli a5, a5, 1
    bne a5, a6, 1f
    bgeu a0, a1, .Lfail
    lbu a5, 0(a0)
    addi a0, a0, 1
    slli a5, a5, 1
    ori a5, a5, 1
    slli a5, a5, 55
1:
    bgez a5, .Ldo_literal
    j .Lenter_new_loop

.Leof_reached:
    sub a0, a4, a2          #  return bytes decompressed (out_ptr - out_start) 
    ret

.Lfail:
    li a0, -1
    ret

#if defined(__ELF__)
.size lz0dec_riscv64, .-lz0dec_riscv64
.section .note.GNU-stack,"",@progbits
#endif

#endif /* __riscv && __riscv_xlen == 64 && !_WIN32 */


