# Ultra-mode (-9) ratio bench

Tools used to evaluate encoder changes for level 9. The LZ0 format and the
decoders are not touched; every result is verified with `lz0dec_ref`.

- `make_corpus.sh` builds `corpus/M` (main set, 30 files up to 330 KiB: text,
  ELF/PE code, XLS, fax bitmap, databases, medical images, PNG/JPEG, PCM,
  RGB textures, structured records, a mixed blob), `corpus/S` (32 KiB slices
  for the exhaustive reference) and `corpus/L` (1-5 MiB, scaling checks).
  Silesia files are read from `$SILESIA_DIR` (default `/tmp/silesia`).
- `ub.c` compresses files and prints `name size packed ms OK hash`.
  Modes: `l1`..`l9` (production levels), `u` (level 9 config + ultra parser),
  `opt[:limit]` (exhaustive ZX0-style reference parser, `lz0_opt_ref.c`;
  O(n * window), only practical on small inputs).
  Experiment overrides: `UB_K`, `UB_MARGIN`, `UB_FINE`, `UB_INJ`, `UB_LEN2`,
  `UB_RLE`, `UB_CANDS`, `UB_CHAIN`, `UB_BT`, `UB_SAWIN`, `UB_MSCALE`, `UB_DEEP`.
- `run.sh <ub> <mode> <dir> <out.tsv>` runs one file per process in parallel.
- `cmp.py a.tsv b.tsv` prints per-file and total deltas.

Build:

    gcc -O3 -std=c11 -D_POSIX_C_SOURCE=200809L -Ilz0 -o /tmp/ub bench/ultra/ub.c \
        lz0/lz0_cost.c lz0/lz0_find.c lz0/lz0_parse.c lz0/lz0_emit.c \
        lz0/lz0_decompress.c lz0/lz0_compress.c lz0/lz0_ultra.c \
        bench/ultra/lz0_opt_ref.c lz0/lz0dec_ref.c

## Results (level 9, before -> after)

| set | before | after | delta |
|-----|-------:|------:|------:|
| S (23 files, 680 KB) | 303 250 | 301 057 | -0.72 % |
| S exhaustive reference | | 300 902 | (gap left: 0.05 %) |
| M (30 files, 5.8 MB) | 2 471 513 | 2 451 496 | -0.81 % (-0.30 % without the RGB24 texture) |

Every file of M got smaller; levels 1-8 are byte-identical to before.
