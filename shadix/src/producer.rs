use std::fs::File;

use itertools::Itertools;
use rand::{
    SeedableRng,
    distr::{Distribution, Uniform},
    rngs::SmallRng,
};

use crate::{
    init::{Info, QueueData, RegionMapError, compute_page_offset},
    mmap::Region,
    queue::{Item, producer::Producer},
};

pub struct Regions<T: 'static>(Vec<&'static mut Region<QueueData<T>>>);
pub struct Producers<T: 'static>(Vec<Producer<'static, T>>);

impl<T: 'static> Regions<T> {
    pub fn new(
        file: &File,
        producer_index: usize,
        page_offset: u64,
    ) -> Result<Self, RegionMapError> {
        let info = unsafe { Region::<Info>::new(file, page_offset).map_err(RegionMapError::Info)? };

        Ok(Regions(
            (0..info.consumer_count)
                .map(|consumer_index| {
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

    pub fn into_producers(self) -> Producers<T> {
        Producers(
            self.0
                .into_iter()
                .map(|region| Producer::new(unsafe { &mut *(&mut region.1 as *mut _) }, &region.0))
                .collect(),
        )
    }
}

impl<T: 'static> Producers<T> {
    pub async fn try_produce(&self, data: &Item<T>) -> Result<(), ()> {
        let dist = Uniform::new(0, self.0.len()).unwrap();
        let mut rng = SmallRng::from_os_rng();
        self.0[dist.sample(&mut rng)].try_produce(data).await
    }
}
