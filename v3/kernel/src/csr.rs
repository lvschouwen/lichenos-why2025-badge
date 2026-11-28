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

/// mscratch = 0: "the kernel runs" for task_trap_entry, so a kernel fault goes to kernel_fault and never through a
/// stale pointer (on the P4 mscratch held the bring-up vector's trap stack top until the harness took over).
pub fn clear_mscratch() {
    // SAFETY: mscratch is only read by the trap entry; 0 is its kernel-running value.
    unsafe { core::arch::asm!("csrw mscratch, zero", options(nomem, nostack)) };
}
pub fn set_mtvec(v: usize) {
    // SAFETY: v is the address of task_trap_entry (256-byte aligned) with the platform's mode bits (direct on
    // qemu-virt, CLIC on the P4); set once, when the harness takes over the hart.
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
