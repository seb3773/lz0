# LZ0 encoder

Version: 0.1

This document is **informative**. The raw bitstream is defined by `LZ0_FORMAT.md`. An encoder may use any method that emits a valid stream.

The production encoder in this repository is frozen H2C8. It is not required to reimplement it in order to decode LZ0.

## Pipeline

```
input
  -> hash3 chain finder (max_chain=8, 2^k skip, jump_levels=0)
  -> SA-IS suffix array + rank
  -> adaptive SA probe +/-4, then +/-8, then +/-16 if best_len < 16
  -> candidate set (max 16)
  -> sparse DP, K=2 live states
  -> LEN=3 plateau emission
  -> token list
  -> bitstream (LSB=8, LEN=3)
```

Finder proposes matches. Parser chooses. Bitstream is behind `cost_*` / `lz0_emit`.

## Frozen defaults

Apply with `lz0_h2c8_apply()` after `lz0_config_default(cfg, LZ0_PARSE_SDP)`:

| Knob | Value |
|------|-------|
| parser | SDP |
| min_match | 2 |
| max_chain | 8 |
| max_candidates | 16 |
| max_states (K) | 2 |
| jump_levels | 0 |
| hash | hash3 + 2^k skip |
| SA | `lz0_sa_algo=1` (SA-IS) |
| SA oracle | `lz0_sa_algo=0` prefix-doubling, identical `sa[]`/`rank[]` |
| SA window | 16, hybrid on, adaptive on |
| lsb_bits | 8 |
| len_code | 3 |

Diagnostic flags (`lz0_chain_noskip`, `lz0_skip_thresh`, `lz0_hash4_probe`, `lz0_deep_probe`, `lz0_sa_lcp`, census) default off / NULL.

## SA builders

- `lz0_sa_algo=1` — production SA-IS
- `lz0_sa_algo=0` — prefix-doubling oracle

Both must produce identical `sa[]` and `rank[]` and therefore identical compressed streams.

Do not micro-optimize SA-IS further in v0.1.

## Tokens emitted

Same semantics as the format:

- LiteralRun
- MatchLast (only immediately after a literal)
- MatchNew (`L >= 2`)
- EOF (`1` then `0`)

`last_offset` starts at 1.

## Ultra parser (level 9)

Level 9 uses `lz0_parse_ultra` (`lz0/lz0_ultra.c`) instead of the generic SDP:

- States `(last_offset, after_literal)` stored as compact 12-byte
  structure-of-arrays rows, with a 64-bit key filter and a cached worst slot.
- State count follows `lz0_ultra_budget` (768 MiB) at 12 bytes per state
  per input position: `K = budget / ((n + 1) * 12)`, capped at 128 and
  floored at 12. That is 128 below 512 KiB, 63 at 1 MiB, 31 at 2 MiB,
  and 12 at 5 MiB.
- Margin prune: a state costing more than `row_best + 32 + elias(msb(offset))`
  bits is dropped (a single repeat match cannot win that back in practice).
- Length-2 matches are allowed at any offset (`lz0_len2_max_off = 0`).
- Inside long byte runs the finder still searches the last 64 positions
  (`lz0_rle_tail`), so matches that continue past a run are found.
- From 16 MiB (`n > 0x00ffffff`) `lz0_parse_ultra` calls the generic SDP
  parser, because the position no longer fits in the packed state. The
  level-9 finder is unchanged. SDP uses `max_states`, which level 9 sets
  to 32.

An exhaustive ZX0-style reference parser (`bench/ultra/lz0_opt_ref.c`) shows
the remaining gap to optimal is about 0.05 % on 32 KiB samples.

## Speed-only paths

These change speed, never output. Every variant gives a bit-identical stream.

- `lz0_simd_matchlen` (`lz0/lz0_simd.h`): one inline 64-bit compare covers the
  first 8 bytes, then the active vector tier runs (AVX2 64/32 B, SSE2 and NEON
  32/16 B, RVV strip-mined, C fallback). Tails use overlapping in-bounds loads,
  so nothing past `max` is read. `make simd` checks every tier against a byte
  loop, with both operands ending on `PROT_NONE` pages.
- SA probe run-ahead (`sa_probe_neighbors`, `lz0/lz0_find.c`): while distance
  `d` is compared, the input bytes for `d + LZ0_SA_PF_K` (default 8) are
  prefetched. The run-ahead never passes the next adaptive stop (d = 4, 8)
  before that stop is ruled out. It is only used from `LZ0_SA_PF_MIN_INPUT`
  (default 2 MiB) up, since on cache-resident inputs it costs more than it
  hides. Probe order and results are unchanged.

## API

```c
void lz0_config_default(lz0_config *cfg, lz0_parse_t parser);
void lz0_h2c8_apply(void);
int lz0_compress(const unsigned char *in, size_t in_sz,
                 unsigned char **out, size_t *out_sz,
                 const lz0_config *cfg, lz0_stats *stats);
```

`lz0_compress` allocates `*out`. Caller frees it. Empty input is rejected by this API (`in_sz == 0` returns `-1`). Empty files are a container concern (`lz0 c` writes size 0, empty payload).

## Decoder independence

Changing the encoder, SA constructor, or DP search must not change the format. Golden vectors in `tests/vectors/` lock the bitstream. Independent decoders `lz0dec_ref.c` and `lz0dec_min.c` do not include encoder headers.
