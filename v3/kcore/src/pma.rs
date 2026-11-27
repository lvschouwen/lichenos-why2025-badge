//! PMA manager logic (#612 checkpoint 2). Pure: encoding, decoding, the table the kernel wants and the checks that
//! decide whether it may be programmed.
//!
//! PMA binds M-mode as well as U, so a wrong entry can kill the kernel itself. The table is therefore additive: the
//! kernel adds explicit cacheable entries for L2MEM, flash and PSRAM at indices below the ROM's whitelist (entry 15,
//! `0xc000001d` / `0x11ffffff` = NAPOT 0x4000_0000 + 256 MiB, RWX, cacheable) and leaves that one in place, so no
//! address the kernel already uses changes attributes. Entries are never locked (L, bit 29, stays clear): the bench has
//! not yet shown whether a locked PMA fence traps or hangs.

pub const EN: u32 = 1;
pub const R: u32 = 1 << 4;
pub const W: u32 = 1 << 3;
pub const X: u32 = 1 << 2;
pub const RWX: u32 = R | W | X;
pub const LOCK: u32 = 1 << 29;
/// Bit 27: non-cacheable. Clear = cacheable.
pub const NONCACHEABLE: u32 = 1 << 27;
pub const MODE_SHIFT: u32 = 30;
pub const TOR: u32 = 1 << MODE_SHIFT;
pub const NAPOT: u32 = 3 << MODE_SHIFT;
pub const ENTRIES: usize = 16;
/// The ROM's whitelist entry.
pub const ROM_ENTRY: usize = 15;
pub const ROM_CFG: u32 = 0xC000_001D;
pub const ROM_ADDR: u32 = 0x11FF_FFFF;

pub const L2MEM_BASE: u32 = 0x4FF0_0000;
pub const L2MEM_LEN: u64 = 1 << 20;
pub const FLASH_BASE: u32 = 0x4000_0000;
pub const FLASH_LEN: u64 = 64 << 20;
pub const PSRAM_BASE: u32 = 0x4800_0000;
pub const PSRAM_LEN: u64 = 64 << 20;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Entry {
    pub cfg: u32,
    pub addr: u32,
}

pub const fn napot(base: u32, len: u64, attrs: u32) -> Entry {
    Entry { cfg: NAPOT | EN | attrs, addr: ((base as u64 | (len / 2 - 1)) >> 2) as u32 }
}

/// The three entries the kernel adds, as (index, entry): cacheable RWX for L2MEM, flash and PSRAM.
pub const fn kernel_entries() -> [(usize, Entry); 3] {
    [
        (0, napot(L2MEM_BASE, L2MEM_LEN, RWX)),
        (1, napot(FLASH_BASE, FLASH_LEN, RWX)),
        (2, napot(PSRAM_BASE, PSRAM_LEN, RWX)),
    ]
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Decoded {
    pub lo: u64,
    pub hi: u64,
    pub perm: u32,
    pub cacheable: bool,
    pub locked: bool,
}

/// What an entry covers; None when it is OFF (valid bit clear or mode OFF) or an empty TOR.
pub fn decode(e: Entry, prev_addr: u32) -> Option<Decoded> {
    let (lo, hi) = match e.cfg >> MODE_SHIFT {
        1 => ((prev_addr as u64) << 2, (e.addr as u64) << 2),
        3 => {
            let ones = (!e.addr).trailing_zeros().min(31);
            let base = (((e.addr as u64) >> (ones + 1)) << (ones + 1)) << 2;
            (base, base + (1u64 << (ones + 3)))
        }
        _ => return None,
    };
    if e.cfg & EN == 0 || lo >= hi {
        return None;
    }
    Some(Decoded { lo, hi, perm: e.cfg & RWX, cacheable: e.cfg & NONCACHEABLE == 0, locked: e.cfg & LOCK != 0 })
}

/// The attributes at byte `a`: the lowest matching entry wins; None = no entry matches (hardware default applies).
pub fn lookup(t: &[Entry; ENTRIES], a: u32) -> Option<Decoded> {
    (0..ENTRIES).find_map(|i| {
        let d = decode(t[i], if i == 0 { 0 } else { t[i - 1].addr })?;
        ((a as u64) >= d.lo && (a as u64) < d.hi).then_some(d)
    })
}

/// Does entry 15 look like the ROM's whitelist (NAPOT 0x4000_0000 + 256 MiB, RWX, cacheable, unlocked)?
pub fn rom_anchor_ok(t: &[Entry; ENTRIES]) -> bool {
    matches!(decode(t[ROM_ENTRY], 0), Some(d)
        if d.lo == 0x4000_0000 && d.hi == 0x5000_0000 && d.perm == RWX && d.cacheable && !d.locked)
}

/// May the kernel program its entries? Its slots must be unlocked and OFF, and entry 15 must be the ROM's. Anything
/// else (a bootloader left its locked table) means the kernel does not touch PMA and reports instead.
pub fn programmable(t: &[Entry; ENTRIES]) -> bool {
    rom_anchor_ok(t)
        && kernel_entries().iter().all(|(i, _)| t[*i].cfg & (LOCK | EN) == 0 && t[*i].cfg >> MODE_SHIFT == 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rom_table() -> [Entry; ENTRIES] {
        let mut t = [Entry { cfg: NONCACHEABLE, addr: 0 }; ENTRIES];
        t[ROM_ENTRY] = Entry { cfg: ROM_CFG, addr: ROM_ADDR };
        t
    }

    #[test]
    fn rom_entry_decodes_to_the_whitelist() {
        let d = decode(Entry { cfg: ROM_CFG, addr: ROM_ADDR }, 0).unwrap();
        assert_eq!((d.lo, d.hi, d.perm, d.cacheable, d.locked), (0x4000_0000, 0x5000_0000, RWX, true, false));
        assert!(rom_anchor_ok(&rom_table()));
        assert!(programmable(&rom_table()));
    }

    #[test]
    fn rom_table_with_nc_off_entries_matches_nothing_else() {
        let t = rom_table();
        assert_eq!(lookup(&t, 0x3FFF_FFFF), None);
        assert_eq!(lookup(&t, 0x5000_0000), None);
        assert!(lookup(&t, 0x4FF7_0000).is_some());
    }

    #[test]
    fn kernel_entries_are_cacheable_rwx_and_never_locked() {
        for (_, e) in kernel_entries() {
            assert_eq!(e.cfg & LOCK, 0);
            assert_eq!(e.cfg & NONCACHEABLE, 0);
            assert_eq!(e.cfg & RWX, RWX);
        }
        let d = decode(kernel_entries()[2].1, 0).unwrap();
        assert_eq!((d.lo, d.hi), (0x4800_0000, 0x4C00_0000));
        let d = decode(kernel_entries()[1].1, 0).unwrap();
        assert_eq!((d.lo, d.hi), (0x4000_0000, 0x4400_0000));
        let d = decode(kernel_entries()[0].1, 0).unwrap();
        assert_eq!((d.lo, d.hi), (0x4FF0_0000, 0x5000_0000));
    }

    /// Everything the kernel touches keeps its attributes: PMA applies to M-mode too.
    #[test]
    fn every_kernel_address_is_unchanged_by_the_plan() {
        let before = rom_table();
        let mut after = before;
        for (i, e) in kernel_entries() {
            after[i] = e;
        }
        let touched = [
            0x4FF0_0000,
            0x4FF1_0000,
            0x4FF4_0000,
            0x4FF7_0000,
            0x4FFB_FFFF,
            0x4FC0_0000,
            0x4FFF_FFFF, // L2MEM and ROM
            0x4000_0000,
            0x4300_0000,
            0x4800_0000,
            0x4BFF_FFFF, // flash, PSRAM windows
            0x5000_0000,
            0x500C_A01C,
            0x5010_8000,
            0x2080_0000,
            0x2000_0000,
            0x3FF1_0000,
            0x8FF7_0000, // outside
        ];
        for a in touched {
            let (b, n) = (lookup(&before, a), lookup(&after, a));
            assert_eq!(b.map(|d| (d.perm, d.cacheable)), n.map(|d| (d.perm, d.cacheable)), "{a:#x}");
        }
        // And the new entries do cover the three regions explicitly.
        for a in [0x4FF7_0000u32, 0x4000_0000, 0x4800_0000] {
            let i = (0..ENTRIES)
                .find(|&i| {
                    decode(after[i], if i == 0 { 0 } else { after[i - 1].addr })
                        .is_some_and(|d| (a as u64) >= d.lo && (a as u64) < d.hi)
                })
                .unwrap();
            assert!(i < 3, "{a:#x} matched entry {i}");
        }
    }

    #[test]
    fn a_locked_or_foreign_table_is_not_programmed() {
        let mut t = rom_table();
        t[1].cfg = LOCK | NAPOT | EN | RWX;
        assert!(!programmable(&t));
        let mut t = rom_table();
        t[ROM_ENTRY].cfg |= LOCK;
        assert!(!programmable(&t));
        let mut t = rom_table();
        t[ROM_ENTRY] = Entry { cfg: LOCK | NAPOT | EN, addr: 0 };
        assert!(!rom_anchor_ok(&t));
    }
}
