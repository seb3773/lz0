#!/usr/bin/env bash
# Build the ultra-mode evaluation corpus.
#   bench/ultra/corpus/M : main set (files up to 512 KiB), used to accept/reject changes
#   bench/ultra/corpus/S : 32 KiB slices, small enough for the exhaustive reference parser
#   bench/ultra/corpus/L : large inputs (1-5 MiB), used to check time and memory scaling
# Sources: repository Canterbury corpus, Silesia corpus (SILESIA_DIR), host
# binaries/media when present, and deterministic synthetic data.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CAN="$ROOT/bench/corpus"
SIL="${SILESIA_DIR:-/tmp/silesia}"
OUT="$ROOT/bench/ultra/corpus"
M="$OUT/M"; S="$OUT/S"; L="$OUT/L"
rm -rf "$OUT"; mkdir -p "$M" "$S" "$L"

slice() { # src dst offset size
    [ -f "$1" ] || { echo "skip $1"; return 0; }
    dd if="$1" of="$2" bs=1 skip=0 count=0 status=none
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys
src, dst, off, size = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
d = open(src, 'rb').read()
if off + size > len(d): off = max(0, len(d) - size)
open(dst, 'wb').write(d[off:off+size])
PY
}

for f in alice29.txt asyoulik.txt cp.html fields.c grammar.lsp xargs.1 sum; do cp "$CAN/$f" "$M/$f"; done
slice "$CAN/kennedy.xls" "$M/kennedy_256k.xls" 0 262144
slice "$CAN/ptt5" "$M/ptt5_256k" 0 262144
slice "$CAN/lcet10.txt" "$M/lcet10_256k.txt" 0 262144

for f in dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml; do
    slice "$SIL/$f" "$M/sil_${f}_256k" 2097152 262144
    slice "$SIL/$f" "$S/sil_${f}_32k" 1048576 32768
done

BASH_BIN="$(readlink -f /bin/bash)"
slice "$BASH_BIN" "$M/elf_bash_256k" 262144 262144
slice "$BASH_BIN" "$S/elf_bash_32k" 131072 32768
PNG=$(ls /nix/store/*/lib/python3*/idlelib/Icons/idle_256.png 2>/dev/null | head -1 || true)
[ -n "$PNG" ] && cp "$PNG" "$M/idle_256.png"
JPG=$(ls /nix/store/*/lib/libreoffice/share/gallery/backgrounds/tiger.jpg 2>/dev/null | head -1 || true)
[ -n "$JPG" ] && slice "$JPG" "$M/tiger_128k.jpg" 0 131072

python3 - "$M" "$S" <<'PY'
import struct, math, os, sys, random
M, S = sys.argv[1], sys.argv[2]
rgb = bytearray(); rgba = bytearray()
for y in range(256):
    for x in range(256):
        r = int(128 + 127 * math.sin(x / 16.0)) & 0xFF
        g = int(128 + 127 * math.cos(y / 16.0)) & 0xFF
        b = int((x * 2 + y * 3) % 256) & 0xFF
        rgb += bytes([r, g, b]); rgba += bytes([r, g, b, 255])
open(os.path.join(M, 'texture_rgb24.raw'), 'wb').write(rgb)
open(os.path.join(M, 'texture_rgba32.raw'), 'wb').write(rgba)
open(os.path.join(S, 'texture_rgb24_32k.raw'), 'wb').write(rgb[65536:65536+32768])
pcm = bytearray()
for i in range(48000):
    t = i / 48000.0
    pcm += struct.pack('<hh', int(14000 * math.sin(2 * math.pi * 440.0 * t)),
                       int(14000 * math.sin(2 * math.pi * 554.37 * t)))
open(os.path.join(M, 'audio_pcm16.raw'), 'wb').write(pcm)
open(os.path.join(S, 'audio_pcm16_32k.raw'), 'wb').write(pcm[32768:65536])
# noisy 16-bit samples with structure (sensor log style records)
rnd = random.Random(1234)
rec = bytearray()
v = 1000
for i in range(16384):
    v += rnd.randint(-40, 40)
    rec += struct.pack('<IHhB', 0x10000 + i * 10, 7, v, rnd.randint(0, 3))
open(os.path.join(M, 'records_struct.bin'), 'wb').write(rec)
open(os.path.join(S, 'records_struct_32k.bin'), 'wb').write(rec[:32768])
PY

# heterogeneous blob (single stream made of several file types)
python3 - "$M" "$CAN" "$SIL" "$BASH_BIN" "${PNG:-}" <<'PY'
import sys, os
M, CAN, SIL, BASHB, PNG = sys.argv[1:6]
parts = [(BASHB, 65536, 300000), (os.path.join(SIL, 'ooffice'), 65536, 1000000),
         (os.path.join(CAN, 'kennedy.xls'), 65536, 0), (PNG, 34816, 0),
         (os.path.join(CAN, 'alice29.txt'), 30720, 0), (os.path.join(SIL, 'xml'), 32768, 500000),
         (os.path.join(CAN, 'sum'), 38240, 0)]
buf = bytearray()
for p, n, off in parts:
    if p and os.path.exists(p):
        d = open(p, 'rb').read()
        buf += d[off:off+n]
open(os.path.join(M, 'blob_mix.bin'), 'wb').write(buf)
PY

for f in fields.c grammar.lsp xargs.1 sum; do cp "$CAN/$f" "$S/$f"; done
slice "$CAN/alice29.txt" "$S/alice_32k.txt" 40000 32768
slice "$CAN/kennedy.xls" "$S/kennedy_32k.xls" 300000 32768
slice "$CAN/ptt5" "$S/ptt5_32k" 100000 32768
cp "$CAN/kennedy.xls" "$L/kennedy.xls"
slice "$SIL/dickens" "$L/sil_dickens_1m" 0 1048576
slice "$SIL/ooffice" "$L/sil_ooffice_2m" 0 2097152
slice "$SIL/mozilla" "$L/sil_mozilla_5m" 0 5242880
slice "$SIL/samba" "$L/sil_samba_1m" 4194304 1048576
slice "$SIL/x-ray" "$L/sil_x-ray_1m" 0 1048576
ls -l "$M" "$S" "$L" | awk '{print $5, $9}'
