//! Both cores on the same memory. Core 1 is started by the platform's `core1::start` (IDF's sequence, proven in #608/#612) and
//! then only loops reading or writing a PSRAM region while core 0 measures; it never prints and never touches the UART, the
//! SYSTIMER, or core 1's own CLIC/CLINT windows. The two cores talk through atomics in internal RAM.
use crate::con::{check, perf};
use crate::hw::{mcycle, ticks, TICK_HZ};
use crate::mem::*;
use crate::membench::{chase_ring, read_words, write_words};
use core::sync::atomic::{AtomicU32, Ordering::SeqCst};
use lichen_kcore::check::Verdict;
use lichen_perflab::line::Val;
use lichen_perflab::rate::mb_s_x100;
use lichen_platform_esp32p4::core1;

extern "C" {
    fn _start1();
}

const IDLE: u32 = 0;
const READ: u32 = 1;
const WRITE: u32 = 2;
static MODE: AtomicU32 = AtomicU32::new(IDLE);
static GO: AtomicU32 = AtomicU32::new(0);
static STOP: AtomicU32 = AtomicU32::new(0);
/// 0 waiting, 1 running a job, 2 job done (results valid).
static STATE: AtomicU32 = AtomicU32::new(0);
static PASSES: AtomicU32 = AtomicU32::new(0);
static CYCLES: AtomicU32 = AtomicU32::new(0);

const REGION: usize = 4 << 20;

/// Core 1's main loop (from `_start1`).
#[no_mangle]
extern "C" fn core1_main() -> ! {
    loop {
        while GO.load(SeqCst) == 0 {
            core::hint::spin_loop();
        }
        GO.store(0, SeqCst);
        STATE.store(1, SeqCst);
        let (mode, start) = (MODE.load(SeqCst), mcycle());
        let (mut passes, mut last) = (0u32, start);
        while STOP.load(SeqCst) == 0 {
            match mode {
                READ => {
                    core::hint::black_box(read_words(cached(PSRAM_OFFSETS.write), REGION / 4));
                }
                WRITE => write_words(cached(PSRAM_OFFSETS.copy_dst), REGION / 4, passes),
                _ => core::hint::spin_loop(),
            }
            passes += 1;
            last = mcycle();
        }
        PASSES.store(passes, SeqCst);
        CYCLES.store(last.wrapping_sub(start), SeqCst);
        STATE.store(2, SeqCst);
    }
}

fn wait_state(want: u32, ms: u64) -> bool {
    let t0 = ticks();
    while STATE.load(SeqCst) != want {
        if ticks().saturating_sub(t0) > ms * TICK_HZ / 1000 {
            return false;
        }
    }
    true
}

/// Start core 1 on `mode`, run `f` on core 0 meanwhile, stop core 1 and return what it did: (MB/s in hundredths of its own cycles, passes).
fn with_core1(mode: u32, cpu_mhz: u64, f: impl FnOnce()) -> Option<(u64, u32)> {
    MODE.store(mode, SeqCst);
    STOP.store(0, SeqCst);
    STATE.store(0, SeqCst);
    GO.store(1, SeqCst);
    if !wait_state(1, 100) {
        return None;
    }
    f();
    STOP.store(1, SeqCst);
    if !wait_state(2, 2000) {
        return None;
    }
    let (passes, cycles) = (PASSES.load(SeqCst), CYCLES.load(SeqCst));
    // bytes per cycle times MHz = MB/s
    let rate = (passes as u64 * REGION as u64 * cpu_mhz * 100) / cycles.max(1) as u64;
    Some((rate, passes))
}

fn start_core1() -> bool {
    // SAFETY: core 1 is in its ROM state (nothing started it yet in this image); `_start1` sets up its own stacks and needs no bss init.
    let polls = unsafe { core1::start(_start1 as *const () as usize, 1_000_000) };
    let ok = polls.is_some();
    check(
        "648.core1",
        if ok { Verdict::Info } else { Verdict::Fail },
        format_args!("core 1 start: {polls:?} polls, running {}", core1::running()),
    );
    ok
}

fn fail(what: &str) -> bool {
    check("648.core1", Verdict::Fail, format_args!("{what}: core 1 did not answer"));
    false
}

pub fn run(cpu_mhz: u32) -> bool {
    crate::con::begin("dual");
    if !start_core1() {
        return false;
    }
    let (read, rd_bytes) = (cached(PSRAM_OFFSETS.read), REGION as u64);
    let mhz = cpu_mhz as u64;
    wb_inv(cached(PSRAM_OFFSETS.read), 3 * REGION);
    wb_inv(cached(PSRAM_OFFSETS.write), REGION);
    // core 0 alone, with core 1 spinning on its flag
    let t0 = ticks();
    core::hint::black_box(read_words(read, REGION / 4));
    let solo = mb_s_x100(rd_bytes, ticks() - t0, TICK_HZ);
    for (metric, mode) in [("lab.dual.read_read", READ), ("lab.dual.read_write", WRITE)] {
        let mut c0 = 0;
        let Some((c1, passes)) = with_core1(mode, mhz, || {
            let t0 = ticks();
            core::hint::black_box(read_words(read, REGION / 4));
            c0 = mb_s_x100(rd_bytes, ticks() - t0, TICK_HZ);
        }) else {
            return fail(metric);
        };
        perf(
            metric,
            &[
                ("c0_mb_s", Val::F2(c0 as i64)),
                ("c1_mb_s", Val::F2(c1 as i64)),
                ("solo_mb_s", Val::F2(solo as i64)),
                ("c1_passes", Val::U(passes as u64)),
            ],
        );
    }
    // internal RAM on core 0 while core 1 streams PSRAM: the two should not disturb each other
    let a = buf_a();
    let mut c0 = 0;
    if with_core1(READ, mhz, || {
        let t0 = ticks();
        for _ in 0..(REGION / BUF) {
            core::hint::black_box(read_words(a, BUF / 4));
        }
        c0 = mb_s_x100(rd_bytes, ticks() - t0, TICK_HZ);
    })
    .is_none()
    {
        return fail("lab.dual.sram_vs_psram");
    }
    perf("lab.dual.sram_vs_psram", &[("c0_sram_mb_s", Val::F2(c0 as i64))]);
    // latency under load: the 8 MB ring of the chase test while core 1 streams
    let (solo_ns, mut loaded_ns) = (chase_ring(8 << 20, 128), 0);
    if with_core1(READ, mhz, || loaded_ns = chase_ring(8 << 20, 128)).is_none() {
        return fail("lab.dual.chase_loaded");
    }
    perf("lab.dual.chase_loaded", &[("ns", Val::F2(loaded_ns as i64)), ("solo_ns", Val::F2(solo_ns as i64))]);
    true
}
