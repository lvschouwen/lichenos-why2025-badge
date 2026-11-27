//! PSRAM bring-up without IDF (M3-1, #620): LDO channel, MPLL, MSPI timing, the IDF write command.
//!
//! Ported from ESP-IDF v6.1 (Apache-2.0) and the Lichen patches of the v2 tree (`tools/idf-patches` 0001 120 MHz,
//! 0003 runtime select, 0004 4x refresh). The order is `esp_psram_impl_enable()` of `esp_psram_impl_ap_hex.c`:
//!
//! 1. `ldo`: the PSRAM rail (LDO channel 2, 1.8 V), then `pll`: MPLL 480 MHz through the analog I2C master.
//! 2. controller: module clock, reset, MPLL as source, pin drive, DQS, CS timing, bus clock 480 / 4 = 120 MHz, DLL.
//! 3. chip: mode registers over the FIFO path (latency codes, refresh 4x from the first access, burst), connection check.
//! 4. `regs::CONFIG_FOR_PSRAM` (the cache/AXI path) and `tune`: reference data at 20 MHz, DQS phase and delay-line sweep at 120 MHz.
//!
//! Floor: 120 MHz (memory: PSRAM stays >= 120 MHz; DMA bit flips #121/#569). There is no lower speed here on purpose:
//! `PSRAM_MHZ` is the only speed, faults are fixed at that clock (refresh, timing), never by slowing down.
//!
//! The CPU/AXI clock must be at least the PSRAM clock (IDF `clk_utils.c` BELOW_FREQ_THRESHOLD on the P4: the MSPI write
//! FIFO can overflow otherwise). This module does not touch the CPU clock; `init` only reports it (`cpu_clock_ok`).
pub mod clockmath;
pub mod dma;
pub mod ldo;
pub mod map;
pub mod mspi;
pub mod patterns;
pub mod pll;
pub mod regs;
pub mod tune;

use crate::mmio::{modify32, read32, write32};

pub const PSRAM_MHZ: u32 = 120;
pub const MPLL_MHZ: u32 = 480;
const BUS_DIV: u32 = MPLL_MHZ / PSRAM_MHZ;
const LOW_SPEED_MHZ: u32 = 20; // mspi_timing_get_psram_low_speed_freq_mhz(): reference data and tuning start

/// How far `init` goes (the probe runs the stages one image at a time).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Upto {
    /// LDO on only.
    Ldo,
    /// + MPLL at 480 MHz.
    Mpll,
    /// + controller clocks and the chip's mode registers (FIFO path only, no AXI access yet).
    Chip,
    /// + AXI configuration and timing tuning: PSRAM usable through the cache once an MMU page is mapped.
    Tuned,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The MPLL calibration never finished.
    MpllCalibration,
    /// The write/read at address 0 did not come back (IDF: "PSRAM chip is not connected").
    NotConnected,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Info {
    pub ldo_dref: u8,
    pub ldo_mul: u8,
    pub mpll_mhz: u32,
    pub modes_before: mspi::ModeRegs,
    pub modes: mspi::ModeRegs,
    pub size_bytes: u32,
    pub tuned: Option<tune::Tuned>,
}

/// Progress lines for the caller's console; this crate prints nothing itself.
pub type Log<'a> = &'a mut dyn FnMut(core::fmt::Arguments);

/// `psram_ctrlr_ll_set_core_clock_div(1)` + `enable_core_clock` + both bus clocks for `mhz`
/// (`mspi_timing_config_set_psram_clock`: divider = MPLL / core divider 1 / mhz).
unsafe fn set_psram_clock(mhz: u32) {
    modify32(regs::PERI_CLK_CTRL00, regs::CORE_CLOCK_DIV_MASK, 0);
    modify32(regs::PERI_CLK_CTRL00, 0, regs::CORE_CLOCK_EN);
    regs::set_bus_clock(MPLL_MHZ / mhz);
}

/// `mspi_timing_psram_config_clear_tuning_regs`
unsafe fn clear_tuning() {
    regs::set_dqs_phase(0);
    regs::set_delaylines(0, 0);
}

/// Bench override of the tuning choice (the probe's way to compare tuning points at 120 MHz): `None` keeps IDF's pick.
#[derive(Clone, Copy, Debug, Default)]
pub struct TuneOverride {
    pub phase: Option<u32>,
    pub data_dl: Option<u32>,
    pub dqs_dl: Option<u32>,
}

/// Bring the PSRAM up to `upto`. Returns what was set and read; every step is logged through `log`.
///
/// # Safety
/// Owns the PSRAM supply, MPLL, the PSRAM controller and its pins: call once, from the boot hart, with nothing else
/// using PSRAM or the MSPI IOMUX. Reads two eFuse read registers (`ldo::read_ldo_efuse`) and writes none.
pub unsafe fn init(upto: Upto, over: TuneOverride, log: Log) -> Result<Info, Error> {
    let mut info = Info::default();

    // 1. LDO, then MPLL (esp_clk_tree_mpll_acquire, esp_clk_tree_mpll_freq_set)
    let e = ldo::read_ldo_efuse();
    let (dref, mul) = ldo::ldo_unit1_dref_mul(ldo::LDO_PSRAM_MV, &e);
    (info.ldo_dref, info.ldo_mul) = (dref, mul);
    log(format_args!(
        "ldo: efuse blk_version={} vo2_dref={} vo2_mul={}; programming dref={dref} mul={mul} for {} mV on channel {}",
        e.blk_version,
        e.vo2_dref,
        e.vo2_mul,
        ldo::LDO_PSRAM_MV,
        ldo::LDO_PSRAM_CHANNEL
    ));
    ldo::ldo_psram_enable(dref, mul, mspi::delay_us);
    let (l, a) = ldo::ldo_psram_state();
    let f = ldo::ldo_fields(l, a);
    log(format_args!("ldo: now xpd={} sw_owner={} tieh={} dref={} mul={}", f[0], f[1], f[3], f[4], f[5]));
    if upto == Upto::Ldo {
        return Ok(info);
    }
    pll::mpll_enable();
    let mpll = pll::mpll_configure(MPLL_MHZ, 2_000_000).ok_or(Error::MpllCalibration)?;
    info.mpll_mhz = mpll;
    log(format_args!("mpll: configured {MPLL_MHZ} MHz, analog registers read back {mpll} MHz"));
    if upto == Upto::Mpll {
        return Ok(info);
    }

    // 2. controller (esp_psram_impl_enable, after the MPLL)
    regs::apply(&regs::MODULE_CLOCK_ON);
    write32(regs::HP_RST_EN0, read32(regs::HP_RST_EN0) | regs::RST_AXI);
    write32(regs::HP_RST_EN0, read32(regs::HP_RST_EN0) | regs::RST_APB);
    write32(regs::HP_RST_EN0, read32(regs::HP_RST_EN0) & !regs::RST_APB);
    write32(regs::HP_RST_EN0, read32(regs::HP_RST_EN0) & !regs::RST_AXI);
    regs::apply(&regs::CLK_SRC_MPLL);
    regs::pin_drv_set(2);
    regs::apply(&regs::DQS_ON);
    regs::apply(&regs::CS_AND_ECC);
    regs::set_bus_clock(BUS_DIV);
    regs::apply(&regs::DLL_ON);
    log(format_args!("ctrl: module clock on, reset, MPLL source, bus clock 480 / {BUS_DIV} = {PSRAM_MHZ} MHz, DLL on"));

    // 3. the chip: mode registers (latency codes, refresh 4x, burst), then the connection check
    info.modes_before = mspi::read_mode_regs();
    mspi::init_mode_regs();
    if !mspi::check_connected() {
        return Err(Error::NotConnected);
    }
    info.modes = mspi::read_mode_regs();
    info.size_bytes = info.modes.size_bytes();
    log(format_args!(
        "chip: vendor={:#x} density={} ({} MB) good_die={} mr0={:#04x} mr4={:#04x} (refresh={}) mr8={:#04x}",
        info.modes.vendor_id(),
        info.modes.density(),
        info.size_bytes >> 20,
        info.modes.known_good_die(),
        info.modes.mr0,
        info.modes.mr4,
        info.modes.refresh(),
        info.modes.mr8
    ));
    if upto == Upto::Chip {
        return Ok(info);
    }

    // 4. AXI configuration and tuning (mspi_timing_psram_tuning)
    regs::apply(&regs::CONFIG_FOR_PSRAM);
    set_psram_clock(LOW_SPEED_MHZ);
    clear_tuning();
    mspi::write_data(&tune::REFERENCE, tune::TEST_ADDR);
    regs::apply(&[regs::VAR_DUMMY_FIFO_OFF]);
    set_psram_clock(PSRAM_MHZ);
    let mut matches = || {
        let mut got = [0u32; tune::TEST_WORDS];
        mspi::read_data(&mut got, tune::TEST_ADDR);
        got == tune::REFERENCE
    };
    let t = tune::sweep(&mut matches);
    log(format_args!(
        "tune: phase id {} (run {}{}), delay line id {} data {} dqs {} (run {}{}); good reads of 100 per delay line: {:?}",
        t.phase_id,
        t.phase_run,
        if t.phase_ok { "" } else { ", FALLBACK" },
        t.delayline_id,
        t.data_dl,
        t.dqs_dl,
        t.delayline_run,
        if t.delayline_ok { "" } else { ", FALLBACK" },
        t.dl_good
    ));
    let mut t = t;
    if over.phase.is_some() || over.data_dl.is_some() || over.dqs_dl.is_some() {
        let phase = over.phase.unwrap_or(tune::PHASES[t.phase_id]);
        (t.data_dl, t.dqs_dl) = (over.data_dl.unwrap_or(t.data_dl), over.dqs_dl.unwrap_or(t.dqs_dl));
        regs::set_dqs_phase(phase);
        regs::set_delaylines(t.data_dl, t.dqs_dl);
        log(format_args!("tune: OVERRIDE phase {phase} data {} dqs {}", t.data_dl, t.dqs_dl));
    }
    set_psram_clock(PSRAM_MHZ);
    regs::apply(&regs::VAR_DUMMY_ON);
    info.tuned = Some(t);
    Ok(info)
}
