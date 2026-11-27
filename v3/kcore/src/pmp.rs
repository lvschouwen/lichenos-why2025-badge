//! PMP planner (#612 checkpoint 2): regions in, 16 PMP entries out, plus discovery and reload diffing. Pure logic.
//!
//! Layout: the dynamic window is entries `0..DYN_BUDGET` (the app's regions, lowest index wins), entries in between
//! stay OFF for static kernel use, and entry 15 is the static deny-all (NAPOT over 4 GiB, no permissions). Entries are
//! never locked: they bind U-mode only and M keeps full access (a locked PMP M-store hangs, #608 run 6).
//!
//! Rejected with an error, never planned partially: W+X (W^X), W without R (reserved encoding), empty or unaligned
//! regions, overlaps (so "a byte is granted iff it lies in a region" is unambiguous) and plans over the budget.

pub const R: u8 = 1;
pub const W: u8 = 2;
pub const X: u8 = 4;
pub const PERM_MASK: u8 = R | W | X;
pub const TOR: u8 = 0x08;
pub const NAPOT: u8 = 0x18;
pub const MODE_MASK: u8 = 0x18;
pub const LOCK: u8 = 0x80;

pub const ENTRIES: usize = 16;
/// At most this many entries per app (the issue's budget); the window is `0..DYN_BUDGET`.
pub const DYN_BUDGET: usize = 8;
pub const DENY_ALL_INDEX: usize = ENTRIES - 1;
/// The CPU-subsystem window (CLINT, CLIC, other core's CLIC, user CLIC, holes). A PMP-DENIED U load there hangs the
/// P4 with no trap (#646, bench 5223aad5), while a PMP-allowed one is refused cleanly by the hardware privilege
/// check and a PMP-denied store or fetch is clean. So every U view carries this fixed R-only entry: U loads reach the
/// hardware check, stores and fetches are denied by the PMP. Index 14: after the dynamic window, before deny-all.
pub const WINDOW_INDEX: usize = ENTRIES - 2;
pub const WINDOW_BASE: u64 = 0x2000_0000;
pub const WINDOW_END: u64 = 0x3000_0000;
/// The P4's PMP granularity (pmpaddr0 reads back 0x3fffffe0 in ROM state, #612 bench).
pub const GRAN_P4: u32 = 128;
const SPACE: u64 = 1 << 32;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Entry {
    pub cfg: u8,
    /// pmpaddr: the byte address >> 2.
    pub addr: u32,
}
impl Entry {
    pub const OFF: Entry = Entry { cfg: 0, addr: 0 };
    pub const DENY_ALL: Entry = Entry { cfg: NAPOT, addr: 0x1FFF_FFFF };
    /// NAPOT over 0x2000_0000..0x3000_0000 (256 MiB), R only.
    pub const WINDOW: Entry = Entry { cfg: NAPOT | R, addr: 0x09FF_FFFF };
    fn mode(self) -> u8 {
        self.cfg & MODE_MASK
    }
}

/// One U-visible region: bytes `base..end` (end exclusive, up to 4 GiB) with R/W/X bits.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Region {
    pub base: u32,
    pub end: u64,
    pub perm: u8,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PlanError {
    /// Empty region, or ends past 4 GiB.
    BadRange,
    /// Base or end not a multiple of the granularity.
    Unaligned,
    /// No permission bits, or bits outside R/W/X.
    BadPerm,
    /// W and X together.
    WriteExec,
    /// W without R: reserved in the PMP encoding.
    WriteOnly,
    Overlap,
    /// More regions than the budget, or the plan needs more than `DYN_BUDGET` entries.
    Budget,
    /// A region touches the CPU-subsystem window, which only the fixed entry may cover (#646).
    CoreLocal,
    /// A TOR top the pmpaddr cannot hold (the top 128 B of the address space).
    TopOfSpace,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Plan {
    entries: [Entry; ENTRIES],
    /// Entries used in the dynamic window.
    used: usize,
}
impl Plan {
    /// Only `plan()` builds a Plan, so every Plan has passed the region checks and carries the fixed entries.
    pub fn entries(&self) -> &[Entry; ENTRIES] {
        &self.entries
    }
    pub fn used(&self) -> usize {
        self.used
    }
    /// The #646 hang reproducer: the same plan without the window entry. Probe builds only.
    #[cfg(feature = "probe-646")]
    pub fn without_window(mut self) -> Plan {
        self.entries[WINDOW_INDEX] = Entry::OFF;
        self
    }
}

fn check_region(r: &Region, gran: u32) -> Result<(), PlanError> {
    if r.end <= r.base as u64 || r.end > SPACE {
        return Err(PlanError::BadRange);
    }
    if !r.base.is_multiple_of(gran) || !r.end.is_multiple_of(gran as u64) {
        return Err(PlanError::Unaligned);
    }
    if r.perm == 0 || r.perm & !PERM_MASK != 0 {
        return Err(PlanError::BadPerm);
    }
    if r.perm & W != 0 && r.perm & X != 0 {
        return Err(PlanError::WriteExec);
    }
    if r.perm & W != 0 && r.perm & R == 0 {
        return Err(PlanError::WriteOnly);
    }
    if (r.base as u64) < WINDOW_END && r.end > WINDOW_BASE {
        return Err(PlanError::CoreLocal);
    }
    Ok(())
}

/// Validate, sort by base, reject overlaps and merge touching regions of equal permission.
fn normalise(regions: &[Region], gran: u32, out: &mut [Region; DYN_BUDGET]) -> Result<usize, PlanError> {
    if regions.len() > DYN_BUDGET {
        return Err(PlanError::Budget);
    }
    let mut n = 0;
    for r in regions {
        check_region(r, gran)?;
        let mut i = n;
        while i > 0 && out[i - 1].base > r.base {
            out[i] = out[i - 1];
            i -= 1;
        }
        out[i] = *r;
        n += 1;
    }
    let mut m = 0;
    for i in 0..n {
        if m > 0 && (out[i].base as u64) < out[m - 1].end {
            return Err(PlanError::Overlap);
        }
        if m > 0 && out[i].base as u64 == out[m - 1].end && out[i].perm == out[m - 1].perm {
            out[m - 1].end = out[i].end;
        } else {
            out[m] = out[i];
            m += 1;
        }
    }
    Ok(m)
}

fn napot_entry(r: &Region, gran: u32) -> Option<Entry> {
    let len = r.end - r.base as u64;
    // NAPOT needs a power of two of at least 8 bytes (NA4 is unavailable at coarse granularity) and the granule.
    if !len.is_power_of_two() || len < (gran as u64).max(8) || !(r.base as u64).is_multiple_of(len) {
        return None;
    }
    Some(Entry { cfg: NAPOT | r.perm, addr: ((r.base as u64 | (len / 2 - 1)) >> 2) as u32 })
}

/// Plan `regions` for U-mode at granularity `gran` (a power of two, 128 on the P4).
pub fn plan(regions: &[Region], gran: u32) -> Result<Plan, PlanError> {
    let mut sorted = [Region { base: 0, end: 0, perm: 0 }; DYN_BUDGET];
    let n = normalise(regions, gran, &mut sorted)?;
    let mut entries = [Entry::OFF; ENTRIES];
    entries[WINDOW_INDEX] = Entry::WINDOW;
    entries[DENY_ALL_INDEX] = Entry::DENY_ALL;
    let mut used = 0usize;
    // The byte address the previous entry's pmpaddr holds, when the next TOR entry may take it as its base.
    let mut tor_base: Option<u64> = None;
    for r in &sorted[..n] {
        if let Some(e) = napot_entry(r, gran) {
            if used >= DYN_BUDGET {
                return Err(PlanError::Budget);
            }
            entries[used] = e;
            used += 1;
            tor_base = None;
            continue;
        }
        if r.end > 0xFFFF_FFFF - gran as u64 + 1 {
            return Err(PlanError::TopOfSpace);
        }
        // Entry 0 in TOR mode starts at address 0 on its own; any other entry takes pmpaddr[i-1].
        let shared = tor_base == Some(r.base as u64) || (used == 0 && r.base == 0);
        if used + 1 + usize::from(!shared) > DYN_BUDGET {
            return Err(PlanError::Budget);
        }
        if !shared {
            entries[used] = Entry { cfg: 0, addr: r.base >> 2 };
            used += 1;
        }
        entries[used] = Entry { cfg: TOR | r.perm, addr: (r.end >> 2) as u32 };
        used += 1;
        tor_base = Some(r.end);
    }
    Ok(Plan { entries, used })
}

/// Is this a safe U view (#646)? It carries the fixed entries and no earlier entry touches the window. The kernel
/// refuses to load any other view.
pub fn view_ok(entries: &[Entry; ENTRIES]) -> bool {
    if entries[WINDOW_INDEX] != Entry::WINDOW || entries[DENY_ALL_INDEX] != Entry::DENY_ALL {
        return false;
    }
    // No entry in front of the window entry may touch the window: a perm-0 or W/X entry there would win over it and
    // bring back the hang (a PMP-denied U load) or open the window to U.
    (0..WINDOW_INDEX).all(|i| entry_range(entries, i).is_none_or(|(lo, hi)| hi <= WINDOW_BASE || lo >= WINDOW_END))
}

/// The byte range entry `i` matches (TOR takes its base from entry i-1; an empty TOR matches nothing).
fn entry_range(entries: &[Entry; ENTRIES], i: usize) -> Option<(u64, u64)> {
    let e = entries[i];
    let (lo, hi) = match e.mode() {
        TOR => ((if i == 0 { 0 } else { (entries[i - 1].addr as u64) << 2 }), (e.addr as u64) << 2),
        NAPOT => {
            let ones = (!e.addr).trailing_zeros().min(31);
            let size = 1u64 << (ones + 3);
            let base = (((e.addr as u64) >> (ones + 1)) << (ones + 1)) << 2;
            (base, base + size)
        }
        _ => return None,
    };
    (lo < hi).then_some((lo, hi))
}

/// What U-mode may do at byte `a` under `entries` (spec semantics: the lowest matching entry decides, no match = deny).
/// This is the oracle the tests compare plans against; the kernel never calls it on the hot path.
pub fn u_perms(entries: &[Entry; ENTRIES], a: u32) -> u8 {
    let a = a as u64;
    for (i, e) in entries.iter().enumerate() {
        if let Some((lo, hi)) = entry_range(entries, i) {
            if a >= lo && a < hi {
                return e.cfg & PERM_MASK;
            }
        }
    }
    0
}

/// What the hardware reads back for `e` at index `i`: low pmpaddr bits are forced by the granularity. A plan whose
/// entries read back unchanged can be verified by a plain read.
pub fn readback(e: Entry, gran: u32) -> Entry {
    let g_mask = (gran / 4).wrapping_sub(1); // bits [G-1:0] of pmpaddr
    let addr = match e.mode() {
        NAPOT => e.addr | (g_mask >> 1),
        _ => e.addr & !g_mask,
    };
    Entry { cfg: e.cfg, addr }
}

/// Must pmpaddr of entry `i` be written to reach `want`, given what is loaded? An OFF entry's address only matters
/// when the next entry is TOR (it is that entry's base). Entries a smaller plan leaves alone are reused as they are.
pub fn needs_write(cur: &Entry, want: &[Entry; ENTRIES], i: usize) -> (bool, bool) {
    let w = want[i];
    let cfg = cur.cfg != w.cfg;
    let addr_matters = w.mode() != 0 || (i + 1 < ENTRIES && want[i + 1].mode() == TOR);
    (cfg, addr_matters && cur.addr != w.addr)
}

/// Outcome of looking at the PMP in the ROM state.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Discovery {
    /// Bit i set: entry i is unlocked and OFF, so the kernel can use it.
    pub free_mask: u16,
    /// Bit i set: entry i is locked (cannot be changed until reset).
    pub locked_mask: u16,
    /// Granularity in bytes, from the all-ones readback of pmpaddr0 (None: not writable, no PMP).
    pub gran: Option<u32>,
}
impl Discovery {
    pub fn free(&self) -> u32 {
        self.free_mask.count_ones()
    }
    /// Entries 0..DYN_BUDGET, the window entry and the deny-all entry are all free, which is what the planner's layout needs.
    pub fn layout_ok(&self) -> bool {
        let need: u16 = ((1u32 << DYN_BUDGET) - 1) as u16 | 1 << WINDOW_INDEX | 1 << DENY_ALL_INDEX;
        self.free_mask & need == need && self.locked_mask & need == 0
    }
}

pub fn discover(cfg: &[u8; ENTRIES], all_ones_readback: u32) -> Discovery {
    let (mut free_mask, mut locked_mask) = (0u16, 0u16);
    for (i, c) in cfg.iter().enumerate() {
        if c & LOCK != 0 {
            locked_mask |= 1 << i;
        } else if c & MODE_MASK == 0 {
            free_mask |= 1 << i;
        }
    }
    let gran = if all_ones_readback == 0 { None } else { Some(1u32 << (all_ones_readback.trailing_zeros() + 2)) };
    Discovery { free_mask, locked_mask, gran }
}

#[cfg(test)]
mod tests {
    use super::*;
    extern crate std;
    use proptest::prelude::*;
    use std::vec;
    use std::vec::Vec;

    const G: u32 = GRAN_P4;

    fn rg(base: u32, end: u64, perm: u8) -> Region {
        Region { base, end, perm }
    }

    #[test]
    fn napot_region_is_one_entry_and_deny_all_closes_the_table() {
        let p = plan(&[rg(0x4FF7_0000, 0x4FF7_1000, R | W)], G).unwrap();
        assert_eq!(p.used, 1);
        assert_eq!(p.entries[0], Entry { cfg: NAPOT | R | W, addr: (0x4FF7_0000 | 0x7FF) >> 2 });
        assert_eq!(p.entries[DENY_ALL_INDEX], Entry::DENY_ALL);
        assert!(p.entries[1..WINDOW_INDEX].iter().all(|e| *e == Entry::OFF));
        assert_eq!(p.entries[WINDOW_INDEX], Entry::WINDOW);
    }

    #[test]
    fn minimum_napot_is_the_granule() {
        let p = plan(&[rg(0x1000, 0x1080, R)], G).unwrap();
        assert_eq!(p.used, 1);
        assert_eq!(readback(p.entries[0], G), p.entries[0]);
    }

    #[test]
    fn tor_pair_for_an_unaligned_span() {
        // 0x1000..0x3000 (8 KiB at a 4 KiB boundary) is not NAPOT: OFF base + TOR top.
        let p = plan(&[rg(0x1000, 0x3000, R | X)], G).unwrap();
        assert_eq!(p.used, 2);
        assert_eq!(p.entries[0], Entry { cfg: 0, addr: 0x1000 >> 2 });
        assert_eq!(p.entries[1], Entry { cfg: TOR | R | X, addr: 0x3000 >> 2 });
    }

    #[test]
    fn tor_at_address_zero_in_entry_zero_needs_no_base() {
        let p = plan(&[rg(0, 0x3000, R)], G).unwrap();
        assert_eq!(p.used, 1);
        assert_eq!(p.entries[0].cfg, TOR | R);
        assert_eq!(u_perms(&p.entries, 0), R);
        assert_eq!(u_perms(&p.entries, 0x2FFF), R);
        assert_eq!(u_perms(&p.entries, 0x3000), 0);
    }

    #[test]
    fn tor_base_after_a_napot_entry_is_explicit() {
        // The NAPOT entry leaves its own address in pmpaddr[0]; the TOR region cannot borrow it as a base.
        let p = plan(&[rg(0x1000, 0x2000, R), rg(0x3000, 0x5000, R | W)], G).unwrap();
        assert_eq!(p.used, 3);
        assert_eq!(p.entries[1], Entry { cfg: 0, addr: 0x3000 >> 2 });
        assert_eq!(p.entries[2].cfg, TOR | R | W);
    }

    #[test]
    fn touching_tor_regions_share_the_boundary_entry() {
        let p = plan(&[rg(0x1000, 0x3000, R), rg(0x3000, 0x6000, R | W)], G).unwrap();
        // base holder, TOR top 0x3000, TOR top 0x6000.
        assert_eq!(p.used, 3);
        assert_eq!(p.entries[1].addr, 0x3000 >> 2);
        assert_eq!(p.entries[2].addr, 0x6000 >> 2);
        assert_eq!(u_perms(&p.entries, 0x2FFF), R);
        assert_eq!(u_perms(&p.entries, 0x3000), R | W);
        assert_eq!(u_perms(&p.entries, 0x5FFF), R | W);
        assert_eq!(u_perms(&p.entries, 0x6000), 0);
    }

    #[test]
    fn touching_regions_of_equal_permission_merge() {
        let p = plan(&[rg(0x3000, 0x6000, R), rg(0x1000, 0x3000, R)], G).unwrap();
        assert_eq!(p.used, 2);
        assert_eq!(u_perms(&p.entries, 0x2FFF), R);
        assert_eq!(u_perms(&p.entries, 0x3000), R);
        assert_eq!(u_perms(&p.entries, 0x6000), 0);
    }

    #[test]
    fn top_of_the_address_space() {
        // Aligned 4 GiB and the top half are NAPOT; an odd span reaching 4 GiB is not expressible as a TOR top.
        assert_eq!(plan(&[rg(0, SPACE, R)], G), Err(PlanError::CoreLocal)); // covers the window
        let top = plan(&[rg(0x8000_0000, SPACE, R)], G).unwrap();
        assert_eq!(u_perms(&top.entries, 0xFFFF_FFFF), R);
        assert_eq!(u_perms(&top.entries, 0x7FFF_FFFF), 0);
        assert_eq!(plan(&[rg(0xFFFF_F000, SPACE, R)], G).unwrap().used, 1); // 4 KiB at the top is aligned NAPOT
        assert_eq!(plan(&[rg(0xFFFF_D000, SPACE, R)], G), Err(PlanError::TopOfSpace)); // 8 KiB: TOR
                                                                                       // Just below the top: a TOR top of 0xFFFF_FF80 is the highest the hardware holds.
        assert!(plan(&[rg(0xFFFF_E000, 0xFFFF_FF80, R)], G).is_ok());
    }

    #[test]
    fn every_plan_carries_the_r_only_core_local_window() {
        let p = plan(&[rg(0x1000, 0x2000, R)], G).unwrap();
        assert!(view_ok(&p.entries));
        for a in [0x2000_0000u32, 0x2000_4000, 0x2081_0000, 0x20B0_0000, 0x2FFF_FFFF] {
            assert_eq!(u_perms(&p.entries, a), R, "{a:#x}");
        }
        for a in [0x1FFF_FFFFu32, 0x3000_0000, 0x3010_0000] {
            assert_eq!(u_perms(&p.entries, a), 0, "{a:#x}");
        }
        // The fixed entry is a real, decodable 256 MiB NAPOT, never locked.
        assert_eq!(Entry::WINDOW.cfg & LOCK, 0);
        assert_eq!(readback(Entry::WINDOW, G), Entry::WINDOW);
        let mut e = p.entries;
        e[WINDOW_INDEX] = Entry::OFF;
        assert!(!view_ok(&e));
        e[WINDOW_INDEX] = Entry::WINDOW;
        e[DENY_ALL_INDEX] = Entry::OFF;
        assert!(!view_ok(&e));
    }

    #[test]
    fn view_ok_refuses_an_earlier_entry_that_touches_the_window() {
        let base = *plan(&[rg(0x1000, 0x2000, R)], G).unwrap().entries();
        assert!(view_ok(&base));
        // A perm-0 NAPOT over the window in front of the fixed entry brings back the hang.
        let mut e = base;
        e[3] = Entry { cfg: NAPOT, addr: 0x09FF_FFFF };
        assert!(!view_ok(&e));
        // A writable TOR pair that straddles the lower edge.
        let mut e = base;
        e[3] = Entry { cfg: 0, addr: 0x1FFF_F000 >> 2 };
        e[4] = Entry { cfg: TOR | R | W, addr: 0x2000_1000 >> 2 };
        assert!(!view_ok(&e));
        // An empty TOR, or entries elsewhere, are fine.
        let mut e = base;
        e[3] = Entry { cfg: 0, addr: 0x5000_0000 >> 2 };
        e[4] = Entry { cfg: TOR | R, addr: 0x4000_0000 >> 2 };
        e[5] = Entry { cfg: NAPOT | R, addr: (0x4800_0000u32 | 0x7FF) >> 2 };
        assert!(view_ok(&e));
    }

    #[test]
    fn regions_touching_the_window_are_refused() {
        assert_eq!(plan(&[rg(0x2000_0000, 0x2000_1000, R)], G), Err(PlanError::CoreLocal));
        assert_eq!(plan(&[rg(0x1FFF_F000, 0x2000_0080, R)], G), Err(PlanError::CoreLocal));
        assert_eq!(plan(&[rg(0x2FFF_F000, 0x3000_0000, R | W)], G), Err(PlanError::CoreLocal));
        assert!(plan(&[rg(0x3000_0000, 0x3000_1000, R)], G).is_ok());
        assert!(plan(&[rg(0x1FFF_F000, 0x2000_0000, R)], G).is_ok());
    }

    #[test]
    fn empty_and_backwards_regions_are_errors() {
        assert_eq!(plan(&[rg(0x1000, 0x1000, R)], G), Err(PlanError::BadRange));
        assert_eq!(plan(&[rg(0x2000, 0x1000, R)], G), Err(PlanError::BadRange));
        assert_eq!(plan(&[rg(0x1000, SPACE + 128, R)], G), Err(PlanError::BadRange));
    }

    #[test]
    fn granularity_is_enforced() {
        assert_eq!(plan(&[rg(0x1040, 0x2000, R)], G), Err(PlanError::Unaligned));
        assert_eq!(plan(&[rg(0x1000, 0x2040, R)], G), Err(PlanError::Unaligned));
        // The same region is fine at a finer granularity (qemu-virt).
        assert!(plan(&[rg(0x1040, 0x2000, R)], 4).is_ok());
    }

    #[test]
    fn w_xor_x_and_reserved_permissions() {
        assert_eq!(plan(&[rg(0x1000, 0x2000, W | X | R)], G), Err(PlanError::WriteExec));
        assert_eq!(plan(&[rg(0x1000, 0x2000, W | X)], G), Err(PlanError::WriteExec));
        assert_eq!(plan(&[rg(0x1000, 0x2000, W)], G), Err(PlanError::WriteOnly));
        assert_eq!(plan(&[rg(0x1000, 0x2000, 0)], G), Err(PlanError::BadPerm));
        assert_eq!(plan(&[rg(0x1000, 0x2000, 8)], G), Err(PlanError::BadPerm));
        assert!(plan(&[rg(0x1000, 0x2000, R | X)], G).is_ok());
    }

    #[test]
    fn overlaps_are_rejected_even_with_equal_permissions() {
        assert_eq!(plan(&[rg(0x1000, 0x3000, R), rg(0x2000, 0x4000, R)], G), Err(PlanError::Overlap));
        assert_eq!(plan(&[rg(0x1000, 0x5000, R), rg(0x2000, 0x3000, R | W)], G), Err(PlanError::Overlap));
    }

    #[test]
    fn budget_is_eight_dynamic_entries() {
        // Eight NAPOT regions fit exactly.
        let eight: [Region; 8] =
            core::array::from_fn(|i| rg(0x10_0000 + i as u32 * 0x2000, 0x10_1000 + i as u64 * 0x2000, R));
        let p = plan(&eight, G).unwrap();
        assert_eq!(p.used, 8);
        assert_eq!(p.entries[DENY_ALL_INDEX], Entry::DENY_ALL);
        // Nine regions, or eight where one needs a TOR pair, do not.
        let nine = [rg(0x1000, 0x2000, R); 9];
        assert_eq!(plan(&nine, G), Err(PlanError::Budget));
        let mut tor8 = eight;
        tor8[7] = rg(0x30_0000, 0x30_3000, R); // 12 KiB: TOR pair
        assert_eq!(plan(&tor8, G), Err(PlanError::Budget));
    }

    #[test]
    fn nothing_is_ever_locked_and_plans_read_back_unchanged() {
        let p = plan(&[rg(0x1000, 0x3000, R | X), rg(0x4FF7_0000, 0x4FF7_1000, R | W)], G).unwrap();
        for e in p.entries {
            assert_eq!(e.cfg & LOCK, 0);
            assert_eq!(readback(e, G), e);
        }
    }

    #[test]
    fn readback_matches_the_rom_dump() {
        // pmpaddr0 all ones reads back 0x3fffffe0 on the P4 (OFF: low G-1 bits zero, upper bits unimplemented).
        assert_eq!(discover(&[0; 16], 0x3FFF_FFE0).gran, Some(128));
        assert_eq!(discover(&[0; 16], 0xFFFF_FFFF).gran, Some(4));
        assert_eq!(discover(&[0; 16], 0).gran, None);
    }

    #[test]
    fn rom_state_has_sixteen_free_entries() {
        let d = discover(&[0; 16], 0x3FFF_FFE0);
        assert_eq!((d.free(), d.locked_mask, d.layout_ok()), (16, 0, true));
        let mut cfg = [0u8; 16];
        cfg[WINDOW_INDEX] = NAPOT | R;
        assert!(!discover(&cfg, 0x3FFF_FFE0).layout_ok());
    }

    #[test]
    fn locked_or_used_entries_are_not_free() {
        let mut cfg = [0u8; 16];
        cfg[2] = LOCK | NAPOT | R; // locked
        cfg[9] = TOR | R; // used, unlocked
        cfg[15] = LOCK; // locked OFF (the bootloader's entry 15 shape)
        let d = discover(&cfg, 0x3FFF_FFE0);
        assert_eq!(d.free(), 13);
        assert_eq!(d.locked_mask, 1 << 2 | 1 << 15);
        assert!(!d.layout_ok());
        cfg[2] = 0;
        cfg[15] = 0;
        assert!(discover(&cfg, 0x3FFF_FFE0).layout_ok()); // entry 9 is outside the planner's layout
    }

    #[test]
    fn reload_reuses_what_already_matches() {
        let a = plan(&[rg(0x1000, 0x2000, R | X), rg(0x4FF7_0000, 0x4FF7_1000, R | W)], G).unwrap();
        let b = plan(&[rg(0x1000, 0x2000, R | X), rg(0x4FF8_0000, 0x4FF8_1000, R | W)], G).unwrap();
        let writes = (0..ENTRIES).filter(|&i| needs_write(&a.entries[i], &b.entries, i) != (false, false)).count();
        assert_eq!(writes, 1); // only the data entry's address
        let same = (0..ENTRIES).filter(|&i| needs_write(&a.entries[i], &a.entries, i) != (false, false)).count();
        assert_eq!(same, 0);
    }

    #[test]
    fn off_entry_address_is_ignored_unless_it_is_a_tor_base() {
        let mut cur = [Entry::OFF; ENTRIES];
        let want = plan(&[rg(0x1000, 0x2000, R)], G).unwrap();
        cur[5] = Entry { cfg: 0, addr: 0x1234 }; // stale address in an unused OFF entry
        assert_eq!(needs_write(&cur[5], &want.entries, 5), (false, false));
        let tor = plan(&[rg(0x1000, 0x3000, R)], G).unwrap();
        assert_eq!(needs_write(&Entry::OFF, &tor.entries, 0), (false, true)); // base holder: address matters
    }

    // ---- proptest: a byte is granted iff it lies in a region, with the right permissions ----

    const PERMS: [u8; 6] = [R, R | W, R | X, X, R | W | X & !X, R]; // never W+X

    fn arb_region() -> impl Strategy<Value = Region> {
        // Mix NAPOT-shaped (power-of-two, aligned) and TOR-shaped (any multiple of 128) regions near interesting
        // addresses, including the very bottom and top of the space.
        let anchors =
            prop_oneof![Just(0u32), Just(0x4FF7_0000u32), Just(0x8000_0000u32), Just(0xFFFF_0000u32), any::<u32>()];
        (anchors, 1u64..64, 0usize..PERMS.len(), any::<bool>(), 7u32..20).prop_map(|(a, units, pi, pow2, sh)| {
            let (base, len) = if pow2 {
                let len = 1u64 << sh;
                ((a as u64 & !(len - 1)) as u32, len)
            } else {
                (a & !(G - 1), units * G as u64)
            };
            let end = (base as u64 + len).min(SPACE);
            Region { base, end, perm: PERMS[pi] }
        })
    }

    fn probe_addrs(rs: &[Region], extra: &[u32]) -> Vec<u32> {
        let mut v: Vec<u32> = vec![
            0,
            1,
            0x7F,
            0x80,
            0xFFFF_FFFF,
            0xFFFF_FF80,
            0xFFFF_FF7F, // fixed edges: the window and its neighbours
            0x1FFF_FFFF,
            0x1FFF_FF80,
            0x2000_0000,
            0x2000_007F,
            0x2FFF_FFFF,
            0x2FFF_FF80,
            0x3000_0000,
            0x3000_007F,
        ];
        for r in rs {
            for d in [
                r.base.wrapping_sub(1),
                r.base,
                r.base + 1,
                r.base.wrapping_add(G),
                (r.end - 1) as u32,
                (r.end - 2) as u32,
            ] {
                v.push(d);
            }
            v.push(r.end as u32); // wraps to 0 for end = 4 GiB: still a valid probe
            v.push((r.end as u32).wrapping_sub(G).wrapping_add(1));
        }
        v.extend_from_slice(extra);
        v
    }

    /// Independent model of the planner's rules: Ok(entries used) or the error class.
    fn model(rs: &[Region]) -> Result<usize, PlanError> {
        if rs.len() > DYN_BUDGET {
            return Err(PlanError::Budget);
        }
        if rs.iter().any(|r| (r.base as u64) < WINDOW_END && r.end > WINDOW_BASE) {
            return Err(PlanError::CoreLocal);
        }
        let mut v = rs.to_vec();
        v.sort_by_key(|r| r.base);
        if v.windows(2).any(|w| (w[1].base as u64) < w[0].end) {
            return Err(PlanError::Overlap);
        }
        let mut merged: Vec<Region> = Vec::new();
        for r in v {
            match merged.last_mut() {
                Some(l) if l.end == r.base as u64 && l.perm == r.perm => l.end = r.end,
                _ => merged.push(r),
            }
        }
        let (mut used, mut tor_base) = (0usize, None::<u64>);
        for r in merged {
            let len = r.end - r.base as u64;
            if len.is_power_of_two() && len >= G as u64 && (r.base as u64).is_multiple_of(len) {
                used += 1;
                tor_base = None;
            } else {
                if r.end > 0xFFFF_FF80 {
                    return Err(PlanError::TopOfSpace);
                }
                let shared = tor_base == Some(r.base as u64) || (used == 0 && r.base == 0);
                used += 1 + usize::from(!shared);
                tor_base = Some(r.end);
            }
            if used > DYN_BUDGET {
                return Err(PlanError::Budget);
            }
        }
        Ok(used)
    }

    fn oracle(rs: &[Region], a: u32) -> u8 {
        if (WINDOW_BASE..WINDOW_END).contains(&(a as u64)) {
            return R;
        }
        rs.iter().filter(|r| (a as u64) >= r.base as u64 && (a as u64) < r.end).fold(0, |p, r| p | r.perm)
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(4000))]

        #[test]
        fn a_byte_is_granted_iff_it_lies_in_a_region(
            rs in proptest::collection::vec(arb_region(), 1..6),
            extra in proptest::collection::vec(any::<u32>(), 0..24),
        ) {
            match plan(&rs, G) {
                Ok(p) => {
                    prop_assert_eq!(model(&rs), Ok(p.used));
                    prop_assert!(p.used <= DYN_BUDGET);
                    for e in p.entries {
                        prop_assert_eq!(e.cfg & LOCK, 0);
                        prop_assert_eq!(readback(e, G), e);
                    }
                    for a in probe_addrs(&rs, &extra) {
                        prop_assert_eq!(u_perms(&p.entries, a), oracle(&rs, a), "addr {:#x}", a);
                    }
                    // W^X holds for every byte.
                    for a in probe_addrs(&rs, &extra) {
                        let g = u_perms(&p.entries, a);
                        prop_assert!(!(g & W != 0 && g & X != 0));
                    }
                }
                Err(e) => {
                    // The exact error class, from an independent model of the rules.
                    prop_assert_eq!(model(&rs), Err(e));
                }
            }
        }

        #[test]
        fn disjoint_regions_always_plan_unless_over_budget_or_at_the_top(
            n in 1usize..5, gap in 0u64..4, seed in any::<u32>(),
        ) {
            // n TOR-shaped regions laid out in order with `gap` granules between them, away from the top.
            let mut rs = Vec::new();
            let mut at = (seed & 0x0FFF_FF80) as u64;
            for i in 0..n {
                let len = (3 + i as u64) * G as u64;
                rs.push(rg(at as u32, at + len, if i % 2 == 0 { R | X } else { R | W }));
                at += len + gap * G as u64;
            }
            let p = plan(&rs, G).unwrap();
            for a in probe_addrs(&rs, &[]) {
                prop_assert_eq!(u_perms(&p.entries, a), oracle(&rs, a));
            }
        }

        #[test]
        fn reload_diff_reaches_the_wanted_state(
            a in proptest::collection::vec(arb_region(), 1..4),
            b in proptest::collection::vec(arb_region(), 1..4),
        ) {
            if let (Ok(pa), Ok(pb)) = (plan(&a, G), plan(&b, G)) {
                // Apply only the entries the diff asks for on top of A; U's view must equal B's.
                let mut cur = pa.entries;
                for (i, c) in cur.iter_mut().enumerate() {
                    let (cfg, addr) = needs_write(c, &pb.entries, i);
                    if cfg { c.cfg = pb.entries[i].cfg; }
                    if addr { c.addr = pb.entries[i].addr; }
                }
                for x in probe_addrs(&b, &[]) {
                    prop_assert_eq!(u_perms(&cur, x), u_perms(&pb.entries, x), "addr {:#x}", x);
                }
            }
        }
    }
}
