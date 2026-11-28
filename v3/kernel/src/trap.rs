use crate::{csr, kprintln, platform, sched};

/// Saved app state. Layout is shared with trap.S: x0..x31 at 4*i, pc at 128.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Context {
    pub regs: [usize; 32],
    pub pc: usize,
}
impl Context {
    pub const ZERO: Self = Self { regs: [0; 32], pc: 0 };
}
const _: () = assert!(core::mem::size_of::<Context>() == 132);

extern "C" {
    pub fn enter_user(ctx: *mut Context) -> !;
    pub fn task_trap_entry();
}

#[no_mangle]
extern "C" fn trap_handler(ctx: *mut Context) -> *mut Context {
    // SAFETY: trap_handler is the only kernel entry while apps run and is not re-entrant (Global invariant).
    let k = unsafe { sched::KERNEL.get() };
    let next = sched::on_trap(k, ctx, csr::mcause(), csr::mtval());
    check_return(k, next);
    next
}

/// Every return to U passes here: `next` must be the current task's context, the PMP must hold exactly that task's
/// view (not just a safe-shaped one), and the platform's own view rule must hold (#646 on the P4). Fails closed.
pub fn check_return(k: &sched::Kernel, next: *const Context) {
    let t = &k.tasks[k.current];
    let s = t.slot;
    if !core::ptr::eq(next, &t.ctx) || !crate::pmp::applied_is(s.text, s.data, s.len) || !platform::view_safe() {
        platform::enter_fatal();
        kprintln!("KERNEL: return to U without task {}'s PMP view: halting", k.current);
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
