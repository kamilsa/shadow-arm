use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use nix::errno::Errno;

use crate::sync::simple_latch::libc_futex;

/// How long a spinning waiter will `sched_yield` in a loop before futex-waiting. Most scheduling
/// rounds with little work finish within this time, so a spinning waiter can avoid the latency of
/// being woken from a futex wait. Longer rounds are dominated by the work itself, so there's little
/// benefit to spinning longer and occupying a CPU.
const SPIN_DURATION: Duration = Duration::from_micros(100);

/// A latch counter.
///
/// If a counter is cloned, it will inherit the counter's state for the current generation. For
/// example if a counter is cloned after it has already counted down, then the new counter will also
/// be treated as if it had already counted down in the current generation. If a counter is cloned
/// before it has counted down, then the new counter will also need to count down in the current
/// generation.
#[derive(Debug)]
pub struct LatchCounter {
    inner: Arc<LatchInner>,
    /// An ID for this counter's count-down round.
    generation: usize,
}

/// A latch waiter.
///
/// If a waiter is cloned, it will inherit the waiter's state for the current generation. For
/// example if a waiter is cloned after it has already waited, then the new waiter will also be
/// treated as if it had already waited in the current generation. If a waiter is cloned before it
/// has waited, then the new waiter will also need to wait in the current generation.
#[derive(Debug)]
pub struct LatchWaiter {
    inner: Arc<LatchInner>,
    /// An ID for this waiter's count-down round.
    generation: usize,
    /// Should we `sched_yield` in a loop for a short time before futex-waiting?
    spin_yield: bool,
}

#[derive(Debug)]
struct LatchInner {
    lock: Mutex<LatchState>,
    /// Incremented after any change to the latch state that may allow a waiter to return. Waiters
    /// futex-wait on this value.
    seq: AtomicU32,
    /// Number of waiters that are currently futex-waiting (or about to) on `seq`. Used to avoid
    /// making a futex-wake syscall when there's nobody to wake.
    sleepers: AtomicU32,
}

#[derive(Debug)]
struct LatchState {
    /// The current latch "round".
    generation: usize,
    /// Number of counters remaining.
    counters: usize,
    /// Number of waiters remaining.
    waiters: usize,
    /// Total number of counters.
    total_counters: usize,
    /// Total number of waiters.
    total_waiters: usize,
}

/// Build a latch counter and waiter. The counter and waiter can be cloned to create new counters
/// and waiters.
pub fn build_count_down_latch() -> (LatchCounter, LatchWaiter) {
    let inner = Arc::new(LatchInner {
        lock: Mutex::new(LatchState {
            generation: 0,
            counters: 1,
            waiters: 1,
            total_counters: 1,
            total_waiters: 1,
        }),
        seq: AtomicU32::new(0),
        sleepers: AtomicU32::new(0),
    });

    let counter = LatchCounter {
        inner: Arc::clone(&inner),
        generation: 0,
    };

    let waiter = LatchWaiter {
        inner,
        generation: 0,
        spin_yield: false,
    };

    (counter, waiter)
}

impl LatchState {
    pub fn advance_generation(&mut self) {
        debug_assert_eq!(self.counters, 0);
        debug_assert_eq!(self.waiters, 0);
        self.counters = self.total_counters;
        self.waiters = self.total_waiters;
        self.generation = self.generation.wrapping_add(1);
    }
}

impl LatchInner {
    /// Wake any waiters so that they re-check the latch state. Must be called after the state was
    /// changed (and may be called after the lock was released).
    fn notify(&self) {
        // This and the `sleepers` increment in `wait()` must be `SeqCst` so that either we see the
        // waiter's `sleepers` increment and wake it, or the waiter sees the new `seq` value and
        // doesn't sleep.
        self.seq.fetch_add(1, Ordering::SeqCst);

        if self.sleepers.load(Ordering::SeqCst) > 0 {
            libc_futex(
                &self.seq,
                libc::FUTEX_WAKE | libc::FUTEX_PRIVATE_FLAG,
                i32::MAX as u32,
                None,
                None,
                0,
            )
            .expect("FUTEX_WAKE failed");
        }
    }

    /// Wait until `seq` is no longer `seq_val`.
    fn wait_for_notify(&self, seq_val: u32, spin_yield: bool) {
        if spin_yield {
            let start = Instant::now();
            while start.elapsed() < SPIN_DURATION {
                if self.seq.load(Ordering::Acquire) != seq_val {
                    return;
                }
                std::hint::spin_loop();
                std::thread::yield_now();
            }
        }

        self.sleepers.fetch_add(1, Ordering::SeqCst);
        if self.seq.load(Ordering::SeqCst) == seq_val {
            let rv = libc_futex(
                &self.seq,
                libc::FUTEX_WAIT | libc::FUTEX_PRIVATE_FLAG,
                seq_val,
                None,
                None,
                0,
            );
            assert!(
                matches!(rv, Ok(_) | Err(Errno::EAGAIN | Errno::EINTR)),
                "FUTEX_WAIT failed with {rv:?}"
            );
        }
        self.sleepers.fetch_sub(1, Ordering::SeqCst);
    }
}

impl LatchCounter {
    /// Decrement the latch count and wake the waiters if the count reaches 0. This must not be
    /// called more than once per generation (must not be called again until all of the waiters have
    /// returned from their [`LatchWaiter::wait()`] calls), otherwise it will panic.
    pub fn count_down(&mut self) {
        let counters;
        {
            let mut lock = self.inner.lock.lock().unwrap();

            if self.generation != lock.generation {
                let latch_gen = lock.generation;
                std::mem::drop(lock);
                panic!(
                    "Counter generation does not match latch generation ({} != {})",
                    self.generation, latch_gen
                );
            }

            lock.counters = lock.counters.checked_sub(1).unwrap();
            counters = lock.counters;
        }

        // if this is the last counter, notify the waiters
        if counters == 0 {
            self.inner.notify();
        }

        self.generation = self.generation.wrapping_add(1);
    }
}

impl LatchWaiter {
    /// Set whether the waiter should `sched_yield` in a loop for a short time before futex-waiting.
    /// This may reduce the latency of [`wait()`](Self::wait) returning if the counters count down
    /// soon after the wait begins, at the cost of occupying a CPU while spinning.
    pub fn set_spin_yield(&mut self, spin_yield: bool) {
        self.spin_yield = spin_yield;
    }

    /// Wait for the latch count to reach 0. If the latch count has already reached 0 for the
    /// current genration, this will return immediately.
    pub fn wait(&mut self) {
        loop {
            // must be read before checking the state so that we don't miss a notification that
            // happens after the check
            let seq_val = self.inner.seq.load(Ordering::Acquire);

            {
                let mut lock = self.inner.lock.lock().unwrap();

                // wait until we're in the active generation and all counters have counted down
                if self.generation == lock.generation && lock.counters == 0 {
                    lock.waiters = lock.waiters.checked_sub(1).unwrap();

                    // if this is the last waiter (and we already know that there are no more
                    // counters), start the next generation
                    let advanced = lock.waiters == 0;
                    if advanced {
                        lock.advance_generation();
                    }
                    let counters = lock.counters;
                    std::mem::drop(lock);

                    // if there are no counters, waiters of the new generation can return
                    if advanced && counters == 0 {
                        self.inner.notify();
                    }

                    break;
                }
            }

            self.inner.wait_for_notify(seq_val, self.spin_yield);
        }

        self.generation = self.generation.wrapping_add(1);
    }
}

impl Clone for LatchCounter {
    fn clone(&self) -> Self {
        let mut lock = self.inner.lock.lock().unwrap();
        lock.total_counters = lock.total_counters.checked_add(1).unwrap();

        // if we haven't already counted down during the current generation
        if self.generation == lock.generation {
            lock.counters = lock.counters.checked_add(1).unwrap();
        }

        LatchCounter {
            inner: Arc::clone(&self.inner),
            generation: self.generation,
        }
    }
}

impl Clone for LatchWaiter {
    fn clone(&self) -> Self {
        let mut lock = self.inner.lock.lock().unwrap();
        lock.total_waiters = lock.total_waiters.checked_add(1).unwrap();

        // if we haven't already waited during the current generation
        if self.generation == lock.generation {
            lock.waiters = lock.waiters.checked_add(1).unwrap();
        }

        LatchWaiter {
            inner: Arc::clone(&self.inner),
            generation: self.generation,
            spin_yield: self.spin_yield,
        }
    }
}

impl std::ops::Drop for LatchCounter {
    fn drop(&mut self) {
        let counters;
        {
            let mut lock = self.inner.lock.lock().unwrap();
            lock.total_counters = lock.total_counters.checked_sub(1).unwrap();

            // if we haven't already counted down during the current generation
            if self.generation == lock.generation {
                lock.counters = lock.counters.checked_sub(1).unwrap();
            }

            counters = lock.counters;
        }

        // if this is the last counter, notify the waiters
        if counters == 0 {
            self.inner.notify();
        }
    }
}

impl std::ops::Drop for LatchWaiter {
    fn drop(&mut self) {
        let advanced_with_no_counters;
        {
            let mut lock = self.inner.lock.lock().unwrap();
            lock.total_waiters = lock.total_waiters.checked_sub(1).unwrap();

            // if we haven't already waited during the current generation
            if self.generation == lock.generation {
                lock.waiters = lock.waiters.checked_sub(1).unwrap();
            }

            // if this is the last waiter and there are no more counters, start the next generation
            let advanced = lock.waiters == 0 && lock.counters == 0;
            if advanced {
                lock.advance_generation();
            }
            advanced_with_no_counters = advanced && lock.counters == 0;
        }

        // if there are no counters, waiters of the new generation can return
        if advanced_with_no_counters {
            self.inner.notify();
        }
    }
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use atomic_refcell::AtomicRefCell;
    use rand::{Rng, SeedableRng};

    use super::*;

    #[test]
    fn test_clone() {
        let (mut counter, mut waiter) = build_count_down_latch();
        let (mut counter_clone, mut waiter_clone) = (counter.clone(), waiter.clone());

        counter.count_down();
        counter_clone.count_down();
        waiter.wait();
        waiter_clone.wait();
    }

    #[test]
    fn test_clone_before_countdown() {
        let (mut counter, mut waiter) = build_count_down_latch();

        // the cloned counter will also need to count down for the current generation
        let mut counter_clone = counter.clone();
        counter.count_down();
        counter_clone.count_down();
        waiter.wait();

        counter.count_down();
        counter_clone.count_down();
        waiter.wait();

        let (mut counter, mut waiter) = build_count_down_latch();

        // the cloned waiter will also need to wait for the current generation
        let mut waiter_clone = waiter.clone();
        counter.count_down();
        waiter.wait();
        waiter_clone.wait();

        counter.count_down();
        waiter.wait();
        waiter_clone.wait();
    }

    #[test]
    fn test_clone_after_countdown() {
        let (mut counter, mut waiter) = build_count_down_latch();

        counter.count_down();
        // the cloned counter will also be considered "counted down" for the current generation
        let mut counter_clone = counter.clone();
        // if the cloned counter did count down here, it would panic
        waiter.wait();

        counter.count_down();
        counter_clone.count_down();
        waiter.wait();

        let (mut counter, mut waiter) = build_count_down_latch();
        let mut waiter_clone = waiter.clone();

        counter.count_down();
        waiter.wait();
        // the cloned waiter will also be considered "waited" for the current generation
        let mut waiter_clone_2 = waiter.clone();
        // if the cloned waiter did wait here, it would be waiting for the next generation
        waiter_clone.wait();

        counter.count_down();
        waiter.wait();
        waiter_clone.wait();
        waiter_clone_2.wait();
    }

    #[test]
    #[should_panic]
    fn test_double_count() {
        let (mut counter, mut _waiter) = build_count_down_latch();
        counter.count_down();
        counter.count_down();
    }

    #[test]
    fn test_single_thread() {
        let (mut counter, mut waiter) = build_count_down_latch();

        counter.count_down();
        waiter.wait();
        counter.count_down();
        waiter.wait();
        counter.count_down();
        waiter.wait();

        let mut waiter_clone = waiter.clone();

        counter.count_down();
        waiter.wait();
        waiter_clone.wait();

        counter.count_down();
        waiter.wait();
        waiter_clone.wait();
    }

    #[test]
    fn test_multi_thread() {
        let (mut exclusive_counter, mut exclusive_waiter) = build_count_down_latch();
        let (mut shared_counter, mut shared_waiter) = build_count_down_latch();
        let repeat = 30;

        let lock = Arc::new(AtomicRefCell::new(()));
        let lock_clone = Arc::clone(&lock);

        // The goal of this test is to make sure that the new threads alternate with the main thread
        // to access the atomic refcell. The new threads each hold on to a shared borrow of the
        // atomic refcell for ~5 ms, then the main thread gets an exclusive borrow for ~5 ms,
        // repeating. If these time slices ever overlap, then either a shared or exclusive borrow
        // will cause a panic and the test will fail. Randomness is added to the sleeps to vary the
        // order in which threads wait and count down, to try to cover more edge cases.

        let thread_fn = move |seed| {
            let mut rng = rand::rngs::StdRng::seed_from_u64(seed);

            for _ in 0..repeat {
                // wait for the main thread to be done with its exclusive borrow
                std::thread::sleep(Duration::from_millis(5));
                exclusive_waiter.wait();
                {
                    // a shared borrow for a duration in the range of 0-10 ms
                    let _x = lock_clone.borrow();
                    std::thread::sleep(Duration::from_millis(rng.random_range(0..10)));
                }
                shared_counter.count_down();
            }
        };

        // start 5 threads
        let handles: Vec<_> = (0..5)
            .map(|seed| {
                let mut f = thread_fn.clone();
                std::thread::spawn(move || f(seed))
            })
            .collect();
        std::mem::drop(thread_fn);

        let mut rng = rand::rngs::StdRng::seed_from_u64(100);

        for _ in 0..repeat {
            {
                // an exclusive borrow for a duration in the range of 0-10 ms
                let _x = lock.borrow_mut();
                std::thread::sleep(Duration::from_millis(rng.random_range(0..10)));
            }
            exclusive_counter.count_down();
            // wait for the other threads to be done with their shared borrow
            std::thread::sleep(Duration::from_millis(5));
            shared_waiter.wait();
        }

        for h in handles {
            h.join().unwrap();
        }
    }

    /// Many short generations with many threads, for both spinning and futex-waiting. Each
    /// generation, every thread adds to a shared sum before counting down, and the waiter checks
    /// that it observes all of the additions.
    fn stress(spin_yield: bool) {
        use std::sync::atomic::AtomicUsize;

        let num_threads = 8;
        let generations = 2000;

        let (mut start_counter, mut start_waiter) = build_count_down_latch();
        let (mut end_counter, mut end_waiter) = build_count_down_latch();
        start_waiter.set_spin_yield(spin_yield);
        end_waiter.set_spin_yield(spin_yield);

        let sum = Arc::new(AtomicUsize::new(0));

        let handles: Vec<_> = (0..num_threads)
            .map(|_| {
                let mut start_waiter = start_waiter.clone();
                let mut end_counter = end_counter.clone();
                let sum = Arc::clone(&sum);
                std::thread::spawn(move || {
                    for _ in 0..generations {
                        start_waiter.wait();
                        sum.fetch_add(1, Ordering::Relaxed);
                        end_counter.count_down();
                    }
                })
            })
            .collect();

        // the original start waiter and end counter aren't used by any thread
        std::mem::drop(start_waiter);
        end_counter.count_down();

        for generation in 0..generations {
            start_counter.count_down();
            end_waiter.wait();
            assert_eq!(sum.load(Ordering::Relaxed), (generation + 1) * num_threads);
            // the original end counter must count down every generation
            if generation + 1 < generations {
                end_counter.count_down();
            }
        }

        for h in handles {
            h.join().unwrap();
        }
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn test_stress_futex() {
        stress(false);
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn test_stress_spin() {
        stress(true);
    }
}
