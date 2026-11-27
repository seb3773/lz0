/*
 * lz0dec_ultra_tiny_x86_16.S - Standalone 16-bit 8086 real-mode LZ0 v0.1
 *                              bootsector / MBR / COM decompressor (~117 B)
 *
 * Build flat raw binary (GNU binutils, zero external dependencies):
 *   cc -c lz0/lz0dec_ultra_tiny_x86_16.S -o lz0_16.o   (or as --32)
 *   objcopy -O binary lz0_16.o lz0dec_ultra_tiny_x86_16.bin
 *
 * Real-mode register ABI:
 *   Entry: DS:SI = compressed LZ0 v0.1 raw stream (valid, non-empty)
 *          ES:DI = destination output buffer (capacity <= 64 KiB, valid)
 *   Exit:  AX    = uncompressed output length (mod 65536)
 *          SI,DI = advanced past input / output end
 *          CLD applied; BX, CX, DX, flags clobbered; DS, ES, BP preserved.
 *   Note: Supports both DS == ES (COM / flat) and DS != ES (bootsector ROM
 *         to far RAM segment). Pure 8086 instructions (no 186+/386+ opcodes).
 */

.code16
.intel_syntax noprefix
.text
.globl lz0dec_ultra_tiny_x86_16

lz0dec_ultra_tiny_x86_16:
    cld
    push di                         /* save out_start */
    mov bx, 1                       /* last_offset = 1 */
    mov dl, 0x80                    /* sentinel bitreader */

.Ldo_literal:
    call .Lelias0                   /* ax = L (>= 1) */
    xchg cx, ax
    rep movsb

.Lafter_lit:
    call .Lbit
    jc .Lenter_new_loop

.Ldo_match_last:
    call .Lelias0                   /* ax = L (>= 1) */

.Lcopy_match:
    xchg cx, ax
    push si
    push ds
    push es
    pop ds                          /* read match history from ES segment */
    mov si, di
    sub si, bx
    rep movsb
    pop ds
    pop si

.Lafter_match:
    call .Lbit
    jnc .Ldo_literal

.Lenter_new_loop:
    call .Lbit
    jnc .Leof

    call .Lelias1                   /* ax = msb (1..256) */
    mov bh, al
    mov bl, 0                       /* bx = (msb * 256) mod 65536 */
    xor ax, ax
    lodsb                           /* al = t, si++ */
    sub bx, ax                      /* bx = offset (1..65535) */

    mov al, 0x20                    /* 3-bit sentinel (ah is already 0) */
.Llen3_lp:
    call .Lbit
    adc al, al
    jnc .Llen3_lp
    add al, 2                       /* L = x + 2 (2..8), or 9 if x == 7 */
    cmp al, 9
    jb .Lcopy_match
    call .Lelias0
    add ax, 8                       /* L = elias + 8 (>= 9) */
    jmp .Lcopy_match

.Leof:
    pop ax                          /* out_start */
    sub di, ax
    xchg ax, di                     /* return uncompressed byte count */
    ret

.Lelias1:
    mov cl, 1
    jmp .Lelias
.Lelias0:
    xor cx, cx
.Lelias:
    mov ax, 1
.Lel_lp:
    call .Lbit
    jc .Lbit_ret
    call .Lbit
    adc ax, ax
    xor al, cl
    jmp .Lel_lp

.Lbit:
    add dl, dl
    jnz .Lbit_ret
    mov dl, [si]
    inc si
    adc dl, dl
.Lbit_ret:
    ret

