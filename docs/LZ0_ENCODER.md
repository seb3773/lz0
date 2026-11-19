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
