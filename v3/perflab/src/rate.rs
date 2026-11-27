//! Rates from SYSTIMER ticks (16 MHz, XTAL-derived: it does not move with the CPU clock, so 360 and 400 MHz runs compare).
//! 128-bit intermediates: 4 GB at 16 MHz does not fit 64 bits once scaled.

/// MB/s in hundredths (1 MB = 10^6 bytes, as the probe-psram perf lines and the v2 `fw sd test` use it).
pub fn mb_s_x100(bytes: u64, ticks: u64, tick_hz: u64) -> u64 {
    if ticks == 0 {
        return 0;
    }
    (bytes as u128 * 100 * tick_hz as u128 / (ticks as u128 * 1_000_000)) as u64
}

/// Nanoseconds per operation in hundredths.
pub fn ns_x100(ticks: u64, ops: u64, tick_hz: u64) -> u64 {
    if ops == 0 || tick_hz == 0 {
        return 0;
    }
    (ticks as u128 * 1_000_000_000 * 100 / (ops as u128 * tick_hz as u128)) as u64
}

/// Millions of operations per second in hundredths.
pub fn mops_x100(ops: u64, ticks: u64, tick_hz: u64) -> u64 {
    if ticks == 0 {
        return 0;
    }
    (ops as u128 * 100 * tick_hz as u128 / (ticks as u128 * 1_000_000)) as u64
}

/// `part` of `whole` in hundredths of a percent, clamped to 100.00 %.
pub fn pct_x100(part: u64, whole: u64) -> u64 {
    if whole == 0 {
        return 0;
    }
    ((part as u128 * 10_000 / whole as u128).min(10_000)) as u64
}

/// Parts per million that `a` is away from `b` (signed; 0 when `b` is 0).
pub fn ppm_diff(a: u64, b: u64) -> i64 {
    if b == 0 {
        return 0;
    }
    ((a as i128 - b as i128) * 1_000_000 / b as i128) as i64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn throughput() {
        // 4 MiB in 1/20 s at 16 MHz = 800 000 ticks -> 83.89 MB/s
        assert_eq!(mb_s_x100(4 << 20, 800_000, 16_000_000), 8388);
        assert_eq!(mb_s_x100(1, 0, 16_000_000), 0);
        // no overflow for a whole 4 GB buffer
        assert_eq!(mb_s_x100(u32::MAX as u64, 16_000_000, 16_000_000), 429_496);
    }

    #[test]
    fn latency_and_rates() {
        // 160 ticks for 1 operation at 16 MHz = 10 us
        assert_eq!(ns_x100(160, 1, 16_000_000), 1_000_000);
        // 1 000 000 hops in 16 000 000 ticks (1 s) = 1000 ns each
        assert_eq!(ns_x100(16_000_000, 1_000_000, 16_000_000), 100_000);
        assert_eq!(ns_x100(5, 0, 16_000_000), 0);
        assert_eq!(mops_x100(2_000_000, 16_000_000, 16_000_000), 200);
    }

    #[test]
    fn percentages_and_ppm() {
        assert_eq!(pct_x100(1, 3), 3333);
        assert_eq!(pct_x100(5, 2), 10_000); // clamped
        assert_eq!(pct_x100(1, 0), 0);
        assert_eq!(ppm_diff(1_000_100, 1_000_000), 100);
        assert_eq!(ppm_diff(999_900, 1_000_000), -100);
        assert_eq!(ppm_diff(1, 0), 0);
    }
}
