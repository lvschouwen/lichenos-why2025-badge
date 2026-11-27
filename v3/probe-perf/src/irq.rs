//! The CLINT timer: its accuracy against the SYSTIMER and the CPU cycle counter, and the latency and jitter of a 1 kHz timer
//! interrupt, with core 0 idle in internal RAM and with it streaming random PSRAM reads (cache misses in the way of the handler).
//! The handler is the vector in boot.S (saves the caller-saved registers on the trap stack, calls `ptrap`, returns with mret);
//! latency is the first mtime read in the handler minus the programmed compare value, so it includes the register save.
use crate::con::{check, perf};
use crate::hw::{mcycle, ticks, TICK_HZ};
use crate::mem::*;
use core::sync::atomic::{AtomicU32, Ordering::Relaxed};
use lichen_kcore::check::Verdict;
use lichen_perflab::irq::{cpu_mhz_x100, irq_stats, mtime_hz, ppm_vs, ticks_to_ns};
use lichen_perflab::line::Val;
use lichen_perflab::rng::Rng;
use lichen_platform_esp32p4::mmio::read32;
use lichen_platform_esp32p4::{clic, clint, trap};

const LINE: usize = 7; // the machine timer
/// mtime low word, read once at handler entry: the three-read `clint::mtime()` would put two more MMIO reads into the latency.
const MTIME_LO: usize = clint::BASE + 0xBFF8;
/// The CLINT tick rate, measured by `accuracy` first.
static HZ: AtomicU32 = AtomicU32::new(40_000_000);
const SAMPLES: usize = 1000;

// u32: at a CPU clock of 360 MHz the counter ticks every 2.8 ns, so a 1 ms gap is 360 000 ticks
static mut LAT: [u32; SAMPLES] = [0; SAMPLES];
static mut GAP: [u32; SAMPLES] = [0; SAMPLES];
static N: AtomicU32 = AtomicU32::new(0);
static TARGET_LO: AtomicU32 = AtomicU32::new(0);
static TARGET_HI: AtomicU32 = AtomicU32::new(0);
static LAST_LO: AtomicU32 = AtomicU32::new(0);

/// The CLINT timer interrupt, called from `ptrap` with interrupts off (CLIC level 0xFF, MIE cleared on entry). Latency and gaps are
/// taken from the low 32 bits of mtime (a 1 ms period is far inside the wrap); the compare value is kept in 64 bits.
pub fn timer_isr() {
    // SAFETY: the CLINT mtime low word, a read.
    let now = unsafe { read32(MTIME_LO) };
    let target = (TARGET_HI.load(Relaxed) as u64) << 32 | TARGET_LO.load(Relaxed) as u64;
    let i = N.load(Relaxed) as usize;
    if i < SAMPLES {
        // SAFETY: the one writer is this handler (core 0, not reentrant); main reads after interrupts are off.
        unsafe {
            (*core::ptr::addr_of_mut!(LAT))[i] = now.wrapping_sub(target as u32);
            (*core::ptr::addr_of_mut!(GAP))[i] = now.wrapping_sub(LAST_LO.load(Relaxed));
        }
    }
    LAST_LO.store(now, Relaxed);
    N.store(i as u32 + 1, Relaxed);
    let next = target + period() as u64;
    TARGET_LO.store(next as u32, Relaxed);
    TARGET_HI.store((next >> 32) as u32, Relaxed);
    clint::set_cmp(next);
}

fn period() -> u32 {
    HZ.load(Relaxed) / 1000
}

/// 100 ms of the CLINT counter, the SYSTIMER and mcycle side by side; sets the tick rate the interrupt test converts with.
fn accuracy() {
    let (m0, s0, c0) = (clint::mtime(), ticks(), mcycle());
    while ticks() - s0 < TICK_HZ / 10 {}
    let (m1, s1, c1) = (clint::mtime(), ticks(), mcycle());
    let (dm, ds, dc) = (m1 - m0, s1 - s0, c1.wrapping_sub(c0) as u64);
    let hz = mtime_hz(dm, ds);
    HZ.store(hz as u32, Relaxed);
    perf(
        "lab.timer.accuracy",
        &[
            ("mtime_khz", Val::U(hz / 1000)),
            ("mtime_vs_40mhz_ppm", Val::I(ppm_vs(hz, 40_000_000))),
            ("cpu_mhz", Val::F2(cpu_mhz_x100(dc, ds) as i64)),
            ("window_ms", Val::U(ds * 1000 / TICK_HZ)),
        ],
    );
}

/// What core 0 does between interrupts.
#[derive(Clone, Copy)]
enum Load {
    Idle,
    PsramRandom,
}

fn run_load(metric: &str, load: Load) -> bool {
    N.store(0, Relaxed);
    let start = clint::mtime() + 4 * period() as u64; // a few ms to get going before the first interrupt
    TARGET_LO.store(start as u32, Relaxed);
    TARGET_HI.store((start >> 32) as u32, Relaxed);
    LAST_LO.store(start as u32, Relaxed);
    clint::set_cmp(start);
    // SAFETY: the vector in boot.S handles the interrupt (ptrap -> timer_isr) and returns with mret; nothing here holds a borrow
    // of what the handler writes.
    unsafe { trap::irq_enable() };
    let (t0, mut rng, base) = (ticks(), Rng::new(7), cached(PSRAM_OFFSETS.chase));
    let mut stalled = false;
    while (N.load(Relaxed) as usize) < SAMPLES {
        match load {
            Load::Idle => core::hint::spin_loop(),
            Load::PsramRandom => {
                for _ in 0..64 {
                    core::hint::black_box(rd(base + rng.below(2 << 20) as usize * 4));
                }
            }
        }
        if ticks() - t0 > 3 * TICK_HZ {
            stalled = true;
            break;
        }
    }
    trap::irq_disable();
    clint::disarm();
    if stalled {
        check(
            "648.irq",
            Verdict::Fail,
            format_args!("{metric}: only {} of {SAMPLES} interrupts in 3 s", N.load(Relaxed)),
        );
        return false;
    }
    report(metric)
}

fn report(metric: &str) -> bool {
    // SAFETY: interrupts are off, so the handler no longer writes LAT and GAP; sorting LAT in place is fine.
    let (lat, gap) = unsafe { (&mut *core::ptr::addr_of_mut!(LAT), &*core::ptr::addr_of!(GAP)) };
    // the first gap runs from the arming, not from an interrupt: it is left out
    let Some(s) = irq_stats(lat, &gap[1..], period() as u64) else { return false };
    let hz = HZ.load(Relaxed) as u64;
    let ns = |t: u64| Val::U(ticks_to_ns(t, hz));
    perf(
        metric,
        &[
            ("latency_median_ns", ns(s.latency.median as u64)),
            ("latency_p95_ns", ns(s.latency.p95 as u64)),
            ("latency_max_ns", ns(s.latency.max as u64)),
            ("jitter_max_ns", ns(s.jitter)),
            ("gap_min_ns", ns(s.gap_min as u64)),
            ("gap_max_ns", ns(s.gap_max as u64)),
            ("period_ns", Val::U(ticks_to_ns(period() as u64, hz))),
            ("samples", Val::U(SAMPLES as u64)),
        ],
    );
    true
}

pub fn run() -> bool {
    crate::con::begin("irq");
    accuracy();
    clic::configure(LINE, clic::ATTR_M_LEVEL, clic::CTL_MAX);
    let thresh = clic::set_threshold(0);
    clic::set_enabled(LINE, true);
    let line = clic::line(LINE);
    check(
        "648.irq.setup",
        Verdict::Info,
        format_args!("clic line {LINE}: ip/ie/attr/ctl {line:02x?}, threshold {thresh}"),
    );
    wb_inv(cached(PSRAM_OFFSETS.chase), 8 << 20);
    let ok = run_load("lab.irq.idle", Load::Idle) && run_load("lab.irq.psram_load", Load::PsramRandom);
    clic::set_enabled(LINE, false);
    ok
}
