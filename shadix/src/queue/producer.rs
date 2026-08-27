use std::sync::atomic::{AtomicU32, Ordering};

use crate::{
    backoff::Backoff,
    cxl,
    queue::{Index, Item},
};

pub struct Producer<'a, T> {
    last_index_written: AtomicU32,
    last_index_read: &'a Index,
    local_last_index_read: AtomicU32,
    buffer: &'a mut [Item<T>],
}

impl<'a, T> Producer<'a, T> {
    pub fn new(buffer: &'a mut [Item<T>], last_index_read: &'a Index) -> Self {
        Self {
            last_index_written: Index::MAX.0.into(),
            last_index_read,
            local_last_index_read: AtomicU32::new(cxl::load(last_index_read).0),
            buffer,
        }
    }

    fn get_item(&self, index: Index) -> &Item<T> {
        &self.buffer[index.0 as usize % self.buffer.len()]
    }

    unsafe fn get_item_mut(&self, index: Index) -> &mut Item<T> {
        let ptr = (self.get_item(index) as *const Item<T>).cast_mut();

        unsafe { ptr.as_mut() }.unwrap()
    }

    pub async fn try_produce(&self, data: &Item<T>) -> Result<(), ()> {
        let last_index_written = Index(self.last_index_written.fetch_add(1, Ordering::Relaxed));

        if last_index_written - Index(self.local_last_index_read.load(Ordering::Relaxed))
            >= self.buffer.len() as u32
        {
            self.local_last_index_read
                .store(cxl::load(self.last_index_read).0, Ordering::Relaxed);

            let mut backoff = Backoff::new();
            while last_index_written - Index(self.local_last_index_read.load(Ordering::Relaxed))
                >= self.buffer.len() as u32
            {
                if self
                    .last_index_written
                    .compare_exchange(
                        last_index_written.incremented_by(1).0,
                        last_index_written.0,
                        Ordering::Relaxed,
                        Ordering::Relaxed,
                    )
                    .is_ok()
                {
                    return Err(());
                }
                backoff.snooze().await;
            }
        }

        let next_index = last_index_written.incremented_by(1);
        let item = unsafe { self.get_item_mut(next_index) };
        item.store(data, next_index);

        Ok(())
    }
}
