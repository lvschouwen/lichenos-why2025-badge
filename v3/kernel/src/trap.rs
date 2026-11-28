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
    platform::before_user();
    next
}

/// A trap taken while the kernel itself ran: a kernel bug. Report and stop; never resume.
#[no_mangle]
extern "C" fn kernel_fault() -> ! {
    platform::enter_fatal();
    kprintln!("KERNEL FAULT mcause={:#x} mepc={:#x} mtval={:#x}", csr::mcause(), csr::mepc(), csr::mtval());
    platform::exit(3)
}
