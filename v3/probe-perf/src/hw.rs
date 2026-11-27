//! The probe's own hardware access: time, the PSRAM rail step, the die temperature, cache and DMA registers. Everything the platform
//! keeps private and #648 needs is re-declared here (the platform's psram files belong to #650 meanwhile); the words and fields come
//! from `lichen-perflab`, which tests them.
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use lichen_perflab::dma::{Burst, RX_MASK, TX_MASK};
use lichen_perflab::ldo::{self, Step};
use lichen_perflab::temp;
use lichen_platform_esp32p4::mmio::{modify32, read32, write32};
use lichen_platform_esp32p4::psram::systimer;

pub const TICK_HZ: u64 = 16_000_000;

/// SYSTIMER ticks (16 MHz, XTAL-derived, so 360 and 400 MHz runs time the same). 0 if the timer does not answer.
pub fn ticks() -> u64 {
    systimer::now_ticks().unwrap_or(0)
}

/// Wait on SYSTIMER, not on the ROM delay: `cpu_to` tells the ROM 360 MHz even for the 400 MHz target, so ROM delays run short there.
pub fn wait_us(us: u32) {
    let t0 = ticks();
    let mut guard = 0u32;
    while ticks().saturating_sub(t0) < us as u64 * 16 {
        guard += 1;
        if guard > 50_000_000 {
            return; // a dead timer must not hang the run
        }
    }
}

pub fn mcycle() -> u32 {
    let v: u32;
    // SAFETY: a read of the cycle counter CSR.
    unsafe { core::arch::asm!("csrr {}, mcycle", out(reg) v) };
    v
}

// ---- the PSRAM rail ------------------------------------------------------------------------------------------------------------

/// Set before the step is written, cleared once the calibrated pair is read back: while it is set every exit path restores 12/2.
static STEPPED: AtomicBool = AtomicBool::new(false);

pub fn ldo_state() -> ldo::State {
    // SAFETY: the two PMU words of the PSRAM rail, reads.
    unsafe { ldo::decode(read32(ldo::LDO_REG), read32(ldo::ANA_REG)) }
}

/// Put the calibrated pair back: ONE masked write, nothing printed first. Safe to call on any path, any number of times. The caller
/// verifies with `ldo_restore_verify` once it may print.
pub fn ldo_restore_raw() {
    if STEPPED.load(Relaxed) {
        // SAFETY: the same register and masked write as the step, with the calibrated pair.
        unsafe { modify32(ldo::ANA_REG, ldo::MASK, Step::Calibrated.bits()) };
    }
}

/// After `ldo_restore_raw`: settle, read back, report. Returns whether the rail is on the calibrated pair (or was never moved).
pub fn ldo_restore_verify() -> bool {
    if !STEPPED.load(Relaxed) {
        return true;
    }
    wait_us(ldo::SETTLE_US);
    let st = ldo_state();
    let ok = st.is(Step::Calibrated);
    crate::con::pass_if(
        ok,
        "648.ldo.restore",
        format_args!("dref={} mul={} xpd={} tieh={} (12/2, xpd 1, tieh 0 expected)", st.dref, st.mul, st.xpd, st.tieh),
    );
    if ok {
        STEPPED.store(false, Relaxed);
    }
    ok
}

/// The 12/2 -> 7/5 step. Refuses (and writes nothing) unless the rail is on and on the calibrated pair. The caller has checked that the
/// die temperature can be read.
pub fn ldo_step(to: Step) -> bool {
    let st = ldo_state();
    if let Err(why) = ldo::plan(&st, to) {
        crate::con::check(
            "648.ldo.step",
            lichen_kcore::check::Verdict::Fail,
            format_args!("refused: {why:?} (state {st:?})"),
        );
        return false;
    }
    STEPPED.store(true, Relaxed);
    // SAFETY: one masked 32-bit write of dref and mul together (read, merge, one write); XPD and TIEH are in the other register and
    // untouched. The restore path undoes it.
    unsafe { modify32(ldo::ANA_REG, ldo::MASK, to.bits()) };
    wait_us(ldo::SETTLE_US);
    let now = ldo_state();
    let ok = now.is(to);
    crate::con::pass_if(
        ok,
        "648.ldo.step",
        format_args!(
            "dref={} mul={} xpd={} tieh={} read back after {} us (expected {}/{})",
            now.dref,
            now.mul,
            now.xpd,
            now.tieh,
            ldo::SETTLE_US,
            to.dref(),
            to.mul()
        ),
    );
    if !ok {
        ldo_restore_raw();
        ldo_restore_verify();
    }
    ok
}

// ---- die temperature (LP_TSENS) ------------------------------------------------------------------------------------------------

const LPPERI: usize = 0x5012_0000;
const LPPERI_CLK_EN: usize = LPPERI; // bit 17 ck_en_lp_tsens, bit 27 i2cmst
const LPPERI_RESET_EN: usize = LPPERI + 0x8; // bit 18 rst_en_lp_tsens
const TSENS_CTRL: usize = 0x5012_F000; // out [7:0], ready 8, sample_en 9, power_up 22
const ANA_MST: usize = 0x5012_4000;
const ANA_CONF1: usize = ANA_MST + 0x1C;
const ANA_CONF2: usize = ANA_MST + 0x20;
const CLK160M: usize = ANA_MST + 0x34;
const SAR_ADC: (u32, u32) = (0x69, 1 << 7); // regi2c block, REGI2C_SAR_I2C_MST_SEL
const TSENS_DAC_REG: u32 = 6; // [3:0]
const PMU_RF_PWC: usize = 0x5011_515C; // bit 26 PERIF_I2C_RSTB, bit 27 XPD_PERIF_I2C

static TEMP_OK: AtomicBool = AtomicBool::new(false);
static TEMP_RANGE: AtomicUsize = AtomicUsize::new(temp::FIRST_RANGE);

unsafe fn i2c_wait() {
    while read32(ANA_MST) & (1 << 25) != 0 {}
}
unsafe fn i2c_write(reg: u32, data: u32) {
    modify32(ANA_CONF2, 0x00FF_FFFF, 0);
    modify32(ANA_CONF1, 0x00FF_FFFF, 0);
    modify32(ANA_CONF2, 0, SAR_ADC.1);
    i2c_wait();
    write32(ANA_MST, SAR_ADC.0 | reg << 8 | 1 << 24 | data << 16);
    i2c_wait();
}
unsafe fn i2c_read(reg: u32) -> u32 {
    modify32(ANA_CONF2, 0x00FF_FFFF, 0);
    modify32(ANA_CONF1, 0x00FF_FFFF, 0);
    modify32(ANA_CONF2, 0, SAR_ADC.1);
    i2c_wait();
    write32(ANA_MST, SAR_ADC.0 | reg << 8);
    i2c_wait();
    (read32(ANA_MST) >> 16) & 0xFF
}
fn set_range(i: usize) {
    // SAFETY: the TSENS_DAC field [3:0] of the SAR ADC analog block, read-modify-write as REGI2C_WRITE_MASK does.
    unsafe {
        let cur = i2c_read(TSENS_DAC_REG);
        i2c_write(TSENS_DAC_REG, (cur & !0xF) | temp::RANGES[i].reg_val as u32);
    }
    TEMP_RANGE.store(i, Relaxed);
}

/// IDF's install + enable: clock, reset pulse, range, power up and sample, 300 us. Returns whether a reading came back.
pub fn temp_init() -> bool {
    // SAFETY: LP peripheral clock and reset bits, the analog I2C master clock (as `clock::cpu_to` leaves it) and the sensor's control
    // word; nothing here touches the supply or a clock tree.
    unsafe {
        modify32(LPPERI_CLK_EN, 0, 1 << 27 | 1 << 17);
        modify32(CLK160M, 0, 1);
        modify32(LPPERI_RESET_EN, 0, 1 << 18);
        modify32(LPPERI_RESET_EN, 1 << 18, 0);
        // regi2c_ctrl_ll_i2c_sar_periph_enable(): the internal I2C bus to the SAR ADC and TSENS registers, reset, power, release
        modify32(PMU_RF_PWC, 1 << 26, 0);
    }
    wait_us(2);
    // SAFETY: as above.
    unsafe {
        modify32(PMU_RF_PWC, 0, 1 << 27);
        modify32(PMU_RF_PWC, 0, 1 << 26);
    }
    set_range(temp::FIRST_RANGE);
    // SAFETY: as above; power_up (22) and sample_en (9).
    unsafe { modify32(TSENS_CTRL, 0, 1 << 22 | 1 << 9) };
    wait_us(300);
    let ok = temp_celsius_x100().is_some();
    TEMP_OK.store(ok, Relaxed);
    ok
}

/// The sensor's control word, for the log when it gives no reading.
pub fn temp_ctrl_word() -> u32 {
    // SAFETY: the sensor's control word, a read.
    unsafe { read32(TSENS_CTRL) }
}

pub fn temp_available() -> bool {
    TEMP_OK.load(Relaxed)
}

/// The sensor's `out` byte, as IDF reads it (the `ready` flag belongs to the wake-up path and never sets here). Three reads, the last
/// one counts; 0 means the sensor is not running.
fn temp_raw() -> Option<u32> {
    let mut v = 0;
    for _ in 0..3 {
        // SAFETY: the sensor's control word, a read.
        v = unsafe { read32(TSENS_CTRL) } & 0xFF;
        wait_us(50);
    }
    (v != 0).then_some(v)
}

/// Degrees in hundredths, uncalibrated (see `temp`), switching to a better range when the reading is near the edge of this one.
pub fn temp_celsius_x100() -> Option<i32> {
    let mut range = TEMP_RANGE.load(Relaxed);
    for _ in 0..2 {
        let c = temp::celsius_x100(temp_raw()?, temp::RANGES[range].offset);
        let better = temp::best_range(c, range);
        if better == range {
            return (-4000..=12_500).contains(&c).then_some(c);
        }
        set_range(better);
        range = better;
        wait_us(300);
    }
    None
}

// ---- cache and DMA registers -----------------------------------------------------------------------------------------------------

const CACHE: usize = 0x3FF1_0000;
/// (L2 size in bytes or 0, L2 line in bytes), as the controller reports them.
pub fn l2_readback() -> (usize, usize) {
    // SAFETY: cache controller configuration registers, reads.
    let (size, line) = unsafe { (read32(CACHE + 0x278), read32(CACHE + 0x27C)) };
    (
        lichen_kcore::l2::cache_size_bytes(size).unwrap_or(0),
        lichen_platform_esp32p4::psram::map::line_bytes_from_reg(line),
    )
}

const AXI_DMA: usize = 0x5008_A000;
const RX_CONF0: usize = AXI_DMA + 0x10;
const TX_CONF0: usize = AXI_DMA + 0x148;
/// Burst size after `dma::init` (which leaves 32 B).
///
/// # Safety
/// The AXI-DMA block after `dma::init`, no transfer running.
pub unsafe fn dma_set_burst(b: Burst) {
    modify32(RX_CONF0, RX_MASK, b.rx());
    modify32(TX_CONF0, TX_MASK, b.tx());
}
pub fn dma_burst_readback() -> u32 {
    // SAFETY: AXI-DMA channel configuration, a read.
    let (rx, tx) = unsafe { (read32(RX_CONF0), read32(TX_CONF0)) };
    let (rc, tc) = ((rx & RX_MASK) >> 4, (tx & TX_MASK) >> 5);
    if rc == tc {
        8 << rc
    } else {
        0
    }
}

pub fn ldo_stepped() -> bool {
    STEPPED.load(Relaxed)
}
