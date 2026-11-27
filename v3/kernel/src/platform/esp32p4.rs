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
const SLOT_TEXT: usize = 0x4FF6_0000;
const SLOT_DATA: usize = 0x4FF7_0000;
const SLOT_LEN: usize = 0x8000;

/// App slot i (plan on #642): 32 KB NAPOT text and data regions in NOLOAD L2MEM, above the kernel's bss and stacks.
pub fn slot(i: usize) -> Slot {
    Slot { text: SLOT_TEXT + i * SLOT_LEN, data: SLOT_DATA + i * SLOT_LEN, len: SLOT_LEN }
}

/// Queue one byte for UART0 and push out whatever the FIFO has room for. Never blocks: a full ring drops its oldest
/// byte (counted, see `take_dropped`), so a dead UART cannot stall the kernel.
pub fn putc(b: u8) {
    if FATAL.load(core::sync::atomic::Ordering::Relaxed) {
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
    if FATAL.load(core::sync::atomic::Ordering::Relaxed) {
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
