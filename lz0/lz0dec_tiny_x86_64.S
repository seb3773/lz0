#if (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && defined(__x86_64__)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.intel_syntax noprefix
.text
.globl CSYM(lz0dec_tiny_x86_64)
#if defined(__ELF__)
.type CSYM(lz0dec_tiny_x86_64), @function
#endif

/*
 * Fast-path bit reader: shifts sentinel byte dl left; 87.5% of reads resolve
 * inline in 2 instructions without a call/ret, falling back to .refill once
 * per input byte.
 */
.macro RBIT
    add dl, dl
    jnz 99f
    call .refill
99:
.endm

/*
 * int lz0dec_tiny_x86_64(const unsigned char *in, int in_size,
 *                        unsigned char *out, int out_max);
 *
 * Size-optimized, contract-compliant LZ0 v0.1 assembly decompressor:
 * - Zero heap allocations, zero lookup tables (~288 B ELF .text).
 * - Full bounds validation matching lz0_decompress_to (guard-page safe).
 */
CSYM(lz0dec_tiny_x86_64):
#if defined(_WIN32)
    push rdi
    push rsi
    push rbp
    mov rbp, rsp

    test rcx, rcx
    jz .fail
    test edx, edx
    jle .fail
    test r8, r8
    jz .fail
    test r9d, r9d
    jle .fail

    movsxd rax, r9d
    mov rdi, r8                     /* out_ptr */
    mov r10, r8                     /* out_start */
    lea r11, [r8 + rax]             /* out_end */
    mov rsi, rcx                    /* in_ptr */
    movsxd rax, edx
    lea r9, [rcx + rax]             /* in_end */
#else
    push rbp
    mov rbp, rsp

    test rdi, rdi
    jz .fail
    test esi, esi
    jle .fail
    test rdx, rdx
    jz .fail
    test ecx, ecx
    jle .fail

    movsxd r9, esi
    add r9, rdi                     /* in_end */
    movsxd r11, ecx
    add r11, rdx                    /* out_end */
    mov rsi, rdi                    /* in_ptr */
    mov rdi, rdx                    /* out_ptr */
    mov r10, rdx                    /* out_start */
#endif
    push 1
    pop r8                          /* r8 = last_offset = 1 */
    mov dl, 0x80                    /* dl = sentinel bitreader */
    jmp .do_literal

.refill:
    cmp rsi, r9
    jae .fail
    mov dl, [rsi]
    inc rsi
    stc
    adc dl, dl
    ret

/* Read Elias gamma into eax; cl = 0 (normal) or cl = 1 (inverted). */
.elias1:
    mov cl, 1
    jmp .elias
.elias0:
    xor ecx, ecx
.elias:
    push 1
    pop rax
.el_lp:
    RBIT
    jc .el_ret
    bt eax, 29
    jc .fail
    RBIT
    adc eax, eax
    xor al, cl
    jmp .el_lp
.el_ret:
    ret

.fail:
    push -1
    pop rax
    leave
#if defined(_WIN32)
    pop rsi
    pop rdi
#endif
    ret

.do_literal:
    call .elias0                    /* rax = L (>= 1) */
    mov rcx, r9
    sub rcx, rsi
    cmp rax, rcx
    ja .fail
    mov rcx, r11
    sub rcx, rdi
    cmp rax, rcx
    ja .fail
    xchg ecx, eax
    /* Avoid ERMS microcode startup penalty on single-byte literals */
    cmp ecx, 1
    ja 1f
    movsb
    jmp .after_lit
1:
    rep movsb

.after_lit:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    RBIT
    jc .enter_new_loop

.do_match_last:
    call .elias0                    /* rax = L (>= 1) */

.copy_match:
    mov rcx, rdi
    sub rcx, r10
    cmp r8, rcx
    ja .fail
    mov rcx, r11
    sub rcx, rdi
    cmp rax, rcx
    ja .fail
    xchg ecx, eax
    push rsi
    mov rsi, rdi
    sub rsi, r8
    rep movsb
    pop rsi

.after_match:
    /* Decision bit after Match: 0 -> LiteralRun, 1 -> enter new_loop */
    RBIT
    jnc .do_literal

.enter_new_loop:
    /* 0 -> EOF, 1 -> MatchNew */
    RBIT
    jnc .eof

    call .elias1                    /* rax = msb (>= 1) */
    cmp rsi, r9
    jae .fail
    shl rax, 8
    xchg r8, rax
    xor eax, eax
    lodsb                           /* al = t, rsi++ */
    sub r8, rax                     /* r8 = msb * 256 - t (>= 1) */

    push 1
    pop rax
.len3_lp:
    RBIT
    adc eax, eax
    cmp al, 8
    jb .len3_lp
    cmp al, 15
    je .len3_long
    sub eax, 6                      /* L = x + 2 (2..8) */
    jmp .copy_match

.len3_long:
    call .elias0
    add eax, 8                      /* L = elias + 8 (>= 9) */
    jmp .copy_match

.eof:
    mov rax, rdi
    sub rax, r10
#if defined(_WIN32)
    pop rbp
    pop rsi
    pop rdi
#else
    pop rbp
#endif
    ret

#if defined(__ELF__)
.size CSYM(lz0dec_tiny_x86_64), .-CSYM(lz0dec_tiny_x86_64)
.section .note.GNU-stack,"",@progbits
#endif

#endif /* (__ELF__ || _WIN32 || __APPLE__) && __x86_64__ */
