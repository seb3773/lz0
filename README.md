# LZ0

**A lightweight, asymmetric LZ compressor designed around an extremely simple, memory-free decoder.**

LZ0 is an experimental lossless compression format and C implementation designed around a single guiding principle:

> **Move complexity from the decoder to the encoder.**

Instead of balancing computational cost equally between compression and decompression, LZ0 deliberately spends more effort during encoding to generate a compact, structured stream that can be decompressed by an ultra-minimal routine. The decoder requires **no dynamic memory allocation (`malloc`)**, maintains minimal state, and processes explicit LZ tokens at hundreds of megabytes per second.

LZ0 is primarily targeted at small-to-medium binary blobs and structured datasets, typically ranging from a few hundred kilobytes to several megabytes.

---

## 1. Design Philosophy

Compression algorithms face an inherent multi-dimensional trade-off between compression ratio, encoding speed, decoding speed, and implementation complexity.

LZ0 resolves this trade-off strictly from the decoder's perspective:

- **Minimal decoder footprint:** The decompression logic is tiny, auditable, and easily embedded into resource-constrained environments (bootloaders, microcontrollers, retro computing, kernel space).
- **Zero dynamic memory:** The decoder uses only a handful of stack registers and an optional bitreader struct; it operates entirely within the caller-provided buffers.
- **Fast decompression:** By avoiding complex entropy decoding (such as arithmetic or adaptive range coding), decompression minimizes branch mispredictions and per-token overhead.
- **High compression ratio:** Targets ratios comparable to ZX0 on binary and text payloads.
- **Practical encoding speed:** Avoids the exhaustive $O(N^2)$ search strategies that make optimal parsing impractically slow on megabyte-sized files.
- **Format independence:** The bitstream describes only pure reconstruction instructions; the encoder is free to evolve (better search, smarter pruning, new heuristics) without breaking or complicating the decoder.

---

## 2. Where LZ0 Fits

LZ0 occupies a distinct design niche between simple retro LZ formats (like ZX0) and modern adaptive entropy-coded LZ compressors (like UPKR).

### LZ0 vs. ZX0
ZX0 established the gold standard for minimal-decoder LZ compression, pairing a byte-by-byte overlapping copy decoder with an optimal-rate dynamic programming parser. However, ZX0's exhaustive search can take several minutes on inputs larger than 1 MiB.

LZ0 shares ZX0's philosophy of decoder simplicity and Elias-based variable integer coding, but adopts a fundamentally faster encoder architecture (combining a hash chain with an $O(N)$ SA-IS suffix array and a sparse dynamic programming parser) and its own bitstream grammar. This produces comparable ratios in seconds instead of minutes.

### LZ0 vs. UPKR
UPKR achieves high compression ratios by coupling LZ matching with adaptive entropy coding (rANS / rABS). This requires the decoder to maintain probability distributions and perform entropy-state updates on every token.

LZ0 deliberately eschews adaptive entropy coding. Its decoder processes raw bits and Elias gamma integers, trading a fraction of compression ratio for dramatic simplicity and higher decoding throughput.

### Architectural Comparison

| Characteristic | ZX0 | LZ0 (H2C8) | UPKR |
|---|---|---|---|
| **Compression Model** | LZ77 / LZSS | LZ-style explicit tokens | LZ + adaptive entropy coding |
| **Entropy Coding** | Elias gamma | Elias gamma (hybrid) | Adaptive rANS / rABS |
| **Encoder Complexity** | Moderate / Exhaustive | High (SA-IS + Sparse DP) | High |
| **Encoding Speed** | Very slow ($> 1$ MiB) | Fast (seconds on 5 MiB) | Fast |
| **Decoder Complexity** | Ultra-simple | Ultra-simple (~150 lines C) | Complex |
| **Decoder RAM** | None (0 bytes `malloc`) | None (0 bytes `malloc`) | Requires state/tables |
| **Bitstream Compatibility** | ZX0 | Independent format | Independent format |

---

## 3. Compression Model & Token Stream

The LZ0 decoder interprets a sequence of four semantic operations:

| Token | Parameters | Description |
|---|---|---|
| `LiteralRun` | Length $L \ge 1$, raw bytes | Copies $L$ uncompressed bytes directly from the stream. |
| `MatchLast` | Length $L \ge 1$ | Copies $L$ bytes starting at `out_pos - last_offset`. |
| `MatchNew` | Offset $\ge 1$, Length $L \ge 2$ | Updates `last_offset = offset`, then copies $L$ bytes from `out_pos - offset`. |
| `EOF` | None | Terminates stream decoding cleanly. |

### Decoder State
The decoder maintains minimal state:
1. `out_pos`: Current write position in the output buffer.
2. `last_offset`: Previously used match offset (initialized to `1`).
3. Bitreader state: Current byte pointer, mask, and bit buffer.

### Overlapping Copy Semantics
Matches are copied byte-by-byte (`out[out_pos + i] = out[src + i]`). Because `src = out_pos - offset`, when `length > offset` the copy automatically repeats earlier bytes. For example, an offset of `1` with a length of `8` expands a single byte into an 8-byte run. Standard non-overlapping `memcpy` must **not** be used.

---

## 4. Format & Bitstream Specification

LZ0 differentiates between the **raw compressed bitstream** and the **on-disk container format**.

### 4.1 On-Disk Container (CLI Format)
Files stored on disk or handled by the `lz0` CLI wrap the raw payload in a minimal 8-byte header:

```
+-------------------+-----------------------------+-------------------------+
| Magic (4 bytes)   | Uncompressed Size (4 bytes) | Raw Payload (N bytes)   |
| 'L' 'Z' '0' '1'   | uint32 little-endian        | Raw LZ0 bitstream       |
+-------------------+-----------------------------+-------------------------+
```

- If `uncompressed_size == 0`, the payload is 0 bytes long.
- The raw decoder rejects empty inputs; empty files are handled strictly at the container level.

### 4.2 Raw Bitstream Layout
The raw bitstream has no magic bytes, header, or length metadata. It is normative according to [docs/LZ0_FORMAT.md](docs/LZ0_FORMAT.md).

- **Bit packing:** Packed **most-significant-bit first (MSB-first)** in each control byte (mask sequence: `0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01`).
- **Interleaved raw bytes:** Literal byte payloads and the 8-bit offset LSB byte are stored unaligned as raw bytes in stream order. When a raw byte is read, the bitreader consumes the next byte from the source; subsequent bits are drawn from a fresh control byte (no backtrack bit-stealing).

### 4.3 Grammar & Transitions
- **Implied First Decision Bit:** Every stream begins with a `LiteralRun`. The leading decision bit `0` is **omitted**. The decoder starts immediately by decoding the literal run length.
- **State transitions:**
  - After `LiteralRun`: Bit `0` $\to$ `MatchLast`. Bit `1` $\to$ enter `new_loop`.
  - After `MatchLast`: Bit `0` $\to$ next `LiteralRun`. Bit `1` $\to$ enter `new_loop`.
  - Inside `new_loop`:
    - Bit `0` $\to$ **`EOF`** (Stream ends).
    - Bit `1` $\to$ `MatchNew` (Reads offset and length).
  - After `MatchNew`: Bit `0` $\to$ next `LiteralRun`. Bit `1` $\to$ continue in `new_loop`.

### 4.4 Elias Gamma Coding
Used for variable-length integers ($v \ge 1$):
- Let $k = \lfloor \log_2(v) \rfloor$. The encoder emits $k$ pairs of `(0, bit)` followed by a stop bit `1`.
- **Standard (uninverted):** Used for literal length and `MatchLast` length.
- **Inverted:** Used for offset MSB. Payload bits are complemented (`bit ^ 1`). Prefix zeros and the stop bit remain unchanged.

### 4.5 Offset Encoding (LSB=8)
Valid offsets satisfy $1 \le \text{offset} \le \text{out\_pos}$.
Offsets are split into an MSB and an 8-bit remainder:
$$\text{msb} = \lfloor (\text{offset} - 1) / 256 \rfloor + 1 = ((\text{offset} - 1) \gg 8) + 1$$
$$\text{rem} = (\text{offset} - 1) \ \& \ 255$$

1. $\text{msb}$ is written as an **inverted Elias gamma** code.
2. The remainder is written as a raw byte: $255 - \text{rem}$.
3. The decoder reconstructs the offset via: $\text{offset} = \text{msb} \times 256 - \text{raw\_byte}$.

### 4.6 MatchNew Length Coding (LEN=3)
Matches with a new offset require $L \ge 2$. Let $v = L - 1$:
- **Short lengths ($2 \le L \le 8$, so $1 \le v \le 7$):** Encoded as 3 fixed bits representing $v - 1$ in MSB-first order (`000` for $L=2$ up to `110` for $L=8$).
- **Long lengths ($L \ge 9$, so $v \ge 8$):** Encoded with an escape prefix `111` followed by $\text{Elias}(v - 7)$ (uninverted).

---

## 5. Encoder Architecture (H2C8)

The production encoder configuration is **H2C8** (Hash 3 + Chain 8 + SA-IS + Sparse DP $K=2$). It processes data through three distinct stages:

```
Input Data
    │
    ├──► 1. Hash-Chain Finder (Hash3, max_chain=8, power-of-2 skip steps)
    │
    ├──► 2. Suffix Array Finder (SA-IS O(N) + rank lookup, adaptive probe ±4 → ±8 → ±16)
    │
    ├──► 3. Sparse DP Parser (K=2 live states, Elias plateau pruning)
    │
    └──► 4. Bitstream Emitter (MSB-first bitstream, LSB=8, LEN=3)
```

### 5.1 Match Finding
1. **Hash-Chain Finder:** Rapidly identifies nearby matches. Uses a 3-byte hash table of $2^{17}$ entries (128K buckets) and traverses a history chain limited to 8 candidates (up to 48 in Ultra) with power-of-2 skip stepping.
2. **SA-IS Suffix Array Finder:** Identifies distant and long matches across the entire buffer in linear $O(N)$ time.
   - Built using an optimized **SA-IS** algorithm (`lz0_sa_algo=1`).
   - Validated against a deterministic $O(N \log^2 N)$ prefix-doubling reference oracle (`lz0_sa_algo=0`), ensuring byte-identical suffix arrays.
   - **Adaptive & Multi-Scale Probing:** Default mode uses rank-locality search ($\pm 4 \to \pm 8 \to \pm 16$). Ultra mode activates multi-scale probing: dense linear search $1 \dots 128$ plus geometric jumps $\{160, 192, 256, 320, 384, 512\}$ to capture long-range matches on large binaries and dumps.
3. **Multi-Window Stride & Media Periodicity:** Automatically detects structural repetitions (table columns in CSV/XLS, machine instruction alignment 4/8 bytes, interleaved channels for RGB24 stride 3, RGBA32 stride 4, and stereo PCM16 stride 4) via multi-window autocorrelation and 4-byte MPEG Layer III header validation for MP3 CBR frame synchronization.

### 5.2 Sparse DP Parser & Profitability Filtering
Instead of a greedy heuristic or a memory-intensive full DP lattice, LZ0 maintains Pareto-optimal live states per input position ($K=2$ in default `-6`, and adaptive $K=32/16/8$ in Ultra `-9` according to file size).

- **Elias Plateau Stepping** (`next_len_plateau`): Because Elias gamma lengths remain constant across powers-of-two intervals ($[2^k, 2^{k+1}-1]$), evaluating intermediate lengths inside a plateau incurs the same bit-cost as the maximum length. The parser skips redundant transitions and only tests plateau upper bounds, dramatically pruning search branches.
- **Strict Length-2 Profitability Filter**: In high-entropy streams (PNG, JPEG, MP3, encrypted sections), matches of length 2 are accepted only if $offset \le 32$ or if matching the previous offset (`repeat-offset`). This strictly prevents bitstream inflation over raw literals.

---

## 6. Decoder Implementations

The repository provides independent, interoperable decoders tailored for different footprint and throughput requirements:

#### C Implementations
1. **`lz0/lz0_decompress.c` (`lz0_decompress_to`)**:  
   The primary library decoder. Features an optimized bitreader loop and direct copy operations.
2. **`lz0/lz0dec_ref.c` (`lz0dec_ref`)**:  
   The normative reference decoder. Co-authoritative with [docs/LZ0_FORMAT.md](docs/LZ0_FORMAT.md); concise, didactic and strictly conforming.
3. **`lz0/lz0dec_min.c` (`lz0dec_min`)**:  
   An ultra-compact, standalone single-function implementation (~40 lines of C99, ~200 bytes of machine code). Zero internal function calls, zero tables in memory, zero includes beyond basic types, and zero heap allocations. Ideal for bootloaders, ROMs, microcontrollers (AVR, STM32, Cortex-M0), and kernel drivers.

#### Compact Assembly Implementations (Footprint & RAM-efficient)
4. **`lz0/lz0dec_x86_64.s` (`lz0dec_x86_64` / `lz0dec_compact`)**:  
   Compact hand-crafted x86-64 assembly decoder (~1.3 KiB code size). Zero lookup tables, pure arithmetic bit-unpacking, tail-overlapping 64-bit stores, and zero heap allocations.
5. **`lz0/lz0dec_arm64.s` (`lz0dec_arm64` / `lz0dec_compact`)**:  
   Compact hand-crafted 64-bit ARM (AArch64) assembly decoder (~1.3 KiB code size). Pure leaf function, zero lookup tables, and zero heap allocations.

#### High-Throughput Assembly Implementations ("FAST" Engine)
6. **`lz0/lz0dec_fast_x86_64.s` (`lz0dec_fast_x86_64` / `lz0dec_fast`)**:  
   Ultra-fast x86-64 assembly decoder (~2.8 KiB code size + 264-byte constant table). Delivers top-tier throughput (up to 800–1200+ MB/s) with a dual FAST/SAFE engine architecture.
7. **`lz0/lz0dec_fast_arm64.s` (`lz0dec_fast_arm64` / `lz0dec_fast`)**:  
   Ultra-fast AArch64 assembly decoder (~2.7 KiB code size + 264-byte constant table). Pure leaf (0 stack bytes), NEON vector copies, and branch-free Elias gamma decoding.

### Architecture du Décodeur ASM "FAST" (x86-64 & ARM64)

Le décodeur haute performance introduit plusieurs innovations micro-architecturales avancées :

* **Architecture à deux moteurs (FAST vs SAFE)** :
  * **Moteur FAST** (actif tant qu'il reste au moins 16 octets en entrée et en sortie) : utilise des transferts vectoriels de 16 octets (SSE2 `movdqu` sur x86, NEON `ldr q`/`str q` sur ARM64).
  * **Moteur SAFE** (pour les 16 derniers octets et les très rares codes Elias géants) : lecteur de bits à sentinelle (`add r8b, r8b` sur x86, `adds w8, w8, w8` sur ARM plaçant le bit dans le carry et détectant l'épuisement par le zero flag), éliminant le registre compteur de bits et garantissant **zéro dépassement de mémoire**.

* **La "Fenêtre de Segment" (Résolution de la contrainte LZ0)** :
  * Dans LZ0, les octets bruts (littéraux et octet bas de match) sont entrelacés avec des groupes de bits de contrôle. Un refill classique de 64 bits avalerait des octets de littéraux.
  * **L'idée clé** : tous les bits de contrôle entre deux lectures d'octets bruts forment un segment continu. Le décodeur charge une fenêtre de 64 bits (\(W = \text{leftover} \mid (\text{bswap}(\text{load64}) \gg k)\)), décode l'intégralité du segment en registres par décalages, et ne commet le pointeur (`in_ptr += q >> 3`) qu'au moment de lire le prochain octet brut.
  * **Impact** : élimination des tests de refill par bit (qui provoquaient de nombreuses erreurs de prédiction de branchement dans le texte).

* **Décodage Elias Gamma sans branchement (*Branch-Free*)** :
  * Pour \(n \le 8\) (longueurs \(\le 511\) et distances \(\le 131\ 071\), soit >99 % des cas réels) :
    * Détection directe de la taille en 1 instruction : `bsr` (x86-64) ou `clz` (ARM64).
    * Extraction de la charge utile via deux lectures dans une table compacte de 256 octets (`.Lunzip`).
    * Zéro boucle, zéro pénalité de branchement (*branch misprediction*).

* **Élimination totale des appels de fonction (`call` / `ret`)** :
  * Sur x86-64, tous les sites d'appels ont été inlinés dans la boucle principale.
  * Sur ARM64, la fonction est une **feuille pure (*pure leaf*)** : 0 octet alloué sur la pile, utilisation exclusive des registres appelants banalisés (`x0..x17` et `v0`).

### Safety Contract
All decoders adhere to strict safety guarantees:
- Return `-1` on any corrupted token, out-of-bounds offset, truncated stream, or buffer overrun.
- Never write beyond `out_max` and never read past `in_size`.
- Overlap-safe byte-by-byte copying.

---

## 7. Production Configuration Summary

| Component | Setting (Default `-6`) | Setting (Ultra `-9`) | Description |
|---|---|---|---|
| **Bit Packing** | **MSB-first** | **MSB-first** | Bits 7 down to 0 in control bytes |
| **Offset Low Part** | 8 bits (`LSB=8`) | 8 bits (`LSB=8`) | 1 raw byte ($255 - \text{rem}$) |
| **New Match Length** | `LEN=3` | `LEN=3` | 3 fixed bits for lengths 2–8; `111` + Elias for $\ge 9$ |
| **Parser** | Sparse DP (`SDP`, $K=2$) | Adaptive Sparse DP ($K=32/16/8$) | Pareto-optimal dynamic programming |
| **Hash Match Finder** | 3-byte hash, max chain 8 | 3-byte hash, max chain 48 | Table size $2^{17}$, power-of-2 skip steps |
| **Suffix Array Builder** | SA-IS linear time $O(N)$ | SA-IS linear time $O(N)$ | Oracle-verified deterministic SA |
| **SA Search Window** | Adaptive $\pm 4 / \pm 8 / \pm 16$ | Dense $1..128$ + geometric jumps up to 512 | Exponential multi-scale probing for large files |
| **Candidate Cap** | 16 | 48 | Maximum candidates evaluated per position |
| **Stride / Periodicity** | Multi-window (stride $\ge 2$) | Multi-window + Containers + Large Strides | Autocorrelation for CSV, XLS, RGB24, RGBA32, PCM16, MP3, TAR (512b), SQLite (4096b), and large pitches (2048..8192) |
| **Bridge Matching** | - | 8-entry MRU offset cache | Bridges short matches ($L \ge 2$) into cheap 1-bit `MatchLast` repeat-offsets |
| **Length-2 Filter** | Strict ($off \le 32$ or rep) | Strict ($off \le 32$ or rep) | Prevents inflation on pre-compressed data (PNG, JPG, MP3) |

---

## 8. Benchmark Results

Measured on x86_64 Linux (GCC `-O3`). Reproducible via `make bench_all` and `make bench_dec`.  
All LZ0 decompressions verified **100% bit-exact** against uncompressed originals.

### 8.1 Real-World Corpus: Multi-Compressor Comparison

Comparison on real-world representative files (Canterbury, Silesia, Windows PE, ELF, pre-compressed images/audio, and raw media textures) between **LZ0** (default `-6` and ultra `-9`), **UPKR**, **LZ4**, and **ZX0**:

| File | Type / Category | Raw Size | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (default) | ZX0 | Decomp. `lz0dec_fast` vs LZ4 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | C source code | 11 150 B | 3 397 B (**30.47%**) | **3 374 B (30.26%)** | 2 928 B (26.26%) [-9] | 5 215 B (46.77%) | 3 214 B (28.83%) | **782 Mo/s** vs 2 554 Mo/s |
| `sum` | SPARC ELF binary | 38 240 B | 12 125 B (**31.71%**) | **11 898 B (31.11%)** | 9 927 B (25.96%) [-6] | 18 812 B (49.19%) | 11 415 B (29.85%) | **808 Mo/s** vs 2 651 Mo/s |
| `lz0_win64.exe` | Windows PE64 | 146 944 B | 75 147 B (**51.14%**) | **73 802 B (50.22%)** | 67 560 B (45.98%) [-6] | 101 596 B (69.14%) | *(trop lent)* | **410 Mo/s** vs 3 429 Mo/s |
| `avatar.jpg` | JPEG Image | 25 380 B | 25 326 B (**99.79%**) | **25 281 B (99.61%)** | 25 749 B (101.45%) [-6] | 25 379 B (100.00%) | *(trop lent)* | **8 848 Mo/s** vs 32 413 Mo/s |
| `break.mp3` | MP3 Audio CBR | 38 080 B | 11 299 B (**29.67%**) | **11 243 B (29.52%)** | 11 287 B (29.64%) [-6] | 11 552 B (30.34%) | *(trop lent)* | **9 730 Mo/s** vs 17 165 Mo/s |
| `star.png` | PNG Image (Deflate) | 355 666 B | 357 301 B (**100.46%**) | **356 643 B (100.27%)** | 364 646 B (102.52%) [-2] | 356 988 B (100.37%) | *(trop lent)* | **10 034 Mo/s** vs 30 048 Mo/s |
| `texture_rgb24.raw` | Raw RGB24 Texture | 196 608 B | 171 712 B (**87.34%**) | **171 429 B (87.19%)** | 86 531 B (44.01%) [-6] | 197 380 B (100.39%) | *(trop lent)* | **262 Mo/s** vs 32 617 Mo/s |
| `texture_rgba32.raw`| Raw RGBA32 Texture | 262 144 B | 146 503 B (**55.89%**) | **135 901 B (51.84%)** | *(exclu >256K)* | 263 173 B (100.39%) | *(trop lent)* | **382 Mo/s** vs 28 854 Mo/s |
| `audio_pcm16.raw` | Stereo PCM16 Audio | 192 000 B | 139 008 B (**72.40%**) | **134 434 B (70.02%)** | 117 053 B (61.07%) [-6] | 163 730 B (85.28%) | *(trop lent)* | **401 Mo/s** vs 7 564 Mo/s |
| `alice29.txt` | English text | 152 089 B | 58 949 B (**38.76%**) | **58 655 B (38.57%)** | 50 488 B (33.20%) [-6] | 88 699 B (58.32%) | *(trop lent)* | **602 Mo/s** vs 2 660 Mo/s |
| `plrabn12.txt` | Poetry text | 481 861 B | 208 776 B (**43.33%**) | **207 515 B (43.07%)** | 175 776 B (36.48%) [-2] | 325 589 B (67.57%) | *(trop lent)* | **530 Mo/s** vs 2 712 Mo/s |
| `kennedy.xls` | Excel Spreadsheet | 1 029 744 B | 204 581 B (**19.87%**) | **201 093 B (19.53%)** | 64 830 B (6.30%) [-2] | 374 704 B (36.39%) | *(trop lent)* | **1 339 Mo/s** vs 3 125 Mo/s |
| `dickens_1m.txt` | Large Text (1 MiB) | 1 048 576 B | 398 158 B (**37.97%**) | **394 996 B (37.67%)** | 331 404 B (31.61%) [-2] | 642 924 B (61.31%) | *(trop lent)* | **534 Mo/s** vs 2 676 Mo/s |
| `bash_x86_64.bin` | ELF x86_64 binary | 1 298 416 B | 613 917 B (**47.28%**) | **600 868 B (46.28%)** | 553 594 B (42.64%) [-2] | 871 664 B (67.13%) | *(trop lent)* | **404 Mo/s** vs 2 797 Mo/s |

> **Key takeaway:** LZ0 delivers dense ratios competitive with UPKR on text and binaries while systematically outperforming both UPKR and LZ4 on pre-compressed and high-entropy media (`avatar.jpg` and `break.mp3` with zero inflation). Against LZ4, LZ0 cuts compressed size by **15% to 48%**, while providing decompressor throughputs of **400 to 1,340 Mo/s** (up to **10,000 Mo/s** on images).

---

### 8.2 Concatenated Heterogeneous Blobs (256 KiB to 5 MiB)

In embedded scenarios (firmware, game resource packages, packers), multiple disparate files are concatenated into a single continuous binary blob. Under **Approach A**, the compressed data remains **one single continuous LZ0 stream** decompressed by freestanding routines (<300 bytes) without container metadata:

| Target / Contents | Raw Size | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (default) | Decomp. `lz0dec_fast` vs UPKR |
|---|---:|---:|---:|---:|---:|---:|
| **`blob_png_256k`** (4 PNGs) | 262 144 B | 247 405 B (94.38%) | **246 567 B (94.06%)** | 247 888 B (94.56%) [-6] | 252 446 B (96.30%) | **1 676 Mo/s** vs 17.6 Mo/s (**95×**) |
| **`blob_mix_256k`** (5 files: ELF, PE, XLS, PNG, txt) | 262 144 B | 128 155 B (48.89%) | 126 018 B (48.07%) | **110 904 B (42.31%)** [-6] | 168 178 B (64.15%) | **676 Mo/s** vs 37.9 Mo/s (**18×**) |
| **`blob_png_512k`** (8 PNGs) | 524 288 B | 504 320 B (96.19%) | **502 897 B (95.92%)** | 508 723 B (97.03%) [-2] | 511 352 B (97.53%) | **2 201 Mo/s** vs 17.1 Mo/s (**128×**) |
| **`blob_mix_512k`** (7 files: ELF, PE, PNGs, XLS, C, txt) | 524 288 B | 286 374 B (54.62%) | 283 212 B (54.02%) | **260 451 B (49.68%)** [-2] | 352 088 B (67.16%) | **779 Mo/s** vs 32.2 Mo/s (**24×**) |
| **`blob_png_2m`** (8 distinct PNGs > 200 KiB) | 2 097 152 B | 2 056 945 B (98.08%) | **2 052 535 B (97.87%)** | 2 089 034 B (99.61%) [-1] | 2 067 355 B (98.58%) | **3 432 Mo/s** vs ~15 Mo/s (**228×**) |
| **`blob_mix_2m`** (9 files: ELF, PE, 3 PNGs, XLS, txt, C) | 2 097 152 B | 1 198 585 B (57.15%) | 1 187 523 B (56.63%) | **1 125 643 B (53.67%)** [-1] | 1 484 306 B (70.78%) | **692 Mo/s** vs ~15 Mo/s (**46×**) |
| **`blob_png_5m`** (15 distinct PNGs) | 5 242 880 B | **4 362 024 B (83.20%)** | *(analyzed)* | *(excluded >20 min)* | 5 210 986 B (99.39%) | **5 046 Mo/s** vs - |
| **`blob_mix_5m`** (15 distinct real-world files) | 5 242 880 B | 2 191 126 B (41.79%) | **2 160 499 B (41.21%)** | *(excluded >30 min)* | 3 093 616 B (59.01%) | **657 Mo/s** vs - |

---

### 8.3 Architectural Trade-Offs: Speed vs. Ratio vs. Complexity

| Compressor | Compressed Ratio | Encoding Time (1 MiB) | Decompression Throughput | Decoder Memory / Footprint |
|---|---:|---:|---:|---|
| **LZ0 (ASM FAST)** | **19% – 52%** | **~1.5 s – 2.7 s** (`-6`) | **400 – 1,340 MB/s** (up to **10,000 MB/s** on images) | **0 bytes** (2.8 KiB asm + 264 B table, 0 malloc) |
| **LZ0 (ASM Compact)** | **19% – 52%** | **~1.5 s – 2.7 s** (`-6`) | **230 – 760 MB/s** (up to **2,850 MB/s** on images) | **0 bytes** (1.3 KiB asm, 0 malloc) |
| **LZ0 (C library)** | **19% – 52%** | **~1.5 s – 2.7 s** (`-6`) | **175 – 600 MB/s** (up to **1,250 MB/s** on images) | **0 bytes** (portable C, 0 malloc) |
| **ZX0** | 19% – 45% | *Minutes to hours* | ~250 MB/s | **0 bytes** (tiny C, 0 malloc) |
| **UPKR** | 6% – 46% | ~30 s – 300 s | **15 – 60 MB/s** (10×–100× slower) | Requires rANS entropy tables |
| **LZ4** | 36% – 100% | < 3 ms | ~2,500 – 32,000 MB/s | Fast hash table |

---

### 8.4 Decompression Speedup: C vs. Compact ASM vs. FAST ASM

Measured with `make bench_dec` (x86-64 native) and `make bench_dec_arm64` (AArch64 under QEMU):

#### x86-64 Native Decompression

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_x86_64`) | ASM FAST (`lz0dec_fast_x86_64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 304.0 MB/s | 315.3 MB/s | 420.3 MB/s | **1,022.2 MB/s** | **+236.3%** |
| `cp.html` | 206.0 MB/s | 212.9 MB/s | 287.4 MB/s | **842.2 MB/s** | **+308.9%** |
| `alice29.txt` | 197.0 MB/s | 206.4 MB/s | 277.1 MB/s | **616.0 MB/s** | **+212.6%** |
| `plrabn12.txt` | 186.4 MB/s | 195.0 MB/s | 268.0 MB/s | **528.1 MB/s** | **+183.3%** |
| `kennedy.xls` | 598.1 MB/s | 600.4 MB/s | 757.2 MB/s | **1,330.3 MB/s** | **+122.4%** |
| `dickens_1m.txt` | 200.2 MB/s | 221.6 MB/s | 291.6 MB/s | **521.4 MB/s** | **+160.5%** |
| `bash_x86_64.bin` | 175.3 MB/s | 179.3 MB/s | 232.9 MB/s | **397.1 MB/s** | **+126.5%** |

#### AArch64 / ARM64 Decompression (under QEMU)

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_arm64`) | ASM FAST (`lz0dec_fast_arm64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 43.4 MB/s | 94.9 MB/s | 74.5 MB/s | **261.0 MB/s** | **+502.0%** |
| `cp.html` | 35.6 MB/s | 76.2 MB/s | 55.3 MB/s | **214.9 MB/s** | **+503.6%** |
| `alice29.txt` | 34.9 MB/s | 68.9 MB/s | 41.8 MB/s | **213.0 MB/s** | **+510.5%** |
| `plrabn12.txt` | 32.3 MB/s | 64.8 MB/s | 34.6 MB/s | **175.6 MB/s** | **+443.7%** |
| `kennedy.xls` | 53.6 MB/s | 125.8 MB/s | 121.8 MB/s | **377.4 MB/s** | **+604.6%** |
| `dickens_1m.txt` | 36.4 MB/s | 72.6 MB/s | 35.6 MB/s | **170.1 MB/s** | **+367.6%** |
| `bash_x86_64.bin` | 30.8 MB/s | 61.3 MB/s | 45.2 MB/s | **143.7 MB/s** | **+366.6%** |

#### RISC-V 64 (RV64GC) Decompression (under QEMU)

| File | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_riscv64`) | ASM FAST (`lz0dec_fast_riscv64`) | Speedup FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 37.1 MB/s | 120.5 MB/s | 176.1 MB/s | **178.4 MB/s** | **+380.5%** |
| `cp.html` | 32.1 MB/s | 103.7 MB/s | 140.4 MB/s | **141.7 MB/s** | **+341.9%** |
| `alice29.txt` | 25.2 MB/s | 89.6 MB/s | 119.0 MB/s | **121.2 MB/s** | **+380.6%** |
| `plrabn12.txt` | 21.6 MB/s | 82.4 MB/s | 107.5 MB/s | **108.6 MB/s** | **+402.5%** |
| `kennedy.xls` | 50.1 MB/s | 200.1 MB/s | 285.6 MB/s | **295.9 MB/s** | **+490.0%** |
| `dickens_1m.txt` | 24.2 MB/s | 88.9 MB/s | 115.1 MB/s | **117.9 MB/s** | **+386.6%** |
| `bash_x86_64.bin` | 23.5 MB/s | 86.6 MB/s | 113.3 MB/s | **112.4 MB/s** | **+378.5%** |

> [!NOTE]
> **Key Architectural Takeaways on Decompression Speedup**:
> * **On x86-64**: Decompression throughput doubles or triples on text and source code (jumping from ~200 MB/s to **520 – 840 MB/s**), and reaches **1,330 MB/s (1.33 GB/s)** on structured datasets (`kennedy.xls`). The speedup stems directly from eliminating branch misprediction penalties on Elias gamma bit-sequences via 64-bit window decoding, and deploying 16-byte SSE2 wildcopies.
> * **On AArch64 / ARM64**: Pure leaf function structure (0 bytes allocated on stack, 0 subroutine calls) combined with branch-free Elias gamma decoding drastically reduces dynamic instruction count, translating to a **5× to 7× speedup** over the standard C decoder.
> * **On RISC-V 64 (RV64GC)**: Pure leaf function structure (0 bytes stack frame, 0 calls, 100% in registers `a0..a7` and `t0..t6`), combined with 64-bit wildcopies (`ld`/`sd`) and arithmetic multiplication broadcast (`mul` by `0x0101010101010101`) for 1-byte runs, delivering a **4.5× to 5.9× speedup (+340% to +490%)** over standard C.

---

### 8.5 Synthetic Scaling Benchmarks

Measured on in-memory PRNG-generated datasets ([docs/BENCHMARKS.md](docs/BENCHMARKS.md)):

| Input Size | Text Ratio | Binary Ratio | Text Encode (H2C8) | Binary Encode (H2C8) | Decompress Throughput |
|---|---:|---:|---:|---:|---:|
| **256 KiB** | 18.44% | 6.63% | 342 ms | 349 ms | ~320 – 825 MB/s |
| **1 MiB** | 18.07% | 5.76% | 1,342 ms | 1,617 ms | ~310 – 1,010 MB/s |
| **2 MiB** | 17.97% | 5.33% | 2,997 ms | 3,651 ms | ~330 – 770 MB/s |
| **5 MiB** | 17.78% | 4.90% | 8,727 ms | 11,428 ms | ~245 – 1,210 MB/s |

---

## 9. Building and Using LZ0

### 9.1 Compiling with Make
A standard C11 compiler (`gcc` or `clang`) and `make` are required:

```bash
# Build the CLI binary in bin/lz0
make lz0

# Run complete test suite (golden vectors, boundary conditions, malformed inputs)
make test

# Run individual verification suites
make vectors     # Golden test vectors against all decoders
make boundary    # Offset and length boundary stress tests
make malformed   # Truncated and corrupt input rejection tests
make fuzz        # Round-trip fuzz testing
make sanitize    # AddressSanitizer + UndefinedBehaviorSanitizer audit
make valgrind    # Valgrind zero-leak dynamic analysis

# Run benchmarks
make bench_all   # Multi-compressor benchmark (LZ0 vs ZX0 vs UPKR vs LZ4)
make bench_dec   # Decompression throughput comparison (C vs ASM)

# Cross-platform tests
make test_arm64    # Cross-compile and test AArch64 ASM under QEMU
make test_riscv64  # Cross-compile and test RISC-V 64 (RV64GC) ASM under QEMU
make bench_dec_riscv64 # Benchmark RISC-V 64 decoders under QEMU
make dist_riscv64  # Build official release package for linux-riscv64
```

### 9.2 Command-Line Usage
The `lz0` executable operates on disk files using the `LZ01` container:

```bash
# Compress a file (default level 6, standard H2C8)
./bin/lz0 c original.bin compressed.lz0

# Compress with explicit compression level (-1 fast to -9 ultra)
./bin/lz0 c -1 original.bin fast.lz0    # Fast mode (K=2, smaller search window)
./bin/lz0 c -9 original.bin ultra.lz0   # Ultra mode (adaptive K=32 DP, multi-scale SA window, media stride detection, strict len2 filter)

# Decompress a file
./bin/lz0 d compressed.lz0 restored.bin

# Verify identical content
cmp original.bin restored.bin
```

### 9.3 C Library API

#### Compression
```c
#include "lz0.h"
#include <stdlib.h>

lz0_config cfg;
lz0_stats stats;

// Configure compression level (1 = fast, 6 = default H2C8, 9 = ultra maximal compression)
lz0_config_level(&cfg, 9);

unsigned char *compressed = NULL;
size_t comp_size = 0;

int status = lz0_compress(input_data, input_size,
                          &compressed, &comp_size,
                          &cfg, &stats);
if (status == 0) {
    // Successfully compressed into 'compressed' buffer (size 'comp_size')
    free(compressed); // Caller owns the allocated output buffer
}
```

#### Decompression
The decompression API does not allocate memory. The caller supplies the destination buffer:

```c
#include "lz0.h"

// Decompresses raw LZ0 bitstream into 'output_buffer'
int decompressed_bytes = lz0_decompress_to(raw_payload, raw_payload_size,
                                           output_buffer, max_output_size);

if (decompressed_bytes < 0) {
    // Decompression failed (malformed bitstream, truncation, or out-of-bounds)
}
```

---

## 10. Specification & Documentation

- [docs/LZ0_FORMAT.md](docs/LZ0_FORMAT.md) — Normative bitstream specification, bit-level grammar, and encoding rules.
- [docs/LZ0_DECODER_SPEC.md](docs/LZ0_DECODER_SPEC.md) — Detailed decoder procedure, bitreader semantics, and invariants.
- [docs/LZ0_ENCODER.md](docs/LZ0_ENCODER.md) — Technical details of the H2C8 encoder pipeline and tuning knobs.
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) — Reproducible benchmark data and methodology.

---

## 11. License

This project is licensed under the same terms as the repository root (`LICENSE`).

## Acknowledgements

LZ0 builds upon concepts from the LZ77/LZSS compression lineage and takes major inspiration from **ZX0** by Einar Saukas, particularly in its commitment to decoder simplicity and Elias-coded offset hierarchies.
