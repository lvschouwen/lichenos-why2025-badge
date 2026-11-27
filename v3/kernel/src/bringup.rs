//! The ESP32-P4 bring-up run (#611, #612 checkpoint 1): banner with the build id, a CHECK line per fact the bench
//! wants, END. Every step that touches hardware is announced with `BEGIN <id>` and drained first: a hang then names
//! the step.
use crate::mtrap;
use crate::platform::{self, hw};
use crate::{kcheck, kprintln};
use lichen_kcore::check::{Tally, Verdict};
use lichen_kcore::l2;

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
/// U spin length for the preemption case: several 100 us ticks at any plausible ROM core clock.
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

fn begin(id: &str) {
    kprintln!("BEGIN {id}");
    platform::flush();
}

fn record(t: &mut Tally, id: &str, ok: Option<bool>, detail: core::fmt::Arguments) {
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
        "lichen-v3 {BUILD_ID} platform {} abi {} (#612 traps + tick, RAM only)",
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

    timer(&mut t);
    fault(&mut t);
    user(&mut t);
    crate::bringup_u::run_rows(&mut t);
    crate::bringup_pmp::run(&mut t);

    let dropped = platform::take_dropped();
    record(&mut t, "log.ring", Some(dropped == 0), format_args!("dropped={dropped}"));

    let _ = lichen_kcore::check::write_end(&mut crate::console::Console, t.end_reason());
    platform::park()
}

/// 10k CLINT ticks through the CLIC and the real trap vector, taken from the kernel's wait loop.
fn timer(t: &mut Tally) {
    begin("timer");
    let mtvec = hw::trap::mtvec();
    record(t, "trap.mtvec", Some(mtvec & 3 == 3), format_args!("mtvec={mtvec:#010x} (mode 3 = CLIC)"));
    let before = hw::clic::line(lichen_kcore::trap::IRQ_MTIMER as usize);
    let period = lichen_kcore::trap::us_to_ticks(TICK_US);
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
            lichen_kcore::trap::ticks_to_us(elapsed)
        ),
    );
    let tol = lichen_kcore::trap::us_to_ticks(TICK_TOL_US);
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
    mtrap::tick_start(lichen_kcore::trap::us_to_ticks(100), u32::MAX);
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
