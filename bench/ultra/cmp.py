#!/usr/bin/env python3
"""Compare two run.sh result tables: per-file size delta and totals."""
import sys

def load(p):
    r = {}
    for line in open(p):
        f = line.split()
        if len(f) >= 5:
            r[f[0]] = (int(f[1]), int(f[2]), float(f[3]), f[4])
    return r

a, b = load(sys.argv[1]), load(sys.argv[2])
ta = tb = 0
ca = cb = 0.0
print(f"{'file':24} {'size':>9} {'A':>9} {'B':>9} {'delta':>7} {'%':>8} {'tA s':>7} {'tB s':>7}")
for k in sorted(a):
    if k not in b:
        continue
    n, sa, xa, _ = a[k]
    _, sb, xb, ok = b[k]
    ta += sa; tb += sb; ca += xa; cb += xb
    print(f"{k:24} {n:9} {sa:9} {sb:9} {sb-sa:7} {100.0*(sb-sa)/sa:8.3f} {xa/1000:7.1f} {xb/1000:7.1f} {'' if ok=='OK' else 'FAIL'}")
print(f"{'TOTAL':24} {'':9} {ta:9} {tb:9} {tb-ta:7} {100.0*(tb-ta)/ta:8.3f} {ca/1000:7.1f} {cb/1000:7.1f}")
