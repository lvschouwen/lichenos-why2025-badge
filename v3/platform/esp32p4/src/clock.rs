//! CPU clock to 360 MHz on the ESP32-P4 rev 1.0 exactly as ESP-IDF v6.1 does it (#645). No kernel state: `mmio`, the
//! register accesses below, a few ROM calls and a logger callback.
//!
//! Sources (ESP-IDF v6.1, `~/esp/esp-idf-v6.1-head/components`, REV_LESS_V3 build as v2):
//! - `bootloader_support/src/esp32p4/bootloader_esp32p4.c: bootloader_hardware_init()` (analog I2C clock, 1.1 V bias)
//! - `esp_hw_support/port/esp32p4/rtc_clk_init.c: rtc_clk_init()` (digital regulators, dbias from eFuse, DCDC)
//! - `esp_hw_support/port/esp32p4/pmu_param.c: get_act_hp_dbias() / get_act_lp_dbias()` (eFuse calibration)
//! - `esp_hw_support/port/esp32p4/rtc_clk.c: rtc_clk_cpu_freq_set_config()` and `rtc_clk_cpu_freq_to_cpll_mhz()` (CPLL 360 MHz,
//!   CPU div 1, MEM div 2, SYS div 1, APB div 2) with `esp_hal_clock/esp32p4/include/hal/clk_tree_ll.h`
//!
//! The register words were recorded by running IDF's own `clk_ll_*` and `pmu_ll_*` on a host (see psram/regs.rs).
//! Left out on purpose: the RC_FAST / RC_SLOW / RC32K calibration writes of rtc_clk_init (they tune the slow clocks, not the
//! CPU) and the flash MSPI clock (bootloader_init_mspi_clock): v3 does not touch flash.
//! eFuse: two read registers, `rd_mac_sys_2` (block version) and `rd_mac_sys_4` (active HP/LP dbias); no write, no trigger.
use crate::mmio::{modify32, read32, write32};
use crate::psram::clockmath::{dbias, dbias_calibrated};
use crate::uart;
use core::fmt::Arguments;

pub type Log<'a> = &'a mut dyn FnMut(Arguments);

const EFUSE_RD_MAC_SYS_2: usize = 0x5012_D000 + 0x4C;
const EFUSE_RD_MAC_SYS_4: usize = 0x5012_D000 + 0x54;

const PMU: usize = 0x5011_5000;
const PMU_HP_ACTIVE_REG0: usize = PMU + 0x28; // regulator0: xpd bit 18, dbias_sel bit 14, hp_dbias_vol [13:9], dbias [31:27]
const PMU_LP_ACTIVE_REG0: usize = PMU + 0x9C; // dbias [31:27]
const PMU_HP_ACTIVE_BIAS: usize = PMU + 0x18; // dcm_vset [22:18]
const PMU_DCDC_SWITCH: usize = PMU + 0x10C; // force_pu [1], force_pd [0]
const PMU_DCM_CTRL: usize = PMU + 0x204; // on_req bit 0, done_force bit 7
const PMU_IMM_HP_CK_POWER: usize = PMU + 0xCC;
const LP_CLKRST_HP_CLK_CTRL: usize = 0x5011_1040; // hp_root_clk_src_sel [1:0]
const HP_CLKRST: usize = 0x500E_6000;
const ROOT0: usize = HP_CLKRST + 0x04; // cpu div [4:0 + numerator/denominator], update bit 4
const ROOT1: usize = HP_CLKRST + 0x08; // mem div [7:0], sys div [31:24]
const ROOT2: usize = HP_CLKRST + 0x0C; // apb div [23:16]
const ANA_PLL_CTRL0: usize = HP_CLKRST + 0xBC; // cpu_pll_cal_end bit 2, cpu_pll_cal_stop bit 3
const LPPERI_CLK_EN: usize = 0x5012_0000;
const ANA_MST: usize = 0x5012_4000;
const I2C0_CTRL: usize = ANA_MST;
const ANA_CONF1: usize = ANA_MST + 0x1C;
const ANA_CONF2: usize = ANA_MST + 0x20;
const CLK160M: usize = ANA_MST + 0x34;
const UART0: usize = 0x500C_A000;

const ROM_DELAY_US: usize = 0x4FC0_003C;
const ROM_UPDATE_CPU_FREQ: usize = 0x4FC0_0044; // ets_update_cpu_frequency(mhz)
const HP_DBIAS_DEFAULT: u32 = 24; // pmu_param.h HP_CALI_ACTIVE_DBIAS_DEFAULT
const LP_DBIAS_DEFAULT: u32 = 29; // LP_CALI_ACTIVE_DBIAS_DEFAULT
const DCM_VSET_DEFAULT: u32 = 27; // HP_CALI_ACTIVE_DCM_VSET_DEFAULT

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Error {
    /// The CPLL calibration did not finish.
    CpllCalibration,
}

fn delay_us(us: u32) {
    // SAFETY: ROM ets_delay_us.
    let f: extern "C" fn(u32) = unsafe { core::mem::transmute(ROM_DELAY_US) };
    f(us)
}

// ---- the analog I2C master (regi2c), three blocks ----------------------------------------------------------------
const BLK_BIAS: (u8, u32) = (0x6A, 1 << 12);
const BLK_DIG: (u8, u32) = (0x6D, 1 << 10);
const BLK_CPLL: (u8, u32) = (0x67, 1 << 11);

unsafe fn i2c_wait() {
    while read32(I2C0_CTRL) & (1 << 25) != 0 {}
}
unsafe fn i2c_select(blk: (u8, u32)) {
    modify32(ANA_CONF2, 0x00FF_FFFF, 0);
    modify32(ANA_CONF1, 0x00FF_FFFF, 0);
    modify32(ANA_CONF2, 0, blk.1);
}
unsafe fn i2c_read(blk: (u8, u32), reg: u8) -> u8 {
    i2c_select(blk);
    i2c_wait();
    write32(I2C0_CTRL, blk.0 as u32 | ((reg as u32) << 8));
    i2c_wait();
    ((read32(I2C0_CTRL) >> 16) & 0xFF) as u8
}
unsafe fn i2c_write(blk: (u8, u32), reg: u8, data: u8) {
    i2c_select(blk);
    i2c_wait();
    write32(I2C0_CTRL, blk.0 as u32 | ((reg as u32) << 8) | (1 << 24) | ((data as u32) << 16));
    i2c_wait();
}
/// `REGI2C_WRITE_MASK(block, reg, msb, lsb, data)`
unsafe fn i2c_write_mask(blk: (u8, u32), reg: u8, msb: u32, lsb: u32, data: u8) {
    let cur = i2c_read(blk, reg) as u32;
    let width = msb - lsb + 1;
    let mask = ((1u32 << width) - 1) << lsb;
    let v = (cur & !mask) | (((data as u32) << lsb) & mask);
    i2c_write(blk, reg, v as u8);
}

unsafe fn put(log: Log, what: &str, addr: usize, mask: u32, val: u32) {
    let before = read32(addr);
    log(format_args!(
        "clk: {what}: {addr:#010x} mask {mask:#010x} val {val:#010x} (IDF writes the same; was {before:#010x})"
    ));
    uart::drain();
    modify32(addr, mask, val);
}

/// CPU MHz measured with mcycle against the SYSTIMER (16 MHz, XTAL-derived) over about 1 ms; `None` if it does not count.
pub fn measure_mhz() -> Option<u32> {
    const SYSTIMER: usize = 0x500E_2000;
    const CONF: usize = SYSTIMER;
    const OP: usize = SYSTIMER + 0x04;
    const LO: usize = SYSTIMER + 0x44;
    // SAFETY: SYSTIMER unit 0: clock gate on (it is the one write), update-and-read of its counter; mcycle CSR read.
    unsafe {
        modify32(CONF, 0, 1 << 31);
        let rd = || {
            modify32(OP, 0, 1 << 30);
            let mut n = 0u32;
            while read32(OP) & (1 << 29) == 0 && n < 1000 {
                n += 1;
            }
            read32(LO)
        };
        let (c0, t0) = (read_mcycle(), rd());
        let mut t;
        let mut c;
        loop {
            c = read_mcycle();
            t = rd();
            if t.wrapping_sub(t0) >= 16_000 {
                break;
            }
        }
        let (dc, dt) = (c.wrapping_sub(c0), t.wrapping_sub(t0));
        if dt == 0 {
            None
        } else {
            Some(psram_mhz(dc, dt))
        }
    }
}
fn psram_mhz(c: u32, t: u32) -> u32 {
    crate::psram::clockmath::mhz_from_systimer(c, t)
}
#[cfg(target_arch = "riscv32")]
#[inline(always)]
unsafe fn read_mcycle() -> u32 {
    let v: u32;
    core::arch::asm!("csrr {}, mcycle", out(reg) v);
    v
}
/// Host builds (the platform crate's unit tests) have no mcycle.
#[cfg(not(target_arch = "riscv32"))]
unsafe fn read_mcycle() -> u32 {
    0
}

/// Print the state a hang or brownout would leave: dividers, source, regulators, DCDC.
pub fn state(log: Log) {
    // SAFETY: reads only.
    unsafe {
        log(format_args!(
            "clk: state src={:#x} root0={:#010x} root1={:#010x} root2={:#010x} hp_reg0={:#010x} lp_reg0={:#010x} dcm_ctrl={:#010x} dcdc_sw={:#x} dcm_vset_reg={:#010x} uart0_clkdiv={:#x}",
            read32(LP_CLKRST_HP_CLK_CTRL) & 3,
            read32(ROOT0),
            read32(ROOT1),
            read32(ROOT2),
            read32(PMU_HP_ACTIVE_REG0),
            read32(PMU_LP_ACTIVE_REG0),
            read32(PMU_DCM_CTRL),
            read32(PMU_DCDC_SWITCH),
            read32(PMU_HP_ACTIVE_BIAS),
            read32(UART0 + 0x14)
        ));
    }
}

/// Bring the CPU to 360 MHz. Returns the measured cycles-per-timer-tick ratio before and after (both `None` if the timer
/// does not count).
///
/// # Safety
/// Changes the core supply (HP/LP regulator bias, DCDC) and the CPU clock tree; RAM-only bring-up on the boot hart with
/// nothing else running. Stop and report on a hang or brownout; do not retry blindly.
pub unsafe fn cpu_to_360(log: Log) -> Result<(Option<u32>, Option<u32>), Error> {
    state(log);
    let before = measure_mhz();
    log(format_args!("clk: before: measured {before:?} MHz"));

    // bootloader_hardware_init(): analog I2C master clock, 160 MHz source, 1.1 V bias
    modify32(LPPERI_CLK_EN, 0, 1 << 27);
    modify32(CLK160M, 0, 1);
    log(format_args!("clk: regi2c BIAS DREG_1P1 [7:4] = 10, DREG_1P1_PVT [3:0] = 10 (bootloader_hardware_init)"));
    i2c_write_mask(BLK_BIAS, 0, 7, 4, 10);
    i2c_write_mask(BLK_BIAS, 1, 3, 0, 10);

    // rtc_clk_init(): digital regulators handed to the PMU, bias forces released
    log(format_args!("clk: regi2c DIG_REG force RTC/DIG dreg = 1, xpd RTC/DIG reg = 0; BIAS or_force_xpd_* = 0"));
    i2c_write_mask(BLK_DIG, 10, 0, 0, 1);
    i2c_write_mask(BLK_DIG, 10, 1, 1, 1);
    i2c_write_mask(BLK_DIG, 13, 2, 2, 0);
    i2c_write_mask(BLK_DIG, 13, 3, 3, 0);
    i2c_write_mask(BLK_BIAS, 4, 0, 0, 0);
    i2c_write_mask(BLK_BIAS, 4, 1, 1, 0);
    i2c_write_mask(BLK_BIAS, 4, 2, 2, 0);
    i2c_write_mask(BLK_BIAS, 4, 3, 3, 0);

    // dbias from the eFuse calibration (read registers only)
    let rd2 = read32(EFUSE_RD_MAC_SYS_2);
    let blk = ((rd2 >> 11) & 3) * 100 + ((rd2 >> 8) & 7);
    let rd4 = read32(EFUSE_RD_MAC_SYS_4);
    let (hp_ef, lp_ef) = if dbias_calibrated(blk) { ((rd4 >> 16) & 0xF, (rd4 >> 20) & 0xF) } else { (0, 0) };
    let hp = dbias(hp_ef, 16, HP_DBIAS_DEFAULT);
    let lp = dbias(lp_ef, 20, LP_DBIAS_DEFAULT);
    log(format_args!("clk: efuse blk_version={blk} active_hp_dbias={hp_ef} active_lp_dbias={lp_ef} -> hp dbias {hp}, lp dbias {lp} (defaults {HP_DBIAS_DEFAULT}/{LP_DBIAS_DEFAULT})"));

    put(log, "hp active regulator xpd on", PMU_HP_ACTIVE_REG0, 1 << 18, 1 << 18);
    put(log, "hp active dbias", PMU_HP_ACTIVE_REG0, 0xF800_0000, hp << 27);
    put(log, "lp active dbias", PMU_LP_ACTIVE_REG0, 0xF800_0000, lp << 27);
    let pvt = (read32(PMU_HP_ACTIVE_REG0) >> 9) & 0x1F;
    let vset = if pvt > DCM_VSET_DEFAULT { pvt } else { DCM_VSET_DEFAULT };
    log(format_args!("clk: hp_dbias_vol read back {pvt} -> dcm_vset {vset}"));
    // DCDC on: pmu_ll_set_dcdc_en(true) = done_force 0, on_req 1; switch not forced down
    put(log, "dcdc enable (done_force 0, on_req 1)", PMU_DCM_CTRL, 0x81, 0x01);
    put(log, "dcdc switch not forced (force_pu 0, force_pd 0)", PMU_DCDC_SWITCH, 3, 0);
    put(log, "hp dcm_vset", PMU_HP_ACTIVE_BIAS, 0x007C_0000, vset << 18);
    put(log, "hand dbias control to the PMU (dbias_sel)", PMU_HP_ACTIVE_REG0, 1 << 14, 1 << 14);
    delay_us(1000);
    put(log, "hp active regulator xpd off", PMU_HP_ACTIVE_REG0, 1 << 18, 0);
    state(log);

    // CPU: CPLL on, CPU to XTAL with all dividers 1, calibrate CPLL to 360, dividers, then the source
    put(log, "cpll power (xpd cpll + i2c)", PMU_IMM_HP_CK_POWER, 0x0880_0000, 0x0880_0000);
    put(log, "cpll clock gate open", PMU_IMM_HP_CK_POWER, 0x0002_0000, 0x0002_0000);
    put(log, "cpu source XTAL", LP_CLKRST_HP_CLK_CTRL, 3, 0);
    put(log, "cpu div 1", ROOT0, 0x1FFF_FFE0, 0);
    put(log, "mem div 1", ROOT1, 0xFF, 0);
    put(log, "sys div 1", ROOT1, 0xFF00_0000, 0);
    put(log, "apb div 1", ROOT2, 0x00FF_0000, 0);
    bus_update();
    put(log, "cpll calibration start", ANA_PLL_CTRL0, 1 << 3, 0);
    log(format_args!("clk: regi2c CPLL ref_div reg2 = 0x50, div reg3 = 0x9 (360 MHz), dcur reg6 = 0x73 (IDF clk_ll_cpll_set_config, rev >= 1)"));
    uart::drain();
    i2c_write(BLK_CPLL, 2, 0x50);
    i2c_write(BLK_CPLL, 3, 0x09);
    i2c_write(BLK_CPLL, 6, 0x73);
    let mut n = 0u32;
    while read32(ANA_PLL_CTRL0) & (1 << 2) == 0 {
        n += 1;
        if n > 5_000_000 {
            return Err(Error::CpllCalibration);
        }
    }
    delay_us(10);
    put(log, "cpll calibration stop", ANA_PLL_CTRL0, 1 << 3, 1 << 3);
    // upscaling order of rtc_clk_cpu_freq_to_cpll_mhz(360): apb, sys, mem, cpu, each followed by a bus update
    put(log, "apb div 2", ROOT2, 0x00FF_0000, 1 << 16);
    bus_update();
    put(log, "sys div 1", ROOT1, 0xFF00_0000, 0);
    bus_update();
    put(log, "mem div 2", ROOT1, 0xFF, 1);
    bus_update();
    put(log, "cpu div 1", ROOT0, 0x1FFF_FFE0, 0);
    bus_update();
    put(log, "cpu source CPLL", LP_CLKRST_HP_CLK_CTRL, 3, 1);
    // SAFETY (transmute): ROM ets_update_cpu_frequency(mhz) so the ROM delay loops keep their scale
    let upd: extern "C" fn(u32) = core::mem::transmute(ROM_UPDATE_CPU_FREQ);
    upd(360);
    uart::spin(100_000);
    state(log);
    let after = measure_mhz();
    log(format_args!("clk: after: measured {after:?} MHz"));
    Ok((before, after))
}

unsafe fn bus_update() {
    modify32(ROOT0, 0, 1 << 4);
    while read32(ROOT0) & (1 << 4) != 0 {}
}
