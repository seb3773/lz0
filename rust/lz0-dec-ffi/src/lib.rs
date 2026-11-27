use core::ffi::c_int;
use core::slice;
use lz0_dec::DecodeError;

unsafe fn call_decoder(
    inp: *const u8,
    in_size: c_int,
    out: *mut u8,
    out_max: c_int,
    f: fn(&[u8], &mut [u8]) -> Result<usize, DecodeError>,
) -> c_int {
    if inp.is_null() || out.is_null() || in_size <= 0 || out_max <= 0 {
        return -1;
    }
    let in_slice = slice::from_raw_parts(inp, in_size as usize);
    let out_slice = slice::from_raw_parts_mut(out, out_max as usize);
    match f(in_slice, out_slice) {
        Ok(n) if n <= c_int::MAX as usize => n as c_int,
        _ => -1,
    }
}

/// C-ABI wrapper for [`lz0_dec::decode`].
///
/// # Safety
/// If `inp` is non-null and `in_size > 0`, `inp` must be valid for reads of
/// `in_size` bytes. If `out` is non-null and `out_max > 0`, `out` must be
/// valid for writes of `out_max` bytes and must not alias `inp`.
#[no_mangle]
pub unsafe extern "C" fn lz0dec_rust_safe(
    inp: *const u8,
    in_size: c_int,
    out: *mut u8,
    out_max: c_int,
) -> c_int {
    call_decoder(inp, in_size, out, out_max, lz0_dec::decode)
}

/// C-ABI wrapper for [`lz0_dec::fast::decode`].
///
/// # Safety
/// If `inp` is non-null and `in_size > 0`, `inp` must be valid for reads of
/// `in_size` bytes. If `out` is non-null and `out_max > 0`, `out` must be
/// valid for writes of `out_max` bytes and must not alias `inp`.
#[no_mangle]
pub unsafe extern "C" fn lz0dec_rust_fast(
    inp: *const u8,
    in_size: c_int,
    out: *mut u8,
    out_max: c_int,
) -> c_int {
    call_decoder(inp, in_size, out, out_max, lz0_dec::fast::decode)
}
