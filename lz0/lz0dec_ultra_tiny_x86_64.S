#if (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && defined(__x86_64__)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.intel_syntax noprefix
.text
.globl CSYM(lz0dec_ultra_tiny_x86_64)
#if defined(__ELF__)
.type CSYM(lz0dec_ultra_tiny_x86_64), @function
#endif

/*
 * int lz0dec_ultra_tiny_x86_64(const unsigned char *in, unsigned char *out);
 *
 * Extreme size-optimized trusted-stream LZ0 v0.1 assembly decompressor:
 * - Zero heap allocations, zero lookup tables (~142 B ELF .text).
 * - Unchecked trusted-input contract: caller guarantees a valid non-empty
 *   LZ0 v0.1 stream and sufficient output capacity.
 */
CSYM(lz0dec_ultra_tiny_x86_64):
#if defined(_WIN32)
    push rbx
    push rsi
    push rdi
    push rdx                        /* out_start */
    mov rsi, rcx                    /* in_ptr */
    mov rdi, rdx                    /* out_ptr */
#else
    push rbx
    push rsi                        /* out_start */
    xchg rdi, rsi                   /* rsi = in_ptr, rdi = out_ptr */
#endif
    push 1
    pop rbx                         /* rbx = last_offset = 1 */
    mov dl, 0x80                    /* dl = sentinel bitreader */

.do_literal:
    call .elias0                    /* eax = L (>= 1) */
    xchg ecx, eax
    rep movsb

.after_lit:
    /* Decision bit after LiteralRun: 0 -> MatchLast, 1 -> enter new_loop */
    call .bit
    jc .enter_new_loop

.do_match_last:
    call .elias0                    /* eax = L (>= 1) */

.copy_match:
    xchg ecx, eax
    push rsi
    mov rsi, rdi
    sub rsi, rbx
    rep movsb
    pop rsi

.after_match:
    /* Decision bit after Match: 0 -> LiteralRun, 1 -> enter new_loop */
    call .bit
    jnc .do_literal

.enter_new_loop:
    /* 0 -> EOF, 1 -> MatchNew */
    call .bit
    jnc .eof

    call .elias1                    /* eax = msb (>= 1) */
    shl eax, 8
    xchg ebx, eax
    xor eax, eax
    lodsb                           /* al = t, rsi++ */
    sub ebx, eax                    /* rbx = msb * 256 - t (>= 1) */

    push 32
    pop rax                         /* 3-bit sentinel for len3 (x = 0..7) */
.len3_lp:
    call .bit
    adc al, al
    jnc .len3_lp
    add al, 2                       /* L = x + 2 (2..8), or 9 if x == 7 */
    cmp al, 9
    jb .copy_match
    call .elias0
    add eax, 8                      /* L = elias + 8 (>= 9) */
    jmp .copy_match

.eof:
    pop rcx                         /* out_start */
    sub rdi, rcx
    xchg eax, edi                   /* return decompressed size */
#if defined(_WIN32)
    pop rdi
    pop rsi
#endif
    pop rbx
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
    call .bit
    jc .bit_ret
    call .bit
    adc eax, eax
    xor al, cl
    jmp .el_lp

/*
 * Shift next bit into CF. On sentinel exhaustion (ZF=1 after add dl, dl),
 * the sentinel bit itself enters CF=1 and is preserved across mov/inc!
 */
.bit:
    add dl, dl
    jnz .bit_ret
    mov dl, [rsi]
    inc rsi
    adc dl, dl
.bit_ret:
    ret

#if defined(__ELF__)
.size CSYM(lz0dec_ultra_tiny_x86_64), .-CSYM(lz0dec_ultra_tiny_x86_64)
.section .note.GNU-stack,"",@progbits
#endif

#endif /* (__ELF__ || _WIN32 || __APPLE__) && __x86_64__ */

