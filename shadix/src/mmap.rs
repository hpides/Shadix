use memmap2::{MmapAsRawDesc, MmapMut, MmapOptions};
use std::io;
use std::marker::PhantomData;
use std::ops::{Deref, DerefMut};

use crate::cxl;

/// DAX devices require 2MB alignment for both offset and size
pub const DAX_ALIGNMENT: usize = 2 * 1024 * 1024; // 2 MiB

pub struct Region<T>(MmapMut, PhantomData<T>);

impl<T> Deref for Region<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { &*(self.0.as_ptr() as *const T) }
    }
}

impl<T> DerefMut for Region<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut *(self.0.as_mut_ptr() as *mut T) }
    }
}

impl<T> Drop for Region<T> {
    fn drop(&mut self) {
        cxl::invalidate(self.deref());
    }
}

impl<T: Sized> Region<T> {
    /// Creates a new memory-mapped region.
    ///
    /// `page_offset` is in units of DAX_ALIGNMENT (2MB), not 4KB pages.
    pub unsafe fn new(descriptor: impl MmapAsRawDesc, page_offset: u64) -> io::Result<Self> {
        assert!(DAX_ALIGNMENT >= align_of::<T>());
        // DAX devices require 2MB-aligned offset and size
        let len = size_of::<T>().next_multiple_of(DAX_ALIGNMENT);
        let offset = page_offset * DAX_ALIGNMENT as u64;
        let mut mmap_options = MmapOptions::new();
        mmap_options.len(len).offset(offset);

        Ok(Self(
            unsafe { mmap_options.map_mut(descriptor)? },
            PhantomData,
        ))
    }

    pub unsafe fn new_static(
        descriptor: impl MmapAsRawDesc,
        page_offset: u64,
    ) -> io::Result<&'static mut Self> {
        Ok(Box::leak(Box::new(unsafe {
            Self::new(descriptor, page_offset)
        }?)))
    }

    /// Returns the number of DAX alignment units (2MB blocks) this region occupies
    pub fn page_count(self: &Self) -> u64 {
        size_of::<T>().div_ceil(DAX_ALIGNMENT) as _
    }

    /// Returns the number of DAX alignment units (2MB blocks) this region type occupies
    pub fn page_count_static() -> u64 {
        size_of::<T>().div_ceil(DAX_ALIGNMENT) as _
    }
}
