//! PMP hardware glue. The layout and all decisions live in `lichen_kcore::pmp` (host-tested): this file only reads and
//! writes CSRs. Entries 0..8 hold the running app's regions, entry 14 is the fixed R-only CPU-subsystem window (#646:
//! a PMP-denied U load there hangs the P4) and entry 15 the static deny-all, all unlocked, so they bind U-mode only and
//! M-mode (the kernel) keeps full access. A locked entry among the ones we need voids the isolation claim, so
//! `report()` refuses to continue then.
use crate::global::Global;
use crate::kprintln;
use core::sync::atomic::{AtomicU32, Ordering};
use lichen_kcore::pmp::{self, Entry, Region, ENTRIES};
use lichen_kcore::view::{self, View};

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
    // SAFETY: `report()` verified that entries 0..8, 14 and 15 are unlocked (and `load` rewrites all 16 bytes of the
    // four words from a checked plan), so none of these writes touches a locked entry.
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

/// What the kernel believes is loaded, so a reload writes only what differs. Per hart, MIE clear (see `Global`).
/// PMP CSRs are per hart, so the shadow and the discovery results are too (#612 checkpoint 3: core 1 runs its own
/// discovery and U view). Each hart touches only its own index.
const HARTS: usize = 2;
static SHADOW: [Global<[Entry; ENTRIES]>; HARTS] = [const { Global::new([Entry::OFF; ENTRIES]) }; HARTS];
static GRAN: [AtomicU32; HARTS] = [const { AtomicU32::new(0) }; HARTS];
static FREE: [AtomicU32; HARTS] = [const { AtomicU32::new(0) }; HARTS];
static LOCKED: [AtomicU32; HARTS] = [const { AtomicU32::new(0) }; HARTS];

/// This hart's index into the per-hart tables (the qemu-virt harness is single-hart).
fn hart() -> usize {
    #[cfg(feature = "esp32p4")]
    return crate::smp::hartid() & 1;
    #[cfg(not(feature = "esp32p4"))]
    0
}

pub fn gran() -> u32 {
    GRAN[hart()].load(Ordering::Relaxed)
}

/// What `report()` found in the ROM state (all zero before it has run).
#[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
pub fn discovery() -> pmp::Discovery {
    pmp::Discovery {
        free_mask: FREE[hart()].load(Ordering::Relaxed) as u16,
        locked_mask: LOCKED[hart()].load(Ordering::Relaxed) as u16,
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
    // Granularity from entry 0 (with it OFF): all ones in, then G = trailing zero bits of what reads back. Entry 0's
    // cfg byte is put back afterwards.
    let cfg0 = read_cfg(0);
    if cfg[0] & pmp::LOCK == 0 {
        write_cfg(0, cfg0 & !0xFF);
    }
    write_addr(0, u32::MAX);
    let ones = read_addr(0);
    write_addr(0, saved[0]);
    if cfg[0] & pmp::LOCK == 0 {
        write_cfg(0, cfg0);
    }
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
        kprintln!("pmp: entries 0..8, 14 and 15 are not all free: isolation is NOT possible this way");
        crate::platform::exit(4);
    }
    if gran > crate::platform::slot(0).len as u32 {
        kprintln!("pmp: granularity {gran} is coarser than an app slot: refusing to run apps");
        crate::platform::exit(4);
    }
    let h = hart();
    GRAN[h].store(gran, Ordering::Relaxed);
    FREE[h].store(d.free_mask as u32, Ordering::Relaxed);
    LOCKED[h].store(d.locked_mask as u32, Ordering::Relaxed);
    // SAFETY: this hart's shadow, MIE clear, no other reference to it exists (the other hart uses its own).
    *unsafe { SHADOW[h].get() } = read_entries();
    d
}

/// Program `want`, writing only the registers that differ from the shadow, then verify by reading back. Returns the
/// number of registers written. Fails closed: a mismatch means U must not run with this view.
#[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
pub fn load(want: &pmp::Plan) -> usize {
    if !pmp::view_ok(want.entries(), gran()) {
        kprintln!("pmp: unsafe view (fixed window/deny-all missing, or an entry touches the window; #646): halting");
        crate::platform::exit(4);
    }
    write_plan(want)
}

/// Is the view the hardware holds (per the shadow) a safe one? `run_u` asserts it before every U entry.
#[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
pub fn view_loaded() -> bool {
    // SAFETY: this hart's shadow, MIE clear, no other reference to it exists (the other hart uses its own).
    pmp::view_ok(unsafe { SHADOW[hart()].get() }, gran())
}

/// `load` without the view check. Only the #646 hang reproducer uses it (probe builds).
#[cfg(feature = "probe-646")]
pub fn load_unchecked(want: &pmp::Plan) -> usize {
    write_plan(want)
}

fn write_plan(want: &pmp::Plan) -> usize {
    // SAFETY: this hart's shadow, MIE clear, no other reference to it exists (the other hart uses its own).
    let sh = unsafe { SHADOW[hart()].get() };
    let mut writes = 0;
    let mut words = [0u32; 4];
    for (i, w) in want.entries().iter().enumerate() {
        let (cfg, addr) = pmp::needs_write(&sh[i], want.entries(), i);
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
    let rb: [Entry; ENTRIES] = core::array::from_fn(|i| pmp::readback(want.entries()[i], g));
    let cur = read_entries();
    for (i, c) in cur.iter().enumerate() {
        if pmp::needs_write(c, &rb, i) != (false, false) {
            kprintln!("pmp: readback mismatch at entry {i} after load: halting");
            crate::platform::exit(4);
        }
    }
}

/// One app's view: its text (R+X) and its data (R+W), everything else denied. The bring-up's U rows load it with
/// `apply`; a process gets it once at load (`app_view`) and `switch` loads it.
#[cfg_attr(not(feature = "esp32p4"), allow(dead_code))]
pub fn apply(text: usize, data: usize, len: usize) {
    load(app_view(text, data, len).plan());
}

/// The checked view of one app slot. Exits on a plan error: the slots are the kernel's own table, so a bad one is a
/// kernel bug.
pub fn app_view(text: usize, data: usize, len: usize) -> View {
    let r = |base: usize, perm| Region { base: base as u32, end: (base + len) as u64, perm };
    checked(&[r(text, pmp::R | pmp::X), r(data, pmp::R | pmp::W)])
}

/// Plan `regions` and check the result once (`View::new`, the #646 rule). Exits on a plan error or an unsafe view.
pub fn checked(regions: &[Region]) -> View {
    match pmp::plan(regions, gran()).map(|p| View::new(p, gran())) {
        Ok(Some(v)) => v,
        Ok(None) => {
            kprintln!("pmp: a planned process view is not safe (#646): halting");
            crate::platform::exit(4)
        }
        Err(e) => {
            kprintln!("pmp: cannot plan the process view: {e:?}: halting");
            crate::platform::exit(4)
        }
    }
}

/// The context-switch reload (#617): write only the registers where `v` differs from the shadow, then read back only
/// those. `v` was checked when it was built, so nothing here walks all 16 entries. Returns the registers written.
/// Fails closed on a readback mismatch, or on a view checked at another granularity than this hart's.
pub fn switch(v: &View) -> u32 {
    if v.gran() != gran() {
        kprintln!("pmp: view checked at granularity {}, this hart has {}: halting", v.gran(), gran());
        crate::platform::exit(4);
    }
    // SAFETY: this hart's shadow, MIE clear, no other reference to it exists (the other hart uses its own).
    let sh = unsafe { SHADOW[hart()].get() };
    let d = view::diff(sh, v);
    let w = v.entries();
    for (i, e) in w.iter().enumerate() {
        if d.addr & (1 << i) != 0 {
            write_addr(i, e.addr);
            sh[i].addr = e.addr;
        }
    }
    for (n, word) in d.cfg.iter().enumerate() {
        if let Some(word) = *word {
            write_cfg(n, word);
            for b in 4 * n..4 * n + 4 {
                sh[b].cfg = w[b].cfg;
            }
        }
    }
    let addr_bad = (0..ENTRIES).find(|&i| d.addr & (1 << i) != 0 && read_addr(i) != view::addr_readback(v, i));
    let cfg_bad = (0..4).find(|&n| d.cfg[n].is_some_and(|word| read_cfg(n) != word));
    if addr_bad.is_some() || cfg_bad.is_some() {
        kprintln!("pmp: readback mismatch after a switch (pmpaddr {addr_bad:?}, pmpcfg {cfg_bad:?}): halting");
        crate::platform::exit(4);
    }
    d.writes()
}

/// Does this hart's PMP (per the shadow, which every write above and in `write_plan` keeps) hold `v`?
pub fn holds(v: &View) -> bool {
    // SAFETY: this hart's shadow, MIE clear, no other reference to it exists.
    view::holds(unsafe { SHADOW[hart()].get() }, v)
}
