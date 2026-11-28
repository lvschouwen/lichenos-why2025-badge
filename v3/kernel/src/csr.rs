//! The only module with CSR inline assembly besides pmp.rs. Each function is a single CSR instruction.

macro_rules! read_csr {
    ($name:ident, $csr:literal) => {
        #[inline(always)]
        pub fn $name() -> usize {
            let v: usize;
            // SAFETY: reading this CSR has no side effects in M-mode.
            unsafe { core::arch::asm!(concat!("csrr {0}, ", $csr), out(reg) v, options(nomem, nostack)) };
            v
        }
    };
}
read_csr!(mcause, "mcause");
read_csr!(mtval, "mtval");
read_csr!(mepc, "mepc");
// mcycle, low word on RV32 (callers time short spans with wrapping_sub). The stamp in trap.S is the same counter.
read_csr!(mcycle, "mcycle");

/// mscratch = 0: "the kernel runs" for task_trap_entry, so a kernel fault goes to kernel_fault and never through a
/// stale pointer (on the P4 mscratch held the bring-up vector's trap stack top until the harness took over).
pub fn clear_mscratch() {
    // SAFETY: mscratch is only read by the trap entry; 0 is its kernel-running value.
    unsafe { core::arch::asm!("csrw mscratch, zero", options(nomem, nostack)) };
}
#[cfg(feature = "esp32p4")]
read_csr!(mscratch, "mscratch");
/// Put back the mscratch value a vector other than task_trap_entry expects (core 1 after its process stage, #617).
#[cfg(feature = "esp32p4")]
pub fn set_mscratch(v: usize) {
    // SAFETY: mscratch is only read by the trap entry; the caller passes the value its vector's invariant needs.
    unsafe { core::arch::asm!("csrw mscratch, {0}", in(reg) v, options(nomem, nostack)) };
}
pub fn set_mtvec(v: usize) {
    // SAFETY: v is the address of a trap vector (task_trap_entry, or trap_entry on core 1 after its process stage;
    // both 256-byte aligned) with the platform's mode bits (direct on qemu-virt, CLIC on the P4).
    unsafe { core::arch::asm!("csrw mtvec, {0}", in(reg) v, options(nomem, nostack)) };
}
/// Prepare mstatus so mret lands in U-mode: MPP = 0 (U), MPIE = 1.
pub fn mstatus_return_to_user() {
    // SAFETY: only the MPP and MPIE fields change; they affect the next mret, which the kernel issues on purpose.
    unsafe {
        core::arch::asm!("csrc mstatus, {0}", in(reg) 0x1800usize, options(nomem, nostack));
        core::arch::asm!("csrs mstatus, {0}", in(reg) 0x80usize, options(nomem, nostack));
    }
}
#[cfg(feature = "qemu-virt")]
pub fn enable_timer_irq() {
    // SAFETY: sets mie.MTIE. In M-mode mstatus.MIE stays 0, so the kernel itself is never interrupted; in U-mode
    // M-level interrupts are always taken.
    unsafe { core::arch::asm!("csrs mie, {0}", in(reg) 0x80usize, options(nomem, nostack)) };
}
