use std::{
    arch::x86_64::_mm_sfence,
    array,
    sync::atomic::{self, AtomicU32, AtomicU64, Ordering},
};

const NUM_BUCKETS: usize = 1 << 14;
const NUM_SEGMENTS: usize = 1 << 3;
const SUBMISSION_OFFSET: u32 = 3;
const UPDATE_STEP: usize = init::QUEUE_LENGTH / 16;

// DONT CHANGE
const SEGMENT_SIZE: u32 = (NUM_BUCKETS / NUM_SEGMENTS) as _;
const SEGMENTS_PER_UPDATE: u32 = UPDATE_STEP as u32 / SEGMENT_SIZE / 64;

use thread_local::ThreadLocal;

use crate::{align::CacheLinePadded, backoff::Backoff, cxl, init, instructions, queue::Index};

#[derive(Debug)]
pub struct ConsumedWriter<'a> {
    last_index_read: &'a mut CacheLinePadded<Index>,
    first_active_bucket: CacheLinePadded<AtomicU32>,
    written_tls: ThreadLocal<Written>,
    skipped_segments: CacheLinePadded<AtomicU32>,
}

impl<'a> ConsumedWriter<'a> {
    pub fn new(last_index_read: &'a mut CacheLinePadded<Index>) -> Self {
        assert_eq!(cxl::load(&**last_index_read), Index::MAX);
        Self {
            last_index_read,
            first_active_bucket: CacheLinePadded::new(AtomicU32::new(0)),
            written_tls: Default::default(),
            skipped_segments: CacheLinePadded::new(AtomicU32::new(0)),
        }
    }

    pub async fn write(&self, index: Index) {
        let bucket_id = index.0 / 64;
        let mut backoff = Backoff::new();
        while bucket_id - self.first_active_bucket.load(Ordering::Relaxed) >= NUM_BUCKETS as _ {
            backoff.snooze().await;
        }
        atomic::fence(Ordering::Acquire);
        let local_written = self.written_tls.get_or_default();
        let bucket_idx = bucket_id % NUM_BUCKETS as u32;
        let bucket = &local_written.buckets[bucket_idx as usize];
        let flag_idx = index.0 % 64;

        let bucket_val = bucket.load(Ordering::Relaxed);
        let new_bucket_val = bucket_val | (1 << flag_idx);
        bucket.store(new_bucket_val, Ordering::Relaxed);
        if flag_idx == 63 {
            self.finish_bucket(bucket_id).await;
        }
    }

    async fn finish_bucket(&self, block_id: u32) {
        if block_id % SEGMENT_SIZE == SEGMENT_SIZE - 1 {
            self.finish_segment(block_id / SEGMENT_SIZE).await;
        }
    }

    async fn finish_segment(&self, segment_id: u32) {
        if segment_id < SUBMISSION_OFFSET
            && self.skipped_segments.load(Ordering::Acquire) < SUBMISSION_OFFSET
        {
            self.skipped_segments.fetch_add(1, Ordering::Release);
            return;
        }

        self.process_segment(segment_id.wrapping_sub(SUBMISSION_OFFSET))
            .await;
    }

    async fn process_segment(&self, segment_id: u32) {
        let mut backoff = Backoff::new();
        while !self.is_segment_completed(segment_id) {
            backoff.snooze().await;
        }
        self.increment_segment(segment_id).await;
    }

    fn is_segment_completed(&self, segment_id: u32) -> bool {
        let first_bucket_idx = first_bucket_idx(segment_id);
        let last_bucket_idx = last_bucket_idx(segment_id);
        let mut aggregated_segment = [0u64; SEGMENT_SIZE as _];
        self.written_tls.iter().for_each(|written| {
            written.buckets[first_bucket_idx as _..=last_bucket_idx as _]
                .iter()
                .enumerate()
                .for_each(|(idx, bucket)| aggregated_segment[idx] |= bucket.load(Ordering::Relaxed))
        });
        aggregated_segment.iter().all(|&value| value == u64::MAX)
    }

    async fn increment_segment(&self, segment_id: u32) {
        self.clear_segment(segment_id);
        let mut backoff = Backoff::new();
        while self.first_active_bucket.load(Ordering::Relaxed) / SEGMENT_SIZE != segment_id {
            backoff.snooze().await;
        }
        if is_update_segment(segment_id) {
            let new_last_index_read =
                (segment_id.wrapping_add(1) * SEGMENT_SIZE * 64).wrapping_sub(1);
            unsafe { self.write_to_cxl(Index(new_last_index_read)) };
        }
        self.first_active_bucket.store(
            first_bucket_id(segment_id.wrapping_add(1)),
            Ordering::Release,
        );
    }

    fn clear_segment(&self, segment_id: u32) {
        let first_bucket_idx = first_bucket_idx(segment_id);
        let last_bucket_idx = last_bucket_idx(segment_id);
        self.written_tls.iter().for_each(|written| {
            written.buckets[first_bucket_idx as _..=last_bucket_idx as _]
                .iter()
                .for_each(|bucket| bucket.store(0, Ordering::Release));
        });
    }

    unsafe fn write_to_cxl(&self, index: Index) {
        let data = CacheLinePadded::new(index);
        unsafe {
            instructions::movdir64b(
                (self.last_index_read as *const CacheLinePadded<Index>).cast_mut() as *mut i64,
                &data as *const _ as *const i64,
            );
            _mm_sfence();
        };
    }
}

fn first_bucket_id(segment_id: u32) -> u32 {
    segment_id * SEGMENT_SIZE
}

fn first_bucket_idx(segment_id: u32) -> u32 {
    first_bucket_id(segment_id) % NUM_BUCKETS as u32
}

fn last_bucket_id(segment_id: u32) -> u32 {
    first_bucket_id(segment_id).wrapping_add(SEGMENT_SIZE - 1)
}

fn last_bucket_idx(segment_id: u32) -> u32 {
    last_bucket_id(segment_id) % NUM_BUCKETS as u32
}

fn is_update_segment(segment_id: u32) -> bool {
    segment_id % SEGMENTS_PER_UPDATE == SEGMENTS_PER_UPDATE - 1
}

#[derive(Debug)]
struct Written {
    pub buckets: [AtomicU64; NUM_BUCKETS],
}

impl Default for Written {
    fn default() -> Self {
        Self {
            buckets: array::from_fn(|_| AtomicU64::new(0)),
        }
    }
}
