/**
 * Zero-dependency, zero-allocation LZ0 v0.1 decompressor for browsers, Node, Deno, and Bun.
 * @module lz0dec
 */

export const ERROR_CODES = Object.freeze({
  INVALID_ARGUMENT: "INVALID_ARGUMENT",
  OVERLAPPING_BUFFERS: "OVERLAPPING_BUFFERS",
  EMPTY_INPUT: "EMPTY_INPUT",
  EMPTY_OUTPUT: "EMPTY_OUTPUT",
  TRUNCATED_INPUT: "TRUNCATED_INPUT",
  INVALID_OFFSET: "INVALID_OFFSET",
  OUTPUT_TOO_SMALL: "OUTPUT_TOO_SMALL",
  LENGTH_OVERFLOW: "LENGTH_OVERFLOW",
  INVALID_CONTAINER: "INVALID_CONTAINER",
});

/** Error thrown by LZ0 decoders with a machine-readable `code` property. */
export class Lz0Error extends Error {
  /**
   * @param {string} code One of `ERROR_CODES`
   * @param {string} message Human-readable diagnostic
   */
  constructor(code, message) {
    super(message);
    this.name = "Lz0Error";
    this.code = code;
  }
}

const ERR_TRUNCATED = -1;
const ERR_OVERFLOW = -2;
const ERR_OFFSET = -3;
const ERR_TOO_SMALL = -4;

function isUint8Array(v) {
  return v instanceof Uint8Array || (ArrayBuffer.isView(v) && v.constructor?.name === "Uint8Array");
}

function validateBuffers(input, output) {
  if (!isUint8Array(input) || !isUint8Array(output)) {
    throw new Lz0Error(ERROR_CODES.INVALID_ARGUMENT, "input and output must be Uint8Array instances");
  }
  if (
    input.buffer === output.buffer &&
    input.byteOffset < output.byteOffset + output.byteLength &&
    output.byteOffset < input.byteOffset + input.byteLength
  ) {
    throw new Lz0Error(ERROR_CODES.OVERLAPPING_BUFFERS, "input and output buffers must not overlap");
  }
}

function throwCode(rc) {
  if (rc === ERR_TRUNCATED) throw new Lz0Error(ERROR_CODES.TRUNCATED_INPUT, "truncated LZ0 compressed input stream");
  if (rc === ERR_OVERFLOW) throw new Lz0Error(ERROR_CODES.LENGTH_OVERFLOW, "Elias gamma integer overflow");
  if (rc === ERR_OFFSET) throw new Lz0Error(ERROR_CODES.INVALID_OFFSET, "invalid match offset outside produced output");
  throw new Lz0Error(ERROR_CODES.OUTPUT_TOO_SMALL, "decompressed data exceeds output buffer length");
}

function copyMatch(output, op, outMax, offset, len) {
  if (offset < 1 || offset > op) return ERR_OFFSET;
  if (op + len > outMax) return ERR_TOO_SMALL;
  if (offset >= len) {
    if (len >= 32) {
      output.copyWithin(op, op - offset, op - offset + len);
    } else {
      const s = op - offset;
      for (let i = 0; i < len; i++) output[op + i] = output[s + i];
    }
  } else if (offset === 1) {
    if (len >= 16) {
      output.fill(output[op - 1], op, op + len);
    } else {
      const val = output[op - 1];
      for (let i = 0; i < len; i++) output[op + i] = val;
    }
  } else if (len >= 32) {
    // Byte-accurate geometric doubling via native memmove (`copyWithin`)
    let curOff = offset;
    let done = curOff;
    output.copyWithin(op, op - curOff, op);
    while (done < len) {
      const chunk = len - done < curOff ? len - done : curOff;
      output.copyWithin(op + done, op, op + chunk);
      done += chunk;
      curOff <<= 1;
    }
  } else {
    const s = op - offset;
    for (let i = 0; i < len; i++) output[op + i] = output[s + i];
  }
  return op + len;
}

// Keep hot loop free of `throw` / `Error` allocations for optimal JIT bytecode density.
function decodeCore(input, output) {
  const inLen = input.length | 0;
  const outMax = output.length | 0;
  let ip = 0;
  let op = 0;
  // Active control byte with LSB sentinel bit: shifting left emits MSB and eventually 0 when empty.
  let bits = 0;
  let lastOffset = 1;

  for (;;) {
    // 1. LiteralRun length (uninverted Elias gamma; leading 0 decision bit is omitted)
    let litLen = 1;
    for (;;) {
      let cur = bits, sh = (cur << 1) & 0xff, b;
      if (sh === 0) {
        if (ip >= inLen) return ERR_TRUNCATED;
        const nb = input[ip++];
        bits = ((nb << 1) | 1) & 0xff;
        b = nb >>> 7;
      } else {
        bits = sh;
        b = cur >>> 7;
      }
      if (b !== 0) break;

      cur = bits;
      sh = (cur << 1) & 0xff;
      let p;
      if (sh === 0) {
        if (ip >= inLen) return ERR_TRUNCATED;
        const nb = input[ip++];
        bits = ((nb << 1) | 1) & 0xff;
        p = nb >>> 7;
      } else {
        bits = sh;
        p = cur >>> 7;
      }
      if (litLen >= 0x40000000) return ERR_OVERFLOW;
      litLen = (litLen << 1) | p;
    }

    if (op + litLen > outMax) return ERR_TOO_SMALL;
    if (ip + litLen > inLen) return ERR_TRUNCATED;
    if (litLen >= 32) {
      output.set(input.subarray(ip, ip + litLen), op);
      ip += litLen;
      op += litLen;
    } else {
      for (let i = 0; i < litLen; i++) output[op++] = input[ip++];
    }

    // Decision bit after LiteralRun: 0 => MatchLast, 1 => new-offset / EOF loop
    let cur = bits, sh = (cur << 1) & 0xff, d;
    if (sh === 0) {
      if (ip >= inLen) return ERR_TRUNCATED;
      const nb = input[ip++];
      bits = ((nb << 1) | 1) & 0xff;
      d = nb >>> 7;
    } else {
      bits = sh;
      d = cur >>> 7;
    }

    // 2. MatchLast (repeats `lastOffset`)
    if (d === 0) {
      let mLen = 1;
      for (;;) {
        cur = bits;
        sh = (cur << 1) & 0xff;
        let b;
        if (sh === 0) {
          if (ip >= inLen) return ERR_TRUNCATED;
          const nb = input[ip++];
          bits = ((nb << 1) | 1) & 0xff;
          b = nb >>> 7;
        } else {
          bits = sh;
          b = cur >>> 7;
        }
        if (b !== 0) break;

        cur = bits;
        sh = (cur << 1) & 0xff;
        let p;
        if (sh === 0) {
          if (ip >= inLen) return ERR_TRUNCATED;
          const nb = input[ip++];
          bits = ((nb << 1) | 1) & 0xff;
          p = nb >>> 7;
        } else {
          bits = sh;
          p = cur >>> 7;
        }
        if (mLen >= 0x40000000) return ERR_OVERFLOW;
        mLen = (mLen << 1) | p;
      }

      op = copyMatch(output, op, outMax, lastOffset, mLen);
      if (op < 0) return op;

      cur = bits;
      sh = (cur << 1) & 0xff;
      if (sh === 0) {
        if (ip >= inLen) return ERR_TRUNCATED;
        const nb = input[ip++];
        bits = ((nb << 1) | 1) & 0xff;
        d = nb >>> 7;
      } else {
        bits = sh;
        d = cur >>> 7;
      }
      if (d === 0) continue;
    }

    // 3. New-offset / EOF loop
    for (;;) {
      cur = bits;
      sh = (cur << 1) & 0xff;
      let flag;
      if (sh === 0) {
        if (ip >= inLen) return ERR_TRUNCATED;
        const nb = input[ip++];
        bits = ((nb << 1) | 1) & 0xff;
        flag = nb >>> 7;
      } else {
        bits = sh;
        flag = cur >>> 7;
      }
      if (flag === 0) return op;

      let msb = 1;
      for (;;) {
        cur = bits;
        sh = (cur << 1) & 0xff;
        let b;
        if (sh === 0) {
          if (ip >= inLen) return ERR_TRUNCATED;
          const nb = input[ip++];
          bits = ((nb << 1) | 1) & 0xff;
          b = nb >>> 7;
        } else {
          bits = sh;
          b = cur >>> 7;
        }
        if (b !== 0) break;

        cur = bits;
        sh = (cur << 1) & 0xff;
        let p;
        if (sh === 0) {
          if (ip >= inLen) return ERR_TRUNCATED;
          const nb = input[ip++];
          bits = ((nb << 1) | 1) & 0xff;
          p = nb >>> 7;
        } else {
          bits = sh;
          p = cur >>> 7;
        }
        if (msb >= 0x40000000) return ERR_OVERFLOW;
        msb = (msb << 1) | (p ^ 1);
      }

      if (ip >= inLen) return ERR_TRUNCATED;
      const off = msb * 256 - input[ip++];
      if (off < 1 || off > op) return ERR_OFFSET;
      lastOffset = off;

      let x = 0;
      for (let k = 0; k < 3; k++) {
        cur = bits;
        sh = (cur << 1) & 0xff;
        let b;
        if (sh === 0) {
          if (ip >= inLen) return ERR_TRUNCATED;
          const nb = input[ip++];
          bits = ((nb << 1) | 1) & 0xff;
          b = nb >>> 7;
        } else {
          bits = sh;
          b = cur >>> 7;
        }
        x = (x << 1) | b;
      }

      let mLen;
      if (x < 7) {
        mLen = x + 2;
      } else {
        let tail = 1;
        for (;;) {
          cur = bits;
          sh = (cur << 1) & 0xff;
          let b;
          if (sh === 0) {
            if (ip >= inLen) return ERR_TRUNCATED;
            const nb = input[ip++];
            bits = ((nb << 1) | 1) & 0xff;
            b = nb >>> 7;
          } else {
            bits = sh;
            b = cur >>> 7;
          }
          if (b !== 0) break;

          cur = bits;
          sh = (cur << 1) & 0xff;
          let p;
          if (sh === 0) {
            if (ip >= inLen) return ERR_TRUNCATED;
            const nb = input[ip++];
            bits = ((nb << 1) | 1) & 0xff;
            p = nb >>> 7;
          } else {
            bits = sh;
            p = cur >>> 7;
          }
          if (tail >= 0x40000000) return ERR_OVERFLOW;
          tail = (tail << 1) | p;
        }
        mLen = 8 + tail;
      }

      op = copyMatch(output, op, outMax, lastOffset, mLen);
      if (op < 0) return op;

      cur = bits;
      sh = (cur << 1) & 0xff;
      if (sh === 0) {
        if (ip >= inLen) return ERR_TRUNCATED;
        const nb = input[ip++];
        bits = ((nb << 1) | 1) & 0xff;
        d = nb >>> 7;
      } else {
        bits = sh;
        d = cur >>> 7;
      }
      if (d === 0) break;
    }
  }
}

/**
 * Decode a raw LZ0 v0.1 compressed bitstream into a caller-supplied `Uint8Array`.
 * Uses zero internal buffer allocations and strictly validates all reads and writes.
 *
 * @param {Uint8Array} input Compressed raw bitstream (`input.length > 0`)
 * @param {Uint8Array} output Caller-owned destination buffer (`output.length > 0`)
 * @returns {number} Exact number of decompressed bytes written to `output`
 */
export function decode(input, output) {
  validateBuffers(input, output);
  if (input.length === 0) throw new Lz0Error(ERROR_CODES.EMPTY_INPUT, "raw input buffer is empty");
  if (output.length === 0) throw new Lz0Error(ERROR_CODES.EMPTY_OUTPUT, "output buffer is empty");
  const written = decodeCore(input, output);
  if (written < 0) throwCode(written);
  return written;
}

/**
 * Parse an 8-byte LZ01 container header and return its declared uncompressed size.
 *
 * @param {Uint8Array} input Buffer starting with `'LZ01'` + little-endian uint32 size
 * @returns {number} Uncompressed byte size
 */
export function containerUncompressedSize(input) {
  if (!isUint8Array(input)) {
    throw new Lz0Error(ERROR_CODES.INVALID_ARGUMENT, "input must be a Uint8Array");
  }
  if (input.length < 8 || input[0] !== 0x4c || input[1] !== 0x5a || input[2] !== 0x30 || input[3] !== 0x31) {
    throw new Lz0Error(ERROR_CODES.INVALID_CONTAINER, "invalid or truncated LZ01 container header");
  }
  return (input[4] | (input[5] << 8) | (input[6] << 16) | (input[7] << 24)) >>> 0;
}

/**
 * Decode an LZ01 container into a caller-supplied `Uint8Array`.
 * Accepts 0-byte empty containers (8-byte header with empty payload).
 *
 * @param {Uint8Array} input Full LZ01 container bytes
 * @param {Uint8Array} output Caller-owned destination buffer (`output.length >= size`)
 * @returns {number} Exact uncompressed size
 */
export function decodeContainer(input, output) {
  validateBuffers(input, output);
  const expected = containerUncompressedSize(input);
  if (expected > output.length) {
    throw new Lz0Error(ERROR_CODES.OUTPUT_TOO_SMALL, `output buffer too small for container (${expected} > ${output.length})`);
  }
  if (expected === 0) {
    if (input.length !== 8) {
      throw new Lz0Error(ERROR_CODES.INVALID_CONTAINER, "zero-size container must have empty payload");
    }
    return 0;
  }
  const written = decode(input.subarray(8), output);
  if (written !== expected) {
    throw new Lz0Error(ERROR_CODES.INVALID_CONTAINER, `container size mismatch (${written} !== ${expected})`);
  }
  return written;
}
