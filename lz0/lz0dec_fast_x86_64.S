#if (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && defined(__x86_64__)

#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.intel_syntax noprefix
/*
 * int lz0dec_fast_x86_64(const unsigned char *in, int in_size,
 *                   unsigned char *out, int out_max);
 *
 * LZ0 decoder for x86-64 (SysV ABI). Baseline ISA only (SSE2); no BMI/AVX.
 *
 * Two engines share one register state:
 *
 *  FAST engine (while in_ptr <= in_end - 16):
 *    A "bit segment" is the run of control bits between two raw-byte reads.
 *    Each segment loads ONE 64-bit MSB-first window (load+bswap+shift+or),
 *    decodes all of its fields with register shifts, and commits the bit
 *    count back to in_ptr just before the next raw read. No per-bit refill
 *    branch, no per-bit counter dependency.
 *    Interleaved Elias-gamma codes with n <= 8 payload bits decode branch-free:
 *    BSR on (window & control-mask) gives the length, two lookups in a
 *    256-byte table give the payload bits. Offset MSB codes with
 *    9 <= n <= 15 (offsets up to 8 MiB) also stay in the window (four
 *    lookups, .Ff_new), so far matches never fall back to the SAFE engine. Copies are 16-byte SSE2 wildcopies behind ONE "end <= limit-16"
 *    compare per token.
 *
 *  SAFE engine (last 16 input bytes, last 16 output bytes, length codes
 *  n > 8, offset codes n > 15):
 *    Sentinel bit reader: r8b = [remaining bits][1][0..]; a single ADD gives
 *    the bit in CF and flags exhaustion in ZF. Exact, overrun-free copies.
 *
 * Every check performed by lz0dec_ref is preserved; results are bit-exact.
 *
 * Registers:
 *   r14 in_ptr        r9  in_end - 16      [rsp]   in_end
 *   r12 out_ptr       r11 out_end - 16     [rsp+8] out_end
 *   r10 out_start     rbx last_offset      r13 0xAAAA800000000000 (Elias ctl mask)
 *   r8  fast: leftover bits of current byte, left-aligned
 *       safe: sentinel bit byte (bits 8..63 zero)
 *   rbp fast: bit counter q (segment start = 7 - leftover bits)
 *   rdi fast: bit window
 *   r15 base of .Lunzip (256-byte Elias payload table, .Ls_tab at +256)
 *   rax rcx rdx rsi : scratch
 */

/* ======================= SAFE engine macros ======================= */

/* one bit -> CF */
.macro GETBIT
    add r8b, r8b
    jnz 7771f
    cmp r14, qword ptr [rsp]
    jae .Lfail
    mov r8b, byte ptr [r14]
    inc r14
    stc
    adc r8b, r8b
7771:
.endm

/* interleaved Elias gamma -> \reg ; inv=1 inverts payload bits */
.macro ELIAS_SLOW reg, inv
    mov \reg, 1
7772:
    GETBIT
    jc 7773f
    cmp \reg, 0x20000000
    jae .Lfail
    GETBIT
  .if \inv
    cmc
  .endif
    adc \reg, \reg
    jmp 7772b
7773:
.endm

/* ======================= FAST engine macros ======================= */

/* rdi <- window = leftover bits ++ bytes at r14. Needs r14+8 <= in_end. */
.macro WINDOW
    mov ecx, ebp
    xor ecx, 7                  /* k = leftover bit count */
    mov rdi, qword ptr [r14]
    bswap rdi
    shr rdi, cl
    or rdi, r8
.endm

/* r14 += q/8 ; r8 = leftover bits (top k' of rdi) ; rbp = q & 7 */
.macro COMMIT
    mov eax, ebp
    shr eax, 3
    add r14, rax
    mov ecx, ebp
    not ecx
    and ecx, 7                  /* k' */
    and ebp, 7
    mov r8, -1
    shr r8, cl
    not r8
    and r8, rdi
.endm

/* fast state -> safe sentinel byte (after COMMIT) */
.macro TO_SAFE
    shr r8, 56
    bts r8d, ebp
.endm

/* safe sentinel byte -> fast state */
.macro TO_FAST
    bsf ebp, r8d
    btr r8d, ebp
    shl r8, 56
.endm

/* Branch-free Elias gamma, n <= 8, from top of rdi -> \reg (consumed).
   n > 8 -> jump to \fb with rdi/rbp untouched. Clobbers rax rcx rdx. */
.macro ELIAS_FAST reg, inv, fb
    mov rdx, rdi
    and rdx, r13                /* control bits c0..c8          */
    jz \fb
    mov rax, rdi
    shr rax, 48                 /* c0 d1 c1 d2 c2 d3 c3 d4 | c4 d5 .. c7 d8 */
    movzx ecx, ah
    movzx eax, al
    movzx ecx, byte ptr [r15 + rcx]   /* d1..d4 */
    movzx eax, byte ptr [r15 + rax]   /* d5..d8 */
    shl ecx, 4
    or eax, ecx
  .if \inv
    xor eax, 0x1FF              /* invert payload, set leading 1 */
  .else
    or eax, 0x100               /* leading 1                    */
  .endif
    bsr rdx, rdx                /* b = 63 - 2n                  */
    mov ecx, 64
    sub ecx, edx                /* consumed = 2n + 1            */
    shl rdi, cl
    add rbp, rcx
    lea ecx, [rdx - 47]
    shr ecx, 1                  /* 8 - n                        */
    shr eax, cl
    mov \reg, rax
.endm

/* Match copy, rcx = len, rbx = offset (validated). Falls through when done
   to .F_decide (caller falls into or jumps to .F_decide); jumps to .Ff_copy
   (nothing written) if within 16 bytes of out_end. Clobbers rax rdx rsi. */
.macro COPY_FAST
    lea rax, [r12 + rcx]
    cmp rax, r11
    ja .Ff_copy
    mov rsi, r12
    sub rsi, rbx
    cmp rbx, 16
    jb .Lcopy_cold_lt16
    movdqu xmm0, xmmword ptr [rsi]
    movdqu xmmword ptr [r12], xmm0
    cmp ecx, 16
    ja .Lcopy_cold_gt16
    mov r12, rax
.endm

/* ============================ entry ============================ */

    .text
    .globl CSYM(lz0dec_fast_x86_64)
#if defined(__ELF__)
    .type CSYM(lz0dec_fast_x86_64), @function
#endif
    .p2align 5
CSYM(lz0dec_fast_x86_64):
#if defined(_WIN32)
    test rcx, rcx
    jz .Lfail_early
    test edx, edx
    jle .Lfail_early
    test r8, r8
    jz .Lfail_early
    test r9d, r9d
    jle .Lfail_early

    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16

    mov r14, rcx                /* in_ptr */
    movsxd rax, edx
    add rax, rcx
    mov qword ptr [rsp], rax    /* in_end */
    movsxd r11, r9d             /* load out_max before overwriting r9 */
    lea r9, [rax - 16]          /* in_safe_end */
    mov r10, r8                 /* out_start */
    mov r12, r8                 /* out_ptr */
    add r11, r8
    mov qword ptr [rsp + 8], r11 /* out_end */
    sub r11, 16                 /* out_safe_end */
#else
    test rdi, rdi
    jz .Lfail_early
    test esi, esi
    jle .Lfail_early
    test rdx, rdx
    jz .Lfail_early
    test ecx, ecx
    jle .Lfail_early

    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16

    mov r14, rdi
    movsxd rax, esi
    add rax, rdi
    mov qword ptr [rsp], rax    /* in_end */
    lea r9, [rax - 16]
    mov r10, rdx
    mov r12, rdx
    movsxd rax, ecx
    add rax, rdx
    mov qword ptr [rsp + 8], rax /* out_end */
    lea r11, [rax - 16]
#endif
    mov r13, 0xAAAA800000000000
    lea r15, [rip + .Lunzip]
    mov ebx, 1                  /* last_offset = 1 */
    mov r8d, 0x80               /* empty bit buffer */

    /* first token is always a LiteralRun */
    cmp r14, r9
    ja .S_literal
    TO_FAST
    WINDOW
    jmp .F_lit

/* ========================= FAST ENGINE ========================= */

    .p2align 5
.F_mn:
    /* MatchNew; rdi positioned at the inverted-Elias offset MSB */
    ELIAS_FAST rsi, 1, .Ff_new
.F_mn_have_msb:
    COMMIT
    cmp r14, qword ptr [rsp]    /* provably in bounds; kept as defence */
    jae .Lfail
    movzx edx, byte ptr [r14]
    inc r14
    shl rsi, 8
    sub rsi, rdx                /* offset = msb*256 - lsb (>= 1) */
    mov rbx, rsi
    mov rax, r12
    sub rax, r10
    cmp rbx, rax                /* offset <= out_pos */
    ja .Lfail
    cmp r14, r9
    ja .Ff_seg_b
.F_seg_b:
    WINDOW
    mov rcx, rdi
    shr rcx, 61                 /* 3-bit length code */
    shl rdi, 3                  /* consume length code bits up front */
    add rbp, 3
    cmp ecx, 7
    je .F_len_elias
    add ecx, 2
    COPY_FAST
    jmp .F_decide

    .p2align 5
.F_decide:
    inc rbp
    add rdi, rdi
    jnc .F_lit
.F_new_enter:
    inc rbp
    add rdi, rdi
    jc .F_mn
    jmp .Leof

    .p2align 4
.F_lit:
    test rdi, rdi
    jns .F_lit_gen
    add rdi, rdi                /* Elias == 1 */
    inc rbp
    mov esi, 1
.F_lit_have:
    COMMIT
    lea rax, [r14 + rsi]
    cmp rax, r9
    ja .Ff_lit_slow
    lea rdx, [r12 + rsi]
    cmp rdx, r11
    ja .Ff_lit_slow
    movdqu xmm0, xmmword ptr [r14]
    movdqu xmmword ptr [r12], xmm0
    cmp rsi, 16
    ja .F_lit_long
    mov r14, rax
    mov r12, rdx

.F_after_lit:                   /* requires r14 <= in_end - 16 */
    WINDOW
    inc rbp
    add rdi, rdi
    jc .F_new_enter
    /* MatchLast; offset needs no check: last_offset <= out_pos always */
    test rdi, rdi
    jns .F_ml_gen
    add rdi, rdi                /* Elias == 1 */
    inc rbp
    mov ecx, 1
.F_copy:
    COPY_FAST
    jmp .F_decide

    .p2align 4
.F_ml_gen:
    ELIAS_FAST rcx, 0, .Ff_ml
    jmp .F_copy

.F_lit_gen:
    ELIAS_FAST rsi, 0, .Ff_lit
    jmp .F_lit_have

.F_len_elias:
    ELIAS_FAST rcx, 0, .Ff_len
    add ecx, 8
    jmp .F_copy

.F_lit_long:                    /* first 16 done; margins guaranteed */
    mov rcx, 16
1:
    movdqu xmm0, xmmword ptr [r14 + rcx]
    movdqu xmmword ptr [r12 + rcx], xmm0
    add rcx, 16
    cmp rcx, rsi
    jb 1b
    mov r14, rax
    mov r12, rdx
    jmp .F_after_lit

/* ---- offset MSB with 9 <= n <= 15 payload bits: still windowed ----
   Far offsets (> 128 KiB) are common on multi-MiB inputs. Decoding them in
   the window (4 table lookups) avoids a FAST->SAFE->FAST round trip and a
   bit-serial loop. The window always holds >= 31 unread bits here (at most
   22 bits are consumed before an offset code), and bits shifted in below
   the valid region are zero, so a stop bit found by BSR is always genuine. */
.Ff_new:
    mov rdx, 0xAAAAAAAA00000000 /* control bits c0..c15 */
    and rdx, rdi
    jz .Ff_new_safe             /* n > 15 */
    mov rax, rdi
    shr rax, 32                 /* eax = b0 b1 b2 b3 (b0 = top byte) */
    movzx ecx, al
    movzx esi, byte ptr [r15 + rcx]   /* d13..d16 */
    movzx ecx, ah
    movzx ecx, byte ptr [r15 + rcx]   /* d9..d12 */
    shl ecx, 4
    or esi, ecx
    shr eax, 16
    movzx ecx, al
    movzx ecx, byte ptr [r15 + rcx]   /* d5..d8 */
    shl ecx, 8
    or esi, ecx
    movzx ecx, ah
    movzx ecx, byte ptr [r15 + rcx]   /* d1..d4 */
    shl ecx, 12
    or esi, ecx
    xor esi, 0x1FFFF            /* invert payload, set leading 1 */
    bsr rdx, rdx                /* b = 63 - 2n */
    mov ecx, 64
    sub ecx, edx                /* consumed = 2n + 1 */
    shl rdi, cl
    add rbp, rcx
    lea ecx, [rdx - 31]
    shr ecx, 1                  /* 16 - n */
    shr esi, cl
    jmp .F_mn_have_msb

/* ---- fast -> safe hand-offs: commit window, continue bit by bit ---- */
.Ff_new_safe:
    COMMIT
    TO_SAFE
    jmp .S_new_body

/* ---- MatchLast and MatchNew lengths with 9 <= n <= 15 payload bits ----
   Window holds >= 61 valid bits at MatchLast (only 1 bit consumed after WINDOW)
   and >= 58 valid bits at MatchNew length (only 3 bits consumed after WINDOW).
   Decoding 9..15 bits directly in the window avoids falling back to SAFE and
   enables 16-byte vectorized wildcopies for long matches. */
.Ff_ml:
    mov rdx, 0xAAAAAAAA00000000 /* control bits c0..c15 */
    and rdx, rdi
    jz .Ff_ml_safe
    pxor xmm1, xmm1             /* extra len = 0 */
    jmp .Ff_len_common

.Ff_len:
    mov rdx, 0xAAAAAAAA00000000 /* control bits c0..c15 */
    and rdx, rdi
    jz .Ff_len_safe
    mov eax, 8
    movd xmm1, eax              /* extra len = 8 */

.Ff_len_common:
    mov rax, rdi
    shr rax, 32                 /* eax = b0 b1 b2 b3 (b0 = top byte) */
    movzx ecx, al
    movzx esi, byte ptr [r15 + rcx]   /* d13..d16 */
    movzx ecx, ah
    movzx ecx, byte ptr [r15 + rcx]   /* d9..d12 */
    shl ecx, 4
    or esi, ecx
    shr eax, 16
    movzx ecx, al
    movzx ecx, byte ptr [r15 + rcx]   /* d5..d8 */
    shl ecx, 8
    or esi, ecx
    movzx ecx, ah
    movzx ecx, byte ptr [r15 + rcx]   /* d1..d4 */
    shl ecx, 12
    or esi, ecx
    or esi, 0x10000             /* set leading 1 (non-inverted) */
    bsr rdx, rdx                /* b = 63 - 2n */
    mov ecx, 64
    sub ecx, edx                /* consumed = 2n + 1 */
    shl rdi, cl
    add rbp, rcx
    lea ecx, [rdx - 31]
    shr ecx, 1                  /* 16 - n */
    shr esi, cl
    movd eax, xmm1
    lea rcx, [rsi + rax]
    jmp .F_copy

.Ff_len_safe:
    COMMIT
    TO_SAFE
    jmp .S_new_len_elias
.Ff_ml_safe:
    COMMIT
    TO_SAFE
    jmp .S_match_last
.Ff_lit:
    COMMIT
    TO_SAFE
    jmp .S_literal
.Ff_copy:
    mov rsi, rcx
    COMMIT
    TO_SAFE
    mov rcx, rsi
    jmp .S_copy
.Ff_lit_slow:                   /* already committed */
    TO_SAFE
    mov rcx, rsi
    jmp .S_lit_have_len
.Ff_seg_b:                      /* already committed */
    TO_SAFE
    jmp .S_new_len3

/* ========================= SAFE ENGINE ========================= */

    .p2align 4
.S_after_lit:
    cmp r14, r9
    jbe .S_to_fast_al
    GETBIT
    jc .S_new_enter
.S_match_last:
    ELIAS_SLOW rcx, 0
    jmp .S_copy

.S_new_enter:
    GETBIT
    jnc .Leof
.S_new_body:
    ELIAS_SLOW rax, 1
    cmp r14, qword ptr [rsp]
    jae .Lfail
    movzx edx, byte ptr [r14]
    inc r14
    shl rax, 8
    sub rax, rdx
    mov rbx, rax
    mov rax, r12
    sub rax, r10
    cmp rbx, rax
    ja .Lfail
    cmp r14, r9
    jbe .S_to_fast_sb
.S_new_len3:
    xor ecx, ecx
    GETBIT
    adc ecx, ecx
    GETBIT
    adc ecx, ecx
    GETBIT
    adc ecx, ecx
    cmp ecx, 7
    jae .S_new_len_elias
    add ecx, 2
    jmp .S_copy
.S_new_len_elias:
    ELIAS_SLOW rcx, 0
    add rcx, 8

/* exact match copy: rcx = len (>= 1), rbx = offset (validated) */
.S_copy:
    mov rax, qword ptr [rsp + 8]
    sub rax, r12
    cmp rcx, rax
    ja .Lfail
    mov rsi, r12
    sub rsi, rbx
    lea rax, [r12 + rcx]        /* end */
    cmp rbx, 1
    je .S_fill1
    cmp rbx, 8
    jb .S_copy_bytes
    cmp rcx, 8
    jb .S_copy_bytes
    lea rbp, [rax - 8]
1:                              /* 8-byte steps while > 8 remain */
    cmp r12, rbp
    jae 2f
    mov rdx, qword ptr [rsi]
    mov qword ptr [r12], rdx
    add rsi, 8
    add r12, 8
    jmp 1b
2:                              /* overlapping exact 8-byte tail */
    mov rdx, rax
    sub rdx, rbx
    mov rdx, qword ptr [rdx - 8]
    mov qword ptr [rax - 8], rdx
    mov r12, rax
    jmp .S_decide
.S_fill1:
    movzx edx, byte ptr [rsi]
    cmp rcx, 8
    jb .S_copy_bytes
    mov rsi, 0x0101010101010101
    imul rdx, rsi
    lea rbp, [rax - 8]
1:
    cmp r12, rbp
    jae 2f
    mov qword ptr [r12], rdx
    add r12, 8
    jmp 1b
2:
    mov qword ptr [rax - 8], rdx
    mov r12, rax
    jmp .S_decide
.S_copy_bytes:
    movzx edx, byte ptr [rsi]
    mov byte ptr [r12], dl
    inc rsi
    inc r12
    cmp r12, rax
    jb .S_copy_bytes
.S_decide:
    GETBIT
    jc .S_new_enter
.S_literal:
    ELIAS_SLOW rcx, 0
/* exact literal copy: rcx = len (>= 1) */
.S_lit_have_len:
    mov rax, qword ptr [rsp + 8]
    sub rax, r12
    cmp rcx, rax
    ja .Lfail
    mov rax, qword ptr [rsp]
    sub rax, r14
    cmp rcx, rax
    ja .Lfail
    lea rax, [r12 + rcx]
    cmp rcx, 16
    jb 3f
    lea rbp, [rax - 16]
1:
    cmp r12, rbp
    jae 2f
    movdqu xmm0, xmmword ptr [r14]
    movdqu xmmword ptr [r12], xmm0
    add r14, 16
    add r12, 16
    jmp 1b
2:                              /* overlapping exact 16-byte tail */
    mov rdx, rax
    sub rdx, r12                /* 1..16 bytes left */
    movdqu xmm0, xmmword ptr [r14 + rdx - 16]
    movdqu xmmword ptr [rax - 16], xmm0
    add r14, rdx
    mov r12, rax
    jmp .S_after_lit
3:
    movzx edx, byte ptr [r14]
    mov byte ptr [r12], dl
    inc r14
    inc r12
    cmp r12, rax
    jb 3b
    jmp .S_after_lit

.S_to_fast_al:
    TO_FAST
    jmp .F_after_lit
.S_to_fast_sb:
    TO_FAST
    jmp .F_seg_b

.Leof:
    mov rax, r12
    sub rax, r10
    jmp .Lret
.Lfail:
    or rax, -1
.Lret:
    add rsp, 16
    pop r15
    pop r14
    pop r13
    pop r12
#if defined(_WIN32)
    pop rsi
    pop rdi
#endif
    pop rbp
    pop rbx
    ret

    /* Shared cold / long-match / small-offset copy paths (single instance). */
#if defined(__ELF__)
    .subsection 1
#endif
.Lcopy_cold_gt16:               /* offset >= 16, len > 16 */
    add rsi, 16
    lea rdx, [r12 + 16]
1:
    movdqu xmm0, xmmword ptr [rsi]
    movdqu xmmword ptr [rdx], xmm0
    add rsi, 16
    add rdx, 16
    cmp rdx, rax
    jb 1b
    mov r12, rax
    jmp .F_decide
.Lcopy_cold_lt16:
    cmp rbx, 8
    jb 2f
    mov rdx, qword ptr [rsi]
    mov rsi, qword ptr [r12 - 8]
1:
    mov qword ptr [r12], rdx
    mov qword ptr [r12 + rbx - 8], rsi
    add r12, rbx
    cmp r12, rax
    jb 1b
    mov r12, rax
    jmp .F_decide
2:
    cmp rbx, 2
    ja 4f
    je 22f
    movzx edx, byte ptr [rsi]   /* offset 1: byte broadcast */
    mov rcx, 0x0101010101010101
    imul rdx, rcx
    movq xmm0, rdx
    punpcklqdq xmm0, xmm0
20:
    movdqu xmmword ptr [r12], xmm0
    add r12, 16
    cmp r12, rax
    jb 20b
    mov r12, rax
    jmp .F_decide
22:                             /* offset 2: halfword broadcast */
    movzx edx, word ptr [rsi]
    imul edx, edx, 0x00010001
    movd xmm0, edx
    pshufd xmm0, xmm0, 0
    jmp 20b
4:
    cmp rbx, 4
    jne 3f
    movd xmm0, dword ptr [rsi]  /* offset 4: dword broadcast */
    pshufd xmm0, xmm0, 0
    jmp 20b
3:                              /* offset p = 3, 5, 6, 7 */
    imul ecx, ebx, -8
    mov rdx, qword ptr [rsi]
    shl rdx, cl
    shr rdx, cl
    neg ecx
    mov rsi, rdx
    shl rsi, cl
    or rdx, rsi
    shl rsi, cl
    or rdx, rsi
    shl rsi, cl
    or rdx, rsi
    movzx ecx, byte ptr [r15 + rbx + 256]
1:
    mov qword ptr [r12], rdx
    add r12, rcx
    cmp r12, rax
    jb 1b
    mov r12, rax
    jmp .F_decide
#if defined(__ELF__)
    .subsection 0
#endif
.Lfail_early:
    mov eax, -1
    ret
#if defined(__ELF__)
    .size CSYM(lz0dec_fast_x86_64), .-CSYM(lz0dec_fast_x86_64)
#endif

#if defined(_WIN32)
    .section .rdata,"dr"
#elif defined(__APPLE__)
    .section __TEXT,__const
#else
    .section .rodata
#endif
    .p2align 6
.Lunzip:                        /* bits 6,4,2,0 of index -> 4-bit value */
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3
    .byte 4,5,4,5,6,7,6,7,4,5,4,5,6,7,6,7
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
    .byte 8,9,8,9,10,11,10,11,8,9,8,9,10,11,10,11
    .byte 12,13,12,13,14,15,14,15,12,13,12,13,14,15,14,15
.Ls_tab:                        /* .Lunzip + 256: s = p*floor(8/p) */
    .byte 0, 0, 8, 6, 8, 5, 6, 7

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif

#endif /* (__ELF__ || _WIN32 || __APPLE__) && __x86_64__ */

