# LZ0 v0.1 benchmarks

Ratio is `csize / original_size * 100`.

Encoder: frozen H2C8, `lz0_sa_algo=1` (SA-IS), `lz0_h2c8_apply()`.

## Snapshot 2026-10-06

Compiler: `gcc -O3 -std=c11 -D_POSIX_C_SOURCE=200809L`

Command:

```
./bench/lz0_freeze
```

Datasets are generated in-process (same PRNG as `bench/freeze.c`), not stored files.

| size | kind | csize | ratio | enc_ms | dec_ms | sa_ms | rss_kb |
|------|------|------:|------:|-------:|-------:|------:|-------:|
| 256K | text | 48348 | 18.44% | 342 | 0.80 | 25 | 712 |
| 256K | bin | 17372 | 6.63% | 349 | 0.31 | 17 | 16100 |
| 1M | text | 189517 | 18.07% | 1342 | 3.23 | 88 | 7892 |
| 1M | bin | 60388 | 5.76% | 1617 | 0.99 | 103 | 23192 |
| 2M | text | 376764 | 17.97% | 2997 | 6.09 | 431 | 45184 |
| 2M | bin | 111839 | 5.33% | 3651 | 2.59 | 433 | 736 |
| 5M | text | 932133 | 17.78% | 8727 | 20.34 | 2030 | 70772 |
| 5M | bin | 256693 | 4.90% | 11428 | 4.12 | 2327 | 67948 |

Wall-clock encode times are host-noisy. SA build and encode CPU times from RDTSC-based census were used during encoder freeze; this table is the public v0.1 snapshot.

Reproduce:

```
make freeze
```

SA-IS vs prefix-doubling identity:

```
make saab
make streamab
```

## Real-World Corpus Comparison (Multi-Compressor)

Command:
```bash
make bench_all
```

Evaluated on standard Canterbury & Silesia corpus files comparing **LZ0** (`-6` default, `-9` ultra), **ZX0**, **UPKR** (`-9` / `-5`), and **LZ4** (default):

| File | Original Size | LZ0 (`-6` default) | LZ0 (`-9` ultra) | ZX0 | UPKR | LZ4 (default) |
|---|---:|---:|---:|---:|---:|---:|
| `fields.c` | 11.1 KiB | 3,367 B (**30.20%**) | 3,341 B (**29.96%**) | 3,214 B (**28.83%**) | 2,928 B (**26.26%**) | 5,215 B (46.77%) |
| `alice29.txt` | 148.5 KiB | 58,992 B (**38.79%**) | 58,568 B (**38.51%**) | *timeout (>60s)* | 50,547 B (**33.24%**) | 88,699 B (58.32%) |
| `plrabn12.txt` | 470.6 KiB | 209,592 B (**43.50%**) | 207,790 B (**43.12%**) | *timeout (>60s)* | 174,559 B (**36.23%**) | 325,589 B (67.57%) |
| `kennedy.xls` | 1,005.6 KiB | 204,305 B (**19.84%**) | 201,110 B (**19.53%**) | *timeout (>60s)* | 64,765 B (**6.29%**) | 374,704 B (36.39%) |
| `dickens_1m.txt` | 1,024.0 KiB | 400,138 B (**38.16%**) | 396,187 B (**37.78%**) | *timeout (>60s)* | 328,450 B (**31.32%**) | 642,924 B (61.31%) |
| `bash_x86_64.bin` | 1,268.0 KiB | 613,639 B (**47.26%**) | 599,401 B (**46.16%**) | *timeout (>60s)* | 553,594 B (**42.64%**) | 871,664 B (67.13%) |

## Decompression Throughput (C vs. Hand-Crafted Assembly)

Command:
```bash
make bench_dec        # Native x86-64
make bench_dec_arm64  # AArch64 under QEMU
```

### x86-64 Native Throughput

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_x86_64`) | ASM FAST (`lz0dec_fast_x86_64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 304.0 MB/s | 315.3 MB/s | 420.3 MB/s | **1,022.2 MB/s** | **+236.3%** |
| `cp.html` | 206.0 MB/s | 212.9 MB/s | 287.4 MB/s | **842.2 MB/s** | **+308.9%** |
| `alice29.txt` | 197.0 MB/s | 206.4 MB/s | 277.1 MB/s | **616.0 MB/s** | **+212.6%** |
| `plrabn12.txt` | 186.4 MB/s | 195.0 MB/s | 268.0 MB/s | **528.1 MB/s** | **+183.3%** |
| `kennedy.xls` | 598.1 MB/s | 600.4 MB/s | 757.2 MB/s | **1,330.3 MB/s** | **+122.4%** |
| `dickens_1m.txt` | 200.2 MB/s | 221.6 MB/s | 291.6 MB/s | **521.4 MB/s** | **+160.5%** |
| `bash_x86_64.bin` | 175.3 MB/s | 179.3 MB/s | 232.9 MB/s | **397.1 MB/s** | **+126.5%** |

### AArch64 / ARM64 Throughput (under QEMU)

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_arm64`) | ASM FAST (`lz0dec_fast_arm64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 43.4 MB/s | 94.9 MB/s | 74.5 MB/s | **261.0 MB/s** | **+502.0%** |
| `cp.html` | 35.6 MB/s | 76.2 MB/s | 55.3 MB/s | **214.9 MB/s** | **+503.6%** |
| `alice29.txt` | 34.9 MB/s | 68.9 MB/s | 41.8 MB/s | **213.0 MB/s** | **+510.5%** |
| `plrabn12.txt` | 32.3 MB/s | 64.8 MB/s | 34.6 MB/s | **175.6 MB/s** | **+443.7%** |
| `kennedy.xls` | 53.6 MB/s | 125.8 MB/s | 121.8 MB/s | **377.4 MB/s** | **+604.6%** |
| `dickens_1m.txt` | 36.4 MB/s | 72.6 MB/s | 35.6 MB/s | **170.1 MB/s** | **+367.6%** |
| `bash_x86_64.bin` | 30.8 MB/s | 61.3 MB/s | 45.2 MB/s | **143.7 MB/s** | **+366.6%** |

> [!NOTE]
> **Key Architectural Takeaways on Decompression Speedup**:
> * **On x86-64**: Decompression throughput doubles or triples on text and source code (jumping from ~200 MB/s to **520 – 840 MB/s**), and reaches **1,330 MB/s (1.33 GB/s)** on structured datasets (`kennedy.xls`). The speedup stems directly from eliminating branch misprediction penalties on Elias gamma bit-sequences via 64-bit window decoding, and deploying 16-byte SSE2 wildcopies.
> * **On AArch64 / ARM64**: Pure leaf function structure (0 bytes allocated on stack, 0 subroutine calls) combined with branch-free Elias gamma decoding drastically reduces dynamic instruction count, translating to a **5× to 7× speedup** over the standard C decoder.

> [!TIP]
> **Decoder Selection**:
> * Use **`lz0dec_min`** for extreme embedded microcontrollers (AVR, Cortex-M0, ROMs, kernel drivers) where every byte of code and RAM counts (~40 lines of pure C99, ~200 bytes machine code, zero table, zero malloc).
> * Use **`lz0dec_compact`** (`lz0dec_x86_64` / `lz0dec_arm64`) for compact, table-free assembly decompression (~1.3 KiB, zero table).
> * Use **`lz0dec_fast`** (`lz0dec_fast_x86_64` / `lz0dec_fast_arm64`) for maximum throughput on desktop/server/mobile platforms (reaches **800–1,330+ MB/s** on x86-64).

Streams must remain byte-identical (`csz` / `bits` / tokens / round-trip) on those corpora.
