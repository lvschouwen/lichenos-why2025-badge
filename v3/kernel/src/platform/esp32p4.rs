//! Kernel glue for the ESP32-P4: the console path (a byte ring in front of UART0) and the way a run ends. The
//! hardware itself lives in `lichen-platform-esp32p4`.
use crate::global::Global;
use lichen_kcore::ring::Ring;
use lichen_platform_esp32p4::uart;

pub use lichen_platform_esp32p4 as hw;

pub const NAME: &str = "esp32p4";
const LOG_RING: usize = 4096;

static RING: Global<Ring<LOG_RING>> = Global::new(Ring::new());
/// Set once by a fatal trap path: from then on `putc` writes straight to the UART and never touches RING, which the
/// faulting code may have been in the middle of updating.
static FATAL: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);

/// Switch the console to the raw, ring-free path for a fault dump. One way: the run ends after the dump.
pub fn enter_fatal() {
    FATAL.store(true, core::sync::atomic::Ordering::Relaxed);
}

pub fn init() {}

// The isolation harness (#613, tests.rs) runs the same scheduler as qemu-virt. What it needs from the chip follows.

/// The peripheral the MMIO isolation case reads: UART0's status register, the address #612's `u.deny.peri` row
/// proved faults cleanly (code 5) on this chip.
pub const MMIO_TARGET: usize = 0x500C_A01C;
/// mtvec mode bits: the P4 hardwires CLIC mode (3).
pub const MTVEC_MODE: usize = 3;
/// The harness tick: 1 ms, as on qemu-virt, so the CPU budget (50 ticks) means the same on both. A function, not a
/// const: the mtime rate is measured at boot (#654, `crate::timebase`).
fn tick() -> u64 {
    crate::timebase::us_to_ticks(1000)
}

/// CLINT compare plus CLIC line 7 (M level, highest priority, threshold 0), as `mtrap::tick_start` arms it.
pub fn timer_init() {
    let line = lichen_kcore::trap::IRQ_MTIMER as usize;
    hw::clint::disarm();
    hw::clic::configure(line, hw::clic::ATTR_M_LEVEL, hw::clic::CTL_MAX);
    hw::clic::set_threshold(0);
    timer_rearm();
    hw::clic::set_enabled(line, true);
}
/// The next deadline, one period from now. `set_cmp` reads back, so the level line has dropped before the mret.
pub fn timer_rearm() {
    hw::clint::set_cmp(hw::clint::mtime() + tick());
}
pub fn timer_stop() {
    hw::clic::set_enabled(lichen_kcore::trap::IRQ_MTIMER as usize, false);
    hw::clint::disarm();
}
pub fn is_timer_irq(code: usize) -> bool {
    code == lichen_kcore::trap::IRQ_MTIMER as usize
}
/// A stray CLIC line is masked (`m_trap` does the same), or its level would be taken again on every return.
pub fn mask_irq(code: usize) {
    if code < hw::clic::LINES {
        hw::clic::set_enabled(code, false);
    }
}

/// Every return to U passes here: never without the fixed window entry (a U window load would hang the P4, #646).
pub fn before_user() {
    if !crate::pmp::view_loaded() {
        enter_fatal();
        crate::kprintln!("KERNEL: return to U without a safe PMP view (#646): halting");
        exit(4);
    }
}

/// Code the loader just wrote must reach instruction fetch (write-back L1 DCache, per-core ICache): see
/// `hw::cache::sync_icache`. A failed cache operation stops the run; the app must not execute stale bytes.
pub fn sync_icache(addr: usize, len: usize) {
    // SAFETY: the slot is L2MEM that no core executes from while the kernel loads it (core 1 is parked in wfi on
    // kernel text, #612 core1.park), and only core 0 uses the cache sync registers.
    let rc = unsafe { hw::cache::sync_icache(addr, len) };
    if rc != 0 {
        crate::kprintln!("KERNEL: cache sync of the app text at {addr:#x} failed (rc {rc}): halting");
        exit(4);
    }
}

pub use lichen_kcore::image::Slot;

/// App slot `i` from the one table in `lichen_kcore::slots` (32 KB NAPOT text and data regions in NOLOAD L2MEM, above
/// the kernel's bss and stacks). An index past the table is a kernel bug and stops the run.
pub fn slot(i: usize) -> Slot {
    lichen_kcore::slots::slot(i).unwrap_or_else(|| exit(2))
}

/// Raw UART output, no ring: after a fatal trap (see FATAL), and always on hart 1. The ring is a single-context
/// `Global` that only core 0 uses, so core 1's prints (its banner before UP, and its failure paths at any time,
/// review M5) can never race core 0's (they may interleave bytes on the wire, nothing worse).
fn raw() -> bool {
    FATAL.load(core::sync::atomic::Ordering::Relaxed) || crate::smp::hartid() != 0
}

/// Queue one byte for UART0 and push out whatever the FIFO has room for. Never blocks: a full ring drops its oldest
/// byte (counted, see `take_dropped`), so a dead UART cannot stall the kernel.
pub fn putc(b: u8) {
    if raw() {
        while !uart::tx_ready() {}
        uart::put_byte(b);
        return;
    }
    // SAFETY: single hart, no interrupts or traps use the console (Global invariant); the &mut ends in this call.
    let r = unsafe { RING.get() };
    r.push(b);
    pump(r);
}

fn pump(r: &mut Ring<LOG_RING>) {
    while uart::tx_ready() {
        match r.pop() {
            Some(b) => uart::put_byte(b),
            None => break,
        }
    }
}

/// Block until every queued byte has left the UART.
pub fn flush() {
    if raw() {
        uart::drain();
        return;
    }
    // SAFETY: as `putc`.
    let r = unsafe { RING.get() };
    while !r.is_empty() {
        pump(r);
    }
    uart::drain();
}

/// Bytes the ring had to drop since the last call.
pub fn take_dropped() -> u32 {
    // SAFETY: as `putc`.
    unsafe { RING.get() }.take_dropped()
}

/// Report the end of the run (`END fatal` for a nonzero code), then park. The host resets the badge after END.
pub fn exit(code: u32) -> ! {
    if crate::smp::hartid() != 0 {
        // Hart 1 never writes END (review M5): it posts its code and parks; core 0 sees it in its next mailbox wait
        // and ends the run with END fatal, so no row can follow an END.
        crate::kprintln!("core1: exit code {code}; core 0 ends the run");
        crate::smp::post_exit(code);
        loop {
            hw::trap::wfi();
        }
    }
    let reason = if code == 0 { "ok" } else { "fatal" };
    let _ = lichen_kcore::check::write_end(&mut crate::console::Console, reason);
    park()
}

pub fn park() -> ! {
    flush();
    loop {
        // SAFETY: wait for interrupt; nothing is enabled, so this parks the hart.
        unsafe { core::arch::asm!("wfi", options(nomem, nostack)) };
    }
}
