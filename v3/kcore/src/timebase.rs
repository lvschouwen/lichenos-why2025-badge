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

/// `ticks` of a clock at `from_hz` as ticks of a clock at `to_hz`, rounded down: a period re-armed after a clock change
/// (#655). Split by whole `from_hz` so the product cannot overflow for any plausible rate (both at most 1 GHz).
pub const fn rescale(ticks: u64, from_hz: u64, to_hz: u64) -> u64 {
    if from_hz == 0 {
        return 0;
    }
    (ticks / from_hz) * to_hz + (ticks % from_hz) * to_hz / from_hz
}

/// One calibration of mtime (#655): its rate, the clock generation it belongs to, and where it starts on mtime and on
/// the kernel's monotonic microsecond clock. A clock change ends an epoch and starts the next; mtime itself does not
/// jump, only its rate changes, at a moment inside the switch that the kernel cannot see. So the time the switch took
/// comes from SYSTIMER, which keeps its rate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Epoch {
    pub hz: u64,
    /// Starts at 1 for the boot measurement; 0 is never a live generation.
    pub gen: u32,
    pub mtime0: u64,
    pub us0: u64,
}

impl Epoch {
    /// The first epoch, from the boot measurement: microsecond 0 at `mtime`.
    pub const fn boot(hz: u64, mtime: u64) -> Self {
        Epoch { hz, gen: 1, mtime0: mtime, us0: 0 }
    }

    /// The monotonic time at `mtime`, in microseconds, rounded down. An mtime before the epoch's start reads as the
    /// start (never earlier).
    pub const fn us_at(&self, mtime: u64) -> u64 {
        self.us0 + ticks_to_us(mtime.saturating_sub(self.mtime0), self.hz)
    }

    /// The mtime at monotonic time `us`, rounded down; a time before the epoch's start is its start.
    pub const fn mtime_at(&self, us: u64) -> u64 {
        self.mtime0 + us_to_ticks(us.saturating_sub(self.us0), self.hz)
    }

    /// The epoch after a clock change. `pre_mtime` was read just before the change, `elapsed_sys` is the SYSTIMER ticks
    /// from that read to the read of `mtime`, which follows the measurement of the new rate `hz`. The new epoch starts
    /// where this one stood at `pre_mtime` plus the SYSTIMER time of the switch, so the clock never goes back and does
    /// not count the switch at the wrong rate.
    pub const fn next(&self, pre_mtime: u64, elapsed_sys: u64, mtime: u64, hz: u64) -> Self {
        let gen = match self.gen.wrapping_add(1) {
            0 => 1,
            g => g,
        };
        Epoch { hz, gen, mtime0: mtime, us0: self.us_at(pre_mtime) + ticks_to_us(elapsed_sys, SYSTIMER_HZ) }
    }

    /// An mtime deadline of this epoch as the mtime of `next` at the same monotonic time. One that passed during the
    /// switch becomes `next`'s start: it fires once, at once, and is neither lost nor counted twice.
    pub const fn rearm(&self, next: &Epoch, deadline: u64) -> u64 {
        next.mtime_at(self.us_at(deadline))
    }
}

/// The measured CPU clock is at `want_mhz` within `tol_pct` (#655): bringup brings PSRAM up at 120 MHz only then, so
/// the CPU is never below the PSRAM clock. False for no measurement or a zero target.
pub const fn cpu_at(measured_mhz: Option<u32>, want_mhz: u32, tol_pct: u32) -> bool {
    match measured_mhz {
        Some(m) => want_mhz != 0 && (m.abs_diff(want_mhz) as u64) * 100 <= want_mhz as u64 * tol_pct as u64,
        None => false,
    }
}

/// A rate calibrated in clock generation `gen` (#655): `Us` values and PERF lines may use it only while the clock is
/// still in that generation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rate {
    pub hz: u64,
    pub gen: u32,
}

impl Rate {
    /// The rate if the clock is still in its generation, else 0: the conversions then give 0, never a wrong number.
    pub const fn at(&self, gen: u32) -> u64 {
        if self.gen == gen {
            self.hz
        } else {
            0
        }
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

    const MHZ40: u64 = 40_000_000;
    const MHZ360: u64 = 360_000_000;

    #[test]
    fn rescale_a_period_across_the_switch() {
        // #655: the 1000 us tick armed at 40 MHz is 40 000 ticks; at 360 MHz it must be 360 000, not 40 000 (111 us).
        assert_eq!(rescale(40_000, MHZ40, MHZ360), 360_000);
        assert_eq!(rescale(360_000, MHZ360, MHZ40), 40_000);
        assert_eq!(rescale(39_998_712, 39_998_712, MHZ360), MHZ360); // a whole second of a non-MHz rate
        assert_eq!(rescale(39_998, 39_998_712, MHZ360), 359_993); // rounded down
        assert_eq!(rescale(86_400 * MHZ360, MHZ360, 1_000_000_000), 86_400 * 1_000_000_000); // no overflow
        assert_eq!(rescale(5, 0, MHZ360), 0);
    }

    #[test]
    fn epoch_maps_both_ways() {
        let e = Epoch::boot(MHZ40, 1_000);
        assert_eq!(e.gen, 1);
        assert_eq!(e.us_at(1_000 + 40_000), 1_000);
        assert_eq!(e.mtime_at(1_000), 1_000 + 40_000);
        assert_eq!(e.us_at(0), 0); // before the start reads as the start
        assert_eq!(e.mtime_at(0), 1_000);
    }

    #[test]
    fn next_epoch_counts_the_switch_by_systimer() {
        // Boot at mtime 0 (40 MHz). The switch starts at 1 s and takes 250 ms by SYSTIMER; mtime meanwhile ran partly
        // at 360 MHz, so the after-read is far past what 40 MHz would give. The new epoch starts at 1.25 s regardless.
        let e = Epoch::boot(MHZ40, 0);
        let pre = MHZ40; // 1 s
        let post = pre + 50_000_000; // whatever mtime counted during the switch
        let n = e.next(pre, 4_000_000, post, MHZ360);
        assert_eq!(n.gen, 2);
        assert_eq!(n.hz, MHZ360);
        assert_eq!(n.us0, 1_250_000);
        assert_eq!(n.us_at(post + 360), 1_250_001);
        // The old epoch read at the same mtime would say 2.25 s: the mixed-rate ticks, counted at 40 MHz.
        assert_eq!(e.us_at(post), 2_250_000);
    }

    #[test]
    fn generation_skips_zero_on_wrap() {
        let e = Epoch { hz: MHZ40, gen: u32::MAX, mtime0: 0, us0: 0 };
        assert_eq!(e.next(0, 0, 0, MHZ360).gen, 1);
    }

    #[test]
    fn rearm_keeps_the_wall_time_of_a_deadline() {
        // A one-shot armed at 40 MHz for 3 s after boot; the switch runs from 1 s to 1.25 s.
        let e = Epoch::boot(MHZ40, 0);
        let deadline = 3 * MHZ40;
        let (pre, post) = (MHZ40, MHZ40 + 50_000_000);
        let n = e.next(pre, 4_000_000, post, MHZ360);
        let cmp = e.rearm(&n, deadline);
        // 1.75 s left after the switch, at 360 MHz.
        assert_eq!(cmp, post + 1_750_000 * 360);
        assert_eq!(n.us_at(cmp), 3_000_000);
        // Not re-armed, the old compare would fire after (3 s * 40 MHz - post) / 360 MHz = 0.19 s instead of 1.75 s.
        assert!(deadline - post < 1_750_000 * 360 / 9);
    }

    #[test]
    fn rearm_a_deadline_passed_in_the_switch_fires_once_at_once() {
        let e = Epoch::boot(MHZ40, 0);
        let deadline = MHZ40 + 4_000_000; // 1.1 s: inside the 1 s .. 1.25 s switch
        let n = e.next(MHZ40, 4_000_000, MHZ40 + 50_000_000, MHZ360);
        assert_eq!(e.rearm(&n, deadline), n.mtime0);
        // Already in the past at the old epoch's start too.
        assert_eq!(e.rearm(&n, 0), n.mtime0);
    }

    #[test]
    fn cpu_at_gates_psram_on_the_cpu_clock() {
        // review-655 M1: a switch that returned Ok but left the CPU on XTAL (or unmeasured) must not pass.
        assert!(cpu_at(Some(360), 360, 1));
        assert!(cpu_at(Some(357), 360, 1)); // 0.83 %
        assert!(!cpu_at(Some(356), 360, 1)); // 1.1 %
        assert!(!cpu_at(Some(40), 360, 1));
        assert!(!cpu_at(None, 360, 1));
        assert!(!cpu_at(Some(0), 0, 1));
    }

    #[test]
    fn rate_is_void_in_another_generation() {
        let r = Rate { hz: MHZ40, gen: 1 };
        assert_eq!(r.at(1), MHZ40);
        // After the switch the boot calibration gives 0, not a number 9 times off.
        assert_eq!(r.at(2), 0);
        assert_eq!(ticks_to_us100(72, r.at(2)), 0);
    }

    proptest! {
        #[test]
        fn round_trip_never_gains(us in 0u64..1_000_000_000_000, hz in 1_000_000u64..=1_000_000_000) {
            let back = ticks_to_us(us_to_ticks(us, hz), hz);
            prop_assert!(back <= us);
            // One tick is at most 1 us at any plausible rate, so at most 1 us is lost.
            prop_assert!(us - back <= 1);
        }

        #[test]
        fn time_is_monotonic_across_a_switch(
            boot in 0u64..1u64 << 40,
            before in 0u64..1u64 << 36,
            during in 0u64..1u64 << 36,
            sys in 0u64..1u64 << 36,
            after in 0u64..1u64 << 36,
            hz0 in 1_000_000u64..=1_000_000_000,
            hz1 in 1_000_000u64..=1_000_000_000,
        ) {
            let e = Epoch::boot(hz0, boot);
            let pre = boot + before;
            let post = pre + during;
            let n = e.next(pre, sys, post, hz1);
            // Nothing read before the switch is later than anything read after it.
            prop_assert!(n.us_at(post + after) >= e.us_at(pre));
            prop_assert!(n.us_at(post + after + 1) >= n.us_at(post + after));
            // The switch itself counts exactly its SYSTIMER time.
            prop_assert_eq!(n.us0 - e.us_at(pre), ticks_to_us(sys, SYSTIMER_HZ));
        }

        #[test]
        fn rearm_keeps_a_deadline_within_1_us_of_rounding(
            d in 0u64..1u64 << 40,
            sys in 0u64..1u64 << 30,
            hz0 in 1_000_000u64..=1_000_000_000,
            hz1 in 1_000_000u64..=1_000_000_000,
        ) {
            let e = Epoch::boot(hz0, 0);
            let pre = d / 2;
            let n = e.next(pre, sys, pre + 1000, hz1);
            let cmp = e.rearm(&n, d);
            prop_assert!(cmp >= n.mtime0);
            // In wall time it fires at the deadline, or at once if the deadline is gone; the loss is under 1 us of
            // rounding at each end.
            let want = e.us_at(d).max(n.us0);
            prop_assert!(n.us_at(cmp) <= want && want - n.us_at(cmp) <= 1);
        }
    }
}
