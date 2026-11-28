//! Per-process PMP views (#617): a plan checked once when the process is loaded, and the register diff a context
//! switch writes. Pure logic; the kernel's `pmp::switch` only turns a `Diff` into CSR writes and readbacks.
//!
//! #612 measured a full reload (`pmp::load`: `view_ok` over all 16 entries, every register compared against the
//! hardware, a readback of all 16) at 96 us median at the 40 MHz ROM clock. A switch between two app views differs in
//! two addresses, so the switch path checks the view once at load (`View::new`), diffs against the shadow in memory,
//! and reads back only what it wrote.
use crate::pmp::{needs_write, readback, view_ok, Entry, Plan, ENTRIES};

/// A plan that passed `view_ok` at granularity `gran`. Only `new` builds one, so a View is always a safe U view (#646:
/// the fixed window and deny-all entries, nothing earlier touching the window).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct View {
    plan: Plan,
    gran: u32,
}
impl View {
    pub fn new(plan: Plan, gran: u32) -> Option<View> {
        view_ok(plan.entries(), gran).then_some(View { plan, gran })
    }
    pub fn entries(&self) -> &[Entry; ENTRIES] {
        self.plan.entries()
    }
    pub fn plan(&self) -> &Plan {
        &self.plan
    }
    /// The granularity the view was checked at: a hart with another granularity must not load it.
    pub fn gran(&self) -> u32 {
        self.gran
    }
}

/// The registers one switch writes: `addr` bit i = pmpaddr i, `cfg[n]` = Some(word) for pmpcfg n.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Diff {
    pub addr: u16,
    pub cfg: [Option<u32>; 4],
}
impl Diff {
    /// Register writes this diff needs.
    pub fn writes(&self) -> u32 {
        self.addr.count_ones() + self.cfg.iter().filter(|c| c.is_some()).count() as u32
    }
}

/// The four pmpcfg words of `entries`.
pub fn cfg_words(entries: &[Entry; ENTRIES]) -> [u32; 4] {
    core::array::from_fn(|n| (0..4).fold(0u32, |w, b| w | (entries[4 * n + b].cfg as u32) << (8 * b)))
}

/// What must be written to turn `shadow` (what the hardware holds) into `want`. A cfg word is written whole when any
/// of its four bytes differs, so entries a smaller view no longer uses are switched OFF in the same write; their
/// addresses are left alone unless they are a TOR base (`needs_write`).
pub fn diff(shadow: &[Entry; ENTRIES], want: &View) -> Diff {
    let w = want.entries();
    let mut d = Diff::default();
    let mut cfg_dirty = 0u8;
    for i in 0..ENTRIES {
        // Most entries match exactly between two app views: one compare, then the next.
        if shadow[i] == w[i] {
            continue;
        }
        if needs_write(&shadow[i], w, i).1 {
            d.addr |= 1 << i;
        }
        if shadow[i].cfg != w[i].cfg {
            cfg_dirty |= 1 << (i / 4);
        }
    }
    if cfg_dirty != 0 {
        let words = cfg_words(w);
        for (n, c) in d.cfg.iter_mut().enumerate() {
            if cfg_dirty & (1 << n) != 0 {
                *c = Some(words[n]);
            }
        }
    }
    d
}

/// Does `shadow` hold `want`, as far as the hardware's matching goes? (Every register `diff` would write matches.)
pub fn holds(shadow: &[Entry; ENTRIES], want: &View) -> bool {
    diff(shadow, want) == Diff::default()
}

/// What the readback of pmpaddr i must show after writing `want`'s address there.
pub fn addr_readback(want: &View, i: usize) -> u32 {
    readback(want.entries()[i], want.gran).addr
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pmp::{plan, Region, DENY_ALL_INDEX, GRAN_P4, R, W, WINDOW_INDEX, X};
    use crate::slots;

    fn app_view(i: usize) -> View {
        let s = slots::slot(i).unwrap();
        let r = |base: usize, perm| Region { base: base as u32, end: (base + s.len) as u64, perm };
        View::new(plan(&[r(s.text, R | X), r(s.data, R | W)], GRAN_P4).unwrap(), GRAN_P4).unwrap()
    }

    fn apply(shadow: &mut [Entry; ENTRIES], d: &Diff, want: &View) {
        for (i, (sh, w)) in shadow.iter_mut().zip(want.entries()).enumerate() {
            if d.addr & (1 << i) != 0 {
                sh.addr = w.addr;
            }
            if d.cfg[i / 4].is_some() {
                sh.cfg = w.cfg;
            }
        }
    }

    #[test]
    fn switching_between_two_app_views_writes_two_addresses_and_no_cfg() {
        let (a, b) = (app_view(0), app_view(1));
        let mut sh = *a.entries();
        let d = diff(&sh, &b);
        assert_eq!(d, Diff { addr: 0b11, cfg: [None; 4] });
        assert_eq!(d.writes(), 2);
        apply(&mut sh, &d, &b);
        assert!(holds(&sh, &b));
        assert!(!holds(&sh, &a));
    }

    #[test]
    fn the_same_view_again_writes_nothing() {
        let a = app_view(0);
        assert_eq!(diff(a.entries(), &a).writes(), 0);
        assert!(holds(a.entries(), &a));
    }

    #[test]
    fn a_smaller_view_switches_the_stale_entries_off() {
        let big = app_view(0);
        let text = slots::slot(0).unwrap().text;
        let small = View::new(
            plan(&[Region { base: text as u32, end: text as u64 + 0x1000, perm: R | X }], GRAN_P4).unwrap(),
            GRAN_P4,
        )
        .unwrap();
        let mut sh = *big.entries();
        let d = diff(&sh, &small);
        // Entry 1 (big's data, R+W) must go OFF: cfg word 0 is rewritten; its stale address does not matter.
        assert_eq!(d.cfg[0], Some(cfg_words(small.entries())[0]));
        apply(&mut sh, &d, &small);
        assert!(holds(&sh, &small));
        assert_eq!(sh[1].cfg, 0);
    }

    #[test]
    fn from_the_rom_state_every_used_register_is_written() {
        let a = app_view(1);
        let d = diff(&[Entry::OFF; ENTRIES], &a);
        assert_eq!(d.addr, 0b11 | 1 << WINDOW_INDEX | 1 << DENY_ALL_INDEX);
        assert!(d.cfg[0].is_some() && d.cfg[3].is_some() && d.cfg[1].is_none() && d.cfg[2].is_none());
    }

    #[test]
    fn a_view_is_only_built_from_a_safe_plan() {
        let a = app_view(0);
        assert_eq!(a.gran(), GRAN_P4);
        // The same plan at a granularity whose NAPOT readback spills over the window is refused (review L-c).
        let just_below = plan(&[Region { base: 0x1FFF_FFF8, end: 0x2000_0000, perm: R }], 4).unwrap();
        assert!(View::new(just_below, 4).is_some());
        assert!(View::new(just_below, 1 << 30).is_none());
    }

    #[test]
    fn addr_readback_applies_the_granularity() {
        let a = app_view(0);
        for i in 0..ENTRIES {
            assert_eq!(addr_readback(&a, i), readback(a.entries()[i], GRAN_P4).addr);
        }
    }
}
