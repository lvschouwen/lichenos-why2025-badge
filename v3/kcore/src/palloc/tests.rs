extern crate std;
use super::*;
use proptest::prelude::*;
use std::collections::{HashMap, HashSet};
use std::vec::Vec;

/// The chip as the allocator sees it: one byte of content per page. A page never written reads `BOOT` (load-ram
/// leaves old data). `fail` makes the next zeroing report a failure without zeroing.
struct Mem {
    pages: HashMap<usize, u8>,
    fail: bool,
    zeroed: Vec<(usize, usize)>,
}
const BOOT: u8 = 0xA5;
impl Mem {
    fn new() -> Mem {
        Mem { pages: HashMap::new(), fail: false, zeroed: Vec::new() }
    }
    fn read(&self, off: usize) -> u8 {
        *self.pages.get(&(off / PAGE)).unwrap_or(&BOOT)
    }
    fn write(&mut self, off: usize, len: usize, v: u8) {
        for p in off / PAGE..(off + len) / PAGE {
            self.pages.insert(p, v);
        }
    }
    fn all(&self, b: &Block, v: u8) -> bool {
        (b.off..b.off + b.len).step_by(PAGE).all(|o| self.read(o) == v)
    }
}
impl Scrub for Mem {
    fn zero(&mut self, off: usize, len: usize) -> bool {
        if std::mem::take(&mut self.fail) {
            return false;
        }
        self.zeroed.push((off, len));
        self.write(off, len, 0);
        true
    }
}

const EXEC: (usize, usize) = (0x10_0000, 64 * PAGE);
const DATA: (usize, usize) = (0x20_0000, 256 * PAGE);

fn fresh(quota: usize) -> Palloc {
    let mut a = Palloc::EMPTY;
    a.configure(EXEC, DATA).unwrap();
    for o in 0..OWNERS {
        a.set_quota(o, quota).unwrap();
    }
    a
}

fn arena_of(k: Kind) -> (usize, usize) {
    if k == Kind::Exec {
        EXEC
    } else {
        DATA
    }
}

/// Is there any size-aligned run of `len` bytes in `kind`'s arena that keeps GUARD from every live block? (The
/// brute-force answer `alloc`'s NoSpace must agree with.)
fn fits(blocks: &[Block], kind: Kind, len: usize) -> bool {
    let (base, size) = arena_of(kind);
    (base.next_multiple_of(len)..base + size).step_by(len).take_while(|o| o + len <= base + size).any(|o| {
        blocks.iter().filter(|b| b.kind == kind).all(|b| o + len + GUARD <= b.off || b.off + b.len + GUARD <= o)
    })
}

fn check_invariants(a: &Palloc, quota: &[usize; OWNERS]) {
    let live: Vec<Block> = a.blocks().collect();
    for (i, b) in live.iter().enumerate() {
        let (base, size) = arena_of(b.kind);
        assert!(b.len.is_power_of_two() && b.len >= PAGE && b.off % b.len == 0, "{b:x?} not NAPOT-shaped");
        assert!(b.off >= base && b.off + b.len <= base + size, "{b:x?} outside its arena");
        assert!(!overlaps(b.off, b.len, HOLE_START, HOLE_END - HOLE_START));
        let r = u_region(b);
        assert!(u_region_ok(&r), "{r:x?}");
        assert_eq!(r.perm, if b.kind == Kind::Exec { R | X } else { R | W });
        for c in &live[i + 1..] {
            assert!(
                b.off + b.len + GUARD <= c.off || c.off + c.len + GUARD <= b.off,
                "{b:x?} and {c:x?} closer than GUARD"
            );
        }
    }
    for (o, q) in quota.iter().enumerate() {
        let sum: usize = live.iter().filter(|b| b.owner == o).map(|b| b.len).sum();
        assert_eq!(a.usage(o), sum);
        assert!(sum <= *q, "owner {o} holds {sum} over its quota {q}");
    }
}

#[derive(Clone, Debug)]
enum Op {
    Alloc {
        owner: usize,
        exec: bool,
        pages_log2: u32,
    },
    /// Free issued handle number `pick` (modulo how many exist) as owner `as_owner`, or as its real owner.
    Free {
        pick: usize,
        as_owner: Option<usize>,
    },
    Release {
        owner: usize,
    },
    FailNextZero,
}

fn op() -> impl Strategy<Value = Op> {
    prop_oneof![
        4 => (0..OWNERS + 1, any::<bool>(), 0u32..6).prop_map(|(owner, exec, pages_log2)| Op::Alloc { owner, exec, pages_log2 }),
        3 => (any::<usize>(), proptest::option::weighted(0.2, 0..OWNERS)).prop_map(|(pick, as_owner)| Op::Free { pick, as_owner }),
        1 => (0..OWNERS).prop_map(|owner| Op::Release { owner }),
        1 => Just(Op::FailNextZero),
    ]
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(400))]

    /// Any sequence of allocs, frees (right owner, wrong owner, stale handles), releases and failing zeroings keeps
    /// every rule of the module doc, and every refusal is the right one.
    #[test]
    fn every_sequence_keeps_the_rules(quotas in proptest::array::uniform8(prop_oneof![Just(0usize), Just(4 * PAGE), Just(16 * PAGE), Just(1 << 20)]),
                                      ops in proptest::collection::vec(op(), 1..80)) {
        let mut a = Palloc::EMPTY;
        a.configure(EXEC, DATA).unwrap();
        for (o, q) in quotas.iter().enumerate() {
            a.set_quota(o, *q).unwrap();
        }
        let mut mem = Mem::new();
        let mut issued: Vec<(Handle, Block, bool)> = Vec::new(); // (handle, block, still live)
        let mut ever: [HashSet<usize>; 2] = [HashSet::new(), HashSet::new()];
        for op in ops {
            match op {
                Op::Alloc { owner, exec, pages_log2 } => {
                    let kind = if exec { Kind::Exec } else { Kind::Data };
                    let len = PAGE << pages_log2;
                    let before: Vec<Block> = a.blocks().collect();
                    let fail = mem.fail;
                    match a.alloc(owner, kind, len, &mut mem) {
                        Ok((h, b)) => {
                            prop_assert!(owner < OWNERS && b.owner == owner && b.kind == kind && b.len == len);
                            prop_assert!(mem.all(&b, 0), "{b:x?} granted with old contents");
                            for p in (b.off..b.off + b.len).step_by(PAGE) {
                                ever[kind as usize].insert(p / PAGE);
                            }
                            // The owner writes all of it.
                            mem.write(b.off, b.len, owner as u8 + 1);
                            issued.push((h, b, true));
                        }
                        Err(e) => {
                            prop_assert_eq!(&a.blocks().collect::<Vec<_>>(), &before, "a refused alloc changed the blocks");
                            let want = if owner >= OWNERS {
                                Error::BadOwner
                            } else if a.usage(owner) + len > quotas[owner] {
                                Error::Quota
                            } else if before.len() == BLOCKS {
                                Error::NoSlot
                            } else if !fits(&before, kind, len) {
                                Error::NoSpace
                            } else {
                                prop_assert!(fail, "Scrub without a failing zeroing");
                                Error::Scrub
                            };
                            prop_assert_eq!(e, want);
                        }
                    }
                }
                Op::Free { pick, as_owner } => {
                    if issued.is_empty() {
                        continue;
                    }
                    let i = pick % issued.len();
                    let (h, b, live) = issued[i];
                    let who = as_owner.unwrap_or(b.owner);
                    let fail = mem.fail;
                    match a.free(who, h, &mut mem) {
                        Ok(zeroed) => {
                            prop_assert!(live && who == b.owner);
                            prop_assert_eq!(zeroed, !fail);
                            if zeroed {
                                prop_assert!(mem.all(&b, 0), "{b:x?} freed but not zero");
                            }
                            issued[i].2 = false;
                        }
                        Err(e) => {
                            prop_assert_eq!(e, Error::BadHandle);
                            prop_assert!(!live || who != b.owner, "a live block's own owner was refused");
                        }
                    }
                }
                Op::Release { owner } => {
                    a.release_owner(owner, &mut mem);
                    prop_assert!(a.blocks().all(|b| b.owner != owner));
                    for e in issued.iter_mut().filter(|e| e.1.owner == owner) {
                        e.2 = false;
                    }
                }
                Op::FailNextZero => mem.fail = true,
            }
            check_invariants(&a, &quotas);
            // A page that was ever executable was never writable, and the reverse.
            prop_assert!(ever[0].is_disjoint(&ever[1]));
        }
    }

    /// U regions from any block are cached-alias only; any region touching the uncached alias or the hole is refused.
    #[test]
    fn u_regions_never_reach_the_uncached_alias(off in 0usize..PSRAM_BYTES, len_log2 in 7u32..22, perm in 1u8..8) {
        let base = (off & !((1usize << len_log2) - 1)) as u64;
        let len = 1u64 << len_log2;
        let unc = Region { base: (UNCACHED_BASE as u64 + base) as u32, end: UNCACHED_BASE as u64 + base + len, perm };
        prop_assert!(!u_region_ok(&unc));
        let cached = Region { base: (CACHED_BASE as u64 + base) as u32, end: CACHED_BASE as u64 + base + len, perm };
        let in_hole = base < HOLE_END as u64 && (HOLE_START as u64) < base + len;
        let past = base + len > PSRAM_BYTES as u64;
        prop_assert_eq!(u_region_ok(&cached), !in_hole && !past && perm & (W | X) != (W | X));
    }
}

#[test]
fn configure_refuses_bad_arenas() {
    let mut a = Palloc::EMPTY;
    assert_eq!(a.alloc(0, Kind::Data, PAGE, &mut Mem::new()), Err(Error::NotReady));
    for (e, d) in [
        ((HOLE_START - PAGE, 2 * PAGE), DATA),    // exec runs into the hole
        (EXEC, (HOLE_END - PAGE, PAGE)),          // data inside the hole
        (EXEC, (EXEC.0 + EXEC.1, PAGE)),          // touching: no guard between
        (EXEC, (EXEC.0 - GUARD, GUARD)),          // touching from below
        ((EXEC.0 + 1, PAGE), DATA),               // unaligned
        (EXEC, (PSRAM_BYTES - PAGE, 2 * PAGE)),   // past the chip
        (EXEC, (0x30_0000, 0)),                   // empty
        ((usize::MAX & !(PAGE - 1), PAGE), DATA), // wraps
    ] {
        assert_eq!(a.configure(e, d), Err(Error::BadArena), "{e:x?} {d:x?}");
    }
    assert_eq!(a.configure(EXEC, (EXEC.0 + EXEC.1 + GUARD, PAGE)), Ok(()));
}

#[test]
fn zeroing_is_lazy_and_only_what_is_granted() {
    let mut a = fresh(1 << 20);
    let mut mem = Mem::new();
    let (_, b) = a.alloc(0, Kind::Data, 4 * PAGE, &mut mem).unwrap();
    assert_eq!(mem.zeroed, [(b.off, b.len)]);
    // Never-granted pages keep their boot contents (nothing zeroed the chip).
    assert_eq!(mem.read(b.off + b.len + GUARD), BOOT);
}

#[test]
fn a_freed_block_reads_zero_and_comes_back_zero_to_the_next_owner() {
    let mut a = fresh(1 << 20);
    let mut mem = Mem::new();
    let (h, b) = a.alloc(1, Kind::Data, 2 * PAGE, &mut mem).unwrap();
    mem.write(b.off, b.len, 0x77);
    assert_eq!(a.free(2, h, &mut mem), Err(Error::BadHandle), "another owner frees it");
    assert_eq!(a.free(1, h, &mut mem), Ok(true));
    assert!(mem.all(&b, 0));
    assert_eq!(a.free(1, h, &mut mem), Err(Error::BadHandle), "stale handle");
    let (_, c) = a.alloc(2, Kind::Data, 2 * PAGE, &mut mem).unwrap();
    assert_eq!(c.off, b.off, "first fit hands the same pages to the next owner");
    assert!(mem.all(&c, 0));
}

#[test]
fn a_failed_zeroing_on_free_is_caught_up_before_the_next_grant() {
    let mut a = fresh(1 << 20);
    let mut mem = Mem::new();
    let (h, b) = a.alloc(1, Kind::Data, PAGE, &mut mem).unwrap();
    mem.write(b.off, b.len, 0x77);
    mem.fail = true;
    assert_eq!(a.free(1, h, &mut mem), Ok(false));
    assert_eq!(mem.read(b.off), 0x77);
    let (_, c) = a.alloc(2, Kind::Data, PAGE, &mut mem).unwrap();
    assert_eq!(c.off, b.off);
    assert!(mem.all(&c, 0));
}

#[test]
fn guards_keep_blocks_apart_and_quota_and_release_work() {
    let mut a = fresh(3 * PAGE);
    let mut mem = Mem::new();
    let (_, b0) = a.alloc(0, Kind::Data, PAGE, &mut mem).unwrap();
    let (_, b1) = a.alloc(0, Kind::Data, PAGE, &mut mem).unwrap();
    assert_eq!(b1.off, b0.off + PAGE + GUARD);
    assert_eq!(a.alloc(0, Kind::Data, 2 * PAGE, &mut mem), Err(Error::Quota));
    assert!(a.release_owner(0, &mut mem));
    assert_eq!(a.usage(0), 0);
    assert!(a.alloc(0, Kind::Data, 2 * PAGE, &mut mem).is_ok());
    assert_eq!(a.alloc(OWNERS, Kind::Data, PAGE, &mut mem), Err(Error::BadOwner));
    assert_eq!(a.alloc(1, Kind::Data, 3 * PAGE, &mut mem), Err(Error::BadSize));
    assert_eq!(a.alloc(1, Kind::Data, PAGE / 2, &mut mem), Err(Error::BadSize));
}

#[test]
fn exec_blocks_come_from_the_exec_arena_with_rx() {
    let mut a = fresh(1 << 20);
    let mut mem = Mem::new();
    let (_, e) = a.alloc(0, Kind::Exec, PAGE, &mut mem).unwrap();
    assert!(e.off >= EXEC.0 && e.off < EXEC.0 + EXEC.1);
    assert_eq!(u_region(&e).perm, R | X);
    assert_eq!(u_region(&e).base as usize, CACHED_BASE + e.off);
}

#[test]
fn the_constants_match_the_p4_map() {
    assert_eq!(UNCACHED_BASE, CACHED_BASE + 0x4000_0000);
    assert_eq!(HOLE_START % PAGE, 0);
    assert_eq!(HOLE_END % PAGE, 0);
    assert!(!u_region_ok(&Region { base: CACHED_BASE as u32, end: (CACHED_BASE + PAGE) as u64, perm: R | W | X }));
}
