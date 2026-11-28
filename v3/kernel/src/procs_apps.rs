//! The #618 stage's processes: two apps built as their own crates (apps/worker, apps/rogue), linked for the app slots
//! on their own and loaded by core 0 into core 1's process table (as `procs::prepare` does for the spinners).
//!
//! `worker` is the well-behaved process: it only counts rounds and checks its own memory and handle. `rogue` misbehaves
//! as its mode says (apps/rogue). Termination here gives everything back (`Kernel::reclaim`).
use crate::loader::{self, LoadError};
use crate::platform;
use crate::procs;
use crate::sched::{self, Supervisor};

static WORKER: &[u8] = include_bytes!(env!("LICHEN_APP_WORKER"));
static ROGUE: &[u8] = include_bytes!(env!("LICHEN_APP_ROGUE"));
/// The same crate linked for slot 0 (an image runs only in the slot it was linked for).
static ROGUE_SLOT0: &[u8] = include_bytes!(env!("LICHEN_APP_ROGUE0"));

/// The worker is process 0 (slot 0), the rogue process 1 (slot 1); idle is `procs::IDLE`.
pub const WORKER_SLOT: usize = 0;
pub const ROGUE_SLOT: usize = 1;
/// apps/rogue's modes (its a0).
pub const CRASH: usize = 0;
pub const SPIN: usize = 1;
pub const EXIT: usize = 2;
pub const WRITE_LOOP: usize = 3;
/// The exit code of rogue's EXIT mode.
pub const ROGUE_EXIT_CODE: isize = 77;
/// Where in the worker's data the crashing rogue stores: inside its canary block, so a store that got through would
/// also end the worker.
pub const TARGET_OFFSET: usize = 64;

/// The address the rogue attacks.
pub fn target() -> usize {
    platform::slot(WORKER_SLOT).data + TARGET_OFFSET
}

/// Core 0: core 1's table gets the worker and a rogue in `mode`, which ends on `cycle_budget` cycles (None: no budget).
/// Same hand-off rules as `procs::prepare`.
pub fn prepare_pair(mode: usize, cycle_budget: Option<u64>) -> Result<(), LoadError> {
    let k = fresh();
    let s = platform::slot(WORKER_SLOT);
    loader::load(k, WORKER_SLOT, "worker", WORKER, s, [0; 4], None)?;
    let s = platform::slot(ROGUE_SLOT);
    loader::load(k, ROGUE_SLOT, "rogue", ROGUE, s, [mode, target(), 0, 0], None)?;
    k.procs[ROGUE_SLOT].cycle_budget = cycle_budget;
    procs::install_idle(k);
    Ok(())
}

/// Core 0: the rogue in EXIT mode alone in the worker's slot, over the code core 1 ran from there before.
pub fn prepare_replace() -> Result<(), LoadError> {
    let k = fresh();
    let s = platform::slot(WORKER_SLOT);
    loader::load(k, WORKER_SLOT, "rogue", ROGUE_SLOT0, s, [EXIT, 0, 0, 0], None)?;
    procs::install_idle(k);
    Ok(())
}

fn fresh() -> &'static mut sched::Kernel {
    // SAFETY: core 1 does not use its Kernel until OP_PROCS (see `procs::prepare`); this is the only reference to it.
    let k = unsafe { sched::of(1) };
    k.reset();
    k.sup = Supervisor::Procs;
    k.reclaim = true;
    k
}
