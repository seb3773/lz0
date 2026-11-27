use crate::{decode, DecodeError};

const MAGIC: [u8; 4] = *b"LZ01";

/// Parses the 8-byte `"LZ01"` container header and returns the uncompressed size
/// if the header is present and fits in `usize`.
pub fn container_uncompressed_size(input: &[u8]) -> Option<usize> {
    if input.len() < 8 || input[0..4] != MAGIC {
        return None;
    }
    let size = u32::from_le_bytes([input[4], input[5], input[6], input[7]]);
    usize::try_from(size).ok()
}

/// Decompresses an `"LZ01"` container stream (8-byte header + raw payload) into
/// `output` without dynamic allocation.
///
/// Note: Empty files (`uncompressed_size == 0`, empty payload) are valid at the
/// container layer and return `Ok(0)` even if `output` is empty.
pub fn decode_container(input: &[u8], output: &mut [u8]) -> Result<usize, DecodeError> {
    let expected = container_uncompressed_size(input).ok_or(DecodeError::InvalidContainer)?;
    let payload = &input[8..];
    if expected == 0 {
        if !payload.is_empty() {
            return Err(DecodeError::InvalidContainer);
        }
        return Ok(0);
    }
    if output.len() < expected {
        return Err(DecodeError::OutputTooSmall);
    }
    let written = decode(payload, &mut output[..expected])?;
    if written != expected {
        return Err(DecodeError::InvalidContainer);
    }
    Ok(written)
}
