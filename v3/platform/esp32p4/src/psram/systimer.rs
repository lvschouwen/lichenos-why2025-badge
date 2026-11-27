//! SYSTIMER unit 0, the XTAL-derived 16 MHz time base that does not move with the CPU clock (the CLINT mtime does on this chip).
use crate::mmio::{modify32, read32};

const SYSTIMER: usize = 0x500E_2000;
const CONF: usize = SYSTIMER; // clk_en bit 31
const OP: usize = SYSTIMER + 0x04; // update bit 30, value_valid bit 29
const HI: usize = SYSTIMER + 0x40; // [19:0]
const LO: usize = SYSTIMER + 0x44;
pub const TICKS_PER_US: u64 = 16;
/// HP_SYS_CLKRST PERI_CLK_CTRL21: systimer_clk_src_sel bit 29 (0 = XTAL, 1 = RC_FAST; IDF systimer_ll.h).
const CLKRST_PERI_CLK_CTRL21: usize = 0x500E_6098;

/// Ticks to microseconds.
pub const fn ticks_to_us(ticks: u64) -> u64 {
    ticks / TICKS_PER_US
}

/// Elapsed ticks between two readings; never wraps below zero.
pub const fn elapsed(from: u64, to: u64) -> u64 {
    to.saturating_sub(from)
}

/// Compose the 52-bit counter from its two words.
pub const fn counter(hi: u32, lo: u32) -> u64 {
    (((hi & 0xF_FFFF) as u64) << 32) | lo as u64
}

/// Read the counter: latch with `update`, wait for `value_valid` (bounded), then read both words. `None` if it never became valid.
/// Opens the SYSTIMER clock gate (the one write of this module).
pub fn now_ticks() -> Option<u64> {
    // SAFETY: SYSTIMER registers: the clock gate, the update strobe and reads of the latched value.
    unsafe {
        modify32(CONF, 0, 1 << 31);
        modify32(OP, 0, 1 << 30);
        let mut n = 0u32;
        while read32(OP) & (1 << 29) == 0 {
            n += 1;
            if n > 100_000 {
                return None;
            }
        }
        Some(counter(read32(HI), read32(LO)))
    }
}

/// The SYSTIMER clock source select: 0 = XTAL (the 16 MHz this module assumes), 1 = RC_FAST. Read only.
pub fn src_sel() -> u32 {
    // SAFETY: a clock-control register, read only.
    unsafe { (read32(CLKRST_PERI_CLK_CTRL21) >> 29) & 1 }
}

/// Microseconds since the counter started (`None` if the counter did not answer).
pub fn now_us() -> Option<u64> {
    now_ticks().map(ticks_to_us)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timer_arithmetic() {
        assert_eq!(ticks_to_us(16_000_000), 1_000_000);
        assert_eq!(elapsed(100, 40), 0); // a reading that went backwards does not wrap
        assert_eq!(elapsed(40, 100), 60);
        assert_eq!(counter(0x1, 0x2), (1 << 32) | 2);
        assert_eq!(counter(0xFFFF_FFFF, 0), 0xF_FFFFu64 << 32); // only 20 high bits exist
    }
}
