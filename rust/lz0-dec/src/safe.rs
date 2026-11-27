use crate::DecodeError;

struct BitReader<'a> {
    src: &'a [u8],
    pos: usize,
    mask: u8,
    val: u8,
}

impl<'a> BitReader<'a> {
    #[inline]
    fn new(src: &'a [u8]) -> Self {
        Self {
            src,
            pos: 0,
            mask: 0,
            val: 0,
        }
    }

    #[inline(always)]
    fn read_byte(&mut self) -> Result<u8, DecodeError> {
        let &b = self.src.get(self.pos).ok_or(DecodeError::TruncatedInput)?;
        self.pos += 1;
        Ok(b)
    }

    #[inline(always)]
    fn read_bit(&mut self) -> Result<usize, DecodeError> {
        self.mask >>= 1;
        if self.mask == 0 {
            self.val = self.read_byte()?;
            self.mask = 128;
        }
        Ok(usize::from((self.val & self.mask) != 0))
    }

    #[inline]
    fn read_elias(&mut self, inverted: usize) -> Result<usize, DecodeError> {
        let mut value = 1usize;
        loop {
            if self.read_bit()? != 0 {
                return Ok(value);
            }
            let p = self.read_bit()?;
            let shifted = value.checked_shl(1).ok_or(DecodeError::LengthOverflow)?;
            value = shifted | (p ^ inverted);
        }
    }

    #[inline]
    fn read_len_new(&mut self) -> Result<usize, DecodeError> {
        let b2 = self.read_bit()?;
        let b1 = self.read_bit()?;
        let b0 = self.read_bit()?;
        let x = (b2 << 2) | (b1 << 1) | b0;
        if x < 7 {
            Ok(x + 2)
        } else {
            self.read_elias(0)?
                .checked_add(8)
                .ok_or(DecodeError::LengthOverflow)
        }
    }
}

#[inline]
fn copy_match(
    output: &mut [u8],
    out_pos: usize,
    offset: usize,
    len: usize,
) -> Result<usize, DecodeError> {
    if offset == 0 || offset > out_pos {
        return Err(DecodeError::InvalidOffset);
    }
    let end = out_pos
        .checked_add(len)
        .ok_or(DecodeError::OutputTooSmall)?;
    if end > output.len() {
        return Err(DecodeError::OutputTooSmall);
    }
    let src = out_pos - offset;
    if offset >= len {
        output.copy_within(src..src + len, out_pos);
    } else if offset == 1 {
        let b = output[src];
        output[out_pos..end].fill(b);
    } else if len <= 16 {
        let win = &mut output[src..end];
        for i in 0..len {
            win[offset + i] = win[i];
        }
    } else {
        // Geometric doubling keeps overlap copies O(log(len / offset)) without unsafe.
        let mut done = 0usize;
        while done < len {
            let span = (offset + done).min(len - done);
            output.copy_within(src..src + span, out_pos + done);
            done += span;
        }
    }
    Ok(end)
}

/// Decompresses a raw LZ0 v0.1 stream into `output` using 100% safe Rust.
///
/// Allocates nothing and returns the number of uncompressed bytes written.
pub fn decode(input: &[u8], output: &mut [u8]) -> Result<usize, DecodeError> {
    if input.is_empty() {
        return Err(DecodeError::EmptyInput);
    }
    if output.is_empty() {
        return Err(DecodeError::EmptyOutput);
    }

    let mut br = BitReader::new(input);
    let mut last_offset = 1usize;
    let mut out_pos = 0usize;

    loop {
        let lit_len = br.read_elias(0)?;
        let out_end = out_pos
            .checked_add(lit_len)
            .ok_or(DecodeError::OutputTooSmall)?;
        if out_end > output.len() {
            return Err(DecodeError::OutputTooSmall);
        }
        let in_end = br
            .pos
            .checked_add(lit_len)
            .ok_or(DecodeError::TruncatedInput)?;
        let lit = br
            .src
            .get(br.pos..in_end)
            .ok_or(DecodeError::TruncatedInput)?;
        output[out_pos..out_end].copy_from_slice(lit);
        br.pos = in_end;
        out_pos = out_end;

        if br.read_bit()? == 0 {
            let len = br.read_elias(0)?;
            out_pos = copy_match(output, out_pos, last_offset, len)?;
            if br.read_bit()? == 0 {
                continue;
            }
        }

        loop {
            if br.read_bit()? == 0 {
                return Ok(out_pos);
            }
            let msb = br.read_elias(1)?;
            let t = br.read_byte()? as usize;
            let scaled = msb.checked_shl(8).ok_or(DecodeError::InvalidOffset)?;
            let offset = scaled.checked_sub(t).ok_or(DecodeError::InvalidOffset)?;
            if offset == 0 || offset > out_pos {
                return Err(DecodeError::InvalidOffset);
            }
            last_offset = offset;
            let len = br.read_len_new()?;
            out_pos = copy_match(output, out_pos, last_offset, len)?;
            if br.read_bit()? == 0 {
                break;
            }
        }
    }
}
