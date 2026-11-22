#!/usr/bin/env bash
# usage: run.sh <ub-binary> <mode> <set-dir> <out.tsv> [jobs]
# Runs one file per process in parallel and writes a sorted result table.
set -euo pipefail
UB="$1"; MODE="$2"; SET="$3"; OUT="$4"; JOBS="${5:-16}"
ls -S "$SET"/* | xargs -P "$JOBS" -I{} "$UB" "$MODE" {} | sort > "$OUT"
awk '{n+=$2; c+=$3; t+=$4; if ($5!="OK") bad++} END {printf "TOTAL %d -> %d  (%.4f%%)  cpu %.1fs  %s\n", n, c, 100*c/n, t/1000, bad?"FAIL":"all OK"}' "$OUT"
