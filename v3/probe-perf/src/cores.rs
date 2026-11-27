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
/// A trap on core 1: set by its trap handler (which only parks), reported by core 0.
static C1_TRAPPED: AtomicU32 = AtomicU32::new(0);
static C1_CAUSE: AtomicU32 = AtomicU32::new(0);
static C1_EPC: AtomicU32 = AtomicU32::new(0);

/// Called from core 1's trap handler.
pub fn core1_trapped(mcause: u32, mepc: u32) {
    C1_CAUSE.store(mcause, SeqCst);
    C1_EPC.store(mepc, SeqCst);
    C1_TRAPPED.store(1, SeqCst);
}

/// Core 0, in `finish`: say what core 1 trapped on, once.
pub fn report_core1_trap() {
    if C1_TRAPPED.swap(2, SeqCst) == 1 {
        check(
            "648.core1.trap",
            Verdict::Fail,
            format_args!("core 1 trapped: mcause={:#x} mepc={:#x}", C1_CAUSE.load(SeqCst), C1_EPC.load(SeqCst)),
        );
    }
}

const REGION: usize = 4 << 20;
/// Core 1 looks at STOP after every chunk, so it never runs on long after core 0 is done and its figure covers the same window.
const CHUNK: usize = 256 << 10;
/// Core 0's window: several passes, so the two cores overlap for far longer than the start-up and the last chunk.
const C0_PASSES: usize = 3;

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
        let (mut chunks, mut last) = (0u32, start);
        while STOP.load(SeqCst) == 0 {
            let off = (chunks as usize % (REGION / CHUNK)) * CHUNK;
            match mode {
                READ => {
                    core::hint::black_box(read_words(cached(PSRAM_OFFSETS.write + off), CHUNK / 4));
                }
                WRITE => write_words(cached(PSRAM_OFFSETS.copy_dst + off), CHUNK / 4, chunks),
                _ => core::hint::spin_loop(),
            }
            chunks += 1;
            last = mcycle();
        }
        PASSES.store(chunks, SeqCst);
        CYCLES.store(last.wrapping_sub(start), SeqCst);
        STATE.store(2, SeqCst);
    }
}

fn wait_state(want: u32, ms: u64) -> bool {
    let t0 = ticks();
    while STATE.load(SeqCst) != want {
        if C1_TRAPPED.load(SeqCst) != 0 {
            return false;
        }
        if ticks().saturating_sub(t0) > ms * TICK_HZ / 1000 {
            return false;
        }
    }
    true
}

/// Start core 1 on `mode`, run `f` on core 0 meanwhile, stop core 1 and return what it did: (MB/s in hundredths of its own cycles,
/// chunks done).
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
    let (chunks, cycles) = (PASSES.load(SeqCst), CYCLES.load(SeqCst));
    // bytes per cycle times MHz = MB/s
    let rate = (chunks as u64 * CHUNK as u64 * cpu_mhz * 100) / cycles.max(1) as u64;
    Some((rate, chunks))
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

/// Core 0's read rate over `C0_PASSES` passes of the 4 MB read region.
fn c0_read_rate() -> u64 {
    let read = cached(PSRAM_OFFSETS.read);
    let t0 = ticks();
    for _ in 0..C0_PASSES {
        core::hint::black_box(read_words(read, REGION / 4));
    }
    mb_s_x100((C0_PASSES * REGION) as u64, ticks() - t0, TICK_HZ)
}

/// Core 0 reads while core 1 reads or writes another region. The two rates added are what the memory system delivers to both cores;
/// against the solo rate they say whether a second core adds throughput or only shares it.
fn pair(metric: &str, mode: u32, mhz: u64, solo: u64) -> bool {
    let mut c0 = 0;
    let Some((c1, chunks)) = with_core1(mode, mhz, || c0 = c0_read_rate()) else { return fail(metric) };
    perf(
        metric,
        &[
            ("c0_mb_s", Val::F2(c0 as i64)),
            ("c1_mb_s", Val::F2(c1 as i64)),
            ("solo_mb_s", Val::F2(solo as i64)),
            ("sum_over_solo_pct", Val::F2(((c0 + c1) * 10_000 / solo.max(1)) as i64)),
            ("c1_chunks", Val::U(chunks as u64)),
        ],
    );
    true
}

/// Internal RAM on core 0 while core 1 streams PSRAM. L1 data cache is probably shared by the two cores (the ROM names two L1 I caches
/// and one L1 D cache), in which case core 1's stream evicts core 0's buffer and this rate falls to the L2MEM refill rate.
fn sram_vs_psram(mhz: u64) -> bool {
    let (a, mut c0) = (buf_a(), 0);
    let solo_t0 = ticks();
    for _ in 0..(REGION / BUF) {
        core::hint::black_box(read_words(a, BUF / 4));
    }
    let solo = mb_s_x100(REGION as u64, ticks() - solo_t0, TICK_HZ);
    let ok = with_core1(READ, mhz, || {
        let t0 = ticks();
        for _ in 0..(C0_PASSES * REGION / BUF) {
            core::hint::black_box(read_words(a, BUF / 4));
        }
        c0 = mb_s_x100((C0_PASSES * REGION) as u64, ticks() - t0, TICK_HZ);
    })
    .is_some();
    perf("lab.dual.sram_vs_psram", &[("c0_sram_mb_s", Val::F2(c0 as i64)), ("solo_sram_mb_s", Val::F2(solo as i64))]);
    ok || fail("lab.dual.sram_vs_psram")
}

pub fn run(cpu_mhz: u32) -> bool {
    crate::con::begin("dual");
    if !start_core1() {
        return false;
    }
    // core 1 counts its own cycles: convert with the measured clock (SYSTIMER against mcycle), not the nominal one
    let mhz = lichen_platform_esp32p4::clock::measure_mhz().unwrap_or(cpu_mhz) as u64;
    wb_inv(cached(PSRAM_OFFSETS.read), 3 * REGION);
    // core 0 alone, with core 1 spinning on its flag
    let solo = c0_read_rate();
    if !(pair("lab.dual.read_read", READ, mhz, solo) && pair("lab.dual.read_write", WRITE, mhz, solo)) {
        return false;
    }
    if !sram_vs_psram(mhz) {
        return false;
    }
    // latency under load: the 8 MB ring of the chase test while core 1 streams
    let (solo_ns, mut loaded_ns) = (chase_ring(8 << 20, 128), 0);
    if with_core1(READ, mhz, || loaded_ns = chase_ring(8 << 20, 128)).is_none() {
        return fail("lab.dual.chase_loaded");
    }
    perf("lab.dual.chase_loaded", &[("ns", Val::F2(loaded_ns as i64)), ("solo_ns", Val::F2(solo_ns as i64))]);
    true
}
