//! The PSRAM rail: LDO channel 2 (VO2) at 1800 mV, ported from ESP-IDF v6.1 `esp_ldo_acquire_channel` and `ldo_ll.h`.
use crate::mmio::read32;

/// The PSRAM rail as v2 runs it: LDO channel 2 (VO2, "unit" 1 in the registers) at 1800 mV. Source: ESP-IDF v6.1
/// `components/esp_hw_support/port/esp32p4/Kconfig.ldo` defaults ESP_LDO_CHAN_PSRAM_DOMAIN = 2 and
/// ESP_LDO_VOLTAGE_PSRAM_DOMAIN = 1800, ESP_LDO_VOLTAGE_STABLE_DELAY_US = 200; v2's `firmware/sdkconfig` does not
/// override them (generated lines 2563-2569). IDF applies them in `esp_clk_tree_mpll_acquire()`,
/// `components/esp_hw_support/port/esp_clk_tree_common.c:365-372`.
pub const LDO_PSRAM_CHANNEL: u32 = 2;
pub const LDO_PSRAM_UNIT: u32 = LDO_PSRAM_CHANNEL - 1;
pub const LDO_PSRAM_MV: i32 = 1800;
pub const LDO_STABLE_DELAY_US: u32 = 200;

/// The eFuse fields `ldo_ll_voltage_to_dref_mul(unit 1, 1800 mV)` consults.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LdoEfuse {
    /// `efuse_hal_blk_version()`: BLK_VERSION_MAJOR * 100 + BLK_VERSION_MINOR.
    pub blk_version: u32,
    /// `EFUSE.rd_mac_sys_2.ldo_vo2_dref` (4 bits).
    pub vo2_dref: u8,
    /// `EFUSE.rd_mac_sys_3.ldo_vo2_mul` (3 bits).
    pub vo2_mul: u8,
}

/// Decode the two read registers. Fields: rd_mac_sys_2 BLK_VERSION_MINOR [10:8], BLK_VERSION_MAJOR [12:11],
/// LDO_VO2_DREF [31:28]; rd_mac_sys_3 LDO_VO2_MUL [5:3] (hw_ver1 `efuse_struct.h`).
pub fn decode_ldo_efuse(rd_mac_sys_2: u32, rd_mac_sys_3: u32) -> LdoEfuse {
    let minor = (rd_mac_sys_2 >> 8) & 0x7;
    let major = (rd_mac_sys_2 >> 11) & 0x3;
    LdoEfuse {
        blk_version: major * 100 + minor,
        vo2_dref: ((rd_mac_sys_2 >> 28) & 0xF) as u8,
        vo2_mul: ((rd_mac_sys_3 >> 3) & 0x7) as u8,
    }
}

const EFUSE: usize = 0x5012_D000; // DR_REG_EFUSE_BASE
const EFUSE_RD_MAC_SYS_2: usize = EFUSE + 0x4C;
const EFUSE_RD_MAC_SYS_3: usize = EFUSE + 0x50;

/// Read the LDO calibration fields. This is the only eFuse access of the PSRAM bring-up and it is read-only.
///
/// # Safety
/// Reads exactly two registers of the eFuse controller, `rd_mac_sys_2` and `rd_mac_sys_3` (the BLOCK1 read-data
/// registers that the hardware fills at reset). No write, no read trigger, no timing or clock change: nothing here
/// touches CMD, CONF, PGM_DATA or PGM_CHECK_VALUE. The owner allowed exactly this read (#620, 2026-11-27).
pub unsafe fn read_ldo_efuse() -> LdoEfuse {
    decode_ldo_efuse(read32(EFUSE_RD_MAC_SYS_2), read32(EFUSE_RD_MAC_SYS_3))
}

/// What `ldo_ll_voltage_to_dref_mul` returns: (dref, mul). For unit 1 IDF uses K = 1, Vos = 0, C = 1 (the eFuse
/// calibration constants are only loaded for units 2 and 3) and picks the (dref, mul) whose Vout is closest to the
/// request; at exactly 1800 mV a block version >= 1 with both calibrated fields non-zero overrides that pick.
pub fn ldo_unit1_dref_mul(voltage_mv: i32, e: &LdoEfuse) -> (u8, u8) {
    // Fixed point as IDF: Vref in units of 1/20 V, Vout in units of 1/80_000_000 V.
    let mut best = (0u8, 0u8);
    let mut min_diff: i32 = 400_000_000;
    for dref in 0u8..16 {
        let vref_20 = if dref < 9 { 10 + dref as i32 } else { 20 + (dref as i32 - 9) * 2 };
        for mul in 0u8..8 {
            let vout = (vref_20 * 1000) * (4000 + mul as i32 * 1000);
            let diff = (voltage_mv * 80_000 - vout).abs();
            if diff < min_diff {
                min_diff = diff;
                best = (dref, mul);
            }
        }
    }
    if e.blk_version >= 1 && voltage_mv == 1800 && e.vo2_dref != 0 && e.vo2_mul != 0 {
        best = (e.vo2_dref, e.vo2_mul);
    }
    best
}

const PMU: usize = 0x5011_5000; // DR_REG_PMU_BASE
/// `PMU.ext_ldo[index]`, two words each (pmu_ext_ldo, pmu_ext_ldo_ana); VO2 is index 3 (`ldo_ll` index_array {0,3,1,4}).
const PMU_EXT_LDO: usize = PMU + 0x1B8;
const LDO_INDEX_VO2: usize = 3;

/// Raw `pmu_ext_ldo` and `pmu_ext_ldo_ana` of the PSRAM rail's LDO.
pub fn ldo_psram_state() -> (u32, u32) {
    let a = PMU_EXT_LDO + LDO_INDEX_VO2 * 8;
    // SAFETY: PMU ext_ldo registers of VO2, reads without side effects.
    unsafe { (read32(a), read32(a + 4)) }
}

/// Fields of the two LDO words: (xpd, force_tieh_sel, tieh_sel, tieh, dref, mul, en_cur_lim, en_vdet).
pub fn ldo_fields(ldo: u32, ana: u32) -> [u32; 8] {
    [
        (ldo >> 8) & 1,
        (ldo >> 7) & 1,
        (ldo >> 9) & 7,
        (ldo >> 14) & 1,
        (ana >> 28) & 0xF,
        (ana >> 23) & 7,
        (ana >> 27) & 1,
        (ana >> 26) & 1,
    ]
}

/// Power the PSRAM rail as `esp_ldo_acquire_channel` does for a software-owned, non-adjustable channel: current limit
/// on, voltage, owner = software, ripple suppression on, enable, current limit off, then wait the stable delay.
/// `delay_us` is the caller's microsecond wait.
///
/// # Safety
/// Reconfigures the PSRAM supply: only call once, with (dref, mul) from `ldo_unit1_dref_mul` for 1800 mV.
pub unsafe fn ldo_psram_enable(dref: u8, mul: u8, delay_us: fn(u32)) {
    let a = PMU_EXT_LDO + LDO_INDEX_VO2 * 8;
    let (ldo, ana) = (a, a + 4);
    const EN_CUR_LIM: u32 = 1 << 27;
    const EN_VDET: u32 = 1 << 26;
    crate::mmio::modify32(ana, 0, EN_CUR_LIM); // ldo_ll_enable_current_limit(true)
    crate::mmio::modify32(ldo, 1 << 14, 0); // tieh = 0: Vref * Mul, not the 3.3 V rail
    crate::mmio::modify32(ana, (0xF << 28) | (7 << 23), ((dref as u32) << 28) | ((mul as u32) << 23));
    crate::mmio::modify32(ldo, 1 << 7, 1 << 7); // force_tieh_sel = 1: owned by software
    crate::mmio::modify32(ldo, 7 << 9, 0); // tieh_sel = 0
    crate::mmio::modify32(ana, 0, EN_VDET); // ripple suppression
    crate::mmio::modify32(ldo, 0, 1 << 8); // xpd
    crate::mmio::modify32(ana, EN_CUR_LIM, 0); // current limit off
    delay_us(LDO_STABLE_DELAY_US);
}

#[cfg(test)]
mod tests {
    use super::*;

    const NO_FUSE: LdoEfuse = LdoEfuse { blk_version: 0, vo2_dref: 0, vo2_mul: 0 };

    #[test]
    fn dref_mul_matches_the_idf_search() {
        // Golden values from IDF's loop (K = 1, Vos = 0, C = 1): Vref = 0.5 + 0.05 * dref (dref < 9) else
        // 1 + 0.1 * (dref - 9); Vout = Vref * (1 + 0.25 * mul).
        assert_eq!(ldo_unit1_dref_mul(1800, &NO_FUSE), (6, 5));
        assert_eq!(ldo_unit1_dref_mul(1200, &NO_FUSE), (2, 4));
        assert_eq!(ldo_unit1_dref_mul(2500, &NO_FUSE), (9, 6));
        assert_eq!(ldo_unit1_dref_mul(1000, &NO_FUSE), (0, 4));
    }

    #[test]
    fn calibrated_fields_override_only_at_1800_with_a_block_version() {
        let cal = LdoEfuse { blk_version: 1, vo2_dref: 7, vo2_mul: 5 };
        assert_eq!(ldo_unit1_dref_mul(1800, &cal), (7, 5));
        // block version 0: ignored
        assert_eq!(ldo_unit1_dref_mul(1800, &LdoEfuse { blk_version: 0, ..cal }), (6, 5));
        // either field zero: the fallback stays
        assert_eq!(ldo_unit1_dref_mul(1800, &LdoEfuse { vo2_dref: 0, ..cal }), (6, 5));
        assert_eq!(ldo_unit1_dref_mul(1800, &LdoEfuse { vo2_mul: 0, ..cal }), (6, 5));
        // other voltages: never
        assert_eq!(ldo_unit1_dref_mul(1200, &cal), (2, 4));
    }

    #[test]
    fn efuse_fields_decode() {
        // major 1, minor 2 -> 102; dref 0xA in [31:28]; mul 5 in [5:3] of the other word
        let e = decode_ldo_efuse((0xA << 28) | (1 << 11) | (2 << 8), 5 << 3);
        assert_eq!(e, LdoEfuse { blk_version: 102, vo2_dref: 0xA, vo2_mul: 5 });
        // neighbouring fields do not leak in
        let e = decode_ldo_efuse(0x0FFF_E0FF, 0xFFFF_FFC7);
        assert_eq!((e.vo2_dref, e.vo2_mul, e.blk_version), (0, 0, 0));
    }

    #[test]
    fn ldo_fields_decode() {
        // xpd bit 8, force_tieh_sel bit 7, tieh_sel [11:9], tieh bit 14; dref [31:28], mul [25:23]...
        let f = ldo_fields((1 << 8) | (1 << 7) | (3 << 9) | (1 << 14), (6 << 28) | (5 << 23) | (1 << 26) | (1 << 27));
        assert_eq!(f, [1, 1, 3, 1, 6, 5, 1, 1]);
    }
}
