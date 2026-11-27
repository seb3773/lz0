#if (defined(__ELF__) || defined(__APPLE__)) && defined(__aarch64__) && !defined(_WIN32)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(lz0dec_ultra_tiny_arm64)
#if defined(__ELF__)
.type lz0dec_ultra_tiny_arm64, %function
#endif

/*
 * int lz0dec_ultra_tiny_arm64(const unsigned char *in, unsigned char *out);
 *
 * Extreme size-optimized trusted-stream LZ0 v0.1 AArch64 assembly decompressor:
 * - 100% pure leaf function: 0 bytes stack allocation, x18 untouched (Darwin-safe).
 * - Zero heap allocations, zero lookup tables (224 B ELF .text).
 * - Unchecked trusted-input contract: caller guarantees a valid non-empty
 *   LZ0 v0.1 stream and sufficient output capacity.
 */
CSYM(lz0dec_ultra_tiny_arm64):
    mov x15, x30                    /* save C caller return address (0 stack) */
    mov x2, x1                      /* x2 = out_ptr (x1 stays out_start) */
    mov x5, #1                      /* x5 = last_offset = 1 */
    mov w6, #0x80000000             /* w6 = sentinel bitreader */

.do_literal:
    bl .elias0                      /* x8 = L (>= 1) */
.lit_lp:
    ldrb w9, [x0], #1
    strb w9, [x2], #1
    subs x8, x8, #1
    b.ne .lit_lp

.after_lit:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    bl .bit
    b.cs .enter_new_loop

.do_match_last:
    bl .elias0                      /* x8 = L (>= 1) */

.copy_match:
    sub x10, x2, x5                 /* x10 = src = out_ptr - last_offset */
.match_lp:
    ldrb w9, [x10], #1
    strb w9, [x2], #1
    subs x8, x8, #1
    b.ne .match_lp

.after_match:
    /* Decision bit after Match: 0 -> LiteralRun, 1 -> enter new_loop */
    bl .bit
    b.cc .do_literal

.enter_new_loop:
    /* 0 -> EOF, 1 -> MatchNew */
    bl .bit
    b.cc .eof

    bl .elias1                      /* x8 = msb (>= 1) */
    ldrb w9, [x0], #1               /* t */
    lsl x5, x8, #8
    sub x5, x5, x9                  /* x5 = msb * 256 - t (>= 1) */

    mov w8, #0x20000000             /* 3-bit sentinel for len3 (x = 0..7) */
.len3_lp:
    bl .bit
    adcs w8, w8, w8
    b.cc .len3_lp
    add x8, x8, #2                  /* L = x + 2 (2..8), or 9 if x == 7 */
    cmp x8, #9
    b.lo .copy_match
    bl .elias0
    add x8, x8, #8                  /* L = elias + 8 (>= 9) */
    b .copy_match

.eof:
    sub x0, x2, x1
    ret x15

/* Read Elias gamma into x8; w12 = 0 (normal) or 1 (inverted). */
.elias1:
    mov w12, #1
    b .elias
.elias0:
    mov w12, #0
.elias:
    mov x14, x30
    mov x8, #1
.el_lp:
    bl .bit
    b.cs .el_ret
    bl .bit
    adc x8, x8, x8
    eor x8, x8, x12
    b .el_lp
.el_ret:
    ret x14

/* Refill sentinel bitreader from [x0++] into C flag. */
.bit:
    adds w6, w6, w6
    b.ne .bit_ret
    ldrb w6, [x0], #1
    lsl w6, w6, #24
    orr w6, w6, #0x800000
    adds w6, w6, w6
.bit_ret:
    ret

#if defined(__ELF__)
.size CSYM(lz0dec_ultra_tiny_arm64), .-CSYM(lz0dec_ultra_tiny_arm64)
.section .note.GNU-stack,"",@progbits
#endif

#endif /* (__ELF__ || __APPLE__) && __aarch64__ && !_WIN32 */

