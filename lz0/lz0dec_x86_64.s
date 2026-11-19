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

    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r14, rdi                    /* in_ptr */
    movsxd rax, esi
    lea r15, [rdi + rax]            /* in_end */

    mov r10, rdx                    /* out_start */
    mov r12, rdx                    /* out_ptr */
    movsxd rax, ecx
    lea r13, [rdx + rax]            /* out_end */

    mov rbx, 1                      /* last_offset = 1 */
    xor r8d, r8d                    /* bit_val = 0 */
    xor r9d, r9d                    /* bits_left = 0 */

    jmp .do_literal

    .p2align 4
.main_loop_after_literal:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jc .enter_new_loop

.do_match_last:
    /* Fast-path Elias == 1 (most frequent case: length is 1) */
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jnc .match_last_full_elias
    mov ecx, 1
    jmp .match_last_have_len

.match_last_full_elias:
    call .read_elias_rest
    mov rcx, rax

.match_last_have_len:
    test rbx, rbx
    jle .fail

    mov rsi, r12
    sub rsi, rbx
    cmp rsi, r10
    jb .fail

    mov rax, r13
    sub rax, r12
    cmp rcx, rax
    ja .fail

    call .copy_match_fast

    /* After MatchLast: read decision bit (0 -> LiteralRun, 1 -> enter new_loop) */
    dec r9d
    jns 2f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
2:
    shl r8b, 1
    jc .enter_new_loop

.do_literal:
    /* Fast-path Elias == 1 (single-byte literal run) */
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jnc .lit_full_elias
    mov ecx, 1
    jmp .lit_have_len

.lit_full_elias:
    call .read_elias_rest
    mov rcx, rax

.lit_have_len:
    mov rax, r13
    sub rax, r12
    cmp rcx, rax
    ja .fail

    mov rax, r15
    sub rax, r14
    cmp rcx, rax
    ja .fail

    /* Fast literal copy */
    test rcx, rcx
    jle .fail

    cmp rcx, 8
    ja .lit_large

    /* Short literal (1..8 bytes) */
    lea rax, [r12 + 8]
    cmp rax, r13
    ja .lit_byte_loop
    lea rax, [r14 + 8]
    cmp rax, r15
    ja .lit_byte_loop

    mov rax, [r14]
    mov [r12], rax
    add r14, rcx
    add r12, rcx
    jmp .main_loop_after_literal

.lit_large:
    /* Large literal (>8 bytes) */
.lit_large_loop:
    cmp rcx, 8
    jb .lit_large_tail
    mov rax, [r14]
    mov [r12], rax
    add r14, 8
    add r12, 8
    sub rcx, 8
    jmp .lit_large_loop

.lit_large_tail:
    test rcx, rcx
    jz .main_loop_after_literal

    lea rax, [r12 + 8]
    cmp rax, r13
    ja .lit_byte_loop
    lea rax, [r14 + 8]
    cmp rax, r15
    ja .lit_byte_loop

    mov rax, [r14 + rcx - 8]
    mov [r12 + rcx - 8], rax
    add r14, rcx
    add r12, rcx
    jmp .main_loop_after_literal

.lit_byte_loop:
    test rcx, rcx
    jz .main_loop_after_literal
    mov al, [r14]
    mov [r12], al
    inc r14
    inc r12
    dec rcx
    jmp .lit_byte_loop

    .p2align 4
.enter_new_loop:
    /* Read 1 bit: 0 -> EOF, 1 -> MatchNew */
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jnc .eof_reached

    /* Bit 1: MatchNew */
    call .read_elias_inv
    test eax, eax
    jle .fail

    cmp r14, r15
    jae .fail
    movzx edx, byte ptr [r14]
    inc r14

    mov rbx, rax
    shl rbx, 8
    sub rbx, rdx

    test rbx, rbx
    jle .fail
    mov rax, r12
    sub rax, r10
    cmp rbx, rax
    ja .fail

    /* 3 bits length */
    xor edx, edx
    dec r9d
    jns 2f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
2:
    shl r8b, 1
    adc edx, edx

    dec r9d
    jns 3f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
3:
    shl r8b, 1
    adc edx, edx

    dec r9d
    jns 4f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
4:
    shl r8b, 1
    adc edx, edx

    cmp edx, 7
    jae .new_len_elias
    add edx, 2
    mov rcx, rdx
    jmp .new_len_ready

.new_len_elias:
    call .read_elias
    lea rcx, [rax + 8]

.new_len_ready:
    mov rax, r13
    sub rax, r12
    cmp rcx, rax
    ja .fail

    mov rsi, r12
    sub rsi, rbx

    call .copy_match_fast

    /* Decision bit after MatchNew */
    dec r9d
    jns 5f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
5:
    shl r8b, 1
    jnc .do_literal
    jmp .enter_new_loop

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
    lea rax, [r12 + 8]
    cmp rax, r13
    ja .match_byte_loop
    mov rax, [rsi]
    mov [r12], rax
    add r12, rcx
    ret

.match_large:
    /* Large match (>8 bytes) with offset >= 8 */
.match_large_loop:
    cmp rcx, 8
    jb .match_large_tail
    mov rax, [rsi]
    mov [r12], rax
    add rsi, 8
    add r12, 8
    sub rcx, 8
    jmp .match_large_loop

.match_large_tail:
    test rcx, rcx
    jz .copy_ret
    lea rax, [r12 + 8]
    cmp rax, r13
    ja .match_byte_loop
    mov rax, [rsi + rcx - 8]
    mov [r12 + rcx - 8], rax
    add r12, rcx
    ret

.match_byte_loop:
    test rcx, rcx
    jz .copy_ret
    mov al, [rsi]
    mov [r12], al
    inc rsi
    inc r12
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

    lea rdx, [r12 + 8]
    cmp rdx, r13
    ja .fill1_bytes
    mov [r12], rax
    add r12, rcx
    ret

.fill1_large:
.fill1_qwords:
    cmp rcx, 32
    jb .fill1_8
    mov [r12], rax
    mov [r12 + 8], rax
    mov [r12 + 16], rax
    mov [r12 + 24], rax
    add r12, 32
    sub rcx, 32
    jmp .fill1_qwords
.fill1_8:
    cmp rcx, 8
    jb .fill1_tail
    mov [r12], rax
    add r12, 8
    sub rcx, 8
    jmp .fill1_8
.fill1_tail:
    test rcx, rcx
    jz .copy_ret
    lea rdx, [r12 + 8]
    cmp rdx, r13
    ja .fill1_bytes
    mov [r12 + rcx - 8], rax
    add r12, rcx
    ret
.fill1_bytes:
    test rcx, rcx
    jz .copy_ret
    mov [r12], al
    inc r12
    dec rcx
    jmp .fill1_bytes

.match_short_bytes:
    /* Offsets 2..7 */
    mov al, [rsi]
    mov [r12], al
    inc rsi
    inc r12
    dec rcx
    jnz .match_short_bytes

.copy_ret:
    ret

    .p2align 4
.read_elias_rest:
    /* First bit 0 was already consumed. Value starts at 1. */
    mov eax, 1
.elias_rest_loop:
    cmp eax, 0x20000000
    jae .fail

    /* Read payload bit */
    dec r9d
    jns 2f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
2:
    shl r8b, 1
    adc eax, eax

    /* Read control bit */
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jnc .elias_rest_loop
    ret

    .p2align 4
.read_elias:
    mov eax, 1
.elias_loop:
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jc .elias_done

    cmp eax, 0x20000000
    jae .fail

    dec r9d
    jns 2f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
2:
    shl r8b, 1
    adc eax, eax
    jmp .elias_loop
.elias_done:
    ret

    .p2align 4
.read_elias_inv:
    mov eax, 1
.elias_inv_loop:
    dec r9d
    jns 1f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
1:
    shl r8b, 1
    jc .elias_inv_done

    cmp eax, 0x20000000
    jae .fail

    dec r9d
    jns 2f
    cmp r14, r15
    jae .fail
    mov r8b, [r14]
    inc r14
    mov r9d, 7
2:
    shl r8b, 1
    cmc
    adc eax, eax
    jmp .elias_inv_loop
.elias_inv_done:
    ret

.eof_reached:
    mov rax, r12
    sub rax, r10
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

.fail:
    mov rax, -1
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

.fail_early:
    mov rax, -1
    ret
