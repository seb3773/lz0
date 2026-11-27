#if (defined(__ELF__) || defined(__APPLE__)) && defined(__aarch64__) && !defined(_WIN32)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(lz0dec_tiny_arm64)
#if defined(__ELF__)
.type lz0dec_tiny_arm64, %function
#endif

/*
 * Inline fast-path bit reader: resolves 87.5% of bit reads in 2 instructions
 * via sentinel w6 and calls .refill only once per input byte.
 */
.macro RBIT
    adds w6, w6, w6
    b.ne 99f
    bl .refill
99:
.endm

/*
 * int lz0dec_tiny_arm64(const unsigned char *in, int in_size,
 *                       unsigned char *out, int out_max);
 *
 * Size-optimized, contract-compliant LZ0 v0.1 AArch64 assembly decompressor:
 * - 100% pure leaf function: 0 bytes stack allocation, x18 untouched (Darwin-safe).
 * - Zero heap allocations, zero lookup tables (376 B ELF .text).
 * - Full bounds validation matching lz0_decompress_to (guard-page safe).
 */
CSYM(lz0dec_tiny_arm64):
    mov x15, x30                    /* save C caller return address (0 stack) */
    cbz x0, .fail
    cmp w1, #0
    b.le .fail
    cbz x2, .fail
    cmp w3, #0
    b.le .fail

    add x1, x0, w1, sxtw            /* x1 = in_end */
    add x4, x2, w3, sxtw            /* x4 = out_end */
    mov x3, x2                      /* x3 = out_ptr */
    mov x5, #1                      /* x5 = last_offset = 1 */
    mov w6, #0x80000000             /* w6 = sentinel bitreader */

.do_literal:
    bl .elias0                      /* x8 = L (>= 1) */
    sub x9, x1, x0
    cmp x8, x9
    b.hi .fail
    sub x9, x4, x3
    cmp x8, x9
    b.hi .fail
.lit_lp:
    ldrb w9, [x0], #1
    strb w9, [x3], #1
    subs x8, x8, #1
    b.ne .lit_lp

.after_lit:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    RBIT
    b.cs .enter_new_loop

.do_match_last:
    bl .elias0                      /* x8 = L (>= 1) */

.copy_match:
    sub x9, x3, x2
    cmp x5, x9
    b.hi .fail
    sub x9, x4, x3
    cmp x8, x9
    b.hi .fail
    sub x10, x3, x5                 /* x10 = src = out_ptr - last_offset */
.match_lp:
    ldrb w9, [x10], #1
    strb w9, [x3], #1
    subs x8, x8, #1
    b.ne .match_lp

.after_match:
    /* Decision bit after Match: 0 -> LiteralRun, 1 -> enter new_loop */
    RBIT
    b.cc .do_literal

.enter_new_loop:
    /* 0 -> EOF, 1 -> MatchNew */
    RBIT
    b.cc .eof

    bl .elias1                      /* x8 = msb (>= 1) */
    cmp x0, x1
    b.hs .fail
    ldrb w9, [x0], #1               /* t */
    lsl x5, x8, #8
    sub x5, x5, x9                  /* x5 = msb * 256 - t (>= 1) */

    mov w8, #1
.len3_lp:
    RBIT
    adc w8, w8, w8
    tbz w8, #3, .len3_lp
    cmp w8, #15
    b.eq .len3_long
    sub x8, x8, #6                  /* L = x + 2 (2..8) */
    b .copy_match

.len3_long:
    bl .elias0
    add x8, x8, #8                  /* L = elias + 8 (>= 9) */
    b .copy_match

.eof:
    sub x0, x3, x2
    ret x15

.fail:
    mov x0, #-1
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
    RBIT
    b.cs .el_ret
    tbnz x8, #29, .fail
    RBIT
    adc x8, x8, x8
    eor x8, x8, x12
    b .el_lp
.el_ret:
    ret x14

/* Refill sentinel bitreader from [x0++] into C flag (preserves C on ret). */
.refill:
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
    ret

#if defined(__ELF__)
.size CSYM(lz0dec_tiny_arm64), .-CSYM(lz0dec_tiny_arm64)
.section .note.GNU-stack,"",@progbits
#endif

#endif /* (__ELF__ || __APPLE__) && __aarch64__ && !_WIN32 */
