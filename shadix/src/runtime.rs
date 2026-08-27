//! Tokio runtime builder with CPU core pinning.

use std::sync::OnceLock;
use std::sync::atomic::{AtomicUsize, Ordering};

use core_affinity::CoreId;

/// Global counter for assigning cores to worker threads.
static CORE_COUNTER: AtomicUsize = AtomicUsize::new(0);

/// Available core IDs for pinning.
static CORE_IDS: OnceLock<Vec<CoreId>> = OnceLock::new();

/// Build a tokio runtime with worker threads pinned to all available CPU cores.
///
/// Detects available cores using `core_affinity` and spawns one worker thread
/// per core, pinning each thread to its corresponding core.
///
/// # Panics
/// Panics if no CPU cores are available or if the runtime fails to build.
pub fn build_pinned_runtime() -> tokio::runtime::Runtime {
    let core_ids = core_affinity::get_core_ids().expect("failed to get core IDs");
    let num_threads = core_ids.len();

    CORE_IDS
        .set(core_ids)
        .expect("runtime already initialized - build_pinned_runtime can only be called once");

    CORE_COUNTER.store(0, Ordering::SeqCst);

    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(num_threads)
        .on_thread_start(|| {
            let idx = CORE_COUNTER.fetch_add(1, Ordering::SeqCst);
            let core_ids = CORE_IDS.get().expect("CORE_IDS not initialized");
            if idx < core_ids.len() {
                let core_id = core_ids[idx];
                if !core_affinity::set_for_current(core_id) {
                    eprintln!("Warning: failed to pin thread to core {}", core_id.id);
                }
            }
        })
        .enable_all()
        .build()
        .expect("failed to build tokio runtime")
}
