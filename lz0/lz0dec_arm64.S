#if defined(__ELF__) && defined(__aarch64__) && !defined(_WIN32)

.text
.globl lz0dec_arm64
#if defined(__ELF__)
.type lz0dec_arm64, %function
#endif
.p2align 4

/*
 * int lz0dec_arm64(const unsigned char *in, int in_size,
 *                  unsigned char *out, int out_max);
 *
 * High-performance hand-crafted AArch64 / ARM64 assembly decoder for LZ0:
 * - 100% pure leaf function: ZERO stack frame allocations (0 bytes stack).
 * - ZERO subroutine calls (no bl/ret) in the decompression hot loop.
 * - Inlined state machine with a sentinel bitreader: w6 = [unread bits][1]
 *   [0..] left-aligned at bit 31; ADDS shifts the next bit into C and flags
 *   exhaustion in Z. Payload bits accumulate with a single ADC.
 * - Fast-path for Elias gamma == 1 (resolved in 4 instructions without loop).
 * - 64-bit wildcopy transfers with boundary safety check.
 * - Broadcasted 64-bit stores for offset == 1 runs.
 */

lz0dec_arm64:
    /* Check invalid inputs early */
    cbz x0, .fail
    cmp w1, #0
    b.le .fail
    cbz x2, .fail
    cmp w3, #0
    b.le .fail

    /* Set up pointers in caller-saved registers x0..x15 */
    sxtw x1, w1
    add x1, x0, x1          /* x1 = in_end */

    sxtw x4, w3             /* read w3 (out_max) before modifying x3! */
    add x4, x2, x4          /* x4 = out_end */
    mov x3, x2              /* x3 = out_ptr */
    /* x2 = out_start */

    mov x5, #1              /* x5 = last_offset = 1 */
    mov w6, #0x80000000     /* w6 = sentinel bit reader, empty */

    /* First token is ALWAYS a LiteralRun */
    b .do_literal

    .p2align 4
.main_loop_after_literal:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    adds w6, w6, w6
    b.ne 1f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
1:
    b.cs .enter_new_loop

.do_match_last:
    /* Fast-path Elias == 1 */
    adds w6, w6, w6
    b.ne 1f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
1:
    b.cc .match_last_full_elias
    mov x8, #1
    b .match_last_have_len

.match_last_full_elias:
    /* First bit was 0: value starts at 1 */
    mov x8, #1
.ml_elias_loop:
    lsr x9, x8, #29
    cbnz x9, .fail

    /* Payload bit */
    adds w6, w6, w6
    b.ne 2f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
2:
    adc x8, x8, x8

    /* Control bit */
    adds w6, w6, w6
    b.ne 3f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
3:
    b.cc .ml_elias_loop

.match_last_have_len:
    /* Check src = out_ptr - last_offset >= out_start */
    sub x10, x3, x5
    cmp x10, x2
    b.lo .fail

    /* Check out_ptr + L <= out_end */
    sub x9, x4, x3
    cmp x8, x9
    b.hi .fail

    b .do_match_copy

    .p2align 4
.do_literal:
    /* Fast-path Elias == 1 */
    adds w6, w6, w6
    b.ne 1f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
1:
    b.cc .lit_full_elias
    /* single-byte literal (most frequent run length): direct copy */
    cmp x3, x4
    b.hs .fail
    cmp x0, x1
    b.hs .fail
    ldrb w9, [x0], #1
    strb w9, [x3], #1
    b .main_loop_after_literal

.lit_full_elias:
    mov x8, #1
.lit_elias_loop:
    lsr x9, x8, #29
    cbnz x9, .fail

    /* Payload bit */
    adds w6, w6, w6
    b.ne 2f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
2:
    adc x8, x8, x8

    /* Control bit */
    adds w6, w6, w6
    b.ne 3f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
3:
    b.cc .lit_elias_loop

.lit_have_len:
    /* Check out_ptr + L <= out_end */
    sub x9, x4, x3
    cmp x8, x9
    b.hi .fail

    /* Check in_ptr + L <= in_end */
    sub x9, x1, x0
    cmp x8, x9
    b.hi .fail

    cmp x8, #8
    b.hi .lit_large

    /* Short literal (1..8 bytes) */
    add x9, x3, #8
    cmp x9, x4
    b.hi .lit_byte_loop
    add x9, x0, #8
    cmp x9, x1
    b.hi .lit_byte_loop

    ldr x9, [x0]
    str x9, [x3]
    add x0, x0, x8
    add x3, x3, x8
    b .main_loop_after_literal

.lit_large:
.lit_large_loop:
    cmp x8, #8
    b.lo .lit_large_tail
    ldr x9, [x0], #8
    str x9, [x3], #8
    sub x8, x8, #8
    b .lit_large_loop

.lit_large_tail:
    cbz x8, .main_loop_after_literal
    add x9, x3, #8
    cmp x9, x4
    b.hi .lit_byte_loop
    add x9, x0, #8
    cmp x9, x1
    b.hi .lit_byte_loop

    sub x11, x8, #8
    ldr x9, [x0, x11]
    str x9, [x3, x11]
    add x0, x0, x8
    add x3, x3, x8
    b .main_loop_after_literal

.lit_byte_loop:
    ldrb w9, [x0], #1
    strb w9, [x3], #1
    subs x8, x8, #1
    b.ne .lit_byte_loop
    b .main_loop_after_literal

    .p2align 4
.enter_new_loop:
    /* Read 1 bit: 0 -> EOF, 1 -> MatchNew */
    adds w6, w6, w6
    b.ne 1f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
1:
    b.cc .eof_reached

    /* Bit 1: MatchNew -> Inverted Elias for offset MSB */
    mov x8, #1
.msb_elias_loop:
    adds w6, w6, w6
    b.ne 2f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
2:
    b.cs .msb_elias_done


    adds w6, w6, w6
    b.ne 3f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
3:
    cset x9, cc             /* Inverted bit: 1 if bit was 0, 0 if bit was 1 */
    add x8, x9, x8, lsl #1
    lsr x9, x8, #30         /* limit check folded into the back-edge: v >= 2^30 after the shift <=> v >= 2^29 before it */
    cbz x9, .msb_elias_loop
    b .fail

.msb_elias_done:
    /* Read raw LSB byte */
    cmp x0, x1
    b.hs .fail
    ldrb w12, [x0], #1

    /* offset = msb * 256 - t (>= 1 guaranteed) */
    lsl x5, x8, #8
    sub x5, x5, x12

    /* Validate offset <= (out_ptr - out_start) */
    sub x9, x3, x2
    cmp x5, x9
    b.hi .fail

    /* Read 3 bits for MatchNew length */
    /* Bit 1 */
    adds w6, w6, w6
    b.ne 4f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
4:
    cset w11, cs

    /* Bit 2 */
    adds w6, w6, w6
    b.ne 5f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
5:
    adc w11, w11, w11

    /* Bit 3 */
    adds w6, w6, w6
    b.ne 6f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
6:
    adc w11, w11, w11

    cmp w11, #7
    b.hs .new_len_elias
    add x8, x11, #2
    b .new_len_ready

.new_len_elias:
    /* Read uninverted Elias gamma */
    mov x8, #1
.nl_elias_loop:
    adds w6, w6, w6
    b.ne 7f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
7:
    b.cs .nl_elias_done


    adds w6, w6, w6
    b.ne 8f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
8:
    adc x8, x8, x8
    lsr x9, x8, #30         /* limit check folded into the back-edge: v >= 2^30 after the shift <=> v >= 2^29 before it */
    cbz x9, .nl_elias_loop
    b .fail

.nl_elias_done:
    add x8, x8, #8

.new_len_ready:
    sub x9, x4, x3
    cmp x8, x9
    b.hi .fail

    sub x10, x3, x5         /* x10 = src */

    /* Falls through directly to .do_match_copy */

.do_match_copy:
    /* Copy x8 bytes (>= 1 guaranteed) from x10 to x3 */
    cmp x5, #8
    b.lo .match_overlap_short

    /* offset >= 8: Fast copy */
    cmp x8, #8
    b.hi .match_large

    /* Short match (1..8 bytes) with offset >= 8 */
    add x9, x3, #8
    cmp x9, x4
    b.hi .match_short_bytes
    ldr x9, [x10]
    str x9, [x3]
    add x3, x3, x8
    b .after_match

.match_large:
    /* Large match (>8 bytes) with offset >= 8 */
.match_large_loop:
    cmp x8, #8
    b.lo .match_large_tail
    ldr x9, [x10], #8
    str x9, [x3], #8
    sub x8, x8, #8
    b .match_large_loop

.match_large_tail:
    cbz x8, .after_match
    add x9, x3, #8
    cmp x9, x4
    b.hi .match_short_bytes
    sub x11, x8, #8
    ldr x9, [x10, x11]
    str x9, [x3, x11]
    add x3, x3, x8
    b .after_match

.match_overlap_short:
    cmp x5, #1
    b.ne .match_short_bytes

    /* Offset == 1: run of identical byte */
    ldrb w9, [x10]
    mov x11, #0x0101010101010101
    mul x9, x9, x11

    cmp x8, #8
    b.hi .fill1_large

    add x12, x3, #8
    cmp x12, x4
    b.hi .fill1_bytes
    str x9, [x3]
    add x3, x3, x8
    b .after_match

.fill1_large:
.fill1_qwords:
    cmp x8, #32
    b.lo .fill1_8
    str x9, [x3], #8
    str x9, [x3], #8
    str x9, [x3], #8
    str x9, [x3], #8
    sub x8, x8, #32
    b .fill1_qwords
.fill1_8:
    cmp x8, #8
    b.lo .fill1_tail
    str x9, [x3], #8
    sub x8, x8, #8
    b .fill1_8
.fill1_tail:
    cbz x8, .after_match
    add x12, x3, #8
    cmp x12, x4
    b.hi .fill1_bytes
    sub x11, x8, #8
    str x9, [x3, x11]
    add x3, x3, x8
    b .after_match
.fill1_bytes:
    strb w9, [x3], #1
    subs x8, x8, #1
    b.ne .fill1_bytes
    b .after_match

.match_short_bytes:
    ldrb w9, [x10], #1
    strb w9, [x3], #1
    subs x8, x8, #1
    b.ne .match_short_bytes

.after_match:
    /* Decision bit after Match (MatchLast or MatchNew): 0 -> LiteralRun, 1 -> new_loop */
    adds w6, w6, w6
    b.ne 9f
    cmp x0, x1
    b.hs .fail
    ldrb w7, [x0], #1
    lsl w7, w7, #24
    orr w6, w7, #0x800000
    adds w6, w6, w6
9:
    b.cc .do_literal
    b .enter_new_loop

.eof_reached:
    sub x0, x3, x2          /* return bytes decompressed */
    ret

.fail:
    mov x0, #-1
    ret

#if defined(__ELF__)
.size lz0dec_arm64, .-lz0dec_arm64
.section .note.GNU-stack,"",@progbits
#endif

#endif /* __aarch64__ && !_WIN32 */

