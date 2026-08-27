use std::{arch::x86_64::_mm_sfence, fs::File, io};

use thiserror::Error;

use crate::{
    align::CacheLinePadded,
    instructions,
    mmap::Region,
    queue::{Index, Queue, initialize_queue},
};

pub const QUEUE_LENGTH: usize = 1 << 22;
pub type Message = u32;
pub type QueueData<T> = (CacheLinePadded<Index>, Queue<T, QUEUE_LENGTH>);

#[repr(C)]
pub struct Info {
    pub producer_count: usize,
    pub consumer_count: usize,
}

#[derive(Error, Debug)]
pub enum RegionMapError {
    #[error("mmaping Info region failed: {0}")]
    Info(io::Error),
    #[error(
        "mmaping queue for producer {producer_index} and consumer {consumer_index} failed: {error}"
    )]
    Queue {
        producer_index: usize,
        consumer_index: usize,
        #[source]
        error: io::Error,
    },
}

pub fn initialize<T: 'static>(
    file: &File,
    page_offset: u64,
    producer_count: usize,
    consumer_count: usize,
) -> Result<(), RegionMapError> {
    let mut info = unsafe { Region::<Info>::new(file, page_offset).map_err(RegionMapError::Info)? };
    info.producer_count = producer_count;
    info.consumer_count = consumer_count;

    for producer_index in 0..producer_count {
        for consumer_index in 0..consumer_count {
            let mut queue = unsafe {
                Region::<QueueData<T>>::new(
                    file,
                    compute_page_offset::<T>(&info, producer_index, consumer_index) + page_offset,
                )
                .map_err(|error| RegionMapError::Queue {
                    producer_index,
                    consumer_index,
                    error,
                })?
            };
            initialize_queue(queue.1.as_mut());
            let data = CacheLinePadded::new(Index::MAX);
            unsafe {
                instructions::movdir64b(
                    &mut queue.0 as *mut _ as *mut i64,
                    &data as *const _ as *const i64,
                );
                _mm_sfence();
            };
        }
    }
    Ok(())
}

pub fn compute_page_offset<T>(info: &Info, producer_index: usize, consumer_index: usize) -> u64 {
    (info.consumer_count * producer_index + consumer_index) as u64
        * Region::<QueueData<T>>::page_count_static()
        + 1
}
