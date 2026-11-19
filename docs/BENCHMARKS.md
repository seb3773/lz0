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

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM pure (`lz0dec_x86_64`) | Speedup vs C `-O3` |
|---|---:|---:|---:|---:|
| `fields.c` | 305.8 MB/s | 368.1 MB/s | **421.2 MB/s** | **+37.7%** |
| `cp.html` | 275.5 MB/s | 321.4 MB/s | **346.7 MB/s** | **+25.8%** |
| `alice29.txt` | 206.9 MB/s | 260.4 MB/s | **280.4 MB/s** | **+35.5%** |
| `plrabn12.txt` | 179.7 MB/s | 224.9 MB/s | **244.5 MB/s** | **+36.1%** |
| `kennedy.xls` | 593.0 MB/s | 681.3 MB/s | **749.8 MB/s** | **+26.4%** |
| `dickens_1m.txt` | 200.1 MB/s | 247.9 MB/s | **281.5 MB/s** | **+40.7%** |
| `bash_x86_64.bin` | 179.3 MB/s | 225.8 MB/s | **248.6 MB/s** | **+38.6%** |

### AArch64 / ARM64 Throughput (under QEMU)

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM pure (`lz0dec_arm64`) | Speedup vs C standard |
|---|---:|---:|---:|---:|
| `fields.c` | 46.6 MB/s | 92.1 MB/s | **86.5 MB/s** | **+85.6%** |
| `cp.html` | 40.5 MB/s | 78.6 MB/s | **73.5 MB/s** | **+81.4%** |
| `alice29.txt` | 37.8 MB/s | 71.2 MB/s | **68.8 MB/s** | **+82.2%** |
| `plrabn12.txt` | 36.9 MB/s | 65.5 MB/s | **59.1 MB/s** | **+60.1%** |
| `kennedy.xls` | 54.8 MB/s | 128.5 MB/s | **125.7 MB/s** | **+129.2%** |
| `dickens_1m.txt` | 43.0 MB/s | 67.1 MB/s | **68.0 MB/s** | **+58.3%** |
| `bash_x86_64.bin` | 32.8 MB/s | 62.5 MB/s | **59.6 MB/s** | **+81.7%** |

Streams must remain byte-identical (`csz` / `bits` / tokens / round-trip) on those corpora.
