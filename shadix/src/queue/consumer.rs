use std::sync::atomic::{AtomicU32, Ordering};

use crate::{
    align::CacheLinePadded,
    backoff::Backoff,
    cxl,
    queue::{Index, Item, consumed_writer::ConsumedWriter},
};

#[derive(Debug)]
pub struct Consumer<'a, T> {
    last_index_read: AtomicU32,
    buffer: &'a [Item<T>],
    consumed_writer: ConsumedWriter<'a>,
}

impl<'a, T> Consumer<'a, T> {
    pub fn new(buffer: &'a [Item<T>], last_index_read: &'a mut CacheLinePadded<Index>) -> Self {
        Self {
            last_index_read: Index::MAX.0.into(),
            buffer,
            consumed_writer: ConsumedWriter::new(last_index_read),
        }
    }

    fn get_item(&self, index: Index) -> &Item<T> {
        &self.buffer[index.0 as usize % self.buffer.len()]
    }

    pub async fn try_consume<F: AsyncFnOnce(&Item<T>)>(&self, consume: F) -> Result<(), F>
    where
        T: Unpin,
    {
        let index = Index(self.last_index_read.fetch_add(1, Ordering::Relaxed)).incremented_by(1);
        let item = self.get_item(index);
        let mut backoff = Backoff::new();
        if cxl::load_local(&item.index) != index {
            while cxl::load(&item.index) != index {
                if self
                    .last_index_read
                    .compare_exchange(
                        index.0,
                        index.decremented_by(1).0,
                        Ordering::Relaxed,
                        Ordering::Relaxed,
                    )
                    .is_ok()
                {
                    return Err(consume);
                }

                backoff.snooze().await;
            }
        }

        item.load();
        consume(&item).await;

        self.consumed_writer.write(index).await;
        Ok(())
    }
}
