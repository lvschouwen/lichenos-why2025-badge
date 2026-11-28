use crate::{csr, kprintln, platform, pmp, sched};

/// Saved process state. Layout is shared with trap.S: x0..x31 at 4*i, pc at 128, the low word of mcycle at the trap
/// entry at 132 (stamped right after x5 is saved; the switch cost is measured from it).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Context {
    pub regs: [usize; 32],
    pub pc: usize,
    pub entry_cycle: usize,
}
impl Context {
    pub const ZERO: Self = Self { regs: [0; 32], pc: 0, entry_cycle: 0 };
}
const _: () = assert!(core::mem::size_of::<Context>() == 136);

extern "C" {
    /// Enter `ctx` (mstatus.MPP must be U) and come back only through `task_leave`, with its value.
    pub fn task_run(ctx: *mut Context) -> usize;
    /// From trap context: return `v` from this hart's `task_run`.
    #[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
    pub fn task_leave(v: usize) -> !;
    pub fn task_trap_entry();
}

#[no_mangle]
extern "C" fn trap_handler(ctx: *mut Context) -> *mut Context {
    // SAFETY: trap_handler is this hart's only kernel entry while processes run and is not re-entrant (Global
    // invariant); each hart uses its own Kernel.
    let k = unsafe { sched::this() };
    let next = sched::on_trap(k, ctx, csr::mcause(), csr::mtval());
    check_return(k, next);
    // SAFETY: this hart's samples; trap context, the only reference.
    let perf = unsafe { sched::perf() };
    if perf.pending {
        // A tick switched processes: entry stamp (trap.S) to here, the restore path and mret not included.
        perf.pending = false;
        // SAFETY: ctx is the preempted process's Context in this hart's table; nothing writes it while we read.
        let entry = unsafe { (*ctx).entry_cycle };
        perf.switch.push((csr::mcycle() as u32).wrapping_sub(entry as u32));
    }
    next
}

/// Every return to U passes here: `next` must be the current process's context, and the PMP must hold exactly that
/// process's view. The view passed `view_ok` when it was built (#646 on the P4: the fixed window and deny-all), so
/// "the shadow holds it" is also the platform's view rule, without the 16-entry walk on every return (#617). Fails
/// closed.
pub fn check_return(k: &sched::Kernel, next: *const Context) {
    let p = &k.procs[k.current];
    if !core::ptr::eq(next, &p.ctx) || !p.view.as_ref().is_some_and(pmp::holds) {
        platform::enter_fatal();
        kprintln!("KERNEL: return to U without process {}'s PMP view: halting", k.current);
        platform::exit(4);
    }
}

/// A trap taken while the kernel itself ran: a kernel bug. Report and stop; never resume. task_kernel_trap has put
/// mscratch back to 0 and switched to the fault stack first.
#[no_mangle]
extern "C" fn kernel_fault() -> ! {
    platform::enter_fatal();
    kprintln!("KERNEL FAULT mcause={:#x} mepc={:#x} mtval={:#x}", csr::mcause(), csr::mepc(), csr::mtval());
    platform::exit(3)
}
