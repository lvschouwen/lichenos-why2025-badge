//! Percentiles and time conversion for the PERF lines (#612). Pure. Checkpoint 3 adds timer jitter and microseconds
//! with two decimals, since a polled cross-core round trip is near 1 us at full clock.
use crate::timebase;
use core::fmt;

/// Median and 95th percentile (nearest rank) of `v`, sorting it in place. None for an empty slice.
pub fn median_p95(v: &mut [u32]) -> Option<(u32, u32)> {
    if v.is_empty() {
        return None;
    }
    v.sort_unstable();
    let n = v.len();
    let p95 = v[(n * 95).div_ceil(100) - 1];
    Some((v[n / 2], p95))
}

/// Nanoseconds for `ticks` at `hz`, rounded down (u64: no overflow for any u32 tick count at any plausible clock).
pub fn ticks_to_ns(ticks: u32, hz: u64) -> u64 {
    if hz == 0 {
        return 0;
    }
    ticks as u64 * 1_000_000_000 / hz
}

/// CPU clock in Hz from a cycle delta and a delta of a reference timebase running at `ref_hz`.
pub fn clock_hz(cycles: u32, ref_ticks: u32, ref_hz: u64) -> u64 {
    if ref_ticks == 0 {
        return 0;
    }
    cycles as u64 * ref_hz / ref_ticks as u64
}

/// The worst distance of a tick gap from the nominal period: max(period - gap_min, gap_max - period).
pub fn jitter_max(period: u64, gap_min: u64, gap_max: u64) -> u64 {
    period.saturating_sub(gap_min).max(gap_max.saturating_sub(period))
}

/// mtime ticks at the measured mtime rate (Hz, #654) shown as microseconds with two decimals (`1.85`), rounded to
/// the nearest hundredth.
#[derive(Clone, Copy)]
pub struct Us {
    pub ticks: u64,
    pub hz: u64,
}
impl fmt::Display for Us {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        Us100(timebase::ticks_to_us100(self.ticks, self.hz)).fmt(f)
    }
}

/// Hundredths of a microsecond, shown as `x.yy`.
#[derive(Clone, Copy)]
pub struct Us100(pub u64);
impl fmt::Display for Us100 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{:02}", self.0 / 100, self.0 % 100)
    }
}

/// CPU cycles at `hz` as hundredths of a microsecond, rounded (0 for an unknown clock). The cross-core round trip
/// is a few hundred cycles, too few for mtime's resolution at the ROM clock (25 ns at 40 MHz), so the RTT row counts
/// mcycle.
pub fn cycles_to_us100(cycles: u32, hz: u64) -> u64 {
    if hz == 0 {
        return 0;
    }
    (cycles as u64 * 100_000_000 + hz / 2) / hz
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::format;

    #[test]
    fn percentiles_use_nearest_rank() {
        assert_eq!(median_p95(&mut []), None);
        assert_eq!(median_p95(&mut [7]), Some((7, 7)));
        let mut v: [u32; 20] = core::array::from_fn(|i| (20 - i) as u32);
        assert_eq!(median_p95(&mut v), Some((11, 19)));
        let mut v: [u32; 100] = core::array::from_fn(|i| i as u32 + 1);
        assert_eq!(median_p95(&mut v), Some((51, 95)));
    }

    #[test]
    fn time_and_clock_conversion() {
        assert_eq!(ticks_to_ns(40, 40_000_000), 1000);
        assert_eq!(ticks_to_ns(u32::MAX, 40_000_000), 107_374_182_375);
        assert_eq!(clock_hz(400_000, 40_000, 40_000_000), 400_000_000);
        assert_eq!(clock_hz(1, 0, 40_000_000), 0);
    }
    #[test]
    fn jitter_from_the_checkpoint_1_run() {
        // gap min/max 39928/40002 at a 40000 period: 72 ticks = 1.80 us at the 40 MHz that run measured.
        assert_eq!(jitter_max(40_000, 39_928, 40_002), 72);
        assert_eq!(format!("{}", Us { ticks: 72, hz: 40_000_000 }), "1.80");
        assert_eq!(jitter_max(100, 100, 100), 0);
    }
    #[test]
    fn cycles_as_microseconds() {
        assert_eq!(cycles_to_us100(360, 360_000_000), 100); // 1 us at 360 MHz
        assert_eq!(cycles_to_us100(36, 360_000_000), 10);
        assert_eq!(cycles_to_us100(u32::MAX, 40_000_000), 10_737_418_238); // 107.37 s
        assert_eq!(cycles_to_us100(5, 0), 0);
        assert_eq!(format!("{}", Us100(1234)), "12.34");
    }
    #[test]
    fn formats_microseconds() {
        let at = |ticks, hz| format!("{}", Us { ticks, hz });
        assert_eq!(at(0, 40_000_000), "0.00");
        assert_eq!(at(1, 40_000_000), "0.03"); // 25 ns rounds to 0.03
        assert_eq!(at(40, 40_000_000), "1.00");
        assert_eq!(at(2000, 40_000_000), "50.00");
        assert_eq!(at(41, 40_000_000), "1.03");
        // At 360 MHz the same tick counts are nine times shorter.
        assert_eq!(at(360, 360_000_000), "1.00");
        assert_eq!(at(2000, 360_000_000), "5.56");
        assert_eq!(at(5, 0), "0.00");
    }
}
