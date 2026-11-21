.intel_syntax noprefix
.text
.globl lz0dec_x86_64
.type lz0dec_x86_64, @function

/*
 * int lz0dec_x86_64(const unsigned char *in, int in_size,
 *                   unsigned char *out, int out_max);
 *
 * High-performance hand-crafted x86-64 assembly decoder for LZ0:
 * - 64-bit word transfers with safe boundary checks (no heap buffer overflow).
 * - Fast-path for Elias gamma == 1 (single-bit check without function call).
 * - Tail-overlapping 64-bit stores eliminating remainder byte-by-byte loops.
 * - Broadcasted 64-bit stores for offset == 1 runs.
 * - Zero dynamic memory allocations.
 * - Only rbx/rbp are saved: 16 bytes of stack plus at most one return
 *   address (.read_elias_rest), i.e. <= 32 bytes total including the
 *   caller's return address. Match copies are reached by JMP and all end
 *   in the shared decision-bit code (.after_match): no call/ret per match.
 * - Sentinel bit reader: r8b = [unread bits][1][0..]. One ADD shifts the
 *   next bit into CF and flags exhaustion in ZF (no bit counter register).
 */

lz0dec_x86_64:
    /* Check invalid inputs */
    test rdi, rdi
    jz .fail_early
    test esi, esi
    jle .fail_early
    test rdx, rdx
    jz .fail_early
    test ecx, ecx
    jle .fail_early

    push rbx
    push rbp

    /* rdi = in_ptr (already in place) */
    movsxd rax, esi
    lea r9, [rdi + rax]            /* in_end */

    mov r10, rdx                    /* out_start */
    mov rbp, rdx                    /* out_ptr */
    movsxd rax, ecx
    lea r11, [rdx + rax]            /* out_end */

    mov rbx, 1                      /* last_offset = 1 */
    mov r8d, 0x80                   /* sentinel bit reader: empty */

    jmp .do_literal

    .p2align 4
.main_loop_after_literal:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jc .enter_new_loop

.do_match_last:
    /* Fast-path Elias == 1 (most frequent case: length is 1) */
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jnc .match_last_full_elias
    mov ecx, 1
    jmp .match_last_have_len

.match_last_full_elias:
    call .read_elias_rest
    mov rcx, rax

.match_last_have_len:
    test rbx, rbx
    jle .fail

    mov rsi, rbp
    sub rsi, rbx
    cmp rsi, r10
    jb .fail

    mov rax, r11
    sub rax, rbp
    cmp rcx, rax
    ja .fail

    jmp .copy_match_fast

    /* Common continuation of every match copy (MatchLast and MatchNew):
       decision bit, 0 -> LiteralRun, 1 -> enter new_loop. */
.after_match:
    add r8b, r8b
    jnz 2f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
2:
    jc .enter_new_loop

.do_literal:
    /* Fast-path Elias == 1 (single-byte literal run) */
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jnc .lit_full_elias
    /* single-byte literal (most frequent run length): direct copy */
    cmp rbp, r11
    jae .fail
    cmp rdi, r9
    jae .fail
    mov al, [rdi]
    mov [rbp], al
    inc rdi
    inc rbp
    jmp .main_loop_after_literal

.lit_full_elias:
    call .read_elias_rest
    mov rcx, rax

.lit_have_len:
    mov rax, r11
    sub rax, rbp
    cmp rcx, rax
    ja .fail

    mov rax, r9
    sub rax, rdi
    cmp rcx, rax
    ja .fail

    /* Fast literal copy */
    test rcx, rcx
    jle .fail

    cmp rcx, 8
    ja .lit_large

    /* Short literal (1..8 bytes) */
    lea rax, [rbp + 8]
    cmp rax, r11
    ja .lit_byte_loop
    lea rax, [rdi + 8]
    cmp rax, r9
    ja .lit_byte_loop

    mov rax, [rdi]
    mov [rbp], rax
    add rdi, rcx
    add rbp, rcx
    jmp .main_loop_after_literal

.lit_large:
    /* Large literal (>8 bytes) */
.lit_large_loop:
    cmp rcx, 8
    jb .lit_large_tail
    mov rax, [rdi]
    mov [rbp], rax
    add rdi, 8
    add rbp, 8
    sub rcx, 8
    jmp .lit_large_loop

.lit_large_tail:
    test rcx, rcx
    jz .main_loop_after_literal

    lea rax, [rbp + 8]
    cmp rax, r11
    ja .lit_byte_loop
    lea rax, [rdi + 8]
    cmp rax, r9
    ja .lit_byte_loop

    mov rax, [rdi + rcx - 8]
    mov [rbp + rcx - 8], rax
    add rdi, rcx
    add rbp, rcx
    jmp .main_loop_after_literal

.lit_byte_loop:
    test rcx, rcx
    jz .main_loop_after_literal
    mov al, [rdi]
    mov [rbp], al
    inc rdi
    inc rbp
    dec rcx
    jmp .lit_byte_loop

    .p2align 4
.enter_new_loop:
    /* Read 1 bit: 0 -> EOF, 1 -> MatchNew */
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jnc .eof_reached

    /* Bit 1: MatchNew */
    /* inverted Elias gamma (offset MSB), inlined: single call site */
    mov eax, 1
.elias_inv_loop:
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jc .elias_inv_done


    add r8b, r8b
    jnz 2f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
2:
    cmc
    adc eax, eax
    cmp eax, 0x40000000             /* limit check folded into the back-edge: v >= 2^30 after the shift <=> v >= 2^29 before it */
    jb .elias_inv_loop
    jmp .fail
.elias_inv_done:
    test eax, eax
    jle .fail

    cmp rdi, r9
    jae .fail
    movzx edx, byte ptr [rdi]
    inc rdi

    mov rbx, rax
    shl rbx, 8
    sub rbx, rdx

    test rbx, rbx
    jle .fail
    mov rax, rbp
    sub rax, r10
    cmp rbx, rax
    ja .fail

    /* 3 bits length */
    xor edx, edx
    add r8b, r8b
    jnz 2f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
2:
    adc edx, edx

    add r8b, r8b
    jnz 3f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
3:
    adc edx, edx

    add r8b, r8b
    jnz 4f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
4:
    adc edx, edx

    cmp edx, 7
    jae .new_len_elias
    add edx, 2
    mov rcx, rdx
    jmp .new_len_ready

.new_len_elias:
    /* Elias gamma (long MatchNew length), inlined: single call site */
    mov eax, 1
.elias_loop:
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jc .elias_done


    add r8b, r8b
    jnz 2f
    cmp rdi, r9
    jae .fail
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
2:
    adc eax, eax
    cmp eax, 0x40000000             /* limit check folded into the back-edge: v >= 2^30 after the shift <=> v >= 2^29 before it */
    jb .elias_loop
    jmp .fail
.elias_done:
    lea rcx, [rax + 8]

.new_len_ready:
    mov rax, r11
    sub rax, rbp
    cmp rcx, rax
    ja .fail

    mov rsi, rbp
    sub rsi, rbx

    jmp .copy_match_fast

    .p2align 4
.copy_match_fast:
    test rcx, rcx
    jle .copy_ret

    cmp rbx, 8
    jb .match_overlap_short

    /* offset >= 8: Fast copy */
    cmp rcx, 8
    ja .match_large

    /* Short match (1..8 bytes) with offset >= 8 */
    lea rax, [rbp + 8]
    cmp rax, r11
    ja .match_byte_loop
    mov rax, [rsi]
    mov [rbp], rax
    add rbp, rcx
    jmp .after_match

.match_large:
    /* Large match (>8 bytes) with offset >= 8 */
.match_large_loop:
    cmp rcx, 8
    jb .match_large_tail
    mov rax, [rsi]
    mov [rbp], rax
    add rsi, 8
    add rbp, 8
    sub rcx, 8
    jmp .match_large_loop

.match_large_tail:
    test rcx, rcx
    jz .copy_ret
    lea rax, [rbp + 8]
    cmp rax, r11
    ja .match_byte_loop
    mov rax, [rsi + rcx - 8]
    mov [rbp + rcx - 8], rax
    add rbp, rcx
    jmp .after_match

.match_byte_loop:
    test rcx, rcx
    jz .copy_ret
    mov al, [rsi]
    mov [rbp], al
    inc rsi
    inc rbp
    dec rcx
    jmp .match_byte_loop

.match_overlap_short:
    cmp rbx, 1
    jne .match_short_bytes

    /* Offset == 1 */
    movzx eax, byte ptr [rsi]
    mov rdx, 0x0101010101010101
    imul rax, rdx

    cmp rcx, 8
    ja .fill1_large

    lea rdx, [rbp + 8]
    cmp rdx, r11
    ja .fill1_bytes
    mov [rbp], rax
    add rbp, rcx
    jmp .after_match

.fill1_large:
.fill1_qwords:
    cmp rcx, 32
    jb .fill1_8
    mov [rbp], rax
    mov [rbp + 8], rax
    mov [rbp + 16], rax
    mov [rbp + 24], rax
    add rbp, 32
    sub rcx, 32
    jmp .fill1_qwords
.fill1_8:
    cmp rcx, 8
    jb .fill1_tail
    mov [rbp], rax
    add rbp, 8
    sub rcx, 8
    jmp .fill1_8
.fill1_tail:
    test rcx, rcx
    jz .copy_ret
    lea rdx, [rbp + 8]
    cmp rdx, r11
    ja .fill1_bytes
    mov [rbp + rcx - 8], rax
    add rbp, rcx
    jmp .after_match
.fill1_bytes:
    test rcx, rcx
    jz .copy_ret
    mov [rbp], al
    inc rbp
    dec rcx
    jmp .fill1_bytes

.match_short_bytes:
    /* Offsets 2..7 */
    mov al, [rsi]
    mov [rbp], al
    inc rsi
    inc rbp
    dec rcx
    jnz .match_short_bytes

.copy_ret:
    jmp .after_match

    .p2align 4
.read_elias_rest:
    /* First bit 0 was already consumed. Value starts at 1. */
    mov eax, 1
.elias_rest_loop:
    cmp eax, 0x20000000
    jae .fail_sub

    /* Read payload bit */
    add r8b, r8b
    jnz 2f
    cmp rdi, r9
    jae .fail_sub
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
2:
    adc eax, eax

    /* Read control bit */
    add r8b, r8b
    jnz 1f
    cmp rdi, r9
    jae .fail_sub
    mov r8b, [rdi]
    inc rdi
    stc
    adc r8b, r8b
1:
    jnc .elias_rest_loop
    ret



.eof_reached:
    mov rax, rbp
    sub rax, r10
    pop rbp
    pop rbx
    ret

.fail_sub:                          /* failure inside .read_elias_rest */
    pop rax                         /* drop its return address */
.fail:
    mov rax, -1
    pop rbp
    pop rbx
    ret

.fail_early:
    mov rax, -1
    ret
