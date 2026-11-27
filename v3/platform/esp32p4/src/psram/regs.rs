//! Register operations of the PSRAM bring-up, as (address, mask, value) triples.
//!
//! The tables were not typed from the headers: they were recorded by running ESP-IDF v6.1's own `psram_ctrlr_ll_*` and
//! `mspi_timing_ll_*` functions (`esp_hal_mspi/esp32p4/include/hal/psram_ctrlr_ll.h`, `mspi_ll.h`, hw_ver1 register
//! headers) on a host with the register pages mapped at their P4 addresses, once over zeroed and once over all-ones
//! memory, and diffing the pages. `mask` is every bit the call changes, `value` what those bits end up as.

use crate::mmio::{modify32, read32, write32};

/// (register address, mask of bits the operation owns, their new value)
pub type Op = (usize, u32, u32);

/// Read-modify-write every triple.
///
/// # Safety
/// Each address must be a register of this chip that may be written at this point of the bring-up (the callers pass
/// the tables below, from the PSRAM controller, its IOMUX pins and the HP clock/reset block).
pub unsafe fn apply(ops: &[Op]) {
    for &(addr, mask, val) in ops {
        modify32(addr, mask, val);
    }
}

pub const SPIMEM2: usize = 0x5008_E000; // DR_REG_PSRAM_MSPI0_BASE: the PSRAM controller seen by the cache (AXI)
pub const SPIMEM3: usize = 0x5008_F000; // DR_REG_PSRAM_MSPI1_BASE: the same device through the "peripheral" (FIFO) path
pub const HP_CLKRST: usize = 0x500E_6000; // DR_REG_HP_SYS_CLKRST_BASE
pub const MSPI_IOMUX: usize = 0x500E_1200; // DR_REG_IOMUX_MSPI_PIN_BASE

// psram_ctrlr_ll_enable_module_clock(true): soc_clk_ctrl0 bit 31, peri_clk_ctrl00 bits 14 and 15
pub const MODULE_CLOCK_ON: [Op; 2] =
    [(HP_CLKRST + 0x14, 0x8000_0000, 0x8000_0000), (HP_CLKRST + 0x30, 0x0000_C000, 0x0000_C000)];
// psram_ctrlr_ll_select_clk_source(MPLL)
pub const CLK_SRC_MPLL: [Op; 1] = [(HP_CLKRST + 0x30, 0x0000_3000, 0x0000_1000)];
// _psram_ctrlr_ll_reset_module_clock: hp_rst_en0 bit 23 (AXI) and bit 25 (APB), set AXI, set APB, clear APB, clear AXI
pub const HP_RST_EN0: usize = HP_CLKRST + 0xC0;
pub const RST_AXI: u32 = 1 << 23;
pub const RST_APB: u32 = 1 << 25;
// psram_ctrlr_ll_enable_core_clock(true) and set_core_clock_div(1): peri_clk_ctrl00 bit 15, div field [23:16] = div - 1
pub const PERI_CLK_CTRL00: usize = HP_CLKRST + 0x30;
pub const CORE_CLOCK_EN: u32 = 0x0000_8000;
pub const CORE_CLOCK_DIV_MASK: u32 = 0x00FF_0000;

// mspi_timing_ll_enable_dqs(true): XPD bit 0 of the two DQS pin registers
pub const DQS_ON: [Op; 2] = [(MSPI_IOMUX + 0x3C, 1, 1), (MSPI_IOMUX + 0x68, 1, 1)];

// The pin registers are 0x4 apart from MSPI_IOMUX + 0x1C: D, Q, WP, HD, D4-D7 (pins 0-7), DQS0 (8), CLK, CS, D8-D15,
// DQS1 (19), see `mspi_ll_pin_t`. Drive strength [13:12] (DQS [16:15]); delay line [7:4] (DQS: [10:7] and [20:17]).
pub const PIN_REG0: usize = MSPI_IOMUX + 0x1C;
pub const PIN_COUNT: usize = 20;
pub const PIN_DQS0: usize = 8;
pub const PIN_DQS1: usize = 19;
pub const DQS0_PHASE_REG: usize = MSPI_IOMUX + 0x3C; // [2:1]
pub const DQS1_PHASE_REG: usize = MSPI_IOMUX + 0x68; // [2:1]
pub const DQS_PHASE_MASK: u32 = 0x6;
pub const PIN_DLC_MASK: u32 = 0xF0;
pub const DQS_DLC_MASK: u32 = 0x001E_0780;

/// `mspi_timing_ll_pin_drv_set(drv)`: all 20 pins.
///
/// # Safety
/// MSPI pin registers, PSRAM bring-up only.
pub unsafe fn pin_drv_set(drv: u32) {
    for p in 0..PIN_COUNT {
        let (mask, shift) = if p == PIN_DQS0 || p == PIN_DQS1 { (0x0001_8000, 15) } else { (0x0000_3000, 12) };
        modify32(PIN_REG0 + 4 * p, mask, (drv << shift) & mask);
    }
}

/// `mspi_timing_ll_set_dqs_phase` for both DQS pins.
///
/// # Safety
/// MSPI pin registers, PSRAM bring-up only.
pub unsafe fn set_dqs_phase(phase: u32) {
    let v = (phase << 1) & DQS_PHASE_MASK;
    modify32(DQS0_PHASE_REG, DQS_PHASE_MASK, v);
    modify32(DQS1_PHASE_REG, DQS_PHASE_MASK, v);
}

/// `mspi_timing_ll_set_delayline` for all 20 pins: the two DQS pins get `dqs`, every other pin `data`.
///
/// # Safety
/// MSPI pin registers, PSRAM bring-up only.
pub unsafe fn set_delaylines(data: u32, dqs: u32) {
    for p in 0..PIN_COUNT {
        let a = PIN_REG0 + 4 * p;
        if p == PIN_DQS0 || p == PIN_DQS1 {
            // dqs_delay_90 [10:7] and dqs_delay_270 [20:17] take the same value
            modify32(a, DQS_DLC_MASK, ((dqs & 0xF) << 7) | ((dqs & 0xF) << 17));
        } else {
            modify32(a, PIN_DLC_MASK, (data & 0xF) << 4);
        }
    }
}

/// `psram_ctrlr_ll_set_bus_clock` register word for a divider (the cache side at SPIMEM2 + 0x50, the FIFO side at
/// SPIMEM3 + 0x14): divider 1 is "equal to system clock" (bit 31), else N = L = div - 1, H = div / 2 - 1.
pub const fn bus_clock_word(div: u32) -> u32 {
    if div == 1 {
        0x8000_0000
    } else {
        ((div - 1) << 16) | ((div / 2 - 1) << 8) | (div - 1)
    }
}
pub const BUS_CLOCK_SPIMEM2: usize = SPIMEM2 + 0x50;
pub const BUS_CLOCK_SPIMEM3: usize = SPIMEM3 + 0x14;

/// # Safety
/// PSRAM controller clock registers, PSRAM bring-up only.
pub unsafe fn set_bus_clock(div: u32) {
    write32(BUS_CLOCK_SPIMEM2, bus_clock_word(div));
    write32(BUS_CLOCK_SPIMEM3, bus_clock_word(div));
}

// CS timing, split transfers, page size and "ECC off" (s_set_psram_cs_timing, split_trans, page_size 2048,
// s_configure_psram_ecc without CONFIG_SPIRAM_ECC_ENABLE), in the order impl_enable does them.
pub const CS_AND_ECC: [Op; 8] = [
    (SPIMEM2 + 0x1A0, 0x0000_007D, 0x0000_000D), // cs_setup 4
    (SPIMEM2 + 0x1A0, 0x0000_0F82, 0x0000_0182), // cs_hold 4
    (SPIMEM2 + 0x1A0, 0x7E00_0000, 0x0400_0000), // cs_hold_delay 3
    (SPIMEM2 + 0x1A0, 0x8000_0000, 0x8000_0000), // split transfers on
    (SPIMEM2 + 0x174, 0x000C_0000, 0x000C_0000), // page size 2048
    (SPIMEM2 + 0x1A0, 0x0001_0000, 0),           // 16-to-18 ECC off
    (SPIMEM2 + 0x174, 0x0010_0000, 0),           // ECC address conversion off
    (SPIMEM2 + 0x130, 0x0000_0004, 0),           // PMS region 0 ECC off
];
// psram_ctrlr_ll_enable_dll(true) for the cache side and the FIFO side
pub const DLL_ON: [Op; 2] = [(SPIMEM2 + 0x190, 0x20, 0x20), (SPIMEM2 + 0x180, 0x20, 0x20)];

// s_config_mspi_for_psram(): AP HEX 16-bit DDR, write command 0x8080, read command 0x0000, 32-bit address, write dummy
// 2 * (5 - 1), read dummy 2 * (10 - 1), variable dummy, hex data lines, AXI on, write/read splice. Order as in IDF.
pub const CONFIG_FOR_PSRAM: [Op; 19] = [
    (SPIMEM2 + 0x40, 0x0010_0000, 0x0010_0000), // wr cmd: use it
    (SPIMEM2 + 0x4C, 0xF000_FFFF, 0xF000_8080), //   16 bits, value 0x8080
    (SPIMEM2 + 0x40, 0x0000_0020, 0x0000_0020), // rd cmd: use it
    (SPIMEM2 + 0x48, 0xF000_FFFF, 0xF000_0000), //   16 bits, value 0x0000
    (SPIMEM2 + 0x40, 0x000F_C000, 0x0007_C000), // address length 32 bits
    (SPIMEM2 + 0x40, 0x0000_0001, 0x0000_0001), // 4-byte address
    (SPIMEM2 + 0x40, 0x0FC0_0008, 0x01C0_0008), // write dummy 8 bits
    (SPIMEM2 + 0x40, 0x0000_0FD0, 0x0000_0450), // read dummy 18 bits
    (SPIMEM2 + 0xD8, 0x0000_0002, 0x0000_0002), // variable dummy
    (SPIMEM2 + 0x44, 0x0080_0000, 0x0080_0000), // write dummy level control
    (SPIMEM2 + 0xD8, 0x0000_0008, 0),           // DDR write data swap off
    (SPIMEM2 + 0xD8, 0x0000_0004, 0),           // DDR read data swap off
    (SPIMEM2 + 0xD8, 0x0000_0001, 0x0000_0001), // DDR on
    (SPIMEM2 + 0x40, 0x0020_0000, 0x0020_0000), // octal line mode ...
    (SPIMEM2 + 0x44, 0x003C_0000, 0x003C_0000), //   ... for command, address and data
    (SPIMEM2 + 0x44, 0x0C00_0000, 0x0C00_0000), // hex data lines
    (SPIMEM2 + 0x3C, 0x8000_0001, 0x0000_0001), // AXI access on
    (SPIMEM2 + 0x0C, 0x0400_0000, 0x0400_0000), // write splice
    (SPIMEM2 + 0x0C, 0x0200_0000, 0x0200_0000), // read splice
];
/// `psram_ctrlr_ll_enable_variable_dummy(true)` for both sides, the last step of impl_enable after tuning.
pub const VAR_DUMMY_ON: [Op; 2] =
    [(SPIMEM2 + 0xD8, 0x0000_0002, 0x0000_0002), (SPIMEM3 + 0xD4, 0x0000_0002, 0x0000_0002)];
/// `psram_ctrlr_ll_enable_variable_dummy(MSPI_ID_3, false)`, used while tuning.
pub const VAR_DUMMY_FIFO_OFF: Op = (SPIMEM3 + 0xD4, 0x0000_0002, 0);

/// Read a register (a thin alias so the callers of this module need no second import).
///
/// # Safety
/// As `read32`.
pub unsafe fn rd(addr: usize) -> u32 {
    read32(addr)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bus_clock_words_match_the_idf_recording() {
        assert_eq!(bus_clock_word(4), 0x0003_0103); // 120 MHz from MPLL 480
        assert_eq!(bus_clock_word(24), 0x0017_0B17); // 20 MHz from MPLL 480 (the tuning low-speed step)
        assert_eq!(bus_clock_word(1), 0x8000_0000);
    }

    #[test]
    fn pin_registers_are_where_the_recording_put_them() {
        // recorded: pin 0 (D) at +0x1C ... DQS0 (8) at +0x3C, CLK (9) at +0x40 ... DQS1 (19) at +0x68
        assert_eq!(PIN_REG0 + 4 * PIN_DQS0, DQS0_PHASE_REG);
        assert_eq!(PIN_REG0 + 4 * PIN_DQS1, DQS1_PHASE_REG);
        assert_eq!(PIN_REG0 + 4 * PIN_COUNT - 4, MSPI_IOMUX + 0x68);
    }
}
