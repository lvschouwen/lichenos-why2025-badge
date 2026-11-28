//! The measured CLINT mtime rate (#654). The hypothesis of #654 is that mtime follows the CPU clock on the P4 (40 MHz
//! on the ROM's XTAL in #608 run 9, 360 MHz at CPU 360 in #648); the `timebase.mtime` row tests it. Either way the rate
//! is measured against SYSTIMER at boot rather than assumed, and every tick conversion in the kernel goes through here.
//! `AtomicU32`s (riscv32imac has no 64-bit atomics) because core 1 converts too; they are written before core 1 starts.
//!
//! A CPU clock change (#655) goes through `switch`: it remeasures the rate, starts a new epoch and clock generation
//! (`lichen_kcore::timebase::Epoch`), and re-arms this hart's tick and U-run deadline in ticks of the new rate.
use crate::global::Global;
use crate::platform::{self, hw};
use core::sync::atomic::{AtomicU32, Ordering};
use lichen_kcore::timebase::{self, Epoch, Rate};

/// Used only when the measurement fails, so the run can still finish and report: the rate #608 run 9 measured with
/// the CPU on the ROM's 40 MHz XTAL. The `timebase.mtime` row is then FAIL; no number derived from it is trusted.
const UNMEASURED_HZ: u32 = 40_000_000;

static HZ: AtomicU32 = AtomicU32::new(UNMEASURED_HZ);
/// The clock generation `HZ` belongs to: 1 after the boot measurement, one more per clock change (0 before any).
static GEN: AtomicU32 = AtomicU32::new(0);
/// The current epoch; written by core 0 only, before core 1 starts (`switch` enforces it), read by core 0 only.
static EPOCH: Global<Epoch> = Global::new(Epoch { hz: UNMEASURED_HZ as u64, gen: 0, mtime0: 0, us0: 0 });

/// The current mtime rate in Hz.
pub fn hz() -> u64 {
    HZ.load(Ordering::Relaxed) as u64
}

/// The current clock generation.
pub fn gen() -> u32 {
    GEN.load(Ordering::Relaxed)
}

/// The mtime rate if the clock is still in generation `g` (taken when a measurement started), else 0: a `Us` built
/// from it prints 0.00 instead of a number in the wrong rate.
pub fn hz_in(g: u32) -> u64 {
    Rate { hz: hz(), gen: gen() }.at(g)
}

/// The kernel's monotonic time in microseconds since the boot measurement (core 0).
pub fn now_us() -> u64 {
    // SAFETY: core 0 only (module doc); the copy ends the borrow at once.
    let e = unsafe { *EPOCH.get() };
    e.us_at(hw::clint::mtime())
}

pub fn us_to_ticks(us: u64) -> u64 {
    timebase::us_to_ticks(us, hz())
}

pub fn ticks_to_us(ticks: u64) -> u64 {
    timebase::ticks_to_us(ticks, hz())
}

/// The outcome of one measurement, for the CHECK line.
pub struct Measured {
    /// The believed rate, or None (the stored rate is then left as it was).
    pub hz: Option<u64>,
    pub mtime_ticks: u64,
    pub systimer_ticks: u64,
}

/// Measure the mtime rate against SYSTIMER (about 2 ms, busy).
fn measure() -> Measured {
    let (mtime_ticks, systimer_ticks) = hw::clint::measure_window().unwrap_or((0, 0));
    let hz = timebase::mtime_hz_from_systimer(mtime_ticks, systimer_ticks);
    Measured { hz, mtime_ticks, systimer_ticks }
}

fn store(e: Epoch) {
    // SAFETY: core 0 before core 1 starts (callers), no other borrow of EPOCH is live.
    unsafe { *EPOCH.get() = e };
    // PLAUSIBLE_HZ ends at 1 GHz, so the rate fits in u32.
    HZ.store(e.hz as u32, Ordering::Relaxed);
    GEN.store(e.gen, Ordering::Relaxed);
}

/// The boot measurement: the first epoch, if the rate is plausible (the stored rate stays `UNMEASURED_HZ` otherwise).
pub fn boot() -> Measured {
    let m = measure();
    if let Some(hz) = m.hz {
        store(Epoch::boot(hz, hw::clint::mtime()));
    }
    m
}

/// What `switch` saw.
pub struct Switched<R> {
    /// What the clock change returned.
    pub out: R,
    /// The remeasure after it; `hz` None means the rate is unknown and the kernel must not go on.
    pub m: Measured,
    pub old: Epoch,
    /// Equal to `old` when the remeasure failed.
    pub new: Epoch,
    /// SYSTIMER ticks from just before the change to the start of the new epoch (0 if SYSTIMER did not answer).
    pub sys_ticks: u64,
}

/// The one way to change the CPU clock (#655): run `change`, then `clock_changed`. Core 0, before core 1 starts (core
/// 1's mtimecmp and tick are its own and this hart cannot re-arm them): it halts otherwise. Never from a trap handler
/// (not checked: the kernel has one caller, bringup).
pub fn switch<R>(change: impl FnOnce() -> R) -> Switched<R> {
    if crate::smp::hartid() != 0 || crate::smp::core1_started() {
        platform::enter_fatal();
        crate::kprintln!("KERNEL: clock switch off core 0 or with core 1 running: halting");
        platform::exit(4);
    }
    let (s0, pre) = (hw::psram::systimer::now_ticks(), hw::clint::mtime());
    let out = change();
    let (m, old, new, sys_ticks) = clock_changed(pre, s0);
    Switched { out, m, old, new, sys_ticks }
}

/// After a CPU clock change: remeasure the mtime rate (2 ms), start the next epoch and generation, re-arm this hart's
/// tick and U-run deadline, and recalibrate the PERF clock. `pre` and `s0` are the mtime and SYSTIMER read just before
/// the change. If the remeasure or SYSTIMER fails nothing changes: the caller ends the run, every rate is unknown.
fn clock_changed(pre: u64, s0: Option<u64>) -> (Measured, Epoch, Epoch, u64) {
    let m = measure();
    // SAFETY: core 0 before core 1 starts (`switch` checked); a copy, the borrow ends at once.
    let old = unsafe { *EPOCH.get() };
    let (s1, now) = (hw::psram::systimer::now_ticks(), hw::clint::mtime());
    let (Some(hz), Some(s0), Some(s1)) = (m.hz, s0, s1) else {
        // HZ, the epoch and the armed compares stay at the old rate, now wrong: the caller ends the run (bringup clock).
        return (Measured { hz: None, ..m }, old, old, 0);
    };
    let elapsed = hw::psram::systimer::elapsed(s0, s1);
    let new = old.next(pre, elapsed, now, hz);
    store(new);
    crate::mtrap::clock_changed(&old, &new);
    crate::bringup_pmp::clock_changed();
    (m, old, new, elapsed)
}
