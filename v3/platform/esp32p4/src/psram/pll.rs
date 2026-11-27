//! MPLL (the PSRAM clock source) through the analog I2C master ("regi2c"), ported from ESP-IDF v6.1:
//! `esp_hw_support/port/esp32p4/rtc_clk.c` (rtc_clk_mpll_enable / _configure), `esp_hal_clock/esp32p4/include/hal/
//! clk_tree_ll.h` (clk_ll_mpll_set_config_v1: v2 builds with CONFIG_ESP32P4_SELECTS_REV_LESS_V3, so the v1 path) and
//! `esp_hal_regi2c/esp32p4/regi2c_impl.c` (the I2C master protocol).
use crate::mmio::{modify32, read32, write32};
use crate::psram::regs::HP_CLKRST;

const PMU_RF_PWC: usize = 0x5011_515C;
const PMU_MSPI_PHY_XPD: u32 = 1 << 24;
const LP_CLKRST_HP_CLK_CTRL: usize = 0x5011_1040;
const HP_MPLL_500M_CLK_EN: u32 = 1 << 28;
const ANA_PLL_CTRL0: usize = HP_CLKRST + 0xBC;
const MSPI_CAL_STOP: u32 = 1 << 9;
const MSPI_CAL_END: u32 = 1 << 8;

const LPPERI_CLK_EN: usize = 0x5012_0000;
const CK_EN_LP_I2CMST: u32 = 1 << 27;
const ANA_MST: usize = 0x5012_4000; // DR_REG_LP_I2C_ANA_MST_BASE
const I2C0_CTRL: usize = ANA_MST;
const ANA_CONF1: usize = ANA_MST + 0x1C;
const ANA_CONF2: usize = ANA_MST + 0x20;
const CLK160M: usize = ANA_MST + 0x34;
const BUSY: u32 = 1 << 25;
const WR: u32 = 1 << 24;

const I2C_MPLL: u8 = 0x63;
const MPLL_SEL_BIT: u32 = 1 << 9; // REGI2C_MSPI_XTAL_MST_SEL
const MPLL_IR_CAL_RSTB_REG: u8 = 1; // bit 5
const MPLL_DIV_REG: u8 = 2; // [7:3] div, [2:0] ref_div
const MPLL_DHREF_REG: u8 = 3; // [5:4]
const XTAL_MHZ: u32 = 40;

/// The register values of the I2C master that the regi2c sequence depends on, for the bench log.
pub fn i2c_master_state() -> (u32, u32) {
    // SAFETY: reads of the LP peripheral clock-enable word and the analog I2C master's clock select.
    unsafe { (read32(LPPERI_CLK_EN), read32(CLK160M)) }
}

/// MPLL register word for a target frequency: div = f / 20 - 1 in [7:3], ref_div 1 in [2:0] (f = 40 * (div + 1) / 2).
pub const fn mpll_div_word(mhz: u32) -> u8 {
    (((mhz / 20 - 1) << 3) | 1) as u8
}
/// Frequency a DIV register word makes: 40 MHz * (div + 1) / (ref_div + 1).
pub const fn mpll_mhz_of_word(w: u8) -> u32 {
    XTAL_MHZ * ((w as u32 >> 3) + 1) / ((w as u32 & 7) + 1)
}

/// # Safety
/// Analog I2C master of the LP block; PSRAM bring-up only, one caller at a time.
unsafe fn regi2c_select() {
    modify32(ANA_CONF2, 0x00FF_FFFF, 0);
    modify32(ANA_CONF1, 0x00FF_FFFF, 0);
    modify32(ANA_CONF2, 0, MPLL_SEL_BIT);
}
unsafe fn regi2c_wait() {
    while read32(I2C0_CTRL) & BUSY != 0 {}
}
unsafe fn regi2c_read(reg: u8) -> u8 {
    regi2c_select();
    regi2c_wait();
    write32(I2C0_CTRL, I2C_MPLL as u32 | ((reg as u32) << 8));
    regi2c_wait();
    ((read32(I2C0_CTRL) >> 16) & 0xFF) as u8
}
unsafe fn regi2c_write(reg: u8, data: u8) {
    regi2c_select();
    regi2c_wait();
    write32(I2C0_CTRL, I2C_MPLL as u32 | ((reg as u32) << 8) | WR | ((data as u32) << 16));
    regi2c_wait();
}

/// Power the MPLL (`clk_ll_mpll_enable`).
///
/// # Safety
/// PMU and LP clock registers; call once, after the PSRAM LDO is up (IDF acquires the LDO first, esp_clk_tree_mpll_acquire).
pub unsafe fn mpll_enable() {
    modify32(PMU_RF_PWC, 0, PMU_MSPI_PHY_XPD);
    modify32(LP_CLKRST_HP_CLK_CTRL, 0, HP_MPLL_500M_CLK_EN);
}

/// Configure the MPLL to `mhz` (`rtc_clk_mpll_configure` with `clk_ll_mpll_set_config_v1`) and return the frequency read
/// back from the analog registers, or `None` if the calibration did not finish within `spins` polls.
///
/// # Safety
/// As `mpll_enable`; the analog I2C master clock must be on (it is the reset default; `i2c_master_state` shows it).
pub unsafe fn mpll_configure(mhz: u32, spins: u32) -> Option<u32> {
    // bootloader_hardware_init(): analog I2C master clock on, and the 160 MHz source selected
    modify32(LPPERI_CLK_EN, 0, CK_EN_LP_I2CMST);
    modify32(CLK160M, 0, 1);
    modify32(ANA_PLL_CTRL0, MSPI_CAL_STOP, 0); // calibration start
    let dhref = regi2c_read(MPLL_DHREF_REG);
    regi2c_write(MPLL_DHREF_REG, dhref | (3 << 4));
    let rstb = regi2c_read(MPLL_IR_CAL_RSTB_REG);
    regi2c_write(MPLL_IR_CAL_RSTB_REG, rstb & 0xDF);
    regi2c_write(MPLL_IR_CAL_RSTB_REG, rstb | (1 << 5));
    regi2c_write(MPLL_DIV_REG, mpll_div_word(mhz));
    let mut n = 0;
    while read32(ANA_PLL_CTRL0) & MSPI_CAL_END == 0 {
        n += 1;
        if n > spins {
            modify32(ANA_PLL_CTRL0, 0, MSPI_CAL_STOP);
            return None;
        }
    }
    modify32(ANA_PLL_CTRL0, 0, MSPI_CAL_STOP); // calibration stop
    Some(mpll_mhz_of_word(regi2c_read(MPLL_DIV_REG)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mpll_words() {
        // 480 MHz: div 23, ref_div 1; 320 MHz: div 15
        assert_eq!(mpll_div_word(480), (23 << 3) | 1);
        assert_eq!(mpll_div_word(320), (15 << 3) | 1);
        assert_eq!(mpll_mhz_of_word(mpll_div_word(480)), 480);
        assert_eq!(mpll_mhz_of_word(mpll_div_word(320)), 320);
    }
}
