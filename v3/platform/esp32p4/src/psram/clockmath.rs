//! Pure arithmetic of the CPU clock bring-up (probe-psram/src/clock.rs), kept here so the host tests cover it.

/// dbias from the eFuse field and IDF's rule (`pmu_param.c` get_act_hp_dbias / get_act_lp_dbias): 0 = not calibrated, use
/// the default; else field + `add` (16 for HP, 20 for LP), at most 31.
pub const fn dbias(efuse_field: u32, add: u32, default: u32) -> u32 {
    if efuse_field == 0 {
        default
    } else if efuse_field + add > 31 {
        31
    } else {
        efuse_field + add
    }
}

/// Gate of `get_act_*_dbias`: block version >= 2 and not 100.
pub const fn dbias_calibrated(blk_version: u32) -> bool {
    blk_version >= 2 && blk_version != 100
}

/// CPU MHz from mcycle cycles counted over `ticks` SYSTIMER ticks (16 MHz, the XTAL-derived timer: independent of the CPU
/// clock, unlike the CLINT mtime which runs at the CPU clock on this chip).
pub fn mhz_from_systimer(cycles: u32, ticks: u32) -> u32 {
    if ticks == 0 {
        return 0;
    }
    (cycles as u64 * 16 / ticks as u64) as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dbias_rule_follows_pmu_param() {
        assert_eq!(dbias(0, 16, 24), 24);
        assert_eq!(dbias(5, 16, 24), 21);
        assert_eq!(dbias(15, 16, 24), 31);
        assert_eq!(dbias(10, 20, 29), 30);
        assert_eq!(dbias(12, 20, 29), 31);
        assert!(dbias_calibrated(3) && dbias_calibrated(102) && !dbias_calibrated(100) && !dbias_calibrated(1));
    }

    #[test]
    fn mhz_from_systimer_ticks() {
        assert_eq!(mhz_from_systimer(360_000, 16_000), 360); // 1 ms
        assert_eq!(mhz_from_systimer(40_000, 16_000), 40);
        assert_eq!(mhz_from_systimer(1, 0), 0);
    }
}
