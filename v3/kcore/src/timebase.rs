//! The CLINT mtime rate as a runtime value (#654). The P4's mtime does not run at a fixed rate: #608 run 9 measured
//! 40 MHz with the CPU on the ROM's XTAL clock, the #648 perf lab 360 MHz with the CPU at 360. So the kernel measures
//! it against a fixed reference (SYSTIMER, 16 MHz) and every tick conversion takes the rate in Hz. Pure; u64 only (no
//! u128 division on riscv32).

/// SYSTIMER unit 0 on the P4: XTAL / 2.5, independent of the CPU clock.
pub const SYSTIMER_HZ: u64 = 16_000_000;

/// The range a measured mtime rate must fall in to be believed: from a slow-clock XTAL fraction up past the 400 MHz
/// bench experiment.
pub const PLAUSIBLE_HZ: core::ops::RangeInclusive<u64> = 1_000_000..=1_000_000_000;

const US_PER_S: u64 = 1_000_000;

/// Microseconds to ticks at `hz`, rounded down. Split by whole seconds so `us * hz` cannot overflow for any u64 `us`
/// whose result fits.
pub const fn us_to_ticks(us: u64, hz: u64) -> u64 {
    (us / US_PER_S) * hz + (us % US_PER_S) * hz / US_PER_S
}

/// Ticks at `hz` to microseconds, rounded down (0 for an unknown rate).
pub const fn ticks_to_us(ticks: u64, hz: u64) -> u64 {
    if hz == 0 {
        return 0;
    }
    (ticks / hz) * US_PER_S + (ticks % hz) * US_PER_S / hz
}

/// Ticks at `hz` as hundredths of a microsecond, rounded to the nearest (0 for an unknown rate).
pub const fn ticks_to_us100(ticks: u64, hz: u64) -> u64 {
    if hz == 0 {
        return 0;
    }
    let whole = (ticks / hz) * 100 * US_PER_S;
    whole + ((ticks % hz) * 100 * US_PER_S + hz / 2) / hz
}

/// The rate of a counter that advanced `ticks` while a reference at `ref_hz` advanced `ref_ticks`, rounded to the
/// nearest Hz. None for an empty window or a product that would overflow (a window far longer than any calibration).
pub const fn rate_hz(ticks: u64, ref_ticks: u64, ref_hz: u64) -> Option<u64> {
    if ref_ticks == 0 {
        return None;
    }
    let Some(p) = ticks.checked_mul(ref_hz) else { return None };
    match p.checked_add(ref_ticks / 2) {
        Some(r) => Some(r / ref_ticks),
        None => None,
    }
}

/// The mtime rate from an mtime and a SYSTIMER delta over the same window, if it is plausible.
pub fn mtime_hz_from_systimer(mtime_ticks: u64, systimer_ticks: u64) -> Option<u64> {
    rate_hz(mtime_ticks, systimer_ticks, SYSTIMER_HZ).filter(|hz| PLAUSIBLE_HZ.contains(hz))
}

/// The P4's XTAL, which mtime counts while the CPU runs from it (#608 run 9).
pub const XTAL_HZ: u64 = 40_000_000;
/// How close a measured mtime rate must be to a candidate clock to count as that clock.
pub const SOURCE_TOL_PCT: u64 = 3;

/// Which clock a measured mtime rate matches (#654: does mtime follow the CPU clock or stay on XTAL?).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MtimeSource {
    /// About XTAL, and the CPU is not (mtime stays on XTAL).
    Xtal,
    /// About the CPU clock, and that is not XTAL (mtime follows the CPU).
    Cpu,
    /// The CPU runs at about XTAL itself (the ROM clock), so the run cannot tell the two apart.
    XtalOrCpu,
}

const fn near(a: u64, b: u64, pct: u64) -> bool {
    b != 0 && a.abs_diff(b) * 100 <= b * pct
}

/// Match `mtime_hz` against XTAL and against `cpu_hz` (measured independently, mcycle against SYSTIMER; 0 if
/// unknown). None when it matches neither: the rate is then not believed.
pub const fn mtime_source(mtime_hz: u64, cpu_hz: u64) -> Option<MtimeSource> {
    let xtal = near(mtime_hz, XTAL_HZ, SOURCE_TOL_PCT);
    let cpu = near(mtime_hz, cpu_hz, SOURCE_TOL_PCT);
    match (xtal, cpu) {
        (true, true) => Some(MtimeSource::XtalOrCpu),
        (true, false) => Some(MtimeSource::Xtal),
        (false, true) => Some(MtimeSource::Cpu),
        (false, false) => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    #[test]
    fn conversions_at_360_mhz() {
        let hz = 360_000_000;
        assert_eq!(us_to_ticks(1000, hz), 360_000);
        assert_eq!(ticks_to_us(360_000, hz), 1000);
        assert_eq!(ticks_to_us(359, hz), 0);
        assert_eq!(ticks_to_us100(360, hz), 100);
        assert_eq!(ticks_to_us100(18, hz), 5); // 50 ns
        assert_eq!(ticks_to_us100(1, hz), 0); // 2.8 ns rounds to 0.00
    }

    #[test]
    fn conversions_at_40_mhz_as_before() {
        let hz = 40_000_000;
        assert_eq!(us_to_ticks(1000, hz), 40_000);
        assert_eq!(ticks_to_us(40_000, hz), 1000);
        assert_eq!(ticks_to_us100(1, hz), 3); // 25 ns rounds to 0.03
        assert_eq!(ticks_to_us100(72, hz), 180);
    }

    #[test]
    fn a_rate_that_is_not_whole_mhz() {
        // An integer ticks-per-microsecond would truncate this to 39 and be 2.5% off.
        let hz = 39_998_712;
        assert_eq!(us_to_ticks(1_000_000, hz), hz);
        assert_eq!(us_to_ticks(1000, hz), 39_998);
        assert_eq!(ticks_to_us(hz, hz), 1_000_000);
        assert_eq!(ticks_to_us(39_998_712 * 3600, hz), 3_600_000_000);
    }

    #[test]
    fn unknown_rate_converts_to_zero() {
        assert_eq!(ticks_to_us(5, 0), 0);
        assert_eq!(ticks_to_us100(5, 0), 0);
    }

    #[test]
    fn large_values_do_not_overflow() {
        // 24 h at 400 MHz: us * hz alone would overflow u64.
        let us = 86_400 * US_PER_S;
        assert_eq!(us_to_ticks(us, 400_000_000), 86_400 * 400_000_000);
        assert_eq!(ticks_to_us(86_400 * 400_000_000, 400_000_000), us);
        assert_eq!(ticks_to_us100(86_400 * 400_000_000, 400_000_000), us * 100);
    }

    #[test]
    fn systimer_ratio() {
        // #608 run 9: 2.5 mtime ticks per SYSTIMER tick.
        assert_eq!(mtime_hz_from_systimer(80_000, 32_000), Some(40_000_000));
        // #648 perf lab: 22.5 per SYSTIMER tick.
        assert_eq!(mtime_hz_from_systimer(720_000, 32_000), Some(360_000_000));
        // Rounded to the nearest Hz, not truncated.
        assert_eq!(rate_hz(2, 3, 1), Some(1));
        assert_eq!(rate_hz(1, 3, 1), Some(0));
        assert_eq!(mtime_hz_from_systimer(1, 0), None);
        assert_eq!(mtime_hz_from_systimer(0, 32_000), None); // a stopped mtime
        assert_eq!(mtime_hz_from_systimer(u64::MAX, 32_000), None); // overflow
        assert_eq!(mtime_hz_from_systimer(4_000_000, 32_000), None); // 2 GHz: not believed
    }

    #[test]
    fn mtime_source_names_the_clock() {
        // This bench (ROM clock): mtime 39 998 503 Hz, mcycle 39 MHz (measure_mhz floors).
        assert_eq!(mtime_source(39_998_503, 39_000_000), Some(MtimeSource::XtalOrCpu));
        // #648: mtime 360 MHz with the CPU at 360.
        assert_eq!(mtime_source(360_000_000, 360_000_000), Some(MtimeSource::Cpu));
        // The other outcome #654 allows: mtime on XTAL while the CPU runs at 360.
        assert_eq!(mtime_source(40_000_000, 360_000_000), Some(MtimeSource::Xtal));
        // mcycle not counting (0): XTAL can still be recognised.
        assert_eq!(mtime_source(40_000_000, 0), Some(MtimeSource::Xtal));
        // Plausible but neither: not believed.
        assert_eq!(mtime_source(100_000_000, 360_000_000), None);
        assert_eq!(mtime_source(380_000_000, 360_000_000), None); // 5.6% off
        assert_eq!(mtime_source(370_000_000, 360_000_000), Some(MtimeSource::Cpu)); // 2.8%
        assert_eq!(mtime_source(0, 0), None);
    }

    #[test]
    fn rate_rounding_cannot_overflow() {
        // ticks * ref_hz fits, the rounding add would not.
        assert_eq!(rate_hz(u64::MAX, 2, 1), None);
        assert_eq!(rate_hz(u64::MAX - 1, 4, 1), None);
    }

    proptest! {
        #[test]
        fn round_trip_never_gains(us in 0u64..1_000_000_000_000, hz in 1_000_000u64..=1_000_000_000) {
            let back = ticks_to_us(us_to_ticks(us, hz), hz);
            prop_assert!(back <= us);
            // One tick is at most 1 us at any plausible rate, so at most 1 us is lost.
            prop_assert!(us - back <= 1);
        }
    }
}
