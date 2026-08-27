use std::arch::asm;

/// Performs a 64-byte direct store using the `movdir64b` instruction.
///
/// # Safety
/// - `dst` must be aligned to 64 bytes
/// - `src` must be valid for reads of 64 bytes
#[cfg(not(feature = "no-movdir64b"))]
#[inline(always)]
pub unsafe fn movdir64b(dst: *mut i64, src: *const i64) {
    unsafe {
        asm!(
            "movdir64b {dst}, [{src}]",
            dst = in(reg) dst,
            src = in(reg) src,
            options(nostack, preserves_flags)
        );
    }
}

/// Fallback implementation for systems without `movdir64b` support.
///
/// # Safety
/// - `dst` must be aligned to 64 bytes
/// - `src` must be valid for reads of 64 bytes
#[cfg(feature = "no-movdir64b")]
#[inline(always)]
pub unsafe fn movdir64b(dst: *mut i64, src: *const i64) {
    use std::arch::x86_64::_mm_sfence;
    unsafe {
        std::ptr::copy_nonoverlapping(src, dst, 8); // 8 * i64 = 64 bytes
        _mm_sfence();
    }
}

/// Flushes a cache line using the `clflushopt` instruction.
///
/// # Safety
/// - `addr` must be a valid address
#[inline(always)]
pub unsafe fn clflushopt(addr: *const u8) {
    unsafe {
        asm!(
            "clflushopt [{addr}]",
            addr = in(reg) addr,
            options(nostack, preserves_flags)
        );
    }
}
