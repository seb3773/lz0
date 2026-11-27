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
make test       # smoke, bounds, vectors, boundary, malformed, guard, simd, sa_probe, short bench
make enc_tiers  # bit-exactness across SIMD tiers (AVX2 / SSE2 / NEON / RVV / scalar)
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
./bin/lz0dec -e min output.lz0 restored.bin   # also: fast, compact, tiny
```

`lz0dec` defaults to the fast assembly decoder on x86-64 (Linux, macOS, Windows), AArch64 (Linux, macOS), and RV64 (Linux). On unsupported targets, `fast`, `compact`, and `tiny` fall back to `lz0dec_min`.

The nine compression levels map to five distinct search presets tuned for speed, memory footprint, and compression density:

| Levels | Search |
|---|---|
| `1` | Streaming low-RAM greedy (`compress_fast_greedy`), 2-slot hash buckets, 4 candidates, no suffix array (`sa_win=0`) — fast low-RAM mode (`1.01–3.95 B/byte` peak heap) |
| `2` | SDP (`K=2`), hash chain 4, 8 candidates, no suffix array (`sa_win=0`) |
| `3` | SDP (`K=2`), hash chain 4, 8 candidates, suffix-array window 4 (`sa_adapt=1`) |
| `4–6` | SDP (`K=2`), hash chain 8, 16 candidates, suffix-array window 16 (`sa_adapt=1`), selective fine match emission. Level 6 is the default. |
| `7–8` | SDP (`K=8`), hash chain 32, 32 candidates, suffix-array window 64 (`sa_adapt=1`), hash-chain skip list |
| `9` | Ultra tiered-SoA parser (`K=64`, skip-list + depth-16 BT finder, 32 candidates, suffix-array window 96 with `sa_adapt=2` skewed probing) |

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

`lz0_config_level` initializes all parser and bitstream settings inside the caller's `lz0_config` struct (`len_code = 3`, `offset_lsb_bits = 8`). Concurrent `lz0_compress` calls using separate `lz0_config` structs are re-entrant and thread-safe (`make threadsafe`). Legacy globals `lz0_len_code` and `lz0_offset_lsb_bits` remain fixed at 3 and 8 for compatibility with encoding v0.1.

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

Matches come from a 3-byte hash chain (`2^17` buckets) and a suffix array built with SA-IS (enabled from level 3 upward). A prefix-doubling builder (`lz0_sa_algo = 0`) exists to check that both builders emit the same `sa[]` and `rank[]`. Every 16 KiB window is independently classified for high entropy, multi-byte raster/audio strides (`bpp`), and local table periodicity, allowing early incompressible bailout (`~480–510 MB/s`, `1.01 B/byte` peak heap on incompressible streams like `star.png`). Level 1 uses a streaming low-RAM greedy fast path (`compress_fast_greedy` in `lz0/lz0_compress.c`) with 2-slot hash buckets (`~79 ms` total across the 19-file corpus). Level 6 keeps two live SDP states per position with selective fine match emission and literal headroom tie-breaking. Level 9 uses `lz0/lz0_ultra.c`: a tiered-SoA sparse DP parser with active-row slab pooling (`8 / 32 / 64..256` slots), open-addressed hash lookup, `8x16` block-max eviction, and a reference-counted DAG backtrack arena (`K = 64` by default, depth-16 bounded binary tree + pure SA window `96`). From 16 MiB upward, `lz0_parse_ultra` falls back cleanly to `lz0_parse_sdp` (`K = 32`).

The scalar C kernels and the x86-64 (SSE2 / AVX2), AArch64 (NEON), and RISC-V (RVV 1.0) vector kernels are checked against each other by `make test` (`simd`, `sa_probe`) and `make enc_tiers`. Any encoder that emits v0.1 is valid. H2C8 does not have to be reimplemented in order to decode LZ0. Notes on the search: [docs/LZ0_ENCODER.md](docs/LZ0_ENCODER.md).

## Decoders

| File | Role | Code Size (`.text`) |
|---|---|---|
| `lz0/lz0_decompress.c` | Library decoder, `lz0_decompress_to` | — |
| `lz0/lz0dec_ref.c` | Reference, read beside the format document | — |
| `lz0/lz0dec_min.c` | One C function, no heap, written to compile small | 1,086 B (x64) / 1,004 B (ARM64) / 782 B (RV64) at `-Os` |
| `lz0/lz0dec_tiny_x86_64.S`, `lz0dec_tiny_arm64.S`, `lz0dec_tiny_riscv64.S` | Tiny assembly, zero heap, full safety contract | 288 B (x64) / 376 B (ARM64) / 300 B (RV64) |
| `lz0/lz0dec_x86_64.S`, `lz0dec_arm64.S`, `lz0dec_riscv64.S` | Compact assembly, zero heap | 1,151 B (x64) / 1,296 B (ARM64) / 1,056 B (RV64) |
| `lz0/lz0dec_fast_x86_64.S`, `lz0dec_fast_arm64.S`, `lz0dec_fast_riscv64.S` | Larger assembly, aimed at throughput | 2,898 B (x64) / 2,852 B (ARM64) / 3,280 B (RV64) |

The tiny and compact AArch64 and RV64 decoders are leaf functions with zero stack frame (`lz0dec_tiny_x86_64` uses only `push rbp; mov rbp, rsp`). The x86-64 compact decoder saves only `rbx`/`rbp` plus at most one return address (`<= 32` bytes stack frame total). The fast decoders keep a small constant table, copy wider in the middle of the buffer, and take a checked path for the tail. `tests/test_guard.c` maps a `PROT_NONE` page immediately after the input and the output.

## Results

Per-file numbers, the corpus, and the machine are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md). Compressed size over original size, from the totals there:

| Set | Raw | LZ4 | LZ4HC -9 | LZ0 -1 | LZ0 -6 | LZ0 -9 |
|---|---:|---:|---:|---:|---:|---:|
| 19 files | 5,861,382 B | 64.43% | 53.46% | 53.47% | 45.14% | 43.36% |
| 8 concatenated blobs | 16,252,928 B | 80.85% | 75.82% | — | 67.72% | 66.99% |

The blob set is heavy with PNG, which is why those ratios sit higher. UPKR is smaller on text, executables, and `kennedy.xls` (6.30% against 18.93% for LZ0 -9). LZ0 -9 is smaller than UPKR on the already-compressed samples in that set (`avatar.jpg`, `star.png`, `break.mp3`), and early incompressible bailout keeps `star.png` at `100.00%` (`+5 B`) across all levels `-1..-9`. ZX0 is slightly smaller than LZ0 -9 on `fields.c` and `sum`. It was not run on the larger files.

LZ4 stores the match distance in 16 bits, so neither LZ4 nor LZ4HC can see a match farther than 65,535 bytes. That shows up on `blob_png_5m` and on the raw textures. LZ4 still decompresses several times faster. The 400–1400 MB/s band is the fast x86-64 decoder on the megabyte text and binaries. The multi-GB/s cells are short files or inputs that stay mostly literal. AArch64 and RISC-V figures in that document were taken under QEMU; use them to compare decoders on that emulator.

## License

MIT. Copyright (c) 2026 seb3773. See [LICENSE](LICENSE).

The stream layout is LZ0's. The choice of a trivial decoder, and Elias gamma for the integers, follows ZX0 by Einar Saukas.
