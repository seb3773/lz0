use lz0_dec::{container_uncompressed_size, decode, decode_container, DecodeError};

fn read_repo_file(rel: &str) -> Vec<u8> {
    let manifest_dir = env!("CARGO_MANIFEST_DIR");
    let path = format!("{manifest_dir}/../../{rel}");
    std::fs::read(&path).unwrap_or_else(|e| panic!("failed to read {path}: {e}"))
}

fn check_both(raw: &[u8], expected: &[u8]) {
    let mut out = vec![0xAAu8; expected.len().max(1)];
    let n = decode(raw, &mut out[..expected.len()]).expect("safe decode failed");
    assert_eq!(n, expected.len());
    assert_eq!(&out[..n], expected);

    #[cfg(feature = "fast")]
    {
        let mut out_fast = vec![0x55u8; expected.len().max(1)];
        let nf = lz0_dec::fast::decode(raw, &mut out_fast[..expected.len()])
            .expect("fast decode failed");
        assert_eq!(nf, expected.len());
        assert_eq!(&out_fast[..nf], expected);
    }
}

fn check_both_err(raw: &[u8], out_cap: usize, expected_err: DecodeError) {
    let mut out = vec![0u8; out_cap];
    let res = decode(raw, &mut out);
    assert_eq!(res, Err(expected_err));

    #[cfg(feature = "fast")]
    {
        let mut out_fast = vec![0u8; out_cap];
        let res_f = lz0_dec::fast::decode(raw, &mut out_fast);
        assert_eq!(res_f, Err(expected_err));
    }
}

#[test]
fn test_vectors_and_containers() {
    let empty_bin = read_repo_file("tests/vectors/empty.bin");
    let empty_cont = read_repo_file("tests/vectors/empty.lz0");
    assert!(empty_bin.is_empty());
    assert_eq!(container_uncompressed_size(&empty_cont), Some(0));
    let mut dummy = [];
    assert_eq!(decode_container(&empty_cont, &mut dummy), Ok(0));

    let vecs = [
        "onebyte",
        "literals",
        "overlap",
        "mixed",
        "matchlast",
        "matchnew",
        "eof_after_lit",
    ];
    for name in vecs {
        let orig = read_repo_file(&format!("tests/vectors/{name}.bin"));
        let raw = read_repo_file(&format!("tests/vectors/{name}.lz0raw"));
        let cont = read_repo_file(&format!("tests/vectors/{name}.lz0"));

        check_both(&raw, &orig);

        assert_eq!(container_uncompressed_size(&cont), Some(orig.len()));
        let mut out = vec![0u8; orig.len() + 8];
        let n = decode_container(&cont, &mut out).expect("decode_container failed");
        assert_eq!(n, orig.len());
        assert_eq!(&out[..n], &orig[..]);
    }
}

#[test]
fn test_malformed_and_bounds() {
    check_both_err(&[], 8, DecodeError::EmptyInput);
    check_both_err(&[0x80, b'a', 0x40], 0, DecodeError::EmptyOutput);
    check_both_err(&[0], 8, DecodeError::TruncatedInput);
    check_both_err(&[0x00], 16, DecodeError::TruncatedInput);
    check_both_err(&[0x80, b'x'], 16, DecodeError::TruncatedInput);
    check_both_err(&[0u8; 8], 16, DecodeError::TruncatedInput);

    // Valid stream: 1 literal 'a' then MatchLast(len=3) => "aaaa"
    // Verify truncation and undersized output rejections.
    let raw_overlap = [0x80 | 0x20, b'a', 0x40];
    let mut check_tmp = [0u8; 4];
    // Hand-encode: L=1 ('1' = 1), lit 'a', dec=0 (MatchLast), L=3 ('0 1 1'), dec=1, eof=0 => 1_0_011_100 = 0x9C?
    // Wait: let's encode cleanly via bits: [1][0][0][1][1][1][0][0] = 0x9C, literal 'a' after byte 0?
    // Remember control byte 0 = 0x9C: first bit '1' -> L=1 literal, reads byte 1 ('a'), then remaining bits of 0x9C:
    // bit6=0 (MatchLast), bit5=0,bit4=1,bit3=1 (L=3), bit2=1 (new_loop), bit1=0 (EOF), bit0=0 (unused).
    let _ = (&raw_overlap, &mut check_tmp);
    let stream_aaaa = [0x9Cu8, b'a'];
    check_both(&stream_aaaa, b"aaaa");
    check_both_err(&stream_aaaa[..1], 4, DecodeError::TruncatedInput);
    check_both_err(&stream_aaaa, 3, DecodeError::OutputTooSmall);

    // Invalid offset: MatchNew trying offset > out_pos (after 1 literal 'x').
    // Bits: 1 (lit=1), 1 (new_loop), 1 (MatchNew), 1 (msb=1), then t=254 => off=2 > out_pos=1.
    // 1_1_1_1_0000 = 0xF0
    let bad_off_stream = [0xF0u8, b'x', 254u8];
    check_both_err(&bad_off_stream, 16, DecodeError::InvalidOffset);

    // Container edge cases
    let mut bad_out = [0u8; 4];
    assert_eq!(
        decode_container(b"LZ00\x00\x00\x00\x00", &mut bad_out),
        Err(DecodeError::InvalidContainer)
    );
    assert_eq!(
        decode_container(b"LZ01\x00\x00\x00\x00extra", &mut bad_out),
        Err(DecodeError::InvalidContainer)
    );
    assert_eq!(
        decode_container(b"LZ01\x10\x00\x00\x00", &mut bad_out),
        Err(DecodeError::OutputTooSmall)
    );
}

#[test]
fn test_elias_overflow_no_panic() {
    // 64 consecutive zero bytes triggers overflowing Elias without panicking in debug mode.
    let zeros = [0u8; 64];
    let mut out = [0u8; 64];
    let err = decode(&zeros, &mut out).unwrap_err();
    assert!(matches!(
        err,
        DecodeError::LengthOverflow | DecodeError::TruncatedInput
    ));
    #[cfg(feature = "fast")]
    {
        let err_f = lz0_dec::fast::decode(&zeros, &mut out).unwrap_err();
        assert!(matches!(
            err_f,
            DecodeError::LengthOverflow | DecodeError::TruncatedInput
        ));
    }
}

#[test]
fn test_deterministic_fuzz_never_panics() {
    let mut rng: u32 = 0xCAFE_BABEu32;
    let mut next_u32 = || -> u32 {
        rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
        rng
    };

    let mut out_safe = [0u8; 1024];
    #[cfg(feature = "fast")]
    let mut out_fast = [0u8; 1024];

    for _ in 0..10_000 {
        let len = 1 + (next_u32() as usize % 64);
        let mut raw = [0u8; 64];
        for b in &mut raw[..len] {
            *b = (next_u32() >> 24) as u8;
        }
        let cap = 1 + (next_u32() as usize % 512);
        let r_safe = decode(&raw[..len], &mut out_safe[..cap]);
        #[cfg(feature = "fast")]
        {
            let r_fast = lz0_dec::fast::decode(&raw[..len], &mut out_fast[..cap]);
            match (r_safe, r_fast) {
                (Ok(ns), Ok(nf)) => {
                    assert_eq!(ns, nf);
                    assert_eq!(&out_safe[..ns], &out_fast[..nf]);
                }
                (Err(_), Err(_)) => {}
                (s, f) => panic!("safe/fast divergence: safe={s:?} fast={f:?}"),
            }
        }
        #[cfg(not(feature = "fast"))]
        let _ = r_safe;
    }
}

#[test]
fn test_long_overlap_periods() {
    // Craft streams exercising offset == 1, offset == 3 (period 3), offset == 8, and offset == 11
    // with long match lengths (> 16) to exercise geometric doubling and fast RLE paths.
    // MatchLast helper with encoded bits:
    // Helper builds valid LZ0 raw stream: Literal(lit) + MatchLast(len) or MatchNew(off, len).
    struct BitWriter {
        buf: Vec<u8>,
        ctrl_idx: usize,
        mask: u8,
    }
    impl BitWriter {
        fn new() -> Self {
            Self {
                buf: Vec::new(),
                ctrl_idx: 0,
                mask: 0,
            }
        }
        fn put_bit(&mut self, b: u8) {
            if self.mask == 0 {
                self.ctrl_idx = self.buf.len();
                self.buf.push(0);
                self.mask = 128;
            }
            if b != 0 {
                self.buf[self.ctrl_idx] |= self.mask;
            }
            self.mask >>= 1;
        }
        fn put_elias(&mut self, v: usize, inv: usize) {
            let bits = usize::BITS - v.leading_zeros();
            for i in (0..bits - 1).rev() {
                self.put_bit(0);
                let p = ((v >> i) & 1) ^ inv;
                self.put_bit(p as u8);
            }
            self.put_bit(1);
        }
        fn put_len_new(&mut self, len: usize) {
            let v = len - 1;
            if v <= 7 {
                let x = v - 1;
                self.put_bit(((x >> 2) & 1) as u8);
                self.put_bit(((x >> 1) & 1) as u8);
                self.put_bit((x & 1) as u8);
            } else {
                self.put_bit(1);
                self.put_bit(1);
                self.put_bit(1);
                self.put_elias(v - 7, 0);
            }
        }
    }

    for &off in &[1usize, 2, 3, 5, 7, 8, 9, 13, 16, 20, 31, 300] {
        for &mlen in &[2usize, 3, 8, 17, 65, 300] {
            let mut expected = Vec::with_capacity(off + mlen);
            for i in 0..off {
                expected.push((i.wrapping_mul(37).wrapping_add(11)) as u8);
            }
            for i in 0..mlen {
                expected.push(expected[i]);
            }

            let mut bw = BitWriter::new();
            bw.put_elias(off, 0);
            bw.buf.extend_from_slice(&expected[..off]);
            bw.put_bit(1); // enter new_loop
            bw.put_bit(1); // MatchNew
            let msb = ((off - 1) >> 8) + 1;
            let t = 255 - ((off - 1) & 255);
            bw.put_elias(msb, 1);
            bw.buf.push(t as u8);
            bw.put_len_new(mlen);
            bw.put_bit(1); // stay in loop
            bw.put_bit(0); // EOF

            check_both(&bw.buf, &expected);
        }
    }
}
