//! PMP hardware glue. The layout and all decisions live in `lichen_kcore::pmp` (host-tested): this file only reads and
//! writes CSRs. Entries 0..8 hold the running app's regions, entry 15 is the static deny-all, both unlocked, so they
//! bind U-mode only and M-mode (the kernel) keeps full access. A locked entry among the ones we need voids the
//! isolation claim, so `report()` refuses to continue then.
use crate::global::Global;
use crate::kprintln;
use core::sync::atomic::{AtomicU32, Ordering};
use lichen_kcore::pmp::{self, Entry, PlanError, Region, ENTRIES};

macro_rules! pmp_csrs {
    ($($i:literal),*) => {
        fn read_addr(i: usize) -> u32 {
            let v: usize;
            match i {
                // SAFETY: reading a PMP address CSR has no side effects.
                $($i => unsafe { core::arch::asm!(concat!("csrr {0}, pmpaddr", $i), out(reg) v) },)*
                _ => v = 0,
            }
            v as u32
        }
        fn write_addr(i: usize, v: u32) {
            match i {
                // SAFETY: only called for entries the kernel owns and has verified unlocked; the cfg byte follows.
                $($i => unsafe { core::arch::asm!(concat!("csrw pmpaddr", $i, ", {0}"), in(reg) v as usize) },)*
                _ => {}
            }
        }
    };
}
pmp_csrs!(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);

fn read_cfg(word: usize) -> u32 {
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
    v as u32
}
fn write_cfg(word: usize, v: u32) {
    // SAFETY: the caller verified (report) that the entries of this word the kernel writes are unlocked, and writes
    // the other bytes back unchanged.
    unsafe {
        match word {
            0 => core::arch::asm!("csrw pmpcfg0, {0}", in(reg) v as usize),
            1 => core::arch::asm!("csrw pmpcfg1, {0}", in(reg) v as usize),
            2 => core::arch::asm!("csrw pmpcfg2, {0}", in(reg) v as usize),
            _ => core::arch::asm!("csrw pmpcfg3, {0}", in(reg) v as usize),
        }
    }
}

pub fn cfg_byte(i: usize) -> u8 {
    (read_cfg(i / 4) >> ((i % 4) * 8)) as u8
}

/// Every entry as the hardware holds it.
pub fn read_entries() -> [Entry; ENTRIES] {
    core::array::from_fn(|i| Entry { cfg: cfg_byte(i), addr: read_addr(i) })
}

/// What the kernel believes is loaded, so a reload writes only what differs. Single hart, MIE clear (see `Global`).
static SHADOW: Global<[Entry; ENTRIES]> = Global::new([Entry::OFF; ENTRIES]);
static GRAN: AtomicU32 = AtomicU32::new(0);
static FREE: AtomicU32 = AtomicU32::new(0);
static LOCKED: AtomicU32 = AtomicU32::new(0);

pub fn gran() -> u32 {
    GRAN.load(Ordering::Relaxed)
}

/// What `report()` found in the ROM state (all zero before it has run).
#[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
pub fn discovery() -> pmp::Discovery {
    pmp::Discovery {
        free_mask: FREE.load(Ordering::Relaxed) as u16,
        locked_mask: LOCKED.load(Ordering::Relaxed) as u16,
        gran: Some(gran()).filter(|g| *g != 0),
    }
}

/// Discovery by readback, in the state the earlier stage left. Prints every non-empty entry, measures the granularity
/// and which entries are free (unlocked, OFF, address writable), then refuses to continue if the planner's layout
/// (entries 0..8 and 15) is not free. Must run before the first `apply`.
pub fn report() -> pmp::Discovery {
    let mut cfg = [0u8; ENTRIES];
    for (i, c) in cfg.iter_mut().enumerate() {
        *c = cfg_byte(i);
        if *c != 0 {
            let lock = if *c & pmp::LOCK != 0 { " LOCKED" } else { "" };
            kprintln!("pmp: entry {i:2} cfg={:#04x} addr={:#010x}{lock}", *c, read_addr(i));
        }
    }
    let saved: [u32; ENTRIES] = core::array::from_fn(read_addr);
    // Granularity from entry 0 (with it OFF): all ones in, then G = trailing zero bits of what reads back.
    let off0 = read_cfg(0) & !0xFF;
    if cfg[0] & pmp::LOCK == 0 {
        write_cfg(0, off0);
    }
    write_addr(0, u32::MAX);
    let ones = read_addr(0);
    write_addr(0, saved[0]);
    let mut d = pmp::discover(&cfg, ones);
    // A free entry must also take an address: write all ones to each, read back, restore.
    let mut writable = 0u16;
    for (i, s) in saved.iter().enumerate() {
        write_addr(i, u32::MAX);
        if read_addr(i) != 0 {
            writable |= 1 << i;
        }
        write_addr(i, *s);
    }
    d.free_mask &= writable;
    let Some(gran) = d.gran else {
        kprintln!("pmp: pmpaddr0 is not writable: no PMP, refusing to run apps");
        crate::platform::exit(4);
    };
    kprintln!("pmp: pmpaddr0 readback {ones:#010x} -> granularity {gran} bytes");
    kprintln!("pmp: discovery: {} of {ENTRIES} entries free, locked mask {:#06x}", d.free(), d.locked_mask);
    if !d.layout_ok() {
        kprintln!("pmp: entries 0..8 and 15 are not all free: isolation is NOT possible this way");
        crate::platform::exit(4);
    }
    if gran > crate::platform::slot(0).len as u32 {
        kprintln!("pmp: granularity {gran} is coarser than an app slot: refusing to run apps");
        crate::platform::exit(4);
    }
    GRAN.store(gran, Ordering::Relaxed);
    FREE.store(d.free_mask as u32, Ordering::Relaxed);
    LOCKED.store(d.locked_mask as u32, Ordering::Relaxed);
    // SAFETY: single hart, MIE clear, no other reference to SHADOW exists.
    *unsafe { SHADOW.get() } = read_entries();
    d
}

/// Program `want`, writing only the registers that differ from the shadow, then verify by reading back. Returns the
/// number of registers written. Fails closed: a mismatch means U must not run with this view.
pub fn load(want: &pmp::Plan) -> usize {
    if !pmp::view_ok(&want.entries) {
        kprintln!("pmp: view without the fixed window and deny-all entries (#646): halting");
        crate::platform::exit(4);
    }
    load_unchecked(want)
}

/// `load` without the fixed-entry check. Only the #646 hang reproducer uses it (probe builds).
pub fn load_unchecked(want: &pmp::Plan) -> usize {
    // SAFETY: single hart, MIE clear, no other reference to SHADOW exists.
    let sh = unsafe { SHADOW.get() };
    let mut writes = 0;
    let mut words = [0u32; 4];
    for (i, w) in want.entries.iter().enumerate() {
        let (cfg, addr) = pmp::needs_write(&sh[i], &want.entries, i);
        if addr {
            write_addr(i, w.addr);
            sh[i].addr = w.addr;
            writes += 1;
        }
        words[i / 4] |= (w.cfg as u32) << ((i % 4) * 8);
        if cfg {
            sh[i].cfg = w.cfg;
        }
    }
    for (n, w) in words.iter().enumerate() {
        if read_cfg(n) != *w {
            write_cfg(n, *w);
            writes += 1;
        }
    }
    verify(want);
    writes
}

fn verify(want: &pmp::Plan) {
    let g = gran();
    let rb: [Entry; ENTRIES] = core::array::from_fn(|i| pmp::readback(want.entries[i], g));
    let cur = read_entries();
    for (i, c) in cur.iter().enumerate() {
        if pmp::needs_write(c, &rb, i) != (false, false) {
            kprintln!("pmp: readback mismatch at entry {i} after load: halting");
            crate::platform::exit(4);
        }
    }
}

/// Plan and load U regions. Exits on a plan error: the kernel's own regions are fixed, a bad one is a kernel bug.
pub fn load_regions(regions: &[Region]) -> Result<usize, PlanError> {
    Ok(load(&pmp::plan(regions, gran())?))
}

/// One app's view: its text (R+X) and its data (R+W), everything else denied. Called on every switch to that app.
pub fn apply(text: usize, data: usize, len: usize) {
    let r = |base: usize, perm| Region { base: base as u32, end: (base + len) as u64, perm };
    if let Err(e) = load_regions(&[r(text, pmp::R | pmp::X), r(data, pmp::R | pmp::W)]) {
        kprintln!("pmp: cannot plan the app view: {e:?}: halting");
        crate::platform::exit(4);
    }
}
