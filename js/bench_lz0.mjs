import { readFileSync, rmSync, mkdtempSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, dirname } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";

import { decode } from "./lz0dec.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = dirname(HERE);
const CLI = join(REPO, "bin", "lz0");

const FILES = [
  "bench/corpus/fields.c",
  "bench/corpus/cp.html",
  "bench/corpus/sum",
  "bench/corpus/lz0_win64.exe",
  "bench/corpus/star.png",
  "bench/corpus/avatar.jpg",
  "bench/corpus/break.mp3",
  "bench/corpus/alice29.txt",
  "bench/corpus/plrabn12.txt",
  "bench/corpus/kennedy.xls",
  "bench/corpus/dickens_1m.txt",
  "bench/corpus/bash_x86_64.bin",
];

function measure(payload, out, minMs = 250) {
  let iters = 0;
  const t0 = performance.now();
  for (;;) {
    for (let i = 0; i < 8; i++) {
      decode(payload, out);
      iters++;
    }
    const elapsed = performance.now() - t0;
    if (elapsed >= minMs) {
      return (out.length * iters) / (elapsed * 1000);
    }
  }
}

const tmp = mkdtempSync(join(tmpdir(), "lz0-bench-js-"));
try {
  console.log(`# Decompression Benchmark: Pure JavaScript (Node ${process.version}, zero-alloc Uint8Array)\n`);
  console.log("| File | Raw Size | Compressed (-6) | JS Sentinel (`decode`) |");
  console.log("|---|---:|---:|---:|");
  for (const rel of FILES) {
    let raw;
    try {
      raw = new Uint8Array(readFileSync(join(REPO, rel)));
    } catch {
      continue;
    }
    const name = rel.split("/").pop();
    const outLz0 = join(tmp, `${name}.lz0`);
    execFileSync(CLI, ["c", "-6", join(REPO, rel), outLz0], { stdio: "ignore" });
    const payload = new Uint8Array(readFileSync(outLz0).subarray(8));
    const out = new Uint8Array(raw.length);
    if (decode(payload, out) !== raw.length || Buffer.compare(out, raw) !== 0) {
      throw new Error(`Verification failed on ${rel}`);
    }
    const tw = performance.now();
    while (performance.now() - tw < 80) decode(payload, out);
    const mbps = measure(payload, out);
    console.log(`| \`${name}\` | ${raw.length} B | ${payload.length} B | **${mbps.toFixed(1)} MB/s** |`);
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
