//! PSRAM in the kernel (#621): bring-up, the kernel-only MMU map, and app memory from `lichen_kcore::palloc`.
//!
//! - **Clock:** the kernel stays at the ROM clock (CPU on the 40 MHz XTAL) with PSRAM at 120 MHz. `cpu_to_360` would
//!   make every cached rate stale and lands with #655's `clock_changed`, kept separate. The bring-up of #620 passed
//!   walking bits, random fills and retention at CPU 40 MHz, and the `psram.zero` row shows cached write-back reaching
//!   the chip at this clock; the bit-8 flips #620 found are uncached single-word stores, which this module never does.
//! - **MMU:** `map_identity` puts physical page n at virtual page n of the window, once: a physical page has exactly
//!   one cached address and the hardware's uncached alias of it. The MMU registers are SPIMEM2 MMIO, which no U view
//!   grants. The excluded range (#620) stays unmapped.
//! - **U access:** only through `grant`, which turns a block into its cached-alias region and refuses anything
//!   `palloc::u_region_ok` refuses (the uncached alias, the hole, W+X).
//! - **Metadata:** the allocator is a zero-initialised static, so it lives in bss in L2MEM. Nothing in-band in PSRAM.
//! - **Single context:** core 0 only, outside traps (the bring-up and, later, the loader).
use crate::global::Global;
use crate::kprintln;
use crate::platform::hw;
use hw::psram::{self, map};
use lichen_kcore::palloc::{self, Block, Error, Handle, Kind, Palloc, Scrub};
use lichen_kcore::pmp::Region;

const PAGES: usize = map::MAPPED_BYTES / map::PAGE_BYTES;
/// Physical arenas: code below the excluded range, stacks and heaps above it. 0x40_0000..0x54_0000 stays out of both
/// (the kernel's own buffers later, and a gap far wider than a guard).
pub const EXEC_ARENA: (usize, usize) = (0, 0x40_0000);
pub const DATA_ARENA: (usize, usize) = (map::EXCLUDED_END, map::MAPPED_BYTES - map::EXCLUDED_END);

const _: () = assert!(palloc::CACHED_BASE == map::CACHED_BASE && palloc::PSRAM_BYTES == map::MAPPED_BYTES);
const _: () = assert!(palloc::HOLE_START == map::EXCLUDED_START && palloc::HOLE_END == map::EXCLUDED_END);

static ALLOC: Global<Palloc> = Global::new(Palloc::EMPTY);

/// Zeroing through the cached alias, then written back and dropped from the data caches: the chip holds zeros and no
/// cached line holds the old data. Exec blocks are also dropped from both cores' L1 ICache (`sync_icache`: ROM
/// `Cache_Invalidate_Addr` with the ICache map, then `fence.i`): the next owner must not run the last owner's code.
/// (The uncached alias is never written: #620's bit-8 flips.)
struct CachedZero;
impl Scrub for CachedZero {
    fn zero(&mut self, off: usize, len: usize, kind: Kind) -> bool {
        let Some(a) = map::cached_range(off, len) else { return false };
        // SAFETY: [off, off + len) is mapped PSRAM inside an arena (the allocator only names its own pages), which no
        // one is granted or executing while it is being zeroed; core 0 is the one user of the cache sync registers.
        unsafe {
            core::ptr::write_bytes(a as *mut u8, 0, len);
            map::cache_writeback_invalidate(a, len) == 0 && (kind == Kind::Data || hw::cache::sync_icache(a, len) == 0)
        }
    }
}

/// Copy `code` into exec block `b` and make it visible to instruction fetch on both cores. False if it does not fit
/// or a cache operation failed (the code must not run then).
pub fn load_code(b: &Block, code: &[u32]) -> bool {
    let Some(a) = map::cached_range(b.off, b.len).filter(|_| b.kind == Kind::Exec && code.len() * 4 <= b.len) else {
        return false;
    };
    // SAFETY: an exec block the kernel owns while no U runs; `a` is its mapped cached alias, `code` fits.
    unsafe {
        core::ptr::copy_nonoverlapping(code.as_ptr(), a as *mut u32, code.len());
        hw::cache::sync_icache(a, code.len() * 4) == 0
    }
}

/// Bring PSRAM up at 120 MHz and map it. Returns the init summary, or why it failed.
pub fn up() -> Result<psram::Info, &'static str> {
    // SAFETY: the one caller, core 0 during bring-up; nothing else uses PSRAM, MPLL or the MSPI pins.
    let info =
        unsafe { psram::init(psram::Upto::Tuned, psram::TuneOverride::default(), &mut |a| kprintln!("psram: {a}")) }
            .map_err(|_| "init")?;
    // SAFETY: init reached Tuned. The L2 tags are undefined after a download-mode boot: invalidate before the first map.
    let rc = unsafe { map::cache_invalidate_all_l2() | map::map_identity(PAGES) };
    if rc != 0 {
        return Err("map");
    }
    // SAFETY: core 0, single context (module doc); no other reference to ALLOC is live.
    unsafe { ALLOC.get() }.configure(EXEC_ARENA, DATA_ARENA).map_err(|_| "arenas")?;
    Ok(info)
}

fn with<R>(f: impl FnOnce(&mut Palloc) -> R) -> R {
    // SAFETY: core 0 only and never from a trap (module doc), so this is the only live reference.
    f(unsafe { ALLOC.get() })
}

pub fn set_quota(owner: usize, bytes: usize) -> Result<(), Error> {
    with(|a| a.set_quota(owner, bytes))
}
pub fn alloc(owner: usize, kind: Kind, len: usize) -> Result<(Handle, Block), Error> {
    with(|a| a.alloc(owner, kind, len, &mut CachedZero))
}
/// Write the cached lines of `[off, off + len)` back to the chip (they stay cached). False if the range is not an
/// arena's or the sync failed.
pub fn writeback(off: usize, len: usize) -> bool {
    // SAFETY: core 0 only (module doc), the one user of the cache sync registers here.
    map::cached_range(off, len).is_some_and(|a| unsafe { map::cache_writeback(a, len) } == 0)
}
/// Every block of `owner` back, zeroed: the hook for a process's end. Core 0 must reap: `end_proc` runs in a trap,
/// on core 1 for the #617 processes, and this module is core 0 only (an unlocked `Global`, the cache sync
/// registers). The owner's PMP view must be unloaded first, so no U run still reaches the pages being zeroed.
pub fn release_owner(owner: usize) -> bool {
    with(|a| a.release_owner(owner, &mut CachedZero))
}
pub fn usage(owner: usize) -> usize {
    with(|a| a.usage(owner))
}

/// The U region of `b`: its cached alias, checked once more (a block from anywhere but the allocator is refused).
pub fn grant(b: &Block) -> Option<Region> {
    let r = palloc::u_region(b);
    palloc::u_region_ok(&r).then_some(r)
}

/// M's cached view of physical offset `off` (one word).
pub fn read_cached(off: usize) -> u32 {
    // SAFETY: callers pass offsets of mapped arena pages (asserted by cached_addr, which refuses the hole and the end).
    unsafe { core::ptr::read_volatile(map::cached_addr(off).unwrap_or_else(|| crate::platform::exit(4)) as *const u32) }
}

/// The chip's content at `off`, past every cache (diagnostics: a cached-only check passes while the chip is stale).
pub fn read_uncached(off: usize) -> u32 {
    // SAFETY: `off` is in a mapped page (callers pass arena offsets) and 4-byte aligned (asserted inside).
    unsafe { map::uncached_read32(off) }
}
