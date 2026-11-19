# LZ0 raw bitstream format

Version: 0.1

This document is normative for the **raw LZ0 bitstream**. An independent decoder written from this document and `lz0dec_ref.c` must agree. If they diverge, both are wrong until reconciled.

The production encoder is **not** an authority for this format.

LZ0 is a new format. It is **not ZX0-compatible**.

## Public encoding

v0.1 public encoding is exactly:

- no magic, no header, no version byte, no uncompressed size
- bits packed most-significant-bit first in each byte
- offset LSB width = 8
- length coding = LEN=3 as defined below

No other `lsb_bits` or `len_code` variant is part of this specification.

## Layers

| Layer | Contents |
|-------|----------|
| Raw bitstream | This document. Consumed by the raw decoder API. |
| Container | `LZ01` + uint32 LE size + raw payload. CLI / on-disk only. Not part of the raw format. |

## Empty input

The raw decoder rejects:

- `in_size <= 0`
- `out_max <= 0`
- null buffers

with return value `-1`.

An uncompressed empty file exists only in the container (`uncompressed_size = 0`, empty payload). Do not pass an empty buffer to the raw decoder.

## Bit packing

### Bit order

Each control byte is filled from bit 7 down to bit 0 (mask sequence 128, 64, 32, 16, 8, 4, 2, 1).

A bit value of 1 sets that mask bit.

### Implied first decision bit

The encoder starts as if a backtrack slot existed before byte 0. The first token is always a literal run, and its leading decision bit `0` is **omitted**.

A decoder therefore begins by reading a literal length. It does **not** read a leading `0` bit.

After that omitted bit, every later decision bit is present in the stream.

### Mixing bits and bytes

Literal payload bytes and the offset LSB byte are stored as whole bytes in stream order. They are not bit-packed.

When a whole byte must be read (literal byte or LSB byte), the decoder consumes the next source byte. Subsequent bits come from a new control byte. For LSB=8 there is no "reuse the LSB of the offset byte as the next bit" backtrack.

### End of stream

The encoder may leave unused low bits of the last control byte as 0. A decoder must not require those bits to be 0. After a valid EOF, extra source bytes are not an error; the decoder stops.

## Elias gamma

Used for:

- literal length (not inverted)
- last-match length (not inverted)
- offset MSB (inverted)

Domain: integer `v >= 1`.

### Encode

Let `k = floor(log2(v))`. Emit `k` pairs `(0, payload_bit)` then a stop bit `1`.

Payload bits are the bits of `v` below the highest set bit, from next-highest to least-significant.

If inverted, each payload bit is complemented. The `0` prefixes and the stop `1` are never inverted.

`v = 1` encodes as a single `1` (no pairs).

### Decode

```
value <- 1
loop:
    b <- read_bit()
    if b = 1:
        return value
    p <- read_bit()
    value <- (value << 1) | (p XOR inverted)
```

`inverted` is 0 for lengths and 1 for offset MSB.

### Examples (not inverted)

| v | bits |
|---|------|
| 1 | `1` |
| 2 | `0 0 1` |
| 3 | `0 1 1` |
| 4 | `0 0 0 0 1` |
| 5 | `0 0 0 1 1` |
| 7 | `0 1 0 1 1` |
| 8 | `0 0 0 0 0 0 1` |

## Length coding (LEN=3)

### Literal run and MatchLast

Length `L >= 1` is Elias gamma of `L`, not inverted.

### MatchNew

MatchNew length `L >= 2`. Encode `v = L - 1`.

If `v <= 7`, emit three bits of `x = v - 1`, most-significant bit first.

If `v > 7`, emit `1 1 1` then Elias gamma of `v - 7`, not inverted.

The decoder reads that code as `v`, then uses `L = v + 1`.

| L | v | bits |
|---|---|------|
| 2 | 1 | `000` |
| 3 | 2 | `001` |
| 4 | 3 | `010` |
| 5 | 4 | `011` |
| 6 | 5 | `100` |
| 7 | 6 | `101` |
| 8 | 7 | `110` |
| 9 | 8 | `111` `1` |
| 10 | 9 | `111` `0 0 1` |
| 11 | 10 | `111` `0 1 1` |

## Offset (LSB=8)

Offset is an integer `offset >= 1`.

```
msb = ((offset - 1) >> 8) + 1
rem = (offset - 1) & 255
```

Emit inverted Elias gamma of `msb`, then one byte `255 - rem`.

Decode:

```
msb = elias_gamma(inverted=1)
t   = read_byte()
offset = msb * 256 - t
```

Examples:

| offset | msb | rem | LSB byte |
|--------|-----|-----|----------|
| 1 | 1 | 0 | 255 |
| 256 | 1 | 255 | 0 |
| 257 | 2 | 0 | 255 |
| 512 | 2 | 255 | 0 |

A decoded offset must satisfy `1 <= offset <= out_pos` at the time of the copy. `offset > out_pos` would read before the start of the output buffer and is invalid.

## Tokens

Semantic tokens:

- **LiteralRun** `L >= 1` followed by `L` literal bytes
- **MatchLast** `L >= 1` copies `L` bytes from `out_pos - last_offset`
- **MatchNew** `L >= 2` copies `L` bytes from `out_pos - offset` and sets `last_offset = offset`
- **EOF** ends the stream

### Initial state

```
out_pos = 0
last_offset = 1
```

The first token is always a LiteralRun.

### Grammar

```
stream     := literal_block more* eof_from_new
literal_block := LITERAL ( LAST last_tail | new_block )
more       := literal_block
last_tail  := next_literal | new_block
next_literal := decision_bit 0     ; then another literal_block
new_block  := decision_bit 1 new_loop
new_loop   := EOF | NEW ( continue_new | next_literal )
continue_new := decision_bit 1 new_loop
eof_from_new := reached via new_loop EOF
```

In bits, after a completed LiteralRun or after a completed match that is leaving the new-loop:

| context after | next bits | meaning |
|---------------|-----------|---------|
| LiteralRun | `0` + elias(L) | MatchLast |
| LiteralRun | `1` | enter new/EOF loop |
| MatchLast | `0` | next LiteralRun |
| MatchLast | `1` | enter new/EOF loop |
| MatchNew | `0` | next LiteralRun |
| MatchNew | `1` | continue new/EOF loop |
| new/EOF loop | `0` | EOF |
| new/EOF loop | `1` + offset + len_new | MatchNew |

EOF is the two-bit sequence that enters the new/EOF loop and then selects EOF: a `1` decision followed by a `0` instead of an offset.

A stream that returns from the decoder without ever seeing EOF is truncated and invalid.

### Bit templates

After the omitted first decision bit:

```
LITERAL := elias(L)  L bytes          ; first literal of the stream
LITERAL := 0  elias(L)  L bytes       ; later literals (the 0 is the decision bit)

LAST    := 0  elias(L)

NEW     := 1  inv_elias(msb)  lsb8  len_new(L-1)
          ; the leading 1 here is the "this is a new offset" bit inside the new/EOF loop

EOF     := 0
          ; the 0 here is the "not a new offset" bit inside the new/EOF loop
```

The decision bit that *enters* the new/EOF loop is a separate `1` written before `NEW`/`EOF`. Encoder emits MatchNew as `1` (enter or continue) then `1` (new offset) then MSB, LSB, length. Encoder emits EOF as `1` then `0`.

### last_offset

- Starts at 1.
- LiteralRun does not change it.
- MatchLast uses it and does not change it.
- MatchNew replaces it with the new offset.
- MatchLast is only valid immediately after a LiteralRun (the encoder never emits last-match after a match). A decoder may still execute a last-match after a match if the bits say so; that is allowed by the bit grammar. Production encoder never produces it.

### Overlap copy

Matches may overlap the destination:

```
src = out_pos - offset
for i in 0 .. L-1:
    out[out_pos + i] = out[src + i]
out_pos += L
```

Byte-by-byte copy is required. `memcpy` that does not handle overlap is incorrect. Offset 1 repeating the last byte is valid (`aaaa` from a single `a`).

## Limits

| Quantity | Min | Max |
|----------|-----|-----|
| offset | 1 | current `out_pos` |
| literal L | 1 | remaining output space |
| last-match L | 1 | remaining output space |
| new-match L | 2 | remaining output space |
| uncompressed size in container | 0 | 2^32 - 1 |

Typical encoder target is up to about 5 MiB. That is not a bitstream limit.

## Error conditions

A raw decoder returns `-1` and must not write past `out_max` or read past `in_size` when:

- input or output pointer is null
- `in_size <= 0` or `out_max <= 0`
- a required bit or byte is missing (truncated Elias, truncated literal, truncated LSB)
- decoded offset `< 1` or `> out_pos`
- a copy or literal would exceed `out_max`
- the stream ends without EOF

Bytes already written to `out` are undefined on error.

No distinct error codes exist in v0.1.

Leftover bytes after EOF are not an error.

## Container (informative here, normative for CLI)

Not part of the raw bitstream.

```
offset 0  4 bytes  magic 'L' 'Z' '0' '1'
offset 4  4 bytes  uncompressed_size, uint32 little-endian
offset 8  N bytes  raw LZ0 payload
```

If `uncompressed_size = 0`, payload must be empty.

If `uncompressed_size > 0`, payload is a raw stream that must decode to exactly that many bytes.

Wrong magic, length mismatch, or raw decode `-1` is a container error.

## Worked example

Uncompressed: `aa` (2 bytes). One possible encoding is a 2-byte literal run then EOF.

1. Omitted first `0`.
2. Elias(2) = `0 0 1`.
3. Bytes `0x61 0x61`.
4. Enter new/EOF: `1`.
5. EOF: `0`.

Bit/byte trace after the omitted bit:

- bits `0 0 1` consume a control byte whose high bits are `001....` then two literal bytes are stored as whole bytes, then remaining bits of a later control byte hold `10` for EOF.

Exact golden bytes for this and other cases live in `tests/vectors/` and must match both independent decoders.

## Authority

Normative pair:

- this file
- `lz0dec_ref.c`

They must agree. Golden vectors are the regression lock for that agreement.
