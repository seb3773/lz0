use core::ptr;

use crate::DecodeError;

#[inline(always)]
unsafe fn read_byte(ip: &mut *const u8, in_end: *const u8) -> Result<u8, DecodeError> {
    if *ip >= in_end {
        return Err(DecodeError::TruncatedInput);
    }
    let b = **ip;
    *ip = ip.add(1);
    Ok(b)
}

#[inline(always)]
unsafe fn read_bit_safe(
    bits: &mut u8,
    ip: &mut *const u8,
    in_end: *const u8,
) -> Result<usize, DecodeError> {
    let cur = *bits;
    let shifted = cur << 1;
    if shifted == 0 {
        let byte = read_byte(ip, in_end)?;
        *bits = (byte << 1) | 1;
        Ok((byte >> 7) as usize)
    } else {
        *bits = shifted;
        Ok((cur >> 7) as usize)
    }
}

#[inline(always)]
unsafe fn read_elias_safe(
    bits: &mut u8,
    ip: &mut *const u8,
    in_end: *const u8,
    inverted: usize,
) -> Result<usize, DecodeError> {
    let mut value = 1usize;
    loop {
        if read_bit_safe(bits, ip, in_end)? != 0 {
            return Ok(value);
        }
        let p = read_bit_safe(bits, ip, in_end)?;
        let shifted = value.checked_shl(1).ok_or(DecodeError::LengthOverflow)?;
        value = shifted | (p ^ inverted);
    }
}

#[inline(always)]
unsafe fn read_len_new_safe(
    bits: &mut u8,
    ip: &mut *const u8,
    in_end: *const u8,
) -> Result<usize, DecodeError> {
    let b2 = read_bit_safe(bits, ip, in_end)?;
    let b1 = read_bit_safe(bits, ip, in_end)?;
    let b0 = read_bit_safe(bits, ip, in_end)?;
    let x = (b2 << 2) | (b1 << 1) | b0;
    if x < 7 {
        Ok(x + 2)
    } else {
        read_elias_safe(bits, ip, in_end, 0)?
            .checked_add(8)
            .ok_or(DecodeError::LengthOverflow)
    }
}

#[inline(always)]
unsafe fn copy_match_exact(
    op: &mut *mut u8,
    out_base: *mut u8,
    out_end: *mut u8,
    offset: usize,
    len: usize,
) -> Result<(), DecodeError> {
    let produced = (*op as usize) - (out_base as usize);
    if offset == 0 || offset > produced {
        return Err(DecodeError::InvalidOffset);
    }
    let out_avail = (out_end as usize) - (*op as usize);
    if len > out_avail {
        return Err(DecodeError::OutputTooSmall);
    }

    let mut dst = *op;
    let src = dst.sub(offset);

    if offset >= len {
        ptr::copy_nonoverlapping(src, dst, len);
        *op = dst.add(len);
    } else if offset == 1 {
        ptr::write_bytes(dst, *src, len);
        *op = dst.add(len);
    } else if offset >= 8 {
        let mut rem = len;
        let mut cur_src = src;
        while rem > 0 {
            let chunk = offset.min(rem);
            ptr::copy_nonoverlapping(cur_src, dst, chunk);
            cur_src = cur_src.add(chunk);
            dst = dst.add(chunk);
            rem -= chunk;
        }
        *op = dst;
    } else {
        let mut done = 0usize;
        while done < len {
            *dst.add(done) = *src.add(done);
            done += 1;
        }
        *op = dst.add(len);
    }
    Ok(())
}

/// Fast-engine segment state matching `lz0dec_fast`:
/// - `ip` points to the next byte whose bits have not yet been partially consumed.
/// - `q` in `0..=7`: bits already consumed from current partial byte (`q = 7` means empty).
/// - `leftover`: remaining `7 - q` bits left-aligned at bits `63..57`.
#[inline(always)]
unsafe fn load_window(ip: *const u8, leftover: u64, q: u32) -> u64 {
    let rem = q ^ 7;
    let raw = u64::from_be(ip.cast::<u64>().read_unaligned());
    leftover | (raw >> rem)
}

#[inline(always)]
unsafe fn commit_window(ip: &mut *const u8, q: &mut u32, leftover: &mut u64, window: u64) {
    let adv = (*q >> 3) as usize;
    *ip = ip.add(adv);
    let q_new = *q & 7;
    *q = q_new;
    *leftover = if q_new == 7 {
        0
    } else {
        window & (!0u64 << (57 + q_new))
    };
}

#[inline(always)]
fn to_safe_bits(leftover: u64, q: u32) -> u8 {
    debug_assert!(q < 8);
    ((leftover >> 56) as u8) | (1u8 << q)
}

#[inline(always)]
unsafe fn ensure_window(
    ip: &mut *const u8,
    in_end: *const u8,
    q: &mut u32,
    leftover: &mut u64,
    window: &mut u64,
) -> bool {
    if *q <= 28 {
        return true;
    }
    commit_window(ip, q, leftover, *window);
    if (in_end as usize) - (*ip as usize) < 8 {
        return false;
    }
    *window = load_window(*ip, *leftover, *q);
    true
}

#[inline(always)]
fn elias_fast<const INV: bool>(window: &mut u64, q: &mut u32) -> Option<usize> {
    let w = *window;
    if (w as i64) < 0 {
        *window = w << 1;
        *q += 1;
        return Some(1);
    }
    let ctrl = w & 0xAAAA_AAAA_AAAA_AAAA_u64;
    let lz = ctrl.leading_zeros();
    if lz == 2 {
        let p = ((w >> 62) & 1) as usize;
        *window = w << 3;
        *q += 3;
        return Some(2 | if INV { p ^ 1 } else { p });
    }
    if lz > 30 {
        return None;
    }
    let mut v = ((w >> (64 - lz)) as u32) & 0x5555_5555;
    v = (v | (v >> 1)) & 0x3333_3333;
    v = (v | (v >> 2)) & 0x0F0F_0F0F;
    v = (v | (v >> 4)) & 0x00FF_00FF;
    v = (v | (v >> 8)) & 0x0000_FFFF;
    let n = lz >> 1;
    let payload = if INV {
        (v as usize) ^ ((1usize << n) - 1)
    } else {
        v as usize
    };
    let consumed = lz + 1;
    *window = w << consumed;
    *q += consumed;
    Some((1usize << n) | payload)
}

#[inline(always)]
unsafe fn copy_match_fast(
    op: &mut *mut u8,
    out_base: *mut u8,
    out_fast_end: *mut u8,
    out_end: *mut u8,
    offset: usize,
    len: usize,
) -> Result<(), DecodeError> {
    let produced = (*op as usize) - (out_base as usize);
    if offset == 0 || offset > produced {
        return Err(DecodeError::InvalidOffset);
    }
    let dst = *op;
    let end = (dst as usize).saturating_add(len);
    if dst <= out_fast_end && end <= out_fast_end as usize {
        let src = dst.sub(offset);
        if offset >= 16 {
            dst.cast::<u128>()
                .write_unaligned(src.cast::<u128>().read_unaligned());
            if len > 16 {
                let mut cur = 16usize;
                while cur < len {
                    dst.add(cur)
                        .cast::<u128>()
                        .write_unaligned(src.add(cur).cast::<u128>().read_unaligned());
                    cur += 16;
                }
            }
            *op = dst.add(len);
            return Ok(());
        }
        if offset >= 8 && len <= 8 {
            dst.cast::<u64>()
                .write_unaligned(src.cast::<u64>().read_unaligned());
            *op = dst.add(len);
            return Ok(());
        }
        if offset == 1 {
            let b = *src as u128;
            let splat = b.wrapping_mul(0x0101_0101_0101_0101_0101_0101_0101_0101_u128);
            dst.cast::<u128>().write_unaligned(splat);
            if len <= 16 {
                *op = dst.add(len);
                return Ok(());
            }
            if len <= 64 {
                let mut cur = 16usize;
                while cur < len {
                    dst.add(cur).cast::<u128>().write_unaligned(splat);
                    cur += 16;
                }
                *op = dst.add(len);
                return Ok(());
            }
        }
    }
    copy_match_exact(op, out_base, out_end, offset, len)
}

#[derive(Clone, Copy)]
enum SafeEntry {
    Literal,
    AfterLiteral,
    MatchLastBody,
    NewLoop,
    MatchNewBody,
    MatchNewLen { prefix_111_read: bool },
}

/// Decompresses a raw LZ0 v0.1 stream into `output` using a segment-window
/// bitreader, branch-free SWAR Elias decoding, and 16-byte unaligned copies
/// in the interior of buffers, falling back to exact tail checks at boundaries.
pub fn decode(input: &[u8], output: &mut [u8]) -> Result<usize, DecodeError> {
    if input.is_empty() {
        return Err(DecodeError::EmptyInput);
    }
    if output.is_empty() {
        return Err(DecodeError::EmptyOutput);
    }

    let mut ip = input.as_ptr();
    let mut op = output.as_mut_ptr();
    let out_base = op;
    let in_end = unsafe { ip.add(input.len()) };
    let out_end = unsafe { op.add(output.len()) };

    let in_fast_end = if input.len() >= 16 {
        unsafe { in_end.sub(16) }
    } else {
        ip
    };
    let out_fast_end = if output.len() >= 16 {
        unsafe { out_end.sub(16) }
    } else {
        op
    };

    let mut last_offset = 1usize;
    let mut safe_bits: u8 = 0x80;
    let mut safe_entry = SafeEntry::Literal;

    unsafe {
        if input.len() >= 16 && output.len() >= 16 {
            let mut q: u32 = 7;
            let mut leftover: u64 = 0;
            let mut window = load_window(ip, leftover, q);

            'fast: loop {
                if !ensure_window(&mut ip, in_end, &mut q, &mut leftover, &mut window) {
                    safe_bits = to_safe_bits(leftover, q);
                    safe_entry = SafeEntry::Literal;
                    break 'fast;
                }
                let lit_len = match elias_fast::<false>(&mut window, &mut q) {
                    Some(v) => v,
                    None => {
                        commit_window(&mut ip, &mut q, &mut leftover, window);
                        safe_bits = to_safe_bits(leftover, q);
                        safe_entry = SafeEntry::Literal;
                        break 'fast;
                    }
                };
                commit_window(&mut ip, &mut q, &mut leftover, window);

                let in_avail = (in_end as usize) - (ip as usize);
                let out_avail = (out_end as usize) - (op as usize);
                if lit_len > in_avail {
                    return Err(DecodeError::TruncatedInput);
                }
                if lit_len > out_avail {
                    return Err(DecodeError::OutputTooSmall);
                }
                if lit_len == 1 {
                    *op = *ip;
                } else if lit_len <= 16 && in_avail >= 16 && out_avail >= 16 {
                    op.cast::<u128>()
                        .write_unaligned(ip.cast::<u128>().read_unaligned());
                } else {
                    ptr::copy_nonoverlapping(ip, op, lit_len);
                }
                ip = ip.add(lit_len);
                op = op.add(lit_len);

                if ip > in_fast_end || op > out_fast_end {
                    safe_bits = to_safe_bits(leftover, q);
                    safe_entry = SafeEntry::AfterLiteral;
                    break 'fast;
                }
                window = load_window(ip, leftover, q);

                let dec = (window as i64) < 0;
                window <<= 1;
                q += 1;

                if !dec {
                    if !ensure_window(&mut ip, in_end, &mut q, &mut leftover, &mut window) {
                        safe_bits = to_safe_bits(leftover, q);
                        safe_entry = SafeEntry::MatchLastBody;
                        break 'fast;
                    }
                    let len = match elias_fast::<false>(&mut window, &mut q) {
                        Some(v) => v,
                        None => {
                            commit_window(&mut ip, &mut q, &mut leftover, window);
                            safe_bits = to_safe_bits(leftover, q);
                            safe_entry = SafeEntry::MatchLastBody;
                            break 'fast;
                        }
                    };
                    if let Err(e) =
                        copy_match_fast(&mut op, out_base, out_fast_end, out_end, last_offset, len)
                    {
                        commit_window(&mut ip, &mut q, &mut leftover, window);
                        return Err(e);
                    }
                    let dec2 = (window as i64) < 0;
                    window <<= 1;
                    q += 1;
                    if !dec2 {
                        if ip <= in_fast_end && op <= out_fast_end {
                            continue 'fast;
                        }
                        commit_window(&mut ip, &mut q, &mut leftover, window);
                        safe_bits = to_safe_bits(leftover, q);
                        safe_entry = SafeEntry::Literal;
                        break 'fast;
                    }
                }

                // NewLoop
                loop {
                    if !ensure_window(&mut ip, in_end, &mut q, &mut leftover, &mut window) {
                        safe_bits = to_safe_bits(leftover, q);
                        safe_entry = SafeEntry::NewLoop;
                        break 'fast;
                    }
                    let is_new = (window as i64) < 0;
                    window <<= 1;
                    q += 1;
                    if !is_new {
                        return Ok((op as usize) - (out_base as usize));
                    }

                    let msb = match elias_fast::<true>(&mut window, &mut q) {
                        Some(v) => v,
                        None => {
                            commit_window(&mut ip, &mut q, &mut leftover, window);
                            safe_bits = to_safe_bits(leftover, q);
                            safe_entry = SafeEntry::MatchNewBody;
                            break 'fast;
                        }
                    };
                    commit_window(&mut ip, &mut q, &mut leftover, window);
                    if ip >= in_end {
                        return Err(DecodeError::TruncatedInput);
                    }
                    let t = *ip as usize;
                    ip = ip.add(1);
                    let scaled = msb.checked_shl(8).ok_or(DecodeError::InvalidOffset)?;
                    let offset = scaled.checked_sub(t).ok_or(DecodeError::InvalidOffset)?;
                    if offset == 0 || offset > (op as usize) - (out_base as usize) {
                        return Err(DecodeError::InvalidOffset);
                    }
                    last_offset = offset;

                    if (in_end as usize) - (ip as usize) < 8 {
                        safe_bits = to_safe_bits(leftover, q);
                        safe_entry = SafeEntry::MatchNewLen {
                            prefix_111_read: false,
                        };
                        break 'fast;
                    }
                    window = load_window(ip, leftover, q);
                    let x = (window >> 61) as usize;
                    window <<= 3;
                    q += 3;
                    let len = if x < 7 {
                        x + 2
                    } else {
                        match elias_fast::<false>(&mut window, &mut q) {
                            Some(v) => v + 8,
                            None => {
                                commit_window(&mut ip, &mut q, &mut leftover, window);
                                safe_bits = to_safe_bits(leftover, q);
                                safe_entry = SafeEntry::MatchNewLen {
                                    prefix_111_read: true,
                                };
                                break 'fast;
                            }
                        }
                    };
                    if let Err(e) =
                        copy_match_fast(&mut op, out_base, out_fast_end, out_end, last_offset, len)
                    {
                        commit_window(&mut ip, &mut q, &mut leftover, window);
                        return Err(e);
                    }
                    let stay = (window as i64) < 0;
                    window <<= 1;
                    q += 1;
                    if !stay {
                        break;
                    }
                }
                if ip > in_fast_end || op > out_fast_end {
                    commit_window(&mut ip, &mut q, &mut leftover, window);
                    safe_bits = to_safe_bits(leftover, q);
                    safe_entry = SafeEntry::Literal;
                    break 'fast;
                }
            }
        }

        // Exact checked safe tail
        'tail: loop {
            match safe_entry {
                SafeEntry::Literal => {
                    let lit_len = read_elias_safe(&mut safe_bits, &mut ip, in_end, 0)?;
                    let in_avail = (in_end as usize) - (ip as usize);
                    let out_avail = (out_end as usize) - (op as usize);
                    if lit_len > in_avail {
                        return Err(DecodeError::TruncatedInput);
                    }
                    if lit_len > out_avail {
                        return Err(DecodeError::OutputTooSmall);
                    }
                    ptr::copy_nonoverlapping(ip, op, lit_len);
                    ip = ip.add(lit_len);
                    op = op.add(lit_len);
                    safe_entry = SafeEntry::AfterLiteral;
                }
                SafeEntry::AfterLiteral => {
                    if read_bit_safe(&mut safe_bits, &mut ip, in_end)? == 0 {
                        safe_entry = SafeEntry::MatchLastBody;
                    } else {
                        safe_entry = SafeEntry::NewLoop;
                    }
                }
                SafeEntry::MatchLastBody => {
                    let len = read_elias_safe(&mut safe_bits, &mut ip, in_end, 0)?;
                    copy_match_exact(&mut op, out_base, out_end, last_offset, len)?;
                    if read_bit_safe(&mut safe_bits, &mut ip, in_end)? == 0 {
                        safe_entry = SafeEntry::Literal;
                    } else {
                        safe_entry = SafeEntry::NewLoop;
                    }
                }
                SafeEntry::NewLoop => {
                    if read_bit_safe(&mut safe_bits, &mut ip, in_end)? == 0 {
                        return Ok((op as usize) - (out_base as usize));
                    }
                    safe_entry = SafeEntry::MatchNewBody;
                }
                SafeEntry::MatchNewBody => {
                    let msb = read_elias_safe(&mut safe_bits, &mut ip, in_end, 1)?;
                    let t = read_byte(&mut ip, in_end)? as usize;
                    let scaled = msb.checked_shl(8).ok_or(DecodeError::InvalidOffset)?;
                    let offset = scaled.checked_sub(t).ok_or(DecodeError::InvalidOffset)?;
                    if offset == 0 || offset > (op as usize) - (out_base as usize) {
                        return Err(DecodeError::InvalidOffset);
                    }
                    last_offset = offset;
                    safe_entry = SafeEntry::MatchNewLen {
                        prefix_111_read: false,
                    };
                }
                SafeEntry::MatchNewLen { prefix_111_read } => {
                    let len = if prefix_111_read {
                        read_elias_safe(&mut safe_bits, &mut ip, in_end, 0)?
                            .checked_add(8)
                            .ok_or(DecodeError::LengthOverflow)?
                    } else {
                        read_len_new_safe(&mut safe_bits, &mut ip, in_end)?
                    };
                    copy_match_exact(&mut op, out_base, out_end, last_offset, len)?;
                    if read_bit_safe(&mut safe_bits, &mut ip, in_end)? == 0 {
                        safe_entry = SafeEntry::Literal;
                        continue 'tail;
                    }
                    safe_entry = SafeEntry::NewLoop;
                }
            }
        }
    }
}
