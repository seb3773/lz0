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
1. **Hash-Chain Finder:** Rapidly identifies nearby matches. Uses a 3-byte hash table of $2^{17}$ entries (128K buckets) and traverses a history chain limited to 8 candidates with power-of-2 skip stepping.
2. **SA-IS Suffix Array Finder:** Identifies distant and long matches across the entire buffer in linear $O(N)$ time.
   - Built using an optimized **SA-IS** algorithm (`lz0_sa_algo=1`).
   - Validated against a deterministic $O(N \log^2 N)$ prefix-doubling reference oracle (`lz0_sa_algo=0`), ensuring byte-identical suffix arrays.
   - **Adaptive Probing:** Performs rank-locality search starting at $\pm 4$, widening to $\pm 8$, and extending to $\pm 16$ only if the current best match length is $< 16$.

### 5.2 Sparse DP Parser ($K=2$) & Plateau Stepping
Instead of a greedy heuristic or a memory-intensive full DP lattice, H2C8 maintains at most **$K=2$ Pareto-optimal live states** per input position.

To minimize parsing time without sacrificing optimality, the parser implements **Elias Plateau Stepping** (`next_len_plateau`):
- Because Elias gamma lengths remain constant across powers-of-two intervals ($[2^k, 2^{k+1}-1]$), evaluating intermediate lengths inside a plateau incurs the same bit-cost as the maximum length of that plateau.
- The parser skips redundant transitions and only tests the plateau upper bounds, dramatically pruning search branches.

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

| Component | Setting | Description |
|---|---|---|
| **Bit Packing** | **MSB-first** | Bits 7 down to 0 in control bytes |
| **Offset Low Part** | 8 bits (`LSB=8`) | 1 raw byte ($255 - \text{rem}$) |
| **New Match Length** | `LEN=3` | 3 fixed bits for lengths 2–8; `111` + Elias for $\ge 9$ |
| **Parser** | Sparse DP (`SDP`) | Dynamic programming with $K=2$ live states |
| **Hash Match Finder** | 3-byte hash | Table size $2^{17}$, max chain 8 |
| **Suffix Array Builder** | SA-IS (`algo=1`) | Linear time $O(N)$, oracle-verified |
| **SA Search Window** | Adaptive $\pm 4 / \pm 8 / \pm 16$ | Expands only when best match $< 16$ |
| **Candidate Cap** | 16 | Maximum candidates evaluated per position |

---

## 8. Benchmark Results

Measured on x86_64 Linux (GCC `-O3`). Reproducible via `make bench_all` and `make bench_dec`.

### 8.1 Real-World Corpus: Multi-Compressor Comparison

Comparison on standard corpus files ([Canterbury Corpus](https://corpus.canterbury.ac.nz/) and [Silesia](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia)) between **LZ0** (default `-6` and ultra `-9`), **ZX0**, **UPKR**, and **LZ4**:

| File | Original Size | LZ0 (`-6` default) | LZ0 (`-9` ultra) | ZX0 | UPKR | LZ4 (default) |
|---|---:|---:|---:|---:|---:|---:|
| `fields.c` | 11.1 KiB | 3,367 B (**30.20%**) | 3,341 B (**29.96%**) | 3,214 B (**28.83%**) | 2,928 B (**26.26%**) | 5,215 B (46.77%) |
| `alice29.txt` | 148.5 KiB | 58,992 B (**38.79%**) | 58,568 B (**38.51%**) | *timeout (>60s)* | 50,547 B (**33.24%**) | 88,699 B (58.32%) |
| `plrabn12.txt` | 470.6 KiB | 209,592 B (**43.50%**) | 207,790 B (**43.12%**) | *timeout (>60s)* | 174,559 B (**36.23%**) | 325,589 B (67.57%) |
| `kennedy.xls` | 1,005.6 KiB | 204,305 B (**19.84%**) | 201,110 B (**19.53%**) | *timeout (>60s)* | 64,765 B (**6.29%**) | 374,704 B (36.39%) |
| `dickens_1m.txt` | 1,024.0 KiB | 400,138 B (**38.16%**) | 396,187 B (**37.78%**) | *timeout (>60s)* | 328,450 B (**31.32%**) | 642,924 B (61.31%) |
| `bash_x86_64.bin` | 1,268.0 KiB | 613,639 B (**47.26%**) | 599,401 B (**46.16%**) | *timeout (>60s)* | 553,594 B (**42.64%**) | 871,664 B (67.13%) |

> **Key takeaway:** LZ0 provides ratios close to ZX0 and UPKR while staying practical for files $\ge 1$ MiB where ZX0's exhaustive search becomes intractable. Compared to LZ4, LZ0 cuts compressed size by **30% to 50%**.

---

### 8.2 Architectural Trade-Offs: Speed vs. Ratio vs. Complexity

| Compressor | Compressed Ratio | Encoding Time (1 MiB) | Decompression Throughput | Decoder Memory / Footprint |
|---|---:|---:|---:|---|
| **LZ0 (ASM FAST)** | **20% – 47%** | **~5.2 s** (`-6`) | **400 – 1,330 MB/s** | **0 bytes** (2.8 KiB asm + 264 B table, 0 malloc) |
| **LZ0 (ASM Compact)** | **20% – 47%** | **~5.2 s** (`-6`) | **230 – 760 MB/s** | **0 bytes** (1.3 KiB asm, 0 malloc) |
| **LZ0 (C library)** | **20% – 47%** | **~5.2 s** (`-6`) | **175 – 600 MB/s** | **0 bytes** (portable C, 0 malloc) |
| **ZX0** | 19% – 45% | *Minutes to hours* | ~250 MB/s | **0 bytes** (tiny C, 0 malloc) |
| **UPKR** | 6% – 36% | ~146 s | **2 – 24 MB/s** (10×–100× slower) | Requires rANS entropy tables |
| **LZ4** | 36% – 67% | < 3 ms | ~2,500 – 3,200 MB/s | Fast hash table |

---

### 8.3 Decompression Speedup: C vs. Compact ASM vs. FAST ASM

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

> [!NOTE]
> **Key Architectural Takeaways on Decompression Speedup**:
> * **On x86-64**: Decompression throughput doubles or triples on text and source code (jumping from ~200 MB/s to **520 – 840 MB/s**), and reaches **1,330 MB/s (1.33 GB/s)** on structured datasets (`kennedy.xls`). The speedup stems directly from eliminating branch misprediction penalties on Elias gamma bit-sequences via 64-bit window decoding, and deploying 16-byte SSE2 wildcopies.
> * **On AArch64 / ARM64**: Pure leaf function structure (0 bytes allocated on stack, 0 subroutine calls) combined with branch-free Elias gamma decoding drastically reduces dynamic instruction count, translating to a **5× to 7× speedup** over the standard C decoder.

---

### 8.4 Synthetic Scaling Benchmarks

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
make test_arm64  # Cross-compile and test AArch64 ASM under QEMU
```

### 9.2 Command-Line Usage
The `lz0` executable operates on disk files using the `LZ01` container:

```bash
# Compress a file (default level 6, standard H2C8)
./bin/lz0 c original.bin compressed.lz0

# Compress with explicit compression level (-1 fast to -9 ultra)
./bin/lz0 c -1 original.bin fast.lz0    # Fast mode (K=2, smaller search window)
./bin/lz0 c -9 original.bin ultra.lz0   # Ultra mode (K=16, SA window 128, deep probes, fine match DP)

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
