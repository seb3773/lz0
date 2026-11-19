#!/usr/bin/env bash
set -euo pipefail

CORPUS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/corpus" && pwd)"
mkdir -p "$CORPUS_DIR"

echo "==> Fetching Canterbury corpus..."
curl -sL https://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz | tar xz -C "$CORPUS_DIR"

echo "==> Fetching Silesia (dickens) slices (1M, 2M, 5M)..."
TMP_DICKENS=$(mktemp)
curl -sL https://sun.aei.polsl.pl/~sdeor/corpus/dickens.bz2 | bzip2 -d > "$TMP_DICKENS"
head -c 1048576 "$TMP_DICKENS" > "$CORPUS_DIR/dickens_1m.txt"
head -c 2097152 "$TMP_DICKENS" > "$CORPUS_DIR/dickens_2m.txt"
head -c 5242880 "$TMP_DICKENS" > "$CORPUS_DIR/dickens_5m.txt"
rm -f "$TMP_DICKENS"

echo "==> Copying host ELF binary..."
cp /bin/bash "$CORPUS_DIR/bash_x86_64.bin"

echo "==> Corpus ready in $CORPUS_DIR"
ls -lh "$CORPUS_DIR"

