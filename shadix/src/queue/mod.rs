pub mod consumed_writer;
pub mod consumer;
pub mod producer;

use core::slice;
use std::{
    arch::x86_64::{
        _mm_mfence, _mm_sfence, _mm_stream_si32,
        _mm_stream_si64,
    },
    ops::Sub,
    sync::atomic::{Ordering, compiler_fence},
};

use crate::instructions::{clflushopt, movdir64b};

use std::time::Instant;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct Index(u32);

impl Index {
    pub const MAX: Index = Index(u32::MAX);
    pub const MIN: Index = Index(u32::MIN);

    pub const fn new(value: u32) -> Self {
        Index(value)
    }

    pub fn increment_by(&mut self, n: u32) {
        *self = self.incremented_by(n);
    }

    pub fn incremented_by(&self, n: u32) -> Index {
        Index(self.0.wrapping_add(n))
    }

    pub fn decremented_by(&self, n: u32) -> Index {
        Index(self.0.wrapping_sub(n))
    }
}

impl Sub for Index {
    type Output = u32;

    fn sub(self, other: Index) -> Self::Output {
        self.0.wrapping_sub(other.0)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(C, align(64))]
pub struct Item<T> {
    index: Index,
    data: T,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(C, align(64))]
pub struct ItemHead {
    index: Index,
}

impl<T> Item<T> {
    pub fn new(data: T) -> Self {
        Self {
            index: Index::MIN,
            data,
        }
    }

    pub(crate) fn load(&self) {
        compiler_fence(Ordering::SeqCst);
        let view = unsafe { self.raw_view() };
        // Invalidate all cache lines except the first one (which contains the index)
        if size_of::<Self>() > 64 {
            for cl in &view[1..] {
                unsafe { clflushopt(cl.as_ptr() as *const u8) };
            }
            compiler_fence(Ordering::SeqCst);
            unsafe { _mm_mfence() };
            compiler_fence(Ordering::SeqCst);
        }
    }

    pub fn data(&self) -> &T {
        &self.data
    }

    pub fn data_mut(&mut self) -> &mut T {
        &mut self.data
    }

    pub(crate) fn store(&mut self, data: &Item<T>, index: Index) {
        let new_view = unsafe { data.raw_view() };
        let target_view = unsafe { self.raw_view_mut() };
        if size_of::<Self>() > 64 {
            let target_without_index = &mut target_view[1..];
            for (dst, src) in target_without_index.iter_mut().zip(new_view.iter().skip(1)) {
                unsafe { movdir64b(dst.as_mut_ptr(), src.as_ptr()) };
            }
            compiler_fence(Ordering::SeqCst);
            unsafe { _mm_sfence() };
            compiler_fence(Ordering::SeqCst);
        }
        // Store first cache line (contains the index)
        let mut last_src = unsafe { (data as *const Item<T> as *const ItemHead).read() };
        last_src.index = index;
        let last_dst = &mut target_view[0];
        unsafe {
            movdir64b(
                last_dst.as_mut_ptr(),
                &last_src as *const ItemHead as *const i64,
            )
        };
    }

    unsafe fn raw_view(&self) -> &[[i64; 8]] {
        unsafe {
            slice::from_raw_parts(
                self as *const Self as *const [i64; 8],
                size_of::<Self>() / 64,
            )
        }
    }

    unsafe fn raw_view_mut(&mut self) -> &mut [[i64; 8]] {
        unsafe {
            slice::from_raw_parts_mut(self as *mut Self as *mut [i64; 8], size_of::<Self>() / 64)
        }
    }
}

pub type Queue<T, const LENGTH: usize> = [Item<T>; LENGTH];

pub fn initialize_queue<T>(buffer: &mut [Item<T>]) {
    zero_buffer(buffer);
    initialize_indices(buffer);
}

fn initialize_indices<T>(buffer: &mut [Item<T>]) {
    println!("Initializing indices...");
    let start = Instant::now();
    for item in buffer.iter_mut() {
        unsafe {
            _mm_stream_si32(&mut item.index as *mut Index as *mut i32, -1);
        };
    }
    compiler_fence(Ordering::SeqCst);
    unsafe { _mm_sfence() };
    compiler_fence(Ordering::SeqCst);
    let elapsed = start.elapsed();
    let throughput = buffer.len() as f64 / elapsed.as_secs_f64();
    println!(
        "Initialized {} indices in {:?} ({:.2} items/s)",
        buffer.len(),
        elapsed,
        throughput
    );
}

fn zero_buffer<T>(buffer: &mut [Item<T>]) {
    println!("Zeroing buffer...");
    let buf_as_i64s = unsafe {
        let (before, data, after) = buffer.align_to_mut::<i64>();
        assert!(before.is_empty());
        assert!(after.is_empty());
        data
    };
    let start = Instant::now();
    for item in buf_as_i64s.iter_mut() {
        unsafe { _mm_stream_si64(item, 0) };
    }
    compiler_fence(Ordering::SeqCst);
    unsafe { _mm_sfence() };
    compiler_fence(Ordering::SeqCst);
    let elapsed = start.elapsed();
    let throughput = buf_as_i64s.len() as f64 / elapsed.as_secs_f64();
    println!(
        "Zeroed {} i64s in {:?} ({:.2} items/s)",
        buf_as_i64s.len(),
        elapsed,
        throughput
    );
}
