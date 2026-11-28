//! QEMU `virt`: NS16550 UART, CLINT timer (10 MHz), sifive_test exit device. Proves kernel logic only.
use crate::sched::Slot;

pub const NAME: &str = "qemu-virt";
pub const UART_BASE: usize = 0x1000_0000;
/// The peripheral the MMIO isolation case reads.
pub const MMIO_TARGET: usize = UART_BASE;
/// mtvec mode bits: direct.
pub const MTVEC_MODE: usize = 0;
const CLINT_MTIMECMP: usize = 0x0200_4000;
const CLINT_MTIME: usize = 0x0200_BFF8;
const TEST_DEV: usize = 0x0010_0000;
/// The virt machine's CLINT runs at a fixed 10 MHz (QEMU's RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ); nothing to measure.
const MTIME_HZ: u64 = 10_000_000;
const TICK: u64 = lichen_kcore::timebase::us_to_ticks(1000, MTIME_HZ); // 1 ms
const SLOT_TEXT: usize = 0x8040_0000;
const SLOT_DATA: usize = 0x8080_0000;
const SLOT_LEN: usize = 0x1_0000;

pub fn init() {}
/// The console is a plain polled UART here, so a fault dump needs no switch.
pub fn enter_fatal() {}
/// CLINT interrupts are the only ones enabled, so nothing else can be pending.
pub fn mask_irq(_code: usize) {}

pub fn slot(i: usize) -> Slot {
    Slot { text: SLOT_TEXT + i * SLOT_LEN, data: SLOT_DATA + i * SLOT_LEN, len: SLOT_LEN }
}

pub fn putc(b: u8) {
    // SAFETY: UART_BASE is the virt machine's 16550; LSR is at +5, THR at +0.
    unsafe {
        while core::ptr::read_volatile((UART_BASE + 5) as *const u8) & 0x20 == 0 {}
        core::ptr::write_volatile(UART_BASE as *mut u8, b);
    }
}

fn mtime() -> u64 {
    // SAFETY: CLINT mtime is a 64-bit MMIO counter; hi/lo/hi read avoids a torn value.
    unsafe {
        loop {
            let hi = core::ptr::read_volatile((CLINT_MTIME + 4) as *const u32);
            let lo = core::ptr::read_volatile(CLINT_MTIME as *const u32);
            if core::ptr::read_volatile((CLINT_MTIME + 4) as *const u32) == hi {
                return (hi as u64) << 32 | lo as u64;
            }
        }
    }
}
pub fn timer_init() {
    timer_rearm();
    crate::csr::enable_timer_irq();
}
pub fn timer_rearm() {
    let t = mtime() + TICK;
    // SAFETY: hart 0's mtimecmp; writing hi = MAX first prevents a spurious match between the two halves.
    unsafe {
        core::ptr::write_volatile((CLINT_MTIMECMP + 4) as *mut u32, u32::MAX);
        core::ptr::write_volatile(CLINT_MTIMECMP as *mut u32, t as u32);
        core::ptr::write_volatile((CLINT_MTIMECMP + 4) as *mut u32, (t >> 32) as u32);
    }
}
pub fn is_timer_irq(code: usize) -> bool {
    code == 7
}
pub fn sync_icache(_addr: usize, _len: usize) {
    // SAFETY: fence.i orders the loader's stores before later instruction fetches on this hart.
    unsafe { core::arch::asm!("fence.i") };
}
pub fn exit(code: u32) -> ! {
    // SAFETY: the sifive_test device ends QEMU: 0x5555 = pass, (code << 16) | 0x3333 = fail.
    unsafe {
        let v = if code == 0 { 0x5555 } else { (code << 16) | 0x3333 };
        core::ptr::write_volatile(TEST_DEV as *mut u32, v);
    }
    // SAFETY: wfi parks the hart after the exit device was written; QEMU ends the run before this returns.
    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}
