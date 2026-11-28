#!/usr/bin/env python3
"""Educational zero-dependency reference decoder and stream inspector for LZ0 v0.1."""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

MAGIC, DEFAULT_MAX_OUT, _MAX_ELIAS = b"LZ01", 64 * 1024 * 1024, 1 << 54


@dataclass(frozen=True)
class Token:
    kind: str
    length: int = 0
    offset: int = 0
    bits: int = 0
    src_pos: int = 0
    ctrl_pos: int = -1
    bit_idx: int = 0


class Lz0Error(ValueError):
    def __init__(self, msg: str, src_pos: int = 0, ctrl_pos: int = -1, bit_idx: int = 0, out_pos: int = 0) -> None:
        loc = f"src byte {src_pos}" + (f", ctrl byte {ctrl_pos} bit {bit_idx} (MSB=0)" if ctrl_pos >= 0 else "")
        super().__init__(f"{msg} ({loc}, out byte {out_pos})")
        self.msg, self.src_pos, self.ctrl_pos, self.bit_idx, self.out_pos = msg, src_pos, ctrl_pos, bit_idx, out_pos


class BitReader:
    """MSB-first LZ0 bitreader keeping the active control byte intact across whole-byte reads."""

    def __init__(self, data: bytes | bytearray | memoryview) -> None:
        self.data, self.pos, self.ctrl_pos, self.ctrl_byte, self.mask, self.bits_read = memoryview(data), 0, -1, 0, 0, 0

    def next_bit_loc(self) -> tuple[int, int]:
        return (self.ctrl_pos, 8 - (self.mask >> 1).bit_length()) if self.mask > 1 else (self.pos, 0)

    def fail(self, msg: str, out_pos: int) -> None:
        raise Lz0Error(msg, self.pos, self.ctrl_pos, 0 if self.mask == 0 else 8 - self.mask.bit_length(), out_pos)

    def read_byte(self, out_pos: int, what: str = "byte") -> int:
        if self.pos >= len(self.data):
            self.fail(f"truncated {what}", out_pos)
        self.pos += 1
        return self.data[self.pos - 1]

    def read_bit(self, out_pos: int, what: str = "control bit") -> int:
        self.mask >>= 1
        if self.mask == 0:
            self.ctrl_pos, self.ctrl_byte, self.mask = self.pos, self.read_byte(out_pos, what), 128
        self.bits_read += 1
        return 1 if (self.ctrl_byte & self.mask) else 0

    def read_elias(self, out_pos: int, inverted: int = 0, what: str = "Elias gamma") -> int:
        value = 1
        while not self.read_bit(out_pos, what):
            p = self.read_bit(out_pos, what)
            if value >= _MAX_ELIAS:
                self.fail(f"Elias overflow reading {what}", out_pos)
            value = (value << 1) | (p ^ inverted)
        return value

    def read_len_new(self, out_pos: int) -> int:
        x = (self.read_bit(out_pos, "MatchNew length") << 2) | (self.read_bit(out_pos, "MatchNew length") << 1) | self.read_bit(out_pos, "MatchNew length")
        return x + 1 if x < 7 else 7 + self.read_elias(out_pos, 0, "MatchNew length tail")


def _copy_match(out: bytearray, offset: int, length: int, r: BitReader, out_max: int | None) -> None:
    out_pos = len(out)
    if offset < 1 or offset > out_pos:
        r.fail(f"invalid match offset {offset} (out_pos={out_pos})", out_pos)
    if out_max is not None and out_pos + length > out_max:
        r.fail(f"match length {length} exceeds out_max ({out_pos + length} > {out_max})", out_pos)
    for _ in range(length):  # byte-by-byte overlap copy (offset < length repeats period)
        out.append(out[-offset])


def decode_raw(data: bytes | bytearray | memoryview, out_max: int | None = None, on_token: Callable[[Token], None] | None = None) -> bytes:
    """Decode a raw LZ0 v0.1 bitstream (first LiteralRun's decision bit 0 is omitted)."""
    if not data or (out_max is not None and out_max <= 0):
        raise Lz0Error("empty raw input" if not data else "non-positive out_max")
    r, out, last_offset, emit = BitReader(data), bytearray(), 1, on_token or (lambda _t: None)
    b0, s0, c0, bi0 = 0, 0, 0, 0
    mark = lambda: (r.bits_read, r.pos, *r.next_bit_loc())

    while True:
        length = r.read_elias(len(out), 0, "LiteralRun length")
        if out_max is not None and len(out) + length > out_max:
            r.fail(f"LiteralRun length {length} exceeds out_max ({len(out) + length} > {out_max})", len(out))
        if r.pos + length > len(r.data):
            r.fail(f"truncated LiteralRun payload (need {length}, have {len(r.data) - r.pos})", len(out))
        out.extend(r.data[r.pos : r.pos + length])
        r.pos += length
        emit(Token("LiteralRun", length, 0, r.bits_read - b0, s0, c0, bi0))

        b0, s0, c0, bi0 = mark()
        in_new = r.read_bit(len(out), "LiteralRun terminator") == 1
        if not in_new:
            length = r.read_elias(len(out), 0, "MatchLast length")
            _copy_match(out, last_offset, length, r, out_max)
            emit(Token("MatchLast", length, last_offset, r.bits_read - b0, s0, c0, bi0))
            b0, s0, c0, bi0 = mark()
            in_new = r.read_bit(len(out), "MatchLast terminator") == 1

        while in_new:
            if not r.read_bit(len(out), "EOF / MatchNew flag"):
                emit(Token("EOF", 0, 0, r.bits_read - b0, s0, c0, bi0))
                return bytes(out)
            last_offset = r.read_elias(len(out), 1, "offset MSB") * 256 - r.read_byte(len(out), "offset LSB")
            if last_offset < 1 or last_offset > len(out):
                r.fail(f"invalid match offset {last_offset} (out_pos={len(out)})", len(out))
            length = r.read_len_new(len(out)) + 1
            _copy_match(out, last_offset, length, r, out_max)
            emit(Token("MatchNew", length, last_offset, r.bits_read - b0, s0, c0, bi0))
            b0, s0, c0, bi0 = mark()
            in_new = r.read_bit(len(out), "MatchNew terminator") == 1


def decode_container(data: bytes | bytearray | memoryview, out_max: int | None = None, on_token: Callable[[Token], None] | None = None) -> bytes:
    """Decode an LZ01 container ('LZ01' + uint32 LE uncompressed size + raw payload)."""
    if len(data) < 8 or bytes(data[:4]) != MAGIC:
        raise Lz0Error("invalid LZ01 container header")
    expected = struct.unpack_from("<I", data, 4)[0]
    if out_max is not None and expected > out_max:
        raise Lz0Error(f"container size {expected} exceeds out_max {out_max}", src_pos=4)
    payload = memoryview(data)[8:]
    if expected == 0:
        if len(payload):
            raise Lz0Error("non-empty payload in zero-size container", src_pos=8)
        return b""
    out = decode_raw(payload, out_max=expected, on_token=on_token)
    if len(out) != expected:
        raise Lz0Error(f"container size mismatch: expected {expected}, got {len(out)}", src_pos=len(data), out_pos=len(out))
    return out


def decode(data: bytes | bytearray | memoryview, *, raw: bool | None = None, out_max: int | None = None, on_token: Callable[[Token], None] | None = None) -> bytes:
    """Decode LZ01 container or raw stream (auto-detected unless `raw` is specified)."""
    is_raw = (bytes(data[:4]) != MAGIC) if raw is None else raw
    return decode_raw(data, out_max, on_token) if is_raw else decode_container(data, out_max, on_token)


def _format_tok(t: Token, bits: bool) -> str:
    s = f"  {t.kind}" if t.kind == "EOF" else (f"  {t.kind:<11} length={t.length}" if t.kind == "LiteralRun" else f"  {t.kind:<11} offset={t.offset:<5} length={t.length}")
    return f"{s:<36} [src={t.src_pos} ctrl={t.ctrl_pos}:{t.bit_idx} bits={t.bits}]" if bits else s


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="lz0.py", description="LZ0 v0.1 reference decoder & inspector")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("decode", "inspect"):
        sp = sub.add_parser(name)
        sp.add_argument("input", type=Path)
        sp.add_argument("--raw", action="store_true")
        sp.add_argument("--container", dest="raw", action="store_false")
        sp.add_argument("--max-out", type=int, default=DEFAULT_MAX_OUT)
        sp.set_defaults(raw=None)
        if name == "decode":
            sp.add_argument("output", type=Path)
        else:
            sp.add_argument("--bits", action="store_true")
            sp.add_argument("--limit", type=int, default=None)
            sp.add_argument("--summary", action="store_true")
    args = p.parse_args(argv)
    data = args.input.read_bytes()
    if args.cmd == "decode":
        try:
            args.output.write_bytes(decode(data, raw=args.raw, out_max=args.max_out))
            return 0
        except Lz0Error as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
    toks: list[Token] = []
    err: Lz0Error | None = None
    out = b""
    try:
        out = decode(data, raw=args.raw, out_max=args.max_out, on_token=toks.append)
    except Lz0Error as exc:
        err = exc
    print(f"LZ0 stream\n----------\nInput size:  {len(data)} bytes\nOutput size: {f'{len(out)} bytes' if err is None else f'failed (out byte {err.out_pos})'}")
    if not args.summary:
        print("\nTokens:")
        for t in (toks if args.limit is None else toks[:args.limit]):
            print(_format_tok(t, args.bits))
        if args.limit is not None and len(toks) > args.limit:
            print(f"  ... ({len(toks) - args.limit} more tokens; total={len(toks)})")
    if args.summary or args.limit is not None:
        c = {k: sum(1 for t in toks if t.kind == k) for k in ("LiteralRun", "MatchNew", "MatchLast", "EOF")}
        print(f"\nSummary: {len(toks)} tokens (lit={c['LiteralRun']}/{sum(t.length for t in toks if t.kind == 'LiteralRun')}B, new={c['MatchNew']}, last={c['MatchLast']}, eof={c['EOF']}, ctrl_bits={sum(t.bits for t in toks)})")
    print("\nDecoded: OK" if err is None else f"\nDecoded: ERROR: {err}")
    return 0 if err is None else 1


if __name__ == "__main__":
    raise SystemExit(main())
