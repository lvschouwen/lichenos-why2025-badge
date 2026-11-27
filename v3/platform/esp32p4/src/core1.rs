//! Starting HP core 1 (#612 checkpoint 3), IDF's start_other_core sequence on the hw_ver1 registers, as #608 run 8
//! proved it on rev v1.0 (v3/probe-p4/src/core1.rs).
//!
//! ROM state (run 8): core 1 is held in reset (RST_EN0 bit 8 set) with its CPU clock off (SOC_CLK_CTRL0 bit 4 clear),
//! not stalled; its CLIC clock (bit 1) is already on. Rule (run 1): an M access from core 0 to core 1's CPU-subsystem
//! windows (its CLIC at 0x2081_0000) while core 1 is in reset stalls core 0 until reset. This module never touches
//! them, before or after the start: each core programs its own CLIC and CLINT compare through its own view.
use crate::mmio::{read32, write32};

/// HP_SYS_CLKRST_SOC_CLK_CTRL0_REG: bit 1 CORE1_CLIC_CLK_EN, bit 4 CORE1_CPU_CLK_EN.
const CLK_CTRL0: usize = 0x500E_6014;
const CORE1_CPU_CLK_EN: u32 = 1 << 4;
/// HP_SYS_CLKRST_HP_RST_EN0_REG: bit 8 RST_EN_CORE1_GLOBAL.
const RST_EN0: usize = 0x500E_60C0;
const RST_EN_CORE1: u32 = 1 << 8;
/// HP_SYSTEM_CPU_CORESTALLED_ST_REG: bit 1 core 1 stalled.
const CORESTALLED_ST: usize = 0x500E_5064;
/// PMU_CPU_SW_STALL_REG: [23:16] hpcore1_stall_code (0x86 stall, 0xFF run).
const PMU_SW_STALL: usize = 0x5011_5200;
const STALL_CODE_MASK: u32 = 0x00FF_0000;
const STALL_CODE_RUN: u32 = 0x00FF_0000;
/// ROM `void ets_set_appcpu_boot_addr(uint32_t)` (esp32p4.rom.ld): the ROM on core 1 waits for this word.
const ETS_SET_APPCPU_BOOT_ADDR: usize = 0x4FC0_00A8;

/// The start registers, for the bench log.
#[derive(Clone, Copy, Debug)]
pub struct Regs {
    pub clk_ctrl0: u32,
    pub rst_en0: u32,
    pub pmu_sw_stall: u32,
    pub corestalled_st: u32,
}

pub fn regs() -> Regs {
    // SAFETY: clock, reset, PMU and HP_SYSTEM status registers of the whole chip (not core 1's own windows); reads
    // without side effects.
    unsafe {
        Regs {
            clk_ctrl0: read32(CLK_CTRL0),
            rst_en0: read32(RST_EN0),
            pmu_sw_stall: read32(PMU_SW_STALL),
            corestalled_st: read32(CORESTALLED_ST),
        }
    }
}

/// Whether core 1 is out of reset with its clock on (the only state in which its windows may be touched at all).
pub fn running() -> bool {
    let r = regs();
    r.clk_ctrl0 & CORE1_CPU_CLK_EN != 0 && r.rst_en0 & RST_EN_CORE1 == 0
}

/// Unstall, clock on, out of reset, then hand `entry` to the ROM. Returns the stall-bit polls it took, or None if
/// core 1 stayed stalled (then nothing else was done). `poll_limit` bounds that wait.
///
/// # Safety
/// `entry` must be code that is safe to run on hart 1 in M-mode with no stack (it sets up its own), and core 1
/// must still be in its ROM state (call this once per boot).
pub unsafe fn start(entry: usize, poll_limit: u32) -> Option<u32> {
    // esp_cpu_unstall(1): stall code 0xFF, then wait for the stalled bit to drop.
    let v = read32(PMU_SW_STALL);
    write32(PMU_SW_STALL, (v & !STALL_CODE_MASK) | STALL_CODE_RUN);
    let mut polls = 0;
    while read32(CORESTALLED_ST) & 2 != 0 {
        polls += 1;
        if polls >= poll_limit {
            return None;
        }
    }
    // cpu_utility_ll_enable_clock_and_reset_app_cpu(): clock on, then out of reset.
    let v = read32(CLK_CTRL0);
    if v & CORE1_CPU_CLK_EN == 0 {
        write32(CLK_CTRL0, v | CORE1_CPU_CLK_EN);
    }
    let v = read32(RST_EN0);
    if v & RST_EN_CORE1 != 0 {
        write32(RST_EN0, v & !RST_EN_CORE1);
    }
    // SAFETY (caller + ROM ABI): the documented ROM entry point at its esp32p4.rom.ld address, a C function of one u32.
    let set_boot: extern "C" fn(u32) = core::mem::transmute(ETS_SET_APPCPU_BOOT_ADDR);
    set_boot(entry as u32);
    Some(polls)
}
