//! The CLIC of the running core (clic_reg.h; the other core's window is at +0x10000). Line `id` has four bytes at
//! CTRL + 4 * id: ip, ie, attr, ctl. Rules (#612): the threshold is MMIO here, not a CSR, and a write takes effect only
//! after a read-back (interrupt_clic.h).
use crate::mmio::{read32, read8, write32, write8};

const BASE: usize = 0x2080_0000;
const THRESH: usize = BASE + 0x8;
const CTRL: usize = BASE + 0x1000;
/// attr: mode M (bits 7:6 = 3), level-triggered (2:1 = 0), not vectored (bit 0 = 0, so it lands at the mtvec base).
pub const ATTR_M_LEVEL: u8 = 0xC0;
/// The highest level.
pub const CTL_MAX: u8 = 0xFF;

const fn byte(id: usize, b: usize) -> usize {
    CTRL + 4 * id + b
}

/// Configure a line (masked while it is set up).
pub fn configure(id: usize, attr: u8, ctl: u8) {
    // SAFETY: this core's CLIC line bytes; ie goes off first so a half-configured line cannot fire.
    unsafe {
        write8(byte(id, 1), 0);
        write8(byte(id, 2), attr);
        write8(byte(id, 3), ctl);
    }
}

pub fn set_enabled(id: usize, on: bool) {
    // SAFETY: this core's CLIC ie byte for the line.
    unsafe { write8(byte(id, 1), on as u8) };
}

/// ip, ie, attr, ctl of a line.
pub fn line(id: usize) -> [u8; 4] {
    // SAFETY: reads of this core's CLIC line bytes.
    core::array::from_fn(|b| unsafe { read8(byte(id, b)) })
}

/// Write the threshold and read it back (the read makes it take effect). Returns the value read.
pub fn set_threshold(v: u32) -> u32 {
    // SAFETY: this core's CLIC threshold register.
    unsafe {
        write32(THRESH, v);
        read32(THRESH)
    }
}
