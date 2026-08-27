use std::time::Instant;

const SPIN_LIMIT: u32 = 6;
const YIELD_LIMIT: u32 = 10;

/// Base duration for yield backoff in nanoseconds (approximately matches one spin iteration).
const BASE_YIELD_NANOS: f64 = 0.5;

/// Performs exponential backoff in async spin loops.
///
/// Similar to `crossbeam_utils::Backoff`, but yields to the tokio runtime
/// instead of the OS scheduler when the spin limit is exceeded.
///
/// # Example
///
/// ```ignore
/// use crate::backoff::Backoff;
///
/// async fn wait_for_value(atomic: &AtomicBool) {
///     let backoff = Backoff::new();
///     while !atomic.load(Ordering::Acquire) {
///         backoff.snooze().await;
///     }
/// }
/// ```
pub struct Backoff {
    step: u32,
}

impl Backoff {
    /// Creates a new `Backoff`.
    #[inline]
    pub const fn new() -> Self {
        Backoff { step: 0 }
    }

    /// Resets the `Backoff`.
    #[inline]
    pub fn reset(&mut self) {
        self.step = 0;
    }

    /// Backs off in a lock-free loop.
    ///
    /// This method should be used when retrying an operation because another
    /// thread made progress. It only executes spin loop hints and never yields.
    #[inline]
    pub fn spin(&mut self) {
        for _ in 0..1 << self.step.min(SPIN_LIMIT) {
            std::hint::spin_loop();
        }

        if self.step <= SPIN_LIMIT {
            self.step += 1;
        }
    }

    /// Backs off in a spin loop, potentially yielding to tokio.
    ///
    /// Initially spins using CPU hints. After the spin limit is exceeded,
    /// yields to the tokio runtime repeatedly until enough time has passed
    /// to match the exponential backoff duration.
    #[inline]
    pub async fn snooze(&mut self) {
        if self.step <= SPIN_LIMIT {
            for _ in 0..1 << self.step {
                std::hint::spin_loop();
            }
        } else {
            let target_nanos = BASE_YIELD_NANOS * (1u64 << self.step.min(YIELD_LIMIT)) as f64;
            let start = Instant::now();
            while (start.elapsed().as_nanos() as f64) < target_nanos {
                tokio::task::yield_now().await;
            }
        }

        if self.step <= YIELD_LIMIT {
            self.step += 1;
        }
    }
}

impl Default for Backoff {
    fn default() -> Self {
        Backoff::new()
    }
}
