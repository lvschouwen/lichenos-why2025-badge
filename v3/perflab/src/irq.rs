//! Timer and interrupt measurements on the CLINT timer. Its tick rate is measured, not assumed: #608 found 40 MHz with the CPU on the
//! crystal, and the SYSTIMER (16 MHz, XTAL-derived) shows it counts faster once the CPU is on the PLL (#648).
use crate::rate::ppm_diff;
use crate::stats::{summarize, Summary};
use lichen_kcore::perf::jitter_max;

/// Nanoseconds for CLINT ticks at `hz`.
pub const fn ticks_to_ns(t: u64, hz: u64) -> u64 {
    if hz == 0 {
        return 0;
    }
    t * 1_000_000_000 / hz
}

/// The CLINT counter's rate from its ticks and the SYSTIMER ticks (16 MHz) over the same interval.
pub const fn mtime_hz(mtime_ticks: u64, systimer_ticks: u64) -> u64 {
    if systimer_ticks == 0 {
        return 0;
    }
    mtime_ticks * 16_000_000 / systimer_ticks
}

/// Result of an interrupt run: the latency of each interrupt (entry time minus the programmed time) and the gap between entries.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IrqStats {
    pub latency: Summary,
    pub gap_min: u32,
    pub gap_max: u32,
    /// Worst distance of a gap from the nominal period, in ticks.
    pub jitter: u64,
}

/// `latency` and `gaps` are in ticks, `period` is the programmed one. Sorts `latency` in place. `None` for no samples.
pub fn irq_stats(latency: &mut [u32], gaps: &[u32], period: u64) -> Option<IrqStats> {
    let lat = summarize(latency)?;
    let (gap_min, gap_max) = (*gaps.iter().min()?, *gaps.iter().max()?);
    Some(IrqStats { latency: lat, gap_min, gap_max, jitter: jitter_max(period, gap_min as u64, gap_max as u64) })
}

/// Parts per million that a measured rate is away from a nominal one.
pub fn ppm_vs(hz: u64, nominal_hz: u64) -> i64 {
    ppm_diff(hz, nominal_hz)
}

/// The CPU clock in hundredths of MHz from a cycle delta and a SYSTIMER delta (16 MHz).
pub fn cpu_mhz_x100(cycles: u64, systimer_ticks: u64) -> u64 {
    if systimer_ticks == 0 {
        return 0;
    }
    (cycles as u128 * 16 * 100 / systimer_ticks as u128) as u64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ticks_follow_the_measured_rate() {
        assert_eq!(ticks_to_ns(1, 40_000_000), 25);
        assert_eq!(ticks_to_ns(40, 40_000_000), 1000);
        assert_eq!(ticks_to_ns(43, 43_200_000), 995);
        assert_eq!(ticks_to_ns(5, 0), 0);
    }

    #[test]
    fn latency_and_jitter() {
        // period 40 000 ticks (1 ms): gaps 39 928 to 40 002 are the #612 checkpoint-1 figures, 72 ticks
        let mut lat = [10u32, 12, 11, 40, 13, 12, 11, 10, 12, 11];
        let gaps = [40_000u32, 39_928, 40_002, 40_000];
        let s = irq_stats(&mut lat, &gaps, 40_000).unwrap();
        assert_eq!((s.latency.min, s.latency.max, s.latency.median), (10, 40, 12));
        assert_eq!((s.gap_min, s.gap_max, s.jitter), (39_928, 40_002, 72));
        assert_eq!(irq_stats(&mut [], &gaps, 40_000), None);
        assert_eq!(irq_stats(&mut [1], &[], 40_000), None);
    }

    #[test]
    fn clock_rates_against_the_systimer() {
        // 100 ms: the SYSTIMER counts 1 600 000; a 43.2 MHz counter counts 4 320 000 (the figure measured at CPU 360 MHz)
        assert_eq!(mtime_hz(4_320_000, 1_600_000), 43_200_000);
        assert_eq!(mtime_hz(4_000_000, 1_600_000), 40_000_000);
        assert_eq!(mtime_hz(1, 0), 0);
        assert_eq!(ppm_vs(43_200_000, 40_000_000), 80_000);
        assert_eq!(ppm_vs(39_996_000, 40_000_000), -100);
        // 360 MHz for 100 ms
        assert_eq!(cpu_mhz_x100(36_000_000, 1_600_000), 36_000);
        assert_eq!(cpu_mhz_x100(1, 0), 0);
    }
}
