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

echo "==> Setting up sample media assets..."
if [ -f /usr/share/plymouth/themes/solar/star.png ]; then
    cp /usr/share/plymouth/themes/solar/star.png "$CORPUS_DIR/star.png"
elif [ -f /usr/share/pixmaps/archlinux-logo.png ]; then
    cp /usr/share/pixmaps/archlinux-logo.png "$CORPUS_DIR/star.png"
fi

if [ -f /usr/share/avatars/Seven_avatar_19.jpg ]; then
    cp /usr/share/avatars/Seven_avatar_19.jpg "$CORPUS_DIR/avatar.jpg"
fi

for mp in /usr/share/*/resources/app/out/vs/platform/accessibilitySignal/browser/media/break.mp3; do
    if [ -f "$mp" ]; then
        cp "$mp" "$CORPUS_DIR/break.mp3"
        break
    fi
done

python3 -c "
import struct, math, os

cdir = '$CORPUS_DIR'

# 1. RGB24 texture
rgb = bytearray()
for y in range(256):
    for x in range(256):
        r = int(128 + 127 * math.sin(x / 16.0)) & 0xFF
        g = int(128 + 127 * math.cos(y / 16.0)) & 0xFF
        b = int((x * 2 + y * 3) % 256) & 0xFF
        rgb.extend([r, g, b])
with open(os.path.join(cdir, 'texture_rgb24.raw'), 'wb') as f:
    f.write(rgb)

# 2. RGBA32 texture
rgba = bytearray()
for y in range(256):
    for x in range(256):
        r = int(128 + 127 * math.sin(x / 16.0)) & 0xFF
        g = int(128 + 127 * math.cos(y / 16.0)) & 0xFF
        b = int((x * 2 + y * 3) % 256) & 0xFF
        a = 255
        rgba.extend([r, g, b, a])
with open(os.path.join(cdir, 'texture_rgba32.raw'), 'wb') as f:
    f.write(rgba)

# 3. PCM16 stereo audio
pcm = bytearray()
for i in range(48000 * 2):
    t = i / 48000.0
    s_l = int(14000 * math.sin(2 * math.pi * 440.0 * t))
    s_r = int(14000 * math.sin(2 * math.pi * 554.37 * t))
    pcm.extend(struct.pack('<hh', s_l, s_r))
with open(os.path.join(cdir, 'audio_pcm16.raw'), 'wb') as f:
    f.write(pcm)
"

echo "==> Corpus ready in $CORPUS_DIR"
ls -lh "$CORPUS_DIR"

