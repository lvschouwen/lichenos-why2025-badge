//! Percentiles and time conversion for the PERF lines (#612). Pure.

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

#[cfg(test)]
mod tests {
    use super::*;

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
}
