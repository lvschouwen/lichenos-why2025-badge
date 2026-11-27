//! Register tables of the application-side power setup of ESP-IDF v6.1 on the ESP32-P4 that the CPU clock bring-up needs before the
//! CPLL: `pmu_init()` of `esp_hw_support/port/esp32p4/pmu_init.c` for the HP-active and LP-active modes (analog, regulator, clock,
//! power fields) and the PVT automatic dbias of `pmu_pvt.c` (`CONFIG_ESP_ENABLE_PVT=y` in v2, "must enable for MP"). The PMU words
//! were recorded by running IDF's `pmu_hp_system_init` / `pmu_lp_system_init` on a host with the PMU page mapped (as psram/regs.rs);
//! the PVT list was produced by compiling IDF's own `pmu_pvt.c` with the register macros replaced by printers, so the order is IDF's.
//! v2 gets its 360 MHz after exactly these steps (the bootloader runs at 90 MHz), so v3 does them before raising the CPU.

/// One step of an ordered register sequence.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    /// read-modify-write: bits in `mask` become `val`
    Bits(usize, u32, u32),
    Set(usize, u32),
    Clear(usize, u32),
    DelayUs(u32),
}

// (address, mask, value): `pmu_hp_system_init(PMU_MODE_HP_ACTIVE, defaults)`
pub const PMU_HP_ACTIVE: [(usize, u32, u32); 13] = [
    (0x5011_5004, 0xFFFF_FFFF, 0xFFFF_FFFF),
    (0x5011_5008, 0xFFFF_FFFF, 0xFFFF_FFFF),
    (0x5011_5014, 0xFFFF_FFFF, 0x7F80_0000),
    (0x5011_5018, 0xFFFC_0000, 0x02EC_0000),
    (0x5011_501C, 0xFFFF_FFFF, 0x0100_00A0),
    (0x5011_5020, 0xFFFF_FFFF, 0xFFFF_FFFF),
    (0x5011_5024, 0xFC00_0000, 0x0800_0000),
    (0x5011_5030, 0x8000_0000, 0x8000_0000),
    (0x5011_512C, 0x0003_0000, 0x0002_0000),
    (0x5011_5000, 0xFFFF_FFFF, 0),
    (0x5011_5010, 0x3F00_0000, 0),
    (0x5011_5028, 0x0783_0000, 0),
    (0x5011_502C, 0xFC00_0000, 0),
];
// `pmu_lp_system_init(PMU_MODE_LP_ACTIVE, defaults)`
pub const PMU_LP_ACTIVE: [(usize, u32, u32); 5] = [
    (0x5011_50AC, 0xFFFF_FFFF, 0x7000_0000),
    (0x5011_509C, 0x07A0_0000, 0),
    (0x5011_50A0, 0xFC00_0000, 0),
    (0x5011_50A8, 0xFFFF_FFFF, 0),
    (0x5011_50BC, 0x8000_0000, 0),
];

const CLKRST: usize = 0x500E_6000;
const PVT: usize = 0x5009_E000;
const PMU_HP_ACTIVE_REG0: usize = 0x5011_5028;
pub const PVT_CMD0: usize = PVT + 0x50;
pub const PVT_CMD1: usize = PVT + 0x54;
pub const PVT_CMD2: usize = PVT + 0x58;

/// `pvt_auto_dbias_init()` up to (not including) the eFuse-dependent offset words, in IDF's order (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`
/// thresholds 164 / 157).
pub const PVT_INIT: [Op; 25] = [
    Op::Set(CLKRST + 0xC0, 0x20),
    Op::Clear(CLKRST + 0xC0, 0x20),
    Op::Set(CLKRST + 0x2C, 0x1),
    Op::Set(CLKRST + 0x18, 0x0200_0000),
    Op::Clear(PVT + 0x64, 0x8000_0000),
    Op::DelayUs(1),
    Op::Bits(PVT + 0x34, 0xFE00_0000, 0x6200_0000),
    Op::Bits(PVT + 0x34, 0x01FC_0000, 0x00D4_0000),
    Op::Bits(PVT + 0x3C, 0x1_FFFF, 0x1_1FFF),
    Op::Bits(PVT + 0x40, 0x1_FFFF, 0x1_7FFF),
    Op::Bits(PVT + 0x44, 0x1_FFFF, 0x1_0000),
    Op::Bits(PVT_CMD0, 0x7FF, 0x24),
    Op::Bits(PVT_CMD1, 0x7FF, 0x5),
    Op::Bits(PVT_CMD2, 0x7FF, 0x427),
    Op::Bits(PVT + 0x64, 0x7FFF_8000, 0x7FFF_8000),
    Op::Bits(CLKRST + 0xA4, 0xFF_0000, 0x1_0000),
    Op::Bits(CLKRST + 0xA8, 0xFF, 0x1),
    Op::Set(CLKRST + 0xA4, 0x0100_0000),
    Op::Set(CLKRST + 0xA8, 0x100),
    Op::Set(CLKRST + 0xA8, 0x200),
    Op::Set(CLKRST + 0xA8, 0x400),
    Op::Set(CLKRST + 0xA8, 0x800),
    Op::Bits(PVT + 0x1C8, 0x3, 0x1),
    Op::Bits(PVT + 0x108, 0x3FC, 0x290),
    Op::Bits(PVT + 0x10C, 0x3FC, 0x274),
];

/// `pvt_func_enable(true)`, then the 1 ms settle `pmu_init()` adds.
pub const PVT_ENABLE: [Op; 10] = [
    Op::Set(CLKRST + 0xA4, 0x0100_0000),
    Op::Set(PMU_HP_ACTIVE_REG0, 0x8000), // DBIAS_INIT: start the calibration at HP_CALI_DBIAS_DEFAULT
    Op::Set(PVT + 0x30, 0x100),
    Op::Set(PVT + 0x108, 0x1),
    Op::DelayUs(10),
    Op::Clear(PMU_HP_ACTIVE_REG0, 0x4000), // DBIAS_SEL: dbias control to the PVT
    Op::Clear(PMU_HP_ACTIVE_REG0, 0x8000), // DBIAS_INIT
    Op::Set(PVT + 0x64, 0x8000_0000),      // the timer: auto dbias on
    Op::DelayUs(50),
    Op::DelayUs(1000),
];

/// The three offset words `set_pvt_hp_lp_gap` writes (flag bit 16, value [15:11]) after the table above.
pub const fn pvt_offset_ops(value: u8) -> [Op; 6] {
    let flag = ((value >> 4) & 1) as u32;
    let abs = (value & 0xF) as u32;
    [
        Op::Bits(PVT_CMD0, 0x1_0000, flag << 16),
        Op::Bits(PVT_CMD0, 0xF800, abs << 11),
        Op::Bits(PVT_CMD1, 0x1_0000, flag << 16),
        Op::Bits(PVT_CMD1, 0xF800, abs << 11),
        Op::Bits(PVT_CMD2, 0x1_0000, flag << 16),
        Op::Bits(PVT_CMD2, 0xF800, abs << 11),
    ]
}

/// `get_lp_hp_gap()` of `pmu_pvt.c`: from the eFuse field `lp_dcdc_dbias_vol_gap` (5 bits, bit 4 = sign) to the byte written into the
/// PVT offset words; `None` where IDF asserts (gap outside -15..=7).
pub const fn pvt_gap_value(efuse_gap: u32) -> Option<u8> {
    let abs = (efuse_gap & 0xF) as i32;
    let gap = if (efuse_gap >> 4) & 1 != 0 { -abs } else { abs } - 8;
    if gap < -15 || gap > 7 {
        return None;
    }
    Some(if gap < 0 { (16 - gap) as u8 } else { gap as u8 })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gap_value_follows_get_lp_hp_gap() {
        assert_eq!(pvt_gap_value(0), Some(24)); // -8 -> 16 + 8: flag 1, magnitude 8 (what IDF wrote with a zero eFuse field)
        assert_eq!(pvt_gap_value(8), Some(0)); // 8 - 8 = 0
        assert_eq!(pvt_gap_value(0xF), Some(7)); // 15 - 8 = 7, the maximum
        assert_eq!(pvt_gap_value(0x10 | 7), Some(16 + 15)); // -7 - 8 = -15, the minimum
        assert_eq!(pvt_gap_value(0x10 | 8), None); // -16: IDF asserts
    }

    #[test]
    fn offset_ops_split_flag_and_magnitude() {
        let o = pvt_offset_ops(24);
        assert_eq!(o[0], Op::Bits(PVT_CMD0, 0x1_0000, 0x1_0000));
        assert_eq!(o[1], Op::Bits(PVT_CMD0, 0xF800, 8 << 11));
        assert_eq!(o[5], Op::Bits(PVT_CMD2, 0xF800, 8 << 11));
        let o = pvt_offset_ops(3);
        assert_eq!(o[0], Op::Bits(PVT_CMD0, 0x1_0000, 0));
    }

    #[test]
    fn tables_hit_the_pmu_page_only_where_expected() {
        for (a, m, v) in PMU_HP_ACTIVE.iter().chain(PMU_LP_ACTIVE.iter()) {
            assert!((0x5011_5000..0x5011_5200).contains(a), "{a:#x}");
            assert_eq!(v & !m, 0, "value outside mask at {a:#x}");
        }
    }
}
