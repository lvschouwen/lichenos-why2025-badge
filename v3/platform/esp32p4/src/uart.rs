//! UART0, as left configured by the ROM (115200 8N1, the download-mode console).
use crate::mmio::{read32, write32};
use crate::psram::systimer;

const UART0: usize = 0x500C_A000; // DR_REG_UART0_BASE
const STATUS: usize = UART0 + 0x1C; // TXFIFO_CNT in [23:16]
/// The TX FIFO holds 128 bytes; stop short of that.
const FIFO_FULL: u32 = 120;

pub fn txfifo_count() -> u32 {
    // SAFETY: UART0 status register, a read without side effects.
    (unsafe { read32(STATUS) } >> 16) & 0xFF
}
/// Room for another byte in the TX FIFO.
pub fn tx_ready() -> bool {
    txfifo_count() < FIFO_FULL
}
/// Send one byte; the caller checked `tx_ready`.
pub fn put_byte(b: u8) {
    // SAFETY: UART0 TX FIFO register; the ROM configured the UART.
    unsafe { write32(UART0, b as u32) };
}
/// After the FIFO empties, the shift register still sends the last byte: 87 us at 115200 8N1. Waited by SYSTIMER so
/// it holds at any CPU clock (a count-based spin is 9 times shorter at 360 MHz; review-655 L1).
const TAIL_US: u64 = 200;

/// Wait until the FIFO is empty and give the shift register time to send the last byte.
pub fn drain() {
    while txfifo_count() != 0 {}
    match systimer::now_ticks() {
        Some(t0) => {
            while systimer::now_ticks().is_some_and(|t| systimer::elapsed(t0, t) < TAIL_US * systimer::TICKS_PER_US) {}
        }
        None => spin(200_000), // SYSTIMER dead: long enough at up to 400 MHz
    }
}
/// A busy wait that the optimiser keeps.
pub fn spin(n: u32) {
    for i in 0..n {
        core::hint::black_box(i);
    }
}
