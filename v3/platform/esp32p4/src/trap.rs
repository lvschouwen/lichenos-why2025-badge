//! Trap-side CSRs of the P4 CLIC and the AXI ICM fault record. Rules (#612): mintstatus is CSR 0x346; the kernel never
//! writes mcause (in CLIC mode it aliases mpp/mpie and carries mpil, which mret restores into mintstatus).
use crate::mmio::read32;

macro_rules! csr_read {
    ($name:ident, $csr:literal) => {
        #[inline(always)]
        pub fn $name() -> u32 {
            let v: u32;
            // SAFETY: reading this M-mode CSR has no side effects.
            unsafe { core::arch::asm!(concat!("csrr {0}, ", $csr), out(reg) v, options(nomem, nostack)) };
            v
        }
    };
}
csr_read!(mcause, "mcause");
csr_read!(mepc, "mepc");
csr_read!(mtval, "mtval");
csr_read!(mstatus, "mstatus");
csr_read!(mtvec, "mtvec");
csr_read!(mintstatus, "0x346");

/// Set mstatus.MIE: M-level interrupts are taken in M-mode from here on. A compiler memory barrier (no `nomem`):
/// the handler writes kernel state, so loads after this call must not be cached from before it.
///
/// # Safety
/// The caller's trap vector must handle an interrupt taken at this point, and no `&mut` into state the handler
/// uses may be live across it.
pub unsafe fn irq_enable() {
    core::arch::asm!("csrsi mstatus, 8", options(nostack));
}
/// Clear mstatus.MIE. Also a compiler memory barrier, so what the handler wrote is read fresh after it.
pub fn irq_disable() {
    // SAFETY: clearing MIE only masks interrupts.
    unsafe { core::arch::asm!("csrci mstatus, 8", options(nostack)) };
}
/// Wait for an interrupt (wakes on a pending enabled line even with MIE clear).
pub fn wfi() {
    // SAFETY: wfi only stalls the hart.
    unsafe { core::arch::asm!("wfi", options(nomem, nostack)) };
}

/// The AXI ICM "CPU address hole" record (icm_sys_reg.h, DR_REG_AXI_ICM_BASE): the P4's only bus-fault record that
/// latches a CPU access to an unmapped address (#608 run 9). A PMP or APM fault leaves it alone.
#[derive(Clone, Copy, Debug)]
pub struct IcmRecord {
    pub int_raw: u32,
    pub cpu_hole_addr: u32,
    pub cpu_hole_info: u32,
}
const ICM: usize = 0x500A_4000;
pub fn icm_record() -> IcmRecord {
    // SAFETY: AXI ICM status registers, reads without side effects.
    unsafe {
        IcmRecord { int_raw: read32(ICM + 0x0C), cpu_hole_addr: read32(ICM + 0x40), cpu_hole_info: read32(ICM + 0x44) }
    }
}
