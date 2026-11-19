# LZ0 decoder specification

Version: 0.1

This document specifies decoder behaviour for the **raw** LZ0 bitstream defined in `LZ0_FORMAT.md`.

Normative authority: `LZ0_FORMAT.md` and `lz0dec_ref.c` must agree. This document is the readable procedure. If this file and the format spec disagree, the format spec plus `lz0dec_ref.c` win after they are reconciled.

The decoder is a property of the format:

- no malloc
- no large tables
- deterministic
- overlap copies are byte-by-byte
- caller provides the output buffer

`lz0dec_min.c` is a size-oriented implementation of the same contract. `lz0_decompress_to` in the library must remain behaviourally identical for the public encoding (LSB=8, LEN=3).

## API

```c
int lz0_decompress_to(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max);
```

On success, return the number of uncompressed bytes written, which is `<= out_max`.

On failure, return `-1`. Bytes already written to `out` are undefined. Never write past `out_max`. Never read past `in_size`.

Reject immediately with `-1` when:

- `in == NULL` or `out == NULL`
- `in_size <= 0`
- `out_max <= 0`

There are no distinct error codes.

## Bitreader state

```
src         pointer to next source byte
src_end     in + in_size
bit_mask    0 at start
bit_value   0 at start
```

`read_byte`:

```
if src >= src_end: fail
b <- *src++
return b
```

`read_bit`:

```
bit_mask <- bit_mask >> 1
if bit_mask == 0:
    bit_mask <- 128
    bit_value <- read_byte()      ; fail if truncated
return (bit_value & bit_mask) != 0
```

LSB=8 uses no backtrack. After `read_byte` for a literal or offset LSB, later bits come from a new control byte via the `bit_mask == 0` refill above.

## Elias gamma

`read_elias(inverted)`:

```
value <- 1
loop:
    if read_bit() == 1:
        return value
    p <- read_bit()
    value <- (value << 1) | (p XOR inverted)
```

`inverted` is 0 for lengths, 1 for offset MSB.

Truncation during either bit is failure.

## Lengths

`read_len()` — literal and last-match:

```
return read_elias(inverted=0)
```

Result `L` must be `>= 1`. Elias never returns 0, so this is automatic if the stream is well-formed.

`read_len_new()` — MatchNew payload `v`, actual copy length `v + 1`:

```
x <- read_bit() << 2 | read_bit() << 1 | read_bit()
if x < 7:
    return x + 1
return 7 + read_elias(inverted=0)
```

## New offset or EOF

Called only after a decision bit of `1` has already entered the new/EOF loop.

`read_new_offset(last_offset)`:

```
flag <- read_bit()
if flag == 0:
    return EOF
msb <- read_elias(inverted=1)
t <- read_byte()                 ; fail if truncated
offset <- msb * 256 - t
if offset < 1:
    fail
last_offset <- offset
return NEW
```

## Copy

```
copy_match(offset, length):
    src_pos <- out_pos - offset
    if src_pos < 0: fail
    if out_pos + length > out_max: fail
    for i in 0 .. length-1:
        out[out_pos + i] <- out[src_pos + i]
    out_pos <- out_pos + length
```

Overlap is required. Do not use a non-overlapping memcpy.

## Main loop

```
out_pos <- 0
last_offset <- 1

loop:
    L <- read_len()
    for i in 0 .. L-1:
        c <- read_byte()
        if out_pos >= out_max: fail
        out[out_pos++] <- c

    if read_bit() == 1:
        loop_new:
            kind <- read_new_offset()
            if kind == EOF:
                return out_pos
            L <- read_len_new() + 1
            copy_match(last_offset, L)
            if read_bit() == 1:
                goto loop_new
            else:
                continue outer loop          ; next literal
    else:
        L <- read_len()
        copy_match(last_offset, L)
        if read_bit() == 0:
            continue outer loop              ; next literal
        goto loop_new equivalent
```

The first `read_len()` is the omitted-leading-zero rule: the stream starts at a literal length, not a decision bit.

Equivalent control-flow matching `lz0_decompress_to`:

```
last_offset = 1
out_pos = 0
for (;;) {
    length = read_len();
    copy length literal bytes;
    if (read_bit()) {
        for (;;) {
            if (read_new_offset says EOF) return out_pos;
            length = read_len_new() + 1;
            copy_match(last_offset, length);
            if (!read_bit()) break;
        }
    } else {
        length = read_len();
        copy_match(last_offset, length);
        if (!read_bit()) continue;
        for (;;) {
            if (read_new_offset says EOF) return out_pos;
            length = read_len_new() + 1;
            copy_match(last_offset, length);
            if (!read_bit()) break;
        }
    }
}
```

## Failure cases

All of these return `-1`:

| Case | Why |
|------|-----|
| null pointer / non-positive sizes | API contract |
| truncated control byte | `read_bit` needs a byte |
| truncated Elias | missing prefix or payload bit |
| truncated literal byte | missing payload |
| truncated offset LSB | missing `t` |
| `offset < 1` | invalid |
| `offset > out_pos` | `src_pos < 0` |
| literal or match would pass `out_max` | overflow |
| input ends without EOF | truncated stream |

Leftover bytes after a successful EOF are ignored.

## Implementation requirements

A conforming raw decoder:

1. Implements the procedure above for LSB=8 and LEN=3.
2. Allocates nothing.
3. Uses only the caller buffers.
4. Copies matches one byte at a time.
5. Returns the written count or `-1`.

Reference implementation: `lz0dec_ref.c`.

Tiny implementation: `lz0dec_min.c`.

Library implementation: `lz0_decompress_to` in `lz0/lz0_decompress.c` (may still contain private knobs; public streams use LSB=8, LEN=3).

## Container decoding (CLI)

Not part of the raw API.

```
if file shorter than 8 bytes: fail
if magic != 'LZ01': fail
size <- uint32le at offset 4
payload <- bytes after offset 8
if size == 0:
    if payload nonempty: fail
    write empty output
    success
allocate or use buffer of `size` bytes
n <- lz0_decompress_to(payload, payload_len, buf, size)
if n < 0 or n != size: fail
write buf[0..n)
```
