//! The ESP32-P4 bring-up run (#611, #612 checkpoint 1): banner with the build id, a CHECK line per fact the bench
//! wants, END. Every step that touches hardware is announced with `BEGIN <id>` and drained first: a hang then names
//! the step.
use crate::mtrap;
use crate::platform::{self, hw};
use crate::{kcheck, kprintln};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::l2;
use lichen_kcore::perf::Us;

pub const BUILD_ID: &str = env!("LICHEN_BUILD_ID");

extern "C" {
    static _load_start: u8;
    static _load_end: u8;
    static _noload_start: u8;
    static _noload_end: u8;
    fn planted_fault();
    static _utext: u8;
    static _udata: u8;
    static _sbss: u8;
    fn u_echo();
    fn u_load();
    fn u_store();
    fn u_spin_illegal();
    fn u_illegal_at();
}

const UPAGE: usize = 4096;
/// U spin length for the preemption case: several 100 us ticks at any plausible core clock (40 to 400 MHz).
const U_SPIN: usize = 4_000_000;

/// The core 0 tick under test: 1 ms, 10 000 of them (#612 checkpoint 1).
const TICK_US: u64 = 1000;
const TICKS: u32 = 10_000;
/// A tick counts as on time within 20 us of its period (handler entry latency; nothing prints while it runs).
const TICK_TOL_US: u64 = 20;

fn addr(s: &u8) -> usize {
    s as *const u8 as usize
}

fn hartid() -> usize {
    let v: usize;
    // SAFETY: mhartid is a read-only CSR with no side effects.
    unsafe { core::arch::asm!("csrr {0}, mhartid", out(reg) v, options(nomem, nostack)) };
    v
}

pub fn begin(id: &str) {
    kprintln!("BEGIN {id}");
    platform::flush();
}

pub fn record(t: &mut Tally, id: &str, ok: Option<bool>, detail: core::fmt::Arguments) {
    let v = match ok {
        Some(true) => Verdict::Pass,
        Some(false) => Verdict::Fail,
        None => Verdict::Info,
    };
    t.record(v);
    kcheck!(id, v, "{detail}");
}

pub fn run(hart: usize, boot_info: *const u8) -> ! {
    platform::init();
    hw::uart::spin(2_000_000); // esptool is still reading the MEM_END reply
    kprintln!("");
    kprintln!(
        "lichen-v3 {BUILD_ID} platform {} abi {} (#612 traps + tick, #613 isolation, RAM only)",
        platform::NAME,
        lichen_abi::ABI_VERSION
    );
    let mut t = Tally::new();

    record(&mut t, "boot.hart", Some(hartid() == 0 && hart == 0), format_args!("mhartid={} a0={hart}", hartid()));
    record(
        &mut t,
        "boot.info",
        None,
        format_args!(
            "a1={boot_info:p} ({})",
            if lichen_kcore::trap::boot_info_in_range(
                boot_info as usize,
                16,
                hw::mem::L2MEM_BASE,
                lichen_kcore::l2::L2MEM_END
            ) {
                "in L2MEM, magic not checked yet (#615)"
            } else {
                "not a boot-info pointer"
            }
        ),
    );

    // SAFETY: linker-defined symbols; only their addresses are used.
    let (ls, le, ns, ne) = unsafe { (addr(&_load_start), addr(&_load_end), addr(&_noload_start), addr(&_noload_end)) };
    let fits = ls >= hw::mem::LOAD_BASE
        && le <= hw::mem::LOAD_LIMIT
        && ns >= hw::mem::NOLOAD_BASE
        && ne <= hw::mem::NOLOAD_LIMIT;
    record(
        &mut t,
        "image.layout",
        Some(fits),
        format_args!("loaded {ls:#x}..{le:#x} ({} B), noload {ns:#x}..{ne:#x} ({} B)", le - ls, ne - ns),
    );

    begin("c6");
    hw::c6::release();
    record(
        &mut t,
        "c6.strap",
        Some(hw::c6::strap_level()),
        format_args!("gpio13 level={}", hw::c6::strap_level() as u8),
    );
    record(&mut t, "c6.en", Some(hw::c6::en_level()), format_args!("gpio12 level={}", hw::c6::en_level() as u8));

    begin("l2");
    let reg = hw::cache::l2_cachesize_reg();
    match l2::cache_size_bytes(reg) {
        Some(size) => {
            record(
                &mut t,
                "l2.cache",
                None,
                format_args!("size={} KB reg={reg:#x} carve={:#x}", size / 1024, l2::carve_start(size)),
            );
            record(
                &mut t,
                "l2.fit",
                Some(ne <= l2::carve_start(size)),
                format_args!("noload end {ne:#x} vs carve {:#x}", l2::carve_start(size)),
            );
        }
        None => record(&mut t, "l2.cache", Some(false), format_args!("unreadable size reg={reg:#x}")),
    }

    mtime_rate(&mut t);
    if !clock(&mut t) {
        // Every rate the kernel holds is unknown: no row after this could be judged.
        finish(t)
    }
    timer(&mut t);
    fault(&mut t);
    user(&mut t);
    crate::bringup_u::run_rows(&mut t);
    crate::bringup_pmp::run(&mut t);
    crate::bringup_core1::run(&mut t);
    // Core-local U rows go last: a hang there must not hide the rows before them.
    crate::bringup_u::core_local_rows(&mut t);
    crate::bringup_psram::run(&mut t);
    // The isolation harness (#613) takes the hart over for good (its own trap vector) and ends the run in `finish`.
    crate::bringup_iso::run(t)
}

/// The last rows and END. The harness calls this from trap context, once every case is judged.
pub fn finish(mut t: Tally) -> ! {
    let dropped = platform::take_dropped();
    record(&mut t, "log.ring", Some(dropped == 0), format_args!("dropped={dropped}"));

    let _ = lichen_kcore::check::write_end(&mut crate::console::Console, t.end_reason());
    platform::park()
}

/// The mtime rate against SYSTIMER (#654), before anything converts microseconds to ticks. PASS only if it matches
/// XTAL or the CPU clock, the latter measured independently (mcycle against SYSTIMER), so the row also answers #654's
/// hypothesis: `source=Cpu` means mtime follows the CPU clock. At the ROM clock the CPU is on XTAL and the row can
/// only say `XtalOrCpu`.
fn mtime_rate(t: &mut Tally) {
    begin("timebase");
    let m = crate::timebase::boot();
    let cpu_mhz = hw::clock::measure_mhz();
    let src_sel = hw::psram::systimer::src_sel();
    let detail = format_args!(
        "{} mtime ticks over {} SYSTIMER ticks (16 MHz, src_sel {src_sel} = {}); CPU {cpu_mhz:?} MHz by mcycle vs SYSTIMER",
        m.mtime_ticks,
        m.systimer_ticks,
        if src_sel == 0 { "XTAL" } else { "RC_FAST, not 16 MHz" }
    );
    match m.hz {
        Some(hz) => {
            let cpu_hz = cpu_mhz.unwrap_or(0) as u64 * 1_000_000;
            let source = lichen_kcore::timebase::mtime_source(hz, cpu_hz);
            let ok = source.is_some() && src_sel == 0;
            record(t, "timebase.mtime", Some(ok), format_args!("mtime_hz={hz} source={source:?}: {detail}"));
        }
        None => record(
            t,
            "timebase.mtime",
            Some(false),
            format_args!("not measured, timing rows use {} Hz unverified: {detail}", crate::timebase::hz()),
        ),
    }
}

/// The periodic tick armed across the clock switch (#655): a period longer than the switch takes (it prints every
/// register write, about 0.54 s at 115200 baud on the bench), so the first deadline falls after it, and three of them
/// so the rescaled period is judged too (left at 40 MHz ticks, the second would come after 1.11 s).
const REARM_US: u64 = 1_000_000;
const REARM_TICKS: u32 = 3;
/// How far each tick may fire from its deadline, judged by SYSTIMER: 0.1 % of the longest interval (the remeasure is
/// good to about 250 ppm, #654) plus the wake-up.
const REARM_TOL_US: u64 = REARM_US * REARM_TICKS as u64 / 1000;
/// The production clock (#645 owner decision: CPU 360, PSRAM 120 on rev 1.0).
const CPU_MHZ: u32 = 360;
const CPU_TOL_PCT: u32 = 1;

fn within_pct(a: u64, b: u64, pct: u64) -> bool {
    b != 0 && a.abs_diff(b) * 100 <= b * pct
}

/// The CPU to 360 MHz through `timebase::switch` (#655), with a tick armed before it at the boot rate that must
/// fire on time after it. False when the switch or the remeasure failed: the kernel cannot judge time then.
fn clock(t: &mut Tally) -> bool {
    begin("clock");
    let sys0 = hw::psram::systimer::now_ticks().unwrap_or(0);
    let us0 = crate::timebase::now_us();
    mtrap::tick_start(crate::timebase::us_to_ticks(REARM_US), REARM_TICKS);
    let sw = crate::timebase::switch(|| {
        let mut log = |a: core::fmt::Arguments| {
            kprintln!("{a}");
            platform::flush();
        };
        // SAFETY: core 0 with nothing else running (`switch` halts otherwise), a RAM-only image: nothing runs from
        // flash or PSRAM while the clock tree and the core supply change.
        unsafe { hw::clock::cpu_to_360(&mut log) }
    });
    let mhz = match sw.out {
        Ok((_, after)) => after,
        Err(_) => None,
    };
    let ok_switch = sw.out.is_ok() && sw.m.hz.is_some();
    let cpu_ok = mhz.is_some_and(|m| within_pct(m as u64, CPU_MHZ as u64, CPU_TOL_PCT as u64));
    let hz = sw.m.hz.unwrap_or(0);
    let source = lichen_kcore::timebase::mtime_source(hz, mhz.unwrap_or(0) as u64 * 1_000_000);
    record(
        t,
        "clock.cpu",
        Some(ok_switch && cpu_ok && source.is_some()),
        format_args!(
            "{:?}: CPU {mhz:?} MHz by mcycle vs SYSTIMER (want {CPU_MHZ} +- {CPU_TOL_PCT} %), mtime {hz} Hz source={source:?}, \
             generation {} -> {}, switch {} us by SYSTIMER",
            sw.out.map(|_| ()),
            sw.old.gen,
            sw.new.gen,
            sw.sys_ticks / 16
        ),
    );
    if !ok_switch {
        mtrap::tick_stop();
        return false;
    }
    kprintln!("PERF cpu.clock mhz={}", mhz.unwrap_or(0));
    rearm_row(t, sys0, us0, sw.sys_ticks / 16);
    true
}

/// timer.rearm: the tick armed before the switch fires at 1, 2 and 3 periods by SYSTIMER (a deadline that the switch
/// outlasted fires once, at once), misses none, and the monotonic clock moved by as much as SYSTIMER.
fn rearm_row(t: &mut Tally, sys0: u64, us0: u64, switch_us: u64) {
    use hw::psram::systimer;
    begin("timer.rearm");
    let limit = sys0 + 2 * REARM_US * REARM_TICKS as u64 * systimer::TICKS_PER_US;
    let mut at = [0u64; REARM_TICKS as usize];
    let mut seen = 0;
    while seen < REARM_TICKS && systimer::now_ticks().is_some_and(|s| s < limit) {
        mtrap::wait_irq();
        let n = mtrap::tick_count().min(REARM_TICKS);
        let now = systimer::now_ticks().unwrap_or(0);
        while seen < n {
            at[seen as usize] = systimer::elapsed(sys0, now) / systimer::TICKS_PER_US;
            seen += 1;
        }
    }
    let us1 = crate::timebase::now_us();
    let (count, missed) = mtrap::tick_stop().map_or((0, 0), |k| (k.stats.count, k.stats.missed));
    // Deadline k is k periods after arming, or at once after the switch if the switch outlasted it.
    let err = |k: usize| at[k].abs_diff(((k as u64 + 1) * REARM_US).max(switch_us));
    let worst = (0..REARM_TICKS as usize).map(err).max().unwrap_or(u64::MAX);
    let got = at[REARM_TICKS as usize - 1];
    let mono = us1.saturating_sub(us0);
    let ok = count == REARM_TICKS
        && missed == 0
        && worst <= REARM_TOL_US
        && us1 >= us0
        && mono.abs_diff(got) <= REARM_TOL_US;
    record(
        t,
        "timer.rearm",
        Some(ok),
        format_args!(
            "{REARM_US} us tick armed at the boot rate: {count} of {REARM_TICKS} fired at {at:?} us by SYSTIMER \
             (want each period +- {REARM_TOL_US}, worst error {worst} us), missed {missed}, monotonic clock \
             {us0} -> {us1} us ({mono} us)"
        ),
    );
    kprintln!("PERF kernel.timer_rearm err_us={worst} switch_us={switch_us}");
}

/// 10k CLINT ticks through the CLIC and the real trap vector, taken from the kernel's wait loop.
fn timer(t: &mut Tally) {
    begin("timer");
    let mtvec = hw::trap::mtvec();
    record(t, "trap.mtvec", Some(mtvec & 3 == 3), format_args!("mtvec={mtvec:#010x} (mode 3 = CLIC)"));
    let before = hw::clic::line(lichen_kcore::trap::IRQ_MTIMER as usize);
    let gen = crate::timebase::gen();
    let period = crate::timebase::us_to_ticks(TICK_US);
    kprintln!("timer: mtimectl={:#x} line 7 before ip/ie/attr/ctl {before:?}", hw::clint::ctl());
    platform::flush();
    // Nothing prints while the tick runs: the UART pump runs with MIE clear and would delay ticks.
    let thresh = mtrap::tick_start(period, TICKS);
    let armed = hw::clic::line(lichen_kcore::trap::IRQ_MTIMER as usize);
    let start = hw::clint::mtime();
    let deadline = start + period * (TICKS as u64 + 1000);
    while mtrap::tick_count() < TICKS && hw::clint::mtime() < deadline {
        mtrap::wait_irq();
    }
    let elapsed = hw::clint::mtime() - start;
    let Some(tick) = mtrap::tick_stop() else {
        record(t, "timer.ticks", Some(false), format_args!("tick state vanished"));
        return;
    };
    let s = tick.stats;
    kprintln!("timer: line 7 armed {armed:?}, threshold read back {thresh:#x}");
    record(
        t,
        "timer.ticks",
        Some(s.count == TICKS),
        format_args!(
            "{} of {TICKS} ticks in {} us (period {TICK_US} us)",
            s.count,
            crate::timebase::ticks_to_us(elapsed)
        ),
    );
    let tol = crate::timebase::us_to_ticks(TICK_TOL_US);
    record(
        t,
        "timer.period",
        Some(s.stable(period, tol)),
        format_args!(
            "gap min/mean/max {}/{}/{} ticks (want {period} +- {tol}), latency min/max {}/{} ticks, missed {}",
            s.gap_min,
            s.gap_mean().unwrap_or(0),
            s.gap_max,
            s.lat_min,
            s.lat_max,
            s.missed
        ),
    );
    let hz = crate::timebase::hz_in(gen);
    let jitter = lichen_kcore::perf::jitter_max(period, s.gap_min, s.gap_max);
    kprintln!(
        "perf: kernel.timer_jitter_core0 = core 0's tick, max |gap - {TICK_US} us|; core 0 in wfi, nothing printing"
    );
    kprintln!(
        "PERF kernel.timer_jitter_core0 max_us={} p2p_us={} n={} period_us={TICK_US}",
        Us { ticks: jitter, hz },
        Us { ticks: s.gap_max.saturating_sub(s.gap_min), hz },
        s.count
    );
    let stray = mtrap::stray_irqs();
    record(t, "timer.stray", Some(stray == 0), format_args!("{stray} interrupt(s) other than the tick"));
}

/// An M-mode illegal instruction goes through the trap vector and the fault dump (mcause, mepc, mtval, ICM record).
fn fault(t: &mut Tally) {
    begin("mfault");
    let pc = planted_fault as *const () as usize as u32;
    mtrap::expect_fault(pc);
    // SAFETY: planted_fault is one illegal word and a ret; the armed handler skips the word, so the call returns.
    unsafe { planted_fault() };
    match mtrap::take_caught() {
        Some(c) => {
            let d = lichen_kcore::trap::decode(c.cause);
            record(
                t,
                "trap.mfault",
                Some(!d.interrupt && d.code == lichen_kcore::trap::EXC_ILLEGAL && d.mpp == 3 && c.epc == pc),
                format_args!("mcause={:#010x} mepc={:#010x} (want {pc:#010x}) mtval={:#010x}", c.cause, c.epc, c.tval),
            );
        }
        None => record(t, "trap.mfault", Some(false), format_args!("the planted fault never trapped")),
    }
}

fn fnaddr(f: unsafe extern "C" fn()) -> usize {
    f as *const () as usize
}

fn fault_line(u: &mtrap::URun) -> (u16, u32, u32) {
    u.fault.map_or((0xFFF, 0, 0), |f| (lichen_kcore::trap::decode(f.cause).code, f.epc, f.tval))
}

/// U-mode under PMP (#612 checkpoint 1 U cases, checkpoint 2 first rows): the stub page R+X, the data page R+W,
/// everything else denied by an explicit deny-all entry.
fn user(t: &mut Tally) {
    begin("pmp");
    crate::pmp::report();
    // SAFETY: linker symbols; only their addresses are used.
    let (utext, udata, kram) = unsafe { (addr(&_utext), addr(&_udata), addr(&_sbss)) };
    // The U data page is NOLOAD: whatever the ROM or an earlier stage left there is not U's to read. The text page is
    // part of the loaded image (padded with zeros by the linker), so its tail needs no clearing.
    // SAFETY: the page is kernel-owned NOLOAD memory that no one else uses yet.
    unsafe { core::ptr::write_bytes(udata as *mut u8, 0, UPAGE) };
    crate::pmp::apply(utext, udata, UPAGE);
    kprintln!("pmp: U view text {utext:#x} R+X, data {udata:#x} R+W ({UPAGE} B each), rest denied");
    let sp = udata + UPAGE;

    begin("u.echo");
    let u = mtrap::run_u(fnaddr(u_echo), sp, 0x1234, 0);
    record(
        t,
        "u.ecall",
        Some(u.echoes == 1 && u.exit == Some(0x1235) && u.fault.is_none()),
        format_args!(
            "U ecall round trip: {} echo(es), exit a0={:x?}, mintstatus {:#x}",
            u.echoes,
            u.exit,
            hw::trap::mintstatus()
        ),
    );

    begin("u.grant");
    let st = mtrap::run_u(fnaddr(u_store), sp, udata, 0xA5A5_0612);
    let ld = mtrap::run_u(fnaddr(u_load), sp, udata, 0);
    // SAFETY: the U data page is NOLOAD kernel-owned memory the kernel may read.
    let m_view = unsafe { core::ptr::read_volatile(udata as *const u32) };
    record(
        t,
        "u.grant",
        Some(st.exit.is_some() && ld.exit == Some(0xA5A5_0612) && m_view == 0xA5A5_0612),
        format_args!(
            "U store+load of its data page: store {:?}, load {:x?}, M reads {m_view:#x}",
            st.exit.is_some(),
            ld.exit
        ),
    );

    // Each must be refused by the PMP: load-access-fault (5) with mtval = the address.
    let denied: [(&str, usize); 6] = [
        ("u.deny.kram", kram),
        ("u.deny.peri", 0x500C_A01C),
        ("u.deny.lpram", 0x5010_8000),
        ("u.deny.uncached", udata + 0x4000_0000),
        ("u.deny.cache-mon", 0x3FF0_0000),
        ("u.deny.cache-ctl", 0x3FF1_0000),
    ];
    for (id, a) in denied {
        begin(id);
        let u = mtrap::run_u(fnaddr(u_load), sp, a, 0);
        let (code, epc, tval) = fault_line(&u);
        record(
            t,
            id,
            Some(code == 5 && tval as usize == a && epc as usize == fnaddr(u_load) && u.exit.is_none()),
            format_args!(
                "U load {a:#010x}: code {code} ({}) mepc {epc:#x} mtval {tval:#x}",
                lichen_kcore::trap::exception_name(code)
            ),
        );
    }
    begin("u.deny.kram-store");
    let u = mtrap::run_u(fnaddr(u_store), sp, kram, 0xBAD);
    let (code, _, tval) = fault_line(&u);
    record(
        t,
        "u.deny.kram-store",
        Some(code == 7 && tval as usize == kram),
        format_args!(
            "U store {kram:#010x}: code {code} ({}) mtval {tval:#x}",
            lichen_kcore::trap::exception_name(code)
        ),
    );

    begin("u.preempt");
    mtrap::tick_start(crate::timebase::us_to_ticks(100), u32::MAX);
    let u = mtrap::run_u(fnaddr(u_spin_illegal), sp, U_SPIN, 0);
    let tick = mtrap::tick_stop();
    let (code, epc, _) = fault_line(&u);
    let want = fnaddr(u_illegal_at);
    record(
        t,
        "u.preempt-illegal",
        Some(code == lichen_kcore::trap::EXC_ILLEGAL && epc as usize == want && u.preempts >= 3),
        format_args!(
            "U spin {U_SPIN} then illegal: {} preemption(s), {} tick(s), fault code {code} at {epc:#x} (want {want:#x})",
            u.preempts,
            tick.map_or(0, |k| k.stats.count)
        ),
    );
}
