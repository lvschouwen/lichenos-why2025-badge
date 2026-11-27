//! #648 perf lab probe (RAM-only, load-ram). One image does one configuration of the matrix, fixed at build time (`build.sh`,
//! `LICHEN_PERF_*`): CPU 360 or 400 MHz, PSRAM 120, 160 or 200 MHz, L2 size and line, DMA burst, and the one LDO step.
//! Output is the bench-log protocol (CHECK, PERF, END) read by `tools/v3_bench_report.py` and `tools/perf_report.py`.
//!
//! Safety, in this order: the PSRAM rail is put back on the calibrated pair by a single raw masked write before anything else on every
//! exit (normal end, failure, panic, trap); the step is refused unless the die temperature can be read; nothing writes an eFuse or the
//! flash; PSRAM is written only through the cached alias or by DMA.
#![no_std]
#![no_main]

use core::arch::global_asm;
use lichen_kcore::check::{write_end, Verdict};
use lichen_perflab::cfg::{parse_ldo, parse_u32, Cfg};
use lichen_perflab::dma::Burst;
use lichen_perflab::l2::L2Cfg;
use lichen_perflab::ldo::Step;
use lichen_perflab::line::Val;
use lichen_perflab::temp::STOP_C;
use lichen_platform_esp32p4::psram::map::{self, CACHED_BASE, PAGE_BYTES};
use lichen_platform_esp32p4::psram::{self, dma};
use lichen_platform_esp32p4::{clock, uart};

#[macro_use]
mod con;
mod cores;
mod cpu;
mod dmabench;
mod hw;
mod integrity;
mod irq;
mod mem;
mod membench;

global_asm!(include_str!("boot.S"));

const fn num(v: Option<u32>) -> u32 {
    match v {
        Some(x) => x,
        None => panic!("a LICHEN_PERF_* option is not a decimal number"),
    }
}
const CFG: Cfg = Cfg {
    cpu_mhz: num(parse_u32(env!("LICHEN_PERF_CPU"))),
    psram_mhz: num(parse_u32(env!("LICHEN_PERF_PSRAM"))),
    l2: L2Cfg { kb: num(parse_u32(env!("LICHEN_PERF_L2KB"))), line: num(parse_u32(env!("LICHEN_PERF_L2LINE"))) },
    burst: match Burst::from_bytes(num(parse_u32(env!("LICHEN_PERF_DMA")))) {
        Some(b) => b,
        None => panic!("LICHEN_PERF_DMA must be 32, 64 or 128"),
    },
    ldo: match parse_ldo(env!("LICHEN_PERF_LDO")) {
        Some(s) => s,
        None => panic!("LICHEN_PERF_LDO must be cal or 1900"),
    },
};
const _: () = match CFG.check() {
    Ok(()) => {}
    Err(_) => panic!("impossible LICHEN_PERF_* combination (see perflab/src/cfg.rs)"),
};
const _: () = assert!(CFG.l2.fits_below(0x4FF3_A000), "link.ld NOLOAD window must stay below the cache carve");
const SUITE: &str = env!("LICHEN_PERF_SUITE");
/// SYSTIMER ticks (low 32 bits: 268 s of range) at the start of the image.
/// The MPLL frequency the PSRAM init read back (MHz).
static MPLL_READ: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
static START_TICKS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
const SECS: u32 = num(parse_u32(env!("LICHEN_PERF_SECS")));

#[panic_handler]
fn panic(i: &core::panic::PanicInfo) -> ! {
    hw::ldo_restore_raw();
    pl!("probe-perf: PANIC {i}");
    con::check("648.panic", Verdict::Fail, format_args!("panic"));
    finish()
}

/// A trap or interrupt, called from the vector in boot.S. Returns only for the CLINT timer interrupt of the irq suite; any other cause
/// is a result: the rail back first, report it once, end. The probe takes no other trap on purpose.
#[no_mangle]
extern "C" fn ptrap(mcause: usize) {
    // CLIC mode: bit 31 interrupt, [11:0] the code (the machine timer is 7); the middle bits are mpp/mpie/mpil.
    if mcause >> 31 == 1 && mcause & 0xFFF == 7 {
        irq::timer_isr();
        return;
    }
    hw::ldo_restore_raw();
    let (e, t, hart): (usize, usize, usize);
    // SAFETY: reads of M-mode trap CSRs.
    unsafe {
        core::arch::asm!("csrr {}, mepc", "csrr {}, mtval", "csrr {}, mhartid", out(reg) e, out(reg) t, out(reg) hart)
    };
    if hart != 0 {
        // core 1 only leaves its mark and parks: core 0 reports it (one END, one UART writer, one hart on the analog I2C master)
        cores::core1_trapped(mcause as u32, e as u32);
        loop {
            // SAFETY: wfi only stalls the hart.
            unsafe { core::arch::asm!("wfi") };
        }
    }
    con::check("648.trap", Verdict::Fail, format_args!("mcause={mcause:#x} mepc={e:#x} mtval={t:#x}"));
    finish()
}

/// The one way out. The rail first (a raw write, nothing printed before it), then its read-back, the last temperature, END.
fn finish() -> ! {
    hw::ldo_restore_raw();
    cores::report_core1_trap();
    hw::ldo_restore_verify();
    if hw::temp_available() {
        temp_checkpoint("lab.temp.end");
    }
    // how long the run took (SYSTIMER since the image started), so the load-ram timeout can be chosen from a measurement
    let ms = (hw::ticks() as u32).wrapping_sub(START_TICKS.load(core::sync::atomic::Ordering::Relaxed)) as u64 * 1000
        / hw::TICK_HZ;
    con::perf("lab.run", &[("elapsed_ms", Val::U(ms))]);
    uart::drain();
    let _ = write_end(&mut con::Con, con::end_reason());
    uart::drain();
    loop {
        core::hint::spin_loop();
    }
}

/// Log the die temperature; false when it is at the stop limit (or, with the rail stepped, unreadable).
fn temp_checkpoint(metric: &str) -> bool {
    match hw::temp_celsius_x100() {
        Some(c) => {
            con::perf(metric, &[("celsius", Val::F2(c as i64))]);
            let hot = c >= STOP_C * 100;
            if hot {
                con::check(
                    "648.thermal",
                    Verdict::Fail,
                    format_args!("{c} centi-degrees at or above the {STOP_C} C stop"),
                );
            }
            !hot
        }
        None if hw::ldo_stepped() => {
            con::check("648.thermal", Verdict::Fail, format_args!("die temperature unreadable with the rail stepped"));
            false
        }
        None => true,
    }
}

fn clock_step() -> bool {
    let target = if CFG.cpu_mhz == 400 { clock::CpuClock::Mhz400 } else { clock::CpuClock::Mhz360 };
    let mut log = |a: core::fmt::Arguments| pl!("{a}");
    // SAFETY: the one caller, boot hart, RAM-only image that owns the machine (as probe-psram).
    match unsafe { clock::cpu_to_mhz(&mut log, target) } {
        Ok((_, Some(m))) => {
            let want = CFG.cpu_mhz;
            con::pass_if(
                m * 100 >= want * 97 && m * 100 <= want * 103,
                "648.cpu",
                format_args!("measured {m} MHz, {want} expected"),
            );
            true
        }
        Ok(_) => {
            con::check("648.cpu", Verdict::Fail, format_args!("clock set, timer did not count: no measurement"));
            false
        }
        Err(e) => {
            con::check("648.cpu", Verdict::Fail, format_args!("{e:?}"));
            false
        }
    }
}

/// The L2 cache as asked: only touched when it differs from what the ROM left. Before PSRAM is mapped, so nothing is cached through it.
fn l2_step() -> bool {
    let (size, line) = hw::l2_readback();
    pl!("l2: ROM state {} KB, {} B lines", size >> 10, line);
    if (size, line) != (CFG.l2.kb as usize * 1024, CFG.l2.line as usize) {
        // SAFETY: nothing is cached through L2 yet (PSRAM is not up), and the NOLOAD window is checked below the carve at build time.
        unsafe {
            map::cache_set_l2_mode(CFG.l2.size_code(), 8, CFG.l2.line_code());
            map::cache_invalidate_all_l2();
        }
    }
    let (size, line) = hw::l2_readback();
    let ok = (size, line) == (CFG.l2.kb as usize * 1024, CFG.l2.line as usize);
    con::pass_if(
        ok,
        "648.l2",
        format_args!("{} KB, {} B lines read back; carve starts at {:#x}", size >> 10, line, CFG.l2.carve()),
    );
    ok
}

/// # Safety
/// As `psram::init`: boot hart, nothing else using PSRAM.
unsafe fn psram_init(mhz: u32, log: &mut dyn FnMut(core::fmt::Arguments)) -> Result<psram::Info, psram::Error> {
    let speed = match mhz {
        160 => psram::SPEED_160,
        200 => psram::SPEED_200,
        _ => return psram::init(psram::Upto::Tuned, psram::TuneOverride::default(), log),
    };
    psram::init_with(psram::Upto::Tuned, psram::TuneOverride::default(), speed, log)
}

fn calibrated(r: &Result<psram::Info, psram::Error>) -> bool {
    matches!(r, Ok(i) if i.tuned.is_some_and(|t| t.phase_ok && t.delayline_ok) && i.size_bytes == mem::CHIP as u32)
}

fn psram_step() -> bool {
    let mut log = |a: core::fmt::Arguments| pl!("psram: {a}");
    // SAFETY: the one init call of this image (boot hart, RAM-only).
    let r = unsafe { psram_init(CFG.psram_mhz, &mut log) };
    let ok = calibrated(&r);
    if let Ok(i) = &r {
        MPLL_READ.store(i.mpll_mhz, core::sync::atomic::Ordering::Relaxed);
    }
    con::pass_if(
        ok,
        "648.psram",
        format_args!("{} MHz requested, calibrated: {ok} ({:?})", CFG.psram_mhz, r.as_ref().err()),
    );
    if !ok && CFG.psram_mhz != 120 {
        // the 120 MHz retry is the #650 evidence that a failed speed can be recovered from; the point itself stays failed
        pl!("psram: {} MHz failed, retrying 120 MHz", CFG.psram_mhz);
        // SAFETY: a second init on the same boot hart, as #650's recovery path expects.
        let r120 = unsafe { psram_init(120, &mut log) };
        con::check(
            "648.psram.retry120",
            if calibrated(&r120) { Verdict::Info } else { Verdict::Fail },
            format_args!(
                "120 MHz after the failed {} MHz: calibrated {} ({:?})",
                CFG.psram_mhz,
                calibrated(&r120),
                r120.as_ref().err()
            ),
        );
    }
    ok
}

fn map_and_dma() -> bool {
    // SAFETY: init reached Tuned; the L2 tags are undefined after a download-mode boot, so they are dropped first.
    let bad = unsafe {
        map::cache_invalidate_all_l2();
        let bad = map::map_identity_with_hole(mem::CHIP / PAGE_BYTES);
        bad + map::cache_invalidate(CACHED_BASE, mem::CHIP)
    };
    // SAFETY: the one init of the AXI-DMA block; the burst is changed while no transfer runs.
    unsafe {
        dma::init();
        hw::dma_set_burst(CFG.burst);
    }
    con::pass_if(
        bad == 0,
        "648.map",
        format_args!("{} MB mapped with the hole, {bad} cache sync failures", mem::CHIP >> 20),
    );
    bad == 0
}

/// What the registers say, not what was asked for.
fn config_report() -> bool {
    let cpu = clock::measure_mhz().unwrap_or(0);
    let (l2kb, l2line) = hw::l2_readback();
    let burst = hw::dma_burst_readback();
    let ldo = hw::ldo_state();
    // the bus divider the controller holds, over the MPLL the init read back
    let div = (hw::psram_bus_word() & 0xFF) + 1;
    let mpll = MPLL_READ.load(core::sync::atomic::Ordering::Relaxed);
    let ps = mpll / div; // low byte + 1: never 0
    con::perf(
        "lab.config",
        &[
            ("cpu_mhz", Val::U(cpu as u64)),
            ("psram_mhz", Val::U(ps as u64)),
            ("psram_bus_div", Val::U(div as u64)),
            ("l2_kb", Val::U((l2kb >> 10) as u64)),
            ("l2_line_b", Val::U(l2line as u64)),
            ("dma_burst_b", Val::U(burst as u64)),
            ("ldo_dref", Val::U(ldo.dref as u64)),
            ("ldo_mul", Val::U(ldo.mul as u64)),
        ],
    );
    let ok = ps == CFG.psram_mhz
        && cpu * 100 >= CFG.cpu_mhz * 97
        && cpu * 100 <= CFG.cpu_mhz * 103
        && l2kb == CFG.l2.kb as usize * 1024
        && l2line == CFG.l2.line as usize
        && burst == CFG.burst.bytes()
        && ldo.is(CFG.ldo);
    con::pass_if(
        ok,
        "648.config",
        format_args!("{CFG} requested; cpu {cpu} MHz measured, read back from the registers"),
    );
    ok
}

fn bring_up() -> bool {
    if !clock_step() {
        return false;
    }
    let temp = hw::temp_init();
    // an unreadable sensor is a gap in the data for a normal run, and a refusal for the voltage step
    let must = CFG.ldo != Step::Calibrated;
    con::check(
        "648.temp",
        if temp || !must { Verdict::Info } else { Verdict::Fail },
        format_args!(
            "die temperature sensor {} (control word {:#010x})",
            if temp { "reads" } else { "gives no reading" },
            hw::temp_ctrl_word()
        ),
    );
    if !temp && must {
        return false; // no step without a temperature
    }
    if temp && !temp_checkpoint("lab.temp.start") {
        return false;
    }
    if !(l2_step() && psram_step() && map_and_dma()) {
        return false;
    }
    if CFG.ldo != Step::Calibrated && !hw::ldo_step(CFG.ldo) {
        return false;
    }
    config_report()
}

/// Seconds of integrity passes: the build option, else a default per suite.
const fn integrity_secs() -> u32 {
    if SECS != 0 {
        SECS
    } else if matches!(SUITE.as_bytes(), b"smoke") {
        10
    } else {
        60
    }
}

fn suites() -> bool {
    let all = matches!(SUITE.as_bytes(), b"smoke" | b"all");
    let want = |s: &str| all || SUITE == s;
    if !(all
        || matches!(SUITE.as_bytes(), b"config" | b"mem" | b"cpu" | b"irq" | b"dual" | b"dma" | b"integrity" | b"park"))
    {
        con::check("648.suite", Verdict::Fail, format_args!("unknown suite {SUITE}"));
        return false;
    }
    if want("mem") && !membench::run() {
        return false;
    }
    if want("cpu") {
        cpu::run();
    }
    if want("irq") && !irq::run() {
        return false;
    }
    if want("dma") && !dmabench::run(CFG.burst.bytes()) {
        return false;
    }
    if want("integrity") && !integrity::run(integrity_secs(), &mut || temp_checkpoint("lab.temp.integrity")) {
        return false;
    }
    if want("park") && !integrity::park(5) {
        return false;
    }
    // last: core 1 keeps polling its flag after the suite, which would disturb whatever ran after it
    !want("dual") || cores::run(CFG.cpu_mhz)
}

fn banner() {
    pl!("lichen-v3 {} platform esp32p4 perflab {CFG} (#648, RAM only)", env!("LICHEN_PERF_ID"));
}

#[no_mangle]
extern "C" fn pmain() -> ! {
    START_TICKS.store(hw::ticks() as u32, core::sync::atomic::Ordering::Relaxed);
    // the first bytes after the ROM hand-over can be lost on the host side, so the banner comes twice
    banner();
    banner();
    con::check("648.cfg", Verdict::Info, format_args!("{CFG} suite {SUITE}"));
    if bring_up() {
        let ok = suites();
        let fails = mem::CACHE_FAILS.load(core::sync::atomic::Ordering::Relaxed);
        con::pass_if(ok && fails == 0, "648.run", format_args!("suites done, {fails} cache sync failures"));
    }
    finish()
}
