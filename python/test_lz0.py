#!/usr/bin/env python3
"""Unit and differential tests for python/lz0.py against golden vectors and malformed inputs."""

from __future__ import annotations

import contextlib
import io
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import lz0

REPO_ROOT = Path(__file__).resolve().parent.parent
VECTORS_DIR = REPO_ROOT / "tests" / "vectors"


def _pack_bits(bits: list[int], whole_bytes: dict[int, bytes]) -> bytes:
    """Minimal helper to synthesize raw LZ0 streams in tests."""
    out = bytearray()
    ctrl_idx = -1
    bit_pos = 0
    for i, bit in enumerate(bits):
        if bit_pos == 0:
            ctrl_idx = len(out)
            out.append(0)
        if bit:
            out[ctrl_idx] |= 1 << (7 - bit_pos)
        bit_pos = (bit_pos + 1) & 7
        if i in whole_bytes:
            out.extend(whole_bytes[i])
    return bytes(out)


def _elias_bits(v: int, inverted: int = 0) -> list[int]:
    k = v.bit_length() - 1
    bits: list[int] = []
    for shift in range(k - 1, -1, -1):
        bits.append(0)
        bits.append(((v >> shift) & 1) ^ inverted)
    bits.append(1)
    return bits


class Lz0PythonTests(unittest.TestCase):
    def test_golden_vectors_container_and_raw(self) -> None:
        cases = ["onebyte", "literals", "overlap", "mixed", "matchlast", "matchnew", "eof_after_lit"]
        for name in cases:
            expected = (VECTORS_DIR / f"{name}.bin").read_bytes()
            cont = (VECTORS_DIR / f"{name}.lz0").read_bytes()
            raw = (VECTORS_DIR / f"{name}.lz0raw").read_bytes()

            tokens_c: list[lz0.Token] = []
            tokens_r: list[lz0.Token] = []
            self.assertEqual(lz0.decode(cont, on_token=tokens_c.append), expected)
            self.assertEqual(lz0.decode_raw(raw, on_token=tokens_r.append), expected)
            self.assertEqual(
                [(t.kind, t.length, t.offset, t.bits) for t in tokens_c],
                [(t.kind, t.length, t.offset, t.bits) for t in tokens_r],
            )
            self.assertEqual(tokens_r[0].kind, "LiteralRun")
            self.assertEqual(tokens_r[-1].kind, "EOF")

    def test_empty_container_vs_empty_raw(self) -> None:
        empty_cont = (VECTORS_DIR / "empty.lz0").read_bytes()
        empty_raw = (VECTORS_DIR / "empty.lz0raw").read_bytes()
        self.assertEqual(len(empty_cont), 8)
        self.assertEqual(len(empty_raw), 0)
        self.assertEqual(lz0.decode(empty_cont), b"")
        with self.assertRaises(lz0.Lz0Error):
            lz0.decode_raw(empty_raw)

    def test_trailing_bytes_after_eof_allowed_in_raw(self) -> None:
        raw = (VECTORS_DIR / "mixed.lz0raw").read_bytes()
        expected = (VECTORS_DIR / "mixed.bin").read_bytes()
        self.assertEqual(lz0.decode_raw(raw + b"\xde\xad\xbe\xef"), expected)

    def test_bit_byte_interleaving_on_matchnew_boundary(self) -> None:
        # 'AB' literal + MatchNew(offset=2, len=10 -> repeats 'AB' 5 times) + EOF
        # LiteralRun(2): elias(2) ('A','B'), decision 1, new_flag 1, inv_elias(1)=1, lsb(254)=0xfe,
        # len_new(v=9=L-1 -> 111 + elias(9-7=2)), decision 1, eof 0.
        bits = _elias_bits(2)  # index 2 -> last bit of LiteralRun len
        after_lit_idx = len(bits) - 1
        bits += [1, 1] + _elias_bits(1, inverted=1)
        lsb_idx = len(bits) - 1
        bits += [1, 1, 1] + _elias_bits(2) + [1, 0]
        stream = _pack_bits(bits, {after_lit_idx: b"AB", lsb_idx: bytes([254])})
        tokens: list[lz0.Token] = []
        decoded = lz0.decode_raw(stream, on_token=tokens.append)
        self.assertEqual(decoded, b"AB" * 6)
        self.assertEqual([(t.kind, t.length, t.offset) for t in tokens], [
            ("LiteralRun", 2, 0),
            ("MatchNew", 10, 2),
            ("EOF", 0, 0),
        ])

    def test_all_prefixes_truncated_rejected(self) -> None:
        for name in ("literals", "mixed", "matchnew", "overlap"):
            raw = (VECTORS_DIR / f"{name}.lz0raw").read_bytes()
            for trunc in range(len(raw)):
                with self.assertRaises(lz0.Lz0Error):
                    lz0.decode_raw(raw[:trunc])

    def test_malformed_and_error_coordinates(self) -> None:
        # Container errors
        with self.assertRaises(lz0.Lz0Error):
            lz0.decode_container(b"BAD!" + struct.pack("<I", 0))
        with self.assertRaises(lz0.Lz0Error):
            lz0.decode_container(b"LZ01" + struct.pack("<I", 0) + b"\x01")
        mixed_raw = (VECTORS_DIR / "mixed.lz0raw").read_bytes()
        with self.assertRaises(lz0.Lz0Error):
            lz0.decode_container(b"LZ01" + struct.pack("<I", 999) + mixed_raw)

        # Zero flood triggers O(1) Elias overflow error cleanly
        with self.assertRaises(lz0.Lz0Error) as ctx:
            lz0.decode_raw(b"\x00" * 64)
        self.assertIn("overflow", ctx.exception.msg.lower())
        self.assertGreaterEqual(ctx.exception.ctrl_pos, 0)

        # Invalid offset (> out_pos): Literal(1)='A', decision=1, flag=1, msb=1, byte=250 -> off=6 > out_pos(1)
        bad_stream = bytearray([0b1111_0000, 0x41, 250, 0b0000_1000])
        with self.assertRaises(lz0.Lz0Error) as err_off:
            lz0.decode_raw(bad_stream)
        self.assertIn("offset", err_off.exception.msg.lower())
        self.assertGreaterEqual(err_off.exception.ctrl_pos, 0)

        # out_max overflow
        with self.assertRaises(lz0.Lz0Error):
            lz0.decode_raw(mixed_raw, out_max=4)

    def test_cli_decode_and_inspect(self) -> None:
        sample = VECTORS_DIR / "mixed.lz0"
        expected = (VECTORS_DIR / "mixed.bin").read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "out.bin"
            self.assertEqual(lz0.main(["decode", str(sample), str(out_path)]), 0)
            self.assertEqual(out_path.read_bytes(), expected)

            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = lz0.main(["inspect", "--bits", str(sample)])
            self.assertEqual(rc, 0)
            text = buf.getvalue()
            self.assertIn("LiteralRun", text)
            self.assertIn("EOF", text)
            self.assertIn("Decoded: OK", text)
            self.assertIn("ctrl=", text)

            # Truncated raw inspect fails with localized error
            bad_path = Path(tmp) / "bad.lz0raw"
            bad_path.write_bytes((VECTORS_DIR / "mixed.lz0raw").read_bytes()[:-1])
            buf_err = io.StringIO()
            with contextlib.redirect_stdout(buf_err):
                rc_err = lz0.main(["inspect", "--raw", str(bad_path)])
            self.assertEqual(rc_err, 1)
            self.assertIn("Decoded: ERROR:", buf_err.getvalue())

    def test_parity_with_c_cli_if_present(self) -> None:
        cli_bin = REPO_ROOT / "bin" / "lz0"
        if not shutil.which(str(cli_bin)):
            self.skipTest("bin/lz0 not built")
        corpus_file = REPO_ROOT / "bench" / "corpus" / "fields.c"
        raw_bytes = corpus_file.read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            lz0_path = Path(tmp) / "fields.lz0"
            for level in ("-1", "-6", "-9"):
                subprocess.run([str(cli_bin), "c", level, str(corpus_file), str(lz0_path)], check=True, stdout=subprocess.DEVNULL)
                self.assertEqual(lz0.decode(lz0_path.read_bytes()), raw_bytes)


if __name__ == "__main__":
    unittest.main()
