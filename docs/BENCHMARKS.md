# LZ0 Benchmarks & Performance Guide

This document compares **LZ0** (release tag 0.4.0 in the Makefile; bitstream v0.1) with **UPKR** (LZ plus an adaptive rANS coder), **LZ4 / LZ4HC**, and **ZX0**. ZX0 is a different bitstream. Its decoder is small and its parser is optimal; it does not read LZ0.

All benchmarks in this document were measured on an **Intel(R) Core(TM) i3-9100 CPU @ 3.60GHz (Coffee Lake Refresh, 4 cores / 4 threads, 6 MiB L3 cache)** running Linux x86_64 (GCC `-O3`). Reproducible via `make bench_all`, `make bench_blobs`, and `make bench_dec`.  
All LZ0 decompressions reported below were verified **100% bit-exact** using both reference and high-performance decoders (`lz0dec_fast`, `lz0dec_compact`, `lz0dec_min`, `lz0dec_ref`).

---

## 1. Real-World Corpus: Representative Individual Files

Multi-compressor evaluation across diverse real-world formats: machine code (ELF x86_64 and SPARC), Windows PE executables, compressed images (PNG, JPEG), pre-compressed audio streams (MP3 CBR), uncompressed raw multimedia data (RGB24 and RGBA32 textures, stereo 16-bit PCM audio), structured tabular spreadsheets (XLS), literary prose, and C source code.

### Comparative Results

| File | Type / Description | Raw Size | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (default) | LZ4HC (`-9`) | ZX0 | Decomp. Throughput `lz0dec_fast` vs LZ4 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | C source code | 11 150 B | 3 407 B (**30.56%**) | **3 344 B (29.99%)** | 2 928 B (26.26%) [-9] | 5 215 B (46.77%) | 4 228 B (37.92%) | 3 214 B (28.83%) | **994 MB/s** vs 2 577 MB/s |
| `sum` | SPARC ELF binary | 38 240 B | 12 139 B (**31.74%**) | **11 766 B (30.77%)** | 9 927 B (25.96%) [-6] | 18 812 B (49.19%) | 16 333 B (42.71%) | 11 415 B (29.85%) | **791 MB/s** vs 2 948 MB/s |
| `lz0_win64.exe` | Windows PE64 binary | 146 944 B | 75 289 B (**51.24%**) | **72 615 B (49.42%)** | 67 560 B (45.98%) [-6] | 101 596 B (69.14%) | 87 689 B (59.68%) | *(too slow)* | **406 MB/s** vs 3 375 MB/s |
| `star.png` | PNG Image (Deflate) | 355 666 B | 357 309 B (**100.46%**) | **356 400 B (100.21%)** | 364 646 B (102.52%) [-2] | 356 988 B (100.37%) | 356 960 B (100.36%) | *(too slow)* | **9 217 MB/s** vs 30 225 MB/s |
| `avatar.jpg` | JPEG Image | 25 380 B | 25 334 B (**99.82%**) | **25 268 B (99.56%)** | 25 749 B (101.45%) [-6] | 25 379 B (100.00%) | 25 326 B (99.79%) | *(too slow)* | **8 205 MB/s** vs 30 985 MB/s |
| `break.mp3` | Audio MP3 CBR | 38 080 B | 11 289 B (**29.65%**) | **11 246 B (29.53%)** | 11 287 B (29.64%) [-6] | 11 552 B (30.34%) | 11 473 B (30.13%) | *(too slow)* | **7 210 MB/s** vs 16 687 MB/s |
| `texture_rgb24.raw` | Raw RGB 24-bit texture | 196 608 B | 154 475 B (**78.57%**) | **131 157 B (66.71%)** | 86 531 B (44.01%) [-6] | 197 380 B (100.39%) | 197 170 B (100.29%) | *(too slow)* | **290 MB/s** vs 30 108 MB/s |
| `texture_rgba32.raw`| Raw RGBA 32-bit texture | 262 144 B | 144 027 B (**54.94%**) | **134 122 B (51.16%)** | *(excluded >256K)* | 263 173 B (100.39%) | 262 907 B (100.29%) | *(too slow)* | **406 MB/s** vs 28 616 MB/s |
| `audio_pcm16.raw` | Stereo PCM16 audio | 192 000 B | 139 027 B (**72.41%**) | **134 230 B (69.91%)** | 117 053 B (60.97%) [-6] | 163 730 B (85.28%) | 157 424 B (81.99%) | *(too slow)* | **416 MB/s** vs 8 148 MB/s |
| `alice29.txt` | English literature text | 152 089 B | 59 804 B (**39.32%**) | **58 323 B (38.35%)** | 50 488 B (33.20%) [-6] | 88 699 B (58.32%) | 63 673 B (41.87%) | *(too slow)* | **582 MB/s** vs 2 575 MB/s |
| `plrabn12.txt` | Classical poetry | 481 861 B | 214 066 B (**44.42%**) | **206 804 B (42.92%)** | 175 776 B (36.48%) [-2] | 325 589 B (67.57%) | 227 457 B (47.20%) | *(too slow)* | **581 MB/s** vs 2 687 MB/s |
| `kennedy.xls` | Excel spreadsheet | 1 029 744 B | 204 890 B (**19.90%**) | **196 737 B (19.11%)** | 64 830 B (6.30%) [-2] | 374 704 B (36.39%) | 324 190 B (31.48%) | *(too slow)* | **1 352 MB/s** vs 2 719 MB/s |
| `dickens_1m.txt`| Text corpus (1 MiB) | 1 048 576 B | 411 317 B (**39.23%**) | **393 497 B (37.53%)** | 331 404 B (31.61%) [-2] | 642 924 B (61.31%) | 445 113 B (42.45%) | *(too slow)* | **631 MB/s** vs 1 887 MB/s |
| `bash_x86_64.bin`| ELF x86_64 binary | 1 298 416 B | 620 832 B (**47.81%**) | **593 597 B (45.72%)** | 553 594 B (42.64%) [-2] | 871 664 B (67.13%) | 714 612 B (55.04%) | *(too slow)* | **400 MB/s** vs 2 652 MB/s |

---

## 2. Concatenated Heterogeneous Multi-Resource Blobs (Approach A)

In many embedded environments (microcontrollers, firmware, game loaders, packed executables), resources are grouped into a **single monolithic blob** resulting from concatenating disparate files (machine code, images, tables, texts).

Under **Approach A**, the compressed data remains **one continuous LZ0 stream**, decompressed in one pass by the freestanding decoders (`lz0dec_min` or the compact assembly), with no container between the concatenated files.

### Comparative Results on Blobs (256 KiB to 5 MiB, 3 to 15 sub-files)

| Target / Contents | Raw Size | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (default) | LZ4HC (`-9`) | Decomp. Throughput `lz0dec_fast` vs UPKR |
|---|---:|---:|---:|---:|---:|---:|---:|
| **`blob_png_256k`**<br>(4 PNGs) | 262 144 B | 247 405 B (94.38%) | **246 192 B (93.91%)** | 247 888 B (94.56%) [-6] | 252 446 B (96.30%) | 248 420 B (94.76%) | **1 625 MB/s** vs 17.6 MB/s (**92×**) |
| **`blob_mix_256k`**<br>(5 files: ELF, PE, XLS, PNG, txt) | 262 144 B | 128 155 B (48.89%) | 124 976 B (47.67%) | **110 904 B (42.31%)** [-6] | 168 178 B (64.15%) | 149 335 B (56.97%) | **673 MB/s** vs 37.9 MB/s (**18×**) |
| **`blob_png_512k`**<br>(8 PNGs) | 524 288 B | 504 320 B (96.19%) | **502 292 B (95.80%)** | 508 723 B (97.03%) [-2] | 511 352 B (97.53%) | 505 646 B (96.44%) | **2 160 MB/s** vs 17.1 MB/s (**126×**) |
| **`blob_mix_512k`**<br>(7 files: ELF, PE, 2 PNGs, XLS, C, txt) | 524 288 B | 286 374 B (54.62%) | 281 539 B (53.70%) | **260 451 B (49.68%)** [-2] | 352 088 B (67.16%) | 318 022 B (60.66%) | **767 MB/s** vs 32.2 MB/s (**24×**) |
| **`blob_png_2m`**<br>(8 distinct PNGs > 200 KiB) | 2 097 152 B | 2 056 945 B (98.08%) | **2 051 622 B (97.83%)** | 2 089 034 B (99.61%) [-1] | 2 067 355 B (98.58%) | 2 053 832 B (97.93%) | **4 042 MB/s** vs ~15 MB/s (**269×**) |
| **`blob_mix_2m`**<br>(9 files: ELF, PE, 3 PNGs, XLS, txt, C, HTML) | 2 097 152 B | 1 198 585 B (57.15%) | 1 181 308 B (56.33%) | **1 125 643 B (53.67%)** [-1] | 1 484 306 B (70.78%) | 1 308 970 B (62.42%) | **681 MB/s** vs ~15 MB/s (**45×**) |
| **`blob_png_5m`**<br>(15 distinct PNGs) | 5 242 880 B | **4 362 024 B (83.20%)** | **4 353 045 B (83.03%)** | *(excluded >20 min)* | 5 210 986 B (99.39%) | 5 183 917 B (98.88%) | **3 606 MB/s** vs - |
| **`blob_mix_5m`**<br>(15 distinct real-world files) | 5 242 880 B | 2 191 126 B (41.79%) | **2 157 767 B (41.16%)** | *(excluded >30 min)* | 3 093 616 B (59.01%) | 2 554 646 B (48.73%) | **733 MB/s** vs - |

---

## 3. In-Depth Comparison: LZ0 vs LZ4 and LZ4HC

While standard LZ4 prioritizes extreme single-pass encoding throughput, **LZ4HC** (`-9` / `-12`) applies an optimal forward parser within the constraints of the standard LZ4 format. Comparing LZ0 against both variants demonstrates the intrinsic advantages of LZ0's prefix-coded Elias gamma bitstream and sliding window model:

### Comprehensive Individual Files Summary

| Target | Raw Size | LZ4 default | LZ4HC (`-9`) | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | Net Gain LZ0 Ultra vs LZ4HC (`-9`) | Decomp: LZ0 FAST vs LZ4 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | 11,150 B | 5,215 B (46.77%) | 4,228 B (37.92%) | 3,407 B (30.56%) | **3,344 B (29.99%)** | **-884 B (-20.9%)** | 994 MB/s vs 2,577 MB/s |
| `grammar.lsp` | 3,721 B | 1,912 B (51.38%) | 1,724 B (46.33%) | 1,440 B (38.70%) | **1,414 B (38.00%)** | **-310 B (-18.0%)** | 1,020 MB/s vs 2,408 MB/s |
| `xargs.1` | 4,227 B | 2,658 B (62.88%) | 2,410 B (57.01%) | 2,038 B (48.21%) | **2,005 B (47.43%)** | **-405 B (-16.8%)** | 950 MB/s vs 2,520 MB/s |
| `cp.html` | 24,603 B | 11,905 B (48.39%) | 10,338 B (42.02%) | 9,255 B (37.62%) | **8,974 B (36.48%)** | **-1,364 B (-13.2%)** | 980 MB/s vs 3,902 MB/s |
| `sum` | 38,240 B | 18,812 B (49.19%) | 16,333 B (42.71%) | 12,139 B (31.74%) | **11,766 B (30.77%)** | **-4,567 B (-28.0%)** | 791 MB/s vs 2,948 MB/s |
| `alice29.txt` | 152,089 B | 88,699 B (58.32%) | 63,673 B (41.87%) | 59,804 B (39.32%) | **58,323 B (38.35%)** | **-5,350 B (-8.4%)** | 582 MB/s vs 2,575 MB/s |
| `asyoulik.txt` | 125,179 B | 79,653 B (63.63%) | 58,908 B (47.06%) | 54,548 B (43.58%) | **53,241 B (42.53%)** | **-5,667 B (-9.6%)** | 553 MB/s vs 2,581 MB/s |
| `plrabn12.txt` | 481,861 B | 325,589 B (67.57%) | 227,457 B (47.20%) | 214,066 B (44.42%) | **206,804 B (42.92%)** | **-20,653 B (-9.1%)** | 581 MB/s vs 2,687 MB/s |
| `lcet10.txt` | 426,754 B | 233,213 B (54.65%) | 165,788 B (38.85%) | 152,228 B (35.67%) | **147,042 B (34.46%)** | **-18,746 B (-11.3%)** | 592 MB/s vs 2,667 MB/s |
| `kennedy.xls` | 1,029,744 B | 374,704 B (36.39%) | 324,190 B (31.48%) | 204,890 B (19.90%) | **196,737 B (19.11%)** | **-127,453 B (-39.3%)** | 1,352 MB/s vs 2,719 MB/s |
| `dickens_1m.txt` | 1,048,576 B | 642,924 B (61.31%) | 445,113 B (42.45%) | 411,317 B (39.23%) | **393,497 B (37.53%)** | **-51,616 B (-11.6%)** | 631 MB/s vs 1,887 MB/s |
| `lz0_win64.exe` | 146,944 B | 101,596 B (69.14%) | 87,689 B (59.68%) | 75,289 B (51.24%) | **72,615 B (49.42%)** | **-15,074 B (-17.2%)** | 406 MB/s vs 3,375 MB/s |
| `bash_x86_64.bin` | 1,298,416 B | 871,664 B (67.13%) | 714,612 B (55.04%) | 620,832 B (47.81%) | **593,597 B (45.72%)** | **-121,015 B (-16.9%)** | 400 MB/s vs 2,652 MB/s |
| `texture_rgb24.raw` | 196,608 B | 197,380 B (100.39%) | 197,170 B (100.29%) | 154,475 B (78.57%) | **131,157 B (66.71%)** | **-66,013 B (-33.5%)** | 290 MB/s vs 30,108 MB/s |
| `texture_rgba32.raw` | 262,144 B | 263,173 B (100.39%) | 262,907 B (100.29%) | 144,027 B (54.94%) | **134,122 B (51.16%)** | **-128,785 B (-49.0%)** | 406 MB/s vs 28,616 MB/s |
| `audio_pcm16.raw` | 192,000 B | 163,730 B (85.28%) | 157,424 B (81.99%) | 139,027 B (72.41%) | **134,230 B (69.91%)** | **-23,194 B (-14.7%)** | 416 MB/s vs 8,148 MB/s |
| `avatar.jpg` | 25,380 B | 25,379 B (100.00%) | 25,326 B (99.79%) | 25,334 B (99.82%) | **25,268 B (99.56%)** | **-58 B (-0.2%)** | 8,205 MB/s vs 30,985 MB/s |
| `star.png` | 355,666 B | 356,988 B (100.37%) | 356,960 B (100.36%) | 357,309 B (100.46%) | **356,400 B (100.21%)** | **-560 B (-0.2%)** | 9,217 MB/s vs 30,225 MB/s |
| `break.mp3` | 38,080 B | 11,552 B (30.34%) | 11,473 B (30.13%) | 11,289 B (29.65%) | **11,246 B (29.53%)** | **-227 B (-2.0%)** | 7,210 MB/s vs 16,687 MB/s |
| **TOTAL** | **5,861,382 B** | **3,776,746 B (64.43%)** | **3,133,723 B (53.46%)** | **2,652,714 B (45.26%)** | **2,541,782 B (43.36%)** | **-591,941 B (-18.9%)** | — |

### Concatenated Blobs & Packages Summary

| Target | Raw Size | LZ4 default | LZ4HC (`-9`) | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | Net Gain LZ0 Ultra vs LZ4HC (`-9`) | Decomp: LZ0 FAST vs LZ4 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `blob_mix_256k.bin` | 262,144 B | 168,178 B (64.15%) | 149,335 B (56.97%) | 128,155 B (48.89%) | **124,976 B (47.67%)** | **-24,359 B (-16.3%)** | 673 MB/s vs 2,563 MB/s |
| `blob_png_256k.bin` | 262,144 B | 252,446 B (96.30%) | 248,420 B (94.76%) | 247,405 B (94.38%) | **246,192 B (93.91%)** | **-2,228 B (-0.9%)** | 1,625 MB/s vs 14,992 MB/s |
| `blob_mix_512k.bin` | 524,288 B | 352,088 B (67.16%) | 318,022 B (60.66%) | 286,374 B (54.62%) | **281,539 B (53.70%)** | **-36,483 B (-11.5%)** | 767 MB/s vs 3,628 MB/s |
| `blob_png_512k.bin` | 524,288 B | 511,352 B (97.53%) | 505,646 B (96.44%) | 504,320 B (96.19%) | **502,292 B (95.80%)** | **-3,354 B (-0.7%)** | 2,160 MB/s vs 19,128 MB/s |
| `blob_mix_2m.bin` | 2,097,152 B | 1,484,306 B (70.78%) | 1,308,970 B (62.42%) | 1,198,585 B (57.15%) | **1,181,308 B (56.33%)** | **-127,662 B (-9.8%)** | 681 MB/s vs 3,004 MB/s |
| `blob_png_2m.bin` | 2,097,152 B | 2,067,355 B (98.58%) | 2,053,832 B (97.93%) | 2,056,945 B (98.08%) | **2,051,622 B (97.83%)** | **-2,210 B (-0.1%)** | 4,042 MB/s vs 5,382 MB/s |
| `blob_mix_5m.bin` | 5,242,880 B | 3,093,616 B (59.01%) | 2,554,646 B (48.73%) | 2,191,126 B (41.79%) | **2,157,767 B (41.16%)** | **-396,879 B (-15.5%)** | 733 MB/s vs 2,806 MB/s |
| `blob_png_5m.bin` | 5,242,880 B | 5,210,986 B (99.39%) | 5,183,917 B (98.88%) | 4,362,024 B (83.20%) | **4,353,045 B (83.03%)** | **-830,872 B (-16.0%)** | 3,606 MB/s vs 4,588 MB/s |
| **TOTAL** | **16,252,928 B** | **13,140,327 B (80.85%)** | **12,322,788 B (75.82%)** | **10,974,934 B (67.53%)** | **10,898,741 B (67.06%)** | **-1,424,047 B (-11.6%)** | — |

### Technical Analysis: Why LZ4HC Cannot Reach LZ0 Density

1. **The 64 KiB Window Ceiling**:  
   LZ4 offsets are structurally restricted to `uint16_t` (65,535 bytes maximum distance). On inputs $\ge 1$ MiB, repetitions distant by more than 64 KiB are completely invisible to LZ4HC. LZ0 uses unbounded prefix codes (Elias gamma MSB + 8-bit LSB), allowing it to match across megabytes. On `blob_png_5m.bin`, LZ0 Ultra saves **830,872 bytes** over LZ4HC.
2. **Byte-Aligned Token Tax**:  
   LZ4 requires 1 full byte for its token (run lengths) plus 2 full bytes for offset, establishing a minimum match overhead of 24 bits. In LZ0, a single control bit dictates token type, and `MatchLast` reuses the previous offset for only 1 bit plus length code.
3. **No Inflation on High Entropy**:  
   On the raw textures and `star.png` in this set, LZ4 grows the file by about 0.3% to 0.4%. LZ0 -9 stays at or under the original size on `avatar.jpg` and grows `star.png` by 0.21%.
4. **Minimal Decoder Equivalence**:  
   While a bare-metal unsafe LZ4 decompressor can be written in ~150–200 bytes of assembly (or ~1–2 KiB safe), LZ0's standalone compact assembly decoders achieve a minimal footprint (~1.1–1.3 KiB code size, 0 heap, 0 stack) despite decoding variable-bit Elias gamma codes and unbounded window offsets. The minimal C99 decoder (`lz0dec_min.c`) compiles to only ~800–900 bytes with `-Os` (806–894 B on RV32IMC).
5. **Decompression Throughput**:  
   In section 4, LZ4 decompresses text and binaries at about 1.9–3.4 GB/s, and faster on the images. The fast LZ0 decoder is the 400–1,350 MB/s band on those text and binary files. UPKR's C unpacker in the same table spans 16–175 MB/s. ZX0 decode speed was not measured here.

---

## 4. Decompression Throughput (Minimal C vs Standard C vs Vector Assembly)

Reproduction commands:
```bash
make bench_all          # Comprehensive ratio & throughput comparison
make bench_dec          # Native x86-64 throughput micro-benchmarks
make bench_dec_arm64    # AArch64 throughput micro-benchmarks (QEMU)
make bench_dec_riscv64  # RISC-V 64 throughput micro-benchmarks (QEMU)
```

### Native x86-64 Decompression Throughput

| File | Minimal C (`lz0dec_min`) | Standard C (`lz0_decompress_to`) | ASM Compact (`lz0dec_x86_64`) | ASM FAST (`lz0dec_fast_x86_64`) | UPKR (C unpacker) | LZ4 (`decompress_safe`) |
|---|---:|---:|---:|---:|---:|---:|
| `fields.c` | 326 MB/s | 364 MB/s | 533 MB/s | **1 070 MB/s** | 60 MB/s | 2 577 MB/s |
| `sum` | 261 MB/s | 284 MB/s | 370 MB/s | **791 MB/s** | 62 MB/s | 2 948 MB/s |
| `lz0_win64.exe` | 155 MB/s | 166 MB/s | 225 MB/s | **406 MB/s** | 36 MB/s | 3 375 MB/s |
| `star.png` | 1 264 MB/s | 4 664 MB/s | 2 850 MB/s | **9 217 MB/s** | 16 MB/s | 30 225 MB/s |
| `avatar.jpg` | 1 222 MB/s | 4 753 MB/s | 2 710 MB/s | **8 205 MB/s** | 16 MB/s | 30 985 MB/s |
| `break.mp3` | 1 870 MB/s | 2 809 MB/s | 3 100 MB/s | **7 210 MB/s** | 56 MB/s | 16 688 MB/s |
| `alice29.txt` | 196 MB/s | 209 MB/s | 296 MB/s | **582 MB/s** | 44 MB/s | 2 575 MB/s |
| `plrabn12.txt` | 180 MB/s | 201 MB/s | 271 MB/s | **581 MB/s** | 40 MB/s | 2 687 MB/s |
| `kennedy.xls` | 576 MB/s | 632 MB/s | 876 MB/s | **1 352 MB/s** | 175 MB/s | 2 719 MB/s |
| `dickens_1m.txt` | 192 MB/s | 221 MB/s | 300 MB/s | **631 MB/s** | 48 MB/s | 1 887 MB/s |
| `bash_x86_64.bin` | 174 MB/s | 182 MB/s | 254 MB/s | **404 MB/s** | 38 MB/s | 2 652 MB/s |

### AArch64 / ARM64 Decompression Throughput (QEMU)

| File | Standard C (`lz0_decompress_to`) | Minimal C (`lz0dec_min`) | ASM Compact (`lz0dec_arm64`) | ASM FAST (`lz0dec_fast_arm64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 58.7 MB/s | 55.4 MB/s | 114.3 MB/s | **182.2 MB/s** | **+211%** |
| `alice29.txt` | 47.1 MB/s | 42.8 MB/s | 81.1 MB/s | **167.1 MB/s** | **+255%** |
| `plrabn12.txt` | 44.3 MB/s | 38.1 MB/s | 65.8 MB/s | **162.7 MB/s** | **+267%** |
| `kennedy.xls` | 83.1 MB/s | 73.8 MB/s | 173.9 MB/s | **207.1 MB/s** | **+149%** |
| `dickens_1m.txt` | 48.7 MB/s | 41.9 MB/s | 76.3 MB/s | **133.6 MB/s** | **+174%** |
| `bash_x86_64.bin` | 41.3 MB/s | 39.9 MB/s | 74.2 MB/s | **109.7 MB/s** | **+165%** |

### RISC-V 64 / RV64GC Decompression Throughput (QEMU)

| File | Standard C (`lz0_decompress_to`) | Minimal C (`lz0dec_min`) | ASM Compact (`lz0dec_riscv64`) | ASM FAST (`lz0dec_fast_riscv64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 36.5 MB/s | 122.4 MB/s | 141.8 MB/s | **186.6 MB/s** | **+412%** |
| `alice29.txt` | 24.7 MB/s | 90.7 MB/s | 110.8 MB/s | **122.0 MB/s** | **+393%** |
| `plrabn12.txt` | 21.1 MB/s | 79.0 MB/s | 92.9 MB/s | **113.8 MB/s** | **+440%** |
| `kennedy.xls` | 45.4 MB/s | 202.9 MB/s | 167.3 MB/s | **309.9 MB/s** | **+583%** |
| `dickens_1m.txt` | 23.4 MB/s | 81.6 MB/s | 99.0 MB/s | **124.6 MB/s** | **+432%** |
| `bash_x86_64.bin` | 21.6 MB/s | 85.2 MB/s | 78.4 MB/s | **112.2 MB/s** | **+420%** |

---

## 5. Key Takeaways and Technical Analysis

### LZ0 vs LZ4: Massive Density Gain with Comfortable Throughput
- Across all structured and binary corpora, **LZ0 reduces compressed size by 15% to 46% compared to LZ4**:
  - On `kennedy.xls`: LZ0 compresses to **196 KiB (19.11%)** vs LZ4 at **374 KiB (36.4%)** (size almost halved).
  - On `sum` (SPARC ELF): LZ0 compresses to **11.7 KiB** vs LZ4 at **18.8 KiB** (-37%).
  - On `blob_mix_2m`: LZ0 compresses to **1.18 MiB** vs LZ4 at **1.48 MiB** (-300 KiB difference).
  - On `blob_mix_5m`: LZ0 compresses to **2.15 MiB** vs LZ4 at **3.09 MiB** (-940 KiB difference).
- On uncompressed raw textures:
  - On `texture_rgba32.raw`: LZ0 compresses to **134 KiB (51.16%)** whereas LZ4 produces inflation (+100.4%).
  - On `texture_rgb24.raw`: LZ0 compresses to **131 KiB (66.71%)** whereas LZ4 produces inflation (+100.4%).
  - On `audio_pcm16.raw`: LZ0 compresses to **134 KiB (69.91%)** vs LZ4 at **163 KiB (85.3%)** (~30 KiB savings).
- In return, `lz0dec_fast` delivers high decompression throughput (**400 to 1,355 MB/s** on text/code and up to **9,000+ MB/s** on images) with an assembly decoder under ~2.7 KiB, zero hash tables, and zero auxiliary heap allocations.

### LZ0 vs UPKR: Superiority on Pre-Compressed Media and Decompression Asymmetry
- **On PNG, JPEG, and High-Entropy Data**: UPKR's probabilistic rANS model suffers from significant structural overhead. LZ0, through byte-aligned literals and strict profitability filtering, **systematically outperforms UPKR** on all pre-compressed files:
  - On `avatar.jpg`: LZ0 compresses to **25 268 B (99.56%)**, beating UPKR (**25 749 B**, inflated at 101.5%).
  - On `break.mp3`: LZ0 compresses to **11 246 B (29.53%)**, beating UPKR (**11 287 B**) and LZ4 (**11 552 B**).
  - On `blob_png_512k`: LZ0 compresses to **502 KiB** vs UPKR at **508 KiB** (-6 KiB difference).
- **Decompression Speed**: `lz0dec_fast` decompresses **18× to 269× faster than UPKR** (which caps out around 15–38 MB/s).
- **Encoding Speed**: LZ0 Normal compresses a 2 MiB blob in ~2.7 seconds where UPKR requires several minutes (**100× faster**).

### Strict Length-2 Profitability Filtering
In high-entropy streams (JPEG, MP3, PNG, encrypted binaries), emitting a 2-byte LZ77 match with a large offset costs more command bits than emitting 2 raw literals.
- LZ0 enforces a strict filter in default modes: length-2 matches are only permitted if the offset is tiny ($\le 32$) or if it is a **repeat offset** (zero offset bits).
- On `avatar.jpg`, both -6 and -9 stay under the original size (99.82% and 99.56%). `star.png` still grows: 100.46% at -6, 100.21% at -9.

### Audio & Multimedia Periodicity Detection (Parity-Safe Strides)
- **Audio MP3 CBR**: MPEG Layer III frame headers are synchronized via a strict filter validating the sync word `0xFF 0xE0`, exact 4-byte header repetition (`memcmp(win+i, win+i-p, 4) == 0`), the Layer III bit, and valid bitrates, eliminating 100% of false positives on graphic textures.
- **RGB24 / RGBA32 Textures & PCM16 Audio**: Autocorrelation uses non-folding parity sampling (`step = (p <= 32) ? 1 : 2`), guaranteeing exact detection of stride 3 (RGB24) and stride 4 (RGBA32 and stereo PCM16). Candidate strides are sorted by descending correlation to prune harmonic multiples.

### Dedicated SoA Ultra Parser ($K=128..12$) and Multi-Scale SA-IS
- In Ultra mode (`-9`), the dedicated `lz0_parse_ultra` parser uses a compact Structure-of-Arrays representation (12 bytes per state), a 64-bit Bloom filter, and cost-margin pruning:
  - **K=128** below 512 KiB, **K=63** at 1 MiB, **K=31** at 2 MiB, **K=12** at 5 MiB. The count is `budget / ((n + 1) * 12)` with a 768 MiB budget, a cap of 128, and a floor of 12. At 16 MiB the ultra parser stops and the generic SDP parser runs with K=32.
- Length-2 matches are evaluated without offset restrictions (`lz0_len2_max_off = 0`), unlocking massive gains on tabular and multimedia formats (RGB24 textures, RGBA32, XLS) without degrading high-entropy data thanks to margin pruning.
- Suffix-array probing at level 9 walks ranks 1 through 128, then steps by 32 and by 64: 160, 192, 224, 256, 320, 384, 448, 512.
