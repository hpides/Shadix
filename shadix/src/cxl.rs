use std::{
    arch::x86_64::_mm_mfence,
    sync::atomic::{Ordering, compiler_fence},
};

use crate::instructions::clflushopt;

pub fn load_local<T>(ptr: *const T) -> T {
    unsafe { ptr.read_volatile() }
}

pub fn load<T>(ptr: *const T) -> T {
    invalidate(ptr);
    load_local(ptr)
}

pub fn invalidate<T>(ptr: *const T) {
    unsafe {
        clflush(ptr);
        compiler_fence(Ordering::SeqCst);
        _mm_mfence();
        compiler_fence(Ordering::SeqCst);
    }
}

const CACHE_LINE_SIZE: usize = 64;

unsafe fn clflush<T>(ptr: *const T) {
    let start = ptr as usize;
    let end = start + std::mem::size_of::<T>();
    let mut addr = start & !(CACHE_LINE_SIZE - 1);
    while addr < end {
        unsafe {
            clflushopt(addr as _);
        }
        addr += CACHE_LINE_SIZE;
    }
}
