use std::fs::File;

use itertools::Itertools;
use rand::{
    SeedableRng,
    distr::{Distribution, Uniform},
    rngs::SmallRng,
};

use crate::{
    align::CacheLinePadded,
    init::{Info, QueueData, RegionMapError, compute_page_offset},
    mmap::Region,
    queue::{Index, Item, consumer::Consumer},
};

pub struct Regions<T: 'static>(Vec<&'static mut Region<QueueData<T>>>);
pub struct Consumers<T: 'static>(Vec<Consumer<'static, T>>);

impl<T: 'static> Regions<T> {
    pub fn new(
        file: &File,
        consumer_index: usize,
        page_offset: u64,
    ) -> Result<Self, RegionMapError> {
        let info = unsafe { Region::<Info>::new(file, page_offset).map_err(RegionMapError::Info)? };

        Ok(Regions(
            (0..info.producer_count)
                .map(|producer_index| {
                    unsafe {
                        Region::new_static(
                            file,
                            page_offset
                                + compute_page_offset::<T>(&info, producer_index, consumer_index),
                        )
                    }
                    .map_err(|error| RegionMapError::Queue {
                        producer_index,
                        consumer_index,
                        error,
                    })
                })
                .try_collect()?,
        ))
    }

    pub fn into_consumers(self) -> Consumers<T>
    where
        T: Unpin,
    {
        Consumers(
            self.0
                .into_iter()
                .map(|region| {
                    Consumer::new(&region.1, unsafe {
                        (&region.0 as *const CacheLinePadded<Index>)
                            .cast_mut()
                            .as_mut()
                            .unwrap()
                    })
                })
                .collect(),
        )
    }
}

impl<T: 'static> Consumers<T> {
    pub async fn try_consume(
        &self,
        consume: impl AsyncFnOnce(&Item<T>),
    ) -> Result<(), impl AsyncFnOnce(&Item<T>)>
    where
        T: Unpin,
    {
        let dist = Uniform::new(0, self.0.len()).unwrap();
        let mut rng = SmallRng::from_os_rng();
        self.0[dist.sample(&mut rng)].try_consume(consume).await
    }
}
