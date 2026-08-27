use std::fs::File;
use std::mem;
use std::path::{Path, PathBuf};
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
/// Maps item_size -> padding size (item_size - 8 for the timestamp field).
macro_rules! dispatch_by_item_size {
    ($item_size:expr, $func:ident($($arg:expr),* $(,)?)) => {
        match $item_size {
            8 => $func::<0>($($arg),*).await,
            16 => $func::<8>($($arg),*).await,
            32 => $func::<24>($($arg),*).await,
            64 => $func::<56>($($arg),*).await,
            128 => $func::<120>($($arg),*).await,
            256 => $func::<248>($($arg),*).await,
            512 => $func::<504>($($arg),*).await,
            1024 => $func::<1016>($($arg),*).await,
            2048 => $func::<2040>($($arg),*).await,
            4096 => $func::<4088>($($arg),*).await,
            8192 => $func::<8184>($($arg),*).await,
            16384 => $func::<16376>($($arg),*).await,
            _ => bail!("Unsupported item size: {}. Supported: 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384", $item_size),
        }
    };
}

/// RTT benchmark for shared memory IPC
#[derive(Debug, Parser)]
struct Args {
    /// Item size in bytes (8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384)
    #[clap(long, short = 's')]
    item_size: usize,

    #[clap(subcommand)]
    action: Action,
}

#[derive(Debug, Subcommand)]
enum Action {
    /// Initialize the shared memory regions
    Init {
        /// Request mmap file
        #[clap(long)]
        request_file: PathBuf,
        /// Request file offset
        #[clap(long, default_value_t = 0)]
        request_offset: u64,
        /// Response mmap file
        #[clap(long)]
        response_file: PathBuf,
        /// Response file offset
        #[clap(long, default_value_t = 0)]
        response_offset: u64,
        /// Number of senders (request producers)
        #[clap(long)]
        senders: usize,
        /// Number of server instances (request consumers / response producers)
        #[clap(long)]
        servers: usize,
        /// Number of receivers (response consumers)
        #[clap(long)]
        receivers: usize,
    },
    /// Send requests (produce to request queue)
    Send {
        /// Request mmap file
        #[clap(long)]
        file: PathBuf,
        /// File offset
        #[clap(long, default_value_t = 0)]
        offset: u64,
        /// Instance ID (0-indexed)
        #[clap(long, short)]
        id: usize,
        /// Number of concurrent tasks
        #[clap(long, short)]
        concurrency: usize,
        /// Duration to run
        #[clap(long, short, value_parser = humantime::parse_duration)]
        duration: Duration,
        /// Total load in mops/s (millions of operations per second)
        #[clap(long, short)]
        load: f64,
    },
    /// Receive responses (consume from response queue) and measure RTT
    Receive {
        /// Response mmap file
        #[clap(long)]
        file: PathBuf,
        /// File offset
        #[clap(long, default_value_t = 0)]
        offset: u64,
        /// Instance ID (0-indexed)
        #[clap(long, short)]
        id: usize,
        /// Number of concurrent tasks
        #[clap(long, short)]
        concurrency: usize,
        /// Duration to run
        #[clap(long, short, value_parser = humantime::parse_duration)]
        duration: Duration,
        /// Output CSV file for raw measurements (send_time_ns,rtt_ns)
        #[clap(long, short)]
        output: Option<PathBuf>,
    },
    /// Echo requests back as responses
    Server {
        /// Request mmap file
        #[clap(long)]
        request_file: PathBuf,
        /// Request file offset
        #[clap(long, default_value_t = 0)]
        request_offset: u64,
        /// Response mmap file
        #[clap(long)]
        response_file: PathBuf,
        /// Response file offset
        #[clap(long, default_value_t = 0)]
        response_offset: u64,
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
    /// Check shared memory regions and print info/state
    Check {
        /// mmap file to check
        #[clap(long)]
        file: PathBuf,
        /// File offset
        #[clap(long, default_value_t = 0)]
        offset: u64,
    },
}

pub fn main() -> anyhow::Result<()> {
    build_pinned_runtime().block_on(async_main())
}

async fn async_main() -> anyhow::Result<()> {
    let args = Args::parse();
    match args.action {
        Action::Init {
            request_file,
            request_offset,
            response_file,
            response_offset,
            senders,
            servers,
            receivers,
        } => {
            let request_file = unsafe { open(&request_file).context("request file open failed")? };
            let response_file =
                unsafe { open(&response_file).context("response file open failed")? };
            dispatch_by_item_size!(
                args.item_size,
                init_impl(
                    &request_file,
                    request_offset,
                    &response_file,
                    response_offset,
                    senders,
                    servers,
                    receivers,
                )
            )
            .context("init failed")?
        }
        Action::Send {
            file,
            offset,
            id,
            concurrency,
            duration,
            load,
        } => {
            let file = unsafe { open(&file).context("file open failed")? };
            dispatch_by_item_size!(
                args.item_size,
                send_impl(&file, offset, id, concurrency, duration, load,)
            )
            .context("send failed")?
        }
        Action::Receive {
            file,
            offset,
            id,
            concurrency,
            duration,
            output,
        } => {
            let file = unsafe { open(&file).context("file open failed")? };
            dispatch_by_item_size!(
                args.item_size,
                receive_impl(&file, offset, id, concurrency, duration, output.as_deref(),)
            )
            .context("receive failed")?
        }
        Action::Server {
            request_file,
            request_offset,
            response_file,
            response_offset,
            id,
            concurrency,
            duration,
        } => {
            let request_file = unsafe { open(&request_file).context("request file open failed")? };
            let response_file =
                unsafe { open(&response_file).context("response file open failed")? };
            dispatch_by_item_size!(
                args.item_size,
                server_impl(
                    &request_file,
                    request_offset,
                    &response_file,
                    response_offset,
                    id,
                    concurrency,
                    duration,
                )
            )
            .context("server failed")?
        }
        Action::Check { file, offset } => {
            let file = unsafe { open(&file).context("file open failed")? };
            dispatch_by_item_size!(args.item_size, check_impl(&file, offset,))
                .context("check failed")?
        }
    };

    Ok(())
}

/// Message type with a timestamp and padding to reach the desired size
#[repr(C)]
#[derive(Clone, Copy)]
struct Message<const N: usize> {
    timestamp: u64,
    _padding: [u8; N],
}

impl<const N: usize> Default for Message<N> {
    fn default() -> Self {
        Self {
            timestamp: 0,
            _padding: [0u8; N],
        }
    }
}

// ============================================================================
// Init
// ============================================================================

async fn init_impl<const N: usize>(
    request_file: &File,
    request_offset: u64,
    response_file: &File,
    response_offset: u64,
    senders: usize,
    servers: usize,
    receivers: usize,
) -> anyhow::Result<()> {
    initialize::<Message<N>>(request_file, request_offset, senders, servers)?;
    println!("Initialized request queues: {senders} senders -> {servers} servers");

    initialize::<Message<N>>(response_file, response_offset, servers, receivers)?;
    println!("Initialized response queues: {servers} servers -> {receivers} receivers");

    Ok(())
}

// ============================================================================
// Check - prints info and queue state
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

            println!(
                "Queue[producer={}, consumer={}] @ ptr={:p}, page={}: {} (flushed={}, items: {}/{} initialized)",
                producer_index,
                consumer_index,
                queue_ptr,
                queue_offset,
                state,
                if flushed_initialized { "init" } else { "used" },
                items_initialized,
                items_initialized + items_not_initialized
            );

            // Print first 10 items and items around the boundary for debugging
            println!("  First 10 items (index, timestamp):");
            for (i, item) in queue.1.iter().take(10).enumerate() {
                let item_ptr = item as *const _ as *const u8;
                let timestamp_ptr = item_ptr as *const u64;
                let index_ptr =
                    unsafe { item_ptr.add(std::mem::size_of::<Message<N>>()) as *const u32 };
                let timestamp = unsafe { *timestamp_ptr };
                let item_index = unsafe { *index_ptr };
                let index_str = if item_index == u32::MAX {
                    "MAX".to_string()
                } else {
                    item_index.to_string()
                };
                println!("    [{}]: index={}, timestamp={}", i, index_str, timestamp);
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
                    let timestamp_ptr = item_ptr as *const u64;
                    let index_ptr =
                        unsafe { item_ptr.add(std::mem::size_of::<Message<N>>()) as *const u32 };
                    let timestamp = unsafe { *timestamp_ptr };
                    let item_index = unsafe { *index_ptr };
                    let index_str = if item_index == u32::MAX {
                        "MAX".to_string()
                    } else {
                        item_index.to_string()
                    };
                    println!("    [{}]: index={}, timestamp={}", i, index_str, timestamp);
                }
            }
        }
    }

    Ok(())
}

// ============================================================================
// Send role - produces requests with timestamps
// ============================================================================

async fn send_impl<const N: usize>(
    file: &File,
    offset: u64,
    id: usize,
    concurrency: usize,
    duration: Duration,
    load: f64,
) -> anyhow::Result<()>
where
    Message<N>: Unpin + Send + Sync,
{
    let delay_nanos = (concurrency as f64 / (load * 1_000_000.0) * 1_000_000_000.0) as u64;

    let producers = Arc::new(
        ProducerRegions::<Message<N>>::new(file, id, offset)
            .context("failed to map producer regions")?
            .into_producers(),
    );

    let benchmark_start = monotonic_nanos();
    let deadline = benchmark_start + duration.as_nanos() as u64;

    let mut handles = Vec::new();

    for _ in 0..concurrency {
        let producers = Arc::clone(&producers);

        handles.push(tokio::spawn(async move {
            let mut count = 0u64;
            let mut next_send_ns = monotonic_nanos();
            let mut item = Item::<Message<N>>::new(unsafe { mem::zeroed() });

            loop {
                item.data_mut().timestamp = monotonic_nanos();
                let mut backoff = Backoff::new();
                let mut iterations = 0u32;
                while producers.try_produce(&item).await.is_err() {
                    iterations += 1;
                    if iterations % (1 << 14) == 0 && monotonic_nanos() > deadline {
                        return count;
                    }
                    backoff.snooze().await;
                }
                count += 1;

                next_send_ns += delay_nanos;
                if next_send_ns > deadline {
                    break;
                }
                while monotonic_nanos() < next_send_ns {
                    tokio::task::yield_now().await;
                }
            }
            count
        }));
    }

    let results = join_all(handles).await;
    let benchmark_end = monotonic_nanos();

    let sent: u64 = results.into_iter().filter_map(|r| r.ok()).sum();
    let duration_s = (benchmark_end - benchmark_start) as f64 / 1_000_000_000.0;
    let throughput = sent as f64 / duration_s / 1_000_000.0;
    println!("Sender {id}: sent {sent} messages ({throughput:.3} MOps/s)");

    Ok(())
}

// ============================================================================
// Receive role - consumes responses and measures RTT
// ============================================================================

/// A single RTT measurement
#[derive(serde::Serialize)]
struct Measurement {
    send_time_ns: u64,
    receive_time_ns: u64,
}

async fn receive_impl<const N: usize>(
    file: &File,
    offset: u64,
    id: usize,
    concurrency: usize,
    duration: Duration,
    output: Option<&Path>,
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
    let deadline = benchmark_start + duration.as_nanos() as u64;

    let mut handles = Vec::new();

    for _ in 0..concurrency {
        let consumers = Arc::clone(&consumers);

        handles.push(tokio::spawn(async move {
            let mut measurements = Vec::new();
            let mut iterations = 0u32;
            let mut backoff = Backoff::new();
            loop {
                iterations = iterations.wrapping_add(1);
                if iterations % (1 << 14) == 0 && monotonic_nanos() > deadline {
                    break;
                }
                if consumers
                    .try_consume(async |response| {
                        let recv_time = monotonic_nanos();
                        measurements.push(Measurement {
                            send_time_ns: response.data().timestamp,
                            receive_time_ns: recv_time,
                        });
                    })
                    .await
                    .is_ok()
                {
                    backoff.reset();
                } else {
                    backoff.snooze().await;
                }
            }
            measurements
        }));
    }

    let results = join_all(handles).await;
    let benchmark_end = monotonic_nanos();
    let all_measurements: Vec<_> = results
        .into_iter()
        .filter_map(|r| r.ok())
        .flatten()
        .collect();
    let recv = all_measurements.len();
    let duration_s = (benchmark_end - benchmark_start) as f64 / 1_000_000_000.0;
    let throughput = recv as f64 / duration_s / 1_000_000.0;
    println!("Receiver {id}: received {recv} messages ({throughput:.3} MOps/s)");

    if !all_measurements.is_empty() {
        let mut rtts: Vec<u64> = all_measurements
            .iter()
            .map(|m| m.receive_time_ns.saturating_sub(m.send_time_ns))
            .collect();
        rtts.sort();
        let len = rtts.len();
        let avg = rtts.iter().sum::<u64>() / len as u64;
        let p50 = rtts[len / 2];
        let p99 = rtts[len * 99 / 100];
        let p999 = rtts.get(len * 999 / 1000).copied().unwrap_or(p99);
        let min = rtts[0];
        let max = rtts[len - 1];

        println!("RTT Statistics ({} samples):", len);
        println!("  Min:  {} ns", min);
        println!("  Avg:  {} ns", avg);
        println!("  P50:  {} ns", p50);
        println!("  P99:  {} ns", p99);
        println!("  P999: {} ns", p999);
        println!("  Max:  {} ns", max);
    }

    if let Some(path) = output {
        let mut writer = csv::Writer::from_path(path).context("failed to create output file")?;
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
// Server role - consumes requests and produces responses (echo)
// ============================================================================

async fn server_impl<const N: usize>(
    request_file: &File,
    request_offset: u64,
    response_file: &File,
    response_offset: u64,
    id: usize,
    concurrency: usize,
    duration: Duration,
) -> anyhow::Result<()>
where
    Message<N>: Unpin + Send + Sync,
{
    let consumers = Arc::new(
        ConsumerRegions::<Message<N>>::new(request_file, id, request_offset)
            .context("failed to map request consumer regions")?
            .into_consumers(),
    );
    let producers = Arc::new(
        ProducerRegions::<Message<N>>::new(response_file, id, response_offset)
            .context("failed to map response producer regions")?
            .into_producers(),
    );

    let benchmark_start = monotonic_nanos();
    let deadline = benchmark_start + duration.as_nanos() as u64;

    let mut handles = Vec::new();

    for _ in 0..concurrency {
        let consumers = Arc::clone(&consumers);
        let producers = Arc::clone(&producers);

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
                        let mut produce_iterations = 0u32;
                        let mut backoff = Backoff::new();
                        while producers.try_produce(item).await.is_err() {
                            produce_iterations = produce_iterations.wrapping_add(1);
                            if produce_iterations % (1 << 14) == 0 && monotonic_nanos() > deadline {
                                break;
                            }
                            backoff.snooze().await;
                        }
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

    let echoed: u64 = results.into_iter().filter_map(|r| r.ok()).sum();
    let duration_s = (benchmark_end - benchmark_start) as f64 / 1_000_000_000.0;
    let throughput = echoed as f64 / duration_s / 1_000_000.0;
    println!("Server {id}: echoed {echoed} messages ({throughput:.3} MOps/s)");

    Ok(())
}
