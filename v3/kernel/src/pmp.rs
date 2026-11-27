//! PMP programming. Per running app: entry 0 = its text (R+X), entry 1 = its data (R+W), entry 2 = the whole address
//! space with no permissions. Entries are unlocked, so they bind U-mode only and M-mode (the kernel) keeps full access.
//! Entry 2 makes default-deny explicit and outranks any grant a firmware stage might have left in a higher entry.
//! The isolation claim is void if a LOCKED entry with a lower index than ours grants access: report() checks that.
use crate::kprintln;

const R: u8 = 1;
const W: u8 = 2;
const X: u8 = 4;
const NAPOT: u8 = 3 << 3;
const LOCK: u8 = 0x80;

/// NAPOT pmpaddr for [base, base+size); size a power of two >= 8, base aligned to size.
pub const fn napot(base: usize, size: usize) -> usize {
    (base | (size / 2 - 1)) >> 2
}
pub const ALL: usize = 0x1FFF_FFFF; // NAPOT over the full 4 GiB

macro_rules! pmpaddr_rw {
    ($($i:literal),*) => {
        fn read_addr(i: usize) -> usize {
            let v: usize;
            match i {
                // SAFETY: reading a PMP address CSR has no side effects.
                $($i => unsafe { core::arch::asm!(concat!("csrr {0}, pmpaddr", $i), out(reg) v) },)*
                _ => v = 0,
            }
            v
        }
        fn write_addr(i: usize, v: usize) {
            match i {
                // SAFETY: only called for unlocked entries the kernel owns; the matching cfg byte is written after.
                $($i => unsafe { core::arch::asm!(concat!("csrw pmpaddr", $i, ", {0}"), in(reg) v) },)*
                _ => {}
            }
        }
    };
}
pmpaddr_rw!(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);

fn read_cfg(word: usize) -> usize {
    let v: usize;
    // SAFETY: reading pmpcfgN has no side effects.
    unsafe {
        match word {
            0 => core::arch::asm!("csrr {0}, pmpcfg0", out(reg) v),
            1 => core::arch::asm!("csrr {0}, pmpcfg1", out(reg) v),
            2 => core::arch::asm!("csrr {0}, pmpcfg2", out(reg) v),
            _ => core::arch::asm!("csrr {0}, pmpcfg3", out(reg) v),
        }
    }
    v
}
fn write_cfg0(v: usize) {
    // SAFETY: entries 0..3 are the kernel's; the caller verified at boot that none is locked.
    unsafe { core::arch::asm!("csrw pmpcfg0, {0}", in(reg) v) };
}

pub fn cfg_byte(i: usize) -> u8 {
    (read_cfg(i / 4) >> ((i % 4) * 8)) as u8
}

/// Print every PMP entry left by earlier boot stages, measure the granularity, and refuse to continue if a locked
/// entry would outrank the kernel's own entries.
pub fn report() {
    let mut locked_low = false;
    for i in 0..16 {
        let c = cfg_byte(i);
        if c != 0 {
            kprintln!("pmp: entry {i:2} cfg={c:#04x} addr={:#010x}{}", read_addr(i), if c & LOCK != 0 { " LOCKED" } else { "" });
        }
        // Entry 3 counts too: a locked TOR entry 3 freezes pmpaddr2, which would silently void the deny-all entry.
        if i < 4 && c & LOCK != 0 {
            locked_low = true;
        }
    }
    if locked_low {
        kprintln!("pmp: entries 0..3 are locked by an earlier stage: isolation is NOT possible this way");
        crate::platform::exit(4);
    }
    // Granularity: with entry 0 OFF, write all ones and read back; G = trailing zero bits.
    write_cfg0(read_cfg(0) & !0xFF);
    write_addr(0, usize::MAX);
    let back = read_addr(0);
    let g = back.trailing_zeros();
    write_addr(0, 0);
    if back == 0 {
        kprintln!("pmp: pmpaddr0 is not writable: no PMP, refusing to run apps");
        crate::platform::exit(4);
    }
    let gran = 1usize << (g + 2);
    kprintln!("pmp: pmpaddr0 readback {back:#010x} -> granularity {gran} bytes");
    if gran > crate::platform::slot(0).len {
        kprintln!("pmp: granularity {gran} is coarser than an app slot: refusing to run apps");
        crate::platform::exit(4);
    }
}

/// Program entries 0..2 for one app. Called on every switch to that app.
pub fn apply(text: usize, data: usize, len: usize) {
    write_cfg0(0); // all four entries OFF while the addresses change
    write_addr(0, napot(text, len));
    write_addr(1, napot(data, len));
    write_addr(2, ALL);
    let cfg = (NAPOT | R | X) as usize | (((NAPOT | R | W) as usize) << 8) | ((NAPOT as usize) << 16);
    write_cfg0(cfg);
    // Fail closed: if any write did not stick (locked or unimplemented bits), never enter U-mode with this view.
    if read_cfg(0) & 0x00FF_FFFF != cfg
        || read_addr(0) != napot(text, len)
        || read_addr(1) != napot(data, len)
        || read_addr(2) != ALL
    {
        kprintln!("pmp: readback mismatch after apply: halting");
        crate::platform::exit(4);
    }
}
