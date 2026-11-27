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

struct State {
    tick: Option<Tick>,
    /// Set by `expect_fault`: the next exception at this pc is recorded and skipped instead of ending the run.
    expect_pc: Option<u32>,
    caught: Option<Caught>,
    stray: u32,
}

/// Touched by the handler and by kernel code with MIE clear only (see the module invariant).
static STATE: Global<State> = Global::new(State { tick: None, expect_pc: None, caught: None, stray: 0 });

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

#[no_mangle]
extern "C" fn m_trap(frame: *mut Frame) {
    let _ = frame;
    let mcause = hw::trap::mcause();
    let c = trap::decode(mcause);
    // SAFETY: trap entry; the kernel holds no &mut into STATE while MIE can be set (module invariant), and a nested
    // trap never reaches here (it takes the nested_fault path).
    let s = unsafe { STATE.get() };
    if c.interrupt {
        if c.code == trap::IRQ_MTIMER {
            on_timer(s);
        } else {
            hw::clic::set_enabled(c.code as usize, false);
            s.stray += 1;
        }
        return;
    }
    let epc = hw::trap::mepc();
    if s.expect_pc == Some(epc) {
        dump("MFAULT", mcause, c);
        s.caught = Some(Caught { cause: mcause, epc, tval: hw::trap::mtval() });
        s.expect_pc = None;
        // SAFETY: skips the one 4-byte instruction the bring-up planted at this pc (it is built with norvc).
        unsafe { core::arch::asm!("csrw mepc, {0}", in(reg) epc + 4, options(nomem, nostack)) };
        return;
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
