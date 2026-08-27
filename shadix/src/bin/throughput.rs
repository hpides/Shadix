use std::fs::File;
use std::path::PathBuf;
use std::ptr;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, bail};
use clap::{Parser, Subcommand};
use shadix::backoff::Backoff;
use shadix::consumer::Regions as ConsumerRegions;
use shadix::init::{Info, QueueData, compute_page_offset, initialize};
use shadix::mmap::Region;
use shadix::open::open;
use shadix::producer::Regions as ProducerRegions;
use shadix::queue::{Index, Item};
use shadix::runtime::build_pinned_runtime;
use futures::future::join_all;
use tokio::time::sleep;

/// Get current time in nanoseconds using CLOCK_MONOTONIC_RAW
#[inline]
fn monotonic_nanos() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC_RAW, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

/// Dispatch to the appropriate impl function based on item size.
macro_rules! dispatch_by_item_size {
    ($item_size:expr, $func:ident($($arg:expr),* $(,)?)) => {
        match $item_size {
            1 => $func::<1>($($arg),*).await,
            8 => $func::<8>($($arg),*).await,
            16 => $func::<16>($($arg),*).await,
            32 => $func::<32>($($arg),*).await,
            64 => $func::<64>($($arg),*).await,
            128 => $func::<128>($($arg),*).await,
            256 => $func::<256>($($arg),*).await,
            512 => $func::<512>($($arg),*).await,
            1024 => $func::<1024>($($arg),*).await,
            2048 => $func::<2048>($($arg),*).await,
            4096 => $func::<4096>($($arg),*).await,
            8192 => $func::<8192>($($arg),*).await,
            16384 => $func::<16384>($($arg),*).await,
            _ => bail!("Unsupported item size: {}. Supported: 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384", $item_size),
        }
    };
}

/// Throughput benchmark for shared memory IPC
#[derive(Debug, Parser)]
struct Args {
    #[clap(subcommand)]
    action: Action,
}

/// Common arguments for file-based operations
#[derive(Debug, clap::Args)]
struct FileArgs {
    /// Item size in bytes (8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384)
    #[clap(long, short = 's')]
    item_size: usize,

    /// Mmap file
    #[clap(long)]
    file: PathBuf,

    /// File offset
    #[clap(long, default_value_t = 0)]
    offset: u64,
}

#[derive(Debug, Subcommand)]
enum Action {
    /// Compute a future CLOCK_MONOTONIC_RAW timestamp for synchronization
    StartTime {
        /// Offset from now (e.g., "100ms", "1s", "500us")
        #[clap(value_parser = humantime::parse_duration)]
        offset: Duration,
    },
    /// Initialize the shared memory regions
    Init {
        #[clap(flatten)]
        file_args: FileArgs,
        /// Number of producers
        #[clap(long)]
        producers: usize,
        /// Number of consumers
        #[clap(long)]
        consumers: usize,
    },
    /// Produce messages
    Produce {
        #[clap(flatten)]
        file_args: FileArgs,
        /// Instance ID (0-indexed)
        #[clap(long, short)]
        id: usize,
        /// Number of concurrent tasks
        #[clap(long, short)]
        concurrency: usize,
        /// Number of batches to send per task
        #[clap(long)]
        batches: u64,
        /// Items per batch
        #[clap(long)]
        batch_size: u64,
        /// Output CSV file for raw measurements
        #[clap(long, short)]
        output: Option<PathBuf>,
        /// Start time for synchronized measurements (Unix timestamp in nanoseconds, CLOCK_MONOTONIC_RAW)
        #[clap(long)]
        start_time: u64,
    },
    /// Consume messages
    Consume {
        #[clap(flatten)]
        file_args: FileArgs,
        /// Instance ID (0-indexed)
        #[clap(long, short)]
        id: usize,
        /// Number of concurrent tasks
        #[clap(long, short)]
        concurrency: usize,
        /// Duration to run
        #[clap(long, short, value_parser = humantime::parse_duration)]
        duration: Duration,
    },
    /// Check the state of the shared memory regions
    Check {
        #[clap(flatten)]
        file_args: FileArgs,
    },
}

pub fn main() -> anyhow::Result<()> {
    build_pinned_runtime().block_on(async_main())
}

async fn async_main() -> anyhow::Result<()> {
    let args = Args::parse();
    match args.action {
        Action::StartTime { offset } => {
            let start_time = monotonic_nanos() + offset.as_nanos() as u64;
            println!("{}", start_time);
        }
        Action::Init {
            file_args,
            producers,
            consumers,
        } => {
            let file = unsafe { open(&file_args.file).context("file open failed")? };
            dispatch_by_item_size!(
                file_args.item_size,
                init_impl(&file, file_args.offset, producers, consumers,)
            )
            .context("init failed")?;
        }
        Action::Produce {
            file_args,
            id,
            concurrency,
            batches,
            batch_size,
            output,
            start_time,
        } => {
            let file = unsafe { open(&file_args.file).context("file open failed")? };
            dispatch_by_item_size!(
                file_args.item_size,
                produce_impl(
                    &file,
                    file_args.offset,
                    id,
                    concurrency,
                    batches,
                    batch_size,
                    output,
                    start_time,
                )
            )
            .context("produce failed")?;
        }
        Action::Consume {
            file_args,
            id,
            concurrency,
            duration,
        } => {
            let file = unsafe { open(&file_args.file).context("file open failed")? };
            dispatch_by_item_size!(
                file_args.item_size,
                consume_impl(&file, file_args.offset, id, concurrency, duration)
            )
            .context("consume failed")?;
        }
        Action::Check { file_args } => {
            let file = unsafe { open(&file_args.file).context("file open failed")? };
            dispatch_by_item_size!(file_args.item_size, check_impl(&file, file_args.offset,))
                .context("check failed")?;
        }
    };

    Ok(())
}

/// Message type with padding to reach the desired size
#[repr(C)]
#[derive(Clone, Copy)]
struct Message<const N: usize> {
    _padding: [u8; N],
}

impl<const N: usize> Default for Message<N> {
    fn default() -> Self {
        Self { _padding: [0u8; N] }
    }
}

// ============================================================================
// Init
// ============================================================================

async fn init_impl<const N: usize>(
    file: &File,
    offset: u64,
    producers: usize,
    consumers: usize,
) -> anyhow::Result<()> {
    initialize::<Message<N>>(file, offset, producers, consumers)?;
    println!("Initialized queues: {producers} producers -> {consumers} consumers");
    Ok(())
}

// ============================================================================
// Produce
// ============================================================================

/// A single batch measurement
#[derive(serde::Serialize)]
struct BatchMeasurement {
    task_id: usize,
    batch_id: u64,
    completion_timestamp_ns: u64,
}

/// Wait precisely until the target time. Returns the current time after waiting.
async fn wait_until(target_ns: u64) -> u64 {
    let mut now = monotonic_nanos();
    if target_ns <= now {
        return now;
    }

    let remaining_ns = target_ns - now;

    // Coarse wait with tokio sleep until ~10ms before target
    if remaining_ns > 10_000_000 {
        sleep(Duration::from_nanos(remaining_ns - 10_000_000)).await;
    }

    // Busy-wait for precise timing
    now = monotonic_nanos();
    while now < target_ns {
        std::hint::spin_loop();
        now = monotonic_nanos();
    }

    now
}

async fn produce_impl<const N: usize>(
    file: &File,
    offset: u64,
    id: usize,
    concurrency: usize,
    batches: u64,
    batch_size: u64,
    output: Option<PathBuf>,
    start_time: u64,
) -> anyhow::Result<()>
where
    Message<N>: Unpin + Send + Sync,
{
    let producers = Arc::new(
        ProducerRegions::<Message<N>>::new(file, id, offset)
            .context("failed to map producer regions")?
            .into_producers(),
    );

    let mut handles = Vec::new();

    for task_id in 0..concurrency {
        let producers = Arc::clone(&producers);

        handles.push(tokio::spawn(async move {
            // Wait for synchronized start inside the task
            let task_start = wait_until(start_time).await;
            let mut measurements = Vec::with_capacity(batches as usize);
            let item = Item::new(Message::default());

            for batch_id in 0..batches {
                // Send batch_size items
                for _ in 0..batch_size {
                    let mut backoff = Backoff::new();
                    while producers.try_produce(&item).await.is_err() {
                        backoff.snooze().await;
                    }
                }

                // Record completion timestamp after batch completes
                measurements.push(BatchMeasurement {
                    task_id,
                    batch_id,
                    completion_timestamp_ns: monotonic_nanos(),
                });
            }

            (task_start, measurements)
        }));
    }

    let results = join_all(handles).await;
    let benchmark_end = monotonic_nanos();

    // Aggregate measurements and find earliest start
    let mut all_measurements = Vec::new();
    let mut earliest_start = u64::MAX;

    for result in results.into_iter().filter_map(|r| r.ok()) {
        let (task_start, measurements) = result;
        earliest_start = earliest_start.min(task_start);
        all_measurements.extend(measurements);
    }

    let total_items = batches * batch_size * concurrency as u64;
    let duration_ns = benchmark_end - earliest_start;
    let duration_s = duration_ns as f64 / 1_000_000_000.0;
    let throughput = total_items as f64 / duration_s / 1_000_000.0;

    println!(
        "Producer {id}: sent {total_items} items in {duration_s:.3}s ({throughput:.3} MOps/s)"
    );

    // Write CSV if output specified
    if let Some(path) = output {
        let mut writer = csv::Writer::from_path(&path).context("failed to create output file")?;
        for measurement in &all_measurements {
            writer.serialize(measurement)?;
        }
        writer.flush()?;
        println!(
            "Wrote {} measurements to {}",
            all_measurements.len(),
            path.display()
        );
    }

    Ok(())
}

// ============================================================================
// Check
// ============================================================================

async fn check_impl<const N: usize>(file: &File, offset: u64) -> anyhow::Result<()> {
    let info = unsafe { Region::<Info>::new(file, offset).context("failed to map info region")? };

    let info_ptr = &*info as *const Info;
    println!("Info @ ptr={:p}, page={}:", info_ptr, offset);
    println!("  producer_count: {}", info.producer_count);
    println!("  consumer_count: {}", info.consumer_count);
    println!();

    for producer_index in 0..info.producer_count {
        for consumer_index in 0..info.consumer_count {
            let queue_offset =
                compute_page_offset::<Message<N>>(&info, producer_index, consumer_index) + offset;
            let queue = unsafe {
                Region::<QueueData<Message<N>>>::new(file, queue_offset).with_context(|| {
                    format!(
                        "failed to map queue for producer {producer_index}, consumer {consumer_index}"
                    )
                })?
            };

            let queue_ptr = &*queue as *const QueueData<Message<N>>;

            // Read last_index_read as raw u32 for display
            let last_index_read_ptr = &*queue.0 as *const Index as *const u32;
            let last_index_read_raw = unsafe { *last_index_read_ptr };

            let flushed_index: Index = *queue.0;
            let flushed_initialized = flushed_index == Index::MAX;

            let mut items_initialized = 0usize;
            let mut items_not_initialized = 0usize;
            for item in queue.1.iter() {
                // Access the index field - it's at offset size_of::<Message<N>>() in Item<T>
                let item_ptr = item as *const _ as *const u8;
                let index_ptr =
                    unsafe { item_ptr.add(std::mem::size_of::<Message<N>>()) as *const Index };
                let item_index = unsafe { *index_ptr };
                if item_index == Index::MAX {
                    items_initialized += 1;
                } else {
                    items_not_initialized += 1;
                }
            }

            let queue_initialized = flushed_initialized && items_not_initialized == 0;
            let state = if queue_initialized {
                "initialized"
            } else {
                "not initialized"
            };

            let last_index_read_str = if last_index_read_raw == u32::MAX {
                "MAX".to_string()
            } else {
                last_index_read_raw.to_string()
            };

            println!(
                "Queue[producer={}, consumer={}] @ ptr={:p}, page={}: {} (last_index_read={}, items: {}/{} initialized)",
                producer_index,
                consumer_index,
                queue_ptr,
                queue_offset,
                state,
                last_index_read_str,
                items_initialized,
                items_initialized + items_not_initialized
            );

            // Print first 10 items and items around the boundary for debugging
            println!("  First 10 items (index):");
            for (i, item) in queue.1.iter().take(10).enumerate() {
                let item_ptr = item as *const _ as *const u8;
                let index_ptr =
                    unsafe { item_ptr.add(std::mem::size_of::<Message<N>>()) as *const u32 };
                let item_index = unsafe { *index_ptr };
                let index_str = if item_index == u32::MAX {
                    "MAX".to_string()
                } else {
                    item_index.to_string()
                };
                println!("    [{}]: index={}", i, index_str);
            }

            // Print items around the initialized/not-initialized boundary
            if items_not_initialized > 0 && items_initialized > 0 {
                let boundary = items_not_initialized;
                let start = boundary.saturating_sub(3);
                let end = (boundary + 3).min(queue.1.len());
                println!("  Items around boundary (slot {}):", boundary);
                for i in start..end {
                    let item = &queue.1[i];
                    let item_ptr = item as *const _ as *const u8;
                    let index_ptr =
                        unsafe { item_ptr.add(std::mem::size_of::<Message<N>>()) as *const u32 };
                    let item_index = unsafe { *index_ptr };
                    let index_str = if item_index == u32::MAX {
                        "MAX".to_string()
                    } else {
                        item_index.to_string()
                    };
                    println!("    [{}]: index={}", i, index_str);
                }
            }
        }
    }

    Ok(())
}

// ============================================================================
// Consume
// ============================================================================

async fn consume_impl<const N: usize>(
    file: &File,
    offset: u64,
    id: usize,
    concurrency: usize,
    _duration: Duration,
) -> anyhow::Result<()>
where
    Message<N>: Unpin + Send + Sync,
{
    let consumers = Arc::new(
        ConsumerRegions::<Message<N>>::new(file, id, offset)
            .context("failed to map consumer regions")?
            .into_consumers(),
    );

    let benchmark_start = monotonic_nanos();
    let deadline = benchmark_start + _duration.as_nanos() as u64;

    let mut handles = Vec::new();

    for _ in 0..concurrency {
        let consumers = Arc::clone(&consumers);
        handles.push(tokio::spawn(async move {
            let mut count = 0u64;
            let mut iterations = 0u32;
            let mut backoff = Backoff::new();
            loop {
                iterations = iterations.wrapping_add(1);
                if iterations % (1 << 14) == 0 && monotonic_nanos() > deadline {
                    break;
                }
                if consumers
                    .try_consume(async |item| {
                        unsafe { ptr::read_volatile(item.data()) };
                    })
                    .await
                    .is_ok()
                {
                    count += 1;
                    backoff.reset();
                } else {
                    backoff.snooze().await;
                }
            }
            count
        }));
    }

    let results = join_all(handles).await;
    let benchmark_end = monotonic_nanos();

    let total_received: u64 = results.into_iter().filter_map(|r| r.ok()).sum();
    let duration_ns = benchmark_end - benchmark_start;
    let duration_s = duration_ns as f64 / 1_000_000_000.0;
    let throughput = total_received as f64 / duration_s / 1_000_000.0;

    println!(
        "Consumer {id}: received {total_received} items in {duration_s:.3}s ({throughput:.3} MOps/s)"
    );

    Ok(())
}
