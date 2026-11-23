# LZ0
A lightweight, asymmetric LZ compressor designed around an extremely simple, memory-free decoder.

LZ0 is an experimental lossless compression format and C implementation designed around a single guiding principle:

Move complexity from the decoder to the encoder.

Instead of balancing computational cost equally between compression and decompression, LZ0 deliberately spends more effort during encoding to generate a compact, structured stream that can be decompressed by an ultra-minimal routine. The decoder requires no dynamic memory allocation (malloc), maintains minimal state, and processes explicit LZ tokens at hundreds of megabytes per second.

The encoder searches the whole buffer. The decoder allocates nothing, keeps a few words of state, and copies each match one byte at a time, so a match may overlap the bytes it has just written. A run of the same byte is a match at offset 1.

The public bitstream is MSB-first, with an 8-bit offset remainder and 3-bit codes for short match lengths. That encoding is v0.1, specified in [docs/LZ0_FORMAT.md](docs/LZ0_FORMAT.md). `lz0/lz0dec_ref.c` implements the same document. (A ZX0 decoder cannot read this stream)

---> It is aimed at blobs of a few kilobytes to a few megabytes whose decompressor has to stay small: a firmware image, a packer, a boot path. On the machine in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) (Core i3-9100), level 6 is a few seconds for a 2 MiB blob. Decompression of text and binaries with the x86-64 fast decoder lands around 400–1400 MB/s there. The ratio is ahead of LZ4 and LZ4HC, close to ZX0, and behind UPKR where an adaptive entropy coder pays for its tables. Already-compressed inputs stay near their original size.

## Build

Requires `gcc` or `clang`, and `make`.

```bash
make lz0        # bin/lz0 and bin/lz0dec
make test       # smoke, bounds, vectors, boundary, malformed, guard, simd, short bench
make fuzz
make sanitize   # ASan + UBSan
make valgrind
```

With a cross compiler and QEMU:

```bash
make test_arm64
make test_riscv64
make bench_dec
make bench_dec_arm64
make bench_dec_riscv64
make bench_all
```

## Command line

`lz0` wraps the raw stream in an 8-byte container: the bytes `LZ01`, then the uncompressed size as a little-endian `uint32`, then the payload. An empty file is that header with size 0 and no payload. The raw decoder rejects an empty buffer. The CLI rejects inputs of 4 GiB or larger, because the size field is 32 bits.

```bash
./bin/lz0 c input.bin output.lz0       # level 6
./bin/lz0 c -9 input.bin output.lz0
./bin/lz0 d output.lz0 restored.bin

./bin/lz0dec output.lz0 restored.bin
./bin/lz0dec -e min output.lz0 restored.bin   # also: fast, compact
```

`lz0dec` defaults to the fast assembly decoder on non-Windows x86-64, AArch64, and RV64. On Windows the assembly decoders are not built, and `fast` / `compact` both call `lz0dec_min`.

The nine levels select four configurations. Within a row, the number does not change the search.

| Levels | Search |
|---|---|
| 1–3 | SDP, K=2, hash chain 4, 8 candidates, suffix-array window 8 |
| 4–6 | SDP, K=2, hash chain 8, 16 candidates, suffix-array window 16. Level 6 is the default (H2C8). |
| 7–8 | SDP, K=8, hash chain 32, 32 candidates, suffix-array window 64, hash-chain skip list |
| 9 | Ultra parser, hash chain 64, 48 candidates, suffix-array window 128 with a wider probe, binary-tree finder (depth 32) |

## Library

```c
#include "lz0.h"

lz0_config cfg;
lz0_config_level(&cfg, 6);

unsigned char *compressed = NULL;
size_t comp_size = 0;
if (lz0_compress(in, in_size, &compressed, &comp_size, &cfg, NULL) != 0) {
    /* failed */
}
/* caller frees compressed */

int n = lz0_decompress_to(raw, (int)raw_size, out, (int)out_cap);
/* n is the number of bytes written, or -1 */
```

`lz0_compress` allocates the output and rejects a zero-length input. `lz0_decompress_to` writes only into the buffer you pass. On failure it returns `-1`; bytes already written are undefined. A bad offset, a truncated stream, or a write past `out_cap` all use that same code.

`lz0_len_code` and `lz0_offset_lsb_bits` are process-wide. The public tools set them to 3 and 8. Other values are outside encoding v0.1.

## Stream

The decoder consumes four operations.

| Operation | Arguments | Effect |
|---|---|---|
| Literal run | `L >= 1`, then L raw bytes | Copy from the input |
| Match at last offset | `L >= 1` | Copy from `out_pos - last_offset` |
| Match at new offset | offset `>= 1`, `L >= 2` | Set `last_offset`, then copy |
| End | | Stop |

`last_offset` starts at 1. The first operation is always a literal run, and the decision bit that would have selected it is omitted. After that:

- after a literal, `0` repeats `last_offset`, and `1` enters the new-offset loop
- after a repeated offset, `0` starts a literal run, and `1` enters that loop
- in the loop, `0` ends the stream, and `1` is a new offset
- after a new offset, `0` starts a literal run, and `1` stays in the loop

A new offset stores `((offset - 1) >> 8) + 1` as inverted Elias gamma, then the byte `255 - ((offset - 1) & 255)`. The decoder rebuilds `msb * 256 - byte`. The offset has to land inside the bytes already produced.

Literal bytes and that remainder byte sit in the byte stream as whole bytes. They leave the current control byte alone: bits still waiting in it are used next. Leftover low bits of the final control byte are ignored.

The copy is a byte loop. A length greater than the offset repeats bytes just written.

Gamma coding, the length code, and the worked examples are in [docs/LZ0_FORMAT.md](docs/LZ0_FORMAT.md). The decoder procedure is in [docs/LZ0_DECODER_SPEC.md](docs/LZ0_DECODER_SPEC.md).

## Encoder

Matches come from a 3-byte hash chain (`2^17` buckets) and a suffix array built with SA-IS. A prefix-doubling builder (`lz0_sa_algo = 0`) exists to check that both builders emit the same `sa[]` and `rank[]`. The parser then picks the tokens. Level 6 keeps two live states per position and skips lengths that sit on the same Elias cost plateau. Level 9 uses `lz0/lz0_ultra.c`: the same model, states stored as 12-byte rows, a 768 MiB budget (K from 128 down to a floor of 12). From 16 MiB upward that parser calls the generic SDP parser instead, because the position no longer fits in the packed state. The level-9 finder settings stay in place, with K = 32.

The scalar C kernels and the x86-64, AArch64, and RISC-V vector kernels are checked against each other by `make test` (target `simd`). Any encoder that emits v0.1 is valid. H2C8 does not have to be reimplemented in order to decode LZ0. Notes on the search: [docs/LZ0_ENCODER.md](docs/LZ0_ENCODER.md).

## Decoders

| File | Role |
|---|---|
| `lz0/lz0_decompress.c` | Library decoder, `lz0_decompress_to` |
| `lz0/lz0dec_ref.c` | Reference, read beside the format document |
| `lz0/lz0dec_min.c` | One C function, no heap, written to compile small |
| `lz0/lz0dec_x86_64.S`, `lz0dec_arm64.S`, `lz0dec_riscv64.S` | Compact assembly |
| `lz0/lz0dec_fast_x86_64.S`, `lz0dec_fast_arm64.S`, `lz0dec_fast_riscv64.S` | Larger assembly, aimed at throughput |

The compact AArch64 and RV64 decoders are leaves with no stack frame. The x86-64 compact decoder uses a 32-byte frame. The fast decoders keep a small constant table, copy wider in the middle of the buffer, and take a checked path for the tail. `tests/test_guard.c` maps a `PROT_NONE` page immediately after the input and the output.

## Results

Per-file numbers, the corpus, and the machine are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). Compressed size over original size, from the totals there:

| Set | Raw | LZ4 | LZ4HC -9 | LZ0 -6 | LZ0 -9 |
|---|---:|---:|---:|---:|---:|
| 19 files | 5,861,382 B | 64.43% | 53.46% | 45.26% | 43.36% |
| 8 concatenated blobs | 16,252,928 B | 80.85% | 75.82% | 67.53% | 67.06% |

The blob set is heavy with PNG, which is why those ratios sit higher. UPKR is smaller on text, executables, and `kennedy.xls` (6.30% against 19.11% for LZ0 -9). LZ0 -9 is smaller than UPKR on the already-compressed samples in that set (`avatar.jpg`, `star.png`, `break.mp3`). ZX0 is slightly smaller than LZ0 -9 on `fields.c` and `sum`. It was not run on the larger files.

LZ4 stores the match distance in 16 bits, so neither LZ4 nor LZ4HC can see a match farther than 65,535 bytes. That shows up on `blob_png_5m` and on the raw textures. LZ4 still decompresses several times faster. The 400–1400 MB/s band is the fast x86-64 decoder on the megabyte text and binaries. The multi-GB/s cells are short files or inputs that stay mostly literal. AArch64 and RISC-V figures in that document were taken under QEMU; use them to compare decoders on that emulator.

## License

MIT. Copyright (c) 2026 seb3773. See [LICENSE](LICENSE).

The stream layout is LZ0's. The choice of a trivial decoder, and Elias gamma for the integers, follows ZX0 by Einar Saukas.
