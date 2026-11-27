//! The measured CLINT mtime rate (#654). The hypothesis of #654 is that mtime follows the CPU clock on the P4 (40 MHz
//! on the ROM's XTAL in #608 run 9, 360 MHz at CPU 360 in #648); the `timebase.mtime` row tests it. Either way the rate
//! is measured against SYSTIMER at boot rather than assumed, and every tick conversion in the kernel goes through here. An `AtomicU32` (riscv32imac has no
//! 64-bit atomics) because core 1 converts too; it is written before core 1 starts.
use crate::platform::hw;
use core::sync::atomic::{AtomicU32, Ordering};
use lichen_kcore::timebase;

/// Used only when the measurement fails, so the run can still finish and report: the rate #608 run 9 measured with
/// the CPU on the ROM's 40 MHz XTAL. The `timebase.mtime` row is then FAIL; no number derived from it is trusted.
const UNMEASURED_HZ: u32 = 40_000_000;

static HZ: AtomicU32 = AtomicU32::new(UNMEASURED_HZ);

/// The current mtime rate in Hz.
pub fn hz() -> u64 {
    HZ.load(Ordering::Relaxed) as u64
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

/// Measure the mtime rate against SYSTIMER (about 2 ms, busy) and store it if it is plausible. Run at boot and again
/// after ANY change of the CPU clock (`cpu_to_360` and friends). Periods already armed and deadlines already computed
/// stay in ticks of the old rate: the caller stops and re-arms them after this returns.
pub fn remeasure() -> Measured {
    let (mtime_ticks, systimer_ticks) = hw::clint::measure_window().unwrap_or((0, 0));
    let hz = timebase::mtime_hz_from_systimer(mtime_ticks, systimer_ticks);
    if let Some(f) = hz {
        // PLAUSIBLE_HZ ends at 1 GHz, so the value fits in u32.
        HZ.store(f as u32, Ordering::Relaxed);
    }
    Measured { hz, mtime_ticks, systimer_ticks }
}
