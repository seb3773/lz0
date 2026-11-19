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

Streams must remain byte-identical (`csz` / `bits` / tokens / round-trip) on those corpora.
