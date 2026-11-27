#![no_std]
#![cfg_attr(not(feature = "fast"), forbid(unsafe_code))]

use core::fmt;

mod container;
#[forbid(unsafe_code)]
mod safe;

#[cfg(feature = "fast")]
pub mod fast;

pub use container::{container_uncompressed_size, decode_container};
pub use safe::decode;

/// Error returned when decoding an LZ0 v0.1 raw stream or LZ01 container.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    /// Input slice is empty, or container size metadata disagrees with payload.
    EmptyInput,
    /// Output buffer is empty (`output.is_empty()`).
    EmptyOutput,
    /// Input ended before the stream EOF marker, literal bytes, or offset LSB.
    TruncatedInput,
    /// Decoded match offset is zero or exceeds the bytes already produced.
    InvalidOffset,
    /// Decoded output exceeds `output.len()`.
    OutputTooSmall,
    /// Elias gamma integer overflowed target address width.
    LengthOverflow,
    /// Container magic or size metadata is invalid.
    InvalidContainer,
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let msg = match self {
            Self::EmptyInput => "input stream is empty",
            Self::EmptyOutput => "output buffer is empty",
            Self::TruncatedInput => "truncated LZ0 input stream",
            Self::InvalidOffset => "invalid match offset",
            Self::OutputTooSmall => "output buffer too small",
            Self::LengthOverflow => "Elias length overflow",
            Self::InvalidContainer => "invalid LZ01 container header",
        };
        f.write_str(msg)
    }
}

impl core::error::Error for DecodeError {}
