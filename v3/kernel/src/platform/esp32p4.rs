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
