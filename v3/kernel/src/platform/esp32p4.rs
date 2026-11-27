//! Kernel glue for the ESP32-P4: the console path (a byte ring in front of UART0) and the way a run ends. The
//! hardware itself lives in `lichen-platform-esp32p4`.
use crate::global::Global;
use lichen_kcore::ring::Ring;
use lichen_platform_esp32p4::uart;

pub use lichen_platform_esp32p4 as hw;

pub const NAME: &str = "esp32p4";
const LOG_RING: usize = 4096;

static RING: Global<Ring<LOG_RING>> = Global::new(Ring::new());

pub fn init() {}

/// Queue one byte for UART0 and push out whatever the FIFO has room for. Never blocks: a full ring drops its oldest
/// byte (counted, see `take_dropped`), so a dead UART cannot stall the kernel.
pub fn putc(b: u8) {
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
