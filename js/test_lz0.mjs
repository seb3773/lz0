import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, dirname } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";

import { ERROR_CODES, Lz0Error, containerUncompressedSize, decode, decodeContainer } from "./lz0dec.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = dirname(HERE);
const VECTORS = join(REPO, "tests", "vectors");

test("golden vectors (raw + LZ01 container)", () => {
  for (const name of ["onebyte", "literals", "overlap", "mixed", "matchlast", "matchnew", "eof_after_lit"]) {
    const expected = new Uint8Array(readFileSync(join(VECTORS, `${name}.bin`)));
    const cont = new Uint8Array(readFileSync(join(VECTORS, `${name}.lz0`)));
    const raw = new Uint8Array(readFileSync(join(VECTORS, `${name}.lz0raw`)));

    assert.equal(containerUncompressedSize(cont), expected.length);

    const outRaw = new Uint8Array(expected.length);
    assert.equal(decode(raw, outRaw), expected.length);
    assert.deepEqual(outRaw, expected);

    const outCont = new Uint8Array(expected.length);
    assert.equal(decodeContainer(cont, outCont), expected.length);
    assert.deepEqual(outCont, expected);
  }
});

test("empty container vs empty raw", () => {
  const emptyCont = new Uint8Array(readFileSync(join(VECTORS, "empty.lz0")));
  const emptyRaw = new Uint8Array(readFileSync(join(VECTORS, "empty.lz0raw")));
  assert.equal(containerUncompressedSize(emptyCont), 0);
  assert.equal(decodeContainer(emptyCont, new Uint8Array(0)), 0);

  assert.throws(() => decode(emptyRaw, new Uint8Array(1)), (e) => e instanceof Lz0Error && e.code === ERROR_CODES.EMPTY_INPUT);
  assert.throws(() => decode(new Uint8Array([0x80]), new Uint8Array(0)), (e) => e instanceof Lz0Error && e.code === ERROR_CODES.EMPTY_OUTPUT);
});

test("argument validation, overlapping vs disjoint ArrayBuffer views", () => {
  const mixedRaw = new Uint8Array(readFileSync(join(VECTORS, "mixed.lz0raw")));
  const mixedBin = new Uint8Array(readFileSync(join(VECTORS, "mixed.bin")));

  assert.throws(() => decode("not bytes", new Uint8Array(8)), (e) => e instanceof Lz0Error && e.code === ERROR_CODES.INVALID_ARGUMENT);
  assert.throws(() => decode(mixedRaw, [0, 1]), (e) => e instanceof Lz0Error && e.code === ERROR_CODES.INVALID_ARGUMENT);

  // Overlapping regions inside one ArrayBuffer must fail
  const shared = new Uint8Array(128);
  shared.set(mixedRaw, 0);
  assert.throws(
    () => decode(shared.subarray(0, mixedRaw.length), shared.subarray(4, 4 + mixedBin.length)),
    (e) => e instanceof Lz0Error && e.code === ERROR_CODES.OVERLAPPING_BUFFERS
  );

  // Disjoint regions inside the SAME ArrayBuffer must work cleanly (zero extra buffers)
  const outSlice = shared.subarray(mixedRaw.length, mixedRaw.length + mixedBin.length);
  assert.equal(decode(shared.subarray(0, mixedRaw.length), outSlice), mixedBin.length);
  assert.deepEqual(outSlice, mixedBin);
});

test("truncated prefixes, bit flips, Elias overflow, and malformed streams", () => {
  for (const name of ["literals", "mixed", "matchnew", "overlap"]) {
    const raw = new Uint8Array(readFileSync(join(VECTORS, `${name}.lz0raw`)));
    const expected = new Uint8Array(readFileSync(join(VECTORS, `${name}.bin`)));
    const out = new Uint8Array(expected.length);
    for (let k = 0; k < raw.length; k++) {
      assert.throws(() => decode(raw.subarray(0, k), out), Lz0Error);
    }
    // Trailing extra bytes after EOF must succeed identically
    const padded = new Uint8Array(raw.length + 4);
    padded.set(raw, 0);
    assert.equal(decode(padded, out), expected.length);
    assert.deepEqual(out, expected);
  }

  // Zero flood cannot wrap signed 32-bit int; must throw LENGTH_OVERFLOW in O(1)
  assert.throws(
    () => decode(new Uint8Array(64), new Uint8Array(64)),
    (e) => e instanceof Lz0Error && e.code === ERROR_CODES.LENGTH_OVERFLOW
  );

  // Invalid offset > op
  const badOff = new Uint8Array([0b1111_0000, 0x41, 250, 0b0000_1000]);
  assert.throws(() => decode(badOff, new Uint8Array(32)), (e) => e instanceof Lz0Error && e.code === ERROR_CODES.INVALID_OFFSET);

  // Output too small
  const mixedRaw = new Uint8Array(readFileSync(join(VECTORS, "mixed.lz0raw")));
  assert.throws(() => decode(mixedRaw, new Uint8Array(4)), (e) => e instanceof Lz0Error && e.code === ERROR_CODES.OUTPUT_TOO_SMALL);

  // Container checks
  assert.throws(() => decodeContainer(new Uint8Array([0x4c, 0x5a, 0x30, 0x32, 0, 0, 0, 0]), new Uint8Array(0)), (e) => e.code === ERROR_CODES.INVALID_CONTAINER);
  assert.throws(() => decodeContainer(new Uint8Array([0x4c, 0x5a, 0x30, 0x31, 0, 0, 0, 0, 1]), new Uint8Array(0)), (e) => e.code === ERROR_CODES.INVALID_CONTAINER);
});

test("long overlap periods (>= 32 bytes geometric doubling & offset=1 fill)", () => {
  // Hand-craft Literal(17 '0..G') + MatchNew(off=17, len=200 -> triggers geometric copyWithin) + EOF
  const lit = new Uint8Array(17);
  for (let i = 0; i < 17; i++) lit[i] = 65 + i;

  // Build stream bits cleanly
  const stream = [];
  let ctrlIdx = -1, bitPos = 0;
  const pushBit = (b) => {
    if (bitPos === 0) { ctrlIdx = stream.length; stream.push(0); }
    if (b) stream[ctrlIdx] |= 1 << (7 - bitPos);
    bitPos = (bitPos + 1) & 7;
  };
  const pushElias = (v, inv = 0) => {
    const k = 31 - Math.clz32(v);
    for (let s = k - 1; s >= 0; s--) { pushBit(0); pushBit(((v >>> s) & 1) ^ inv); }
    pushBit(1);
  };

  pushElias(17);
  for (const ch of lit) stream.push(ch);
  pushBit(1); // new loop
  pushBit(1); // MatchNew flag
  pushElias(1, 1); // offset MSB=1
  stream.push(256 - 17); // offset LSB
  pushBit(1); pushBit(1); pushBit(1);
  pushElias(199 - 7); // len = 200 -> v = 199
  pushBit(1); pushBit(0); // EOF

  const out = new Uint8Array(17 + 200);
  const n = decode(new Uint8Array(stream), out);
  assert.equal(n, 217);
  for (let i = 0; i < 217; i++) assert.equal(out[i], lit[i % 17]);
});

test("differential parity with C CLI across levels -1, -6, -9", { skip: false }, (t) => {
  let cliBuilt = true;
  try {
    readFileSync(join(REPO, "bin", "lz0"));
  } catch {
    cliBuilt = false;
  }
  if (!cliBuilt) {
    t.skip("bin/lz0 not built");
    return;
  }
  const tmp = mkdtempSync(join(tmpdir(), "lz0-js-"));
  try {
    const corpus = join(REPO, "bench", "corpus", "fields.c");
    const expected = new Uint8Array(readFileSync(corpus));
    const lz0File = join(tmp, "fields.lz0");
    for (const lvl of ["-1", "-6", "-9"]) {
      execFileSync(join(REPO, "bin", "lz0"), ["c", lvl, corpus, lz0File], { stdio: "ignore" });
      const cont = new Uint8Array(readFileSync(lz0File));
      const out = new Uint8Array(containerUncompressedSize(cont));
      assert.equal(decodeContainer(cont, out), expected.length);
      assert.deepEqual(out, expected);
    }
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
});

