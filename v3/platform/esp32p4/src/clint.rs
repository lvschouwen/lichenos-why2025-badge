//! The CLINT machine timer of the running core (TRM v1.0 2.9.3.5, confirmed on v1.0 by #608 B3t). mtime runs at
//! XTAL = 40 MHz (#608 run 9); tick maths live in `lichen_kcore::trap`. Each core sees its own mtimecmp at the same
//! address; mtime is shared. U-mode is refused this window regardless of PMP (#608).
use crate::mmio::{read32, write32};

pub const BASE: usize = 0x2000_0000;
const MTIMECMP_LO: usize = BASE + 0x4000;
const MTIMECMP_HI: usize = BASE + 0x4004;
const MTIMECTL: usize = BASE + 0x4010;
const MTIME_LO: usize = BASE + 0xBFF8;
const MTIME_HI: usize = BASE + 0xBFFC;

/// The 64-bit counter; hi/lo/hi so a carry between the halves never gives a torn value.
pub fn mtime() -> u64 {
    loop {
        // SAFETY: CLINT counter registers, reads without side effects.
        let (hi, lo, hi2) = unsafe { (read32(MTIME_HI), read32(MTIME_LO), read32(MTIME_HI)) };
        if hi == hi2 {
            return (hi as u64) << 32 | lo as u64;
        }
    }
}

/// Set this core's compare value. hi = MAX first, so no intermediate value can match early.
pub fn set_cmp(t: u64) {
    // SAFETY: this core's mtimecmp; the order keeps the pair from ever being below the target.
    unsafe {
        write32(MTIMECMP_HI, u32::MAX);
        write32(MTIMECMP_LO, t as u32);
        write32(MTIMECMP_HI, (t >> 32) as u32);
        // Read back so the posted write has reached the CLINT (and the level line has dropped) before the handler's
        // mret can re-enable MIE; otherwise the same interrupt could be taken twice (review of 8d0b0003).
        let _ = read32(MTIMECMP_HI);
    }
}

/// Never fire again (the level-type line drops once mtime < mtimecmp).
pub fn disarm() {
    set_cmp(u64::MAX);
}

/// MTIMECTL (bit 0 MTIME_EN; the ROM leaves 0x1).
pub fn ctl() -> u32 {
    // SAFETY: a CLINT control register, read only.
    unsafe { read32(MTIMECTL) }
}
