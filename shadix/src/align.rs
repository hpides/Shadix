use std::ops::{Deref, DerefMut};

#[repr(align(64))]
#[derive(Default, Debug)]
pub struct CacheLinePadded<T>(T);

impl<T> From<T> for CacheLinePadded<T> {
    fn from(value: T) -> Self {
        Self(value)
    }
}

impl<T> CacheLinePadded<T> {
    pub fn new(_0: T) -> Self {
        Self(_0)
    }
}

impl<T> DerefMut for CacheLinePadded<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl<T> Deref for CacheLinePadded<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
