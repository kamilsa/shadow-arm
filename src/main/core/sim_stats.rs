use std::cell::RefCell;
use std::sync::Mutex;
use std::time::Duration;

use anyhow::Context;
use serde::Serialize;

use crate::utility::counter::Counter;

/// Simulation statistics to be accessed by a single thread.
#[derive(Debug)]
pub struct LocalSimStats {
    pub alloc_counts: RefCell<Counter>,
    pub dealloc_counts: RefCell<Counter>,
    pub syscall_counts: RefCell<Counter>,
}

impl LocalSimStats {
    pub fn new() -> Self {
        Self {
            alloc_counts: RefCell::new(Counter::new()),
            dealloc_counts: RefCell::new(Counter::new()),
            syscall_counts: RefCell::new(Counter::new()),
        }
    }
}

impl Default for LocalSimStats {
    fn default() -> Self {
        Self::new()
    }
}

/// Statistics about the manager's scheduling rounds. These include real (wall-clock) times, so
/// they are not deterministic.
#[derive(Serialize, Clone, Debug, Default)]
pub struct SchedulingStats {
    /// Number of scheduling rounds.
    pub rounds: u64,
    /// Total real time of all rounds.
    pub round_time_ns: u128,
    /// Total real time that the manager thread spent waiting for the worker threads to finish
    /// their rounds.
    pub round_wait_time_ns: u128,
    /// Total real time, summed over all worker threads, between a worker thread finishing its
    /// hosts and the end of the round. This includes both load imbalance and the time to
    /// synchronize the threads at the end of the round.
    pub worker_idle_time_ns: u128,
}

impl SchedulingStats {
    pub fn add_round(
        &mut self,
        round_time: Duration,
        round_wait_time: Duration,
        worker_idle_times: impl IntoIterator<Item = Duration>,
    ) {
        self.rounds += 1;
        self.round_time_ns += round_time.as_nanos();
        self.round_wait_time_ns += round_wait_time.as_nanos();
        self.worker_idle_time_ns += worker_idle_times
            .into_iter()
            .map(|x| x.as_nanos())
            .sum::<u128>();
    }
}

/// Simulation statistics to be accessed by multiple threads.
#[derive(Debug)]
pub struct SharedSimStats {
    pub alloc_counts: Mutex<Counter>,
    pub dealloc_counts: Mutex<Counter>,
    pub syscall_counts: Mutex<Counter>,
    pub scheduling: Mutex<SchedulingStats>,
}

impl SharedSimStats {
    pub fn new() -> Self {
        Self {
            alloc_counts: Mutex::new(Counter::new()),
            dealloc_counts: Mutex::new(Counter::new()),
            syscall_counts: Mutex::new(Counter::new()),
            scheduling: Mutex::new(SchedulingStats::default()),
        }
    }

    /// Add stats from a local object to a shared object. May reset fields of `local`.
    pub fn add_from_local_stats(&self, local: &LocalSimStats) {
        let mut shared_alloc_counts = self.alloc_counts.lock().unwrap();
        let mut shared_dealloc_counts = self.dealloc_counts.lock().unwrap();
        let mut shared_syscall_counts = self.syscall_counts.lock().unwrap();

        let mut local_alloc_counts = local.alloc_counts.borrow_mut();
        let mut local_dealloc_counts = local.dealloc_counts.borrow_mut();
        let mut local_syscall_counts = local.syscall_counts.borrow_mut();

        shared_alloc_counts.add_counter(&local_alloc_counts);
        shared_dealloc_counts.add_counter(&local_dealloc_counts);
        shared_syscall_counts.add_counter(&local_syscall_counts);

        *local_alloc_counts = Counter::new();
        *local_dealloc_counts = Counter::new();
        *local_syscall_counts = Counter::new();
    }
}

impl Default for SharedSimStats {
    fn default() -> Self {
        Self::new()
    }
}

/// Simulation statistics in the format to be output.
#[derive(Serialize, Clone, Debug)]
struct SimStatsForOutput {
    pub objects: ObjectStatsForOutput,
    pub syscalls: Counter,
    pub scheduling: SchedulingStats,
}

#[derive(Serialize, Clone, Debug)]
struct ObjectStatsForOutput {
    pub alloc_counts: Counter,
    pub dealloc_counts: Counter,
}

impl SimStatsForOutput {
    /// Takes data from `stats` and puts it into a structure designed for output. May reset fields
    /// of `stats`.
    pub fn new(stats: &SharedSimStats) -> Self {
        Self {
            objects: ObjectStatsForOutput {
                alloc_counts: std::mem::take(&mut stats.alloc_counts.lock().unwrap()),
                dealloc_counts: std::mem::take(&mut stats.dealloc_counts.lock().unwrap()),
            },
            syscalls: std::mem::take(&mut stats.syscall_counts.lock().unwrap()),
            scheduling: std::mem::take(&mut stats.scheduling.lock().unwrap()),
        }
    }
}

/// May reset fields of `stats`.
pub fn write_stats_to_file(
    filename: &std::path::Path,
    stats: &SharedSimStats,
) -> anyhow::Result<()> {
    let stats = SimStatsForOutput::new(stats);

    let file = std::fs::File::create(filename)
        .with_context(|| format!("Failed to create file '{}'", filename.display()))?;

    serde_json::to_writer_pretty(file, &stats).with_context(|| {
        format!(
            "Failed to write stats json to file '{}'",
            filename.display()
        )
    })?;

    Ok(())
}
