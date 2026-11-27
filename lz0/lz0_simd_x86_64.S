/*
 * lz0_simd_x86_64.S - Hand-written SIMD kernels for the LZ0 encoder (x86-64).
 *
 * Encoder-side only: these kernels speed up searches/reductions performed by
 * the match finder and the DP parsers. They return exactly the same results
 * as the portable C references in lz0_simd.c (same index on ties, same
 * counts), so the compressed bitstream is bit-identical with or without them.
 *
 * Two tiers, selected at run time by lz0_simd_init():
 *   - SSE2 : x86-64 baseline, always available.
 *   - AVX2 : Haswell+ / Zen+ / Gracemont+.
 *
 * ABI: System V AMD64 (Linux, BSD, macOS) and Win64 (MinGW-w64 / PE64
 * wrapper saving rdi, rsi and xmm6..xmm8 where used). All functions use only
 * volatile registers across their body (leaf functions except Win64 wrapper),
 * never read outside [p, p + n) (tails use overlapping in-bounds loads or
 * scalar code, never over-reads), and AVX2 entry points execute VZEROUPPER
 * before returning.
 *
 * Exported functions (all "int" are 32-bit, n is the element count):
 *   int      lz0_find_u32_{sse2,avx2}  (const uint32_t *p, int n, uint32_t key)
 *            -> lowest i with p[i] == key, or -1.
 *   int      lz0_argmax_i32_{sse2,avx2}(const int32_t *p, int n)     n >= 1
 *            -> lowest i with p[i] == max(p[0..n)).
 *   int      lz0_find_off_{sse2,avx2}  (const lz0_match *m, int n, uint32_t off)
 *            -> lowest i with m[i].offset == off, or -1.
 *               lz0_match is { uint32_t offset; uint32_t length; } (8 bytes).
 *   int      lz0_kc_worst_{sse2,avx2}  (const int32_t *score, const lz0_match *m, int n)
 *            n >= 1 -> lowest i minimising (score[i], m[i].length)
 *            in lexicographic order (signed score, unsigned length).
 *   uint32_t lz0_count_eq_{sse2,avx2}  (const uint8_t *a, const uint8_t *b,
 *                                      uint32_t len, uint32_t step)
 *            step in {1, 2, 4} -> number of t = 0, step, 2*step, ... < len
 *            with a[t] == b[t].
 *   void     lz0_mru_batch_sse2(uint32_t mru[8], int *n_mru,
 *                               const lz0_match *m, int n)
 *            move-to-front of every non-zero m[i].offset with length >= 4,
 *            or length >= 3 and offset <= 512, into the 8-entry MRU list.
 *   int      lz0_matchlen_{sse2,avx2}  (const uint8_t *a, const uint8_t *b,
 *                                      int max)
 *            -> number of identical leading bytes, in [0, max] (0 if
 *               max <= 0); reads only a[0..max) and b[0..max).
 *   uint64_t lz0_bin40_hits_{sse2,avx2}(const uint8_t *p, uint32_t w0, uint32_t pm)
 *            -> bitmask b in [0, 40) of 2-byte prefix matches at bin offsets.
 *   void     lz0_u_row_prep_avx2       (lz0_u_prep *p)
 *            -> batch ultra-DP row prep (b_out, b2_out, masks, um[], h_lit[]).
 *   int      lz0_sa_scan_side_avx2     (lz0_sa_scan_args *a)
 *            -> batch suffix-array prefix/distance side scan (count <= 128).
 */

#if (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__)) && defined(__x86_64__)

        .text

/* Symbol helper: global but hidden (never exported from a shared object,
 * so internal tail-jumps stay direct), function type, 16-byte aligned. */
#if defined(__APPLE__)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

#define BODY(name) .Lbody_##name

#if defined(_WIN32)
/* Win64 -> SysV ABI wrapper preserving rdi, rsi (and xmm6..xmm8 for mru). */
#define FUNC(name)                  \
        .globl  name;               \
        .p2align 4;                 \
name:                               \
        pushq   %rdi;               \
        pushq   %rsi;               \
        movq    %rcx, %rdi;         \
        movq    %rdx, %rsi;         \
        movq    %r8,  %rdx;         \
        movq    %r9,  %rcx;         \
        call    BODY(name);         \
        popq    %rsi;               \
        popq    %rdi;               \
        ret;                        \
BODY(name):

#define FUNC_XMM8(name)             \
        .globl  name;               \
        .p2align 4;                 \
name:                               \
        pushq   %rdi;               \
        pushq   %rsi;               \
        subq    $56, %rsp;          \
        movdqa  %xmm6, (%rsp);      \
        movdqa  %xmm7, 16(%rsp);    \
        movdqa  %xmm8, 32(%rsp);    \
        movq    %rcx, %rdi;         \
        movq    %rdx, %rsi;         \
        movq    %r8,  %rdx;         \
        movq    %r9,  %rcx;         \
        call    BODY(name);         \
        movdqa  (%rsp), %xmm6;      \
        movdqa  16(%rsp), %xmm7;    \
        movdqa  32(%rsp), %xmm8;    \
        addq    $56, %rsp;          \
        popq    %rsi;               \
        popq    %rdi;               \
        ret;                        \
BODY(name):

#define ENDF(name)
#elif defined(__APPLE__)
#define FUNC(name)                  \
        .globl  CSYM(name);         \
        .private_extern CSYM(name); \
        .p2align 4;                 \
CSYM(name):                         \
BODY(name):

#define FUNC_XMM8(name) FUNC(name)
#define ENDF(name)
#else
#define FUNC(name)                  \
        .globl  name;               \
        .hidden name;               \
        .type   name, @function;    \
        .p2align 4;                 \
name:                               \
BODY(name):

#define FUNC_XMM8(name) FUNC(name)
#define ENDF(name) .size name, .-name
#endif

/* ------------------------------------------------------------------------
 * lz0_find_u32_sse2(p=rdi, n=esi, key=edx) -> eax
 * 8 keys per iteration (2 x 4 lanes), overlapping in-bounds tail.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_sse2)
        movslq  %esi, %rsi
        cmpq    $4, %rsi
        jl      .Lfu_s_small
        movd    %edx, %xmm0
        pshufd  $0, %xmm0, %xmm0            /* xmm0 = key x4 */
        xorl    %eax, %eax                  /* i = 0 */
        leaq    -8(%rsi), %rcx              /* last start of an 8-wide block */
        cmpq    %rcx, %rax
        jg      .Lfu_s_4
.Lfu_s_loop8:
        movdqu  (%rdi,%rax,4), %xmm1
        movdqu  16(%rdi,%rax,4), %xmm2
        pcmpeqd %xmm0, %xmm1
        pcmpeqd %xmm0, %xmm2
        movdqa  %xmm1, %xmm3
        por     %xmm2, %xmm3
        pmovmskb %xmm3, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit8
        addq    $8, %rax
        cmpq    %rcx, %rax
        jle     .Lfu_s_loop8
.Lfu_s_4:                                   /* 0..7 elements left */
        leaq    -4(%rsi), %rcx              /* rcx = n - 4 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfu_s_tail
        movdqu  (%rdi,%rax,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit
        addq    $4, %rax
.Lfu_s_tail:                                /* 0..3 left: overlap at n - 4 */
        cmpq    %rsi, %rax
        jge     .Lfu_s_none
        movq    %rcx, %rax
        movdqu  (%rdi,%rax,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit
.Lfu_s_none:
        movl    $-1, %eax
        ret
.Lfu_s_hit8:
        movmskps %xmm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit
        movmskps %xmm2, %edx
        addq    $4, %rax
.Lfu_s_hit:
        bsfl    %edx, %edx
        addl    %edx, %eax
        ret
.Lfu_s_small:                               /* n < 4: scalar */
        xorl    %eax, %eax
        testq   %rsi, %rsi
        jle     .Lfu_s_none
.Lfu_s_sloop:
        cmpl    %edx, (%rdi,%rax,4)
        je      .Lfu_s_sret
        incq    %rax
        cmpq    %rsi, %rax
        jl      .Lfu_s_sloop
        movl    $-1, %eax
.Lfu_s_sret:
        ret
ENDF(lz0_find_u32_sse2)

/* ------------------------------------------------------------------------
 * lz0_find_u32_avx2(p=rdi, n=esi, key=edx) -> eax
 * 16 keys per iteration (2 x 8 lanes), overlapping in-bounds tail.
 * n < 8 is delegated to the SSE2 kernel (no upper-lane state touched).
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_avx2)
        cmpl    $8, %esi
        jl      BODY(lz0_find_u32_sse2)
        movslq  %esi, %rsi
        vmovd   %edx, %xmm0
        vpbroadcastd %xmm0, %ymm0           /* ymm0 = key x8 */
        xorl    %eax, %eax
        leaq    -16(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lfu_a_8
.Lfu_a_loop16:
        vpcmpeqd (%rdi,%rax,4), %ymm0, %ymm1
        vpcmpeqd 32(%rdi,%rax,4), %ymm0, %ymm2
        vpor    %ymm1, %ymm2, %ymm3
        vptest  %ymm3, %ymm3
        jnz     .Lfu_a_hit16
        addq    $16, %rax
        cmpq    %rcx, %rax
        jle     .Lfu_a_loop16
.Lfu_a_8:                                   /* 0..15 left */
        leaq    -8(%rsi), %rcx              /* rcx = n - 8 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfu_a_tail
        vpcmpeqd (%rdi,%rax,4), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_a_hit
        addq    $8, %rax
.Lfu_a_tail:                                /* 0..7 left: overlap at n - 8 */
        cmpq    %rsi, %rax
        jge     .Lfu_a_none
        movq    %rcx, %rax
        vpcmpeqd (%rdi,%rax,4), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_a_hit
.Lfu_a_none:
        movl    $-1, %eax
        vzeroupper
        ret
.Lfu_a_hit16:
        vmovmskps %ymm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_a_hit
        vmovmskps %ymm2, %edx
        addq    $8, %rax
.Lfu_a_hit:
        bsfl    %edx, %edx
        addl    %edx, %eax
        vzeroupper
        ret
ENDF(lz0_find_u32_avx2)

/* ------------------------------------------------------------------------
 * Signed 32-bit max for SSE2 (no PMAXSD before SSE4.1):
 *   dst = max(dst, src), clobbers tmp.
 * ---------------------------------------------------------------------- */
#define SSE2_MAXSD(src, dst, tmp)   \
        movdqa  dst, tmp;           \
        pcmpgtd src, tmp;           /* tmp = dst > src */ \
        pand    tmp, dst;           \
        pandn   src, tmp;           \
        por     tmp, dst

#define SSE2_MINSD(src, dst, tmp)   \
        movdqa  src, tmp;           \
        pcmpgtd dst, tmp;           /* tmp = src > dst */ \
        pand    tmp, dst;           \
        pandn   src, tmp;           \
        por     tmp, dst

/* ------------------------------------------------------------------------
 * lz0_argmax_i32_sse2(p=rdi, n=esi) -> eax      (n >= 1)
 * Pass 1: vector max (overlapping tail is harmless for a max).
 * Pass 2: lowest index holding the max (tail-call into find_u32).
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_sse2)
        movslq  %esi, %rsi
        cmpq    $4, %rsi
        jl      .Lam_s_small
        movdqu  (%rdi), %xmm0
        movl    $4, %eax
        leaq    -4(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lam_s_tail
.Lam_s_loop:
        movdqu  (%rdi,%rax,4), %xmm1
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        addq    $4, %rax
        cmpq    %rcx, %rax
        jle     .Lam_s_loop
.Lam_s_tail:
        movdqu  (%rdi,%rcx,4), %xmm1        /* last 4, may overlap */
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0x4e, %xmm0, %xmm1
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0xb1, %xmm0, %xmm1
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        movd    %xmm0, %edx
        jmp     BODY(lz0_find_u32_sse2)     /* rdi, esi unchanged */
.Lam_s_small:                               /* n in 1..3 */
        xorl    %eax, %eax
        movl    (%rdi), %edx
        movl    $1, %ecx
.Lam_s_sloop:
        cmpq    %rsi, %rcx
        jge     .Lam_s_sret
        cmpl    %edx, (%rdi,%rcx,4)
        jle     .Lam_s_snext
        movl    (%rdi,%rcx,4), %edx
        movl    %ecx, %eax
.Lam_s_snext:
        incq    %rcx
        jmp     .Lam_s_sloop
.Lam_s_sret:
        ret
ENDF(lz0_argmax_i32_sse2)

/* ------------------------------------------------------------------------
 * lz0_argmax_i32_avx2(p=rdi, n=esi) -> eax      (n >= 1)
 * Two independent accumulators, 16 lanes per iteration.
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_avx2)
        cmpl    $8, %esi
        jl      BODY(lz0_argmax_i32_sse2)
        movslq  %esi, %rsi
        vmovdqu (%rdi), %ymm0
        vmovdqa %ymm0, %ymm1
        movl    $8, %eax
        leaq    -16(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lam_a_8
.Lam_a_loop16:
        vpmaxsd (%rdi,%rax,4), %ymm0, %ymm0
        vpmaxsd 32(%rdi,%rax,4), %ymm1, %ymm1
        addq    $16, %rax
        cmpq    %rcx, %rax
        jle     .Lam_a_loop16
.Lam_a_8:
        leaq    -8(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lam_a_tail
        vpmaxsd (%rdi,%rax,4), %ymm0, %ymm0
.Lam_a_tail:
        vpmaxsd (%rdi,%rcx,4), %ymm1, %ymm1 /* last 8, may overlap */
        vpmaxsd %ymm1, %ymm0, %ymm0
        vextracti128 $1, %ymm0, %xmm1
        vpmaxsd %xmm1, %xmm0, %xmm0
        vpshufd $0x4e, %xmm0, %xmm1
        vpmaxsd %xmm1, %xmm0, %xmm0
        vpshufd $0xb1, %xmm0, %xmm1
        vpmaxsd %xmm1, %xmm0, %xmm0
        vmovd   %xmm0, %edx
        jmp     BODY(lz0_find_u32_avx2)     /* n >= 8, VZEROUPPER done there */
ENDF(lz0_argmax_i32_avx2)

/* ------------------------------------------------------------------------
 * lz0_find_off_sse2(m=rdi, n=esi, off=edx) -> eax
 * Array of {offset, length} pairs: 2 entries per XMM, offset lanes are the
 * even dword lanes (movmskps bits 0 and 2).
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_sse2)
        movslq  %esi, %rsi
        cmpq    $2, %rsi
        jl      .Lfo_s_small
        movd    %edx, %xmm0
        pshufd  $0, %xmm0, %xmm0
        xorl    %eax, %eax
        leaq    -4(%rsi), %rcx              /* 4 entries (2 x XMM) per iteration */
        cmpq    %rcx, %rax
        jg      .Lfo_s_2
.Lfo_s_loop4:
        movdqu  (%rdi,%rax,8), %xmm1
        movdqu  16(%rdi,%rax,8), %xmm2
        pcmpeqd %xmm0, %xmm1
        pcmpeqd %xmm0, %xmm2
        movmskps %xmm1, %edx
        movmskps %xmm2, %r8d
        shll    $4, %r8d
        orl     %r8d, %edx
        andl    $0x55, %edx
        jnz     .Lfo_s_hit
        addq    $4, %rax
        cmpq    %rcx, %rax
        jle     .Lfo_s_loop4
.Lfo_s_2:                                   /* 0..3 entries left */
        leaq    -2(%rsi), %rcx              /* rcx = n - 2 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfo_s_tail
        movdqu  (%rdi,%rax,8), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        andl    $0x5, %edx
        jnz     .Lfo_s_hit
        addq    $2, %rax
.Lfo_s_tail:                                /* 0..1 left: overlap at n - 2 */
        cmpq    %rsi, %rax
        jge     .Lfo_s_none
        movq    %rcx, %rax
        movdqu  (%rdi,%rax,8), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        andl    $0x5, %edx
        jnz     .Lfo_s_hit
.Lfo_s_none:
        movl    $-1, %eax
        ret
.Lfo_s_hit:
        bsfl    %edx, %edx
        shrl    $1, %edx
        addl    %edx, %eax
        ret
.Lfo_s_small:                               /* n in {<=0, 1} */
        testq   %rsi, %rsi
        jle     .Lfo_s_none
        cmpl    %edx, (%rdi)
        jne     .Lfo_s_none
        xorl    %eax, %eax
        ret
ENDF(lz0_find_off_sse2)

/* ------------------------------------------------------------------------
 * lz0_find_off_avx2(m=rdi, n=esi, off=edx) -> eax
 * 4 entries per YMM, 8 per iteration; offset lanes = mask bits 0,2,4,6.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_avx2)
        cmpl    $4, %esi
        jl      BODY(lz0_find_off_sse2)
        movslq  %esi, %rsi
        vmovd   %edx, %xmm0
        vpbroadcastd %xmm0, %ymm0
        xorl    %eax, %eax
        leaq    -8(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lfo_a_4
.Lfo_a_loop8:
        vpcmpeqd (%rdi,%rax,8), %ymm0, %ymm1
        vpcmpeqd 32(%rdi,%rax,8), %ymm0, %ymm2
        vmovmskps %ymm1, %edx
        vmovmskps %ymm2, %r8d
        shll    $8, %r8d
        orl     %r8d, %edx
        andl    $0x5555, %edx
        jnz     .Lfo_a_hit
        addq    $8, %rax
        cmpq    %rcx, %rax
        jle     .Lfo_a_loop8
.Lfo_a_4:                                   /* 0..7 left */
        leaq    -4(%rsi), %rcx              /* rcx = n - 4 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfo_a_tail
        vpcmpeqd (%rdi,%rax,8), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        andl    $0x55, %edx
        jnz     .Lfo_a_hit
        addq    $4, %rax
.Lfo_a_tail:                                /* 0..3 left: overlap at n - 4 */
        cmpq    %rsi, %rax
        jge     .Lfo_a_none
        movq    %rcx, %rax
        vpcmpeqd (%rdi,%rax,8), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        andl    $0x55, %edx
        jnz     .Lfo_a_hit
.Lfo_a_none:
        movl    $-1, %eax
        vzeroupper
        ret
.Lfo_a_hit:
        bsfl    %edx, %edx
        shrl    $1, %edx
        addl    %edx, %eax
        vzeroupper
        ret
ENDF(lz0_find_off_avx2)

/* ------------------------------------------------------------------------
 * Shared tail of lz0_kc_worst_*: select, among the set bits of r9 (indices
 * whose score equals the minimum), the lowest index with the smallest
 * length. In: rsi = m, r9 = candidate mask (non-zero). Out: eax.
 * ---------------------------------------------------------------------- */
#define KC_PICK(pfx)                                    \
        bsfq    %r9, %rax;          /* best = lowest */ \
        movl    4(%rsi,%rax,8), %r10d; /* best length */\
        testl   %r10d, %r10d;                           \
        jz      pfx##_done;                             \
        leaq    -1(%r9), %r11;                          \
        andq    %r11, %r9;          /* clear lowest */  \
        jz      pfx##_done;                             \
pfx##_scan:                                             \
        bsfq    %r9, %rcx;                              \
        movl    4(%rsi,%rcx,8), %r11d;                  \
        cmpl    %r10d, %r11d;                           \
        jae     pfx##_next;         /* unsigned, strict */ \
        movl    %r11d, %r10d;                           \
        movl    %ecx, %eax;                             \
        testl   %r10d, %r10d;                           \
        jz      pfx##_done;                             \
pfx##_next:                                             \
        leaq    -1(%r9), %r11;                          \
        andq    %r11, %r9;                              \
        jnz     pfx##_scan;                             \
pfx##_done:

/* ------------------------------------------------------------------------
 * lz0_kc_worst_sse2(score=rdi, m=rsi, n=edx) -> eax      (1 <= n <= 64)
 * Pass 1: minimum score. Pass 2: 64-bit mask of indices with that score.
 * Pass 3 (KC_PICK): smallest length among them, lowest index on ties.
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_sse2)
        movslq  %edx, %rdx
        cmpq    $4, %rdx
        jl      .Lkw_s_small
        movdqu  (%rdi), %xmm0
        movl    $4, %eax
        leaq    -4(%rdx), %r10              /* r10 = n - 4 */
        cmpq    %r10, %rax
        jg      .Lkw_s_red
.Lkw_s_min:
        movdqu  (%rdi,%rax,4), %xmm1
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)
        addq    $4, %rax
        cmpq    %r10, %rax
        jle     .Lkw_s_min
.Lkw_s_red:
        movdqu  (%rdi,%r10,4), %xmm1        /* last 4, may overlap */
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0x4e, %xmm0, %xmm1
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0xb1, %xmm0, %xmm1
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)     /* xmm0 = min x4 */
        xorl    %r9d, %r9d                  /* r9 = equality mask */
        xorl    %ecx, %ecx                  /* rcx = i (also shift count) */
.Lkw_s_eq:
        cmpq    %r10, %rcx
        jg      .Lkw_s_eqtail
        movdqu  (%rdi,%rcx,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
        addq    $4, %rcx
        jmp     .Lkw_s_eq
.Lkw_s_eqtail:
        cmpq    %rdx, %rcx
        jge     .Lkw_s_pick
        movq    %r10, %rcx                  /* overlap at n - 4: OR is idempotent */
        movdqu  (%rdi,%rcx,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
.Lkw_s_pick:
        KC_PICK(.Lkw_s)
        ret
.Lkw_s_small:                               /* n in 1..3: scalar lexicographic min */
        xorl    %eax, %eax
        movl    (%rdi), %r8d                /* best score */
        movl    4(%rsi), %r9d               /* best length */
        movl    $1, %ecx
.Lkw_s_sloop:
        cmpq    %rdx, %rcx
        jge     .Lkw_s_sret
        movl    (%rdi,%rcx,4), %r10d
        cmpl    %r8d, %r10d
        jl      .Lkw_s_take                 /* signed score < best */
        jne     .Lkw_s_snext
        movl    4(%rsi,%rcx,8), %r11d
        cmpl    %r9d, %r11d
        jae     .Lkw_s_snext                /* unsigned length >= best */
.Lkw_s_take:
        movl    %r10d, %r8d
        movl    4(%rsi,%rcx,8), %r9d
        movl    %ecx, %eax
.Lkw_s_snext:
        incq    %rcx
        jmp     .Lkw_s_sloop
.Lkw_s_sret:
        ret
ENDF(lz0_kc_worst_sse2)

/* ------------------------------------------------------------------------
 * lz0_kc_worst_avx2(score=rdi, m=rsi, n=edx) -> eax      (1 <= n <= 64)
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_avx2)
        cmpl    $32, %edx
        je      .Lkw_a_32
        cmpl    $8, %edx
        jl      BODY(lz0_kc_worst_sse2)
        movslq  %edx, %rdx
        vmovdqu (%rdi), %ymm0
        movl    $8, %eax
        leaq    -8(%rdx), %r10              /* r10 = n - 8 */
        cmpq    %r10, %rax
        jg      .Lkw_a_red
.Lkw_a_min:
        vpminsd (%rdi,%rax,4), %ymm0, %ymm0
        addq    $8, %rax
        cmpq    %r10, %rax
        jle     .Lkw_a_min
.Lkw_a_red:
        vpminsd (%rdi,%r10,4), %ymm0, %ymm0 /* last 8, may overlap */
        vextracti128 $1, %ymm0, %xmm1
        vpminsd %xmm1, %xmm0, %xmm0
        vpshufd $0x4e, %xmm0, %xmm1
        vpminsd %xmm1, %xmm0, %xmm0
        vpshufd $0xb1, %xmm0, %xmm1
        vpminsd %xmm1, %xmm0, %xmm0
        vinserti128 $1, %xmm0, %ymm0, %ymm0 /* ymm0 = min x8 */
        xorl    %r9d, %r9d
        xorl    %ecx, %ecx
.Lkw_a_eq:
        cmpq    %r10, %rcx
        jg      .Lkw_a_eqtail
        vpcmpeqd (%rdi,%rcx,4), %ymm0, %ymm1
        vmovmskps %ymm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
        addq    $8, %rcx
        jmp     .Lkw_a_eq
.Lkw_a_eqtail:
        cmpq    %rdx, %rcx
        jge     .Lkw_a_pick
        movq    %r10, %rcx
        vpcmpeqd (%rdi,%rcx,4), %ymm0, %ymm1
        vmovmskps %ymm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
.Lkw_a_pick:
        vzeroupper
        KC_PICK(.Lkw_a)
        ret
.Lkw_a_32:
        vmovdqu (%rdi), %ymm0
        vmovdqu 32(%rdi), %ymm1
        vmovdqu 64(%rdi), %ymm2
        vmovdqu 96(%rdi), %ymm3
        vpminsd %ymm1, %ymm0, %ymm4
        vpminsd %ymm3, %ymm2, %ymm5
        vpminsd %ymm5, %ymm4, %ymm4
        vextracti128 $1, %ymm4, %xmm5
        vpminsd %xmm5, %xmm4, %xmm4
        vpshufd $0x4e, %xmm4, %xmm5
        vpminsd %xmm5, %xmm4, %xmm4
        vpshufd $0xb1, %xmm4, %xmm5
        vpminsd %xmm5, %xmm4, %xmm4
        vpbroadcastd %xmm4, %ymm4
        vpcmpeqd %ymm4, %ymm0, %ymm0
        vpcmpeqd %ymm4, %ymm1, %ymm1
        vpcmpeqd %ymm4, %ymm2, %ymm2
        vpcmpeqd %ymm4, %ymm3, %ymm3
        vmovmskps %ymm0, %r9d
        vmovmskps %ymm1, %eax
        vmovmskps %ymm2, %ecx
        vmovmskps %ymm3, %r8d
        shll    $8, %eax
        shll    $16, %ecx
        shll    $24, %r8d
        orl     %eax, %r9d
        orl     %ecx, %r9d
        orl     %r8d, %r9d
        vzeroupper
        KC_PICK(.Lkw_a32)
        ret
ENDF(lz0_kc_worst_avx2)

/* ------------------------------------------------------------------------
 * lz0_count_eq_sse2(a=rdi, b=rsi, len=edx, step=ecx) -> eax
 * Lane-select mask (bytes = 1 on the sampled positions) is loaded from a
 * table indexed by step. Per 16 bytes: PCMPEQB, PAND, PSADBW, PADDQ.
 * Vector blocks start at multiples of 16, which are multiples of the step,
 * so the sampling phase is preserved. The remainder (< 16 bytes) is scalar.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_sse2)
        movl    %edx, %edx                  /* zero-extend len */
        movl    %ecx, %ecx                  /* zero-extend step */
        leaq    .Lce_masks(%rip), %rax
        cmpl    $2, %ecx
        je      .Lce_s_m2
        cmpl    $4, %ecx
        je      .Lce_s_m4
        movdqa  (%rax), %xmm4               /* step 1 (and any other value: */
        jmp     .Lce_s_go                   /*  rejected by the C wrapper) */
.Lce_s_m2:
        movdqa  32(%rax), %xmm4
        jmp     .Lce_s_go
.Lce_s_m4:
        movdqa  64(%rax), %xmm4
.Lce_s_go:
        pxor    %xmm5, %xmm5                /* zero, for PSADBW */
        pxor    %xmm3, %xmm3                /* 2 x u64 accumulators */
        xorl    %r8d, %r8d                  /* t = 0 */
        leaq    -16(%rdx), %r9
.Lce_s_loop:
        cmpq    %r9, %r8
        jg      .Lce_s_rest
        movdqu  (%rdi,%r8), %xmm0
        movdqu  (%rsi,%r8), %xmm1
        pcmpeqb %xmm1, %xmm0
        pand    %xmm4, %xmm0
        psadbw  %xmm5, %xmm0
        paddq   %xmm0, %xmm3
        addq    $16, %r8
        jmp     .Lce_s_loop
.Lce_s_rest:
        pshufd  $0x4e, %xmm3, %xmm0
        paddq   %xmm0, %xmm3
        movd    %xmm3, %eax                 /* count <= len < 2^32 */
        /* scalar remainder: t = r8 (a multiple of 16, hence of step) */
.Lce_s_tail:
        cmpq    %rdx, %r8
        jae     .Lce_s_ret
        movzbl  (%rdi,%r8), %r9d
        cmpb    (%rsi,%r8), %r9b
        jne     .Lce_s_tnext
        incl    %eax
.Lce_s_tnext:
        addq    %rcx, %r8
        jmp     .Lce_s_tail
.Lce_s_ret:
        ret
ENDF(lz0_count_eq_sse2)

/* ------------------------------------------------------------------------
 * lz0_count_eq_avx2(a=rdi, b=rsi, len=edx, step=ecx) -> eax
 * 64 bytes per iteration with two accumulators.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_avx2)
        movl    %edx, %edx
        movl    %ecx, %ecx
        leaq    .Lce_masks(%rip), %rax
        cmpl    $2, %ecx
        je      .Lce_a_m2
        cmpl    $4, %ecx
        je      .Lce_a_m4
        vmovdqa (%rax), %ymm4
        jmp     .Lce_a_go
.Lce_a_m2:
        vmovdqa 32(%rax), %ymm4
        jmp     .Lce_a_go
.Lce_a_m4:
        vmovdqa 64(%rax), %ymm4
.Lce_a_go:
        vpxor   %xmm5, %xmm5, %xmm5
        vpxor   %xmm2, %xmm2, %xmm2
        vpxor   %xmm3, %xmm3, %xmm3
        xorl    %r8d, %r8d
        leaq    -64(%rdx), %r9
.Lce_a_loop64:
        cmpq    %r9, %r8
        jg      .Lce_a_32
        vmovdqu (%rdi,%r8), %ymm0
        vmovdqu 32(%rdi,%r8), %ymm1
        vpcmpeqb (%rsi,%r8), %ymm0, %ymm0
        vpcmpeqb 32(%rsi,%r8), %ymm1, %ymm1
        vpand   %ymm4, %ymm0, %ymm0
        vpand   %ymm4, %ymm1, %ymm1
        vpsadbw %ymm5, %ymm0, %ymm0
        vpsadbw %ymm5, %ymm1, %ymm1
        vpaddq  %ymm0, %ymm2, %ymm2
        vpaddq  %ymm1, %ymm3, %ymm3
        addq    $64, %r8
        jmp     .Lce_a_loop64
.Lce_a_32:
        leaq    -32(%rdx), %r9
        cmpq    %r9, %r8
        jg      .Lce_a_red
        vmovdqu (%rdi,%r8), %ymm0
        vpcmpeqb (%rsi,%r8), %ymm0, %ymm0
        vpand   %ymm4, %ymm0, %ymm0
        vpsadbw %ymm5, %ymm0, %ymm0
        vpaddq  %ymm0, %ymm2, %ymm2
        addq    $32, %r8
.Lce_a_red:
        vpaddq  %ymm3, %ymm2, %ymm2
        vextracti128 $1, %ymm2, %xmm3
        vpaddq  %xmm3, %xmm2, %xmm2
        vpshufd $0x4e, %xmm2, %xmm3
        vpaddq  %xmm3, %xmm2, %xmm2
        vmovd   %xmm2, %eax
        vzeroupper
.Lce_a_tail:                                /* r8 is a multiple of 32 */
        cmpq    %rdx, %r8
        jae     .Lce_a_ret
        movzbl  (%rdi,%r8), %r9d
        cmpb    (%rsi,%r8), %r9b
        jne     .Lce_a_tnext
        incl    %eax
.Lce_a_tnext:
        addq    %rcx, %r8
        jmp     .Lce_a_tail
.Lce_a_ret:
        ret
ENDF(lz0_count_eq_avx2)


/* ------------------------------------------------------------------------
 * lz0_mru_batch_sse2(mru=rdi, n_mru=rsi, m=rdx, n=ecx)
 * The list lives in xmm0 (lanes 0-3) and xmm1 (lanes 4-7) for the whole
 * batch. For each selected offset:
 *   idx  = lowest live lane holding off, else min(n_mru, 7) (and n_mru++
 *          if it was < 8)
 *   new  = lanes 0..idx from [off, m0, m1, ..., m6], lanes > idx unchanged
 * which is exactly the scalar move-to-front / insert-at-front of mru_add().
 * ---------------------------------------------------------------------- */
FUNC_XMM8(lz0_mru_batch_sse2)
        movdqu  (%rdi), %xmm0
        movdqu  16(%rdi), %xmm1
        movl    (%rsi), %r8d                /* n_mru */
        movdqa  .Lmru_lanes(%rip), %xmm6    /* {0,1,2,3} */
        movdqa  .Lmru_lanes+16(%rip), %xmm7 /* {4,5,6,7} */
        movslq  %ecx, %rcx
        xorl    %r9d, %r9d                  /* i */
.Lmb_loop:
        cmpq    %rcx, %r9
        jge     .Lmb_done
        movl    (%rdx,%r9,8), %r10d         /* offset */
        movl    4(%rdx,%r9,8), %eax         /* length */
        incq    %r9
        testl   %r10d, %r10d                /* offset 0 is never recorded */
        jz      .Lmb_loop
        cmpl    $4, %eax
        jae     .Lmb_take
        cmpl    $3, %eax
        jb      .Lmb_loop
        cmpl    $512, %r10d
        ja      .Lmb_loop
.Lmb_take:
        movd    %r10d, %xmm2
        pshufd  $0, %xmm2, %xmm2            /* off x4 */
        movd    %r8d, %xmm3
        pshufd  $0, %xmm3, %xmm3            /* n_mru x4 */
        movdqa  %xmm3, %xmm4
        pcmpgtd %xmm6, %xmm3                /* live lanes 0-3 */
        pcmpgtd %xmm7, %xmm4                /* live lanes 4-7 */
        movdqa  %xmm0, %xmm5
        pcmpeqd %xmm2, %xmm5
        pand    %xmm5, %xmm3
        movdqa  %xmm1, %xmm5
        pcmpeqd %xmm2, %xmm5
        pand    %xmm5, %xmm4
        movmskps %xmm3, %eax
        movmskps %xmm4, %r10d
        shll    $4, %r10d
        orl     %r10d, %eax
        jz      .Lmb_insert
        bsfl    %eax, %eax                  /* idx = found lane */
        jmp     .Lmb_shift
.Lmb_insert:
        movl    $7, %eax
        cmpl    $8, %r8d
        jge     .Lmb_shift                  /* full: drop lane 7 */
        movl    %r8d, %eax                  /* idx = n_mru */
        incl    %r8d
.Lmb_shift:
        incl    %eax
        movd    %eax, %xmm3
        pshufd  $0, %xmm3, %xmm3            /* idx + 1 */
        movdqa  %xmm3, %xmm4
        pcmpgtd %xmm6, %xmm3                /* M0: lanes 0-3 <= idx */
        pcmpgtd %xmm7, %xmm4                /* M1: lanes 4-7 <= idx */
        movdqa  %xmm1, %xmm5                /* s1 = [m3, m4, m5, m6] */
        pslldq  $4, %xmm5
        movdqa  %xmm0, %xmm2
        psrldq  $12, %xmm2
        por     %xmm2, %xmm5
        movdqa  %xmm0, %xmm2                /* s0 = [off, m0, m1, m2] */
        pslldq  $4, %xmm2
        movd    -8(%rdx,%r9,8), %xmm8       /* reload off into lane 0 */
        por     %xmm8, %xmm2
        pand    %xmm3, %xmm2                /* blend lanes 0-3 */
        pandn   %xmm0, %xmm3
        por     %xmm2, %xmm3
        movdqa  %xmm3, %xmm0
        pand    %xmm4, %xmm5                /* blend lanes 4-7 */
        pandn   %xmm1, %xmm4
        por     %xmm5, %xmm4
        movdqa  %xmm4, %xmm1
        jmp     .Lmb_loop
.Lmb_done:
        movdqu  %xmm0, (%rdi)
        movdqu  %xmm1, 16(%rdi)
        movl    %r8d, (%rsi)
        ret
ENDF(lz0_mru_batch_sse2)

/* ------------------------------------------------------------------------
 * int lz0_matchlen_sse2(a=rdi, b=rsi, max=edx) -> eax
 * Number of identical leading bytes, in [0, max] (0 if max <= 0).
 * 32 bytes per iteration (2 x PCMPEQB, PAND, one PMOVMSKB), then one
 * 16-byte block, then an overlapping in-bounds block ending at max (its
 * leading bytes are already known equal, so the first mismatch it reports
 * is at or after the current position). max < 16: two 8-byte words (the
 * second overlapping) or a byte loop. Never reads a[max] / b[max].
 * The mismatch masks fed to TZCNT are never zero; on pre-BMI1 CPUs TZCNT
 * decodes as BSF, which returns the same index for a non-zero source.
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_sse2)
        movslq  %edx, %rdx
        xorl    %eax, %eax                  /* n = 0 */
        cmpq    $16, %rdx
        jl      .Lml_s_lt16
        leaq    -32(%rdx), %rcx             /* last start of a 32-byte block */
        cmpq    %rcx, %rax
        jg      .Lml_s_16
.Lml_s_loop32:
        movdqu  (%rdi,%rax), %xmm0
        movdqu  16(%rdi,%rax), %xmm1
        movdqu  (%rsi,%rax), %xmm2
        movdqu  16(%rsi,%rax), %xmm3
        pcmpeqb %xmm2, %xmm0
        pcmpeqb %xmm3, %xmm1
        movdqa  %xmm0, %xmm2
        pand    %xmm1, %xmm2
        pmovmskb %xmm2, %r8d
        cmpl    $0xffff, %r8d
        jne     .Lml_s_hit32
        addq    $32, %rax
        cmpq    %rcx, %rax
        jle     .Lml_s_loop32
.Lml_s_16:                                  /* 0..31 bytes left, max >= 16 */
        leaq    -16(%rdx), %rcx             /* rcx = max - 16 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lml_s_tail
        movdqu  (%rdi,%rax), %xmm0
        movdqu  (%rsi,%rax), %xmm2
        pcmpeqb %xmm2, %xmm0
        pmovmskb %xmm0, %r8d
        xorl    $0xffff, %r8d               /* NOT (16 lanes) */
        jnz     .Lml_s_hit
        addq    $16, %rax
.Lml_s_tail:                                /* 0..15 left: overlap at max - 16 */
        cmpq    %rdx, %rax
        jge     .Lml_s_full
        movq    %rcx, %rax
        movdqu  (%rdi,%rax), %xmm0
        movdqu  (%rsi,%rax), %xmm2
        pcmpeqb %xmm2, %xmm0
        pmovmskb %xmm0, %r8d
        xorl    $0xffff, %r8d
        jnz     .Lml_s_hit
.Lml_s_full:
        movl    %edx, %eax
        ret
.Lml_s_hit32:
        pmovmskb %xmm0, %r8d
        xorl    $0xffff, %r8d
        jnz     .Lml_s_hit
        pmovmskb %xmm1, %r8d
        xorl    $0xffff, %r8d
        addq    $16, %rax
.Lml_s_hit:
        tzcntl  %r8d, %r8d
        addl    %r8d, %eax
        ret
.Lml_s_lt16:                                /* max < 16 */
        cmpq    $8, %rdx
        jl      .Lml_s_bytes
        movq    (%rdi), %r8
        xorq    (%rsi), %r8
        jnz     .Lml_s_qhit
        leaq    -8(%rdx), %rax              /* overlapping word at max - 8 */
        movq    (%rdi,%rax), %r8
        xorq    (%rsi,%rax), %r8
        jnz     .Lml_s_qhit
        movl    %edx, %eax
        ret
.Lml_s_qhit:
        tzcntq  %r8, %r8
        shrl    $3, %r8d
        addl    %r8d, %eax
        ret
.Lml_s_bytes:                               /* max < 8 (possibly <= 0) */
        cmpq    %rdx, %rax
        jge     .Lml_s_ret
        movzbl  (%rdi,%rax), %r8d
        cmpb    (%rsi,%rax), %r8b
        jne     .Lml_s_ret
        incl    %eax
        jmp     .Lml_s_bytes
.Lml_s_ret:
        ret
ENDF(lz0_matchlen_sse2)

/* ------------------------------------------------------------------------
 * int lz0_matchlen_avx2(a=rdi, b=rsi, max=edx) -> eax
 * 64 bytes per iteration (2 x VPCMPEQB with memory operands, VPAND, one
 * VPMOVMSKB; an all-equal mask is 0xffffffff, so INC sets ZF), then one
 * 32-byte block, then an overlapping in-bounds block ending at max.
 * Mismatch index = TZCNT(NOT mask). max < 32 tail-jumps to the SSE2 kernel
 * before any YMM register is touched.
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_avx2)
        cmpl    $32, %edx
        jl      BODY(lz0_matchlen_sse2)
        movslq  %edx, %rdx
        xorl    %eax, %eax
        leaq    -64(%rdx), %rcx             /* last start of a 64-byte block */
        cmpq    %rcx, %rax
        jg      .Lml_a_32
.Lml_a_loop64:
        vmovdqu (%rdi,%rax), %ymm0
        vmovdqu 32(%rdi,%rax), %ymm1
        vpcmpeqb (%rsi,%rax), %ymm0, %ymm0
        vpcmpeqb 32(%rsi,%rax), %ymm1, %ymm1
        vpand   %ymm1, %ymm0, %ymm2
        vpmovmskb %ymm2, %r8d
        incl    %r8d                        /* 0xffffffff -> 0 */
        jnz     .Lml_a_hit64
        addq    $64, %rax
        cmpq    %rcx, %rax
        jle     .Lml_a_loop64
.Lml_a_32:                                  /* 0..63 left, max >= 32 */
        leaq    -32(%rdx), %rcx             /* rcx = max - 32 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lml_a_tail
        vmovdqu (%rdi,%rax), %ymm0
        vpcmpeqb (%rsi,%rax), %ymm0, %ymm0
        vpmovmskb %ymm0, %r8d
        xorl    $-1, %r8d                   /* NOT, setting ZF */
        jnz     .Lml_a_hit
        addq    $32, %rax
.Lml_a_tail:                                /* 0..31 left: overlap at max - 32 */
        cmpq    %rdx, %rax
        jge     .Lml_a_full
        movq    %rcx, %rax
        vmovdqu (%rdi,%rax), %ymm0
        vpcmpeqb (%rsi,%rax), %ymm0, %ymm0
        vpmovmskb %ymm0, %r8d
        xorl    $-1, %r8d
        jnz     .Lml_a_hit
.Lml_a_full:
        movl    %edx, %eax
        vzeroupper
        ret
.Lml_a_hit64:
        vpmovmskb %ymm0, %r8d
        xorl    $-1, %r8d
        jnz     .Lml_a_hit
        vpmovmskb %ymm1, %r8d
        xorl    $-1, %r8d
        addq    $32, %rax
.Lml_a_hit:
        tzcntl  %r8d, %r8d
        addl    %r8d, %eax
        vzeroupper
        ret
ENDF(lz0_matchlen_avx2)

/* ------------------------------------------------------------------------
 * uint64_t lz0_bin40_hits_sse2(const uint8_t *p = rdi, uint32_t w0 = esi,
 *                              uint32_t pm = edx)
 * Bitmask b in [0, 40) of positions in lz0_bin_offsets[b] matching
 * (ld16(p + 256 - off) & (pm & 0xffff)) == (w0 & 0xffff).
 * Touches only xmm0..xmm5 (Win64 ABI safe).
 * ---------------------------------------------------------------------- */
FUNC(lz0_bin40_hits_sse2)
        andl    $0xffff, %esi
        andl    $0xffff, %edx
        movd    %esi, %xmm4
        pshufd  $0, %xmm4, %xmm4            /* vw0 */
        movd    %edx, %xmm5
        pshufd  $0, %xmm5, %xmm5            /* vpm */
        leaq    .Lb40_rev8(%rip), %rsi

        /* b=0..7 */
        movdqu  240(%rdi), %xmm0
        movdqu  224(%rdi), %xmm1
        pand    %xmm5, %xmm0
        pand    %xmm5, %xmm1
        pcmpeqd %xmm4, %xmm0
        pcmpeqd %xmm4, %xmm1
        movmskps %xmm0, %ecx                /* b3..0 */
        movmskps %xmm1, %r8d                /* b7..4 */
        shll    $4, %ecx
        orl     %r8d, %ecx
        movzbl  (%rsi,%rcx), %eax

        /* b=8..15 */
        movdqu  208(%rdi), %xmm0
        movdqu  192(%rdi), %xmm1
        pand    %xmm5, %xmm0
        pand    %xmm5, %xmm1
        pcmpeqd %xmm4, %xmm0
        pcmpeqd %xmm4, %xmm1
        movmskps %xmm0, %ecx
        movmskps %xmm1, %r8d
        shll    $4, %ecx
        orl     %r8d, %ecx
        movzbl  (%rsi,%rcx), %ecx
        shll    $8, %ecx
        orl     %ecx, %eax

        /* b=16..23 */
        movdqu  160(%rdi), %xmm0
        movdqu  176(%rdi), %xmm1
        shufps  $0x88, %xmm1, %xmm0         /* b19,18,17,16 */
        movdqu  128(%rdi), %xmm2
        movdqu  144(%rdi), %xmm3
        shufps  $0x88, %xmm3, %xmm2         /* b23,22,21,20 */
        pand    %xmm5, %xmm0
        pand    %xmm5, %xmm2
        pcmpeqd %xmm4, %xmm0
        pcmpeqd %xmm4, %xmm2
        movmskps %xmm0, %ecx
        movmskps %xmm2, %r8d
        shll    $4, %ecx
        orl     %r8d, %ecx
        movzbl  (%rsi,%rcx), %ecx
        shll    $16, %ecx
        orl     %ecx, %eax

        /* b=24..31 */
        movdqu  96(%rdi), %xmm0
        movdqu  112(%rdi), %xmm1
        shufps  $0x88, %xmm1, %xmm0
        movdqu  64(%rdi), %xmm2
        movdqu  80(%rdi), %xmm3
        shufps  $0x88, %xmm3, %xmm2
        pand    %xmm5, %xmm0
        pand    %xmm5, %xmm2
        pcmpeqd %xmm4, %xmm0
        pcmpeqd %xmm4, %xmm2
        movmskps %xmm0, %ecx
        movmskps %xmm2, %r8d
        shll    $4, %ecx
        orl     %r8d, %ecx
        movzbl  (%rsi,%rcx), %ecx
        shll    $24, %ecx
        orl     %ecx, %eax

        /* b=32..39 */
        movdqu  32(%rdi), %xmm0
        movdqu  48(%rdi), %xmm1
        shufps  $0x88, %xmm1, %xmm0
        movdqu  (%rdi), %xmm2
        movdqu  16(%rdi), %xmm3
        shufps  $0x88, %xmm3, %xmm2
        pand    %xmm5, %xmm0
        pand    %xmm5, %xmm2
        pcmpeqd %xmm4, %xmm0
        pcmpeqd %xmm4, %xmm2
        movmskps %xmm0, %ecx
        movmskps %xmm2, %r8d
        shll    $4, %ecx
        orl     %r8d, %ecx
        movzbl  (%rsi,%rcx), %ecx
        shlq    $32, %rcx
        orq     %rcx, %rax
        ret
ENDF(lz0_bin40_hits_sse2)

/* ------------------------------------------------------------------------
 * uint64_t lz0_bin40_hits_avx2(const uint8_t *p = rdi, uint32_t w0 = esi,
 *                              uint32_t pm = edx)
 * Touches only ymm0..ymm5 (Win64 ABI safe).
 * ---------------------------------------------------------------------- */
FUNC(lz0_bin40_hits_avx2)
        andl    $0xffff, %esi
        andl    $0xffff, %edx
        vmovd   %esi, %xmm4
        vpbroadcastd %xmm4, %ymm4           /* vw0 */
        vmovd   %edx, %xmm5
        vpbroadcastd %xmm5, %ymm5           /* vpm */

        vpand   192(%rdi), %ymm5, %ymm0     /* b15..8 */
        vpand   224(%rdi), %ymm5, %ymm1     /* b7..0 */
        vpcmpeqd %ymm4, %ymm0, %ymm0
        vpcmpeqd %ymm4, %ymm1, %ymm1
        vmovmskps %ymm0, %r8d
        vmovmskps %ymm1, %r9d

        vmovdqu 128(%rdi), %ymm0
        vmovdqu 160(%rdi), %ymm1
        vpsllq  $32, %ymm0, %ymm0
        vpblendd $0xAA, %ymm0, %ymm1, %ymm0
        vpand   %ymm5, %ymm0, %ymm0
        vpcmpeqd %ymm4, %ymm0, %ymm0
        vmovmskps %ymm0, %r10d              /* b16..23 shuffled */

        vmovdqu 64(%rdi), %ymm2
        vmovdqu 96(%rdi), %ymm3
        vpsllq  $32, %ymm2, %ymm2
        vpblendd $0xAA, %ymm2, %ymm3, %ymm2
        vpand   %ymm5, %ymm2, %ymm2
        vpcmpeqd %ymm4, %ymm2, %ymm2
        vmovmskps %ymm2, %r11d              /* b24..31 shuffled */

        vmovdqu (%rdi), %ymm0
        vmovdqu 32(%rdi), %ymm1
        vpsllq  $32, %ymm0, %ymm0
        vpblendd $0xAA, %ymm0, %ymm1, %ymm0
        vpand   %ymm5, %ymm0, %ymm0
        vpcmpeqd %ymm4, %ymm0, %ymm0
        vmovmskps %ymm0, %ecx               /* b32..39 shuffled */

        vzeroupper
        leaq    .Lb40_rev8(%rip), %rsi
        movzbl  (%rsi,%r9), %eax
        movzbl  (%rsi,%r8), %edx
        shll    $8, %edx
        orl     %edx, %eax
        movzbl  256(%rsi,%r10), %r8d
        movzbl  256(%rsi,%r11), %r9d
        shll    $16, %r8d
        shll    $24, %r9d
        orl     %r8d, %eax
        orl     %r9d, %eax
        movzbl  256(%rsi,%rcx), %ecx
        shlq    $32, %rcx
        orq     %rcx, %rax
        ret
ENDF(lz0_bin40_hits_avx2)

#if defined(_WIN32)
        .section .rdata,"dr"
#elif defined(__APPLE__)
        .section __TEXT,__const
#else
        .section .rodata
#endif
        .p2align 5
.Lmru_lanes:
        .long   0, 1, 2, 3, 4, 5, 6, 7
.Lce_masks:                                 /* 32-byte lane-select masks */
        .fill   32, 1, 1                    /* step 1 */
        .rept   16                          /* step 2 */
        .byte   1, 0
        .endr
        .rept   8                           /* step 4 */
        .byte   1, 0, 0, 0
        .endr
.Lb40_rev8:                                 /* bit-reverse 8-bit index */
        .byte   0,128,64,192,32,160,96,224,16,144,80,208,48,176,112,240
        .byte   8,136,72,200,40,168,104,232,24,152,88,216,56,184,120,248
        .byte   4,132,68,196,36,164,100,228,20,148,84,212,52,180,116,244
        .byte   12,140,76,204,44,172,108,236,28,156,92,220,60,188,124,252
        .byte   2,130,66,194,34,162,98,226,18,146,82,210,50,178,114,242
        .byte   10,138,74,202,42,170,106,234,26,154,90,218,58,186,122,250
        .byte   6,134,70,198,38,166,102,230,22,150,86,214,54,182,118,246
        .byte   14,142,78,206,46,174,110,238,30,158,94,222,62,190,126,254
        .byte   1,129,65,193,33,161,97,225,17,145,81,209,49,177,113,241
        .byte   9,137,73,201,41,169,105,233,25,153,89,217,57,185,121,249
        .byte   5,133,69,197,37,165,101,229,21,149,85,213,53,181,117,245
        .byte   13,141,77,205,45,173,109,237,29,157,93,221,61,189,125,253
        .byte   3,131,67,195,35,163,99,227,19,147,83,211,51,179,115,243
        .byte   11,139,75,203,43,171,107,235,27,155,91,219,59,187,123,251
        .byte   7,135,71,199,39,167,103,231,23,151,87,215,55,183,119,247
        .byte   15,143,79,207,47,175,111,239,31,159,95,223,63,191,127,255
.Lb40_unshuf:                               /* .Lb40_rev8 + 256 */
        .byte   0x00,0x08,0x80,0x88,0x04,0x0c,0x84,0x8c,0x40,0x48,0xc0,0xc8,0x44,0x4c,0xc4,0xcc
        .byte   0x02,0x0a,0x82,0x8a,0x06,0x0e,0x86,0x8e,0x42,0x4a,0xc2,0xca,0x46,0x4e,0xc6,0xce
        .byte   0x20,0x28,0xa0,0xa8,0x24,0x2c,0xa4,0xac,0x60,0x68,0xe0,0xe8,0x64,0x6c,0xe4,0xec
        .byte   0x22,0x2a,0xa2,0xaa,0x26,0x2e,0xa6,0xae,0x62,0x6a,0xe2,0xea,0x66,0x6e,0xe6,0xee
        .byte   0x01,0x09,0x81,0x89,0x05,0x0d,0x85,0x8d,0x41,0x49,0xc1,0xc9,0x45,0x4d,0xc5,0xcd
        .byte   0x03,0x0b,0x83,0x8b,0x07,0x0f,0x87,0x8f,0x43,0x4b,0xc3,0xcb,0x47,0x4f,0xc7,0xcf
        .byte   0x21,0x29,0xa1,0xa9,0x25,0x2d,0xa5,0xad,0x61,0x69,0xe1,0xe9,0x65,0x6d,0xe5,0xed
        .byte   0x23,0x2b,0xa3,0xab,0x27,0x2f,0xa7,0xaf,0x63,0x6b,0xe3,0xeb,0x67,0x6f,0xe7,0xef
        .byte   0x10,0x18,0x90,0x98,0x14,0x1c,0x94,0x9c,0x50,0x58,0xd0,0xd8,0x54,0x5c,0xd4,0xdc
        .byte   0x12,0x1a,0x92,0x9a,0x16,0x1e,0x96,0x9e,0x52,0x5a,0xd2,0xda,0x56,0x5e,0xd6,0xde
        .byte   0x30,0x38,0xb0,0xb8,0x34,0x3c,0xb4,0xbc,0x70,0x78,0xf0,0xf8,0x74,0x7c,0xf4,0xfc
        .byte   0x32,0x3a,0xb2,0xba,0x36,0x3e,0xb6,0xbe,0x72,0x7a,0xf2,0xfa,0x76,0x7e,0xf6,0xfe
        .byte   0x11,0x19,0x91,0x99,0x15,0x1d,0x95,0x9d,0x51,0x59,0xd1,0xd9,0x55,0x5d,0xd5,0xdd
        .byte   0x13,0x1b,0x93,0x9b,0x17,0x1f,0x97,0x9f,0x53,0x5b,0xd3,0xdb,0x57,0x5f,0xd7,0xdf
        .byte   0x31,0x39,0xb1,0xb9,0x35,0x3d,0xb5,0xbd,0x71,0x79,0xf1,0xf9,0x75,0x7d,0xf5,0xfd
        .byte   0x33,0x3b,0xb3,0xbb,0x37,0x3f,0xb7,0xbf,0x73,0x7b,0xf3,0xfb,0x77,0x7f,0xf7,0xff

        .p2align 5
.Lu_7fffffff:
        .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
.Lu_126:
        .long 126, 126, 126, 126, 126, 126, 126, 126
.Lu_ones:
        .long 1, 1, 1, 1, 1, 1, 1, 1
.Lu_80000000:
        .long 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000
.Lu_hmult:
        .long 0x9E3779B1, 0x9E3779B1, 0x9E3779B1, 0x9E3779B1, 0x9E3779B1, 0x9E3779B1, 0x9E3779B1, 0x9E3779B1

        .text
/* ------------------------------------------------------------------------
 * void lz0_u_row_prep_avx2(lz0_u_prep *p = rdi)
 * Guard-page clean for all 1 <= nsi <= 64, Win64/ELF/Darwin ABI safe.
 * ---------------------------------------------------------------------- */
FUNC(lz0_u_row_prep_avx2)
        movq    (%rdi), %r10                /* r10 = rkey */
        movq    8(%rdi), %r11               /* r11 = rcost */
        movl    28(%rdi), %edx
        movslq  %edx, %rdx                  /* rdx = nsi (1..64) */
        cmpq    $8, %rdx
        jl      .Lurp_small

        /* 1. Find global min cost across 0..nsi-1 (overlapping in-bounds tail) */
        vmovdqu (%r11), %ymm4
        movl    $8, %eax
        leaq    -8(%rdx), %r9               /* r9 = nsi - 8 >= 0 */
        cmpq    %r9, %rax
        jg      .Lurp_p1_red
.Lurp_p1_loop:
        vpminsd (%r11,%rax,4), %ymm4, %ymm4
        addq    $8, %rax
        cmpq    %r9, %rax
        jle     .Lurp_p1_loop
.Lurp_p1_red:
        vpminsd (%r11,%r9,4), %ymm4, %ymm4  /* overlapping tail [nsi-8 .. nsi) */
        vextracti128 $1, %ymm4, %xmm0
        vpminsd %xmm0, %xmm4, %xmm4
        vpshufd $0x4e, %xmm4, %xmm0
        vpminsd %xmm0, %xmm4, %xmm4
        vpshufd $0xb1, %xmm4, %xmm0
        vpminsd %xmm0, %xmm4, %xmm4
        vpbroadcastd %xmm4, %ymm4
        xorl    %eax, %eax
.Lurp_findb:
        cmpq    %r9, %rax
        jle     1f
        movq    %r9, %rax
1:
        vpcmpeqd (%r11,%rax,4), %ymm4, %ymm0
        vmovmskps %ymm0, %r8d
        testl   %r8d, %r8d
        jnz     .Lurp_foundb
        addq    $8, %rax
        jmp     .Lurp_findb
.Lurp_foundb:
        tzcntl  %r8d, %r8d
        addl    %eax, %r8d
        movl    %r8d, 44(%rdi)              /* b_out */
        movl    (%r10,%r8,4), %r8d
        andl    $0x7fffffff, %r8d
        movl    %r8d, 52(%rdi)              /* boff in _pad */

        /* 2. Pass 2: min2 (for off != boff), um[64], h_lit[64], pass_mask, lit_cand_mask */
        vmovd   %r8d, %xmm4
        vpbroadcastd %xmm4, %ymm4           /* ymm4 = boff */
        vmovdqa .Lu_7fffffff(%rip), %ymm5   /* ymm5 = running min2 */
        xorl    %ecx, %ecx                  /* rcx = k */
        xorq    %rsi, %rsi                  /* rsi = pass_mask */
        xorq    %r8, %r8                    /* r8  = lit_cand_mask */
.Lurp_p2_loop:
        vmovdqu (%r10,%rcx,4), %ymm0        /* key */
        vmovmskps %ymm0, %edx               /* U_LIT_FLAG bits */
        vpand   .Lu_7fffffff(%rip), %ymm0, %ymm1 /* off */
        vpor    .Lu_80000000(%rip), %ymm1, %ymm2 /* lk = off | U_LIT_FLAG */
        vpsrld  $16, %ymm2, %ymm3
        vpxor   %ymm3, %ymm2, %ymm2
        vpmulld .Lu_hmult(%rip), %ymm2, %ymm2
        vpsrld  $24, %ymm2, %ymm2
        vmovdqu %ymm2, 328(%rdi,%rcx,4)     /* h_lit */
        vpcmpeqd %ymm4, %ymm1, %ymm2        /* off == boff */
        vmovdqu (%r11,%rcx,4), %ymm3        /* cost */
        vmovdqa .Lu_7fffffff(%rip), %ymm0
        vpblendvb %ymm2, %ymm0, %ymm3, %ymm2
        vpminsd %ymm2, %ymm5, %ymm5
        /* lit && off <= pos check (unsigned u32 off <= pos via ^0x80000000) */
        vmovd   24(%rdi), %xmm0
        vpbroadcastd %xmm0, %ymm0
        vpxor   .Lu_80000000(%rip), %ymm0, %ymm0
        vpxor   .Lu_80000000(%rip), %ymm1, %ymm2
        vpcmpgtd %ymm0, %ymm2, %ymm0        /* -1 if off > pos */
        vmovmskps %ymm0, %eax
        xorl    $0xff, %eax                 /* 1 if off <= pos */
        andl    %eax, %edx                  /* edx = lit && off <= pos */
        /* um = margin + 2*((float)((off-1)>>8 + 1) >> 23) - 253 */
        vpsubd  .Lu_ones(%rip), %ymm1, %ymm1
        vpsrld  $8, %ymm1, %ymm1
        vpaddd  .Lu_ones(%rip), %ymm1, %ymm1
        vcvtdq2ps %ymm1, %ymm1
        vpsrld  $23, %ymm1, %ymm1
        vpsubd  .Lu_126(%rip), %ymm1, %ymm1
        vpaddd  %ymm1, %ymm1, %ymm1
        vpsubd  .Lu_ones(%rip), %ymm1, %ymm1
        vmovd   36(%rdi), %xmm0
        vpbroadcastd %xmm0, %ymm0
        vpaddd  %ymm0, %ymm1, %ymm1
        vmovdqu %ymm1, 72(%rdi,%rcx,4)
        /* pass check: row_best + um + 1 > cost */
        vmovd   32(%rdi), %xmm0
        vpbroadcastd %xmm0, %ymm0
        vpaddd  %ymm0, %ymm1, %ymm1
        vpaddd  .Lu_ones(%rip), %ymm1, %ymm1
        vpcmpgtd %ymm3, %ymm1, %ymm2
        vmovmskps %ymm2, %eax
        andl    %eax, %edx
        shlq    %cl, %rax
        shlq    %cl, %rdx
        orq     %rax, %rsi
        orq     %rdx, %r8
        addq    $8, %rcx
        cmpq    %r9, %rcx
        jle     .Lurp_p2_loop
        movl    28(%rdi), %edx
        movslq  %edx, %rdx
        cmpq    %rdx, %rcx
        jge     .Lurp_p2_red
        movq    %r9, %rcx                   /* overlapping tail [nsi-8 .. nsi) */
        jmp     .Lurp_p2_loop
.Lurp_p2_red:
        cmpl    $0, 40(%rdi)                /* exact? */
        jne     .Lurp_exact_fix
.Lurp_masks_ok:
        movq    %rsi, 56(%rdi)              /* pass_mask */
        movq    %r8, 64(%rdi)               /* lit_cand_mask */
        vextracti128 $1, %ymm5, %xmm0
        vpminsd %xmm0, %xmm5, %xmm5
        vpshufd $0x4e, %xmm5, %xmm0
        vpminsd %xmm0, %xmm5, %xmm5
        vpshufd $0xb1, %xmm5, %xmm0
        vpminsd %xmm0, %xmm5, %xmm5
        vmovd   %xmm5, %eax
        cmpl    $0x7fffffff, %eax
        jne     .Lurp_findb2
        movl    $-1, 48(%rdi)
        vzeroupper
        ret
.Lurp_exact_fix:
        /* Recompute exact um = 0x0fffffff, pass_mask = all nsi bits, lit_cand_mask = lit & off<=pos */
        movl    28(%rdi), %ecx
        movq    $-1, %rsi
        cmpl    $64, %ecx
        je      1f
        movl    $1, %eax
        shlq    %cl, %rax
        decq    %rax
        movq    %rax, %rsi
1:
        xorl    %ecx, %ecx
        xorq    %r8, %r8
        movl    28(%rdi), %edx
2:
        movl    $0x0fffffff, 72(%rdi,%rcx,4)
        movl    (%r10,%rcx,4), %eax
        testl   $0x80000000, %eax
        jz      3f
        andl    $0x7fffffff, %eax
        cmpl    24(%rdi), %eax
        ja      3f
        btsq    %rcx, %r8
3:
        incq    %rcx
        cmpl    %edx, %ecx
        jl      2b
        jmp     .Lurp_masks_ok
.Lurp_findb2:
        vpbroadcastd %xmm5, %ymm5
        xorl    %ecx, %ecx
.Lurp_fb2_loop:
        cmpq    %r9, %rcx
        jle     1f
        movq    %r9, %rcx
1:
        vpcmpeqd (%r11,%rcx,4), %ymm5, %ymm0
        vmovdqu (%r10,%rcx,4), %ymm1
        vpand   .Lu_7fffffff(%rip), %ymm1, %ymm1
        vpcmpeqd %ymm4, %ymm1, %ymm2
        vpandn  %ymm0, %ymm2, %ymm0
        vmovmskps %ymm0, %eax
        testl   %eax, %eax
        jnz     .Lurp_fb2_hit
        addq    $8, %rcx
        jmp     .Lurp_fb2_loop
.Lurp_fb2_hit:
        tzcntl  %eax, %eax
        addl    %ecx, %eax
        movl    %eax, 48(%rdi)
        vzeroupper
        ret

.Lurp_small:                                /* 1 <= nsi <= 7: scalar, no YMM touched */
        xorl    %eax, %eax                  /* b_out */
        movl    (%r11), %r8d                /* minc */
        movl    $1, %ecx
1:
        cmpq    %rdx, %rcx
        jge     2f
        movl    (%r11,%rcx,4), %r9d
        cmpl    %r8d, %r9d
        jge     3f
        movl    %r9d, %r8d
        movl    %ecx, %eax
3:
        incq    %rcx
        jmp     1b
2:
        movl    %eax, 44(%rdi)              /* b_out */
        movl    (%r10,%rax,4), %r8d
        andl    $0x7fffffff, %r8d
        movl    %r8d, 52(%rdi)              /* boff in _pad */
        movl    $0x7fffffff, %r9d           /* min2 */
        movl    $-1, 48(%rdi)               /* b2_out */
        movq    $0, 56(%rdi)                /* pass_mask */
        movq    $0, 64(%rdi)                /* lit_cand_mask */
        xorl    %ecx, %ecx                  /* s */
.Lurp_sm_loop:
        cmpq    %rdx, %rcx
        jge     .Lurp_sm_done
        movl    (%r10,%rcx,4), %eax         /* key */
        movl    %eax, %esi
        andl    $0x7fffffff, %esi           /* off */
        cmpl    52(%rdi), %esi
        je      1f
        movl    (%r11,%rcx,4), %r8d
        cmpl    %r9d, %r8d
        jge     1f
        movl    %r8d, %r9d
        movl    %ecx, 48(%rdi)
1:
        movl    %eax, %r8d
        orl     $0x80000000, %r8d           /* lk */
        movl    %r8d, %eax
        shrl    $16, %eax
        xorl    %eax, %r8d
        imull   $0x9E3779B1, %r8d, %r8d
        shrl    $24, %r8d
        movl    %r8d, 328(%rdi,%rcx,4)      /* h_lit[s] */
        cmpl    $0, 40(%rdi)
        je      2f
        movl    $0x0fffffff, 72(%rdi,%rcx,4)
        btsq    %rcx, 56(%rdi)
        jmp     3f
2:
        movl    %esi, %r8d
        decl    %r8d
        shrl    $8, %r8d
        incl    %r8d                        /* msb */
        movl    $1, %eax                    /* eb */
        cmpl    $1, %r8d
        jbe     4f
        bsrl    %r8d, %eax
        leal    1(%rax,%rax), %eax
4:
        addl    36(%rdi), %eax              /* um = margin + eb */
        movl    %eax, 72(%rdi,%rcx,4)
        addl    32(%rdi), %eax              /* row_best + um */
        cmpl    (%r11,%rcx,4), %eax
        jl      5f
        btsq    %rcx, 56(%rdi)
3:
        movl    (%r10,%rcx,4), %eax         /* key */
        testl   $0x80000000, %eax
        jz      5f
        cmpl    24(%rdi), %esi              /* off <= pos */
        ja      5f
        btsq    %rcx, 64(%rdi)
5:
        incq    %rcx
        jmp     .Lurp_sm_loop
.Lurp_sm_done:
        ret
ENDF(lz0_u_row_prep_avx2)

/* ------------------------------------------------------------------------
 * int lz0_sa_scan_side_avx2(lz0_sa_scan_args *a = rdi) -> returns nhits
 * Guard-page clean, encodes (k==3 ? 0x80 : 0) | d_idx in out_k[h],
 * Win64/ELF/Darwin ABI safe.
 * ---------------------------------------------------------------------- */
FUNC(lz0_sa_scan_side_avx2)
        movq    (%rdi), %r10                /* r10 = sa_ptr */
        movq    8(%rdi), %r11               /* r11 = in */
        movl    16(%rdi), %esi              /* esi = pos */
        movl    24(%rdi), %ecx              /* ecx = rem count */
        movl    $0, 44(%rdi)                /* nhits = 0 */
        testl   %ecx, %ecx
        jle     .Lsa_ret0

        vmovdqa .Lu_80000000(%rip), %ymm5
        vmovd   %esi, %xmm0
        vpbroadcastd %xmm0, %ymm4
        vpxor   %ymm5, %ymm4, %ymm4         /* ymm4 = pos ^ 0x80000000 */

        cmpl    $0, 28(%rdi)
        jl      .Lsa_L_loop8

.Lsa_R_loop8:
        cmpl    $8, %ecx
        jl      .Lsa_R_tail
        movl    28(%r10), %eax              /* src_far = sa[7] */
        leal    1(%rax), %edx
        cmpl    20(%rdi), %edx              /* src_far + 1 < n ? */
        jae     .Lsa_R_tail
        movzwl  (%r11,%rax), %edx
        cmpw    32(%rdi), %dx
        jne     .Lsa_R_tail

        vmovdqu (%r10), %ymm0
        vpxor   %ymm5, %ymm0, %ymm1
        vpcmpgtd %ymm1, %ymm4, %ymm2
        vmovmskps %ymm2, %eax               /* eax = mask (src < pos) */

        cmpb    $0, 35(%rdi)
        jne     .Lsa_R_blk_l2

        cmpb    $0, 37(%rdi)
        je      .Lsa_R_tail
        movl    28(%r10), %edx
        leal    2(%rdx), %r8d
        cmpl    20(%rdi), %r8d
        jae     .Lsa_R_tail
        movzbl  2(%r11,%rdx), %r8d
        cmpb    34(%rdi), %r8b
        jne     .Lsa_R_tail

        testl   %eax, %eax
        jz      .Lsa_R_next8
        movl    %ecx, 24(%rdi)              /* save rem */
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    44(%rdi), %edx
        movq    %r10, %r11
        subq    (%rdi), %r11
        shrq    $2, %r11                    /* base d_idx */
.Lsa_R_emit3:
        tzcntl  %eax, %ecx
        btrl    %ecx, %eax
        movl    (%r10,%rcx,4), %esi
        movl    %esi, (%r8,%rdx,4)
        addl    %r11d, %ecx
        orl     $0x80, %ecx
        movb    %cl, (%r9,%rdx)
        incl    %edx
        testl   %eax, %eax
        jnz     .Lsa_R_emit3
        movl    %edx, 44(%rdi)
        movl    24(%rdi), %ecx
        movl    16(%rdi), %esi
        movq    8(%rdi), %r11
.Lsa_R_next8:
        addq    $32, %r10
        subl    $8, %ecx
        jmp     .Lsa_R_loop8

.Lsa_R_blk_l2:
        cmpb    $0, 36(%rdi)
        jne     .Lsa_ret
        testl   %eax, %eax
        jz      .Lsa_R_next8
        vmovd   %esi, %xmm2
        vpbroadcastd %xmm2, %ymm2           /* pos */
        vpsubd  %ymm0, %ymm2, %ymm1         /* off = pos - src */
        vmovd   40(%rdi), %xmm3
        vpbroadcastd %xmm3, %ymm3           /* l2_cut */
        vpxor   %ymm5, %ymm1, %ymm1
        vpxor   %ymm5, %ymm3, %ymm3
        vpcmpgtd %ymm1, %ymm3, %ymm2        /* -1 if (u32)off < (u32)l2_cut */
        vmovmskps %ymm2, %edx
        andl    %edx, %eax
        jz      .Lsa_R_next8
        movl    %ecx, 24(%rdi)
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    44(%rdi), %edx
        movq    %r10, %r11
        subq    (%rdi), %r11
        shrq    $2, %r11
.Lsa_R_emit2:
        tzcntl  %eax, %ecx
        btrl    %ecx, %eax
        movl    (%r10,%rcx,4), %esi
        movl    %esi, (%r8,%rdx,4)
        addl    %r11d, %ecx
        movb    %cl, (%r9,%rdx)
        incl    %edx
        testl   %eax, %eax
        jnz     .Lsa_R_emit2
        movl    %edx, 44(%rdi)
        movl    24(%rdi), %ecx
        movl    16(%rdi), %esi
        movq    8(%rdi), %r11
        jmp     .Lsa_R_next8

.Lsa_R_tail:
        testl   %ecx, %ecx
        jle     .Lsa_ret
.Lsa_R_tloop:
        movl    (%r10), %eax
        leal    1(%rax), %edx
        cmpl    20(%rdi), %edx
        jae     .Lsa_ret
        movzwl  (%r11,%rax), %edx
        cmpw    32(%rdi), %dx
        jne     .Lsa_ret
        cmpb    $0, 35(%rdi)
        jne     .Lsa_R_t_isl2
        cmpb    $0, 37(%rdi)
        je      .Lsa_R_t_gol2
        leal    2(%rax), %edx
        cmpl    20(%rdi), %edx
        jae     .Lsa_R_t_gol2
        movzbl  2(%r11,%rax), %edx
        cmpb    34(%rdi), %dl
        jne     .Lsa_R_t_gol2
        cmpl    %esi, %eax
        jae     .Lsa_R_tnext
        movl    44(%rdi), %edx
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    %eax, (%r8,%rdx,4)
        movq    %r10, %rax
        subq    (%rdi), %rax
        shrq    $2, %rax
        orl     $0x80, %eax
        movb    %al, (%r9,%rdx)
        incl    %edx
        movl    %edx, 44(%rdi)
        jmp     .Lsa_R_tnext
.Lsa_R_t_gol2:
        movb    $1, 35(%rdi)
.Lsa_R_t_isl2:
        cmpb    $0, 36(%rdi)
        jne     .Lsa_ret
        cmpl    %esi, %eax
        jae     .Lsa_R_tnext
        movl    %esi, %edx
        subl    %eax, %edx
        cmpl    40(%rdi), %edx
        jae     .Lsa_R_tnext
        movl    44(%rdi), %edx
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    %eax, (%r8,%rdx,4)
        movq    %r10, %rax
        subq    (%rdi), %rax
        shrq    $2, %rax
        movb    %al, (%r9,%rdx)
        incl    %edx
        movl    %edx, 44(%rdi)
.Lsa_R_tnext:
        addq    $4, %r10
        decl    %ecx
        jnz     .Lsa_R_tloop
        jmp     .Lsa_ret

.Lsa_L_loop8:
        cmpl    $8, %ecx
        jl      .Lsa_L_tail
        movl    -28(%r10), %eax
        leal    1(%rax), %edx
        cmpl    20(%rdi), %edx
        jae     .Lsa_L_tail
        movzwl  (%r11,%rax), %edx
        cmpw    32(%rdi), %dx
        jne     .Lsa_L_tail

        vmovdqu -28(%r10), %ymm0
        vpxor   %ymm5, %ymm0, %ymm1
        vpcmpgtd %ymm1, %ymm4, %ymm2
        vmovmskps %ymm2, %eax               /* bit 7 = r10[0], bit 0 = r10[-7] */

        cmpb    $0, 35(%rdi)
        jne     .Lsa_L_blk_l2

        cmpb    $0, 37(%rdi)
        je      .Lsa_L_tail
        movl    -28(%r10), %edx
        leal    2(%rdx), %r8d
        cmpl    20(%rdi), %r8d
        jae     .Lsa_L_tail
        movzbl  2(%r11,%rdx), %r8d
        cmpb    34(%rdi), %r8b
        jne     .Lsa_L_tail

        testl   %eax, %eax
        jz      .Lsa_L_next8
        movl    %ecx, 24(%rdi)
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    44(%rdi), %edx
        movq    (%rdi), %r11
        subq    %r10, %r11
        shrq    $2, %r11
.Lsa_L_emit3:
        bsrl    %eax, %ecx
        btrl    %ecx, %eax
        subl    $7, %ecx
        movslq  %ecx, %rsi
        movl    (%r10,%rsi,4), %esi
        movl    %esi, (%r8,%rdx,4)
        movl    %r11d, %esi
        subl    %ecx, %esi
        orl     $0x80, %esi
        movb    %sil, (%r9,%rdx)
        incl    %edx
        testl   %eax, %eax
        jnz     .Lsa_L_emit3
        movl    %edx, 44(%rdi)
        movl    24(%rdi), %ecx
        movl    16(%rdi), %esi
        movq    8(%rdi), %r11
.Lsa_L_next8:
        subq    $32, %r10
        subl    $8, %ecx
        jmp     .Lsa_L_loop8

.Lsa_L_blk_l2:
        cmpb    $0, 36(%rdi)
        jne     .Lsa_ret
        testl   %eax, %eax
        jz      .Lsa_L_next8
        vmovd   %esi, %xmm2
        vpbroadcastd %xmm2, %ymm2
        vpsubd  %ymm0, %ymm2, %ymm1
        vmovd   40(%rdi), %xmm3
        vpbroadcastd %xmm3, %ymm3
        vpxor   %ymm5, %ymm1, %ymm1
        vpxor   %ymm5, %ymm3, %ymm3
        vpcmpgtd %ymm1, %ymm3, %ymm2
        vmovmskps %ymm2, %edx
        andl    %edx, %eax
        jz      .Lsa_L_next8
        movl    %ecx, 24(%rdi)
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    44(%rdi), %edx
        movq    (%rdi), %r11
        subq    %r10, %r11
        shrq    $2, %r11
.Lsa_L_emit2:
        bsrl    %eax, %ecx
        btrl    %ecx, %eax
        subl    $7, %ecx
        movslq  %ecx, %rsi
        movl    (%r10,%rsi,4), %esi
        movl    %esi, (%r8,%rdx,4)
        movl    %r11d, %esi
        subl    %ecx, %esi
        movb    %sil, (%r9,%rdx)
        incl    %edx
        testl   %eax, %eax
        jnz     .Lsa_L_emit2
        movl    %edx, 44(%rdi)
        movl    24(%rdi), %ecx
        movl    16(%rdi), %esi
        movq    8(%rdi), %r11
        jmp     .Lsa_L_next8

.Lsa_L_tail:
        testl   %ecx, %ecx
        jle     .Lsa_ret
.Lsa_L_tloop:
        movl    (%r10), %eax
        leal    1(%rax), %edx
        cmpl    20(%rdi), %edx
        jae     .Lsa_ret
        movzwl  (%r11,%rax), %edx
        cmpw    32(%rdi), %dx
        jne     .Lsa_ret
        cmpb    $0, 35(%rdi)
        jne     .Lsa_L_t_isl2
        cmpb    $0, 37(%rdi)
        je      .Lsa_L_t_gol2
        leal    2(%rax), %edx
        cmpl    20(%rdi), %edx
        jae     .Lsa_L_t_gol2
        movzbl  2(%r11,%rax), %edx
        cmpb    34(%rdi), %dl
        jne     .Lsa_L_t_gol2
        cmpl    %esi, %eax
        jae     .Lsa_L_tnext
        movl    44(%rdi), %edx
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    %eax, (%r8,%rdx,4)
        movq    (%rdi), %rax
        subq    %r10, %rax
        shrq    $2, %rax
        orl     $0x80, %eax
        movb    %al, (%r9,%rdx)
        incl    %edx
        movl    %edx, 44(%rdi)
        jmp     .Lsa_L_tnext
.Lsa_L_t_gol2:
        movb    $1, 35(%rdi)
.Lsa_L_t_isl2:
        cmpb    $0, 36(%rdi)
        jne     .Lsa_ret
        cmpl    %esi, %eax
        jae     .Lsa_L_tnext
        movl    %esi, %edx
        subl    %eax, %edx
        cmpl    40(%rdi), %edx
        jae     .Lsa_L_tnext
        movl    44(%rdi), %edx
        movq    48(%rdi), %r8
        movq    56(%rdi), %r9
        movl    %eax, (%r8,%rdx,4)
        movq    (%rdi), %rax
        subq    %r10, %rax
        shrq    $2, %rax
        movb    %al, (%r9,%rdx)
        incl    %edx
        movl    %edx, 44(%rdi)
.Lsa_L_tnext:
        subq    $4, %r10
        decl    %ecx
        jnz     .Lsa_L_tloop
.Lsa_ret:
        movl    44(%rdi), %eax
        vzeroupper
        ret
.Lsa_ret0:
        xorl    %eax, %eax
        ret
ENDF(lz0_sa_scan_side_avx2)

#if defined(__ELF__)
        .section .note.GNU-stack, "", @progbits
#endif

#endif /* (__ELF__ || _WIN32 || __APPLE__) && __x86_64__ */
