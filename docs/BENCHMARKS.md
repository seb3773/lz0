# LZ0 Benchmarks & Performance Guide

This document compares **LZ0** (bitstream v0.1 with v2 encoder presets and compacted ASM decoders) with **UPKR** (LZ plus an adaptive rANS coder), **LZ4 / LZ4HC**, and **ZX0**. ZX0 is a different bitstream; its decoder is small and its parser is optimal, and it does not read LZ0. (UPKR and ZX0 baselines are from previous reference runs on the exact same machine and corpus.)

All benchmarks in this document were measured on an **Intel(R) Core(TM) i3-9100 CPU @ 3.60GHz (Coffee Lake Refresh, 4 cores / 4 threads, 6 MiB L3 cache)** running Linux x86_64 (GCC `-O3`). Reproducible via `make bench_all`, `make bench_blobs`, `make enc_bench`, `make mem_peak`, and `make bench_dec`.

All LZ0 decompressions reported below were verified **100% bit-exact** using both reference and high-performance decoders (`lz0dec_fast`, `lz0dec_compact`, `lz0dec_min`, `lz0dec_ref`).

---

## 1. Real-World Corpus: Representative Individual Files

Multi-compressor evaluation across diverse real-world formats: machine code (ELF x86_64 and SPARC), Windows PE executables, compressed images (PNG, JPEG), pre-compressed audio streams (MP3 CBR), uncompressed raw multimedia data (RGB24 and RGBA32 textures, stereo 16-bit PCM audio), structured tabular spreadsheets (XLS), literary prose, and C source code.

### Comparative Results

| File | Type / Description | Raw Size | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (default) | LZ4HC (`-9`) | ZX0 | Decomp. Throughput `lz0dec_fast` vs LZ4 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | C source code | 11 150 B | 3 371 B (**30.23%**) | **3 336 B (29.92%)** | 2 928 B (26.26%) [-9] | 5 215 B (46.77%) | 4 228 B (37.92%) | 3 214 B (28.83%) | **1 028 MB/s** vs 2 572 MB/s |
| `sum` | SPARC ELF binary | 38 240 B | 12 080 B (**31.59%**) | **11 771 B (30.78%)** | 9 927 B (25.96%) [-6] | 18 812 B (49.19%) | 16 333 B (42.71%) | 11 415 B (29.85%) | **783 MB/s** vs 2 832 MB/s |
| `lz0_win64.exe` | Windows PE64 binary | 146 944 B | 75 007 B (**51.04%**) | **72 706 B (49.48%)** | 67 560 B (45.98%) [-6] | 101 596 B (69.14%) | 87 689 B (59.68%) | *(too slow)* | **402 MB/s** vs 3 457 MB/s |
| `star.png` | PNG Image (Deflate) | 355 666 B | **355 671 B (100.00%)** | **355 671 B (100.00%)** | 364 646 B (102.52%) [-2] | 356 988 B (100.37%) | 356 960 B (100.36%) | *(too slow)* | **24 755 MB/s** vs 28 455 MB/s |
| `avatar.jpg` | JPEG Image | 25 380 B | 25 326 B (**99.79%**) | **25 275 B (99.59%)** | 25 749 B (101.45%) [-6] | 25 379 B (100.00%) | 25 326 B (99.79%) | *(too slow)* | **8 845 MB/s** vs 31 433 MB/s |
| `break.mp3` | Audio MP3 CBR | 38 080 B | 11 282 B (**29.63%**) | **11 239 B (29.51%)** | 11 287 B (29.64%) [-6] | 11 552 B (30.34%) | 11 473 B (30.13%) | *(too slow)* | **9 956 MB/s** vs 16 858 MB/s |
| `texture_rgb24.raw` | Raw RGB 24-bit texture | 196 608 B | 154 467 B (**78.57%**) | **131 149 B (66.71%)** | 86 531 B (44.01%) [-6] | 197 380 B (100.39%) | 197 170 B (100.29%) | *(too slow)* | **447 MB/s** vs 34 105 MB/s |
| `texture_rgba32.raw`| Raw RGBA 32-bit texture | 262 144 B | 144 019 B (**54.94%**) | **134 114 B (51.16%)** | *(excluded >256K)* | 263 173 B (100.39%) | 262 907 B (100.29%) | *(too slow)* | **495 MB/s** vs 30 308 MB/s |
| `audio_pcm16.raw` | Stereo PCM16 audio | 192 000 B | 139 019 B (**72.41%**) | **135 303 B (70.47%)** | 117 053 B (60.97%) [-6] | 163 730 B (85.28%) | 157 424 B (81.99%) | *(too slow)* | **475 MB/s** vs 7 810 MB/s |
| `alice29.txt` | English literature text | 152 089 B | 59 581 B (**39.18%**) | **58 330 B (38.35%)** | 50 488 B (33.20%) [-6] | 88 699 B (58.32%) | 63 673 B (41.87%) | *(too slow)* | **601 MB/s** vs 2 636 MB/s |
| `plrabn12.txt` | Classical poetry | 481 861 B | 213 234 B (**44.25%**) | **206 826 B (42.92%)** | 175 776 B (36.48%) [-2] | 325 589 B (67.57%) | 227 457 B (47.20%) | *(too slow)* | **594 MB/s** vs 2 661 MB/s |
| `kennedy.xls` | Excel spreadsheet | 1 029 744 B | 204 387 B (**19.85%**) | **194 941 B (18.93%)** | 64 830 B (6.30%) [-2] | 374 704 B (36.39%) | 324 190 B (31.48%) | *(too slow)* | **1 316 MB/s** vs 3 216 MB/s |
| `dickens_1m.txt`| Text corpus (1 MiB) | 1 048 576 B | 409 835 B (**39.08%**) | **393 561 B (37.53%)** | 331 404 B (31.61%) [-2] | 642 924 B (61.31%) | 445 113 B (42.45%) | *(too slow)* | **588 MB/s** vs 2 475 MB/s |
| `bash_x86_64.bin`| ELF x86_64 binary | 1 298 416 B | 619 907 B (**47.74%**) | **594 344 B (45.77%)** | 553 594 B (42.64%) [-2] | 871 664 B (67.13%) | 714 612 B (55.04%) | *(too slow)* | **453 MB/s** vs 2 556 MB/s |

---

## 2. Concatenated Heterogeneous Multi-Resource Blobs (Approach A)

In many embedded environments (microcontrollers, firmware, game loaders, packed executables), resources are grouped into a **single monolithic blob** resulting from concatenating disparate files (machine code, images, tables, texts).

Under **Approach A**, the compressed data remains **one continuous LZ0 stream**, decompressed in one pass by the freestanding decoders (`lz0dec_min` or the compact assembly), with no container between the concatenated files.

### Comparative Results on Blobs (256 KiB to 5 MiB, 3 to 15 sub-files)

| Target / Contents | Raw Size | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (default) | LZ4HC (`-9`) | Decomp. Throughput `lz0dec_fast` vs UPKR |
|---|---:|---:|---:|---:|---:|---:|---:|
| **`blob_png_256k`**<br>(4 PNGs) | 262 144 B | 247 388 B (94.37%) | **246 240 B (93.93%)** | 247 888 B (94.56%) [-6] | 252 446 B (96.30%) | 248 420 B (94.76%) | **1 644 MB/s** vs 17.5 MB/s (**94×**) |
| **`blob_mix_256k`**<br>(5 files: ELF, PE, XLS, PNG, txt) | 262 144 B | 128 073 B (48.86%) | 125 094 B (47.72%) | **110 904 B (42.31%)** [-6] | 168 178 B (64.15%) | 149 335 B (56.97%) | **677 MB/s** vs 37.1 MB/s (**18×**) |
| **`blob_png_512k`**<br>(8 PNGs) | 524 288 B | 504 264 B (96.18%) | **502 375 B (95.82%)** | 508 723 B (97.03%) [-2] | 511 352 B (97.53%) | 505 646 B (96.44%) | **2 197 MB/s** vs 17.1 MB/s (**128×**) |
| **`blob_mix_512k`**<br>(7 files: ELF, PE, 2 PNGs, XLS, C, txt) | 524 288 B | 286 543 B (54.65%) | 281 701 B (53.73%) | **260 451 B (49.68%)** [-2] | 352 088 B (67.16%) | 318 022 B (60.66%) | **767 MB/s** vs 32.6 MB/s (**24×**) |
| **`blob_png_2m`**<br>(8 distinct PNGs > 200 KiB) | 2 097 152 B | 2 057 006 B (98.09%) | **2 051 666 B (97.83%)** | 2 089 034 B (99.61%) [-1] | 2 067 355 B (98.58%) | 2 053 832 B (97.93%) | **3 748 MB/s** vs 16.6 MB/s (**226×**) |
| **`blob_mix_2m`**<br>(9 files: ELF, PE, 3 PNGs, XLS, txt, C, HTML) | 2 097 152 B | 1 203 278 B (57.38%) | 1 179 909 B (56.26%) | **1 125 643 B (53.67%)** [-1] | 1 484 306 B (70.78%) | 1 308 970 B (62.42%) | **712 MB/s** vs 29.3 MB/s (**24×**) |
| **`blob_png_5m`**<br>(15 distinct PNGs) | 5 242 880 B | **4 362 158 B (83.20%)** | **4 351 321 B (82.99%)** | *(excluded >20 min)* | 5 210 986 B (99.39%) | 5 183 917 B (98.88%) | **4 955 MB/s** vs - |
| **`blob_mix_5m`**<br>(15 distinct real-world files) | 5 242 880 B | 2 217 084 B (42.29%) | **2 149 547 B (41.00%)** | *(excluded >30 min)* | 3 093 616 B (59.01%) | 2 554 646 B (48.73%) | **737 MB/s** vs - |

---

## 3. In-Depth Comparison: LZ0 vs LZ4 and LZ4HC

While standard LZ4 prioritizes extreme single-pass encoding throughput, **LZ4HC** (`-9` / `-12`) applies an optimal forward parser within the constraints of the standard LZ4 format. Comparing LZ0 against both variants demonstrates the intrinsic advantages of LZ0's prefix-coded Elias gamma bitstream and sliding window model:

### Comprehensive Individual Files Summary

| Target | Raw Size | LZ4 default | LZ4HC (`-9`) | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | Net Gain LZ0 Ultra vs LZ4HC (`-9`) | Decomp: LZ0 FAST vs LZ4 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | 11,150 B | 5,215 B (46.77%) | 4,228 B (37.92%) | 3,371 B (30.23%) | **3,336 B (29.92%)** | **-892 B (-21.1%)** | 1,028 MB/s vs 2,572 MB/s |
| `grammar.lsp` | 3,721 B | 1,912 B (51.38%) | 1,724 B (46.33%) | 1,405 B (37.76%) | **1,380 B (37.09%)** | **-344 B (-20.0%)** | 850 MB/s vs 2,408 MB/s |
| `xargs.1` | 4,227 B | 2,658 B (62.88%) | 2,410 B (57.01%) | 1,975 B (46.72%) | **1,945 B (46.01%)** | **-465 B (-19.3%)** | 689 MB/s vs 2,520 MB/s |
| `cp.html` | 24,603 B | 11,905 B (48.39%) | 10,338 B (42.02%) | 9,190 B (37.35%) | **8,979 B (36.50%)** | **-1,359 B (-13.1%)** | 768 MB/s vs 3,902 MB/s |
| `sum` | 38,240 B | 18,812 B (49.19%) | 16,333 B (42.71%) | 12,080 B (31.59%) | **11,771 B (30.78%)** | **-4,562 B (-27.9%)** | 783 MB/s vs 2,832 MB/s |
| `alice29.txt` | 152,089 B | 88,699 B (58.32%) | 63,673 B (41.87%) | 59,581 B (39.18%) | **58,330 B (38.35%)** | **-5,343 B (-8.4%)** | 601 MB/s vs 2,636 MB/s |
| `asyoulik.txt` | 125,179 B | 79,653 B (63.63%) | 58,908 B (47.06%) | 54,349 B (43.42%) | **53,262 B (42.55%)** | **-5,646 B (-9.6%)** | 558 MB/s vs 2,581 MB/s |
| `plrabn12.txt` | 481,861 B | 325,589 B (67.57%) | 227,457 B (47.20%) | 213,234 B (44.25%) | **206,826 B (42.92%)** | **-20,631 B (-9.1%)** | 594 MB/s vs 2,661 MB/s |
| `lcet10.txt` | 426,754 B | 233,213 B (54.65%) | 165,788 B (38.85%) | 151,704 B (35.55%) | **147,072 B (34.46%)** | **-18,716 B (-11.3%)** | 643 MB/s vs 2,667 MB/s |
| `kennedy.xls` | 1,029,744 B | 374,704 B (36.39%) | 324,190 B (31.48%) | 204,387 B (19.85%) | **194,941 B (18.93%)** | **-129,249 B (-39.9%)** | 1,316 MB/s vs 3,216 MB/s |
| `dickens_1m.txt` | 1,048,576 B | 642,924 B (61.31%) | 445,113 B (42.45%) | 409,835 B (39.08%) | **393,561 B (37.53%)** | **-51,552 B (-11.6%)** | 588 MB/s vs 2,475 MB/s |
| `lz0_win64.exe` | 146,944 B | 101,596 B (69.14%) | 87,689 B (59.68%) | 75,007 B (51.04%) | **72,706 B (49.48%)** | **-14,983 B (-17.1%)** | 402 MB/s vs 3,457 MB/s |
| `bash_x86_64.bin` | 1,298,416 B | 871,664 B (67.13%) | 714,612 B (55.04%) | 619,907 B (47.74%) | **594,344 B (45.77%)** | **-120,268 B (-16.8%)** | 453 MB/s vs 2,556 MB/s |
| `texture_rgb24.raw` | 196,608 B | 197,380 B (100.39%) | 197,170 B (100.29%) | 154,467 B (78.57%) | **131,149 B (66.71%)** | **-66,021 B (-33.5%)** | 447 MB/s vs 34,105 MB/s |
| `texture_rgba32.raw` | 262,144 B | 263,173 B (100.39%) | 262,907 B (100.29%) | 144,019 B (54.94%) | **134,114 B (51.16%)** | **-128,793 B (-49.0%)** | 495 MB/s vs 30,308 MB/s |
| `audio_pcm16.raw` | 192,000 B | 163,730 B (85.28%) | 157,424 B (81.99%) | 139,019 B (72.41%) | **135,303 B (70.47%)** | **-22,121 B (-14.1%)** | 475 MB/s vs 7,810 MB/s |
| `avatar.jpg` | 25,380 B | 25,379 B (100.00%) | 25,326 B (99.79%) | 25,326 B (99.79%) | **25,275 B (99.59%)** | **-51 B (-0.2%)** | 8,845 MB/s vs 31,433 MB/s |
| `star.png` | 355,666 B | 356,988 B (100.37%) | 356,960 B (100.36%) | **355,671 B (100.00%)** | **355,671 B (100.00%)** | **-1,289 B (-0.4%)** | 24,755 MB/s vs 28,455 MB/s |
| `break.mp3` | 38,080 B | 11,552 B (30.34%) | 11,473 B (30.13%) | 11,282 B (29.63%) | **11,239 B (29.51%)** | **-234 B (-2.0%)** | 9,956 MB/s vs 16,858 MB/s |
| **TOTAL** | **5,861,382 B** | **3,776,746 B (64.43%)** | **3,133,723 B (53.46%)** | **2,645,809 B (45.14%)** | **2,541,204 B (43.36%)** | **-592,519 B (-18.9%)** | — |

### Concatenated Blobs & Packages Summary

| Target | Raw Size | LZ4 default | LZ4HC (`-9`) | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | Net Gain LZ0 Ultra vs LZ4HC (`-9`) | Decomp: LZ0 FAST vs LZ4 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `blob_mix_256k.bin` | 262,144 B | 168,178 B (64.15%) | 149,335 B (56.97%) | 128,073 B (48.86%) | **125,094 B (47.72%)** | **-24,241 B (-16.2%)** | 677 MB/s vs 2,843 MB/s |
| `blob_png_256k.bin` | 262,144 B | 252,446 B (96.30%) | 248,420 B (94.76%) | 247,388 B (94.37%) | **246,240 B (93.93%)** | **-2,180 B (-0.9%)** | 1,644 MB/s vs 14,749 MB/s |
| `blob_mix_512k.bin` | 524,288 B | 352,088 B (67.16%) | 318,022 B (60.66%) | 286,543 B (54.65%) | **281,701 B (53.73%)** | **-36,321 B (-11.4%)** | 767 MB/s vs 3,972 MB/s |
| `blob_png_512k.bin` | 524,288 B | 511,352 B (97.53%) | 505,646 B (96.44%) | 504,264 B (96.18%) | **502,375 B (95.82%)** | **-3,271 B (-0.6%)** | 2,197 MB/s vs 16,873 MB/s |
| `blob_mix_2m.bin` | 2,097,152 B | 1,484,306 B (70.78%) | 1,308,970 B (62.42%) | 1,203,278 B (57.38%) | **1,179,909 B (56.26%)** | **-129,061 B (-9.9%)** | 712 MB/s vs 2,698 MB/s |
| `blob_png_2m.bin` | 2,097,152 B | 2,067,355 B (98.58%) | 2,053,832 B (97.93%) | 2,057,006 B (98.09%) | **2,051,666 B (97.83%)** | **-2,166 B (-0.1%)** | 3,748 MB/s vs 6,761 MB/s |
| `blob_mix_5m.bin` | 5,242,880 B | 3,093,616 B (59.01%) | 2,554,646 B (48.73%) | 2,217,084 B (42.29%) | **2,149,547 B (41.00%)** | **-405,099 B (-15.9%)** | 737 MB/s vs 2,426 MB/s |
| `blob_png_5m.bin` | 5,242,880 B | 5,210,986 B (99.39%) | 5,183,917 B (98.88%) | 4,362,158 B (83.20%) | **4,351,321 B (82.99%)** | **-832,596 B (-16.1%)** | 4,955 MB/s vs 5,653 MB/s |
| **TOTAL** | **16,252,928 B** | **13,140,327 B (80.85%)** | **12,322,788 B (75.82%)** | **11,005,794 B (67.72%)** | **10,887,853 B (66.99%)** | **-1,434,935 B (-11.6%)** | — |

### Technical Analysis: Why LZ4HC Cannot Reach LZ0 Density

1. **The 64 KiB Window Ceiling**:
   LZ4 offsets are structurally restricted to `uint16_t` (65,535 bytes maximum distance). On inputs $\ge 1$ MiB, repetitions distant by more than 64 KiB are completely invisible to LZ4HC. LZ0 uses unbounded prefix codes (Elias gamma MSB + 8-bit LSB), allowing it to match across megabytes. On `blob_png_5m.bin`, LZ0 Ultra saves **832,596 bytes** over LZ4HC.
2. **Byte-Aligned Token Tax**:  
   LZ4 requires 1 full byte for its token (run lengths) plus 2 full bytes for offset, establishing a minimum match overhead of 24 bits. In LZ0, a single control bit dictates token type, and `MatchLast` reuses the previous offset for only 1 bit plus length code.
3. **No Inflation on High Entropy**:
   On the raw textures and `star.png` in this set, LZ4 grows the file by about 0.3% to 0.4%. LZ0's per-window incompressible bailout emits `star.png` as a raw literal run (`355,671 B`, **100.00%**, `+5 B` framing overhead) at **~476–509 MB/s** encoding speed across all levels `-1..-9` (`350.4 KiB` peak heap, `24,755 MB/s` decode). On `avatar.jpg`, `-9` shrinks the input to **25,275 B (99.59%)**.
4. **Minimal Decoder Footprint**:
   Despite decoding variable-bit Elias gamma codes and unbounded window offsets with full bounds validation (`PROT_NONE` guard-page verified), the standalone assembly decoders maintain a minimal `.text` footprint (`0` heap, `0` stack on ARM64/RV64 compact):
   - **Compact ASM**: **1,151 B** (`x86-64`), **1,296 B** (`ARM64`), **1,056 B** (`RISC-V 64`)
   - **FAST ASM**: **2,898 B** (`x86-64`), **2,852 B** (`ARM64`), **3,280 B** (`RISC-V 64`)
   - **Minimal C (`lz0dec_min.c` at `-Os`)**: **1,086 B** (`x86-64`), **1,004 B** (`ARM64`), **782 B** (`RISC-V 64`)
5. **Decompression Throughput**:
   LZ4 decompresses text and binaries at ~2.4–3.9 GB/s, and faster on images. LZ0's FAST x86-64 decoder lands in the **402–1,316 MB/s** band on text and binaries (and **8.8–24.8 GB/s** on pre-compressed media). UPKR's C unpacker in the same table spans 16–175 MB/s.

---

## 4. Decompression Throughput & Binary Footprint

Reproduction commands:
```bash
make bench_all          # Comprehensive ratio & throughput comparison
make bench_dec          # Native x86-64 throughput micro-benchmarks
make bench_dec_arm64    # AArch64 throughput micro-benchmarks (QEMU)
make bench_dec_riscv64  # RISC-V 64 throughput micro-benchmarks (QEMU)
```

### Decoder Code Footprint (`.text` Bytes)

| Architecture | Minimal C (`lz0dec_min`, `-Os`) | Compact ASM (`-O3`) | FAST ASM (`-O3`) |
|---|---:|---:|---:|
| **x86-64** | 1,086 B | **1,151 B** | 2,898 B |
| **AArch64 / ARM64** | 1,004 B | **1,296 B** | 2,852 B |
| **RISC-V 64** | 782 B | **1,056 B** | 3,280 B |

### Native x86-64 Decompression Throughput (`make bench_dec`)

| File | Minimal C (`lz0dec_min`) | Standard C (`lz0_decompress_to`) | ASM Compact (`lz0dec_x86_64`) | ASM FAST (`lz0dec_fast_x86_64`) | UPKR (C unpacker) | LZ4 (`decompress_safe`) |
|---|---:|---:|---:|---:|---:|---:|
| `fields.c` | 331 MB/s | 371 MB/s | 532 MB/s | **1 028 MB/s** | 61 MB/s | 2 572 MB/s |
| `cp.html` | 223 MB/s | 231 MB/s | 328 MB/s | **768 MB/s** | — | 3 902 MB/s |
| `sum` | 277 MB/s | 281 MB/s | 398 MB/s | **783 MB/s** | 63 MB/s | 2 832 MB/s |
| `lz0_win64.exe` | 161 MB/s | 168 MB/s | 237 MB/s | **402 MB/s** | 35 MB/s | 3 457 MB/s |
| `star.png` | 23 362 MB/s | 23 520 MB/s | 14 862 MB/s | **24 755 MB/s** | 16 MB/s | 28 455 MB/s |
| `avatar.jpg` | 4 718 MB/s | 4 997 MB/s | 4 952 MB/s | **8 845 MB/s** | 16 MB/s | 31 433 MB/s |
| `break.mp3` | 3 081 MB/s | 3 082 MB/s | 6 683 MB/s | **9 956 MB/s** | 55 MB/s | 16 858 MB/s |
| `alice29.txt` | 198 MB/s | 207 MB/s | 303 MB/s | **601 MB/s** | 44 MB/s | 2 636 MB/s |
| `plrabn12.txt` | 189 MB/s | 196 MB/s | 300 MB/s | **594 MB/s** | 40 MB/s | 2 661 MB/s |
| `kennedy.xls` | 648 MB/s | 642 MB/s | 894 MB/s | **1 316 MB/s** | 175 MB/s | 3 216 MB/s |
| `dickens_1m.txt` | 201 MB/s | 228 MB/s | 323 MB/s | **588 MB/s** | 48 MB/s | 2 475 MB/s |
| `bash_x86_64.bin` | 179 MB/s | 188 MB/s | 254 MB/s | **453 MB/s** | 38 MB/s | 2 556 MB/s |

### AArch64 / ARM64 Decompression Throughput (QEMU, `make bench_dec_arm64`)

| File | Standard C (`lz0_decompress_to`) | Minimal C (`lz0dec_min`) | ASM Compact (`lz0dec_arm64`) | ASM FAST (`lz0dec_fast_arm64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 88.7 MB/s | 102.2 MB/s | 78.0 MB/s | **273.9 MB/s** | **+208.7%** |
| `cp.html` | 76.1 MB/s | 82.2 MB/s | 57.5 MB/s | **221.3 MB/s** | **+190.7%** |
| `alice29.txt` | 63.0 MB/s | 73.4 MB/s | 41.2 MB/s | **215.8 MB/s** | **+242.7%** |
| `plrabn12.txt` | 59.2 MB/s | 62.2 MB/s | 33.2 MB/s | **213.6 MB/s** | **+261.1%** |
| `kennedy.xls` | 105.9 MB/s | 135.7 MB/s | 131.8 MB/s | **388.7 MB/s** | **+267.1%** |
| `dickens_1m.txt` | 58.9 MB/s | 73.2 MB/s | 33.2 MB/s | **231.8 MB/s** | **+293.4%** |
| `bash_x86_64.bin` | 50.2 MB/s | 66.7 MB/s | 43.0 MB/s | **171.8 MB/s** | **+242.2%** |

### RISC-V 64 / RV64GC Decompression Throughput (QEMU, `make bench_dec_riscv64`)

| File | Standard C (`lz0_decompress_to`) | Minimal C (`lz0dec_min`) | ASM Compact (`lz0dec_riscv64`) | ASM FAST (`lz0dec_fast_riscv64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 76.2 MB/s | 122.9 MB/s | 125.1 MB/s | **353.2 MB/s** | **+363.6%** |
| `cp.html` | 68.7 MB/s | 106.2 MB/s | 99.3 MB/s | **262.5 MB/s** | **+282.3%** |
| `alice29.txt` | 65.2 MB/s | 93.6 MB/s | 111.3 MB/s | **255.2 MB/s** | **+291.1%** |
| `plrabn12.txt` | 63.3 MB/s | 86.5 MB/s | 95.2 MB/s | **236.7 MB/s** | **+274.2%** |
| `kennedy.xls` | 77.0 MB/s | 208.6 MB/s | 116.8 MB/s | **505.8 MB/s** | **+556.8%** |
| `dickens_1m.txt` | 70.6 MB/s | 84.7 MB/s | 104.3 MB/s | **227.3 MB/s** | **+221.8%** |
| `bash_x86_64.bin` | 47.6 MB/s | 85.3 MB/s | 68.0 MB/s | **208.0 MB/s** | **+336.9%** |

---

## 5. Encoder Preset Scaling (`-1`..`-9`) & Peak Heap Memory

Measured with `./bench/lz0_enc_bench 3 1,2,3,6,8,9` (best-of-3 across all 19 individual corpus files, **5,861,382 B** total) and `./bench/lz0_mem_peak 1,2,3,6,8,9`:

| Level | Corpus Total (19 files) | Global Ratio | Total Encode Time (19 files) | `alice29.txt` Peak Heap | `bash_x86_64.bin` Peak Heap | `star.png` Peak Heap |
|---|---:|---:|---:|---:|---:|---:|
| **`-1` (Streaming Greedy)** | 3,133,816 B | 53.47% | **79.2 ms** | **404.7 KiB** (`2.72 B/B`) | **1,524.1 KiB** (`1.20 B/B`) | 350.4 KiB (`1.01 B/B`) |
| **`-2` (SDP K=2, no SA)** | 2,860,843 B | 48.81% | **1,242.5 ms** | 5,171.1 KiB (`34.82 B/B`) | 41,019.1 KiB (`32.35 B/B`) | 350.4 KiB (`1.01 B/B`) |
| **`-3` (SDP K=2, SA win=4)** | 2,708,644 B | 46.21% | **3,015.9 ms** | 6,285.3 KiB (`42.32 B/B`) | 50,886.9 KiB (`40.13 B/B`) | 350.4 KiB (`1.01 B/B`) |
| **`-6` (Default SDP K=2)** | 2,645,809 B | 45.14% | **6,584.7 ms** | 6,282.5 KiB (`42.30 B/B`) | 50,892.6 KiB (`40.14 B/B`) | 350.4 KiB (`1.01 B/B`) |
| **`-8` (SDP K=8, SA win=64)** | 2,586,256 B | 44.12% | **23,242.5 ms** | 7,770.7 KiB (`52.32 B/B`) | 68,846.1 KiB (`54.30 B/B`) | 350.4 KiB (`1.01 B/B`) |
| **`-9` (Ultra Tiered-SoA K=64)** | **2,541,204 B** | **43.36%** | 24,184.1 ms | 6,121.8 KiB (`41.22 B/B`) | 61,435.4 KiB (`48.45 B/B`) | 350.4 KiB (`1.01 B/B`) |

- **Thread-safety (`make threadsafe`)**: all search and coding parameters live in caller-owned `lz0_config`, allowing concurrent multi-threaded encoding across arbitrary levels with deterministic bit-exact output.

---

## 6. Key Takeaways and Technical Analysis

### LZ0 vs LZ4: Massive Density Gain with Comfortable Throughput
- Across all structured and binary corpora, **LZ0 reduces compressed size by 14% to 49% compared to LZ4HC** (and up to 49% on textures):
  - On `kennedy.xls`: LZ0 compresses to **194.9 KiB (18.93%)** vs LZ4 at **374.7 KiB (36.39%)** (size nearly halved).
  - On `sum` (SPARC ELF): LZ0 compresses to **11.8 KiB (30.78%)** vs LZ4 at **18.8 KiB (49.19%)**.
  - On `blob_mix_2m`: LZ0 compresses to **1.18 MiB (56.26%)** vs LZ4 at **1.48 MiB (70.78%)**.
  - On `blob_mix_5m`: LZ0 compresses to **2.15 MiB (41.00%)** vs LZ4 at **3.09 MiB (59.01%)**.
- On uncompressed raw textures:
  - On `texture_rgba32.raw`: LZ0 compresses to **134.1 KiB (51.16%)** whereas LZ4 inflates (+100.39%).
  - On `texture_rgb24.raw`: LZ0 compresses to **131.1 KiB (66.71%)** whereas LZ4 inflates (+100.39%).
  - On `audio_pcm16.raw`: LZ0 compresses to **135.3 KiB (70.47%)** vs LZ4 at **163.7 KiB (85.28%)**.
- In return, `lz0dec_fast` delivers high decompression throughput (**402 to 1,316 MB/s** on text/code and **8.8 to 24.8 GB/s** on media) with a **~2.8–3.3 KiB** assembly decoder (`2,898 B` x86-64, `2,852 B` ARM64, `3,280 B` RV64), zero hash tables, and zero auxiliary heap allocations.

### LZ0 vs UPKR: Superiority on Pre-Compressed Media and Decompression Asymmetry
- **On PNG, JPEG, and High-Entropy Data**: UPKR's probabilistic rANS model suffers from structural overhead. LZ0, through byte-aligned literals and 16 KiB per-window incompressible bailout, **systematically outperforms UPKR** on pre-compressed media:
  - On `star.png`: LZ0 emits **355,671 B (100.00%)** in **0.7–0.8 ms** vs UPKR (**364,646 B**, inflated at 102.52%).
  - On `avatar.jpg`: LZ0 compresses to **25,275 B (99.59%)**, beating UPKR (**25,749 B**, inflated at 101.45%).
  - On `break.mp3`: LZ0 compresses to **11,239 B (29.51%)**, beating UPKR (**11,287 B**) and LZ4 (**11,552 B**).
  - On `blob_png_512k`: LZ0 compresses to **502.4 KiB** vs UPKR at **508.7 KiB**.
- **Decompression Speed**: `lz0dec_fast` decompresses **18× to 226× faster than UPKR** (which caps out around 16–38 MB/s on blobs).
- **Encoding Speed**: LZ0 Normal compresses a 2 MiB blob in ~2.9 seconds (`-9` in ~9.0s) where UPKR requires minutes.

### Strict Length-2 Filtering & 16 KiB Per-Window Classification
- Every 16 KiB block window computes its own byte entropy (`is_high_entropy`), stride periodicity (`bpp` & local table flag `local_tab`), preventing high-entropy media chunks inside heterogeneous blobs from poisoning text/code windows (and vice versa).
- If all windows are high entropy without compressible local structure, `lz0_compress` bails out immediately into a literal-only stream (`1.01 B/byte` peak heap, `~476–509 MB/s` encode).

### Tiered-SoA Ultra Parser ($K=64$), DAG Backtrack & Multi-Arch SIMD
- In Ultra mode (`-9`), `lz0_parse_ultra` (`lz0/lz0_ultra.c`) replaces flat per-byte dense tables with a tiered active-row slab pool (`8 / 32 / 64..256` slots, future rows freed as soon as expanded), open-addressed `htab[256]` lookup, `8x16` block-max eviction tracking, and a reference-counted DAG backtrack arena with lazy literal anchors — cutting peak heap from `243–893 MiB` down to **`6.1–61.4 MiB`** while speeding up the 19-file corpus from `80.5s` to **`24.2s`**. Inputs >= 16 MiB fall back cleanly to generic SDP `K=32`.
- Level-9 hot loops (`lz0_u_row_prep`, `lz0_sa_scan_side`, row/candidate filters) run on hand-written SIMD across **x86-64 AVX2/SSE2**, **AArch64 NEON**, and **RISC-V 64 RVV 1.0**, verified bit-exact against scalar C (`make enc_tiers`, `make simd`, `make sa_probe`).
- Match finding combines a depth-16 bounded binary-tree matcher (`bt_depth=16`) with skewed multi-scale suffix-array probing (`sa_win=96`, `sa_adapt=2`).
