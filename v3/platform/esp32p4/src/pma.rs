//! PMA entry access (M1-B, #612): CSRs 0xBC0.. (cfg) and 0xBD0.. (addr), per hart. PMA binds M-mode too, so the
//! decisions (what to program, whether it is safe) live in `lichen_kcore::pma`; this file is the raw CSR access.
//! Writes are address first, then cfg, and followed by `fence` + `fence.i` so no instruction or access in flight uses
//! the old attributes.

macro_rules! pma_csrs {
    ($($i:literal => $c:literal $a:literal),*) => {
        /// (cfg, addr) of entry `i`; (0, 0) for an index past 15.
        pub fn read(i: usize) -> (u32, u32) {
            let (c, a): (usize, usize);
            match i {
                // SAFETY: reading PMA CSRs has no side effects.
                $($i => unsafe {
                    core::arch::asm!(concat!("csrr {0}, ", $c), out(reg) c, options(nomem, nostack));
                    core::arch::asm!(concat!("csrr {0}, ", $a), out(reg) a, options(nomem, nostack));
                },)*
                _ => (c, a) = (0, 0),
            }
            (c as u32, a as u32)
        }
        /// Write entry `i`: address first, then cfg. The caller checked that the entry is unlocked.
        pub fn write(i: usize, cfg: u32, addr: u32) {
            match i {
                // SAFETY: M-mode PMA CSR writes to an entry the kernel owns (checked by kcore::pma::programmable).
                $($i => unsafe {
                    core::arch::asm!(concat!("csrw ", $a, ", {0}"), in(reg) addr as usize, options(nostack));
                    core::arch::asm!(concat!("csrw ", $c, ", {0}"), in(reg) cfg as usize, options(nostack));
                },)*
                _ => {}
            }
        }
    };
}
pma_csrs!(0 => "0xbc0" "0xbd0", 1 => "0xbc1" "0xbd1", 2 => "0xbc2" "0xbd2", 3 => "0xbc3" "0xbd3",
    4 => "0xbc4" "0xbd4", 5 => "0xbc5" "0xbd5", 6 => "0xbc6" "0xbd6", 7 => "0xbc7" "0xbd7",
    8 => "0xbc8" "0xbd8", 9 => "0xbc9" "0xbd9", 10 => "0xbca" "0xbda", 11 => "0xbcb" "0xbdb",
    12 => "0xbcc" "0xbdc", 13 => "0xbcd" "0xbdd", 14 => "0xbce" "0xbde", 15 => "0xbcf" "0xbdf");

pub const ENTRIES: usize = 16;

pub fn fence() {
    // SAFETY: plain fences; they only order memory and instruction fetch.
    unsafe { core::arch::asm!("fence", "fence.i", options(nostack)) };
}
