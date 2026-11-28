//! PSRAM app memory (#621): a page allocator over physical PSRAM offsets, metadata only. Pure logic; the kernel's
//! `psram` module supplies the zeroing (`Scrub`) and turns a block into a PMP region (`u_region`).
//!
//! Rules, each one a host proptest below:
//! - **Blocks** are a power of two of at least one page and aligned to their size, so a grant is one NAPOT entry
//!   (an app's view has 8 dynamic entries, and text, data, stack and heap already want four).
//! - **X/W split:** an exec arena and a data arena on disjoint physical ranges, fixed at `configure`. A page never
//!   changes arena, so a page an app could once write can never be granted executable to anyone, and the reverse.
//! - **Guards:** no two live blocks are closer than `GUARD` bytes. Guard pages belong to no one and are never granted,
//!   so a run off the end of a block faults in the PMP instead of reaching a neighbour. Arenas are configured at
//!   least `GUARD` apart, so the rule holds across them too.
//! - **Quotas:** bytes per owner; an allocation past the quota is refused, `release_owner` frees all of an owner's.
//! - **Zeroing:** a block is zeroed when it is freed, and any page that was not zeroed since its last use (boot
//!   included: load-ram leaves old PSRAM contents) is zeroed before it is granted. A page is never granted dirty.
//!   Zeroing is lazy: only the pages an allocation takes, never the whole chip at boot.
//! - **Aliases:** the only U address of a block is its cached-alias one (`u_region`). The uncached alias of the same
//!   bytes (0x8800_0000 and up) is never in a U region (`u_region_ok`), and the excluded range (#620) never in an arena.
//!
//! Single context: the kernel owns one `Palloc` and calls it from core 0 only.
use crate::pmp::{Region, R, W, X};

pub const PAGE: usize = 4096;
pub const GUARD: usize = PAGE;
/// The chip (32 MB, platform `map::MAPPED_BYTES`).
pub const PSRAM_BYTES: usize = 32 << 20;
pub const MAX_PAGES: usize = PSRAM_BYTES / PAGE;
/// The cached alias (platform `map::CACHED_BASE`): the only window U may be granted.
pub const CACHED_BASE: usize = 0x4800_0000;
/// The uncached alias of the same bytes (platform `map::CACHED_BASE + UNCACHED_OFFSET`): kernel only.
pub const UNCACHED_BASE: usize = 0x8800_0000;
/// The excluded physical range (#620, platform `map::EXCLUDED_START/END`): unmapped, never in an arena.
pub const HOLE_START: usize = 0x54_0000;
pub const HOLE_END: usize = 0x5C_0000;
/// Owners (process ids) with a quota.
pub const OWNERS: usize = 8;
/// Live blocks at most.
pub const BLOCKS: usize = 32;

const WORDS: usize = MAX_PAGES / 32;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    /// Code: U gets R+X, never W.
    Exec = 0,
    /// Stacks and heaps: U gets R+W, never X.
    Data = 1,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Block {
    /// Physical PSRAM offset.
    pub off: usize,
    pub len: usize,
    pub kind: Kind,
    pub owner: usize,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Error {
    /// `configure`: an arena that is unaligned, empty, outside the chip, in the excluded range, or too close to the other.
    BadArena,
    /// Not configured yet.
    NotReady,
    BadOwner,
    /// Not a power of two of at least one page, or larger than the arena.
    BadSize,
    Quota,
    NoSpace,
    /// The block table is full.
    NoSlot,
    /// The zeroing reported a failure: nothing was granted, the pages stay marked dirty.
    Scrub,
    /// A handle that is stale, never issued, or another owner's.
    BadHandle,
}

/// Zeroes physical PSRAM. `zero` returns false when the bytes may not have reached the memory (a cache sync failed).
/// For `Kind::Exec` it must also drop the range from every instruction cache: stale code of the previous owner must
/// not run in the next owner's block.
pub trait Scrub {
    fn zero(&mut self, off: usize, len: usize, kind: Kind) -> bool;
}

/// A block handle: generation << 8 | index. Stale after `free`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Handle(pub u32);

#[derive(Clone, Copy)]
struct Bits([u32; WORDS]);
impl Bits {
    const ZERO: Bits = Bits([0; WORDS]);
    fn get(&self, p: usize) -> bool {
        self.0[p / 32] & (1 << (p % 32)) != 0
    }
    fn set(&mut self, from: usize, n: usize, on: bool) {
        for p in from..from + n {
            if on {
                self.0[p / 32] |= 1 << (p % 32);
            } else {
                self.0[p / 32] &= !(1 << (p % 32));
            }
        }
    }
    fn any(&self, from: usize, n: usize) -> bool {
        (from..from + n).any(|p| self.get(p))
    }
}

/// One arena: pages `base / PAGE ..` of the chip. `used`: inside a live block. `clean`: zeroed since its last use.
#[derive(Clone, Copy)]
struct Arena {
    base: usize,
    pages: usize,
    used: Bits,
    clean: Bits,
}
impl Arena {
    const EMPTY: Arena = Arena { base: 0, pages: 0, used: Bits::ZERO, clean: Bits::ZERO };

    /// The first size-aligned run of `n` pages with no used page within `GUARD` on either side.
    fn find(&self, n: usize) -> Option<usize> {
        let g = GUARD / PAGE;
        let first = (self.base / PAGE).next_multiple_of(n) - self.base / PAGE;
        (first..self.pages).step_by(n).take_while(|p| p + n <= self.pages).find(|&p| {
            let lo = p.saturating_sub(g);
            !self.used.any(lo, (p + n + g).min(self.pages) - lo)
        })
    }
}

/// `live` instead of an `Option<Block>`: None's niche is a non-zero Kind byte, which would move `EMPTY` out of bss.
#[derive(Clone, Copy)]
struct Slot {
    block: Block,
    live: bool,
    gen: u32,
}
impl Slot {
    fn get(&self) -> Option<Block> {
        self.live.then_some(self.block)
    }
}

pub struct Palloc {
    arenas: [Arena; 2],
    slots: [Slot; BLOCKS],
    quota: [usize; OWNERS],
    usage: [usize; OWNERS],
    ready: bool,
}

const fn overlaps(a: usize, alen: usize, b: usize, blen: usize) -> bool {
    a < b + blen && b < a + alen
}

impl Palloc {
    /// All zero: a `static` of it lands in bss (NOLOAD L2MEM, nothing against the loaded-image limit).
    pub const EMPTY: Palloc = Palloc {
        arenas: [Arena::EMPTY; 2],
        slots: [Slot { block: Block { off: 0, len: 0, kind: Kind::Exec, owner: 0 }, live: false, gen: 0 }; BLOCKS],
        quota: [0; OWNERS],
        usage: [0; OWNERS],
        ready: false,
    };

    /// Set the two arenas (physical `(offset, bytes)`) and drop every block. Every page starts dirty.
    pub fn configure(&mut self, exec: (usize, usize), data: (usize, usize)) -> Result<(), Error> {
        let ok = |(b, l): (usize, usize)| {
            l > 0
                && b % PAGE == 0
                && l % PAGE == 0
                && b.checked_add(l).is_some_and(|e| e <= PSRAM_BYTES)
                && !overlaps(b, l, HOLE_START, HOLE_END - HOLE_START)
        };
        if !ok(exec) || !ok(data) || overlaps(exec.0, exec.1 + GUARD, data.0, data.1 + GUARD) {
            return Err(Error::BadArena);
        }
        *self = Palloc::EMPTY;
        for (a, (b, l)) in self.arenas.iter_mut().zip([exec, data]) {
            (a.base, a.pages) = (b, l / PAGE);
        }
        self.ready = true;
        Ok(())
    }

    pub fn set_quota(&mut self, owner: usize, bytes: usize) -> Result<(), Error> {
        *self.quota.get_mut(owner).ok_or(Error::BadOwner)? = bytes;
        Ok(())
    }

    pub fn usage(&self, owner: usize) -> usize {
        self.usage.get(owner).copied().unwrap_or(0)
    }

    /// A zeroed block of `len` bytes for `owner`. Dirty pages in it are zeroed first; on a zeroing failure nothing is
    /// granted.
    pub fn alloc(&mut self, owner: usize, kind: Kind, len: usize, s: &mut dyn Scrub) -> Result<(Handle, Block), Error> {
        if !self.ready {
            return Err(Error::NotReady);
        }
        if owner >= OWNERS {
            return Err(Error::BadOwner);
        }
        let a = &self.arenas[kind as usize];
        if !len.is_power_of_two() || len < PAGE || len > a.pages * PAGE {
            return Err(Error::BadSize);
        }
        if self.usage[owner] + len > self.quota[owner] {
            return Err(Error::Quota);
        }
        let i = self.slots.iter().position(|s| !s.live).ok_or(Error::NoSlot)?;
        let n = len / PAGE;
        let p = a.find(n).ok_or(Error::NoSpace)?;
        let a = &mut self.arenas[kind as usize];
        let mut q = p;
        while q < p + n {
            if a.clean.get(q) {
                q += 1;
                continue;
            }
            let run = (q..p + n).take_while(|&r| !a.clean.get(r)).count();
            if !s.zero(a.base + q * PAGE, run * PAGE, kind) {
                return Err(Error::Scrub);
            }
            a.clean.set(q, run, true);
            q += run;
        }
        // `clean` of a used page is never read: `drop_slot` sets it from the zeroing's result when the block is freed.
        a.used.set(p, n, true);
        let b = Block { off: a.base + p * PAGE, len, kind, owner };
        self.usage[owner] += len;
        (self.slots[i].block, self.slots[i].live) = (b, true);
        Ok((Handle(self.slots[i].gen << 8 | i as u32), b))
    }

    fn lookup(&self, owner: usize, h: Handle) -> Option<usize> {
        let i = (h.0 & 0xFF) as usize;
        let s = self.slots.get(i)?;
        (s.gen == h.0 >> 8 && s.get().is_some_and(|b| b.owner == owner)).then_some(i)
    }

    /// Free `owner`'s block `h` and zero it. The pages are free either way; if the zeroing failed they stay dirty and
    /// are zeroed before anyone gets them again. Returns whether the zeroing succeeded.
    pub fn free(&mut self, owner: usize, h: Handle, s: &mut dyn Scrub) -> Result<bool, Error> {
        let i = self.lookup(owner, h).ok_or(Error::BadHandle)?;
        Ok(self.drop_slot(i, s))
    }

    fn drop_slot(&mut self, i: usize, s: &mut dyn Scrub) -> bool {
        let Some(b) = self.slots[i].get() else { return true };
        self.slots[i].live = false;
        self.slots[i].gen = (self.slots[i].gen + 1) & 0xFF_FFFF;
        self.usage[b.owner] -= b.len;
        let a = &mut self.arenas[b.kind as usize];
        let (p, n) = ((b.off - a.base) / PAGE, b.len / PAGE);
        a.used.set(p, n, false);
        let zeroed = s.zero(b.off, b.len, b.kind);
        a.clean.set(p, n, zeroed);
        zeroed
    }

    /// Free every block of `owner` (its process ended). Returns whether all zeroing succeeded.
    pub fn release_owner(&mut self, owner: usize, s: &mut dyn Scrub) -> bool {
        let mut ok = true;
        for i in 0..BLOCKS {
            if self.slots[i].get().is_some_and(|b| b.owner == owner) {
                ok &= self.drop_slot(i, s);
            }
        }
        ok
    }

    /// The live blocks.
    pub fn blocks(&self) -> impl Iterator<Item = Block> + '_ {
        self.slots.iter().filter_map(Slot::get)
    }
}

/// The U region of a block: its cached-alias address, R+X for code, R+W for data. Never the uncached alias.
pub fn u_region(b: &Block) -> Region {
    let base = CACHED_BASE + b.off;
    Region { base: base as u32, end: (base + b.len) as u64, perm: if b.kind == Kind::Exec { R | X } else { R | W } }
}

/// May a U view carry `r` as PSRAM memory? Only inside the cached alias, outside the excluded range, and W^X; a
/// region touching the uncached alias never.
pub fn u_region_ok(r: &Region) -> bool {
    let (b, e) = (r.base as u64, r.end);
    let cached = (CACHED_BASE as u64, (CACHED_BASE + PSRAM_BYTES) as u64);
    let hole = ((CACHED_BASE + HOLE_START) as u64, (CACHED_BASE + HOLE_END) as u64);
    let uncached = (UNCACHED_BASE as u64, (UNCACHED_BASE + PSRAM_BYTES) as u64);
    b < e
        && b >= cached.0
        && e <= cached.1
        && !(b < hole.1 && hole.0 < e)
        && !(b < uncached.1 && uncached.0 < e)
        && r.perm & (W | X) != (W | X)
}

#[cfg(test)]
mod tests;
