.text
.globl lz0dec_arm64
.type lz0dec_arm64, %function
.p2align 4

/*
 * int lz0dec_arm64(const unsigned char *in, int in_size,
 *                  unsigned char *out, int out_max);
 *
 * High-performance hand-crafted AArch64 / ARM64 assembly decoder for LZ0:
 * - 100% pure leaf function: ZERO stack frame allocations (0 bytes stack).
 * - ZERO subroutine calls (no bl/ret) in the decompression hot loop.
 * - Inlined state machine with mask-based bitreader (tst + cset).
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
    mov w6, #0              /* w6 = val = 0 */
    mov w7, #0              /* w7 = mask = 0 */

    /* First token is ALWAYS a LiteralRun */
    b .do_literal

    .p2align 4
.main_loop_after_literal:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    lsr w7, w7, #1
    cbnz w7, 1f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
1:
    tst w6, w7
    b.ne .enter_new_loop

.do_match_last:
    /* Fast-path Elias == 1 */
    lsr w7, w7, #1
    cbnz w7, 1f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
1:
    tst w6, w7
    b.eq .match_last_full_elias
    mov x8, #1
    b .match_last_have_len

.match_last_full_elias:
    /* First bit was 0: value starts at 1 */
    mov x8, #1
.ml_elias_loop:
    lsr x9, x8, #29
    cbnz x9, .fail

    /* Payload bit */
    lsr w7, w7, #1
    cbnz w7, 2f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
2:
    tst w6, w7
    cset x9, ne
    add x8, x9, x8, lsl #1

    /* Control bit */
    lsr w7, w7, #1
    cbnz w7, 3f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
3:
    tst w6, w7
    b.eq .ml_elias_loop

.match_last_have_len:
    /* Validate last_offset >= 1 */
    cmp x5, #1
    b.lt .fail

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
    lsr w7, w7, #1
    cbnz w7, 1f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
1:
    tst w6, w7
    b.eq .lit_full_elias
    mov x8, #1
    b .lit_have_len

.lit_full_elias:
    mov x8, #1
.lit_elias_loop:
    lsr x9, x8, #29
    cbnz x9, .fail

    /* Payload bit */
    lsr w7, w7, #1
    cbnz w7, 2f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
2:
    tst w6, w7
    cset x9, ne
    add x8, x9, x8, lsl #1

    /* Control bit */
    lsr w7, w7, #1
    cbnz w7, 3f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
3:
    tst w6, w7
    b.eq .lit_elias_loop

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
    cbz x8, .main_loop_after_literal
    ldrb w9, [x0], #1
    strb w9, [x3], #1
    sub x8, x8, #1
    b .lit_byte_loop

    .p2align 4
.enter_new_loop:
    /* Read 1 bit: 0 -> EOF, 1 -> MatchNew */
    lsr w7, w7, #1
    cbnz w7, 1f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
1:
    tst w6, w7
    b.eq .eof_reached

    /* Bit 1: MatchNew -> Inverted Elias for offset MSB */
    mov x8, #1
.msb_elias_loop:
    lsr w7, w7, #1
    cbnz w7, 2f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
2:
    tst w6, w7
    b.ne .msb_elias_done

    lsr x9, x8, #29
    cbnz x9, .fail

    lsr w7, w7, #1
    cbnz w7, 3f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
3:
    tst w6, w7
    cset x9, eq             /* Inverted bit: 1 if bit was 0, 0 if bit was 1 */
    add x8, x9, x8, lsl #1
    b .msb_elias_loop

.msb_elias_done:
    cmp x8, #1
    b.lt .fail

    /* Read raw LSB byte */
    cmp x0, x1
    b.hs .fail
    ldrb w12, [x0], #1

    /* offset = msb * 256 - t */
    lsl x5, x8, #8
    sub x5, x5, x12

    /* Validate offset: 1 <= offset <= (out_ptr - out_start) */
    cmp x5, #1
    b.lt .fail
    sub x9, x3, x2
    cmp x5, x9
    b.hi .fail

    /* Read 3 bits for MatchNew length */
    /* Bit 1 */
    lsr w7, w7, #1
    cbnz w7, 4f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
4:
    tst w6, w7
    cset w11, ne

    /* Bit 2 */
    lsr w7, w7, #1
    cbnz w7, 5f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
5:
    tst w6, w7
    cset w9, ne
    add w11, w9, w11, lsl #1

    /* Bit 3 */
    lsr w7, w7, #1
    cbnz w7, 6f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
6:
    tst w6, w7
    cset w9, ne
    add w11, w9, w11, lsl #1

    cmp w11, #7
    b.hs .new_len_elias
    add x8, x11, #2
    b .new_len_ready

.new_len_elias:
    /* Read uninverted Elias gamma */
    mov x8, #1
.nl_elias_loop:
    lsr w7, w7, #1
    cbnz w7, 7f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
7:
    tst w6, w7
    b.ne .nl_elias_done

    lsr x9, x8, #29
    cbnz x9, .fail

    lsr w7, w7, #1
    cbnz w7, 8f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
8:
    tst w6, w7
    cset x9, ne
    add x8, x9, x8, lsl #1
    b .nl_elias_loop

.nl_elias_done:
    add x8, x8, #8

.new_len_ready:
    sub x9, x4, x3
    cmp x8, x9
    b.hi .fail

    sub x10, x3, x5         /* x10 = src */

    /* Falls through directly to .do_match_copy */

    .p2align 4
.do_match_copy:
    /* Copy x8 bytes from x10 to x3 */
    cmp x8, #0
    b.le .after_match

    cmp x5, #8
    b.lo .match_overlap_short

    /* offset >= 8: Fast copy */
    cmp x8, #8
    b.hi .match_large

    /* Short match (1..8 bytes) with offset >= 8 */
    add x9, x3, #8
    cmp x9, x4
    b.hi .match_byte_loop
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
    b.hi .match_byte_loop
    sub x11, x8, #8
    ldr x9, [x10, x11]
    str x9, [x3, x11]
    add x3, x3, x8
    b .after_match

.match_byte_loop:
    cbz x8, .after_match
    ldrb w9, [x10], #1
    strb w9, [x3], #1
    sub x8, x8, #1
    b .match_byte_loop

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
    cbz x8, .after_match
    strb w9, [x3], #1
    sub x8, x8, #1
    b .fill1_bytes

.match_short_bytes:
    ldrb w9, [x10], #1
    strb w9, [x3], #1
    subs x8, x8, #1
    b.ne .match_short_bytes

.after_match:
    /* Decision bit after Match (MatchLast or MatchNew): 0 -> LiteralRun, 1 -> new_loop */
    lsr w7, w7, #1
    cbnz w7, 9f
    cmp x0, x1
    b.hs .fail
    ldrb w6, [x0], #1
    mov w7, #128
9:
    tst w6, w7
    b.eq .do_literal
    b .enter_new_loop

.eof_reached:
    sub x0, x3, x2          /* return bytes decompressed */
    ret

.fail:
    mov x0, #-1
    ret
