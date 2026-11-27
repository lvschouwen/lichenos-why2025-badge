//! ESP32-P4 M-mode trap handling and the core 0 tick (#612 checkpoint 1, increment 1).
//!
//! The kernel takes M-mode interrupts only inside `wait_irq`'s window (MIE set for one wfi), so no `&mut` to kernel
//! state is ever live when the handler runs, and the handler cannot nest (the CPU clears MIE on entry; a fault inside
//! the handler goes to `nested_fault`, see boot_esp32p4.S).
//!
//! A synchronous exception can come from anywhere, also from code holding the console ring, so the fatal paths switch
//! the console to its raw ring-free mode (`platform::enter_fatal`) before they print. Exceptions are fatal (dump,
//! END fatal), except one the bring-up arms on purpose with `expect_fault`: that one is
//! dumped through the same path, recorded, and skipped, so the bench sees a real dump and the run still ends cleanly.
use crate::global::Global;
use crate::platform::{self, hw};
use crate::{kcheck, kprintln};
use lichen_kcore::check::Verdict;
use lichen_kcore::trap::{self, Cause, PeriodStats};

/// The caller-saved registers trap_entry stores, in its order, then the interrupted sp.
#[repr(C)]
pub struct Frame {
    pub ra: usize,
    pub t: [usize; 3],
    pub a: [usize; 8],
    pub t3_6: [usize; 4],
    pub sp: usize,
}
const _: () = assert!(core::mem::size_of::<Frame>() == 68);

pub struct Tick {
    pub period: u64,
    pub cmp: u64,
    pub target: u32,
    pub stats: PeriodStats,
}

#[derive(Clone, Copy)]
pub struct Caught {
    pub cause: u32,
    pub epc: u32,
    pub tval: u32,
}

/// What one U run did (`run_u`).
#[derive(Clone, Copy, Default)]
pub struct URun {
    pub echoes: u32,
    pub preempts: u32,
    /// a0 of the exit ecall, if the run ended with one.
    pub exit: Option<usize>,
    /// The exception that ended the run otherwise.
    pub fault: Option<Caught>,
}

struct State {
    tick: Option<Tick>,
    /// Some while a U run is in progress.
    user: Option<URun>,
    /// Set by `expect_fault`: the next exception at this pc is recorded and skipped instead of ending the run.
    expect_pc: Option<u32>,
    caught: Option<Caught>,
    stray: u32,
}

/// Touched by the handler and by kernel code with MIE clear only (see the module invariant).
static STATE: Global<State> = Global::new(State { tick: None, user: None, expect_pc: None, caught: None, stray: 0 });

fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    hw::trap::irq_disable();
    // SAFETY: MIE is clear here and the handler is not running (it never calls `with`), so this is the only &mut.
    f(unsafe { STATE.get() })
}

/// Configure CLINT + CLIC line 7 for a periodic tick and arm the first deadline. Returns the threshold read back.
pub fn tick_start(period: u64, target: u32) -> u32 {
    hw::clint::disarm();
    hw::clic::configure(trap::IRQ_MTIMER as usize, hw::clic::ATTR_M_LEVEL, hw::clic::CTL_MAX);
    let thresh = hw::clic::set_threshold(0);
    let cmp = hw::clint::mtime() + period;
    with(|s| s.tick = Some(Tick { period, cmp, target, stats: PeriodStats::new() }));
    hw::clint::set_cmp(cmp);
    hw::clic::set_enabled(trap::IRQ_MTIMER as usize, true);
    thresh
}

pub fn tick_stop() -> Option<Tick> {
    hw::clic::set_enabled(trap::IRQ_MTIMER as usize, false);
    hw::clint::disarm();
    with(|s| s.tick.take())
}

pub fn tick_count() -> u32 {
    with(|s| s.tick.as_ref().map_or(0, |t| t.stats.count))
}

/// Sleep until an interrupt is pending, then take it in a window with MIE set. wfi wakes on a pending enabled CLIC
/// line with MIE clear (bench, increment 1), so the interrupt is never taken mid-way through kernel code.
pub fn wait_irq() {
    hw::trap::irq_disable();
    hw::trap::wfi();
    // SAFETY: no reference into STATE (or any Global the handler uses) is live here; `with` borrows end before it.
    unsafe { hw::trap::irq_enable() };
    hw::trap::irq_disable();
}

pub fn stray_irqs() -> u32 {
    with(|s| s.stray)
}

/// The next exception at `pc` is dumped, recorded and skipped (pc += 4) instead of ending the run.
pub fn expect_fault(pc: u32) {
    with(|s| {
        s.expect_pc = Some(pc);
        s.caught = None;
    });
}
pub fn take_caught() -> Option<Caught> {
    with(|s| {
        s.expect_pc = None;
        s.caught.take()
    })
}

/// The fault dump: mcause decoded, mepc, mtval, mstatus, mintstatus and the AXI ICM address-hole record.
fn dump(tag: &str, mcause: u32, c: Cause) {
    let icm = hw::trap::icm_record();
    kprintln!(
        "{tag} mcause={mcause:#010x} ({} code {} mpp {} mpil {}) mepc={:#010x} mtval={:#010x} mstatus={:#010x} \
         mintstatus={:#010x}",
        if c.interrupt { "irq" } else { trap::exception_name(c.code) },
        c.code,
        c.mpp,
        c.mpil,
        hw::trap::mepc(),
        hw::trap::mtval(),
        hw::trap::mstatus(),
        hw::trap::mintstatus(),
    );
    kprintln!(
        "{tag} icm int_raw={:#010x} cpu_hole_addr={:#010x} cpu_hole_info={:#010x}",
        icm.int_raw,
        icm.cpu_hole_addr,
        icm.cpu_hole_info
    );
}

fn on_timer(s: &mut State) {
    let Some(t) = s.tick.as_mut() else {
        hw::clint::disarm();
        s.stray += 1;
        return;
    };
    let now = hw::clint::mtime();
    if now < t.cmp {
        // The line fired before its deadline: not a tick (spurious, or a stale level). Count it, keep the deadline.
        s.stray += 1;
        return;
    }
    t.stats.record(t.cmp, now);
    if t.stats.count >= t.target {
        hw::clint::disarm();
        return;
    }
    let (next, skipped) = trap::next_deadline(t.cmp, t.period, hw::clint::mtime());
    t.stats.add_missed(skipped);
    t.cmp = next;
    hw::clint::set_cmp(next);
}

extern "C" {
    fn run_user(entry: usize, sp: usize, a0: usize, a1: usize);
}

/// Watchdog tick period for U runs that have no tick of their own.
const WATCHDOG_US: u64 = 10_000;

/// Run U code at `entry` (on the U stub page, under the PMP view the caller applied) until it exits or faults.
pub fn run_u(entry: usize, sp: usize, a0: usize, a1: usize) -> URun {
    // U never runs without the fixed window entry (a U window load would hang, #646) ...
    if !crate::pmp::view_loaded() {
        platform::enter_fatal();
        kprintln!("KERNEL: run_u without a safe PMP view (#646): halting");
        platform::exit(4);
    }
    // ... nor without a tick: TW is hardwired 0, so a U wfi would park the hart. A run with no tick of its own gets a
    // watchdog tick that only preempts (nothing counts it).
    let watchdog = with(|s| s.tick.is_none());
    if watchdog {
        tick_start(trap::us_to_ticks(WATCHDOG_US), u32::MAX);
    }
    with(|s| s.user = Some(URun::default()));
    // SAFETY: run_user saves the kernel's callee-saved registers and returns here through to_kernel when m_trap ends
    // the run; no &mut into STATE is live across it (the `with` borrow above has ended).
    unsafe { run_user(entry, sp, a0, a1) };
    if watchdog {
        tick_stop();
    }
    with(|s| s.user.take().unwrap_or_default())
}

fn set_mepc(v: u32) {
    // SAFETY: mepc is only read by the coming mret; the callers pass the trapped pc + 4 (norvc, 4-byte instructions).
    unsafe { core::arch::asm!("csrw mepc, {0}", in(reg) v, options(nomem, nostack)) };
}

/// A trap from U: 0 = resume U, 1 = the run is over (to_kernel).
fn from_user(s: &mut State, frame: &mut Frame, mcause: u32, c: Cause) -> usize {
    let Some(u) = s.user.as_mut() else {
        platform::enter_fatal();
        dump("KERNEL FAULT (U trap with no U run)", mcause, c);
        platform::exit(3)
    };
    let epc = hw::trap::mepc();
    if c.code == trap::EXC_ECALL_U {
        match trap::classify_ucall(frame.a[7]) {
            trap::UCall::Echo => {
                u.echoes += 1;
                frame.a[0] = frame.a[0].wrapping_add(1);
            }
            trap::UCall::Unknown => frame.a[0] = trap::ENOSYS_RET,
            trap::UCall::Exit => {
                u.exit = Some(frame.a[0]);
                return 1;
            }
        }
        set_mepc(epc + 4);
        return 0;
    }
    u.fault = Some(Caught { cause: mcause, epc, tval: hw::trap::mtval() });
    1
}

#[no_mangle]
extern "C" fn m_trap(frame: *mut Frame) -> usize {
    let mcause = hw::trap::mcause();
    let c = trap::decode(mcause);
    // SAFETY: trap entry; the kernel holds no &mut into STATE while MIE can be set (module invariant), and a nested
    // trap never reaches here (it takes the nested_fault path).
    let s = unsafe { STATE.get() };
    if c.interrupt {
        if c.code == trap::IRQ_MTIMER {
            on_timer(s);
            if c.mpp == 0 {
                if let Some(u) = s.user.as_mut() {
                    u.preempts += 1;
                }
            }
        } else {
            hw::clic::set_enabled(c.code as usize, false);
            s.stray += 1;
        }
        return 0;
    }
    if c.mpp == 0 {
        // SAFETY: trap_entry passes its own frame on the trap stack; nothing else references it during m_trap.
        return from_user(s, unsafe { &mut *frame }, mcause, c);
    }
    let epc = hw::trap::mepc();
    if s.expect_pc == Some(epc) {
        dump("MFAULT", mcause, c);
        s.caught = Some(Caught { cause: mcause, epc, tval: hw::trap::mtval() });
        s.expect_pc = None;
        set_mepc(epc + 4);
        return 0;
    }
    platform::enter_fatal();
    dump("KERNEL FAULT", mcause, c);
    kcheck!("trap.fatal", Verdict::Fail, "unexpected M-mode exception, see the dump above");
    platform::exit(3)
}

/// A trap while a trap was being handled: mscratch was 0. Report on the fault stack and stop.
#[no_mangle]
extern "C" fn nested_fault() -> ! {
    let mcause = hw::trap::mcause();
    platform::enter_fatal();
    dump("NESTED FAULT", mcause, trap::decode(mcause));
    platform::exit(4)
}
